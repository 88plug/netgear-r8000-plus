# Radio-hang watchdog -- notes

## The problem

R8000's three BCM43602 radios run on `brcmfmac`, a driver that talks to
closed firmware over a PCIe message-ring protocol. That firmware wedges
under sustained/heavy traffic on this exact chip+driver combination; when it
does, the radio stops passing traffic until something resets it. This is the
"radios hang every few days, reboot-to-recover" caveat already noted in this
repo's own `docs/FINDINGS.md`, and it's well documented upstream, specifically on
R8000 hardware:

- **openwrt/openwrt#14685** -- "brcmfmac makes CPU stalls" on Netgear R8000
  (BCM4709), reported on several OpenWrt versions/kernels (SNAPSHOT r25222,
  and again independently on 23.05.2/kernel 5.15.137). Log signatures:
  ```
  ieee80211 phy0: brcmf_msgbuf_query_dcmd: Timeout on response for query command
  ieee80211 phy1: brcmf_msgbuf_tx_ioctl: Failed to reserve space in commonring
  ieee80211 phy1: brcmf_cfg80211_dump_station: BRCMF_C_GET_ASSOCLIST failed, err=-12
  ...
  phy0: PSM microcode watchdog fired at 41733 (seconds)
  ieee80211 phy1: brcmf_psm_watchdog_notify: PSM's watchdog has fired!
  ```
  https://github.com/openwrt/openwrt/issues/14685
- **openwrt/openwrt#20514** -- R8000, tri-radio BCM43602, reports a related
  wedge on the flowring teardown path on stock 24.10.3:
  ```
  brcmfmac: brcmf_msgbuf_delete_flowring: timed out waiting for txstatus
  ```
  https://github.com/openwrt/openwrt/issues/20514

Both are firmware-level failures (PSM = the radio's power-save microcode; the
"commonring"/"msgbuf" errors are the host<->firmware command channel itself
timing out), not a mac80211/config bug -- there is no known permanent driver
fix as of 25.12.5, this is a hardware/firmware ceiling in `brcmfmac` on
BCM43602 that upstream (CC'd: the brcmfmac maintainer, see the #14685 thread)
has not resolved.

## Why not just cron a plain `wifi reload`

That's the common community mitigation and it's a reasonable *first* attempt
(cheap, no module reload, no dropped associations if the radio wasn't
actually wedged) -- this watchdog does exactly that on the first detection.
But the msgbuf/commonring/PSM-watchdog signatures above mean the firmware
itself has crashed, not just mac80211 state getting confused. A `wifi
reload` stays entirely above the driver and doesn't touch the PCIe
attach/reset path, so it will not reliably clear a genuine firmware wedge --
and repeatedly hammering `wifi reload` against a chip that's actually wedged
risks pushing it into a worse state rather than a better one. `brcmfmac`
also has no host-triggered silicon-reset ioctl for this failure class, so
the only things that reliably clear a real firmware crash are (a) reloading
the `brcmfmac` kernel module, which re-runs PCIe device attach/reset, or
(b) a full reboot.

## What this watchdog does

`files/etc/init.d/radio-watchdog` wires `files/usr/sbin/radio-watchdog-check`
into root's crontab (every 5 min) -- no persistent daemon, negligible idle
cost. The checker:

1. Reads only the *new* `logread` lines since its last run (tracked via a
   last-seen-line marker in `/tmp/radio-watchdog/`) and greps them for the
   four signatures above.
2. On a match, counts how many matches have occurred in a trailing 1-hour
   window and escalates:
   - **1st event/hour:** `wifi reload` (cheap, tries the soft path first).
   - **2nd event/hour:** full `brcmfmac` module reload (`rmmod`/`modprobe`
     brcmfmac and any module using it, e.g. `brcmfmac_wcc`) -- this redoes
     the PCIe attach/reset that a plain reload skips.
   - **3rd+ event/hour:** reboot, since the module reload didn't hold.
     Rate-limited to at most once per 6 hours (persisted across reboots in
     `/etc/radio-watchdog.last-reboot`) specifically to avoid a reboot loop
     -- if the cooldown hasn't elapsed, it falls back to another module
     reload instead of rebooting again.

All actions are logged via `logger -t radio-watchdog`, visible in `logread`.

## Known simplification

A module reload resets all three radios together (they share the
`brcmfmac`/`brcmfmac_wcc` modules), even if only one `phyN` logged the
wedge. Per-radio PCIe function reset (via `/sys/bus/pci/.../remove`+rescan
for just the affected function) is possible but materially more complex and
fragile to script reliably; resetting all three for a rare event (hours-to-
days between occurrences per the reports above) is an accepted trade-off,
not a shortcut taken to save effort.

## Install

Copy `files/` on top of the image's `files/` overlay (ImageBuilder `FILES=`
or the repo's existing `image-files/` convention), or drop directly onto a
running router and `chmod +x` both files, then:

```sh
/etc/init.d/radio-watchdog enable
/etc/init.d/radio-watchdog start
```

**Status: already shipped.** `files/etc/init.d/radio-watchdog` and
`files/usr/sbin/radio-watchdog-check` here are byte-for-byte identical
(`diff`, no output) to `v2-files/etc/init.d/radio-watchdog` and
`v2-files/usr/sbin/radio-watchdog-check`. Confirmed live: both files present
on the router and the cron entry active (`crontab -l | grep radio-watchdog`,
2026-07-24).

## Verify

```sh
crontab -l | grep radio-watchdog          # cron entry present
logread -e radio-watchdog                 # watchdog's own action log
cat /tmp/radio-watchdog/events             # recent detection timestamps (if any)
```

To dry-run the escalation logic without waiting for a real wedge, inject a
matching line and run the checker manually:
```sh
logger -t kernel "ieee80211 phy1: brcmf_psm_watchdog_notify: PSM's watchdog has fired!"
/usr/sbin/radio-watchdog-check
logread -e radio-watchdog
```
