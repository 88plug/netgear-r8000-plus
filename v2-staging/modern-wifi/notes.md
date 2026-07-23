# Modern roaming / steering / QoS -- investigation notes

Device: NETGEAR R8000, OpenWrt 25.12.5 r33051-f5dae5ece4 (bcm53xx/generic),
3x BCM43602 (brcmfmac), root SSH, 192.168.1.1. Investigated live on the
router plus this tree's OpenWrt source
(`/home/andrew/netgearr8000/openwrt`) and the pre-built ImageBuilder package
index (`openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64/.packageinfo`).

Workstream scope: roaming/handoff (802.11r/k/v), client-steering daemon,
OWE, SQM/QoS. Base WPA3-SAE/802.11w is a sibling workstream
(`../wpa3/`) -- its findings on the wpad package and hostapd build are
independently corroborated below (both investigations reached the same
conclusion from different angles).

## 1. brcmfmac AP-mode capability -- what actually works

Method: `iw phy phyN info` for driver/firmware capability, `dmesg | grep
brcmf` for firmware identity, and `strings /usr/sbin/hostapd` on the live
binary to confirm which features are actually compiled in (not just what a
`.config` template file *suggests* -- this OpenWrt tree's hostapd
`Makefile` builds via `DRIVER_MAKEOPTS` command-line overrides on top of a
base `.config`, so the static `files/hostapd-basic.config` template alone
is not reliable evidence of the real feature set -- see the "correction"
note in section 2).

| Feature | Status | Evidence |
|---|---|---|
| 802.11r (Fast BSS Transition) | **Works** | `ft_over_ds`, FT R0KH/R1KH, `ft_psk_generate_local`, `pmk_r1_push` strings present in `/usr/sbin/hostapd` |
| 802.11k (RRM / neighbor reports) | **Works** | `rrm_neighbor_report`, `rrm_beacon_req`, `neighbor_report_tx`, `rrm_nr_get_own/set/list` strings present |
| 802.11v (WNM / BSS Transition Mgmt) | **Works** | `bss_transition_query_rx`, `bss_transition_request_tx`, `bss_transition_response_rx`, `disassoc_imminent_rssi_threshold` strings present |
| MBO (Multiband Operation) | **Works**, not used here | `MBO-CELL-PREFERENCE`, `mbo_cell_capa`, `mbo_cell_data_conn_pref` strings present; requires PMF (802.11w) on the BSS or hostapd refuses to start it -- not enabled in this workstream's config, see section 5 |
| OWE (Enhanced Open) | **Works**, incl. transition mode | Full `owe_transition_ifname/ssid/bssid`, `owe_group(s)`, `owe_ptk_workaround` string set present |
| Multi-BSS (multiple SSIDs/radio) | **Works**, up to 4/radio | `iw phy phyN info` -- "valid interface combinations: `#{ AP } <= 4, total <= 4, #channels <= 1`" on all 3 phys |
| DFS (radar detection/CAC) | **Works**, offloaded | `iw phy0/phy2 info` -- "Supported extended features: ... `[ DFS_OFFLOAD ]`" |
| 802.11s (mesh) | **Does NOT work** | See section 1a |
| AP-VLAN / full dynamic VLAN | **Not advertised** | No "AP/VLAN" in `iw phy info` supported interface modes on any of the 3 phys; not needed for this task |
| WMM (baseline QoS categories) | **Works** (mandatory) | Implicit in HT/VHT AP operation; all 3 radios report HT20/HT40 (+VHT80 on the two 5 GHz radios) capability |
| Airtime fairness (NL80211 airtime API) | **Not verified / likely absent** | No `AIRTIME_FAIRNESS` (or any airtime-related) entry in "Supported extended features" on any of the 3 phys -- only `CQM_RSSI_LIST` (+`DFS_OFFLOAD` on the 5 GHz radios). `CONFIG_AIRTIME_POLICY` is compiled into hostapd, but a hostapd build flag existing doesn't mean the *driver* honors the corresponding NL80211 airtime commands -- brcmfmac is a full-MAC (firmware-scheduled) driver, not a mac80211 soft-MAC driver with kernel-side TXQ scheduling, so there is no local evidence this does anything real here. Not shipped in this workstream's config; WMM + SQM/cake (section 4) are the QoS wins that are actually verified. |

