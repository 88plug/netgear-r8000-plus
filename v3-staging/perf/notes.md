# Packet-processing performance tuning -- R8000 / BCM4709 / bcm53xx -- notes

Scope: every real lever available for NAT/forwarding throughput on this
specific board, tested live over SSH against the router at `192.168.1.1`
(OpenWrt 25.12.5, kernel 6.12.94, dual-core ARMv7 Cortex-A9 @ ~1GHz,
`BCM5301X`/BCM4709, board `netgear,r8000`). Every claim below that says
"confirmed live" was actually run and re-checked on the hardware in this
session, not inferred. Where live verification wasn't possible (item 6),
that's stated plainly rather than guessed at.

## Bottom line

| # | Item | Verdict | Real win? |
|---|---|---|---|
| 1 | HW flowtable offload (`flags offload`) | Confirmed unavailable -- silent no-op, no driver support | **No -- hard wall** |
| 2 | RPS (multi-core packet steering) | Confirmed **broken** on this board's stock config; fixed | **Yes -- the biggest lever here** |
| 2b | XPS | Confirmed not applicable (1 TX queue everywhere) | N/A, not a missed win |
| 3 | IRQ affinity | Confirmed both hw IRQ families were 100% on CPU0; fixed | **Yes -- secondary, real** |
| 4 | ethtool offloads | Could not verify live (`ethtool` not installed, no WAN uplink to fetch it); wired into the fix script, guarded | **Deferred, not blocked** |
| 5 | Crypto/SPU acceleration | Confirmed none exposed to Linux | **No -- hard wall** |
| 6 | SW flow-offload sanity check | Mechanism confirmed correctly wired; live traffic engagement not testable this session (no WAN uplink) | Mechanism proven, live proof pending |

---

## 1. Hardware flowtable offload (`flow_offloading_hw`)

**Verdict: genuinely unavailable on this SoC's driver stack. Leave
`flow_offloading_hw '0'` -- it already is, that's correct.**

Live test performed (fully reversible, reverted and re-verified at the end):

```
uci set firewall.@defaults[0].flow_offloading_hw='1'; uci commit firewall
/etc/init.d/firewall reload
```

Result -- `nft list ruleset` **before** (sw, the normal/recommended state):

```
flowtable ft {
    hook ingress priority filter
    devices = { "br-lan", "wan" }
    counter
}
```

**after** setting `flow_offloading_hw='1'`:

```
flowtable ft {
    hook ingress priority filter
    devices = { "lan1", "lan2", "lan3", "lan4", "phy0-ap0", "phy1-ap0", "phy1-ap1", "phy2-ap0", "wan" }
    flags offload
    counter
}
```

nftables **accepts** the config -- the device set gets rewritten from the
bridge (`br-lan`) to the individual member ports (fw4 has to do this
because a hardware-offloaded flowtable can only reference real physical
netdevs, not a software bridge), and `flags offload` is added with zero
errors, zero new lines in `dmesg` (checked with `dmesg -c` immediately
before the reload, then `dmesg` immediately after -- empty), and no
firewall-reload failure.

That silence is itself the answer. If bgmac/b53-srab/brcmfmac actually
implemented hardware TC/flowtable offload (`ndo_setup_tc` /
`flow_indr_dev_register` for `TC_SETUP_FT`), we'd expect either a driver
message confirming hardware programming or a rejection. Getting neither
is exactly the documented failure mode for this feature on unsupported
hardware: nftables' flowtable `flags offload` is generic infrastructure
that any driver *can* implement, but doesn't require the driver to -- if
no participating netdev's driver implements the callback, the kernel
silently treats it identically to software offload. No error surfaces
anywhere; it just never touches real hardware.

Corroborated two ways beyond this device:
- The `bgmac` driver is a basic SoC gigabit MAC driver; it does not
  implement `ndo_setup_tc`/hardware flow offload at all.
- OpenWrt's own flow-offloading documentation and community reports are
  explicit that hardware flow offload (`flow_offloading_hw`) support is
  limited to a small set of platforms (chiefly MediaTek mt76/mt7621's
  PPE, and a few others with a real hardware NAT/flow-classifier block)
  and that bcm53xx is not among them.
