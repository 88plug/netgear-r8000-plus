# BCM4709/BCM5301X "Northstar" Flow Accelerator (FA) — Hardware Documentation Survey

Target part: Broadcom BCM4709A0 (Netgear R8000), "Northstar" family (BCM4707/4708/4709,
BCM53010-53018, all sharing the `brcm,bcm5301x` / `brcm,bcm-nsp-*` device-tree bindings).
Goal of this workstream: locate hardware documentation for the FA block — datasheets,
register references, MMIO layout, NATP/flow-cache programming interface — and determine
whether `bcma` enumerates it as a discrete core.

## Bottom line

- **No official Broadcom datasheet or register reference for FA is public.** Full
  BCM4709/BCM5301X programmer's references are NDA-gated (Broadcom licensee portal only).
- **`bcma` does NOT enumerate a separate "FA" core.** There is no `BCMA_CORE_*` ID for
  it anywhere in the mainline core-ID list (checked the full current list, see below).
  FA is not a standalone AMBA/OCP backplane slave.
- **However, a real, byte-level FA register map exists in the wild**, leaked as part of
  Broadcom's `et` (Ethernet) driver source (`etc_fa.c`, `etc_fa.h`, `fa_core.h`,
  Broadcom copyright 2013–2014) inside several vendor GPL/HND source drops that were
  *not* stripped down to binary-only `.ko` files the way `ctf.ko`/`fa.ko` usually are.
  Copies of these exact files are saved next to this note in `extracted-sources/`.
- **Where it physically lives**: FA is a sub-block hanging off the register window of
  a Gigabit MAC (`GMAC`) core instance on the SiliconBackplane/AXI bus — specifically
  **GMAC core-unit 3** (a 4th, otherwise-unused-as-a-MAC GMAC instance), at a fixed
  byte offset **`FA_BASE_OFFSET = 0xc00`** within that core's register window, even
  though the block is logically wired to GMAC-2/switch port 8's traffic path. This is
  stated verbatim in Broadcom's own source comment (see below) — it is enough to program
  from an open driver in principle, but is undocumented by any public datasheet and
  never implemented in mainline Linux.

---

## 1. Official documentation: none public

- BCM4709/BCM53012 distributor briefs (Jotrin, Broadcom's own product pages) are
  marketing-level only ("Communications Processor with Network Acceleration Hardware").
  No register-level PDF is indexed publicly.
- Broadcom's own semiconductor documentation portal (`knowledge.broadcom.com`,
  `docs.broadcom.com`) requires a customer/licensee login for anything beyond product
  briefs; searches surfaced no public FA/BCM4709 programmer's reference.
- WikiDevi/DeviWiki (`deviwiki.com`, `wikidevi.wi-cat.ru`) and the Tom's Hardware
  "Router SoC 101" series document board-level facts (CPU, RAM, radios) but nothing
  register-level about FA.
- Conclusion: FA has no public, Broadcom-issued register reference. Everything below
  is reconstructed from (a) leaked SDK source that ended up in GPL drops, and (b)
  Linux kernel / OpenWrt developer commentary from Broadcom's own upstream engineers.

## 2. `bcma` core enumeration: FA is not a discrete core

Full current `BCMA_CORE_*` list was pulled from `include/linux/bcma/bcma.h`
(mainline, `torvalds/linux`) and cross-checked against `drivers/bcma/scan.c`'s name
table. Relevant/near-miss entries:

```
BCMA_CORE_ROBOSWITCH      0x81C   /* legacy RoboSwitch, older MIPS-era chips */
BCMA_CORE_MAC_GBIT        0x82D   /* Gigabit MAC (GMAC) core — THIS is FA's host core */
BCMA_CORE_4706_MAC_GBIT   0x52D
BCMA_CORE_4706_MAC_GBIT_COMMON  0x5DC
```

No `BCMA_CORE_FA`, `BCMA_CORE_FLOW_ACCEL`, `BCMA_CORE_NATP`, or similar ID exists
anywhere in the header (verified against the complete `#define BCMA_CORE_*` list,
~90 entries, up to `BCMA_CORE_SYS_MEM 0x849`). `drivers/bcma/scan.c`'s human-readable
core-name table has no FA-related string either.

This is expected once you see the leaked SDK source (§4): FA is not exposed to the
SiliconBackplane's OCP enumeration ROM as its own component with a manufacturer/id/class
tag. It is physically a sub-region *inside* one GMAC core's own 4 KiB MMIO window
(`BCMA_CORE_SIZE == 0x1000`), reached at `+0xc00` from that core's base — i.e. it rides
on the back of a `BCMA_CORE_MAC_GBIT` (0x82D) instance that mainline `bcma`/`bgmac`
already enumerates and maps, just never reads/writes above the offsets `bgmac` cares
about.

