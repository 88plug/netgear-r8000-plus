# WPA3-SAE + 802.11w (MFP) -- investigation notes

Device: NETGEAR R8000, OpenWrt 25.12.5 r33051-f5dae5ece4 (bcm53xx/generic),
brcmfmac, 3x BCM43602 (2x 5 GHz PCIe + 1x 2.4 GHz PCIe). Root SSH,
192.168.1.1.

## 1. What's on the device today

```
$ apk list --installed | grep -iE 'wpad|hostapd|mbedtls'
apk-mbedtls-3.0.5-r3
hostapd-common-2025.08.26~ca266cc2-r2
libmbedtls21-3.6.6-r2
libustream-mbedtls20201210-2026.03.01~99f1c0db-r1
wpad-basic-mbedtls-2025.08.26~ca266cc2-r2
```

Note: `opkg` is **not present** on this build -- 25.12.5 uses `apk` as the
package manager. The task's suggested `opkg list-installed` command doesn't
exist on this image; used `apk list --installed` instead.

```
$ hostapd -v
hostapd v2.12-devel
$ wpa_supplicant -v
wpa_supplicant v2.12-devel
```

Both built from upstream hostap commit `ca266cc2` (2025-08-26), pulled in by
this tree's `package/network/services/hostapd/Makefile`
(`PKG_SOURCE_DATE:=2025-08-26`, `PKG_SOURCE_VERSION:=ca266cc24d8705eb...`).

`iw phy phy0/phy1/phy2 info | grep -iE "SAE|MFP"` -> **no output on any
radio.** This is expected, not a gap: cfg80211 only advertises
`NL80211_EXT_FEATURE_SAE` for drivers that offload SAE key derivation to
firmware. brcmfmac's AP path here has no such offload (this chip's firmware
is a 2015-era build, `7.35.177.56`, long before any SAE-in-firmware feature
existed on Broadcom parts), so SAE/MFP are handled entirely in userspace by
hostapd -- cfg80211 has nothing radio-specific to advertise. Confirmed the
feature actually is compiled into the running binary a different way:

```
$ strings /usr/sbin/wpad | grep -iE 'sae_password|sae_groups|sae_pwe|ieee80211w'
sae_password_file '%s' not found.
rsn_pairwise
ieee80211w
Line %d: Invalid sae_password
sae_password_file
sae_groups
sae_pwe
...
```

## 2. Package decision: keep `wpad-basic-mbedtls`

Full reasoning and source citations are in `imagebuilder-packages.md`.
Summary: in this tree's `hostapd/Makefile`, `CONFIG_SAE=y` is added
unconditionally for every non-"internal" `SSL_VARIANT` (openssl / wolfssl /
mbedtls) regardless of `LOCAL_VARIANT` (basic / full / mesh) -- the "basic"
trim only drops EAP/RADIUS/SuiteB192/DPP/Hotspot2.0, never SAE. The
`wpad-basic-mbedtls` package description in that same Makefile says so
explicitly: *"WPA-PSK, SAE (WPA3-Personal), 802.11r and 802.11w support"*.
802.11w/MFP itself isn't even compile-time gated in this hostapd snapshot
(no `CONFIG_IEEE80211W` `#ifdef` survives in `build_features.h`, and every
`hostapd-*.config` template, including `full`, leaves it commented) -- it's
purely a runtime option.

This matches an earlier (mid-2025-ish) web search consensus for
`wpad-basic-mbedtls` vs `wpad-mbedtls`, but I did not rely on that search on
its own -- the decision above is grounded directly in the source that will
actually build the v2 image, not general OpenWrt documentation for an
unspecified release. `wpad-mbedtls`/`wpad-openssl` ("full") only add
WPA3-Enterprise 192-bit, EAP methods, RADIUS accounting/server, DPP, and
Hotspot 2.0 extras -- none needed for personal/PSK WPA3 on a home AP, and
not worth the extra binary size on an embedded target.

**No package change is required for WPA3-SAE + MFP capability.** The v2
`imagebuilder-packages.md` change is to pin `wpad-basic-mbedtls` explicitly
on the `make image` command line instead of relying on the implicit
`DEVICE_netgear_r8000_PACKAGES` profile default, plus add `hostapd-utils`
for post-flash verification (`hostapd_cli`).

## 3. UCI encryption scheme (verified against this tree's netifd scripts)

`package/network/config/wifi-scripts/files/lib/netifd/netifd-wireless.sh`,
function `wireless_vif_parse_encryption()`:

| UCI `option encryption` | internal `auth_type` | meaning |
|---|---|---|
| `sae` (or `psk3`) | `sae` | WPA3-Personal only |
| `sae-mixed` (or `psk3-mixed`) | `psk-sae` | WPA2-PSK / WPA3-SAE transition |
| `psk2` | `psk` | WPA2-PSK only (unchanged, for reference) |

