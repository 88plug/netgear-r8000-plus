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
- `image-files/etc/init.d/nvram` (ImageBuilder FILES override)

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

_(v2 on-device verification results appended after sysupgrade.)_