### 1a. Why 802.11s mesh is a hard no here (not a packaging choice)

Two independent, compounding reasons -- either alone would already rule it
out:

1. **Package**: `wpad-basic-mbedtls` (this tree's `hostapd/Makefile`,
   `LOCAL_VARIANT=basic`) never gets `CONFIG_MESH=y` -- only the `mesh` and
   `full` variants do (`wpad-mesh-mbedtls`, `wpad-mbedtls`). Confirmed on
   the live binary: `strings /usr/sbin/hostapd | grep -i mesh` returns
   **nothing** -- no `mesh_fwding`, `mesh_max_peer`, `MESH_PEER`, etc.
2. **Driver/firmware**: `iw phy phy0/phy1/phy2 info` -- "Supported
   interface modes" lists only `IBSS`, `managed`, `AP`, `P2P-client`,
   `P2P-GO`, `P2P-device` on **all three** radios. There is no `mesh point`
   mode at all. Even switching to `wpad-mesh-mbedtls` would not make 802.11s
   work -- brcmfmac's cfg80211 registration for this chip/firmware simply
   never advertises `NL80211_IFTYPE_MESH_POINT` as a supported interface
   type. This is a hard driver/firmware ceiling, not something a hostapd
   config or package swap can work around.

Firmware identity, for completeness (`dmesg`): `BCM43602/1 wl0: Sep 18 2015
03:30:01 version 7.35.177.56` -- a decade-old proprietary blob, no
newer version has ever been published for this chip. No mesh support was
ever added to it and none is coming.

### 1b. Pre-existing, unrelated hardware quirk (context for channel planning)

`phy0` (radio0) is regulatory-limited to channels 149-165 only (34-144 all
show `(disabled)`); `phy2` (radio2) is the inverse -- 36-144 usable
(several with `(radar detection)`), 149-165 disabled. This is the same
missing-CLM-blob issue the WPA3 workstream already documented
(`../wpa3/notes.md` section 4, tracked upstream as `openwrt/openwrt#20514`
/ `#19333`) -- not something this workstream can or should fix, but it
constrains which 5 GHz channels usteer's band-steering can actually offer a
client on each of the two 5 GHz radios. Noted here because it's directly
relevant to steering behavior, not just channel selection.

## 2. Correction to a commonly-repeated claim: `wpad-basic-mbedtls` is enough

Some older OpenWrt-era guidance (and the OpenWrt wiki's usteer page as
currently written) says 802.11k/r/v need the "full" wpad variant, not
"basic". That was true on older releases where the `basic` `.config`
template genuinely left `CONFIG_WNM` unset. **On this tree (OpenWrt
25.12.5, hostapd `v2.12-devel` snapshot `ca266cc2`, 2025-08-26), that's no
longer accurate** -- verified two ways, matching the WPA3 workstream's
independent finding on the SAE/MFP side:

- Source: `package/network/services/hostapd/Makefile` builds every variant
  via `DRIVER_MAKEOPTS` command-line overrides layered on top of the base
  `.config` file, and 802.11v/WNM support has since been merged into
  hostapd's mainline `ap_mgmt` handling rather than staying behind a
  separate opt-in flag in this snapshot.
- Binary: `strings /usr/sbin/hostapd` on the live device shows the full
  `bss_transition_*`/`rrm_*`/`ft_*`/`owe_*` symbol sets present (section 1
  table) in the exact `wpad-basic-mbedtls` package v1 already ships.

**No wpad/hostapd package change is needed for this workstream.** See
`imagebuilder-packages.md`.

## 3. Fast roaming / handoff (802.11r/k/v) -- honest impact assessment

