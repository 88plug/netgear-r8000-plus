# netgear-r8000-plus: what's different from a stock OpenWrt bcm53xx build

Everything below was found by direct hardware testing on the operator's own
R8000 — driver source reading, live register/firmware testing, and real-client
WiFi verification — not assumed from documentation. Each item cross-references
the full technical writeup so nothing here needs re-deriving.

## Sources mined

- **OpenWrt's own issue tracker** ([openwrt/openwrt](https://github.com/openwrt/openwrt/issues)) —
  the two open bugs this repo actually root-caused and fixed/worked around
  (#20514, #21655), plus one genuine open upstream wall this repo confirmed but
  could not close (#18743).
- **Mainline `torvalds/linux`** — read directly to confirm the multi-BSS driver
  bug (`brcmf_cfg80211_request_ap_if()`) is still present and unpatched today,
  and to rule out any chip-ID gating that would make the fix R8000-specific.
- **Broadcom's own GPL-compliance source drops** (`RMerl/asuswrt-merlin`,
  `FreshTomato-Project/freshtomato-arm`, `release/src-rt-6.x.4708/`) — mined for
  the FA/CTF hardware NAT-accelerator register interface; see
  `hwoffload-research/graveyard-vendor/notes.md` for full provenance.
- **Real-client testing** (an actual laptop associating over the air) surfaced
  the WPA3-SAE bug that synthetic/config-only verification had missed.

## Bugs fixed

| # | Fix | Source | Why it matters |
|---|-----|--------|-----------------|
| 1 | 5GHz radios fail to load firmware (`brcmf_c_process_clm_blob` error -2, "dead 5GHz") | [CONFIRMED⚡] [openwrt/openwrt#20514](https://github.com/openwrt/openwrt/issues/20514) — root-caused here as a missing `netgear,r8000` bcm53xx nvram-init entry, not a chip/driver limit | The board's own calibration data was never wired into the bcm53xx nvram-init table. `patches/0001-nvram-bcm53xx-add-netgear-r8000-43602.patch` adds this unit's own extracted calibration. All 3 radios now confirmed live. |
| 2 | Guest network / multi-SSID silently fails to bring up extra BSSes on this driver+firmware combo | [CODE] found by reading `brcmf_cfg80211_request_ap_if()` directly — no matching upstream issue found, but confirmed the same unpatched code path exists in current mainline | The driver returns early instead of falling through to the legacy `bsscfg:ssid` MBSS path when an `interface_create` iovar version query fails (older firmware). `patches/861` restores the fallback. Chip-generic — see "Device support beyond this router" in [README.md](README.md). |
| 3 | WPA3-SAE was configured and claimed working but never actually broadcast on this hardware | [CODE] found via real-client RSN-element scanning, not assumed from config | `brcmf_configure_wpaie: Invalid key mgmt info` fired on every boot; a live scan showed `PSK`/`PSK-SHA256` only, SAE silently absent — a real BCM43602/brcmfmac firmware limitation. Reverted all 4 interfaces to plain `psk2` (WPA2-PSK) rather than ship a security posture the hardware doesn't deliver. `ieee80211w` (MFP), `ieee80211k`, `bss_transition` all independently confirmed to still work and were kept. |
| 4 | `sysupgrade` silently resets config on this device family | [CONFIRMED⚡] [openwrt/openwrt#21655](https://github.com/openwrt/openwrt/issues/21655), open upstream, not fixed here (not this project's driver stack to fix) | Worked around, not patched: `docs/RUNBOOK.md`'s flash procedure makes a verified-non-empty `sysupgrade -b` backup mandatory before every flash. Every version since has been reproducibly re-flashed without config loss. |

## Hardening (walls confirmed real, not left ambiguous)

| Item | Verdict | Evidence |
|---|---|---|
| OWE (Enhanced Open) | **Confirmed unfixable** — firmware wall, not a driver bug | No `WPA3_AUTH_OWE` support in the firmware blob itself. `docs/FINDINGS.md` §9, §12. Removed rather than shipped half-working. |
| DFS channels | **Confirmed permanently unsupported** — firmware wall, not config/driver | Firmware returns raw `BCME_UNSUPPORTED` (index 23 in brcmfmac's own error table), recovered via a diagnostic-instrumented driver rebuild. Four independent live tests converge (clm_blob re-pairing, two regulatory-domain tests, the raw firmware error code itself). `docs/FINDINGS.md` §16–18. |
| `pstore`/`ramoops` kernel debug support | **Genuine open upstream wall**, not shipped | Both the pstore code and the devicetree reservation work correctly, but the resulting locally-built kernel hits a `struct module` ABI mismatch loading `brcmfmac` — reproduces even swapping in the byte-identical known-good module, isolating it to the *kernel build itself*, not this project's changes. Matches open, unresolved [openwrt/openwrt#18743](https://github.com/openwrt/openwrt/issues/18743). Build-order/race was tested directly as an alternate hypothesis and falsified. `docs/FINDINGS.md` §15. Router reverted after every test; no regression shipped. |
| FA/CTF hardware NAT accelerator | **Hardware fully proven working; architecturally unreachable from this software stack** | Every register-level mechanism (GMAC table-init, indirect data read/write, switch-side enable, a complete real-connection NAPT row) was proven correct against real hardware and real traffic. Root cause for why the FA hit-counter never moved: the per-packet consultation hook (`ctf_forward()`) only exists in Broadcom's closed-source vendor Ethernet driver (`et_linux.c`), never in mainline `bgmac.c`. Reaching real acceleration would mean porting that closed vendor driver's hook into mainline — a different, much larger project than a flowtable-offload backend. Full driver (`hwoffload-research/fa-probe/fa_accel.c`) and investigation: `docs/WINS.md` "FA/CTF hardware accelerator" section. |

## Features added

| Feature | What it does |
|---|---|
| SQM (`cake`) with real-measured WAN bandwidth | `sqm.wan.download`/`upload` set from an actual measured link (SSH SOCKS-tunneled real `curl`, not BusyBox `wget`, to get past TLS/bot-detection false negatives), not left at a placeholder `0`. Live `tc qdisc show` confirms real shaping on both directions. |
| `radio-watchdog` | Escalating recovery for brcmfmac radio wedges — a real, observed failure mode on this hardware, not a hypothetical. |
| Guest network isolation | `wireless.guest.isolate=1` on the guest SSID, bridged separately from LAN. |
| `usteer` band-steering | Client steering across the 3 radios. |
| LED netdev-binding fix | LEDs correctly track the interfaces they're meant to represent. |
| `perf-tune` | Runtime performance tuning applied via the `v2-files` overlay at boot. |

## Explicitly out of scope (and why)

- **Real FA/CTF hardware acceleration.** The hardware and the driver are both
  proven correct; the missing piece is a mainline `bgmac.c` port of Broadcom's
  closed-source CTF datapath hook — a full Ethernet-driver project, not
  something a flowtable-offload backend can add. See "Hardening" above.
- **WPA3-SAE and OWE.** Both are real firmware limitations on this exact
  chip/firmware combination, not gaps this project's own code could close.
  Shipping a config that silently doesn't do what it claims is worse than not
  shipping it.
- **DFS channel enablement.** Firmware-level `BCME_UNSUPPORTED`, confirmed by
  four independent tests including the raw firmware error code. Not a config
  or driver fix away.

## Contribute by porting

Two of the fixes above are genuinely upstream-submittable, chip-generic, and
already confirmed to affect more than just this one router — see "Device
support beyond this router" in [README.md](README.md):

- **`patches/0001-nvram-bcm53xx-add-netgear-r8000-43602.patch`** — adds this
  board's own extracted calibration to bcm53xx's nvram-init table. Directly
  closes [openwrt/openwrt#20514](https://github.com/openwrt/openwrt/issues/20514)
  upstream.
- **`patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch`** — the
  `brcmf_cfg80211_request_ap_if()` MBSS fallback fix. Confirmed against current
  mainline `torvalds/linux`: the bug is still present and unpatched there.
  Affects every brcmfmac device running pre-`interface_create`-iovar firmware,
  not just this one.

Both patches are already isolated, single-purpose commits — cherry-picking
either into an upstream OpenWrt or mainline `brcmfmac` PR is a small, direct
port, not a rewrite.
