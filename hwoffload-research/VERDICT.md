# Verdict: open Flow-Accelerator driver for the R8000 (BCM4709/BCM53012)

## TL;DR

**A concrete path exists. This is not a wall.** Three of the four research
threads (`graveyard-vendor`, `hardware-docs`, `graveyard-openwrt`) independently
converged on the same finding: Broadcom's own SDK6 source for the exact SoC
family the R8000 uses (`src-rt-6.x.4708`, `DEVID53012`) ships the **complete,
buildable, GPL-licensed C source** for the FA hardware driver — full register
struct, every bit field, the indirect table-access protocol, the bit-packed
NAPT-row format, and the switch-side enablement sequence. This is not a
register map reconstructed by disassembly guesswork; it is Broadcom's own
driver source, verified in two independent vendor trees (ASUS Merlin,
FreshTomato), for the specific chip in this router.

**But there is one real, unresolved gap, and it is not a documentation gap —
it's a hardware-verification gap:** nobody has ever confirmed that the FA
silicon block is physically present, powered, and functional on an actual
R8000 board. The stock firmware analyzed in `blob-analysis` (build 10.1.88,
May 2024, the exact `.chk` in `images/`) contains **zero** FA-related code —
not a stripped-down reference, not a gated NVRAM path, but a total absence:
no `fa_napt_add`, no `faregs`, no `mem_acc_ctl`, no `ctf_fa_mode` anywhere in
`ctf.ko` or `et.ko`'s symbol tables or strings (re-verified directly against
`blob-analysis/dumps/et_symbols.txt`, `et_strings.txt`, `ctf_symbols.txt`,
`ctf_strings.txt` while writing this verdict — grep for FA/napt/mem_acc/faregs
patterns returns nothing). That is consistent with, and extends, what a real
R8000 owner (Ian Kent, 2015 openwrt-devel) already reported: Netgear's GPL
releases for this router have never included FA code, and he could not
confirm whether the hardware itself is live or fused off. **Register map:
solved. Silicon presence: unverified.** The first step of any real effort has
to be a hardware probe, not driver-writing.

---

## Resolving the apparent contradiction between the four reports

`blob-analysis` concludes "no hardware Flow-Accelerator/NAT-ASIC block at all
exists on this chip." `graveyard-vendor` and `hardware-docs` produced a full
register map and driver source for FA on exactly this chip family. These are
not actually contradictory once precisely stated:

- `blob-analysis` is a claim about **what Netgear shipped in this specific
  firmware build** — true, and now double-checked: zero FA code anywhere in
  the running blob.
- `graveyard-vendor`/`hardware-docs` are claims about **what Broadcom's SDK
  for this chip family supports and what other licensees (Asus, Tomato)
  shipped** — also true, and backed by real source, not speculation.
- Broadcom sells FA as an optional, NVRAM-gated feature of the same silicon
  IP block (`fa_overridden == 2 && ctf_fa_mode != 0` gates `fa_attach()` per
  `hardware-docs` §4d). Netgear's R8000 build simply never turned it on, and
  never linked the code in at all — a **shipping/business decision**, not
  proof the SoC lacks the block. Rafał Miłecki (OpenWrt's own bcm53xx/b53
  maintainer) independently said the same thing about the sibling
  Asus RT-AC88U in 2020: *"I think gmac2 is required if you want to enable
  FA... even though there isn't a Linux driver for it yet"* — i.e. even
  upstream's own expert treats FA on this chip family as real-but-undriven,
  not fictional.

So: the task's original premise ("hardware FA block exists, is that block
recoverable") is **correct for the chip family**, and `blob-analysis`'s literal
finding ("not in this firmware") is also correct and is actually the single
most important data point for planning next steps — it tells you the R8000's
own vendor firmware is useless as a reference implementation to diff against;
you're working purely from the Asus/Tomato SDK6 source plus a cold hardware
probe.

---

## 1. Is there enough to write a real driver?

**Yes, for the register-level interface. Not yet for confirming the silicon
is alive.** Concretely, what's recoverable and where from:

