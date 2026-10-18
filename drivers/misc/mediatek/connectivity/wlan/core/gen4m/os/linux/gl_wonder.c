// SPDX-License-Identifier: GPL-2.0
/*
 * gl_wonder.c - virtual "wonder" cfg80211 phy for Google "mosey" (AirDrop via
 * Quick Share), bridged onto the gen4m proprietary radiotap monitor netdev.
 *
 * What mosey_server / libmosey_daemon_ffi expects (from mosey-extended notes):
 *   - a wiphy named "wonder"
 *   - NL80211_CMD_NEW_INTERFACE type MONITOR named "wonder0"
 *   - NL80211_CMD_SET_CHANNEL to 5745 MHz (ch149), later other social channels
 *   - nl80211 vendor commands, vendor_id 0x1A11, subcmd 1..5
 *   - PF_PACKET/TPACKET_V3 radiotap I/O on wonder0
 *
 * How it is bridged here:
 *   wonder0 TX  -> strip caller radiotap, prepend the bare 8-byte radiotap
 *                  header that wlanMonStartXmit() accepts, then dev_queue_xmit
 *                  on "radiotap0". Management frames only (AWDL PSF/MIF are
 *                  action frames). Data frames are dropped and counted.
 *   radiotap0 RX-> rx_handler on radiotap0 re-targets skbs at wonder0.
 *   channel     -> same firmware command as the "MONITOR" private command.
 *
 * STATUS: first draft, not compiled against or run on a device by the author.
 * The vendor-command attribute layout is unknown and is logged / guessed, see
 * wonder_vendor_* below.
 */

#include "precomp.h"

#if CFG_SUPPORT_SNIFFER

#include <net/cfg80211.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <linux/moduleparam.h>

#define WONDER_PHY_NAME		"wonder"
#define WONDER_IF_NAME		"wonder0"
#define WONDER_VENDOR_ID	0x001A11
#define WONDER_STA_IF		"wlan0"
#define WONDER_DEFAULT_FREQ	5745

/* Attribute id used for the MAC in the get_if_mac_addr reply. UNVERIFIED. */
static int wonder_mac_attr = 1;
module_param(wonder_mac_attr, int, 0644);

struct wonder_vif {
	struct wireless_dev wdev;
};

static struct wiphy *wonder_wiphy;
static struct net_device __rcu *wonder_vif_dev;
static struct net_device __rcu *wonder_mon_dev;	/* gen4m radiotap0 */
static u8 wonder_mac[ETH_ALEN];
static u32 wonder_cur_freq;
static struct work_struct wonder_up_work;

/* ---------------------------------------------------------------- */
/* channel control via the existing gen4m MONITOR firmware command  */
/* ---------------------------------------------------------------- */
static int wonder_set_channel_mhz(u32 mhz)
{
	struct net_device *sta;
	struct GLUE_INFO *prGlueInfo;
	struct PARAM_CUSTOM_MONITOR_SET_STRUCT rMon;
	uint32_t u4BufLen = 0;
	uint32_t rStatus;
	int ch;
	enum ENUM_BAND eBand;

	ch = ieee80211_frequency_to_channel(mhz);
	if (!ch)
		return -EINVAL;

	sta = dev_get_by_name(&init_net, WONDER_STA_IF);
	if (!sta)
		return -ENODEV;

	prGlueInfo = *((struct GLUE_INFO **) netdev_priv(sta));
	if (!prGlueInfo || !prGlueInfo->prAdapter || !prGlueInfo->u4ReadyFlag) {
		dev_put(sta);
		return -ENODEV;
	}

	eBand = (ch <= 14) ? BAND_2G4 : BAND_5G;
	if (!rlmDomainIsLegalChannel(prGlueInfo->prAdapter, eBand, ch)) {
		dev_put(sta);
		return -EINVAL;
	}

	if (!prGlueInfo->fgIsEnableMon) {
		prGlueInfo->fgIsEnableMon = TRUE;
		schedule_work(&prGlueInfo->monWork);
	}

	/* Same as priv_driver_set_monitor(): 20 MHz, no secondary, band left 0 */
	kalMemZero(&rMon, sizeof(rMon));
	rMon.ucEnable = 1;
	rMon.ucPriChannel = ch;
	rMon.ucSco = CHNL_EXT_SCN;
	rMon.ucChannelWidth = CW_20_40MHZ;

	rStatus = kalIoctl(prGlueInfo, wlanoidSetMonitor, &rMon, sizeof(rMon),
			   FALSE, FALSE, TRUE, &u4BufLen);
	dev_put(sta);

	if (rStatus != WLAN_STATUS_SUCCESS)
		return -EIO;

	wonder_cur_freq = mhz;
	return 0;
}

