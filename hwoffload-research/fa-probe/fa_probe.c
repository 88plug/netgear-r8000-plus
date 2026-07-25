// SPDX-License-Identifier: GPL-2.0
/*
 * fa_probe.c - Read-only diagnostic probe for the Broadcom BCM4709
 * on-chip Flow-Accelerator (FA / "CTF" hardware NAT-offload) register
 * block, as documented in Broadcom's SDK6 fa_core.h.
 *
 * Physical address derivation (see hwoffload-research/VERDICT.md, section
 * "Minimal first-step proof-of-concept"):
 *
 *   - mainline device-tree base for the gmac3/GMAC-3 core-unit register
 *     window on this SoC (bcm-ns.dtsi):           0x18027000
 *   - Broadcom's own FA_BASE_OFFSET, from fa_core.h (FA registers live
 *     inside GMAC-3's corereg window, per the etc_fa.c comment):  0xc00
 *   - probe base = 0x18027000 + 0xc00 =            0x18027c00
 *
 * This is an INFERRED address (no BCM4709 datasheet confirms it directly),
 * cross-checked against two independent facts: (1) the leaked Broadcom
 * SDK6 driver source's own offset constant, and (2) the R8000 board DTS
 * not overriding gmac0..gmac3, combined with gmac3's DT `reg` covering
 * only the first 2KB of the 4KB core window - meaning offset 0xc00 (3KB
 * in) is outside whatever `bgmac` itself reserves, so there is no known
 * request_mem_region() conflict with the running brcm/bgmac driver stack.
 *
 * Register offsets below come directly from the `faregs_t` struct in
 * fa_core.h (graveyard-vendor/extracted-source/fa_core.h):
 *   control  @ 0x00
 *   status   @ 0x30
 *   stats[]  @ 0x3c .. 0x63  (10 x u32 hit/miss/fail counters)
 *   ...      through m_accdata[7] @ 0xbc, i.e. struct end at 0xc0.
 *
 * THIS MODULE PERFORMS ONLY READS. It does not write to the FA block, does
 * not attempt the fa_core.h CTF_CTL_*_INIT strobe / bring-up sequence, and
 * does not remap or touch GMAC-3's own register range (0x18027000 -
 * 0x180277ff), which `bgmac` may itself have partially reserved - this
 * probe's mapping starts strictly at the FA sub-window, 0xc00 bytes in.
 *
 * =====================================================================
 * RISK ASSESSMENT - read before loading this module on real hardware
 * =====================================================================
 *
 * There are two structurally different ways an MMIO read can go wrong on
 * this platform, and they carry very different risk levels. Which one
 * applies here is the entire question this probe exists to answer, and
 * this comment records the reasoning for why one is judged far more
 * likely than the other - it is an architectural inference, NOT an
 * empirical guarantee, because nobody has run this exact read against
 * real R8000 silicon before.
 *
 * 1. Reading a genuinely UNMAPPED/RESERVED physical address - one that
 *    does not correspond to any core, wrapper, or peripheral the SoC's
 *    on-chip interconnect (Broadcom's "SiliconBackplane", an AMBA/AXI-
 *    derived on-chip bus fabric) knows how to decode at all. Such an
 *    access typically falls through to the interconnect's default-slave/
 *    error path, which the ARM AXI-to-CPU bridge converts into a
 *    synchronous external abort ("external abort on non-linefetch").
 *    This IS dangerous: a synchronous external abort taken inside a
 *    readl() in kernel context is, on mainline ARM32 Linux, normally
 *    fatal - there is no generic exception-table-based recovery for a
 *    hardware bus-error response the way there is for a page fault on an
 *    unmapped *virtual* address (that's what copy_from_kernel_nofault()-
 *    style helpers protect against, and it is NOT the same failure mode
 *    as an external abort from a real bus error). If this were the
 *    situation, the likely outcome is a kernel oops/panic on load.
 *
 * 2. Reading a REAL, backplane-decoded core wrapper's register bank while
 *    the core's *internal logic* is clock-gated, held in reset, or
 *    otherwise not brought up - e.g. a hardware feature block that
 *    exists on the die but was never enabled by this SKU's firmware/
 *    NVRAM. On Broadcom's SiliconBackplane, the wrapper/bridge logic that
 *    answers backplane transactions for a given core-unit is a distinct,
 *    always-present piece of interconnect glue from the core's own
 *    internal register logic; the wrapper keeps responding to backplane
 *    reads even when the inner core is unclocked, most commonly with a
 *    fixed pattern (0xFFFFFFFF or 0x00000000) rather than a bus abort,
 *    because the transaction is still being answered by a real, decoded
 *    slave - it just has nothing live behind the specific register
 *    offset. This is architecturally the SAME core-unit/backplane wrapper
 *    that owns GMAC-3's known-decoded, known-populated register window
 *    (0x18027000-0x180277ff) - the wrapper's existence and liveness on
 *    THIS exact board is not in question, only whether the FA sub-block
 *    behind offset 0xc00 within it is clocked/enabled.
 *
 * ASSESSMENT: because FA_PHYS_BASE (0x18027c00) sits *inside* the GMAC-3
 * core-unit's already-decoded, already-populated 4KB backplane aperture -
 * not in some unrelated unmapped gap of the physical address map - this
 * read is judged to fall into risk category (2), not (1): most likely
 * outcome is a clean, all-1s or all-0s readback if FA is unclocked/
 * absent on this SKU, or plausible non-trivial register content if it is
 * live. A hard external-abort fault is assessed as LOW-but-NONZERO
 * probability, not ruled out - segmentation-fused SKUs can in principle
 * omit the backplane slave port for a disabled feature entirely, which
 * would put this back in category (1). This is precisely the open
 * question VERDICT.md identifies as unverified by anyone to date, on any
 * R8000 board, and is the reason this module exists.
 *
 * IMPORTANT: readl() is used below because it is the semantically correct
 * MMIO accessor (proper compiler/memory barriers, no raw-pointer-
 * dereference undefined behavior) - it is NOT, and must not be read as, a
 * software safety net against a hardware bus fault. If risk category (1)
 * turns out to apply, readl() will fault exactly as a raw dereference
 * would. The defensive measures actually available in software here are
 * limited to: (a) checking ioremap()'s return value (ioremap() itself
 * only builds page tables and cannot fault - the risk is entirely in the
 * readl() calls that follow it), (b) mapping only the minimal 0x100-byte
 * window needed, (c) performing no writes whatsoever, and (d) having a
 * known-good recovery path staged before running this on the router (see
 * docs/RUNBOOK.md "Recovery / unbrick": nmrpflash + stock
 * .chk on hand) in case a fault does occur and the board needs recovery.
 *
 * Net call: safe enough to justify running as a deliberate, later step
 * with recovery tooling staged and ready - not zero-risk, and this
 * module must NOT be loaded on the device until that separate decision
 * is made.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>

/* GMAC-3 corereg base (0x18027000) + Broadcom's FA_BASE_OFFSET (0xc00) */
#define FA_PHYS_BASE	0x18027c00UL