| Component | Status | Source |
|---|---|---|
| FA register struct (`faregs_t`), all bit fields | **Open, complete** | `graveyard-vendor/extracted-source/fa_core.h`, `hardware-docs/extracted-sources/fa_core.h` (identical, Broadcom © 2013) |
| FA base address/offset scheme | **Open, complete, with source-code justification** | `etc_fa.c: fa_corereg()` — `FA_BASE_OFFSET = 0xc00` inside GMAC core-unit 3's window; comment explains why ("GMAC-2 connect FA but FA regs fall in GMAC-3 corereg space") |
| Physical MMIO address on BCM4709 | **Inferred, not datasheet-confirmed** | Combine mainline DT `gmac3` base `0x18027000` (`bcm-ns.dtsi`, verified locally in this repo's built kernel tree) + `FA_BASE_OFFSET 0xc00` → **`0x18027c00`** |
| Indirect 3-table access protocol (select→stage→poll-busy) | **Open, complete, working reference code** | `etc_fa.c` `SELECT_MACC_TABLE_RD/WR()`, `CTF_FA_MACC_RD/WR()` |
| NAPT-row bit-packing (5-tuple + NAT + valid bit) | **Open, complete** | `etc_fa.c: fa_napt_prep_ipv4_word()` (encode), `fa_dump_nf_entry()` (decode) |
| Flow add/delete/live entry points | **Open, complete** | `etc_fa.c: fa_napt_add/del/live()`, called from `et_linux.c: et_fa_normal_cb()` |
| Switch-side enablement (BRCM-HDR tag, OOB pause, CFP FIN/RST mirroring) | **Open, complete** | `bcmrobo.c: robo_fa_enable/aux_init/aux_enable/imp_port_upd()` |
| SRAB bus protocol to reach switch registers | **Open, complete, and already running in mainline** (`drivers/net/dsa/b53/b53_srab.c`) | `bcmrobo.c`, `chipcommonb.h`; base `0x18007000` |
| GMAC/UniMAC reset & speed/duplex control | **Open, complete, and already running in mainline OpenWrt** (`bgmac`/`unimac.h`) — verified bit-for-bit against the blob's `gmac_init_reset` disassembly | `blob-analysis/dumps/et_disasm.txt`, mainline `bgmac.h`/`unimac.h` |
| CTF flow-selection policy (which flows get offloaded) | **Closed** (`hndctf.c` never shipped as source anywhere) | but ABI is fully known (`hndctf.h`), and has a ready-made open substitute: Linux `nf_flowtable` / act_ct hardware-offload hooks |
| Confirmation FA silicon is present/enabled on R8000's specific stepping | **Unknown — never tested by anyone, in or out of tree** | requires a physical probe |
| Chip-rev-specific quirks (e.g. the documented "777 WAR" erratum) | **Partially open** — one rev-2 erratum is documented; whether R8000's stepping needs it or others is unconfirmed | `etc_fa.c: CTF_FA_WAR777_ON/OFF` |

Verdict on Q1: **tractable, not intractable.** The register-level spec is not
"partial" — it's Broadcom's actual driver source for this chip family, cross-
corroborated in two independent vendor trees plus mainline kernel commentary
from Broadcom's own upstream engineer. The only closed component (`hndctf.c`)
is the policy layer, not the hardware interface, and it doesn't need to be
reverse-engineered — it needs to be replaced with `nf_flowtable`, which is a
normal, already-solved integration pattern in current Linux/OpenWrt.

---

## 2. The concrete path

### What to reimplement, and from which files

Target: a new OpenWrt/mainline-style driver, e.g.
`drivers/net/ethernet/broadcom/bgmac-fa.c` (satellite to `bgmac`, not a fork
of it), wired as an `nf_flowtable` / `flow_offload` hardware-offload backend.

1. **Port `fa_core.h` verbatim** as the register struct/bit-field header. It's
   already GPL-compatible C (Broadcom's own permissive header, present in the
   Merlin/Tomato trees) — this is a near-zero-effort, low-risk step.
2. **Port the indirect-table access routines from `etc_fa.c`** — `fa_attach()`
   minus the `robo` handle dependency (reimplement the small piece of
   `robo_fa_enable`/`robo_fa_aux_init` needed to turn on BRCM-HDR tagging via
   SRAB, which is separately fully open in `bcmrobo.c`), `fa_up()`/`fa_down()`
   bring-up sequence, `fa_napt_add()`/`fa_napt_del()`/`fa_napt_live()`, and the
   `fa_napt_prep_ipv4_word()`/`fa_dump_nf_entry()` encode/decode pair.
3. **Wire flow admission to `nf_flowtable` instead of `hndctf`** — implement
   `.ndo_setup_tc` / `flow_offload` hardware-offload callbacks (the same
   pattern MediaTek's PPE offload and other in-tree HW NAT drivers use) that
   call the ported `fa_napt_add/del()` on flow add/teardown events, instead of
   the closed CTF hash table calling them.
4. **Add the switch-side glue** — port `robo_fa_enable/aux_init/aux_enable`
   from `bcmrobo.c` into `b53_srab.c` (or a b53 extension), since SRAB access
   is already fully implemented in mainline and only needs the FA-specific
   register writes (BRCM-HDR enable, OOB pause, CFP TCAM FIN/RST rules) added
   on top — see the concrete page/offset table in
   `graveyard-vendor/notes.md` §4.

   **Reconciliation (2026-07-23): this port is lower-effort than the general
   phrasing above implies.** `bcmrobo.c`'s own SRAB bus layer
   (`srab_wreg`/`srab_rreg`: arbitrate `rcareq`/`rcagnt`, load `wd_h`/`wd_l`,
   issue `cmdstat = (page<<24)|(offset<<16)|gordyn|write`, poll, read
   `rd_h`/`rd_l`) is not something a driver has to re-port at all — mainline
   `b53_srab.c` (`hardware-docs/extracted-sources/b53_srab.c`) already
   implements the byte-identical protocol as generic, page+register-addressed
   primitives (`b53_srab_read{8,16,32,48,64}` /
   `b53_srab_write{8,16,32,48,64}`, wired into `struct b53_io_ops` at
   `b53_srab.c:480-489`) — confirmed function-for-function equivalent to
   `bcmrobo.c`'s `srab_wreg`/`srab_rreg` (same page/offset/go-ready bit
   layout, same `wd_h/wd_l/rd_h/rd_l` register pair). So `robo_fa_enable()`
   (2 register writes: `PAGE_MMR/REG_BRCM_HDR`, `PAGE_FC/REG_FC_OOBPAUSE`)
   and `robo_fa_aux_enable()` (1 write: `PAGE_CFP/REG_CFP_CTL_REG`) reduce to
   direct `dev->ops->write8()` calls against already-running mainline
   infrastructure — no new bus driver, no new arbitration/polling logic, just
   new page/offset constants. Only `robo_fa_aux_init()`'s CFP TCAM programming
   (4 rules via the `REG_CFPTCAM_ACC` indirect command register) needs its own
   new indirect-access helper, structurally identical to (and reusable
   alongside) the FA table-access pattern in step 2 above — b53 doesn't
   already have a CFP TCAM helper, since mainline b53 never drives FA. Net
   effect: the switch-side glue step is almost entirely "add page/offset
   defines + a handful of `write8()` calls to existing b53 ops," not a
   from-scratch register-access port — tightens the Medium-Low estimate in
   the effort table below toward Low.
5. **Do not touch `et.ko`/GMAC-3's regular UniMAC path** — it's unrelated;
   `bgmac` already owns it.

### Effort/risk breakdown

| Phase | Effort | Risk |
|---|---|---|
| Port `fa_core.h`, table-access protocol, row encode/decode | Low | Low — it's a straight C port of open, complete source |
| Hardware probe to confirm FA presence/address on real R8000 silicon | Low effort, **high uncertainty of outcome** | **This is the actual risk of the whole project** — if the block is fused off or absent on this SKU, nothing downstream matters |
| SRAB/switch-side enablement port | Low-Medium | Low — SRAB itself is proven, mainline-driven hardware on this exact board |
| `nf_flowtable` integration (replacing closed `hndctf` policy) | Medium | Medium — well-trodden pattern elsewhere in Linux, but new code for this driver |
| Chip-rev quirk handling (777 WAR and any undiscovered R8000-specific errata) | Unknown until probed | Medium — the leaked source spans multiple Northstar steppings; R8000's own quirks aren't validated anywhere |
| Stability/production hardening (concurrent SRAB bus arbitration with b53, ECC error handling, ageing under real traffic) | Medium-High | Medium — normal driver-maturity work once the above is proven |

None of these are reverse-engineering-from-nothing effort. All of them are
either "port working reference C" or "normal new Linux driver integration
work." The schedule risk is concentrated entirely in the first probe step.

### Minimal first-step proof-of-concept

Goal: determine, non-destructively, whether anything real answers at the
inferred FA register base on a live R8000 board — before investing in the
rest of the driver.

1. **Confirm `gmac3`/FA's MMIO window isn't already exclusively claimed.**
   R8000's own board DTS (`bcm4709-netgear-r8000.dts`, this repo's build tree)
   does **not** override `gmac0..gmac3` at all — only `&srab`, `&pcie0/1`,
   `&usb2/3`. All four `gmac0..gmac3` nodes in `bcm-ns.dtsi` have no `status`
   property (defaults to enabled), and `bgmac` will attempt to probe all four.
   Critically: `gmac3`'s DT `reg` is only `<0x27000 0x800>` — i.e. `bgmac`, if
   it reserves that region at all, only reserves the **first 2 KB** of the
   4 KB physical core window. FA's registers start at offset `0xc00` (3 KB
   in), **outside `bgmac`'s declared/reserved range** — so a standalone probe
   module can `ioremap(0x18027c00, 0x100)` directly without any
   `request_mem_region` conflict with `bgmac`, regardless of whether `bgmac`
   successfully binds `gmac3` as a (probably link-down, unused) netdev.
