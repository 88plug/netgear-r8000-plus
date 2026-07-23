# FA/CTF control-register bring-up — RESULT (2026-07-23)

## Verdict: silicon presence CONFIRMED. Table-init control-plane works.

This closes the exact gap `VERDICT.md`'s TL;DR called out as unresolved:
*"nobody has ever confirmed that the FA silicon block is physically present,
powered, and functional on an actual R8000 board."* It now has been.

## What was done

`fa_bringup.c` (new module, GMAC-side FA control register only — see its own
file header for full scope/risk-assessment) performed exactly the
`fa_setmode()`-equivalent sequence from Broadcom's own SDK6 `etc_fa.c`
(`mode == CTF_FA_NORMAL` branch):

1. Read `control` before write: `0x00001400` — matches every prior probe
   read, byte-identical across two more flash/reboot cycles since
   `GO_NOGO_BRINGUP.md`'s last check.
2. Wrote `control = 0x0007f408` (pre-existing value OR'd with
   `CTF_CTL_DSBL_MAC_DA_CHECK | CTF_CTL_NAPT_FLOW_INIT | CTF_CTL_NEXT_HOP_INIT
   | CTF_CTL_HWQ_INIT | CTF_CTL_LAB_INIT | CTF_CTL_HB_INIT | CTF_CTL_CRC_OWRT`).
3. Polled `status` for `CTF_INTSTAT_INIT_DONE` (bounded, 1ms steps, 100ms cap).

## Result

```
control BEFORE write = 0x00001400
writing control = 0x0007f408
status AFTER write/poll = 0x000003e2   (waited ~1ms)
CTF_INTSTAT_INIT_DONE asserted (all 5 init bits set)
```

`0x3e2` decodes to bits 1, 5, 6, 7, 8, 9 — i.e. every one of
`CTF_INTSTAT_NAPT_FLOW_INIT_DONE`, `CTF_INTSTAT_NXT_HOP_INIT_DONE`,
`CTF_INTSTAT_HWQ_INIT_DONE`, `CTF_INTSTAT_LAB_INIT_DONE`,
`CTF_INTSTAT_HB_INIT_DONE` flipped from 0 to 1 in response to their
respective control bits, within 1ms — five independent sub-operations,
each answering correctly and individually. That is not a fixed/floating
pattern; a stuck or undriven register cannot selectively assert five
different bits in response to five different control inputs.

**On unload** (`rmmod`), read back `control = 0x00041408` before the
module's revert-write: the five `_INIT` strobe bits had already
self-cleared to 0 on their own, while the two persistent config bits
(`DSBL_MAC_DA_CHECK`, `CRC_OWRT`) remained set — exactly the behavior
expected of a well-designed register that distinguishes one-shot
strobe bits from persistent mode bits. The module's revert-write then
restored `control` to the original `0x00001400`. A follow-up read via
`fa_probe.ko` confirmed: `control=0x00001400 status=0x000003e2` — control
fully restored, `status` still correctly showing the tables as
initialized (clearing the strobe bits doesn't un-initialize them, which
is exactly right).

**System stability, checked immediately before and after:** uptime
unbroken (1h06m throughout), 0% ping loss, no new dmesg errors/warnings,
all 4 WiFi SSIDs (3 main + guest) still up and unaffected. Zero adverse
effect from this write.

## What this does NOT yet confirm

- **NAPT/next-hop table row programming** (`mem_acc_ctl`/`m_accdata[]`
  indirect writes, the WAR777 workaround path) — not attempted this pass,
  deliberately out of scope. This is the actual data-plane write path and
  is a further, separate escalation.
- **Switch-side enablement** (`robo_fa_enable()` over SRAB) — discovered
  mid-implementation that a complete `fa_up()` also toggles this, on a
  different hardware subsystem (the DSA switch, carrying all LAN port
  traffic) than the isolated GMAC-3 FA sub-block tested here. Explicitly
  not touched — this needs its own separate go/no-go, the same way this
  step needed its own separate go-ahead from the read-only probe.
- **Real traffic acceleration** — nothing here proves a packet would
  actually get hardware-forwarded faster; it proves the control plane
  (the table-init handshake) is alive and correct.

## Follow-up: indirect memory-access (data-plane write path) — CONFIRMED

`fa_macc_test.c` tested the `mem_acc_ctl`/`m_accdata[]` indirect table-write
path (with the WAR777 workaround) against an inert Next-Hop table slot
(index 0 — not referenced by any flow-table row, so no live traffic touches
it): wrote `0x11111111 / 0x22222222 / 0x33333333` to the 3 NH-row words,
read them back.

