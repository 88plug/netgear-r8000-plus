# R8000 LED workstream — findings and fix

## TL;DR

The device tree is fine. The bug is 100% in userspace: `board.d/01_leds`
only ever wired up two of the eleven LEDs this board exposes (USB2/USB3).
Power, WAN/Internet, and all three WiFi-band LEDs had no UCI `led`
section at all, so they sat at whatever the kernel left them at boot and
could never react to real state. Fixed by extending the `netgear,r8000`
case in `01_leds`. Verified live on the router by applying the
equivalent UCI and confirming the sysfs LED trigger wiring is correct.

No DTS changes were made or are needed (see "DTS" below). One nvram-init
addition was made for parity with sibling boards but is *not* the fix
for the visible LEDs — see "ledbh nvram" below, this directly
corrects the task's initial premise.

## 1. Live LED state (before fix)

`ls /sys/class/leds/` on 192.168.1.1 showed 11 registered LED class
devices:

| sysfs name | trigger (before) | brightness (before) | why |
|---|---|---|---|
| `bcm53xx:white:power` | `none` | **1 (on)** | correct — driven by the shared `/etc/diag.sh` `status_led_on()`, not board.d. Working as intended. |
| `bcm53xx:amber:power` | `none` | 0 | correct — fault-only LED, no fault, off is right |
| `bcm53xx:white:wan` | `default-on` | **1 (on)** | **bug** — DTS default, WAN was actually down (`ubus call network.interface.wan status` → `"up": false`) but LED showed solid white regardless |
| `bcm53xx:amber:wan` | `none` | 0 | unused (reserved, see below) |
| `bcm53xx:white:2ghz` | `none` | 0 | **bug** — never configured, can never light even though phy1 (2.4GHz radio) is up |
| `bcm53xx:white:5ghz-1` | `none` | 0 | **bug** — same, phy0 |
| `bcm53xx:white:5ghz-2` | `none` | 0 | **bug** — same, phy2 |
| `bcm53xx:white:wireless` | `none` | 0 | unused (reserved, see below) |
| `bcm53xx:white:wps` | `none` | 0 | **bug** — never configured, not under UCI management |
| `bcm53xx:white:usb2` | `usbport` | 0 (no device attached) | correct — already wired in board.d, working |
| `bcm53xx:white:usb3` | `usbport` | 0 (no device attached) | correct — already wired in board.d, working |

`/etc/config/system` on the router only contained `led_usb2` and
`led_usb3` sections — nothing else. `cat /etc/config/system` output
matched exactly what `target/linux/bcm53xx/base-files/etc/board.d/01_leds`'s
`netgear,r8000)` case produces: two `ucidef_set_led_usbport` calls and
nothing more.

## 2. Root cause

`/etc/init.d/led` (`config_foreach load_led`) only touches LEDs that
have a `config led` section in `/etc/config/system`. Any sysfs LED
*not* referenced there is simply never visited — it keeps whatever
trigger/brightness the kernel left at DT-probe time, forever, and the
`led turnon`/`turnoff`/`blink` commands can't manage it either through
UCI restarts.

