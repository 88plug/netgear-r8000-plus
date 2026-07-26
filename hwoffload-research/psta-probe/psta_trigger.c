// SPDX-License-Identifier: GPL-2.0
/*
 * psta_trigger - one-shot: call brcmfmac's new exported
 * brcmf_start_psta_repeater() against an already-associated STA
 * interface and report whether a companion netdev appeared.
 *
 * Unlike psta_probe.c (which poked the "psta" iovar directly via
 * symbol_get() against brcmf_fil_*), this calls the real driver-side
 * function added by patch 864, which additionally arms the vif-event
 * wait so a firmware BRCMF_E_IF_ADD (if the firmware actually sends
 * one for the psta companion bsscfg) gets wired up as a real netdev
 * instead of being silently dropped by brcmf_notify_vif_event().
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/init.h>
#include <linux/err.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trigger brcmf_start_psta_repeater() against an associated STA vif");

static char *ifname = "phy1-sta0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "brcmfmac STA netdev to target (default phy1-sta0)");

static char *newifname = "psta_rpt0";
module_param(newifname, charp, 0444);
MODULE_PARM_DESC(newifname, "name to give the companion netdev if one appears");

static int mrpt = 0;
module_param(mrpt, int, 0444);

struct brcmf_if;

extern struct net_device *brcmf_start_psta_repeater(struct brcmf_if *ifp,
						     const char *ifname,
						     u32 mrpt);

typedef struct net_device *(*brcmf_start_psta_repeater_t)(struct brcmf_if *ifp,
							    const char *ifname,
							    u32 mrpt);
static brcmf_start_psta_repeater_t start_fn;

static struct net_device *target_ndev;

static int __init psta_trigger_init(void)
{
	struct brcmf_if *ifp;
	struct net_device *new_ndev;

	start_fn = (brcmf_start_psta_repeater_t)symbol_get(brcmf_start_psta_repeater);
	if (!start_fn) {
		pr_err("psta_trigger: brcmf_start_psta_repeater not found - is the patched brcmfmac.ko loaded?\n");
		return -ENOENT;
	}

	target_ndev = dev_get_by_name(&init_net, ifname);
	if (!target_ndev) {
		pr_err("psta_trigger: interface '%s' not found\n", ifname);
		symbol_put(brcmf_start_psta_repeater);
		start_fn = NULL;
		return -ENODEV;
	}

	ifp = (struct brcmf_if *)netdev_priv(target_ndev);
	pr_info("psta_trigger: calling brcmf_start_psta_repeater(ifp=%p, name=%s, mrpt=%d)\n",
		ifp, newifname, mrpt);

	new_ndev = start_fn(ifp, newifname, mrpt);
	if (IS_ERR(new_ndev)) {
		pr_info("psta_trigger: RESULT: failed, err=%ld\n", PTR_ERR(new_ndev));
	} else {
		pr_info("psta_trigger: RESULT: companion netdev '%s' created (ifindex=%d)\n",
			new_ndev->name, new_ndev->ifindex);
	}

	return 0;
}

static void __exit psta_trigger_exit(void)
{
	pr_info("psta_trigger: unloading (companion netdev, if any, is left in place - remove manually with 'ip link del')\n");
	if (target_ndev) {
		dev_put(target_ndev);
		target_ndev = NULL;
	}
	if (start_fn) {
		symbol_put(brcmf_start_psta_repeater);
		start_fn = NULL;
	}
}

module_init(psta_trigger_init);
module_exit(psta_trigger_exit);
