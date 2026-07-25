# Channel-unlock on-device verification (2026-07-23, live 192.168.1.1)

Four independent override mechanisms were tried live, in sequence, each
checked against `iw phy phyX info` before and after. All four left the
per-channel disabled/enabled state **byte-for-byte identical**. Raw
transcripts of each step are in this session's tool log; this file is the
condensed record with the baseline and final captures.

## Baseline (before any override attempt)

```
$ iw reg get
global
country 00: DFS-UNSET
  (five rules, incl. 5735-5835 @80 PASSIVE-SCAN, 5490-5730 @160 DFS, etc.)
phy#2  country 99: DFS-UNSET  (2402-2482@40) (2474-2494@20) (5140-5360@160) (5460-5860@160), all (6,20)
phy#1  country 99: DFS-UNSET  (same 4 rules)
phy#0  country 99: DFS-UNSET  (same 4 rules)

$ iw phy phy0 info | grep MHz    # radio0, 5GHz-high
  34,36,38,40,42,44,46,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144: all (disabled)
  149,153,157,161,165: 20.0 dBm

$ iw phy phy2 info | grep MHz    # radio2, 5GHz-low
  36,40,44,48: 20.0 dBm
  everything else (34,38,42,46,52-144,149-165): (disabled)

$ iw phy phy1 info | grep MHz    # radio1, 2.4GHz
  1-11: 20.0 dBm; 12,13,14: (disabled)
```

## Attempt 1 -- `iw reg set US` (global cfg80211 regdomain)

```
$ iw reg set US
$ iw reg get
global
country US: DFS-FCC   <- global domain DID change
phy#0/1/2  country 99: DFS-UNSET   <- UNCHANGED, self-managed wiphys ignore the global hint
$ iw phy phy0 info | grep MHz
  IDENTICAL to baseline (34-144 still all disabled, 149-165 still 20.0dBm)
```
**Result: no effect on any radio's actual channel list.** Confirms brcmfmac
registers these wiphys with `REGULATORY_WIPHY_SELF_MANAGED` -- the classic
"reg set" CLI command only ever touches the *global* fallback domain, which
self-managed wiphys explicitly do not consult.

## Attempt 2 -- nvram `disband5grp` (Broadcom 5GHz sub-band disable bitmask)

```
before: 0:disband5grp=0x7  (radio0)   2:disband5grp=0x18  (radio2)
$ nvram set 0:disband5grp=0; nvram set 2:disband5grp=0; nvram commit
$ rmmod brcmfmac_wcc; rmmod brcmfmac; modprobe brcmfmac   # re-probe, re-read nvram
$ iw phy phy3 info | grep MHz   # (renumbered phy after reload; same MAC/radio as old phy0)
  IDENTICAL to baseline
$ iw phy phy5 info | grep MHz   # radio2 after reload
  IDENTICAL to baseline
```
**Result: no effect.** `disband5grp` is a real Broadcom SROM field (its
value differs meaningfully between radio0/radio2 in stock nvram, and its
bit pattern lines up with which sub-band each radio happens to show
enabled), but on this firmware build -- no CLM loaded -- it is not being
consulted. See notes.md for why: the actual gate is a devicetree
`ieee80211-freq-limit` hard-disable in the kernel itself, which sits
upstream of any nvram/SROM field the firmware might otherwise read.

## Attempt 3 -- nvram `ccode` / `regrev` (Broadcom country + revision pair)

```
before: 0:ccode=Q2  0:regrev=86
$ nvram set 0:ccode=0; nvram set 0:regrev=0; nvram commit
$ rmmod brcmfmac_wcc; rmmod brcmfmac; modprobe brcmfmac
$ iw phy phy6 info | grep MHz   # radio0 after reload, ccode=0/regrev=0
  IDENTICAL to baseline (still only 149-165 enabled)
```
**Result: no effect.** Confirms the earlier dmesg diagnosis literally:
"no clm_blob available ... device may have limited channels available" --
without a CLM table there is nothing to look `ccode`/`regrev` up *in*, so
the firmware falls back to the same hardcoded default regardless of what
country/revision is requested.
(`0:ccode`/`0:regrev` restored to `Q2`/`86` immediately after this test.)

