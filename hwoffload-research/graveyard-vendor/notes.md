# Vendor GPL mining: Broadcom Northstar (BCM4709) Flow-Accelerator / CTF hardware offload

Target: Netgear R8000 (BCM4709 "Northstar", dual Cortex-A9). Goal was to find, in
vendor GPL trees, the parts of the FA (Flow Accelerator hardware block) / CTF
(Cut-Through Forwarding) NAT-acceleration stack that are open source, and extract
the concrete register definitions + flow-insertion mechanism usable to seed an
open OpenWrt/Linux driver.

**Result: much more is open than expected.** Only the software CTF flow-cache
core (`hndctf.c` — the hash table / connection-tracking algorithm that decides
*which* flows are hot enough to accelerate) is closed. Everything downstream of
that decision — the entire FA hardware register interface, the NAPT/next-hop/pool
table programming, the switch-side (integrated BCM5301x ROBO switch) register
writes needed to route traffic through the FA block, and the SRAB (Switch
Register Access Bridge) used to reach the switch's register space from the ARM
CPU — is present as plain, compilable, GPL C source.

## Where it was found

Primary source (fully mirrors the original Netgear/Broadcom "release" tree
layout used by Tomato-derived firmwares):

- **`RMerl/asuswrt-merlin`** (github.com/RMerl/asuswrt-merlin), branch `master`,
  directory **`release/src-rt-6.x.4708/`** — this is the SDK6 tree for
  BCM4708/4709 ("Northstar" / `src-rt-6.x.4708`), i.e. exactly the R8000's SoC
  family. `include/bcmrobo.h` confirms this: `DEVID53012 = 0x53012 /* BCM4709,
  High SKU device */` and `ROBO_IS_BCM5301X()`.
- **Cross-checked and corroborated** against `FreshTomato-Project/freshtomato-arm`
  (branch `arm-master`), same path `release/src-rt-6.x.4708/`. File set is
  byte-for-byte identical in layout (same 16 files under `et/sys/`), confirming
  this isn't a Merlin-specific addition — it's what Broadcom shipped in the SDK6
  GPL drop to every Northstar licensee.
- **Negative result, for contrast:** `hajuuk/R7000` (a full Netgear R7000 GPL
  dump, same SoC family, different/newer firmware build) ships this same module
  set (`drivers/net/ctf`, `drivers/net/et`, `drivers/net/emf`, `src/router/ctf`,
  `src/router/emf`) as **prebuilt `.o`/`.ko` blobs only** — no `.c` source, with
  the kbuild `Makefile` falling back to `$(CTFSRC)/linux/ctf.o` when no `.c` is
  present. This is a real per-release/per-vendor choice: Netgear's own R7000 GPL
  archive for that build did not include what Asus's and Tomato's GPL archives
  did. **Conclusion for future mining: prefer the Asus/Tomato `release/src-rt-*`
  trees over Netgear's own R7000/R8000 tarballs** — same SDK, more complete
  disclosure.
- Not yet needed/pulled: DD-WRT mirror, Asus RT-AC88U GPL, Netgear's own R8000
  tarball — the Merlin SDK6 tree already gave complete, buildable source for
  every register-level piece requested. (`asuswrt-merlin.ng`'s newer HND/SDK7
  tree exists too, for reference, at `release/src-rt/` in that repo, gated by
  `BCMFA=n` in `target.mak` by default — not pulled since SDK6 already answered
  the question.)

## Directory map (relative to `release/src-rt-6.x.4708/` in the repos above)

```
et/sys/etc_fa.c        1971 lines  FULL C source: FA hardware driver (register-level)
et/sys/etc_fa.h         113 lines  fa_t public API declarations
et/include/fa_core.h    261 lines  FA register map (faregs_t) + all control bit defs
et/sys/et_linux.c      3287 lines  Linux et driver; ctf_forward()/fa_process_*() call sites
et/sys/et_export.h       54 lines  et_fa_* glue prototypes (lock, dev-on flag, proc fs)
et/sys/etc.c            (et common HW driver; etc->fa handle, /proc fa dump ioctls)
et/sys/etcgmac.c        2098 lines  GMAC/UniMAC core driver for the Ethernet MACs
include/ctf/hndctf.h     398 lines  CTF public struct/API (ctf_t, ctf_ipc_t, function-ptr table)
include/ctf/ctf_cfg.h     78 lines  CTF Kconfig-style feature defines
include/bcmrobo.h        188 lines  ROBO switch driver struct, SRAB register struct, DEVIDs
shared/bcmrobo.c        3118 lines  FULL C source: integrated switch driver (incl. FA glue)
include/chipcommonb.h    127 lines  ChipCommonB core register offsets incl. SRAB base
```