## 3. Switch-side confirmation: "Flow Accelerator" tied to port 8 / GMAC-2

Independent of the leaked driver source, Broadcom's own upstream kernel engineers have
referenced the FA block by name in public Linux kernel development:

- Florian Fainelli (Broadcom), commit message for
  **"ARM: dts: NSP: Switch to port 8 for CPU port"** (accepted into `devicetree/next`,
  2018): moving Northstar-Plus reference boards to switch port 8 as the CPU port
  "allows preparing room for supporting the **Flow Accelerator 2 NAPT offload**", and
  frees port 5 to be fully configurable (internal/SGMII/RGMII/etc).
  (https://lkml.iu.edu/hypermail/linux/kernel/1803.1/04661.html)
- `drivers/net/dsa/b53/b53_common.c` (mainline) — for the related BCM58xx switch die
  family the driver code and comment explicitly say: *"Broadcom BCM58xx chips have a
  flow accelerator on Port 8 which requires us to use the prepended Broadcom tag
  type"* — confirming the FA block's hardware-tag/re-circulation dependency on the
  IMP/CPU switch port, and that this switch IP (shared across the b53-managed chip
  family, including BCM5301x on Northstar and BCM58xx elsewhere) is the FA's home.
  mainline `b53` only *reacts* to FA's tag-protocol requirement for BCM58xx; it never
  programs FA registers for BCM5301x, and FA is left in bypass on all upstream targets.
- The R8000's own device tree (`bcm4709-netgear-r8000.dts`) configures the SRAB switch
  with port 8 as `label = "cpu"`, ports 5/7 disabled (they're reserved for the
  WLAN-forwarder GMACs per §4), confirming the same port-8/CPU-port topology.
- `hndfwd.h` (Broadcom, see §4) independently documents port 8 as "IMP port capable
  (Integrated Management Port)... All routed LAN and WAN traffic are directed to this
  port" — this is the packet path FA intercepts.

## 4. The leaked register-level source (the actual finding)

GitHub code search over public forks of Broadcom's HND/`et` driver tree (ASUS Merlin,
FreshTomato, Advanced Tomato, various vendor GPL drops, a Xiaomi Mi WiFi GPL dump, and
`jameshilliard`'s R7000 GPL mirror) turned up **full, non-obfuscated C source** for the
FA glue layer — this is normally exactly what ships only as `fa.ko`/`ctf.ko` binary
blobs, but a handful of vendor GPL trees (Netgear R6300v2, various `src-rt-6.x.4708`
trees) included it as buildable source. Local copies saved in `extracted-sources/`:

| File | What it is |
|---|---|
| `fa_core.h` | "Broadcom SiliconBackplane FA (Flow accelerator) definitions" (© 2013 Broadcom) — the actual register struct (`faregs_t`) and every bit-field, described in detail below. |
| `etc_fa.h` | "Flow Accelerator setup functions" (© 2014 Broadcom) — driver API: `fa_attach()`, `fa_napt_add/del/live()`, `fa_conntrack()`, `fa_regs_show()`, etc. Takes the same `ctf_ipc_t` connection-tuple struct that Rafał Miłecki's 2013 CTF reverse-engineering effort already documented for the software (netfilter-shortcut) side. |
| `etc_fa.c` | Full 1884-line implementation: core discovery/attach, NAPT table read/write helpers, `fa_napt_add()`, hashing, NVRAM gating (`ctf_fa_mode`, `fa_overridden`). |
| `hndfwd.h` | "HND Forwarder between GMAC and HW switching capable WLAN interfaces" (Broadcom) — the architecture doc explaining the 3-GMAC Northstar topology and exactly what FA does at a system level. |

### 4a. FA register map (`fa_core.h`)

```c
#define FA_BASE_OFFSET   0xc00      /* byte offset within host GMAC core's MMIO window */

typedef volatile struct _faregs {
    uint32  control;          /* 0x00 */
    uint32  mem_acc_ctl;      /* 0x04 — indirect table access: select table + index + R/W */
    uint32  bcm_hdr_ctl;      /* 0x08 — Broadcom in-band tag (re-circulation header) control */
    uint32  l2_skip_ctl;      /* 0x0c */
    uint32  l2_tag;           /* 0x10 */
    uint32  l2_llc_max_len;   /* 0x14 */
    uint32  l2_snap_typelo;   /* 0x18 */
    uint32  l2_snap_typehi;   /* 0x1c */
    uint32  l2_ethtype;       /* 0x20 */
    uint32  l3_ipv6_type;     /* 0x24 */
    uint32  l3_ipv4_type;     /* 0x28 */
    uint32  l3_napt_ctl;      /* 0x2c — hash seed / timestamp control for NAPT lookups */
    uint32  status;           /* 0x30 — interrupt status (per-stage init-done bits) */
    uint32  status_mask;      /* 0x34 */
    uint32  rcv_status_en;    /* 0x38 */
    uint32  stats[10];        /* 0x3c..0x60 — hit/miss/parse-failure counters */
    uint32  error;            /* 0x64 */
    uint32  error_mask;       /* 0x68 */
    uint32  dbg_ctl;          /* 0x6c */
    uint32  dbg_status;       /* 0x70 — includes MEM_ACC_BUSY poll bit */
    uint32  mem_dbg;          /* 0x74 */
    uint32  ecc_dbg;          /* 0x78 */
    uint32  ecc_error;        /* 0x7c */
    uint32  ecc_error_mask;   /* 0x80 */
    uint32  eccst[5];         /* 0x84..0x94 — ECC error address capture per internal table */
    uint32  hwq_max_depth;    /* 0x98 */
    uint32  lab_max_depth;    /* 0x9c */
    uint32  m_accdata[8];     /* 0xa0..0xbf — 256-bit data window for indirect table R/W */
} faregs_t;
```

Full bit-field definitions for every register above (control-reg init/bypass bits,
BRCM-header enable bits, L2/L3 protocol match registers, the three internal tables —
NAPT flow table / NAPT pool table / next-hop table — interrupt/error/debug bits) are
in `extracted-sources/fa_core.h` verbatim. Table capacities are also defined:
`CTF_MAX_FLOW_TABLE = 1024`, `CTF_MAX_NEXTHOP_TABLE_INDEX = 128`,
`CTF_MAX_POOL_TABLE_INDEX = 4`, `CTF_MAX_BUCKET_INDEX = 4`.

The three internal hardware tables are **not** memory-mapped as flat arrays — they're
reached the same register-indirect way SRAB reaches switch registers: write
`mem_acc_ctl` with `(table_select << 10) | index | RD/WR-bit`, then read/write the
8×32-bit `m_accdata[]` window (`FA_MAXDATA = 8`, "0:255 bits" — a 256-bit flow-entry
row), polling `dbg_status & CTF_DBG_MEM_ACC_BUSY` for completion. This mechanism is
implemented in `etc_fa.c` as `SELECT_MACC_TABLE_RD/WR()` + `CTF_FA_MACC_RD/WR()`.

### 4b. Where the registers physically live — `etc_fa.c: fa_corereg()`

```c
/* GMAC-2 connect FA but FA regs fall in GMAC-3 corereg space so using GMAC-3 as base. */
if (unit == 2) {
    si_setcore(fai->sih, GMAC_CORE_ID, unit);          /* unit==2, i.e. GMAC-2/port-8 */
    fai->fadevid = (void *)si_addrspace(fai->sih, 0);
    if ((fai->regs = si_setcore(fai->sih, GMAC_CORE_ID, 3)))   /* GMAC core-UNIT 3 */
        fai->regs = (faregs_t *)((uint8 *)fai->regs + FA_BASE_OFFSET);
    si_setcoreidx(fai->sih, idx);
}
```

`GMAC_CORE_ID` = **0x82D**, i.e. mainline's `BCMA_CORE_MAC_GBIT` (confirmed identical
value cross-referenced against `include/linux/hndsoc.h`-equivalent headers in several
vendor trees, e.g. `GMAC_CORE_ID_NS 0x82d` "Northstar's Gigabit MAC core"). So:

