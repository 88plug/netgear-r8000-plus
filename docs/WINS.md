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

## Outstanding (user tasks, not bugs)

- Change temp passphrases: `ChangeMe-R8000-2026`, `GuestChangeMe2026`.
- Set SQM WAN bandwidth to activate bufferbloat control.
- Move `R8000-Guest` to an isolated network/VLAN for true guest isolation.
