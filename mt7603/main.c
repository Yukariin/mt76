/* SPDX-License-Identifier: ISC */
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/module.h>
#include "mt7603.h"
#include "eeprom.h"

static int
mt7603_start(struct ieee80211_hw *hw)
{
	struct mt7603_dev *dev = hw->priv;

	mt7603_mac_start(dev);
	dev->survey_time = ktime_get_boottime();
	set_bit(MT76_STATE_RUNNING, &dev->mt76.state);
	mt7603_mac_work(&dev->mac_work.work);

	return 0;
}

static void
mt7603_stop(struct ieee80211_hw *hw)
{
	struct mt7603_dev *dev = hw->priv;

	clear_bit(MT76_STATE_RUNNING, &dev->mt76.state);
	cancel_delayed_work_sync(&dev->mac_work);
	mt7603_mac_stop(dev);
}

static void
mt7603_txq_init(struct mt7603_dev *dev, struct ieee80211_txq *txq)
{
	struct mt76_txq *mtxq;

	if (!txq)
		return;

	mtxq = (struct mt76_txq *) txq->drv_priv;
	if (txq->sta) {
		struct mt7603_sta *sta = (struct mt7603_sta *)txq->sta->drv_priv;
		mtxq->wcid = &sta->wcid;
	} else {
		struct mt7603_vif *mvif = (struct mt7603_vif *)txq->vif->drv_priv;
		mtxq->wcid = &mvif->sta.wcid;
	}

	mt76_txq_init(&dev->mt76, txq);
}

static int
mt7603_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt7603_vif *mvif = (struct mt7603_vif *)vif->drv_priv;
	struct mt7603_dev *dev = hw->priv;
	u8 bc_addr[ETH_ALEN];
	int idx;
	int ret = 0;

	mutex_lock(&dev->mt76.mutex);

	mvif->idx = ffs(~dev->vif_mask) - 1;
	if (mvif->idx >= MT7603_MAX_INTERFACES) {
		ret = -ENOSPC;
		goto out;
	}

	mt76_wr(dev, MT_MAC_ADDR0(mvif->idx),
		get_unaligned_le32(vif->addr));
	mt76_wr(dev, MT_MAC_ADDR1(mvif->idx),
		(get_unaligned_le16(vif->addr + 4) |
		 MT_MAC_ADDR1_VALID));

	idx = MT7603_WTBL_RESERVED - 1 - mvif->idx;
	dev->vif_mask |= BIT(mvif->idx);
	mvif->sta.wcid.idx = idx;
	mvif->sta.wcid.hw_key_idx = -1;

	eth_broadcast_addr(bc_addr);
	mt7603_wtbl_init(dev, idx, mvif->idx, bc_addr);

	rcu_assign_pointer(dev->mt76.wcid[idx], &mvif->sta.wcid);
	mt7603_txq_init(dev, vif->txq);

out:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static void
mt7603_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt7603_vif *mvif = (struct mt7603_vif *)vif->drv_priv;
	struct mt7603_dev *dev = hw->priv;
	int idx = mvif->sta.wcid.idx;

	mt7603_beacon_set_timer(dev, mvif->idx, 0);

	rcu_assign_pointer(dev->mt76.wcid[idx], NULL);
	mt76_txq_remove(&dev->mt76, vif->txq);

	mutex_lock(&dev->mt76.mutex);
	dev->vif_mask &= ~BIT(mvif->idx);
	mutex_unlock(&dev->mt76.mutex);
}

static void
mt7603_init_edcca(struct mt7603_dev *dev)
{
	/* Set lower signal level to -65dBm */
	mt76_rmw_field(dev, MT_RXTD(8), MT_RXTD_8_LOWER_SIGNAL, 0x23);

	/* clear previous energy detect monitor results */
	mt76_rr(dev, MT_MIB_STAT_ED);

	if (dev->ed_monitor)
		mt76_set(dev, MT_MIB_CTL, MT_MIB_CTL_ED_TIME);
	else
		mt76_clear(dev, MT_MIB_CTL, MT_MIB_CTL_ED_TIME);

	dev->ed_strict_mode = 0xff;
	dev->ed_strong_signal = 0;
	dev->ed_time = ktime_get_boottime();

	mt7603_edcca_set_strict(dev, false);
}

