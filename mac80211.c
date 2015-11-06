/*
 * Copyright (C) 2006-2015, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available by writing to the Free Software Foundation, Inc.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

/* Description:  This file implements mac80211 related functions. */

#include <linux/etherdevice.h>

#include "sysadpt.h"
#include "dev.h"
#include "fwcmd.h"
#include "tx.h"

#define MWL_DRV_NAME        KBUILD_MODNAME

#define MAX_AMPDU_ATTEMPTS  5

static const struct ieee80211_rate mwl_rates_24[] = {
	{ .bitrate = 10, .hw_value = 2, },
	{ .bitrate = 20, .hw_value = 4, },
	{ .bitrate = 55, .hw_value = 11, },
	{ .bitrate = 110, .hw_value = 22, },
	{ .bitrate = 220, .hw_value = 44, },
	{ .bitrate = 60, .hw_value = 12, },
	{ .bitrate = 90, .hw_value = 18, },
	{ .bitrate = 120, .hw_value = 24, },
	{ .bitrate = 180, .hw_value = 36, },
	{ .bitrate = 240, .hw_value = 48, },
	{ .bitrate = 360, .hw_value = 72, },
	{ .bitrate = 480, .hw_value = 96, },
	{ .bitrate = 540, .hw_value = 108, },
};

static const struct ieee80211_rate mwl_rates_50[] = {
	{ .bitrate = 60, .hw_value = 12, },
	{ .bitrate = 90, .hw_value = 18, },
	{ .bitrate = 120, .hw_value = 24, },
	{ .bitrate = 180, .hw_value = 36, },
	{ .bitrate = 240, .hw_value = 48, },
	{ .bitrate = 360, .hw_value = 72, },
	{ .bitrate = 480, .hw_value = 96, },
	{ .bitrate = 540, .hw_value = 108, },
};

static void mwl_mac80211_tx(struct ieee80211_hw *hw,
			    struct ieee80211_tx_control *control,
			    struct sk_buff *skb)
{
	struct mwl_priv *priv = hw->priv;

	if (!priv->radio_on) {
		wiphy_warn(hw->wiphy,
			   "dropped TX frame since radio is disabled\n");
		dev_kfree_skb_any(skb);
		return;
	}

	mwl_tx_xmit(hw, control, skb);
}

static int mwl_mac80211_start(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;
	int rc;

	/* Enable TX reclaim and RX tasklets. */
	tasklet_enable(&priv->tx_task);
	tasklet_enable(&priv->rx_task);
	tasklet_enable(&priv->qe_task);

	/* Enable interrupts */
	mwl_fwcmd_int_enable(hw);

	rc = mwl_fwcmd_radio_enable(hw);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_rate_adapt_mode(hw, 0);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_wmm_mode(hw, true);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_ht_guard_interval(hw, GUARD_INTERVAL_AUTO);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_dwds_stamode(hw, true);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_fw_flush_timer(hw, SYSADPT_AMSDU_FLUSH_TIME);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_optimization_level(hw, 1);
	if (rc)
		goto fwcmd_fail;

	ieee80211_wake_queues(hw);
	return 0;

fwcmd_fail:
	mwl_fwcmd_int_disable(hw);
	tasklet_disable(&priv->tx_task);
	tasklet_disable(&priv->rx_task);
	tasklet_disable(&priv->qe_task);

	return rc;
}

static void mwl_mac80211_stop(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;

	mwl_fwcmd_radio_disable(hw);

	ieee80211_stop_queues(hw);

	/* Disable interrupts */
	mwl_fwcmd_int_disable(hw);

	/* Disable TX reclaim and RX tasklets. */
	tasklet_disable(&priv->tx_task);
	tasklet_disable(&priv->rx_task);
	tasklet_disable(&priv->qe_task);

	/* Return all skbs to mac80211 */
	mwl_tx_done((unsigned long)hw);
}

