# ImageBuilder PACKAGES change -- WPA3-SAE / 802.11w

## CORRECTION (2026-07-23, see FINDINGS.md item 2 / v7)

This doc's "keep wpad-basic-mbedtls" recommendation is correct for WPA3-SAE
and 802.11w/MFP alone, but does NOT account for `bss_transition` (802.11v),
which this repo's own modern-wifi/roaming workstream also ships in the same
merged wireless.conf. `wpad-basic-mbedtls` lacks CONFIG_WNM, so hostapd
rejects `bss_transition` outright and every AP interface fails to come up --
this was already found once (FINDINGS.md's v2b entry) and regressed again on
v7 by following this doc's package list verbatim. If the merged config ships
`bss_transition`, use `wpad-mbedtls` (with `-wpad-basic-mbedtls` to drop the
device-profile default), not `wpad-basic-mbedtls`.

## Decision: no wpad package swap

v1 shipped `wpad-basic-mbedtls`, which is also the OpenWrt upstream default
for this device profile (see evidence below). It already provides full
WPA3-Personal (SAE) and 802.11w (MFP) support. **v2 keeps
`wpad-basic-mbedtls`** -- do not switch to `wpad-mbedtls` or
`wpad-openssl`/`wpad-wolfssl`; those "full" variants only add EAP/RADIUS,
WPA3-Enterprise 192-bit (Suite B), DPP/Easy Connect and Hotspot 2.0 extras,
none of which this workstream (WPA3-Personal + MFP on a home AP) needs.

## Evidence

1. Device-installed package (confirmed live over SSH, `apk list --installed`,
   this build uses `apk`, not `opkg` -- `opkg` is not present on 25.12.5):
   ```
   wpad-basic-mbedtls-2025.08.26~ca266cc2-r2
   hostapd-common-2025.08.26~ca266cc2-r2
   libmbedtls21-3.6.6-r2
   libustream-mbedtls20201210-2026.03.01~99f1c0db-r1
   ```
   `hostapd -v` / `wpa_supplicant -v` on-device report `v2.12-devel`
   (upstream commit `ca266cc2`, dated 2025-08-26).

2. `iw phy phy0/phy1/phy2 info | grep -iE "SAE|MFP"` returns nothing on
   either radio. This is expected and not a red flag: SAE/MFP for AP mode
   on brcmfmac is handled entirely in userspace by hostapd (no firmware SAE
   offload exists on the BCM43602's 2015-era firmware, `7.35.177.56`), so
   cfg80211 never advertises `NL80211_EXT_FEATURE_SAE`. Confirmed the
   feature is compiled into the running binary instead:
   `strings /usr/sbin/wpad | grep -iE 'sae_password|sae_groups|sae_pwe|ieee80211w'`
   returns the full set of SAE/MFP config keywords.

3. Source-level confirmation, this exact tree
   (`openwrt/package/network/services/hostapd/Makefile`, lines 108-151):
   for every non-"internal" `SSL_VARIANT` (openssl / wolfssl / mbedtls),
   `DRIVER_MAKEOPTS += CONFIG_TLS=<x> CONFIG_SAE=y` is added **unconditionally
   for every `LOCAL_VARIANT`** (basic / mesh / full) -- `basic` is not
   special-cased out. The `wpad-basic-mbedtls` package description in the
   same file states this explicitly:
   > "WPA-PSK, SAE (WPA3-Personal), 802.11r and 802.11w support"

4. 802.11w/MFP is not compile-time gated at all in this hostapd snapshot:
   `hostapd-*.config` templates leave `#CONFIG_IEEE80211W=y` commented out
   for every variant including `hostapd-full.config`, and
   `src/src/utils/build_features.h` has no `"11w"`/MFP entry (unlike `sae`,
   `owe`, `suiteb192`, which are real `#ifdef`-gated features). MFP support
   is unconditionally compiled in; it is controlled purely at runtime via
   the `ieee80211w` UCI/hostapd.conf option.

5. `openwrt/package/network/config/wifi-scripts/files/lib/netifd/hostapd.sh`
   (the netifd->hostapd.conf translator actually used to render the config
   on-device) auto-defaults `ieee80211w` per `auth_type`:
   - `auth_type=sae` (UCI `encryption 'sae'`) -> `set_default ieee80211w 2`
   - `auth_type=psk-sae` (UCI `encryption 'sae-mixed'`) -> `set_default
     ieee80211w 1` on non-6GHz bands
   confirming `wpad-basic-mbedtls` + this UCI scheme is a supported,
   first-class path, not a workaround.

6. This device's ImageBuilder profile already defaults to
   `wpad-basic-mbedtls` upstream (`.profiles.mk` in the ImageBuilder tarball):
   ```
   DEVICE_netgear_r8000_PACKAGES:=wpad-basic-mbedtls kmod-brcmfmac \
     brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 \
     kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 \
     kmod-phy-bcm-ns-usb3
   ```

## The actual PACKAGES change

Functionally: **none required** for WPA3-SAE/MFP capability -- it's already
there. The concrete change for the v2 build is to stop relying on the
implicit profile default and pin it explicitly on the `make image` command
line (self-documenting, survives if the upstream device profile default
ever changes), and add the CLI debug utilities used to verify SAE/MFP is
actually negotiated per-station after flashing (see notes.md test plan).

```sh
cd openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64

make image \
  PROFILE="netgear_r8000" \
  PACKAGES="wpad-basic-mbedtls hostapd-utils wpa-cli wireless-regdb kmod-brcmfmac brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 kmod-phy-bcm-ns-usb3" \
  FILES="files/"
```

- `wpad-basic-mbedtls` -- explicit (was implicit via `DEVICE_..._PACKAGES`).
  Provides WPA3-SAE + 802.11w/MFP, matches v1 and the upstream default.
- `hostapd-utils` -- installs `hostapd_cli`. Used post-flash to confirm the
  negotiated `key_mgmt`/`pairwise_cipher` per associated station
  (`hostapd_cli -i <ifname> all_sta`, `hostapd_cli -i <ifname> status`).
- `wpa-cli` -- matching client-side CLI, useful if a wpa_supplicant-based
  test client is used against this AP.
- `wireless-regdb` -- already present on the current device; carried
  forward explicitly rather than dropped.
- The `kmod-brcmfmac` / firmware / USB kmod set is unchanged from the
  existing `DEVICE_netgear_r8000_PACKAGES` default -- listed for
  completeness of the build command, not because anything changed there.

If FILES="files/" is used to stage `/etc/config/wireless` at build time,
place `etc/config/wireless` from this directory at `files/etc/config/wireless`
relative to the ImageBuilder root before running `make image`.
