# Wave 2 Research Index — 7 parallel threads

Status snapshot at 2026-07-23 12:07 PDT. This is a bookkeeping index, not a
technical synthesis — it exists so this wave's scope is on record even before
each agent's findings are hand-merged into `VERDICT.md` / `docs/FINDINGS.md`
by the coordinating session. **Do not confuse with the separate, independently
running first-wave FA hardware-probe synthesis** (`fa-probe/` — building
`fa_probe.c`/`Makefile`, will land its own go/no-go doc in that directory).

Filesystem check at index time: no new files appeared under
`hwoffload-research/` from any of the 7 threads below (six of seven were
scoped as report-back-only, so this is expected, not a gap). The only fresh
activity in `hwoffload-research/` in this window is the first-wave FA-probe
build (`fa-probe/fa_probe.c`, `fa-probe/Makefile`), which is out of scope for
this index per the task framing above.

## The 7 threads

| # | Topic | What was being checked | Status |
|---|-------|------------------------|--------|
| 1 | Live WiFi capability-string cross-check | Compare `iw phy info` output against claimed BCM43602 `brcmfmac` capabilities (WPA3/SAE, 802.11r/k/v, OWE) on the live bench unit, to confirm what the running radio actually advertises vs. what upstream docs/issue trackers claim | findings pending consolidation by main session |
| 2 | Read two previously-unreviewed notes files | `graveyard-openwrt/notes.md` (OpenWrt/mainline FA-driver graveyard mining) and `v2-staging/extras/dsa-switch/NOTES.md` (b53/SRAB switch specifics) — folded into main docs if not already | findings pending consolidation by main session (see independent read-through below) |
| 3 | UART/serial console hardware research | R8000 board-level UART header/pads, baud rate, pinout, and any CFE bootloader serial-console access path (recovery net item, ties into the "TFTP needs serial" caveat already in project `CLAUDE.md`) | findings pending consolidation by main session |
| 4 | BCM53012 switch chip broader errata research | Wider errata/bug search on BCM53012 beyond the already-documented EAP_MODE_SIMPLIFIED regression — any other known silicon or driver-level issues on this switch family | findings pending consolidation by main session |
| 5 | Remaining SROM/NVRAM calibration parameter cross-reference | Cross-check outstanding SROM/NVRAM calibration parameters (antenna/power tables, board-specific cal data) against what OpenWRT's `bcm53xx` target actually consumes vs. what stock Netgear firmware carries | findings pending consolidation by main session |
| 6 | Other unused hardware blocks on BCM4709 SoC | Survey of SoC blocks beyond the Flow Accelerator that are present in silicon but undriven/unused in both stock and OpenWRT firmware (crypto engine, other GMACs, etc.) | findings pending consolidation by main session |
| 7 | Upstream/mainline prior-art check for FA driver work | Fresh check of `openwrt/openwrt`, `torvalds/linux`, and standalone repos for any Flow-Accelerator driver work — substantially overlaps thread 2's `graveyard-openwrt/notes.md`, which already documents this in depth (see below); worth diffing the two reports for anything thread 7 turned up that predates or postdates that file | findings pending consolidation by main session |

## Independent cross-check of the two notes files (thread 2)

### `hwoffload-research/graveyard-openwrt/notes.md`

Conclusion is unambiguous: **no driver, register map, or partial decode of
the Northstar-family Flow Accelerator exists anywhere** — not in OpenWrt, not
in mainline Linux, not in any standalone GitHub repo. The single concrete
lead is a 2015 openwrt-devel email from Ian Kent (who owned an R8000)
speculating the FA sits behind the GMAC2/"GMAC-3" MMIO window, gated by a
PCIe-enumerated device `bgmac` never configures — never followed up, no
register offset ever published. The file carefully disambiguates this from a
*different*, real, merged "Flow Accelerator" reference in mainline
`b53_common.c`, which only applies to the later BCM58xx "Northstar Plus"
family (`is58xx()`) and explicitly does not cover BCM53012 (`is5301x()`, the
R8000's actual chip) — a conflation the notes flag as already present in
secondary web commentary, so worth guarding against in any doc that cites
this. OpenWrt's actual answer to bcm53xx NAT throughput is a CPU-side
RPS/IRQ-steering script (`fastnetwork`), not hardware offload. Bottom line for
this project: any FA driver work here would be net-new, unassisted by prior
art.

### `v2-staging/extras/dsa-switch/NOTES.md`

Documents the live b53-SRAB switch state on the bench unit: BCM53012 rev 5
detected correctly, standard R8000 port topology (lan1-4/wan/cpu@8, ports 5/7
disabled — present only in the shared DTS template for boards like the
Linksys EA9500 that use them). The one substantive finding is a
already-resolved regression: upstream commit `4227ea91e265` (backported to
6.12.30) broke standalone/non-bridged ports on Northstar switches via
`EAP_MODE_SIMPLIFIED`; OpenWrt patch
`701-net-dsa-b53-disable-EAP-setup-on-Northstar-switches.patch` fixes it and
is confirmed present and applied in this repo's 25.12.5 tree — explicitly
flagged as "no action needed, don't re-fix it." A benign first-probe-fails/
second-probe-succeeds boot-log pattern is also noted as harmless and not
worth re-investigating if seen again. Same bottom line as the graveyard file
on hardware NAT: bcm53xx has none upstream (per `openwrt/openwrt#7023`),
mitigated only by the software flow-offload approach documented in the
sibling `flow-offload/NOTES.md`.

These two files are consistent with each other and non-contradictory — both
converge on "no hardware NAT/FA path exists upstream for this chip family,
CPU-side mitigations are the only working answer today."
