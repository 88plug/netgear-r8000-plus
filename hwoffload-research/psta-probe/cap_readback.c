// SPDX-License-Identifier: GPL-2.0
/*
 * cap_readback - read-only: dump the firmware's "cap" iovar string
 * (its own advertised capability list) directly from a live brcmfmac
 * interface, via the same symbol_get() technique as psta_readback.c.
 *
 * brcmfmac's own feature.c determines MCHAN/RSDB support by substring-
 * matching this exact string ("mchan"/"rsdb" tokens) against the
 * firmware's own self-reported capabilities - this reads that string
 * directly so we can verify (not infer from driver behavior) whether
 * this exact chip/firmware combination's own cap string contains
 * "mchan" or "rsdb" at all.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only firmware 'cap' iovar dump");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);

struct brcmf_if;
extern int brcmf_fil_iovar_data_get(struct brcmf_if *ifp, const char *name,
				     void *data, unsigned int len);
typedef int (*brcmf_fil_iovar_data_get_t)(struct brcmf_if *ifp, const char *name,
					   void *data, unsigned int len);
static brcmf_fil_iovar_data_get_t get_fn;

#define CAP_BUF_LEN 1024

static int __init cap_readback_init(void)
{
	struct net_device *ndev;
	struct brcmf_if *ifp;
	char *buf;
	int err;

	get_fn = (brcmf_fil_iovar_data_get_t)symbol_get(brcmf_fil_iovar_data_get);
	if (!get_fn) {
		pr_err("cap_readback: brcmf_fil_iovar_data_get not found\n");
		return -ENOENT;
	}

	ndev = dev_get_by_name(&init_net, ifname);
	if (!ndev) {
		pr_err("cap_readback: interface '%s' not found\n", ifname);
		symbol_put(brcmf_fil_iovar_data_get);
		return -ENODEV;
	}

	buf = kzalloc(CAP_BUF_LEN, GFP_KERNEL);
	if (!buf) {
		dev_put(ndev);
		symbol_put(brcmf_fil_iovar_data_get);
		return -ENOMEM;
	}

	ifp = (struct brcmf_if *)netdev_priv(ndev);
	err = get_fn(ifp, "cap", buf, CAP_BUF_LEN - 1);
	buf[CAP_BUF_LEN - 1] = '\0';
	pr_info("cap_readback: err=%d\n", err);
	pr_info("cap_readback: cap=\"%s\"\n", buf);
	pr_info("cap_readback: contains 'mchan'? %s\n",
		strnstr(buf, "mchan", CAP_BUF_LEN) ? "YES" : "no");
	pr_info("cap_readback: contains 'rsdb'? %s\n",
		strnstr(buf, "rsdb", CAP_BUF_LEN) ? "YES" : "no");
	pr_info("cap_readback: contains 'p2p'? %s\n",
		strnstr(buf, "p2p", CAP_BUF_LEN) ? "YES" : "no");

	kfree(buf);
	dev_put(ndev);
	symbol_put(brcmf_fil_iovar_data_get);
	return 0;
}

static void __exit cap_readback_exit(void)
{
}

module_init(cap_readback_init);
module_exit(cap_readback_exit);
