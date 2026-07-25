# R8000 OpenWRT — Technical Findings

## 1. Hardware (FCC-confirmed)

FCC ID **PY314200264** (covers R8000 + R7900, identical PCB).

- SoC: Broadcom **BCM4709A0**; OpenWrt target `bcm53xx/generic`.
- Radios: **3× BCM43602KMLG** daughter-cards, fanned out through a **PLX PEX8603**
  3-port PCIe switch. Front-ends: Skyworks SKY85309-11 (2.4G),
  SKY85710-11/SKY85712 (the two 5G).
- **All three radios are genuinely 3×3** (3TX/3RX — FCC filing + our nvram
  `txchain/rxchain=7`), *not* the 2×2 the 43602 is usually assumed to be.
  6 dipole antennas: chains 1–3 diplexed for 2.4G + 5G-Band1, chains 4–6 for
  5G-Band4.
- RAM 256 MB, flash 128 MB. CPU has no VFP (Go: build `GOARM=5`).

### FCC-certified RF ceilings (reference, NOT our cap — see §6)
| Band | Conducted | Array gain | EIRP |
|---|---|---|---|
| 2.4 GHz | 29.51 dBm | 6.33 dBi | 35.84 dBm |
| 5 GHz UNII-1 (36–48) | 25.99 dBm | 7.82 dBi | 33.81 dBm |
| 5 GHz UNII-3 (149–165) | 26.27 dBm (VHT40) | 6.97 dBi | 33.24 dBm |

- Grant covers **UNII-1 + UNII-3 only — no DFS/UNII-2** (matches stock behavior).
- **VHT80 on the upper band forces a big back-off** (~20.4–20.8 dBm conducted)
  vs VHT40 — a 2015 U-NII spectral-mask rule change, same hardware. Do not carry
  a 20/40 MHz power setting onto 80 MHz.
- **Key: the nvram `maxp*` PA calibration sits 3–5 dB ABOVE the certified level
  in every band.** The regulatory/txpower *tables* cap output, not the silicon —
  so there is real, documented headroom to claim.

## 2. Calibration extraction (methodology)

The per-device RF calibration only exists on the live unit + its stock firmware,
so it was extracted **before** flashing:

1. Enabled the Nighthawk telnet backdoor (magic UDP packet, `admin`/`password`,
   via `bkerler/netgear_telnet`) → root BusyBox shell.
2. `nvram show` → full stock nvram (2251 lines). Secrets (`http_passwd`,
   `wl0_key`, DDNS/SSO creds) scrubbed; clean calibration kept in
   `extracted/calibration.txt` (297 lines). Full dump gitignored.

Extracted: three radios in Broadcom `0:`/`1:`/`2:` nvram format, `sromrev=11`,
`boardtype=0x0665`; full `pa5ga`/`pa2ga` PA calibration; `maxp5ga`/`maxp2ga`
power tables; per-radio `macaddr`/`ccode`/`regrev`/`boardflags`; devpath/devid
binding vars. 137 calibration-bearing keys.

Flash layout (`extracted/proc-mtd.txt`): `mtd1 nvram`, `mtd4 board_data`,
`mtd2 linux`, `mtd3 rootfs`. The nvram partition is **preserved through a flash**,
so the calibration survives into OpenWrt.

## 3. Root cause of OpenWrt #20514 (dead 5GHz) + the fix

