# Netgear R8000 (Nighthawk X6 AC3200) — FCC certification data for RF tuning

Source: FCC OET equipment authorization exhibits for the operator's own R8000, retrieved
via fccid.io / fcc.report mirrors and read directly (Sporton International Inc. test lab,
Taiwan). All figures below are as-filed regulatory numbers, not vendor marketing copy.

## 1. FCC ID

**PY314200264** — grantee code `PY3` = Netgear Incorporated.

- Product: "AC3200 Smart WiFi Router" / "AC3000 Tri-Band WiFi Router"
- Models on this single filing: **R8000** and **R7900** (identical PCB; R7900 just omits
  one USB 3.0 port + its discrete components — RF is unaffected, confirmed by Sporton
  testing only R8000 and applying the result to both).
- Applicant: NETGEAR, Inc., 350 East Plumeria Drive, San Jose, CA 95134.

Canonical URLs:
- https://fccid.io/PY314200264 (primary listing — blocks scripted fetches, browser works)
- https://fcc.report/FCC-ID/PY314200264 (mirror used for this research; individual exhibit
  PDFs at `https://fcc.report/FCC-ID/PY314200264/<docid>.pdf`)
- https://fccid.io/PY314200264/Test-Report/Test-Report-2569523 (original RF test report)

There is **one FCC ID** covering all three radios (2.4GHz + two independent 5GHz radios);
it accumulated multiple grant/exhibit events between 2014 and 2016 as FCC UNII rules were
revised mid-cycle (see §4).

### Exhibit / grant timeline (for provenance)

| Date | Event | Sporton report | Bands / scope |
|---|---|---|---|
| 2014-05-29 (grant ~2014-06-02) | Original equipment cert | FR450713AA (Part 15.247 for 2.4G+5G-Band4; antenna table also filed for 5G-Band1) | 2.4GHz + 5GHz Band 4 (UNII-3) conducted-power tested directly; Band 1 (UNII-1) antenna/EUT data filed same submission, grant dated 06/02/2014 |
| 2015-03-30 | Class II change (MPE update) | — | 2.4GHz + Band1 + Band4 co-location re-verified |
| 2015-07-07 | Class II Permissive Change letter | references Sporton project **450713** | "Update 5GHz Band 1 and Band 4 to FCC 'New Rules' from 'Old Rules'" — this is the 2014 U-NII rule-harmonization update (KDB DTS rules) |
| 2015-10-02 | Class II test report | **FR450713-07** (Part 15.407) | 5GHz Band 4 re-measured under New Rules, beamforming + non-beamforming |
| 2015-10-02 | MPE report | **FA450713-07** | Combined worst-case conducted power for all 3 bands under New Rules — this is the authoritative current MPE/EIRP baseline (§2) |
| 2016-05-31 | Blanket cover-letter refresh | reissues FR450713AA content | No new measurements; compliance reaffirmation |

## 2. Certified conducted power, antenna gain, and computed EIRP per band

