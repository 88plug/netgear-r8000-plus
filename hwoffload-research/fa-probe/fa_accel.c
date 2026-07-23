// SPDX-License-Identifier: GPL-2.0
/*
 * fa_accel.c - Phase A+B of the FA/CTF hardware NAT-acceleration driver,
 * per /home/andrew/.claude/plans/drifting-dazzling-mccarthy.md (approved
 * plan). Registers as an indirect TC-flower/flowtable hardware-offload
 * backend (the same interface real hardware offload drivers like
 * drivers/net/ethernet/mediatek/mtk_ppe_offload.c implement), and for now
 * ONLY logs what it sees - it never writes to any FA register and always
 * returns -EOPNOTSUPP, so every flow stays on the existing software
 * flowtable path unconditionally.
 *
 * =====================================================================
 * WHY THIS ARCHITECTURE (not a bespoke netfilter/conntrack hook)
 * =====================================================================
 *
 * This router's own nftables ruleset already runs `flow add @ft` in its
 * forward chain (nft_flow_offload.c) - that is the existing, live signal
 * for "this established connection qualifies for offload". The kernel
 * inserts it into the SOFTWARE flowtable first and unconditionally, then
 * - only afterward, asynchronously, best-effort - looks for a registered
 * hardware-offload backend via the TC-flower indirect-block mechanism
 * (flow_indr_dev_register()) for any flowtable member device that lacks
 * its own ndo_setup_tc (br-lan/wan on this router qualify, since neither
 * bgmac nor the bridge implement hardware TC offload themselves).
 *
 * Every failure path in this callback (returning -EOPNOTSUPP, or any
 * other negative value) is silently absorbed by the framework -
 * confirmed by reading nf_flow_table_offload.c this session: a failed
 * hardware add just means that flow never gets marked hardware-offloaded
 * and continues running on the software flowtable that was already
 * handling it. There is no path back to conntrack or the connection
 * itself. This is why Phase A/B can be loaded on the live router with
 * zero risk to any real traffic - it is architecturally impossible for
 * this module, as written, to drop or misroute a packet, because it
 * never says yes.
 *
 * =====================================================================
 * PHASE A vs PHASE B (both implemented here, always returning -EOPNOTSUPP)
 * =====================================================================
 *
 * Phase A: register the indirect block callback, log every
 * FLOW_CLS_REPLACE/DESTROY/STATS callback that fires. Proves the
 * registration/dispatch plumbing works.
 *
 * Phase B: on FLOW_CLS_REPLACE, additionally decode the generic
 * flow_rule's match keys (protocol, IPv4 addrs, ports) and mangle/
 * redirect actions into a human-readable log of what a real
 * fa_napt_prep_ipv4_word()-style row would contain (see
 * graveyard-vendor/extracted-source/etc_fa.c for that row format) -
 * purely for validating the translation logic against real flows,
 * still never touching hardware or returning success.
 *
 * Phase C (real FA table writes) and Phase D (persistent bring-up) are
 * explicitly NOT part of this file - see the plan doc for why those are
 * separate, later decisions.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/list.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

static LIST_HEAD(fa_block_cb_list);

static void fa_log_ipv4_tuple(const char *what, struct flow_rule *rule)
{
	__be32 src_ip = 0, dst_ip = 0;
	__be16 src_port = 0, dst_port = 0;
	u8 proto = 0;
	bool have_ip = false, have_ports = false;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		proto = match.key->ip_proto;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		src_ip = match.key->src;
		dst_ip = match.key->dst;
		have_ip = true;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		src_port = match.key->src;
		dst_port = match.key->dst;
		have_ports = true;
	}

	if (!have_ip) {
		pr_info("fa_accel: %s: non-IPv4 flow (no IPV4_ADDRS key) - "
			"would be BCME_ERROR/skip in a real fa_napt_add(), same as etc_fa.c's v6 path\n",
			what);
		return;
	}

	pr_info("fa_accel: %s: proto=%u %pI4:%u -> %pI4:%u (pre-NAT/match tuple)\n",
		what, proto, &src_ip, have_ports ? ntohs(src_port) : 0,
		&dst_ip, have_ports ? ntohs(dst_port) : 0);
}

static void fa_log_would_be_napt_row(struct flow_rule *rule)
{
	struct flow_action_entry *act;
	int i;
	bool saw_mangle = false, saw_redirect = false;
	struct net_device *egress_dev = NULL;

	fa_log_ipv4_tuple("match", rule);

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			saw_mangle = true;
			break;
		case FLOW_ACTION_REDIRECT:
			saw_redirect = true;
			egress_dev = act->dev;
			break;
		default:
			break;
		}
	}

	pr_info("fa_accel: would-be NAPT row: action=%s%s egress_dev=%s "
		"(mirrors fa_napt_prep_ipv4_word()'s tbl[1..3] NAT/tuple words "
		"and tbl[6] LAN->WAN/WAN->LAN direction field - not written, "
		"Phase B is log-only)\n",
		saw_mangle ? "CTF_NAPT_OVRW_IP" : "(no NAT mangle seen)",
		saw_redirect ? "+REDIRECT" : "",
		egress_dev ? egress_dev->name : "(none)");
}

static int fa_setup_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		pr_info("fa_accel: FLOW_CLS_REPLACE cookie=0x%lx - decoding (Phase B), "
			"NOT installing (Phase A/B never return success)\n",
			cls->cookie);
		fa_log_would_be_napt_row(cls->rule);
		return -EOPNOTSUPP;
	case FLOW_CLS_DESTROY:
		pr_info("fa_accel: FLOW_CLS_DESTROY cookie=0x%lx\n", cls->cookie);
		return 0;
	case FLOW_CLS_STATS:
		pr_info("fa_accel: FLOW_CLS_STATS cookie=0x%lx - nothing to report, "
			"this flow was never offloaded by us\n", cls->cookie);
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static int fa_indr_setup_cb(struct net_device *dev, struct Qdisc *sch, void *cb_priv,
			     enum tc_setup_type type, void *type_data,
			     void *data,
			     void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_offload *bo = type_data;

	if (!dev)
		return -EOPNOTSUPP;

	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	pr_info("fa_accel: indirect TC_SETUP_FT block setup for dev=%s command=%d\n",
		dev->name, bo->command);

	return flow_block_cb_setup_simple(bo, &fa_block_cb_list, fa_setup_block_cb,
					   dev, dev, false);
}

static int __init fa_accel_init(void)
{
	int ret;

	pr_info("fa_accel: Phase A+B - registering as indirect flowtable hw-offload "
		"backend (log-only, never returns success, zero FA register access)\n");

	ret = flow_indr_dev_register(fa_indr_setup_cb, NULL);
	if (ret) {
		pr_err("fa_accel: flow_indr_dev_register failed (%d)\n", ret);
		return ret;
	}

	pr_info("fa_accel: registered. Will log FLOW_CLS_REPLACE/DESTROY/STATS "
		"for any flowtable member device (e.g. br-lan, wan) without its "
		"own hardware TC offload - always returns -EOPNOTSUPP\n");

	return 0;
}

static void __exit fa_accel_exit(void)
{
	flow_indr_dev_unregister(fa_indr_setup_cb, NULL, NULL);
	pr_info("fa_accel: unregistered, module unloaded\n");
}

module_init(fa_accel_init);
module_exit(fa_accel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("FA/CTF hw-offload driver, Phase A+B (log-only, no hardware access)");
