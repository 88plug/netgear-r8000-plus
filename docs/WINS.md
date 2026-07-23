# R8000 → OpenWRT — Complete Wins Log

Every capability shipped, from a factory-reset router to a custom OpenWRT build
that fixes bugs the community called unfixable and a kernel driver limit called
impossible. All verified on the live device. Branch `openwrt-r8000-plus`.

---

## 🏆 Headline: fixed a brcmfmac kernel driver bug → multi-BSS unlocked

The `-95`/EOPNOTSUPP that made OWE, guest networks, and multi-SSID-per-radio
"impossible" on BCM43602 was a **driver bug**, not a chip limit.

- **Root cause:** `brcmf_cfg80211_request_ap_if()` tries the modern
  `interface_create` iovar; the R8000's 2015 firmware doesn't implement it, so
  the version query fails and the driver did `return -EOPNOTSUPP` — skipping the
  legacy `bsscfg:ssid` MBSS fallback right below it that this firmware DOES
  support (MBSS was even detected; it advertised `#{AP}<=4`).
- **Fix:** `patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch` (2 lines) —
  fall through to the legacy path instead of bailing.
- **Built** as `kmod-brcmfmac` via the 25.12.5 bcm53xx **SDK** (ABI/vermagic
  matched), **baked into v3** via a FILES overlay.
- **Proven on hardware:** 2nd AP `add_iface` → exit 0 (was -95); **6 BSSes**
  beaconing (R8000 ×3 + R8000-Open + R8000-Open-OWE + R8000-Guest), up from a
  hard cap of 3. Upstreamable to OpenWrt mac80211 / linux brcmfmac.

---

## The full win list

| # | Win | Proof |
|---|---|---|
| 1 | **5GHz revived** (OpenWrt #20514, "unfixable") | Root-caused to a missing `netgear,r8000` bcm53xx nvram-init entry (present for the twin RT-AC3200, never the R8000); fixed with this unit's own extracted calibration. `patches/0001`. 3 radios up. |
| 2 | **Calibration extracted** from the live router | Nighthawk telnet backdoor → root shell → full NVRAM dump; 137 per-radio cal keys incl. 5GHz `pa5ga`. Community said this data was "embedded in the driver binary" / unrecoverable. `extracted/`. |
| 3 | **WPA3-SAE + MFP** | `SAE / FT-SAE (CCMP)` on all radios |
| 4 | **802.11r fast roaming** | `FT-SAE / FT-PSK` in beacons |
| 5 | **802.11k RRM** | neighbor/beacon reports |
| 6 | **802.11v BSS-Transition** | switched to full `wpad-mbedtls` (`CONFIG_WNM=y`) — enabled the feature instead of stripping it; `bss_transition=1`, 0 hostapd errors |
| 7 | **usteer band-steering** | `/sbin/usteerd` steering across the 3 radios (unified `R8000` SSID) |
| 8 | **OWE (Enhanced Open)** | transition pair live (driver fix) |
| 9 | **Guest network / multi-SSID** | `R8000-Guest` 2nd BSS (driver fix) |
| 10 | **SQM/cake** | installed; set WAN bandwidth to activate |
| 11 | **Software flow-offload** | `flow_offloading=1` |
| 12 | **LEDs fixed** | all front-panel LEDs defined (Power/WAN/2.4/5-1/5-2/USB/WPS) |
| 13 | **LuCI web UI** | :80 |
| 14 | **Max TX power** | txpower at the decoded PA calibration ceiling (26.5/26.5/22.5 dBm/chain), firmware-clamped |
| 15 | **5GHz split VHT80** | phy0 upper (ch149/153) + phy2 lower (ch36), no co-channel |
| 16 | **FCC hardware truth** | FCC ID PY314200264: confirmed genuine **3×3** (not 2×2), per-band certified EIRP, PA headroom |

---

## Honest walls (proven limits, not effort gaps)

| Item | Why it's a real wall |
|---|---|
| DFS-channel unlock (clm_blob) | Extracted blob firmware-rejected (`-52`), fatal to radio init; no valid clm exists for 43602 |
| 802.11s mesh · airtime-fairness | brcmfmac driver/firmware doesn't advertise them on these radios |
| Latest radio firmware | Newest upstream `brcmfmac43602-pcie.bin` is byte-identical to the 2015 blob already installed |
| "De-neuter regulatory tables" for more channels | Channels are gated by DTS `ieee80211-freq-limit` (hardware antenna diplexing), not by the regdb — regdb is inert here (4 override methods tested; one crashed the router) |
| MLO | N/A — R8000 is WiFi 5 (VHT); MLO is a WiFi 7 feature |

---

## Reproducibility

- **Patches:** `patches/0001` (5GHz nvram init), `patches/861` (brcmfmac MBSS) — both upstreamable.
- **Images:** `images/*-r8000plus-v3-*.chk` (final), plus v1/v2/v2b/v2c history + stock revert `.chk`.
- **Built package:** `images/packages/kmod-brcmfmac-*.apk`.
- **Docs:** [README](README.md) · [RUNBOOK](RUNBOOK.md) (access/flash/recovery) · [FINDINGS](FINDINGS.md) (technical detail §1–8).

## v5/v6 — app-plus pass, sysupgrade bug caught live, packaging regression found+fixed

Scoped app-plus graveyard-mining pass (bcm53xx + brcmfmac only, not the full
openwrt/openwrt monorepo) plus a direct anti-dogma re-investigation of two
claimed walls, under live-hardware testing. Full detail: FINDINGS.md §9–12.

- **radio-watchdog shipped** (v5+): an escalating brcmfmac-wedge recovery
  daemon, fully written earlier but never wired into the image, confirmed
  against still-open upstream issue #14685. Zero regression risk, pure
  addition.
- **OWE confirmed unfixable, with real rigor this time.** Traced the actual
  mechanism via Broadcom's own proprietary DHD driver source (not just
  reading the absence of code): OWE requires a firmware-emitted event
  (`WLC_E_OWE_INFO`) carrying DH key material for host-side ECDH — this has
  to be compiled into the wl0 firmware binary. Our firmware (2015-09-18)
  predates OWE's 2016 RFC by a year; no driver patch can add a firmware-side
  protocol handler. Removed from the shipped config.