- Matches this repo's own prior finding (`v2-staging/extras/flow-offload/NOTES.md`,
  citing OpenWrt issue #7023): the BCM4709's proprietary CTF/FlowCache
  accelerator that stock Netgear firmware uses is a closed Broadcom block
  never wired into the mainline/OpenWrt driver stack. This session confirms
  the *generic* nftables-level hardware-offload path (a different,
  newer mechanism than CTF) is equally unavailable -- not just the
  Broadcom-specific one.

Reverted and re-verified: `flow_offloading_hw` back to `'0'`,
`nft list flowtable inet fw4 ft` back to the software-only form with
`devices = { "br-lan", "wan" }` and no `flags offload`.

## 2. Multi-core packet steering (RPS) -- the real lever, and it was broken

**Verdict: the stock `packet_steering=1` baseline does *not* achieve what
it's supposed to on this board. Root-caused and fixed
(`files/etc/init.d/perf-tune`).**

### The bug

`target/linux/bcm53xx/base-files/usr/libexec/platform/packet-steering.sh`
(the platform hook `packet_steering` procd service invokes on every
`network`/`firewall` config change) only ever writes to two netdevs:
`br-lan` and **`eth0`**. With `flow_offloading=1` / `flow_offloading_hw=0`
(this router's actual, recommended state), its logic is:

```sh
elif [ ${flow_offloading:-0} -gt 0 ]; then
    # SW offloading
    echo 2 > /sys/class/net/eth0/queues/rx-0/rps_cpus
```

i.e. it assumes **`eth0` is the DSA conduit** carrying all switch traffic.
That assumption is correct on *some* bcm53xx boards. It is **not** correct
on the R8000. Confirmed live:

```
$ ip -br link show
eth0             DOWN           e2:87:2e:dd:81:26 <BROADCAST,MULTICAST>
eth1             DOWN           12:ed:94:f2:aa:49 <BROADCAST,MULTICAST>
eth2             UP             e8:fc:af:f9:f1:38 <BROADCAST,MULTICAST,UP,LOWER_UP>
lan1@eth2        UP             ...
wan@eth2         LOWERLAYERDOWN ...
```

`eth0`/`eth1` are unused, always-down on-chip GMAC controllers with zero
switch ports wired to them on this board -- `/proc/net/dev` confirms 0
packets in either direction, ever. `eth2` is the real DSA conduit
(`ip -d link show wan` reports `dsa conduit eth2`), and it's the only
interface with live traffic counters incrementing.

Net effect **before this fix**: `packet_steering=1` + `flow_offloading=1`
was setting RPS on a completely idle interface (`eth0`) while leaving RPS
*disabled* (`rps_cpus=0`) on the interface actually carrying every
forwarded/bridged packet (`eth2`). Combined with IRQ affinity (item 3,
also broken), **100% of packet-forwarding softirq work was landing on a
single CPU core** -- the second Cortex-A9 core was doing zero networking
work despite the router believing it had steering enabled. This is a
genuine, verified defect in how the generic bcm53xx platform hook maps to
this specific board's interface naming, not a config gap on the
operator's side.

### The fix

`files/etc/init.d/perf-tune` sets `rps_cpus` to a mask covering every
online CPU (computed from `/proc/cpuinfo`, not hardcoded -- currently
`3` = both cores) on: `eth2` (the actual conduit), `wan`, `lan1`-`lan4`
(DSA slaves), `br-lan`, and every `phyN-apM` wifi AP BSS present (glob
match, so it covers all 6 BSSes from the multi-BSS driver fix in
`docs/WINS.md`, not just however many are up right now). Applied
unconditionally regardless of offload state -- RPS-spreading softirq work
across both cores is never harmful, so this doesn't need to replicate the
stock hook's offload-state conditionals.

Verified live, three ways:
1. **Direct effect**: `cat /sys/class/net/eth2/queues/rx-0/rps_cpus` reads
   `3` after `/etc/init.d/perf-tune start` (was `0`).
2. **Idempotency**: ran `start` twice back to back, no errors, same
   result.
3. **Reload-trigger wiring actually fires**: `service_triggers()` uses the
   same `procd_add_reload_trigger network/firewall` +
   `procd_add_raw_trigger "interface.*"` primitives as the stock
   `packet_steering` service. Important nuance discovered *and proven*
   live: these triggers fire on the `config.change` ubus event emitted by
   `/sbin/reload_config` (which diffs `uci show <pkg>` against a stored
   md5 snapshot and only fires for packages that *actually changed*) --
   **not** on a bare `/etc/init.d/firewall reload` invocation, which just
   calls that service's own `reload_service()` directly without going
   through the config-change notification bus. This is true of the stock
   `packet_steering` service too (reproduced: manually breaking `eth0`'s
   RPS then running `/etc/init.d/firewall reload` does *not* restore it
   either -- confirmed on the untouched stock service, not a regression
   from this change). It **does** fire correctly through the real path
   (`uci set` + `uci commit` + `/sbin/reload_config`, which is what
   LuCI's "Save & Apply" and normal `uci commit` workflows trigger):
   verified by deliberately zeroing `eth2`'s RPS mask, making a real
   firewall config change, running `/sbin/reload_config`, and watching
   both the mask (`0` -> `3`) and a fresh
   `perf-tune: RPS mask 3 applied...` line land in `logread`.

**Known, accepted race** (documented rather than hidden): when
`flow_offloading_hw` is set to `1` (the no-op hardware-offload-attempt
state from item 1 -- not the recommended state), the stock hook's branch
for that case *does* write `br-lan`'s RPS to `0`, and since both services
react to the same `config.change` event with no defined ordering between
them, `br-lan` can end up `0` or `3` depending on which callback runs
last. Reproduced live. This is low-stakes for two reasons: (a) it only
happens in a state this document recommends never using (item 1), and (b)
in the actual steady state (`flow_offloading=1`, `flow_offloading_hw=0`)
the stock hook's sw-offload branch never touches `br-lan` at all, so
there's no race in normal operation -- confirmed live, `br-lan` stayed at
`3` through a real config-change reload in the steady state. `eth2` (the
interface that actually matters) is never touched by the stock hook in
*any* branch, so it has no race in any state.

### XPS -- confirmed not applicable, not a missed win

Every netdev checked (`eth2`, `wan`, `lan1`, `br-lan`, `phy0-ap0`) reports
`numtxqueues 1` / `numrxqueues 1` via `ip -d link show`, and each has
exactly one `tx-0`/`rx-0` queue directory in sysfs. XPS steers *which of
several TX queues* a sending CPU uses; with one hardware TX queue per
device everywhere on this board (bgmac/DSA conduit+slaves *and*
brcmfmac AP interfaces all single-queue), there is nothing for XPS to do.
This isn't a config gap -- it's the actual queue count this SoC/driver
combination exposes. `perf-tune` deliberately does not touch `xps_cpus`
anywhere.

## 3. IRQ affinity

**Verdict: real, secondary win. Both hardware IRQ families were 100% on
CPU0; split across both cores. No irqbalance daemon needed or installed.**

`/proc/interrupts` before any change:

```
           CPU0       CPU1
 36:       1040          0   GIC-0 181  eth2
 48:        234          0   GIC-0 163  brcmf_pcie_intr
 49:       3598          0   GIC-0 169  brcmf_pcie_intr, brcmf_pcie_intr
```

Every packet-relevant hardware interrupt -- the wired MAC (`eth2`) *and*
both wifi radios' PCIe MSI interrupts -- was landing entirely on CPU0.
`smp_affinity` for all of these was `3` (unrestricted, both CPUs
allowed) -- nothing was *forcing* this, it's just where the in-kernel
default left them at probe time, and no active balancer (`irqbalance` is
not installed on this build) ever moved them. CPU1 was doing zero
hardware interrupt service for networking.

`perf-tune` statically pins (via `/proc/irq/<n>/smp_affinity`, looked up
by name from `/proc/interrupts` rather than hardcoded IRQ numbers, so it
self-heals if a future kernel/DTS revision changes IRQ numbering):
- `eth2`'s IRQ -> CPU0 only (`smp_affinity=1`)
- both `brcmf_pcie_intr` IRQs -> CPU1 only (`smp_affinity=2`)

Verified live: `/proc/irq/36/smp_affinity_list` -> `0`,
`/proc/irq/48,49/smp_affinity_list` -> `1`, after running the script.

**Why static pinning instead of `irqbalance`**: this board has exactly
three packet-relevant interrupt sources and two CPUs -- a fixed, known,
small topology. A persistent balancing daemon polling and rebalancing
that topology is pure overhead on a 1GHz dual-core part for a decision
two `echo` commands make once (and re-assert on every config-relevant
reload). `perf-tune` checks for `/etc/init.d/irqbalance` and defers to it
if present/enabled, so this doesn't need to be revisited if a future
build adds it for other reasons.

**Expected impact**: this is the secondary lever, not the primary one.
Splitting wired vs. wifi hardware-interrupt service reduces the two NIC
families contending for the *same* core's interrupt budget under combined
wired+wireless load, and complements item 2 (RPS distributes the
*softirq processing* that follows an interrupt, independent of which core
took the interrupt itself). Mostly a latency/jitter-under-combined-load
win rather than a big raw-throughput number by itself.

## 4. ethtool offloads

**Verdict: wired into the fix script, but could not be verified live this
session -- `ethtool` isn't installed on this build, and there's no way to
install it live.**

```
$ ethtool eth2
ash: ethtool: not found
$ opkg list-installed
ash: opkg: not found        # this build uses apk, not opkg (25.12.5)
$ apk list --installed | grep -i ethtool
                             # nothing
$ ping -c2 1.1.1.1
ping: sendto: Network unreachable   # WAN has no cable/carrier on this bench unit
```

No `opkg`, no local package cache for `ethtool` in this repo's
`images/packages/` (only a custom-built `kmod-brcmfmac`), and the router's
WAN is physically disconnected (per this repo's own bench state) so `apk
add ethtool` has no route to `downloads.openwrt.org` either. There is no
`python3`/`python` on-device to hand-roll a `SIOCETHTOOL` ioctl check as a
substitute.

`ip -d link show eth2` / `ip -d link show wan` do show generic
`gso_max_size`/`tso_max_size`/`gro_max_size` ceiling values, but these are
kernel-default *ceilings if the feature is used*, not proof of which
`NETIF_F_*` feature bits the `bgmac` driver actually has turned on --
that specific state is exactly what `ethtool -k` exists to show, and only
`ethtool -k`.

**What's done anyway**: `apply_ethtool_offloads()` in `perf-tune` issues
`ethtool -K <dev> {rx,tx,sg,tso,gso,gro} on` individually (one feature per
call, so an unsupported flag on this driver never blocks the others) for
`eth2`, `wan`, `br-lan`. It's guarded with
`command -v ethtool >/dev/null 2>&1` and logs a clear skip message when
absent -- confirmed live: `perf-tune: ethtool not installed -- skipping
offload tuning`. The moment a future image ships `ethtool`, this activates
automatically with no further changes needed.

**Action needed for a future build**: add `ethtool` to the ImageBuilder
`PACKAGES` list (standard `base` feed package, no local packaging work
needed -- same feed source already used for `wpad-basic-mbedtls` etc. per
`v2-staging/wpa3/imagebuilder-packages.md`'s convention). Once installed,
re-run `perf-tune` and confirm with `ethtool -k eth2` which flags were
already-on (checksum offload is very likely already default-enabled on
`bgmac` -- it's a standard basic-MAC feature) vs. actually toggled by this
script.

## 5. Crypto / SPU acceleration

**Verdict: hard wall. No hardware crypto engine exposed to Linux on this
SoC. Nothing to enable.**

```
$ cat /proc/crypto | grep -A1 name
name : crc32c        driver: crc32c-generic
name : zstd           driver: zstd-scomp / zstd-generic
name : ghash          driver: ghash-generic
name : lzo-rle        driver: lzo-rle-scomp / lzo-rle-generic
name : lzo            driver: lzo-scomp / lzo-generic
name : deflate         driver: deflate-scomp / deflate-generic
name : aes             driver: aes-generic
...
$ ls /sys/module | grep -iE 'crypto|spu'
cryptomgr
$ dmesg | grep -iE 'crypto|spu'
                       # nothing
```

Every registered algorithm is `-generic`/`-scomp` (pure software) -- no
`bcm-spu`, no hardware AES/hash driver, nothing ARM-CE-accelerated even.
BCM4709 does not expose a kernel crypto-API hardware backend on this
build. There is no routing/VPN-path crypto accelerator to turn on.

WiFi WPA/WPA2/WPA3 crypto is real hardware/firmware-accelerated, but
entirely inside the BCM43602 radio firmware blob (`brcmfmac`) -- opaque
to Linux, not configurable, not something OpenWrt enables or could
improve; it's simply not part of the CPU packet-forwarding path this
workstream is about.

Practical implication for the future, not an actionable item today: no
VPN (WireGuard/IPsec) is currently configured on this router. If one is
added later, be aware any VPN throughput will be **100% software AES on a
1GHz dual-core Cortex-A9** with no crypto-extension/NEON acceleration
confirmed available -- expect VPN throughput well below the router's raw
NAT-forwarding ceiling.

## 6. SW flow-offload sanity check

**Verdict: mechanism confirmed correctly wired via source inspection.
Live traffic engagement (watching a conntrack entry flip to `[OFFLOAD]`
under real load) could not be demonstrated this session -- no WAN uplink.**

```
$ nft list chain inet fw4 forward
chain forward {
    type filter hook forward priority filter; policy drop;
    meta l4proto { tcp, udp } flow add @ft
    ct state vmap { established : accept, related : accept } ...
    iifname "br-lan" jump forward_lan ...
    iifname "wan" jump forward_wan ...
    jump handle_reject
}
```

The `flow add @ft` statement -- the actual mechanism that shortcuts an
established flow's subsequent packets around the full
netfilter/routing/bridging stack -- is present, unconditional for any
forwarded TCP/UDP packet, and correctly scoped to the flowtable whose
`devices = { "br-lan", "wan" }` matches this router's real LAN/WAN split.
This is the identical, already-shipped mechanism this repo's own
`docs/WINS.md` lists as win #11 (`flow_offloading=1`).

What could **not** be demonstrated live: this bench unit's WAN port has
`NO-CARRIER` (no physical uplink cable -- confirmed, `ip -br link show`
shows `wan@eth2 LOWERLAYERDOWN`, and `ping -c2 1.1.1.1` returns `Network
unreachable`). The flowtable only accelerates *forwarded* (cross-zone,
routed) traffic; my own SSH session to the router's own `192.168.1.1`
address is *local input* traffic, not forwarded, and never touches this
flowtable regardless of offload state -- so `/proc/net/nf_conntrack`
correctly shows no `[OFFLOAD]`-flagged entries right now, and that's
expected, not a red flag.

Considered and rejected as a workaround: synthesizing forwarded traffic
with a temporary veth pair / network namespace pair. Rejected because the
flowtable's device set is specifically `{br-lan, wan}` -- a synthetic
third interface wouldn't be a member of that set (and modifying the live
flowtable's device membership to include a fake interface, plus adding
firewall accept rules for it, is a meaningfully invasive, error-prone
change to a live firewall config for a check whose *mechanism* is already
unambiguous from the ruleset itself). Not worth the risk for a bench
sanity check when the code path is this explicit.

**Recommended follow-up once this router has a live WAN uplink** (not
done here, listed for whoever deploys it):
```sh
# generate real cross-zone load (e.g. a large download from a LAN client
# to a real Internet host through this router), then:
nft list chain inet fw4 forward | grep -i offload   # mechanism present
cat /proc/net/nf_conntrack | grep -i OFFLOAD         # live entries, once real traffic flows
```

---

## Realistic throughput ceiling -- honest estimate, not a measurement

**Not measured on this router this session** -- WAN is disconnected, so
there is no way to run a real NAT throughput test (e.g. `iperf3`
LAN-client -> real Internet host) right now. What follows is a
reasoned estimate grounded in the hardware facts confirmed above plus
this repo's own prior citation (`v2-staging/extras/flow-offload/NOTES.md`)
of a directly comparable bcm53xx board (Phicomm K3, same SoC class)
measured at ~500 Mbit/s CPU-bound with software offload from an OpenWrt
forum report -- **not a promise, and should be re-measured with `iperf3`
once this router has a real WAN uplink.**

- **Before this session's fixes**: despite `flow_offloading=1` being set,
  the RPS misconfiguration meant this router was almost certainly getting
  *less* benefit from software offload than the comparable-hardware forum
  report above, since that report's setup presumably had working RPS
  where this one's didn't -- i.e. likely bound closer to a single Cortex-A9
  core's raw netfilter-processing ceiling for aggregate throughput,
  probably in the same ballpark as or below the ~500 Mbit/s reference
  point, worse for many-small-flow/high-pps workloads than for one large
  bulk transfer.
- **After this session's fixes (RPS + IRQ affinity correctly spreading
  softirq work across both cores)**: aggregate, many-concurrent-flow NAT
  throughput should meaningfully improve toward (and plausibly exceed,
  given the reference point was itself not necessarily RPS-correct)
  the ~500-900 Mbit/s range on a gigabit WAN link, since RPS's real benefit
  is letting *different flows* use *different cores* in parallel.
- **What RPS does *not* fix**: a single TCP flow's packets hash to one
  core for ordering -- RPS parallelizes across flows, not within one flow.
  A single large single-flow transfer is still bound by one 1GHz
  Cortex-A9 core's per-packet forwarding cost (netfilter + flowtable
  shortcut lookup), likely still in the low-to-mid hundreds of Mbit/s for
  one flow alone, not full gigabit.
- **Small-packet/high-pps workloads** (VoIP, gaming, many simultaneous
  connections) will hit the CPU-cost-per-packet wall well before
  large-packet bulk-transfer throughput does, regardless of any of these
  fixes -- that's a property of doing NAT in software on a 2009-class ARM
  core, not something tunable away.
- **Ceiling this SoC will not reach on OpenWrt, confirmed this session**:
  full line-rate gigabit NAT throughput for a single flow, or anything
  close to the stock-firmware CTF-accelerated ~940 Mbit/s reference point
  cited in this repo's own flow-offload notes -- that required Broadcom's
  proprietary hardware accelerator, which (item 1) is definitively not
  available in the open driver stack.

**Recommended verification once WAN is live**: `iperf3` from a LAN client
through this router to a real Internet-side (or a second router acting as
a WAN-side iperf3 server) target, single-flow and multi-flow (`-P 4` or
higher), before/after comparison is no longer possible (the fix is
already applied and live-tested), but an absolute measurement against
this document's estimate would confirm or correct the numbers above.

## Files in this directory

- `network.snippet` -- UCI `network.globals.packet_steering` baseline
  (already correct on this router; documented for the files/ overlay).
- `files/etc/init.d/perf-tune` -- the actual fix: RPS (item 2), IRQ
  affinity (item 3), best-effort ethtool offloads (item 4). Installed,
  enabled, and live-tested end-to-end on the bench router this session
  (start/stop/idempotency/reload-trigger-firing/race-condition all
  exercised for real, not assumed -- see item 2's verification section).
- This file.

## Install

Copy `files/etc/init.d/perf-tune` into the image's `FILES=` overlay at
the same path (mirrors this repo's existing `image-files/` /
`v2-staging/extras/radio-watchdog/files/` convention), and merge
`network.snippet`'s `packet_steering '1'` into the built image's
`config globals 'globals'` section (already the default; only needed if
building `/etc/config/network` from scratch rather than relying on the
upstream default). No `PACKAGES` addition is required for `perf-tune`
itself (pure shell, no new binaries) -- add `ethtool` to `PACKAGES` only
if/when item 4's live verification is wanted.

At runtime the script self-installs like any other init.d service:
```sh
/etc/init.d/perf-tune enable
/etc/init.d/perf-tune start
```