2. **Write a 30-line out-of-tree kernel module** (or a `devmem2`/`busybox
   devmem` one-liner, if `/dev/mem` + `CONFIG_STRICT_DEVMEM` allow it on the
   already-built OpenWrt image — `images/openwrt-25.12.5-*-netgear_r8000-*.chk`
   is already flashable) that:
   - `ioremap(0x18027c00, 0x100)` (covers `control` through the `status`
     registers per the `faregs_t` layout in `fa_core.h`).
   - Reads `status` (offset `0x30` from FA base, i.e. phys `0x18027c30`) and
     `control` (offset `0x00`, phys `0x18027c00`).
   - **Interpretation**: all-`0xffffffff` or a bus-fault/abort on read strongly
     suggests the block is unclocked/unpowered/absent on this SKU. A plausible
     non-`0xffffffff`, non-zero value — especially one that changes when you
     toggle NVRAM-equivalent state or issue the `CTF_CTL_NAPT_FLOW_INIT` strobe
     from `fa_core.h` and re-read `status` for `CTF_INTSTAT_INIT_DONE` — is
     strong evidence the silicon is alive and matches the leaked source's
     documented behavior.
3. **If alive**: proceed to `fa_up()`'s bring-up sequence (strobe all 5
   table-init bits, poll `INTSTAT_INIT_DONE`), then attempt one static NAPT
   flow insertion via the indirect `mem_acc_ctl`/`m_accdata[]` write sequence
   for a hand-crafted 5-tuple, and verify with a packet from a LAN client
   through port 8 that hardware forwarding actually happens (e.g. via `stats[]`
   HIT counter increment at FA base `+0x3c`, before wiring up any real
   flow-offload integration).