`hostapd.sh` then auto-defaults `ieee80211w` per `auth_type` if not set:
- `auth_type=sae` -> `ieee80211w=2` (MFP required -- SAE's own requirement)
- `auth_type=psk-sae` -> `ieee80211w=1` on non-6GHz bands (MFP optional, for
  WPA2-only client compatibility), `2` only on 6 GHz.

The shipped `etc/config/wireless` sets `ieee80211w` explicitly for both
cases rather than relying on the implicit default, so the config is
self-documenting.

## 4. Radio topology and channel calibration caveat

```
phy0 (radio0, PCIe 0000:01:00.0, 5 GHz #1): ch 149,153,157,161,165 usable
                                             (20 dBm); ch 34-144 disabled
phy1 (radio1, PCIe 0001:03:00.0, 2.4 GHz):  ch 1-11 usable (20 dBm); 12-14
                                             disabled (regulatory)
phy2 (radio2, PCIe 0001:04:00.0, 5 GHz #2): ch 36,38,40... usable (20 dBm);
                                             ch 149-165 disabled (inverse of
                                             phy0!)
```

dmesg on every one of the 3 brcmfmac PCIe devices shows:
```
Direct firmware load for brcm/brcmfmac43602-pcie.netgear,r8000.bin failed with error -2
Direct firmware load for brcm/brcmfmac43602-pcie.txt failed with error -2
brcmf_c_process_clm_blob: no clm_blob available (err=-2), device may have limited channels available
brcmf_c_process_txcap_blob: no txcap_blob available (err=-2)
Firmware: BCM43602/1 wl0: Sep 18 2015 03:30:01 version 7.35.177.56 (r587209)
```
This is a pre-existing, separate issue (missing CLM/txcap calibration blob
for this exact firmware, tracked upstream as
[openwrt/openwrt#20514](https://github.com/openwrt/openwrt/issues/20514) and
[#19333](https://github.com/openwrt/openwrt/issues/19333) -- both confirm
the identical symptom on other R8000 units, neither resolved upstream as of
those reports) -- **not a WPA3/SAE problem**, and out of scope for this
workstream. It only affects which channels are legal to pick. The shipped
`etc/config/wireless` keeps `radio0` on channel 149 and `radio2` on channel
36, matching the channels the existing v1 config already used and that are
confirmed enabled on each radio, so this workstream doesn't compound that
issue.

## 5. Known risk to verify after flashing: AP-mode WPA3-SAE on 43602/brcmfmac

Found via targeted search, not assumed: **`openwrt/openwrt#9855`** ("WPA3
not working at all on Netgear R8000 with 22.03.0-rc1", filed 2022-05-08).
Reporter enabled SAE or mixed mode on an R8000, and every client (iPhone,
Android, Linux) failed to associate -- reported as a wrong-password error
client-side, hostapd log showed only immediate disassociation. Closed by
maintainers as **"won't fix" / "different project"**, i.e. judged an
upstream brcmfmac/hostapd interoperability problem rather than an
OpenWrt-side bug to patch.

Assessment of relevance to this v2 build:
- That report is 3+ years old, against OpenWrt 21.02/22.03-era hostapd.
  This tree's hostapd is a specific, much newer snapshot
  (`v2.12-devel`, commit `ca266cc2`, 2025-08-26).
- A separate, better-documented brcmfmac/hostap regression from August 2024
  (upstream hostap commit `41638606054a...`, "Mark authorization completed
  on driver indication during 4-way HS offload") caused WPA2-PSK/WPA3-SAE
  auth timeouts on brcmfmac -- but that is a **client-mode**
  (`wpa_supplicant`, STA 4-way-handshake-offload notification) bug, not the
  **AP-mode** (`hostapd`) path this router uses. Different code path;
  should not apply here, but flagging the distinction since it's easy to
  conflate the two given both involve brcmfmac + wpa_supplicant/hostapd.
- No 2024-2026 report of the specific 2022 AP-mode SAE failure mode was
  found in this search pass.

**This is the single highest-risk unknown in this workstream** because it
cannot be conclusively ruled out from source/package inspection alone --
only a live join test proves it. I did not modify the router's live,
in-use production `/etc/config/wireless` to test this in place (that would
take down the operator's working WiFi to test a build artifact that hasn't
been flashed yet); the config here is meant to be staged into the v2 image
build (see `imagebuilder-packages.md`, `FILES=`) and validated after
flashing, in the same session as the rest of the v2 bring-up.

## 6. Post-flash test plan

1. Flash v2 image, let it boot with the staged `etc/config/wireless`.
2. `hostapd_cli -i <ifname-for-wpa3_radio0> status` -- confirm process is
   up and `key_mgmt` field lists `SAE`.
3. Join test against `R8000-5G-WPA3` (SAE-only) with an actual WPA3+PMF
   capable client (modern iOS/Android/recent Linux `wpa_supplicant`/`iwd`,
   or a Windows 11 box) -- confirm full association + DHCP, not just
   auth-then-disassociate (the exact failure mode in #9855).
4. `hostapd_cli -i <ifname> all_sta` after the client above associates --
   confirm the station's `key_mgmt=SAE` and (if 802.11w negotiated)
   `mfp=1`.
5. Join test against `R8000-5G` and `R8000-2G` (mixed) with both a WPA3
   client and an older WPA2-only client -- confirm both connect, and that
   the WPA2-only client is not forced into PMF (would fail to associate if
   `ieee80211w` were misconfigured to `2` there).
6. `iw dev <ifname> station dump` on an associated client to sanity-check
   the negotiated cipher (`CCMP` pairwise, no `TKIP` fallback expected).
7. If step 3 fails with immediate disassociation (matching #9855's
   symptom): try `sae_pwe` forced to `1` (hunting-and-pecking only, this
   config leaves it at hostapd's own default of `2` = both H2E and H&P) as
   the first diagnostic step, and capture `hostapd -dd` debug output around
   the failed SAE commit/confirm exchange before escalating further.

## 7. Files in this directory

- `notes.md` -- this file
- `imagebuilder-packages.md` -- PACKAGES decision, evidence, and the
  `make image` command for v2
- `etc/config/wireless` -- deployable wireless config: WPA3-SAE-only AP +
  WPA2/WPA3-mixed AP on `radio0` (5 GHz), WPA2/WPA3-mixed AP with MFP
  optional on `radio1` (2.4 GHz), `radio2` (2nd 5 GHz radio) left at v1
  baseline/disabled (out of scope, pattern documented inline for later use)