static int
mt7603_set_channel(struct mt7603_dev *dev, struct cfg80211_chan_def *def)
{
	u8 *rssi_data = (u8 *) dev->mt76.eeprom.data;
	int idx, ret;
	u8 bw = MT_BW_20;
	bool failed = false;

	cancel_delayed_work_sync(&dev->mac_work);

	mutex_lock(&dev->mt76.mutex);
	set_bit(MT76_RESET, &dev->mt76.state);

	mt76_set_channel(&dev->mt76);
	mt7603_mac_stop(dev);

	if (def->width == NL80211_CHAN_WIDTH_40)
		bw = MT_BW_40;

	dev->mt76.chandef = *def;
	mt76_rmw_field(dev, MT_AGG_BWCR, MT_AGG_BWCR_BW, bw);
	ret = mt7603_mcu_set_channel(dev);
	if (ret) {
		failed = true;
		goto out;
	}

	if (def->chan->band == NL80211_BAND_5GHZ) {
		idx = 1;
		rssi_data += MT_EE_RSSI_OFFSET_5G;
	} else {
		idx = 0;
		rssi_data += MT_EE_RSSI_OFFSET_2G;
	}

	memcpy(dev->rssi_offset, rssi_data, sizeof(dev->rssi_offset));

	idx |= (def->chan -
		mt76_hw(dev)->wiphy->bands[def->chan->band]->channels) << 1;
	mt76_wr(dev, MT_WF_RMAC_CH_FREQ, idx);
	mt7603_mac_start(dev);
	mt7603_mac_set_timing(dev);

	clear_bit(MT76_RESET, &dev->mt76.state);

	mt76_txq_schedule_all(&dev->mt76);

	ieee80211_queue_delayed_work(mt76_hw(dev), &dev->mac_work,
				     MT7603_WATCHDOG_TIME);

	/* reset channel stats */
	mt76_clear(dev, MT_MIB_CTL, MT_MIB_CTL_READ_CLR_DIS);
	mt76_set(dev, MT_MIB_CTL,
		 MT_MIB_CTL_CCA_NAV_TX | MT_MIB_CTL_PSCCA_TIME);
	mt76_rr(dev, MT_MIB_STAT_PSCCA);
	mt7603_cca_stats_reset(dev);

	dev->survey_time = ktime_get_boottime();

	mt7603_init_edcca(dev);

out:
	mutex_unlock(&dev->mt76.mutex);

	if (failed)
		mt7603_mac_work(&dev->mac_work.work);

	return ret;
}

static int
mt7603_config(struct ieee80211_hw *hw, u32 changed)
{
	struct mt7603_dev *dev = hw->priv;
	int ret = 0;

	if (changed & (IEEE80211_CONF_CHANGE_CHANNEL |
		       IEEE80211_CONF_CHANGE_POWER))
		ret = mt7603_set_channel(dev, &hw->conf.chandef);

	if (changed & IEEE80211_CONF_CHANGE_MONITOR) {
		mutex_lock(&dev->mt76.mutex);

		if (!(hw->conf.flags & IEEE80211_CONF_MONITOR))
			dev->rxfilter |= MT_WF_RFCR_DROP_OTHER_UC;
		else
			dev->rxfilter &= ~MT_WF_RFCR_DROP_OTHER_UC;

		mt76_wr(dev, MT_WF_RFCR, dev->rxfilter);

		mutex_unlock(&dev->mt76.mutex);
	}

	return ret;
}