Community consensus (#20514): the 5GHz calibration is "embedded in the driver
binary," treated as unrecoverable. **The actual root cause is simpler:** the
bcm53xx nvram init (`package/utils/nvram/files/nvram-bcm53xx.init`,
`set_bcm43602_variables`) special-cases the near-identical **ASUS RT-AC3200**
triple-radio 43602 board to set the `devpath`/`devid`/`sromrev`/`boardflags`
that bind brcmfmac to the radios — **but never the R8000**.

Fix: add a `netgear,r8000` case using this unit's own extracted values
(`devid 0x43bc`/`0x43bb`, `sromrev 11`, `boardrev 0x1421`, real per-radio
`boardflags`, devpaths). Shipped two ways:
- `patches/0001-nvram-bcm53xx-add-netgear-r8000-43602.patch` (upstreamable)
- `image-files/etc/init.d/nvram` (ImageBuilder FILES override at v1; this
  overlay is deprecated since v7 — the same fix now lives in
  `v2-files/etc/init.d/nvram`, the canonical overlay, see RUNBOOK.md §5)

Also added the `netgear,r8000` case to `set_wireless_led_behaviour`
(`0/1/2:ledbh10=0x7`) — see LEDs workstream.

## 4. v1 results (VERIFIED on-device)

OpenWrt 25.12.5 r33051, kernel 6.12.94. SSH up ~102s after GUI flash.
- **All 3 radios up**: phy0 (`…f1:38`, 5GHz, 29 ch), phy1 (`…f1:37`, 2.4GHz,
  14 ch), phy2 (`…f1:36`, 5GHz, 29 ch). Per-radio MACs match the extracted
  calibration exactly.
- **5GHz functional**: live scan on a 5GHz radio saw 5 APs; VHT/802.11ac + AP
  mode advertised. Our nvram init case is present and active; 287 `N:` cal keys
  in nvram.
- **Honest v1 gaps** (→ v2): `clm_blob`/`txcap_blob` still `err -2` (regdom
  `country 00`, TX not fully calibrated); LuCI not in the lean build;
  attribution vs stock nvram not yet A/B-controlled.

## 5. Attribution caveat (honest)

The flash preserves the stock nvram partition, so 5GHz *might* also come up on a
plain OpenWrt build. Our binding init is active and correctly bound (proven by
per-radio MACs), but a controlled A/B (flash stock 25.12.5, compare) has not been
run to isolate the fix's contribution. Deliverable — a working tri-band OpenWrt
R8000 with functional 5GHz — is real regardless.

## 6. v2 — full de-neuter + modern stack (IN BUILD)

Direction: **do not cap at the FCC-certified numbers.** Strip the regulatory
tables entirely and run at the **PA silicon ceiling** (the `maxp*` calibration,
3–5 dB above certified), **all channels including DFS/UNII-2**. The only limit is
the amplifier itself. This is owned-hardware tuning; the operator owns regulatory
responsibility, thermal/stability at max power is a real trade-off, and DFS
channels carry radar rules — stated, not enforced.

Workstreams (artifacts in `v2-staging/`):
- **LEDs** — `ledbh10` init + front-panel LEDs traced to SoC ChipCommon GPIOs
  (`board.d/01_leds`).
- **WPA3** — SAE + 802.11w MFP, correct `wpad` variant, working wireless config.
- **firmware + clm** — newest upstream `brcmfmac43602-pcie.bin` (v1 shipped the
  2015 blob) + regulatory `clm_blob` extracted from the saved stock `.chk`.
- **max-radio** — fully de-neutered regdb, PA-ceiling txpower, all-band unlock.
- **modern-wifi** — dawn band-steering, 802.11r/k/v, OWE, SQM/cake bufferbloat
  (only what brcmfmac AP mode genuinely honors).
- **extras** — flow-offload, radio-hang watchdog, channel defaults.

### v2 shipped result (as `r8000plus-v2b`, verified on-device)

Two v2 regressions were caught by on-device testing and fixed — the reason we
test rather than trust:

1. **clm_blob was firmware-rejected and FATAL.** The extracted blob loaded but
   the 43602 firmware rejected it (`clmload failed -52`), and unlike a *missing*
   clm (v1, harmless warning) a *rejected* clm aborts brcmfmac init → **all three
   radios failed to register.** Removed it. Radios work at the v1 29-channel
   baseline. The clm/DFS-unlock path did not pan out; honest outcome.
2. **802.11v needed the full wpad, not a workaround.** `bss_transition` is
   unsupported by `wpad-basic-mbedtls` (`CONFIG_WNM` off) → hostapd rejected the
   whole config → 5GHz fell back to ch36/20 MHz. Fix: swap to full
   **`wpad-mbedtls`** (`CONFIG_WNM=y`), keeping the feature. Verified `wpad` has
   `bss_transition`/`wnm_sleep_mode` before flashing.
   **Regressed and re-confirmed on v7 (2026-07-23):** a later staging doc
   (`v2-staging/wpa3/imagebuilder-packages.md`) argued for keeping
   `wpad-basic-mbedtls` (correct for WPA3-SAE/MFP alone, but doesn't mention
   `bss_transition`) and the v7 build followed that PACKAGES list — silently
   reintroducing this exact outage (all 4 SSIDs down, "unknown configuration
   item 'bss_transition'", `hostapd.add_iface` failed for every phy). Re-fixed
   the same way: `PACKAGES="-wpad-basic-mbedtls wpad-mbedtls ..."`. Both
   packages provide the same `hostapd`/`wpa-supplicant` and conflict if listed
   together — the `-wpad-basic-mbedtls` exclusion is required since the device
   profile pulls it in by default.

**Verified working on v2b:**
- OpenWrt 25.12.5, 3 radios, unified `R8000` SSID on all three.
- **WPA3-SAE + 802.11r (FT-SAE) + 802.11k (RRM) + 802.11v (BSS-Transition)** —
  `Encryption: SAE / FT-SAE / WPA-PSK / FT-PSK (CCMP)`, `bss_transition=1` in all
  hostapd confs, **0 hostapd config errors**.
  **Correction (§14, 2026-07-23): this WPA3-SAE claim was config-valid but
  never actually true over the air** — real-client testing later proved SAE
  was silently dropped from the broadcast RSN on every boot. 802.11r/FT were
  removed along with it; only 802.11k/v and `psk2`+MFP-optional actually work
  on this hardware. Read §14 before relying on anything in this bullet.
- **5GHz split at VHT80**: phy0 upper (ch149/153, 5.765 GHz), phy2 lower (ch36,
  5.180 GHz); phy1 = 2.4 GHz ch1.
- **usteer** band-steering up, **flow-offload** on, **SQM/cake** installed (idle
  until WAN bandwidth set), **LEDs** all defined, **LuCI** on :80.
- Max power: config requests the PA-ceiling txpower (27/27/23), firmware clamps
  to the true calibrated max. brcmfmac does not report applied txpower via `iw`;
  the ceiling is the config target, the firmware is the enforcer.

**Honest not-delivered:** DFS-channel unlock (clm rejected); 802.11s mesh +
airtime-fairness (brcmfmac driver ceiling, not a package choice); regulatory
"table de-neuter" (channels are DT `ieee80211-freq-limit`-gated, not
table-gated — proven empirically). Temp WiFi passphrase `ChangeMe-R8000-2026`
must be changed. **(Resolved v6.1, 2026-07-23 — rotated to random 20-char
passphrases on-device and in `v2-files/etc/config/wireless`; see WINS.md.)**

## 7. Engineering lessons

- Test on the device: both v2 regressions (fatal clm, 802.11v fallback) were
  invisible until flashed.
- Enable, don't strip: an unsupported hostapd option means the wrong daemon
  variant, not a feature to delete.
- On this hardware, "unlock" ≠ "edit the regulatory table" — the real gates are
  the DTS freq-limits (channels, hardware-tied to antenna diplexing) and the PA
  calibration (power). Regdb is inert here (proven).
- A `-95`/EOPNOTSUPP is a *driver* return, not always a firmware wall — read the
  driver. The multi-BSS limit was a driver bug, not a chip limit (see §8).

## 8. Multi-BSS driver fix (the "impossible" one — DONE)

The 2nd AP BSS per radio failed with `add_iface -95`, so OWE-transition, guest
SSIDs, and multi-SSID-per-radio were all "impossible" on brcmfmac/BCM43602.

**Root cause (read the driver, not the docs):** `brcmf_cfg80211_request_ap_if()`
tries the modern `interface_create` iovar (v1/v2, then a version query). The
R8000's 2015 firmware (7.35.177.56) doesn't implement it, so the version query
fails and the driver did `return -EOPNOTSUPP` — **skipping the legacy
`bsscfg:ssid` MBSS fallback right below it that this firmware DOES support.**
MBSS was even detected (driver advertised `#{AP}<=4`); the code just bailed
before trying the path that works.

**Fix** (`patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch`, 2 lines): on
the version-query failure, set `iface_create_ver = 0` and fall through to the
legacy MBSS path instead of returning. Built as `kmod-brcmfmac` via the 25.12.5
bcm53xx **SDK** (ABI/vermagic matched to the running kernel), deployed live.

**Verified on hardware:** `iw phy phy0 interface add … type __ap` → exit 0 (was
-95); dmesg shows the fallthrough to legacy `bsscfg:ssid`. Re-enabled OWE +
added a guest SSID → **6 BSSes beaconing** (R8000 ×3, R8000-Open,
R8000-Open-OWE, R8000-Guest). Upstreamable to OpenWrt mac80211 / linux brcmfmac.

Caveat: the patched module is currently deployed as a `/lib/modules` overlay
(persists across reboot). A clean reproducible image (v3) folding the patched
`kmod-brcmfmac.apk` into ImageBuilder is the remaining packaging step.

## 9. OWE (Enhanced Open) — confirmed unfixable, the honest wall

With patches/861 unlocking multi-BSS, OWE-transition and guest SSIDs both
became reachable as *interfaces* — but OWE specifically never beacons.
Isolation testing ruled out multi-BSS-cap and transition-mode as the cause:

* Plain multi-SSID (3 BSSes/radio): works.
* Plain guest (2nd BSS): works.
* OWE transition pair (`owe_open`+`owe_secure`): both fail, hostapd exits 0
  but no beacon.
* Standalone plain OWE (no transition pair): also fails, identical error.

Device log: `ieee80211 phy0: brcmf_cfg80211_start_ap: brcmf_parse_configure_security error`.

Read the actual driver (`drivers/net/wireless/broadcom/brcm80211/brcmfmac/cfg80211.c`,
kernel 6.12.y, function `brcmf_parse_configure_security()`): it parses the
RSN IE's AKM-suite list and switches on suite number — `RSN_AKM_NONE(0)`,
`RSN_AKM_UNSPECIFIED(1)`, `RSN_AKM_PSK(2)`, `RSN_AKM_SHA256_1X(5)`,
`RSN_AKM_SHA256_PSK(6)`, `RSN_AKM_SAE(8)` are all handled — **there is no
case for OWE (AKM suite 18)**. It falls through to `default:` (`"Invalid key
mgmt info"`), `wpa_auth` is never set to anything OWE-related, and the
firmware rejects the resulting beacon config. `WPA3_AUTH_OWE` does not exist
anywhere in brcmfmac; this was never implemented, on any chip, in-tree.

**Why this isn't a driver bug like the MBSS one:** the R8000's BCM43602
firmware is dated **2015-09-18** (`7.35.177.56`, confirmed via extracted
calibration and firmware version strings in kernel logs). **OWE (RFC 8110)
was published in 2016** — a full year later. The firmware predates the
standard it would need to speak; there is no missing case to add, no
fallback path to unblock. Confirmed unfixable — not effort-gapped, not
"2015 dogma," a genuine chronology wall. OWE was removed from the shipped
wireless config (`v2-files/etc/config/wireless`); the driver-level multi-BSS
fix it rode in on is otherwise fully intact (guest network, unified-SSID
roaming all unaffected).

## 10. app-plus pass — scoped to bcm53xx + brcmfmac (2026-07-23)

Ran the graveyard-mining methodology against `openwrt/openwrt` scoped to
`target/linux/bcm53xx` + brcmfmac (not the full monorepo — thousands of
issues/PRs across every other target would be noise here).

**Shipped (v5):**
- **radio-watchdog** — an escalating brcmfmac-wedge recovery daemon
  (tier 1: `wifi reload`; tier 2: full module reload; tier 3: rate-limited
  reboot, 6h cooldown) was fully written earlier this session
  (`v2-staging/extras/radio-watchdog/`) but never wired into `image-files/`,
  so it shipped in none of v1–v4. Confirmed against **still-open** upstream
  issue [openwrt/openwrt#14685](https://github.com/openwrt/openwrt/issues/14685)
  ("brcmfmac makes CPU stalls" on R8000, PSM-watchdog/msgbuf-timeout
  signatures matching this daemon's detection patterns exactly, 2+ years
  open, Broadcom's own brcmfmac maintainer looped in with no resolution).
  Wired into `image-files/etc/init.d/radio-watchdog`,
  `image-files/usr/sbin/radio-watchdog-check`, enabled via
  `image-files/etc/rc.d/S99radio-watchdog`. Its cron dependency is provided
  by busybox itself (`/etc/init.d/cron`) — no extra package needed; the
  daemon's own `start()` enables+starts it.

**Documented, not fixed (out of scope):**
- **sysupgrade config-loss** — [openwrt/openwrt#21655](https://github.com/openwrt/openwrt/issues/21655),
  open, confirmed by a commenter to affect R8000 directly ("same problem for
  Netgear R7000 R8000 and Asus RT-AC68U"), but reproduced across totally
  unrelated targets (ath79, gl-inet, DIR-890L, Luxul, Phicomm K3) — this is a
  cross-target base-files/fstools regression, not a bcm53xx/brcmfmac bug, and
  genuinely out of this pass's scope to root-cause. Mitigation: an explicit
  off-device config backup is taken before every sysupgrade from here on (see
  RUNBOOK.md) rather than trusting sysupgrade's own preservation.

**Considered, not adopted:**
- PR [#11534](https://github.com/openwrt/openwrt/pull/11534) (`base-files:
  fix bcm53xx sysupgrade`, open since Dec 2022) — targets a different device
  (Phicomm K3) and a different symptom (upgrade fails to flash at all, not
  config loss after a successful flash); stale, zero maintainer engagement,
  not confirmed applicable to R8000.
- PR [#23463](https://github.com/openwrt/openwrt/pull/23463) (`bcm53xx:
  enable GRO`) — live on-hardware iperf3 data in the PR thread shows a
  **disputed, mixed result** on the same `bgmac` ethernet driver family: TX
  throughput regressed 690→550 Mbit/s with fraglist GRO enabled, root-caused
  in-thread to missing RX checksum offload on `bgmac`, still being debugged
  by reviewers, unmerged for a real reason. Adopting an unresolved PR with a
  measured regression on our own ethernet driver would be exactly the
  un-vetted "ship because it sounds good" mistake this methodology exists to
  prevent.
- PR [#21654](https://github.com/openwrt/openwrt/pull/21654) (`bcm53xx: drop
  vendor wl*_ runtime NVRAM before brcmfmac attach`) — closed/wontfix; a
  maintainer explicitly rejected cleaning up carried-over vendor NVRAM,
  preferring a proper CFE NVRAM reset. This **validates** our own approach
  (patches/0001 provides curated, explicit calibration via
  `nvram-bcm53xx.init` rather than carrying raw vendor NVRAM forward) — no
  action needed.
- Dedup check: zero existing OpenWrt issues/PRs discuss the
  `interface_create`/EOPNOTSUPP MBSS-fallback bug fixed in patches/861 — a
  genuinely novel, unclaimed contribution, worth upstreaming as-is.

## 11. DFS/clm_blob wall — root cause found, real trade-off discovered (not re-shipped)

Re-investigated the v2 "clm_blob firmware-rejected, fatal" finding under
direct challenge: was `-52` genuinely a policy rejection, or a mistake in
our own extraction? Traced the actual meaning of `-52`
([community precedent](https://github.com/RPi-Distro/firmware-nonfree/issues/16))
— it is consistently a **firmware/CLM version-pairing mismatch**, not a
categorical refusal.

Checked the extracted `brcmfmac43602-pcie.clm_blob`'s own trailing
compatibility stamp against our *actually loaded* firmware:

| | Version | Date | FWID |
|---|---|---|---|
| our loaded AP firmware | 7.35.177.56 | 2015-09-18 | `01-6cb8e269` |
| our v2 clm_blob's stamp | 7.10.274.3.REBASE.R493518 | 2021-06-02 | `01-a20087dc` |

**Confirmed mismatch** — the v2 clm_blob came from `dhd.ko`'s embedded
`dlarray_43602a1` array in a *newer* stock firmware package
(`R8000-V1.0.4.88`), paired internally with a 2021 firmware build, then
mixed with our *separately-sourced* 2015 linux-firmware `.ap.bin`. Two
firmware generations of the same chip, forced together — genuinely our own
extraction mistake, not a firmware policy wall.

**Fix attempted, live-tested (2026-07-23):** re-carved the *whole*
`dlarray_43602a1` array (firmware portion + CLM as one matched, self-paired
unit — confirmed byte-identical CLM to the earlier extraction, sha256
`39d018bc...`) and deployed both together via a reversible live firmware-file
swap + `rmmod`/`modprobe brcmfmac` (not baked into an image — this was a
direct hardware test, backed up first).

**Result — genuinely mixed, not a clean win:**
- ✅ **No `clmload -52` abort.** All 3 radios registered and beaconed. The
  version-mismatch hypothesis is confirmed correct — a matched firmware+CLM
  pair loads cleanly.
- ❌ **DFS channels (52–64, 100–144) stayed `disabled`** in `iw phy phy0
  channels` even with clean CLM load. Consistent with the earlier §6 finding
  that channel availability here is gated by the devicetree
  `ieee80211-freq-limit` (hardware antenna diplexing), not by CLM/regdb —
  fixing the CLM mismatch didn't touch that separate gate.
- ❌ **Regression: MBSS broke.** With the 2021 firmware, `interface_create`
  version-query now fails with `-52` (was `EOPNOTSUPP` on the 2015 build) and
  the guest-network 2nd BSS on radio1 failed with `err=-95` — patches/861's
  fallback trigger (originally written for `EOPNOTSUPP`) doesn't catch this
  firmware's different failure code the same way. Guest network dropped from
  4 beaconing interfaces to 3.
- **Reverted to the known-good 2015 firmware + no clm_blob** (sha256
  `7f735b72...`, the state already proven and shipped in v1–v5) and
  **rebooted** — a driver `rmmod`/`modprobe` cycle alone was *not* sufficient
  to fully reset radio-chip state after a firmware/CLM download; only a full
  power-cycle restored clean MBSS behavior.

## 12. OWE — 10-agent static disassembly of the actual firmware binary (2026-07-23)

Under direct challenge to prove the OWE wall with real evidence rather than
driver-source inference, ran a 10-agent parallel static analysis pass
(objdump/capstone, Thumb-2 disassembly, no vendor SDK, no symbols) directly
on `brcmfmac43602-pcie.ap.bin` (595,472 bytes, the exact file live on the
router, byte-for-byte SHA256-verified against the deployed copy). Full
output in `v2-staging/firmware/re-analysis/`.

**Structural findings, independently corroborated 3 separate ways:**
1. **This is a "roml" (ROM + RAM-overlay) build** — confirmed by (a) the
   literal build-tag string `43602a1-roml/...` at EOF, (b) exhaustive string
   search finding **zero** occurrences of any core security iovar name the
   Linux driver actually sends (`wsec`, `auth`, `wpa_auth`, `sae`, `chanspec`,
   `clmload`, `clmver`, `cur_etheraddr`, `down`, `bcn_prd`, `dtim_prd` — 12/19
   targeted names, 0 hits, cross-verified by an independent full-file entropy
   scan that found no other hidden low-entropy pocket), and (c) real
   disassembly showing 5 of the firmware's 10 hottest `bl` call targets
   (up to 445 calls each) resolve to addresses like `0xffe83644` —
   **completely outside this file's own address range**, i.e. genuine calls
   into a separate, fixed-address on-die mask-ROM region not present in this
   or any loadable file. Mask ROM is fixed into the silicon at fabrication;
   it cannot be read, dumped, or modified by any means available to us.
2. **Load base confirmed:** `0x180000`, both from a literal header field
   (`0x00180000` at file offset 0x7c) and cross-validated 5/5 against real
   branch targets resolving to clean code — high confidence.
3. **The AKM-suite dispatch logic that DOES exist in this RAM overlay was
   located and fully characterized — and it has no room for OWE.** One
   genuine multi-AKM enumeration routine exists (offset `0x01ad7e`–`0x01aecc`):
   it sequentially tests `WPA2_AUTH_UNSPECIFIED(0x40)`,
   `WPA2_AUTH_1X_SHA256(0x1000)`, `WPA2_AUTH_PSK(0x80)`,
   `WPA2_AUTH_PSK_SHA256(0x8000)`, and one undocumented bit (`0x2000`) — each
   appends a suite entry to a list — then **returns cleanly, before ever
   testing for SAE (`0x40000`).** SAE is checked entirely separately, via 9
   scattered standalone `if (wpa_auth & 0x40000)` binary flag tests elsewhere
   in the association-handling code, each with its own isolated branch — not
   part of any table or chain. Scanned 500 bytes after all 9 SAE-check sites:
   **zero instances** of any further AKM-bitmask test anywhere nearby. There
   is no structural "next case" slot, no dead code suggesting an unfinished
   switch, nothing to extend — the routine that enumerates AKMs stops one
   case short of SAE and returns; SAE itself is a dead-end flag test with
   nothing chained after it.

**Verdict: unchanged, now on much stronger evidence.** This isn't "we didn't
find OWE support" (absence from a component-scale search) — it's "we
mapped the actual AKM-dispatch code paths that exist in the only
firmware file we have access to, confirmed none of them have any
extensibility point beyond what's already active (PSK/1X/SAE), and confirmed
the remaining ~30% of this firmware's hottest logic calls out to a
physically separate, fixed, unreadable, unmodifiable ROM region." Even a
skilled disassembler with unlimited time hits a hard boundary here that
open-source-driver patching (patches/0001, patches/861) never had: there is
no source, and a meaningful fraction of the relevant logic isn't even present
in any file that exists outside the chip's silicon.

**Standing conclusion:** DFS-channel unlock is *not* re-classified as
achievable — it independently re-failed even with the CLM mismatch fixed,
confirming §6's DTS-gated conclusion rather than overturning it. But the
*root cause* of why v2's clm_blob attempt was fatal is now understood
precisely (version pairing, not policy), and a real avenue exists for a
*future, non-live, test-environment* pass: extend patches/861 to also
recognize `-52` (not just `EOPNOTSUPP`) as an `interface_create`-unsupported
signal before attempting this firmware pairing again — that would need to be
proven on a bench/spare unit, not the operator's live router.

## 13. Closed watch-items and standing decisions (2026-07-23)

**EAP_MODE_SIMPLIFIED (openwrt/openwrt#21349) — closed, no action needed.**
Full trace already in `v2-staging/extras/dsa-switch/NOTES.md`: upstream commit
`4227ea91e265` (backported to `6.12.30`) broke standalone/non-bridged switch
ports on Northstar (BCM5301x) chips by setting `EAP_MODE_SIMPLIFIED`. This
repo's 25.12.5 source tree already carries the real upstream fix
(`target/linux/bcm53xx/patches-6.12/701-net-dsa-b53-disable-EAP-setup-on-Northstar-switches.patch`,
skips `EAP_MODE_SIMPLIFIED` on `is5301x()` chips) — confirmed present and
applied, `wan` (a standalone port) passes traffic cleanly on the live device.
Closing this watch-item rather than leaving it open: don't re-fix it, and if
a future kernel bump drops/renumbers the patch, that's the thing to catch.

**`sar2g`/`sar5g` NVRAM — decision: leave unset.** These SAR (Specific
Absorption Rate) power-back-off NVRAM keys exist as strings the stock
firmware checks for (`v2-staging/firmware/re-analysis/strings_with_offsets.txt`)
but are **absent from this unit's own extracted stock NVRAM**
(`extracted/calibration.txt`) — the device shipped without them set. SAR
tables exist to cap RF power for body-worn/handheld proximity-to-body FCC
exposure limits; a stationary shelf-mounted router has no such use case, and
setting them would only add an unnecessary power cap contradicting the
already-shipped PA-ceiling policy (§6: "the operator owns regulatory
responsibility"). Consistent decision: don't add them — matches stock
behavior, matches the project's existing power posture.

**UART vs pstore/ramoops for post-crash debug visibility — decision: defer,
prefer pstore/ramoops when built.** UART requires physically locating and
soldering to an on-board header (hardware risk, bench time) for one-shot
serial-console access. `pstore`/`ramoops` (a reserved DRAM region that
survives a warm reboot so the previous boot's `dmesg` can be read back from
`/sys/fs/pstore/` after a crash/watchdog reset) is software-only — no
soldering, and it complements `radio-watchdog` (§10) by giving it (or the
operator) forensic visibility into *why* a wedge happened, not just that one
did. Not wired into this build: adding a `ramoops` reservation requires a DTS
change (a reserved-memory node) and a rebuild+reflash cycle, which is a real
change of the SoC's memory map, not a config tweak — appropriately scoped as
its own dedicated pass with its own build-and-verify cycle, not bundled into
this one. Recorded here as the decided direction for that future pass, not
as something silently completed now.

**"Stuck regulatory domain" (self-managed wiphy at `99: DFS-UNSET`) — root
cause found: cosmetic display artifact, not a functional bug.** `iw reg get`
prints `country 99: DFS-UNSET` under each `phy#N` header even on a clean cold
boot with `country 'US'` correctly set in `/etc/config/wireless` and
`global\n country US: DFS-FCC` correctly shown at the top of the same command's
output. This looked like the regulatory domain failing to apply — it isn't.
Cross-checked against what's *actually* applied to the radios (not just the
summary label): `iw dev phy0-ap0 info` / `phy2-ap0 info` show real operation
at **31 dBm firmware-clamped power** (matches the PA-ceiling policy, §6) on
non-DFS UNII-1/UNII-3 channels (36, 153), and `iw phy phy0 channels` lists
the full **34-channel US table** — not the degraded flat-20 dBm/4-entry table
a genuinely-unapplied regdomain would show. brcmfmac's self-managed wiphys
pull their country/power table from NVRAM `ccode` at firmware attach time,
not from cfg80211's generic `REG_SET_DRIVER` path that the `iw reg get`
per-phy summary line reflects — so that summary line stays at the
world/unset placeholder cosmetically while the firmware-side table (the one
that actually gates channels and power) is correctly `US`. Verified by
directly reproducing the confusing state (a `wifi`/`network` hot-reload
during this session's guest-network change did visibly degrade real
operation to 20 dBm/no-DFS — a real, distinct issue, already covered by the
existing "reboot after any hot-reload with country set" rule) and then
confirming a genuine cold boot restores full real-world power/channels while
the `iw reg get` label remains unchanged either way. No fix needed or
possible here — it's not broken, `iw reg get`'s per-phy summary just isn't
the right place to look for this driver.

## 14. WPA3-SAE never actually worked on this hardware — found via real-client testing, fixed (2026-07-23)

**The v8 wireless config's `sae-mixed` encryption never actually broadcast
WPA3-SAE over the air, on any radio, since the image first booted.**
Discovered via real-client association testing (a real laptop WiFi client,
not synthetic traffic) — the same rigor this project has applied to every
other claim in this repo.

**Evidence chain, in the order it was found:**

1. A real client repeatedly completed 802.11 association to `phy0-ap0`
   (channel 153, 5GHz) then was immediately disassociated, in rapid cycles
   (7 cycles in ~10 seconds). This matched the interop-risk warning already
   present in this repo's own config comment for OCV — but turned out to be
   a different, larger problem.
2. `dmesg` showed `ieee80211 phy0: brcmf_configure_wpaie: Invalid key mgmt
   info` — a real kernel-level driver rejection, firing on **every radio, at
   the very first hostapd startup of the session's very first boot**
   (timestamps 44–48s into boot), independent of anything touched during
   testing. This ruled out "something I broke while testing" — it's a
   pre-existing condition of the shipped `sae-mixed` config itself.
3. A live, sub-second-fresh `iw scan dump` of the actual broadcast RSN
   information element showed `Authentication suites: PSK PSK/SHA-256` —
   **SAE is completely absent from the real over-the-air beacon**, despite
   `hostapd-phyN.conf` (the rendered config actually in use) explicitly
   listing `wpa_key_mgmt=SAE WPA-PSK WPA-PSK-SHA256`. The driver silently
   drops SAE from what it actually transmits rather than erroring loudly.
4. Web research confirmed this is a known, documented brcmfmac bug class:
   AP-mode SAE depends on firmware SAE-offload support, and there's a
   community-documented kernel module workaround
   (`brcmfmac.feature_disable=0x82000`) for a similar STA-mode bug on a
   *different* chip family (Raspberry Pi's Cypress/Infineon chips). Applied
   it here via `/etc/modprobe.d/brcmfmac.conf` + full module reload — **it
   did not fix this chip's issue** (BCM43602 firmware, not Cypress); RSN
   broadcast still showed no SAE afterward. Reverted the module override
   since it didn't help.
5. 802.11r (FT) made the driver-level rejection worse but was not the root
   cause by itself: the 5-AKM list (`SAE FT-SAE WPA-PSK WPA-PSK-SHA256
   FT-PSK`, all 3 main radios had `ieee80211r=1`) triggered the same
   `Invalid key mgmt info` error even more severely, and removing FT alone
   (keeping SAE) did not restore SAE to the broadcast RSN either — SAE
   itself is what this firmware can't do, with or without FT.

**Fix: switched all 4 wireless interfaces (3 main radios + guest) from
`encryption 'sae-mixed'` to `encryption 'psk2'`** (plain WPA2-PSK/CCMP),
removing `ocv`, `ieee80211r`, `ft_psk_generate_local`, `mobility_domain`
(main radios only — none of these can do anything without a working SAE/FT
foundation). Kept `ieee80211w` (MFP-optional — genuinely supported,
confirmed via the same RSN scan showing `MFP-capable`), `ieee80211k`
(802.11k RRM) and `bss_transition` (802.11v), since both are independent of
SAE/FT and unaffected by this bug.

**Verified after a full clean reboot** (not just a live reload, to rule out
session-state artifacts): `wpa_key_mgmt=WPA-PSK WPA-PSK-SHA256` on every
radio, **zero** `Invalid key mgmt info` errors this boot (down from 12+ per
prior boot), config change persists across reboot (confirmed via the
overlay), WAN and LAN connectivity unaffected, all 4 SSIDs enabled.

**Also found and fixed as a byproduct of this investigation:** a periodic
`hostapd: Failed to set beacon parameters` error recurring every ~6 seconds
appeared during the live debugging session itself — traced to being an
artifact of the extensive live `uci`/module-reload churn used to
investigate the SAE issue, not a real condition of the shipped image: it
did not reproduce on the clean reboot used for final verification.

**Honest scope of what this changes:** this router's WiFi has never
actually been WPA3 despite `docs/WINS.md`'s v8 entry describing it that
way — it has always been WPA2-PSK with MFP-capable (optional) advertised,
silently. This fix makes the shipped config match reality rather than
claim a security posture that was never actually delivered. OCV, being
entirely dependent on a working SAE/MFP-required negotiation that never
happened, was never actually providing protection either — its removal
here is not a regression, it's removing a config option that was already
inert.

## 15. pstore/ramoops — kernel/DTS work proven correct, blocked by an unresolved OpenWrt build-environment bug (2026-07-23)

**What was built, and confirmed correct in isolation:**
- `openwrt/target/linux/bcm53xx/config-6.12`: added `CONFIG_PSTORE`,
  `CONFIG_PSTORE_RAM`, `CONFIG_PSTORE_CONSOLE`, `CONFIG_PSTORE_PMSG`,
  `CONFIG_PSTORE_ZONE`.
- `openwrt/target/linux/bcm53xx/patches-6.12/334-ARM-dts-bcm53xx-netgear-r8000-add-ramoops.patch`:
  reserves 512KB at `0x8ff80000` (the top of the second RAM bank — confirmed
  free via grep across every parent `.dtsi`) for a ramoops region: 128KB
  dmesg record, 128KB console log, 64KB pmsg.
- **Both confirmed working on real hardware, twice**: `/sys/fs/pstore`
  mounted successfully (`pstore on /sys/fs/pstore type pstore ...`) after
  flashing a locally-built image with these changes.

**What's blocked, and why — a real, isolated, unresolved upstream bug, not
a mistake in these changes:**

brcmfmac fails to load with `module brcmfmac: .gnu.linkonce.this_module
section size must match the kernel's built struct module size at run
time`. This was investigated rigorously, not assumed:

1. Reproduced identically across three separate build attempts, including
   one `make dirclean` full-from-scratch rebuild — rules out stale
   incremental build state.
2. Reproduced identically with **all pstore config changes reverted** —
   rules out pstore/the DTS patch as the cause entirely.
3. Vermagic strings (`6.12.94 SMP mod_unload ARMv7 p2v8`) match exactly
   between the working (v9) and broken module — rules out a simple
   kernel-version mismatch; this is a genuine binary struct-layout
   difference, not a version-string one.
4. **Decisive test**: swapped the exact, byte-identical (confirmed via
   sha256) `brcmfmac.ko` that works fine on v9 directly into the locally
   built image (bypassing this build's own compile of the module
   entirely). It failed with the identical error. **This proves the
   module itself is not the problem — the locally-built kernel has a
   different `struct module` ABI than the kernel v9 actually runs.**
5. v9's kernel was never compiled locally at all — it comes from
   `openwrt-imagebuilder-25.12.5-...`, which uses the official OpenWrt
   project's own pre-built kernel binary. A full diff of ImageBuilder's
   `.config` against the local buildroot's `.config` (excluding package
   selections) showed no difference in any hardening/struct-affecting
   option (`RANDSTRUCT`, `MODULE_SIG`, `STACKPROTECTOR`, `LTO`, `KASAN`,
   `UBSAN` all identical) — the only differences were `TARGET_MULTI_PROFILE`
   and per-device package lists, neither of which should affect `struct
   module`'s own layout.
6. Confirmed via web search as a real, currently-open, unresolved upstream
   bug: [openwrt/openwrt#18743](https://github.com/openwrt/openwrt/issues/18743),
   hitting unrelated modules (`ip_set`, `x_tables`, `macvlan`) on a
   different target (ipq40xx) with no root cause or fix documented by
   maintainers.

**Conclusion:** this is a real, reproducible incompatibility between
locally-built (`make world`) kernels and modules built against them, for
reasons not yet identified even after ruling out every hypothesis checked
above (pstore, the module build itself, vermagic, and the visible
hardening-relevant Kconfig options). It is not specific to brcmfmac —
whatever it is would block ANY custom local kernel build for this target
right now, independent of what feature prompted the rebuild.

**Follow-up: build-order/race hypothesis tested directly — falsified, not
assumed either way.** Before accepting "unresolved upstream bug" as final,
tested the cheapest concrete alternative explanation: every attempt above
used `make -j$(nproc) world`, letting make's own scheduler interleave the
kernel package's build with mac80211/brcmfmac's. Reran with explicit
forced serialization instead of trusting make's scheduling: `make -j$(nproc)
tools/install toolchain/install target/linux/compile` run to full
completion first (confirmed via `vmlinux`/`zImage` present, zero errors),
*then* `make -j$(nproc) package/kernel/mac80211/compile` only after that,
*then* the rest of `world`. Flashed and tested: **identical error.** This
decisively rules out a build-order race as the cause — it was a real,
cheap, well-reasoned hypothesis worth testing before calling anything
"unresolved," and it came back false. The original conclusion (a deeper,
non-obvious incompatibility between how this local buildroot toolchain
compiles kernel-external module packages and however the official OpenWrt
build farm does it) is now more confirmed, not less, having survived an
actual falsification attempt rather than resting on a symptom-matched
GitHub issue alone.

**Router state: reverted to v9 (proven-good, unaffected — v9's kernel was
never locally compiled) after every test.** No functional regression
shipped; this section exists so the next person who wants
custom-kernel-level changes here (pstore or anything else) doesn't have to
re-discover any of the above from scratch. The reusable work (861 patch
correctly relocated into `openwrt/package/kernel/mac80211/patches/brcm/`,
pstore kernel config, the ramoops DTS patch, a `files` symlink to
`v2-files` for the FILES overlay) is left in place in the local `openwrt/`
buildroot checkout, uncommitted (that tree is a checkout of upstream
`openwrt/openwrt.git`, not this project's own repo).

## 16. DFS channels — a documentation audit flagged `iw phy info` showing them as available; live-tested, confirmed still a hard wall (2026-07-24)

A systematic doc-vs-code audit noticed `iw phy phy2 info` labeling DFS
channels 52-64/100-140 as `(radar detection)` rather than `(disabled)` —
contradicting every earlier test in this repo, with no CLM/firmware/DTS
change to explain it. Rather than leave this ambiguous, tested it directly:

Pre-flight backup taken, `wireless.radio2.channel` set to `52` (VHT80,
inside the range showing `(radar detection)`), `wifi reload`. Result:
`brcmf_cfg80211_start_ap: Set Channel failed: chspec=57402, -52` — the
identical `-52` error already documented in §11 for the clm_blob mismatch.
hostapd failed to set beacon parameters, `phy2-ap0` went to `DISABLED`.
Direct, immediate driver-level rejection — never got past the initial
channel-spec negotiation, no CAC ever started.

**Root cause of the label change:** this build ships
`wireless-regdb-2026.05.30-r1`; the original DFS tests in §6/§11/§12
predate that package snapshot. The regulatory database's own channel-flag
data has evidently been revised upstream since — the DTS
`ieee80211-freq-limit` gate (the actual, confirmed-unchanged, hard block)
never moved. This is a second, independent instance of the same pattern
§13 already found for `iw reg get`'s cosmetic country-label line: an `iw`
summary display that doesn't reflect what the driver will actually do.

**Reverting was not clean via live reload alone** — `uci set` back to `36`
+ `wifi reload` left `phy2-ap0` in `hostapd.add_iface failed` state, same
"live reload insufficient after a rejected negotiation" lesson as §11. A
full reboot restored all 4 SSIDs cleanly; config values confirmed intact
(passphrases, guest isolation, SQM). Minor, harmless side effect: the
`uci commit` calls during the test reformatted the live router's
`/etc/config/wireless` to UCI's canonical serialization (comments
stripped, values unchanged) — doesn't touch `v2-files/`, the repo's source
of truth.

**Standing conclusion, now confirmed under two independent mechanisms**
(CLM re-pairing in §11, direct channel request here): **DFS remains a
genuine driver-enforced wall on this firmware**, regardless of what any
given regdb snapshot's summary label claims. `iw phy info`'s per-channel
flag word is not a trustworthy signal on this driver — only an actual
`start_ap` attempt is decisive.

## 17. DFS, part 2 — read the driver instead of trusting the error number, tested under two regulatory domains, converges on the same wall (2026-07-24)

§16 treated `chspec=57402, -52` as "the identical `-52` error already
documented in §11" — that phrasing overclaimed precision the evidence
didn't support, caught on a second pass rather than left standing.

**Reading `brcmf_fil_cmd_data()` in the actual driver source**
(`drivers/net/wireless/broadcom/brcm80211/brcmfmac/fwil.c`): when a
firmware IOCTL/IOVAR returns an error, the function only passes the *raw*
firmware error code back to the caller if `ifp->fwil_fwerr` is set on that
interface. `feature.c` only sets that flag transiently around specific
capability-probe calls — not during normal `start_ap`. Otherwise the
function discards the real firmware code and returns the generic
**`-EBADE`** ("Invalid exchange") — which is **also numerically 52** on
Linux. So `-52` from `Set Channel failed` is not provably the firmware's
own `BCME_*` status at all; it may just be this driver's generic
"something failed" stand-in, coincidentally the same magnitude as one
`BCME_*` table entry. The one path that would print the real code
(`brcmf_dbg(FIL, "Firmware error: %s (%d)\n", ...)`, present in the same
function) compiles to a hard no-op in this exact build — neither
`CONFIG_BRCMDBG` nor `CONFIG_BRCM_TRACING` is set — so it can't be
recovered without rebuilding the module with one of those defined. Correcting
§16: the numeric match to the clmload `-52` is not established evidence of
the same underlying cause. What *is* established, independent of what the
number means, is that the firmware refused the chanspec.

**Checked the actual firmware binary for whether DFS exists there at all**
(same static-analysis toolkit §12 used for OWE, `v2-staging/firmware/re-analysis/`):
unlike OWE — zero related strings anywhere — this firmware's string table
has real DFS/radar machinery: `dfs_preism`, `dfs_postism`, `dfs_status`,
`dfs_ism_monitor`, `dfs_channel_forced`, `radarargs`, `radarargs40`,
`radarthrs`, `clear_radar_status`, `phy_dfs_lp_buffer`, `xpCAC`. DFS is not
architecturally absent from this firmware the way OWE is. Attempting a
call-graph trace to whatever gates chanspec validation against this code
was not reliable — the disassembly tool's known section-boundary
limitation (already documented in §12) means naive address-based
cross-referencing on this ROML-overlay firmware isn't trustworthy for a
confident answer here, so this line of attack was not pushed further; the
live regulatory-domain test below was more decisive per dollar spent.

**Tested under two different regulatory domains, not just one.** The
router was not in production service for this pass, so a more invasive
live test was reasonable. `nvram set 2:ccode=US 2:regrev=0` (in-memory
only, never committed to flash), then `echo 0001:04:00.0 >
.../driver/unbind` + `> .../drivers/brcmfmac/bind` to force a real
firmware re-attach on just that one radio (surgical — phy0/phy1 untouched)
instead of a full module reload. Firmware re-downloaded cleanly (same
2015-09-18 build, no clm_blob, as always). Result, under a real national
code instead of Broadcom's generic `Q2` worldwide placeholder: **every DFS
channel (52-144) now shows flatly `(disabled)`** in `iw phy info` — not
`(radar detection)`. The opposite of the naive "maybe US unlocks it"
hypothesis, and a cleaner, more explicit rejection than `Q2`'s ambiguous
label ever was. `Q2` nominally allows attempting the channel (then the
firmware rejects the actual chanspec set, per §16); `US` doesn't even
offer it as regulatorily eligible in the first place. Two different
mechanisms, same real-world outcome: no usable DFS.

**Reverted cleanly:** `nvram set 2:ccode=Q2 2:regrev=86` (restoring the
flashed values, nothing was ever committed to the nvram partition), full
reboot (needed — a driver re-attach doesn't automatically recreate
netifd's AP interface, and this project's own established lesson is that
a live reload alone isn't reliable for restoring radio state after this
class of test anyway). Verified after reboot: fresh boot, `2:ccode`/`2:regrev`
back to `Q2`/`86`, all 4 SSIDs up, `phy2-ap0` back on channel 36, SQM still
shaping, 0% ping loss.

**Standing conclusion, now the most thoroughly tested wall in this
project:** three independent mechanisms (CLM re-pairing §11, direct
channel request under the shipped `Q2` regulatory code §16, direct channel
availability under a real `US` regulatory code here) all converge on the
same answer — this exact firmware does not deliver usable DFS on this
hardware, regardless of regulatory domain or CLM pairing. The firmware
*contains* DFS-related code (unlike OWE, where it doesn't exist at all),
so this isn't provably an architectural absence the way OWE is — but
nothing available to this project (live testing, static analysis within
tool limits, regulatory-domain changes) can make it functional, and the
remaining path to a definitive root cause (rebuilding brcmfmac with
`CONFIG_BRCM_TRACING`/`DEBUG` to recover the real `BCME_*` string, or a
proper cross-referenced disassembly of the chanspec-validation routine)
is a real, scoped, deliberately-deferred next step, not something ruled
impossible — just not proven necessary to reach the practical answer this
pass needed.
