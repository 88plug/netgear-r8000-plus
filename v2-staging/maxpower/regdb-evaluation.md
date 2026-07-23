# wireless-regdb / country-override evaluation

The task asked for three mechanisms to be evaluated for the channel unlock:
a permissive regulatory country, patching `wireless-regdb`, and the
brcmfmac reg-override behavior. All three were evaluated; only one produces
any effect on this hardware today, and that effect is currently a no-op
(inert until a clm_blob exists). Full derivation and on-device proof is in
`notes.md` and `verify/before-after.md`; this file is the regdb-specific
half of that evaluation plus the apply commands.

## 1. Permissive regulatory country (`iw reg set <cc>` / `option country`)

**Verified live: no effect on this device's actual channel availability**,
because brcmfmac registers all three R8000 wiphys as
`REGULATORY_WIPHY_SELF_MANAGED`. Self-managed wiphys explicitly do not
consult the global cfg80211 regdomain that a bare `iw reg set <cc>` (or
OpenWrt's `mac80211.sh`, which calls exactly that) changes. See
`verify/before-after.md` Attempt 1.

There is a second, real path -- hostapd's own `country_code=` config
option, which (unlike the bare CLI command) issues a wiphy-*scoped*
regulatory request that self-managed wiphys do listen for. This is the
mechanism `option country` in `/etc/config/wireless` actually wires up to
once an interface using that radio starts (`package/network/config/
wifi-scripts/files/lib/netifd/hostapd.sh` appends `country_code=$country`
to the generated hostapd config). It was also tested live (Attempt 4) --
still zero effect on the channel set, and additionally triggered a
watchdog reboot when pushed against an already-loaded wiphy. **Still worth
setting** (`option country 'US'` is included in this workstream's
`etc/config/wireless` fragment) because it's inert-safe at rest, matches
the SKU's own DTS-declared `brcm,ccode-map = "JP-JP-78", "US-Q2-86"` and
the top-level nvram `ccode=US`, and becomes meaningful once a clm_blob is
present (see notes.md).

## 2. Patching `wireless-regdb` to relax US limits

**Evaluated and deliberately NOT done.** `package/firmware/wireless-regdb`
in this tree (v2026.05.30) builds `regulatory.db`, which is exactly the
data structure that feeds the *global* cfg80211 domain -- the same one
Attempt 1 proved this device's wiphys ignore entirely while self-managed.
Patching regdb bounds (e.g. raising the UNII-1 EIRP ceiling toward this
device's actual FCC-certified 25.99dBm, which is legitimately higher than
generic regdb defaults per `../fccid/specs.md` §5) would be real,
defensible work *in general* -- but on this specific hardware, in its
current state, it would have **zero measurable effect**, because the code
path that would apply it (global-domain intersection) is the exact path
these wiphys bypass. Doing it anyway would be dead weight: a patch that
builds cleanly and does nothing on the device it's for. Flagging this back
explicitly since `../fccid/specs.md` §5 recommends regdb editing as
"real, legal upside" without accounting for the self-managed discovery
made in this workstream -- that recommendation needs the self-managed
caveat attached, or it should be re-scoped to "once a clm_blob makes the
per-wiphy path meaningful."

If this ever needs revisiting (e.g. if a future clm_blob still leaves the
device honoring the *global* regdomain for some sub-path), the apply
commands would be:

```sh
# NOT currently useful on this hardware -- kept for reference only.
# 1. Edit package/firmware/wireless-regdb/patches/ with a bounds patch
#    against db.txt (upstream source), e.g. raising US UNII-1 EIRP.
# 2. Rebuild: make package/firmware/wireless-regdb/{clean,compile} V=s
# 3. Deploy regulatory.db to /lib/firmware/ on-device, then:
iw reg set US   # or whatever alpha2 the edited entry uses
iw reg get      # confirm the *global* domain reflects the new numbers
# --- but confirm first that the target wiphy is NOT self-managed, e.g.:
iw reg get | grep -A3 '^phy#'   # if phy-scoped "country NN" lines appear
                                 # separate from "global", it's self-managed
                                 # and this whole path is a no-op, as here.
```

## 3. brcmfmac reg-override behavior -- what actually gates channels here

This is the one that matters. Full derivation in `notes.md`; summary:

`brcmfmac`'s cfg80211 registration for this device calls
`wiphy_read_of_freq_limits()` against the devicetree `ieee80211-freq-limit`
property present on each 5GHz radio node (confirmed directly in this
device's own live decompiled DT, `../leds/router-live.dts:131-177`) --
this disables any channel outside the declared range **unconditionally**,
before CLM, before ccode/regrev, before any regdomain logic runs at all.
radio0 is DT-limited to 5735-5835MHz (149-165 only); radio2 to
5170-5730MHz (34-144). This is a genuine board-hardware fact (separate
antenna chains + different Skyworks front-end module per radio, per
`../fccid/specs.md` §3), encoded once, upstream, in the kernel's own
devicetree source for this exact board -- not an OpenWrt-specific
restriction and not something any of the three mechanisms above can move.

**"Winning" mechanism, in actual precedence order on this device:**
1. DT `ieee80211-freq-limit` (hard board-hardware gate -- always wins,
   present today, cannot be overridden from userspace)
2. Firmware CLM blob, if present (governs what subset *within* that DT
   envelope is enabled for a given country -- absent today)
3. Per-wiphy self-managed regdomain via hostapd `country_code=` (only
   matters once #2 exists to give it something to look up)
4. Global cfg80211/wireless-regdb domain via bare `iw reg set` (never
   consulted by these wiphys at all, self-managed)