static void
mt7603_configure_filter(struct ieee80211_hw *hw, unsigned int changed_flags,
			unsigned int *total_flags, u64 multicast)
{
	struct mt7603_dev *dev = hw->priv;
	u32 flags = 0;

#define MT76_FILTER(_flag, _hw) do { \
		flags |= *total_flags & FIF_##_flag;			\
		dev->rxfilter &= ~(_hw);				\
		dev->rxfilter |= !(flags & FIF_##_flag) * (_hw);	\
	} while (0)

	dev->rxfilter &= ~(MT_WF_RFCR_DROP_OTHER_BSS |
			   MT_WF_RFCR_DROP_OTHER_BEACON |
			   MT_WF_RFCR_DROP_FRAME_REPORT |
			   MT_WF_RFCR_DROP_PROBEREQ |
			   MT_WF_RFCR_DROP_MCAST_FILTERED |
			   MT_WF_RFCR_DROP_MCAST |
			   MT_WF_RFCR_DROP_BCAST |
			   MT_WF_RFCR_DROP_DUPLICATE |
			   MT_WF_RFCR_DROP_A2_BSSID |
			   MT_WF_RFCR_DROP_UNWANTED_CTL |
			   MT_WF_RFCR_DROP_STBC_MULTI);

	MT76_FILTER(OTHER_BSS, MT_WF_RFCR_DROP_OTHER_TIM |
			       MT_WF_RFCR_DROP_A3_MAC |
			       MT_WF_RFCR_DROP_A3_BSSID);

	MT76_FILTER(FCSFAIL, MT_WF_RFCR_DROP_FCSFAIL);

	MT76_FILTER(CONTROL, MT_WF_RFCR_DROP_CTS |
			     MT_WF_RFCR_DROP_RTS |
			     MT_WF_RFCR_DROP_CTL_RSV |
			     MT_WF_RFCR_DROP_NDPA);

	*total_flags = flags;
	mt76_wr(dev, MT_WF_RFCR, dev->rxfilter);
}

static void
mt7603_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_bss_conf *info, u32 changed)
{
	struct mt7603_dev *dev = hw->priv;
	struct mt7603_vif *mvif = (struct mt7603_vif *)vif->drv_priv;

	mutex_lock(&dev->mt76.mutex);

	if (changed & (BSS_CHANGED_ASSOC | BSS_CHANGED_BSSID)) {
		if (info->assoc || info->ibss_joined) {
			mt76_wr(dev, MT_BSSID0(mvif->idx),
				get_unaligned_le32(info->bssid));
			mt76_wr(dev, MT_BSSID1(mvif->idx),
				(get_unaligned_le16(info->bssid + 4) |
				 MT_BSSID1_VALID));
		} else {
			mt76_wr(dev, MT_BSSID0(mvif->idx), 0);
			mt76_wr(dev, MT_BSSID1(mvif->idx), 0);
		}
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		dev->slottime = info->use_short_slot ? 9 : 20;
		mt7603_mac_set_timing(dev);
	}

	if (changed & (BSS_CHANGED_BEACON_ENABLED | BSS_CHANGED_BEACON_INT)) {
		int beacon_int = !!info->enable_beacon * info->beacon_int;

		tasklet_disable(&dev->pre_tbtt_tasklet);
		mt7603_beacon_set_timer(dev, mvif->idx, beacon_int);
		tasklet_enable(&dev->pre_tbtt_tasklet);
	}

	mutex_unlock(&dev->mt76.mutex);
}

int
mt7603_sta_add(struct mt76_dev *mdev, struct ieee80211_vif *vif,
	       struct ieee80211_sta *sta)
{
	struct mt7603_dev *dev = container_of(mdev, struct mt7603_dev, mt76);
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	struct mt7603_vif *mvif = (struct mt7603_vif *)vif->drv_priv;
	int idx;
	int ret = 0;

	idx = mt76_wcid_alloc(dev->mt76.wcid_mask, MT7603_WTBL_STA - 1);
	if (idx < 0)
		return -ENOSPC;

	__skb_queue_head_init(&msta->psq);
	msta->ps = ~0;
	msta->wcid.sta = 1;
	msta->wcid.idx = idx;
	mt7603_wtbl_init(dev, idx, mvif->idx, sta->addr);
	mt7603_wtbl_set_ps(dev, msta, false);
	mt7603_wtbl_update_cap(dev, sta);

	if (vif->type == NL80211_IFTYPE_AP)
		set_bit(MT_WCID_FLAG_CHECK_PS, &msta->wcid.flags);

