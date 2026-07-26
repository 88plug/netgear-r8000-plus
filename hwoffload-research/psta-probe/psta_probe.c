// SPDX-License-Identifier: GPL-2.0
/*
 * psta_probe - one-shot diagnostic: send the "psta" iovar (Broadcom
 * Proxy-STA / Proxy-STA-Repeater firmware mode) to an already-associated
 * brcmfmac STA interface, using brcmfmac's own EXPORT_SYMBOL_GPL'd
 * brcmf_fil_iovar_data_set() - no brcmfmac source modification at all.
 *
 * "psta" and its companion "psta_mrpt" are confirmed-present ASCII
 * strings inside this router's own loaded firmware blob (both the 2015
 * and 2021 blobs). PSTA_MODE_REPEATER = 2 (from Broadcom's own
 * wlioctl_defs.h, cross-checked against multiple vendor SDK trees).
 * This probe tests whether the firmware actually ACCEPTS the iovar on
 * this exact chip - the only way to know without vendor docs.
 *
 * Module param ifname selects the target interface (default phy1-sta0).
 * Unloading the module reverts psta to PSTA_MODE_DISABLED (0).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-shot psta/PSTA_MODE_REPEATER iovar probe against brcmfmac");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "brcmfmac STA netdev to target (default phy1-sta0)");

static int mrpt = 0;
module_param(mrpt, int, 0444);
MODULE_PARM_DESC(mrpt, "psta_mrpt value to set alongside psta (default 0)");

/* Opaque - we never dereference this struct's fields ourselves, only
 * hand the pointer to brcmfmac's own exported function, which has the
 * real definition internally. */
struct brcmf_if;

/* Declared (never linked directly - brcmfmac was built in a different
 * tree than this probe, so no Module.symvers is available for normal
 * static cross-module linking) purely so symbol_get()'s typeof() has
 * something to resolve; symbol_get() does a live kernel symbol-table
 * lookup by name instead and pins the owning module while held. */
extern int brcmf_fil_iovar_data_set(struct brcmf_if *ifp, const char *name,
				     const void *data, unsigned int len);
extern int brcmf_fil_cmd_data_set(struct brcmf_if *ifp, unsigned int cmd,
				   const void *data, unsigned int len);
extern int brcmf_fil_iovar_data_get(struct brcmf_if *ifp, const char *name,
				     void *data, unsigned int len);

typedef int (*brcmf_fil_iovar_data_set_t)(struct brcmf_if *ifp, const char *name,
					   const void *data, unsigned int len);
typedef int (*brcmf_fil_cmd_data_set_t)(struct brcmf_if *ifp, unsigned int cmd,
					 const void *data, unsigned int len);
typedef int (*brcmf_fil_iovar_data_get_t)(struct brcmf_if *ifp, const char *name,
					   void *data, unsigned int len);
static brcmf_fil_iovar_data_set_t brcmf_fil_iovar_data_set_fn;
static brcmf_fil_cmd_data_set_t brcmf_fil_cmd_data_set_fn;
static brcmf_fil_iovar_data_get_t brcmf_fil_iovar_data_get_fn;

#define PSTA_MODE_DISABLED	0
#define PSTA_MODE_PROXY		1
#define PSTA_MODE_REPEATER	2

/* From brcmfmac's fwil.h dongle command codes */
#define BRCMF_C_UP		2
#define BRCMF_C_DOWN		3

static void encode_le32(unsigned char *buf, unsigned int val)
{
	buf[0] = (unsigned char)(val & 0xff);
	buf[1] = (unsigned char)((val >> 8) & 0xff);
	buf[2] = (unsigned char)((val >> 16) & 0xff);
	buf[3] = (unsigned char)((val >> 24) & 0xff);
}

static struct net_device *target_ndev;

static int fw_cmd(struct brcmf_if *ifp, unsigned int cmd, unsigned int val)
{
	unsigned char buf[4];

	encode_le32(buf, val);
	return brcmf_fil_cmd_data_set_fn(ifp, cmd, buf, sizeof(buf));
}

/*
 * Replicates the real Broadcom wlconf.c sequence around the psta iovar:
 * bring the interface DOWN first, set psta (+ psta_mrpt) while down, then
 * bring it back UP - many Broadcom firmware "mode" iovars are documented
 * (and observed elsewhere in this exact driver, e.g. the apsta-forcing
 * code) to only take effect / only be accepted while the interface is
 * down. Our first probe attempt set psta on an already-up, already-
 * associated interface and got -52; this tests whether DOWN/UP bracketing
 * is the missing piece rather than a hard firmware rejection.
 */