- FA is logically the offload path for **GMAC-2** (switch port 8, the CPU/IMP/
  "Networking GMAC" — §3), but
- its actual register window is borrowed from a **4th GMAC core instance**
  (core-unit index 3) that Northstar's silicon includes on the backplane but that the
  3-GMAC forwarder architecture (`hndfwd.h`) never uses as an actual MAC.

### 4c. Deriving a concrete physical address (BCM4709, inferred — not datasheet-confirmed)

Mainline's device tree (`arch/arm/boot/dts/broadcom/bcm-ns.dtsi`, same file that
backs the R8000's `.dts`) maps all 4 GMAC cores on the ChipCommon/AXI bus at
`0x18000000`:

```
gmac0: ethernet@24000  reg = <0x24000 0x800>;   -> phys 0x18024000
gmac1: ethernet@25000  reg = <0x25000 0x800>;   -> phys 0x18025000
gmac2: ethernet@26000  reg = <0x26000 0x800>;   -> phys 0x18026000
gmac3: ethernet@27000  reg = <0x27000 0x800>;   -> phys 0x18027000   <-- FA host core
```

Combining the upstream-confirmed `gmac3` base (`0x18027000`) with the leaked SDK's
`FA_BASE_OFFSET` (`0xc00`) gives an **inferred FA register base of `0x18027c00`** on
BCM4709. Two caveats on this number: (1) it is a derivation from two independently-
sourced public facts, not something read off an official memory map, so treat it as a
strong hypothesis to verify against silicon, not a confirmed constant; (2) mainline's
DT node for `gmac3` only reserves `0x800` bytes (deliberately excluding the FA
sub-region since `bgmac` never touches it) — the real per-core backplane window is the
standard `BCMA_CORE_SIZE` of `0x1000` (4 KiB), which comfortably contains `0xc00`
through the end of `faregs_t` (offset `0xa0 + 8*4 = 0xc0` past `FA_BASE_OFFSET`, i.e.
ending around `0x18027cc0`).

