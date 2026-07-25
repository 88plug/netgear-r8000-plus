# Channel unlock + max power + regulatory override + best PHY — findings

Workstream scope: make the full channel set and the maximum calibrated
transmit power *available* on the operator's own R8000, and get the
regulatory override plumbing right. Does not own clm_blob/txcap_blob
extraction (`../firmware/`) or which channel each radio defaults to
(`../extras/5ghz-channel-defaults/`).

## TL;DR

- **Channel unlock is gated by the device's own devicetree
  (`ieee80211-freq-limit`), not by any regulatory-domain mechanism.**
  radio0 is hard-limited to 5735-5835MHz (ch149-165) and is already at
  100% of that range. radio2 is hard-limited to 5170-5730MHz (ch34-144)
  and is currently at 4 of the ~19 usable channels in that range (36/40/
  44/48); the rest (UNII-2 + UNII-2e, DFS) are additionally gated behind
  the missing clm_blob. **Nothing in this workstream's control (`iw reg
  set`, nvram `ccode`/`regrev`/`disband5grp`, hostapd `country_code`) can
  move either boundary or unlock DFS without that blob** — verified
  empirically with four separate live tests, see `verify/before-after.md`.
- **Decoded PA ceiling:** 2.4GHz 26.5dBm/chain; 5GHz-high (radio0,
  149-165) 26.5dBm/chain; 5GHz-low (radio2, whole DTS range) 22.5dBm/chain
  flat. All ×3 chains. Configured in `etc/config/wireless` by requesting
  slightly above these values so the driver's own hardware clamp — not a
  guess on this workstream's part — sets the real ceiling.
- **One of the four unlock attempts crashed the router** (hardware
  watchdog reboot). Full repro and the specific bad sequence are below —
  read "Known-bad sequence" before enabling an AP with `country` set.

## Decoded power ceiling

`maxp5ga*`/`maxp2ga*` in `../../extracted/calibration.txt` are Broadcom
quarter-dBm (qdbm) values: divide by 4 for dBm. Cross-checked against the
matching-shape `pa5ga*`/`pa2ga*` PA calibration arrays (12 hex values =
4 groups × 3 polynomial coefficients for the 5GHz radios, confirming the
4-group sub-band structure independently of the maxp values themselves),
and against a live on-device reading (see below).

| Radio | nvram prefix | Raw maxp (qdbm) | dBm/chain (÷4) | Sub-band group | Reachable per DTS? |
|---|---|---|---|---|---|
| radio1 (2.4GHz) | `1:` | maxp2ga0/1/2 = 106 | **26.5** | n/a (single group) | yes, whole 2.4GHz |
| radio0 (5GHz #1) | `0:` | maxp5ga* = 54, 90, 90, **106** | 13.5 / 22.5 / 22.5 / **26.5** | low / mid / 2e / **high** | **only "high" (149-165)** — DT-locked |
| radio2 (5GHz #2) | `2:` | maxp5ga* = 90, 90, 90, 90 | 22.5 flat | low / mid / 2e / high | low today; low+mid+2e once DFS activates — DT-locked, high unreachable |

All three radios report `txchain=7` / `rxchain=7` (3 chains) and the FCC
teardown (`../fccid/specs.md` §3) independently confirms genuine 3×3 per
radio, not shared streams. Aggregate 3-chain EIRP (per-chain dBm +
10·log₁₀3 ≈ +4.8dB, incoherent combining) works out to ~31.3dBm for the
2.4GHz and radio0-high ceilings, ~27.3dBm for radio2's flat ceiling.

**Live cross-check:** during the Attempt-4 test in `verify/before-after.md`,
a real AP came up on radio0 channel 153 (within its 26.5dBm/chain "high"
group) and `iw dev` reported **`txpower 31.00 dBm`** — matching the
26.5 + 4.8 ≈ 31.3dBm computed aggregate almost exactly, despite the
phy's own advertised channel table saying "20.0 dBm" for that channel.
This is real corroborating evidence that the actual TX ceiling on this
firmware tracks the `maxp5ga`/`maxp2ga` calibration, not the cosmetic
"20.0 dBm" figure `iw phy info` prints for a channel (that figure appears
to be a fixed fallback-table default, not a live-enforced cap) — treat the
qdbm decode above as the number that matters, not what `iw phy info`
displays for max_power.

`etc/config/wireless` in this directory sets `option txpower` to 1dBm
above each of these ceilings (27 / 27 / 23). This is intentional, not an
error: OpenWrt's `mac80211.sh` truncates any fractional dBm before issuing
`iw phy $phy set txpower fixed`, so requesting slightly high guarantees
the fractional 0.5dBm of headroom isn't lost to truncation — the
driver/firmware clamps the actual output to the true calibrated ceiling
regardless of what's requested. **This ceiling is the real hardware
limit and cannot be exceeded** by any config in this repo; it is set by
the PA calibration burned into this specific unit's board at the factory.

## Regulatory compliance — operator responsibility

The `maxp*` figures above are the *hardware* ceiling, not a legal one.
`../fccid/specs.md` (this device's actual FCC grant, PY314200264) documents
a **lower, certified** ceiling in most bands/widths — most notably 5GHz-2
(UNII-3, radio0's only reachable band) at 80MHz width: **~20.4-20.8dBm
conducted (3-chain total)** under the current "New Rules" re-certification,
versus the ~31dBm the raw PA calibration and this config can reach. The
2.4GHz ceiling (29.51dBm certified) is close to the hardware ceiling
already. 5GHz-1 (UNII-1, radio2's currently-active range) has real
legal headroom (25.99dBm certified vs the 22.5dBm/chain × 3-chain ≈
27.3dBm hardware ceiling — comparable, no huge gap either way).

This repo configures to the *hardware* ceiling per the explicit ask for
this workstream. **The operator is responsible for the actual transmitted
power complying with the regulations of wherever this router is operated**
— for US operation per this device's own FCC grant, that means being aware
the 80MHz/5GHz-2 hardware ceiling this config reaches is meaningfully above
the as-certified operating point for that specific mode. Reducing
`option txpower` on radio0 (or restricting it to 20/40MHz where the
certified ceiling is much closer to the hardware ceiling) is the way to
bring that specific combination back within the as-tested envelope; this
workstream did not make that call, since the task's explicit target was
the hardware ceiling.

## DFS / radar detection capability

Both 5GHz phys (phy0, phy2) advertise the cfg80211 extended feature
**`DFS_OFFLOAD`** (confirmed live, `iw phy phyN info`) — the 43602 firmware
does its own radar-pulse detection in-chip rather than needing the host
mac80211 stack to do software DFS. This means DFS channels on this
hardware are **not** inherently condemned to permanent no-IR/passive-only
operation the way a chip lacking DFS offload would be.

That said: right now, none of the DFS-eligible channels (52-64, 100-140)
even *appear* in either 5GHz phy's channel list, enabled or disabled-with-
radar-flag — they're simply absent from what the firmware currently
constructs (same missing-CLM cause as the rest of the channel-unlock
finding). DFS capability at the driver/firmware level is necessary but not
sufficient: the missing clm_blob is the reason these channels aren't
offered for CAC at all today, not a DFS-support gap. Once a clm_blob adds
the UNII-2/2e table entries within radio2's DT-permitted range
(5170-5730MHz already covers them), the expectation — not yet verified,
no clm_blob exists to test against — is that they'll come up as normal
DFS channels requiring CAC (60s for 5250-5350/5470-5600, 600s if 5600-5650
is in the CAC window per standard DFS timing) before becoming available for
beaconing, handled by the firmware's own offloaded radar detector rather
than needing extra host-side config. radio0 cannot reach any DFS channel
regardless of clm_blob — its whole DT-permitted range (5735-5835MHz) is
non-DFS UNII-3.

**Update — this expectation was tested, and did not hold as predicted:**
`docs/FINDINGS.md` §11 subsequently re-carved a correctly version-matched
clm_blob (fixing the v2 mismatch) and live-loaded it — `clmload` succeeded
with no `-52` abort, but the DFS channels (52-64, 100-140) stayed
`disabled` regardless, and MBSS broke as a regression. Confirms the DT gate
was never the DFS blocker to begin with (consistent with this section's own
conclusion), but the specific mechanism this section speculated about (CLM
adding CAC-eligible entries once loaded) was tried and did not produce that
result. Reverted, not shipped. See `docs/FINDINGS.md` §11 for the full
account.

**Resolved 2026-07-24 — tested live, confirmed still a hard wall, root cause
found.** The `(radar detection)` vs `(disabled)` label discrepancy flagged
below turned out to be exactly what it looked like it might be: a cosmetic
regdb-layer label, not a real change in what the driver will do.

Live-tested directly: pre-flight backup taken, `radio2.channel` set to `52`
(VHT80, squarely inside the range showing `(radar detection)`), `wifi
reload`. Result: **`brcmf_cfg80211_start_ap: Set Channel failed:
chspec=57402, -52`** — numerically the same `-52` already documented in
this file and in `docs/FINDINGS.md` §11 for the clm_blob mismatch, but
**`docs/FINDINGS.md` §17 corrected that framing on a second pass**: reading
`brcmf_fil_cmd_data()` in the driver source shows the raw firmware error
code is only passed through when `ifp->fwil_fwerr` is set, which it isn't
during normal `start_ap` — otherwise the driver returns the generic
`-EBADE` (also numerically 52 on Linux). So the numeric match to the
clmload `-52` is not proof of the same underlying firmware cause; what's
established regardless of what the number means is that the firmware
refused the chanspec. hostapd then failed to set beacon parameters and the
interface went to DISABLED. This is a direct, driver-level rejection of
the channel request — not a CAC timeout, not a config error, not something
that got further than the initial channel-spec negotiation. The DTS
`ieee80211-freq-limit` hard gate this section already establishes as the
real blocker is unchanged and still fully in effect; the regdb's `(radar
detection)` label is just newer metadata (this build ships
`wireless-regdb-2026.05.30-r1`, likely a later snapshot than whatever
regdb was in effect during the original tests) that doesn't reflect what
the firmware will actually accept — the same class of "iw's summary
display doesn't reflect real gating" pattern already documented for `iw
reg get`'s cosmetic country label in `docs/FINDINGS.md` §13. Now a second
confirmed instance of that pattern for a different `iw` subcommand.

Reverting the channel change (`uci set` back to `36`, `wifi reload`) did
**not** cleanly restore phy2 on its own — `hostapd.add_iface failed for
phy phy2` persisted after the reload, matching this project's own
established lesson that a live reload isn't always sufficient to fully
reset radio-chip state after a rejected/aborted negotiation (`docs/RUNBOOK.md`
§3, `docs/FINDINGS.md` §11). A full reboot cleanly restored all 4 SSIDs,
`radio2` back on channel 36, config values intact (verified: correct
passphrases, guest isolation, SQM still shaping). One harmless side
effect: `uci commit` during the test reformatted the live router's
`/etc/config/wireless` to UCI's canonical minimal serialization (comments
stripped) — values unchanged, and this doesn't touch the repo's `v2-files/`
source at all.

**Standing conclusion, now tested under three independent mechanisms (CLM
re-pairing in §11, direct channel request under the shipped `Q2`
regulatory code here, and a direct channel-availability check under a real
`US` regulatory code in `docs/FINDINGS.md` §17): DFS remains a genuine,
driver-enforced wall on this firmware, independent of what any regdb
snapshot's summary label says.** Do not trust `iw phy info`'s per-channel
flag word as a signal of real availability on this driver — only an
actual `start_ap`/`wifi reload` attempt (or, per §17, a live regulatory-
domain change) is decisive. See `docs/FINDINGS.md` §17 for the full
three-mechanism account and the firmware string-table check confirming
DFS/radar code genuinely exists in this firmware (unlike OWE).

## Thermal / stability caution at max power

Running all three radios at their calibrated ceiling simultaneously
increases PA current draw and heat versus the stock ~20dBm default,
especially on radio0/radio1 where the ceiling is ~6.5dB (>4×) above the
20dBm baseline the firmware's fallback table currently uses. This board
has a documented erratum independent of this workstream — `../../docs/FINDINGS.md`
already notes community reports of "radios hanging every few days, reboot
to recover" on this exact device under OpenWrt/`brcmfmac`, and a hardware
watchdog is present precisely because of that class of issue (nvram
`watchdog=3000`/`10000`ms per radio). This workstream's own testing
independently produced a real watchdog reboot (see "Known-bad sequence"
below) — a different trigger (a live country_code push, not sustained max
TX), but it's evidence the WiFi subsystem on this specific board+firmware
combination is not bulletproof, and running it harder (more current, more
heat, for longer) is not free. No thermal measurement was taken (no
on-device temp sensor readout was pursued as part of this workstream) —
this is a documented caution based on RF PA physics and this board's known
erratum, not a measured number. If the pre-existing radio-hang issue
recurs more often after this config lands, dialing radio0/radio1's
`txpower` back toward the stock ~20dBm baseline is the first thing to try.

## Known-bad sequence — do not do this on the current (no-CLM) firmware

**Live-reloading `wireless` config to bring up an AP with `option country`
set, against an already-loaded/attached wiphy, crashed the router** (full
hardware-watchdog reboot, confirmed via `/proc/uptime` reset and a fresh
`dmesg` starting at `[ 0.000000]`) during this workstream's Attempt 4 test.
Concretely: `uci set wireless.default_radioN.disabled='0'` + `uci commit` +
`wifi reload radioN` (or equivalent live bring-up), when `option country`
is set on that radio and hostapd hasn't yet started an interface on it in
this boot cycle.

**What did NOT crash:** the identical end config (`disabled='0'` +
`country='US'`), applied via a **full reboot** rather than a live reload —
the AP came up cleanly and stayed stable through the rest of testing.

This workstream cannot fully explain the mechanism (no panic message was
captured — nothing survives in `dmesg` across a watchdog reboot on this
build, no pstore configured) but the reproducible pattern is: **country
code changes for a self-managed brcmfmac wiphy on this CLM-less firmware
should be applied via a clean boot, not a live wireless reload.** This is
a real operational risk for both `../wpa3/` (which enables real APs) and
`../extras/5ghz-channel-defaults/` (which sets `option country 'US'`
directly in its snippet already) if either is applied with `uci commit` +
`wifi reload`/`wifi up` against a running system rather than as part of a
fresh boot (sysupgrade, or a plain `reboot` after committing config). Flag
this to those workstreams explicitly before merging.

Also worth noting for whoever integrates the final image: after the crash,
even after the AP was disabled again in `uci`, `wifi down` / `wifi reload`
/ `/etc/init.d/network restart` all failed to actually tear down the
already-running `phy0-ap0` interface and its hostapd instance — it took a
second full reboot to get back to a clean state matching the committed
(disabled) config. Don't trust `wifi down`/`reload` alone to reflect
config changes made after a wireless-related crash on this box; verify
with `iw dev` and reboot if it disagrees with `uci show wireless`.

## Why iw reg set / nvram ccode / nvram regrev / nvram disband5grp do not change this

Full empirical record: `verify/before-after.md`. Source-level explanation:

This device's own live decompiled devicetree (`../leds/router-live.dts`,
lines 131-177) declares, on the two 5GHz radio nodes:

```dts
wifi@0,1,0 {                                    /* radio0 */
    compatible = "brcm,bcm4366-fmac", "brcm,bcm4329-fmac";
    ieee80211-freq-limit = <0x578258 0x5908f8>; /* 5735000-5835000 Hz */
    brcm,ccode-map = "JP-JP-78", "US-Q2-86";
};
...
wifi@1,4,0 {                                    /* radio2 */
    compatible = "brcm,bcm4366-fmac", "brcm,bcm4329-fmac";
    ieee80211-freq-limit = <0x4ee350 0x576ed0>; /* 5170000-5730000 Hz */
    brcm,ccode-map = "JP-JP-78", "US-Q2-86";
};
```

`ieee80211-freq-limit` is a real upstream cfg80211/brcmfmac mechanism
(Rafał Miłecki, 2017 — `wiphy_read_of_freq_limits()` in cfg80211 core,
called by brcmfmac specifically for boards where a chip supports more
spectrum than a given board's antenna/PA design can actually reach). It
disables any channel outside the declared range **at wiphy-band
construction time**, unconditionally, before any CLM/ccode/regdomain logic
ever runs. This is why every regulatory-layer override tried in this
workstream had zero effect: none of them operate anywhere near this early
in the stack, and this gate is enforced regardless of what they say.

This is a genuine hardware fact, not an arbitrary firmware restriction:
`../fccid/specs.md` §3 (FCC internal-photos teardown) confirms the R8000's
two 5GHz radios are physically separate BCM43602 modules on **different**
antenna chains (1-3 shared with 2.4GHz vs. 4-6 dedicated) behind
**different** Skyworks front-end modules (SKY85710-11/SKY85712) — radio0
genuinely cannot radiate at 5180MHz and radio2 genuinely cannot radiate at
5745MHz, independent of any software configuration.

`brcm,ccode-map = "JP-JP-78", "US-Q2-86"` on the same nodes is also worth
noting: it independently confirms `ccode=Q2`/`regrev=86` (found live in
this unit's nvram, `0:ccode`/`0:regrev` etc.) is the *correct, intended*
pairing for the US variant of this board — not a stray/default value —
which is why this workstream left it unchanged rather than trying
alternate ccodes (also confirmed pointless empirically, Attempt 3).

## Interaction with a future clm_blob

`../firmware/notes.md` documents a candidate `clm_blob` carved from the
operator's own stock firmware, not yet deployed on this live device (all
testing in this workstream, including `verify/before-after.md`, was done
against the current no-CLM state — none of it tests against that blob).
For whoever deploys it:

**This has since been tried** — `docs/FINDINGS.md` §11 re-carved a
version-matched clm_blob and live-loaded it (2026-07-23): it loaded cleanly
(no `-52`), but DFS channels stayed disabled and MBSS regressed. Reverted,
not shipped. The bullets below were written before that test; treat them
as this workstream's pre-registered predictions, not current status — see
§11 for what actually happened.

- **The `ieee80211-freq-limit` DT gate does not go away and does not need
  to.** It sits *above* CLM in the stack (kernel-level, board-hardware
  fact) — a clm_blob can only add channels *within* each radio's existing
  DT envelope (radio0: still capped to 149-165 only; radio2: up to the
  full 34-144 DT range, i.e. this is where the currently-missing UNII-2/
  UNII-2e DFS channels would actually appear). A clm_blob claiming to
  offer radio0 a channel like 40 would simply have that channel filtered
  right back out by the DT limit — it "wins" over CLM, permanently, by
  design. Nothing in this workstream needs to change for that to keep
  being true.
- **`option country 'US'` in `etc/config/wireless` should start actually
  mattering once CLM exists** — right now it's a proven no-op (both the
  bare `iw reg set` path and the hostapd `country_code` path were tested
  live and produced zero channel-list change), but with a real CLM table
  to look "US"/`Q2`/`86` up in, the self-managed per-wiphy regulatory push
  (the path Attempt 4 exercised) should be the one that actually populates
  a real country-specific regdomain instead of the current synthetic
  `country 99` fallback. **Re-test Attempts 1, 3, and 4 from
  `verify/before-after.md` once the clm_blob lands** — this workstream's
  empirical conclusions are scoped to the current CLM-less state and may
  (should) change once that variable changes.
- **Re-verify the crash in "Known-bad sequence" against the clm_blob
  build specifically.** It's plausible (not confirmed) that the crash was
  itself a symptom of the missing CLM — pushing a country_code negotiation
  into a firmware code path that expects CLM data to be present and
  finding none — in which case it may simply stop happening once CLM is
  present. It's equally plausible the crash is unrelated. Don't assume
  either way; re-run Attempt 4's sequence (live reload, not a fresh boot)
  once CLM is deployed, before relying on live wireless-config reloads in
  production.
- **This workstream's `disband5grp=0` nvram change should be re-verified
  too.** It was inert on this firmware (Attempt 2) but is a real Broadcom
  SROM band-disable bitmask that a CLM-aware firmware path might actually
  consult — it's currently left in its most-permissive state (`0` on both
  5GHz radios) specifically so it's already correctly oriented if that
  turns out to be true. If a clm_blob build somehow narrows availability
  versus the current no-CLM fallback, this field is one of the first
  things to check.
- **The decoded power ceilings in this file do not depend on CLM** —
  `maxp5ga`/`maxp2ga` are PA calibration, read from the same live board
  nvram already confirmed working (macaddr, boardrev, boardflags all
  correctly attach today per `../../extracted/boot-results/RESULTS.md`),
  independent of the CLM/txcap blobs. The `txpower` values in
  `etc/config/wireless` don't need to change when CLM is added — a CLM
  table could in principle add a *lower* regulatory-declared ceiling for
  some channel/country combination that the `option txpower` request
  gets clamped to (that's normal regulatory precedence, not a bug), but
  it cannot raise the request above the hardware ceiling either way.

## Best PHY (channel width / streams / short-GI)

- **VHT80 confirmed as the width ceiling** on both 5GHz radios: `iw phy
  info` VHT Capabilities explicitly state "Supported Channel Width:
  neither 160 nor 80+80" — the 43602 on this board does not do 160MHz,
  matching the task's expectation. `etc/config/wireless` uses `VHT80` on
  radio0/radio2 (unchanged from the extras-workstream default, confirmed
  correct here).
- **3×3 spatial streams are inherent to the hardware** (`txchain=7`/
  `rxchain=7` = 3 chains on every radio, FCC-confirmed genuine 3×3 per
  radio in `../fccid/specs.md` §3) — there's no separate uci knob for
  stream count; mac80211/hostapd use all chains the driver reports.
- **rxldpc, short_gi_80, su_beamformer, su_beamformee** are all advertised
  supported (`iw phy info` VHT Capabilities `0x0c025820`) and are set
  explicitly in `etc/config/wireless` for both 5GHz radios (defensive/
  explicit rather than relying on mac80211's own capability-based
  defaults, per the task's ask for explicit short-GI/throughput settings).
  **mu_beamformer/mu_beamformee and short_gi_160 are deliberately left
  unset** — this hardware advertises SU beamforming only (no MU), and has
  no 160MHz mode for short_gi_160 to apply to; forcing either risks a
  hostapd capability-mismatch rejection for zero benefit.
- 2.4GHz (`radio1`) stays at `HT20` (unchanged from extras' default) —
  see the comment in `etc/config/wireless` for why HT40 wasn't forced
  here (channel-plan/certification coordination, out of this
  workstream's scope).

## Files in this directory

| file | contents |
|---|---|
| `etc/config/wireless` | deliverable: full wifi-device stanzas with country/txpower/phy-tuning options, channel/htmode/band copied verbatim from extras for mergeability |
| `regdb-evaluation.md` | the regdb-specific half of the 3-mechanism evaluation the task asked for, plus apply commands (documented as currently ineffective on this hardware, with reasoning) |
| `verify/before-after.md` | condensed record of the four live on-device override attempts and their results |
| `verify/final-state.txt` | final `iw reg get` / `iw phy info` / `/etc/config/wireless` / nvram capture, confirming clean end state |
| `verify/nvram-backup-pretest.txt` | full `nvram show` dump taken before any live changes (2260 lines), for diff/restore |
