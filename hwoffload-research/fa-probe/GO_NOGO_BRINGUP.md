# GO/NO-GO: `fa_up()` bring-up + first NAPT table write

> **Resolved 2026-07-23 — this gate is closed, not an open decision.** The
> item this doc left open (a separate operator go-ahead for the
> register-write bring-up) was subsequently obtained, and the bring-up
> ran successfully — see `BRINGUP_RESULT.md` and `docs/WINS.md`'s "FA/CTF
> hardware accelerator" section for the outcome. Investigation closed;
> the historical record below (written before that authorization) is
> left as written.

This is the **next** gate after `GO_NOGO.md` (the read-only register-probe
gate, already run — see its outcome below). It does **not** authorize running
the bring-up sequence; it answers the two open questions `GO_NOGO.md` itself
flagged as blockers before that decision could even be considered, and lays
out what a real go/no-go on the bring-up itself would need.

## Where this picks up

`GO_NOGO.md`'s probe ran on 2026-07-23: `fa_probe.ko` loaded cleanly, read
`control=0x00001400 status=0x00000002` at phys `0x18027c00`, logged its own
verdict — "non-trivial register content observed - possible sign the FA
block is live" — and explicitly declined to interpret further: *"Do NOT
proceed to the init-strobe/bring-up sequence based on this alone; further
analysis needed first."* Matched against `GO_NOGO.md`'s own pre-committed
outcome table, this landed on the **"non-trivial, non-repeating values"**
row: possible sign of life, not confirmation, next step is chip-revision
confirmation — not table writes. This doc is that next step.

## What this doc resolves: chip identity

`GO_NOGO.md`'s WAR777 discussion left two items explicitly open before any
bring-up: (a) whether R8000's actual BCM4709 die/stepping matches the
documented erratum (BCM4707 rev 2), (b) whether that stepping had even been
confirmed live in this synthesis window. Read-only, live-router evidence for
both, gathered 2026-07-23:

```
[    2.763007] bcma-host-soc 18000000.axi: bus0: Found chip with id 53010, rev 0x00 and package 0x00
```

**This is the SiliconBackplane/ChipCommon-level SoC chip ID** (read via the
`bcma` bus scan at boot, not the switch's own self-ID) — i.e. this is the
actual on-die identity of the R8000's main SoC, confirmed live, not inferred
from the board name or DTS `compatible` string.

**Do not conflate this with the separately-confirmed switch identity.** The
integrated ROBO/b53 switch IP block self-identifies through a *different* ID
register, reached over SRAB, and was already independently confirmed in
`v2-staging/extras/dsa-switch/NOTES.md` / `hardware-docs/notes.md` §5 as
`found switch: BCM53012, rev 5`. Two different ID spaces, same package:
- **SoC package (bcma/ChipCommon):** chip id **53010**, rev **0x00**.
- **Integrated switch IP (SRAB self-ID):** **BCM53012**, rev **5**.
Conflating these would be exactly the kind of chip-family mixup
`WAVE2_INDEX.md` thread 2 already flagged as a risk to guard against
(BCM53012/`is5301x()` vs the unrelated BCM58xx `is58xx()` "Flow Accelerator"
reference in `b53_common.c`) — worth the same discipline here.

**Relevance to WAR777:** the documented erratum (`etc_fa.c:
CTF_FA_WAR777_ON/OFF`) is stated for **BCM4707 rev 2**. This unit's
SiliconBackplane SoC chip id is **53010 rev 0x00** — different chip id,
different revision, from the literal erratum condition. That is a real,
now-confirmed data point, not a guess — but it is **evidence the erratum
doesn't literally apply, not proof no analogous quirk exists on 53010
rev0x00**: Broadcom's programmer's references for this exact chip/stepping
remain NDA-gated (per `hardware-docs/notes.md` §1), so an undocumented
53010-specific table-access quirk can't be ruled out by this check alone.

## Verdict on THIS doc's question: chip identity — RESOLVED

Both `GO_NOGO.md` blockers are now closed with live evidence:
- R8000's actual SoC chip id/rev: **confirmed, `53010 rev 0x00`** (was
  "unconfirmed in this synthesis window").