	return ret;
}

void
mt7603_sta_remove(struct mt76_dev *mdev, struct ieee80211_vif *vif,
		  struct ieee80211_sta *sta)
{
	struct mt7603_dev *dev = container_of(mdev, struct mt7603_dev, mt76);
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	struct mt76_wcid *wcid = (struct mt76_wcid *) sta->drv_priv;

	spin_lock_bh(&dev->ps_lock);
	__skb_queue_purge(&msta->psq);
	mt7603_filter_tx(dev, wcid->idx, true);
	spin_unlock_bh(&dev->ps_lock);

	mt7603_wtbl_clear(dev, wcid->idx);
}

static void
mt7603_ps_tx_list(struct mt7603_dev *dev, struct sk_buff_head *list)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(list)) != NULL)
		mt7603_tx_queue_mcu(dev, skb_get_queue_mapping(skb), skb);
}

void
mt7603_sta_ps(struct mt76_dev *mdev, struct ieee80211_sta *sta, bool ps)
{
	struct mt7603_dev *dev = container_of(mdev, struct mt7603_dev, mt76);
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	struct sk_buff_head list;

	mt76_stop_tx_queues(&dev->mt76, sta, false);
	mt7603_wtbl_set_ps(dev, msta, ps);
	if (ps)
		return;

	__skb_queue_head_init(&list);

	spin_lock_bh(&dev->ps_lock);
	skb_queue_splice_tail_init(&msta->psq, &list);
	spin_unlock_bh(&dev->ps_lock);

	mt7603_ps_tx_list(dev, &list);
}

static void
mt7603_release_buffered_frames(struct ieee80211_hw *hw,
			       struct ieee80211_sta *sta,
			       u16 tids, int nframes,
			       enum ieee80211_frame_release_type reason,
			       bool more_data)
{
	struct mt7603_dev *dev = hw->priv;
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	struct sk_buff_head list;
	struct sk_buff *skb, *tmp;

	__skb_queue_head_init(&list);

	spin_lock_bh(&dev->ps_lock);
	skb_queue_walk_safe(&msta->psq, skb, tmp) {
		if (!nframes)
			break;

		if (!(tids & BIT(skb->priority)))
			continue;

		skb_set_queue_mapping(skb, MT_TXQ_PSD);
		__skb_unlink(skb, &msta->psq);
		__skb_queue_tail(&list, skb);
	}
	spin_unlock_bh(&dev->ps_lock);

	mt7603_ps_tx_list(dev, &list);

	if (nframes)
		mt76_release_buffered_frames(hw, sta, tids, nframes, reason,
					     more_data);
}

static int
mt7603_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
	       struct ieee80211_vif *vif, struct ieee80211_sta *sta,
	       struct ieee80211_key_conf *key)
{
	struct mt7603_dev *dev = hw->priv;
	struct mt7603_vif *mvif = (struct mt7603_vif *)vif->drv_priv;
	struct mt7603_sta *msta = sta ? (struct mt7603_sta *)sta->drv_priv :
				  &mvif->sta;
	struct mt76_wcid *wcid = &msta->wcid;
	int idx = key->keyidx;

	/* fall back to sw encryption for unsupported ciphers */
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_TKIP:
	case WLAN_CIPHER_SUITE_CCMP:
		break;
	default:
		return -EOPNOTSUPP;
	}

	/*
	 * The hardware does not support per-STA RX GTK, fall back
	 * to software mode for these.
	 */
	if ((vif->type == NL80211_IFTYPE_ADHOC ||
	     vif->type == NL80211_IFTYPE_MESH_POINT) &&
	    (key->cipher == WLAN_CIPHER_SUITE_TKIP ||
	     key->cipher == WLAN_CIPHER_SUITE_CCMP) &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return -EOPNOTSUPP;

	if (cmd == SET_KEY) {
		key->hw_key_idx = wcid->idx;
		wcid->hw_key_idx = idx;
	} else {
		if (idx == wcid->hw_key_idx)
			wcid->hw_key_idx = -1;

		key = NULL;
	}
	mt76_wcid_key_setup(&dev->mt76, wcid, key);

	return mt7603_wtbl_set_key(dev, wcid->idx, key);
}