`01_leds`'s `netgear,r8000)` case only called `ucidef_set_led_usbport`
twice. Nothing else was wired: no `wan`, no per-band wifi LEDs, no wps,
no explicit power (power doesn't need one — see below).

## 3. Device tree — verified correct, no changes made

The upstream/local `.dts` source is **not vendored** in this repo's
`openwrt/target/linux/bcm53xx/` tree (bcm53xx pulls
`arch/arm/boot/dts/broadcom/bcm4708-netgear-r8000.dts` from the fetched
Linux kernel source at build time). Rather than reconstruct it from
memory, I pulled the ground truth directly off the running unit:

```
ssh root@192.168.1.1 'cat /sys/firmware/fdt' > router.dtb
dtc -I dtb -O dts router.dtb -o router-live.dts
```

(`router.dtb` / `router-live.dts` in this directory — 749-line full
live tree; `leds-and-buttons-excerpt.dts` is the trimmed `leds {}` /
`gpio-keys {}` nodes with GPIO numbers annotated.)

Findings:
- All 11 LEDs are present with correct `label`s matching what shows up
  in `/sys/class/leds/`.
- All LED and button nodes hang off **one single GPIO controller**:
  `axi@18000000/chipcommon@0` (phandle 6), the BCM4709 SoC's own
  ChipCommon GPIO block. There is **no** wifi-chip-local GPIO involved
  anywhere in this DT — confirms the WiFi band LEDs are plain SoC GPIOs,
  not driven through the BCM43602 radios (see section 5).
- `led-power-white` (gpio 2) and `led-wan-white` (gpio 8) both carry
  `linux,default-trigger = "default-on"` in the DTS itself — that's
  where the "WAN LED always on" behavior comes from; it's a boot-time
  default meant to be overridden by board.d, which never happened for
  `wan`.
- GPIO map (chipcommon-relative, flag 1=`GPIO_ACTIVE_LOW`,
  0=`GPIO_ACTIVE_HIGH`, raw values from the live blob):

  | LED | GPIO | Notes |
  |---|---|---|
  | power (white) | 2 | active-low, default-on |
  | power (amber) | 3 | active-low |
  | wan (white) | 8 | active-low, default-on |
  | wan (amber) | 9 | active-high |
  | 5ghz-1 | 12 | active-low, radio slot 0 / phy0 |
  | 2ghz | 13 | active-low, radio slot 1 / phy1 |
  | wireless | 14 | active-high, no confirmed distinct front-panel icon |
  | wps | 15 | active-high |
  | 5ghz-2 | 16 | active-low, radio slot 2 / phy2 |
  | usb3 | 17 | active-low |
  | usb2 | 18 | active-low |

  (Buttons on the same controller, for completeness: rfkill=gpio4,
  wps=gpio5, reset=gpio6, backlight/LED-dimmer=gpio19.)

**Conclusion: no DTS correction is needed.** The tree is complete and
accurate; the task step asking for "DTS led-node corrections" turned
up nothing to correct.

## 4. The fix — `board.d/01_leds`

Patched file: `01_leds` in this directory (also applied in-tree at
`openwrt/target/linux/bcm53xx/base-files/etc/board.d/01_leds`).

Added to the `netgear,r8000)` case:

```sh
ucidef_set_led_netdev "wan" "Internet" "bcm53xx:white:wan" "wan" "link"
ucidef_set_led_netdev "wlan2g" "2.4GHz" "bcm53xx:white:2ghz" "wlan1" "link"
ucidef_set_led_netdev "wlan5g1" "5GHz-1" "bcm53xx:white:5ghz-1" "wlan2" "link"
ucidef_set_led_netdev "wlan5g2" "5GHz-2" "bcm53xx:white:5ghz-2" "wlan0" "link"
ucidef_set_led_default "wps" "WPS" "bcm53xx:white:wps" "0"
ucidef_set_led_default "wireless" "Wireless" "bcm53xx:white:wireless" "0"
```

(USB2/USB3 lines are untouched — they already worked.)

Power was deliberately **not** given a board.d entry. It's already
handled correctly by the shared, target-wide `etc/diag.sh`
(`get_status_led()` auto-picks the first `*:power` LED that isn't
amber/red and calls `status_led_on()` once boot reaches "done"). This
was directly confirmed working on the live unit (`brightness: 1`,
correct). Adding a UCI `led` section for it would be redundant and
risks fighting `diag.sh`'s direct sysfs writes for no benefit.

### wlan# ↔ radio/band mapping — how it was determined and its one caveat

`ucidef_set_led_netdev` needs an interface name, not a phy index, and
the kernel doesn't expose a `phyNtpt` LED trigger for brcmfmac (FullMAC
driver, doesn't register with mac80211's LED code — confirmed: the
`trigger` sysfs file on every one of these LEDs lists only
`none timer heartbeat default-on netdev usbport`, no `phyXtpt`). So the
binding has to be by `wlanN` device name, and interface numbering is
PCI-probe-order dependent, not guaranteed by spec — I resolved it with
two independent, mutually-confirming pieces of hard-wired hardware
data rather than guessing:

1. `extracted/calibration.txt` (per-radio SROM) MAC addresses:
   radio slot 0 → `...f1:38`, slot 1 → `...f1:37`, slot 2 → `...f1:36`.
2. Live `iw dev` on the router: phy0 addr `...f1:38` (5GHz) is `wlan2`,
   phy1 addr `...f1:37` (2.4GHz) is `wlan1`, phy2 addr `...f1:36` (5GHz)
   is `wlan0`.
3. `extracted/boot-results/RESULTS.md` independently states phy0/phy1/phy2
   band assignments, matching #1 exactly.

So: radio slot 0 (phy0, 5GHz) = **wlan2** → `5ghz-1` LED; slot 1 (phy1,
2.4GHz) = **wlan1** → `2ghz` LED; slot 2 (phy2, 5GHz) = **wlan0** →
`5ghz-2` LED. This is what's in the patch.

**Caveat, called out in a comment in the patched file too:** the
GPIO↔radio-slot mapping (from the DTS + calibration.txt) is
hardware-fixed and will never change. The radio-slot↔`wlanN` mapping is
PCI-probe-order-determined by the kernel/driver and, while it should be
stable across boots of the *same* kernel/driver build on the *same*
hardware, isn't architecturally guaranteed. Re-verify after any
brcmfmac/kernel bump with:
```sh
for w in /sys/class/net/wlan*; do echo "$w $(cat $w/address) phy=$(readlink $w/phy80211)"; done
```
and cross-check the MACs against `calibration.txt`'s `N:macaddr` values.

### Verified live (already applied to the running router)

