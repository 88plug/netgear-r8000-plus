# r8000plus first boot — results (2026-07-23)

Flashed `openwrt-25.12.5-r8000plus-…-netgear_r8000-squashfs.chk` via stock GUI.
OpenWrt 25.12.5 r33051, kernel 6.12.94 armv7l. SSH up ~102s after flash.

## VERIFIED WORKING
- **All 3 radios up**: phy0 (…f1:38, 5GHz, 29 ch), phy1 (…f1:37, 2.4GHz, 14 ch),
  phy2 (…f1:36, 5GHz, 29 ch). Per-radio MACs match our extracted calibration.
- **5GHz functional**: live scan on wlan0 (phy2) saw 5 APs → RX proven on 5GHz.
  phy2 advertises **AP mode + VHT (802.11ac)**.
- Our `netgear,r8000` nvram-init case is present in the running `/etc/init.d/nvram`
  (grep = 1); devpath0/1/2 set to our values; 287 `N:` cal keys in nvram.

## HONEST CAVEATS (not yet done)
1. **CLM + txcap regulatory blobs still missing** (`err -2`, same lines as #20514):
   "no clm_blob available, device may have limited channels"; regdom is the
   conservative `country 00`. TX power / DFS regulatory not fully calibrated.
   Next: extract `clm_blob` from the stock `wl` driver (binwalk the saved stock
   `.chk`) → add to image for full regulatory + power calibration.
2. **Attribution not controlled**: our binding init is active, but the stock nvram
   partition is preserved through the flash, so 5GHz *might* also come up on a
   plain OpenWrt build. To prove our patch is the decisive factor, flash stock
   25.12.5 and compare (controlled A/B). Not yet run.
3. LuCI not in this lean ImageBuilder build (SSH-only). Easy to add.

## Net
A working tri-band OpenWrt R8000 with functional 5GHz — the "dead 5GHz" the
community reported is not dead here. Full regulatory calibration (clm_blob) and
the controlled A/B remain.