- **DFS/clm_blob root cause found — real, not a wall, but a hazard.**
  v2's "fatal clmload -52" was traced to a genuine bug in our OWN extraction:
  the CLM blob was paired with a *different, newer* firmware build (2021)
  than what's actually loaded (2015). Live-tested the correctly-paired
  firmware+CLM: it loads cleanly (proving the mismatch theory) but the newer
  firmware **regresses MBSS** (`interface_create` now fails `-52` instead of
  the `EOPNOTSUPP` patches/861 targets) and DFS channels **still** stayed
  disabled (confirms the separate DTS-gating wall). Reverted to the
  known-good 2015 firmware/no-CLM combination. Real finding, not re-shipped.
- **Found and fixed our own v5 packaging regression.** Rebuilding v5 via
  plain ImageBuilder `make image` silently pulled the *stock* upstream
  `kmod-brcmfmac` from the official binary repo instead of our
  patches/861-patched local build (the patched `.apk` had been archived to
  `images/packages/` but never copied into ImageBuilder's own `packages/`
  dir for this rebuild) — silently reverting the MBSS fix. Caught via
  `iw dev` interface-count verification, confirmed via module checksum diff,
  fixed by placing the patched apk correctly and rebuilding as v6.
  **Lesson: verify the actual deployed artifact, not just the package list.**
- **Live-confirmed upstream #21655 (sysupgrade config-loss) on our own
  device**, twice (v5 and v6 flashes) — `/etc/config/{wireless,firewall,sqm,
  system,usteer}` reset to defaults on both config-preserving sysupgrades,
  exactly matching the bug thread's description. Recovered cleanly both
  times because a pre-flight `sysupgrade -b` backup was already
  standard procedure by then. **This is now mandatory before every future
  sysupgrade** — see RUNBOOK.md.

**v6 is the current shipping image**: v4's proven wins + radio-watchdog +
OWE cleanly removed (no dead interfaces) + the packaging regression fixed +
5GHz/MBSS/WPA3/802.11r-k-v/guest-network all independently re-verified live
after two full flash-and-recovery cycles.

## v6.1 — finish pass: guest isolation, passphrases, regdomain root-cause, FA next-gate (2026-07-23)

- **Real WiFi passphrases live.** Temp `ChangeMe-R8000-2026`/`GuestChangeMe2026`
  replaced with random 20-char alnum passphrases on the router and in
  `v2-files/etc/config/wireless`.
- **Guest network isolated.** `R8000-Guest` moved off `lan` onto its own
  bridge/subnet (`br-guest`, 192.168.2.0/24) with a dedicated firewall zone
  (forward-allowed to `wan` only, explicit reject rule to `lan`) — verified
  live (`br-guest` up, zone/forwarding/rule confirmed via `uci show
  firewall`). `v2-files/etc/config/{network,firewall,dhcp,wireless}` now
  carry this as a reproducible template.
