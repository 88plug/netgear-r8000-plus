// SPDX-License-Identifier: GPL-2.0
/*
 * psta_readback - read-only companion to psta_probe: reads back the
 * current "psta" iovar value without touching it, so we can confirm a
 * prior set actually stuck (rather than being silently reset on the
 * BRCMF_C_UP transition or by re-association).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only psta iovar readback probe");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);

struct brcmf_if;

extern int brcmf_fil_iovar_data_get(struct brcmf_if *ifp, const char *name,
				     void *data, unsigned int len);

typedef int (*brcmf_fil_iovar_data_get_t)(struct brcmf_if *ifp, const char *name,
					   void *data, unsigned int len);
static brcmf_fil_iovar_data_get_t get_fn;

static int __init psta_readback_init(void)
{
	struct net_device *ndev;
	struct brcmf_if *ifp;
	unsigned char buf[4] = { 0, 0, 0, 0 };
	int err;
	unsigned int val;

	get_fn = (brcmf_fil_iovar_data_get_t)symbol_get(brcmf_fil_iovar_data_get);
	if (!get_fn) {
		pr_err("psta_readback: brcmf_fil_iovar_data_get not found\n");
		return -ENOENT;
	}

	ndev = dev_get_by_name(&init_net, ifname);
	if (!ndev) {
		pr_err("psta_readback: interface '%s' not found\n", ifname);
		symbol_put(brcmf_fil_iovar_data_get);
		return -ENODEV;
	}

	ifp = (struct brcmf_if *)netdev_priv(ndev);
	err = get_fn(ifp, "psta", buf, sizeof(buf));
	val = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	pr_info("psta_readback: psta=%u -> err=%d (0=DISABLED,1=PROXY,2=REPEATER)\n", val, err);

	dev_put(ndev);
	symbol_put(brcmf_fil_iovar_data_get);
	return 0;
}

static void __exit psta_readback_exit(void)
{
}

module_init(psta_readback_init);
module_exit(psta_readback_exit);