static int mwl_mac80211_add_interface(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif;
	u32 macids_supported;
	int macid;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		macids_supported = priv->ap_macids_supported;
		break;
	case NL80211_IFTYPE_STATION:
		macids_supported = priv->sta_macids_supported;
		break;
	default:
		return -EINVAL;
	}

	macid = ffs(macids_supported & ~priv->macids_used);

	if (!macid) {
		wiphy_warn(hw->wiphy, "no macid can be allocated\n");
		return -EBUSY;
	}
	macid--;

	/* Setup driver private area. */
	mwl_vif = mwl_dev_get_vif(vif);
	memset(mwl_vif, 0, sizeof(*mwl_vif));
	mwl_vif->macid = macid;
	mwl_vif->seqno = 0;
	mwl_vif->is_hw_crypto_enabled = false;
	mwl_vif->beacon_info.valid = false;
	mwl_vif->iv16 = 1;
	mwl_vif->iv32 = 0;
	mwl_vif->keyidx = 0;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
		ether_addr_copy(mwl_vif->bssid, vif->addr);
		mwl_fwcmd_set_new_stn_add_self(hw, vif);
		break;
	case NL80211_IFTYPE_MESH_POINT:
		ether_addr_copy(mwl_vif->bssid, vif->addr);
		mwl_fwcmd_set_new_stn_add_self(hw, vif);
		break;
	case NL80211_IFTYPE_STATION:
		ether_addr_copy(mwl_vif->sta_mac, vif->addr);
		mwl_fwcmd_bss_start(hw, vif, true);
		mwl_fwcmd_set_infra_mode(hw, vif);
		mwl_fwcmd_set_mac_addr_client(hw, vif, vif->addr);
		break;
	default:
		return -EINVAL;
	}

	priv->macids_used |= 1 << mwl_vif->macid;
	spin_lock_bh(&priv->vif_lock);
	list_add_tail(&mwl_vif->list, &priv->vif_list);
	spin_unlock_bh(&priv->vif_lock);

	return 0;
}

static void mwl_mac80211_remove_vif(struct mwl_priv *priv,
				    struct ieee80211_vif *vif)
{
	struct mwl_vif *mwl_vif = mwl_dev_get_vif(vif);

	if (!priv->macids_used)
		return;

	mwl_tx_del_pkts_via_vif(priv->hw, vif);

	priv->macids_used &= ~(1 << mwl_vif->macid);
	spin_lock_bh(&priv->vif_lock);
	list_del(&mwl_vif->list);
	spin_unlock_bh(&priv->vif_lock);
}

static void mwl_mac80211_remove_interface(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif)
{
	struct mwl_priv *priv = hw->priv;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		mwl_fwcmd_set_new_stn_del(hw, vif, vif->addr);
		break;
	case NL80211_IFTYPE_STATION:
		mwl_fwcmd_remove_mac_addr(hw, vif, vif->addr);
		break;
	default:
		break;
	}

	mwl_mac80211_remove_vif(priv, vif);
}

static int mwl_mac80211_config(struct ieee80211_hw *hw,
			       u32 changed)
{
	struct ieee80211_conf *conf = &hw->conf;
	int rc;

	wiphy_debug(hw->wiphy, "change: 0x%x\n", changed);

	if (conf->flags & IEEE80211_CONF_IDLE)
		rc = mwl_fwcmd_radio_disable(hw);
	else
		rc = mwl_fwcmd_radio_enable(hw);

	if (rc)
		goto out;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		int rate = 0;

		if (conf->chandef.chan->band == IEEE80211_BAND_2GHZ) {
			mwl_fwcmd_set_apmode(hw, AP_MODE_2_4GHZ_11AC_MIXED);
			mwl_fwcmd_set_linkadapt_cs_mode(hw,
							LINK_CS_STATE_CONSERV);
			rate = mwl_rates_24[0].hw_value;
		} else if (conf->chandef.chan->band == IEEE80211_BAND_5GHZ) {
			mwl_fwcmd_set_apmode(hw, AP_MODE_11AC);
			mwl_fwcmd_set_linkadapt_cs_mode(hw,
							LINK_CS_STATE_AUTO);
			rate = mwl_rates_50[0].hw_value;
		}

		rc = mwl_fwcmd_set_rf_channel(hw, conf);
		if (rc)
			goto out;
		rc = mwl_fwcmd_use_fixed_rate(hw, rate, rate);
		if (rc)
			goto out;
		rc = mwl_fwcmd_max_tx_power(hw, conf, 0);
		if (rc)
			goto out;
		rc = mwl_fwcmd_tx_power(hw, conf, 0);
		if (rc)
			goto out;
		rc = mwl_fwcmd_set_cdd(hw);
	}

out:

	return rc;
}