Per the task framing: 802.11r/k/v roaming benefit is fundamentally a
**between-AP** thing. Here's exactly how that plays out on this specific
device and the current sibling-workstream SSID layout:

- The WPA3 workstream's current draft (`../wpa3/etc/config/wireless`) uses
  **three different SSID names** -- `R8000-5G-WPA3`, `R8000-5G`,
  `R8000-2G` -- one per band/security tier. 802.11r/k/v (and usteer's
  steering) only have an effect **between BSSes sharing the same SSID**.
  With three distinct names, there is currently no same-SSID BSS pair
  anywhere on this single device to roam or steer between.
- **Today, with that layout**: 802.11r/k/v cost nothing to enable and
  future-proof a second physical AP later broadcasting a matching SSID
  name (classic multi-AP ESS roaming -- this is the scenario 802.11r/k/v
  were actually designed for). No measurable single-device benefit yet.
- **If real, immediate steering across this device's own 3 radios is
  wanted now**: the SSID needs to be the same string across the radios
  meant to share a roaming/steering domain (e.g. unify `R8000-5G` and
  `R8000-2G` into one name broadcast on both bands). That is a naming
  decision for whoever owns the final SSID scheme, not something imposed
  here -- both options are laid out with copy-pasteable config in
  `etc/config/wireless-roaming.additions` (PART 1).

Config (options to add to each existing WPA3-workstream `wifi-iface`
section, not a new file) is in `etc/config/wireless-roaming.additions`.

## 4. Bufferbloat / QoS -- SQM (sqm-scripts + cake)

This is the workstream's highest-confidence, highest-impact item, and it's
completely independent of everything above: it's a WAN-side `tc`/cake qdisc
change, not a wireless-driver feature, so none of brcmfmac's limitations
apply. WAN net-device confirmed live as `wan` (a real device name on this
board, not an alias -- `board.json`'s `network.wan.macaddr` matches
`/sys/class/net/wan/address` on the router).

