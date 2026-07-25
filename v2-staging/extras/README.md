# extras workstream -- mined findings + buildable artifacts

Scope: everything **except** LEDs / WPA3 / firmware (those are separate
workstreams under `v2-staging/leds`, `v2-staging/wpa3`, `v2-staging/firmware`).
Covers software flow-offload (NAT throughput), the radios-hang-every-few-days
watchdog mitigation, DSA switch (b53/SRAB) specifics, and 5 GHz channel
defaults. Mined from `gh search issues|prs --repo openwrt/openwrt`, the
OpenWrt forum, and this repo's own checked-out 25.12.5 source tree +
live-flashed R8000 (SSH, `192.168.1.1`).

## Directory map

```
extras/
├── flow-offload/
│   ├── firewall.snippet     -- config defaults additions for /etc/config/firewall
│   └── NOTES.md              -- why sw-only, bcm53xx RPS interaction, SQM caveat
├── radio-watchdog/
│   ├── files/etc/init.d/radio-watchdog        -- installs/removes the cron entry
│   ├── files/usr/sbin/radio-watchdog-check    -- detection + escalating recovery
│   └── NOTES.md               -- failure signatures, issue refs, escalation rationale
├── 5ghz-channel-defaults/
│   ├── wireless.snippet      -- per-radio band/channel/htmode defaults
│   └── NOTES.md               -- DTS + live iw + live board-detection evidence
└── dsa-switch/
    └── NOTES.md               -- b53/SRAB status, EAP regression (already fixed), no-hw-NAT note
```

`radio-watchdog/files/` mirrors this repo's existing `image-files/` overlay
convention (see `image-files/etc/init.d/nvram`) -- copy `files/*` on top of
the image's file overlay, or `ImageBuilder FILES=`.

## What was verified, and how

Everything below was cross-checked against at least two independent
sources -- upstream GitHub issues/PRs, the 25.12.5 source tree already
checked out in this repo, and/or the operator's own live-flashed R8000 over
SSH -- not asserted from memory. Full citations are in each topic's
`NOTES.md`.

- **Flow-offload**: confirmed via OpenWrt issue #7023 (bcm53xx has no
  upstream hardware NAT path; software flow-offload is the accepted
  mitigation) and this target's own `packet-steering.sh` platform hook,
  read directly from the source tree, which auto-retunes RPS CPU affinity
  the moment `flow_offloading` is toggled on.
- **Radio watchdog**: failure signatures (PSM watchdog fired,
  msgbuf/commonring timeouts) pulled verbatim from OpenWrt issues #14685 and
  #20514, both filed against R8000 hardware specifically. The init script +
  checker were copied onto the live router, syntax-checked with the
  router's own `/bin/busybox` ash, and functionally exercised end-to-end:
  tier-1 (`wifi reload`) and tier-2 (real `rmmod`/`modprobe brcmfmac` cycle)
  both fired correctly on injected log signatures and the radios came back
  cleanly; tier-3 (reboot) was confirmed for real (triggered by a test
  harness quoting slip, not intentionally) -- the router rebooted and came
  back healthy in under a minute, which is exactly the escalation path
  working as designed. The rate-limit/cooldown logic was re-verified in
  isolation afterward. Install/uninstall was tested for idempotency (`start`
  twice does not duplicate the crontab entry; `stop` cleanly removes it).
- **5 GHz channel defaults**: confirmed three ways -- the DTS
  `ieee80211-freq-limit` values in the 25.12.5 kernel source, `iw phy`
  output captured on the live router (`extracted/boot-results/`), and the
  router's own board-detection-generated `/etc/config/wireless` (read live
  over SSH). All three agree: radio0 ≥149 only, radio2 ≤48 usable now
  (≤144 once the CLM blob lands), radio1 = 2.4 GHz.
- **DSA switch**: confirmed the R8000 uses the SRAB variant of b53 against
  an integrated BCM53012 core (`dmesg`), confirmed the known
  EAP_MODE_SIMPLIFIED/standalone-port regression from issue #21349 is
  already patched in this tree (`patches-6.12/701-...`), and confirmed
  live that `wan` (a standalone/non-bridged port, the exact topology that
  regression broke) comes up cleanly on this build.