static void mwl_mac80211_bss_info_changed_sta(struct ieee80211_hw *hw,
					      struct ieee80211_vif *vif,
					      struct ieee80211_bss_conf *info,
					      u32 changed)
{
	if (changed & BSS_CHANGED_ERP_PREAMBLE)
		mwl_fwcmd_set_radio_preamble(hw,
					     vif->bss_conf.use_short_preamble);

	if ((changed & BSS_CHANGED_ASSOC) && vif->bss_conf.assoc)
		mwl_fwcmd_set_aid(hw, vif, (u8 *)vif->bss_conf.bssid,
				  vif->bss_conf.aid);
}

static void mwl_mac80211_bss_info_changed_ap(struct ieee80211_hw *hw,
					     struct ieee80211_vif *vif,
					     struct ieee80211_bss_conf *info,
					     u32 changed)
{
	if (changed & BSS_CHANGED_ERP_PREAMBLE)
		mwl_fwcmd_set_radio_preamble(hw,
					     vif->bss_conf.use_short_preamble);

	if (changed & BSS_CHANGED_BASIC_RATES) {
		int idx;
		int rate;

		/* Use lowest supported basic rate for multicasts
		 * and management frames (such as probe responses --
		 * beacons will always go out at 1 Mb/s).
		 */
		idx = ffs(vif->bss_conf.basic_rates);
		if (idx)
			idx--;

		if (hw->conf.chandef.chan->band == IEEE80211_BAND_2GHZ)
			rate = mwl_rates_24[idx].hw_value;
		else
			rate = mwl_rates_50[idx].hw_value;

		mwl_fwcmd_use_fixed_rate(hw, rate, rate);
	}

	if (changed & (BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON)) {
		struct sk_buff *skb;

		skb = ieee80211_beacon_get(hw, vif);

		if (skb) {
			mwl_fwcmd_set_beacon(hw, vif, skb->data, skb->len);
			dev_kfree_skb_any(skb);
		}

		if ((info->ssid[0] != '\0') &&
		    (info->ssid_len != 0) &&
		    (!info->hidden_ssid))
			mwl_fwcmd_broadcast_ssid_enable(hw, vif, true);
		else
			mwl_fwcmd_broadcast_ssid_enable(hw, vif, false);
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED)
		mwl_fwcmd_bss_start(hw, vif, info->enable_beacon);
}

static void mwl_mac80211_bss_info_changed(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  struct ieee80211_bss_conf *info,
					  u32 changed)
{
	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		mwl_mac80211_bss_info_changed_ap(hw, vif, info, changed);
		break;
	case NL80211_IFTYPE_STATION:
		mwl_mac80211_bss_info_changed_sta(hw, vif, info, changed);
		break;
	default:
		break;
	}
}

static void mwl_mac80211_configure_filter(struct ieee80211_hw *hw,
					  unsigned int changed_flags,
					  unsigned int *total_flags,
					  u64 multicast)
{
	/* AP firmware doesn't allow fine-grained control over
	 * the receive filter.
	 */
	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC;
}

static int mwl_mac80211_set_key(struct ieee80211_hw *hw,
				enum set_key_cmd cmd_param,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				struct ieee80211_key_conf *key)
{
	struct mwl_vif *mwl_vif;
	int rc = 0;
	u8 encr_type;
	u8 *addr;

	mwl_vif = mwl_dev_get_vif(vif);

	if (!sta) {
		addr = vif->addr;
	} else {
		addr = sta->addr;
		if (vif->type == NL80211_IFTYPE_STATION)
			ether_addr_copy(mwl_vif->bssid, addr);
	}

	if (cmd_param == SET_KEY) {
		rc = mwl_fwcmd_encryption_set_key(hw, vif, addr, key);

		if (rc)
			goto out;

		if ((key->cipher == WLAN_CIPHER_SUITE_WEP40) ||
		    (key->cipher == WLAN_CIPHER_SUITE_WEP104)) {
			encr_type = ENCR_TYPE_WEP;
		} else if (key->cipher == WLAN_CIPHER_SUITE_CCMP) {
			encr_type = ENCR_TYPE_AES;
			if ((key->flags & IEEE80211_KEY_FLAG_PAIRWISE) == 0) {
				if (vif->type != NL80211_IFTYPE_STATION)
					mwl_vif->keyidx = key->keyidx;
			}
		} else if (key->cipher == WLAN_CIPHER_SUITE_TKIP) {
			encr_type = ENCR_TYPE_TKIP;
		} else {
			encr_type = ENCR_TYPE_DISABLE;
		}

		rc = mwl_fwcmd_update_encryption_enable(hw, vif, addr,
							encr_type);
		if (rc)
			goto out;

		mwl_vif->is_hw_crypto_enabled = true;
	} else {
		rc = mwl_fwcmd_encryption_remove_key(hw, vif, addr, key);
		if (rc)
			goto out;
	}

out:

