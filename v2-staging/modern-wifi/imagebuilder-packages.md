# ImageBuilder PACKAGES change -- roaming / steering / QoS

**Authoritative build recipe is `docs/RUNBOOK.md` §5 -- this file is
history, not a live reference.** Two things below are now known stale
(fixed 2026-07-24): the "No wpad/hostapd package change" conclusion, and
the `make image` command block's plain `wpad-basic-mbedtls`. See the
correction notes inline.

All package names below were verified to exist for this exact target by
grepping the ImageBuilder's own package index
(`openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64/.packageinfo`,
built from the same `repositories` feed set as this device's live image --
`releases/25.12.5/{targets/bcm53xx/generic,packages/arm_cortex-a9/{base,luci,packages,routing,telephony,video}}`).
The router itself has no WAN/internet route in this environment
(`apk update` fails with "Network unreachable"), so `.packageinfo` is the
authoritative source of truth here, not a live `apk search`.

## CORRECTION (2026-07-24, see docs/FINDINGS.md v2b entry): wpad DOES need to change

The "No wpad/hostapd package change" conclusion originally below was wrong
in the way that matters: `strings`-on-binary evidence that `bss_transition`
symbols exist in `wpad-basic-mbedtls` does not mean hostapd will actually
*start* with a `bss_transition '1'` config option. On the real v7 build,
`wpad-basic-mbedtls` (`CONFIG_WNM` off) made hostapd reject the config
outright ("unknown configuration item 'bss_transition'"), failing
`hostapd.add_iface` **for every phy** and taking down all 4 SSIDs on first
boot -- the same bug `docs/FINDINGS.md`'s v2b entry had already found and
fixed once. Fix: **`wpad-mbedtls`** (`CONFIG_WNM=y`), not
`wpad-basic-mbedtls`. See `docs/RUNBOOK.md` §5 and
`../wpa3/imagebuilder-packages.md`'s own CORRECTION section (same
underlying issue, found independently by two workstreams).

## No wpad/hostapd package change [SUPERSEDED, see correction above]

v1 and the sibling WPA3 workstream both keep `wpad-basic-mbedtls`. This
workstream's own investigation (`../notes.md` section 1) independently
confirms the same binary already has 802.11r, 802.11k (RRM), 802.11v (WNM/
BSS-Transition), OWE, and MBO compiled in -- verified via `strings
/usr/sbin/hostapd` on the live device, not just source inspection. Nothing
here requires `wpad-mbedtls`/`wpad-openssl` ("full") or `wpad-mesh-*`
(802.11s mesh doesn't work on this hardware regardless of package -- see
notes.md section 1). **This reasoning is real (the symbols are genuinely
compiled in) but incomplete: symbol presence isn't the same as the compiled
`.config` accepting the corresponding hostapd.conf directive at parse time.
The live production failure above is the actual test that matters, and it
says `wpad-mbedtls` is required whenever `bss_transition` ships.**

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
  PACKAGES="-wpad-basic-mbedtls wpad-mbedtls hostapd-utils wpa-cli wireless-regdb kmod-brcmfmac brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 kmod-phy-bcm-ns-usb3 usteer sqm-scripts" \
  FILES="files/"
```

**Fixed 2026-07-24: `-wpad-basic-mbedtls wpad-mbedtls`, not plain
`wpad-basic-mbedtls`** -- see the CORRECTION section above; this is the
exact command block that would have reproduced v7's all-4-SSIDs-down outage
if copy-pasted verbatim. `usteer`/`sqm-scripts` appended, `wpad` swap
otherwise matches `../wpa3/imagebuilder-packages.md`'s own fixed command.
Add `luci-app-usteer luci-app-sqm` too if the merged image ships LuCI. **The
actual shipping build (v7 onward) additionally sets `FILES=/home/andrew/netgearr8000/v2-files`
(not `files/"`) and adds `luci`/`ethtool`** to the merged `PACKAGES=` line
-- see `docs/RUNBOOK.md` §5 for the exact, currently-used recipe; the
`FILES="files/"` staging note below describes this workstream's own
narrower package additions, not the full merged command.

If `FILES="files/"` is used, this directory's `etc/config/usteer` and
`etc/config/sqm` should be staged at `files/etc/config/usteer` and
`files/etc/config/sqm` respectively, relative to the ImageBuilder root. The
`etc/config/wireless-roaming.additions` file is NOT meant to be staged
as-is -- it documents options to merge into the WPA3 workstream's
`files/etc/config/wireless` by hand (see that file's header).