static int
mt7603_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 queue,
	       const struct ieee80211_tx_queue_params *params)
{
	struct mt7603_dev *dev = hw->priv;
	u16 cw_min = (1 << 5) - 1;
	u16 cw_max = (1 << 10) - 1;
	u32 val;

	queue = dev->mt76.q_tx[queue].hw_idx;

	if (params->cw_min)
		cw_min = params->cw_min;
	if (params->cw_max)
		cw_max = params->cw_max;

	mutex_lock(&dev->mt76.mutex);
	mt7603_mac_stop(dev);

	val = mt76_rr(dev, MT_WMM_TXOP(queue));
	val &= ~(MT_WMM_TXOP_MASK << MT_WMM_TXOP_SHIFT(queue));
	val |= params->txop << MT_WMM_TXOP_SHIFT(queue);
	mt76_wr(dev, MT_WMM_TXOP(queue), val);

	val = mt76_rr(dev, MT_WMM_AIFSN);
	val &= ~(MT_WMM_AIFSN_MASK << MT_WMM_AIFSN_SHIFT(queue));
	val |= params->aifs << MT_WMM_AIFSN_SHIFT(queue);
	mt76_wr(dev, MT_WMM_AIFSN, val);

	val = mt76_rr(dev, MT_WMM_CWMIN);
	val &= ~(MT_WMM_CWMIN_MASK << MT_WMM_CWMIN_SHIFT(queue));
	val |= cw_min << MT_WMM_CWMIN_SHIFT(queue);
	mt76_wr(dev, MT_WMM_CWMIN, val);

	val = mt76_rr(dev, MT_WMM_CWMAX(queue));
	val &= ~(MT_WMM_CWMAX_MASK << MT_WMM_CWMAX_SHIFT(queue));
	val |= cw_max << MT_WMM_CWMAX_SHIFT(queue);
	mt76_wr(dev, MT_WMM_CWMAX(queue), val);

	mt7603_mac_start(dev);
	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static void
mt7603_sw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       const u8 *mac)
{
	struct mt7603_dev *dev = hw->priv;

	set_bit(MT76_SCANNING, &dev->mt76.state);
	mt7603_beacon_set_timer(dev, -1, 0);
}

static void
mt7603_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt7603_dev *dev = hw->priv;

	clear_bit(MT76_SCANNING, &dev->mt76.state);
	mt7603_beacon_set_timer(dev, -1, dev->beacon_int);
}

static void
mt7603_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	     u32 queues, bool drop)
{
}

static int
mt7603_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    struct ieee80211_ampdu_params *params)
{
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct mt7603_dev *dev = hw->priv;
	struct ieee80211_sta *sta = params->sta;
	struct ieee80211_txq *txq = sta->txq[params->tid];
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	u16 tid = params->tid;
	u16 *ssn = &params->ssn;
	u8 ba_size = params->buf_size;
	struct mt76_txq *mtxq;

	if (!txq)
		return -EINVAL;

	mtxq = (struct mt76_txq *)txq->drv_priv;

	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		mt76_rx_aggr_start(&dev->mt76, &msta->wcid, tid, *ssn,
				   params->buf_size);
		mt7603_mac_rx_ba_reset(dev, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_RX_STOP:
		mt76_rx_aggr_stop(&dev->mt76, &msta->wcid, tid);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		mtxq->aggr = true;
		mtxq->send_bar = false;
		mt7603_mac_tx_ba_reset(dev, msta->wcid.idx, tid, *ssn, ba_size);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		mtxq->aggr = false;
		ieee80211_send_bar(vif, sta->addr, tid, mtxq->agg_ssn);
		mt7603_mac_tx_ba_reset(dev, msta->wcid.idx, tid, *ssn, -1);
		break;
	case IEEE80211_AMPDU_TX_START:
		mtxq->agg_ssn = *ssn << 4;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		mtxq->aggr = false;
		mt7603_mac_tx_ba_reset(dev, msta->wcid.idx, tid, *ssn, -1);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	}

	return 0;
}