static int set_psta(struct brcmf_if *ifp, unsigned int mode, unsigned int mrpt_val)
{
	unsigned char buf[4];
	int err, down_err, up_err;

	down_err = fw_cmd(ifp, BRCMF_C_DOWN, 1);
	pr_info("psta_probe: BRCMF_C_DOWN -> err=%d\n", down_err);

	encode_le32(buf, mode);
	err = brcmf_fil_iovar_data_set_fn(ifp, "psta", buf, sizeof(buf));
	pr_info("psta_probe: set psta=%u -> err=%d\n", mode, err);

	if (!err && mode == PSTA_MODE_REPEATER) {
		encode_le32(buf, mrpt_val);
		err = brcmf_fil_iovar_data_set_fn(ifp, "psta_mrpt", buf, sizeof(buf));
		pr_info("psta_probe: set psta_mrpt=%u -> err=%d\n", mrpt_val, err);
	}

	up_err = fw_cmd(ifp, BRCMF_C_UP, 1);
	pr_info("psta_probe: BRCMF_C_UP -> err=%d\n", up_err);

	return err;
}

static int __init psta_probe_init(void)
{
	struct brcmf_if *ifp;

	brcmf_fil_iovar_data_set_fn =
		(brcmf_fil_iovar_data_set_t)symbol_get(brcmf_fil_iovar_data_set);
	if (!brcmf_fil_iovar_data_set_fn) {
		pr_err("psta_probe: brcmf_fil_iovar_data_set not found - is brcmfmac loaded?\n");
		return -ENOENT;
	}
	brcmf_fil_cmd_data_set_fn =
		(brcmf_fil_cmd_data_set_t)symbol_get(brcmf_fil_cmd_data_set);
	if (!brcmf_fil_cmd_data_set_fn) {
		pr_err("psta_probe: brcmf_fil_cmd_data_set not found - is brcmfmac loaded?\n");
		symbol_put(brcmf_fil_iovar_data_set);
		brcmf_fil_iovar_data_set_fn = NULL;
		return -ENOENT;
	}
	brcmf_fil_iovar_data_get_fn =
		(brcmf_fil_iovar_data_get_t)symbol_get(brcmf_fil_iovar_data_get);
	if (!brcmf_fil_iovar_data_get_fn)
		pr_err("psta_probe: brcmf_fil_iovar_data_get not found (readback disabled)\n");

	target_ndev = dev_get_by_name(&init_net, ifname);
	if (!target_ndev) {
		pr_err("psta_probe: interface '%s' not found\n", ifname);
		symbol_put(brcmf_fil_iovar_data_set);
		symbol_put(brcmf_fil_cmd_data_set);
		brcmf_fil_iovar_data_set_fn = NULL;
		brcmf_fil_cmd_data_set_fn = NULL;
		return -ENODEV;
	}

	ifp = (struct brcmf_if *)netdev_priv(target_ndev);
	pr_info("psta_probe: targeting '%s', ifp=%p\n", ifname, ifp);

	set_psta(ifp, PSTA_MODE_REPEATER, mrpt);

	return 0;
}

static void __exit psta_probe_exit(void)
{
	if (target_ndev) {
		struct brcmf_if *ifp = (struct brcmf_if *)netdev_priv(target_ndev);

		if (brcmf_fil_iovar_data_get_fn) {
			unsigned char buf[4] = { 0, 0, 0, 0 };
			int gerr = brcmf_fil_iovar_data_get_fn(ifp, "psta", buf, sizeof(buf));
			unsigned int val = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

			pr_info("psta_probe: readback psta=%u -> err=%d (before revert)\n", val, gerr);
		}

		pr_info("psta_probe: reverting psta to PSTA_MODE_DISABLED on unload\n");
		set_psta(ifp, PSTA_MODE_DISABLED, 0);
		dev_put(target_ndev);
	}
	if (brcmf_fil_iovar_data_set_fn) {
		symbol_put(brcmf_fil_iovar_data_set);
		brcmf_fil_iovar_data_set_fn = NULL;
	}
	if (brcmf_fil_cmd_data_set_fn) {
		symbol_put(brcmf_fil_cmd_data_set);
		brcmf_fil_cmd_data_set_fn = NULL;
	}
	if (brcmf_fil_iovar_data_get_fn) {
		symbol_put(brcmf_fil_iovar_data_get);
		brcmf_fil_iovar_data_get_fn = NULL;
	}
}

module_init(psta_probe_init);
module_exit(psta_probe_exit);