- WAR777 applicability: **the documented erratum's literal condition
  (BCM4707 rev 2) does not match this chip** — narrows the risk but doesn't
  eliminate the general class of chip-rev-specific quirks `VERDICT.md`'s
  effort table already flagged as "Unknown until probed."

## What this doc does NOT resolve: the bring-up decision itself

Closing the chip-identity question is necessary but not sufficient to green-
light `fa_up()`. Unlike the read-only probe (`GO_NOGO.md`), the bring-up
sequence **writes** to hardware: strobes all 5 table-init control bits, polls
`INTSTAT_INIT_DONE`, then performs indirect `mem_acc_ctl`/`m_accdata[]` writes
to insert a NAPT flow row — this is a fundamentally different risk class than
a single read (§`GO_NOGO.md`'s own category-1/category-2 abort analysis was
scoped to reads; it does not cover write-path failure modes, e.g. a
write landing on an unclocked/held-in-reset block behaving differently than a
read). Per this project's own standing discipline (`docs/RUNBOOK.md`: "confirm the
recovery net is actually staged" before any panic/brick-risk operation) and
the probe module's own explicit caution, this stays a **separate, deliberate
decision** — not something this chip-identity confirmation authorizes by
itself. A real go/no-go for the bring-up step would additionally need:

1. **A repeat register read** immediately before the bring-up attempt, to
   confirm `control=0x00001400 status=0x00000002` is stable/repeatable
   (not, e.g., stale bus data or a floating/undriven line that happens to
   read non-zero once) — `GO_NOGO.md`'s own interpretation table treats
   "non-repeating" values differently from a confirmed-stable reading, and
   this hasn't been checked.
2. **`panic_on_oops=0` + recovery net re-confirmed staged** immediately
   before the attempt (nmrpflash + known-good `.chk`, per `docs/RUNBOOK.md`) — a
   register *write* going wrong is architecturally the same brick-class risk
   as the original read, and the mitigation is identical.
3. **An explicit operator go-ahead for this specific step**, separate from
   the read-only probe's authorization — the two carry different risk
   profiles and this project's own pattern throughout (v2 clm_blob, OWE
   disassembly, MBSS fix) has been to gate each escalation in risk
   individually rather than treat an earlier "go" as blanket authorization
   for what comes next.

## Stability re-check — done, 2026-07-23 (still read-only, same risk class as the original probe)

Ran the item-1 stability check from the previous section: reloaded the
unchanged `fa_probe.ko`, this time **after a full router reboot** (not just a
second `insmod` in the same boot session):

```
control=0x00001400 status=0x00000002
```

**Identical to the first read**, byte-for-byte, across a full power cycle.
This matters: a genuinely floating/undriven bus line (the signature of an
unclocked-but-backplane-decoded wrapper per `GO_NOGO.md`'s category-2
analysis) would not reliably be expected to latch the exact same pattern
twice across a reset — floating inputs are typically noisy, not
deterministic. A stable, repeatable, non-zero, non-`0xffffffff` value across
both a fresh module load and a full cold boot is stronger evidence for a
real, driven register than a single read was. It is **still not proof of a
live, functional FA block** — a fixed hardwired-strap pattern or a
consistently-latched reset-default value would look identical to this and
would NOT mean the block does anything when strobed — but item 1 of the
bring-up checklist above is now satisfied: the reading is confirmed stable,
not a one-off fluke.

Cleanup: `panic_on_oops` was set to `0` before this reload (system default
after a fresh boot is `1`, confirmed directly — the earlier session's note
that "0 is already default" was incorrect, it had been left at `0` from an
earlier manual test in that session) and restored to `1` immediately after
`rmmod` (exit 0).

## Recommended next step

Chip identity (resolved above) and read stability (resolved above) were the
two concrete, boundable items on the pre-bring-up checklist. What's left —
item 3, an explicit separate operator go-ahead for the register-*write*
bring-up step — is not something to resolve unilaterally in a docs/config
pass; it stays a deliberate future decision, consistent with this project's
pattern of gating each risk escalation individually. This doc's job (turn
"inconclusive" into "as resolved as read-only investigation can make it")
is done; `fa_up()` itself is not run here.