static void
mt7603_sta_rate_tbl_update(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct mt7603_dev *dev = hw->priv;
	struct mt7603_sta *msta = (struct mt7603_sta *)sta->drv_priv;
	struct ieee80211_sta_rates *sta_rates = rcu_dereference(sta->rates);
	int i;

	spin_lock_bh(&dev->mt76.lock);
	for (i = 0; i < ARRAY_SIZE(msta->rates); i++) {
		msta->rates[i].idx = sta_rates->rate[i].idx;
		msta->rates[i].count = sta_rates->rate[i].count;
		msta->rates[i].flags = sta_rates->rate[i].flags;

		if (msta->rates[i].idx < 0 || !msta->rates[i].count)
			break;
	}
	msta->n_rates = i;
	mt7603_wtbl_set_rates(dev, msta, NULL, msta->rates);
	msta->rate_probe = false;
	spin_unlock_bh(&dev->mt76.lock);
}

static void mt7603_set_coverage_class(struct ieee80211_hw *hw,
				      s16 coverage_class)
{
	struct mt7603_dev *dev = hw->priv;

	dev->coverage_class = coverage_class;
	mt7603_mac_set_timing(dev);
}

static void mt7603_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_vif *vif = info->control.vif;
	struct mt7603_dev *dev = hw->priv;
	struct mt76_wcid *wcid = &dev->global_sta.wcid;

	if (control->sta) {
		struct mt7603_sta *msta;

		msta = (struct mt7603_sta *)control->sta->drv_priv;
		wcid = &msta->wcid;
		/* sw encrypted frames */
		if (!info->control.hw_key && wcid->hw_key_idx != 0xff &&
		    ieee80211_has_protected(hdr->frame_control))
			control->sta = NULL;
	}

	if (vif && !control->sta) {
		struct mt7603_vif *mvif;

		mvif = (struct mt7603_vif *)vif->drv_priv;
		wcid = &mvif->sta.wcid;
	}

	mt76_tx(&dev->mt76, control->sta, wcid, skb);
}

static int
mt7603_set_tim(struct ieee80211_hw *hw, struct ieee80211_sta *sta, bool set)
{
	return 0;
}

const struct ieee80211_ops mt7603_ops = {
	.tx = mt7603_tx,
	.start = mt7603_start,
	.stop = mt7603_stop,
	.add_interface = mt7603_add_interface,
	.remove_interface = mt7603_remove_interface,
	.config = mt7603_config,
	.configure_filter = mt7603_configure_filter,
	.bss_info_changed = mt7603_bss_info_changed,
	.sta_state = mt76_sta_state,
	.set_key = mt7603_set_key,
	.conf_tx = mt7603_conf_tx,
	.sw_scan_start = mt7603_sw_scan,
	.sw_scan_complete = mt7603_sw_scan_complete,
	.flush = mt7603_flush,
	.ampdu_action = mt7603_ampdu_action,
	.get_txpower = mt76_get_txpower,
	.wake_tx_queue = mt76_wake_tx_queue,
	.sta_rate_tbl_update = mt7603_sta_rate_tbl_update,
	.release_buffered_frames = mt7603_release_buffered_frames,
	.set_coverage_class = mt7603_set_coverage_class,
	.set_tim = mt7603_set_tim,
	.get_survey = mt76_get_survey,
};

MODULE_LICENSE("Dual BSD/GPL");

static int __init mt7603_init(void)
{
	int ret;

	ret = platform_driver_register(&mt76_wmac_driver);
	if (ret)
		return ret;

#ifdef CONFIG_PCI
	ret = pci_register_driver(&mt7603_pci_driver);
	if (ret)
		platform_driver_unregister(&mt76_wmac_driver);
#endif
	return ret;
}

static void __exit mt7603_exit(void)
{
#ifdef CONFIG_PCI
	pci_unregister_driver(&mt7603_pci_driver);
#endif
	platform_driver_unregister(&mt76_wmac_driver);
}

module_init(mt7603_init);
module_exit(mt7603_exit);