/* ---------------------------------------------------------------- */
/* wonder0 netdev                                                   */
/* ---------------------------------------------------------------- */
static netdev_tx_t wonder_xmit(struct sk_buff *skb, struct net_device *dev)
{
	static const u8 bare_rt[8] = { 0, 0, 8, 0, 0, 0, 0, 0 };
	struct net_device *mon;
	struct sk_buff *n;
	u16 rt_len, fc;
	unsigned int flen;
	u8 *frame;

	if (skb_linearize(skb) || skb->len < 8 || skb->data[0] != 0)
		goto drop;

	rt_len = skb->data[2] | (skb->data[3] << 8);
	if (rt_len < 8 || skb->len <= rt_len ||
	    skb->len - rt_len < 24)
		goto drop;

	frame = skb->data + rt_len;
	flen = skb->len - rt_len;
	fc = frame[0] | (frame[1] << 8);
	if (fc & 0x000c)	/* management frames only for now */
		goto drop;

	rcu_read_lock();
	mon = rcu_dereference(wonder_mon_dev);
	if (!mon || !(mon->flags & IFF_UP)) {
		rcu_read_unlock();
		goto drop;
	}
	n = netdev_alloc_skb(mon, sizeof(bare_rt) + flen);
	if (!n) {
		rcu_read_unlock();
		goto drop;
	}
	skb_put_data(n, bare_rt, sizeof(bare_rt));
	skb_put_data(n, frame, flen);
	n->dev = mon;
	n->protocol = htons(ETH_P_802_2);
	skb_reset_mac_header(n);
	dev_queue_xmit(n);
	rcu_read_unlock();

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += flen;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;

drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int wonder_open(struct net_device *dev)
{
	netif_carrier_on(dev);
	netif_tx_start_all_queues(dev);
	return 0;
}

static int wonder_stop(struct net_device *dev)
{
	netif_tx_stop_all_queues(dev);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops wonder_netdev_ops = {
	.ndo_open = wonder_open,
	.ndo_stop = wonder_stop,
	.ndo_start_xmit = wonder_xmit,
	.ndo_set_mac_address = eth_mac_addr,
};

static void wonder_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->type = ARPHRD_IEEE80211_RADIOTAP;
	dev->netdev_ops = &wonder_netdev_ops;
	dev->needs_free_netdev = true;
	dev->flags |= IFF_NOARP;
	dev->flags &= ~IFF_MULTICAST;
	dev->tx_queue_len = 1000;
}

/* ---------------------------------------------------------------- */
/* cfg80211 ops                                                     */
/* ---------------------------------------------------------------- */
static struct wireless_dev *wonder_add_intf(struct wiphy *wiphy,
					    const char *name,
					    unsigned char name_assign_type,
					    enum nl80211_iftype type,
					    struct vif_params *params)
{
	struct net_device *dev;
	struct wonder_vif *vif;
	int ret;

	if (type != NL80211_IFTYPE_MONITOR)
		return ERR_PTR(-EOPNOTSUPP);
	if (rtnl_dereference(wonder_vif_dev))
		return ERR_PTR(-EBUSY);

	dev = alloc_netdev(sizeof(*vif), name, name_assign_type, wonder_setup);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	vif = netdev_priv(dev);
	vif->wdev.wiphy = wiphy;
	vif->wdev.iftype = NL80211_IFTYPE_MONITOR;
	vif->wdev.netdev = dev;
	dev->ieee80211_ptr = &vif->wdev;
	SET_NETDEV_DEV(dev, wiphy_dev(wiphy));
	memcpy(dev->dev_addr, wonder_mac, ETH_ALEN);

	ret = register_netdevice(dev);
	if (ret) {
		free_netdev(dev);
		return ERR_PTR(ret);
	}
	rcu_assign_pointer(wonder_vif_dev, dev);
	return &vif->wdev;
}

static int wonder_del_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct net_device *dev = wdev->netdev;

	if (rtnl_dereference(wonder_vif_dev) == dev)
		RCU_INIT_POINTER(wonder_vif_dev, NULL);
	synchronize_net();
	unregister_netdevice(dev);
	return 0;
}

static int wonder_change_intf(struct wiphy *wiphy, struct net_device *dev,
			      enum nl80211_iftype type,
			      struct vif_params *params)
{
	return type == NL80211_IFTYPE_MONITOR ? 0 : -EOPNOTSUPP;
}