All of the above is copied verbatim into `extracted-source/` next to this file
(license headers intact — Broadcom's own permissive-looking "Copyright (C)
20xx, Broadcom Corporation... Permission to use, copy, modify, and/or
distribute this software for any purpose with or without fee is hereby
granted..." notice, present at the top of every file listed).

## What is genuinely still closed

`ctf_attach()` / `_ctf_attach()` / `ctf_kattach()` and the `ctf_fn_t` vtable
implementation itself (the file that would be `hndctf.c`) — the software
flow-cache: hash table of `ctf_ipc_t` 5-tuple entries, connection add/delete/
lookup, bridge-cache (`ctf_brc_t`), and the "liveness" aging logic — is
**not** present as source in any of the trees checked. It ships only as
`ctf.o` / `ctf.ko`. `include/ctf/hndctf.h` gives you the full public struct
layout and the calling convention (a function-pointer table hanging off
`ctf_t.fn`, populated at attach time via `ctf_attach_fn`), so the ABI is known,
but the hashing/aging implementation itself is opaque.

This matches the task's starting assumption almost exactly — it's just that the
"only the core ctf algorithm is closed" line turns out to be literally true,
down to the single missing file.

## The mechanism, end to end (now fully documented from open source)

### 1. Software/hardware split (`et_linux.c`)

- Every ethernet driver instance (`et`) that supports FA registers itself with
  the closed CTF core: `ctf_fa_register(et->cih, et_fa_normal_cb, dev)`
  (et_linux.c:738) if the device is FA-capable, else a no-op
  `et_fa_default_cb` (et_linux.c:741).
- Fast-path RX still goes through the closed software CTF forwarder first —
  `et_ctf_forward()` calls `ctf_forward(et->cih, skb, skb->dev)`
  (et_linux.c:2231, 2708) — CTF's *own* logic decides whether a flow is a
  forwarding candidate.
- When CTF decides to **also** offload a flow to hardware, it calls back into
  `et_fa_normal_cb()` (et_linux.c:3236) with one of four commands:
  `FA_CB_ADD_NAPT`, `FA_CB_DEL_NAPT`, `FA_CB_GET_LIVE`, `FA_CB_CONNTRACK` — each
  dispatches straight into the fully-open `fa_napt_add()` / `fa_napt_del()` /
  `fa_napt_live()` / `fa_conntrack()` in `etc_fa.c`.
- On RX, if BRCM_HDR mode is active, `fa_process_rx()` strips/parses a 4- or
  8-byte "Broadcom header" prepended by hardware that encodes hit/miss result
  and the hardware flow-table index (`bcm_hdr_t`, a packed bitfield in
  `etc_fa.h`); on TX, `fa_process_tx()` prepends a 4-byte placeholder header.

### 2. FA hardware register block (`fa_core.h`)

The FA registers live inside GMAC core's address space at a fixed offset:
```c
#define FA_BASE_OFFSET   0xc00        /* fa_core.h:24 */
```
`fa_corereg()` in `etc_fa.c` (line 379) resolves the actual virtual address:
GMAC core unit 2 is the port physically wired to FA, but **the FA register
window is mapped through GMAC core unit 3's address space** (comment in source:
*"GMAC-2 connect FA but FA regs fall in GMAC-3 corereg space so using GMAC-3 as
base"*) — i.e. `si_setcore(sih, GMAC_CORE_ID, 3)` then `+ FA_BASE_OFFSET`.
GMAC core register layout in `gmac_core.h` shows UniMAC registers start at
`0x800`, so FA control registers sit right after the MAC's own register block.

Register struct (`faregs_t`, all 32-bit, offsets from `FA_BASE_OFFSET`):

| offset | field | purpose |
|---|---|---|
| 0x00 | `control` | mode bits, table-init strobes (see below) |
| 0x04 | `mem_acc_ctl` | indirect table-access controller (index/select/R-W) |
| 0x08 | `bcm_hdr_ctl` | enable/parse the 4-8 byte hardware header |
| 0x0c | `l2_skip_ctl` | L2 header skip / SNAP conversion |
| 0x10 | `l2_tag` | expected 802.1Q tag ethertype(s) |
| 0x14 | `l2_llc_max_len` | LLC/SNAP max length |
| 0x18/0x1c | `l2_snap_typelo/hi` | SNAP type match |
| 0x20 | `l2_ethtype` | IPv4/IPv6 ethertype match |
| 0x24 | `l3_ipv6_type` | expected IPv6 next-header (TCP=6/UDP=0x11) |
| 0x28 | `l3_ipv4_type` | expected IPv4 protocol + `CTF_L3_IPV4_CKSUM_EN` |
| 0x2c | `l3_napt_ctl` | hash select, hash seed, timestamp epoch (aging) |
| 0x30/0x34 | `status`/`status_mask` | init-done + parse-error interrupt bits |
| 0x38 | `rcv_status_en` | which L2/L3 parse-fail conditions raise status |
| 0x3c..0x60 | `stats[10]` | HIT/MISS/SNAP_FAIL/.../V4HL_FAIL hit counters |
| 0x64/0x68 | `error`/`error_mask` | HWQ/RXQ/TXQ/LAB/HB overflow flags |
| 0x6c/0x70 | `dbg_ctl`/`dbg_status` | force-hit/force-miss debug, `MEM_ACC_BUSY` |
| 0x74..0x94 | ECC debug/error regs | 5x ECC syndrome capture registers |
| 0x98/0x9c | `hwq_max_depth`/`lab_max_depth` | queue-depth high-water marks |
| 0xa0..0xa7 | `m_accdata[8]` | 8x32-bit indirect data window for table R/W |

Key bit definitions (all in `fa_core.h`):
- `CTF_CTL_SW_ACC_MODE` / `CTF_CTL_BYPASS_CTF` — mode select (see `fa_setmode()`)
- `CTF_CTL_NAPT_FLOW_INIT | NEXT_HOP_INIT | HWQ_INIT | LAB_INIT | HB_INIT` — one-shot
  table-clear strobes, self-clearing; poll `status & CTF_INTSTAT_INIT_DONE` (all
  5 done-bits) to confirm.
- `CTF_MEMACC_TBL_NF=0 / NP=1 / NH=2` (flow / pool / next-hop) select which of
  the three indirect tables `mem_acc_ctl` addresses; `CTF_MEMACC_RD_WR_N` bit12
  chooses read vs write; low 10 bits are the row index.
- Three tables total: **NAPT flow table** (1024 entries, `CTF_MAX_FLOW_TABLE`),
  **next-hop table** (128 entries, `CTF_MAX_NEXTHOP_TABLE_INDEX`), **pool
  table** (4 entries, `CTF_MAX_POOL_TABLE_INDEX`).

### 3. Flow-insertion mechanism (`etc_fa.c`, fully open)

Indirect-table access pattern used for every table (flow/next-hop/pool):
```c
W_REG(osh, &regs->mem_acc_ctl, CTF_MEMACC_WR_TABLE(table, index)); /* select row */
for (i = n; i; i--) W_REG(osh, &regs->m_accdata[i-1], d[i-1]);     /* stage data */
/* poll regs->dbg_status & CTF_DBG_MEM_ACC_BUSY until clear (10ms timeout) */
```
(Read is the mirror: select with `CTF_MEMACC_RD_TABLE`, then read back
`m_accdata[]`.) This exact 3-step dance (select → stage/read data words →
poll busy) is the entire "flow-insertion mechanism" the task asked for, and
it's identical for all three tables.

`fa_napt_add()` (etc_fa.c:1515) is the top-level entry point CTF calls to
offload a flow:
1. Compute the flow-table row index. Two hashing modes exist, selected at
   compile time by `BCMFA_HW_HASH`:
   - HW hash (`CTF_CTL_L3NAPT_HASH_SEL`, `CTFCTL_L3NAPT_HASH_SEED`): the
     hardware itself hashes and the row index arrives back in the RX
     Broadcom-header (`bcm_hdr_t.oc10.napt_flow_id`/`bkt_id`, 4-way bucketed:
     `CTF_MAX_BUCKET_INDEX=4`).
   - SW hash (default): CRC-CCITT (poly 0x1021) over
     `1-bit-LSH(sip‖dip‖sport‖dport) ‖ tcp-flag`, low byte only
     (`fa_crc_ccitt()`, etc_fa.c:350) — 256 buckets × 4 ways = 1024 rows,
     matching `CTF_MAX_FLOW_TABLE`.
2. Resolve/allocate a **next-hop entry** (dest MAC + VLAN tag-op +
   L2-frame-type) via `fa_add_nhop_entry()` — a small free-list allocator over
   128 slots, reference-counted so multiple flows to the same next-hop MAC
   share one entry.
3. Resolve/allocate a **pool entry** (source-MAC remap, "internal"=LAN-side vs
   "external"=WAN-side, `CTF_NP_INTERNAL`/`CTF_NP_EXTERNAL`) — only 4 slots,
   effectively one internal + one external per FA-capable interface pair.
4. Pack the 8×32-bit NAPT flow-table row (`fa_napt_prep_ipv4_word()`,
   etc_fa.c:825) — bit-packed 5-tuple + NAT IP/port + action flags + valid bit
   (`1<<20`) + `ipv4_entry` flag (`tbl[7] bit31`). The exact bit layout is
   fully worked out in that function and in the inverse
   `fa_dump_nf_entry()` (used for `/proc` dumps) — this is a directly portable
   spec for an OpenWrt driver's table-encode/decode routines.
5. Write the row via the indirect-access pattern above.
Deletion (`fa_napt_del()`/`_fa_napt_del()`) reads the row back, clears the
valid bit (`tbl[vidx] &= ~(1<<20)`), writes it back, and releases the next-hop
reference count (freeing the next-hop slot only when refcount hits 0).

`fa_up()`/`fa_down()` (etc_fa.c:950/891) show the full bring-up/mode-switch
sequence: set mode bits, strobe all 5 table-init bits, spin-wait on
`INTSTAT_INIT_DONE`, program L2-skip/L3-NAPT-ctl/IPv4-checksum-enable, then
(critically) call into the **switch driver** to flip on BRCM-HDR tagging and
CFP redirection — this is the wired-to-switch half of the mechanism, below.

There is also a documented **hardware erratum workaround** ("777 WAR",
`CTF_FA_WAR777_ON/OFF`) present verbatim in source: on chip rev 2 of BCM4707
specifically, `HWQ_THRESHLD` must be forced to 0 before any indirect table
access and restored to `0x140` after, with a 1ms delay — real silicon-rev
gotcha, worth carrying into any reimplementation targeting early R8000
hardware revisions.

### 4. Switch-side glue (`bcmrobo.c`, fully open) — the "switch NATP register
   programming" the task asked about

The R8000's integrated switch is a BCM5301x-family ROBO switch
(`DEVID53012` for the BCM4709 "high SKU", confirmed in `bcmrobo.h`). FA-related
switch functions (all `#if !defined(_CFE_) && defined(BCMFA)`):

- **`robo_fa_enable(robo, on, bhdr)`** (bcmrobo.c:1714) — the actual "NATP
  register" analog: on enable, writes `PAGE_MMR(0x02) / REG_BRCM_HDR(0x03)` = 1
  to turn on Broadcom-header tagging on the IMP (CPU-facing) port, and sets bit
  8 of `PAGE_FC(0x0a) / REG_FC_OOBPAUSE(0xe0)` to switch flow control to
  out-of-band signaling (required so the switch and FA's own queue backpressure
  can talk to each other without eating into the Ethernet frame flow-control
  path). Both are 1-2 register single-page/offset writes.
- **`robo_fa_aux_init(robo)`** (bcmrobo.c:1611) — sets up the switch's CFP
  (Content-aware Field Processor / TCAM classifier) to detect **TCP FIN/RST**
  packets and mirror them to an "aux" port/interface so software conntrack sees
  connection teardown even though the data path bypasses Linux via FA. Programs
  4 TCAM rules (IPv4 FIN, IPv4 RST, IPv6 FIN, IPv6 RST) via `PAGE_CFPTCAM(0xa0)`
  and `PAGE_CFP(0xa1)` registers, using an indirect access-and-poll pattern
  nearly identical in shape to the FA table access above
  (`REG_CFPTCAM_ACC` command register with `CFP_ACC_XCESS_ADDR/RAM_SEL/OP_SEL`
  fields, `CFP_ACC_OP_STR_DONE` strobe, then `CFP_ACC_RD_STS_WAIT` poll macro).
  It also programs a GMII port-override register
  (`REG_CTRL_PORT0_GMIIPO(0x58)+aux_pid`) to force the aux port link
  up/2000Mbps/full-duplex in software (there's no PHY on that logical port).
- **`robo_fa_aux_enable(robo, enable)`** (bcmrobo.c:1680) — toggles CFP
  matching on/off per physical port via `PAGE_CFP / REG_CFP_CTL_REG(0x00)`,
  bitmap of ports 0-4 (`phy_portmap`, extendable with an RGMII port via nvram
  `rgmii_port` when `RGMII_BCM_FA` is defined).
- **`robo_fa_imp_port_upd()`** (bcmrobo.c:1350) — on BCM4707-family chips with
  FA active, remaps the CPU (IMP) port index used for VLAN/port-tag
  configuration to the last port in the descriptor table (the FA-dedicated
  "aux" GMAC), so the normal VLAN-config code path transparently targets the
  right physical port without special-casing FA elsewhere.

Full switch register **page/offset map** extracted from `bcmrobo.c` (defines
at file top, ~line 60-215) — this is the general BCM5301x-integrated-switch
register map, not FA-specific, and is independently useful for a b53-style
driver on this platform:

```
PAGE_CTRL      0x00   PAGE_STATUS   0x01   PAGE_MMR    0x02
PAGE_VTBL      0x05   PAGE_FC       0x0a   PAGE_VLAN   0x34
PAGE_CFPTCAM   0xa0   PAGE_CFP      0xa1

REG_CTRL_PORT0..7   0x00-0x07   REG_CTRL_IMP        0x08
REG_CTRL_MODE       0x0B        REG_CTRL_SRST        0x79
REG_CTRL_PORT0_GMIIPO..PORT7_GMIIPO   0x58-0x5f (53012 GMII port-state override)
REG_MGMT_CFG 0x00 / REG_IMP0_PORT 0x01 / REG_IMP1_PORT 0x02 / REG_BRCM_HDR 0x03  (page MMR)
REG_VLAN_CTRL0/1/4/5, REG_VLAN_ACCESS(0x06), REG_VLAN_WRITE(0x08), REG_VLAN_READ(0x0c)
REG_VLAN_PTAG0..8   0x10,0x12,...,0x20   (per-port default tag, page VLAN)
REG_VTBL_CTRL(0x00)/MINDX(0x02)/ARL_E0(0x10)/SCTRL(0x20)/SADDR(0x22)/SRES(0x24)  (ARL/L2 table, page VTBL)
REG_FC_OOBPAUSE     0xe0        (page FC)
REG_CFPTCAM_ACC     0x00, DATA0-7 0x10-0x2c, MASK0-7 0x30-0x4c,
                     ACT_POL_DATA0/1 0x50/0x54, RATE_METER0/1 0x60/0x64,
                     RATE_INBAND/OUTBAND 0x70/0x74           (page CFPTCAM)
REG_CFP_CTL_REG     0x00, UDF_{0,1,2}_A/B/C_0_8 (slice UDF windows), UDF_0_D_0_11  (page CFP)
```

### 5. SRAB — how the ARM CPU reaches those switch registers at all

On BCM4707/4708/4709, the switch is reached over the **ChipCommonB SRAB**
(Switch Register Access Bridge), not SPI/MDIO like older Broadcom platforms.
Fully open in `bcmrobo.c` (`#ifdef ROBO_SRAB`, ~line 668-940) +
`chipcommonb.h` + `bcmrobo.h`:

```c
#define NS_CHIPCB_SRAB   0x18007000     /* ChipCommonB core, physical/AXI addr */
```
8-register MMIO window (`srabregs_t`, offsets from `NS_CHIPCB_SRAB`):
```
0x2c  cmdstat   command/status: [31:24]=page [23:16]=offset [bit1]=write [bit0]=go/ready
0x30  wd_h      write-data high 32 bits (for 6/8-byte values, e.g. MAC addrs)
0x34  wd_l      write-data low 32 bits
0x38  rd_h      read-data high 32 bits
0x3c  rd_l      read-data low 32 bits
0x40  ctrls     rcareq/rcagnt (bus-arbitration request/grant), sw_init_done
0x44  intr      interrupt-pulse capture from the switch
```
Access sequence (`srab_wreg`/`srab_rreg`, bcmrobo.c:761/845):
1. `ctrls |= rcareq` then spin until `ctrls & rcagnt` (arbitrate for the shared
   SRAB bus against other masters).
2. Load `wd_h`/`wd_l` with the value (byte-swizzled per length: 1/2/4/6/8
   bytes supported — 6/8 for MAC-address-sized registers).
3. Write `cmdstat = (page<<24)|(offset<<16)|gordyn|write` to issue the
   transaction; for reads omit the `write` bit.
4. Spin (up to 1000 retries) until `cmdstat & gordyn` clears; on timeout, call
   `srab_interface_reset()` (sets `cmdstat` reset bit, waits for it to
   self-clear, and separately waits for `ctrls & sw_init_done`).
5. For reads, pull the result from `rd_h`/`rd_l`.
6. Release the bus: `ctrls &= ~rcareq`.

This SRAB protocol (independent of FA — it's the *general* mechanism for any
register access to the integrated switch) is corroborated internally:
`chipcommonb.h` gives `CHIPCB_SRAB_CMDSTAT_OFFSET = 0x2c` and
`CHIPCB_SRAB_RDL_OFFSET = 0x3c`, matching the struct layout exactly, and is
independently used elsewhere in `etc_fa.c`'s `fa_chip_rev()` (bcmrobo.c-adjacent
code in etc_fa.c:1256) to read out chip revision via a raw one-off SRAB
transaction, without going through the `bcm_robo_*` wrapper at all — i.e. two
independent call sites in two different open files agree on the protocol.

## Direct implication for an OpenWrt driver

Everything needed to reimplement FA as an open driver against a from-scratch
software-CTF-equivalent (e.g. driving flow insertion straight from OpenWrt's
own conntrack/nf_flow_offload hooks instead of Broadcom's closed hash-table
core) is now in hand:
- Full FA register map + control bits (`fa_core.h`).
- Full indirect table-access protocol + working example code for all 3 tables
  (`etc_fa.c`).
- Exact bit-packed row format for NAPT/next-hop/pool entries (encode in
  `fa_napt_prep_ipv4_word`/`fa_add_nhop_entry`/`fa_add_pool_entry`, decode in
  `fa_dump_*_entry`).
- Full switch-side enablement sequence (BRCM-HDR tag, OOB pause, CFP TCAM aux
  mirroring for FIN/RST) with register pages/offsets.
- Full SRAB bus protocol to actually reach those switch registers from Linux,
  base address included.
- The one thing that must be built fresh rather than ported: the *policy*
  layer that decides which flows are hot enough to offload (closed
  `hndctf.c`) — but that's exactly the job nf_flow_offload/act_ct-style
  fastpath frameworks in current upstream Linux already do, so it's a matter
  of wiring a new `net/whatever/bcm_fa.c` offload backend into that existing
  framework rather than reverse-engineering a black box.

## Files

- `extracted-source/etc_fa.c`, `etc_fa.h`, `fa_core.h` — the FA hardware driver.
- `extracted-source/et_export.h` — os-glue prototypes the FA driver depends on
  (see `et_linux.c` for the implementations, copied in full for the call-site
  context around lines 718-756, 2185-2231, 2693-2802, 3188-3264).
- `extracted-source/et_linux.c`, `etc.c`, `etc.h`, `etcgmac.c`, `etcgmac.h`,
  `etc47xx.c`, `etc_adm.c`, `etc_adm.h` — full et/GMAC driver for context
  (packet path, ioctl surface incl. `/proc` FA dump, register glue).
- `extracted-source/hndctf.h`, `ctf_cfg.h` — CTF public ABI (struct layout,
  vtable, IPv4/IPv6 tuple format) — the contract the closed `hndctf.o` honors.
- `extracted-source/bcmrobo.c`, `bcmrobo.h` — full switch driver incl. SRAB bus
  protocol and all FA-related switch register programming.
- `extracted-source/chipcommonb.h` — SRAB base address / offsets.
- `extracted-source/gmac_core.h`, `gmac_common.h` — GMAC/UniMAC register layout
  (context for where FA's `0xc00` offset sits relative to the MAC registers).

## Source repos (for re-fetching / going further)

- https://github.com/RMerl/asuswrt-merlin — `release/src-rt-6.x.4708/` (primary,
  used here) — legacy SDK6, BCM4708/4709. Also has `release/src-rt-7.14.114.x/`
  and `release/src-rt-7.x.main/` (newer SDK7 trees, other chip families) and
  `release/src-rt/` (HND/SDK7 ARM, `asuswrt-merlin.ng` fork has the same path)
  for later Broadcom chips — not needed for R8000/BCM4709 but present if a
  later-generation comparison is ever wanted.
- https://github.com/FreshTomato-Project/freshtomato-arm — `arm-master` branch,
  identical `release/src-rt-6.x.4708/` tree, used to corroborate.
- https://github.com/hajuuk/R7000 — genuine Netgear R7000 GPL dump; documents
  the *negative* case (same modules shipped as prebuilt blobs instead of
  source) and is useful only as a reminder that vendor choice of disclosure
  completeness varies release to release, not just company to company.