Rather than trust the patch on paper, I reproduced exactly what
`config_generate` would have written to `/etc/config/system` from the
patched `01_leds` (`config_generate` is what turns `board.d`'s
`ucidef_set_led_*` calls, via `/etc/board.json`, into the real UCI —
confirmed by reading `bin/config_generate`'s `generate_led()`), applied
it with `uci` directly, and ran `/etc/init.d/led restart`:

- `bcm53xx:white:wan`, `:2ghz`, `:5ghz-1`, `:5ghz-2` all came up with
  `trigger=[netdev]`, correct `device_name` (`wan`/`wlan1`/`wlan2`/`wlan0`),
  and `mode=link` active (confirmed via the `link` sysfs file = `1`).
- Brought `wlan1` administratively up as a test: the `2ghz` LED's
  `link` file tracked it live. (No carrier yet since no AP config is
  loaded on this bench image and no WAN cable is plugged in on this
  bench — but the plumbing itself, trigger→device→mode, is proven
  correct and will assert brightness the moment the interface actually
  gets carrier, exactly like the pre-existing, known-good `usbport`
  LEDs already do.)
- `wps` / `wireless` came up `trigger=none`, `brightness=0` — explicitly
  managed now instead of orphaned.
- This live UCI change **is committed** on the router right now (already
  fixes the LEDs on this unit immediately). `wlan1` was returned to its
  original admin-down state after the test. The actual source fix for
  the next image build/flash is the `01_leds` file in this directory.

### USB2/USB3 — confirmed already correct, one adjacent (non-LED) finding

Both LEDs' `usbport` trigger config was already correct and untouched.
Restarting the led service reproduced one pre-existing, harmless
warning:
```
can't create /sys/class/leds/bcm53xx:white:usb3/ports/usb4-port1: Permission denied
```
Root cause (not an LED bug): `dmesg` shows only `ehci-platform` (bus 1,
USB2.0 480Mbps) and `ohci-platform` (bus 2, 12Mbps companion) — **no
xHCI/USB3.0 controller is loaded on this build**, so the `usb4-port1`
entry (the expected SuperSpeed bus for the physical "USB 3.0" port) never
gets created. `usb1-port1`/`usb2-port1` (EHCI+OHCI) for that same
physical port are real and do get wired, so the LED will still light for
any device plugged into that port — it just won't run at SuperSpeed and
the warning is cosmetic. This is a separate, kernel-config-level
USB3/xHCI workstream (verify `CONFIG_USB_XHCI_*` for bcm53xx target),
out of scope for this LED workstream — flagging it since it plausibly
explains an "the USB3 port doesn't feel right" type complaint distinct
from the LED wiring itself.

## 5. ledbh nvram — corrects the task's initial premise

The task described the WiFi LEDs as going "via 0:ledbh10 / 1:ledbh10."
That's the mechanism used by *other* BCM43602 boards in this exact repo
(`openwrt/package/utils/nvram/files/nvram-bcm53xx.init`,
`set_wireless_led_behaviour()`, e.g. `asus,rt-ac3200`) — but it did not
have a `netgear,r8000)` case at all, and more importantly:

- `grep -i ledbh` across `extracted/nvram-stock.txt` **and**
  `extracted/calibration.txt` returns **zero matches**. Stock R8000
  nvram never populated any `ledbh` variable.
- The live device tree (section 3, above) proves the visible
  2.4GHz/5GHz-1/5GHz-2 LEDs are wired to the **main SoC's ChipCommon
  GPIOs**, not to any GPIO on the BCM43602 radio chips themselves.
  `ledbh` is a Broadcom SROM concept the wifi chip's own firmware
  would use to drive a *chip-local* LED GPIO pin — a physically
  different pin than the ones actually connected on this board.

So setting `ledbh10` cannot be "the fix" for the LEDs the operator was
looking at — those are proven to be GPIO-driven and are fixed by
section 4 above, already verified live.

I still added a `netgear,r8000)` case to `set_wireless_led_behaviour()`
in `nvram-bcm53xx.init` (patched copy in this directory), setting
`0:ledbh10=0x7 1:ledbh10=0x7 2:ledbh10=0x7` (all three radios — R8000's
`set_bcm43602_variables()` case, right below it in the same file, shows
all three are BCM43602-class: `devid=0x43bc`/`0x43bb`), because:
- It's what the task asked for.
- It's harmless: sanity-checked `nvram set/get/unset 0:ledbh10=0x7` etc.
  directly on the router (not committed — see below) and it round-trips
  cleanly with no errors.
- It matches this file's own established convention for sibling
  BCM43602 boards, for parity.
- If the wifi chip firmware *does* consult it in some capacity not
  visible from the DT (unverifiable without opening the unit or the
  proprietary vendor driver), this now covers it for free.

**This nvram change was deliberately left uncommitted on the live
router** — I only round-tripped `set`/`get`/`unset` in memory to confirm
the mechanism works, then reverted. It's an `init.d` script baked into
the firmware image (`START=02`, runs every boot before any LED init),
so it can't be meaningfully "live-patched" without a rebuild anyway; the
persisted `nvram commit` for this specific addition is left for the
operator's next full image build/flash cycle rather than pushed
unilaterally to the running unit's calibration partition.

## Files in this directory

| file | what |
|---|---|
| `01_leds` | patched board.d LED script — **the fix** (also applied in-tree) |
| `nvram-bcm53xx.init` | patched nvram-init script — ledbh10 parity addition, not the fix (also applied in-tree) |
| `router.dtb` | raw device tree blob pulled live from `/sys/firmware/fdt` on 192.168.1.1, 2026-07-23 |
| `router-live.dts` | full decompile of the above (`dtc -I dtb -O dts`) — ground truth for what's actually running |
| `leds-and-buttons-excerpt.dts` | trimmed `leds {}` / `gpio-keys {}` nodes from the above, annotated with GPIO numbers and radio-slot mapping — reference only, not a patch |
| `notes.md` | this file |
