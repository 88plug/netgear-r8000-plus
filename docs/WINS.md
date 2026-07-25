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
- **Confirmed 2026-07-25, not just found here: this is a shared-driver bug,
  not an R8000 quirk.** `request_ap_if()` has zero chip-ID checks — same code
  path for every brcmfmac chip. Confirmed still present, unpatched, in current
  `torvalds/linux` master. Provenance-searched (11 queries, lore.kernel.org/
  patchwork/bugzilla/GitHub/Infineon community — Infineon are brcmfmac's
  current maintainers): closest prior art (Ian Lin/Infineon 2022) fixes a
  different failure point in the same function, not this one — no anticipating
  reference found for this specific fix. Realistically affects OpenWrt users
  on other BCM43602-class tri-band hardware (R7900, RT-AC3200 — confirmed same
  chip + OpenWrt-supported; **not** R7000/R7000P/R8500/R7500, which use
  different chips despite similar model numbers/marketing). See
  [README.md](README.md#device-support-beyond-this-router-researched-2026-07-25)
  for the full breakdown — realistic population is real but modest, not the
  "millions" a first guess assumed before checking.

---

## The full win list

| # | Win | Proof |
|---|---|---|
| 1 | **5GHz revived** (OpenWrt #20514, "unfixable") | Root-caused to a missing `netgear,r8000` bcm53xx nvram-init entry (present for the twin RT-AC3200, never the R8000); fixed with this unit's own extracted calibration. `patches/0001`. 3 radios up. |
| 2 | **Calibration extracted** from the live router | Nighthawk telnet backdoor → root shell → full NVRAM dump; 137 per-radio cal keys incl. 5GHz `pa5ga`. Community said this data was "embedded in the driver binary" / unrecoverable. `extracted/`. |
| 3 | ~~WPA3-SAE + MFP~~ → **WPA2-PSK (`psk2`) + MFP-optional** | SAE never actually broadcast — firmware rejects the key-mgmt on every boot, found via real-client RSN scan; reverted project-wide, live-confirmed (`wireless.*.encryption='psk2'`). See "Correction" section below, FINDINGS.md §14. |
| 4 | ~~802.11r fast roaming~~ **removed** | Depended on the same SAE/FT foundation; dropped alongside WPA3-SAE (`ieee80211r`/`ft_psk_generate_local`/`mobility_domain` removed, live-confirmed absent). See "Correction" section below. |
| 5 | **802.11k RRM** | neighbor/beacon reports; live-confirmed `ieee80211k='1'` |
| 6 | **802.11v BSS-Transition** | switched to full `wpad-mbedtls` (`CONFIG_WNM=y`) — enabled the feature instead of stripping it; `bss_transition=1`, 0 hostapd errors, live-confirmed |
| 7 | **usteer band-steering** | `/sbin/usteerd` steering across the 3 radios (unified `R8000` SSID); live-confirmed running |
| 8 | ~~OWE (Enhanced Open)~~ **removed** | Confirmed unfixable — this firmware (2015) predates OWE's 2016 RFC, no `WLC_E_OWE_INFO` in the binary. Removed from shipped config (v6). See v5/v6 section below, FINDINGS.md §9/§12. |
| 9 | **Guest network / multi-SSID** | `R8000-Guest` 2nd BSS (driver fix); live-confirmed on `br-guest`/192.168.2.0/24 |
| 10 | **SQM/cake** | installed; WAN bandwidth set 91000/89000 kbit/s (v11) — live-confirmed `tc qdisc` shaping both directions |
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
| DFS-channel unlock | Confirmed under three independent mechanisms (clm_blob re-pairing, direct channel request under shipped `Q2` regulatory code, direct channel request under a real `US` regulatory code — 2026-07-24) — same real-world outcome every time. Firmware genuinely contains DFS/radar code (unlike OWE, which has none), but nothing testable makes it deliver a usable channel. `iw phy info`'s channel-flag label isn't a trustworthy signal on this driver. FINDINGS.md §16-17. |
| 802.11s mesh · airtime-fairness | brcmfmac driver/firmware doesn't advertise them on these radios |
| Latest radio firmware | Newest upstream `brcmfmac43602-pcie.bin` is byte-identical to the 2015 blob already installed |
| "De-neuter regulatory tables" for more channels | Channels are gated by DTS `ieee80211-freq-limit` (hardware antenna diplexing), not by the regdb — regdb is inert here (4 override methods tested; one crashed the router) |
| MLO | N/A — R8000 is WiFi 5 (VHT); MLO is a WiFi 7 feature |

---

## Reproducibility

- **Patches:** `patches/0001` (5GHz nvram init), `patches/861` (brcmfmac MBSS) — both upstreamable.
- **Images:** `images/*-r8000plus-v11-*.chk` (current, flashed), plus full v1–v10 history + stock revert `.chk`.
- **Built package:** `images/packages/kmod-brcmfmac-*.apk`.
- **Docs:** [README](README.md) · [RUNBOOK](RUNBOOK.md) (access/flash/recovery) · [FINDINGS](FINDINGS.md) (technical detail §1–15).

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
  `v2-files/etc/config/wireless` (real passphrases — that file is gitignored,
  never committed, confirmed 2026-07-25 when the repo's GitHub-publish
  readiness was audited). **Gap found and fixed the same day:** the repo had
  no tracked template for that gitignored file, so a fresh clone would be
  missing it entirely and `make image` would fail. Added
  `v2-files/etc/config/wireless.example` (tracked, `CHANGE-ME` placeholders)
  as the reference to copy from — `docs/RUNBOOK.md` §5 now documents the
  copy-and-fill step.
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

- **SQM WAN bandwidth — set 2026-07-24, see the v10/v11 entries below.**
  Superseded: this used to say it couldn't be set without a real link. WAN
  got connected this session and real numbers are live and shipped.
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

**Update, same night: the switch-side step is done too.** Ten parallel
research passes actually answered the switch-side risk question instead
of leaving it as "too different, hold off" — confirmed `BRCM_HDR` tag
mode is already active via mainline's own driver (didn't need touching),
confirmed real documented LAN-bricking history exists for this exact
switch chip (the caution was earned, not excessive), confirmed DSA tag
parsing safely drops malformed frames rather than crashing, and confirmed
a real power-cycle fully resets the switch independent of prior state.
With that in hand, `fa_switch_oobpause_test.c` replicated mainline
`b53_srab.c`'s exact bus-arbitration protocol and toggled the
`REG_FC_OOBPAUSE` bit live: write confirmed, revert confirmed, zero
system impact. All three FA/CTF hardware mechanisms (GMAC table-init,
GMAC indirect data path, switch-side enable) are now empirically proven
working. Full detail: `hwoffload-research/fa-probe/BRINGUP_RESULT.md`.

**Update, same night: real traffic verified, not just synthetic tests.**
The operator connected the R8000's WAN to a real upstream and a real
HTTP connection was routed through the router's actual NAT path.
`fa_accel.c`'s Phase B decode fired on the real connection and correctly
extracted both directions' pre-NAT and post-NAT tuples - exactly the
data a real NAPT row needs, from a real flow, not synthetic input. This
closes the last open verification gap noted in every earlier FA entry.
Everything reverted afterward (flow_offloading_hw back to 0, module
unloaded, temporary host route removed). The only remaining step to a
working feature is Phase C proper: having the driver actually build and
write the row instead of logging it - a real integration task now, not
a hardware unknown.

**Update, same night: Phase C written; live packet test attempted but not
completed.** `fa_accel.c` gained a real write path -
`fa_flow_replace_live()` builds and writes a real NAPT row from a real
`flow_rule`, reads it back, and only reports success if the readback
matches. Two of the previously one-shot register writes (`fa_bringup`,
switch OOBPAUSE) were re-built as leave-active-until-rmmod variants so a
real test window was possible, and a stats-counter probe
(`fa_stats_probe.c`, HIT/MISS/error/ecc_error) was staged as the
before/after oracle. Bring-up sequence executed and confirmed clean at
every step - but before the actual test packet could be sent, the
operator disconnected the WAN uplink, so there was no path left for it to
take. **The write path is built and matches every previously-proven real
data input, but has not yet been exercised against a real forwarded
packet.** Everything was fully reverted in order and confirmed
byte-identical to baseline: `fa_accel` unloaded, `flow_offloading_hw` back
to 0, OOBPAUSE back to 0x0000, control register back to 0x00001400.
Router stable throughout (uptime unbroken, 0% ping loss, all 4 SSIDs up).
Full detail: `hwoffload-research/fa-probe/BRINGUP_RESULT.md`. Real
end-to-end forwarding proof is the one thing this project has never yet
empirically shown - needs a fresh WAN-connected window, same staged
procedure, new go/no-go.

**Update, same night: live=1 test completed with WAN reconnected - honest
result, not a clean win.** Re-ran the full staged sequence and actually
fired the test packet this time. `fa_accel`'s write path worked completely:
both directions of a real connection were decoded, a real row built and
written, read back and verified before ever reporting success, and torn
down correctly on connection close - all against real data, no synthetic
inputs anywhere. But the FA hardware's own HIT counter never moved (stayed
at 0) across that entire test, despite the row being correctly written and
marked valid. Two honest explanations, neither confirmed: the connection
was too short (~38ms between write and teardown) for any packet to have a
real chance to traverse the row, or FA silicon was never actually wired
into the live packet datapath by anything built this session (only its own
config/table registers were touched - nothing tells switch/MAC hardware to
route packets through FA's lookup engine in the first place). Everything
reverted and confirmed clean: `fa_accel` unloaded, hw offload back to 0,
switch OOBPAUSE and GMAC control register back to exact baseline, zero FA
modules left loaded, router stable throughout (0% ping loss to router and
to the real internet, all 4 SSIDs up). Full detail:
`hwoffload-research/fa-probe/BRINGUP_RESULT.md`.

**Update, same night: root cause found - the FA/CTF investigation is now
closed.** Traced the actual per-packet consultation mechanism to its
source: `ctf_forward()` is called directly from Broadcom's own patched
vendor Ethernet driver (`et_linux.c`, confirmed the correct GMAC-generation
driver for this SoC), on every received packet, before it reaches Linux's
normal networking stack - that's the real hook FA/CTF depends on. This
router runs mainline `bgmac.c`, an entirely separate upstream driver with
zero CTF/FA awareness anywhere in it or in the b53/DSA switch driver
family (independently verified against current mainline source). The
vendor's own attach point (`ctf_attach_fn`) is an exported function
pointer that only Broadcom's closed-source module would ever populate -
nothing in mainline does. Public documentation (SNBForums/Merlin/DD-WRT)
independently confirms FA/CTF always requires that closed-source module
plus WAN-type constraints that don't apply here since nothing is attached.

This means every hardware mechanism this project verified this session -
table-init, indirect data read/write, switch-side enable, a complete real
NAPT row, real connection data decoding, and now a real write-verified row
for a live connection - was correct and real, and none of it was ever
going to be consulted by actual traffic, on this software stack, no matter
how long the test connection ran. That's not a bug in this project's
driver; it's an architectural fact about mainline OpenWrt vs. Broadcom's
closed vendor stack. Reaching real acceleration would mean porting
Broadcom's own CTF/FA kernel module into `bgmac.c` itself - a mainline
Ethernet driver project, not a flowtable-offload-backend project, and a
different undertaking than anything scoped here. Full detail:
`hwoffload-research/fa-probe/BRINGUP_RESULT.md` and
`hwoffload-research/VERDICT.md`.

## Correction, same night: v8's WPA3 claim was never actually true - found and fixed via real-client testing

Real-client WiFi testing (a real laptop associating over the air, not
synthetic traffic) found that **WPA3-SAE never actually broadcast on this
router, on any radio, since v8 first booted** - the earlier v8 entry above
describing OCV/WPA3 hardening was honest about being *unverified against
real clients*, and that caution turned out to be exactly right, for a
bigger reason than expected.

`brcmf_configure_wpaie: Invalid key mgmt info` fires in dmesg on every
radio at the very first hostapd startup of every boot (confirmed
independent of anything touched this session), and a live, sub-second-fresh
scan of the actual broadcast RSN element showed `Authentication suites: PSK
PSK/SHA-256` only - SAE silently absent, despite hostapd's own config
explicitly requesting it. This is a real BCM43602/brcmfmac firmware
limitation (confirmed via web research as a known driver bug class; a
documented module-parameter workaround for a similar bug on different
chips did not fix it here - reverted). 802.11r (FT) made the same driver
rejection worse but wasn't the root cause by itself.

**Fixed:** all 4 wireless interfaces (3 main radios + guest) switched from
`sae-mixed` to plain `psk2` (WPA2-PSK/CCMP), removing `ocv`/`ieee80211r`/
`ft_psk_generate_local`/`mobility_domain` (dead weight without a working
SAE/FT foundation) while keeping `ieee80211w` (MFP-optional - genuinely
works, confirmed via the same RSN scan), `ieee80211k`, and `bss_transition`
(both independent of SAE/FT). Verified clean after a full reboot: zero
driver errors (down from 12+/boot), config persists across reboot, all 4
SSIDs up, WAN/LAN unaffected. Full detail: `docs/FINDINGS.md` §14.

This router's WiFi has always been WPA2-PSK with MFP-capable advertised,
not WPA3 - this fix makes the shipped config honestly match what the
hardware actually does, rather than silently claim a posture it never
delivered.

## SQM WAN bandwidth - measurement method proven, still correctly NOT set

The "Outstanding" item above (no real WAN link to measure) is partially
resolved: WAN got a real connection tonight (double-NAT through the
operator's own home network, for the FA/CTF live-traffic test) and a real
download measurement is now proven feasible from the router itself -
`wget -O /dev/null "https://speed.cloudflare.com/__down?bytes=52428800"`
timed at 50MB/5.09s, ~82.4 Mbps. Upload measurement is not yet proven: the
router's minimal `wget` couldn't complete a POST-based upload test against
either Cloudflare's `__up` endpoint or a thinkbroadband mirror (connection
reset / server error respectively) - needs `iperf3` or a working upload
target, not attempted further tonight.

**Deliberately NOT setting `sqm.wan.download`/`upload` from this number.**
This bench connection is a temporary double-NAT through the operator's own
home network, not the router's actual final ISP link - the real deployment
bandwidth could be very different (faster or slower). Writing tonight's
82.4 Mbps into production SQM config would silently misconfigure the
shaper once deployed for real, which is worse than leaving it at the
current `0`/disabled placeholder. What tonight actually adds: the exact
command to run once the router is at its real deployment location, so
this is a two-minute task then instead of an open question.

## pstore/ramoops — real, rigorous investigation; genuine upstream wall found, not shipped

Built a full custom kernel (CONFIG_PSTORE/CONFIG_PSTORE_RAM + a devicetree
patch reserving 512KB for a ramoops region) via the full OpenWrt buildroot
(a bigger undertaking than anything else tonight - hours-scale, toolchain
+ kernel + all packages from source). Both changes work: `/sys/fs/pstore`
mounted successfully on real hardware, twice.

But brcmfmac fails to load against this locally-built kernel with a
`struct module` ABI mismatch - and this is not a mistake in the pstore
work. Isolated rigorously: reproduces with pstore fully reverted (not the
cause), reproduces even swapping in the exact byte-identical `brcmfmac.ko`
that works fine on v9 (not a module-build problem - the *kernel* itself
differs from what v9 actually runs, which was never locally compiled at
all, only ever built by the official OpenWrt project). Matches a real,
currently-open, unresolved upstream bug
([openwrt/openwrt#18743](https://github.com/openwrt/openwrt/issues/18743))
hitting unrelated modules on a different target, with no fix documented
anywhere.

Router reverted to v9 after every test - no regression shipped. Full
diagnostic trail in `docs/FINDINGS.md` §15, so this doesn't need
re-discovering next time someone wants a custom kernel build here.

**Follow-up: tested the build-order/race hypothesis directly rather than
assuming "unresolved upstream bug" was the final word.** Forced explicit
serialization - fully finished the kernel compile first (confirmed via
vmlinux/zImage present), only then built mac80211/brcmfmac, only then the
rest of world - instead of trusting make's own parallel scheduling.
Flashed, tested: identical error. This falsifies build ordering as the
cause and leaves the original conclusion more confirmed, having actually
survived a real test instead of resting on a matched GitHub issue alone.

## v10 — app-plus pass: build-recipe drift found and closed, reproducibility proven (2026-07-24)

A full app-plus pass (repomix + edgar-morin) against this already heavily
mined repo found no remaining hardware/feature work — FA/CTF, pstore/ramoops,
OWE, and DFS are all confirmed-closed honest walls, and everything else on
`hwoffload-research/`'s own bookkeeping index either already shipped or was
never followed up on and turned out to be moot. The one real, confirmed gap:
**this project's own build recipe had drifted from what it actually builds,
for the third time.**

- **`docs/RUNBOOK.md` §5 still documented `FILES=image-files/`**, three
  shipped versions (v7, v8, v9) after commit `1e2baf2` made `v2-files/` the
  real overlay. Following the runbook literally today would have silently
  shipped a build missing guest-network isolation, `perf-tune`,
  `radio-watchdog`'s rc.d enable symlink, and the LED netdev-binding fix —
  exactly the class of mistake that already caused the v5→v6 packaging
  regression and the v7 `wpad-basic-mbedtls` outage.
- **`v2-staging/wpa3/imagebuilder-packages.md`'s copy-pasteable `make image`
  command still said `wpad-basic-mbedtls`** even after two "SUPERSEDED"/
  "CORRECTION" headers had been stacked on top of it identifying that as
  wrong — the actual runnable command was never fixed. This is the literal
  doc that produced the v7 regression; it was still loaded.
- **`hwoffload-research/WAVE2_INDEX.md` had 4 research threads stuck at
  "pending consolidation" indefinitely**, unlike threads 1-2 which got an
  honest close-out. Closed 3 as moot/subsumed by later decisions already on
  record (UART superseded by the pstore/ramoops choice, BCM53012 errata
  subsumed by the already-closed EAP_MODE_SIMPLIFIED fix, SROM/NVRAM
  cross-reference moot since calibration already works end-to-end); left
  thread 6 (unused SoC blocks/crypto engine survey) honestly open rather
  than silently marking it done.

**Fix verified, not just written.** Reconstructed the actual correct
`PACKAGES=` list from the live v9 router's own `apk list --installed`
(ground truth), corrected `docs/RUNBOOK.md` to the real recipe (`FILES=v2-files`,
`-wpad-basic-mbedtls wpad-mbedtls`, the local patched-`kmod-brcmfmac` repo
requirement spelled out), then **built from the corrected recipe and diffed
the result against the live router**: package manifest is byte-for-byte
identical to v9's installed set (zero packages missing either direction),
and `brcmfmac.ko`'s md5 matches the live, patches/861-verified module
exactly (`8398326dd28491534f9e2ce35b71ee56`). `image-files/` marked
deprecated in place (`image-files/DEPRECATED.md`) rather than silently left
to mislead the next rebuild.

**Shipped as `images/openwrt-25.12.5-r8000plus-v10-bcm53xx-generic-netgear_r8000-squashfs.chk`**,
sha256 `f604c6b5c43dc2007e1c945d2758e8f37142ab54ef08b9ba0728a60f0e63b938`.
**Not flashed to the router.** Content is functionally identical to the
already-verified-live v9 (that's the point — it proves the fixed docs are
correct); reflashing a running router for a docs-only fix with zero
functional delta would be pure risk for no gain, given this project's own
history of every flash surfacing at least one surprise. The earlier
abandoned pstore-experiment `.chk` that had been occupying the `v10`
filename (never shipped, see the pstore/ramoops entry above) was moved to
`images/graveyard/` first so it wasn't silently overwritten.

## v11 — SQM bandwidth measured for real and shipped, flashed live (2026-07-24)

WAN got connected this session (previous "Outstanding" blocker). The
router's own minimal `wget` gave misleading numbers — 403'd by Cloudflare's
speed endpoint, and consistently slow (~1.5 Mbps, twice) against a congested
OVH mirror — neither reflected the real link. Fix: tunneled through the
router itself (`ssh -D` SOCKS proxy) so the transfer used the actual WAN
path end-to-end, then measured with the host's real `curl` instead of the
router's constrained wget. Clean, consistent results: **101.3 Mbps down**
(Linode Newark), **99.4 Mbps up** (Cloudflare) — in the same ballpark as
the earlier 82.4 Mbps double-NAT test, so trusted.

Set `sqm.wan.download=91000` / `upload=89000` kbit/s (90% headroom) live via
UCI first, verified `tc qdisc show` — cake actually shaping both directions,
not just configured — then folded the same numbers into `v2-files/etc/config/sqm`
as the shipped default so they're baked into the image, not dependent on
sysupgrade's config preservation (which this device has a confirmed history
of losing, #21655).

**Shipped and flashed:** `images/openwrt-25.12.5-r8000plus-v11-bcm53xx-generic-netgear_r8000-squashfs.chk`,
sha256 `7d1fdd678f109ee03cdf2fc03d12d3c480cbb363937138e464ab062232f819fd`.
Only content delta from v9/v10: the SQM numbers — package manifest and
`brcmfmac.ko` checksum both confirmed identical before flashing. Pre-flight
backup taken (`sysupgrade -b`, verified non-empty, contains
wireless/firewall/sqm) per the mandatory RUNBOOK procedure. Flashed via
`sysupgrade`, back up in 64s. **Verified live, not just flashed:**
fresh boot (uptime 0 min), config sizes healthy on every file
`#21655` has previously reset (no hit this time), all 4 SSIDs up
(R8000 ×3 + R8000-Guest), guest bridge up, `brcmfmac.ko` md5 still matches
the proven `patches/861` module, cake shaping live on `wan` at 89Mbit,
`radio-watchdog` enabled, 0% ping loss to the real internet.

## Doc audit + DFS re-test + package audit, no new image needed (2026-07-24)

Three follow-ups after v11, none of which changed what's actually shipped:

1. **10-agent documentation audit** (separate detailed commit) closed real
   drift across README.md, this file, CLAUDE.md, and a dozen research/staging
   docs — see git log for the full breakdown, not repeated here.
2. **DFS channels re-tested live**, prompted by the audit flagging `iw phy
   info` showing them as `(radar detection)` instead of `(disabled)`.
   Confirmed still a hard driver-level wall (`-52`, same as the clm_blob
   test) — the regdb label changed, reality didn't. Full account:
   `docs/FINDINGS.md` §16.
3. **Package upgrade audit** (`use-latest-version` scan): nothing in this
   repo is actually ours to bump. The only real dependency manifest it found
   was `hwoffload-research/ghidra-cli/` — a third-party RE tool (its own
   `.git`, gitignored, not this project's code, same category as `openwrt/`
   itself). OpenWrt 25.12.5 confirmed as the current point release; the
   `PACKAGES=` list carries no version pins, so every build already pulls
   whatever's current in the feed.

Rebuilt anyway to confirm reproducibility: the result is **byte-for-byte
identical** to v11 (same sha256, same manifest) — expected, since nothing
that landed in the image actually changed (the DFS test was fully reverted;
the docs-only commits don't touch `v2-files/`). Not shipped as a separate
version number — a byte-identical duplicate of v11 would just be clutter.
v11 remains the current running image.
