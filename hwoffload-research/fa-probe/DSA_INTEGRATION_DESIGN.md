# FA/CTF + mainline DSA integration — design draft (untested)

Status: **draft, not built as a loadable artifact, not tested on real hardware.**
This is the concrete next increment beyond FINDINGS.md #61-#65 - a real design
and code sketch for the missing piece those findings identified (a mainline
kernel driver integration for FA's own header format), written so the next
live test is informed by a real hypothesis instead of another blind register
flip. Do not load this against the real router without first re-reading
FINDINGS.md #65's safety methodology (auto-revert watchdog, non-persistent
register writes, recovery net staged) - this crosses into new-driver-code risk,
not just register-poke risk, and deserves the same care.

## What's settled (real, live-tested evidence, FINDINGS.md #61-#65)

- FA silicon is real, present, and responds correctly to every register this
  project has read or written (GMAC control/status, both switch-side SRAB
  writes, and FA's own `bcm_hdr_ctl`).
- Enabling `bcm_hdr_ctl` (`CTF_BRCM_HDR_HW_EN|SW_RX_EN|SW_TX_EN|PARSE_IGN_EN`,
  the real value from `etc_fa.c`'s `fa_up()`) causes real, severe, reproducible
  traffic disruption (60-100% loss across two independent trials) on `eth2`,
  the DSA conduit carrying every LAN port.
- Mainline `bgmac.c` has zero CTF/FA awareness. Mainline `b53`/DSA already
  manages a *different*, already-working 4-byte tag (`tag_brcm`, confirmed via
  `ip -d link show lan1`: `dsa conduit eth2`, and via mainline source: DSA's
  Broadcom tag is 4 bytes, positioned before the MAC DA/SA/EtherType -
  identical size and position to FA's own header).
- Broadcom's own vendor code (`etc_fa.c: fa_process_tx/fa_process_rx`) does
  `PKTPUSH`/`PKTPULL` by exactly 4 (or 8, when `op_code==0x1`) bytes - real
  evidence FA's header is an *insertion/removal* operation on the vendor's own
  (non-DSA) driver, not a same-size reformat of an existing field.

## The open question this design has to commit to an answer for

DSA already inserts/strips its own 4-byte tag on every frame on `eth2`
*today*, correctly, in production. Broadcom's vendor architecture (`et_linux.c`)
never had to coexist with a *separate*, generic DSA layer - it did all tag
handling itself, in one driver. Two hypotheses for how the real switch silicon
actually behaves once `bcm_hdr_ctl`'s `HW_EN` is set:

- **Hypothesis A (stacked/additive)** - the switch inserts DSA's normal 4-byte
  port tag *and*, separately, FA's own 4-or-8-byte header, for a combined
  6-12 byte prefix depending on op_code. DSA's existing tag_brcm strip (4
  bytes) would leave 4-8 unstripped FA bytes in front of what it hands to the
  bridge - directly explains "every frame corrupted" without any assumption
  about port misidentification.
- **Hypothesis B (reformatted-in-place)** - `HW_EN` changes what the *same*
  existing 4-byte tag slot contains (FA-aware bits, including `src_pid` which
  overlaps with what a port tag encodes) rather than adding bytes. DSA's tag
  parsing would then read the *wrong* bits as port number, causing
  misrouting/drops without an actual length change.

**This design commits to Hypothesis A** - it is the one directly supported by
primary source (`PKTPUSH`/`PKTPULL` are unambiguously insert/remove
operations, not in-place rewrites), and it is falsifiable independently of
Hypothesis B: if A is right, a correct strip of the *extra* bytes on top of
DSA's own strip should restore clean traffic; if the real behavior is
actually B, this driver would need to intercept *before* DSA's own tag_brcm
parsing instead of after it - a structurally different (and larger) change,
noted as the fallback plan below.

## Why a new DSA tag protocol, not an rx_handler

`netdev_rx_handler_register()` - the obvious "add a hook" mechanism, and what
this project's other hotplug/probe modules already use for simpler cases - is
**not available on `eth2`**: DSA itself already owns that slot (confirmed,
`dsa_conduit_setup()` registers DSA's own rx_handler on the conduit device
at attach time; only one rx_handler per netdevice is permitted by the kernel).

The correct, mainline-idiomatic extension point is a **new `dsa_tag_driver`**
(`struct dsa_device_ops`), the same mechanism `tag_brcm.c` itself uses. Under
Hypothesis A, this driver **wraps** tag_brcm's own port-tag handling and adds
an extra strip/insert pass for FA's header on top of it, rather than
reimplementing port-tag parsing from scratch - reuse, not a rewrite, matching
this project's own `[Efficiency]`/DRY discipline.

## Design (Hypothesis A)

```c
// fa_tag.c — DSA tag driver: tag_brcm's own port tag, PLUS FA's header
// strip/insert on top. UNTESTED - see DSA_INTEGRATION_DESIGN.md.
//
// Wire layout assumed (Hypothesis A, ingress/CPU-bound direction):
//   [ FA header: 4 or 8 bytes ][ DSA tag_brcm: 4 bytes ][ real Ethernet frame ]
// (FA header first because etc_fa.c's fa_process_rx() pulls it as the very
// first bytes of the packet buffer, before anything else touches it - see
// PKTDATA/PKTPULL calls at the top of fa_process_rx().)

#include <linux/etherdevice.h>
#include <linux/dsa/tag_brcm.h>   // reuse mainline's own tag_brcm helpers
#include <net/dsa.h>

#define FA_HDR_MIN_LEN   4
#define FA_HDR_MAX_LEN   8
#define FA_OPC_8BYTE     0x1     /* from bcm_hdr_t.oc10/oc0 op_code, etc_fa.h */

static struct sk_buff *fa_tag_xmit(struct sk_buff *skb, struct net_device *dev)
{
	/*
	 * TX direction: CPU -> switch. fa_process_tx() always sends the
	 * simple 4-byte, all-zero (op_code=0) variant - the switch doesn't
	 * need flow-index info for CPU-originated traffic, only the RX
	 * direction (switch -> CPU, for a HIT) uses the richer 8-byte form.
	 * Prepend it, THEN let tag_brcm's own xmit build its normal port tag
	 * on top (closest to the wire) - reuse, not reimplementation.
	 */
	if (skb_cow_head(skb, FA_HDR_MIN_LEN) < 0) {
		kfree_skb(skb);
		return NULL;
	}
	skb_push(skb, FA_HDR_MIN_LEN);
	memset(skb->data, 0, FA_HDR_MIN_LEN);   /* op_code=0, matches fa_process_tx() */

	return brcm_tag_xmit(skb, dev);   /* reuse mainline tag_brcm's own xmit */
}

static struct sk_buff *fa_tag_rcv(struct sk_buff *skb, struct net_device *dev)
{
	u8 op_code;
	u8 fa_hdr_len;

	if (unlikely(!pskb_may_pull(skb, FA_HDR_MIN_LEN)))
		return NULL;

	/* bcm_hdr_t.oc10.op_code / .oc0.op_code: top 3 bits of the first
	 * network-order 32-bit word (etc_fa.h). Mirrors fa_process_rx()'s
	 * own bhdr.oc10.op_code read exactly. */
	op_code = skb->data[0] >> 5;
	fa_hdr_len = (op_code == FA_OPC_8BYTE) ? FA_HDR_MAX_LEN : FA_HDR_MIN_LEN;

	if (unlikely(!pskb_may_pull(skb, fa_hdr_len)))
		return NULL;

	/* TODO once live-verified: decode napt_flow_id/hdr_chk_result/
	 * all_bkts_full from the 8-byte variant here and attach to skb
	 * metadata (e.g. skb->mark or a dedicated cb[] field) before
	 * discarding the header, so FINDINGS.md's fa_stats_probe-style hit
	 * accounting can be corroborated from software too, not just FA's
	 * own hardware counters. Not required for the correctness question
	 * this design exists to test first (does stripping restore clean
	 * traffic at all) - added here as a stub deliberately, not to over-
	 * build before Hypothesis A itself is confirmed. */

	skb_pull(skb, fa_hdr_len);   /* mirrors etc_fa.c's PKTPULL(pull) exactly */

	return brcm_tag_rcv(skb, dev);   /* hand off to mainline tag_brcm's own rcv */
}

static const struct dsa_device_ops fa_netdev_ops = {
	.name		= "fa-brcm",
	.proto		= DSA_TAG_PROTO_FA_BRCM,   /* new enum value, needs core.c registration */
	.xmit		= fa_tag_xmit,
	.rcv		= fa_tag_rcv,
	.needed_headroom = FA_HDR_MAX_LEN + BRCM_LEG_TAG_LEN,	/* tag_brcm.h constant */
	.needed_tailroom = 0,
};

DSA_TAG_DRIVER(fa_netdev_ops);
MODULE_LICENSE("GPL");
```

**This is a sketch, not a compiling driver.** Known gaps before it could even
build:
- `DSA_TAG_PROTO_FA_BRCM` doesn't exist - adding a new tag protocol enum
  value means either patching `include/net/dsa.h` (upstream-style, needs a
  kernel rebuild) or registering dynamically if the kernel version supports
  it (needs checking against this exact 6.12.94 tree).
- `brcm_tag_xmit`/`brcm_tag_rcv` are not exported symbols in mainline
  `tag_brcm.c` today (they're `static`) - reusing them as claimed above
  requires either an upstream patch exporting them, or duplicating their
  logic locally (which reintroduces the "don't reinvent, reuse" tension this
  design tried to avoid - a real open question, not glossed over here).
- Actually *switching* `eth2`'s active tag protocol from `tag_brcm` to this
  new one at runtime is itself non-trivial - DSA binds a tag protocol at
  probe time from devicetree/driver defaults, not something this project's
  usual insmod/rmmod pattern can flip live the way a register write can.

## Fallback plan if live testing shows Hypothesis A is wrong

If a correctly-implemented Hypothesis-A strip does NOT restore clean traffic,
the real behavior is closer to Hypothesis B (in-place reformat) or something
neither hypothesis anticipated. That would mean the fix has to happen
*before* DSA's own `tag_brcm` parsing runs, not after - i.e. intercepting at
the raw netdevice `.ndo_start_xmit`/an actual `bgmac.c` patch, the
heavier, originally-scoped option from FINDINGS.md #62's correction. This
design deliberately did not start there, since Hypothesis A is cheaper to
build and independently falsifiable - fail fast on the cheaper hypothesis
before committing to the bigger one.

## What would actually validate this design (not done - explicitly deferred)

1. Get this to actually compile against the real kernel tree (resolve the
   `DSA_TAG_PROTO_*`/`brcm_tag_xmit`/`_rcv` export gaps above - real kernel
   patch work, not a quick fix).
2. Load it via the SAME auto-revert watchdog pattern as FINDINGS.md #65 -
   bounded timer, confirm-file-gated, `setsid`-detached - since this is a new
   kernel driver on the operator's only router, not a proven-safe register
   read.
3. Re-run the exact same `bcm_hdr_ctl` enablement, this time with this tag
   driver bound to `eth2` instead of stock `tag_brcm`, and check: does
   `ping 192.168.1.1` stay clean (confirms Hypothesis A + correct strip) or
   does it still show loss (confirms Hypothesis A's strip logic is wrong, or
   Hypothesis B, or something else - each with a different next step)?
4. Only after step 3 succeeds: wire `fa_accel.c`'s already-proven Phase
   A/B/C logic to this tag driver's RX path (via the metadata stub left in
   `fa_tag_rcv()` above) and re-check the real FA hit-counter
   (`fa_stats_probe.c`, FINDINGS.md #62) for a genuine, non-zero increment -
   the actual "hw offload working" evidence bar, not yet met by anything in
   this project.
