# Software flow-offload on bcm53xx (R8000) -- notes

## What this is for

NAT/forwarding throughput. Stock R8000 firmware hits ~1 Gbit WAN<->LAN using
Broadcom's proprietary CTF ("NATP accelerator") hardware path. OpenWrt's
mainline `bgmac`/`b53` drivers for this SoC do not implement that path, so
routed (NAT'd) traffic on a vanilla OpenWrt bcm53xx build is CPU-bound on the
2x Cortex-A9 @ ~1 GHz. The only offload mechanism available on this target is
kernel **software flow-offloading** (flowtable-based shortcut for already-
established connections, still CPU work but skips the full netfilter/routing
stack per packet).

There is no hardware flow-offload path to enable on bcm53xx -- it's not a
config gap, the silicon-acceleration block that stock firmware drives just
isn't wired up in the open driver stack. `flow_offloading_hw` should stay `0`.

## Evidence

- OpenWrt issue #7023 (DSA driver performance issue, EA9500 -- same b53/SRAB
  switch family as the R8000's onboard BCM53012):
  - bcm53xx maintainer confirms no upstream NATP/CTF-equivalent:
    https://github.com/openwrt/openwrt/issues/7023#issuecomment-1040297058
  - Fix recommended in-thread: enable software flow-offloading:
    https://github.com/openwrt/openwrt/issues/7023#issuecomment-1040297068
  - `perf record` from that thread shows the CPU hotspots (`csum_partial`,
    `__copy_to_user_std`, `v7_dma_inv_range`) that flow-offload's flowtable
    shortcut is specifically designed to bypass on established flows.
- OpenWrt forum, "OpenWRT 19.07 and bcm53xx target + flow offloading": a
  Phicomm K3 (bcm53xx) user measured ~500 Mbit/s routed with offload off vs.
  their old Tomato+CTF box at ~940 Mbit/s on the same 1 Gbit link -- the gap
  matches the CPU-bound-forwarding diagnosis above.
  https://forum.openwrt.org/t/openwrt-19-07-and-bcm53xx-target-flow-offloading/52356

## bcm53xx-specific interaction: packet steering / RPS

This target ships a platform hook that most other targets don't:
`target/linux/bcm53xx/base-files/usr/libexec/platform/packet-steering.sh`.
It's invoked automatically by the generic `packet_steering` procd service
(`package/network/config/netifd/files/etc/init.d/packet_steering`) on every
`network`/`firewall` reload, and reads
`firewall.@defaults[0].flow_offloading` / `flow_offloading_hw` directly to
retarget RPS (receive packet steering) CPU affinity:

- offload **off** (default OpenWrt state): RPS is spread across `br-lan`
  (CPU1 handles LAN-side RX), `eth0` untouched.
- offload **on, software**: RPS moves to `eth0` (the DSA conduit interface
  carrying all switch ports) so the *second* CPU core participates in
  forwarding through the flowtable fast path -- this is the actual mechanism
  that lets a 2-core Cortex-A9 approach line-rate with sw offload enabled.
- offload **on, hardware**: same as off (no-op on this target since there's
  no hw path to steer around).

Net effect: turning on `flow_offloading` in `/etc/config/firewall` and
reloading is *sufficient* on the R8000 -- the RPS/CPU-affinity retuning
happens automatically via the existing bcm53xx platform hook, no extra script
needed. This snippet does not duplicate that logic; it only sets the UCI
option that triggers it.

## Known incompatibility

Software flow-offloading is not compatible with SQM (`luci-app-sqm` /
`sqm-scripts`) -- flowtable fast-path bypasses the qdisc SQM depends on for
shaping. If SQM/QoS is wanted on this router, `flow_offloading` must stay off
and throughput will be CPU-bound per the numbers above. This is a genuine
trade-off on this hardware, not a bug to work around.

## Install

Either:
1. Append the `config defaults` block from `firewall.snippet` into
   `/etc/config/firewall` in the image's files/ overlay (merge with the
   existing `config defaults` stanza -- don't duplicate the section), or
2. At runtime:
   ```sh
   uci set firewall.@defaults[0].flow_offloading='1'
   uci set firewall.@defaults[0].flow_offloading_hw='0'
   uci commit firewall
   /etc/init.d/firewall reload
   ```
   (Reloading firewall also fires the `packet_steering` reload trigger, which
   re-runs `packet-steering.sh` and repositions RPS -- no separate step
   needed.)

## Verify

```sh
# offload active in the live ruleset:
nft list chain inet fw4 forward | grep -i offload
# RPS moved to eth0 per the platform hook:
cat /sys/class/net/eth0/queues/rx-0/rps_cpus     # expect 2 (CPU1 mask) once enabled
cat /sys/class/net/br-lan/queues/rx-0/rps_cpus   # expect 0 once enabled
```
