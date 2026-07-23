# R8000 stock firmware — CTF / hardware-acceleration blob analysis

Source image: `/home/andrew/netgearr8000/images/R8000-V1.0.4.88_10.1.88.chk`
(NETGEAR R8000, board ID `U12H315T00_NETGEAR`, built 2024-05-08, kernel
`2.6.36.4brcmarm+`, SoC family Broadcom BCM4709 / BCM5301X "Northstar",
dual Cortex-A9).

Working data lives under `/home/andrew/netgearr8000/hwoffload-research/blob-analysis/`:
- `extract/` — binwalk TRX/LZMA extraction output
- `rootfs/` — unsquashed stock rootfs (unsquashfs, since binwalk 3.x's bundled
  `sasquatch` extractor wasn't on PATH — the system `squashfs-tools`
  `unsquashfs 4.7.5` handled the SquashFS4/xz partition directly)
- `dumps/` — per-module `modinfo`, `nm` symbol tables, `strings`, full
  `objdump -d` disassembly and `readelf -a` for every accel-relevant `.ko`

## 1. Extraction

```
CHK header (58B, board U12H315T00_NETGEAR) -> TRX v1, 2 partitions
  partition_0 (2,052,920 B, LZMA) -> decompressed.bin (5,042,304 B):
      ARM self-decompressing zImage stub, CPIO early-init archive at 0x20000,
      "Linux version 2.6.36.4brcmarm+ ... #17 SMP PREEMPT Wed May 8 2024" at 0x37F3AC.
      This is the kernel partition; not unpacked further (piggy-compressed
      vmlinuz payload — out of scope, see §5).
  partition_1 (28,138,668 B, SquashFS4/xz, 1778 inodes) -> rootfs/
```

Tooling: `arm-none-eabi-binutils` (objdump/readelf/nm work fine against
these relocatable `.ko` object files — no OS-specific linking needed) and
Python `capstone` 5.0.7 as a fallback disassembler.

## 2. Module inventory

`/lib/modules/2.6.36.4brcmarm+/kernel/drivers/net/` on the stock rootfs:

| module | license (MODULE_LICENSE) | role |
|---|---|---|
| `ctf/ctf.ko` (25,512 B) | **Proprietary** | Cut-Through Forwarding — software L2/L3 fast-path |
| `et/et.ko` (73,800 B) | **Proprietary** | GMAC3 (integrated Gigabit MAC) + RoboSwitch glue driver |
| `dpsta/dpsta.ko` | (unset/GPL-shaped, has debug info) | proxy-STA / 4-address WDS bridging for wireless, calls into CTF's device registration, no MMIO of its own |
| `igs/igs.ko` | Proprietary | IGMP snooping, depends on `emf` |
| `emf/emf.ko` | Proprietary | Efficient Multicast Forwarding (multicast bridge filter), no MMIO |
| `dhd/dhd.ko` (1.4 MB) | Proprietary | Wi-Fi (4366) FullMAC host driver — unrelated to wired accel |

