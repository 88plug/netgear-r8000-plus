# DSA switch (b53/SRAB) specifics on the R8000 -- notes

No config snippet in this section -- current status is "already correct in
25.12.5", so this is a documented finding + a watch item, not a patch to
carry.

## What's on this board

Confirmed live (`dmesg`, this repo's flashed 25.12.5 unit):
```
b53-srab-switch 18007000.ethernet-switch: found switch: BCM53012, rev 5
```
The R8000 uses the **SRAB** (Switch Register Access Bus) variant of the b53
driver against an integrated BCM53012-compatible switch core inside the
BCM4709 SoC -- not a separate MDIO-attached switch chip. Port layout from
`&srab` in the DTS (`bcm4709-netgear-r8000.dts`):

| DSA port | Role  | Note |
|----------|-------|------|
| port@0-3 | lan1-4 | |
| port@4   | wan    | |
| port@5, port@7 | disabled | present in the shared `bcm-ns.dtsi` switch template for boards with a second switch/GMAC link (e.g. Linksys EA9500); not populated on R8000's simpler single-switch board |
| port@8   | cpu    | |

All ports enumerate and lan1 reaches full link on boot:
```
b53-srab-switch 18007000.ethernet-switch lan1: configuring for phy/gmii link mode
b53-srab-switch 18007000.ethernet-switch lan1: Link is Up - 1Gbps/Full - flow control rx/tx
```
Traffic is carried on a single conduit interface (`eth2` on this board) with
`lan1-4`/`wan` as DSA slave netdevs bridged into `br-lan` (lan1-4) or left
standalone (`wan`) -- standard bcm53xx DSA topology.

## Watch item: b53 EAP_MODE_SIMPLIFIED regression (already fixed, already
## present in this tree)

**openwrt/openwrt#21349** -- "bcm53xx: sysupgrade does not reliably preserve
configuration on RT-AC3200 / RT-AC5300" (title is misleading; the actual bug
reported and root-caused in the thread is **standalone/non-bridged switch
ports going dark**, first seen as "no Internet after upgrade to 25.12.0-rc1"
on a Netgear R6250). https://github.com/openwrt/openwrt/issues/21349

Root cause, nailed down in-thread by the bcm53xx maintainer (rmilecki):
upstream kernel commit `4227ea91e265` ("net: dsa: b53: prevent standalone
from trying to forward to other ports") was backported into the `6.12.y`
stable series at `6.12.30` and broke non-bridged (standalone) ports on
**Northstar** SoCs (BCM53011/BCM53012 -- i.e. this exact switch family) by
setting `EAP_MODE_SIMPLIFIED`, which those chips don't handle correctly in
standalone mode. Symptom: a switch port not in a bridge (a bare `wan`
interface, exactly R8000's default topology) stops passing traffic even
though the link shows up. Workaround identified in-thread: put the WAN port
in its own bridge (`br-wan`) instead of leaving it standalone -- works, but
is explicitly called an ugly hack by the person who found it, not the real
fix.

**Current status on this build: already fixed, not a live issue.**
`target/linux/bcm53xx/patches-6.12/701-net-dsa-b53-disable-EAP-setup-on-Northstar-switches.patch`
is present in this repo's checked-out 25.12.5 source tree and applies the
real upstream fix (skip `EAP_MODE_SIMPLIFIED` entirely on `is5301x()`
chips -- i.e. BCM53011/53012/Northstar):
```c
if (is5301x(dev))
	return;
```
The patch's own header cites both the upstream regression and this exact
issue number:
```
* https://github.com/openwrt/openwrt/issues/21187
* https://github.com/openwrt/openwrt/issues/21349
```
Confirmed live on the flashed R8000: `wan` is a standalone (non-bridged)
port in `/etc/config/network` and lan1 (also effectively standalone from a
DSA-forwarding perspective until bridged) came up cleanly with full duplex
and flow control, no port stuck at "configuring" or dropping traffic.

**Action for the v2 build: none required, this is already correct.** Listed
here so it's not accidentally "fixed" a second time (e.g. by someone adding
the `br-wan` workaround from the GitHub thread) and so a future kernel bump
that might re-drop or renumber this patch gets caught by keeping the patch
filename/reference on the radar.

## Why there's no hardware NAT to lean on here

Related, and the direct reason the `flow-offload/` section of this workstream
exists: this same switch family (b53/SRAB on Northstar) has no upstream
hardware NAT acceleration. From **openwrt/openwrt#7023** (DSA driver
performance issue, Linksys EA9500 -- same BCM53012-family switch as R8000):
LAN-to-LAN switching hits full 1 Gbit line rate (that part is hardware
switched, no CPU involvement), but LAN-to-WAN *routed* (NAT'd) traffic was
capped around 500 Mbit/s on a 1 Gbit link, because routing/NAT has to go
through the CPU -- stock firmware's Broadcom CTF/NATP hardware accelerator
for that path isn't implemented in the mainline driver stack. See
`../flow-offload/NOTES.md` for the mitigation (software flow-offloading).
https://github.com/openwrt/openwrt/issues/7023

## Minor, non-blocking observation from live boot log

One boot-log oddity worth flagging for anyone debugging switch issues later
(not currently causing a problem -- second attempt succeeds cleanly):
```
[    3.018030] b53-srab-switch b53-srab-switch: error -EINVAL: invalid resource (null)
[    3.025734] b53-srab-switch b53-srab-switch: probe with driver b53-srab-switch failed with error -22
[    6.604463] b53-srab-switch 18007000.ethernet-switch: found switch: BCM53012, rev 5
```
First probe attempt at t=3.0s fails with `-EINVAL` (missing resource), then
a second probe at t=6.6s succeeds and the switch initializes normally (all
5 ports up, DSA tree set up, lan1 reaches full link by t=9.7s). Read as a
benign probe-ordering retry (a dependency not ready on the first pass), not
a functional issue on this board -- included here only so it's recognized
as already-observed-and-harmless if it shows up again in future kernel bumps
rather than re-investigated from scratch.
