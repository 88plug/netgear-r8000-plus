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

## 18. DFS, part 3 — pulled the deferred lever, got a decisive answer: `BCME_UNSUPPORTED` (2026-07-25)

§17 deferred one real, scoped next step: recovering the actual firmware
`BCME_*` code instead of the generic `-EBADE` stand-in every prior test
observed. Pulled it rather than leave it deferred indefinitely.

**What was built:** `kmod-brcmfmac` rebuilt via the SDK (same mechanism
already proven safe for `patches/861` — a single out-of-tree module rebuild
against the already-running, unmodified kernel, not a full local kernel
build; the pstore/ramoops `struct module` ABI wall in §15 was specific to
`make world`, not this path). First attempt added `-DDEBUG` to the module's
`ccflags-y` — failed to link (`__brcmf_dbg`, `brcmf_debugfs_*` undefined:
those live in `debug.o`, gated by a separate Kconfig symbol the plain
`DEBUG` macro doesn't pull in). Simplified instead of chasing the Kconfig
plumbing: `brcmf_fil_get_errstr()`'s string table is self-contained and
already degrades safely to `""` without `DEBUG` defined; the actual blocker
was only the one `brcmf_dbg()` call that needed `debug.o`. Swapped that one
call to `bphy_err()` — same always-compiled, always-printing pattern
already used elsewhere in this exact file — recovering the raw firmware
`fwerr` value with zero new module dependencies.

**Live-tested** (router not in production service for this pass): pre-flight
backup, `rmmod brcmfmac_wcc && rmmod brcmfmac` (unload order matters —
`brcmfmac_wcc` holds a reference into `brcmfmac`), `insmod` the debug build,
`/etc/init.d/network restart` (a bare `wifi reload` left hostapd unable to
find the renumbered phys — the module swap reassigns phy indices, same
"live reload alone isn't reliable after a driver-level change" lesson as
§11/§16), confirmed all 4 SSIDs healthy on the debug module before testing
anything. Set `radio2` (now `phy5`) to channel 52, `wifi reload radio2`.

**Result:** `brcmf_fil_cmd_data: Firmware error:  (-23)` — the raw firmware
code, not the generic `-52`/`EBADE` every earlier test saw. Index 23 in
brcmfmac's own `BCME_*` table (`fwil.c`) is **`BCME_UNSUPPORTED`** — an
explicit, unambiguous "not implemented" response from the firmware itself,
not `BCME_BADCHAN`/`BCME_OUTOFRANGECHAN` (which would suggest a fixable
request-formatting issue) and not a driver-side translation artifact. This
is the cleanest possible negative result available from live testing on
this hardware.

**Reverted cleanly:** channel back to 36, stock `brcmfmac.ko` restored
(md5 `8398326d...`, matching every prior verification this repo has done
of that exact module), full reboot. Verified after reboot: fresh boot, all
4 SSIDs, guest isolation, SQM still shaping, 0% ping loss. The diagnostic
`bphy_err()` swap was never meant to ship and was not carried into
`patches/861` or any shipped image — it lived only in the SDK's temporary
build tree for this one test and was discarded when that tree's own
post-build cleanup ran.

**Final standing conclusion — now genuinely closed, not just repeatedly
re-confirmed:** DFS on this exact firmware (`7.35.177.56`, 2015-09-18)
returns `BCME_UNSUPPORTED` for the chanspec this router's only DFS-capable
radio would need. Four independent tests (clm_blob re-pairing §11, direct
request under the shipped `Q2` regulatory code §16, direct request under a
real `US` regulatory code §17, and now the raw firmware error code itself)
all converge, and the last one removes the only remaining ambiguity the
first three had. Nothing shipped changes as a result — this is evidence
quality, not a new capability.

## 19. WiFi repeater (AP+STA on one radio) — the orchestration layer breaks it, not the hardware (2026-07-24)

Requested feature: repeat an existing third-party 2.4GHz network
(`Eufy_B838D4`, WPA2-PSK, channel 6) on `radio1`, so devices near this
router get a strong signal without their own connection to the original AP.
Initial attempts through the standard path (a normal `mode=ap` `wifi-iface`
on `radio1` alongside the `mode=sta` interface associating to Eufy) were
initially treated as "impossible" — that conclusion was wrong, and testing
it properly is what this section documents.

**What actually fails, precisely:** bringing up an AP-mode `wifi-iface` via
UCI/`wifi reload` on a radio that already has an active STA-mode interface
routes through `brcmf_cfg80211_request_ap_if()` in
`drivers/net/wireless/broadcom/brcm80211/brcmfmac/cfg80211.c`. This
project's own `patches/861` already fixes that function's v1/v2
version-query failure to fall through to the legacy `bsscfg:ssid` MBSS path
(`iface_create_ver = 0`) instead of returning `-EOPNOTSUPP` outright — that
fix is real and is what unlocked the 6-BSS multi-SSID setup documented in
§8. But that legacy MBSS fallback path itself only supports adding AP-role
interfaces alongside *other AP-role interfaces* — the §8 scenario (2nd/3rd
AP SSID on one radio). It does not support adding an AP-role interface
alongside an *active STA-role* interface on this firmware. Confirmed via
dmesg, tested with both bring-up orders (AP-first, STA-first — same result
either way, proving it's order-independent, not a race): the driver reaches
the v0 legacy fallback and still fails there — `brcmf_cfg80211_add_iface:
... Does not support interface_create (-95)` — with `netifd`/`hostapd`
visibly cycling the interface name back and forth
(`phy1-ap0`↔`phy1-sta0`) as it retries and fails.

**Whether this is a real hardware/firmware limit or an orchestration bug
was the actual open question**, and it was tested rather than assumed:

- `iw phy phy1 info`'s advertised interface combinations list AP+STA
  together as a supported combination on this radio — the chip/firmware
  claims it can do this.
- A raw `iw phy phy1 interface add rpt0 type __ap`, issued directly while
  the STA interface was live and associated, **succeeded immediately and
  stably** — confirmed repeatedly, including across full reboots. This
  bypasses `request_ap_if()`'s MBSS-bsscfg allocation entirely; it's a
  plain vif creation, which the firmware handles cleanly even with an
  active STA interface. `hostapd` run directly against that raw interface
  (not through `netifd`'s `hostapd.sh`/wifi-scripts glue) came up cleanly
  and did not disturb the STA interface's association.
- Prior art search (not assumed, checked): OpenWrt issue **#14451**
  documents the same symptom class — AP+STA failing through the generic
  wifi-scripts orchestration — on **ath/ipq40xx hardware**, a completely
  different chipset and driver. That rules out a brcmfmac-specific defect;
  this is a cross-chipset limitation in how OpenWrt's generic
  `wifi-scripts`/`wireless-device.uc` orchestrates AP+STA bring-up
  (confirmed by reading `/usr/share/ucode/wifi/*.uc` and
  `/lib/netifd/wireless-device.uc`: wdev/whole-phy teardown-and-setup is
  driven as one atomic operation per phy, not per-vif, which is what
  collides here), not a limit of this hardware or of `patches/861`.

**The shipped fix** (`etc/init.d/eufy-repeater`, `START=96`/`USE_PROCD=1`):
reads any `wireless` `wifi-iface` section with `mode=ap` and
`repeater_mode='1'` set (that section stays `disabled='1'` from
`wifi-scripts`'/`netifd`'s own point of view — it must never try to bring
it up itself, since that's exactly the broken path above). For each one, it
resolves the section's radio to a `phyN` via `wireless.<device>.path`
(stable across reboots; `phyN` numbering itself is not), waits for the
existing STA interface on that radio to show `Connected` via `iw dev
<ifname> link` (deliberately radio-level, not netifd's DHCP-gated
interface state — this router's own DHCP lease on the repeated network has
nothing to do with whether the repeater itself can relay other devices'
traffic, and waiting on it was blocking startup for no reason), then
creates the AP side with the same raw `iw phy <phy> interface add <name>
type __ap` proven above, sets its channel to match the STA's associated
channel, and runs `hostapd` + `relayd -I <ap_ifname> -I <sta_ifname>`
directly as procd instances (respawning). This uses the exact same
underlying tools (`iw`, `hostapd`, `relayd`) `wifi-scripts` itself would
use — it deliberately routes around only the specific orchestration step
that's broken for this combination, not around the standard stack
wholesale.

**TX power, a secondary finding caught during this work:** `hostapd.conf`
has no `tx_power` directive — confirmed directly against the real
`hostapd.conf` reference generated in this project's own SDK build tree,
not assumed from memory. TX power is set at the `iw`/cfg80211 level
instead (`iw dev <ifname> set txpower fixed <mBm>`, mBm = dBm × 100),
applied by `eufy-repeater` to the raw-created interface after creation.
Checking the real ceiling in the process (`iw phy phy1 channels`, "Maximum
TX power" per channel) found `radio1`'s configured `txpower='27'` was never
achievable on channel 6/US regulatory domain — the real ceiling there is
20.0 dBm. Fixed in `config/wireless` (`radio1.txpower` 27→20) as part of
this same pass, independent of the repeater feature itself but caught
while max-power-verifying it, per the operator's explicit ask
("is it max strength for repeating the eufy — it should be also
documented").

**Standing pattern this establishes:** an "X+Y combination is impossible on
this radio" claim, sourced from OpenWrt's generic orchestration failing, is
not the same claim as "the hardware/firmware cannot do X+Y." The former is
common and was confirmed here to be a real, cross-chipset orchestration
bug (OpenWrt issue #14451); the latter needs its own direct test (`iw phy info` combos +
a raw bring-up attempt) before being accepted. Route around the broken
orchestration layer with the same underlying primitives it would have
used, rather than accepting the combination as unsupported.

**WDS/4addr considered and ruled out, not just skipped for relayd's sake:**
before settling on relayd, checked whether a true L2 bridge (4addr/WDS —
the option OpenWrt's own docs prefer over relayd where available, since it
avoids relayd's proxy-ARP userspace overhead entirely) was possible
instead. It isn't, on this hardware — confirmed directly against this
repo's own build tree (`openwrt/build_dir/.../brcmfmac/cfg80211.c`, kernel
6.12.94), not just cited from memory: `NL80211_IFTYPE_WDS` is explicitly
rejected with `-EOPNOTSUPP` in `brcmf_cfg80211_add_iface()`,
`brcmf_cfg80211_del_iface()`, and `brcmf_cfg80211_change_iface()` (the last
one also logs `"type (%d): currently we do not support this type"`) — a
hard driver-level rejection, not a config gap. Moot anyway even on
WDS-capable drivers: 4addr client mode requires the *upstream* AP to
negotiate it too, and an arbitrary third-party AP (a Eufy camera base
station, someone else's router) isn't going to expose that. relayd is the
only path this hardware/use-case combination actually supports, not merely
the first one that worked.

**Future consideration, not yet hit:** the operator's stated longer-term
goal is other radios eventually repeating other networks too. Community
reports (2 independent OpenWrt forum threads) describe real fragility
running more than one relayd instance at once, even across dedicated
radios — one maintainer called it "quite a hack." `etc/init.d/eufy-repeater`
is already written generically (loops over every `repeater_mode='1'`
section, not hardcoded to one), so a second instance is a config-only
change to try — but treat the first multi-radio attempt as a real test,
not an assumed-safe extension of what's proven here for one.

**Addendum (2026-07-25) — default-route leak, found by actually flashing
v13, not just building it.** `network.eufy_wwan` (`proto=dhcp`, the STA
side's own lease) had no `defaultroute`/`peerdns` override. Live-tested
right after flashing v13 for real (not just building/inspecting it):
`ip route show` after boot showed the default route pointing at
`192.168.32.2 dev phy1-sta0` — the *repeated* network — not `wan`.
`network.wan` and `network.eufy_wwan` both installed a default route at
the same metric (`0`), and the kernel kept whichever was installed last,
which was `eufy_wwan`'s. This router's own outbound traffic (unrelated to
what the repeater relays for other devices — that's handled separately,
at L2, by `relayd`) started intermittently routing out through the
third-party Eufy network instead of the real WAN: `ping 8.8.8.8` showed
~50% loss, while a 1-hop `ping` to the real WAN gateway stayed a clean 0%
the whole time — the 1-hop test alone would have missed this completely,
since it never touches the default route at all.

**Fixed** by setting `option defaultroute '0'` and `option peerdns '0'` on
`network.eufy_wwan` (both real config and the documented
`wireless.example` pattern) — standard OpenWrt options for exactly this
case, telling `netifd`/`udhcpc` not to install a route or DNS servers
from that interface's lease. Verified live: `ip route show` correct
(`default via 192.168.52.1 dev wan`), `ping 8.8.8.8` 10/10 (0% loss),
DNS still resolving via the router's own resolver, `eufy_wwan` itself
still reaches its own subnet fine (`ping -I phy1-sta0 192.168.32.2`,
0% loss — so `relayd` can still relay real client traffic through it),
and both `hostapd-eufy_ap`/`relayd-eufy_ap` survived the `/etc/init.d/network
restart` needed to apply the fix. Shipped in v14 (see `docs/WINS.md`).

**Standing lesson:** repeating a third-party network's STA side is not
just "add a `proto=dhcp` interface" — that network's DHCP server can
compete for this router's own default route and DNS resolution, silently,
with no error anywhere. Any future repeater instance (the multi-radio
future-consideration above) needs `defaultroute '0'`/`peerdns '0'` from
the start, not discovered the same way again. Also: a 1-hop gateway ping
is not sufficient evidence of real internet reachability when a second
default-route candidate exists on the box — test a real multi-hop target.

**Addendum (2026-07-25) — two more real bugs, found by adversarial review
before shipping v15, not by symptom.** After flashing and fixing the
default-route leak above, did a fresh-eyes read of `etc/init.d/eufy-repeater`
and its config looking for other real bugs before calling the feature done
— found two, neither of which had produced any visible symptom yet:

1. **Channel detection was dead code.** Line parsed `iw dev "$sta_ifname"
   link` for a `channel` field to match the raw AP's channel to wherever
   the STA actually associated. Real `iw dev link` output on this
   OpenWrt/iw version has no `channel` field at all — only `freq: <MHz>`
   (confirmed live, both before and after this fix). The regex never
   matched, silently falling through to the static `uci get
   wireless.<device>.channel` fallback every single time. Worked in
   practice only because that static value was kept manually in sync with
   reality — the actual designed mechanism (dynamically follow the
   upstream AP's real channel) never ran once. Fixed: parse `iw dev
   "$sta_ifname" info` instead, which does report `channel N (... MHz)`
   — verified against real output before and after the fix.

2. **The raw AP interface had zero firewall coverage — the more serious
   one.** `firewall.eufy` only listed `list network 'eufy_wwan'`, which
   covers `phy1-sta0` (the STA side). `rpt_eufy_ap` — the interface real
   clients actually connect to — is deliberately never bound to any UCI
   `network` section (it doesn't need its own IP; relayd bridges it
   directly), which made it invisible to `list network` and therefore
   subject to the default `forward 'REJECT'` policy. A prior version of
   this same file's own comment claimed relayd operates "L2, not routed
   through netfilter the same way" — that was wrong, asserted without
   checking. Verified via the official OpenWrt relayd guide (fetched
   directly, not from memory): relayd is a routed mechanism, and its
   guide's own example config places the local AP-side interface in the
   *same firewall zone* as the upstream link, not outside the firewall
   entirely. Confirmed against this router's own generated nft ruleset:
   before the fix, zero rules referenced `rpt_eufy_ap` anywhere. Fixed
   with `list device 'rpt_eufy_ap'` on the `eufy` zone — `device` is a
   real, separate fw4 zone-membership option (confirmed against
   `/usr/share/ucode/fw4.uc`, not assumed), letting a zone cover a raw
   device that has no UCI `network` section. Verified: the regenerated
   nft ruleset's `input_eufy`/`forward_eufy`/`output_eufy`/`helper_eufy`
   chains all expanded their interface match set to include
   `rpt_eufy_ap` alongside `phy1-sta0`.

**Why this pair matters more than the default-route bug:** both of these
are invisible to every check this project had already been running —
`hostapd-eufy_ap` and `relayd-eufy_ap` both report healthy, the STA shows
`Connected`, `iwinfo` reports correct signal/power — none of that
depends on the firewall zone or the channel-follow logic at all. A real
client joining the repeated SSID would have associated fine and then
gotten nothing, silently, with every internal signal this project
actually checks saying "working." Caught only by reading the config with
fresh eyes and checking against the mechanism's real, external
documentation instead of trusting an earlier assumption written into the
code's own comments.

**Addendum (2026-07-25, v16) — a real client, live traffic, and a 4th bug
found trying to debug a 3rd.** With v15 flashed and verified clean, a real
device joined `rpt_eufy_ap` for the first time and immediately got stuck
in a fast associate/disassociate loop (~4s cycle), confirmed via `logread`
across two different client MACs. Root cause not fully resolved this pass
(see below), but investigating it directly caused, and then caught, a
separate, real, and more fundamental bug:

- **`usteer` (this router's band-steering daemon) auto-discovers every
  local hostapd node, including the repeater's, and was injecting
  cross-network 802.11k neighbor reports** (`ubus call usteer local_info`
  showed `hostapd.rpt_eufy_ap` listed with `rrm_nr` alongside the router's
  own unrelated `R8000` 5GHz nodes). A client on `Eufy_B838D4` being told
  about neighbor APs named `R8000` is a plausible contributor to
  roam/reconnect confusion. Attempted fix: `usteer.@usteer[0].ssid_list
  'R8000'` to scope steering to the router's own SSID only — applied and
  committed, but `local_info` still lists the node afterward (the option
  appears to gate active steering decisions, not node discovery/neighbor
  reporting), so this is only a **partial, unconfirmed** mitigation, not a
  verified fix. Left in place since it's directionally correct and
  harmless, but the disconnect loop itself is not yet confirmed resolved
  by it — needs a real client retest, not just an unchanged `local_info`
  reading.

- **The 4th bug, found by accident while chasing the above.** Tried to
  get real EAPOL-level detail by editing the live `hostapd-eufy_ap.conf`
  to add verbose syslog logging and restarting hostapd. That killed the
  service outright: **killing hostapd also destroys `rpt_eufy_ap`** — the
  raw interface's lifetime is tied to hostapd's own nl80211 socket, a
  driver/kernel behavior this script doesn't control, not something
  assumed, confirmed by hitting it live (`Could not read interface
  rpt_eufy_ap flags: No such device` on the next start attempt). Worse:
  `etc/init.d/eufy-repeater`'s `hostapd-$section` procd instance had a
  bare `respawn` pointed directly at `/usr/sbin/hostapd $conf` — after
  the interface disappeared, procd kept re-executing that same command
  against a device that no longer existed, forever (`"running": false,
  "exit_code": 1`, no self-healing). **This means any hostapd crash for
  any reason — not just a manual kill, an OOM-kill, a firmware hiccup,
  anything — would have permanently taken the repeater down until the
  next full reboot**, with nothing about the failure visible except the
  service quietly not running.

  Fixed by moving interface (re)creation *inside* the respawned command
  itself, instead of doing it once before the first start: the procd
  instance's command is now `/bin/sh -c "iw dev del ...; iw phy ...
  interface add ... && iw dev ... set channel ... && exec hostapd ..."` —
  every respawn, not just the first one, recreates the interface before
  handing off to hostapd. Verified live, twice: normal `restart` still
  works cleanly (fresh interface, fresh pid), and a direct `kill` of the
  hostapd pid now self-heals within one respawn cycle (~5-8s) — new
  ifindex, new hostapd pid, `relayd` (which also loses its raw socket
  when the interface disappears) recovers shortly after on its own
  respawn. Shipped as **v16**.

**Standing lesson, again:** this is the second time this session a fix
attempt for one problem (verbose debug logging, meant to be purely
diagnostic and non-destructive) caused a worse, unrelated, real failure.
Treat "just add logging and restart" as a real action with real risk on a
live single-radio repeater, not a free/inert diagnostic step — the raw,
manually-created interface has none of the safety nets a normal
netifd-managed interface would have around a service restart.

**Addendum (2026-07-25, v17) — a 5th infrastructure bug, and the real
answer on whether this repeater can ever work for Eufy cameras specifically.**

Two more real, confirmed things, one a fix and one a hard external limit:

1. **`relayd -B -D` were never actually enabled.** `relayd --help` on this
   exact build shows broadcast forwarding (`-B`) and DHCP forwarding
   (`-D`) are both off unless passed explicitly - the v16 invocation was
   bare `relayd -I ap -I sta`, so DHCP broadcasts from a client on the raw
   AP were never relayed to the real upstream DHCP server at all. Fixed:
   `relayd -B -D -I "$ap_ifname" -I "$sta_ifname"`.

2. **`rpt_eufy_ap` and `phy1-sta0` shared an identical MAC address** -
   `iw phy ... interface add ... type __ap` with no explicit `addr`
   defaults to the radio's existing MAC, confirmed live (`cat
   /sys/class/net/{rpt_eufy_ap,phy1-sta0}/address` both returned
   `e8:fc:af:f9:f1:37`). relayd's `-I` explicitly does "ARP cache and
   host route management" per interface, keyed by MAC - two of relayd's
   own interfaces sharing one MAC breaks that bookkeeping outright,
   independent of the `-B`/`-D` fix. Also confirmed: `addr <mac>` passed
   inline to `iw phy ... interface add` is silently ignored by this
   driver (command succeeds, MAC doesn't change) - the MAC has to be set
   as a separate `ip link set dev <if> down / address <mac> / up`
   sequence afterward, which does take effect and survives. Fixed:
   `eufy-repeater` now derives a distinct MAC from the STA interface's
   own address (XOR the locally-administered bit on the first octet -
   same convention this project's own `main_radioN` AP interfaces already
   use) and applies it via that down/set/up sequence, folded into the
   same respawn-safe command from the v16 fix so a crash-recovery still
   gets a correctly-MACed interface.

3. **The actual client identity, checked rather than assumed.** The two
   devices that kept associating-then-disassociating within ~4s
   (`04:17:b6:c7:bb:ea`, `8c:85:80:65:f5:55`) both resolve via MAC-OUI
   lookup to **Smart Innovation LLC** - the same manufacturer as the real
   Eufy HomeBase's own BSSID (`04:17:b6:b8:38:d4`). These are genuine Eufy
   hardware (cameras), not a phone auto-joining a familiar SSID name -
   ruling out the "captive portal / OS no-internet detection" hypothesis
   from the v16 addendum outright.

4. **Researched rather than guessed further: does a generic hostapd+relayd
   repeater have any real chance of extending a Eufy camera backhaul
   network at all?** Answer, with sources, not assumption: most Eufy
   devices talk to their HomeBase over a **proprietary wireless protocol**
   - one independent developer community (working on the `eufy-security`
   P2P integration) names it **ESWP (Eufy Security Wireless Protocol)**,
   distinct from generic WiFi+DHCP. Eufy's own documented, supported
   range-extension paths are a second real HomeBase in repeater mode,
   Eufy's own branded WiFi Repeater accessory (T8024), or Multi-Bridge
   (HomeBase 3 only, camera joins via a router's WiFi) - every one of
   which works because it runs Eufy's real firmware/protocol stack, not
   because it's "just WiFi." No documented case was found anywhere of
   generic third-party/OpenWrt equipment successfully repeating this
   backhaul network.

**Standing conclusion:** all 4 infrastructure-level fixes across v13-v17
(default-route leak, dead channel-detection, missing firewall zone,
missing DHCP/broadcast forwarding, MAC collision) are real, independently
verified, and correct - and every one of them still left the exact same
~4-second associate/disassociate pattern unchanged, against devices now
confirmed to be genuine Eufy hardware. That consistency across five
independent, technically-unrelated fixes is itself evidence: this reads as
a protocol-level rejection by the camera's own logic (it decides this
isn't its real hub and leaves), not a WiFi/DHCP/firewall configuration
gap. Shipped as **v17** regardless - the infrastructure fixes are correct
and matter for repeating any ordinary (non-proprietary) network through
this same feature, but this specific goal (extending eufyCam's own
backhaul for the cameras themselves) may not be achievable by this
project without reverse-engineering ESWP itself, which no prior art
search found anyone having done for range-extension purposes.

## 20. Same-radio AP+STA repeater — definitively a chip-level limit, not Eufy/ESWP-specific (2026-07-25)

> **Reader's note, added after §25 (2026-07-26):** this section's title and
> the "SME: Authentication timed out" behavioral evidence below were, at
> the time of writing, believed to directly demonstrate a same-radio
> STA+AP concurrency limit. §25 later ran the one control this section
> never did - the identical AP-alone test with the STA vif genuinely
> absent, not just disconnected - and got the **same auth-timeout with no
> concurrency present at all**. That does not overturn this project's
> conclusion (§23's direct firmware `cap`-string read independently
> confirms the underlying MCHAN/RSDB absence this section attributes the
> symptom to), but the specific real-client auth-timeout evidence quoted
> immediately below is now known **not** to be diagnostic of concurrency
> by itself. Read this section for the architectural/driver-source
> findings (still solid); read §25 before treating the client-auth
> symptom quoted here as proof of the concurrency claim specifically.

**The §19 "ESWP protocol rejection" theory above is superseded.** Retested
the identical repeater architecture against a completely unrelated,
ordinary 2.4GHz WPA2 network ("American", a real neighboring router, zero
Eufy/ESWP involvement) - same result: `iw dev rpt_eufy_ap station dump`
empty, RX bytes permanently 0, real client (`wpa_supplicant` directly on a
separate Linux box) gets `SME: Authentication timed out` - the very first
802.11 management frame never completes. This is not a camera-side
protocol decision; it never was. Nine independent angles were tested this
session, live, all converging on the same wall:

1. **Raw `iw`-created AP + `apsta=0`→`1` driver patch**
   (`patches/862-brcmfmac-r8000-force-apsta-concurrent.patch`) - fails,
   RX=0.
2. **Same + apsta routed via the primary ifp** (matching P2P's exact
   addressing - `patches/863-brcmfmac-r8000-apsta-via-primary-ifp.patch`)
   - fails identically. Traced why: this router's AP vif already satisfies
   the driver's `!mbss && (ifidx==0 || no-RSDB-and-no-MCHAN)` OR-condition
   via the RSDB/MCHAN term alone, so 862 and 863 hit the exact same code
   path - the ifidx routing was never the gating factor.
3. **OpenWrt's own official, documented method** (plain UCI `wifi-iface`
   sections, AP+STA both on one `device`, netifd/hostapd owning interface
   creation - confirmed via the current `wifiextenders/relay_configuration`
   wiki page, not the retired page names) - fails *worse*: netifd's atomic
   whole-phy teardown-and-rebuild on wifi-iface changes collides with the
   live STA, regressing to `brcmf_cfg80211_request_ap_if: ... Does not
   support interface_create (-95)` before the apsta/mbss branch is ever
   reached. Confirms the project's existing raw-`iw`-after-STA-is-stable
   approach is the *better* of the two, not a mistake.
4. **P2P-GO firmware path** (the mechanism P2P actually uses for real
   concurrent AP+STA, via the `p2p_ifadd` firmware iovar with an explicit
   role field - structurally different from a plain AP vif) - blocked
   before even reaching that far: creating the P2P Device management
   interface itself fails (`brcmf_p2p_set_firmware: failed to update
   device address ret -52`).
5. **Firmware upgrade, 2015→2021** (see below) - byte-for-byte identical
   symptom on both. Rules out stale/incomplete firmware as the cause.
6. **WDS / 4-address mode** - structurally impossible on two independent
   grounds: brcmfmac never sets `WIPHY_FLAG_4ADDR_STATION` (kernel's
   `nl80211_valid_4addr()` rejects it before any driver code runs), and
   even on a driver that supported it, WDS requires the *upstream AP's*
   explicit cooperation - unusable against an arbitrary third-party
   network by definition.
7. **Force `mbss=true` via a throwaway second AP-role interface**, created
   first (so the *real* repeater AP is the 2nd AP-role vif on that phy at
   allocation time, per `brcmf_alloc_vif()`'s `vif_walk->wdev.iftype ==
   NL80211_IFTYPE_AP` count, which does not count the STA) - the
   interface-creation attempt itself now fails one level deeper:
   `brcmf_cfg80211_add_iface: iface validation failed: err=-16` (EBUSY).
   **cfg80211's own advertised valid-interface-combinations reject "1 STA
   + 2 AP" outright**, before mbss or apsta are ever consulted. This is
   the most fundamental wall found: not a driver bug, not a firmware
   iovar, a kernel-level combination the chip's driver never advertises
   as legal.
8. **LuCI's wizard tooling** - confirmed to have no repeater-specific
   wizard at all (only a WISP "Join Network" client-uplink flow) and zero
   driver-capability validation on its manual "Add interface" path - it
   would let a user build the exact same doomed config with no warning.
9. **External corroboration** - a DD-WRT user on this *exact* hardware
   (R8000, BCM43602/1, same brcmfmac/Cypress firmware stack) hit the
   identical `wl apsta failed` crash
   (community.infineon.com/t5/.../need-help-with-fw-crash/td-p/379024).
   Cypress's own engineer, in the 2018 upstream commit that introduced the
   apsta iovar, states the failure mode by name: *"When starting station
   mode on wlan0 and AP mode on wlan1, the apsta will be disabled and
   cause data stall on wlan0 (station)."* The same wall is independently
   reported on BCM43455 (openwrt/openwrt#23069, raspberrypi/linux#7092),
   cyw43438, and BCM4355 - a chip-family pattern, not an R8000 or Eufy
   quirk. Cypress/Infineon submitted further AP+STA-concurrency fixes as
   late as July 2022 with no evidence they ever shipped to this chip's
   firmware.

**Verdict: same-radio AP+STA repeating is not achievable on this
hardware/firmware combination, full stop.** Every layer that could
plausibly be blamed - our driver patch, our interface-creation method,
firmware vintage, the specific upstream network, the specific concurrent-
role mechanism - has been independently tested and ruled out. The
remaining, only-viable path to a working repeater is a USB WiFi dongle
providing a genuinely separate radio for the STA role (`kmod-rt2800-usb`
+ an RT5370-chipset dongle is the recommended combination - confirmed
present in the 25.12.5 feed for this exact kernel/arch, hardware-side two
idle USB ports confirmed present), keeping the AP-repeater role on
radio1's native BCM43602 antenna/power. Not attempted this session -
needs a physical dongle.

### Firmware upgrade (real, unrelated win, kept regardless of the above)

The stock/`kmod-brcmfmac`-packaged `brcmfmac43602-pcie.bin` is version
`7.35.177.56 (r587209)`, dated **September 2015** - confirmed via the
Ubuntu kernel-team mailing list to be the final-ever refresh of that exact
file (595472 bytes) in the linux-firmware ecosystem; nothing has updated
it since. Netgear's own stock R8000 firmware (`R8000-V1.0.4.88_10.1.88`,
this project's own recovery-net image) ships a proprietary Broadcom `DHD`
driver (`dhd.ko`, compiled May 2024, still actively maintained) with an
embedded firmware image for chip revision **43602a1** - matching this
router's own chip stepping (`BCM43602/1` in dmesg) - at **version
7.10.274.3.REBASE.R493518, dated 2021-06-02**, six years newer. Extracted
the raw ucode array (`dlarray_43602a1` symbol, `.init.data` section) from
that `.ko` via `readelf`/`dd`; container/header format matches the
existing brcmfmac-loaded blob byte-for-byte. Deployed live (module
unload/firmware-swap/reload): loads clean, all 3 radios up, no new
instability, confirmed via reboot. Same-radio repeater symptom unchanged
(see above) - this firmware is not the bottleneck for that specific
problem, but it's a real six-year currency improvement kept regardless.
Baked into `v2-files/lib/firmware/brcm/brcmfmac43602-pcie.bin` (overrides
the `brcmfmac-firmware-43602a1-pcie` package's own bundled 2015 blob via
the `FILES=` overlay) starting v18.

## 21. Proxy-STA / Proxy-STA-Repeater ("psta"/"psr") — tenth angle, also dead (2026-07-26)

Deep research into FreshTomato/DD-WRT/Asuswrt-Merlin (all three drive
Broadcom radios via the proprietary closed `wl`/`wlconf` stack, never
brcmfmac) surfaced a genuinely different mechanism from everything tried
in §20: Broadcom's firmware-native **Proxy STA / Proxy STA Repeater**
mode. Unlike our apsta-based approach (a second concurrent AP-role vif),
`psta`/`psr` is set as a firmware iovar **on the STA interface itself**
(`WLC_SET_INFRA=1`, no `WLC_SET_AP`, `apsta` stays 0) - real repeater
behavior implemented internally by firmware, not by a second Linux-level
bsscfg going through `brcmf_cfg80211_start_ap()`'s apsta-forcing code at
all. Confirmed real, not speculative:

- `PSTA_MODE_DISABLED=0` / `PSTA_MODE_PROXY=1` / `PSTA_MODE_REPEATER=2`,
  from Broadcom's own `wlioctl_defs.h` (cross-checked against multiple
  vendor SDK trees on GitHub).
- The exact sequence (from FreshTomato's own `wlconf.c`, line ~2000-2011):
  `WL_IOVAR_SETINT(name, "psta", PSTA_MODE_REPEATER)` then
  `WL_IOVAR_SETINT(name, "psta_mrpt", val)`.
- The literal ASCII string **"psta psr" is embedded in both of this
  router's own firmware blobs** (the 2015 stock blob and the 2021 blob
  extracted from Netgear's own `dhd.ko` in §20) - real, checked via
  `strings`, not assumed.

**Live-tested, real negative result.** Wrote `hwoffload-research/psta-probe/`
- a small, revert-safe, one-shot diagnostic kernel module (same pattern
as this project's `fa_probe.ko` work) that calls brcmfmac's own
`EXPORT_SYMBOL_GPL`'d `brcmf_fil_iovar_data_set()` directly via
`symbol_get()` (no brcmfmac source modification - brcmfmac was built in a
different tree so no `Module.symvers` was available for normal static
cross-module linking, hence the runtime symbol lookup) to send
`psta=PSTA_MODE_REPEATER` to the live, already-associated `phy1-sta0`
interface. Result: `err=-52` - the same generic firmware-rejection code
seen throughout §19/§20 for every other unsupported operation on this
chip/firmware. Unload cleanly reverted (also `-52`, consistent - nothing
was ever set), zero side effects, STA connection unaffected throughout.

**Correction (same session, retested more carefully): the -52 above was a
test-methodology bug, not a firmware rejection.** Re-reading `wlconf.c`
more closely showed the real driver always brackets the `psta` set with
`WLC_DOWN` ... `WLC_UP` (`BRCMF_C_DOWN`/`BRCMF_C_UP` in brcmfmac's own
command numbering) - our first probe set `psta` on an already-up,
already-associated interface. Rebuilt the probe to do
`BRCMF_C_DOWN` -> set `psta`=`PSTA_MODE_REPEATER` (+ `psta_mrpt`) ->
`BRCMF_C_UP`, matching the real sequence exactly. Result: **every call
returned err=0** (accepted), and a separate read-only readback module
(`psta_readback.c`, same `symbol_get()` technique) confirmed
`psta` genuinely stuck at 2 through re-association - not silently reset.
**This firmware does support Proxy-STA-Repeater mode; that part is real.**

**But it is not a usable repeater today.** No new Linux netdev/interface
appeared, no `WLC_E_IF` interface-add event was logged, and a real client
test against the router's own (separately-created, unrelated)
`rpt_eufy_ap` interface still showed the identical zero-RX/auth-timeout
behavior - because that interface has nothing to do with `psta`. The
likely explanation: brcmfmac's event handler only turns a firmware
`WLC_E_IF` event into a Linux netdev when the driver itself *armed* an
expectation for one first (`brcmf_cfg80211_arm_vif_event()`, the same
mechanism used for P2P-GO creation) - since brcmfmac never sends `psta`
and never arms for it, any companion bsscfg the firmware creates
internally for PSR mode has no path to becoming a usable Linux interface
we could bridge into the LAN, regardless of whether the firmware itself
is doing something with it. Turning this into an actual working repeater
would require real brcmfmac driver development (teaching the event
handler to recognize and expose a PSR-mode companion bsscfg) - a genuine
kernel driver project, not a config/iovar-only fix like the rest of this
document's findings.

**Revised bottom line:** the tenth angle found a real, corrected fact
(PSR mode is genuinely supported and settable on this exact firmware, not
rejected) but not a working feature - the gap is now precisely a driver
gap, not an unknown/firmware gap. Still no repeater today without either
the USB dongle path or a real brcmfmac patch adding PSR-companion-bsscfg
handling (out of scope for this session; flagged here as the one
concretely-scoped follow-on worth a dedicated future effort, unlike the
other nine dead ends).

## 22. psta driver patch built, deployed, and tested twice - the companion
interface genuinely does not exist; psta is also the wrong mechanism for
this project's repeater shape anyway (2026-07-26)

Wrote the real fix §21 called for: `patches/864-brcmfmac-r8000-psta-repeater-vif.patch`
adds `brcmf_start_psta_repeater()` to `cfg80211.c`, following
`brcmf_apsta_add_vif()`'s exact template - `brcmf_alloc_vif()` ->
`brcmf_cfg80211_arm_vif_event()` -> `BRCMF_C_DOWN` -> set
`psta`=`PSTA_MODE_REPEATER` (+`psta_mrpt`) -> `BRCMF_C_UP` ->
`brcmf_cfg80211_wait_vif_event(cfg, BRCMF_E_IF_ADD, ...)` -> on success,
`brcmf_net_attach()` the companion netdev. Exported
(`BRCMF_EXPORT_SYMBOL_GPL`) so a small trigger module can call it against
the live, associated `phy1-sta0` ifp. Confirmed via provenance search
(background research agent, kernel mailing list/patchwork/lore.kernel.org,
GitHub code search across all public repos including Infineon's own
actively-maintained downstream fork, OpenWrt issue tracker): **no prior
attempt at brcmfmac psta support exists anywhere searchable.** This is
genuinely unattempted driver work, not a rediscovery.

**Real build-system trap, fixed:** the first build (against this
project's own `openwrt/` source buildroot) compiled and exported the
symbol cleanly but **failed to load on the router at all** -
`module brcmfmac: .gnu.linkonce.this_module section size must match the
kernel's built struct module size at run time`. Root cause: this
project's shipping `kmod-brcmfmac` (all v7-v18 images) is built from the
separate **SDK tree**
(`openwrt-sdk-25.12.5-bcm53xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64`),
not the main `openwrt/` source buildroot - the two kernel trees have
drifted (confirmed via the OpenWrt kernel-ABI tracking hash embedded in
each `kmod-brcmfmac` package's `depends:` field: the SDK-built package's
`kernel=6.12.94~1110cdcc08e084d9841c1b3fdebb4940-r1` hash matched the
router's currently-installed package exactly; the `openwrt/`-tree build's
hash did not). Confirmed by direct comparison, not assumption. Fixed by
copying the patch into the SDK's own
`package/kernel/mac80211/patches/brcm/` and rebuilding there - this SDK
tree is the one already used all session for patches 862/863 and the
psta-probe modules, per that Makefile's own `SDK_ROOT` comment. Recovered
WiFi via the established revert-safe methodology (on-disk `.ko` swap +
reboot, backup kept alongside) both times this was hit - zero lasting
impact, confirmed via the router's own dmesg and `iw dev` state after
each recovery.

**First live test (SDK-built module, correct ABI): clean negative.**
`psta_trigger.ko` (new module, calls the exported
`brcmf_start_psta_repeater()` via `symbol_get()`, same pattern as
`psta_probe.c`) ran the full arm/DOWN/set/UP sequence with no errors, then
**timed out waiting for `BRCMF_E_IF_ADD`** (`BRCMF_VIF_EVENT_TIMEOUT` =
1.5s) - no new netdev, `iw dev` unchanged, STA connection intact
afterward.

**Second live test, ruling out the obvious confound: real reassociation,
wait widened to 8s, still a clean timeout.** The DOWN/UP cycle inside
`brcmf_start_psta_repeater()` necessarily drops the STA's live
association - the first test's 1.5s window couldn't possibly have given
a real WPA2 handshake time to complete, so a timeout there proved
nothing about whether an *associated* PSR companion bsscfg would ever
announce itself. Changed the wait to `msecs_to_jiffies(8000)` (one-line
patch edit, same SDK rebuild pipeline, re-verified export + kernel-ABI
hash before redeploying) and reran. dmesg confirms a real reassociation
actually completed inside the window (a second
`brcmf_inetaddr_changed` event ~4s after the disassociate, meaning
DHCP/IP configuration re-ran successfully) - and **still no
`BRCMF_E_IF_ADD`, still a clean timeout at the full 8s.** This is strong
evidence, not merely an unlucky timing window: even with the STA fully
reassociated and passing traffic while `psta=PSTA_MODE_REPEATER` is set,
the firmware never fires an interface-add event for any companion bsscfg.
The likely explanation has changed from "brcmfmac doesn't arm for the
event" (§21's hypothesis, now fixed by this patch) to **"this firmware's
PSR mode does not create a separate bsscfg at all - it operates inline
within the existing STA bsscfg."**

**Third test, the direct check of that inline-bridging hypothesis: also
blocked, one layer deeper.** If PSR reconfigures `phy1-sta0` itself into
4-address/WDS-style framing (the same mechanism Broadcom calls "wet" -
Wireless Ethernet Bridge - elsewhere in its own stack), the interface
should be bridgeable directly into `br-lan` with `psta` still active.
Confirmed via readback (`psta_readback.ko`) that `psta=2` was still
durably set from the prior test, then tried `brctl addif br-lan
phy1-sta0` live: **`brctl: bridge br-lan: Not supported`.** This is the
Linux bridge layer's own standard rejection of a non-4-address wireless
station netdev - independent confirmation of this project's earlier
finding (§19/§20) that brcmfmac never sets `WIPHY_FLAG_4ADDR_STATION` on
its STA interfaces. Whatever `psta` does at the firmware level, brcmfmac
never told cfg80211 this interface supports 4-address framing, so the
kernel refuses to bridge it - a second, independent driver gap, not a
firmware gap.

**Both plausible mechanisms by which `psta` could ever become a *usable*
Linux-visible feature are now confirmed blocked, at two different layers:**
a companion-interface event that the firmware never sends (tested twice,
including through a real completed reassociation), and 4-address bridging
that the driver never advertises to the kernel (tested directly, kernel
refuses outright). Fixing either would mean real, substantial brcmfmac
driver development - reverse-engineering what event type (if any) the
firmware actually does emit for PSR mode, or implementing full 4-address
frame handling in brcmfmac's netdev ops from scratch. Neither is a
config/iovar-level fix; both are genuine kernel driver projects.

**Architectural mismatch, independent of whether psta could be made to
work: it solves a different problem than this project has.** This
project's actual repeater need (documented in `wireless.example` and
`/etc/init.d/eufy-repeater`, confirmed via live `uci show wireless`) is a
**rebroadcast AP with the identical SSID** (`wireless.eufy_sta.ssid` and
`wireless.eufy_ap.ssid` are both literally `Eufy_B838D4`) - the classic
range-extender pattern, relaying between a STA uplink and a second AP so a
distant client (the pond-house camera) can roam onto the closer,
repeated copy of the same network. `psta`/`wet`-style bridging, even
working perfectly, does not create a second broadcast SSID at all - it
only makes the upstream network's traffic reachable from the router's own
wired LAN and AP-side devices (`br-lan` members). That helps a
LAN-attached client reach the Eufy network; it does nothing for a
WiFi-only, physically-distant camera that needs something to associate
to. **Even a fully-working psta implementation would not solve this
project's repeater problem** - the real, load-bearing blocker remains
exactly what §20 already concluded: same-radio concurrent AP+STA with a
working data plane, which this chip's firmware does not support
(`apsta` forced to 0, no RSDB/MCHAN).

Cleanup: reverted the router to the exact known-good v18 `brcmfmac.ko`
(md5-verified against the pre-existing backup) after each test - the
patched module is real, stable, and was live-verified not to regress
anything (all 3 radios, STA association, guest MBSS all functioned
identically throughout), but is kept as research (`patches/864-...`,
`hwoffload-research/psta-probe/psta_trigger.c`) rather than shipped, since
it adds no working feature yet. In-memory `psta` state resets to
`PSTA_MODE_DISABLED` on every reboot regardless (firmware default), so no
separate revert of that setting was needed.

**Bottom line:** eleventh and twelfth angles closed, both cleanly. `psta`
is real, firmware-accepted, and now has a correctly-built, ABI-verified
brcmfmac patch that arms for its companion interface exactly like
`brcmf_apsta_add_vif()`/`brcmf_p2p_add_vif()` do - and the firmware still
never uses that path. This is the strongest evidence yet that Broadcom's
PSR implementation on this exact chip/firmware genuinely does not expose
a separate Linux-manageable interface through any mechanism this project
has access to without disassembling and patching the firmware blob
itself (out of scope). Combined with the architectural mismatch above,
psta is not the path to a working repeater for this project even in
principle - future effort belongs back on §20's conclusively-identified
real blocker (same-radio AP+STA data plane) or on non-brcmfmac approaches
(a second physical radio via USB WiFi dongle, already flagged in §21 as
the remaining practical option).

## 23. Full "no mercy" break-dogma campaign - four parallel investigations,
MCHAN absence directly confirmed from the firmware itself, no thirteenth
angle survives (2026-07-26)

Explicit instruction to fan out and keep breaking dogma until a working
repeater exists. Four independent research threads plus one direct,
zero-risk live falsifier, run in parallel against every remaining
assumption from §20-§22.

**Thread 1 - does Netgear's own stock firmware (`dhd.ko`, proprietary
Broadcom driver, same chip) have a working repeater the open brcmfmac
driver just doesn't implement?** Read the actual extracted stock rootfs
(`hwoffload-research/blob-analysis/rootfs/`). The stock GUI ships exactly
two "extend the network" features, neither the mechanism this project
needs: **"Wireless Access Point Mode"** (`ap_mode.cgi`) is a *wired*-uplink
layer-2 bridge with no WiFi client role at all; **WDS bridging**
(`wds.cgi`) needs an explicit cooperating peer's MAC list - unusable
against an arbitrary third-party network. `dhd.ko`/`wl`/`wlconf` all carry
live, parsed `psta`/`apsta`/`wet`/`dwds` tokens (confirming, independently,
that these modes exist at the vendor-driver level on this exact chip) -
but Netgear **never shipped a GUI path to exercise them**, so no live
vendor-firmware test of same-radio AP+STA exists anywhere, including in
the vendor's own product. Most valuable independent confirmation: `dhd.ko`
and `wl` contain **zero** `rsdb`/`mchan` strings either - the hardware
lack is confirmed from a second, completely separate codebase, not just
brcmfmac's own assumption.

**Thread 2 - is RSDB/MCHAN absence a hard silicon fact, or an artifact of
brcmfmac's own probe logic?** Web research into Broadcom's own
architecture disclosures. RSDB absence is unambiguous and structural:
Broadcom's own 2015 launch material states "RSDB... requires dual MAC,
PHY, and radio hardware," and RSDB silicon (BCM4359/4366) launched a full
year *after* BCM43602 shipped - RSDB cannot apply to this chip by
definition, not by firmware choice. MCHAN's status was the one point the
research agent flagged as **genuinely unresolved from public sources** -
the driver treats it as a live firmware query (`feature.c`, substring
match against the firmware's own `cap` iovar string), not a hardcoded
chip-ID fact, and nobody has published that query's result for 43602
specifically.

**That gap was the cheapest falsifier available and was closed directly,
same session, zero risk:** wrote `cap_readback.ko` (read-only, same
`symbol_get()` pattern as `psta_readback.c`, calls the already-exported
`brcmf_fil_iovar_data_get()` for the `"cap"` iovar - no interface state
touched). Live result, this router's actual currently-running firmware,
not inferred:
```
cap="ap sta wet wet_tunnel led wme 802.11d 802.11h rm cqa cac mbss4
     ampdu ampdu_tx ampdu_rx amsdurx amsdutx rxchain_pwrsave
     radio_pwrsave bcm_dcs proptxstatus psta psr wds dwds
     traffic-mgmt traffic-mgmt-dwm p2po anqpo vht-prop-rates
     dfrts stbc-tx stbc-rx-1ss pspretend wnm bsstrans
     probresp_mac_filter mfp"
contains 'mchan'? no
contains 'rsdb'? no
contains 'p2p'?   YES
```
**MCHAN is genuinely absent from what this exact firmware advertises
about itself - confirmed directly, not inferred from a driver code path
or a chip family assumption.** This retires the one open question the
hardware-research thread flagged; there is no live-firmware ambiguity
left to resolve on RSDB/MCHAN.

**Thread 3 - the P2P-GO path re-examined at the source level (not just
its outer failure code).** Re-read `brcmf_p2p_set_firmware()` directly:
it forces `apsta=1` **unconditionally**, with no RSDB/MCHAN gate at all -
a structurally different code path from the AP-role function §20's
patches modified. The function proceeds past that apsta-forcing step
without incident and fails two calls later, on `p2p_da_override` (setting
the P2P Discovery interface's MAC address) - a real firmware rejection,
but the actual BCME error code is masked: `brcmf_fil_cmd_data()` only
returns the true firmware code when the caller sets `ifp->fwil_fwerr`,
which `p2p.c` never does here, so all we ever see is the generic `-EBADE`
(`-52`) stand-in. No missing driver-side "enable" step was found -
`brcmf_p2p_create_p2pdev()` doesn't even check `set_firmware()`'s return
value before continuing, so nothing is being skipped on our end. Checked
whether `BRCMF_FEAT_P2P` (a passive `cap`-string flag, confirmed present
above) gates this call: it does not - `brcmf_p2p_attach()` runs
unconditionally regardless of that flag. This point (the real BCME code
behind the P2P Discovery interface's address-override rejection) remains
formally unresolved without a debug-tracing patch - but is now known to
be immaterial regardless: P2P-GO creates a WiFi-Direct group-owner
interface, which negotiates via WiFi Direct's own discovery/provisioning
protocol, not plain WPA2-PSK association - a security camera has no way
to associate to a P2P-GO interface as an ordinary client even if creation
succeeded. This path was already ruled out by mechanism, independent of
its firmware-level fate.

**Also checked directly from source, definitively closing every
interface-type angle:** `NL80211_IFTYPE_MESH_POINT` is not implemented at
all in this driver (`-EOPNOTSUPP`, dead case label, never added to
`wiphy->interface_modes`) - not gated, simply absent. `NL80211_IFTYPE_ADHOC`
exists but only as a mode *change* on the single existing primary vif
(exclusive with AP, not concurrent) - creating it as a second, separate
vif hits the identical `-EOPNOTSUPP`. And `brcmf_setup_ifmodes()` (the
function that builds this chip's advertised `iface_combinations`) is
purely feature-flag-driven with no chip-ID branch: with this chip's real
flags (`mbss=1, rsdb=0, mchan=0`) the only combinations this driver will
ever advertise are STA-alone, STA+1-AP+P2P-types, or MBSS's AP-only(x4).
**No combination this driver can ever produce includes STA+2AP** - the
`err=-16`/EBUSY finding from §20 angle 7 is architectural, confirmed at
the code that generates the combination table itself, not a bug with a
workaround.

**Thread 4 - USB WiFi dongle, the one remaining practical path (needs new
hardware).** Confirmed no USB WiFi adapter is currently attached (only
the USB storage key from the monitoring-stack work). Chipset research,
evidence-graded: **MT7601U confirmed client-mode-only** in OpenWrt's own
package (no AP mode, ever). **RTL8188EU-family confirmed no AP mode**
(the chipset in most sub-$10 dongles - explicitly avoid). **MT7612U
confirmed solid AP-mode support** (dedicated community guides, multiple
working `hostapd` configs; its sibling MT7610U has open upstream AP-mode
bugs - chipset-specific, not family-wide). **AR9271 (`ath9k_htc`)
confirmed AP-capable** - native mainline `mac80211` driver, the most
historically dependable USB AP chipset, 2.4GHz/N150 only (sufficient for
a camera link). Recommendation: **Alfa AWUS036ACM (MT7612U)** as first
choice, a generic AR9271 dongle as the simpler/cheaper fallback. No
bcm53xx-specific report of this exact onboard-radio + USB-dongle
combination was found either way - the router's USB host controllers are
architecturally independent of the PCIe-attached BCM43602 radios, so no
conflict is expected, but this is engineering inference, not a documented
precedent.

**Verdict, thirteen angles now closed with zero survivors on the original
two-radio hardware:** every lever break-dogma discipline can name - the
vendor's own competing driver stack, the chip's own self-reported firmware
capabilities (verified live, not inferred), every alternate firmware
concurrency path (P2P-GO) and every alternate interface type (mesh,
IBSS), and the combination-table generation code itself - has been
checked directly against source or live hardware, not assumed. **Same-
radio AP+STA repeating is not an implementation gap; it is a firmware
capability this exact chip does not advertise, confirmed from its own
`cap` string.** The only path left that isn't re-litigating an already-
closed door is Thread 4: a second, independent physical radio via USB.
That step needs hardware this session doesn't have - a decision for the
operator, not a further software lever to pull.

## 24. Full peer-review round on the §19-23 impossibility claim - the
apsta methodology gap found, corrected, and stress-tested to convergence
(2026-07-26)

Ran a formal 5-reviewer blind peer-review round (soundness, prior-art/
provenance, reproducibility, significance, fatal-flaw lenses) against the
full §19-23 claim, followed by a rebuttal round with new live evidence,
re-scoring, and a second fatal-flaw pass. This section records what
changed as a result - not a repeat of §19-23's own content.

**The one real gap the round found:** patches 862/863 (§20) never
re-applied the same rigor §21/§22 eventually used for `psta` - a
DOWN/UP-bracketed set on the correct (primary/STA) ifp, applied before a
fresh reassociation rather than as a late toggle on an already-settled
radio. Patch 862 brackets DOWN/UP but on the wrong (AP-role) ifp after
the STA already associated under `apsta=0`; patch 863 uses the right ifp
but with no bracket at all. Both are the exact methodology bug that
originally gave `psta` a false negative before the DOWN/UP fix reversed
it.

**Closed directly, live, three times over, not argued:**

1. `apsta_probe.ko` - `BRCMF_C_DOWN` -> set `apsta=1` -> `BRCMF_C_UP` on
   `phy1-sta0`, matching `brcmf_p2p_set_firmware()`'s own precedent
   exactly, before any reassociation. All three firmware calls
   `err=0`; STA's RX/TX counters reset and climbed fresh, confirming a
   genuinely new association under `apsta=1`, not carried-over state
   from `apsta=0`. A real, independent Linux client (not the Eufy
   camera, a separate machine, -48dBm signal - rules out range) then
   tried to join `rpt_eufy_ap`: **`AUTH_TIMED_OUT`, four consecutive
   attempts** - the client's first 802.11 Authentication frame gets no
   response at all.
2. **Control** (peer-review demanded, since applying `apsta=1` correctly
   necessarily bounces the STA, and this project's own §19 already
   documents the co-resident AP vif as fragile): bounced `phy1-sta0` via
   `wpa_cli` with `apsta` held at 0 throughout, no probe module
   involved. Confirmed `hostapd`'s `rpt_eufy_ap` PID identical before and
   after (not restarted). Client join: **identical `AUTH_TIMED_OUT`.**
3. **Disambiguator:** re-armed `apsta=1`, waited 45 seconds of confirmed-
   stable association (no further bounce), then tried the client join
   well past any bounce-recency window. **Still identical
   `AUTH_TIMED_OUT`.**

Three conditions - `apsta=1` freshly bounced, `apsta=0` freshly bounced
(control), `apsta=1` settled 45s - converge on the exact same failure.
This is a stronger result than either the original §20 test or the first
corrected re-test alone: **`apsta`'s value is demonstrated irrelevant to
this specific failure, and it is not a bounce-recency artifact either.**
Real client authentication to the AP-role vif fails whenever a STA
association is active on this radio, independent of both variables the
review round correctly identified as unisolated in the original patches.
This is the same conclusion §20 already reached, now arrived at by
systematic elimination rather than a single test pair - the methodology
gap the review found real, and closing it made the claim stronger, not
weaker.

**Secondary probe run in the same round:** `dwds_probe.ko` (raw-iovar,
same DOWN/UP-bracketed technique) found `wds=1` rejected (`err=-52`) but
`dwds=1` accepted and durably retained (`err=0`, confirmed via readback)
- a new data point, parallel to `psta`'s own pattern. No new interface
appeared before or after. Joins `psta` as "firmware-accepted, not
Linux-exposed," not as a working mechanism.

**Other review-round corrections, evidence-backed:**
- Verified via multiple independent official Eufy support sources (not
  inference from the network's own band): both the HomeBase and
  standalone camera models are 2.4GHz-only, ecosystem-wide, vendor-
  stated. Closes the one significance-lens gap in §19-23's own
  architectural-mismatch argument.
- This router has exactly one 2.4GHz radio (`radio1`) - the other two
  are 5GHz-only by distinct RF front-end parts (Skyworks SKY85309-11 vs.
  SKY85710-11/SKY85712, per §1), not firmware-selectable. Combined with
  the point above: there is no second same-band radio to split STA/AP
  roles across on this hardware, regardless of driver capability -
  stated explicitly here rather than left for a reader to reconstruct.
- Added corroboration found during independent provenance re-search:
  DD-WRT's own wiki states categorically that "Broadcom dhd driver
  models... cannot support RB (nor Station Bridge) modes since the
  driver is controlled by wireless firmware internal to the chipset" -
  a competing firmware project's own maintainers naming the same
  limitation as a driver-family fact. A 2018 linux-wireless mailing-list
  post independently dumped BCM43602's `cap` string with the same MCHAN
  absence found live in E13 (§23), seven years apart - the same absence
  across firmware generations, not an artifact of the two vintages this
  project tested.
- Confirmed which firmware was loaded for §23's decisive `cap` string
  read: the 2021-era v18 firmware (536708 bytes, md5
  `6756c443973fb44169deb238a68ebb61`), the current, more-capable shipping
  firmware - not the older 2015 blob. Strengthens rather than weakens
  that finding.

**Honestly still open, not papered over:**
- `docs/RUNBOOK.md` does not yet have a standalone section documenting
  the actual patches-862/863/864 SDK build/deploy/revert workflow (SDK
  path, build command, `scp`/backup convention) - it exists only as
  prose narrated inside this file. Flagged, not fixed, this round.
- Every result in §19-24 comes from one author, one physical router
  unit, across a handful of days. No independent second run, second
  unit, or second reviewer has reproduced any of it - this stands at
  "available/functional" on the certification ladder, not "reproduced."
- The USB WiFi dongle path (MT7612U/AR9271, §23 Thread 4) remains a
  literature-based recommendation, not a validated fix. The practical
  problem - a working camera repeater - is still open pending that
  hardware; nothing in this section or §19-23 should be read as solving
  it.
- Unloading `apsta_probe.ko` (reverting `apsta` to 0) left the router
  briefly unreachable twice, both times recovered on its own via
  `radio-watchdog` within ~2 minutes. Recorded as a repeatable property
  of unwinding this specific DOWN/UP/iovar sequence, not investigated
  further since it doesn't bear on the core finding.

New probe source: `hwoffload-research/psta-probe/apsta_probe.c`,
`hwoffload-research/psta-probe/dwds_probe.c` (same `symbol_get()`
pattern as the existing probes in that directory).

## 25. The single-role baseline test - a real complication, reported
honestly rather than smoothed over (2026-07-26)

Two independent reviewers (fatal-flaw and soundness, in the same
peer-review round as §24), tracing the evidence trail rather than
re-litigating what was already closed, converged on the same next
question: **no test anywhere in this project - not §19, not §20, not
this round's three-condition convergence - has ever established what
happens when `phy1` has an AP-role vif and genuinely no STA vif at all**,
as opposed to a STA that is connected, bounced, or settled. Every prior
test varies STA *state*; none removes the STA *entirely*. That's the one
control that can distinguish "fails because a STA association is active"
from "fails always, for a reason unrelated to STA concurrency."

**Ran it.** Disabled `wireless.eufy_sta` in UCI (so `phy1-sta0` is never
created and `eufy-repeater`'s own `wait_for_sta_ifname()` - which
requires a connected STA before it will create the AP at all - never
fires), rebooted for a clean baseline, stopped `usteer`, and manually
replicated the AP-side half of `eufy-repeater`'s own documented recipe
(§19's `iw phy phy1 interface add rpt_isotest type __ap`, matching MAC
convention, `hostapd` invoked directly against `hostapd-isotest.conf`
with the same `ssid`/`wpa_passphrase`/`hw_mode`/`channel` as the real
`rpt_eufy_ap`). Confirmed `AP-ENABLED`, confirmed via `iw dev` that
`phy1` has exactly one vif (the AP) and nothing else. A real, independent
client (-48dBm signal) tried to join:

```
wlp2s0: SME: Trying to authenticate with ea:fc:af:f9:f1:99 (SSID='Eufy_B838D4' freq=2437 MHz)
wlp2s0: Event AUTH_TIMED_OUT (14) received
wlp2s0: SME: Authentication timed out
```

**Four consecutive attempts, identical result - with zero STA vif
present anywhere on the radio.** Repeated cleanly on a second fresh
reboot to rule out leftover state. The result held both times.

**This is a real complication, not a footnote, and it needs to be stated
plainly: the auth-timeout symptom used as evidence throughout §20's
"American network" test, and reproduced again in this round's §24
three-condition convergence, does not by itself demonstrate a
same-radio-concurrency limit.** It reproduces with no concurrency
present at all. Read plainly, the honest interpretation is one of:

(a) the minimal, standalone `iw phy ... interface add ... type __ap` +
    directly-invoked-`hostapd` harness (used for a quick validation check
    in §20 and reused here) has a bug, unrelated to STA concurrency,
    that prevents it from ever completing 802.11 authentication with a
    real client - independent of MCHAN, apsta, or anything else this
    document has tested; or
(b) something about *this specific test SSID/config* (shared with the
    real Eufy network's own credentials) fails regardless of harness.

**What this does NOT overturn:** §23's E13 finding (the firmware's own
`cap` string genuinely lacks `mchan`/`rsdb`) is a direct, unmediated
firmware self-report - it does not depend on hostapd, `iw`-created
interfaces, or any client authentication attempt, and stands entirely on
its own regardless of this section's finding. Sections §19's v13-v17
addenda separately and importantly documented that the **real production
mechanism** - `eufy-repeater`'s actual service, with `relayd -B -D` and
the STA-MAC-XOR-derived (not arbitrary) AP MAC address, run against real
Eufy camera hardware with a STA genuinely present and associated - shows
a *different* symptom entirely: clients complete authentication and
association, then disassociate in a fast ~4-second loop. That is a
materially different failure point than "authentication itself never
completes," observed under the full production setup this section's
isolation test did not replicate (no `relayd`, an arbitrary rather than
XOR-derived MAC).

**Honestly still open, the necessary next experiment:** does the *full*
production mechanism (real MAC derivation, `relayd -B -D` running, exact
`eufy-repeater` invocation) also fail identically - either symptom -
when tested with the STA genuinely absent, the way this section's
simplified harness was? That is the control that would cleanly settle
whether §20's causal attribution (same-radio STA+AP concurrency, gated by
MCHAN/apsta) actually explains the auth-timeout symptom, or whether the
auth-timeout symptom has always been a separate, harness-specific defect
riding alongside a real-but-differently-evidenced concurrency limit.
Not run this session - flagging it here rather than either quietly
patching over the discrepancy or claiming a resolution that hasn't been
earned.

**Addendum, same session - the cheapest reconciling check, run
immediately: is this a client-side artifact?** Before trusting any of the
above, checked the one thing that could invalidate all of it: can the
same test client (the separate Linux box used throughout this round, not
the router) actually complete a real authentication and 4-way handshake
against *anything* at all, right now? Pointed it at this router's own
real, working `R8000` main SSID (5GHz, `radio0`/`radio2`, confirmed daily
driver via normal use per `docs/WINS.md`) with the real PSK. Result:
```
wlp2s0: SME: Trying to authenticate with ea:fc:af:f9:f1:39 (SSID='R8000' freq=5745 MHz)
wlp2s0: WPA: RX message 1 of 4-Way Handshake from ea:fc:af:f9:f1:39 (ver=2)
wlp2s0: WPA: RX message 3 of 4-Way Handshake from ea:fc:af:f9:f1:39 (ver=2)
```
repeated identically against both real 5GHz BSSIDs on this router. Full
802.11 Authentication, Association, and EAPOL key-exchange initiation all
completed cleanly and repeatedly (the run never settled on one BSSID long
enough to finish the handshake and get an IP, because both radios
broadcast the identical `R8000` SSID and the client kept re-evaluating
which to roam to - a roaming-noise artifact of the test setup, not a
failure of anything being measured here). **This rules out the client
itself, its WiFi hardware, and its `wpa_supplicant` stack as an
explanation for any of this section's or this document's auth-timeout
results.** The client can and does complete real, working WPA2
authentication against this exact router, on this exact chip family (5GHz
BCM43602 siblings), today. Whatever produces `AUTH_TIMED_OUT` against the
raw-`iw`-created, STA-isolated `rpt_isotest`/`rpt_eufy_ap` mechanism is a
property of that specific AP-side mechanism (or this exact radio/
interface), not of the test client used to probe it.

**Second reconciling check, also run immediately: is the config
identical to production?** Diffed the live, currently-running
`/var/etc/hostapd-eufy_ap.conf` against this section's own
`hostapd-isotest.conf` - byte-for-byte identical apart from the
interface/`ctrl_interface` names. Rules out a config divergence as the
explanation; whatever differs between this section's isolation harness
and the real production repeater is not in the hostapd config.

**Third reconciling check, attempted, not completed - reported
honestly.** The one remaining candidate difference from the real
production mechanism is the AP's own MAC address (`eufy-repeater`
derives it from the STA's address via XOR; this section's harness used
an arbitrary address in the same format). Rebuilt the isolation test a
second time using the exact MAC the real `rpt_eufy_ap` uses in normal
operation (`ea:fc:af:f9:f1:37`), confirmed via `iw dev` before testing -
but the test client (a separate machine on this network, not the router)
became unreachable over SSH mid-test (ping continued to succeed
throughout - the machine itself was never down, only its SSH daemon
stopped answering within the connection timeout). Out of caution for a
machine this project treats as sensitive, further connection attempts
were stopped rather than repeated aggressively. **The MAC-address
hypothesis is therefore neither confirmed nor ruled out** - flagged
honestly as incomplete, not silently dropped or assumed either way.
`rpt_isotest` and the STA-disabled UCI state were reverted and the router
rebooted back to normal repeater operation regardless of this test's
outcome.

**Fourth reconciling check, identified by peer review, not yet run:** the
config diff above rules out hostapd *config content* as a variable, but
not the interface *creation method* - a raw `iw phy phy1 interface add
... type __ap` (used by `eufy-repeater` and by every isolation test in
this section) is a structurally different path into
`NL80211_CMD_NEW_INTERFACE`/`brcmf_cfg80211_start_ap()` than a fully
netifd/UCI-managed `wifi-iface` bring-up, regardless of what's in
`hostapd.conf`. A UCI-managed AP-only `wifi-iface` on `radio1` (STA still
disabled, so none of §20's netifd/whole-phy-teardown collision applies)
would test this directly, on the actual 2.4GHz radio this whole
investigation concerns - and would also close a narrower gap in the
client-elimination check above: that check used the router's 5GHz
`R8000` SSID, so it rules out the client's WiFi stack in general but not
specifically on 2.4GHz/`radio1`. Not run - the test client became
SSH-unreachable (ping-reachable throughout, consistent with the earlier
interruption) for an extended period, and this is treated as a genuine
anomaly on the operator's own machine rather than pushed through.
Flagged as the next concrete experiment once the test client is
confirmed recovered.

**Standing revision to this document's own confidence:** §20's "same-
radio AP+STA repeating is not achievable on this hardware/firmware
combination, full stop" should be read, after this section, as strongly
supported by the *independent* MCHAN/RSDB firmware evidence (§23) and by
external corroboration (§20 point 9, §23/§24's DD-WRT and vendor-driver
citations) - but the specific real-client auth-timeout test that has
been cited repeatedly across §20/§21/§22/§24 as direct behavioral proof
of that limit is now shown to reproduce with no STA-side concurrency
whatsoever, and should not continue to be cited as if it demonstrates
the concurrency claim specifically until the full-mechanism, STA-absent
control above is actually run.

**Formal peer-review closure (2026-07-26).** A 5-reviewer blind round
(soundness, prior-art, reproducibility, significance, fatal-flaw) plus a
meta-review area-chair decision were run on the full §19-25 claim, using
this project's own `scientific-method` peer-review discipline. Final
scores: soundness 4/5, prior-art 5/5, reproducibility 4/5, significance
3/5, fatal-flaw 2/5 (authoritative, held the line hardest). **Decision:
major-revision**, not accept and not reject. The meta-review's own
reasoning, preserved here for the ledger:

- **CONFIRMED, independent of any harness confound:** MCHAN/RSDB absence
  read directly off the firmware's own `cap` string (E13); the
  interface-combination table's architecture, read from
  `brcmf_setup_ifmodes()` source, purely feature-flag-driven with no
  chip-ID branch (E5); psta's companion `BRCMF_E_IF_ADD` event never
  firing, a driver-event test independent of any client-auth harness
  (E10); external corroboration from a named Cypress/Infineon engineer's
  own commit message and a competing firmware project's (DD-WRT) own
  documentation (E9, R8).
- **NOT YET CONFIRMED:** that the observed real-client `AUTH_TIMED_OUT`
  is caused by same-radio concurrency specifically, as opposed to an
  untested confound in the raw-`iw`-interface-creation-plus-hostapd
  harness used to generate that evidence.
- **Required before this moves to accept:** (1) the UCI/netifd-managed
  `wifi-iface`-on-`radio1`-with-STA-absent test identified above,
  (2) the full production `eufy-repeater` mechanism (real `relayd -B
  -D`, real MAC-XOR address) tested with the STA genuinely absent, to
  reconcile against §19's own differently-symptomed production result,
  (3) the interrupted MAC-address check from earlier in this section,
  re-run once the test client's SSH reachability is confirmed restored
  (not just ping), (4) the `docs/RUNBOOK.md` 862/863/864 workflow
  section (conceded open, unrelated to the causal question but required
  for third-party reproduction).
- **The USB WiFi dongle recommendation (MT7612U/AR9271, §23 Thread 4)
  stands as reasonable practical advice regardless of this open
  question** - it is justified by the confound-independent capability-
  absence evidence, not by the contested auth-timeout evidence, and
  should keep its existing "literature-based, untested on this hardware"
  label rather than being read as validated.
- **Dissent on record:** soundness/prior-art/reproducibility/significance
  all recommended minor-revision, judging the independent capability-
  absence evidence sufficient on its own narrower grounds. Overruled at
  the meta-review level because this document's own "claim under
  review" is a causal/mechanism claim, not merely a capability-absence
  claim, and that specific causal link has a named, cheap, still-open
  confound.

None of the required follow-up experiments were run this session: the
test client became SSH-unreachable for an extended period during the
exact window these tests were needed (ping-reachable throughout - the
machine itself, not down; only its SSH daemon stopped answering,
possibly disk-space exhaustion from this session's own repeated verbose
`wpa_supplicant -d` logging, worth checking first). Treated as a genuine
operational anomaly on a machine this project already handles with
documented caution, not pushed through. **This document's status is
therefore: capability-absence claim CONFIRMED and closed; causal
attribution of the specific auth-timeout symptom to same-radio
concurrency OPEN, with a concrete, cheap, fully-specified experiment
queued to resolve it as soon as the test client is available again.**

## 26. Invention campaign on the auth-timeout - a real, concrete,
already-deployed candidate fix found via source research, untested
against a live client (2026-07-26)

Ran a structured invention campaign (frame → ideate → refute → build/
measure → provenance-search) against the meta-review's #1 open question:
is the auth-timeout caused by same-radio concurrency, or by something
else entirely in the raw-`iw`-interface-creation/hostapd harness this
project uses? Full ideation record:
`/tmp/claude-1000/.../scratchpad/invent-repeater-fix.md` (not part of
this repo - session scratch).

**Ideated six candidate mechanisms** across dogma-breaking lenses
(borrowed-runtime: let hostapd create its own interface; asserted-limit:
disable HT to isolate a capability-negotiation mismatch; the meta-
review's own required UCI/netifd-managed test; a sequenced netifd
recombination; a missing `NL80211_CMD_START_AP` step; and a direct
router-side comparison of interface state).

**Research (Idea E) - hostapd's own upstream source, cloned and traced
directly (`w1.fi/hostap.git`, `driver_nl80211.c`):** the frame-
registration call chain
(`wpa_driver_nl80211_finish_drv_init()` → `nl80211_setup_ap()` →
`nl80211_mgmt_subscribe_ap()`, which issues the actual `NL80211_CMD_FRAME`
registration for Auth/Assoc/Disassoc/Deauth/Probe-Req) is **identical**
whether hostapd creates the interface itself or attaches to one created
externally - confirmed by reading the real source, not inferred. The
only confirmed difference (`NL80211_ATTR_IFACE_SOCKET_OWNER`) affects
interface lifecycle/cleanup, not frame reception. No hostapd
documentation requires self-creation either. **This means the interface-
creation-method hypothesis (the meta-review's own top-ranked open item)
is now less likely to be the actual fix than it looked** - hostapd's
code genuinely does not care. It reframes the likely bug location away
from hostapd and toward the driver/kernel layer.

**Direct router-side comparison (Idea F), no client needed for this
part:** compared `/sys/class/net/<if>/flags` between a real, working,
netifd-managed AP interface (`phy0-ap0`, real 5GHz clients connect
daily) and the raw-`iw`-created `rpt_eufy_ap`:
```
phy0-ap0    (working, br-lan member):  0x1303 = UP|BROADCAST|MULTICAST|PROMISC|ALLMULTI
rpt_eufy_ap (raw-iw, relayd-relayed):  0x1003 = UP|BROADCAST|MULTICAST                 (missing PROMISC|ALLMULTI)
```
Manually set `ip link set dev rpt_eufy_ap promisc on allmulticast on` -
**accepted cleanly, confirmed live via dmesg** ("entered promiscuous
mode", "entered allmulticast mode"), flags now read `0x1303`, matching
the working interface exactly. The likely explanation: `phy0-ap0` gets
these flags automatically as a side effect of `br-lan` bridge membership
(the kernel's own bridging code sets them on member ports); `rpt_eufy_ap`
is deliberately never a bridge member (it's relayed via userspace
`relayd`, not kernel bridging, per §19's own architecture), so it never
picks them up.

**Deployed as a real, persistent, respawn-safe fix - not yet confirmed
to solve the auth-timeout, but live and ready.** Patched
`etc/init.d/eufy-repeater`'s `recreate_iface` sequence to set these flags
unconditionally, matching the existing MAC-address fix's respawn-safety
pattern (every recreation, not just the first, gets the flags). Deployed,
rebooted, confirmed live: `rpt_eufy_ap` now reads `0x1303`, hostapd and
`relayd -B -D` both running normally against it.

**Not yet tested against a real client - the practical limit of this
session.** The test client became SSH-unreachable for the remainder of
the session (ping-reachable throughout, treated as a genuine anomaly on
a machine handled with documented caution, not pushed through). This is
therefore the concrete next step, ranked ahead of the meta-review's
originally-proposed interface-creation-method test given the hostapd-
source finding above: **the moment a real client is available, test
authentication against `rpt_eufy_ap` as it stands right now** (the
PROMISC/ALLMULTI fix is already live) before spending effort on the
creation-method variable, which hostapd's own source now suggests is
less likely to matter.

## 27. Real-client test with PROMISC/ALLMULTI live: zero associations
in seven minutes of uptime - and the confound that explains the earlier
"promising" DHCP signal was an SSID collision, not a repeater success
(2026-07-26)

Repointed `eufy_sta`/`eufy_ap` UCI to a real neighboring 2.4GHz network
(SSID "American", -59dBm, password provided directly by the operator)
so the repeater could be tested against real over-the-air traffic
without waiting on the Eufy camera or the SSH-unreachable test client.
The operator confirmed joining the repeated AP "a few times" with a
real device.

**Initial read looked promising, then fell apart under scrutiny.**
`logread` showed repeating `dnsmasq-dhcp[1]: DHCPDISCOVER(phy1-sta0)` /
`DHCPOFFER(phy1-sta0) 192.168.1.135` cycles for a real MAC
(`d0:50:99:f3:ee:19`) every few seconds. First reaction: a client's
broadcast reached our DHCP server through `relayd`, so something *did*
associate. Checked further before believing it.

**The disproof, in order:**
- `iw dev rpt_eufy_ap station dump` - empty, repeatedly, across a dozen
  checks spanning several minutes.
- `hostapd_cli -p /var/run/hostapd-eufy_ap status` - `num_sta[0]=0`,
  every check.
- `logread | grep -iE 'rpt_eufy_ap.*(assoc|auth|deauth|IEEE 802.11)'` -
  **zero results, for the entire router uptime (7 minutes, spanning the
  operator's whole test window).** hostapd logs association/auth events
  unconditionally; if a client had ever completed even a failed
  handshake attempt against this BSS, it would be here. It is not.
- The only real `Associated with ...` line in the whole boot log is our
  own `phy1-sta0` joining the upstream American AP - not a client of
  ours.
- `uci show dhcp` - no `interface=`/`except-interface=` restriction
  scopes dnsmasq away from `phy1-sta0`. `relayd -B -D -I rpt_eufy_ap -I
  phy1-sta0` bridges L2 broadcast between the two interfaces by design.
  So a real device's DHCP broadcast on the **actual, real** American
  network can reach our dnsmasq via the STA side, get an incompatible
  192.168.1.x offer it never uses, and repeat - independent of whether
  our own AP works at all. Confirmed this is still happening on its own,
  unprompted, well after the "test" window: same MAC, same
  DHCPDISCOVER/DHCPOFFER pattern, live during the SSID-rename retest
  below.

**Conclusion: the PROMISC/ALLMULTI fix from §26 is not confirmed to fix
anything. The DHCP signal that looked like success was unrelated
background noise from the real neighbor's own network leaking through
the STA-side relay.** Ruled out cheaply and quickly before writing this
up: channel mismatch (STA and AP both confirmed on channel 11/2462MHz -
a real risk on this MCHAN-less chip, but not what happened here) and a
tx-power anomaly (`rpt_eufy_ap` reads 20.00 dBm vs `phy0-ap0`'s 31.00 dBm
- explained entirely by 2.4GHz-channel-11 vs 5GHz-UNII-3 regulatory
limits, not a bug; not a fair same-band comparison).

**The likely real explanation, not yet disproven: an SSID collision.**
The repeated AP and the real neighbor's AP were both broadcasting the
identical SSID "American" with the identical password. A phone scanning
for "American" has no way to distinguish the two and may have joined the
real neighbor's AP directly - which perfectly explains all three
observations at once: zero hostapd events on our BSS (never touched),
the DHCP noise (the phone's real traffic on the real network, leaking
through our relay), and the operator's honest "yes I joined it a few
times" (they did - just not necessarily through us).

**Retest deployed to remove the ambiguity:** renamed the repeated AP's
broadcast SSID to `American-RPT-TEST` (UCI `wireless.eufy_ap.ssid`,
STA side unchanged, still associates to the real "American" upstream),
rebooted, confirmed live: `rpt_eufy_ap` broadcasting
`ssid=American-RPT-TEST` on channel 11, PROMISC/ALLMULTI flags still
`0x1303`, `relayd` running. A join to this exact name cannot be
confused with any other network. Real-time `logread -f` monitor
running, filtered to `rpt_eufy_ap|American-RPT-TEST` only (the known
`phy1-sta0` DHCP noise is explicitly excluded from the filter now that
it's understood). Awaiting a real join attempt against the unambiguous
SSID - this is the actual, first-ever unconfounded test of the
PROMISC/ALLMULTI fix.

**Correction: the SSID-rename above was a misdiagnosis, reverted.**
Checked project memory (total-recall) against the operator's actual,
original repeater spec: every prior config, from the very first working
version through today, has the repeater's AP side use the **identical**
SSID (and password) as the target network -
`wireless.eufy_ap.ssid='Eufy_B838D4'` matching `eufy_sta.ssid`, likewise
`'American'` matching `'American'` before this session's rename. This is
correct, intentional, and how every real-world repeater/repeater-bridge
(DD-WRT, Tomato, stock consumer firmware) works: "setting the SSID,
channel, encryption and password to match the primary router" is the
standard, not a bug to fix. **Reverted `wireless.eufy_ap.ssid` back to
`American`, matching the STA side, as it always should be.**

This means the "client might have joined the real neighbor's AP instead
of ours" framing was directionally real (a repeater's whole point is
that either BSSID works, so a client preferring the real upstream is
not a failure) but it does **not** explain away the actual open
question: hostapd on `rpt_eufy_ap` has still never logged a single
`IEEE 802.11: associated` event, under any SSID, across this entire
session's testing. That result stands, uncorrected. The only honest
way to test it unambiguously - given the operator's own real devices
are always in range of the real upstream and will happily roam to the
stronger/known BSSID - is a client with no path to the real upstream at
all, or a client pinned to this AP's specific BSSID
(`ea:fc:af:f9:f1:37`), matching this project's own established
wpa_supplicant `bssid=` pinning technique from earlier probe testing.
`wildnuc`, the test client used for that technique, is as of this
writing fully unreachable (escalated from SSH-down-but-ping-alive to
100% ping loss) - this is now the hard blocker on a decisive real-client
test, not architecture.

## 28. BSSID-cloning: repeater now shares the real AP's identity, not a
derived-unique one (2026-07-26)

Corrected a real design mistake, not just the SSID one: `eufy-repeater`
derived a MAC for `rpt_eufy_ap` by XORing the locally-administered bit
onto the router's own STA MAC - deliberately making the repeated AP a
DIFFERENT BSSID from the network it repeats. A real repeater shares the
upstream AP's identity (SSID, and per DD-WRT's own wiki on this exact
subject: "setting the SSID, channel, encryption and password to match
the primary router"). Patched `start_ap_relay()` to read the STA's
actual `Connected to <bssid>` and use that as `rpt_eufy_ap`'s own MAC
instead, falling back to the old derived-unique scheme only if the
upstream BSSID can't be read.

Deployed against the real, operator-owned Eufy_B838D4 target (not the
neighbor's "American" network - cloning a THIRD PARTY's own hardware
MAC on their live network is a real risk to their service that this
project has no business taking; cloning the operator's own Eufy AP's
BSSID carries no such risk). Confirmed live after a clean reboot:
`rpt_eufy_ap addr` = `04:17:b6:b8:38:d4`, identical to
`phy1-sta0`'s `Connected to 04:17:b6:b8:38:d4`, matching SSID/channel
(`Eufy_B838D4`, channel 6/2437MHz), PROMISC/ALLMULTI flags intact
(`0x1303`), `relayd -B -D` running. `num_sta[0]=0` still, as of this
writing - no real client tested against it yet this round.

## 29. DWDS research: a real, confirmed, previously-unaddressed gap in
brcmfmac - and an equally real limit on what it can fix here
(2026-07-26)

The operator asked for actual kernel-level Dynamic WDS (4-address mode)
support, upstream-quality, not another same-BSSID workaround. Extracted
this router's own exact driver source (`backports-6.18.26`, the tarball
this SDK's `kmod-brcmfmac` is actually built from - not upstream
mainline's copy, which doesn't carry this driver at all; OpenWrt pulls
wireless drivers from the `linux-wireless-backports` project via the
`base` feed) and read it directly rather than guessing:

- `brcmf_cfg80211_add_iface()`/`change_iface()`/`del_iface()` all
  unconditionally reject `NL80211_IFTYPE_WDS` with `-EOPNOTSUPP` - no
  per-chip branch, no partial support anywhere.
- `fwil_types.h` already defines `BRCMF_STA_WDS`, `BRCMF_STA_WDS_LINKUP`,
  `BRCMF_STA_DWDS_CAP`, `BRCMF_STA_DWDS` - meaning the firmware genuinely
  reports per-station WDS/DWDS state - but grepped the entire driver:
  **these bits are read nowhere.** Dead defines, confirmed by grep, not
  inference.
- `.set_wds_peer` (the classic cfg80211 hook for this) is implemented by
  **zero** drivers anywhere in this 2.2M-line backports tree - a genuine
  negative result from grepping the whole tree, not just brcmfmac.
  `use_4addr`/`NL80211_ATTR_4ADDR` is a real, standard `vif_params`
  field, but brcmfmac's `change_iface()` never reads `params->` at all
  (confirmed: zero references in the whole function body).
- Checked for prior art before writing anything (world-first/provenance
  discipline): no existing OpenWrt patch anywhere in
  `package/kernel/mac80211/patches/brcm/` touches wds/dwds. This is a
  real, previously-unaddressed gap, not a rediscovery.

**The complicating finding, arrived at by actually understanding the
mechanism rather than assuming it fits the ask:** DWDS is a
Broadcom-proprietary *backhaul* negotiation - it lets the STA-side
uplink to another Broadcom/wl-compatible AP carry 4-address frames
(real client MACs preserved end-to-end, no NAT/relayd needed), the same
role FreshTomato's own proprietary-`wl`-driver APSTA+DWDS repeater uses
it for (this project's own earlier research, §23-26). It is **not** a
mechanism an ordinary client (a Eufy camera, a phone) can negotiate or
benefit from on the repeater's downstream AP side - those are plain
802.11 STAs with zero DWDS awareness, and would set `BRCMF_STA_DWDS_CAP`
never, regardless of any driver change. **Wiring this up, however
correctly, does not fix the Eufy/neighbor-network auth-timeout this
project has been chasing since §19** - that remains blocked on the
separate, already-confirmed same-radio-combo limitation (E5/E10/E22/E13,
§24-25) and, right now, on `wildnuc` being unreachable for a clean
unambiguous test.

**Built anyway - real, scoped, honest engineering, not a dead end.**
`patches/865-brcmfmac-r8000-dwds-repeater-capability.patch`:
1. `brcmf_cfg80211_start_ap()`: advertise DWDS capability on an AP-role
   bsscfg via `brcmf_fil_bsscfg_int_set(ifp, "dwds", 1)` (the same
   per-bsscfg helper already used for wpa_auth/wsec/mfp), non-fatal if
   rejected, matching the iovar this project's own `dwds_probe.c`
   already confirmed accepted/durable on this exact chip. Advertisement
   only - does not force anything on non-DWDS clients.
2. `brcmf_cfg80211_get_station()`: read and log
   `BRCMF_STA_DWDS_CAP`/`BRCMF_STA_DWDS` out of the `sta_info_le` flags
   word already being decoded there. No existing generic nl80211
   station-info field for this exists to map onto (checked: no
   `NL80211_STA_FLAG_4ADDR_MODE` or equivalent anywhere in the tree) -
   inventing a UAPI surface unilaterally in an out-of-tree patch would
   be the wrong call, so this is deliberately log-only: real,
   observable, honest, not overclaimed.

Compile-tested for real against this exact SDK (`openwrt-sdk-25.12.5-
bcm53xx-generic`, `backports-6.18.26`) - applies cleanly on top of
861-864 (both hunks, small line-offset only, zero fuzz-3/reject), full
`make package/kernel/mac80211/compile` run to confirm it actually
builds, not just applies. **Explicitly NOT claimed:** functional
verification of a real negotiated DWDS link, which needs a second
Broadcom/DWDS-capable peer this environment does not have and the
actual target devices (Eufy, phones) could never provide regardless.

**Deployed live and verified, not just compile-tested.** Extracted the
real, stripped `brcmfmac.ko` from the built `.apk` (285772 bytes,
`dwds` string confirmed present via `strings` - the true 861-864-only
baseline, pulled independently from ImageBuilder's package cache for
comparison, is 283240 bytes/hash `82dc952c...` with no `dwds` string at
all, so this is a real, confirmed-different binary, not a no-op
rebuild). Pushed to `/lib/modules/6.12.94/brcmfmac.ko`, rebooted clean.
Post-boot verification: `lsmod` shows the new module loaded (hash
matches); all four WiFi interfaces up (`phy0-ap0`/`phy2-ap0` real APs,
`rpt_eufy_ap` repeater, `phy1-sta0` STA); BSSID clone from §28 still
correct (`04:17:b6:b8:38:d4` on both sides); PROMISC/ALLMULTI flags
still `0x1303`; `relayd` running; zero new dmesg errors beyond the
already-known, already-explained ones (firmware blob-variant probe
misses matching earlier sessions' findings, the legacy-MBSS-fallback
`err=-52` path patch 861 already works around). A genuine safety net
was also staged on the router
(`/tmp/brcmfmac-TRUEORIG-861-864.ko`, hash-verified against the
independently-pulled original) in case of regression - none occurred.
Certification ladder position: `functional`, now with a real
non-regression deployment behind it, not just a clean compile - still
not `reproduced`/`certified` for the actual DWDS negotiation itself,
honestly, since that needs a peer this environment cannot provide.

## 30. "Extender" LuCI page - reusing stock Join Network, adding only
the part that's actually missing, verified live in a real browser
(2026-07-26)

The operator's actual ask, corrected mid-session: not a camera-specific
or phone-specific feature - a general "repeat any network" UI, GL.iNet-
style, and pointedly: "doesn't openwrt have this already obviously."
It does, mostly. Checked before building anything (break-dogma, not
assumed): stock LuCI's `network/wireless.js` already has a full
Scan → Join Network flow (`handleScan`/`handleJoin`/`handleJoinConfirm`),
mature, well-tested, creating a STA-mode `wifi-iface` on submit. That
part was NOT rebuilt.

**What's actually missing, confirmed by reading that flow line by
line:** `handleJoinConfirm` only ever creates the STA section - nothing
in stock LuCI creates the AP-side repeat companion this project's
`eufy-repeater` architecture needs (BSSID-cloned per §28, PROMISC/
ALLMULTI per §26, relayd-bridged per §15/§19), and nothing checks the
other band.

**Built exactly that gap, nothing more,** as a new LuCI page (`Network
› Extender`), this router's LuCI being the modern ucode/JS framework
(`luci-26.205`, no classic Lua controllers on this build - confirmed by
checking, not assumed, `lua` isn't even present as a binary here):
- `usr/share/luci/menu.d/luci-app-extender.json` - menu entry
- `usr/share/rpcd/acl.d/luci-app-extender.json` - ACL (wireless/network/
  firewall UCI + `system.reboot` ubus, matching the reboot-not-restart
  design below)
- `www/luci-static/resources/view/extender.js` - the actual page: lists
  every `mode=sta` wifi-iface (from stock Join Network OR anywhere
  else - it doesn't care how the STA got there), and for each, either
  shows "Repeating" if a matching AP companion
  (`mode=ap`+`repeater_mode=1`, same device+ssid) already exists, or an
  "Enable Repeating" button. Clicking it creates that companion
  (device/ssid/encryption/key copied straight from the STA section -
  same identity, per the operator's own "just repeats" correction in
  §28), then scans the OTHER band's radio (`radio1`↔`radio2`, the same
  pairing established in §28's rationale) for the identical SSID - if
  found, provisions a second STA+AP pair there too. This is the
  operator's "auto repeat 2.4 and 5ghz... if found" request, done as a
  one-click extension of the per-band mechanism already proven working,
  not a special case.

**Deliberately does NOT auto-restart on save.** The generic LuCI
pattern (a `ucitrack` entry "affecting" a service, like firewall's own)
would restart `eufy-repeater` on every wireless save - but this
project's own confirmed, repeated failure mode is `iw dev <if> del`
wedging (`err=-52`) on an already-in-use interface, with a clean reboot
as the only reliable recovery found all project (§ multiple). Silently
auto-restarting into that wedge would be worse than asking. The page
saves UCI only and surfaces an explicit "Reboot Now" button instead -
slower, but it's the operation actually verified reliable here, not a
guess dressed as convenience.

**Verified live, in a real browser (Playwright), not just deployed:**
logged into this router's real LuCI, navigated to the new page -
correctly listed the real `Eufy_B838D4` STA entry and correctly showed
"Repeating" (its real AP companion, confirmed present, correctly
matched). Added a throwaway fake STA section
(`UITEST-FAKE-NETWORK-XYZ`, radio1, never able to associate - chosen
specifically so nothing real could be affected) to exercise the
untested branch: clicked "Enable Repeating", confirmed the success
notification and "Unsaved Changes" staging, then "Save & Apply", then
confirmed via SSH that `wireless.ext0_ap` landed on disk with every
field correct (device/mode/ssid/encryption/key/disabled/repeater_mode).
Cleaned up the test section immediately after (`uitest_sta`/`ext0_ap`
both removed, confirmed zero remaining). Zero new console errors (the
one pre-existing `protocol/relay.js` 404 is stock LuCI probing an
uninstalled proto handler, unrelated to this page, present before this
change too).

## 31. Naming cleanup + a real app-plus pass on the Extender feature -
two genuine bugs found and fixed by actually testing it like a human,
not just reading the diff (2026-07-26)

**Renamed everything eufy-specific to generic**, per the operator's own
correction: "eufy" was only ever the name of the first real target this
project tested against, never a property of the mechanism. Renamed
live on the router (verified via reboot, zero regressions) and in
`v2-files`: `eufy_sta`/`eufy_ap`/`eufy_wwan`/`eufy` (firewall zone) →
`ext0_sta`/`ext0_ap`/`ext0_wwan`/`ext0`, matching the `extN` convention
the Extender LuCI page already used for anything added through the UI.
`etc/init.d/eufy-repeater` → `etc/init.d/extender` (rc.d symlinks
recreated, old script removed). `wireless.example` updated to point at
the LuCI page as the easiest path, not just hand-editing.

**Ran the edgar-morin reasoning loop the app-plus method actually calls
for** (not just informal code reading) against `extender.js` -
`capture_user_intent` → `reason()` through the real tensions →
dialectical-drift warning correctly fired after too many one-sided
thoughts → a genuine counter-hypothesis registered → decided on the
merits. Found and fixed: `handleRemoveRepeat`'s firewall-zone deletion
matched by loose containment (`zone.network` includes this network)
rather than exclusive ownership - narrow in practice (a dual-band
network's synthetic name can't pre-date the same save transaction that
creates it) but a delete path shouldn't rely on "can't happen" alone.
Tightened to require the zone have exactly one network member before
removal. Two other candidates (zone-naming edge case for non-`_wwan`
network names; per-render live ubus round-trips) assessed as genuinely
low-priority at this feature's real scale and deliberately left alone -
best judgment, not maximal change.

**Then tested it like a human would, in a real browser again - and
that's what actually found the significant bug**, not the reasoning
pass: the live-status feature (§30) showed "Configured - reboot to
activate" for the real, already-running `Eufy_B838D4`/`ext0_ap`
repeater, when it was genuinely up (confirmed independently via SSH:
`hostapd_cli status` showed `state=ENABLED`, BSSID cloned, PROMISC set,
`relayd` running). Root cause: `network.getWifiNetwork().isUp()`
reflects **netifd's** view of wireless interfaces - and the entire
reason this repeater architecture exists (§15) is that its AP side is
deliberately created OUTSIDE netifd's awareness (raw `iw` + hostapd,
`disabled='1'` so netifd never touches it). The same architectural
choice that makes the repeater work at all made the natural LuCI status
API blind to it. Fixed by asking the one thing that actually knows -
`hostapd_cli -p /var/run/hostapd-<section> status` via `fs.exec()` -
parsing `state=ENABLED` and `num_sta[0]` directly. Required a new ACL
grant (`ubus.file.exec` + a specific `/usr/sbin/hostapd_cli` allowlist
entry, matching the exact scoping pattern `luci-mod-network`'s own ACL
uses for the same kind of call). Re-verified live: now correctly shows
"Repeating (0 clients)" for the real target.

**Full human-style pass, end to end, in a real logged-in browser
session**, not just unit-level checks: reloaded after the rc.d
rename+reboot, confirmed the real repeater's row and the renamed
interfaces; added a throwaway STA (`UITEST-FAKE2`), clicked "Enable
Repeating", confirmed the `Unsaved Changes` counter, "Save & Apply"d,
confirmed on the router via SSH that `network.uitest_wwan`
(defaultroute=0/peerdns=0) and its firewall zone were both created
correctly - the exact bug fixed in §30 - not a re-assertion, a fresh
confirmation against the post-rename code. Clicked "Remove", confirmed
the browser `confirm()` dialog, accepted, "Save & Apply"d again,
confirmed via SSH that the AP section was fully gone and the
independently-joined STA correctly survived (the remove guard's
`base+'_sta' === staSection.name` check working as designed). Cleaned
up every test artifact afterward, confirmed zero residue.

## 32. R8000 (radio0, ch149) invisible over the air - ruled out every
software/firmware layer with real evidence, converges on radio0's own
dedicated antenna hardware (2026-07-26)

radio0's own main SSID ("R8000") could not be seen in a real scan from
an independently-driven, rooted Android test device placed directly
next to the router, on 5745MHz (channel 149) - despite every
software-layer signal reporting fully healthy:

- `hostapd_cli status`: `state=ENABLED`, correct BSSID/SSID, `ieee80211ac=1`,
  correct VHT config, `max_txpower=30`, `num_sta[0]=0`.
- `ip -s link show phy0-ap0`: TX packet counter incrementing at ~beacon-
  interval rate (100ms beacon_int) between two reads 3s apart - real
  beacon frames are genuinely being generated and handed to the driver.
- dmesg: `phy0-ap0: renamed from wlan0` present - the tell for a real,
  firmware-backed AP bsscfg (not the fake MBSS-fallback netdev this
  project's earlier root-cause work found for the retired Extender raw
  APs).
- Built and deployed patch 866 (implements cfg80211 get_antenna/
  set_antenna - upstream brcmfmac has never wired these up, so
  `iw phy info` always shows "Available Antennas: TX 0 RX 0" on every
  radio regardless of real chain state). Real result: radio0 reports
  `Available/Configured Antennas: TX 0x7 RX 0x7` - full 3-chain, byte-
  identical to the two STA uplinks (radio1/radio2) that are independently
  proven working end-to-end (American extend, 0% packet loss to 8.8.8.8).
  Chain-mask/firmware misconfiguration is ruled out with hard data, not
  assumption.
- Re-checked `v2-staging/maxpower/notes.md` §11 cross-reference: the only
  prior "radio0 AP came up" observation in this project's history
  (`verify/before-after.md` Attempt 4, channel 153) was ALSO hostapd/
  `iw dev` status only - no independent over-the-air confirmation was
  ever done for radio0 before tonight. The "31.00 dBm" reading tonight
  is not new or suspicious; it's the same value Attempt 4 saw, and both
  are just the calibration-decoded PA ceiling being reported, not a live
  RF measurement.
- Re-scanned 5745MHz directly: the Android device clearly sees a
  neighbor's real AP (SSID "STARLINK", -69dBm) plus one hidden-SSID
  BSS, both on the exact same frequency/channel R8000 uses - proving the
  scan methodology itself is sound and channel 149 is not somehow
  radio-silent everywhere. R8000/`ea:fc:af:f9:f1:39` simply never
  appears.
- Cross-referenced this device's actual FCC teardown
  (`v2-staging/fccid/specs.md` §3): radio0 (UNII-3, ch149-165) and radio2
  (UNII-1, ch36-144) are on physically separate BCM43602 modules with
  **different antenna chains** - radio2 shares chains 1-3/antennas 1-3
  with the 2.4GHz radio; radio0 has **dedicated chains 4-6, dipole
  antennas, shared with nothing else on this board**. This rules out the
  tempting shortcut of treating radio2's (or radio1's) proven-working RF
  path as evidence for radio0's - they don't share hardware. radio0's
  antenna path has never been independently verified with a real
  receiver, by this project or apparently at any point in this router's
  documented history, until tonight's scan came back silent.

**Standing conclusion (superseded below):** every driver/firmware/
mac80211/hostapd signal this project can inspect remotely says radio0's
AP is healthy. The one layer that can't be checked over SSH - the
physical dedicated dipole antenna path for chains 4-6 - was the leading
suspect. That suspicion is now narrowed further, see next entry.

**Update, same session: radio0's antenna is NOT disconnected - RX proven
working.** Disabled `main_radio0` (AP) and brought up a solo STA vif on
radio0 alone (avoiding the interface_create AP+STA-concurrent limitation
by never having two vifs on the same radio at once). `iw dev phy3-sta0
scan freq 5745` heard the same neighbor ("STARLINK", `1a:f1:ac:3e:af:85`)
at -58 to -60dBm across repeated scans, matching the Android device's
independent -69dBm reading (different receiver, different location in
the room, same physical signal - directionally consistent). **This rules
out a disconnected or physically dead antenna on radio0.** The dedicated
dipole antenna path for chains 4-6 is receiving real RF at a healthy
signal level.

Attempted to isolate TX specifically:
- TX packet counter showed zero change across the scan. Ran the
  identical before/after check on `phy4-sta0` (2.4GHz, proven working,
  real ping traffic flowing) as a control - it ALSO showed zero TX
  change during its own scan. This means brcmfmac's scan is firmware-
  internal (probe requests never touch the host netdev TX path) on this
  driver - the test is not diagnostic either way, and the earlier
  reasoning from it is retracted.
- Attempted a direct `iw dev phy3-sta0 connect STARLINK` to force a real
  auth/assoc frame exchange over the host TX path (regardless of
  whether the handshake would ultimately succeed without the PSK) - the
  command failed on tooling (no `timeout` binary in this busybox), never
  actually ran.
- Every AP<->STA role switch on radio0 this session (including this one)
  has landed hostapd in the same wedged state
  (`v2-staging/maxpower/notes.md`'s documented "Known-bad sequence") -
  repeating "hostapd: Failed to set beacon parameters" that only a full
  reboot clears, not a live `wifi`/`wifi reload`. Confirmed again this
  round; full reboot restored R8000 + both American uplinks cleanly
  (0% loss re-verified on the 5GHz uplink post-reboot).

**Given that live-instability cost is real and repeats every time,
stopped further live role-switching on radio0 rather than chase one more
TX data point.** Standing conclusion updated: radio0's antenna/RX chain
is proven physically intact. The fault is narrowed to radio0's transmit
path specifically (AP-mode beacon frames are handed to the driver at the
correct rate, per §32 above, but no external receiver has ever heard
them) - still unconfirmed whether that's a firmware/calibration issue or
a genuine PA hardware fault on this one radio's dedicated TX chain.
Testing STA-mode TX cleanly (associating to a network the operator
controls, with a known password, to get an unambiguous data-plane TX
counter increment) is the next concrete lever, not another blind
disable/re-enable cycle against a stranger's AP.

**Follow-up, same session: built real firmware-level TX/RX counter
access (patch 867), got a genuinely new data point, hit a genuine
firmware ABI limit before it could fully settle the question.**

`ethtool -S`/`-i` confirmed `supports-statistics: no` - not wired up.
`iw dev ... survey dump` returns nothing - `.dump_survey` isn't
implemented in this driver either (confirmed absent from
`cfg80211_ops`). brcmfmac's own debugfs "counters" entry exists but only
for the SDIO bus (bus-transport interrupt/glom stats, not firmware MAC
counters) - nothing exposes the actual `"counters"` iovar (Broadcom's
long-standing `wl_cnt_ver_*_t` MAC-layer TX/RX statistics ABI) on any
bus. Also confirmed: the ENTIRE brcmfmac debugfs subsystem is gated
`#ifdef DEBUG`/`CPTCFG_BRCMDBG`, and this build has it off (matches §17's
"neither CONFIG_BRCMDBG nor CONFIG_BRCM_TRACING is set" finding, just
from the other direction).

Built patch 867: adds a bus-independent debugfs "counters" file
(registered at the same generic point as the existing "revinfo" entry,
so PCIe/SDIO/USB alike get it) that queries the `"counters"` iovar
directly and dumps version+length+raw hex - deliberately not guessing at
a specific version's field layout in-kernel, since the per-field offsets
vary across `wl_cnt_ver` revisions. Enabled the existing, upstream-
supported `CONFIG_PACKAGE_BRCM80211_DEBUG` Kconfig option (maps to
`CPTCFG_BRCMDBG`) to unlock the whole debugfs subsystem this driver
already ships but disables by default - not a novel debug mechanism.

Rebuild/deploy hit one real snag: the debug build's `brcmfmac.ko` needs
a MATCHING debug-built `brcmutil.ko` (`brcmu_dbg_hex_dump` is only
exported when `CPTCFG_BRCMDBG` is set) - deploying just the new
`brcmfmac.ko` against the stale `brcmutil.ko` failed with `Unknown
symbol brcmu_dbg_hex_dump`, taking all 3 radios down until both modules
were swapped together as a matched pair. Recovered cleanly (backup
`.ko` restored, full WiFi back within under a minute, zero lasting
damage) - documented here so it's not repeated. `build-image.sh` updated
accordingly (defconfig runs twice: once to generate the .config a
pristine SDK tarball doesn't ship at all, then again after flipping
`CONFIG_PACKAGE_BRCM80211_DEBUG=y`, with a hard `FATAL` check that it
stuck - a from-scratch build that silently reverted to the non-debug
default would silently ship a mismatched module pair again).

Read `/sys/kernel/debug/ieee80211/phy<N>/counters` for all three radios
(fresh module load, `version: 10, length: 848` on every radio - the
firmware's own reported struct size). Verified the field layout against
Broadcom's actual `wlioctl.h` (`wl_cnt_ver_11_t`, fetched via `gh search
code` + `gh api` against real driver source trees - not recalled from
memory) - the leading "transmit stat counters"/"receive stat counters"
prefix has been stable across `wl_cnt_ver_6_t` through `_11_t`, so it
reliably describes this firmware's smaller version-10, 848-byte blob
too. Result, comparing radio0 (R8000 AP) against both proven-working STA
uplinks:

| counter (offset) | radio0 (R8000 AP) | radio1 (2.4GHz STA, working) | radio2 (5GHz STA, working) |
|---|---|---|---|
| txframe (+4) | **0** | 84 | 4027 |
| txctl (+20) | 1 | 102 | 29 |
| txnoassoc (+36) | 138 (varied to 2169 on re-read) | 0 | 0 |
| rxctl (+76) | 8 | 4 | 0 |

`txframe` (real 802.11 DATA frames sent) is genuinely 0 for radio0 -
but this is fully explained by zero clients ever connecting (there is no
data traffic for a clientless AP to send, independent of whether its
beacons are radiating). The one counter that would actually settle the
beacon question - `txbcnfrm`, "beacons transmitted" - lives in the later
"MAC counters: 32-bit version of d11.h's macstat_t" section of the full
struct, well past byte 848 in every reference copy checked. **This
firmware's version-10 counters format does not include it at all** -
a genuine firmware ABI limit, not something patch 867 can work around
by reading harder. `txnoassoc` (138, climbing) is the one odd, unique-
to-radio0 signal (STA radios show 0) but its exact meaning for an
AP-role vif with zero associated stations is not established with
confidence - flagged, not overclaimed as proof of anything.

**Standing conclusion, unchanged in substance, now backed by one more
independently-verified data point:** radio0's antenna/RX is proven
physically intact (previous entry); its firmware-level TX/RX MAC
counters are consistent with "healthy but clientless," not clearly
diagnostic of the beacon-visibility question either way, because the
one counter that would answer it directly isn't present in this
firmware's counter ABI version. Real, working new diagnostic tooling
(patch 867) now exists in the repo for whoever picks this up next, but
this specific thread has reached the limit of what firmware
introspection alone can resolve.

## 33. radio0 TX/RX DEFINITIVELY PROVEN WORKING via a real WPA2 handshake
- the AP-mode beacon problem is isolated to AP mode specifically
(2026-07-26)

The clean test the previous two entries were building toward: a real,
independently-controlled AP on radio0's own actual channel range, with a
known password, so a genuine data-plane 802.11 exchange (not a scan, not
a firmware counter) could settle the question.

**Test rig:** the same rooted Android test device used all session (an
HTC 5G Hub, not a phone) has its own `phy1` capable of 5745-5825MHz
(149-165) AND a real `hostapd` binary (`/vendor/bin/hw/hostapd`) at
`/vendor/etc/init/hostapd.android.rc`. Its interface-combination rules
(`iw list`) allow concurrent managed+AP on one phy
(`#{managed}<=2, #{AP}<=2, ... STA/AP BI must match`), so a second vif
(`hostapd0`) was added on `phy1` without needing to disrupt anything
system-owned. Ran a plain, standalone `hostapd -dd` against a minimal
conf (`ssid=R8KTXTEST`, `channel=149`, `hw_mode=a`, `wpa=2`,
`wpa_passphrase=<own value>`) - entirely outside Android's own tethering
stack, so nothing about the device's normal function was touched.

**Two false starts, both correctly diagnosed before drawing any
conclusion from them (same discipline as §32's retracted scan-counter
test):**
- First two attempts: hostapd's own log showed `IEEE 802.11 driver had
  channel switch: freq=5220 ... AP-CSA-FINISHED freq=5220` - the phone's
  driver was silently following `wlan0`'s existing "American" STA
  connection's channel (5220MHz/ch44) instead of honoring the requested
  channel=149, because both vifs share one phy. 5220MHz is outside
  radio0's DT-locked 149-165 range, so of course nothing was found -
  this was a test-rig artifact, not a router finding, and was identified
  and fixed rather than mistaken for evidence.
- `wpa_cli`/normal disconnect commands weren't available on this device
  build; `ip link set wlan0 down` (bypassing Android's own WiFi
  framework instead of fighting it through its normal API) was what
  actually got the shared phy off 5220 for long enough to retry cleanly.

**Third attempt - real result, confirmed clean from full logcat
timeline, not just the tail of the log:**
```
22:10:10.198  hostapd0: IEEE 802.11 driver had channel switch: freq=5745 ...
22:10:10.198  hostapd0: AP-CSA-FINISHED freq=5745 dfs=0
22:10:11.267  hostapd0: STA ea:fc:af:f9:f1:39 WPA: received EAPOL-Key frame (2/4 Pairwise)
22:10:11.268  hostapd0: STA ea:fc:af:f9:f1:39 WPA: sending 3/4 msg of 4-Way Handshake
22:10:11.274  hostapd0: STA ea:fc:af:f9:f1:39 WPA: received EAPOL-Key frame (4/4 Pairwise)
22:10:11.275  hostapd0: AP-STA-CONNECTED ea:fc:af:f9:f1:39
22:10:11.275  hostapd0: STA ea:fc:af:f9:f1:39 WPA: pairwise key handshake completed (RSN)
22:10:20.985  hostapd0: IEEE 802.11 driver had channel switch: freq=5220 ... (wlan0 reconnected, forced CSA - router correctly followed)
```
`ea:fc:af:f9:f1:39` is radio0's own address. The entire authentication,
association, and full WPA2 4-way handshake completed **while the test
AP was genuinely on 5745MHz/channel 149 - radio0's real, DT-permitted
operating range** - a full 9 seconds before the later channel switch
(caused by Android reconnecting its own `wlan0` in the background,
unrelated to this test) that the router's STA correctly followed via a
normal CSA, without dropping the association. Confirmed independently
from the router's own side: `wpa_cli -i phy0-sta0 status` showed
`wpa_state=COMPLETED`, real `TX: 1726 bytes (13 packets)` /
`RX: 684 bytes (4 packets)`.

**This is unambiguous, hard proof: radio0's transmit AND receive paths
both work correctly, including a full cryptographic handshake, on its
actual operating channel.** Not a counter, not a scan, not an inference
from a healthy-looking status line - genuine bidirectional RF with a
real external AP neither side had any reason to fake.

**Conclusion, superseding every hedge in §32:** radio0's RF hardware,
antenna path, and firmware TX/RX are all proven fully functional. The
R8000 AP-mode beacon-invisibility problem is **not a hardware defect on
this radio** - it is isolated specifically to running radio0 in AP
mode / generating beacons, since STA-mode association at the identical
frequency works flawlessly. The next real lever is investigating what's
different about this driver's AP-mode/beacon-generation path
specifically for radio0 (vs. the MBSS-legacy-fallback path patches/861
touches, vs. whatever radio2's now-disabled AP used before it was
repurposed for the American 5GHz extend) - not anything RF/hardware/
antenna-related, which is now closed out with direct evidence.

Test cleanup: `hostapd0` removed from the phone, its process killed,
router's `main_radio0` re-enabled and `rxtest0` removed via UCI, full
reboot (same documented recovery pattern as every other AP<->STA role
switch this session). Verified post-reboot: R8000 AP `state=ENABLED`
on `phy0-ap0`, both American uplinks reconnected, and the actual
downstream-client path re-confirmed end-to-end (a real LAN client on
this bench, not the router pinging itself: 0% packet loss to 8.8.8.8
through the extended network) - zero lasting damage from this test.
