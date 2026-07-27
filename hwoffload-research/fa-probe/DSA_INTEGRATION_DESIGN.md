# FA/CTF + mainline DSA integration — design draft (untested, v2)

Status: **draft, not built as a loadable artifact, not tested on real hardware.**
v2 supersedes the first draft after reading mainline's actual
`net/dsa/tag_brcm.c` directly (present in this project's full `openwrt/`
build tree, not just the out-of-tree-module SDK's headers) - the real source
changes the diagnosis significantly. Do not load anything based on this
design against the real router without first re-reading FINDINGS.md #65's
safety methodology (auto-revert watchdog, non-persistent writes, recovery
net staged) - a kernel patch here is at least as risky as the register
writes already tested, not less.

## What's settled (real, live-tested evidence, FINDINGS.md #61-#65)

- FA silicon is real, present, and responds correctly to every register this
  project has read or written.
- Enabling `bcm_hdr_ctl` (`CTF_BRCM_HDR_HW_EN|SW_RX_EN|SW_TX_EN|PARSE_IGN_EN`,
  the real value from `etc_fa.c`'s `fa_up()`) causes real, severe, reproducible,
  *variable* traffic disruption (60% loss trial 1, 100% loss trial 2) on
  `eth2`, the DSA conduit carrying every LAN port.
- Mainline `bgmac.c` has zero CTF/FA awareness.

## v2 finding: FA's op_code field and DSA's tag_brcm opcode check are almost
## certainly the SAME bits - this is a rejection, not a length/shift bug

Reading `net/dsa/tag_brcm.c` (real source, this project's `openwrt/`
build tree) directly:

```c
static struct sk_buff *brcm_tag_rcv_ll(struct sk_buff *skb, ...)
{
	...
	/* The opcode should never be different than 0b000 */
	if (unlikely((brcm_tag[0] >> BRCM_OPCODE_SHIFT) & BRCM_OPCODE_MASK))
		return NULL;   // <-- silently drops the frame
	...
}
```

`BRCM_OPCODE_SHIFT` is 5, `BRCM_OPCODE_MASK` is `0x7` - i.e. **the top 3 bits
of the tag's first byte.** Compare directly against `etc_fa.h`'s `bcm_hdr_t`:

```c
struct {
	uint32_t src_pid   :5;  /* 4:0 */
	uint32_t tc        :3;  /* 7:5 */
	...
	uint32_t op_code   :3;  /* 31:29, i.e. top 3 bits of the first byte */
} oc0;  /* and oc10, same op_code position */
```

**FA's `op_code` occupies the exact same bit positions DSA's own opcode
check inspects.** `fa_process_rx()` treats `op_code==0x2` as a real hit
(decodes `napt_flow_id`) and `op_code==0x1` as the 8-byte variant - both
non-zero. DSA's `brcm_tag_rcv_ll()` was written years before FA support
existed anywhere near mainline and has exactly one rule: *any* non-zero
opcode is treated as a malformed/unrecognized tag and the frame is
**silently dropped** (`return NULL`), never reaching the bridge at all.

This supersedes v1's "extra unstripped bytes shift the whole frame"
model. It also directly explains an observation v1's model didn't:
**the loss rate was variable (60% then 100%), not both a fixed ~100%** -
under a pure byte-shift model every single frame would misparse
identically; under the opcode-collision model, only frames the switch's
own FA logic actually tags with a non-zero opcode (hits, or whatever
subset of traffic FA's classifier is engaging on) get dropped - a data-
dependent rate is exactly what a firing-condition-gated rejection
predicts, not a structural shift.

**Both the tag format and its wire position still need direct
confirmation** - v1's claim that FA's header sits *before* the whole
frame came from a general deepwiki answer, not this project's own read
of the real source; the real `tag_brcm.c` shows the *default*
`DSA_TAG_PROTO_BRCM` tag sits **after the MAC source address**, not
before it (`brcm_tag_xmit`: "Build the tag after the MAC Source
Address", offset `2 * ETH_ALEN`) - a `DSA_TAG_PROTO_BRCM_PREPEND` variant
also exists (tag genuinely first) and it is not yet confirmed which one
this exact `b53` instance uses. This is a real, still-open, checkable
question (see "Remaining verification" below) - the opcode-collision
finding does not depend on resolving it (either wire position hits the
same opcode-bit-position check), but the exact patch location inside
`tag_brcm.c` does.

## v3 finding: mainline b53 ALREADY has a "this chip needs the FA-aware
## tag" special case - for the wrong device ID

Checked `drivers/net/dsa/b53/b53_common.c`'s real `b53_get_tag_protocol()`
(same `openwrt/` build tree) for how the tag protocol variant actually
gets selected, and found mainline maintainers already solved almost
exactly this problem, for a sibling chip:

```c
/* Broadcom BCM58xx chips have a flow accelerator on Port 8
 * which requires us to use the prepended Broadcom tag type
 */
if (dev->chip_id == BCM58XX_DEVICE_ID && port == B53_CPU_PORT) {
	dev->tag_protocol = DSA_TAG_PROTO_BRCM_PREPEND;
	goto out;
}
dev->tag_protocol = DSA_TAG_PROTO_BRCM;   /* <-- this router falls through to here */
```

Mainline **already documents, in its own comment, that a flow accelerator
on the CPU port requires the PREPEND tag variant** - this is exactly
this project's own problem, already solved once, for `BCM58XX_DEVICE_ID`
only. This router's actual switch chip ID is `0x53012`
(`BCM53012_DEVICE_ID`) - already a real, fully-integrated entry
elsewhere in this same file's chip table (confirmed:
`.chip_id = BCM53012_DEVICE_ID, .dev_name = "BCM53012", ... .imp_port = 8`,
same IMP/CPU port index the BCM58xx special case checks) - but it is
**not** included in the flow-accelerator/PREPEND check above, so this
router falls through to the plain `DSA_TAG_PROTO_BRCM` protocol
unconditionally, regardless of whether FA is enabled.

This reframes the candidate fix again, to something smaller and more
directly evidenced than either v1 or v2's sketch:

```c
if ((dev->chip_id == BCM58XX_DEVICE_ID ||
     dev->chip_id == BCM53012_DEVICE_ID) && port == B53_CPU_PORT) {
	dev->tag_protocol = DSA_TAG_PROTO_BRCM_PREPEND;
	goto out;
}
```

**One condition, reusing code mainline already ships and already tests
for a sibling chip with the identical hardware feature** - not a new
driver, not even a new function, the smallest possible change consistent
with "this chip has a flow accelerator, use the tag format mainline's
own comment says that requires." Whether `DSA_TAG_PROTO_BRCM_PREPEND`
alone is *sufficient* (its `brcm_tag_rcv_prepend()` still calls the same
`brcm_tag_rcv_ll()` with its same strict opcode check - see v2's finding
below) or whether the opcode-acceptance patch from v2 is *also* needed
is the real remaining unknown - these are not mutually exclusive
hypotheses, they may both be required together. Recorded in the order
found, oldest-first, so the reasoning trail stays honest rather than
silently rewritten.

## v2 finding, still relevant: the opcode check itself

If the tag-protocol-selection fix in v3 turns out insufficient alone
(PREPEND positioning fixes *where* the tag is read from but not
*whether* a non-zero opcode gets rejected), the opcode-acceptance
patch below is still the needed complement: **teach
`brcm_tag_rcv_ll()` to accept FA's non-zero opcodes instead of
unconditionally rejecting them**, rather than building a second parallel
header on top of the existing tag. Concretely, in `net/dsa/tag_brcm.c`:

```c
/* was: */
if (unlikely((brcm_tag[0] >> BRCM_OPCODE_SHIFT) & BRCM_OPCODE_MASK))
	return NULL;

/* candidate fix - accept FA's known opcodes instead of any non-zero: */
op_code = (brcm_tag[0] >> BRCM_OPCODE_SHIFT) & BRCM_OPCODE_MASK;
if (unlikely(op_code != 0 && op_code != FA_OPC_HIT_4B && op_code != FA_OPC_HIT_8B))
	return NULL;
if (op_code == FA_OPC_HIT_8B) {
	/* etc_fa.c's fa_process_rx(): op_code==0x1 means an extra 4 bytes
	 * follow before the "normal" 4-byte tag content starts - pull them
	 * here, mirroring PKTPULL(..., 8) exactly, before falling through
	 * to the existing source_port/skb_pull_rcsum logic below. */
}
/* FA_OPC_HIT_4B (0x2): napt_flow_id/hdr_chk_result live in the SAME
 * word tag_brcm already parses for other fields - decode alongside
 * source_port, don't discard. */
```

This reuses ALL of `brcm_tag_rcv_ll()`'s existing source-port extraction,
frame validation, and `skb_pull_rcsum()` logic unchanged - the patch is
a relaxation of one `if`, not a parallel implementation. This is a much
better fit for this project's own reuse-don't-reinvent discipline than
v1's sketch, and it's a real, testable, falsifiable hypothesis: if this
diagnosis is right, this exact patch (plus a kernel rebuild) should make
`bcm_hdr_ctl` traffic stay clean where it currently drops 60-100% of
frames.

## Remaining verification before writing the real patch

1. ~~Confirm which `DSA_TAG_PROTO_BRCM*` variant this board's `b53` driver
   actually selects~~ **DONE (v3 above):** confirmed live in
   `b53_common.c` - this router's chip (`BCM53012_DEVICE_ID`) falls
   through to plain `DSA_TAG_PROTO_BRCM`, unlike sibling `BCM58XX_DEVICE_ID`
   chips with the same flow-accelerator feature, which mainline already
   special-cases to `DSA_TAG_PROTO_BRCM_PREPEND`.
2. **Confirm the exact bit position match precisely**, byte-for-byte,
   between `bcm_hdr_t`'s `oc0`/`oc10` layout and `tag_brcm.c`'s
   `BRCM_OPCODE_SHIFT`/`BRCM_IG_*`/`BRCM_EG_*` field layout - the 3-bit
   opcode position matching is a strong signal, not yet a byte-for-byte
   proven identity of the whole 32-bit word.
3. **This is a full kernel source change**, not an out-of-tree module -
   needs a real image rebuild (`openwrt/` tree, not the SDK used for
   `hwoffload-research/fa-probe/*.ko`) and a full reflash to test, not an
   insmod/rmmod cycle. Higher stakes than anything tested live so far;
   the auto-revert-watchdog pattern doesn't directly apply to a kernel
   image (can't `rmmod` a statically-linked tag driver) - a bad patch
   here would need a sysupgrade-back-to-a-known-good-image revert, not a
   live unload. Plan the test accordingly: keep the immediately-prior
   working image on hand and ready to reflash, exactly as this project's
   own recovery-net discipline already requires for every other flash.

## What this v2 finding is worth, independent of ever building the patch

Even without writing or testing the actual patch, this is a materially
better, source-grounded answer to "why does enabling FA's header break
traffic" than v1's guess: it's a specific, named line of code
(`tag_brcm.c`'s opcode check) with a specific, checkable reason, not a
general "something about extra bytes." That's real progress on the
open question FINDINGS.md #65 left unresolved, obtained by reading this
project's own already-present, real kernel source tree rather than by
running the same live experiment a third time.
