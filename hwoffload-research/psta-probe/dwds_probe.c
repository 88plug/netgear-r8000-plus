// SPDX-License-Identifier: GPL-2.0
/*
 * dwds_probe - raw-iovar test of "wds"/"dwds" (Dynamic WDS), the same
 * cap-string tokens as "psta"/"psr" but never probed at the firmware
 * level directly - only the kernel bridge-layer rejection of 4-address
 * STA bridging was tested (brctl addif -> "Not supported", a driver-gap
 * finding). This checks whether the FIRMWARE itself accepts wds/dwds on
 * the STA interface at all, independent of that driver gap, using the
 * same DOWN/set/UP/readback technique already proven correct for psta.
 *
 * Peer-review finding this addresses: "wds dwds" appear in the same cap
 * string as "psta psr" (cap_readback.ko's own output) but were never
 * raw-iovar probed the way psta was - only the kernel-level 4addr
 * rejection was tested, which proves a driver gap, not a firmware
 * capability gap.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Raw-iovar probe of wds/dwds on the STA interface");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);

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
static brcmf_fil_iovar_data_set_t set_fn;
static brcmf_fil_cmd_data_set_t cmd_fn;
static brcmf_fil_iovar_data_get_t get_fn;

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
	return cmd_fn(ifp, cmd, buf, sizeof(buf));
}

static int try_iovar(struct brcmf_if *ifp, const char *name, unsigned int val)
{
	unsigned char buf[4];
	int err;

	encode_le32(buf, val);
	err = set_fn(ifp, name, buf, sizeof(buf));
	pr_info("dwds_probe: set %s=%u -> err=%d\n", name, val, err);
	return err;
}

static void readback(struct brcmf_if *ifp, const char *name)
{
	unsigned char buf[4] = { 0, 0, 0, 0 };
	int err;
	unsigned int val;

	if (!get_fn)
		return;
	err = get_fn(ifp, name, buf, sizeof(buf));
	val = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	pr_info("dwds_probe: readback %s=%u -> err=%d\n", name, val, err);
}

static int __init dwds_probe_init(void)
{
	struct brcmf_if *ifp;
	int down_err, up_err;

	set_fn = (brcmf_fil_iovar_data_set_t)symbol_get(brcmf_fil_iovar_data_set);
	if (!set_fn) {
		pr_err("dwds_probe: brcmf_fil_iovar_data_set not found\n");
		return -ENOENT;
	}
	cmd_fn = (brcmf_fil_cmd_data_set_t)symbol_get(brcmf_fil_cmd_data_set);
	if (!cmd_fn) {
		pr_err("dwds_probe: brcmf_fil_cmd_data_set not found\n");
		symbol_put(brcmf_fil_iovar_data_set);
		set_fn = NULL;
		return -ENOENT;
	}
	get_fn = (brcmf_fil_iovar_data_get_t)symbol_get(brcmf_fil_iovar_data_get);
	if (!get_fn)
		pr_err("dwds_probe: brcmf_fil_iovar_data_get not found (readback disabled)\n");

	target_ndev = dev_get_by_name(&init_net, ifname);
	if (!target_ndev) {
		pr_err("dwds_probe: interface '%s' not found\n", ifname);
		symbol_put(brcmf_fil_iovar_data_set);
		symbol_put(brcmf_fil_cmd_data_set);
		set_fn = NULL;
		cmd_fn = NULL;
		return -ENODEV;
	}

	ifp = (struct brcmf_if *)netdev_priv(target_ndev);
	pr_info("dwds_probe: targeting '%s', ifp=%p\n", ifname, ifp);

	down_err = fw_cmd(ifp, BRCMF_C_DOWN, 1);
	pr_info("dwds_probe: BRCMF_C_DOWN -> err=%d\n", down_err);

	try_iovar(ifp, "wds", 1);
	try_iovar(ifp, "dwds", 1);

	up_err = fw_cmd(ifp, BRCMF_C_UP, 1);
	pr_info("dwds_probe: BRCMF_C_UP -> err=%d\n", up_err);

	readback(ifp, "wds");
	readback(ifp, "dwds");

	return 0;
}

static void __exit dwds_probe_exit(void)
{
	if (target_ndev) {
		struct brcmf_if *ifp = (struct brcmf_if *)netdev_priv(target_ndev);

		fw_cmd(ifp, BRCMF_C_DOWN, 1);
		try_iovar(ifp, "wds", 0);
		try_iovar(ifp, "dwds", 0);
		fw_cmd(ifp, BRCMF_C_UP, 1);
		dev_put(target_ndev);
	}
	if (set_fn) { symbol_put(brcmf_fil_iovar_data_set); set_fn = NULL; }
	if (cmd_fn) { symbol_put(brcmf_fil_cmd_data_set); cmd_fn = NULL; }
	if (get_fn) { symbol_put(brcmf_fil_iovar_data_get); get_fn = NULL; }
}

module_init(dwds_probe_init);
module_exit(dwds_probe_exit);
