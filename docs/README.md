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

## Device support beyond this router (researched 2026-07-25)

`patches/861` (the multi-BSS/guest-network driver fix, [WINS.md](WINS.md)
headline win) was found and tested on this one R8000, but the code it touches
— `brcmf_cfg80211_request_ap_if()` in mainline's shared `brcmfmac` driver — has
**zero chip-ID checks**. It's the same function for every Broadcom FullMAC chip
brcmfmac supports. Confirmed directly against current `torvalds/linux` master:
the bug is still present and unpatched there today. Whether a given device hits
it depends on **firmware vintage, not chip model** — any brcmfmac device whose
loaded firmware predates the `interface_create` iovar (roughly pre-2016
Broadcom firmware builds) hits the same early-return-instead-of-fallback bug.

**Confirmed-affected hardware (same BCM43602 chip, sourced against
WikiDevi/DeviWiki hardware teardowns):**

| Device | BCM43602? | OpenWrt (bcm53xx) status |
|---|---|---|
| Netgear R8000 | yes (×3) | supported — this repo |
| Netgear R7900 | yes (×3, same board family) | supported |
| Asus RT-AC3200 | yes (×3) | supported since 2024-2025 |
| Netgear D7000 | yes (×2) | no mainline OpenWrt target found |
| Linksys EA9200 | yes (×3) | no mainline OpenWrt target found |
| TP-Link Archer C3200 | yes (×3) | no confirmed mainline support |

**Do not assume from model-number similarity — these are a *different* chip and
are unaffected:** Netgear R7000 (BCM4360), R7000P/R8000P (BCM4365/E), R8500/R8300/R7800
(BCM4366), R7500 (Qualcomm, no Broadcom radio at all). The R7000 in particular
is the one genuinely best-selling router in this family — conflating its
popularity with the actually-affected R8000/R7900/RT-AC3200 population would
overstate this by a wide margin. We're not doing that here.

**Provenance-searched, not just assumed novel** (11 queries across
lore.kernel.org, patchwork.kernel.org, bugzilla.kernel.org, GitHub, and the
Infineon community forum — Infineon engineers are brcmfmac's current upstream
maintainers): closest prior art is Ian Lin/Infineon's 2022 patch, which added
a fallback for a *different* failure point in the same function (the
`interface_create` *creation call* failing after a successful version query) —
not for the version-*query* itself failing, which is what this fix addresses.
No anticipating reference found for this specific fix; gaps: no access to
non-public Broadcom/Cypress SDK trackers, linux-wireless mailing list archive
searched via lore.kernel.org's web index only (not a full local mirror).

**Realistic scope, not a round number.** No sales figures exist for the
actually-affected models (R8000/R7900 were premium tri-band flagships, not
mass-market). Stock vendor firmware uses Broadcom's closed driver and never
hits this code path at all — only OpenWrt (or another mainline-brcmfmac-based
build) exposes it. The honest population is **OpenWrt users on BCM43602-class
tri-band hardware who want a working guest network or multi-SSID** — a real
but modest number, not "millions." The value of this fix is that it's a small,
clean, upstream-submittable 2-line change against the driver's own documented
design intent (confirmed on-list: brcmfmac's maintainer has stated the driver
is supposed to fall back to the legacy interface-creation path for older
firmware) — not its reach.

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