### 4d. System-level operation (`hndfwd.h`)

Verbatim from Broadcom's own header comment — this is the clearest public description
of what the FA hardware actually does:

> "Software Cut-Through-Forwarding CTF will accelerate WLAN <-> WAN traffic. When the
> hardware Flow Accelerator is enabled, WLAN <-> WAN traffic need not re-enter the host
> CPU, other than the first few packets that are needed to establish the flows in the
> FA, post DPI or Security related flow classification functions."

I.e.: the software CTF path (netfilter shortcut around conntrack, already documented
publicly since Rafał Miłecki's 2013 RE effort, `ctf_ipc_t`/`ctf_brc_t` structs) is what
classifies and admits a flow on its first few packets; once admitted, `fa_napt_add()`
programs that same `ctf_ipc_t` tuple into the FA hardware's NAPT flow table (via the
`mem_acc_ctl`/`m_accdata[]` indirect window, §4a), after which the switch fully
re-circulates matching packets through port 8 in hardware — NAT/NAPT rewrite included —
without any host CPU involvement at all.

Gating: `fa_attach()` requires the NVRAM var `fa_overridden == 2` and
`ctf_fa_mode != 0` to even probe the hardware — this matches (and now explains) years
of community-sourced NVRAM tribal knowledge on SNBForums about `ctf_fa_cap`/
`ctf_fa_mode` (values informally reported as 0/1/2, "CTF-only" vs "CTF+FA capable" vs
"CTF+FA enabled") — `etc_fa.c` defines the mode constants directly:
`CTF_FA_BYPASS=1`, `CTF_FA_NORMAL=2`, `CTF_FA_SW_ACC=3`.

## 5. SRAB — the one genuinely open, documented register interface on this SoC

Not FA itself, but worth recording: the only fully public, in-mainline, register-level
interface into this switch silicon is the **SRAB** (Switch Register Access Bridge),
`drivers/net/dsa/b53/b53_srab.c`, MMIO at **`0x18007000`** (`brcm,bcm5301x-srab`,
confirmed both in `bcm-ns.dtsi` and by the R8000's own DT override). It's an
indirect-access bridge (`B53_SRAB_CMDSTAT`/`WD_H`/`WD_L`/`RD_H`/`RD_L`, page+reg
addressed, grant/request handshake via `B53_SRAB_CTRLS`) into the b53-managed switch's
internal register space — the same switch core that self-identifies as
**"BCM53012, rev 5"** at runtime on an R8000 (confirmed via a public OpenWrt/Gargoyle
boot log: `b53-srab-switch 18007000.ethernet-switch: found switch: BCM53012, rev 5`).
SRAB can access general switch registers (VLAN, port state, mirroring, MIB counters)
and is what upstream `b53`/DSA already drives — but it does not expose the FA
sub-block; FA is a GMAC-core peripheral, not a switch (SRAB) register-page.

## 6. What community open-source efforts have (and haven't) done

- No public project has produced an open Linux driver that programs the FA block
  itself. OpenWrt's `bcm53xx` target boots BCM4709 routers and drives the switch via
  `b53`+SRAB and Ethernet via `bgmac`, but leaves FA (and CTF) completely unused —
  confirmed by grepping current `bcm-ns.dtsi`/`bcm5301x.dtsi`/`bcm4709-netgear-r8000.dts`
  for any "fa"/"flow"/"accel" node: there is none.
- CTF (the *software* netfilter-bypass half) was reverse-engineered publicly by Rafał
  Miłecki in 2013 (netdev list, `hndctf.h` struct dump) but never reimplemented/merged
  — mainline instead grew its own generic software flow-offload infrastructure
  (`nf_flowtable`, 2018+) and, in the OpenWrt world, third-party projects like
  `natflow`/SFE, which are independent hash-table fastpaths that don't touch Broadcom's
  hardware at all.
- No public project has attempted the FA hardware path specifically, as far as this
  search could determine — most likely because (a) FA's existence/register layout was
  never as visible as CTF's software hook, and (b) CTF/software-flowtable offload alone
  already recovers most of the throughput loss for typical WAN speeds, leaving little
  incentive to chase the harder hardware path.

## 7. Assessment: is this enough to program from an open driver?

Enough exists publicly (as of this survey) to attempt it, with real risk:

- **What's solid**: the full register struct and every documented bit field
  (`fa_core.h`), the base-offset/host-core addressing scheme and its exact
  justification in a source comment (`etc_fa.c: fa_corereg()`), the indirect
  three-table (NAPT flow / NAPT pool / next-hop) programming protocol
  (`SELECT_MACC_TABLE_*` + `m_accdata[]`), the NVRAM gating semantics, and independent
  corroboration from Broadcom's own upstream kernel engineers (Fainelli's commit
  message, the b53 driver comment) that this exact switch IP's FA block sits on
  port 8 and requires the prepended-tag protocol.
