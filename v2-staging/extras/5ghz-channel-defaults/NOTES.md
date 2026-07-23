# 5 GHz channel defaults -- notes

## Claim

- `radio0` = 5 GHz **upper** half only, hardware-locked to channel ≥149.
- `radio2` = 5 GHz **lower** half only, hardware-locked to channel ≤48
  without the missing CLM/regulatory blob (≤144 once that's supplied).
- `radio1` = 2.4 GHz.

This is not a mac80211/regdomain quirk to route around -- it's the physical
board design (two separate 5 GHz BCM43602 radios, each tuned/filtered for a
different sub-band), and it's baked into the kernel's own device tree for
this exact model. Verified three independent ways below.

## Evidence 1: the DTS itself (`ieee80211-freq-limit`)

`bcm4709-netgear-r8000.dts` in the 25.12.5 tree
(`arch/arm/boot/dts/broadcom/bcm4709-netgear-r8000.dts`):

```dts
&pcie0 {
	...
	wifi@0,1,0 {
		compatible = "brcm,bcm4366-fmac", "brcm,bcm4329-fmac";
		ieee80211-freq-limit = <5735000 5835000>;   /* radio0: 5735-5835 MHz */
		brcm,ccode-map = "JP-JP-78", "US-Q2-86";
	};
};

&pcie1 {
	...
	bridge@1,0 { wifi@0,0 { ... /* radio1: no freq-limit -> 2.4 GHz */ }; };
	bridge@1,2,2 {
		wifi@1,4,0 {
			ieee80211-freq-limit = <5170000 5730000>;  /* radio2: 5170-5730 MHz */
			brcm,ccode-map = "JP-JP-78", "US-Q2-86";
		};
	};
};
```

5735-5835 MHz covers channels 147-167 -- in practice, the non-DFS UNII-3 set
149/153/157/161/165. 5170-5730 MHz covers channels 34-146 -- UNII-1/2/2e,
non-DFS 36/40/44/48 plus the DFS range 52-144.

## Evidence 2: live `iw phy` on the operator's own flashed unit (25.12.5)

From `extracted/boot-results/iw-phy-info.txt` (this repo, captured 2026-07-23
on the actual router):

- **phy0** (radio0, MAC `...f1:38`): 34-144 all listed `(disabled)`;
  149/153/157/161/165 listed at `20.0 dBm` (enabled).
- **phy2** (radio2, MAC `...f1:36`): 36/40/44/48 listed at `20.0 dBm`
  (enabled); 52-144 and 149-165 all listed `(disabled)`.
- **phy1** (radio1, MAC `...f1:37`): 2.4 GHz, channels 1-11 at `20.0 dBm`,
  12-14 `(disabled)` (US channel set).

The 52-144 DFS range on phy2 shows in the frequency table (so the silicon/
regulatory limit does include it, matching the DTS's 5170-5730 span) but is
currently disabled -- consistent with `extracted/FINDINGS.md`'s finding that
the CLM/txcap regulatory blobs are still missing on this build (driver log:
`brcmf_c_process_clm_blob: no clm_blob available, device may have limited
channels available`), so DFS/CAC-gated channels don't activate yet. That's a
separate, already-tracked gap (see FINDINGS.md item 2), not something this
note's defaults need to solve -- the defaults below simply avoid parking a
radio on a channel range it can't currently use.

## Evidence 3: OpenWrt's own board-detection default `/etc/config/wireless`

Confirmed live via SSH on the router (25.12.5, this repo's flashed build) --
board detection (which reads the same DTS freq-limits through
`/lib/wifi/mac80211.sh`) already generates:

```
config wifi-device 'radio0'
	option band     '5g'
	option channel  '149'
	option htmode   'VHT80'

config wifi-device 'radio1'
	option band     '2g'
	option channel  '1'
	option htmode   'HT20'

config wifi-device 'radio2'
	option band     '5g'
	option channel  '36'
	option htmode   'VHT80'
```

i.e. OpenWrt already defaults radio0 to 149 and radio2 to 36 with zero
intervention -- `wireless.snippet` in this directory is that same default,
made explicit and documented so it survives a config regeneration/rewrite
without silently reverting to something invalid.

## Why this matters in practice: don't fight the split

OpenWrt issue **#13902** ("Channels 149 and 165 in the 5G WiFi network
cannot be used") is a good example of what goes wrong when someone assumes
one 5 GHz radio should serve the whole band and manually reassigns channels
without knowing about the split:
https://github.com/openwrt/openwrt/issues/13902

Key clarifications from that thread, applicable here:
- Channel 149 "becoming" 153 in the UI when using an 80 MHz-wide setup is
  correct display behavior (149 is the *control* channel of the 149/153
  40 MHz pair / the low quarter of an 80 MHz block starting at 149), not a
  bug: https://github.com/openwrt/openwrt/issues/13902#issuecomment-1802061185
- Channel 165 at 80 MHz width is invalid in most regdomains (165 only
  supports 20/40 MHz standalone in most countries) --
  a maintainer's comment specifically flags the R8000 case:
  *"OpenWrt enables different channels for each of the two 5G chips than
  the stock firmware does -- probably channels that are blocked by the
  fullmac-firmware or EEPROM"* --
  https://github.com/openwrt/openwrt/issues/13902#issuecomment-1802061185
- Power/regdomain guidance from the same thread, still good general advice
  for this board: avoid SRD 13 dBm/25 mW low-power channels, set
  `min_tx_power` sensibly, and prefer letting `auto` channel selection run
  within each radio's actual supported set rather than forcing a channel
  the chip physically can't reach.

## Install

Merge the two 5 GHz `config wifi-device` blocks from `wireless.snippet`
into `/etc/config/wireless` (radio1/2.4 GHz block included for completeness,
already OpenWrt's own default). This matches what board detection already
produces -- the value of shipping it explicitly is that it's documented and
won't drift silently if `/etc/config/wireless` is ever regenerated.