Also checked and ruled out as irrelevant to hardware flow acceleration:
`lib/acos_nat.ko` (Netgear "ACOS" firmware's own userspace-facing NAT-rule /
URL-filter helper — `FindNatRule`, `HTTPURLFilterInspect`, etc., pure
netfilter-hook software, no register access) and
`drivers/net/ipset/skipctf.ko` (license `Circle` — a 3rd-party
parental-control partner module that just flags specific 5-tuples to be
*excluded* from CTF so Circle's DPI can still see them; also pure software).

There is **no `fa.ko`, `bcm_fa.ko`, or `natp.ko`** anywhere in the image, and
no string matches for `flow accel`, `flowtable`, `natp`, `hw nat`, `archer`,
or `runner` in any module. See §4 for why.

Userspace: `/usr/sbin/et` is the standard Broadcom `et` diag/config CLI
(register dump / speed-duplex / MIB counters via ioctls into `et.ko`); no
separate `fa`/`ctf` CLI tool ships (CTF is configured purely via `/proc` and
a netlink socket the module creates itself — see §3).

## 3. CTF (`ctf.ko`) — confirmed 100% software, zero MMIO

Full symbol table (`dumps/ctf_symbols.txt`) has **no** `ioremap`, `readl`,
`writel`, `si_*`, or `osl_pcie_*` references at all. Its only undefined
externals are: `bcm_bprintf`, `bcm_ether_ntoa`, `csum_partial`,
`ctf_attach_fn`, `dev_queue_xmit`, `getintvar`, `init_net`, `jiffies`,
`memcmp`/`memcpy`/`__memzero`, `netlink_kernel_create/release`,
`netlink_unicast`, `osl_ctfpool_*`, `osl_malloc/mfree`, `osl_pktfree`,
`osl_pkt_frmnative`, `ppp_{rx,tx}stats_upd`, `printk`, spinlock primitives,
`skb_*`, `sprintf`, `strncpy`. Every one of those is a kernel/skb/allocator
primitive — nothing touches a hardware register or bus.

**Mechanism** (from disassembly of `_ctf_forward`, `_ctf_ipc_add`,
`_ctf_brc_lkup_ll`, and the dump-format strings in `_ctf_dump`):

- CTF keeps its own **in-kernel software hash table** of established
  flows, entirely separate from (and instead of) Linux's normal
  netfilter/conntrack + routing path:
  - `ci_head` — a 128-bucket singly-linked hash table of `struct
    ctf_ipc` entries (**IP connection cache**). `_ctf_ipc_add` allocates
    124 bytes (`osl_malloc(..., 0x7c)`) per entry, `memcpy`s the caller's
    tuple struct in, computes the bucket by summing the 8 payload DWORDs
    (source/dest address words, IPv4 or IPv6) plus the 16-bit sport/dport
    and 8-bit protocol, XOR-folding the sum down to 7 bits
    (`(sum + sum>>16); (+ >>8); & 0x7f`), then pushes the entry onto
    `ci_head[bucket]`.
  - a second, parallel **bridge cache** (`_ctf_brc_*`, "Bridge cache:
    MacAddr / Interface / Hits" per the dump strings) keyed by MAC
    address, used for pure L2 (non-NAT'd, non-routed) bridged flows and
    for "hot" bridge-cache entries.
  - `_ctf_forward` is the actual fast path: given an skb, it inspects the
    Ethernet header (offset checks for ethertype `0x0800` IPv4 /
    `0x8100` VLAN / `0x86dd` IPv6 inline in the disassembly), does an
    `_ctf_brc_lkup_ll` / `_ctf_ipc_lkup_ll` against the software tables
    under `_raw_spin_lock_bh`, rewrites the L2/L3/L4 headers in place for
    the matched flow (NAT translation, TTL decrement, checksum fixup —
    all still CPU/software), and calls straight into `dev_queue_xmit()`
    on the outgoing netdev.
  - There is no step anywhere in this path that pushes a flow, ARL
    entry, or NAT rule into a hardware table. "Insertion into hardware"
    as posed by the task premise does not happen — insertion is into the
    `ci_head`/bridge-cache software hash tables described above, which
    live in ARM CPU DRAM and are walked by the ARM CPU on every packet.
  - Net effect: CTF *shortcuts the Linux netfilter/bridge/routing stack*
    for known flows (skip conntrack lookup, skip the full `ip_forward()`
    call graph, skip bridge STP/learning re-checks) — it is a **software
    fast-path optimization inside the kernel**, not a hardware
    flow-acceleration engine. This matches Broadcom's public description
    of CTF in their SDK release notes (`ctf` = "Cut Through Forwarding",
    marketed alongside the *separate* hardware `FA`/"Flow Accelerator"
    feature that Broadcom only ships on later chip families — see §4).
  - `ctf_kattach`/`ctf_netlink_sock_cb` open a generic-netlink socket
    (`netlink_kernel_create`) used by userspace (the `nvram`/`acos`
    config layer, via `_ctf_cfg_req_process`) to enable/disable CTF per
    LAN/WAN bridge, add static VLAN mappings (`_ctf_dev_vlan_add`), and
    dump the tables (`ctf_disable`, `Clear Port %d ctf entries`, `IP
    connection cache: %d entries`).

## 4. et.ko (GMAC3) — the *actual* hardware-touching module, and it maps 1:1 onto OpenWrt's already-open `bgmac`/`unimac` driver

`et.ko`'s undefined-symbol list (`dumps/et_symbols.txt`) is very different
from ctf.ko's: `__arm_ioremap`, `bcm_robo_attach`, `bcm_robo_config_vlan`,
`bcm_robo_detach`, `bcm_robo_enable_device`, `bcm_robo_enable_switch`,
`bcm_robo_snoop_mac_match`, `robo_read_link_status`, `robo_power_save_*`,
`robo_watchdog`, `get_robo_ptr`, `si_attach`, `si_setcore`,
`si_setcoreidx`, `si_coreid`/`si_corerev`, `si_core_reset`,
`si_pmu_chipcontrol`, `si_gpio*`, `osl_pcie_rreg`. These are all the
Broadcom "hnd" SiliconBackplane bus-enumeration layer (`si_*`) and the
RoboSwitch MDIO/register driver (`bcm_robo_*`/`robo_*`) — **and none of
them are defined inside `et.ko` itself.** They resolve against the
monolithic kernel image (`vmlinuz`), i.e. Broadcom compiled the
SiliconBackplane bus driver and the switch-register driver **directly into
the kernel**, not as separate loadable modules. Getting their literal
register-offset constants therefore requires disassembling the kernel
image proper (piggy-compressed inside `partition_0.bin`), which is future
work — see §5. However, the DTS cross-reference in §4b below already gives
us the physical MMIO map for this hardware from the GPL side, independent
of the blob.

What *is* inside `et.ko` and directly disassemble-able is the **GMAC3
UniMAC control path** (`gmac_init_reset`, `gmac_clear_reset`,
`gmac_speed`, `gmac_loopback`, `gmac_macloopback`, `gmac_mf_lkup`,
`chipconfigtimer`) — this is register-level MMIO access to the per-port
Gigabit MAC block, reached through the "hnd" OSL wrapper
`osl_pcie_rreg`/`osl_pcie_wreg` (Broadcom's SDK keeps this historical name
even on the ARM/AXI backplane — it is a plain 32-bit MMIO accessor, not
real PCIe config space, on this SoC).

Disassembly of `gmac_init_reset` (`dumps/et_disasm.txt`):

```
54a4:  push {r0,r1,r2,r4,r5,lr}; r4 = et_info
54cc:  r0 = [r4,#16]                  ; osh (osl handle)
54d4:  r1 = r5 + 0x800                ; r5 = GMAC core's mapped register base
54d8:  r1 = r1 + 0x8                  ; -> r1 = base + 0x808
54dc:  bl osl_pcie_rreg(r0, r1, &tmp, 4)   ; read 32-bit reg at core-base+0x808
54e8:  r2 = tmp                        ; value just read
54ec:  r3 = [ [r4,#4], #0x70 ]         ; corerev field from a sih/genl struct
54f0:  cmp r3, #3
54f4:  movls r3, #0x800                ; corerev <= 3 : bit 11
54f8:  movhi r3, #0x2000               ; corerev >  3 : bit 13
54fc:  orr r3, r3, r2                  ; OR reset bit into existing reg value
5500:  str r3, [r5, #2056]             ; write back to base+0x808
5504:  bl osl_delay
5508:  pop {..., pc}
```

This is the **UniMAC reset sequence**, and it is a verified, bit-for-bit
match against the mainline/OpenWrt GPL `bgmac`/`unimac` driver:

- `drivers/net/ethernet/broadcom/bgmac.h` (mainline): `#define
  BGMAC_UNIMAC 0x800` — the per-GMAC UniMAC sub-block starts at offset
  `0x800` inside each GMAC core's register window, exactly matching
  `r5 + 0x800` above.
- `drivers/net/ethernet/broadcom/unimac.h` (mainline, GPL-2.0-only):
  `#define UMAC_CMD 0x008` — offset `+0x008` from the UniMAC base, i.e.
  absolute `+0x808`, exactly matching the blob's `base+0x808` access.
  Its documented bitfields: `CMD_SW_RESET_OLD = (1<<11) = 0x800` and
  `CMD_SW_RESET = (1<<13) = 0x2000` — **exactly** the two constants
  (`0x800`/`0x2000`) the blob selects between based on core revision.
  Same header also defines `CMD_TX_EN`(bit0), `CMD_RX_EN`(bit1),
  `CMD_SPEED_{10,100,1000,2500}` at shift 2, `CMD_PROMISC`(bit4),
  `CMD_HD_EN`(bit10, half-duplex) — the same register `gmac_speed()`
  (`dumps/et_disasm.txt`, fn at `0x574c`) reads/writes for link
  negotiation, using the identical bit layout.

**Conclusion for et.ko/UniMAC: this part of the "closed" driver is not
actually novel or secret** — its register map is already fully
documented and already implemented, register-identical, in the GPL
`bgmac.ko` + `unimac.h` that ships in mainline Linux and that OpenWrt's
own `bcm53xx` target already builds and uses for the R8000 today (see
`bcm-ns.dtsi` below). No reverse engineering is required to reproduce
this piece — it's already open and already running on R8000-class OpenWrt
builds.

### 4b. MMIO map, cross-referenced against the OpenWrt/mainline DTS

`arch/arm/boot/dts/broadcom/bcm-ns.dtsi` (pulled in by
`bcm4709.dtsi` → `bcm4709-netgear-r8000.dts`, found in the already-built
kernel tree at
`openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64/build_dir/target-arm_cortex-a9_musl_eabi/linux-bcm53xx_generic/linux-6.12.94/arch/arm/boot/dts/broadcom/`)
gives the physical layout directly:

```
gmac0: ethernet@24000   reg = 0x24000 0x800   (-> physical 0x18024000, 4 GMAC/UniMAC cores)
gmac1: ethernet@25000   reg = 0x25000 0x800   (-> physical 0x18025000)
gmac2: ethernet@26000   reg = 0x26000 0x800   (-> physical 0x18026000)
gmac3: ethernet@27000   reg = 0x27000 0x800   (-> physical 0x18027000)
pwm:                     0x18002000 0x28
mdio:                    0x18003000 0x8       (brcm,iproc-mdio)
mdio-mux@18003000
(chipcommon-ish node)    0x18004000 0x14
srab: ethernet-switch@18007000   reg = 0x18007000 0x1000
       compatible = "brcm,bcm53011-srab", "brcm,bcm5301x-srab"
```

`R8000`'s own DTS (`bcm4709-netgear-r8000.dts`) enables `&srab` with 9
ports (`port@0`..`port@4` = lan1-4/wan, `port@5`/`port@7` disabled fixed-link
1000/full, `port@8` = cpu) — i.e. the SRAB (**Switch Register Access
Bridge**) is the MMIO front-end OpenWrt already uses, via the upstream
`net/dsa/b53` driver family, to program the BCM5301X's integrated 5+ port
switch (VLAN table, ARL/MAC-learn table, port config) at physical base
`0x18007000`, size 4 KiB. `target/linux/generic/hack-6.12/773-bgmac-add-srab-switch.patch`
in this repo's OpenWrt tree is the exact patch that wires `bgmac` to
`b53-srab-switch` at `ioremap(0x18007000, 0x1000)` — i.e. **OpenWrt's
current bcm53xx target already reimplements the switch side of this
"hardware accelerator" question from scratch, openly, register-compatible
with the same silicon this stock image runs on.**

The stock blob's `bcm_robo_*` calls (§4) are almost certainly this same
SRAB register block accessed via Broadcom's older "RoboSwitch" MDIO/SPI-
style API name (kept for source compatibility across chip generations —
Broadcom's SDK used `bcm_robo_*` as the switch API name from the earliest
external BCM53xx switch chips all the way through the SRAB-integrated
switch on Northstar); it is consistent with, not contradictory to, the
`brcm,bcm5301x-srab` binding OpenWrt already targets at `0x18007000`.
Getting the *literal* opcode/field layout of `bcm_robo_config_vlan`'s
SRAB register writes would require disassembling the monolithic kernel
image (§5) since — as noted in §4 — that function is not present inside
any `.ko` in this image. Given b53/SRAB is already fully open and already
runs this exact chip, that RE work has no payoff for the stated goal
(reimplementing acceleration as an open driver): it's already done
upstream.

## 5. What's left unrecovered, and why it doesn't matter

- `si_*` (SiliconBackplane enumeration) and `bcm_robo_*`
  (switch/SRAB register access) function bodies live in the compressed
  kernel image (`partition_0.bin` → LZMA → 5,042,304-byte self-decompressing
  ARM zImage with an embedded early-init CPIO and a further
  piggy-compressed vmlinux payload), not in any standalone `.ko`. Fully
  unpacking that piggy payload (extract the embedded gzip/LZMA stream,
  reconstruct the vmlinux ELF, disassemble `bcm_robo_config_vlan` etc.)
  was not carried out — it is a strictly bigger effort than the modules
  already extracted, and per §4b the register target it would confirm
  (SRAB @ 0x18007000) is already fully implemented and open via
  `net/dsa/b53` + the `773-bgmac-add-srab-switch.patch` hack in this
  OpenWrt tree. RE'ing it would at most confirm bit-level ARL/VLAN table
  formats Broadcom has never published, which OpenWrt's b53 driver
  already reverse-engineered/implemented independently and which this
  router does not need re-derived to run OpenWrt.
- No hardware Flow-Accelerator / NAT-processor (Broadcom "FA", the
  Runner/Archer-style ASIC block found on later BCM63xx/4906+/68xx SoCs)
  exists on BCM4709/BCM5301X at all — confirmed both by the total absence
  of any such module/string in the firmware (§2) and by CTF's symbol
  table proving its "acceleration" is pure software (§3). The task's
  premise ("the SoC's hardware packet Flow-Accelerator (FA) block") does
  not hold for this specific chip generation/board; Northstar's
  acceleration story is: software CTF shortcut + a plain (NAT-unaware)
  L2 switch (SRAB) + UniMAC-level line-rate MACs. This is a materially
  different, and much simpler, situation than newer Broadcom home-router
  SoCs.

## 6. Verdict

**How much of the hardware-programming path is recoverable from the GPL
portions vs. locked in the closed blob:**

1. **Fully recoverable / already recovered, GPL side**: the entire
   physical MMIO map for both blocks this router's "acceleration" story
   touches — 4× GMAC/UniMAC cores at `0x18024000`-`0x18027000` and the
   SRAB switch register block at `0x18007000` — comes straight from the
   mainline/OpenWrt DTS (`bcm-ns.dtsi`, `bcm4709-netgear-r8000.dts`), and
   OpenWrt's `bgmac` + `net/dsa/b53` (srab variant) drivers already
   program this hardware end-to-end today, with a verified bit-identical
   UniMAC `CMD` register sequence against the closed blob's own
   `gmac_init_reset`/`gmac_speed` code. **Zero additional reverse
   engineering is needed to reimplement the real hardware-register path
   as an open driver — it already exists and already runs on this exact
   board** (`openwrt-*-r8000*.chk` images already in
   `/home/andrew/netgearr8000/images/`).
2. **Locked in the closed blob, but low-value to recover**: the
   `bcm_robo_*`/`si_*` function bodies proper (they live in the
   proprietary-but-not-yet-fully-unpacked kernel image, §5). These would
   only yield Broadcom's *literal* opcode sequence for programming the
   SRAB ARL/VLAN tables — a target the open b53 driver already hits by
   an independently-developed, working register interface for the same
   `brcm,bcm5301x-srab` hardware.
3. **Not hardware at all, so nothing to recover**: CTF itself. It is a
   pure-software Linux-kernel flow-cache/fast-path shortcut with no
   register access whatsoever (§3), fully described here down to its
   hash function, bucket count, entry size, and struct layout. Its open
   functional equivalent already exists in mainline Linux as software
   flow offload / `nf_flowtable` (and, on hardware that has one, the
   kernel's hardware flow-offload framework) — OpenWrt's `firewall4`/
   `nftables` flowtable support is the direct modern analogue, and
   nothing on the R8000's silicon needs a new register interface to
   provide it.

**Bottom line**: for the R8000/BCM4709 specifically, there is no
undiscovered hardware NAT/flow-acceleration engine to reimplement. The
one real hardware surface (GMAC/UniMAC + SRAB switch) is already fully
open, already register-verified against the stock blob in this analysis,
and already shipped in this repo's OpenWrt bcm53xx target. The only
"acceleration" that has no open equivalent already running is CTF's
software fast-path shortcut, which is fully characterized above and has
a direct open substitute (kernel software/hardware flow offload) rather
than requiring RE of undocumented registers.