## Prioritized include/defer list for the v2 build

| # | Item | Priority | Why |
|---|------|----------|-----|
| 1 | `5ghz-channel-defaults/wireless.snippet` | **Include** | Zero risk -- it's what board detection already generates; shipping it explicitly just makes it durable against a future config regen. Directly answers a real community confusion point (#13902). |
| 2 | `flow-offload/firewall.snippet` | **Include** | One UCI toggle, no new code, meaningfully closes the routed-throughput gap vs. stock firmware (~500 Mbit/s CPU-bound ceiling per forum reports without it) with a platform hook already in-tree doing the hard part. |
| 3 | `radio-watchdog/` | **Include, with the caveat documented in its NOTES.md** | This is the single highest-value fix for the R8000's most-reported OpenWrt complaint (radios wedging, #14685/#20514, already called out in this repo's own CLAUDE.md). Tested live end-to-end including a real reboot-and-recover cycle. The known simplification (module reload resets all 3 radios together, not just the wedged one) is accepted scope, not a shortcut. |
| 4 | `dsa-switch/` (EAP regression watch item) | **Include as documentation, no code change** | Nothing to patch -- fix (`patches-6.12/701-...`) is already present and confirmed working live. Value is purely in not re-breaking it later (e.g. someone "fixing" standalone WAN with the `br-wan` hack from the GitHub thread when it's not needed here) and in flagging the benign SRAB double-probe boot-log line so it isn't re-investigated from scratch. |
| 5 | Per-radio PCIe function reset (surgical single-radio recovery instead of watchdog's all-3 module reload) | **Defer** | Real improvement, materially more complex (`/sys/bus/pci/.../remove`+rescan scripting, PCIe function-to-phy mapping at runtime) for a failure mode that recurs on the order of hours-to-days. Not worth the fragility for v2; the all-3 reload is simple and already verified to work. |
| 6 | CLM/txcap regulatory blob (unlocks DFS channels 52-144 on radio2, full US ccode power tables) | **Defer -- tracked, not in this workstream's scope** | Already tracked in `extracted/FINDINGS.md` as a separate, larger effort (extracting/formatting the blob from the stock `wl` driver). The channel-defaults note here deliberately works around its absence (defaults to non-DFS channels) rather than blocking on it. |
| 7 | b53 EAP standalone-port `br-wan` workaround from #21349 | **Defer / do not apply** | Superseded by the real upstream fix already in this tree. Listed only as a "don't re-add this" note in `dsa-switch/NOTES.md`. |

## Status vs `v2-files/` (verified 2026-07-24)

Items 1-4 above are no longer pending -- all are shipped in `v2-files/` and
confirmed live on the flashed router (currently v11):

- **#1 5ghz-channel-defaults**: `v2-files/etc/config/wireless` radio0/1/2
  `band`/`channel`/`htmode` match `wireless.snippet` exactly (149/VHT80,
  1/HT20, 36/VHT80). Confirmed live via `uci show wireless`.
- **#2 flow-offload**: shipped as `v2-files/etc/uci-defaults/99-flow-offload`
  (a uci-defaults script, not a static merge into `firewall.snippet`'s file)
  -- same effect, `flow_offloading='1'` / `flow_offloading_hw='0'`, and those
  two options are also already present directly in
  `v2-files/etc/config/firewall`'s `config defaults` block. Confirmed live
  via `uci show firewall.@defaults[0]`.
- **#3 radio-watchdog**: `v2-staging/extras/radio-watchdog/files/` is
  byte-for-byte identical (`diff`, no output) to
  `v2-files/etc/init.d/radio-watchdog` and
  `v2-files/usr/sbin/radio-watchdog-check`. Confirmed live: both files
  present on the router and the cron entry is active (`crontab -l`).
- **#4 dsa-switch**: no code to ship (documentation-only item) -- status
  claim re-confirmed live, see `dsa-switch/NOTES.md`.

Items 5-7 remain deferred/not-applied as originally recorded.
