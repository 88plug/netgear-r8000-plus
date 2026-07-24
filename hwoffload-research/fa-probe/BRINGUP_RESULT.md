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

## Follow-up: real driver skeleton (Phase A+B), per the approved plan

`fa_accel.c` is the first step toward an actual persistent feature, not
another one-shot register test. Full design: `/home/andrew/.claude/plans/drifting-dazzling-mccarthy.md`
(approved). Architecture: registers as an indirect TC-flower/flowtable
hardware-offload backend (`flow_indr_dev_register()`), the exact interface
real hardware NAT accelerators (e.g. MediaTek's `mtk_ppe_offload.c`) already
use - not a bespoke netfilter/conntrack hook. This router's own `flow add @ft`
nftables rule already produces the "this flow qualifies for offload" signal
today; a backend just has to answer `FLOW_CLS_REPLACE`/`DESTROY`/`STATS`
callbacks. Research this session confirmed every failure path here (a
returned error, a translation gap) is silently absorbed by the framework -
software flowtable already runs the connection unconditionally, hardware
offload is strictly best-effort on top. That structural guarantee is what
made Phase A+B safe to load on the live router at all.

Phase A+B does exactly two things and nothing else: registers the block
callback (proves the dispatch plumbing works), and on `FLOW_CLS_REPLACE`
decodes the generic `flow_rule` into what a real `fa_napt_prep_ipv4_word()`-
style row would contain - logged only. It always returns `-EOPNOTSUPP`.
Zero FA/CTF register access anywhere in this file.

**Loaded live, verified:** registration confirmed via dmesg, uptime
unbroken, 0% ping loss, all 4 SSIDs intact, clean `rmmod`. No
`FLOW_CLS_REPLACE` fired - expected and consistent with this bench setup
having no forwarded traffic (no WAN cable, the one LAN client's SSH
session is local input, not forwarded). Full logging verification needs a
second test client generating real LAN-to-LAN or LAN-to-WAN traffic, or
real deployment - noted as an open follow-up in the plan, not a defect.

**Deliberately not built this pass:** Phase C (real FA table writes -
needs Phase B's logged output verified against real flows first) and
Phase D (persistent GMAC/switch bring-up at driver load) - both remain
their own separate decisions per the approved plan.

## Follow-up: real, complete NAPT flow row - write, read, delete - CONFIRMED

`fa_napt_row_test.c` closes the actual remaining hardware question: not an
inert test slot this time, but a COMPLETE, realistic NAPT flow-table row,
built with Broadcom's own bit-packing macros verbatim
(`fa_napt_prep_ipv4_word()`, `CTF_FA_SET_NH_ENTRY`) - simulating a real LAN
client (192.168.1.50:34567) NAT'd to a public destination (93.184.216.34:80,
synthetic, no packet ever sent).

**Result: every word matched, both ways.**
```
next-hop row:  0x00080004 0xf56df778 0x00000006  - all 3 words MATCH
NAPT flow row: 0x00000000 0x6c110008 0x00282edc 0xc3838028
               0x5db8d822 0xc0a80132 0x00100000 0x80000000  - all 8 words MATCH
valid bit after write  = 1
valid bit after delete = 0   DELETE-CONFIRMED
```

This is a meaningful upgrade from the earlier inert Next-Hop slot test
(`fa_macc_test.c`), which used an arbitrary test pattern and saw one word
truncate to a narrower real field width (correctly, as later analysis
showed). This test used *correctly-scoped* real data throughout, and the
complete 8-word flow row - the actual production data structure real
hardware NAT acceleration depends on - round-tripped perfectly, including
the full write -> read -> mark-invalid -> delete -> verify lifecycle
`_fa_napt_del()` itself uses.

System stability re-checked: uptime unbroken, 0% ping loss, all 4 SSIDs
intact, clean `rmmod`. Table index 0 in both tables was never referenced
by anything else in the running kernel (`fa_accel.c` still always returns
`-EOPNOTSUPP`), so this remained fully isolated from any live packet path.

**What this proves, precisely:** the complete data-plane write mechanism
for real NAT flow entries works, correctly, on this exact silicon.
**What it still doesn't prove:** that the hardware actually *consults*
this table for real forwarded packets and rewrites them correctly in
flight - that requires wiring this into `fa_accel.c`'s real
`FLOW_CLS_REPLACE` handler (still returning `-EOPNOTSUPP` today) and
testing against real traffic, which still needs a second test client or
real deployment.

## Follow-up: real traffic verification - the last open gap, closed

Every prior test used synthetic data or waited on a precondition ("needs
real forwarded traffic"). This one didn't: the operator connected the
R8000's WAN port to a real upstream (via a spare LAN port on their own
network, safe double-NAT, no disruption to their primary network), and a
real HTTP connection was routed from a laptop through the R8000's actual
LAN->WAN NAT path (bound to a specific interface + a single narrow host
route, so only this one test destination's traffic left via that path -
everything else on the laptop stayed on its normal connection).

**Prerequisite found and fixed live:** the flowtable didn't have `flags
offload` set (`flow_offloading_hw` was `0`), so the kernel's hardware-
offload dispatch was never even attempted regardless of traffic - this
project's own earlier perf-tune research had already safely toggled this
exact flag once before and reverted it, confirming it does nothing on its
own (no driver was registered to receive it). This time something was:
`fa_accel.c`. Toggled it on live, tested, toggled back off - same
already-proven-safe pattern.

**Result: `fa_accel.c`'s Phase B decode fired on a real connection and
decoded both directions of the real NAT'd flow correctly:**
```
Forward (LAN->WAN): 192.168.1.2:41762 -> 104.20.23.154:80
  post-NAT:         192.168.1.86:41762 -> (dst unchanged, correct for SNAT)
  egress_dev=wan

Reverse (WAN->LAN): 104.20.23.154:80 -> 192.168.1.86:41762
  post-NAT:         (src unchanged) -> 192.168.1.2:41762
  egress_dev=lan1
```
Both directions, both pre-NAT and post-NAT tuples, correct egress device
per direction - exactly the data `fa_napt_prep_ipv4_word()` needs to build
a real row, extracted from a real HTTP connection, not synthetic input.

System stability re-checked: uptime unbroken, 0% ping loss, all 4 SSIDs
intact throughout. Everything reverted afterward: `flow_offloading_hw`
back to `0` (confirmed via `nft list flowtable`, device list back to
`br-lan`/`br-guest`/`wan`), `fa_accel` unloaded cleanly, the laptop's
temporary host route removed.

**This closes the verification gap noted in every earlier entry in this
file.** Combined with `fa_napt_row_test.c`'s proof that a complete, real
NAPT row can be written/read/deleted on the hardware, and this proof that
real connection data decodes correctly into exactly that row's inputs,
the only remaining step to a working feature is connecting the two:
having `fa_accel.c`'s `FLOW_CLS_REPLACE` handler actually construct and
write the row (Phase C proper) instead of logging what it would contain.
That is a real, separate, deliberate step - not a hardware unknown
anymore, an integration task.

## Follow-up: Phase C real write path built; live=1 forwarding test attempted but not completed

`fa_accel.c` gained a real Phase C: `fa_flow_replace_live()` decodes a real
`flow_rule` (including destination MAC), builds both the next-hop and NAPT
rows with the same verbatim macros proven in `fa_napt_row_test.c`, writes
them via the same WAR777-wrapped indirect path proven in `fa_macc_test.c`,
and — critically — **reads the row back and verifies it matches before ever
returning success**; a mismatch frees the slot and returns `-EOPNOTSUPP`
instead of silently claiming the offload. `fa_flow_destroy_live()` clears
the valid bit and frees the slot on flow teardown. Gated behind
`module_param(live, bool, 0444)`, default `false` (Phase A/B logging-only
behavior unchanged).

To make a real end-to-end test possible, two of the previously one-shot,
auto-reverting register writes were re-implemented to persist for the
duration of a test window instead of reverting immediately:
`fa_switch_oobpause_persist.c` (OOBPAUSE stays set until `rmmod`, unlike
`fa_switch_oobpause_test.c` which reverts inside its own `init()`) and
`fa_bringup.c` loaded without an immediate `rmmod`. A new read-only
`fa_stats_probe.c` was added specifically as the pre/post oracle for this
test: `control`, `status`, `stats[HIT]`, `stats[MISS]`, `error`,
`ecc_error`.

**Sequence executed, this pass:**
1. `fa_bringup.ko` loaded and left active (not rmmod'd) — `INIT_DONE`
   confirmed, control `0x0007f408`.
2. `fa_switch_oobpause_persist.ko` loaded and left active — OOBPAUSE
   confirmed set to `0x0100`.
3. `flow_offloading_hw=1` set live, `flags offload` confirmed present in
   `nft list flowtable inet fw4 ft`.
4. `fa_accel.ko live=1` loaded — clean registration confirmed via dmesg.
5. Pre-test baseline read via `fa_stats_probe.ko`: `control=0x00041408
   status=0x000003e2 hit=0 miss=222 error=0x00000000 ecc_error=0x00000000`.
6. Stability re-checked at each step — uptime unbroken, 0% ping loss, no
   new dmesg errors, all 4 SSIDs intact throughout.

**The test packet itself was not sent.** Between step 5 and firing the test
request, the operator disconnected the R8000's WAN uplink (the same spare
port used for the earlier successful logging-mode verification). With no
WAN link, `curl`'s request to the real test destination failed instantly
(`http_code=000`) — there is no path off the router for it to take, not a
driver or hardware failure. **`fa_flow_replace_live()`'s actual write path
has therefore been built and code-reviewed against the same real-data
inputs proven correct in `fa_napt_row_test.c` and the earlier real-traffic
decode, but has never been exercised end-to-end against a live packet.**
That is the one thing this project has still not empirically proven: that
FA hardware, once brought up, actually consults a row this driver wrote and
forwards a real packet through it correctly.

**Full revert executed and confirmed, in order:** `fa_accel` unloaded
(`unregistered, module unloaded`); `flow_offloading_hw` back to `0`
(`nft list flowtable` shows the flowtable with no `flags offload`, device
list back to `br-guest`/`br-lan`/`wan`); `fa_switch_oobpause_persist`
unloaded (`reverted OOBPAUSE to 0x0000`); `fa_bringup` unloaded (control
restored, final `fa_probe` read confirms `control=0x00001400
status=0x000003e2` — byte-identical to every prior baseline in this file).
Post-revert stability: uptime 2:57 unbroken, 0% ping loss, all 4 SSIDs
(`R8000` x3 + `R8000-Guest`) enabled.

## Follow-up: live=1 real-packet test completed — write path proven, hardware consultation NOT proven

With WAN reconnected and link stable, the full sequence was re-run and the
test packet actually sent this time (`curl` LAN->WAN through the router's
real NAT path to `104.20.23.154:80`, `http_code=200`, `time=0.052s`,
`local_ip=192.168.1.2 local_port=57534`).

**`fa_accel`'s write path fired for real, on both directions, and the
built-in verify-before-accept gate passed both times:**
```
fa_accel: live: cookie=0xc22b4304 nh_idx=1 nf_idx=1 written+verified,
  192.168.1.2:57534 -> 104.20.23.154:80 egress=wan dir=LAN->WAN - accepting offload
fa_accel: live: cookie=0xc22b4354 nh_idx=2 nf_idx=2 written+verified,
  104.20.23.154:80 -> 192.168.52.247:57534 egress=lan1 dir=WAN->LAN - accepting offload
```
~38ms later, both flows tore down correctly via the framework's normal
`FLOW_CLS_DESTROY` path:
```
fa_accel: FLOW_CLS_DESTROY cookie=0xc22b4304 -> marked invalid, freed
fa_accel: FLOW_CLS_DESTROY cookie=0xc22b4354 -> marked invalid, freed
```
This proves, for the first time against a real live connection rather than
synthetic data: the driver decodes a real flow correctly in both
directions, builds a real NAPT+next-hop row pair, writes it, **reads it
back and confirms the readback matches before ever reporting success**, and
correctly tears it down on connection close. Every piece of software-side
plumbing this project set out to build works.

**What it does not prove: that FA hardware ever actually consulted either
row to forward a real packet.** `fa_stats_probe.ko` read before and after:
```
before: hit=0 miss=378
after:  hit=0 miss=40
```
`hit` never moved. (The `miss` drop, not a rise, indicates this counter is
read-to-clear — each read zeroes it, so `378` and `40` are two independent
windows, not a running total; not a red flag on its own.) But `hit`
staying at `0` across a write-verified, accepted, real connection is a real
negative result, not an absence of data.

Two honest candidate explanations, neither confirmed:
1. **The window was too short.** Only ~38ms separated write from destroy —
   consistent with the kernel's flowtable offload typically engaging only
   once a connection is already established+replied, for a single fast
   HTTP GET that may have already been finishing by the time the row was
   written. Very few or zero packets may have had any chance to traverse
   the row before it was torn back down.
2. **FA may not be wired into the live datapath at all.** Everything
   touched this session — GMAC table-init, GMAC indirect data path, the
   switch's `REG_FC_OOBPAUSE` bit — is FA's own control/config/table-write
   surface. Nothing this session configured any mechanism that tells the
   switch or MAC hardware to actually *route packets through* FA's lookup
   engine for consultation. A perfectly valid row sitting in a table that
   nothing queries would produce exactly this signature: correct writes,
   zero hits, connection working fine the whole time via the ordinary
   software path (which the framework guarantees regardless, per the
   architecture in `drifting-dazzling-mccarthy.md`).

Distinguishing these needs either a longer-lived connection (large
download, many requests over a kept-alive session — a wider write-to-destroy
window to give any real consultation a chance to register) or a return to
`etc_fa.c`/vendor source specifically to find what, if anything, activates
FA as a **datapath** participant rather than just a configured table.
Neither has been done. This is a new, distinct research question, not a
minor follow-up.

**Full revert executed and confirmed:** `fa_accel` unregistered, flowtable
back to `br-guest`/`br-lan`/`wan` with no `flags offload`,
`fa_switch_oobpause_persist` and `fa_bringup` both unloaded cleanly,
`fa_probe` re-read confirms `control=0x00001400 status=0x000003e2` —
identical to every baseline in this file. `lsmod` confirms zero FA test
modules remain loaded. Stability: uptime unbroken (3:48), 0% ping loss to
both the router and the real internet test target, all 4 SSIDs enabled.

## What this changes

`VERDICT.md`'s central open question — "is the FA silicon block physically
present, powered, and functional" — is resolved: **yes**, on this exact
board, confirmed via its own real init-done handshake, not just a
plausible-looking register value. Every register-level mechanism and the
full software write/verify/decode/teardown path are now proven correct
against real connection data. What remains genuinely open, after a real
end-to-end attempt: whether FA silicon is wired into the live packet
datapath at all, versus being a correctly-programmable table that nothing
in this project has yet connected to actual traffic. That is the real next
research question — not a bring-up step, not a go/no-go on enabling
something proven, but an open unknown that a short single-request test
cannot resolve either way.