	return rc;
}

static int mwl_mac80211_set_rts_threshold(struct ieee80211_hw *hw,
					  u32 value)
{
	return mwl_fwcmd_set_rts_threshold(hw, value);
}

static int mwl_mac80211_sta_add(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif;
	struct mwl_sta *sta_info;
	struct ieee80211_key_conf *key;
	int rc;
	int i;

	mwl_vif = mwl_dev_get_vif(vif);
	sta_info = mwl_dev_get_sta(sta);

	memset(sta_info, 0, sizeof(*sta_info));
	if (vif->type == NL80211_IFTYPE_MESH_POINT)
		sta_info->is_mesh_node = true;
	if (sta->ht_cap.ht_supported) {
		sta_info->is_ampdu_allowed = true;
		sta_info->is_amsdu_allowed = false;
		if (sta->ht_cap.cap & IEEE80211_HT_CAP_MAX_AMSDU)
			sta_info->amsdu_ctrl.cap = MWL_AMSDU_SIZE_8K;
		else
			sta_info->amsdu_ctrl.cap = MWL_AMSDU_SIZE_4K;
	}
	sta_info->iv16 = 1;
	sta_info->iv32 = 0;
	spin_lock_init(&sta_info->amsdu_lock);
	spin_lock_bh(&priv->sta_lock);
	list_add_tail(&sta_info->list, &priv->sta_list);
	spin_unlock_bh(&priv->sta_lock);

	if (vif->type == NL80211_IFTYPE_STATION)
		mwl_fwcmd_set_new_stn_del(hw, vif, sta->addr);

	rc = mwl_fwcmd_set_new_stn_add(hw, vif, sta);

	for (i = 0; i < NUM_WEP_KEYS; i++) {
		key = (struct ieee80211_key_conf *)mwl_vif->wep_key_conf[i].key;

		if (mwl_vif->wep_key_conf[i].enabled)
			mwl_mac80211_set_key(hw, SET_KEY, vif, sta, key);
	}

	return rc;
}

static int mwl_mac80211_sta_remove(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta)
{
	struct mwl_priv *priv = hw->priv;
	int rc;
	struct mwl_sta *sta_info = mwl_dev_get_sta(sta);

	mwl_tx_del_sta_amsdu_pkts(sta);

	spin_lock_bh(&priv->stream_lock);
	mwl_fwcmd_del_sta_streams(hw, sta);
	spin_unlock_bh(&priv->stream_lock);

	mwl_tx_del_pkts_via_sta(hw, sta);

	rc = mwl_fwcmd_set_new_stn_del(hw, vif, sta->addr);

	spin_lock_bh(&priv->sta_lock);
	list_del(&sta_info->list);
	spin_unlock_bh(&priv->sta_lock);

	return rc;
}

static int mwl_mac80211_conf_tx(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				u16 queue,
				const struct ieee80211_tx_queue_params *params)
{
	struct mwl_priv *priv = hw->priv;
	int rc = 0;

	if (WARN_ON(queue > SYSADPT_TX_WMM_QUEUES - 1))
		return -EINVAL;

	memcpy(&priv->wmm_params[queue], params, sizeof(*params));

	if (!priv->wmm_enabled) {
		rc = mwl_fwcmd_set_wmm_mode(hw, true);
		priv->wmm_enabled = true;
	}

	if (!rc) {
		int q = SYSADPT_TX_WMM_QUEUES - 1 - queue;

		rc = mwl_fwcmd_set_edca_params(hw, q,
					       params->cw_min, params->cw_max,
					       params->aifs, params->txop);
	}

	return rc;
}

static int mwl_mac80211_get_stats(struct ieee80211_hw *hw,
				  struct ieee80211_low_level_stats *stats)
{
	return mwl_fwcmd_get_stat(hw, stats);
}

static int mwl_mac80211_get_survey(struct ieee80211_hw *hw,
				   int idx,
				   struct survey_info *survey)
{
	struct mwl_priv *priv = hw->priv;
	struct ieee80211_conf *conf = &hw->conf;

	if (idx != 0)
		return -ENOENT;