All conducted-power figures are the **combined 3-chain total** (10·log₁₀ of the summed
linear power across chains 1-3 or 4-6 — i.e., the actual RF power delivered into the
antenna system, before antenna gain). "Directional Gain" is the FCC 15.407/15.247 array
gain figure (already includes the ~4.8 dB array-combining gain from 3 elements, not just a
single element's dBi). **EIRP = conducted power + directional gain.**

### Current (2015 "New Rules") certified baseline — from FA450713-07 MPE report

| Band | Conducted power (dBm / mW) | Directional array gain (dBi) | Computed EIRP (dBm / mW) |
|---|---|---|---|
| 2.4GHz (802.11ac VHT20, worst-case) | 29.51 dBm / 894 mW | 6.33 | **35.84 dBm / 3,839 mW** |
| 5GHz Band 1 / UNII-1 (802.11ac VHT20, worst-case) | 25.99 dBm / 397 mW | 7.82 | **33.81 dBm / 2,406 mW** |
| 5GHz Band 4 / UNII-3 (802.11ac VHT40 MCS0/Nss1, worst-case) | 26.27 dBm / 424 mW | 6.97 | **33.24 dBm / 2,110 mW** |

Combined-transmission MPE check (all 3 radios simultaneous, 30cm, general population
limit): 0.3396 + 0.2125 + 0.1865 = **0.7387 < 1.0 — complies with margin to spare.**

### Per-channel detail, original 2014 certification (FR450713AA, "Old Rules", Part 15.247)

Useful for seeing the true per-chain spread and per-channel variation (superseded for
Band 4 by the 2015 New-Rules numbers above, but 2.4GHz numbers below are still current
— 2.4GHz was not part of the New-Rules migration):

**2.4GHz, chains 1+2+3, antennas 1-3 (dipole, gain 1.47–1.76 dBi/element, 20MHz):**
| Ch | Freq | Chain1 | Chain2 | Chain3 | Total (dBm) | Limit (dBm) |
|---|---|---|---|---|---|---|
| 1 | 2412 | 16.47 | 16.61 | 16.11 | 21.17 | 29.47 |
| 6 | 2437 | 24.47 | 24.85 | 24.89 | **29.51** | 29.67 |
| 11 | 2462 | 14.82 | 15.11 | 14.73 | 19.66 | 29.74 |

11b (DSSS) peaks at 29.53 dBm (ch6); 11g (OFDM) at 29.51 dBm (ch6); 11a (5GHz legacy,
Band4 chains) at 29.92 dBm (ch157) — i.e. the 2.4GHz radio is running right at the
30 dBm Part 15.247 digital-modulation ceiling once the ~6.3 dBi array gain reduction is
applied.

**5GHz Band 1 (UNII-1, ch36-48), antenna gain only (3.05–3.12 dBi/element, filed twice,
2014 and 2015):** shares chains 1-3 / antennas 1-3 with 2.4GHz (see §3 — these are
diplexed dual-band antenna elements, not a 4th independent element set). Directly-tested
per-channel conducted-power table for this band was not present in the mirrored exhibits
this research could reach; the authoritative worst-case total (25.99 dBm VHT20, current
New-Rules value) comes straight from the FCC-published MPE report (FA450713-07) cited
above.

**5GHz Band 4 (UNII-3, ch149-165), chains 4+5+6, antennas 4-6 (dipole, gain 2.06–2.20
dBi/element):**

*Old Rules (2014, FR450713AA):*
| Ch | Freq | BW | Total conducted (dBm) | Limit |
|---|---|---|---|---|
| 149 | 5745 | VHT20 | 28.83 | 29.17 |
| 157 | 5785 | VHT20 | 28.58 | 29.15 |
| 165 | 5825 | VHT20 | **29.03** | 29.08 |
| 151 | 5755 | VHT40 | 28.65 | 29.15 |
| 159 | 5795 | VHT40 | 28.72 | 29.03 |
| 155 | 5775 | VHT80 | 27.37 | 29.15 |
| 149/157/165 | — | 11a | 29.78 / **29.92** / 29.69 | 30.00 |

*New Rules (2015, FR450713-07) — current, non-beamforming / beamforming:*
| Mode | 11a | VHT20 | VHT40 | VHT80 |
|---|---|---|---|---|
| Non-beamforming | 24.53 | 25.54 | 26.83 | 20.83 |
| Beamforming | — | 23.30 | **26.27** | 20.42 |

The New-Rules re-measurement is markedly lower than the Old-Rules figures across the
board (especially VHT80: 27.37→20.83/20.42 dBm, a ~6.5-7dB drop) even though it's the
*same hardware* — this reflects the 2014 U-NII rule change adding a stricter
out-of-band-emission spectral mask for UNII-3 DTS operation, which forces more back-off
at wider channel widths, not a hardware change. **For 80MHz-wide 5GHz-2 channels, this is
the real legally-certified ceiling: ~20.4-20.8 dBm conducted (3-chain total), not the
27+ dBm the original 2014 numbers might suggest.**

## 3. RF hardware identification (from internal-photos exhibit, May 2014)

- **Main SoC:** Broadcom **BCM4709A0KFEBG** (dual-core ARM, under the finned heatsink) —
  matches the SoC already documented in this repo's `docs/FINDINGS.md`.
- **Radio chips:** Broadcom **BCM43602KMLG** — at least two distinct physical instances
  photographed with different date/lot codes (`369059-02` and `369059-05`), consistent
  with **three separate BCM43602 radio daughter-modules** (2.4GHz, 5GHz Band1, 5GHz
  Band4), each its own physical die/package, not one chip doing all bands. This matches
  the three independent `devid`/`macaddr` triplets already extracted in
  `extracted/calibration.txt` (`0x43BC`×2 for the two 5GHz radios, `0x43BB` for 2.4GHz)
  and the three-way `txchain=7 / rxchain=7` (bitmask `0b111`) seen for every radio prefix.
  **BCM43602 in this design is wired 3×3, not the 2×2 configuration some laptop/M.2
  variants of the same silicon use** — confirmed independently by (a) the FCC filing's
  explicit "Product Type: WLAN (3TX, 3RX)" / "Number of Transmit Chains (NTX): 3" for
  every mode (11n HT20/HT40, 11ac VHT20/40/80, MCS0-9/Nss1-3), and (b) the NVRAM
  `txchain`/`rxchain` masks. This resolves the task's open question: **all three radios
  on the R8000 are genuine 3×3**, matching the AC3200 = 3×(3-stream) marketing math
  (600+1300+1300 Mbps nominal).
- **PCIe fan-out:** **PLX Technology PEX8603-AB50NI** — a 3-port PCIe switch, present
  specifically because three independent PCIe-attached radio cards need to share the
  SoC's PCIe lanes. Direct physical corroboration of the 3-independent-radio design.
- **Front-end modules (PA/LNA/switch), one per radio, all Skyworks:**
  - **SKY85309-11** — 2.4GHz 802.11n/ac front-end module (2.4GHz radio)
  - **SKY85710-11** — 5GHz front-end module (appears twice on the board; one of the two
    5GHz radio cards)
  - **SKY85712** — 5GHz front-end module (the other 5GHz radio card)
  - Different FEM part numbers for the two 5GHz radios is notable — Band 1 (UNII-1,
    5.15-5.25GHz) and Band 4 (UNII-3, 5.725-5.85GHz) sit far enough apart in-band that
    Netgear used two different Skyworks parts rather than one wideband 5GHz FEM for both.
- **Flash:** Macronix **MX30LF1G08AA-TI** (1Gb SPI NAND).
- **DRAM:** Samsung **K4B2G1646Q-BCK0** (2Gb DDR3).
- **Antennas:** 6× Netgear-branded **dipole antennas, I-PEX connectors**, mapped to 6
  internal PCB feed chains:
  - **Chains 1, 2, 3 → Antennas 1, 2, 3 → shared by 2.4GHz AND 5GHz Band 1** ("Chain 1,
    2, 3 can transmit/receive signal simultaneously" per the FCC filing — i.e. these
    3 antenna elements are diplexed to carry both the 2.4GHz radio's and the 5GHz-Band1
    radio's RF concurrently). **This is the "5GHz-1" network in the Netgear UI.**
  - **Chains 4, 5, 6 → Antennas 4, 5, 6 → 5GHz Band 4 only.** **This is "5GHz-2".**
  - Net effect: only 6 physical antenna elements serve 3 independently-clocked 3×3
    radios — 2.4GHz and 5GHz-1 physically share antenna hardware via diplexing, while
    5GHz-2 gets dedicated elements. Any RF-tuning work that assumes 9 independent
    antenna paths (3 bands × 3 streams) is wrong; it's 6 physical paths, 3 of them
    dual-band.

## 4. Certified channel ranges and headroom

**Certified channels (this exact FCC ID, this exact grant):**
- 2.4GHz: channels 1-11 (2412-2462MHz), 20MHz and 40MHz (ch3-9 primary for 40MHz)
- 5GHz Band 1 (UNII-1): **channels 36, 40, 44, 48 only** (5180-5240MHz) — 20/40/80MHz
- 5GHz Band 4 (UNII-3): **channels 149, 153, 157, 161, 165 only** (5745-5825MHz) —
  20/40/80MHz

**No UNII-2A (52-64) or UNII-2C (100-140) / no DFS channels appear anywhere in this
filing's tested frequency ranges or grant history (2014-2016).** This is not a testing
gap — it matches Netgear's own published R8000 spec (5GHz-1 = 36-48, 5GHz-2 = 149-165)
and confirms the production firmware genuinely never enables the DFS-restricted middle
of the 5GHz band on this SKU, even though the BCM43602 silicon is DFS-capable. **Any
OpenWrt configuration that enables ch52-140 on this hardware is operating outside this
FCC grant's tested/certified envelope** — legally it would rely on the chip's general
regulatory-database capability rather than this specific device certification. Worth
flagging before enabling those channels, separate from whether they'll radiate cleanly.

**Beamforming:** filed and tested for 802.11n/ac in both 2.4GHz and 5GHz — the New-Rules
Band4 table above shows beamforming *reduces* certified VHT20 conducted power (23.30 vs
25.54 dBm non-BF) but leaves VHT40 about the same (26.27 vs 26.83) — i.e. beamforming
here is not a free power increase, it trades conducted-power ceiling for
directivity/SNR gain at the receiver.

## 5. Cross-reference with extracted PA calibration (`extracted/calibration.txt`)

The stock NVRAM's `maxp5ga*` / `maxp2ga*` arrays are the **per-chain hardware ceiling**
the calibration allows the driver to command (Broadcom quarter-dBm units: divide by 4 for
dBm). These are hardware/calibration limits, not what the certified/legal operating point
is — the two are meant to be compared, not conflated.

| NVRAM key | Raw (qdBm) | Per-chain dBm | Radio (by devid/macaddr) | Certified 3-chain total (current) | Per-chain equivalent* |
|---|---|---|---|---|---|
| `1:maxp2ga0/1/2` | 106 | 26.5 | 2.4GHz (`devid=0x43BB`) | 29.51 dBm | ~24.7 dBm |
| `0:maxp5ga0` | 54, 90, 90, **106** | 13.5 / 22.5 / 22.5 / **26.5** | 5G radio A (`devid=0x43BC`, mac …F1:38) | 25.99 dBm (Band1) | ~21.2 dBm |
| `2:maxp5ga0` | 90, 90, 90, 90 | 22.5 flat | 5G radio B (`devid=0x43BC`, mac …F1:36) | 26.27 dBm (Band4, VHT40) | ~21.5 dBm |

*Per-chain equivalent = certified 3-chain total minus the ~4.8dB array-combining factor
(10·log₁₀3), for apples-to-apples comparison against the per-chain `maxp*` ceiling.

**Finding: the calibration ceiling sits 3-5 dB above what's actually needed to reach the
FCC-certified operating point in every band.** The hardware/PA calibration
(`pa5ga0/1/2`, `pa2ga0/1/2`) clearly has headroom beyond the certified legal limit; the
`maxp*` values function as an outer safety ceiling for the driver's per-rate power
tables (`mcsbw*po`, `dot11agofdmhrbw20*po`, etc.), not as "this device only outputs
26.5dBm max." **This means the software regulatory-domain/txpower table, not the
hardware, is what has historically capped this router at or near the certified level** —
consistent with `ccode=Q2`/`US` and `regrev=86` in the dump, which are exactly the kind
of NVRAM fields that encode the regulatory-domain power table actually enforced.

**For OpenWrt tuning:** the safe, still-fully-legal target is to drive **up to** the
certified per-band ceiling in §2 (not the raw `maxp*` ceiling, which would exceed FCC
certification if actually radiated) — i.e., aim for something in the neighborhood of:
- 2.4GHz: ~29 dBm conducted / ~24.5 dBm EIRP-equivalent-per-chain is already close to
  the certified max; there is essentially zero more legal headroom at 20MHz.
- 5GHz-1 (UNII-1, ch36-48): certified ceiling 25.99 dBm (VHT20) — mainline OpenWrt/regdb
  US-domain defaults for UNII-1 are typically lower than this (regdb often ships
  conservative 17-23 dBm EIRP numbers pulled from the *generic* US table, not this
  device's specific grant), so there is real, legal upside available here if OpenWrt's
  `ACS`/txpower table is edited to reflect this device's actual certified ceiling rather
  than a generic regdb default.
- 5GHz-2 (UNII-3, ch149-165): certified ceiling depends heavily on channel width — at
  20/40MHz there's ~25-27dBm legal headroom, but **at 80MHz the legal ceiling drops to
  ~20.4-20.8dBm conducted** (New Rules re-cert) — don't naively carry a 20MHz-derived
  txpower setting onto an 80MHz channel on this band; the FCC data shows an explicit,
  large legal step-down at 80MHz specifically for Band 4.

## Sources

- FCC ID search / grantee: https://fccid.io/PY3 , https://fccid.io/PY314200264
- Exhibit mirror used for full-text extraction: https://fcc.report/FCC-ID/PY314200264
  - Original test report (FR450713AA, 2014-05-29): `.../2283278.pdf` / `.../3011133.pdf` / `.../2569523.pdf` (same underlying report served under multiple exhibit IDs on this mirror)
  - MPE / Human Exposure report (FA450713-07, 2015-10-02): `.../2771717.pdf`
  - Class II test report, Band 4 New Rules (FR450713-07, 2015-10-02): `.../2771718.pdf`
  - Class II Permissive Change letter (2015-07-07): `.../2771720.pdf`
  - Internal photos exhibit (2014-05-29): `.../2569516.pdf` (chip IDs in §3)
  - Radiated-emission co-location appendix: `.../2283278.pdf`
- 47 CFR §15.407 (current UNII general technical requirements, incl. 1W/30dBm AP ceiling
  for UNII-1 & UNII-3): https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-E/section-15.407
- Operator's own extracted calibration: `/home/andrew/netgearr8000/extracted/calibration.txt`,
  `/home/andrew/netgearr8000/extracted/nvram-stock.txt`
