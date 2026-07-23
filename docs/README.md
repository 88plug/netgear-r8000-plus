# Netgear R8000 → OpenWRT (the "plus" conversion)

Converting the operator's own **Netgear Nighthawk X6 R8000** (AC3200, tri-band,
BCM4709 + 3× BCM43602) from stock firmware to a **custom OpenWRT 25.12.5** build
that fixes defects the community considered unfixable — and tunes the radios to
their documented hardware ceiling.

## Current status (2026-07-23)

- **v1 SHIPPED & VERIFIED:** OpenWrt 25.12.5 running on the R8000. All 3 radios
  up; both 5GHz radios functional (29 channels each, VHT/AP-capable); live 5GHz
  scan works. The "dead 5GHz" (#20514) is not dead.
- **v2 IN BUILD:** LEDs, WPA3-SAE, newest 43602 firmware + extracted `clm_blob`,
  max-power/all-band unlock (to FCC-certified ceilings), modern roaming/steering
  (dawn, 802.11r/k/v), OWE, SQM/cake. Assembled from parallel workstreams under
  `v2-staging/`, flashed via `sysupgrade`.

## Docs

- **[RUNBOOK.md](RUNBOOK.md)** — operations: host/bench setup, router access
  (stock telnet + OpenWrt SSH), flashing (GUI / sysupgrade / nmrpflash / TFTP),
  recovery/unbrick, building images.
- **[FINDINGS.md](FINDINGS.md)** — technical findings: hardware (FCC-confirmed
  3×3), calibration extraction, root cause of #20514 + the fix, v1 results, FCC
  certified RF ceilings, and v2 results.

## Repo layout

```
CLAUDE.md              project context / framing
docs/                  these docs
patches/               upstreamable OpenWrt source patches (e.g. r8000 43602 init)
image-files/           FILES overlay baked into images (patched /etc/init.d/nvram)
v2-staging/            per-workstream v2 artifacts (leds, wpa3, firmware, maxpower,
                       modern-wifi, extras, fccid)
extracted/             calibration + boot evidence (nvram-stock.txt gitignored — secrets)
images/                *.chk images (gitignored) + sha256sums
openwrt/               OpenWrt v25.12.5 source clone (gitignored)
```

## Branch

All work on `openwrt-r8000-plus`.
```