	survey->channel = conf->chandef.chan;
	survey->filled = SURVEY_INFO_NOISE_DBM;
	survey->noise = priv->noise;

	return 0;
}

static int mwl_mac80211_ampdu_action(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     enum ieee80211_ampdu_mlme_action action,
				     struct ieee80211_sta *sta,
				     u16 tid, u16 *ssn, u8 buf_size)
{
	int rc = 0;
	struct mwl_priv *priv = hw->priv;
	struct mwl_ampdu_stream *stream;
	u8 *addr = sta->addr, idx;
	struct mwl_sta *sta_info;

	sta_info = mwl_dev_get_sta(sta);

	spin_lock_bh(&priv->stream_lock);

	stream = mwl_fwcmd_lookup_stream(hw, addr, tid);

	switch (action) {
	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
		break;
	case IEEE80211_AMPDU_TX_START:
		if (!sta_info->is_ampdu_allowed) {
			wiphy_warn(hw->wiphy, "ampdu not allowed\n");
			rc = -EPERM;
			break;
		}

		if (!stream) {
			stream = mwl_fwcmd_add_stream(hw, sta, tid);
			if (!stream) {
				wiphy_warn(hw->wiphy, "no stream found\n");
				rc = -EPERM;
				break;
			}
		}

		spin_unlock_bh(&priv->stream_lock);
		rc = mwl_fwcmd_check_ba(hw, stream, vif);
		spin_lock_bh(&priv->stream_lock);
		if (rc) {
			mwl_fwcmd_remove_stream(hw, stream);
			wiphy_err(hw->wiphy,
				  "ampdu start error code: %d\n", rc);
			rc = -EPERM;
			break;
		}
		stream->state = AMPDU_STREAM_IN_PROGRESS;
		*ssn = 0;
		ieee80211_start_tx_ba_cb_irqsafe(vif, addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		if (stream) {
			if (stream->state == AMPDU_STREAM_ACTIVE) {
				mwl_tx_del_ampdu_pkts(hw, sta, tid);
				idx = stream->idx;
				spin_unlock_bh(&priv->stream_lock);
				mwl_fwcmd_destroy_ba(hw, idx);
				spin_lock_bh(&priv->stream_lock);
			}

			mwl_fwcmd_remove_stream(hw, stream);
			ieee80211_stop_tx_ba_cb_irqsafe(vif, addr, tid);
		} else
			rc = -EPERM;
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		if (stream) {
			if (WARN_ON(stream->state !=
				    AMPDU_STREAM_IN_PROGRESS)) {
				rc = -EPERM;
				break;
			}
			spin_unlock_bh(&priv->stream_lock);
			rc = mwl_fwcmd_create_ba(hw, stream, buf_size, vif);
			spin_lock_bh(&priv->stream_lock);

			if (!rc) {
				stream->state = AMPDU_STREAM_ACTIVE;
			} else {
				idx = stream->idx;
				spin_unlock_bh(&priv->stream_lock);
				mwl_fwcmd_destroy_ba(hw, idx);
				spin_lock_bh(&priv->stream_lock);
				mwl_fwcmd_remove_stream(hw, stream);
				wiphy_err(hw->wiphy,
					  "ampdu operation error code: %d\n",
					  rc);
			}
		} else
			rc = -EPERM;
		break;
	default:
		rc = -ENOTSUPP;
		break;
	}

	spin_unlock_bh(&priv->stream_lock);

	return rc;
}

const struct ieee80211_ops mwl_mac80211_ops = {
	.tx                 = mwl_mac80211_tx,
	.start              = mwl_mac80211_start,
	.stop               = mwl_mac80211_stop,
	.add_interface      = mwl_mac80211_add_interface,
	.remove_interface   = mwl_mac80211_remove_interface,
	.config             = mwl_mac80211_config,
	.bss_info_changed   = mwl_mac80211_bss_info_changed,
	.configure_filter   = mwl_mac80211_configure_filter,
	.set_key            = mwl_mac80211_set_key,
	.set_rts_threshold  = mwl_mac80211_set_rts_threshold,
	.sta_add            = mwl_mac80211_sta_add,
	.sta_remove         = mwl_mac80211_sta_remove,
	.conf_tx            = mwl_mac80211_conf_tx,
	.get_stats          = mwl_mac80211_get_stats,
	.get_survey         = mwl_mac80211_get_survey,
	.ampdu_action       = mwl_mac80211_ampdu_action,
};
