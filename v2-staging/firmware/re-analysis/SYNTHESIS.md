# Synthesis: Static Analysis Pass on brcmfmac43602-pcie.ap.bin — OWE Question

**Target:** `/home/andrew/netgearr8000/v2-staging/firmware/brcmfmac43602-pcie.ap.bin`
(595,472 bytes, ARM Thumb-2, no ELF/symbol table, version banner confirms
`7.35.177.56`, built `Fri 2015-09-18 03:31:06 PDT`, FWID `01-6cb8e269` —
matches the firmware already characterized in `docs/FINDINGS.md` section 9.)

**Question:** does anything in this static-analysis pass change the "OWE is
confirmed unfixable on this firmware" conclusion in FINDINGS.md section 9?

## Data actually available for this synthesis

Nine agents were dispatched in parallel to analyze this binary from different
angles. This synthesis was written after waiting twice (~90s, then a further
~75s independent check) for their output directory to populate. **Only 4 of
the ~9 expected output files existed when the wait window closed:**

| File | Status | Size |
|---|---|---|
| `strings_with_offsets.txt` | present | 112 KB |
| `full_disasm.txt` | present | 8.6 MB (246,827 instructions decoded) |
| `function_boundaries.txt` | present | 1,879 candidate functions |
| `code_data_map.txt` | present | coarse 512B-window code/data heuristic |
| jump-table / dispatch-pattern search | **missing — no file produced** | — |
| DHD vendor-source event-ID cross-reference | **missing — no file produced** | — |
| security-constant (`wpa_auth`) anchor search | **missing — no file produced** | — |
| iovar string-table structural check | **missing — no file produced** | — |
| AP-vs-STA structural diff | **missing — no file produced** | — |
| ISA/entry-point identification | folded into `full_disasm.txt` header (Thumb-2, base offset 0, vector-table-style branch stubs at 0x0–0x1c) | — |

This is a genuine, honest gap. **The two agents most likely to surface direct
OWE-relevant evidence — the DHD event-ID cross-reference and the `wpa_auth`
security-constant anchor search — did not complete in the observation
window.** Everything below is either (a) what the 4 available structural
files show, or (b) targeted follow-up I ran myself against `full_disasm.txt`
and `strings_with_offsets.txt` to partially cover the gap, clearly marked as
such and clearly caveated as unsymbolized inference.

## What was found

### (a) Plausible event/dispatch table structures — yes, generically; not confirmed as the WLC_E table

`full_disasm.txt` contains **53 `TBB`/`TBH` (table-branch-byte / table-branch-
halfword) instructions** — Thumb-2's compiler-generated jump-table dispatch
idiom, the classic lowering of a C `switch` with enough contiguous cases.
Example sites: `0x0057da`, `0x005978`, `0x008dd8`, `0x00b2a2`, `0x00bc6a`,
`0x0115ee`, `0x019f9e`, `0x01a7d8`, `0x01e8c0`, `0x01ead6`, `0x01ef24`,
`0x020054`, `0x028cba`, `0x02a130`, `0x02b40e`, plus ~38 more through the
image. No `ldr pc, [pc, ...]` indirect-branch-table variant was found (0
hits) — all multi-way dispatch in this firmware uses the `TBB`/`TBH` idiom.

This confirms the firmware's compiler/toolchain **does** use table-based
dispatch generally, so "a dispatch table exists somewhere that could
theoretically be extended" is true as a category. **But which, if any, of
these 53 tables is the `WLC_E_*` event-generation dispatcher (as opposed to
an ioctl dispatcher, an iovar dispatcher, a radio state machine, a rate-
control table, or something else entirely) was not established** — that
correlation is exactly what the missing DHD event-ID cross-reference agent
was tasked to determine by comparing table case counts/strides against the
known `WLC_E_*` enum from Broadcom's DHD vendor source. Without symbols or
that cross-reference, a `TBB` table is just an anonymous N-way branch; I
cannot respray any of the 53 as "the event table" without guessing.

### (b) Latent/unused capability for external PMK or generic association handling — no evidence found, but also not ruled out

I searched `full_disasm.txt` for a specific candidate: a comparison chain
against small values ending near the SAE constant referenced in FINDINGS.md
(AKM/`wpa_auth` dispatch that "terminates after SAE"). One candidate chain
was found at `0x00c0a4`–`0x00c0ce`:

