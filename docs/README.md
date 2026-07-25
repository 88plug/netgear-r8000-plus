# Netgear R8000 → OpenWRT (the "plus" conversion)

Converting the operator's own **Netgear Nighthawk X6 R8000** (AC3200, tri-band,
BCM4709 + 3× BCM43602) from stock firmware to a **custom OpenWRT 25.12.5** build
that fixes defects the community considered unfixable — and tunes the radios to
their documented hardware ceiling.

## Current status (2026-07-24)

- **v11 SHIPPED, FLASHED, VERIFIED LIVE** —
  `images/openwrt-25.12.5-r8000plus-v11-bcm53xx-generic-netgear_r8000-squashfs.chk`.
  Confirmed via SSH 2026-07-24: all 3 radios up, 4 SSIDs (`R8000` ×3 +
  `R8000-Guest`), **`psk2`/WPA2-PSK** (not WPA3-SAE — see below), guest client
  isolation (`wireless.guest.isolate=1`), **usteer** band-steering,
  **SQM/cake** with real-measured WAN bandwidth (91000/89000 kbit/s down/up,
  live `tc qdisc` shaping confirmed), multi-BSS driver fix (`patches/861`),
  working LEDs, `radio-watchdog`, LuCI. The "dead 5GHz" (#20514) is not dead —
  root cause and fix in [FINDINGS](FINDINGS.md) §3.
- **WPA3-SAE was removed, not shipped.** Real-client testing found it never
  actually broadcast on this hardware; reverted to plain WPA2-PSK (`psk2`) in
  v9. See [FINDINGS](FINDINGS.md) §14.
- **OWE (Enhanced Open) was removed, confirmed unfixable** — a firmware-side
  wall (no `WPA3_AUTH_OWE` support in the blob), not a driver bug. See
  [FINDINGS](FINDINGS.md) §9, §12.
- **No temp passphrase remains.** The `ChangeMe-R8000-2026` default from
  earlier versions was rotated out for real random passphrases; see
  [WINS.md](WINS.md) "v6.1" (2026-07-23). Don't reuse that string.
- Full version-by-version history (v1 → v11) — what shipped, what regressed,
  what was fixed: [WINS.md](WINS.md).

## Docs

- **[RUNBOOK.md](RUNBOOK.md)** — operations: host/bench setup, router access
  (stock telnet + OpenWrt SSH), flashing (GUI / sysupgrade / nmrpflash / TFTP),
  recovery/unbrick, building images.
- **[FINDINGS.md](FINDINGS.md)** — technical findings: hardware (FCC-confirmed
  3×3), calibration extraction, root cause of #20514 + the fix, multi-BSS
  driver fix, OWE/DFS hardware walls, the WPA3-SAE failure + fix, the
  pstore/ramoops build-environment wall, and FCC certified RF ceilings.
- **[WINS.md](WINS.md)** — chronological version log (v1 → v11): what
  shipped, what regressed, what was fixed, and the honest walls at each step.

## Repo layout

```
CLAUDE.md              project context / framing
docs/                  these docs
patches/               upstreamable OpenWrt source patches (e.g. r8000 43602 init)
v2-files/              canonical FILES overlay baked into images since v7
                       (patched /etc/init.d/nvram, guest network, perf-tune,
                       radio-watchdog, LED binding)
image-files/           DEPRECATED pre-v7 overlay, kept for history only —
                       see image-files/DEPRECATED.md
v2-staging/            per-workstream v2 artifacts (leds, wpa3, firmware, maxpower,
                       modern-wifi, extras, fccid)
v3-staging/            per-workstream v3+ artifacts (leds, perf)
hwoffload-research/    Broadcom switch/flow-accelerator (FA/CTF) hardware
                       offload research — see hwoffload-research/VERDICT.md
extracted/             calibration + boot evidence (nvram-stock.txt gitignored — secrets)
images/                *.chk images (gitignored) + sha256sums
backups/               pre-flash config backups (gitignored — live device config)
openwrt/               OpenWrt v25.12.5 source clone (gitignored)
```

## Branch

All work on `openwrt-r8000-plus`.
```
