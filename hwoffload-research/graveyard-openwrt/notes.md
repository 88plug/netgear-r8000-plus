# OpenWrt / mainline Linux graveyard mining: bcm53xx / BCM5301X / BCM4709 hardware flow-NAT offload

Scope: search `openwrt/openwrt`, `torvalds/linux`, LKML/netdev/patchwork archives, OpenWrt
forum, and standalone GitHub repos for any real (code/register-level) work toward a hardware
flow/NAT-offload driver for the Northstar switch-integrated "Flow Accelerator" (FA) block —
as opposed to wishful forum talk. Target device: Netgear R8000, SoC BCM4709 (Northstar,
Cortex-A9) with integrated BCM53012 switch.

## TL;DR — the single most concrete foundation found

**There is no code.** Nobody — not OpenWrt, not mainline Linux, not a standalone GitHub repo —
has ever published a driver, register map, or even a partial decode of the Northstar
switch-side Flow Accelerator. The single most concrete thing that exists is a **speculative
mailing-list post from a named kernel developer who owned this exact router**:

> Ian Kent, replying on openwrt-devel, 2015-04-15, in a thread about R8000 CPU-port
> handling (Ian confirms in the prior message in the same thread that he is running "my R8000
> (BCM53012) rev 5 device"):
>
> "[port 8/5/7 combining] might be related to the 'Flow Accelerator' that appears to live in
> the GMAC-3 address space and may be connected to the third PCIE device that's currently not
> configured by bgmac... That also looks to be related to the mystery Cut Through Forward
> code, so I'm not sure it'd be worth the effort of working out how this all works. The Flow
> Accelerator code isn't in the Netgear GPL code either, so it's a guessing game... Netgear GPL
> code is configured to disable the Flow Accelerator code, but... that doesn't mean it's really
> disabled in the actual firmware. Anyway, food for thought."
> — https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30713.html

That's it. One paragraph of educated speculation from 2015, never followed up, never turned
into a patch, never even turned into a documented register offset. Nobody publicly reverse-
engineered further. This is a dead end with a specific, actionable-sounding hypothesis (FA
lives at the GMAC2/"GMAC-3" MMIO window, gated behind an unconfigured 3rd PCIe-enumerated
device on the SoC's internal bus) but zero follow-through.

**Important disambiguation** (this cost real effort to untangle — see below): mainline Linux
*does* have real, merged code mentioning a "Flow Accelerator" on switch port 8 — but it is
gated to a **different, later Broadcom chip family (BCM58xx "Northstar Plus" / NSP)**, not the
BCM4709/BCM53012 "Northstar" family the R8000 uses. See "Two different Flow Accelerators"
below. Do not confuse the two — a lot of secondary web commentary already does.

---

## Two different "Flow Accelerators" — do not conflate them

Broadcom's iProc SoC line has (at least) two switch-integrated hardware forwarding-accelerator
generations, both informally called "Flow Accelerator" (FA) in Broadcom-internal naming, and
they show up in different Linux driver code:

### 1. BCM58xx "Northstar Plus" (NSP) — has real, merged, *acknowledgment* code (not a driver)

`drivers/net/dsa/b53/b53_common.c` (current mainline, function `b53_get_tag_protocol()`):

```c
/* Broadcom BCM58xx chips have a flow accelerator on Port 8
 * which requires us to use the prepended Broadcom tag type
 */
if (dev->chip_id == BCM58XX_DEVICE_ID && port == B53_CPU_PORT) {
        dev->tag_protocol = DSA_TAG_PROTO_BRCM_PREPEND;
        goto out;
}
```
https://github.com/torvalds/linux/blob/master/drivers/net/dsa/b53/b53_common.c

`is58xx()` in `drivers/net/dsa/b53/b53_priv.h` gates this to `BCM58XX_DEVICE_ID` (0x5800),
`BCM583XX_DEVICE_ID`, `BCM7445_DEVICE_ID`, `BCM7278_DEVICE_ID`, `BCM53134_DEVICE_ID` — i.e.
Northstar Plus / set-top-box chips. **BCM53012 (the R8000's switch) is in the separate
`is5301x()` group and never hits this branch.**

The commit chain behind this (all real, all merged, none of it a working NAPT/flow-offload
driver — only plumbing so the DSA tag protocol doesn't break when port 8 is the CPU port):
- `11606039604c` "net: dsa: b53: Support prepended Broadcom tags" — Florian Fainelli,
  2017-11-10. Commit message states: *"On BCM58xx devices (Northstar Plus), there is an
  accelerator attached to port 8 which would only work if we use prepended Broadcom tags."*
  https://github.com/torvalds/linux/commit/11606039604c4ce2d3c5045e30efb0c687a6a0de
- "ARM: dts: NSP: Switch to port 8 for CPU port" — Florian Fainelli, applied to
  devicetree/next 2018-03-12. Commit message: *"...prepare room for supporting the Flow
  Accelerator 2 NAPT offload, and frees up port 5 to be made fully configurable for the modes
  it supports: internal, SGMII, RGMII etc."* This is the **only place in the entire upstream
  record where anyone officially names "Flow Accelerator 2" and "NAPT offload" together** —
  and it is explicitly a Northstar-Plus-only device-tree change, not driver code, not a
  register map, and nothing ever implemented "Flow Accelerator 2" itself upstream.
  https://lkml.iu.edu/hypermail/linux/kernel/1803.1/04661.html
- `63f8428b4077de3664eb0b252393c839b0b293ec` "net: dsa: b53: Fix IMP port setup on BCM5301x" —
  Rafał Miłecki, 2021-09-05. Unrelated to FA directly; fixes the CPU/IMP port register split
  for BCM5301x (the family the R8000 *is* in) after years of the port-8-vs-port-5 confusion
  visible throughout this history. https://github.com/torvalds/linux/commit/63f8428b4077de3664eb0b252393c839b0b293ec

**Conclusion on this branch: real, merged, upstream, but it is tag-protocol/devicetree
plumbing for a different SoC generation (NSP/58xx) than the R8000 (Northstar/4709+53012), and
"Flow Accelerator 2 NAPT offload" itself was never implemented in any driver anyone can find —
it's referenced once, in a commit message, as a stated future goal, and nothing followed.**

### 2. BCM4709/BCM53012 "Northstar" (the R8000's actual chip) — zero code, one speculative email

No file in mainline Linux or OpenWrt references a flow accelerator on BCM5301x/BCM53010-19
chip IDs (`is5301x()` group). The only lead is the Ian Kent email quoted in the TL;DR above,
from an April 2015 openwrt-devel thread about `b53: override CPU port state on BCM5301X with
CPU port other than 8` (patch by Rafał Miłecki). Full thread, all 7 messages, April 2015:
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30674.html (patch v1, Miłecki)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30677.html (patch v2, Miłecki)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30675.html (reply, Jonas Gorski)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30676.html (reply, Miłecki)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30711.html (reply, **Ian
  Kent** — confirms the patch "also allows the switch on my R8000 (BCM53012) rev 5 device to
  function"; notes vendor firmware "uses port 8 as the cpu port (+5 and 7 for some unknown
  purpose)" on higher-rev switches)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30712.html (reply, Miłecki —
  offers a mundane theory: combining 3 ports to the CPU may just be for aggregate throughput,
  since 2 Gbps of LAN traffic can't fit through one 1 Gbps CPU-facing port)
- https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg30713.html (reply, **Ian
  Kent** — the Flow Accelerator/GMAC-3/PCIe speculation quoted in full in the TL;DR)

Mirrors of the same thread (both currently 503/Anubis-blocked, listed for completeness):
- https://openwrt-devel.openwrt.narkive.com/xEMaeLze/patch-b53-override-cpu-port-state-on-bcm5301x-with-cpu-port-other-than-8
- https://patchwork.ozlabs.org/project/openwrt/patch/1428858111-22887-1-git-send-email-zajec5@gmail.com/

A related, independent confirmation that GMAC2 (the "3rd" MAC/PCIe-numbered device Ian Kent's
theory points at) is tied to the undriven FA block, from a 2020 ARM devicetree-list thread
about disabling unused GMACs on the Asus RT-AC88U (also BCM4709+53012-class hardware):
> Rafał Miłecki: "I think gmac2 is required if you want to enable FA (flow acceleration /
> accelerator) - even though there isn't Linux driver for it yet."
> https://www.spinics.net/lists/devicetree/msg490009.html

**This is the closest thing to a "real RE attempt" for the R8000's actual chip: a plausible,
specific hardware hypothesis (FA MMIO lives behind GMAC2/"GMAC-3", requires an internal
PCIe-enumerated device bgmac doesn't configure) from a developer who owned the hardware — and
then nothing. No register dump, no follow-up patch, no probing code, no repo.**

---

## OpenWrt `bcm53xx` target history — what's actually there

Shallow-cloned `openwrt/openwrt` and grepped `target/linux/bcm53xx/**` for
`flow.acceler|flow_acceler|napt|\bFA\b`: **zero hits.** The bcm53xx target has never carried
any code, patch, or config option referencing hardware flow/NAT acceleration. Everything the
target does for NAT throughput is CPU-side:

- `48774...`/multiple: **RPS/packet-steering script** (`bcm53xx: enable & setup packet
  steering`, Rafał Miłecki, 2022-06-10) — adds `/etc/init.d/fastnetwork` which writes
  `rps_cpus` masks to steer IRQs/softirqs across the 2 Cortex-A9 cores differently depending on
  whether *software* `flow_offloading` (netfilter flowtable, pure Linux, no hardware
  involvement) is on. Commit message claims 40-50% NAT-masquerade improvement, reaching
  940-942 Mb/s on BCM4708/BCM47094 this way. This is the actual, working, merged answer to "how
  do I get gigabit NAT on bcm53xx" — and it's a scheduling/affinity trick, not a hardware
  offload driver. https://www.mail-archive.com/openwrt-devel@lists.openwrt.org/msg62276.html
- Historical R8000-specific commits, none touching FA, all about which switch port is wired to
  the CPU MAC (a prerequisite/adjacent problem, not the accelerator itself):
  - `2d3aaa2d2fce` "bcm53xx: fix default network interface on Netgear R8000" (rmilecki,
    2015-05-13) — vendor NVRAM says CPU port is switch port 8 (connected to eth2); OpenWrt
    needed the same mapping.
  - `e6944a3490ab` "bcm53xx: add workaround for Netgear R8000 network" (rmilecki,
    2015-10-30) — port 8 CPU mapping "doesn't work right now," falls back to port 5.
  - `06ac2f5c7430` "b53: improve overriding CPU port state on BCM5301X" (rmilecki,
    2015-04-12) — the v1/v2 patch discussed in the Ian Kent thread above.

No pushed-but-unmerged branch on `openwrt/openwrt` touches this either — `git ls-remote
--heads` on the canonical repo returns only 11 branches total (release/staging branches), none
named or grep-matching bcm53xx/northstar/flow/accel/natp/fa.

## GitHub-wide search — no standalone repo exists

`gh api search/repositories` for `northstar bcm53xx offload`, `bcm5301x nat OR "flow
accelerator"`, and `"BCM4709" flow accelerator` all returned **zero results**. `gh api
search/code` for `bcmfwd` (the Broadcom SDK header name speculatively cited in one secondary
summary as where FA/NAPT register definitions might live) returned only unrelated noise (no
real `bcmfwd.h` on GitHub in any vendor GPL dump that's publicly indexed). Nobody has a
Northstar/BCM4709 FA driver attempt sitting in a personal repo, at least not one indexed by
GitHub code/repo search.

## Forum discussion (aspirational only, no register/code content)

- "OpenWRT 19.07 and bcm53xx target + flow offloading" — the actual fix for the reported
  500 Mbps WAN→LAN ceiling on a Phicomm K3 (also bcm53xx/Northstar) was `ethtool -K eth0 gro
  off` plus irqbalance/RPS tuning — a `bgmac` driver GRO regression, unrelated to any hardware
  accelerator. https://forum.openwrt.org/t/openwrt-19-07-and-bcm53xx-target-flow-offloading/52356
- "Where to find the latest news on NAT acceleration?" — confirms hardware flow offload is a
  thing on MT7621/MT7622 (Netfilter flowtable hooks into MediaTek's PPE), explicitly **not**
  available for bcm53xx; a poster there flatly asks "Is it possible to get HW NAT on bcm53xx?"
  and gets no answer. https://forum.openwrt.org/t/where-to-find-the-latest-news-on-nat-acceleration/75457/
- Long-running "Netfilter 'Flow offload' / HW NAT" megathread (2018-2023, 250+ posts) — general
  netfilter-flowtable/hardware-offload discussion across all OpenWrt targets; spot searches for
  bcm53xx/BCM5301X/Northstar-specific content inside it turn up nothing beyond the two threads
  above being cross-linked in. https://forum.openwrt.org/t/netfilter-flow-offload-hw-nat/10237
- General community consensus quoted across multiple threads: Broadcom's proprietary CTF
  (Cut-Through Forwarding, the mechanism stock Netgear/Asus/Linksys firmware uses for
  wire-speed NAT) is closed-source, its GPL releases either omit or explicitly disable the FA
  code path, and getting real hardware-NAT support upstream would require a from-scratch,
  netfilter-flowtable-API-shaped reimplementation — "the effort needed is manpower, not
  money."

## Bug reports checked (context, not FA-specific)

- `openwrt/openwrt#7023` (FS#2186, "DSA driver performance issue") — Linksys EA9500
  (dual BCM53012+BCM53125 switch, same GMAC-facing architecture as R8000) capped at ~500 Mbps
  LAN→WAN under the DSA driver vs. 1 Gbps on factory firmware; reporter's own analysis blames
  general DSA tagging overhead, not a missing accelerator specifically. No FA content.

---

## What would actually be needed to move this forward (not attempted by anyone found)

1. Confirm/refute Ian Kent's 2015 hypothesis: probe whatever MMIO window sits behind
   GMAC2/"GMAC-3" on a live BCM4709/BCM53012 R8000 (register dump via `devmem`/`/dev/mem` while
   comparing switch port 8 behavior with/without CPU-port-8 vendor NVRAM config) to see if
   anything responds there that isn't already claimed by `bgmac`/`b53`.
2. Extract and diff Netgear's actual R8000 GPL tarball kernel source (not just the SDK headers)
   for any `ctf.ko`/`fa.ko`-equivalent module or Kconfig symbol that's compiled out — Ian Kent
   already looked and didn't find it in what Netgear shipped as of 2015; a fresh pull against
   the current/last R8000 GPL release might differ, and comparing against a Northstar-Plus
   vendor GPL release (which *does* ship FA-aware b53 tag-protocol code, per part 1 above) could
   help identify the shared register layout even if the block itself differs per chip rev.
3. Nothing register-level (offsets, bit layouts, NAPT table format) has ever been published for
   either chip generation's FA block by anyone, in or out of tree — this would be net-new work,
   not a matter of finishing someone else's partial driver.