- **"Stuck regulatory domain" finding closed — cosmetic, not a bug.**
  `iw reg get`'s per-phy `country 99: DFS-UNSET` label is a brcmfmac
  self-managed-wiphy display artifact; actual applied power/channels
  (verified via `iw dev ... info` / `iw phy ... channels`, not just the
  summary line) correctly reflect `US` — 31 dBm firmware-clamped, full
  34-channel table. Full trace in FINDINGS.md §13. Separately reconfirmed
  live: a `wifi`/`network` hot-reload (as opposed to a fresh boot) *does*
  still visibly degrade real operation to 20 dBm/no-DFS, matching the
  existing "reboot after any hot-reload with country set" rule.
- **FA hardware-offload research: chip identity resolved, bring-up still
  gated.** Live `dmesg` confirms the R8000's actual SiliconBackplane SoC
  chip id as `53010 rev 0x00` (distinct from the separately-confirmed
  switch-IP self-ID `BCM53012 rev 5`) — closes the die-revision unknown
  `GO_NOGO.md` flagged before any `fa_up()` bring-up could be considered.
  A second, post-reboot `fa_probe.ko` read returned byte-identical
  `control`/`status` values to the first — stable, repeatable, real evidence
  the block is driven, not floating bus noise. Full detail:
  `hwoffload-research/fa-probe/GO_NOGO_BRINGUP.md`. **The register-write
  bring-up itself was deliberately not run** — it's a distinct risk class
  from a read-only probe and stays its own future go/no-go.
- **Switch-side FA glue reconciled with mainline.** Confirmed mainline
  `b53_srab.c`'s generic `b53_srab_read/write{8,16,32,48,64}` primitives are
  function-for-function equivalent to `bcmrobo.c`'s own SRAB bus layer — the
  switch-side port (`VERDICT.md` §2 step 4) needs new page/offset register
  writes on top of already-running infrastructure, not a new bus driver.
  Lowers that step's effort estimate from Medium-Low toward Low.
- **`sar2g`/`sar5g` NVRAM — decided: leave unset**, and **UART vs
  pstore/ramoops — decided: prefer pstore/ramoops** (deferred to its own
  build+DTS-change pass, not silently done). Both documented in
  FINDINGS.md §13.
