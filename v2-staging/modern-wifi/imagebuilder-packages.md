# ImageBuilder PACKAGES change -- roaming / steering / QoS

All package names below were verified to exist for this exact target by
grepping the ImageBuilder's own package index
(`openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64/.packageinfo`,
built from the same `repositories` feed set as this device's live image --
`releases/25.12.5/{targets/bcm53xx/generic,packages/arm_cortex-a9/{base,luci,packages,routing,telephony,video}}`).
The router itself has no WAN/internet route in this environment
(`apk update` fails with "Network unreachable"), so `.packageinfo` is the
authoritative source of truth here, not a live `apk search`.

## No wpad/hostapd package change

v1 and the sibling WPA3 workstream both keep `wpad-basic-mbedtls`. This
workstream's own investigation (`../notes.md` section 1) independently
confirms the same binary already has 802.11r, 802.11k (RRM), 802.11v (WNM/
BSS-Transition), OWE, and MBO compiled in -- verified via `strings
/usr/sbin/hostapd` on the live device, not just source inspection. Nothing
here requires `wpad-mbedtls`/`wpad-openssl` ("full") or `wpad-mesh-*`
(802.11s mesh doesn't work on this hardware regardless of package -- see
notes.md section 1).

## New packages

| Package | Version (this feed) | Why |
|---|---|---|
| `usteer` | `2025.10.04~1d6524c6e6b58853f053c7816249a8f68ad9b0e8-r1` | Client-steering daemon. See notes.md "usteer vs dawn" for why usteer over dawn. |
| `sqm-scripts` | `1.7.2-r1` | Bufferbloat control (SQM/cake) on WAN. Auto-pulls `kmod-sched-cake`, `kmod-ifb`, `tc`, `ip`, `iptables-mod-ipopt`. |
| `luci-app-usteer` *(optional)* | tracks `usteer` | LuCI GUI for usteer status/tuning. Add only if the image ships LuCI. |
| `luci-app-sqm` *(optional)* | tracks `sqm-scripts` | LuCI GUI for SQM. Add only if the image ships LuCI. |

Dependencies (`libubus`, `libubox`, `libblobmsg-json`, `libnl-tiny`,
`kmod-sched-cake`, `kmod-ifb`, `tc`, `ip`, `iptables-mod-ipopt`) are resolved
automatically by the ImageBuilder from each package's `Depends:` line --
listed here for awareness, not required on the `PACKAGES=` command line.

## Considered and not selected: dawn

`dawn` (`2025.11.07~7414c34a08c7cbbd5f292e0e25e53f1edbfc751a-r1`) also exists
in this exact feed set (`package/feeds/packages/dawn/Makefile`) and is a
viable alternative. Full evaluation in `../notes.md`. Short version: usteer
has a smaller dependency footprint (no `umdns`/mDNS discovery layer, no
`libgcrypt`) which matters more on this device's ~250 MB RAM / dual-core
Cortex-A9 than on typical modern hardware, and is the daemon the current
OpenWrt project (not a separate upstream) actively maintains. `dawn` remains
a documented fallback if a future multi-vendor mesh (non-OpenWrt APs mixed
in) needs its UMDNS-based discovery instead of usteer's ubus-based one.

## The actual PACKAGES change

This workstream's packages are additive to the WPA3 workstream's
`make image` command (`../wpa3/imagebuilder-packages.md`) -- both need to
land on the same final `PACKAGES=` line for the merged v2 image:

```sh
cd openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64

make image \
  PROFILE="netgear_r8000" \
  PACKAGES="wpad-basic-mbedtls hostapd-utils wpa-cli wireless-regdb kmod-brcmfmac brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 kmod-phy-bcm-ns-usb3 usteer sqm-scripts" \
  FILES="files/"
```

(WPA3 workstream's package set unchanged, left verbatim; `usteer` and
`sqm-scripts` appended. Add `luci-app-usteer luci-app-sqm` too if the merged
image ships LuCI.)

If `FILES="files/"` is used, this directory's `etc/config/usteer` and
`etc/config/sqm` should be staged at `files/etc/config/usteer` and
`files/etc/config/sqm` respectively, relative to the ImageBuilder root. The
`etc/config/wireless-roaming.additions` file is NOT meant to be staged
as-is -- it documents options to merge into the WPA3 workstream's
`files/etc/config/wireless` by hand (see that file's header).