/* faregs_t runs control(0x00) .. m_accdata[7] end at 0xc0; round up with
 * margin so the whole struct, including the stats[] array, is covered.
 */
#define FA_MAP_SIZE	0x100UL

/* Offsets within faregs_t (fa_core.h) */
#define FA_REG_CONTROL	0x00
#define FA_REG_STATUS	0x30

static void __iomem *fa_base;

static int __init fa_probe_init(void)
{
	u32 control, status;

	pr_info("fa_probe: probing FA register block at phys=0x%08lx size=0x%lx\n",
		FA_PHYS_BASE, FA_MAP_SIZE);

	/*
	 * ioremap() only establishes a kernel virtual mapping (page-table
	 * entries); it does not itself issue any bus transaction and so
	 * cannot fault. It CAN fail (e.g. vmalloc address space exhaustion),
	 * which must be checked - dereferencing a NULL fa_base below would
	 * be a guaranteed, unconditional oops, independent of anything the
	 * FA hardware does.
	 */
	fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
	if (!fa_base) {
		pr_err("fa_probe: ioremap(0x%08lx, 0x%lx) failed\n",
			FA_PHYS_BASE, FA_MAP_SIZE);
		return -ENOMEM;
	}

	/*
	 * This is where the real risk in this module lives - see the risk
	 * assessment in the file header. readl() is the correct accessor
	 * for MMIO (vs. a raw pointer dereference, which is undefined
	 * behavior on __iomem memory and offers no barrier guarantees), but
	 * it provides no protection against a genuine hardware bus fault if
	 * this offset turns out not to be backed by any live backplane
	 * wrapper at all.
	 */
	control = readl(fa_base + FA_REG_CONTROL);
	status  = readl(fa_base + FA_REG_STATUS);

	pr_info("fa_probe: control=0x%08x status=0x%08x\n", control, status);

	if (control == 0xffffffffU && status == 0xffffffffU)
		pr_info("fa_probe: both registers read all-ones - consistent with "
			"an unclocked/absent FA block behind a live backplane wrapper "
			"(see risk-assessment category 2 in this module's source)\n");
	else if (control == 0U && status == 0U)
		pr_info("fa_probe: both registers read all-zero - consistent with "
			"a held-in-reset FA block, or an uninitialised-but-present one\n");
	else
		pr_info("fa_probe: non-trivial register content observed - possible "
			"sign the FA block is live. Do NOT proceed to the init-strobe/"
			"bring-up sequence based on this alone; further analysis needed "
			"first\n");

	/*
	 * Per the task requirements for this first version: unmap and
	 * return success immediately after the read, rather than holding
	 * the mapping for the module's lifetime. This keeps the loaded
	 * module's footprint on the running kernel minimal - it exists only
	 * to have performed this one read and logged the result via dmesg,
	 * and can be safely unloaded afterward.
	 */
	iounmap(fa_base);
	fa_base = NULL;

	pr_info("fa_probe: unmapped FA register block, probe complete\n");

	return 0;
}

static void __exit fa_probe_exit(void)
{
	/* fa_base was already unmapped in fa_probe_init(); nothing to do
	 * here beyond confirming removal in the kernel log.
	 */
	pr_info("fa_probe: module unloaded\n");
}

module_init(fa_probe_init);
module_exit(fa_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("Read-only MMIO probe for the BCM4709 Flow-Accelerator (FA) register block");
