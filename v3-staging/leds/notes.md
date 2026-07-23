# R8000 front-panel LED fix — session 2 (production image, phyN-apM naming)

Builds directly on `v2-staging/leds/notes.md` (DTS decompiled from live
`/sys/firmware/fdt`, all 11 GPIOs mapped and confirmed sound, no kernel/DTS
fix needed). That session applied a working live UCI config using legacy
`wlan0`/`wlan1`/`wlan2` netdev names against a bench image with no AP config
loaded. This unit has since been reflashed/reconfigured to a production
image where hostapd creates multi-BSS `phyN-apM` interfaces instead —
those old `wlanN` names in the router's `/etc/config/system` no longer
matched any live netdev, so the netdev-trigger LEDs silently stayed dark
again. This session re-diagnoses and re-fixes against the current interface
names.

## 1. Starting state (this session)

`uci show system` already had `led_wan`, `led_wlan2g`, `led_wlan5g1`,
`led_wlan5g2`, `led_wps`, `led_wireless`, `led_usb2`, `led_usb3` sections
(carried over from the v2-staging session's live test). The three WiFi LED
sections pointed `dev` at `wlan1` / `wlan2` / `wlan0` — none of which exist
as netdevs on this build (`ip link show` / `iw dev` confirm only
`phy0-ap0/1/2`, `phy1-ap0/1`, `phy2-ap0` exist). `wireless`/`wps` had no
`trigger` option at all (`option default '0'` only), leaving them at
whatever the kernel/board.d left them — which happened to be
`brightness=1` at the moment of inspection because an earlier manual
force-test (`echo none>trigger; echo 1>brightness`) had been run directly
against them to confirm the GPIOs were alive, per the task brief.

Confirmed radio/band mapping via `iw dev` channel/frequency (ground truth,
not assumed):

| phy | AP iface | channel/freq | band |
|---|---|---|---|
| phy1 | phy1-ap0 | ch 1 / 2412 MHz | 2.4GHz |
| phy0 | phy0-ap0 | ch 153 / 5765 MHz | 5GHz upper |
| phy2 | phy2-ap0 | ch 36 / 5180 MHz | 5GHz lower |

Matches the task brief's stated mapping exactly.

## 2. Before / after brightness table

| LED (`bcm53xx:white:*`) | trigger before | brightness before | trigger after | brightness after | notes |
|---|---|---|---|---|---|
| `2ghz` | `none` | 1 (manual force-test) | `netdev`, dev=`phy1-ap0`, mode=`link tx rx` | **1** | `link=1 tx=1 rx=1` confirmed — real AP-up state, not a leftover force value |
| `5ghz-1` | `none` | 1 (manual force-test) | `netdev`, dev=`phy0-ap0`, mode=`link tx rx` | **1** | same, real state |
| `5ghz-2` | `none` | 1 (manual force-test) | `netdev`, dev=`phy2-ap0`, mode=`link tx rx` | **1** | same, real state |
| `wireless` | `none` | 1 (manual force-test) | `netdev`, dev=`phy1-ap0`, mode=`link tx rx` | **1** | previously fully unmanaged (no trigger option at all); now tracks the 2.4GHz AP (always-present radio) as a general "wireless up" indicator |
| `wan` | `netdev`, dev=`wan` | 0 | unchanged | 0 | already correctly wired; 0 is correct — `ip link show wan` = `NO-CARRIER`/`LOWERLAYERDOWN`, i.e. no WAN cable plugged in. Not a bug, no fix applied. |
| `wps` | `none` (default=0) | 0 | unchanged (default=0) | 0 | deliberately left off — WPS-button-driven blink during pairing, not a steady-state indicator (see reasoning below) |
| `usb2` | `usbport` | 0 | unchanged | 0 | already correct; 0 = no device on that physical port |
| `usb3` | `usbport` | 0 | unchanged | 0 | already correct; 0 = no device on that physical port. `led restart` prints a harmless `usb4-port1: Permission denied` — pre-existing, unrelated to this fix (no xHCI/USB3 controller loaded on this build, see v2-staging notes §4 "adjacent finding") |
| `power` (white) | `none` | 1 | unchanged | 1 | correct as-is — driven by shared `/etc/diag.sh` `status_led_on()`, not board.d/uci |
| `power` (amber) | `none` | 0 | unchanged | 0 | correct as-is — fault-only, no fault |
| `wan` (amber) | `none` | 0 | unchanged | 0 | reserved/unused, no bug reported for it |

**Result: all 4 targeted WiFi-status LEDs (2.4GHz, 5GHz-1, 5GHz-2,
Wireless) are now genuinely netdev-driven and physically lit
(brightness=1, `link`/`tx`/`rx` mode files all =1) because their
respective APs are actually up** — this is real link-state tracking, not
a residual manual force value. Verified by reading each LED's individual
`link`/`tx`/`rx` sysfs attribute files after the `netdev` trigger switch,
not just `brightness`.

## 3. GPIO / DTS status — no fix needed

Every WiFi-adjacent LED (`2ghz`, `5ghz-1`, `5ghz-2`, `wireless`, `wps`) was
already proven to physically light (`brightness=1` under a manual
`trigger=none` force) before this session started, per the task brief.
This session's fix only had to correct the netdev `dev=` binding — the
GPIOs, DTS `linux,default-trigger` wiring, and kernel `ledtrig-netdev`
plumbing were never the problem. See `v2-staging/leds/notes.md` §3 and
`v2-staging/leds/leds-and-buttons-excerpt.dts` for the full GPIO table
(chipcommon GPIOs 12/13/14/16 for 5ghz-1/2ghz/wireless/5ghz-2
respectively) — unchanged, still accurate, still correct.

**No DTS gpio-led mapping bug exists.** Nothing in this repo's
`openwrt/target/linux/bcm53xx` tree needed touching.

## 4. WPS LED — left off, on purpose

`bcm53xx:white:wps` remains `trigger=none`/`brightness=0`
(`option default '0'` in uci). Kept this way rather than forcing
`default-on` or `heartbeat` because:
- The physical WPS LED's job is to blink *during an active WPS pairing
  attempt* (normally driven by hostapd's `wps_pushbutton` state machine
  toggling the LED directly, not by a static uci `led` trigger).
- Forcing it `default-on` would misrepresent "WPS is idle" as "WPS is
  active/available", and `heartbeat` has no relationship to WPS state at
  all — both would be actively misleading rather than merely dark.
- The GPIO itself is confirmed alive (force-test brightness=1 before this
  session), so if/when WPS pushbutton signalling is wired up to this LED
  in hostapd config, it will work; that's a separate, optional workstream
  from "make the status LEDs reflect reality," which is what this task
  asked for.

## 5. Files in this directory

| file | what |
|---|---|
| `system.conf` | full `/etc/config/system` pulled live from 192.168.1.1 **after** the fix was applied and verified — the authoritative working config |
| `99-led-wifi-netdev-fix` | uci-defaults script (drop into `openwrt/package/base-files/files/etc/uci-defaults/` or a custom-files overlay) that reproduces the same fix on a fresh flash/build, using `ucidef_set_led_netdev` bound to `phyN-ap0` device names instead of the legacy `wlanN` scheme |
| `notes.md` | this file |

## 6. Live verification commands used (for reproducibility)

```sh
# apply
uci set system.led_wlan2g.dev='phy1-ap0';   uci set system.led_wlan2g.mode='link tx rx'
uci set system.led_wlan5g1.dev='phy0-ap0';  uci set system.led_wlan5g1.mode='link tx rx'
uci set system.led_wlan5g2.dev='phy2-ap0';  uci set system.led_wlan5g2.mode='link tx rx'
uci set system.led_wireless.trigger='netdev'
uci set system.led_wireless.dev='phy1-ap0'
uci set system.led_wireless.mode='link tx rx'
uci -q delete system.led_wireless.default
uci commit system
/etc/init.d/led restart

# verify (per LED)
cat /sys/class/leds/bcm53xx:white:2ghz/trigger      # -> ... [netdev] ...
cat /sys/class/leds/bcm53xx:white:2ghz/device_name  # -> phy1-ap0
cat /sys/class/leds/bcm53xx:white:2ghz/brightness   # -> 1
cat /sys/class/leds/bcm53xx:white:2ghz/link         # -> 1
cat /sys/class/leds/bcm53xx:white:2ghz/tx           # -> 1
cat /sys/class/leds/bcm53xx:white:2ghz/rx           # -> 1
```

This `uci commit` persists across reboots on this unit's overlay as-is
(no reflash needed for the fix to survive). `99-led-wifi-netdev-fix` is
for baking the same behavior into the next image build.