static int wonder_set_mon_chan(struct wiphy *wiphy,
			       struct cfg80211_chan_def *chandef)
{
	return wonder_set_channel_mhz(chandef->chan->center_freq);
}

static const struct cfg80211_ops wonder_cfg_ops = {
	.add_virtual_intf = wonder_add_intf,
	.del_virtual_intf = wonder_del_intf,
	.change_virtual_intf = wonder_change_intf,
	.set_monitor_channel = wonder_set_mon_chan,
};

/* ---------------------------------------------------------------- */
/* vendor commands, vendor_id 0x1A11 (layout of attributes UNVERIFIED) */
/* ---------------------------------------------------------------- */
static void wonder_dump_attrs(const char *what, const void *data, int len)
{
	const struct nlattr *a;
	int rem;

	pr_info("wonder: %s len=%d\n", what, len);
	if (!data || len <= 0)
		return;
	print_hex_dump(KERN_INFO, "wonder: payload: ", DUMP_PREFIX_OFFSET, 16,
		       1, data, len, false);
	nla_for_each_attr(a, data, len, rem)
		pr_info("wonder:   attr type=%u len=%u\n", nla_type(a),
			nla_len(a));
}

static int wonder_vendor_set_frequency(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const void *data, int len)
{
	const struct nlattr *a;
	int rem;

	wonder_dump_attrs("set_frequency", data, len);
	/* Heuristic: first 4-byte attr that looks like a Wi-Fi MHz value. */
	if (data && len > 0) {
		nla_for_each_attr(a, data, len, rem) {
			u32 v;

			if (nla_len(a) != sizeof(u32))
				continue;
			v = nla_get_u32(a);
			if ((v >= 2412 && v <= 2484) ||
			    (v >= 5180 && v <= 5885)) {
				wonder_set_channel_mhz(v);
				break;
			}
		}
	}
	return 0;
}

static int wonder_vendor_noop(struct wiphy *wiphy, struct wireless_dev *wdev,
			      const void *data, int len)
{
	wonder_dump_attrs("vendor noop (filter/rate/reg)", data, len);
	return 0;
}

static int wonder_vendor_get_mac(struct wiphy *wiphy, struct wireless_dev *wdev,
				 const void *data, int len)
{
	struct sk_buff *reply;

	wonder_dump_attrs("get_if_mac_addr", data, len);
	reply = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, 16);
	if (!reply)
		return -ENOMEM;
	if (nla_put(reply, wonder_mac_attr, ETH_ALEN, wonder_mac)) {
		kfree_skb(reply);
		return -ENOBUFS;
	}
	return cfg80211_vendor_cmd_reply(reply);
}

#define WONDER_VCMD(_sub, _fn) {					\
	.info = { .vendor_id = WONDER_VENDOR_ID, .subcmd = (_sub) },	\
	.flags = 0,							\
	.doit = (_fn),							\
}

static const struct wiphy_vendor_command wonder_vendor_cmds[] = {
	WONDER_VCMD(1, wonder_vendor_set_frequency),
	WONDER_VCMD(2, wonder_vendor_noop),	/* set_filter */
	WONDER_VCMD(3, wonder_vendor_noop),	/* set_fixed_tx_rate */
	WONDER_VCMD(4, wonder_vendor_noop),	/* set_reg */
	WONDER_VCMD(5, wonder_vendor_get_mac),	/* get_if_mac_addr */
};

/* ---------------------------------------------------------------- */
/* bands                                                            */
/* ---------------------------------------------------------------- */
#define WCH(_b, _c, _f) { .band = (_b), .center_freq = (_f), \
			  .hw_value = (_c), .max_power = 20 }

static struct ieee80211_channel wonder_chans_2g[] = {
	WCH(NL80211_BAND_2GHZ, 1, 2412),
	WCH(NL80211_BAND_2GHZ, 6, 2437),
	WCH(NL80211_BAND_2GHZ, 11, 2462),
};

static struct ieee80211_channel wonder_chans_5g[] = {
	WCH(NL80211_BAND_5GHZ, 36, 5180),
	WCH(NL80211_BAND_5GHZ, 40, 5200),
	WCH(NL80211_BAND_5GHZ, 44, 5220),
	WCH(NL80211_BAND_5GHZ, 48, 5240),
	WCH(NL80211_BAND_5GHZ, 149, 5745),
	WCH(NL80211_BAND_5GHZ, 153, 5765),
	WCH(NL80211_BAND_5GHZ, 157, 5785),
	WCH(NL80211_BAND_5GHZ, 161, 5805),
	WCH(NL80211_BAND_5GHZ, 165, 5825),
};