- **EAP_MODE_SIMPLIFIED (#21349) — closed**, folded into FINDINGS.md §13
  from where it lived (`v2-staging/extras/dsa-switch/NOTES.md`): the
  upstream fix is already present and applied in this repo's 25.12.5 tree,
  no action needed.

## Outstanding (genuinely blocked, not effort gaps)

- **SQM WAN bandwidth** — cannot be set correctly right now: `wan@eth2` is
  link-down (`LOWERLAYERDOWN`, no cable connected) in this bench setup, so
  there's no real ISP link to measure. `sqm.wan.download`/`upload` are `0`
  (idle) until the router is deployed at its real network location with the
  actual WAN link connected — set them from a real speed test at that point,
  not a guessed number now.
- Always run `sysupgrade -b` and download the backup **before** any future
  sysupgrade — config loss on this device is confirmed, not hypothetical
  (process rule, see RUNBOOK.md, not a one-time task).

## v7 — consolidated build: everything actually baked into one shippable image (2026-07-23)

Every prior workstream (WPA3, guest isolation, roaming, perf-tune, LED fix,
flow-offload, radio-watchdog) had been live-verified on the router by hand at
some point, but `image-files/` — the directory actually used to build the
shipped v6 `.chk` — only ever carried radio-watchdog + nvram, not the rest.
v7 closes that gap: `v2-files/` (the complete, consolidated overlay) is now
the actual `FILES=` source for a real build, not just a reference tree.

- **Caught a real regression before it shipped a second time.**
  `v2-files/lib/modules/6.12.94/brcmfmac.ko` (a stale raw-`.ko` artifact,
  gitignored, never actually used to build anything) did not match the
  driver actually running on the router. Verified empirically (unpacked
  `images/packages/kmod-brcmfmac-6.12.94.6.18.26-r1.apk` with apk-tools'
  own `adbdump`, diffed its embedded file hash against the live module) —
  the `.apk` is correct, the stale `.ko` was excluded from the build overlay
  entirely. Same class of mistake `docs/WINS.md`'s v5→v6 packaging fix
  already caught once; caught the same way this time: verify the actual
  artifact, not the file that happens to be sitting in the tree.
- **Found and fixed a live regression the flash itself exposed.** The v7
  image's first build used `wpad-basic-mbedtls` (per
  `v2-staging/wpa3/imagebuilder-packages.md`'s explicit recommendation) and
  shipped `bss_transition` (802.11v) in the merged wireless config anyway —
  hostapd rejected the whole config as an unknown directive, taking down
  **all four SSIDs** (3 main + guest) on first boot. This is the exact bug
  `docs/FINDINGS.md`'s v2b entry had already found and fixed once, silently
  regressed by a later, narrower-scoped staging doc. Re-fixed the same way:
  swapped to `wpad-mbedtls` (`-wpad-basic-mbedtls wpad-mbedtls` in
  `PACKAGES=`, since the two conflict and the device profile pulls in
  `wpad-basic-mbedtls` by default). Rebuilt, reflashed, verified live:
  zero hostapd errors, `bss_transition=1` active in the running config, all
  4 SSIDs up. Corrected both `FINDINGS.md` and the wpa3 staging doc so this
  doesn't regress a third time.
- **v7 is the current shipping image**, `openwrt-25.12.5-r8000plus-v7-bcm53xx-generic-netgear_r8000-squashfs.chk`,
  sha256 `a513eece58dcae7c4d2f50d030c71ce2b5dfd0635a02da675243eb66abbd2c67`.
  Verified live post-flash: config sizes non-default (no #21655 hit this
  time), guest SSID + firewall isolation intact, `radio-watchdog`/`perf-tune`
  enabled, driver checksum matches the confirmed-good module.

## v8 — wifi hardening + one closed performance gap (2026-07-23)

- **OCV (Operating Channel Validation)** enabled on all three main radios —
  anti channel-manipulation downgrade defense, OpenWrt's own standard
  hardening for WPA3/802.11r. **Shipped as unverified-against-real-client-
  hardware** (documented in `v2-files/etc/config/wireless`'s header, with a
  one-line rollback) rather than silently declared done — OCV has known
  interop bugs with buggy client OCV implementations, and this exact repo
  just proved (`bss_transition`, this session) that untested hostapd config
  changes can silently break association. Live-verified only that it
  doesn't break *this* boot: zero hostapd errors, all 4 SSIDs up, `ocv=1`
  active in the running config.
- **Guest SSID client isolation closed.** `option isolate '1'` (→
  `ap_isolate=1` in the running hostapd config) was flagged as a gap in the
  original guest-isolation code review and never actually closed — a guest
  network that isolates from LAN but lets guest devices see each other
  wasn't a complete implementation of its own goal. Verified live in the
  running `hostapd-phy1.conf`.
- **Closed the `ethtool` verification gap** perf-tune's own research
  (v3-staging/perf/notes.md item 4) had left open. `ethtool` package added,
  confirmed working live: `rx-checksumming` and `tcp-segmentation-offload`
  are hardware-fixed **off** on `bgmac` (not toggleable, not a config gap),
  `tx-checksumming` and `generic-receive-offload` are **on**. Real data
  replacing a "could not verify" note.
- **v8 is the current shipping image**, `openwrt-25.12.5-r8000plus-v8-bcm53xx-generic-netgear_r8000-squashfs.chk`,
  sha256 `cac883b07e5357c4a56662c305c326efb048edabc339791718a3b1a1ec307cd2`.

## FA/CTF hardware accelerator: silicon presence CONFIRMED (2026-07-23)

The single biggest open question in this project's hardware-offload
research — is the Flow-Accelerator silicon block physically present,
powered, and functional on this exact R8000, or fused/absent like the
2015-era stock firmware's total lack of FA code suggested — is now
**resolved: it's real, it's alive, and its control plane works.**

`fa_bringup.c` performed the actual `fa_setmode()`-equivalent table-init
write (from Broadcom's own leaked SDK6 `etc_fa.c`) directly against the
live router's FA register block at `0x18027c00`: all 5
`CTF_INTSTAT_*_INIT_DONE` bits asserted within 1ms, each answering its own
distinct control bit correctly, self-cleared as one-shot strobes should on
unload while the persistent config bits stayed set — sophisticated,
correct, purposeful hardware behavior, not floating bus noise. Zero
adverse effect on the running router: uptime unbroken, all 4 SSIDs
untouched, no new errors. Full writeup:
`hwoffload-research/fa-probe/BRINGUP_RESULT.md`.

This was gated behind its own explicit operator go-ahead, separate from
the earlier read-only probe's authorization, per this project's own
`GO_NOGO_BRINGUP.md` — and scoped deliberately narrow: NAPT/next-hop table
row programming (the actual data-plane write path, needs the WAR777
workaround) and switch-side `robo_fa_enable()` over SRAB (discovered
mid-implementation to be a required part of a *complete* `fa_up()`, and a
materially different risk surface — it touches the DSA switch carrying all
LAN ports, not just this isolated GMAC-3 sub-block) were both explicitly
left untouched. Those are the next real steps, and each needs its own
separate go/no-go, the same discipline that gated this one.