```
00c0a4: cmp.w r0, #0x10000   ; -> orr r3, r3, #0x10000
00c0b0: cmp.w r0, #0x20000   ; -> orr r3, r3, #0x20000
00c0bc: cmp.w r0, #0x30000   ; -> orr r3, r3, #0x30000
00c0c8: cmp.w r0, #0x40000   ; it eq -> orreq r3, r3, #0x40000
00c0d2: str r3, [r1]
00c0d4: movs r0, #0
00c0d6: pop {r4, r5, pc}     ; clean return, no further case, no default branch
```

`0x40000` does coincide with the value FINDINGS.md's chronology argument
implicitly references for SAE in Broadcom's `wpa_auth` bit layout. **I am
flagging this only as a possible lead, not a confirmed hit** — the
surrounding code (immediately preceding lines OR further bits — `0x100000`,
`0x400000`, `0x800000` — into the *same* accumulator register `r3` from
sub-fields of `r0` masked with `0x70000`) reads more like a capability- or
chanspec-bitfield builder than an AKM/`wpa_auth` type dispatch. Telling those
apart with certainty requires either symbols or the dedicated security-
constant anchor agent's cross-referenced work, which did not land. **Do not
treat this as confirmation of the wpa_auth dispatch chain described in
FINDINGS.md — it may be entirely unrelated code that happens to share a
numeric coincidence.**

Beyond that one ambiguous lead, no other candidate for OWE-adjacent
capability (DH Parameter IE handling, external-PMK acceptance path, generic-
association fallback) was identified. `strings_with_offsets.txt` — 6,724
extracted strings — contains **zero** matches for `owe`, `E_OWE`, `WLC_E_`,
`wpa_auth`, `WPA3`, `sae`, `RSN`, `diffie`, `ecdh`, or `auth` as whole/near
words. The only semantically legible content in the entire strings dump is
the version banner at the very end of the file (offset 595246, the last ~226
bytes) and a sparse handful of IOVAR-style names (`measpower`, `roamoffl_
bssid_list`, `wl_nonassoc_rxchain_pwrsave_enable`, etc.) scattered through
what is otherwise almost entirely non-printable / garbage-string noise. This
firmware blob is heavily stripped: no readable event-name table, no visible
AKM/security-constant labels, nothing string-adjacent that names OWE or
anything resembling it.

### (c) Does the wpa_auth comparison chain confirm a clean dead-end after SAE with no extensibility?