## Attempt 4 -- hostapd `country_code` push via a real AP bring-up (per-wiphy self-managed path)

This is the mechanism that actually *can* reach a self-managed wiphy
(hostapd's nl80211 driver back end issues a wiphy-scoped
`NL80211_CMD_REQ_SET_REG`, unlike the bare global `iw reg set` in
Attempt 1) -- so it's the most realistic "does the regulatory override
actually win" test.

```
$ uci set wireless.radio0.country='US'
$ uci set wireless.default_radio0.disabled='0'
$ uci commit wireless
$ wifi reload radio0
```

**The router hard-crashed and rebooted** (hardware watchdog: full cold
boot, `/proc/uptime` reset to ~0, `dmesg` restarted at `[ 0.000000]`,
no graceful shutdown log). Reconnected once the watchdog reboot completed.

Post-reboot, the AP came up cleanly on channel 153 (5765MHz, VHT80,
`5745-5825` group -- entirely within the pre-existing 149-165 set, i.e.
this specific attempt happened to land on an already-unlocked channel, not
a newly-unlocked one) and stayed stable. `iw reg get` post-reboot showed
the identical `country 99` custom domain on all three phys -- **no new
channels appeared**, confirming (a) the crash bought nothing regulatory,
and (b) even a "successful" hostapd country_code push doesn't move the
disabled/enabled channel set on this firmware.

Interesting side-observation while the test AP was up: `iw dev` reported
`txpower 31.00 dBm` for the live AP on channel 153 -- noticeably higher
than the "20.0 dBm" the phy's channel table declares, and it lines up
almost exactly with this workstream's independently-decoded PA ceiling
(`0:maxp5ga*[3]=106` qdbm = 26.5 dBm/chain conducted, +10·log10(3) for
3 chains ≈ 31.3 dBm). This is corroborating evidence (not proof) that the
live TX ceiling tracks the calibration data, not the regdomain's
advertised summary figure -- see notes.md "Decoded power ceiling".

The device was returned to a clean baseline afterwards: AP disabled again,
full reboot to clear the wedged/orphaned hostapd+phy0-ap0 state that a
plain `wifi down`/`wifi reload`/`/etc/init.d/network restart` could not
fully tear down (see notes.md "Known-bad sequence" for why a live reload
is worse than a fresh boot here). `disband5grp=0` on radio0/radio2 was
**left** at the permissive value from Attempt 2 (proven inert and stable
across two subsequent full reboots; harmless, and correctly oriented for
if/when a clm_blob starts actually consulting it). `ccode`/`regrev` were
restored to stock (`Q2`/`86`) on all radios.

## Final state (after all four attempts, post cleanup)

Captured in `final-state.txt` in this directory -- byte-for-byte identical
channel-enabled/disabled pattern to the baseline above, on all three radios.
`nvram-backup-pretest.txt` in this directory is the full `nvram show` dump
captured before any of the above began (2260 lines), for diff/restore
reference.

**This is a 2026-07-23 capture, not current state.** A live re-check on
2026-07-24 found radio2 (phy2)'s channel table no longer matches this
capture -- see notes.md's "Open item, flagged 2026-07-24" note in the DFS
section for the discrepancy and why it wasn't chased further or used to
rewrite this file's historical record.

## Bottom line

None of the four mechanisms available from userspace/nvram changed which
channels this firmware reports as available, on any radio, and one of them
(hostapd country_code push against a live-loaded self-managed wiphy on this
CLM-less firmware) crashed the router. See notes.md for the source-level
explanation (`ieee80211-freq-limit` devicetree hard-gate) and what would
actually need to change for more channels to become available.

**Note on reading `iw reg get`'s per-phy `country 99: DFS-UNSET` line above:**
`docs/FINDINGS.md` §13 later root-caused this label as a cosmetic display
artifact of brcmfmac's self-managed wiphys — it stays at that placeholder
regardless of whether country/power is genuinely applied; the real signal
is the per-phy channel list and `iw dev <if> info`'s applied txpower, which
is what Attempts 1-4 above actually checked (not just the label). Does not
change any conclusion here — this repo's own testing already used the
correct signal — but don't cite the `country 99` line by itself as proof of
anything in future work; see §13 for the full explanation.