4. **If dead** (bus fault, all-Fs, no state change under init strobe): that
   is itself a definitive, useful, and cheap result — it converts "unverified"
   into "confirmed absent on this SKU," closing the project with real evidence
   rather than leaving it as permanent vaporware speculation the way the 2015
   Ian Kent thread did.

This whole first step is read/write to a handful of MMIO registers on
hardware the operator already owns and already has working OpenWrt images
for (`images/openwrt-25.12.5-*-netgear_r8000-*.chk`) — it requires no new
firmware build system work, no kernel patching yet, just a probe module.

---

## 3. If it turns out to be a wall — name exactly what would be missing

Assuming the step-2 probe comes back dead (FA absent/unpowered on this SKU),
the wall is **narrow and specific**, not "everything is closed":

- **Not missing**: the register map, the table-access protocol, the row
  format, the switch enablement sequence, the SRAB bus protocol — all fully
  open per §1's table above, regardless of probe outcome.
- **Missing and would stay missing**: proof that the R8000's specific BCM4709
  die/package has the FA IP block fused on. Broadcom silicon families
  routinely ship the same base die across SKUs with features fused off for
  product segmentation — there is no public confirmation either way for the
  53012/"High SKU" part specifically, only Ian Kent's 2015 "guessing game"
  and Miłecki's 2020 "I think... even though there isn't a Linux driver for
  it yet," neither of which is a hardware-verified answer.
- **Missing regardless of fusing**: R8000-specific chip-revision quirks
  analogous to the documented "777 WAR" erratum — the leaked source spans
  several Northstar steppings and explicitly chip-rev-gates at least one
  known silicon bug; nothing public confirms which quirks (if any) apply to
  the R8000's actual `BCM53012 rev 5` (per the boot-log string
  `hardware-docs` cites: `found switch: BCM53012, rev 5`).

None of these missing pieces are "specific registers/opcodes/format only
present in the closed `ctf.ko`" — because, as `blob-analysis` proved by direct
disassembly, `ctf.ko` has **zero hardware-register content of any kind**
(no `ioremap`/`readl`/`writel`/`si_*` in its entire symbol table). The closed
`ctf.ko` in this firmware is 100% a CPU-side software hash table; it was never
where the hardware format lived. The FA hardware format lives, fully
documented, in the *other* SDK licensees' open `etc_fa.c`/`fa_core.h` — the
task's original assumption that the format is "only present in the closed
ctf.ko" does not hold; that closed module simply never had it.

---

## Recommended next step

Run the §2 MMIO probe (`ioremap(0x18027c00, ...)`, read `control`/`status`,
attempt the init-strobe/poll sequence) against real R8000 hardware, using the
already-built `images/openwrt-25.12.5-*-netgear_r8000-*.chk` as the base to
add a tiny out-of-tree probe module to. This is the one experiment that
converts the entire project from "well-documented but unverified" to either
"confirmed live, proceed to full driver per the §2 plan" or "confirmed absent,
stop here with a real, board-specific negative result" — and it costs a
single afternoon of hands-on hardware time, not a reverse-engineering
campaign.