static struct ieee80211_rate wonder_rates[] = {
	{ .bitrate = 60, .hw_value = 0 },
	{ .bitrate = 120, .hw_value = 1 },
	{ .bitrate = 240, .hw_value = 2 },
};

static struct ieee80211_supported_band wonder_band_2g = {
	.band = NL80211_BAND_2GHZ,
	.channels = wonder_chans_2g,
	.n_channels = ARRAY_SIZE(wonder_chans_2g),
	.bitrates = wonder_rates,
	.n_bitrates = ARRAY_SIZE(wonder_rates),
};

static struct ieee80211_supported_band wonder_band_5g = {
	.band = NL80211_BAND_5GHZ,
	.channels = wonder_chans_5g,
	.n_channels = ARRAY_SIZE(wonder_chans_5g),
	.bitrates = wonder_rates,
	.n_bitrates = ARRAY_SIZE(wonder_rates),
};

/* ---------------------------------------------------------------- */
/* radiotap0 attach: RX handler + bring-up                          */
/* ---------------------------------------------------------------- */
static rx_handler_result_t wonder_rx(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device *wd;

	rcu_read_lock();
	wd = rcu_dereference(wonder_vif_dev);
	if (!wd || !(wd->flags & IFF_UP)) {
		rcu_read_unlock();
		return RX_HANDLER_PASS;
	}
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb) {
		rcu_read_unlock();
		return RX_HANDLER_CONSUMED;
	}
	wd->stats.rx_packets++;
	wd->stats.rx_bytes += skb->len;
	skb->dev = wd;
	skb->protocol = htons(ETH_P_802_2);
	skb->pkt_type = PACKET_OTHERHOST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(skb);
	*pskb = skb;
	rcu_read_unlock();
	return RX_HANDLER_ANOTHER;
}

static void wonder_up_workfn(struct work_struct *w)
{
	struct net_device *mon;

	rtnl_lock();
	mon = __dev_get_by_name(&init_net, "radiotap0");
	if (mon && !(mon->flags & IFF_UP))
		dev_open(mon);
	rtnl_unlock();
}

static int wonder_netdev_event(struct notifier_block *nb, unsigned long ev,
			       void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (strncmp(dev->name, "radiotap", 8) ||
	    dev->type != ARPHRD_IEEE80211_RADIOTAP)
		return NOTIFY_DONE;

	switch (ev) {
	case NETDEV_REGISTER:
		if (!netdev_rx_handler_register(dev, wonder_rx, NULL)) {
			rcu_assign_pointer(wonder_mon_dev, dev);
			schedule_work(&wonder_up_work);
		}
		break;
	case NETDEV_UNREGISTER:
		if (rtnl_dereference(wonder_mon_dev) == dev) {
			RCU_INIT_POINTER(wonder_mon_dev, NULL);
			netdev_rx_handler_unregister(dev);
		}
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block wonder_nb = {
	.notifier_call = wonder_netdev_event,
};

/* ---------------------------------------------------------------- */
static int __init wonder_init(void)
{
	int ret;

	eth_random_addr(wonder_mac);
	INIT_WORK(&wonder_up_work, wonder_up_workfn);

	wonder_wiphy = wiphy_new_nm(&wonder_cfg_ops, 0, WONDER_PHY_NAME);
	if (!wonder_wiphy)
		return -ENOMEM;

	wonder_wiphy->interface_modes = BIT(NL80211_IFTYPE_MONITOR);
	wonder_wiphy->bands[NL80211_BAND_2GHZ] = &wonder_band_2g;
	wonder_wiphy->bands[NL80211_BAND_5GHZ] = &wonder_band_5g;
	wonder_wiphy->vendor_commands = wonder_vendor_cmds;
	wonder_wiphy->n_vendor_commands = ARRAY_SIZE(wonder_vendor_cmds);
	memcpy(wonder_wiphy->perm_addr, wonder_mac, ETH_ALEN);

	ret = wiphy_register(wonder_wiphy);
	if (ret) {
		wiphy_free(wonder_wiphy);
		wonder_wiphy = NULL;
		return ret;
	}

	register_netdevice_notifier(&wonder_nb);
	pr_info("wonder: phy '%s' registered (vendor id 0x%x)\n",
		WONDER_PHY_NAME, WONDER_VENDOR_ID);
	return 0;
}
late_initcall(wonder_init);

#endif /* CFG_SUPPORT_SNIFFER */
