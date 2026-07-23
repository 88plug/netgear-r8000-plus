# R8000 calibration extraction — the breakthrough

**Date:** 2026-07-23 · **Router:** live, stock FW `V1.0.4.88` at 192.168.1.1

## What we did

Enabled the Nighthawk telnet backdoor (telnetenable magic UDP packet, admin/password,
via `bkerler/netgear_telnet`), got a **root BusyBox shell**, and dumped the full stock
NVRAM before flashing — because the per-device RF calibration only exists on the live
device and its stock firmware.

## Why it matters

OpenWRT issue **#20514** (dead 5GHz on R8000) is caused by missing brcmfmac
calibration (`brcmfmac43602-pcie.txt`/`.clm_blob`). Community consensus (#20514
@brada4): *"they are not files, they are embedded in the driver binary"* — i.e. treated
as unrecoverable. **We recovered the device-specific half directly from the router.**

## What we extracted (`extracted/nvram-stock.txt`, 2251 lines)

Three radios in Broadcom `N:` nvram format (`0:`, `1:`, `2:`), `sromrev=11`,
`boardtype=0x0665`:

- **`2:pa5ga0/1/2`** — full 5GHz PA calibration (2 five-GHz radios: idx 0 and 2)
- **`1:pa2ga0/1/2`** — 2.4GHz PA calibration (radio idx 1)
- **`maxp5ga*` / `maxp2ga*`** — max-power tables per band/chain
- per-radio `macaddr` (…F1:38/37/36), `ccode`, `regrev`, `boardflags`, `boardrev`
- **137 calibration-bearing keys total**

## Flash map (`extracted/proc-mtd.txt`)

`mtd1 nvram` · `mtd4 board_data` · `mtd2 linux` · `mtd3 rootfs` · `mtd17 brcmnand`
(board_data + card SROM survive a firmware flash; the stock `.chk` is saved locally
for offline CLM/driver extraction — `images/R8000-V1.0.4.88_10.1.88.chk`).

## Next (the OpenWRT-plus build)

1. Format `0:/1:/2:` cal → `brcmfmac43602-pcie.txt` (per-device nvram) for the image.
2. Extract the chip-generic `.clm_blob` offline from the stock `wl` driver (binwalk `.chk`).
3. Bake nvram + clm + radio-hang watchdog + 5GHz channel defaults + flow-offload into
   an OpenWRT 25.12.5 R8000 image (ImageBuilder for files/config; buildroot only if we
   patch C).