- **What's missing/unverified**: (1) the physical base address for BCM4709 specifically
  is *derived*, not read from an authoritative memory map — needs on-hardware probing
  (e.g. reading the SiliconBackplane's own enumeration ROM / EROM at runtime, or MMIO
  probing at the inferred `0x18027c00` under a debug UART/JTAG session) to confirm;
  (2) the "prepended Broadcom tag" framing format that FA/re-circulation requires
  (`bcm_hdr_t` in `etc_fa.h`, and `DSA_TAG_PROTO_BRCM_PREPEND` in mainline `b53`) needs
  to be cross-checked bit-for-bit against this specific chip revision — the leaked
  source spans several chip generations (BCM4706 through BCM5301x/58xx) and register
  layout/behavior nuances (e.g. the `FA_777WAR_ENABLED` chip-rev-2 workaround already
  visible in `fa_attach()`) suggest revision-specific quirks aren't uniform; (3) no
  public source describes the SiliconBackplane-level clock/reset sequencing needed to
  bring the GMAC-3/FA sub-block out of reset outside of the vendor `et` driver's own
  init path, which itself depends on the closed `robo`/switch attach sequence
  (`fa_attach()` takes a `void *robo` handle from the switch driver).

## Files in `extracted-sources/`

- `fa_core.h` — FA register struct + bit fields (Broadcom, © 2013)
- `etc_fa.h` — FA driver API surface (Broadcom, © 2014)
- `etc_fa.c` — full FA attach/NAPT-table/register-access implementation (1884 lines)
- `hndfwd.h` — Northstar 3-GMAC forwarder architecture doc (Broadcom)
- `b53_srab.c` — mainline Linux SRAB switch-register-bridge driver (for comparison/context)
- `bcm-ns.dtsi`, `bcm4709-netgear-r8000.dts` — mainline device tree, GMAC/SRAB physical addresses

## Key sources (all public)

- Florian Fainelli, "ARM: dts: NSP: Switch to port 8 for CPU port" —
  https://lkml.iu.edu/hypermail/linux/kernel/1803.1/04661.html
- `drivers/net/dsa/b53/b53_common.c`, `b53_srab.c`, `b53_regs.h` — torvalds/linux mainline
- `include/linux/bcma/bcma.h`, `drivers/bcma/scan.c` — torvalds/linux mainline
- `arch/arm/boot/dts/broadcom/{bcm-ns.dtsi,bcm5301x.dtsi,bcm4708.dtsi,bcm4709-netgear-r8000.dts}` — torvalds/linux mainline
- Rafał Miłecki, "Understanding/reimplementing forwarding acceleration used by Broadcom (ctf)", netdev list, 2013 — https://lists.openwall.net/netdev/2013/08/24/13
- GitHub code search hits (leaked Broadcom `et`/HND driver source in vendor GPL forks):
  `comcat/miwifi` (fa_core.h), `Jackysi/advancedtomato-arm` (etc_fa.h), `jeremyd2019/r6300v2`
  (etc_fa.c), `RMerl/asuswrt-merlin.ng` (hndfwd.h, hndctf.h)
- SNBForums, "Broadcom's hardware acceleration" — community NVRAM (`ctf_fa_cap`/`ctf_fa_mode`)
  reverse-engineering, now corroborated by the source above —
  https://www.snbforums.com/threads/broadcoms-hardware-acceleration.18144/
