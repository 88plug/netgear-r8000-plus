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

## What this changes

`VERDICT.md`'s central open question — "is the FA silicon block physically
present, powered, and functional" — is resolved: **yes**, on this exact
board, confirmed via its own real init-done handshake, not just a
plausible-looking register value. The next real step toward actual
hardware NAT acceleration is the NAPT table-write path (with the WAR777
workaround) and, separately, the switch-side SRAB enable — both are new,
distinct go/no-go decisions, not authorized by this result.
