// SPDX-License-Identifier: GPL-2.0
/*
 * apsta_probe - re-test of the apsta=1 forcing patches (862/863) with the
 * SAME rigor §21/§22 eventually applied to psta after catching a
 * methodology bug there: bracket the set with BRCMF_C_DOWN/BRCMF_C_UP,
 * apply it to the PRIMARY/STA ifp (matching brcmf_p2p_set_firmware()'s
 * own precedent exactly - the only place in this driver apsta=1 is known
 * to be accepted), and do it BEFORE a fresh reassociation rather than as
 * a late toggle on an already-associated, already-scheduled radio.
 *
 * Peer-review finding this addresses: patch 862 DOWN/UP-brackets the set
 * but on the AP-role ifp, after the STA already associated under
 * apsta=0. Patch 863 uses the correct (primary/STA) ifp but with NO
 * DOWN/UP bracket at all - a bare live set on an already-up interface,
 * the exact same class of methodology bug the first psta probe made
 * (later corrected, and the correction reversed the whole conclusion).
 * This probe applies the correct ifp AND the correct bracket together,
 * for the first time.
 *
 * Sequence, matching brcmf_p2p_set_firmware() (p2p.c) exactly:
 *   BRCMF_C_DOWN -> set apsta=1 -> BRCMF_C_UP
 * on phy1-sta0 (the primary/STA ifp for radio1). Does NOT force
 * reassociation itself - the operator/test script should do that
 * afterward (wpa_cli reconnect, or let the DOWN/UP cycle's own
 * disassociate trigger wpa_supplicant's automatic reconnect) so the STA
 * establishes its firmware-side scheduling state fresh, under apsta=1,
 * rather than having it already fixed under apsta=0.
 *
 * Unloading reverts apsta to 0 (the driver's own unpatched default).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Re-test apsta=1 with correct DOWN/UP bracket + correct (primary/STA) ifp");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "brcmfmac primary/STA netdev to target (default phy1-sta0)");

struct brcmf_if;

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

#define BRCMF_C_UP	2
#define BRCMF_C_DOWN	3

static struct net_device *target_ndev;

static void encode_le32(unsigned char *buf, unsigned int val)
{
	buf[0] = (unsigned char)(val & 0xff);
	buf[1] = (unsigned char)((val >> 8) & 0xff);
	buf[2] = (unsigned char)((val >> 16) & 0xff);
	buf[3] = (unsigned char)((val >> 24) & 0xff);
}

static int fw_cmd(struct brcmf_if *ifp, unsigned int cmd, unsigned int val)
{
	unsigned char buf[4];

	encode_le32(buf, val);
	return brcmf_fil_cmd_data_set_fn(ifp, cmd, buf, sizeof(buf));
}

static int set_apsta(struct brcmf_if *ifp, unsigned int mode)
{
	unsigned char buf[4];
	int err, down_err, up_err;

	down_err = fw_cmd(ifp, BRCMF_C_DOWN, 1);
	pr_info("apsta_probe: BRCMF_C_DOWN -> err=%d\n", down_err);

	encode_le32(buf, mode);
	err = brcmf_fil_iovar_data_set_fn(ifp, "apsta", buf, sizeof(buf));
	pr_info("apsta_probe: set apsta=%u -> err=%d\n", mode, err);

	up_err = fw_cmd(ifp, BRCMF_C_UP, 1);
	pr_info("apsta_probe: BRCMF_C_UP -> err=%d\n", up_err);

	return err;
}

static int __init apsta_probe_init(void)
{
	struct brcmf_if *ifp;

	brcmf_fil_iovar_data_set_fn =
		(brcmf_fil_iovar_data_set_t)symbol_get(brcmf_fil_iovar_data_set);
	if (!brcmf_fil_iovar_data_set_fn) {
		pr_err("apsta_probe: brcmf_fil_iovar_data_set not found\n");
		return -ENOENT;
	}
	brcmf_fil_cmd_data_set_fn =
		(brcmf_fil_cmd_data_set_t)symbol_get(brcmf_fil_cmd_data_set);
	if (!brcmf_fil_cmd_data_set_fn) {
		pr_err("apsta_probe: brcmf_fil_cmd_data_set not found\n");
		symbol_put(brcmf_fil_iovar_data_set);
		brcmf_fil_iovar_data_set_fn = NULL;
		return -ENOENT;
	}
	brcmf_fil_iovar_data_get_fn =
		(brcmf_fil_iovar_data_get_t)symbol_get(brcmf_fil_iovar_data_get);
	if (!brcmf_fil_iovar_data_get_fn)
		pr_err("apsta_probe: brcmf_fil_iovar_data_get not found (readback disabled)\n");

	target_ndev = dev_get_by_name(&init_net, ifname);
	if (!target_ndev) {
		pr_err("apsta_probe: interface '%s' not found\n", ifname);
		symbol_put(brcmf_fil_iovar_data_set);
		symbol_put(brcmf_fil_cmd_data_set);
		brcmf_fil_iovar_data_set_fn = NULL;
		brcmf_fil_cmd_data_set_fn = NULL;
		return -ENODEV;
	}

	ifp = (struct brcmf_if *)netdev_priv(target_ndev);
	pr_info("apsta_probe: targeting '%s', ifp=%p\n", ifname, ifp);

	set_apsta(ifp, 1);

	return 0;
}

static void __exit apsta_probe_exit(void)
{
	if (target_ndev) {
		struct brcmf_if *ifp = (struct brcmf_if *)netdev_priv(target_ndev);

		if (brcmf_fil_iovar_data_get_fn) {
			unsigned char buf[4] = { 0, 0, 0, 0 };
			int gerr = brcmf_fil_iovar_data_get_fn(ifp, "apsta", buf, sizeof(buf));
			unsigned int val = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

			pr_info("apsta_probe: readback apsta=%u -> err=%d (before revert)\n", val, gerr);
		}

		pr_info("apsta_probe: reverting apsta to 0 on unload\n");
		set_apsta(ifp, 0);
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

module_init(apsta_probe_init);
module_exit(apsta_probe_exit);