Words 0 and 1 round-tripped byte-for-byte. Word 2 came back as `0x00000003`
— at first glance a mismatch, but Broadcom's own `CTF_FA_SET_NH_ENTRY` macro
(`etc_fa.c`) only ever assigns `d[2] = (s[0] & 0xE0) >> 5` — a **3-bit**
field. `0x33333333 & 0x7 = 0x3`, exactly the read-back value. The hardware
silently truncated to precisely its documented, real field width — the
same behavior Broadcom's own driver's row-packing macro assumes. A
floating/undriven or garbage register would not be expected to truncate to
exactly a specific, independently-documented bit width; this is further,
specific confirmation the mem-access path works as designed, not a failure
of it. (The naive PASS/FAIL check inside `fa_macc_test.c` itself flags this
as a "MISMATCH" since it doesn't know the field-width story — that's a
limitation of the test script's own logic, not the hardware.)

System stability re-checked after this write too: uptime unbroken, 0%
ping loss, all 4 SSIDs intact, clean `rmmod`.

**Two real hardware mechanisms are now confirmed working**: the table-init
control handshake, and the indirect data read/write path used to actually
program table rows. What's still needed for real traffic acceleration:
constructing a complete, semantically valid NAPT flow-table row (not just
an inert Next-Hop slot) and the separate switch-side SRAB enable — both
remain open, deliberately un-attempted steps.

## Follow-up: switch-side SRAB register (robo_fa_enable's OOB_PAUSE bit) — CONFIRMED

`fa_switch_oobpause_test.c` tested the switch-side half of Broadcom's
`robo_fa_enable()` - the `REG_FC_OOBPAUSE` bit on the BCM53012 switch,
reached via the SRAB bus (physical base `0x18007000`, confirmed live via
this router's own devicetree: `ethernet-switch@18007000`, compatible
`brcm,bcm53012-srab`).

This was treated as its own, separate go/no-go from the GMAC-side FA work:
the SRAB bus is actively used *right now* by the already-loaded `b53_srab`
driver managing this exact switch (unlike the FA/CTF block, which nothing
else in the kernel touches), so a naive independent access could race with
it. Ten parallel research passes were run first to actually answer that
concern rather than guess:

- Confirmed `robo_fa_enable()`'s OTHER register (`BRCM_HDR` tag mode) is
  **already active today** via mainline's own `b53_brcm_hdr_setup()` -
  not touched here, since writing it again would add risk with no new
  information.
- Confirmed this exact device/switch family has real documented history
  of CPU-port/tagging bugs causing full LAN loss (OpenWrt #13784 bricked
  R8000 wired LAN on a stock release; #9024 on this exact switch chip) -
  the caution was warranted, not excessive.
- Confirmed DSA's tag-parsing is bounds-checked everywhere (malformed
  frames get safely dropped, not a crash) and that a real power-cycle
  (not a soft reboot) fully resets the switch ASIC independent of
  whatever state Linux left it in - the actual recovery story if
  something had gone wrong.
- Confirmed the RCAREQ/RCAGNT grant handshake is genuine hardware
  arbitration, not a software convention - built to handle concurrent
  requesters correctly, which is what actually made proceeding
  reasonable despite the shared-bus concern.

Implementation replicated mainline `b53_srab.c`'s exact protocol (request
grant, encode page+register into `B53_SRAB_CMDSTAT`, poll the busy bit,
release grant) rather than inventing a new one - registers: `CTRLS@0x40`,
`CMDSTAT@0x2c`, `RD_L@0x3c`, `WD_L@0x34`.

**Result:**
```
OOBPAUSE before        = 0x0000 (bit8=0)
OOBPAUSE after write    = 0x0100 (bit8=1)  SET-CONFIRMED
OOBPAUSE after revert   = 0x0000            REVERT-CONFIRMED
```

Write and revert both confirmed via read-back, not assumed. System
stability re-checked immediately after: uptime unbroken, 0% ping loss,
`lan1` (the only physically connected port) still up, all 4 WiFi SSIDs
intact, no new dmesg errors, clean `rmmod`.

**All three FA/CTF hardware mechanisms this project set out to verify are
now confirmed working**, empirically, on real silicon: the GMAC-side
table-init control handshake, the GMAC-side indirect data read/write
path, and the switch-side enable register. What remains for actual
traffic acceleration is constructing and inserting a complete, real NAPT
flow-table row (not just an inert test slot) referencing real connection
state - a data-construction task at this point, not a remaining hardware
question.

## What this changes

`VERDICT.md`'s central open question — "is the FA silicon block physically
present, powered, and functional" — is resolved: **yes**, on this exact
board, confirmed via its own real init-done handshake, not just a
plausible-looking register value. The next real step toward actual
hardware NAT acceleration is the NAPT table-write path (with the WAR777
workaround) and, separately, the switch-side SRAB enable — both are new,
distinct go/no-go decisions, not authorized by this result.