**Partially, with a caveat.** The one candidate chain found (above, `(b)`)
does dead-end cleanly after its last comparison with no `default:`/error
branch beyond a plain return — structurally consistent with FINDINGS.md's
description of the *host driver's* `brcmf_parse_configure_security()`
switch statement (which also dead-ends into a `default:` "Invalid key mgmt
info" case). But this chain is **in the firmware binary**, not the kernel
driver source FINDINGS.md already read directly — and, per `(b)` above, I
cannot confirm this chain is actually the `wpa_auth` dispatch rather than an
unrelated bitfield builder. So this point neither confirms nor overturns
FINDINGS.md's driver-side finding; it is an independent, unconfirmed,
firmware-side data point that happens to rhyme with it.

### (d) Other structurally relevant findings

- **Code/data proportions** (`code_data_map.txt`, coarse 512-byte sliding
  window heuristic): of 595,472 bytes, ~427,536 bytes (72%) score as
  confident CODE, ~24,576 bytes (4%) as confident DATA (mostly zero-padding
  regions), and **~143,360 bytes (24%) as UNCERTAIN** — the heuristic
  explicitly can't tell code from data there. A quarter of this firmware is
  structurally ambiguous to a coarse pass. Any event-name table, dispatch
  table backing store, or capability-flag table not caught by the strings
  extraction could live in that UNCERTAIN 24%, and this pass did not resolve
  it further.
- **Function boundary detection** (`function_boundaries.txt`, 1,879
  candidates via push/pop-prologue heuristic): most candidates are small
  (tens to low hundreds of bytes) as expected for firmware leaf functions,
  but a few `approx=1` (no clean epilogue found) spans are suspiciously
  large — e.g. `0x07f994`–`0x0859b6` (24,610 bytes) and `0x057164`–
  `0x05caf6` (22,930 bytes). These are very likely heuristic-boundary
  failures (a genuine 24 KB single function is implausible), not real
  single functions — probably regions containing tail-call exits, data
  interleaved with code, or dispatch-table bodies that confuse a linear
  push/pop scan. Two large functions *did* get clean bounds:
  `0x07c5b0`–`0x07ddd2` (6,178 bytes) and `0x0902f4`–`0x09152a` (4,662
  bytes) — these are the largest genuinely-bounded functions in the image
  and would be reasonable next-look candidates for "big state machine /
  frame parser," but neither was inspected for OWE-relevant content in this
  pass (out of scope without the missing agents' targeted cross-references).

## Verdict

**No. This static-analysis pass does not change the OWE-unfixable
conclusion in FINDINGS.md section 9.** The conclusion stands, for two
independent reasons — one substantive, one about the limits of this method:

1. **Nothing found changes the substantive picture.** No firmware-level
   evidence of Diffie-Hellman Parameter IE handling, a `WLC_E_OWE_INFO` (or
   equivalent) event, external-PMK acceptance, or an OWE-labeled anything was
   found anywhere in the 4 available structural outputs or in my own
   follow-up greps of the disassembly and strings dump. The one numerically
   suggestive lead (the `0x40000`-terminated compare chain at `0x00c0a4`) is
   ambiguous on inspection and more likely an unrelated bitfield builder than
   the `wpa_auth` AKM dispatch — I'm not willing to call it a hit.

2. **This pass hit a real ceiling, and it's worth being specific about why.**
   Five of the nine planned agents — critically, the DHD vendor-source
   event-ID cross-reference and the `wpa_auth` security-constant anchor
   search, the two tasks most directly aimed at this exact question — did
   not produce output in the observation window. What's left is generic
   structural tooling (raw disassembly, function-boundary heuristics, a
   coarse code/data map, a strings dump) applied to a **completely
   unsymbolized, heavily-stripped ARM/Thumb-2 binary**. That combination can
   establish *architecture-level facts* (Thumb-2 confirmed, 53 TBB/TBH
   dispatch tables exist somewhere in the image, ~24% of the file resists
   confident code/data classification) but it fundamentally cannot answer
   *semantic* questions like "is this specific table the event dispatcher"
   or "is this specific compare chain the AKM switch" without either
   symbols, string-adjacent labels (which this firmware doesn't have — the
   strings dump is almost entirely non-printable garbage outside the
   version banner), or the kind of targeted cross-referencing the missing
   agents were assigned to do. Absence of evidence in this pass is
   therefore weak evidence of absence at best — it rules out "an agent
   found and confirmed an OWE hook," not "an OWE hook could not possibly
   exist anywhere in the unexamined 24% of the file."

3. **Even a hypothetical positive finding here would not, by itself, unblock
   OWE.** FINDINGS.md section 9 already establishes that the actual failure
   point is in the **host kernel driver** (`brcmf_parse_configure_security()`
   in `cfg80211.c`), which has no `case` for AKM suite 18 (OWE) and returns
   an error *before ever issuing an IOVAR to the firmware*. Firmware-side
   dispatch extensibility, even if it existed and even if this pass had
   confirmed it, would be necessary-but-nowhere-near-sufficient: it would
   still require (a) a driver patch to recognize and forward OWE AKM
   negotiation, (b) firmware code to parse an incoming DH Parameter IE out of
   an association request and emit a new event carrying it upward — an
   entirely different code path (management-frame IE parsing + event
   emission) than a `wpa_auth` compare chain — and (c) firmware code to
   accept a PMK set back from the host after ECDH completes. A 2015-09-18
   build, a full year before OWE (RFC 8110, 2016) existed as a standard, has
   no structural reason to contain (b) or (c) regardless of what its dispatch
   tables look like. The chronology wall FINDINGS.md already identified
   remains the load-bearing argument; nothing in this pass touches it.

**Bottom line for the runbook:** OWE stays out of scope on this firmware,
same as FINDINGS.md section 9 already concluded. If this question is worth
revisiting later, the productive next step is not more generic
disassembly/strings tooling — it's specifically re-running (or manually
performing) the two agent tasks that didn't complete: the DHD vendor-source
`WLC_E_*` cross-reference against the 53 identified `TBB`/`TBH` tables, and a
targeted `wpa_auth`-constant search using known Broadcom bit values as
anchors rather than the coincidental-value grep I did here.