Package: `sqm-scripts` (auto-pulls `kmod-sched-cake`; both confirmed to
exist in this exact target's package feed, see `imagebuilder-packages.md`).
Config: `etc/config/sqm`, using the `cake` qdisc via `piece_of_cake.qos`
(the current recommended script, per OpenWrt's own SQM guide) rather than
older `fq_codel`/`simple.qos` -- cake's per-host fairness and DSCP-aware
tin classification is a strict upgrade for a home-router link.

**Download/upload bandwidth in the shipped config are placeholders (`0`)
and must be set from a real measured speed test before this does anything
useful** -- see the file's header comment for the measurement/tuning
procedure. Link-layer `overhead`/`linklayer` default to a DOCSIS/cable
profile (22 bytes, ethernet framing) as the most common consumer WAN type
for this router; the file documents the full overhead table (DOCSIS/pure
Ethernet/PPPoE-VDSL2/bridged-VDSL2/ADSL) so it can be corrected to the
actual WAN type in one edit.

Also considered: `qosify` (`2024.09.20~1501e0935...-r1`, "A simple QoS
solution based eBPF + CAKE") exists in this same feed and is a newer,
Felix-Fietkau-maintained alternative built on eBPF classification instead
of `sqm-scripts`' iptables-based approach. Not selected here: `sqm-scripts`
is the far more battle-tested, widely-documented default (this is what the
task asked to evaluate), and `qosify`'s eBPF path needs
`@HAS_BPF_TOOLCHAIN`/`@NEED_BPF_TOOLCHAIN` -- an extra build-time
dependency with less field history on bcm53xx specifically. Worth a look
later if `sqm-scripts`' CPU cost turns out to be a problem on this
dual-core Cortex-A9 (unverified either way -- no load test was run as part
of this pass).

## 5. usteer vs dawn -- which steering daemon

Both `dawn` and `usteer` exist in this exact target's package feed
(confirmed via `.packageinfo`, not assumed):

| | `usteer` | `dawn` |
|---|---|---|
| Version (this feed) | `2025.10.04~1d6524c6...` | `2025.11.07~7414c34a...` |
| Maintainer | David Bauer -- current OpenWrt core/packages maintainer | Nick Hainke, Berlin Open Wireless Lab (independent upstream project) |
| Dependencies | `libubox libubus libblobmsg-json libnl-tiny` | `libubus libubox libblobmsg-json libuci libgcrypt libiwinfo umdns` |
| Inter-AP transport | Own lightweight UDP protocol over `network` (ubus-integrated) | mDNS (`umdns`) service discovery |
| Steering mechanism | Probe/assoc accept-reject + 802.11v BSS-Transition-Management, tunable "aggressiveness" 0-4 | Similar BTM-based approach, plus its own "hearing map"/scoring system |

Decision: **usteer**. Reasoning:

- Meaningfully smaller footprint (no `umdns`/mDNS discovery daemon, no
  `libgcrypt`) on a device with ~250 MB RAM and a dual-core Cortex-A9 --
  confirmed via `free -m` on the live router (`total 249420` kB).
- Actively maintained inside the OpenWrt project itself, not a separate
  upstream with its own release cadence to track.
- Its config model (single `config usteer` instance auto-discovering every
  local hostapd BSS via ubus) maps directly onto this device's actual
  topology: one hostapd process group across three physical radios, no
  need for dawn's UMDNS-based multi-host discovery for a single box today.
- Both have a LuCI app in the feed (`luci-app-usteer` / `luci-app-dawn`) --
  no difference there.

`dawn` is not a bad choice and remains documented in
`imagebuilder-packages.md` as a fallback -- it may be worth revisiting if a
future multi-AP setup mixes in non-OpenWrt devices where dawn's
UMDNS-based discovery has broader interop precedent than usteer's ubus
protocol. Not a concern for an OpenWrt-only fleet.

Config: `etc/config/usteer`. Requires `ieee80211k`/`bss_transition`/
`rrm_neighbor_report` on the target SSIDs (section 3 /
`etc/config/wireless-roaming.additions`) to have a non-disruptive steering
path -- without them usteer can still load/signal-kick, but only by
outright rejecting/disconnecting, which is a worse client experience.

## 6. Files in this directory

- `notes.md` -- this file
- `imagebuilder-packages.md` -- PACKAGES additions (`usteer`, `sqm-scripts`,
  optional LuCI apps), evidence, and the merged `make image` command
- `etc/config/usteer` -- deployable usteer config (single instance, whole
  device)
- `etc/config/sqm` -- deployable SQM/cake config for the `wan` interface
  (bandwidth placeholders MUST be filled in before use, see section 4)
- `etc/config/wireless-roaming.additions` -- **not a standalone UCI file**;
  documents the exact 802.11r/k/v options to add to the WPA3 workstream's
  existing `wifi-iface` sections, the SSID-alignment decision needed for
  real cross-band steering (section 3), and two new standalone
  `wifi-iface` sections for an OWE ("Enhanced Open") transition-mode
  network pair

## 7. What was deliberately left out, and why

- **802.11s mesh** -- hard driver/firmware ceiling, see section 1a.
- **MBO** -- compiled in and available, but no clear home-AP use case here
  and adds a PMF-coordination dependency with the WPA3 workstream for no
  demonstrated benefit; see section 1 table.
- **Kernel/NL80211 airtime fairness** -- not verified as functional on this
  driver (section 1 table); shipping a config option that silently no-ops
  would violate the task's own instruction not to configure things the
  driver can't honor. WMM (already mandatory/on) + SQM/cake (section 4) are
  the QoS mechanisms actually verified to work.
- **Multi-AP (WiFi Alliance EasyMesh-adjacent `multi_ap` hostapd option)**
  -- present in the schema (`wireless.wifi-iface.json`) and the hostapd
  binary, but is a full controller/agent onboarding protocol out of scope
  for a single-device roaming/steering/QoS pass; not evaluated here.
