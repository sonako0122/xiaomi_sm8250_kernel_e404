/*
 * Copyright (c) 2016-2021 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <qdf_types.h>
#include <qdf_lock.h>
#include <qdf_net_types.h>
#include <qdf_lro.h>
#include <qdf_module.h>
#include <hal_hw_headers.h>
#include <hal_api.h>
#include <hif.h>
#include <htt.h>
#include <wdi_event.h>
#include <queue.h>
#include "dp_types.h"
#include "dp_internal.h"
#include "dp_tx.h"
#include "dp_tx_desc.h"
#include "dp_rx.h"
#include "dp_rx_mon.h"
#ifdef DP_RATETABLE_SUPPORT
#include "dp_ratetable.h"
#endif
#include <cdp_txrx_handle.h>
#include <wlan_cfg.h>
#include "cdp_txrx_cmn_struct.h"
#include "cdp_txrx_stats_struct.h"
#include "cdp_txrx_cmn_reg.h"
#include <qdf_util.h>
#include "dp_peer.h"
#include "dp_rx_mon.h"
#include "htt_stats.h"
#include "dp_htt.h"
#ifdef WLAN_SUPPORT_RX_FISA
#include <dp_fisa_rx.h>
#endif
#include "htt_ppdu_stats.h"
#include "qdf_mem.h"   /* qdf_mem_malloc,free */
#include "cfg_ucfg_api.h"
#include "dp_mon_filter.h"
#ifdef QCA_LL_TX_FLOW_CONTROL_V2
#include "cdp_txrx_flow_ctrl_v2.h"
#else
static inline void
cdp_dump_flow_pool_info(struct cdp_soc_t *soc)
{
	return;
}
#endif
#include "dp_ipa.h"
#include "dp_cal_client_api.h"
#ifdef FEATURE_WDS
#include "dp_txrx_wds.h"
#endif
#ifdef ATH_SUPPORT_IQUE
#include "dp_txrx_me.h"
#endif
#if defined(DP_CON_MON)
#ifndef REMOVE_PKT_LOG
#include <pktlog_ac_api.h>
#include <pktlog_ac.h>
#endif
#endif

#ifdef WLAN_FEATURE_STATS_EXT
#define INIT_RX_HW_STATS_LOCK(_soc) \
	qdf_spinlock_create(&(_soc)->rx_hw_stats_lock)
#define DEINIT_RX_HW_STATS_LOCK(_soc) \
	qdf_spinlock_destroy(&(_soc)->rx_hw_stats_lock)
#else
#define INIT_RX_HW_STATS_LOCK(_soc)  /* no op */
#define DEINIT_RX_HW_STATS_LOCK(_soc) /* no op */
#endif

#ifdef DP_PEER_EXTENDED_API
#define SET_PEER_REF_CNT_ONE(_peer) \
	qdf_atomic_set(&(_peer)->ref_cnt, 1)
#else
#define SET_PEER_REF_CNT_ONE(_peer)
#endif

/*
 * The max size of cdp_peer_stats_param_t is limited to 16 bytes.
 * If the buffer size is exceeding this size limit,
 * dp_txrx_get_peer_stats is to be used instead.
 */
QDF_COMPILE_TIME_ASSERT(cdp_peer_stats_param_t_max_size,
			(sizeof(cdp_peer_stats_param_t) <= 16));

#ifdef WLAN_FEATURE_DP_EVENT_HISTORY
/*
 * If WLAN_CFG_INT_NUM_CONTEXTS is changed, HIF_NUM_INT_CONTEXTS
 * also should be updated accordingly
 */
QDF_COMPILE_TIME_ASSERT(num_intr_grps,
			HIF_NUM_INT_CONTEXTS == WLAN_CFG_INT_NUM_CONTEXTS);

/*
 * HIF_EVENT_HIST_MAX should always be power of 2
 */
QDF_COMPILE_TIME_ASSERT(hif_event_history_size,
			(HIF_EVENT_HIST_MAX & (HIF_EVENT_HIST_MAX - 1)) == 0);
#endif /* WLAN_FEATURE_DP_EVENT_HISTORY */

/*
 * If WLAN_CFG_INT_NUM_CONTEXTS is changed,
 * WLAN_CFG_INT_NUM_CONTEXTS_MAX should also be updated
 */
QDF_COMPILE_TIME_ASSERT(wlan_cfg_num_int_ctxs,
			WLAN_CFG_INT_NUM_CONTEXTS_MAX >=
			WLAN_CFG_INT_NUM_CONTEXTS);

#ifdef WLAN_RX_PKT_CAPTURE_ENH
#include "dp_rx_mon_feature.h"
#else
/*
 * dp_config_enh_rx_capture()- API to enable/disable enhanced rx capture
 * @pdev_handle: DP_PDEV handle
 * @val: user provided value
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_config_enh_rx_capture(struct dp_pdev *pdev_handle, uint8_t val)
{
	return QDF_STATUS_E_INVAL;
}
#endif /* WLAN_RX_PKT_CAPTURE_ENH */

#ifdef WLAN_TX_PKT_CAPTURE_ENH
#include "dp_tx_capture.h"
#else
/*
 * dp_config_enh_tx_capture()- API to enable/disable enhanced tx capture
 * @pdev_handle: DP_PDEV handle
 * @val: user provided value
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_config_enh_tx_capture(struct dp_pdev *pdev_handle, uint8_t val)
{
	return QDF_STATUS_E_INVAL;
}
#endif

void *dp_soc_init(struct dp_soc *soc, HTC_HANDLE htc_handle,
		  struct hif_opaque_softc *hif_handle);
static void dp_pdev_detach(struct cdp_pdev *txrx_pdev, int force);
static struct dp_soc *
dp_soc_attach(struct cdp_ctrl_objmgr_psoc *ctrl_psoc, HTC_HANDLE htc_handle,
	      qdf_device_t qdf_osdev,
	      struct ol_if_ops *ol_ops, uint16_t device_id);
static void dp_pktlogmod_exit(struct dp_pdev *handle);
static inline QDF_STATUS dp_peer_create_wifi3(struct cdp_soc_t *soc_hdl,
					      uint8_t vdev_id,
					      uint8_t *peer_mac_addr);
static QDF_STATUS dp_peer_delete_wifi3(struct cdp_soc_t *soc_hdl,
				       uint8_t vdev_id,
				       uint8_t *peer_mac, uint32_t bitmap);
static void dp_vdev_flush_peers(struct cdp_vdev *vdev_handle,
				bool unmap_only);
#ifdef ENABLE_VERBOSE_DEBUG
bool is_dp_verbose_debug_enabled;
#endif

#if defined(WLAN_CFR_ENABLE) && defined(WLAN_ENH_CFR_ENABLE)
static void dp_cfr_filter(struct cdp_soc_t *soc_hdl,
			  uint8_t pdev_id,
			  bool enable,
			  struct cdp_monitor_filter *filter_val);
static bool dp_get_cfr_rcc(struct cdp_soc_t *soc_hdl, uint8_t pdev_id);
static void dp_set_cfr_rcc(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			   bool enable);
static inline void
dp_get_cfr_dbg_stats(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
		     struct cdp_cfr_rcc_stats *cfr_rcc_stats);
static inline void
dp_clear_cfr_dbg_stats(struct cdp_soc_t *soc_hdl, uint8_t pdev_id);
static inline void
dp_enable_mon_reap_timer(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			 bool enable);
#endif
static inline bool
dp_is_enable_reap_timer_non_pkt(struct dp_pdev *pdev);
static uint8_t dp_soc_ring_if_nss_offloaded(struct dp_soc *soc,
					    enum hal_ring_type ring_type,
					    int ring_num);
#define DP_INTR_POLL_TIMER_MS	5
/* Generic AST entry aging timer value */
#define DP_AST_AGING_TIMER_DEFAULT_MS	1000
#define DP_MCS_LENGTH (6*MAX_MCS)

#define DP_CURR_FW_STATS_AVAIL 19
#define DP_HTT_DBG_EXT_STATS_MAX 256
#define DP_MAX_SLEEP_TIME 100
#ifndef QCA_WIFI_3_0_EMU
#define SUSPEND_DRAIN_WAIT 500
#else
#define SUSPEND_DRAIN_WAIT 3000
#endif

#ifdef IPA_OFFLOAD
/* Exclude IPA rings from the interrupt context */
#define TX_RING_MASK_VAL	0xb
#define RX_RING_MASK_VAL	0x7
#else
#define TX_RING_MASK_VAL	0xF
#define RX_RING_MASK_VAL	0xF
#endif

#define STR_MAXLEN	64

#define RNG_ERR		"SRNG setup failed for"

/* Threshold for peer's cached buf queue beyond which frames are dropped */
#define DP_RX_CACHED_BUFQ_THRESH 64

/* Budget to reap monitor status ring */
#define DP_MON_REAP_BUDGET 1024

/**
 * default_dscp_tid_map - Default DSCP-TID mapping
 *
 * DSCP        TID
 * 000000      0
 * 001000      1
 * 010000      2
 * 011000      3
 * 100000      4
 * 101000      5
 * 110000      6
 * 111000      7
 */
static uint8_t default_dscp_tid_map[DSCP_TID_MAP_MAX] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7,
};

/**
 * default_pcp_tid_map - Default PCP-TID mapping
 *
 * PCP     TID
 * 000      0
 * 001      1
 * 010      2
 * 011      3
 * 100      4
 * 101      5
 * 110      6
 * 111      7
 */
static uint8_t default_pcp_tid_map[PCP_TID_MAP_MAX] = {
	0, 1, 2, 3, 4, 5, 6, 7,
};

/**
 * @brief Cpu to tx ring map
 */
uint8_t
dp_cpu_ring_map[DP_NSS_CPU_RING_MAP_MAX][WLAN_CFG_INT_NUM_CONTEXTS_MAX] = {
	{0x0, 0x1, 0x2, 0x0, 0x0, 0x1, 0x2, 0x0, 0x0, 0x1, 0x2},
	{0x1, 0x2, 0x1, 0x2, 0x1, 0x2, 0x1, 0x2, 0x1, 0x2, 0x1},
	{0x0, 0x2, 0x0, 0x2, 0x0, 0x2, 0x0, 0x2, 0x0, 0x2, 0x0},
	{0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2},
	{0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3},
#ifdef WLAN_TX_PKT_CAPTURE_ENH
	{0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1}
#endif
};

/**
 * @brief Select the type of statistics
 */
enum dp_stats_type {
	STATS_FW = 0,
	STATS_HOST = 1,
	STATS_TYPE_MAX = 2,
};

/**
 * @brief General Firmware statistics options
 *
 */
enum dp_fw_stats {
	TXRX_FW_STATS_INVALID	= -1,
};

/**
 * dp_stats_mapping_table - Firmware and Host statistics
 * currently supported
 */
const int dp_stats_mapping_table[][STATS_TYPE_MAX] = {
	{HTT_DBG_EXT_STATS_RESET, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TX, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_RX, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TX_HWQ, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TX_SCHED, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_ERROR, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TQM, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_TQM_CMDQ, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_TX_DE_INFO, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TX_RATE, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_RX_RATE, TXRX_HOST_STATS_INVALID},
	{TXRX_FW_STATS_INVALID, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_TX_SELFGEN_INFO, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_TX_MU_HWQ, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_RING_IF_INFO, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_SRNG_INFO, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_SFM_INFO, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_PDEV_TX_MU, TXRX_HOST_STATS_INVALID},
	{HTT_DBG_EXT_STATS_ACTIVE_PEERS_LIST, TXRX_HOST_STATS_INVALID},
	/* Last ENUM for HTT FW STATS */
	{DP_HTT_DBG_EXT_STATS_MAX, TXRX_HOST_STATS_INVALID},
	{TXRX_FW_STATS_INVALID, TXRX_CLEAR_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_RX_RATE_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_TX_RATE_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_TX_HOST_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_RX_HOST_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_AST_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_SRNG_PTR_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_RX_MON_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_REO_QUEUE_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_SOC_CFG_PARAMS},
	{TXRX_FW_STATS_INVALID, TXRX_PDEV_CFG_PARAMS},
	{TXRX_FW_STATS_INVALID, TXRX_SOC_INTERRUPT_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_SOC_FSE_STATS},
	{TXRX_FW_STATS_INVALID, TXRX_HAL_REG_WRITE_STATS},
};

/* MCL specific functions */
#if defined(DP_CON_MON)
/**
 * dp_soc_get_mon_mask_for_interrupt_mode() - get mon mode mask for intr mode
 * @soc: pointer to dp_soc handle
 * @intr_ctx_num: interrupt context number for which mon mask is needed
 *
 * For MCL, monitor mode rings are being processed in timer contexts (polled).
 * This function is returning 0, since in interrupt mode(softirq based RX),
 * we donot want to process monitor mode rings in a softirq.
 *
 * So, in case packet log is enabled for SAP/STA/P2P modes,
 * regular interrupt processing will not process monitor mode rings. It would be
 * done in a separate timer context.
 *
 * Return: 0
 */
static inline
uint32_t dp_soc_get_mon_mask_for_interrupt_mode(struct dp_soc *soc, int intr_ctx_num)
{
	return 0;
}

/*
 * dp_service_mon_rings()- service monitor rings
 * @soc: soc dp handle
 * @quota: number of ring entry that can be serviced
 *
 * Return: None
 *
 */
static void dp_service_mon_rings(struct  dp_soc *soc, uint32_t quota)
{
	int ring = 0, work_done;
	struct dp_pdev *pdev = NULL;

	for (ring = 0 ; ring < MAX_NUM_LMAC_HW; ring++) {
		pdev = dp_get_pdev_for_lmac_id(soc, ring);
		if (!pdev)
			continue;
		work_done = dp_mon_process(soc, ring, quota);

		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  FL("Reaped %d descs from Monitor rings"),
			  work_done);
	}
}

/*
 * dp_mon_reap_timer_handler()- timer to reap monitor rings
 * reqd as we are not getting ppdu end interrupts
 * @arg: SoC Handle
 *
 * Return:
 *
 */
static void dp_mon_reap_timer_handler(void *arg)
{
	struct dp_soc *soc = (struct dp_soc *)arg;

	dp_service_mon_rings(soc, QCA_NAPI_BUDGET);

	qdf_timer_mod(&soc->mon_reap_timer, DP_INTR_POLL_TIMER_MS);
}

#ifndef REMOVE_PKT_LOG
/**
 * dp_pkt_log_init() - API to initialize packet log
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @scn: HIF context
 *
 * Return: none
 */
void dp_pkt_log_init(struct cdp_soc_t *soc_hdl, uint8_t pdev_id, void *scn)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *handle =
		dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (!handle) {
		dp_err("pdev handle is NULL");
		return;
	}

	if (handle->pkt_log_init) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "%s: Packet log not initialized", __func__);
		return;
	}

	pktlog_sethandle(&handle->pl_dev, scn);
	pktlog_set_pdev_id(handle->pl_dev, pdev_id);
	pktlog_set_callback_regtype(PKTLOG_DEFAULT_CALLBACK_REGISTRATION);

	if (pktlogmod_init(scn)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "%s: pktlogmod_init failed", __func__);
		handle->pkt_log_init = false;
	} else {
		handle->pkt_log_init = true;
	}
}

/**
 * dp_pkt_log_con_service() - connect packet log service
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @scn: device context
 *
 * Return: none
 */
static void dp_pkt_log_con_service(struct cdp_soc_t *soc_hdl,
				   uint8_t pdev_id, void *scn)
{
	dp_pkt_log_init(soc_hdl, pdev_id, scn);
	pktlog_htc_attach();
}

/**
 * dp_pktlogmod_exit() - API to cleanup pktlog info
 * @pdev: Pdev handle
 *
 * Return: none
 */
static void dp_pktlogmod_exit(struct dp_pdev *pdev)
{
	struct dp_soc *soc = pdev->soc;
	struct hif_opaque_softc *scn = soc->hif_handle;

	if (!scn) {
		dp_err("Invalid hif(scn) handle");
		return;
	}

	/* stop mon_reap_timer if it has been started */
	if (pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED &&
	    soc->reap_timer_init && (!dp_is_enable_reap_timer_non_pkt(pdev)))
		qdf_timer_sync_cancel(&soc->mon_reap_timer);

	pktlogmod_exit(scn);
	pdev->pkt_log_init = false;
}
#else
static void dp_pkt_log_con_service(struct cdp_soc_t *soc_hdl,
				   uint8_t pdev_id, void *scn)
{
}

static void dp_pktlogmod_exit(struct dp_pdev *handle) { }
#endif
/**
 * dp_get_num_rx_contexts() - get number of RX contexts
 * @soc_hdl: cdp opaque soc handle
 *
 * Return: number of RX contexts
 */
static int dp_get_num_rx_contexts(struct cdp_soc_t *soc_hdl)
{
	int i;
	int num_rx_contexts = 0;

	struct dp_soc *soc = (struct dp_soc *)soc_hdl;

	for (i = 0; i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++)
		if (wlan_cfg_get_rx_ring_mask(soc->wlan_cfg_ctx, i))
			num_rx_contexts++;

	return num_rx_contexts;
}

#else
static void dp_pktlogmod_exit(struct dp_pdev *handle) { }

/**
 * dp_soc_get_mon_mask_for_interrupt_mode() - get mon mode mask for intr mode
 * @soc: pointer to dp_soc handle
 * @intr_ctx_num: interrupt context number for which mon mask is needed
 *
 * Return: mon mask value
 */
static inline
uint32_t dp_soc_get_mon_mask_for_interrupt_mode(struct dp_soc *soc, int intr_ctx_num)
{
	return wlan_cfg_get_rx_mon_ring_mask(soc->wlan_cfg_ctx, intr_ctx_num);
}

/*
 * dp_service_lmac_rings()- timer to reap lmac rings
 * @arg: SoC Handle
 *
 * Return:
 *
 */
static void dp_service_lmac_rings(void *arg)
{
	struct dp_soc *soc = (struct dp_soc *)arg;
	int ring = 0, i;
	struct dp_pdev *pdev = NULL;
	union dp_rx_desc_list_elem_t *desc_list = NULL;
	union dp_rx_desc_list_elem_t *tail = NULL;

	/* Process LMAC interrupts */
	for  (ring = 0 ; ring < MAX_NUM_LMAC_HW; ring++) {
		int mac_for_pdev = ring;
		struct dp_srng *rx_refill_buf_ring;

		pdev = dp_get_pdev_for_lmac_id(soc, mac_for_pdev);
		if (!pdev)
			continue;

		rx_refill_buf_ring = &soc->rx_refill_buf_ring[mac_for_pdev];

		dp_mon_process(soc, mac_for_pdev,
			       QCA_NAPI_BUDGET);

		for (i = 0;
		     i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++)
			dp_rxdma_err_process(&soc->intr_ctx[i], soc,
					     mac_for_pdev,
					     QCA_NAPI_BUDGET);

		if (!dp_soc_ring_if_nss_offloaded(soc, RXDMA_BUF,
						  mac_for_pdev))
			dp_rx_buffers_replenish(soc, mac_for_pdev,
						rx_refill_buf_ring,
						&soc->rx_desc_buf[mac_for_pdev],
						0, &desc_list, &tail);
	}

	qdf_timer_mod(&soc->lmac_reap_timer, DP_INTR_POLL_TIMER_MS);
}

#endif

static int dp_peer_add_ast_wifi3(struct cdp_soc_t *soc_hdl,
				 uint8_t vdev_id,
				 uint8_t *peer_mac,
				 uint8_t *mac_addr,
				 enum cdp_txrx_ast_entry_type type,
				 uint32_t flags)
{
	int ret = -1;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc_hdl,
						       peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Peer is NULL!\n", __func__);
		goto fail;
	}

	ret = dp_peer_add_ast((struct dp_soc *)soc_hdl,
			      peer,
			      mac_addr,
			      type,
			      flags);
fail:
	if (peer)
		dp_peer_unref_delete(peer);

	return ret;
}

static int dp_peer_update_ast_wifi3(struct cdp_soc_t *soc_hdl,
						uint8_t vdev_id,
						uint8_t *peer_mac,
						uint8_t *wds_macaddr,
						uint32_t flags)
{
	int status = -1;
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_ast_entry  *ast_entry = NULL;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc_hdl,
						       peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Peer is NULL!\n", __func__);
		goto fail;
	}

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = dp_peer_ast_hash_find_by_pdevid(soc, wds_macaddr,
						    peer->vdev->pdev->pdev_id);

	if (ast_entry) {
		status = dp_peer_update_ast(soc,
					    peer,
					    ast_entry, flags);
	}
	qdf_spin_unlock_bh(&soc->ast_lock);

fail:
	if (peer)
		dp_peer_unref_delete(peer);

	return status;
}

/*
 * dp_wds_reset_ast_wifi3() - Reset the is_active param for ast entry
 * @soc_handle:		Datapath SOC handle
 * @wds_macaddr:	WDS entry MAC Address
 * @peer_macaddr:	WDS entry MAC Address
 * @vdev_id:		id of vdev handle
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_wds_reset_ast_wifi3(struct cdp_soc_t *soc_hdl,
					 uint8_t *wds_macaddr,
					 uint8_t *peer_mac_addr,
					 uint8_t vdev_id)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_ast_entry *ast_entry = NULL;
	struct dp_ast_entry *tmp_ast_entry;
	struct dp_peer *peer;
	struct dp_pdev *pdev;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	pdev = vdev->pdev;

	if (peer_mac_addr) {
		peer = dp_peer_find_hash_find(soc, peer_mac_addr,
					      0, vdev->vdev_id);
		if (!peer) {
			return QDF_STATUS_E_FAILURE;
		}

		if (peer->delete_in_progress) {
			dp_peer_unref_delete(peer);
			return QDF_STATUS_E_FAILURE;
		}

		qdf_spin_lock_bh(&soc->ast_lock);
		DP_PEER_ITERATE_ASE_LIST(peer, ast_entry, tmp_ast_entry) {
			if ((ast_entry->type == CDP_TXRX_AST_TYPE_WDS_HM) ||
			    (ast_entry->type == CDP_TXRX_AST_TYPE_WDS_HM_SEC))
				dp_peer_del_ast(soc, ast_entry);
		}
		qdf_spin_unlock_bh(&soc->ast_lock);
		dp_peer_unref_delete(peer);

		return QDF_STATUS_SUCCESS;
	} else if (wds_macaddr) {
		qdf_spin_lock_bh(&soc->ast_lock);
		ast_entry = dp_peer_ast_hash_find_by_pdevid(soc, wds_macaddr,
							    pdev->pdev_id);

		if (ast_entry) {
			if ((ast_entry->type == CDP_TXRX_AST_TYPE_WDS_HM) ||
			    (ast_entry->type == CDP_TXRX_AST_TYPE_WDS_HM_SEC))
				dp_peer_del_ast(soc, ast_entry);
		}
		qdf_spin_unlock_bh(&soc->ast_lock);
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_wds_reset_ast_table_wifi3() - Reset the is_active param for all ast entry
 * @soc:		Datapath SOC handle
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_wds_reset_ast_table_wifi3(struct cdp_soc_t  *soc_hdl,
			     uint8_t vdev_id)
{
	struct dp_soc *soc = (struct dp_soc *) soc_hdl;
	struct dp_pdev *pdev;
	struct dp_vdev *vdev;
	struct dp_peer *peer;
	struct dp_ast_entry *ase, *temp_ase;
	int i;

	qdf_spin_lock_bh(&soc->ast_lock);

	for (i = 0; i < MAX_PDEV_CNT && soc->pdev_list[i]; i++) {
		pdev = soc->pdev_list[i];
		qdf_spin_lock_bh(&pdev->vdev_list_lock);
		DP_PDEV_ITERATE_VDEV_LIST(pdev, vdev) {
			DP_VDEV_ITERATE_PEER_LIST(vdev, peer) {
				DP_PEER_ITERATE_ASE_LIST(peer, ase, temp_ase) {
					if ((ase->type ==
						CDP_TXRX_AST_TYPE_WDS_HM) ||
					    (ase->type ==
						CDP_TXRX_AST_TYPE_WDS_HM_SEC))
						dp_peer_del_ast(soc, ase);
				}
			}
		}
		qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	}

	qdf_spin_unlock_bh(&soc->ast_lock);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_wds_flush_ast_table_wifi3() - Delete all wds and hmwds ast entry
 * @soc:		Datapath SOC handle
 *
 * Return: None
 */
static void dp_wds_flush_ast_table_wifi3(struct cdp_soc_t  *soc_hdl)
{
	struct dp_soc *soc = (struct dp_soc *) soc_hdl;
	struct dp_pdev *pdev;
	struct dp_vdev *vdev;
	struct dp_peer *peer;
	struct dp_ast_entry *ase, *temp_ase;
	int i;

	qdf_spin_lock_bh(&soc->ast_lock);

	for (i = 0; i < MAX_PDEV_CNT && soc->pdev_list[i]; i++) {
		pdev = soc->pdev_list[i];
		qdf_spin_lock_bh(&pdev->vdev_list_lock);
		DP_PDEV_ITERATE_VDEV_LIST(pdev, vdev) {
			DP_VDEV_ITERATE_PEER_LIST(vdev, peer) {
				DP_PEER_ITERATE_ASE_LIST(peer, ase, temp_ase) {
					if ((ase->type ==
						CDP_TXRX_AST_TYPE_STATIC) ||
						(ase->type ==
						 CDP_TXRX_AST_TYPE_SELF) ||
						(ase->type ==
						 CDP_TXRX_AST_TYPE_STA_BSS))
						continue;
					dp_peer_del_ast(soc, ase);
				}
			}
		}
		qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	}

	qdf_spin_unlock_bh(&soc->ast_lock);
}

/**
 * dp_peer_get_ast_info_by_soc_wifi3() - search the soc AST hash table
 *                                       and return ast entry information
 *                                       of first ast entry found in the
 *                                       table with given mac address
 *
 * @soc : data path soc handle
 * @ast_mac_addr : AST entry mac address
 * @ast_entry_info : ast entry information
 *
 * return : true if ast entry found with ast_mac_addr
 *          false if ast entry not found
 */
static bool dp_peer_get_ast_info_by_soc_wifi3
	(struct cdp_soc_t *soc_hdl,
	 uint8_t *ast_mac_addr,
	 struct cdp_ast_entry_info *ast_entry_info)
{
	struct dp_ast_entry *ast_entry = NULL;
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;

	qdf_spin_lock_bh(&soc->ast_lock);

	ast_entry = dp_peer_ast_hash_find_soc(soc, ast_mac_addr);
	if (!ast_entry || !ast_entry->peer) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return false;
	}
	if (ast_entry->delete_in_progress && !ast_entry->callback) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return false;
	}
	ast_entry_info->type = ast_entry->type;
	ast_entry_info->pdev_id = ast_entry->pdev_id;
	ast_entry_info->vdev_id = ast_entry->peer->vdev->vdev_id;
	ast_entry_info->peer_id = ast_entry->peer->peer_ids[0];
	qdf_mem_copy(&ast_entry_info->peer_mac_addr[0],
		     &ast_entry->peer->mac_addr.raw[0],
		     QDF_MAC_ADDR_SIZE);
	qdf_spin_unlock_bh(&soc->ast_lock);
	return true;
}

/**
 * dp_peer_get_ast_info_by_pdevid_wifi3() - search the soc AST hash table
 *                                          and return ast entry information
 *                                          if mac address and pdev_id matches
 *
 * @soc : data path soc handle
 * @ast_mac_addr : AST entry mac address
 * @pdev_id : pdev_id
 * @ast_entry_info : ast entry information
 *
 * return : true if ast entry found with ast_mac_addr
 *          false if ast entry not found
 */
static bool dp_peer_get_ast_info_by_pdevid_wifi3
		(struct cdp_soc_t *soc_hdl,
		 uint8_t *ast_mac_addr,
		 uint8_t pdev_id,
		 struct cdp_ast_entry_info *ast_entry_info)
{
	struct dp_ast_entry *ast_entry;
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;

	qdf_spin_lock_bh(&soc->ast_lock);

	ast_entry = dp_peer_ast_hash_find_by_pdevid(soc, ast_mac_addr, pdev_id);

	if (!ast_entry || !ast_entry->peer) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return false;
	}
	if (ast_entry->delete_in_progress && !ast_entry->callback) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return false;
	}
	ast_entry_info->type = ast_entry->type;
	ast_entry_info->pdev_id = ast_entry->pdev_id;
	ast_entry_info->vdev_id = ast_entry->peer->vdev->vdev_id;
	ast_entry_info->peer_id = ast_entry->peer->peer_ids[0];
	qdf_mem_copy(&ast_entry_info->peer_mac_addr[0],
		     &ast_entry->peer->mac_addr.raw[0],
		     QDF_MAC_ADDR_SIZE);
	qdf_spin_unlock_bh(&soc->ast_lock);
	return true;
}

/**
 * dp_peer_ast_entry_del_by_soc() - delete the ast entry from soc AST hash table
 *                            with given mac address
 *
 * @soc : data path soc handle
 * @ast_mac_addr : AST entry mac address
 * @callback : callback function to called on ast delete response from FW
 * @cookie : argument to be passed to callback
 *
 * return : QDF_STATUS_SUCCESS if ast entry found with ast_mac_addr and delete
 *          is sent
 *          QDF_STATUS_E_INVAL false if ast entry not found
 */
static QDF_STATUS dp_peer_ast_entry_del_by_soc(struct cdp_soc_t *soc_handle,
					       uint8_t *mac_addr,
					       txrx_ast_free_cb callback,
					       void *cookie)

{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;
	struct dp_ast_entry *ast_entry = NULL;
	txrx_ast_free_cb cb = NULL;
	void *arg = NULL;

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = dp_peer_ast_hash_find_soc(soc, mac_addr);
	if (!ast_entry) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return -QDF_STATUS_E_INVAL;
	}

	if (ast_entry->callback) {
		cb = ast_entry->callback;
		arg = ast_entry->cookie;
	}

	ast_entry->callback = callback;
	ast_entry->cookie = cookie;

	/*
	 * if delete_in_progress is set AST delete is sent to target
	 * and host is waiting for response should not send delete
	 * again
	 */
	if (!ast_entry->delete_in_progress)
		dp_peer_del_ast(soc, ast_entry);

	qdf_spin_unlock_bh(&soc->ast_lock);
	if (cb) {
		cb(soc->ctrl_psoc,
		   dp_soc_to_cdp_soc(soc),
		   arg,
		   CDP_TXRX_AST_DELETE_IN_PROGRESS);
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_peer_ast_entry_del_by_pdev() - delete the ast entry from soc AST hash
 *                                   table if mac address and pdev_id matches
 *
 * @soc : data path soc handle
 * @ast_mac_addr : AST entry mac address
 * @pdev_id : pdev id
 * @callback : callback function to called on ast delete response from FW
 * @cookie : argument to be passed to callback
 *
 * return : QDF_STATUS_SUCCESS if ast entry found with ast_mac_addr and delete
 *          is sent
 *          QDF_STATUS_E_INVAL false if ast entry not found
 */

static QDF_STATUS dp_peer_ast_entry_del_by_pdev(struct cdp_soc_t *soc_handle,
						uint8_t *mac_addr,
						uint8_t pdev_id,
						txrx_ast_free_cb callback,
						void *cookie)

{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;
	struct dp_ast_entry *ast_entry;
	txrx_ast_free_cb cb = NULL;
	void *arg = NULL;

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = dp_peer_ast_hash_find_by_pdevid(soc, mac_addr, pdev_id);

	if (!ast_entry) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return -QDF_STATUS_E_INVAL;
	}

	if (ast_entry->callback) {
		cb = ast_entry->callback;
		arg = ast_entry->cookie;
	}

	ast_entry->callback = callback;
	ast_entry->cookie = cookie;

	/*
	 * if delete_in_progress is set AST delete is sent to target
	 * and host is waiting for response should not sent delete
	 * again
	 */
	if (!ast_entry->delete_in_progress)
		dp_peer_del_ast(soc, ast_entry);

	qdf_spin_unlock_bh(&soc->ast_lock);

	if (cb) {
		cb(soc->ctrl_psoc,
		   dp_soc_to_cdp_soc(soc),
		   arg,
		   CDP_TXRX_AST_DELETE_IN_PROGRESS);
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_srng_find_ring_in_mask() - find which ext_group a ring belongs
 * @ring_num: ring num of the ring being queried
 * @grp_mask: the grp_mask array for the ring type in question.
 *
 * The grp_mask array is indexed by group number and the bit fields correspond
 * to ring numbers.  We are finding which interrupt group a ring belongs to.
 *
 * Return: the index in the grp_mask array with the ring number.
 * -QDF_STATUS_E_NOENT if no entry is found
 */
static int dp_srng_find_ring_in_mask(int ring_num, int *grp_mask)
{
	int ext_group_num;
	int mask = 1 << ring_num;

	for (ext_group_num = 0; ext_group_num < WLAN_CFG_INT_NUM_CONTEXTS;
	     ext_group_num++) {
		if (mask & grp_mask[ext_group_num])
			return ext_group_num;
	}

	return -QDF_STATUS_E_NOENT;
}

static int dp_srng_calculate_msi_group(struct dp_soc *soc,
				       enum hal_ring_type ring_type,
				       int ring_num)
{
	int *grp_mask;

	switch (ring_type) {
	case WBM2SW_RELEASE:
		/* dp_tx_comp_handler - soc->tx_comp_ring */
		if (ring_num < 3)
			grp_mask = &soc->wlan_cfg_ctx->int_tx_ring_mask[0];

		/* dp_rx_wbm_err_process - soc->rx_rel_ring */
		else if (ring_num == 3) {
			/* sw treats this as a separate ring type */
			grp_mask = &soc->wlan_cfg_ctx->
				int_rx_wbm_rel_ring_mask[0];
			ring_num = 0;
		} else {
			qdf_assert(0);
			return -QDF_STATUS_E_NOENT;
		}
	break;

	case REO_EXCEPTION:
		/* dp_rx_err_process - &soc->reo_exception_ring */
		grp_mask = &soc->wlan_cfg_ctx->int_rx_err_ring_mask[0];
	break;

	case REO_DST:
		/* dp_rx_process - soc->reo_dest_ring */
		grp_mask = &soc->wlan_cfg_ctx->int_rx_ring_mask[0];
	break;

	case REO_STATUS:
		/* dp_reo_status_ring_handler - soc->reo_status_ring */
		grp_mask = &soc->wlan_cfg_ctx->int_reo_status_ring_mask[0];
	break;

	/* dp_rx_mon_status_srng_process - pdev->rxdma_mon_status_ring*/
	case RXDMA_MONITOR_STATUS:
	/* dp_rx_mon_dest_process - pdev->rxdma_mon_dst_ring */
	case RXDMA_MONITOR_DST:
		/* dp_mon_process */
		grp_mask = &soc->wlan_cfg_ctx->int_rx_mon_ring_mask[0];
	break;
	case RXDMA_DST:
		/* dp_rxdma_err_process */
		grp_mask = &soc->wlan_cfg_ctx->int_rxdma2host_ring_mask[0];
	break;

	case RXDMA_BUF:
		grp_mask = &soc->wlan_cfg_ctx->int_host2rxdma_ring_mask[0];
	break;

	case RXDMA_MONITOR_BUF:
		/* TODO: support low_thresh interrupt */
		return -QDF_STATUS_E_NOENT;
	break;

	case TCL_DATA:
	case TCL_CMD:
	case REO_CMD:
	case SW2WBM_RELEASE:
	case WBM_IDLE_LINK:
		/* normally empty SW_TO_HW rings */
		return -QDF_STATUS_E_NOENT;
	break;

	case TCL_STATUS:
	case REO_REINJECT:
		/* misc unused rings */
		return -QDF_STATUS_E_NOENT;
	break;

	case CE_SRC:
	case CE_DST:
	case CE_DST_STATUS:
		/* CE_rings - currently handled by hif */
	default:
		return -QDF_STATUS_E_NOENT;
	break;
	}

	return dp_srng_find_ring_in_mask(ring_num, grp_mask);
}

static void dp_srng_msi_setup(struct dp_soc *soc, struct hal_srng_params
			      *ring_params, int ring_type, int ring_num)
{
	int msi_group_number;
	int msi_data_count;
	int ret;
	uint32_t msi_data_start, msi_irq_start, addr_low, addr_high;

	ret = pld_get_user_msi_assignment(soc->osdev->dev, "DP",
					    &msi_data_count, &msi_data_start,
					    &msi_irq_start);

	if (ret)
		return;

	msi_group_number = dp_srng_calculate_msi_group(soc, ring_type,
						       ring_num);
	if (msi_group_number < 0) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO_LOW,
			FL("ring not part of an ext_group; ring_type: %d,ring_num %d"),
			ring_type, ring_num);
		ring_params->msi_addr = 0;
		ring_params->msi_data = 0;
		return;
	}

	if (msi_group_number > msi_data_count) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_WARN,
			FL("2 msi_groups will share an msi; msi_group_num %d"),
			msi_group_number);

		QDF_ASSERT(0);
	}

	pld_get_msi_address(soc->osdev->dev, &addr_low, &addr_high);

	ring_params->msi_addr = addr_low;
	ring_params->msi_addr |= (qdf_dma_addr_t)(((uint64_t)addr_high) << 32);
	ring_params->msi_data = (msi_group_number % msi_data_count)
		+ msi_data_start;
	ring_params->flags |= HAL_SRNG_MSI_INTR;
}

/**
 * dp_print_ast_stats() - Dump AST table contents
 * @soc: Datapath soc handle
 *
 * return void
 */
#ifdef FEATURE_AST
void dp_print_ast_stats(struct dp_soc *soc)
{
	uint8_t i;
	uint8_t num_entries = 0;
	struct dp_vdev *vdev;
	struct dp_pdev *pdev;
	struct dp_peer *peer;
	struct dp_ast_entry *ase, *tmp_ase;
	char type[CDP_TXRX_AST_TYPE_MAX][10] = {
			"NONE", "STATIC", "SELF", "WDS", "MEC", "HMWDS", "BSS",
			"DA", "HMWDS_SEC"};

	DP_PRINT_STATS("AST Stats:");
	DP_PRINT_STATS("	Entries Added   = %d", soc->stats.ast.added);
	DP_PRINT_STATS("	Entries Deleted = %d", soc->stats.ast.deleted);
	DP_PRINT_STATS("	Entries Agedout = %d", soc->stats.ast.aged_out);
	DP_PRINT_STATS("	Entries MAP ERR  = %d", soc->stats.ast.map_err);

	DP_PRINT_STATS("AST Table:");

	qdf_spin_lock_bh(&soc->ast_lock);
	for (i = 0; i < MAX_PDEV_CNT && soc->pdev_list[i]; i++) {
		pdev = soc->pdev_list[i];
		qdf_spin_lock_bh(&pdev->vdev_list_lock);
		DP_PDEV_ITERATE_VDEV_LIST(pdev, vdev) {
			DP_VDEV_ITERATE_PEER_LIST(vdev, peer) {
				DP_PEER_ITERATE_ASE_LIST(peer, ase, tmp_ase) {
				    DP_PRINT_STATS("%6d mac_addr = "QDF_MAC_ADDR_FMT
					    " peer_mac_addr = "QDF_MAC_ADDR_FMT
					    " peer_id = %u"
					    " type = %s"
					    " next_hop = %d"
					    " is_active = %d"
					    " ast_idx = %d"
					    " ast_hash = %d"
					    " delete_in_progress = %d"
					    " pdev_id = %d"
					    " vdev_id = %d",
					    ++num_entries,
					    QDF_MAC_ADDR_REF(ase->mac_addr.raw),
					    QDF_MAC_ADDR_REF(ase->peer->mac_addr.raw),
					    ase->peer->peer_ids[0],
					    type[ase->type],
					    ase->next_hop,
					    ase->is_active,
					    ase->ast_idx,
					    ase->ast_hash_value,
					    ase->delete_in_progress,
					    ase->pdev_id,
					    vdev->vdev_id);
				}
			}
		}
		qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	}
	qdf_spin_unlock_bh(&soc->ast_lock);
}
#else
void dp_print_ast_stats(struct dp_soc *soc)
{
	DP_PRINT_STATS("AST Stats not available.Enable FEATURE_AST");
	return;
}
#endif

/**
 *  dp_print_peer_table() - Dump all Peer stats
 * @vdev: Datapath Vdev handle
 *
 * return void
 */
static void dp_print_peer_table(struct dp_vdev *vdev)
{
	struct dp_peer *peer = NULL;

	DP_PRINT_STATS("Dumping Peer Table  Stats:");
	TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem) {
		if (!peer) {
			DP_PRINT_STATS("Invalid Peer");
			return;
		}
		DP_PRINT_STATS("    peer_mac_addr = "QDF_MAC_ADDR_FMT
			       " nawds_enabled = %d"
			       " bss_peer = %d"
			       " wds_enabled = %d"
			       " tx_cap_enabled = %d"
			       " rx_cap_enabled = %d"
			       " delete in progress = %d"
			       " peer id = %d",
			       QDF_MAC_ADDR_REF(peer->mac_addr.raw),
			       peer->nawds_enabled,
			       peer->bss_peer,
			       peer->wds_enabled,
			       peer->tx_cap_enabled,
			       peer->rx_cap_enabled,
			       peer->delete_in_progress,
			       peer->peer_ids[0]);
	}
}

#ifdef WLAN_DP_PER_RING_TYPE_CONFIG
/**
 * dp_srng_configure_interrupt_thresholds() - Retrieve interrupt
 * threshold values from the wlan_srng_cfg table for each ring type
 * @soc: device handle
 * @ring_params: per ring specific parameters
 * @ring_type: Ring type
 * @ring_num: Ring number for a given ring type
 *
 * Fill the ring params with the interrupt threshold
 * configuration parameters available in the per ring type wlan_srng_cfg
 * table.
 *
 * Return: None
 */
static void
dp_srng_configure_interrupt_thresholds(struct dp_soc *soc,
				       struct hal_srng_params *ring_params,
				       int ring_type, int ring_num,
				       int num_entries)
{
	if (ring_type == WBM2SW_RELEASE && (ring_num == 3)) {
		ring_params->intr_timer_thres_us =
				wlan_cfg_get_int_timer_threshold_other(soc->wlan_cfg_ctx);
		ring_params->intr_batch_cntr_thres_entries =
				wlan_cfg_get_int_batch_threshold_other(soc->wlan_cfg_ctx);
	} else {
		ring_params->intr_timer_thres_us =
				soc->wlan_srng_cfg[ring_type].timer_threshold;
		ring_params->intr_batch_cntr_thres_entries =
				soc->wlan_srng_cfg[ring_type].batch_count_threshold;
	}
	ring_params->low_threshold =
			soc->wlan_srng_cfg[ring_type].low_threshold;

	if (ring_params->low_threshold)
		ring_params->flags |= HAL_SRNG_LOW_THRES_INTR_ENABLE;
}
#else
static void
dp_srng_configure_interrupt_thresholds(struct dp_soc *soc,
				       struct hal_srng_params *ring_params,
				       int ring_type, int ring_num,
				       int num_entries)
{
	if (ring_type == REO_DST) {
		ring_params->intr_timer_thres_us =
			wlan_cfg_get_int_timer_threshold_rx(soc->wlan_cfg_ctx);
		ring_params->intr_batch_cntr_thres_entries =
			wlan_cfg_get_int_batch_threshold_rx(soc->wlan_cfg_ctx);
	} else if (ring_type == WBM2SW_RELEASE && (ring_num < 3)) {
		ring_params->intr_timer_thres_us =
			wlan_cfg_get_int_timer_threshold_tx(soc->wlan_cfg_ctx);
		ring_params->intr_batch_cntr_thres_entries =
			wlan_cfg_get_int_batch_threshold_tx(soc->wlan_cfg_ctx);
	} else {
		ring_params->intr_timer_thres_us =
			wlan_cfg_get_int_timer_threshold_other(soc->wlan_cfg_ctx);
		ring_params->intr_batch_cntr_thres_entries =
			wlan_cfg_get_int_batch_threshold_other(soc->wlan_cfg_ctx);
	}

	/* Enable low threshold interrupts for rx buffer rings (regular and
	 * monitor buffer rings.
	 * TODO: See if this is required for any other ring
	 */
	if ((ring_type == RXDMA_BUF) || (ring_type == RXDMA_MONITOR_BUF) ||
	    (ring_type == RXDMA_MONITOR_STATUS)) {
		/* TODO: Setting low threshold to 1/8th of ring size
		 * see if this needs to be configurable
		 */
		ring_params->low_threshold = num_entries >> 3;
		ring_params->intr_timer_thres_us =
			wlan_cfg_get_int_timer_threshold_rx(soc->wlan_cfg_ctx);
		ring_params->flags |= HAL_SRNG_LOW_THRES_INTR_ENABLE;
		ring_params->intr_batch_cntr_thres_entries = 0;
	}
}
#endif


#ifdef DP_MEM_PRE_ALLOC
void *dp_context_alloc_mem(struct dp_soc *soc, enum dp_ctxt_type ctxt_type,
			   size_t ctxt_size)
{
	void *ctxt_mem;

	if (!soc->cdp_soc.ol_ops->dp_prealloc_get_context) {
		dp_warn("dp_prealloc_get_context null!");
		goto dynamic_alloc;
	}

	ctxt_mem = soc->cdp_soc.ol_ops->dp_prealloc_get_context(ctxt_type);

	if (ctxt_mem)
		goto end;

dynamic_alloc:
	dp_info("Pre-alloc of ctxt failed. Dynamic allocation");
	ctxt_mem = qdf_mem_malloc(ctxt_size);
end:
	return ctxt_mem;
}

void dp_context_free_mem(struct dp_soc *soc, enum dp_ctxt_type ctxt_type,
			 void *vaddr)
{
	QDF_STATUS status;

	if (soc->cdp_soc.ol_ops->dp_prealloc_put_context) {
		status = soc->cdp_soc.ol_ops->dp_prealloc_put_context(
								ctxt_type,
								vaddr);
	} else {
		dp_warn("dp_prealloc_get_context null!");
		status = QDF_STATUS_E_NOSUPPORT;
	}

	if (QDF_IS_STATUS_ERROR(status)) {
		dp_info("Context not pre-allocated");
		qdf_mem_free(vaddr);
	}
}

static inline
void *dp_srng_aligned_mem_alloc_consistent(struct dp_soc *soc,
					   struct dp_srng *srng,
					   struct hal_srng_params *ring_params,
					   uint32_t ring_type)
{
	void *mem;

	qdf_assert(!srng->is_mem_prealloc);

	if (!soc->cdp_soc.ol_ops->dp_prealloc_get_consistent) {
		dp_warn("dp_prealloc_get_consistent is null!");
		goto qdf;
	}

	mem =
		soc->cdp_soc.ol_ops->dp_prealloc_get_consistent
						(&srng->alloc_size,
						&srng->base_vaddr_unaligned,
						&srng->base_paddr_unaligned,
						&ring_params->ring_base_paddr,
						DP_RING_BASE_ALIGN, ring_type);

	if (mem) {
		srng->is_mem_prealloc = true;
		goto end;
	}
qdf:
	mem =  qdf_aligned_mem_alloc_consistent(soc->osdev, &srng->alloc_size,
						&srng->base_vaddr_unaligned,
						&srng->base_paddr_unaligned,
						&ring_params->ring_base_paddr,
						DP_RING_BASE_ALIGN);
end:
	dp_info("%s memory %pK dp_srng %pK alloc_size %d num_entries %d",
		srng->is_mem_prealloc ? "pre-alloc" : "dynamic-alloc", mem,
		srng, srng->alloc_size, srng->num_entries);
	return mem;
}

static inline void dp_srng_mem_free_consistent(struct dp_soc *soc,
					       struct dp_srng *srng)
{
	if (srng->is_mem_prealloc) {
		if (!soc->cdp_soc.ol_ops->dp_prealloc_put_consistent) {
			dp_warn("dp_prealloc_put_consistent is null!");
			QDF_BUG(0);
			return;
		}
		soc->cdp_soc.ol_ops->dp_prealloc_put_consistent
						(srng->alloc_size,
						 srng->base_vaddr_unaligned,
						 srng->base_paddr_unaligned);

	} else {
		qdf_mem_free_consistent(soc->osdev, soc->osdev->dev,
					srng->alloc_size,
					srng->base_vaddr_unaligned,
					srng->base_paddr_unaligned, 0);
	}
}

void dp_desc_multi_pages_mem_alloc(struct dp_soc *soc,
				   enum dp_desc_type desc_type,
				   struct qdf_mem_multi_page_t *pages,
				   size_t element_size,
				   uint16_t element_num,
				   qdf_dma_context_t memctxt,
				   bool cacheable)
{
	if (!soc->cdp_soc.ol_ops->dp_get_multi_pages) {
		dp_warn("dp_get_multi_pages is null!");
		goto qdf;
	}

	pages->num_pages = 0;
	pages->is_mem_prealloc = 0;
	soc->cdp_soc.ol_ops->dp_get_multi_pages(desc_type,
						element_size,
						element_num,
						pages,
						cacheable);
	if (pages->num_pages)
		goto end;

qdf:
	qdf_mem_multi_pages_alloc(soc->osdev, pages, element_size,
				  element_num, memctxt, cacheable);
end:
	dp_info("%s desc_type %d element_size %d element_num %d cacheable %d",
		pages->is_mem_prealloc ? "pre-alloc" : "dynamic-alloc",
		desc_type, (int)element_size, element_num, cacheable);
}

void dp_desc_multi_pages_mem_free(struct dp_soc *soc,
				  enum dp_desc_type desc_type,
				  struct qdf_mem_multi_page_t *pages,
				  qdf_dma_context_t memctxt,
				  bool cacheable)
{
	if (pages->is_mem_prealloc) {
		if (!soc->cdp_soc.ol_ops->dp_put_multi_pages) {
			dp_warn("dp_put_multi_pages is null!");
			QDF_BUG(0);
			return;
		}

		soc->cdp_soc.ol_ops->dp_put_multi_pages(desc_type, pages);
		qdf_mem_zero(pages, sizeof(*pages));
	} else {
		qdf_mem_multi_pages_free(soc->osdev, pages,
					 memctxt, cacheable);
	}
}

#else

static inline
void *dp_srng_aligned_mem_alloc_consistent(struct dp_soc *soc,
					   struct dp_srng *srng,
					   struct hal_srng_params *ring_params,
					   uint32_t ring_type)

{
	return qdf_aligned_mem_alloc_consistent(soc->osdev, &srng->alloc_size,
						&srng->base_vaddr_unaligned,
						&srng->base_paddr_unaligned,
						&ring_params->ring_base_paddr,
						DP_RING_BASE_ALIGN);
}

static inline void dp_srng_mem_free_consistent(struct dp_soc *soc,
					       struct dp_srng *srng)
{
	qdf_mem_free_consistent(soc->osdev, soc->osdev->dev,
				srng->alloc_size,
				srng->base_vaddr_unaligned,
				srng->base_paddr_unaligned, 0);
}

#endif /* DP_MEM_PRE_ALLOC */
/**
 * dp_srng_setup() - Internal function to setup SRNG rings used by data path
 * @soc: datapath soc handle
 * @srng: srng handle
 * @ring_type: ring that needs to be configured
 * @mac_id: mac number
 * @num_entries: Total number of entries for a given ring
 *
 * Return: non-zero - failure/zero - success
 */
static int dp_srng_setup(struct dp_soc *soc, struct dp_srng *srng,
			 int ring_type, int ring_num, int mac_id,
			 uint32_t num_entries, bool cached)
{
	hal_soc_handle_t hal_soc = soc->hal_soc;
	uint32_t entry_size = hal_srng_get_entrysize(hal_soc, ring_type);
	struct hal_srng_params ring_params;
	uint32_t max_entries = hal_srng_max_entries(hal_soc, ring_type);

	/* TODO: Currently hal layer takes care of endianness related settings.
	 * See if these settings need to passed from DP layer
	 */
	qdf_mem_zero(&ring_params, sizeof(struct hal_srng_params));

	num_entries = (num_entries > max_entries) ? max_entries : num_entries;
	srng->hal_srng = NULL;
	srng->alloc_size = num_entries * entry_size;
	srng->num_entries = num_entries;

	if (!dp_is_soc_reinit(soc)) {
		if (!cached) {
			ring_params.ring_base_vaddr =
			    dp_srng_aligned_mem_alloc_consistent(soc, srng,
								 &ring_params,
								 ring_type);
		} else {
			ring_params.ring_base_vaddr = qdf_aligned_malloc(
					&srng->alloc_size,
					&srng->base_vaddr_unaligned,
					&srng->base_paddr_unaligned,
					&ring_params.ring_base_paddr,
					DP_RING_BASE_ALIGN);
		}

		if (!ring_params.ring_base_vaddr) {
			dp_err("alloc failed - ring_type: %d, ring_num %d",
					ring_type, ring_num);
			return QDF_STATUS_E_NOMEM;
		}
	}

	ring_params.ring_base_paddr = (qdf_dma_addr_t)qdf_align(
			(unsigned long)(srng->base_paddr_unaligned),
			DP_RING_BASE_ALIGN);

	ring_params.ring_base_vaddr = (void *)(
			(unsigned long)(srng->base_vaddr_unaligned) +
			((unsigned long)(ring_params.ring_base_paddr) -
			 (unsigned long)(srng->base_paddr_unaligned)));

	qdf_assert_always(ring_params.ring_base_vaddr);

	ring_params.num_entries = num_entries;

	dp_info("Ring type: %d, num:%d vaddr %pK paddr %pK entries %u",
		ring_type, ring_num,
		(void *)ring_params.ring_base_vaddr,
		(void *)ring_params.ring_base_paddr,
		ring_params.num_entries);

	if (soc->intr_mode == DP_INTR_MSI) {
		dp_srng_msi_setup(soc, &ring_params, ring_type, ring_num);
		dp_verbose_debug("Using MSI for ring_type: %d, ring_num %d",
				 ring_type, ring_num);

	} else {
		ring_params.msi_data = 0;
		ring_params.msi_addr = 0;
		dp_verbose_debug("Skipping MSI for ring_type: %d, ring_num %d",
				 ring_type, ring_num);
	}

	dp_srng_configure_interrupt_thresholds(soc, &ring_params,
					       ring_type, ring_num,
					       num_entries);

	if (cached) {
		ring_params.flags |= HAL_SRNG_CACHED_DESC;
		srng->cached = 1;
	}

	srng->hal_srng = hal_srng_setup(hal_soc, ring_type, ring_num,
		mac_id, &ring_params);

	if (!srng->hal_srng) {
		if (cached) {
			qdf_mem_free(srng->base_vaddr_unaligned);
		} else {
			dp_srng_mem_free_consistent(soc, srng);
		}
	}

	return 0;
}

/*
 * dp_srng_deinit() - Internal function to deinit SRNG rings used by data path
 * @soc: DP SOC handle
 * @srng: source ring structure
 * @ring_type: type of ring
 * @ring_num: ring number
 *
 * Return: None
 */
static void dp_srng_deinit(struct dp_soc *soc, struct dp_srng *srng,
			   int ring_type, int ring_num)
{
	if (!srng->hal_srng) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Ring type: %d, num:%d not setup"),
			  ring_type, ring_num);
		return;
	}

	hal_srng_cleanup(soc->hal_soc, srng->hal_srng);
	srng->hal_srng = NULL;
}

/**
 * dp_srng_cleanup - Internal function to cleanup SRNG rings used by data path
 * Any buffers allocated and attached to ring entries are expected to be freed
 * before calling this function.
 */
static void dp_srng_cleanup(struct dp_soc *soc, struct dp_srng *srng,
	int ring_type, int ring_num)
{
	if (!dp_is_soc_reinit(soc)) {
		if (!srng->hal_srng && (srng->alloc_size == 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  FL("Ring type: %d, num:%d not setup"),
				  ring_type, ring_num);
			return;
		}

		if (srng->hal_srng) {
			hal_srng_cleanup(soc->hal_soc, srng->hal_srng);
			srng->hal_srng = NULL;
		}
	}

	if (srng->alloc_size && srng->base_vaddr_unaligned) {
		if (!srng->cached) {
			dp_srng_mem_free_consistent(soc, srng);
		} else {
			qdf_mem_free(srng->base_vaddr_unaligned);
		}
		srng->alloc_size = 0;
		srng->base_vaddr_unaligned = NULL;
	}
	srng->hal_srng = NULL;
}

/* TODO: Need this interface from HIF */
void *hif_get_hal_handle(struct hif_opaque_softc *hif_handle);

#ifdef WLAN_FEATURE_DP_EVENT_HISTORY
int dp_srng_access_start(struct dp_intr *int_ctx, struct dp_soc *dp_soc,
			 hal_ring_handle_t hal_ring_hdl)
{
	hal_soc_handle_t hal_soc = dp_soc->hal_soc;
	uint32_t hp, tp;
	uint8_t ring_id;

	hal_get_sw_hptp(hal_soc, hal_ring_hdl, &tp, &hp);
	ring_id = hal_srng_ring_id_get(hal_ring_hdl);

	hif_record_event(dp_soc->hif_handle, int_ctx->dp_intr_id,
			 ring_id, hp, tp, HIF_EVENT_SRNG_ACCESS_START);

	return hal_srng_access_start(hal_soc, hal_ring_hdl);
}

void dp_srng_access_end(struct dp_intr *int_ctx, struct dp_soc *dp_soc,
			hal_ring_handle_t hal_ring_hdl)
{
	hal_soc_handle_t hal_soc = dp_soc->hal_soc;
	uint32_t hp, tp;
	uint8_t ring_id;

	hal_get_sw_hptp(hal_soc, hal_ring_hdl, &tp, &hp);
	ring_id = hal_srng_ring_id_get(hal_ring_hdl);

	hif_record_event(dp_soc->hif_handle, int_ctx->dp_intr_id,
			 ring_id, hp, tp, HIF_EVENT_SRNG_ACCESS_END);

	return hal_srng_access_end(hal_soc, hal_ring_hdl);
}
#endif /* WLAN_FEATURE_DP_EVENT_HISTORY */

/*
 * dp_should_timer_irq_yield() - Decide if the bottom half should yield
 * @soc: DP soc handle
 * @work_done: work done in softirq context
 * @start_time: start time for the softirq
 *
 * Return: enum with yield code
 */
static enum timer_yield_status
dp_should_timer_irq_yield(struct dp_soc *soc, uint32_t work_done,
			  uint64_t start_time)
{
	uint64_t cur_time = qdf_get_log_timestamp();

	if (!work_done)
		return DP_TIMER_WORK_DONE;

	if (cur_time - start_time > DP_MAX_TIMER_EXEC_TIME_TICKS)
		return DP_TIMER_TIME_EXHAUST;

	return DP_TIMER_NO_YIELD;
}

/**
 * dp_process_lmac_rings() - Process LMAC rings
 * @int_ctx: interrupt context
 * @total_budget: budget of work which can be done
 *
 * Return: work done
 */
static int dp_process_lmac_rings(struct dp_intr *int_ctx, int total_budget)
{
	struct dp_intr_stats *intr_stats = &int_ctx->intr_stats;
	struct dp_soc *soc = int_ctx->soc;
	uint32_t remaining_quota = total_budget;
	struct dp_pdev *pdev = NULL;
	uint32_t work_done  = 0;
	int budget = total_budget;
	int ring = 0;

	/* Process LMAC interrupts */
	for  (ring = 0 ; ring < MAX_NUM_LMAC_HW; ring++) {
		int mac_for_pdev = ring;

		pdev = dp_get_pdev_for_lmac_id(soc, mac_for_pdev);
		if (!pdev)
			continue;
		if (int_ctx->rx_mon_ring_mask & (1 << mac_for_pdev)) {
			work_done = dp_mon_process(soc, mac_for_pdev,
						   remaining_quota);
			if (work_done)
				intr_stats->num_rx_mon_ring_masks++;
			budget -= work_done;
			if (budget <= 0)
				goto budget_done;
			remaining_quota = budget;
		}

		if (int_ctx->rxdma2host_ring_mask &
				(1 << mac_for_pdev)) {
			work_done = dp_rxdma_err_process(int_ctx, soc,
							 mac_for_pdev,
							 remaining_quota);
			if (work_done)
				intr_stats->num_rxdma2host_ring_masks++;
			budget -=  work_done;
			if (budget <= 0)
				goto budget_done;
			remaining_quota = budget;
		}

		if (int_ctx->host2rxdma_ring_mask &
					(1 << mac_for_pdev)) {
			union dp_rx_desc_list_elem_t *desc_list = NULL;
			union dp_rx_desc_list_elem_t *tail = NULL;
			struct dp_srng *rx_refill_buf_ring;

			if (wlan_cfg_per_pdev_lmac_ring(soc->wlan_cfg_ctx))
				rx_refill_buf_ring =
					&soc->rx_refill_buf_ring[mac_for_pdev];
			else
				rx_refill_buf_ring =
					&soc->rx_refill_buf_ring[pdev->lmac_id];

			intr_stats->num_host2rxdma_ring_masks++;
			DP_STATS_INC(pdev, replenish.low_thresh_intrs,
				     1);
			dp_rx_buffers_replenish(soc, mac_for_pdev,
						rx_refill_buf_ring,
						&soc->rx_desc_buf[mac_for_pdev],
						0, &desc_list, &tail);
		}
	}

budget_done:
	return total_budget - budget;
}

/*
 * dp_service_srngs() - Top level interrupt handler for DP Ring interrupts
 * @dp_ctx: DP SOC handle
 * @budget: Number of frames/descriptors that can be processed in one shot
 *
 * Return: remaining budget/quota for the soc device
 */
static uint32_t dp_service_srngs(void *dp_ctx, uint32_t dp_budget)
{
	struct dp_intr *int_ctx = (struct dp_intr *)dp_ctx;
	struct dp_intr_stats *intr_stats = &int_ctx->intr_stats;
	struct dp_soc *soc = int_ctx->soc;
	int ring = 0;
	uint32_t work_done  = 0;
	int budget = dp_budget;
	uint8_t tx_mask = int_ctx->tx_ring_mask;
	uint8_t rx_mask = int_ctx->rx_ring_mask;
	uint8_t rx_err_mask = int_ctx->rx_err_ring_mask;
	uint8_t rx_wbm_rel_mask = int_ctx->rx_wbm_rel_ring_mask;
	uint8_t reo_status_mask = int_ctx->reo_status_ring_mask;
	uint32_t remaining_quota = dp_budget;

	dp_verbose_debug("tx %x rx %x rx_err %x rx_wbm_rel %x reo_status %x rx_mon_ring %x host2rxdma %x rxdma2host %x\n",
			 tx_mask, rx_mask, rx_err_mask, rx_wbm_rel_mask,
			 reo_status_mask,
			 int_ctx->rx_mon_ring_mask,
			 int_ctx->host2rxdma_ring_mask,
			 int_ctx->rxdma2host_ring_mask);

	/* Process Tx completion interrupts first to return back buffers */
	while (tx_mask) {
		if (tx_mask & 0x1) {
			work_done = dp_tx_comp_handler(int_ctx,
						       soc,
						       soc->tx_comp_ring[ring].hal_srng,
						       ring, remaining_quota);

			if (work_done) {
				intr_stats->num_tx_ring_masks[ring]++;
				dp_verbose_debug("tx mask 0x%x ring %d, budget %d, work_done %d",
						 tx_mask, ring, budget,
						 work_done);
			}

			budget -= work_done;
			if (budget <= 0)
				goto budget_done;

			remaining_quota = budget;
		}
		tx_mask = tx_mask >> 1;
		ring++;
	}

	/* Process REO Exception ring interrupt */
	if (rx_err_mask) {
		work_done = dp_rx_err_process(int_ctx, soc,
					      soc->reo_exception_ring.hal_srng,
					      remaining_quota);

		if (work_done) {
			intr_stats->num_rx_err_ring_masks++;
			dp_verbose_debug("REO Exception Ring: work_done %d budget %d",
					 work_done, budget);
		}

		budget -=  work_done;
		if (budget <= 0) {
			goto budget_done;
		}
		remaining_quota = budget;
	}

	/* Process Rx WBM release ring interrupt */
	if (rx_wbm_rel_mask) {
		work_done = dp_rx_wbm_err_process(int_ctx, soc,
						  soc->rx_rel_ring.hal_srng,
						  remaining_quota);

		if (work_done) {
			intr_stats->num_rx_wbm_rel_ring_masks++;
			dp_verbose_debug("WBM Release Ring: work_done %d budget %d",
					 work_done, budget);
		}

		budget -=  work_done;
		if (budget <= 0) {
			goto budget_done;
		}
		remaining_quota = budget;
	}

	/* Process Rx interrupts */
	if (rx_mask) {
		for (ring = 0; ring < soc->num_reo_dest_rings; ring++) {
			if (!(rx_mask & (1 << ring)))
				continue;
			work_done = dp_rx_process(int_ctx,
						  soc->reo_dest_ring[ring].hal_srng,
						  ring,
						  remaining_quota);
			if (work_done) {
				intr_stats->num_rx_ring_masks[ring]++;
				dp_verbose_debug("rx mask 0x%x ring %d, work_done %d budget %d",
						 rx_mask, ring,
						 work_done, budget);
				budget -=  work_done;
				if (budget <= 0)
					goto budget_done;
				remaining_quota = budget;
			}
		}
	}

	if (reo_status_mask) {
		if (dp_reo_status_ring_handler(int_ctx, soc))
			int_ctx->intr_stats.num_reo_status_ring_masks++;
	}

	work_done = dp_process_lmac_rings(int_ctx, remaining_quota);
	if (work_done) {
		budget -=  work_done;
		if (budget <= 0)
			goto budget_done;
		remaining_quota = budget;
	}

	qdf_lro_flush(int_ctx->lro_ctx);
	intr_stats->num_masks++;

budget_done:
	return dp_budget - budget;
}

/* dp_interrupt_timer()- timer poll for interrupts
 *
 * @arg: SoC Handle
 *
 * Return:
 *
 */
static void dp_interrupt_timer(void *arg)
{
	struct dp_soc *soc = (struct dp_soc *) arg;
	enum timer_yield_status yield = DP_TIMER_NO_YIELD;
	uint32_t work_done  = 0, total_work_done = 0;
	int budget = 0xffff;
	uint32_t remaining_quota = budget;
	uint64_t start_time;
	int i;

	if (!qdf_atomic_read(&soc->cmn_init_done))
		return;

	start_time = qdf_get_log_timestamp();

	while (yield == DP_TIMER_NO_YIELD) {
		for (i = 0;
		     i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++) {
			if (!soc->intr_ctx[i].rx_mon_ring_mask)
				continue;

			work_done = dp_process_lmac_rings(&soc->intr_ctx[i],
							  remaining_quota);
			if (work_done) {
				budget -=  work_done;
				if (budget <= 0) {
					yield = DP_TIMER_WORK_EXHAUST;
					goto budget_done;
				}
				remaining_quota = budget;
				total_work_done += work_done;
			}
		}

		yield = dp_should_timer_irq_yield(soc, total_work_done,
						  start_time);
		total_work_done = 0;
	}

budget_done:
	if (yield == DP_TIMER_WORK_EXHAUST ||
	    yield == DP_TIMER_TIME_EXHAUST)
		qdf_timer_mod(&soc->int_timer, 1);
	else
		qdf_timer_mod(&soc->int_timer, DP_INTR_POLL_TIMER_MS);
}

/*
 * dp_soc_attach_poll() - Register handlers for DP interrupts
 * @txrx_soc: DP SOC handle
 *
 * Host driver will register for “DP_NUM_INTERRUPT_CONTEXTS” number of NAPI
 * contexts. Each NAPI context will have a tx_ring_mask , rx_ring_mask ,and
 * rx_monitor_ring mask to indicate the rings that are processed by the handler.
 *
 * Return: 0 for success, nonzero for failure.
 */
static QDF_STATUS dp_soc_attach_poll(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;
	int i;

	soc->intr_mode = DP_INTR_POLL;

	for (i = 0; i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++) {
		soc->intr_ctx[i].dp_intr_id = i;
		soc->intr_ctx[i].tx_ring_mask =
			wlan_cfg_get_tx_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].rx_ring_mask =
			wlan_cfg_get_rx_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].rx_mon_ring_mask =
			wlan_cfg_get_rx_mon_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].rx_err_ring_mask =
			wlan_cfg_get_rx_err_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].rx_wbm_rel_ring_mask =
			wlan_cfg_get_rx_wbm_rel_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].reo_status_ring_mask =
			wlan_cfg_get_reo_status_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].rxdma2host_ring_mask =
			wlan_cfg_get_rxdma2host_ring_mask(soc->wlan_cfg_ctx, i);
		soc->intr_ctx[i].soc = soc;
		soc->intr_ctx[i].lro_ctx = qdf_lro_init();
	}

	qdf_timer_init(soc->osdev, &soc->int_timer,
			dp_interrupt_timer, (void *)soc,
			QDF_TIMER_TYPE_WAKE_APPS);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_soc_set_interrupt_mode() - Set the interrupt mode in soc
 * soc: DP soc handle
 *
 * Set the appropriate interrupt mode flag in the soc
 */
static void dp_soc_set_interrupt_mode(struct dp_soc *soc)
{
	uint32_t msi_base_data, msi_vector_start;
	int msi_vector_count, ret;

	soc->intr_mode = DP_INTR_INTEGRATED;

	if (!(soc->wlan_cfg_ctx->napi_enabled) ||
	    (soc->cdp_soc.ol_ops->get_con_mode &&
	     soc->cdp_soc.ol_ops->get_con_mode() == QDF_GLOBAL_MONITOR_MODE)) {
		soc->intr_mode = DP_INTR_POLL;
	} else {
		ret = pld_get_user_msi_assignment(soc->osdev->dev, "DP",
						  &msi_vector_count,
						  &msi_base_data,
						  &msi_vector_start);
		if (ret)
			return;

		soc->intr_mode = DP_INTR_MSI;
	}
}

static QDF_STATUS dp_soc_interrupt_attach(struct cdp_soc_t *txrx_soc);
#if defined(DP_INTR_POLL_BOTH)
/*
 * dp_soc_interrupt_attach_wrapper() - Register handlers for DP interrupts
 * @txrx_soc: DP SOC handle
 *
 * Call the appropriate attach function based on the mode of operation.
 * This is a WAR for enabling monitor mode.
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS dp_soc_interrupt_attach_wrapper(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	if (!(soc->wlan_cfg_ctx->napi_enabled) ||
	    (soc->cdp_soc.ol_ops->get_con_mode &&
	     soc->cdp_soc.ol_ops->get_con_mode() ==
	     QDF_GLOBAL_MONITOR_MODE)) {
		dp_info("Poll mode");
		return dp_soc_attach_poll(txrx_soc);
	} else {
		dp_info("Interrupt  mode");
		return dp_soc_interrupt_attach(txrx_soc);
	}
}
#else
#if defined(DP_INTR_POLL_BASED) && DP_INTR_POLL_BASED
static QDF_STATUS dp_soc_interrupt_attach_wrapper(struct cdp_soc_t *txrx_soc)
{
	return dp_soc_attach_poll(txrx_soc);
}
#else
static QDF_STATUS dp_soc_interrupt_attach_wrapper(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	if (hif_is_polled_mode_enabled(soc->hif_handle))
		return dp_soc_attach_poll(txrx_soc);
	else
		return dp_soc_interrupt_attach(txrx_soc);
}
#endif
#endif

static void dp_soc_interrupt_map_calculate_integrated(struct dp_soc *soc,
		int intr_ctx_num, int *irq_id_map, int *num_irq_r)
{
	int j;
	int num_irq = 0;

	int tx_mask =
		wlan_cfg_get_tx_ring_mask(soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_mask =
		wlan_cfg_get_rx_ring_mask(soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_mon_mask =
		wlan_cfg_get_rx_mon_ring_mask(soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_err_ring_mask = wlan_cfg_get_rx_err_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_wbm_rel_ring_mask = wlan_cfg_get_rx_wbm_rel_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int reo_status_ring_mask = wlan_cfg_get_reo_status_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rxdma2host_ring_mask = wlan_cfg_get_rxdma2host_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int host2rxdma_ring_mask = wlan_cfg_get_host2rxdma_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int host2rxdma_mon_ring_mask = wlan_cfg_get_host2rxdma_mon_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);

	soc->intr_mode = DP_INTR_INTEGRATED;

	for (j = 0; j < HIF_MAX_GRP_IRQ; j++) {

		if (tx_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				(wbm2host_tx_completions_ring1 - j);
		}

		if (rx_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				(reo2host_destination_ring1 - j);
		}

		if (rxdma2host_ring_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				rxdma2host_destination_ring_mac1 - j;
		}

		if (host2rxdma_ring_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				host2rxdma_host_buf_ring_mac1 -	j;
		}

		if (host2rxdma_mon_ring_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				host2rxdma_monitor_ring1 - j;
		}

		if (rx_mon_mask & (1 << j)) {
			irq_id_map[num_irq++] =
				ppdu_end_interrupts_mac1 - j;
			irq_id_map[num_irq++] =
				rxdma2host_monitor_status_ring_mac1 - j;
		}

		if (rx_wbm_rel_ring_mask & (1 << j))
			irq_id_map[num_irq++] = wbm2host_rx_release;

		if (rx_err_ring_mask & (1 << j))
			irq_id_map[num_irq++] = reo2host_exception;

		if (reo_status_ring_mask & (1 << j))
			irq_id_map[num_irq++] = reo2host_status;

	}
	*num_irq_r = num_irq;
}

static void dp_soc_interrupt_map_calculate_msi(struct dp_soc *soc,
		int intr_ctx_num, int *irq_id_map, int *num_irq_r,
		int msi_vector_count, int msi_vector_start)
{
	int tx_mask = wlan_cfg_get_tx_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_mask = wlan_cfg_get_rx_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_mon_mask = wlan_cfg_get_rx_mon_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_err_ring_mask = wlan_cfg_get_rx_err_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rx_wbm_rel_ring_mask = wlan_cfg_get_rx_wbm_rel_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int reo_status_ring_mask = wlan_cfg_get_reo_status_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);
	int rxdma2host_ring_mask = wlan_cfg_get_rxdma2host_ring_mask(
					soc->wlan_cfg_ctx, intr_ctx_num);

	unsigned int vector =
		(intr_ctx_num % msi_vector_count) + msi_vector_start;
	int num_irq = 0;

	soc->intr_mode = DP_INTR_MSI;

	if (tx_mask | rx_mask | rx_mon_mask | rx_err_ring_mask |
	    rx_wbm_rel_ring_mask | reo_status_ring_mask | rxdma2host_ring_mask)
		irq_id_map[num_irq++] =
			pld_get_msi_irq(soc->osdev->dev, vector);

	*num_irq_r = num_irq;
}

static void dp_soc_interrupt_map_calculate(struct dp_soc *soc, int intr_ctx_num,
				    int *irq_id_map, int *num_irq)
{
	int msi_vector_count, ret;
	uint32_t msi_base_data, msi_vector_start;

	ret = pld_get_user_msi_assignment(soc->osdev->dev, "DP",
					    &msi_vector_count,
					    &msi_base_data,
					    &msi_vector_start);
	if (ret)
		return dp_soc_interrupt_map_calculate_integrated(soc,
				intr_ctx_num, irq_id_map, num_irq);

	else
		dp_soc_interrupt_map_calculate_msi(soc,
				intr_ctx_num, irq_id_map, num_irq,
				msi_vector_count, msi_vector_start);
}

/*
 * dp_soc_interrupt_attach() - Register handlers for DP interrupts
 * @txrx_soc: DP SOC handle
 *
 * Host driver will register for “DP_NUM_INTERRUPT_CONTEXTS” number of NAPI
 * contexts. Each NAPI context will have a tx_ring_mask , rx_ring_mask ,and
 * rx_monitor_ring mask to indicate the rings that are processed by the handler.
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS dp_soc_interrupt_attach(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	int i = 0;
	int num_irq = 0;

	for (i = 0; i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++) {
		int ret = 0;

		/* Map of IRQ ids registered with one interrupt context */
		int irq_id_map[HIF_MAX_GRP_IRQ];

		int tx_mask =
			wlan_cfg_get_tx_ring_mask(soc->wlan_cfg_ctx, i);
		int rx_mask =
			wlan_cfg_get_rx_ring_mask(soc->wlan_cfg_ctx, i);
		int rx_mon_mask =
			dp_soc_get_mon_mask_for_interrupt_mode(soc, i);
		int rx_err_ring_mask =
			wlan_cfg_get_rx_err_ring_mask(soc->wlan_cfg_ctx, i);
		int rx_wbm_rel_ring_mask =
			wlan_cfg_get_rx_wbm_rel_ring_mask(soc->wlan_cfg_ctx, i);
		int reo_status_ring_mask =
			wlan_cfg_get_reo_status_ring_mask(soc->wlan_cfg_ctx, i);
		int rxdma2host_ring_mask =
			wlan_cfg_get_rxdma2host_ring_mask(soc->wlan_cfg_ctx, i);
		int host2rxdma_ring_mask =
			wlan_cfg_get_host2rxdma_ring_mask(soc->wlan_cfg_ctx, i);
		int host2rxdma_mon_ring_mask =
			wlan_cfg_get_host2rxdma_mon_ring_mask(
				soc->wlan_cfg_ctx, i);

		soc->intr_ctx[i].dp_intr_id = i;
		soc->intr_ctx[i].tx_ring_mask = tx_mask;
		soc->intr_ctx[i].rx_ring_mask = rx_mask;
		soc->intr_ctx[i].rx_mon_ring_mask = rx_mon_mask;
		soc->intr_ctx[i].rx_err_ring_mask = rx_err_ring_mask;
		soc->intr_ctx[i].rxdma2host_ring_mask = rxdma2host_ring_mask;
		soc->intr_ctx[i].host2rxdma_ring_mask = host2rxdma_ring_mask;
		soc->intr_ctx[i].rx_wbm_rel_ring_mask = rx_wbm_rel_ring_mask;
		soc->intr_ctx[i].reo_status_ring_mask = reo_status_ring_mask;
		soc->intr_ctx[i].host2rxdma_mon_ring_mask =
			 host2rxdma_mon_ring_mask;

		soc->intr_ctx[i].soc = soc;

		num_irq = 0;

		dp_soc_interrupt_map_calculate(soc, i, &irq_id_map[0],
					       &num_irq);

		ret = hif_register_ext_group(soc->hif_handle,
				num_irq, irq_id_map, dp_service_srngs,
				&soc->intr_ctx[i], "dp_intr",
				HIF_EXEC_NAPI_TYPE, QCA_NAPI_DEF_SCALE_BIN_SHIFT);

		if (ret) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("failed, ret = %d"), ret);

			return QDF_STATUS_E_FAILURE;
		}
		soc->intr_ctx[i].lro_ctx = qdf_lro_init();
	}

	hif_configure_ext_group_interrupts(soc->hif_handle);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_soc_interrupt_detach() - Deregister any allocations done for interrupts
 * @txrx_soc: DP SOC handle
 *
 * Return: none
 */
static void dp_soc_interrupt_detach(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;
	int i;

	if (soc->intr_mode == DP_INTR_POLL) {
		qdf_timer_free(&soc->int_timer);
	} else {
		hif_deconfigure_ext_group_interrupts(soc->hif_handle);
		hif_deregister_exec_group(soc->hif_handle, "dp_intr");
	}

	for (i = 0; i < wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx); i++) {
		soc->intr_ctx[i].tx_ring_mask = 0;
		soc->intr_ctx[i].rx_ring_mask = 0;
		soc->intr_ctx[i].rx_mon_ring_mask = 0;
		soc->intr_ctx[i].rx_err_ring_mask = 0;
		soc->intr_ctx[i].rx_wbm_rel_ring_mask = 0;
		soc->intr_ctx[i].reo_status_ring_mask = 0;
		soc->intr_ctx[i].rxdma2host_ring_mask = 0;
		soc->intr_ctx[i].host2rxdma_ring_mask = 0;
		soc->intr_ctx[i].host2rxdma_mon_ring_mask = 0;

		qdf_lro_deinit(soc->intr_ctx[i].lro_ctx);
	}
}

#define AVG_MAX_MPDUS_PER_TID 128
#define AVG_TIDS_PER_CLIENT 2
#define AVG_FLOWS_PER_TID 2
#define AVG_MSDUS_PER_FLOW 128
#define AVG_MSDUS_PER_MPDU 4

/*
 * Allocate and setup link descriptor pool that will be used by HW for
 * various link and queue descriptors and managed by WBM
 */
static int dp_hw_link_desc_pool_setup(struct dp_soc *soc)
{
	int link_desc_size = hal_get_link_desc_size(soc->hal_soc);
	int link_desc_align = hal_get_link_desc_align(soc->hal_soc);
	uint32_t max_clients = wlan_cfg_get_max_clients(soc->wlan_cfg_ctx);
	uint32_t num_mpdus_per_link_desc =
		hal_num_mpdus_per_link_desc(soc->hal_soc);
	uint32_t num_msdus_per_link_desc =
		hal_num_msdus_per_link_desc(soc->hal_soc);
	uint32_t num_mpdu_links_per_queue_desc =
		hal_num_mpdu_links_per_queue_desc(soc->hal_soc);
	uint32_t max_alloc_size = wlan_cfg_max_alloc_size(soc->wlan_cfg_ctx);
	uint32_t total_link_descs, total_mem_size;
	uint32_t num_mpdu_link_descs, num_mpdu_queue_descs;
	uint32_t num_tx_msdu_link_descs, num_rx_msdu_link_descs;
	uint32_t entry_size, num_entries;
	int i;
	uint32_t cookie = 0;
	qdf_dma_addr_t *baseaddr = NULL;
	uint32_t page_idx = 0;
	struct qdf_mem_multi_page_t *pages;
	struct qdf_mem_dma_page_t *dma_pages;
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t num_descs_per_page;

	/* Only Tx queue descriptors are allocated from common link descriptor
	 * pool Rx queue descriptors are not included in this because (REO queue
	 * extension descriptors) they are expected to be allocated contiguously
	 * with REO queue descriptors
	 */
	num_mpdu_link_descs = (max_clients * AVG_TIDS_PER_CLIENT *
		AVG_MAX_MPDUS_PER_TID) / num_mpdus_per_link_desc;

	num_mpdu_queue_descs = num_mpdu_link_descs /
		num_mpdu_links_per_queue_desc;

	num_tx_msdu_link_descs = (max_clients * AVG_TIDS_PER_CLIENT *
		AVG_FLOWS_PER_TID * AVG_MSDUS_PER_FLOW) /
		num_msdus_per_link_desc;

	num_rx_msdu_link_descs = (max_clients * AVG_TIDS_PER_CLIENT *
		AVG_MAX_MPDUS_PER_TID * AVG_MSDUS_PER_MPDU) / 6;

	num_entries = num_mpdu_link_descs + num_mpdu_queue_descs +
		num_tx_msdu_link_descs + num_rx_msdu_link_descs;

	/* Round up to power of 2 */
	total_link_descs = 1;
	while (total_link_descs < num_entries)
		total_link_descs <<= 1;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO_HIGH,
		FL("total_link_descs: %u, link_desc_size: %d"),
		total_link_descs, link_desc_size);
	total_mem_size =  total_link_descs * link_desc_size;

	total_mem_size += link_desc_align;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO_HIGH,
		FL("total_mem_size: %d"), total_mem_size);

	pages = &soc->link_desc_pages;
	dp_set_max_page_size(pages, max_alloc_size);
	if (!dp_is_soc_reinit(soc)) {
		dp_desc_multi_pages_mem_alloc(soc, DP_HW_LINK_DESC_TYPE,
					      pages,
					      link_desc_size,
					      total_link_descs,
					      0, false);
		if (!pages->num_pages) {
			dp_err("Multi page alloc fail for hw link desc pool");
			goto fail_page_alloc;
		}
		qdf_minidump_log(
			(void *)(pages->dma_pages->page_v_addr_start),
			pages->num_pages *
			sizeof(struct qdf_mem_dma_page_t),
			"hw_link_desc_bank");
	}

	/* Allocate and setup link descriptor idle list for HW internal use */
	entry_size = hal_srng_get_entrysize(soc->hal_soc, WBM_IDLE_LINK);
	total_mem_size = entry_size * total_link_descs;

	if (total_mem_size <= max_alloc_size) {
		void *desc;

		if (dp_srng_setup(soc, &soc->wbm_idle_link_ring,
				  WBM_IDLE_LINK, 0, 0, total_link_descs, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("Link desc idle ring setup failed"));
			goto fail;
		}

		qdf_minidump_log(soc->wbm_idle_link_ring.base_vaddr_unaligned,
				 soc->wbm_idle_link_ring.alloc_size,
				 "wbm_idle_link_ring");

		hal_srng_access_start_unlocked(soc->hal_soc,
			soc->wbm_idle_link_ring.hal_srng);
		page_idx = 0; count = 0;
		offset = 0;
		pages = &soc->link_desc_pages;
		if (pages->dma_pages)
			dma_pages = pages->dma_pages;
		else
			goto fail;
		num_descs_per_page =
			pages->num_element_per_page;
		while ((desc = hal_srng_src_get_next(
				soc->hal_soc,
				soc->wbm_idle_link_ring.hal_srng)) &&
		       (count < total_link_descs)) {
			page_idx = count / num_descs_per_page;
			offset = count % num_descs_per_page;
			cookie = LINK_DESC_COOKIE(count, page_idx);
			hal_set_link_desc_addr(
				desc, cookie,
				dma_pages[page_idx].page_p_addr
				+ (offset * link_desc_size));
			count++;
		}
		hal_srng_access_end_unlocked(soc->hal_soc,
			soc->wbm_idle_link_ring.hal_srng);
	} else {
		uint32_t num_scatter_bufs;
		uint32_t num_entries_per_buf;
		uint32_t rem_entries;
		uint8_t *scatter_buf_ptr;
		uint16_t scatter_buf_num;
		uint32_t buf_size = 0;

		soc->wbm_idle_scatter_buf_size =
			hal_idle_list_scatter_buf_size(soc->hal_soc);
		num_entries_per_buf = hal_idle_scatter_buf_num_entries(
			soc->hal_soc, soc->wbm_idle_scatter_buf_size);
		num_scatter_bufs = hal_idle_list_num_scatter_bufs(
					soc->hal_soc, total_mem_size,
					soc->wbm_idle_scatter_buf_size);

		if (num_scatter_bufs > MAX_IDLE_SCATTER_BUFS) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
					FL("scatter bufs size out of bounds"));
			goto fail;
		}

		for (i = 0; i < num_scatter_bufs; i++) {
			baseaddr = &soc->wbm_idle_scatter_buf_base_paddr[i];
			if (!dp_is_soc_reinit(soc)) {
				buf_size = soc->wbm_idle_scatter_buf_size;
				soc->wbm_idle_scatter_buf_base_vaddr[i] =
					qdf_mem_alloc_consistent(soc->osdev,
								 soc->osdev->
								 dev,
								 buf_size,
								 baseaddr);
			}
			if (!soc->wbm_idle_scatter_buf_base_vaddr[i]) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL("Scatter lst memory alloc fail"));
				goto fail;
			}
		}

		/* Populate idle list scatter buffers with link descriptor
		 * pointers
		 */
		scatter_buf_num = 0;
		scatter_buf_ptr = (uint8_t *)(
			soc->wbm_idle_scatter_buf_base_vaddr[scatter_buf_num]);
		rem_entries = num_entries_per_buf;
		pages = &soc->link_desc_pages;
		page_idx = 0; count = 0;
		offset = 0;
		num_descs_per_page =
			pages->num_element_per_page;
		if (pages->dma_pages)
			dma_pages = pages->dma_pages;
		else
			goto fail;
		while (count < total_link_descs) {
			page_idx = count / num_descs_per_page;
			offset = count % num_descs_per_page;
			cookie = LINK_DESC_COOKIE(count, page_idx);
			hal_set_link_desc_addr(
				(void *)scatter_buf_ptr,
				cookie,
				dma_pages[page_idx].page_p_addr +
				(offset * link_desc_size));
			rem_entries--;
			if (rem_entries) {
				scatter_buf_ptr += entry_size;
			} else {
				rem_entries = num_entries_per_buf;
				scatter_buf_num++;
				if (scatter_buf_num >= num_scatter_bufs) {
					scatter_buf_num--;
					break;
				}

				scatter_buf_ptr = (uint8_t *)
					(soc->wbm_idle_scatter_buf_base_vaddr[
					 scatter_buf_num]);
			}
			count++;
		}
		/* Setup link descriptor idle list in HW */
		hal_setup_link_idle_list(soc->hal_soc,
			soc->wbm_idle_scatter_buf_base_paddr,
			soc->wbm_idle_scatter_buf_base_vaddr,
			num_scatter_bufs, soc->wbm_idle_scatter_buf_size,
			(uint32_t)(scatter_buf_ptr -
			(uint8_t *)(soc->wbm_idle_scatter_buf_base_vaddr[
			scatter_buf_num])), total_link_descs);
	}
	return 0;

fail:
	if (soc->wbm_idle_link_ring.hal_srng) {
		dp_srng_cleanup(soc, &soc->wbm_idle_link_ring,
				WBM_IDLE_LINK, 0);
	}

	for (i = 0; i < MAX_IDLE_SCATTER_BUFS; i++) {
		if (soc->wbm_idle_scatter_buf_base_vaddr[i]) {
			qdf_mem_free_consistent(soc->osdev, soc->osdev->dev,
				soc->wbm_idle_scatter_buf_size,
				soc->wbm_idle_scatter_buf_base_vaddr[i],
				soc->wbm_idle_scatter_buf_base_paddr[i], 0);
			soc->wbm_idle_scatter_buf_base_vaddr[i] = NULL;
		}
	}

	pages = &soc->link_desc_pages;
	qdf_minidump_remove(
		(void *)pages->dma_pages->page_v_addr_start);
	dp_desc_multi_pages_mem_free(soc, DP_HW_LINK_DESC_TYPE,
				     pages, 0, false);
	return QDF_STATUS_E_FAILURE;

fail_page_alloc:
	return QDF_STATUS_E_FAULT;
}

/*
 * Free link descriptor pool that was setup HW
 */
static void dp_hw_link_desc_pool_cleanup(struct dp_soc *soc)
{
	int i;
	struct qdf_mem_multi_page_t *pages;

	if (soc->wbm_idle_link_ring.hal_srng) {
		qdf_minidump_remove(
			soc->wbm_idle_link_ring.base_vaddr_unaligned);
		dp_srng_cleanup(soc, &soc->wbm_idle_link_ring,
			WBM_IDLE_LINK, 0);
	}

	for (i = 0; i < MAX_IDLE_SCATTER_BUFS; i++) {
		if (soc->wbm_idle_scatter_buf_base_vaddr[i]) {
			qdf_mem_free_consistent(soc->osdev, soc->osdev->dev,
				soc->wbm_idle_scatter_buf_size,
				soc->wbm_idle_scatter_buf_base_vaddr[i],
				soc->wbm_idle_scatter_buf_base_paddr[i], 0);
			soc->wbm_idle_scatter_buf_base_vaddr[i] = NULL;
		}
	}

	pages = &soc->link_desc_pages;
	qdf_minidump_remove(
		(void *)pages->dma_pages->page_v_addr_start);
	dp_desc_multi_pages_mem_free(soc, DP_HW_LINK_DESC_TYPE,
				     pages, 0, false);
}

#ifdef IPA_OFFLOAD
#define REO_DST_RING_SIZE_QCA6290 1023
#ifndef CONFIG_WIFI_EMULATION_WIFI_3_0
#define REO_DST_RING_SIZE_QCA8074 1023
#define REO_DST_RING_SIZE_QCN9000 2048
#else
#define REO_DST_RING_SIZE_QCA8074 8
#define REO_DST_RING_SIZE_QCN9000 8
#endif /* CONFIG_WIFI_EMULATION_WIFI_3_0 */

#else

#define REO_DST_RING_SIZE_QCA6290 1024
#ifndef CONFIG_WIFI_EMULATION_WIFI_3_0
#define REO_DST_RING_SIZE_QCA8074 2048
#define REO_DST_RING_SIZE_QCN9000 2048
#else
#define REO_DST_RING_SIZE_QCA8074 8
#define REO_DST_RING_SIZE_QCN9000 8
#endif /* CONFIG_WIFI_EMULATION_WIFI_3_0 */
#endif /* IPA_OFFLOAD */

#ifndef FEATURE_WDS
static void dp_soc_wds_attach(struct dp_soc *soc)
{
}

static void dp_soc_wds_detach(struct dp_soc *soc)
{
}
#endif
/*
 * dp_soc_reset_ring_map() - Reset cpu ring map
 * @soc: Datapath soc handler
 *
 * This api resets the default cpu ring map
 */

static void dp_soc_reset_cpu_ring_map(struct dp_soc *soc)
{
	uint8_t i;
	int nss_config = wlan_cfg_get_dp_soc_nss_cfg(soc->wlan_cfg_ctx);

	for (i = 0; i < WLAN_CFG_INT_NUM_CONTEXTS; i++) {
		switch (nss_config) {
		case dp_nss_cfg_first_radio:
			/*
			 * Setting Tx ring map for one nss offloaded radio
			 */
			soc->tx_ring_map[i] = dp_cpu_ring_map[DP_NSS_FIRST_RADIO_OFFLOADED_MAP][i];
			break;

		case dp_nss_cfg_second_radio:
			/*
			 * Setting Tx ring for two nss offloaded radios
			 */
			soc->tx_ring_map[i] = dp_cpu_ring_map[DP_NSS_SECOND_RADIO_OFFLOADED_MAP][i];
			break;

		case dp_nss_cfg_dbdc:
			/*
			 * Setting Tx ring map for 2 nss offloaded radios
			 */
			soc->tx_ring_map[i] =
				dp_cpu_ring_map[DP_NSS_DBDC_OFFLOADED_MAP][i];
			break;

		case dp_nss_cfg_dbtc:
			/*
			 * Setting Tx ring map for 3 nss offloaded radios
			 */
			soc->tx_ring_map[i] =
				dp_cpu_ring_map[DP_NSS_DBTC_OFFLOADED_MAP][i];
			break;

		default:
			dp_err("tx_ring_map failed due to invalid nss cfg");
			break;
		}
	}
}

/*
 * dp_soc_ring_if_nss_offloaded() - find if ring is offloaded to NSS
 * @dp_soc - DP soc handle
 * @ring_type - ring type
 * @ring_num - ring_num
 *
 * return 0 or 1
 */
static uint8_t dp_soc_ring_if_nss_offloaded(struct dp_soc *soc, enum hal_ring_type ring_type, int ring_num)
{
	uint8_t nss_config = wlan_cfg_get_dp_soc_nss_cfg(soc->wlan_cfg_ctx);
	uint8_t status = 0;

	switch (ring_type) {
	case WBM2SW_RELEASE:
	case REO_DST:
	case RXDMA_BUF:
		status = ((nss_config) & (1 << ring_num));
		break;
	default:
		break;
	}

	return status;
}

/*
 * dp_soc_disable_mac2_intr_mask() - reset interrupt mask for WMAC2 hw rings
 * @dp_soc - DP Soc handle
 *
 * Return: Return void
 */
static void dp_soc_disable_mac2_intr_mask(struct dp_soc *soc)
{
	int *grp_mask = NULL;
	int group_number;

	grp_mask = &soc->wlan_cfg_ctx->int_host2rxdma_ring_mask[0];
	group_number = dp_srng_find_ring_in_mask(0x2, grp_mask);
	wlan_cfg_set_host2rxdma_ring_mask(soc->wlan_cfg_ctx,
					  group_number, 0x0);

	grp_mask = &soc->wlan_cfg_ctx->int_rx_mon_ring_mask[0];
	group_number = dp_srng_find_ring_in_mask(0x2, grp_mask);
	wlan_cfg_set_rx_mon_ring_mask(soc->wlan_cfg_ctx,
				      group_number, 0x0);

	grp_mask = &soc->wlan_cfg_ctx->int_rxdma2host_ring_mask[0];
	group_number = dp_srng_find_ring_in_mask(0x2, grp_mask);
	wlan_cfg_set_rxdma2host_ring_mask(soc->wlan_cfg_ctx,
					  group_number, 0x0);

	grp_mask = &soc->wlan_cfg_ctx->int_host2rxdma_mon_ring_mask[0];
	group_number = dp_srng_find_ring_in_mask(0x2, grp_mask);
	wlan_cfg_set_host2rxdma_mon_ring_mask(soc->wlan_cfg_ctx,
					      group_number, 0x0);
}

/*
 * dp_soc_reset_intr_mask() - reset interrupt mask
 * @dp_soc - DP Soc handle
 *
 * Return: Return void
 */
static void dp_soc_reset_intr_mask(struct dp_soc *soc)
{
	uint8_t j;
	int *grp_mask = NULL;
	int group_number, mask, num_ring;

	/* number of tx ring */
	num_ring = wlan_cfg_num_tcl_data_rings(soc->wlan_cfg_ctx);

	/*
	 * group mask for tx completion  ring.
	 */
	grp_mask =  &soc->wlan_cfg_ctx->int_tx_ring_mask[0];

	/* loop and reset the mask for only offloaded ring */
	for (j = 0; j < num_ring; j++) {
		if (!dp_soc_ring_if_nss_offloaded(soc, WBM2SW_RELEASE, j)) {
			continue;
		}

		/*
		 * Group number corresponding to tx offloaded ring.
		 */
		group_number = dp_srng_find_ring_in_mask(j, grp_mask);
		if (group_number < 0) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
					FL("ring not part of any group; ring_type: %d,ring_num %d"),
					WBM2SW_RELEASE, j);
			return;
		}

		/* reset the tx mask for offloaded ring */
		mask = wlan_cfg_get_tx_ring_mask(soc->wlan_cfg_ctx, group_number);
		mask &= (~(1 << j));

		/*
		 * reset the interrupt mask for offloaded ring.
		 */
		wlan_cfg_set_tx_ring_mask(soc->wlan_cfg_ctx, group_number, mask);
	}

	/* number of rx rings */
	num_ring = wlan_cfg_num_reo_dest_rings(soc->wlan_cfg_ctx);

	/*
	 * group mask for reo destination ring.
	 */
	grp_mask = &soc->wlan_cfg_ctx->int_rx_ring_mask[0];

	/* loop and reset the mask for only offloaded ring */
	for (j = 0; j < num_ring; j++) {
		if (!dp_soc_ring_if_nss_offloaded(soc, REO_DST, j)) {
			continue;
		}

		/*
		 * Group number corresponding to rx offloaded ring.
		 */
		group_number = dp_srng_find_ring_in_mask(j, grp_mask);
		if (group_number < 0) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
					FL("ring not part of any group; ring_type: %d,ring_num %d"),
					REO_DST, j);
			return;
		}

		/* set the interrupt mask for offloaded ring */
		mask =  wlan_cfg_get_rx_ring_mask(soc->wlan_cfg_ctx, group_number);
		mask &= (~(1 << j));

		/*
		 * set the interrupt mask to zero for rx offloaded radio.
		 */
		wlan_cfg_set_rx_ring_mask(soc->wlan_cfg_ctx, group_number, mask);
	}

	/*
	 * group mask for Rx buffer refill ring
	 */
	grp_mask = &soc->wlan_cfg_ctx->int_host2rxdma_ring_mask[0];

	/* loop and reset the mask for only offloaded ring */
	for (j = 0; j < MAX_PDEV_CNT; j++) {
		int lmac_id = wlan_cfg_get_hw_mac_idx(soc->wlan_cfg_ctx, j);

		if (!dp_soc_ring_if_nss_offloaded(soc, RXDMA_BUF, j)) {
			continue;
		}

		/*
		 * Group number corresponding to rx offloaded ring.
		 */
		group_number = dp_srng_find_ring_in_mask(lmac_id, grp_mask);
		if (group_number < 0) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
					FL("ring not part of any group; ring_type: %d,ring_num %d"),
					REO_DST, lmac_id);
			return;
		}

		/* set the interrupt mask for offloaded ring */
		mask =  wlan_cfg_get_host2rxdma_ring_mask(soc->wlan_cfg_ctx,
				group_number);
		mask &= (~(1 << lmac_id));

		/*
		 * set the interrupt mask to zero for rx offloaded radio.
		 */
		wlan_cfg_set_host2rxdma_ring_mask(soc->wlan_cfg_ctx,
			group_number, mask);
	}
}

#ifdef IPA_OFFLOAD
/**
 * dp_reo_remap_config() - configure reo remap register value based
 *                         nss configuration.
 *		based on offload_radio value below remap configuration
 *		get applied.
 *		0 - both Radios handled by host (remap rings 1, 2, 3 & 4)
 *		1 - 1st Radio handled by NSS (remap rings 2, 3 & 4)
 *		2 - 2nd Radio handled by NSS (remap rings 1, 2 & 4)
 *		3 - both Radios handled by NSS (remap not required)
 *		4 - IPA OFFLOAD enabled (remap rings 1,2 & 3)
 *
 * @remap1: output parameter indicates reo remap 1 register value
 * @remap2: output parameter indicates reo remap 2 register value
 * Return: bool type, true if remap is configured else false.
 */
bool dp_reo_remap_config(struct dp_soc *soc, uint32_t *remap1, uint32_t *remap2)
{
	*remap1 = HAL_REO_REMAP_IX2(REO_REMAP_SW1, 16) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 17) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 18) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW1, 19) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 20) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 21) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW1, 22) |
		  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 23);

	*remap2 = HAL_REO_REMAP_IX3(REO_REMAP_SW3, 24) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 25) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 26) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 27) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 28) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 29) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 30) |
		  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 31);

	dp_debug("remap1 %x remap2 %x", *remap1, *remap2);

	return true;
}

/**
 * dp_ipa_get_tx_ring_size() - Get Tx ring size for IPA
 *
 * @tx_ring_num: Tx ring number
 * @tx_ipa_ring_sz: Return param only updated for IPA.
 *
 * Return: None
 */
static void dp_ipa_get_tx_ring_size(int tx_ring_num, int *tx_ipa_ring_sz)
{
	if (tx_ring_num == WLAN_CFG_IPA_TX_N_TXCMPL_RING)
		*tx_ipa_ring_sz = WLAN_CFG_IPA_TX_RING_SIZE;
}

/**
 * dp_ipa_get_tx_comp_ring_size() - Get Tx comp ring size for IPA
 *
 * @tx_comp_ring_num: Tx comp ring number
 * @tx_comp_ipa_ring_sz: Return param only updated for IPA.
 *
 * Return: None
 */
static void dp_ipa_get_tx_comp_ring_size(int tx_comp_ring_num,
					 int *tx_comp_ipa_ring_sz)
{
	if (tx_comp_ring_num == WLAN_CFG_IPA_TX_N_TXCMPL_RING)
		*tx_comp_ipa_ring_sz = WLAN_CFG_IPA_TX_COMP_RING_SIZE;
}
#else
static bool dp_reo_remap_config(struct dp_soc *soc,
				uint32_t *remap1,
				uint32_t *remap2)
{
	uint8_t offload_radio = wlan_cfg_get_dp_soc_nss_cfg(soc->wlan_cfg_ctx);
	uint8_t target_type;

	target_type = hal_get_target_type(soc->hal_soc);

	switch (offload_radio) {
	case dp_nss_cfg_default:
		*remap1 = HAL_REO_REMAP_IX2(REO_REMAP_SW1, 16) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 17) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 18) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 19) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW1, 20) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 21) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 22) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 23);

		*remap2 = HAL_REO_REMAP_IX3(REO_REMAP_SW1, 24) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 25) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 26) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 27) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 28) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 29) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 30) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 31);
		break;
	case dp_nss_cfg_first_radio:
		*remap1 = HAL_REO_REMAP_IX2(REO_REMAP_SW2, 16) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 17) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 18) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 19) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 20) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 21) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW2, 22) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 23);

		*remap2 = HAL_REO_REMAP_IX3(REO_REMAP_SW4, 24) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 25) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 26) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 27) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 28) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 29) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 30) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW2, 31);
		break;
	case dp_nss_cfg_second_radio:
		*remap1 = HAL_REO_REMAP_IX2(REO_REMAP_SW1, 16) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 17) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 18) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW1, 19) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 20) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW4, 21) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW1, 22) |
			  HAL_REO_REMAP_IX2(REO_REMAP_SW3, 23);

		*remap2 = HAL_REO_REMAP_IX3(REO_REMAP_SW4, 24) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 25) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 26) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 27) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 28) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW3, 29) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW4, 30) |
			  HAL_REO_REMAP_IX3(REO_REMAP_SW1, 31);

		break;
	case dp_nss_cfg_dbdc:
	case dp_nss_cfg_dbtc:
		/* return false if both or all are offloaded to NSS */
		return false;
	}

	dp_debug("remap1 %x remap2 %x offload_radio %u",
		 *remap1, *remap2, offload_radio);
	return true;
}

static void dp_ipa_get_tx_ring_size(int ring_num, int *tx_ipa_ring_sz)
{
}

static void dp_ipa_get_tx_comp_ring_size(int tx_comp_ring_num,
					 int *tx_comp_ipa_ring_sz)
{
}
#endif /* IPA_OFFLOAD */

/*
 * dp_reo_frag_dst_set() - configure reo register to set the
 *                        fragment destination ring
 * @soc : Datapath soc
 * @frag_dst_ring : output parameter to set fragment destination ring
 *
 * Based on offload_radio below fragment destination rings is selected
 * 0 - TCL
 * 1 - SW1
 * 2 - SW2
 * 3 - SW3
 * 4 - SW4
 * 5 - Release
 * 6 - FW
 * 7 - alternate select
 *
 * return: void
 */
static void dp_reo_frag_dst_set(struct dp_soc *soc, uint8_t *frag_dst_ring)
{
	uint8_t offload_radio = wlan_cfg_get_dp_soc_nss_cfg(soc->wlan_cfg_ctx);

	switch (offload_radio) {
	case dp_nss_cfg_default:
		*frag_dst_ring = REO_REMAP_TCL;
		break;
	case dp_nss_cfg_first_radio:
		/*
		 * This configuration is valid for single band radio which
		 * is also NSS offload.
		 */
	case dp_nss_cfg_dbdc:
	case dp_nss_cfg_dbtc:
		*frag_dst_ring = HAL_SRNG_REO_ALTERNATE_SELECT;
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("dp_reo_frag_dst_set invalid offload radio config"));
		break;
	}
}

#ifdef ENABLE_VERBOSE_DEBUG
static void dp_enable_verbose_debug(struct dp_soc *soc)
{
	struct wlan_cfg_dp_soc_ctxt *soc_cfg_ctx;

	soc_cfg_ctx = soc->wlan_cfg_ctx;

	if (soc_cfg_ctx->per_pkt_trace & dp_verbose_debug_mask)
		is_dp_verbose_debug_enabled = true;

	if (soc_cfg_ctx->per_pkt_trace & hal_verbose_debug_mask)
		hal_set_verbose_debug(true);
	else
		hal_set_verbose_debug(false);
}
#else
static void dp_enable_verbose_debug(struct dp_soc *soc)
{
}
#endif

#ifdef WLAN_FEATURE_STATS_EXT
static inline void dp_create_ext_stats_event(struct dp_soc *soc)
{
	qdf_event_create(&soc->rx_hw_stats_event);
}
#else
static inline void dp_create_ext_stats_event(struct dp_soc *soc)
{
}
#endif

/*
 * dp_soc_cmn_setup() - Common SoC level initializion
 * @soc:		Datapath SOC handle
 *
 * This is an internal function used to setup common SOC data structures,
 * to be called from PDEV attach after receiving HW mode capabilities from FW
 */
static int dp_soc_cmn_setup(struct dp_soc *soc)
{
	int i, cached;
	struct hal_reo_params reo_params;
	int tx_ring_size;
	int tx_comp_ring_size;
	int reo_dst_ring_size;
	uint32_t entries;
	struct wlan_cfg_dp_soc_ctxt *soc_cfg_ctx;

	if (qdf_atomic_read(&soc->cmn_init_done))
		return 0;

	if (dp_hw_link_desc_pool_setup(soc))
		goto fail1;

	soc_cfg_ctx = soc->wlan_cfg_ctx;

	dp_enable_verbose_debug(soc);

	/* Setup SRNG rings */
	/* Common rings */
	entries = wlan_cfg_get_dp_soc_wbm_release_ring_size(soc_cfg_ctx);

	if (dp_srng_setup(soc, &soc->wbm_desc_rel_ring, SW2WBM_RELEASE, 0, 0,
			  entries, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed for wbm_desc_rel_ring"));
		goto fail1;
	}

	qdf_minidump_log(soc->wbm_desc_rel_ring.base_vaddr_unaligned,
			 soc->wbm_desc_rel_ring.alloc_size,
			 "wbm_desc_rel_ring");

	soc->num_tcl_data_rings = 0;
	/* Tx data rings */
	if (!wlan_cfg_per_pdev_tx_ring(soc_cfg_ctx)) {
		soc->num_tcl_data_rings =
			wlan_cfg_num_tcl_data_rings(soc_cfg_ctx);
		tx_comp_ring_size =
			wlan_cfg_tx_comp_ring_size(soc_cfg_ctx);
		tx_ring_size =
			wlan_cfg_tx_ring_size(soc_cfg_ctx);
		for (i = 0; i < soc->num_tcl_data_rings; i++) {
			dp_ipa_get_tx_ring_size(i, &tx_ring_size);

			if (dp_srng_setup(soc, &soc->tcl_data_ring[i],
					  TCL_DATA, i, 0, tx_ring_size, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					QDF_TRACE_LEVEL_ERROR,
					FL("dp_srng_setup failed for tcl_data_ring[%d]"), i);
				goto fail1;
			}

			/* Disable cached desc if NSS offload is enabled */
			cached = WLAN_CFG_DST_RING_CACHED_DESC;
			if (wlan_cfg_get_dp_soc_nss_cfg(soc_cfg_ctx))
				cached = 0;

			dp_ipa_get_tx_comp_ring_size(i, &tx_comp_ring_size);
			/*
			 * TBD: Set IPA WBM ring size with ini IPA UC tx buffer
			 * count
			 */
			if (dp_srng_setup(soc, &soc->tx_comp_ring[i],
					  WBM2SW_RELEASE, i, 0,
					  tx_comp_ring_size,
					  cached)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					QDF_TRACE_LEVEL_ERROR,
					FL("dp_srng_setup failed for tx_comp_ring[%d]"), i);
				goto fail1;
			}
		}
	} else {
		/* This will be incremented during per pdev ring setup */
		soc->num_tcl_data_rings = 0;
	}

	if (dp_tx_soc_attach(soc)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("dp_tx_soc_attach failed"));
		goto fail1;
	}

	entries = wlan_cfg_get_dp_soc_tcl_cmd_ring_size(soc_cfg_ctx);
	/* TCL command and status rings */
	if (dp_srng_setup(soc, &soc->tcl_cmd_ring, TCL_CMD, 0, 0,
			  entries, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed for tcl_cmd_ring"));
		goto fail2;
	}

	entries = wlan_cfg_get_dp_soc_tcl_status_ring_size(soc_cfg_ctx);
	if (dp_srng_setup(soc, &soc->tcl_status_ring, TCL_STATUS, 0, 0,
			  entries, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed for tcl_status_ring"));
		goto fail2;
	}

	reo_dst_ring_size = wlan_cfg_get_reo_dst_ring_size(soc->wlan_cfg_ctx);

	/* TBD: call dp_tx_init to setup Tx SW descriptors and MSDU extension
	 * descriptors
	 */

	/* Rx data rings */
	if (!wlan_cfg_per_pdev_rx_ring(soc_cfg_ctx)) {
		soc->num_reo_dest_rings =
			wlan_cfg_num_reo_dest_rings(soc_cfg_ctx);
		QDF_TRACE(QDF_MODULE_ID_DP,
			QDF_TRACE_LEVEL_INFO,
			FL("num_reo_dest_rings %d"), soc->num_reo_dest_rings);

		/* Disable cached desc if NSS offload is enabled */
		cached = WLAN_CFG_DST_RING_CACHED_DESC;
		if (wlan_cfg_get_dp_soc_nss_cfg(soc_cfg_ctx))
			cached = 0;

		for (i = 0; i < soc->num_reo_dest_rings; i++) {
			if (dp_srng_setup(soc, &soc->reo_dest_ring[i], REO_DST,
					  i, 0, reo_dst_ring_size, cached)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "reo_dest_ring [%d]"), i);
				goto fail2;
			}
		}
	} else {
		/* This will be incremented during per pdev ring setup */
		soc->num_reo_dest_rings = 0;
	}

	entries = wlan_cfg_get_dp_soc_rxdma_err_dst_ring_size(soc_cfg_ctx);
	/* LMAC RxDMA to SW Rings configuration */
	if (!wlan_cfg_per_pdev_lmac_ring(soc_cfg_ctx)) {

		for (i = 0; i < MAX_RX_MAC_RINGS; i++) {
			if (dp_srng_setup(soc, &soc->rxdma_err_dst_ring[i],
					  RXDMA_DST, 0, i, entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_err_dst_ring"));
				goto fail2;
			}
		}
	}
	/* TBD: call dp_rx_init to setup Rx SW descriptors */

	/* REO reinjection ring */
	entries = wlan_cfg_get_dp_soc_reo_reinject_ring_size(soc_cfg_ctx);
	if (dp_srng_setup(soc, &soc->reo_reinject_ring, REO_REINJECT, 0, 0,
			  entries, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("dp_srng_setup failed for reo_reinject_ring"));
		goto fail2;
	}


	/* Rx release ring */
	if (dp_srng_setup(soc, &soc->rx_rel_ring, WBM2SW_RELEASE, 3, 0,
			  wlan_cfg_get_dp_soc_rx_release_ring_size(soc_cfg_ctx),
			  0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("dp_srng_setup failed for rx_rel_ring"));
		goto fail2;
	}


	/* Rx exception ring */
	entries = wlan_cfg_get_dp_soc_reo_exception_ring_size(soc_cfg_ctx);
	if (dp_srng_setup(soc, &soc->reo_exception_ring,
			  REO_EXCEPTION, 0, MAX_REO_DEST_RINGS, entries, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("dp_srng_setup failed for reo_exception_ring"));
		goto fail2;
	}


	/* REO command and status rings */
	if (dp_srng_setup(soc, &soc->reo_cmd_ring, REO_CMD, 0, 0,
			  wlan_cfg_get_dp_soc_reo_cmd_ring_size(soc_cfg_ctx),
			  0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed for reo_cmd_ring"));
		goto fail2;
	}

	hal_reo_init_cmd_ring(soc->hal_soc, soc->reo_cmd_ring.hal_srng);
	TAILQ_INIT(&soc->rx.reo_cmd_list);
	qdf_spinlock_create(&soc->rx.reo_cmd_lock);

	if (dp_srng_setup(soc, &soc->reo_status_ring, REO_STATUS, 0, 0,
			  wlan_cfg_get_dp_soc_reo_status_ring_size(soc_cfg_ctx),
			  0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed for reo_status_ring"));
		goto fail2;
	}

	/*
	 * Skip registering hw ring interrupts for WMAC2 on IPQ6018
	 * WMAC2 is not there in IPQ6018 platform.
	 */
	if (hal_get_target_type(soc->hal_soc) == TARGET_TYPE_QCA6018) {
		dp_soc_disable_mac2_intr_mask(soc);
	}

	/* Reset the cpu ring map if radio is NSS offloaded */
	if (wlan_cfg_get_dp_soc_nss_cfg(soc_cfg_ctx)) {
		dp_soc_reset_cpu_ring_map(soc);
		dp_soc_reset_intr_mask(soc);
	}

	/* Setup HW REO */
	qdf_mem_zero(&reo_params, sizeof(reo_params));

	if (wlan_cfg_is_rx_hash_enabled(soc_cfg_ctx)) {

		/*
		 * Reo ring remap is not required if both radios
		 * are offloaded to NSS
		 */
		if (!dp_reo_remap_config(soc,
					&reo_params.remap1,
					&reo_params.remap2))
			goto out;

		reo_params.rx_hash_enabled = true;
	}

	/* setup the global rx defrag waitlist */
	TAILQ_INIT(&soc->rx.defrag.waitlist);
	soc->rx.defrag.timeout_ms =
		wlan_cfg_get_rx_defrag_min_timeout(soc_cfg_ctx);
	soc->rx.defrag.next_flush_ms = 0;
	soc->rx.flags.defrag_timeout_check =
		wlan_cfg_get_defrag_timeout_check(soc_cfg_ctx);
	qdf_spinlock_create(&soc->rx.defrag.defrag_lock);

	dp_create_ext_stats_event(soc);
out:
	/*
	 * set the fragment destination ring
	 */
	dp_reo_frag_dst_set(soc, &reo_params.frag_dst_ring);

	hal_reo_setup(soc->hal_soc, &reo_params);

	hal_reo_set_err_dst_remap(soc->hal_soc);

	qdf_atomic_set(&soc->cmn_init_done, 1);

	dp_soc_wds_attach(soc);

	qdf_nbuf_queue_init(&soc->htt_stats.msg);
	return 0;
fail2:
	dp_tx_soc_detach(soc);
fail1:
	/*
	 * Cleanup will be done as part of soc_detach, which will
	 * be called on pdev attach failure
	 */
	return QDF_STATUS_E_FAILURE;
}

/*
 * dp_soc_cmn_cleanup() - Common SoC level De-initializion
 *
 * @soc: Datapath SOC handle
 *
 * This function is responsible for cleaning up DP resource of Soc
 * initialled in dp_pdev_attach_wifi3-->dp_soc_cmn_setup, since
 * dp_soc_detach_wifi3 could not identify some of them
 * whether they have done initialized or not accurately.
 *
 */
static void dp_soc_cmn_cleanup(struct dp_soc *soc)
{
	if (!dp_is_soc_reinit(soc)) {
		dp_tx_soc_detach(soc);
	}

	qdf_spinlock_destroy(&soc->rx.defrag.defrag_lock);
	qdf_spinlock_destroy(&soc->rx.reo_cmd_lock);
}

static QDF_STATUS
dp_pdev_detach_wifi3(struct cdp_soc_t *psoc, uint8_t pdev_id,
		     int force);

static QDF_STATUS dp_lro_hash_setup(struct dp_soc *soc, struct dp_pdev *pdev)
{
	struct cdp_lro_hash_config lro_hash;
	QDF_STATUS status;

	if (!wlan_cfg_is_lro_enabled(soc->wlan_cfg_ctx) &&
	    !wlan_cfg_is_gro_enabled(soc->wlan_cfg_ctx) &&
	    !wlan_cfg_is_rx_hash_enabled(soc->wlan_cfg_ctx)) {
		dp_err("LRO, GRO and RX hash disabled");
		return QDF_STATUS_E_FAILURE;
	}

	qdf_mem_zero(&lro_hash, sizeof(lro_hash));

	if (wlan_cfg_is_lro_enabled(soc->wlan_cfg_ctx) ||
	    wlan_cfg_is_gro_enabled(soc->wlan_cfg_ctx)) {
		lro_hash.lro_enable = 1;
		lro_hash.tcp_flag = QDF_TCPHDR_ACK;
		lro_hash.tcp_flag_mask = QDF_TCPHDR_FIN | QDF_TCPHDR_SYN |
			 QDF_TCPHDR_RST | QDF_TCPHDR_ACK | QDF_TCPHDR_URG |
			 QDF_TCPHDR_ECE | QDF_TCPHDR_CWR;
	}

	qdf_get_random_bytes(lro_hash.toeplitz_hash_ipv4,
		 (sizeof(lro_hash.toeplitz_hash_ipv4[0]) *
		 LRO_IPV4_SEED_ARR_SZ));
	qdf_get_random_bytes(lro_hash.toeplitz_hash_ipv6,
		 (sizeof(lro_hash.toeplitz_hash_ipv6[0]) *
		 LRO_IPV6_SEED_ARR_SZ));

	qdf_assert(soc->cdp_soc.ol_ops->lro_hash_config);

	if (!soc->cdp_soc.ol_ops->lro_hash_config) {
		QDF_BUG(0);
		dp_err("lro_hash_config not configured");
		return QDF_STATUS_E_FAILURE;
	}

	status = soc->cdp_soc.ol_ops->lro_hash_config(soc->ctrl_psoc,
						      pdev->pdev_id,
						      &lro_hash);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		dp_err("failed to send lro_hash_config to FW %u", status);
		return status;
	}

	dp_info("LRO CMD config: lro_enable: 0x%x tcp_flag 0x%x tcp_flag_mask 0x%x",
		lro_hash.lro_enable, lro_hash.tcp_flag,
		lro_hash.tcp_flag_mask);

	dp_info("toeplitz_hash_ipv4:");
	qdf_trace_hex_dump(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			   lro_hash.toeplitz_hash_ipv4,
			   (sizeof(lro_hash.toeplitz_hash_ipv4[0]) *
			   LRO_IPV4_SEED_ARR_SZ));

	dp_info("toeplitz_hash_ipv6:");
	qdf_trace_hex_dump(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			   lro_hash.toeplitz_hash_ipv6,
			   (sizeof(lro_hash.toeplitz_hash_ipv6[0]) *
			   LRO_IPV6_SEED_ARR_SZ));

	return status;
}

/*
* dp_rxdma_ring_setup() - configure the RX DMA rings
* @soc: data path SoC handle
* @pdev: Physical device handle
*
* Return: 0 - success, > 0 - failure
*/
#ifdef QCA_HOST2FW_RXBUF_RING
static int dp_rxdma_ring_setup(struct dp_soc *soc,
	 struct dp_pdev *pdev)
{
	struct wlan_cfg_dp_pdev_ctxt *pdev_cfg_ctx;
	int max_mac_rings;
	int i;
	int ring_size;

	pdev_cfg_ctx = pdev->wlan_cfg_ctx;
	max_mac_rings = wlan_cfg_get_num_mac_rings(pdev_cfg_ctx);
	ring_size =  wlan_cfg_get_rx_dma_buf_ring_size(pdev_cfg_ctx);

	for (i = 0; i < max_mac_rings; i++) {
		dp_verbose_debug("pdev_id %d mac_id %d", pdev->pdev_id, i);
		if (dp_srng_setup(soc, &pdev->rx_mac_buf_ring[i],
				  RXDMA_BUF, 1, i, ring_size, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP,
				 QDF_TRACE_LEVEL_ERROR,
				 FL("failed rx mac ring setup"));
			return QDF_STATUS_E_FAILURE;
		}
	}
	return QDF_STATUS_SUCCESS;
}
#else
static int dp_rxdma_ring_setup(struct dp_soc *soc,
	 struct dp_pdev *pdev)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * dp_dscp_tid_map_setup(): Initialize the dscp-tid maps
 * @pdev - DP_PDEV handle
 *
 * Return: void
 */
static inline void
dp_dscp_tid_map_setup(struct dp_pdev *pdev)
{
	uint8_t map_id;
	struct dp_soc *soc = pdev->soc;

	if (!soc)
		return;

	for (map_id = 0; map_id < DP_MAX_TID_MAPS; map_id++) {
		qdf_mem_copy(pdev->dscp_tid_map[map_id],
			     default_dscp_tid_map,
			     sizeof(default_dscp_tid_map));
	}

	for (map_id = 0; map_id < soc->num_hw_dscp_tid_map; map_id++) {
		hal_tx_set_dscp_tid_map(soc->hal_soc,
					default_dscp_tid_map,
					map_id);
	}
}

/**
 * dp_pcp_tid_map_setup(): Initialize the pcp-tid maps
 * @pdev - DP_PDEV handle
 *
 * Return: void
 */
static inline void
dp_pcp_tid_map_setup(struct dp_pdev *pdev)
{
	struct dp_soc *soc = pdev->soc;

	if (!soc)
		return;

	qdf_mem_copy(soc->pcp_tid_map, default_pcp_tid_map,
		     sizeof(default_pcp_tid_map));
	hal_tx_set_pcp_tid_map_default(soc->hal_soc, default_pcp_tid_map);
}

#ifdef IPA_OFFLOAD
/**
 * dp_setup_ipa_rx_refill_buf_ring - Setup second Rx refill buffer ring
 * @soc: data path instance
 * @pdev: core txrx pdev context
 *
 * Return: QDF_STATUS_SUCCESS: success
 *         QDF_STATUS_E_RESOURCES: Error return
 */
static int dp_setup_ipa_rx_refill_buf_ring(struct dp_soc *soc,
					   struct dp_pdev *pdev)
{
	struct wlan_cfg_dp_soc_ctxt *soc_cfg_ctx;
	int entries;

	soc_cfg_ctx = soc->wlan_cfg_ctx;
	entries = wlan_cfg_get_dp_soc_rxdma_refill_ring_size(soc_cfg_ctx);

	/* Setup second Rx refill buffer ring */
	if (dp_srng_setup(soc, &pdev->rx_refill_buf_ring2, RXDMA_BUF,
			  IPA_RX_REFILL_BUF_RING_IDX, pdev->pdev_id, entries, 0)
	   ) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_srng_setup failed second rx refill ring"));
		return QDF_STATUS_E_FAILURE;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_cleanup_ipa_rx_refill_buf_ring - Cleanup second Rx refill buffer ring
 * @soc: data path instance
 * @pdev: core txrx pdev context
 *
 * Return: void
 */
static void dp_cleanup_ipa_rx_refill_buf_ring(struct dp_soc *soc,
					      struct dp_pdev *pdev)
{
	dp_srng_cleanup(soc, &pdev->rx_refill_buf_ring2, RXDMA_BUF,
			IPA_RX_REFILL_BUF_RING_IDX);
}

#else
static int dp_setup_ipa_rx_refill_buf_ring(struct dp_soc *soc,
					   struct dp_pdev *pdev)
{
	return QDF_STATUS_SUCCESS;
}

static void dp_cleanup_ipa_rx_refill_buf_ring(struct dp_soc *soc,
					      struct dp_pdev *pdev)
{
}
#endif

#if !defined(DISABLE_MON_CONFIG)
/**
 * dp_mon_rings_setup() - Initialize Monitor rings based on target
 * @soc: soc handle
 * @pdev: physical device handle
 *
 * Return: nonzero on failure and zero on success
 */
static
QDF_STATUS dp_mon_rings_setup(struct dp_soc *soc, struct dp_pdev *pdev)
{
	int mac_id = 0;
	int pdev_id = pdev->pdev_id;
	int entries;
	struct wlan_cfg_dp_pdev_ctxt *pdev_cfg_ctx;

	pdev_cfg_ctx = pdev->wlan_cfg_ctx;

	for (mac_id = 0; mac_id < NUM_RXDMA_RINGS_PER_PDEV; mac_id++) {
		int lmac_id = dp_get_lmac_id_for_pdev_id(soc, mac_id, pdev_id);

		if (soc->wlan_cfg_ctx->rxdma1_enable) {
			entries =
			   wlan_cfg_get_dma_mon_buf_ring_size(pdev_cfg_ctx);
			if (dp_srng_setup(soc,
					  &soc->rxdma_mon_buf_ring[lmac_id],
					  RXDMA_MONITOR_BUF, 0,	lmac_id,
					  entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_mon_buf_ring "));
				return QDF_STATUS_E_NOMEM;
			}

			entries =
			   wlan_cfg_get_dma_mon_dest_ring_size(pdev_cfg_ctx);
			if (dp_srng_setup(soc,
					  &soc->rxdma_mon_dst_ring[lmac_id],
					  RXDMA_MONITOR_DST, 0, lmac_id,
					  entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_mon_dst_ring"));
				return QDF_STATUS_E_NOMEM;
			}

			entries =
			    wlan_cfg_get_dma_mon_stat_ring_size(pdev_cfg_ctx);
			if (dp_srng_setup(soc,
					  &soc->rxdma_mon_status_ring[lmac_id],
					  RXDMA_MONITOR_STATUS, 0, lmac_id,
					  entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_mon_status_ring"));
				return QDF_STATUS_E_NOMEM;
			}

			entries =
			   wlan_cfg_get_dma_mon_desc_ring_size(pdev_cfg_ctx);
			if (dp_srng_setup(soc,
				  &soc->rxdma_mon_desc_ring[lmac_id],
					  RXDMA_MONITOR_DESC, 0, lmac_id,
					  entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_mon_desc_ring"));
				return QDF_STATUS_E_NOMEM;
			}
		} else {
			entries =
			   wlan_cfg_get_dma_mon_stat_ring_size(pdev_cfg_ctx);
			if (dp_srng_setup(soc,
					  &soc->rxdma_mon_status_ring[lmac_id],
					  RXDMA_MONITOR_STATUS, 0, lmac_id,
					  entries, 0)) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL(RNG_ERR "rxdma_mon_status_ring"));
				return QDF_STATUS_E_NOMEM;
			}
		}
	}

	return QDF_STATUS_SUCCESS;
}
#else
static
QDF_STATUS dp_mon_rings_setup(struct dp_soc *soc, struct dp_pdev *pdev)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/*dp_iterate_update_peer_list - update peer stats on cal client timer
 * @pdev_hdl: pdev handle
 */
#ifdef ATH_SUPPORT_EXT_STAT
void  dp_iterate_update_peer_list(struct cdp_pdev *pdev_hdl)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_hdl;
	struct dp_soc *soc = pdev->soc;
	struct dp_vdev *vdev = NULL;
	struct dp_peer *peer = NULL;

	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	DP_PDEV_ITERATE_VDEV_LIST(pdev, vdev) {
		DP_VDEV_ITERATE_PEER_LIST(vdev, peer) {
			dp_cal_client_update_peer_stats(&peer->stats);
		}
	}
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);
}
#else
void  dp_iterate_update_peer_list(struct cdp_pdev *pdev_hdl)
{
}
#endif

/*
 * dp_htt_ppdu_stats_attach() - attach resources for HTT PPDU stats processing
 * @pdev: Datapath PDEV handle
 *
 * Return: QDF_STATUS_SUCCESS: Success
 *         QDF_STATUS_E_NOMEM: Error
 */
static QDF_STATUS dp_htt_ppdu_stats_attach(struct dp_pdev *pdev)
{
	pdev->ppdu_tlv_buf = qdf_mem_malloc(HTT_T2H_MAX_MSG_SIZE);

	if (!pdev->ppdu_tlv_buf) {
		QDF_TRACE_ERROR(QDF_MODULE_ID_DP, "ppdu_tlv_buf alloc fail");
		return QDF_STATUS_E_NOMEM;
	}

	return QDF_STATUS_SUCCESS;
}

#ifdef WLAN_FEATURE_DP_RX_RING_HISTORY
#ifndef RX_DEFRAG_DO_NOT_REINJECT
/**
 * dp_soc_rx_reinject_ring_history_attach - Attach the reo reinject ring
 *					    history.
 * @soc: DP soc handle
 *
 * Return: None
 */
static void dp_soc_rx_reinject_ring_history_attach(struct dp_soc *soc)
{
	soc->rx_reinject_ring_history = dp_context_alloc_mem(
			soc, DP_RX_REINJECT_RING_HIST_TYPE, rx_ring_hist_size);
	if (soc->rx_reinject_ring_history)
		qdf_atomic_init(&soc->rx_reinject_ring_history->index);
}
#else /* RX_DEFRAG_DO_NOT_REINJECT */
static inline void
dp_soc_rx_reinject_ring_history_attach(struct dp_soc *soc)
{
}
#endif /* RX_DEFRAG_DO_NOT_REINJECT */

/**
 * dp_soc_rx_history_attach() - Attach the ring history record buffers
 * @soc: DP soc structure
 *
 * This function allocates the memory for recording the rx ring, rx error
 * ring and the reinject ring entries. There is no error returned in case
 * of allocation failure since the record function checks if the history is
 * initialized or not. We do not want to fail the driver load in case of
 * failure to allocate memory for debug history.
 *
 * Returns: None
 */
static void dp_soc_rx_history_attach(struct dp_soc *soc)
{
	int i;
	uint32_t rx_ring_hist_size;
	uint32_t rx_err_ring_hist_size;
	uint32_t rx_reinject_hist_size;

	rx_ring_hist_size = sizeof(*soc->rx_ring_history[0]);
	rx_err_ring_hist_size = sizeof(*soc->rx_err_ring_history);
	rx_reinject_hist_size = sizeof(*soc->rx_reinject_ring_history);

	for (i = 0; i < MAX_REO_DEST_RINGS; i++) {
		soc->rx_ring_history[i] = dp_context_alloc_mem(
				soc, DP_RX_RING_HIST_TYPE, rx_ring_hist_size);
		if (soc->rx_ring_history[i])
			qdf_atomic_init(&soc->rx_ring_history[i]->index);
	}

	soc->rx_err_ring_history = dp_context_alloc_mem(
			soc, DP_RX_ERR_RING_HIST_TYPE, rx_ring_hist_size);
	if (soc->rx_err_ring_history)
		qdf_atomic_init(&soc->rx_err_ring_history->index);

	dp_soc_rx_reinject_ring_history_attach(soc);
}

static void dp_soc_rx_history_detach(struct dp_soc *soc)
{
	int i;

	for (i = 0; i < MAX_REO_DEST_RINGS; i++)
		dp_context_free_mem(soc, DP_RX_RING_HIST_TYPE,
				    soc->rx_ring_history[i]);

	dp_context_free_mem(soc, DP_RX_ERR_RING_HIST_TYPE,
			    soc->rx_err_ring_history);

	/*
	 * No need for a featurized detach since qdf_mem_free takes
	 * care of NULL pointer.
	 */
	dp_context_free_mem(soc, DP_RX_REINJECT_RING_HIST_TYPE,
			    soc->rx_reinject_ring_history);
}

#else
static inline void dp_soc_rx_history_attach(struct dp_soc *soc)
{
}

static inline void dp_soc_rx_history_detach(struct dp_soc *soc)
{
}
#endif

/*
* dp_pdev_attach_wifi3() - attach txrx pdev
* @txrx_soc: Datapath SOC handle
* @htc_handle: HTC handle for host-target interface
* @qdf_osdev: QDF OS device
* @pdev_id: PDEV ID
*
* Return: QDF_STATUS
*/
static inline QDF_STATUS dp_pdev_attach_wifi3(struct cdp_soc_t *txrx_soc,
					      HTC_HANDLE htc_handle,
					      qdf_device_t qdf_osdev,
					      uint8_t pdev_id)
{
	int ring_size;
	int entries;
	struct wlan_cfg_dp_soc_ctxt *soc_cfg_ctx;
	int nss_cfg;
	void *sojourn_buf;
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;
	struct dp_pdev *pdev = NULL;
	QDF_STATUS ret;

	if (dp_is_soc_reinit(soc)) {
		pdev = soc->pdev_list[pdev_id];
	} else {
		pdev = dp_context_alloc_mem(soc, DP_PDEV_TYPE, sizeof(*pdev));
		qdf_minidump_log(pdev, sizeof(*pdev), "dp_pdev");
	}

	if (!pdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("DP PDEV memory allocation failed"));
		ret = QDF_STATUS_E_NOMEM;
		goto fail0;
	}

	pdev->filter = dp_mon_filter_alloc(pdev);
	if (!pdev->filter) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Memory allocation failed for monitor filters"));
		qdf_mem_free(pdev);
		ret = QDF_STATUS_E_NOMEM;
		goto fail0;
	}

	/*
	 * Variable to prevent double pdev deinitialization during
	 * radio detach execution .i.e. in the absence of any vdev.
	 */
	pdev->pdev_deinit = 0;
	pdev->invalid_peer = qdf_mem_malloc(sizeof(struct dp_peer));

	if (!pdev->invalid_peer) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Invalid peer memory allocation failed"));
		dp_mon_filter_dealloc(pdev);
		qdf_mem_free(pdev);
		ret = QDF_STATUS_E_NOMEM;
		goto fail0;
	}

	soc_cfg_ctx = soc->wlan_cfg_ctx;
	pdev->wlan_cfg_ctx = wlan_cfg_pdev_attach(soc->ctrl_psoc);

	if (!pdev->wlan_cfg_ctx) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("pdev cfg_attach failed"));

		qdf_mem_free(pdev->invalid_peer);
		dp_mon_filter_dealloc(pdev);
		qdf_mem_free(pdev);
		ret = QDF_STATUS_E_FAILURE;
		goto fail0;
	}

	/*
	 * set nss pdev config based on soc config
	 */
	nss_cfg = wlan_cfg_get_dp_soc_nss_cfg(soc_cfg_ctx);
	wlan_cfg_set_dp_pdev_nss_enabled(pdev->wlan_cfg_ctx,
			(nss_cfg & (1 << pdev_id)));

	pdev->soc = soc;
	pdev->pdev_id = pdev_id;
	soc->pdev_list[pdev_id] = pdev;

	pdev->lmac_id = wlan_cfg_get_hw_mac_idx(soc->wlan_cfg_ctx, pdev_id);
	soc->pdev_count++;

	TAILQ_INIT(&pdev->vdev_list);
	qdf_spinlock_create(&pdev->vdev_list_lock);
	pdev->vdev_count = 0;

	qdf_spinlock_create(&pdev->tx_mutex);
	qdf_spinlock_create(&pdev->neighbour_peer_mutex);
	TAILQ_INIT(&pdev->neighbour_peers_list);
	pdev->neighbour_peers_added = false;
	pdev->monitor_configured = false;
	pdev->enable_reap_timer_non_pkt = false;

	if (dp_soc_cmn_setup(soc)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_soc_cmn_setup failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	/* Setup per PDEV TCL rings if configured */
	if (wlan_cfg_per_pdev_tx_ring(soc->wlan_cfg_ctx)) {
		ring_size =
			wlan_cfg_tx_ring_size(soc_cfg_ctx);

		if (dp_srng_setup(soc, &soc->tcl_data_ring[pdev_id], TCL_DATA,
				  pdev_id, pdev_id, ring_size, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("dp_srng_setup failed for tcl_data_ring"));
			ret = QDF_STATUS_E_FAILURE;
			goto fail1;
		}

		ring_size =
			wlan_cfg_tx_comp_ring_size(soc_cfg_ctx);

		if (dp_srng_setup(soc, &soc->tx_comp_ring[pdev_id],
				  WBM2SW_RELEASE, pdev_id, pdev_id,
				  ring_size, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("dp_srng_setup failed for tx_comp_ring"));
			ret = QDF_STATUS_E_FAILURE;
			goto fail1;
		}
		soc->num_tcl_data_rings++;
	}

	/* Tx specific init */
	if (dp_tx_pdev_attach(pdev)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_tx_pdev_attach failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	ring_size = wlan_cfg_get_reo_dst_ring_size(soc->wlan_cfg_ctx);
	/* Setup per PDEV REO rings if configured */
	if (wlan_cfg_per_pdev_rx_ring(soc_cfg_ctx)) {
		if (dp_srng_setup(soc, &soc->reo_dest_ring[pdev_id], REO_DST,
				  pdev_id, pdev_id, ring_size, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("dp_srng_setup failed for reo_dest_ringn"));
			ret = QDF_STATUS_E_FAILURE;
			goto fail1;
		}
		soc->num_reo_dest_rings++;
	}

	ring_size =
		wlan_cfg_get_dp_soc_rxdma_refill_ring_size(soc->wlan_cfg_ctx);

	if (dp_srng_setup(soc, &soc->rx_refill_buf_ring[pdev->lmac_id],
			  RXDMA_BUF, 0, pdev->lmac_id, ring_size, 0)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			 FL("dp_srng_setup failed rx refill ring"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	if (dp_rxdma_ring_setup(soc, pdev)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			 FL("RXDMA ring config failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	if (dp_mon_rings_setup(soc, pdev)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("MONITOR rings setup failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	entries = wlan_cfg_get_dp_soc_rxdma_err_dst_ring_size(soc_cfg_ctx);
	if (wlan_cfg_per_pdev_lmac_ring(soc->wlan_cfg_ctx)) {
		if (dp_srng_setup(soc,
				  &soc->rxdma_err_dst_ring[pdev->lmac_id],
				  RXDMA_DST,
				  0, pdev->lmac_id, entries, 0)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  FL(RNG_ERR "rxdma_err_dst_ring"));
			ret = QDF_STATUS_E_FAILURE;
			goto fail1;
		}
	}

	if (dp_setup_ipa_rx_refill_buf_ring(soc, pdev)) {
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	if (dp_ipa_ring_resource_setup(soc, pdev)) {
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	if (dp_ipa_uc_attach(soc, pdev) != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("dp_ipa_uc_attach failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	/* Rx specific init */
	if (dp_rx_pdev_attach(pdev)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("dp_rx_pdev_attach failed"));
		ret = QDF_STATUS_E_FAILURE;
		goto fail2;
	}

	DP_STATS_INIT(pdev);

	/* Monitor filter init */
	pdev->mon_filter_mode = MON_FILTER_ALL;
	pdev->fp_mgmt_filter = FILTER_MGMT_ALL;
	pdev->fp_ctrl_filter = FILTER_CTRL_ALL;
	pdev->fp_data_filter = FILTER_DATA_ALL;
	pdev->mo_mgmt_filter = FILTER_MGMT_ALL;
	pdev->mo_ctrl_filter = FILTER_CTRL_ALL;
	pdev->mo_data_filter = FILTER_DATA_ALL;

	dp_local_peer_id_pool_init(pdev);

	dp_dscp_tid_map_setup(pdev);
	dp_pcp_tid_map_setup(pdev);

	/* Rx monitor mode specific init */
	if (dp_rx_pdev_mon_attach(pdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
				"dp_rx_pdev_mon_attach failed");
		ret = QDF_STATUS_E_FAILURE;
		goto fail2;
	}

	if (dp_wdi_event_attach(pdev)) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
				"dp_wdi_evet_attach failed");
		ret = QDF_STATUS_E_FAILURE;
		goto wdi_attach_fail;
	}

	/* set the reo destination during initialization */
	pdev->reo_dest = pdev->pdev_id + 1;

	/*
	 * initialize ppdu tlv list
	 */
	TAILQ_INIT(&pdev->ppdu_info_list);
	pdev->tlv_count = 0;
	pdev->list_depth = 0;

	qdf_mem_zero(&pdev->sojourn_stats, sizeof(struct cdp_tx_sojourn_stats));

	pdev->sojourn_buf = qdf_nbuf_alloc(pdev->soc->osdev,
			      sizeof(struct cdp_tx_sojourn_stats), 0, 4,
			      TRUE);

	if (pdev->sojourn_buf) {
		sojourn_buf = qdf_nbuf_data(pdev->sojourn_buf);
		qdf_mem_zero(sojourn_buf, sizeof(struct cdp_tx_sojourn_stats));
	}
	/* initlialize cal client timer */
	dp_cal_client_attach(&pdev->cal_client_ctx,
			     dp_pdev_to_cdp_pdev(pdev),
			     pdev->soc->osdev,
			     &dp_iterate_update_peer_list);
	qdf_event_create(&pdev->fw_peer_stats_event);

	pdev->num_tx_allowed = wlan_cfg_get_num_tx_desc(soc->wlan_cfg_ctx);

	dp_init_tso_stats(pdev);

	if (dp_htt_ppdu_stats_attach(pdev) != QDF_STATUS_SUCCESS) {
		ret = QDF_STATUS_E_FAILURE;
		goto fail1;
	}

	dp_tx_ppdu_stats_attach(pdev);

	return QDF_STATUS_SUCCESS;

wdi_attach_fail:
	/*
	 * dp_mon_link_desc_pool_cleanup is done in dp_pdev_detach
	 * and hence need not to be done here.
	 */
	dp_rx_pdev_mon_detach(pdev);

fail2:
	dp_rx_pdev_detach(pdev);
	dp_ipa_uc_detach(soc, pdev);

fail1:
	soc->pdev_count--;
	if (pdev->invalid_peer)
		qdf_mem_free(pdev->invalid_peer);

	if (pdev->filter)
		dp_mon_filter_dealloc(pdev);

	dp_pdev_detach((struct cdp_pdev *)pdev, 0);
fail0:
	return ret;
}

/*
* dp_rxdma_ring_cleanup() - configure the RX DMA rings
* @soc: data path SoC handle
* @pdev: Physical device handle
*
* Return: void
*/
#ifdef QCA_HOST2FW_RXBUF_RING
static void dp_rxdma_ring_cleanup(struct dp_soc *soc,
	 struct dp_pdev *pdev)
{
	int i;

	for (i = 0; i < MAX_RX_MAC_RINGS; i++)
		dp_srng_cleanup(soc, &pdev->rx_mac_buf_ring[i],
			 RXDMA_BUF, 1);

	if (soc->reap_timer_init) {
		qdf_timer_free(&soc->mon_reap_timer);
		soc->reap_timer_init = 0;
	}
}
#else
static void dp_rxdma_ring_cleanup(struct dp_soc *soc,
	 struct dp_pdev *pdev)
{
	if (soc->lmac_timer_init) {
		qdf_timer_stop(&soc->lmac_reap_timer);
		qdf_timer_free(&soc->lmac_reap_timer);
		soc->lmac_timer_init = 0;
	}
}
#endif

/*
 * dp_neighbour_peers_detach() - Detach neighbour peers(nac clients)
 * @pdev: device object
 *
 * Return: void
 */
static void dp_neighbour_peers_detach(struct dp_pdev *pdev)
{
	struct dp_neighbour_peer *peer = NULL;
	struct dp_neighbour_peer *temp_peer = NULL;

	TAILQ_FOREACH_SAFE(peer, &pdev->neighbour_peers_list,
			neighbour_peer_list_elem, temp_peer) {
		/* delete this peer from the list */
		TAILQ_REMOVE(&pdev->neighbour_peers_list,
				peer, neighbour_peer_list_elem);
		qdf_mem_free(peer);
	}

	qdf_spinlock_destroy(&pdev->neighbour_peer_mutex);
}

/**
* dp_htt_ppdu_stats_detach() - detach stats resources
* @pdev: Datapath PDEV handle
*
* Return: void
*/
static void dp_htt_ppdu_stats_detach(struct dp_pdev *pdev)
{
	struct ppdu_info *ppdu_info, *ppdu_info_next;

	TAILQ_FOREACH_SAFE(ppdu_info, &pdev->ppdu_info_list,
			ppdu_info_list_elem, ppdu_info_next) {
		if (!ppdu_info)
			break;
		qdf_assert_always(ppdu_info->nbuf);
		qdf_nbuf_free(ppdu_info->nbuf);
		qdf_mem_free(ppdu_info);
	}

	if (pdev->ppdu_tlv_buf)
		qdf_mem_free(pdev->ppdu_tlv_buf);

}

#if !defined(DISABLE_MON_CONFIG)

static
void dp_mon_ring_cleanup(struct dp_soc *soc, struct dp_pdev *pdev,
			 int mac_id)
{
	if (soc->wlan_cfg_ctx->rxdma1_enable) {
		dp_srng_cleanup(soc,
				&soc->rxdma_mon_buf_ring[mac_id],
				RXDMA_MONITOR_BUF, 0);

		dp_srng_cleanup(soc,
				&soc->rxdma_mon_dst_ring[mac_id],
				RXDMA_MONITOR_DST, 0);

		dp_srng_cleanup(soc,
				&soc->rxdma_mon_status_ring[mac_id],
				RXDMA_MONITOR_STATUS, 0);

		dp_srng_cleanup(soc,
				&soc->rxdma_mon_desc_ring[mac_id],
				RXDMA_MONITOR_DESC, 0);

		dp_srng_cleanup(soc,
				&soc->rxdma_err_dst_ring[mac_id],
				RXDMA_DST, 0);
	} else {
		dp_srng_cleanup(soc,
				&soc->rxdma_mon_status_ring[mac_id],
				RXDMA_MONITOR_STATUS, 0);

		dp_srng_cleanup(soc,
				&soc->rxdma_err_dst_ring[mac_id],
				RXDMA_DST, 0);
	}

}
#else
static void dp_mon_ring_cleanup(struct dp_soc *soc, struct dp_pdev *pdev,
				int mac_id)
{
}
#endif

/**
 * dp_mon_ring_deinit() - Placeholder to deinitialize Monitor rings
 *
 * @soc: soc handle
 * @pdev: datapath physical dev handle
 * @mac_id: mac number
 *
 * Return: None
 */
static void dp_mon_ring_deinit(struct dp_soc *soc, struct dp_pdev *pdev,
			       int mac_id)
{
}

/**
 * dp_pdev_mem_reset() - Reset txrx pdev memory
 * @pdev: dp pdev handle
 *
 * Return: None
 */
static void dp_pdev_mem_reset(struct dp_pdev *pdev)
{
	uint16_t len = 0;
	uint8_t *dp_pdev_offset = (uint8_t *)pdev;

	len = sizeof(struct dp_pdev) -
		offsetof(struct dp_pdev, pdev_deinit) -
		sizeof(pdev->pdev_deinit);
	dp_pdev_offset = dp_pdev_offset +
			 offsetof(struct dp_pdev, pdev_deinit) +
			 sizeof(pdev->pdev_deinit);

	qdf_mem_zero(dp_pdev_offset, len);
}

#ifdef WLAN_DP_PENDING_MEM_FLUSH
/**
 * dp_pdev_flush_pending_vdevs() - Flush all delete pending vdevs in pdev
 * @pdev: Datapath PDEV handle
 *
 * This is the last chance to flush all pending dp vdevs/peers,
 * some peer/vdev leak case like Non-SSR + peer unmap missing
 * will be covered here.
 *
 * Return: None
 */
static void dp_pdev_flush_pending_vdevs(struct dp_pdev *pdev)
{
	struct dp_vdev *vdev = NULL;

	while (true) {
		qdf_spin_lock_bh(&pdev->vdev_list_lock);
		TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {
			if (vdev->delete.pending)
				break;
		}
		qdf_spin_unlock_bh(&pdev->vdev_list_lock);

		/*
		 * vdev will be freed when all peers get cleanup,
		 * dp_delete_pending_vdev will remove vdev from vdev_list
		 * in pdev.
		 */
		if (vdev)
			dp_vdev_flush_peers((struct cdp_vdev *)vdev, 0);
		else
			break;
	}
}
#else
static void dp_pdev_flush_pending_vdevs(struct dp_pdev *pdev)
{
}
#endif

/**
 * dp_pdev_deinit() - Deinit txrx pdev
 * @txrx_pdev: Datapath PDEV handle
 * @force: Force deinit
 *
 * Return: None
 */
static void dp_pdev_deinit(struct cdp_pdev *txrx_pdev, int force)
{
	struct dp_pdev *pdev = (struct dp_pdev *)txrx_pdev;
	struct dp_soc *soc = pdev->soc;
	qdf_nbuf_t curr_nbuf, next_nbuf;
	int mac_id;

	/*
	 * Prevent double pdev deinitialization during radio detach
	 * execution .i.e. in the absence of any vdev
	 */
	if (pdev->pdev_deinit)
		return;

	pdev->pdev_deinit = 1;

	dp_wdi_event_detach(pdev);

	dp_pdev_flush_pending_vdevs(pdev);

	dp_tx_pdev_detach(pdev);

	if (wlan_cfg_per_pdev_tx_ring(soc->wlan_cfg_ctx)) {
		dp_srng_deinit(soc, &soc->tcl_data_ring[pdev->pdev_id],
			       TCL_DATA, pdev->pdev_id);
		dp_srng_deinit(soc, &soc->tx_comp_ring[pdev->pdev_id],
			       WBM2SW_RELEASE, pdev->pdev_id);
	}

	dp_pktlogmod_exit(pdev);

	dp_rx_fst_detach(soc, pdev);
	dp_rx_pdev_detach(pdev);
	dp_rx_pdev_mon_detach(pdev);
	dp_neighbour_peers_detach(pdev);
	qdf_spinlock_destroy(&pdev->tx_mutex);
	qdf_spinlock_destroy(&pdev->vdev_list_lock);

	dp_ipa_uc_detach(soc, pdev);

	dp_cleanup_ipa_rx_refill_buf_ring(soc, pdev);

	/* Cleanup per PDEV REO rings if configured */
	if (wlan_cfg_per_pdev_rx_ring(soc->wlan_cfg_ctx)) {
		dp_srng_deinit(soc, &soc->reo_dest_ring[pdev->pdev_id],
			       REO_DST, pdev->pdev_id);
	}

	dp_srng_deinit(soc, &soc->rx_refill_buf_ring[pdev->lmac_id],
		       RXDMA_BUF, 0);

	dp_rxdma_ring_cleanup(soc, pdev);

	for (mac_id = 0; mac_id < NUM_RXDMA_RINGS_PER_PDEV; mac_id++) {
		int lmac_id =
			dp_get_lmac_id_for_pdev_id(soc, mac_id, pdev->pdev_id);

		dp_mon_ring_deinit(soc, pdev, lmac_id);

		dp_srng_deinit(soc, &soc->rxdma_err_dst_ring[lmac_id],
			       RXDMA_DST, 0);
	}

	curr_nbuf = pdev->invalid_peer_head_msdu;
	while (curr_nbuf) {
		next_nbuf = qdf_nbuf_next(curr_nbuf);
		qdf_nbuf_free(curr_nbuf);
		curr_nbuf = next_nbuf;
	}
	pdev->invalid_peer_head_msdu = NULL;
	pdev->invalid_peer_tail_msdu = NULL;

	dp_htt_ppdu_stats_detach(pdev);

	dp_tx_ppdu_stats_detach(pdev);

	qdf_nbuf_free(pdev->sojourn_buf);
	qdf_nbuf_queue_free(&pdev->rx_ppdu_buf_q);

	dp_cal_client_detach(&pdev->cal_client_ctx);

	soc->pdev_count--;

	/* only do soc common cleanup when last pdev do detach */
	if (!(soc->pdev_count))
		dp_reo_cmdlist_destroy(soc);

	wlan_cfg_pdev_detach(pdev->wlan_cfg_ctx);
	if (pdev->invalid_peer)
		qdf_mem_free(pdev->invalid_peer);

	/*
	 * Fee the monitor filter allocated and stored
	 */
	if (pdev->filter)
		dp_mon_filter_dealloc(pdev);

	qdf_mem_free(pdev->dp_txrx_handle);
	dp_pdev_mem_reset(pdev);
}

/**
 * dp_pdev_deinit_wifi3() - Deinit txrx pdev
 * @psoc: Datapath psoc handle
 * @pdev_id: Id of datapath PDEV handle
 * @force: Force deinit
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_pdev_deinit_wifi3(struct cdp_soc_t *psoc, uint8_t pdev_id,
		     int force)
{
	struct dp_soc *soc = (struct dp_soc *)psoc;
	struct dp_pdev *txrx_pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)psoc,
						   pdev_id);

	if (!txrx_pdev)
		return QDF_STATUS_E_FAILURE;

	soc->dp_soc_reinit = TRUE;

	dp_pdev_deinit((struct cdp_pdev *)txrx_pdev, force);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_pdev_detach() - Complete rest of pdev detach
 * @txrx_pdev: Datapath PDEV handle
 * @force: Force deinit
 *
 * Return: None
 */
static void dp_pdev_detach(struct cdp_pdev *txrx_pdev, int force)
{
	struct dp_pdev *pdev = (struct dp_pdev *)txrx_pdev;
	struct dp_soc *soc = pdev->soc;
	struct rx_desc_pool *rx_desc_pool;
	int mac_id, mac_for_pdev;
	int lmac_id;

	if (wlan_cfg_per_pdev_tx_ring(soc->wlan_cfg_ctx)) {
		dp_srng_cleanup(soc, &soc->tcl_data_ring[pdev->pdev_id],
				TCL_DATA, pdev->pdev_id);
		dp_srng_cleanup(soc, &soc->tx_comp_ring[pdev->pdev_id],
				WBM2SW_RELEASE, pdev->pdev_id);
	}

	dp_mon_link_free(pdev);

	/* Cleanup per PDEV REO rings if configured */
	if (wlan_cfg_per_pdev_rx_ring(soc->wlan_cfg_ctx)) {
		dp_srng_cleanup(soc, &soc->reo_dest_ring[pdev->pdev_id],
				REO_DST, pdev->pdev_id);
	}
	dp_rxdma_ring_cleanup(soc, pdev);
	wlan_cfg_pdev_detach(pdev->wlan_cfg_ctx);

	dp_srng_cleanup(soc, &soc->rx_refill_buf_ring[pdev->lmac_id],
			RXDMA_BUF, 0);
	dp_cleanup_ipa_rx_refill_buf_ring(soc, pdev);

	for (mac_id = 0; mac_id < NUM_RXDMA_RINGS_PER_PDEV; mac_id++) {
		lmac_id =
			dp_get_lmac_id_for_pdev_id(soc, mac_id, pdev->pdev_id);
		dp_mon_ring_cleanup(soc, pdev, lmac_id);
		dp_srng_cleanup(soc, &soc->rxdma_err_dst_ring[lmac_id],
				RXDMA_DST, 0);

		if (dp_is_soc_reinit(soc)) {
			mac_for_pdev =
				dp_get_lmac_id_for_pdev_id(soc, mac_id,
							   pdev->pdev_id);
			rx_desc_pool = &soc->rx_desc_status[mac_for_pdev];
			dp_rx_desc_pool_free(soc, rx_desc_pool);
			rx_desc_pool = &soc->rx_desc_mon[mac_for_pdev];
			dp_rx_desc_pool_free(soc, rx_desc_pool);
		}
	}

	if (dp_is_soc_reinit(soc)) {
		rx_desc_pool = &soc->rx_desc_buf[pdev->lmac_id];
		dp_rx_desc_pool_free(soc, rx_desc_pool);
	}

	/* only do soc common cleanup when last pdev do detach */
	if (!(soc->pdev_count))
		dp_soc_cmn_cleanup(soc);

	soc->pdev_list[pdev->pdev_id] = NULL;
	qdf_minidump_remove(pdev);
	dp_context_free_mem(soc, DP_PDEV_TYPE, pdev);
}

/*
 * dp_pdev_detach_wifi3() - detach txrx pdev
 * @psoc: Datapath soc handle
 * @pdev_id: pdev id of pdev
 * @force: Force detach
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_pdev_detach_wifi3(struct cdp_soc_t *psoc, uint8_t pdev_id,
				       int force)
{
	struct dp_soc *soc = (struct dp_soc *)psoc;
	struct dp_pdev *txrx_pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)psoc,
						   pdev_id);

	if (!txrx_pdev) {
		dp_err("Couldn't find dp pdev");
		return QDF_STATUS_E_FAILURE;
	}

	if (dp_is_soc_reinit(soc)) {
		dp_pdev_detach((struct cdp_pdev *)txrx_pdev, force);
	} else {
		dp_pdev_deinit((struct cdp_pdev *)txrx_pdev, force);
		dp_pdev_detach((struct cdp_pdev *)txrx_pdev, force);
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_reo_desc_freelist_destroy() - Flush REO descriptors from deferred freelist
 * @soc: DP SOC handle
 */
static inline void dp_reo_desc_freelist_destroy(struct dp_soc *soc)
{
	struct reo_desc_list_node *desc;
	struct dp_rx_tid *rx_tid;

	qdf_spin_lock_bh(&soc->reo_desc_freelist_lock);
	while (qdf_list_remove_front(&soc->reo_desc_freelist,
		(qdf_list_node_t **)&desc) == QDF_STATUS_SUCCESS) {
		rx_tid = &desc->rx_tid;
		qdf_mem_unmap_nbytes_single(soc->osdev,
			rx_tid->hw_qdesc_paddr,
			QDF_DMA_BIDIRECTIONAL,
			rx_tid->hw_qdesc_alloc_size);
		qdf_mem_free(rx_tid->hw_qdesc_vaddr_unaligned);
		qdf_mem_free(desc);
	}
	qdf_spin_unlock_bh(&soc->reo_desc_freelist_lock);
	qdf_list_destroy(&soc->reo_desc_freelist);
	qdf_spinlock_destroy(&soc->reo_desc_freelist_lock);
}

/**
 * dp_soc_mem_reset() - Reset Dp Soc memory
 * @soc: DP handle
 *
 * Return: None
 */
static void dp_soc_mem_reset(struct dp_soc *soc)
{
	uint16_t len = 0;
	uint8_t *dp_soc_offset = (uint8_t *)soc;

	len = sizeof(struct dp_soc) -
		offsetof(struct dp_soc, dp_soc_reinit) -
		sizeof(soc->dp_soc_reinit);
	dp_soc_offset = dp_soc_offset +
			offsetof(struct dp_soc, dp_soc_reinit) +
			sizeof(soc->dp_soc_reinit);

	qdf_mem_zero(dp_soc_offset, len);
}

/**
 * dp_soc_deinit() - Deinitialize txrx SOC
 * @txrx_soc: Opaque DP SOC handle
 *
 * Return: None
 */
static void dp_soc_deinit(void *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;
	int i;

	qdf_atomic_set(&soc->cmn_init_done, 0);

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		if (soc->pdev_list[i])
			dp_pdev_deinit((struct cdp_pdev *)
					soc->pdev_list[i], 1);
	}

	qdf_flush_work(&soc->htt_stats.work);
	qdf_disable_work(&soc->htt_stats.work);

	/* Free pending htt stats messages */
	qdf_nbuf_queue_free(&soc->htt_stats.msg);

	dp_peer_find_detach(soc);

	/* Free the ring memories */
	/* Common rings */
	dp_srng_deinit(soc, &soc->wbm_desc_rel_ring, SW2WBM_RELEASE, 0);

	/* Tx data rings */
	if (!wlan_cfg_per_pdev_tx_ring(soc->wlan_cfg_ctx)) {
		for (i = 0; i < soc->num_tcl_data_rings; i++) {
			dp_srng_deinit(soc, &soc->tcl_data_ring[i],
				       TCL_DATA, i);
			dp_srng_deinit(soc, &soc->tx_comp_ring[i],
				       WBM2SW_RELEASE, i);
		}
	}

	/* TCL command and status rings */
	dp_srng_deinit(soc, &soc->tcl_cmd_ring, TCL_CMD, 0);
	dp_srng_deinit(soc, &soc->tcl_status_ring, TCL_STATUS, 0);

	/* Rx data rings */
	if (!wlan_cfg_per_pdev_rx_ring(soc->wlan_cfg_ctx)) {
		soc->num_reo_dest_rings =
			wlan_cfg_num_reo_dest_rings(soc->wlan_cfg_ctx);
		for (i = 0; i < soc->num_reo_dest_rings; i++) {
			/* TODO: Get number of rings and ring sizes
			 * from wlan_cfg
			 */
			dp_srng_deinit(soc, &soc->reo_dest_ring[i],
				       REO_DST, i);
		}
	}
	/* REO reinjection ring */
	dp_srng_deinit(soc, &soc->reo_reinject_ring, REO_REINJECT, 0);

	/* Rx release ring */
	dp_srng_deinit(soc, &soc->rx_rel_ring, WBM2SW_RELEASE, 0);

	/* Rx exception ring */
	/* TODO: Better to store ring_type and ring_num in
	 * dp_srng during setup
	 */
	dp_srng_deinit(soc, &soc->reo_exception_ring, REO_EXCEPTION, 0);

	/* REO command and status rings */
	dp_srng_deinit(soc, &soc->reo_cmd_ring, REO_CMD, 0);
	dp_srng_deinit(soc, &soc->reo_status_ring, REO_STATUS, 0);

	dp_soc_wds_detach(soc);

	qdf_spinlock_destroy(&soc->peer_ref_mutex);
	qdf_spinlock_destroy(&soc->htt_stats.lock);

	htt_soc_htc_dealloc(soc->htt_handle);

	dp_reo_desc_freelist_destroy(soc);

	qdf_spinlock_destroy(&soc->ast_lock);

	DEINIT_RX_HW_STATS_LOCK(soc);

	dp_soc_mem_reset(soc);
}

/**
 * dp_soc_deinit_wifi3() - Deinitialize txrx SOC
 * @txrx_soc: Opaque DP SOC handle
 *
 * Return: None
 */
static void dp_soc_deinit_wifi3(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	soc->dp_soc_reinit = 1;
	dp_soc_deinit(txrx_soc);
}

/*
 * dp_soc_detach() - Detach rest of txrx SOC
 * @txrx_soc: DP SOC handle, struct cdp_soc_t is first element of struct dp_soc.
 *
 * Return: None
 */
static void dp_soc_detach(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;
	int i;

	qdf_atomic_set(&soc->cmn_init_done, 0);

	/* TBD: Call Tx and Rx cleanup functions to free buffers and
	 * SW descriptors
	 */

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		if (soc->pdev_list[i])
			dp_pdev_detach((struct cdp_pdev *)
					     soc->pdev_list[i], 1);
	}

	/* Free the ring memories */
	/* Common rings */
	qdf_minidump_remove(soc->wbm_desc_rel_ring.base_vaddr_unaligned);
	dp_srng_cleanup(soc, &soc->wbm_desc_rel_ring, SW2WBM_RELEASE, 0);

	if (dp_is_soc_reinit(soc)) {
		dp_tx_soc_detach(soc);
	}

	/* Tx data rings */
	if (!wlan_cfg_per_pdev_tx_ring(soc->wlan_cfg_ctx)) {
		for (i = 0; i < soc->num_tcl_data_rings; i++) {
			dp_srng_cleanup(soc, &soc->tcl_data_ring[i],
				TCL_DATA, i);
			dp_srng_cleanup(soc, &soc->tx_comp_ring[i],
				WBM2SW_RELEASE, i);
		}
	}

	/* TCL command and status rings */
	dp_srng_cleanup(soc, &soc->tcl_cmd_ring, TCL_CMD, 0);
	dp_srng_cleanup(soc, &soc->tcl_status_ring, TCL_STATUS, 0);

	/* Rx data rings */
	if (!wlan_cfg_per_pdev_rx_ring(soc->wlan_cfg_ctx)) {
		soc->num_reo_dest_rings =
			wlan_cfg_num_reo_dest_rings(soc->wlan_cfg_ctx);
		for (i = 0; i < soc->num_reo_dest_rings; i++) {
			/* TODO: Get number of rings and ring sizes
			 * from wlan_cfg
			 */
			dp_srng_cleanup(soc, &soc->reo_dest_ring[i],
				REO_DST, i);
		}
	}
	/* REO reinjection ring */
	dp_srng_cleanup(soc, &soc->reo_reinject_ring, REO_REINJECT, 0);

	/* Rx release ring */
	dp_srng_cleanup(soc, &soc->rx_rel_ring, WBM2SW_RELEASE, 0);
	dp_srng_cleanup(soc, &soc->rx_rel_ring, WBM2SW_RELEASE, 3);

	/* Rx exception ring */
	/* TODO: Better to store ring_type and ring_num in
	 * dp_srng during setup
	 */
	dp_srng_cleanup(soc, &soc->reo_exception_ring, REO_EXCEPTION, 0);

	/* REO command and status rings */
	dp_srng_cleanup(soc, &soc->reo_cmd_ring, REO_CMD, 0);
	dp_srng_cleanup(soc, &soc->reo_status_ring, REO_STATUS, 0);
	dp_hw_link_desc_pool_cleanup(soc);

	htt_soc_detach(soc->htt_handle);
	soc->dp_soc_reinit = 0;

	wlan_cfg_soc_detach(soc->wlan_cfg_ctx);
	dp_soc_rx_history_detach(soc);

	qdf_minidump_remove(soc);
	qdf_mem_free(soc);
}

/*
 * dp_soc_detach_wifi3() - Detach txrx SOC
 * @txrx_soc: DP SOC handle, struct cdp_soc_t is first element of struct dp_soc.
 *
 * Return: None
 */
static void dp_soc_detach_wifi3(struct cdp_soc_t *txrx_soc)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	if (dp_is_soc_reinit(soc)) {
		dp_soc_detach(txrx_soc);
	} else {
		dp_soc_deinit(txrx_soc);
		dp_soc_detach(txrx_soc);
	}
}

#if !defined(DISABLE_MON_CONFIG)
/**
 * dp_mon_htt_srng_setup() - Prepare HTT messages for Monitor rings
 * @soc: soc handle
 * @pdev: physical device handle
 * @mac_id: ring number
 * @mac_for_pdev: mac_id
 *
 * Return: non-zero for failure, zero for success
 */
static QDF_STATUS dp_mon_htt_srng_setup(struct dp_soc *soc,
					struct dp_pdev *pdev,
					int mac_id,
					int mac_for_pdev)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (soc->wlan_cfg_ctx->rxdma1_enable) {
		status = htt_srng_setup(soc->htt_handle, mac_for_pdev,
					soc->rxdma_mon_buf_ring[mac_id]
					.hal_srng,
					RXDMA_MONITOR_BUF);

		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt srng setup message for Rxdma mon buf ring");
			return status;
		}

		status = htt_srng_setup(soc->htt_handle, mac_for_pdev,
					soc->rxdma_mon_dst_ring[mac_id]
					.hal_srng,
					RXDMA_MONITOR_DST);

		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt srng setup message for Rxdma mon dst ring");
			return status;
		}

		status = htt_srng_setup(soc->htt_handle, mac_for_pdev,
					soc->rxdma_mon_status_ring[mac_id]
					.hal_srng,
					RXDMA_MONITOR_STATUS);

		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt srng setup message for Rxdma mon status ring");
			return status;
		}

		status = htt_srng_setup(soc->htt_handle, mac_for_pdev,
				soc->rxdma_mon_desc_ring[mac_id]
					.hal_srng,
					RXDMA_MONITOR_DESC);

		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt srng message for Rxdma mon desc ring");
			return status;
		}
	} else {
		status = htt_srng_setup(soc->htt_handle, mac_for_pdev,
					soc->rxdma_mon_status_ring[mac_id]
					.hal_srng,
					RXDMA_MONITOR_STATUS);

		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt srng setup message for Rxdma mon status ring");
			return status;
		}
	}

	return status;

}
#else
static QDF_STATUS dp_mon_htt_srng_setup(struct dp_soc *soc,
					struct dp_pdev *pdev,
					int mac_id,
					int mac_for_pdev)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/*
 * dp_rxdma_ring_config() - configure the RX DMA rings
 *
 * This function is used to configure the MAC rings.
 * On MCL host provides buffers in Host2FW ring
 * FW refills (copies) buffers to the ring and updates
 * ring_idx in register
 *
 * @soc: data path SoC handle
 *
 * Return: zero on success, non-zero on failure
 */
#ifdef QCA_HOST2FW_RXBUF_RING
static QDF_STATUS dp_rxdma_ring_config(struct dp_soc *soc)
{
	int i;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	for (i = 0; i < MAX_PDEV_CNT; i++) {
		struct dp_pdev *pdev = soc->pdev_list[i];

		if (pdev) {
			int mac_id;
			bool dbs_enable = 0;
			int max_mac_rings =
				 wlan_cfg_get_num_mac_rings
				(pdev->wlan_cfg_ctx);
			int lmac_id = dp_get_lmac_id_for_pdev_id(soc, 0, i);

			htt_srng_setup(soc->htt_handle, 0,
				       soc->rx_refill_buf_ring[lmac_id]
				       .hal_srng,
				       RXDMA_BUF);

			if (pdev->rx_refill_buf_ring2.hal_srng)
				htt_srng_setup(soc->htt_handle, 0,
					pdev->rx_refill_buf_ring2.hal_srng,
					RXDMA_BUF);

			if (soc->cdp_soc.ol_ops->
				is_hw_dbs_2x2_capable) {
				dbs_enable = soc->cdp_soc.ol_ops->
					is_hw_dbs_2x2_capable(
							(void *)soc->ctrl_psoc);
			}

			if (dbs_enable) {
				QDF_TRACE(QDF_MODULE_ID_TXRX,
				QDF_TRACE_LEVEL_ERROR,
				FL("DBS enabled max_mac_rings %d"),
					 max_mac_rings);
			} else {
				max_mac_rings = 1;
				QDF_TRACE(QDF_MODULE_ID_TXRX,
					 QDF_TRACE_LEVEL_ERROR,
					 FL("DBS disabled, max_mac_rings %d"),
					 max_mac_rings);
			}

			QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
					 FL("pdev_id %d max_mac_rings %d"),
					 pdev->pdev_id, max_mac_rings);

			for (mac_id = 0; mac_id < max_mac_rings; mac_id++) {
				int mac_for_pdev =
					dp_get_mac_id_for_pdev(mac_id,
							       pdev->pdev_id);
				/*
				 * Obtain lmac id from pdev to access the LMAC
				 * ring in soc context
				 */
				lmac_id =
				dp_get_lmac_id_for_pdev_id(soc,
							   mac_id,
							   pdev->pdev_id);
				QDF_TRACE(QDF_MODULE_ID_TXRX,
					 QDF_TRACE_LEVEL_ERROR,
					 FL("mac_id %d"), mac_for_pdev);

				htt_srng_setup(soc->htt_handle, mac_for_pdev,
					 pdev->rx_mac_buf_ring[mac_id]
						.hal_srng,
					 RXDMA_BUF);
				htt_srng_setup(soc->htt_handle, mac_for_pdev,
				soc->rxdma_err_dst_ring[lmac_id]
					.hal_srng,
					RXDMA_DST);

				/* Configure monitor mode rings */
				status = dp_mon_htt_srng_setup(soc, pdev,
							       lmac_id,
							       mac_for_pdev);
				if (status != QDF_STATUS_SUCCESS) {
					dp_err("Failed to send htt monitor messages to target");
					return status;
				}

			}
		}
	}

	/*
	 * Timer to reap rxdma status rings.
	 * Needed until we enable ppdu end interrupts
	 */
	qdf_timer_init(soc->osdev, &soc->mon_reap_timer,
		       dp_mon_reap_timer_handler, (void *)soc,
		       QDF_TIMER_TYPE_WAKE_APPS);
	soc->reap_timer_init = 1;
	return status;
}
#else
/* This is only for WIN */
static QDF_STATUS dp_rxdma_ring_config(struct dp_soc *soc)
{
	int i;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	int mac_for_pdev;
	int lmac_id;

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		struct dp_pdev *pdev =  soc->pdev_list[i];

		if (!pdev)
			continue;

		mac_for_pdev = i;
		lmac_id = dp_get_lmac_id_for_pdev_id(soc, 0, i);

		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rx_refill_buf_ring[lmac_id].
			       hal_srng, RXDMA_BUF);
#ifndef DISABLE_MON_CONFIG

		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rxdma_mon_buf_ring[lmac_id].hal_srng,
			       RXDMA_MONITOR_BUF);
		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rxdma_mon_dst_ring[lmac_id].hal_srng,
			       RXDMA_MONITOR_DST);
		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rxdma_mon_status_ring[lmac_id].hal_srng,
			       RXDMA_MONITOR_STATUS);
		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rxdma_mon_desc_ring[lmac_id].hal_srng,
			       RXDMA_MONITOR_DESC);
#endif
		htt_srng_setup(soc->htt_handle, mac_for_pdev,
			       soc->rxdma_err_dst_ring[lmac_id].hal_srng,
			       RXDMA_DST);
	}

	/* Configure LMAC rings in Polled mode */
	if (soc->lmac_polled_mode) {
		/*
		 * Timer to reap lmac rings.
		 */
		qdf_timer_init(soc->osdev, &soc->lmac_reap_timer,
			       dp_service_lmac_rings, (void *)soc,
			       QDF_TIMER_TYPE_WAKE_APPS);
		soc->lmac_timer_init = 1;
		qdf_timer_mod(&soc->lmac_reap_timer, DP_INTR_POLL_TIMER_MS);
	}
	return status;
}
#endif

#ifdef NO_RX_PKT_HDR_TLV
static QDF_STATUS
dp_rxdma_ring_sel_cfg(struct dp_soc *soc)
{
	int i;
	int mac_id;
	struct htt_rx_ring_tlv_filter htt_tlv_filter = {0};
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	htt_tlv_filter.mpdu_start = 1;
	htt_tlv_filter.msdu_start = 1;
	htt_tlv_filter.mpdu_end = 1;
	htt_tlv_filter.msdu_end = 1;
	htt_tlv_filter.attention = 1;
	htt_tlv_filter.packet = 1;
	htt_tlv_filter.packet_header = 0;

	htt_tlv_filter.ppdu_start = 0;
	htt_tlv_filter.ppdu_end = 0;
	htt_tlv_filter.ppdu_end_user_stats = 0;
	htt_tlv_filter.ppdu_end_user_stats_ext = 0;
	htt_tlv_filter.ppdu_end_status_done = 0;
	htt_tlv_filter.enable_fp = 1;
	htt_tlv_filter.enable_md = 0;
	htt_tlv_filter.enable_md = 0;
	htt_tlv_filter.enable_mo = 0;

	htt_tlv_filter.fp_mgmt_filter = 0;
	htt_tlv_filter.fp_ctrl_filter = FILTER_CTRL_BA_REQ;
	htt_tlv_filter.fp_data_filter = (FILTER_DATA_UCAST |
					 FILTER_DATA_MCAST |
					 FILTER_DATA_DATA);
	htt_tlv_filter.mo_mgmt_filter = 0;
	htt_tlv_filter.mo_ctrl_filter = 0;
	htt_tlv_filter.mo_data_filter = 0;
	htt_tlv_filter.md_data_filter = 0;

	htt_tlv_filter.offset_valid = true;

	htt_tlv_filter.rx_packet_offset = RX_PKT_TLVS_LEN;
	/*Not subscribing rx_pkt_header*/
	htt_tlv_filter.rx_header_offset = 0;
	htt_tlv_filter.rx_mpdu_start_offset =
				HAL_RX_PKT_TLV_MPDU_START_OFFSET(soc->hal_soc);
	htt_tlv_filter.rx_mpdu_end_offset =
				HAL_RX_PKT_TLV_MPDU_END_OFFSET(soc->hal_soc);
	htt_tlv_filter.rx_msdu_start_offset =
				HAL_RX_PKT_TLV_MSDU_START_OFFSET(soc->hal_soc);
	htt_tlv_filter.rx_msdu_end_offset =
				HAL_RX_PKT_TLV_MSDU_END_OFFSET(soc->hal_soc);
	htt_tlv_filter.rx_attn_offset =
				HAL_RX_PKT_TLV_ATTN_OFFSET(soc->hal_soc);

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		struct dp_pdev *pdev = soc->pdev_list[i];

		if (!pdev)
			continue;

		for (mac_id = 0; mac_id < NUM_RXDMA_RINGS_PER_PDEV; mac_id++) {
			int mac_for_pdev =
				dp_get_mac_id_for_pdev(mac_id, pdev->pdev_id);
			/*
			 * Obtain lmac id from pdev to access the LMAC ring
			 * in soc context
			 */
			int lmac_id =
				dp_get_lmac_id_for_pdev_id(soc, mac_id,
							   pdev->pdev_id);

			htt_h2t_rx_ring_cfg(soc->htt_handle, mac_for_pdev,
					    soc->rx_refill_buf_ring[lmac_id].
					    hal_srng,
					    RXDMA_BUF, RX_DATA_BUFFER_SIZE,
					    &htt_tlv_filter);
		}
	}
	return status;
}
#else
static QDF_STATUS
dp_rxdma_ring_sel_cfg(struct dp_soc *soc)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/*
 * dp_rx_target_fst_config() - configure the RXOLE Flow Search Engine
 *
 * This function is used to configure the FSE HW block in RX OLE on a
 * per pdev basis. Here, we will be programming parameters related to
 * the Flow Search Table.
 *
 * @soc: data path SoC handle
 *
 * Return: zero on success, non-zero on failure
 */
#ifdef WLAN_SUPPORT_RX_FLOW_TAG
static QDF_STATUS
dp_rx_target_fst_config(struct dp_soc *soc)
{
	int i;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		struct dp_pdev *pdev = soc->pdev_list[i];

		/* Flow search is not enabled if NSS offload is enabled */
		if (pdev &&
		    !wlan_cfg_get_dp_pdev_nss_enabled(pdev->wlan_cfg_ctx)) {
			status = dp_rx_flow_send_fst_fw_setup(pdev->soc, pdev);
			if (status != QDF_STATUS_SUCCESS)
				break;
		}
	}
	return status;
}
#elif defined(WLAN_SUPPORT_RX_FISA)
/**
 * dp_rx_target_fst_config() - Configure RX OLE FSE engine in HW
 * @soc: SoC handle
 *
 * Return: Success
 */
static inline QDF_STATUS dp_rx_target_fst_config(struct dp_soc *soc)
{
	/* Check if it is enabled in the INI */
	if (!soc->fisa_enable) {
		dp_err("RX FISA feature is disabled");
		return QDF_STATUS_E_NOSUPPORT;
	}

	return dp_rx_flow_send_fst_fw_setup(soc, soc->pdev_list[0]);
}

#define FISA_MAX_TIMEOUT 0xffffffff
#define FISA_DISABLE_TIMEOUT 0
static QDF_STATUS dp_rx_fisa_config(struct dp_soc *soc)
{
	struct dp_htt_rx_fisa_cfg fisa_config;

	fisa_config.pdev_id = 0;
	fisa_config.fisa_timeout = FISA_MAX_TIMEOUT;

	return dp_htt_rx_fisa_config(soc->pdev_list[0], &fisa_config);
}
#else /* !WLAN_SUPPORT_RX_FISA */
static inline QDF_STATUS dp_rx_target_fst_config(struct dp_soc *soc)
{
	return QDF_STATUS_SUCCESS;
}
#endif /* !WLAN_SUPPORT_RX_FISA */

#ifndef WLAN_SUPPORT_RX_FISA
static QDF_STATUS dp_rx_fisa_config(struct dp_soc *soc)
{
	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS dp_rx_dump_fisa_stats(struct dp_soc *soc)
{
	return QDF_STATUS_SUCCESS;
}

static void dp_rx_dump_fisa_table(struct dp_soc *soc)
{
}
#endif /* !WLAN_SUPPORT_RX_FISA */

/*
 * dp_soc_attach_target_wifi3() - SOC initialization in the target
 * @cdp_soc: Opaque Datapath SOC handle
 *
 * Return: zero on success, non-zero on failure
 */
static QDF_STATUS
dp_soc_attach_target_wifi3(struct cdp_soc_t *cdp_soc)
{
	struct dp_soc *soc = (struct dp_soc *)cdp_soc;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	htt_soc_attach_target(soc->htt_handle);

	status = dp_rxdma_ring_config(soc);
	if (status != QDF_STATUS_SUCCESS) {
		dp_err("Failed to send htt srng setup messages to target");
		return status;
	}

	status = dp_rxdma_ring_sel_cfg(soc);
	if (status != QDF_STATUS_SUCCESS) {
		dp_err("Failed to send htt ring config message to target");
		return status;
	}

	status = dp_rx_target_fst_config(soc);
	if (status != QDF_STATUS_SUCCESS &&
	    status != QDF_STATUS_E_NOSUPPORT) {
		dp_err("Failed to send htt fst setup config message to target");
		return status;
	}

	if (status == QDF_STATUS_SUCCESS) {
		status = dp_rx_fisa_config(soc);
		if (status != QDF_STATUS_SUCCESS) {
			dp_err("Failed to send htt FISA config message to target");
			return status;
		}
	}

	DP_STATS_INIT(soc);

	dp_runtime_init(soc);

	/* initialize work queue for stats processing */
	qdf_create_work(0, &soc->htt_stats.work, htt_t2h_stats_handler, soc);

	qdf_minidump_log(soc, sizeof(*soc), "dp_soc");

	return QDF_STATUS_SUCCESS;
}

/*
* dp_vdev_attach_wifi3() - attach txrx vdev
* @txrx_pdev: Datapath PDEV handle
* @vdev_mac_addr: MAC address of the virtual interface
* @vdev_id: VDEV Id
* @wlan_op_mode: VDEV operating mode
* @subtype: VDEV operating subtype
*
* Return: status
*/
static QDF_STATUS dp_vdev_attach_wifi3(struct cdp_soc_t *cdp_soc,
				       uint8_t pdev_id,
				       uint8_t *vdev_mac_addr,
				       uint8_t vdev_id,
				       enum wlan_op_mode op_mode,
				       enum wlan_op_subtype subtype)
{
	struct dp_soc *soc = (struct dp_soc *)cdp_soc;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	struct dp_vdev *vdev = qdf_mem_malloc(sizeof(*vdev));

	if (!pdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("DP PDEV is Null for pdev id %d"), pdev_id);
		qdf_mem_free(vdev);
		goto fail0;
	}

	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("DP VDEV memory allocation failed"));
		goto fail0;
	}

	vdev->pdev = pdev;
	vdev->vdev_id = vdev_id;
	vdev->opmode = op_mode;
	vdev->subtype = subtype;
	vdev->osdev = soc->osdev;

	vdev->osif_rx = NULL;
	vdev->osif_rsim_rx_decap = NULL;
	vdev->osif_get_key = NULL;
	vdev->osif_rx_mon = NULL;
	vdev->osif_tx_free_ext = NULL;
	vdev->osif_vdev = NULL;

	vdev->delete.pending = 0;
	vdev->safemode = 0;
	vdev->drop_unenc = 1;
	vdev->sec_type = cdp_sec_type_none;
	vdev->multipass_en = false;
#ifdef notyet
	vdev->filters_num = 0;
#endif

	qdf_mem_copy(
		&vdev->mac_addr.raw[0], vdev_mac_addr, QDF_MAC_ADDR_SIZE);

	/* TODO: Initialize default HTT meta data that will be used in
	 * TCL descriptors for packets transmitted from this VDEV
	 */

	TAILQ_INIT(&vdev->peer_list);
	dp_peer_multipass_list_init(vdev);

	if ((soc->intr_mode == DP_INTR_POLL) &&
	    wlan_cfg_get_num_contexts(soc->wlan_cfg_ctx) != 0) {
		if ((pdev->vdev_count == 0) ||
		    (wlan_op_mode_monitor == vdev->opmode))
			qdf_timer_mod(&soc->int_timer, DP_INTR_POLL_TIMER_MS);
	}

	soc->vdev_id_map[vdev_id] = vdev;

	if (wlan_op_mode_monitor == vdev->opmode) {
		pdev->monitor_vdev = vdev;
		return QDF_STATUS_SUCCESS;
	}

	vdev->tx_encap_type = wlan_cfg_pkt_type(soc->wlan_cfg_ctx);
	vdev->rx_decap_type = wlan_cfg_pkt_type(soc->wlan_cfg_ctx);
	vdev->dscp_tid_map_id = 0;
	vdev->mcast_enhancement_en = 0;
	vdev->raw_mode_war = wlan_cfg_get_raw_mode_war(soc->wlan_cfg_ctx);
	vdev->prev_tx_enq_tstamp = 0;
	vdev->prev_rx_deliver_tstamp = 0;

	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	/* add this vdev into the pdev's list */
	TAILQ_INSERT_TAIL(&pdev->vdev_list, vdev, vdev_list_elem);
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	pdev->vdev_count++;

	if (wlan_op_mode_sta != vdev->opmode &&
	    wlan_op_mode_ndi != vdev->opmode)
		vdev->ap_bridge_enabled = true;
	else
		vdev->ap_bridge_enabled = false;
	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
		  "%s: wlan_cfg_ap_bridge_enabled %d",
		  __func__, vdev->ap_bridge_enabled);

	dp_tx_vdev_attach(vdev);

	if (pdev->vdev_count == 1)
		dp_lro_hash_setup(soc, pdev);

	dp_info("Created vdev %pK ("QDF_MAC_ADDR_FMT")", vdev,
		QDF_MAC_ADDR_REF(vdev->mac_addr.raw));
	DP_STATS_INIT(vdev);

	if (wlan_op_mode_sta == vdev->opmode)
		dp_peer_create_wifi3((struct cdp_soc_t *)soc, vdev_id,
				     vdev->mac_addr.raw);

	return QDF_STATUS_SUCCESS;

fail0:
	return QDF_STATUS_E_FAILURE;
}

/**
 * dp_vdev_register_wifi3() - Register VDEV operations from osif layer
 * @soc: Datapath soc handle
 * @vdev_id: id of Datapath VDEV handle
 * @osif_vdev: OSIF vdev handle
 * @txrx_ops: Tx and Rx operations
 *
 * Return: DP VDEV handle on success, NULL on failure
 */
static QDF_STATUS dp_vdev_register_wifi3(struct cdp_soc_t *soc,
					 uint8_t vdev_id,
					 ol_osif_vdev_handle osif_vdev,
					 struct ol_txrx_ops *txrx_ops)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	vdev->osif_vdev = osif_vdev;
	vdev->osif_rx = txrx_ops->rx.rx;
	vdev->osif_rx_stack = txrx_ops->rx.rx_stack;
	vdev->osif_rx_flush = txrx_ops->rx.rx_flush;
	vdev->osif_gro_flush = txrx_ops->rx.rx_gro_flush;
	vdev->osif_rsim_rx_decap = txrx_ops->rx.rsim_rx_decap;
	vdev->osif_fisa_rx = txrx_ops->rx.osif_fisa_rx;
	vdev->osif_fisa_flush = txrx_ops->rx.osif_fisa_flush;
	vdev->osif_get_key = txrx_ops->get_key;
	vdev->osif_rx_mon = txrx_ops->rx.mon;
	vdev->osif_tx_free_ext = txrx_ops->tx.tx_free_ext;
	vdev->tx_comp = txrx_ops->tx.tx_comp;
#ifdef notyet
#if ATH_SUPPORT_WAPI
	vdev->osif_check_wai = txrx_ops->rx.wai_check;
#endif
#endif
#ifdef UMAC_SUPPORT_PROXY_ARP
	vdev->osif_proxy_arp = txrx_ops->proxy_arp;
#endif
	vdev->me_convert = txrx_ops->me_convert;

	/* TODO: Enable the following once Tx code is integrated */
	if (vdev->mesh_vdev)
		txrx_ops->tx.tx = dp_tx_send_mesh;
	else
		txrx_ops->tx.tx = dp_tx_send;

	txrx_ops->tx.tx_exception = dp_tx_send_exception;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO_LOW,
		"DP Vdev Register success");

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_peer_flush_ast_entry() - Forcibily flush all AST entry of peer
 * @soc: Datapath soc handle
 * @peer: Datapath peer handle
 * @peer_id: Peer ID
 * @vdev_id: Vdev ID
 *
 * Return: void
 */
static void dp_peer_flush_ast_entry(struct dp_soc *soc,
				    struct dp_peer *peer,
				    uint16_t peer_id,
				    uint8_t vdev_id)
{
	struct dp_ast_entry *ase, *tmp_ase;

	if (soc->is_peer_map_unmap_v2) {
		DP_PEER_ITERATE_ASE_LIST(peer, ase, tmp_ase) {
				dp_rx_peer_unmap_handler
						(soc, peer_id,
						 vdev_id,
						 ase->mac_addr.raw,
						 1);
		}
	}
}

/**
 * dp_vdev_flush_peers() - Forcibily Flush peers of vdev
 * @vdev: Datapath VDEV handle
 * @unmap_only: Flag to indicate "only unmap"
 *
 * Return: void
 */
static void dp_vdev_flush_peers(struct cdp_vdev *vdev_handle, bool unmap_only)
{
	struct dp_vdev *vdev = (struct dp_vdev *)vdev_handle;
	struct dp_pdev *pdev = vdev->pdev;
	struct dp_soc *soc = pdev->soc;
	struct dp_peer *peer;
	uint16_t *peer_ids;
	struct dp_peer **peer_array = NULL;
	uint8_t i = 0, j = 0;
	uint8_t m = 0, n = 0;

	peer_ids = qdf_mem_malloc(soc->max_peers * sizeof(peer_ids[0]));
	if (!peer_ids) {
		dp_err("DP alloc failure - unable to flush peers");
		return;
	}

	if (!unmap_only) {
		peer_array = qdf_mem_malloc(
				soc->max_peers * sizeof(struct dp_peer *));
		if (!peer_array) {
			qdf_mem_free(peer_ids);
			dp_err("DP alloc failure - unable to flush peers");
			return;
		}
	}

	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem) {
		if (!unmap_only && n < soc->max_peers)
			peer_array[n++] = peer;

		for (i = 0; i < MAX_NUM_PEER_ID_PER_PEER; i++)
			if (peer->peer_ids[i] != HTT_INVALID_PEER)
				if (j < soc->max_peers)
					peer_ids[j++] = peer->peer_ids[i];
	}
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	/*
	 * If peer id is invalid, need to flush the peer if
	 * peer valid flag is true, this is needed for NAN + SSR case.
	 */
	if (!unmap_only) {
		for (m = 0; m < n ; m++) {
			peer = peer_array[m];

			dp_info("peer: "QDF_MAC_ADDR_FMT" is getting deleted",
				QDF_MAC_ADDR_REF(peer->mac_addr.raw));
			/* only if peer valid is true */
			if (peer->valid)
				dp_peer_delete_wifi3((struct cdp_soc_t *)soc,
						     vdev->vdev_id,
						     peer->mac_addr.raw, 0);
		}
		qdf_mem_free(peer_array);
	}

	for (i = 0; i < j ; i++) {
		peer = __dp_peer_find_by_id(soc, peer_ids[i]);

		if (!peer)
			continue;

		dp_info("peer ref cnt %d", qdf_atomic_read(&peer->ref_cnt));
		/*
		 * set ref count to one to force delete the peers
		 * with ref count leak
		 */
		SET_PEER_REF_CNT_ONE(peer);
		dp_info("peer: "QDF_MAC_ADDR_FMT" is getting unmap",
			QDF_MAC_ADDR_REF(peer->mac_addr.raw));
		/* free AST entries of peer */
		dp_peer_flush_ast_entry(soc, peer,
					peer_ids[i],
					vdev->vdev_id);

		dp_rx_peer_unmap_handler(soc, peer_ids[i],
					 vdev->vdev_id,
					 peer->mac_addr.raw, 0);
	}

	qdf_mem_free(peer_ids);
	dp_info("Flushed peers for vdev object %pK ", vdev);
}

/*
 * dp_vdev_detach_wifi3() - Detach txrx vdev
 * @cdp_soc: Datapath soc handle
 * @vdev_id: VDEV Id
 * @callback: Callback OL_IF on completion of detach
 * @cb_context:	Callback context
 *
 */
static QDF_STATUS dp_vdev_detach_wifi3(struct cdp_soc_t *cdp_soc,
				       uint8_t vdev_id,
				       ol_txrx_vdev_delete_cb callback,
				       void *cb_context)
{
	struct dp_soc *soc = (struct dp_soc *)cdp_soc;
	struct dp_pdev *pdev;
	struct dp_neighbour_peer *peer = NULL;
	struct dp_neighbour_peer *temp_peer = NULL;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	pdev = vdev->pdev;

	if (wlan_op_mode_sta == vdev->opmode)
		dp_peer_delete_wifi3((struct cdp_soc_t *)soc, vdev->vdev_id,
				     vdev->vap_self_peer->mac_addr.raw, 0);

	/*
	 * If Target is hung, flush all peers before detaching vdev
	 * this will free all references held due to missing
	 * unmap commands from Target
	 */
	if (!hif_is_target_ready(HIF_GET_SOFTC(soc->hif_handle)))
		dp_vdev_flush_peers((struct cdp_vdev *)vdev, false);
	else if (hif_get_target_status(soc->hif_handle) == TARGET_STATUS_RESET)
		dp_vdev_flush_peers((struct cdp_vdev *)vdev, true);

	dp_rx_vdev_detach(vdev);
	/*
	 * move it after dp_rx_vdev_detach(),
	 * as the call back done in dp_rx_vdev_detach()
	 * still need to get vdev pointer by vdev_id.
	 */
	soc->vdev_id_map[vdev->vdev_id] = NULL;
	/*
	 * Use peer_ref_mutex while accessing peer_list, in case
	 * a peer is in the process of being removed from the list.
	 */
	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	/* check that the vdev has no peers allocated */
	if (!TAILQ_EMPTY(&vdev->peer_list)) {
		/* debug print - will be removed later */
		dp_warn("not deleting vdev object %pK ("QDF_MAC_ADDR_FMT") until deletion finishes for all its peers",
			vdev, QDF_MAC_ADDR_REF(vdev->mac_addr.raw));

		if (vdev->vdev_dp_ext_handle) {
			qdf_mem_free(vdev->vdev_dp_ext_handle);
			vdev->vdev_dp_ext_handle = NULL;
		}
		/* indicate that the vdev needs to be deleted */
		vdev->delete.pending = 1;
		vdev->delete.callback = callback;
		vdev->delete.context = cb_context;
		qdf_spin_unlock_bh(&soc->peer_ref_mutex);
		return QDF_STATUS_E_FAILURE;
	}
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	if (wlan_op_mode_monitor == vdev->opmode)
		goto free_vdev;

	qdf_spin_lock_bh(&pdev->neighbour_peer_mutex);
	if (!soc->hw_nac_monitor_support) {
		TAILQ_FOREACH(peer, &pdev->neighbour_peers_list,
			      neighbour_peer_list_elem) {
			QDF_ASSERT(peer->vdev != vdev);
		}
	} else {
		TAILQ_FOREACH_SAFE(peer, &pdev->neighbour_peers_list,
				   neighbour_peer_list_elem, temp_peer) {
			if (peer->vdev == vdev) {
				TAILQ_REMOVE(&pdev->neighbour_peers_list, peer,
					     neighbour_peer_list_elem);
				qdf_mem_free(peer);
			}
		}
	}
	qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);

	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	/* remove the vdev from its parent pdev's list */
	TAILQ_REMOVE(&pdev->vdev_list, vdev, vdev_list_elem);
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);

	dp_tx_vdev_detach(vdev);
free_vdev:
	if (wlan_op_mode_monitor == vdev->opmode) {
		if (soc->intr_mode == DP_INTR_POLL)
			qdf_timer_sync_cancel(&soc->int_timer);
		pdev->monitor_vdev = NULL;
	}

	if (vdev->vdev_dp_ext_handle) {
		qdf_mem_free(vdev->vdev_dp_ext_handle);
		vdev->vdev_dp_ext_handle = NULL;
	}

	dp_info("deleting vdev object %pK ("QDF_MAC_ADDR_FMT")", vdev,
		QDF_MAC_ADDR_REF(vdev->mac_addr.raw));

	qdf_mem_free(vdev);

	if (callback)
		callback(cb_context);

	return QDF_STATUS_SUCCESS;
}

#ifdef FEATURE_AST
/*
 * dp_peer_delete_ast_entries(): Delete all AST entries for a peer
 * @soc - datapath soc handle
 * @peer - datapath peer handle
 *
 * Delete the AST entries belonging to a peer
 */
static inline void dp_peer_delete_ast_entries(struct dp_soc *soc,
					      struct dp_peer *peer)
{
	struct dp_ast_entry *ast_entry, *temp_ast_entry;

	dp_debug("peer: %pK, self_ast: %pK", peer, peer->self_ast_entry);
	DP_PEER_ITERATE_ASE_LIST(peer, ast_entry, temp_ast_entry)
		dp_peer_del_ast(soc, ast_entry);

	peer->self_ast_entry = NULL;
}
#else
static inline void dp_peer_delete_ast_entries(struct dp_soc *soc,
					      struct dp_peer *peer)
{
}
#endif
#if ATH_SUPPORT_WRAP
static inline struct dp_peer *dp_peer_can_reuse(struct dp_vdev *vdev,
						uint8_t *peer_mac_addr)
{
	struct dp_peer *peer;

	peer = dp_peer_find_hash_find(vdev->pdev->soc, peer_mac_addr,
				      0, vdev->vdev_id);
	if (!peer)
		return NULL;

	if (peer->bss_peer)
		return peer;

	dp_peer_unref_delete(peer);
	return NULL;
}
#else
static inline struct dp_peer *dp_peer_can_reuse(struct dp_vdev *vdev,
						uint8_t *peer_mac_addr)
{
	struct dp_peer *peer;

	peer = dp_peer_find_hash_find(vdev->pdev->soc, peer_mac_addr,
				      0, vdev->vdev_id);
	if (!peer)
		return NULL;

	if (peer->bss_peer && (peer->vdev->vdev_id == vdev->vdev_id))
		return peer;

	dp_peer_unref_delete(peer);
	return NULL;
}
#endif

#ifdef FEATURE_AST
static inline void dp_peer_ast_handle_roam_del(struct dp_soc *soc,
					       struct dp_pdev *pdev,
					       uint8_t *peer_mac_addr)
{
	struct dp_ast_entry *ast_entry;

	qdf_spin_lock_bh(&soc->ast_lock);
	if (soc->ast_override_support)
		ast_entry = dp_peer_ast_hash_find_by_pdevid(soc, peer_mac_addr,
							    pdev->pdev_id);
	else
		ast_entry = dp_peer_ast_hash_find_soc(soc, peer_mac_addr);

	if (ast_entry && ast_entry->next_hop && !ast_entry->delete_in_progress)
		dp_peer_del_ast(soc, ast_entry);

	qdf_spin_unlock_bh(&soc->ast_lock);
}
#endif

#ifdef PEER_CACHE_RX_PKTS
static inline void dp_peer_rx_bufq_resources_init(struct dp_peer *peer)
{
	qdf_spinlock_create(&peer->bufq_info.bufq_lock);
	peer->bufq_info.thresh = DP_RX_CACHED_BUFQ_THRESH;
	qdf_list_create(&peer->bufq_info.cached_bufq, DP_RX_CACHED_BUFQ_THRESH);
}
#else
static inline void dp_peer_rx_bufq_resources_init(struct dp_peer *peer)
{
}
#endif

/*
 * dp_peer_create_wifi3() - attach txrx peer
 * @soc_hdl: Datapath soc handle
 * @vdev_id: id of vdev
 * @peer_mac_addr: Peer MAC address
 *
 * Return: 0 on success, -1 on failure
 */
static QDF_STATUS
dp_peer_create_wifi3(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		     uint8_t *peer_mac_addr)
{
	struct dp_peer *peer;
	int i;
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_pdev *pdev;
	struct cdp_peer_cookie peer_cookie;
	enum cdp_txrx_ast_entry_type ast_type = CDP_TXRX_AST_TYPE_STATIC;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev || !peer_mac_addr)
		return QDF_STATUS_E_FAILURE;

	pdev = vdev->pdev;
	soc = pdev->soc;

	/*
	 * If a peer entry with given MAC address already exists,
	 * reuse the peer and reset the state of peer.
	 */
	peer = dp_peer_can_reuse(vdev, peer_mac_addr);

	if (peer) {
		qdf_atomic_init(&peer->is_default_route_set);
		dp_peer_cleanup(vdev, peer, true);

		qdf_spin_lock_bh(&soc->ast_lock);
		dp_peer_delete_ast_entries(soc, peer);
		peer->delete_in_progress = false;
		qdf_spin_unlock_bh(&soc->ast_lock);

		if ((vdev->opmode == wlan_op_mode_sta) &&
		    !qdf_mem_cmp(peer_mac_addr, &vdev->mac_addr.raw[0],
		     QDF_MAC_ADDR_SIZE)) {
			ast_type = CDP_TXRX_AST_TYPE_SELF;
		}
		dp_peer_add_ast(soc, peer, peer_mac_addr, ast_type, 0);
		/*
		* Control path maintains a node count which is incremented
		* for every new peer create command. Since new peer is not being
		* created and earlier reference is reused here,
		* peer_unref_delete event is sent to control path to
		* increment the count back.
		*/
		if (soc->cdp_soc.ol_ops->peer_unref_delete) {
			soc->cdp_soc.ol_ops->peer_unref_delete(
				soc->ctrl_psoc,
				pdev->pdev_id,
				peer->mac_addr.raw, vdev->mac_addr.raw,
				vdev->opmode);
		}

		peer->valid = 1;
		dp_local_peer_id_alloc(pdev, peer);

		qdf_spinlock_create(&peer->peer_info_lock);
		dp_peer_rx_bufq_resources_init(peer);

		DP_STATS_INIT(peer);
		DP_STATS_UPD(peer, rx.avg_rssi, INVALID_RSSI);

		return QDF_STATUS_SUCCESS;
	} else {
		/*
		 * When a STA roams from RPTR AP to ROOT AP and vice versa, we
		 * need to remove the AST entry which was earlier added as a WDS
		 * entry.
		 * If an AST entry exists, but no peer entry exists with a given
		 * MAC addresses, we could deduce it as a WDS entry
		 */
		dp_peer_ast_handle_roam_del(soc, pdev, peer_mac_addr);
	}

#ifdef notyet
	peer = (struct dp_peer *)qdf_mempool_alloc(soc->osdev,
		soc->mempool_ol_ath_peer);
#else
	peer = (struct dp_peer *)qdf_mem_malloc(sizeof(*peer));
#endif

	if (!peer)
		return QDF_STATUS_E_FAILURE; /* failure */

	qdf_mem_zero(peer, sizeof(struct dp_peer));

	TAILQ_INIT(&peer->ast_entry_list);

	/* store provided params */
	peer->vdev = vdev;

	if ((vdev->opmode == wlan_op_mode_sta) &&
	    !qdf_mem_cmp(peer_mac_addr, &vdev->mac_addr.raw[0],
			 QDF_MAC_ADDR_SIZE)) {
		ast_type = CDP_TXRX_AST_TYPE_SELF;
	}
	dp_peer_add_ast(soc, peer, peer_mac_addr, ast_type, 0);
	qdf_spinlock_create(&peer->peer_info_lock);

	dp_peer_rx_bufq_resources_init(peer);

	qdf_mem_copy(
		&peer->mac_addr.raw[0], peer_mac_addr, QDF_MAC_ADDR_SIZE);

	/* initialize the peer_id */
	for (i = 0; i < MAX_NUM_PEER_ID_PER_PEER; i++)
		peer->peer_ids[i] = HTT_INVALID_PEER;

	/* reset the ast index to flowid table */
	dp_peer_reset_flowq_map(peer);

	qdf_spin_lock_bh(&soc->peer_ref_mutex);

	qdf_atomic_init(&peer->ref_cnt);

	/* keep one reference for attach */
	qdf_atomic_inc(&peer->ref_cnt);

	/* add this peer into the vdev's list */
	if (wlan_op_mode_sta == vdev->opmode)
		TAILQ_INSERT_HEAD(&vdev->peer_list, peer, peer_list_elem);
	else
		TAILQ_INSERT_TAIL(&vdev->peer_list, peer, peer_list_elem);

	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	/* TODO: See if hash based search is required */
	dp_peer_find_hash_add(soc, peer);

	/* Initialize the peer state */
	peer->state = OL_TXRX_PEER_STATE_DISC;

	dp_info("vdev %pK created peer %pK ("QDF_MAC_ADDR_FMT") ref_cnt: %d",
		vdev, peer, QDF_MAC_ADDR_REF(peer->mac_addr.raw),
		qdf_atomic_read(&peer->ref_cnt));
	/*
	 * For every peer MAp message search and set if bss_peer
	 */
	if (qdf_mem_cmp(peer->mac_addr.raw, vdev->mac_addr.raw,
			QDF_MAC_ADDR_SIZE) == 0 &&
			(wlan_op_mode_sta != vdev->opmode)) {
		dp_info("vdev bss_peer!!");
		peer->bss_peer = 1;
		vdev->vap_bss_peer = peer;
	}

	if (wlan_op_mode_sta == vdev->opmode &&
	    qdf_mem_cmp(peer->mac_addr.raw, vdev->mac_addr.raw,
			QDF_MAC_ADDR_SIZE) == 0) {
		vdev->vap_self_peer = peer;
	}

	for (i = 0; i < DP_MAX_TIDS; i++)
		qdf_spinlock_create(&peer->rx_tid[i].tid_lock);

	peer->valid = 1;
	dp_local_peer_id_alloc(pdev, peer);
	DP_STATS_INIT(peer);
	DP_STATS_UPD(peer, rx.avg_rssi, INVALID_RSSI);

	qdf_mem_copy(peer_cookie.mac_addr, peer->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
	peer_cookie.ctx = NULL;
	peer_cookie.pdev_id = pdev->pdev_id;
	peer_cookie.cookie = pdev->next_peer_cookie++;
#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
	dp_wdi_event_handler(WDI_EVENT_PEER_CREATE, pdev->soc,
			     (void *)&peer_cookie,
			     peer->peer_ids[0], WDI_NO_VAL, pdev->pdev_id);
#endif
	if (soc->wlanstats_enabled) {
		if (!peer_cookie.ctx) {
			pdev->next_peer_cookie--;
			qdf_err("Failed to initialize peer rate stats");
		} else {
			peer->wlanstats_ctx = (struct cdp_peer_rate_stats_ctx *)
						peer_cookie.ctx;
		}
	}
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_vdev_get_default_reo_hash() - get reo dest ring and hash values for a vdev
 * @vdev: Datapath VDEV handle
 * @reo_dest: pointer to default reo_dest ring for vdev to be populated
 * @hash_based: pointer to hash value (enabled/disabled) to be populated
 *
 * Return: None
 */
static
void dp_vdev_get_default_reo_hash(struct dp_vdev *vdev,
				  enum cdp_host_reo_dest_ring *reo_dest,
				  bool *hash_based)
{
	struct dp_soc *soc;
	struct dp_pdev *pdev;

	pdev = vdev->pdev;
	soc = pdev->soc;
	/*
	 * hash based steering is disabled for Radios which are offloaded
	 * to NSS
	 */
	if (!wlan_cfg_get_dp_pdev_nss_enabled(pdev->wlan_cfg_ctx))
		*hash_based = wlan_cfg_is_rx_hash_enabled(soc->wlan_cfg_ctx);

	/*
	 * Below line of code will ensure the proper reo_dest ring is chosen
	 * for cases where toeplitz hash cannot be generated (ex: non TCP/UDP)
	 */
	*reo_dest = pdev->reo_dest;
}

#ifdef IPA_OFFLOAD
/**
 * dp_is_vdev_subtype_p2p() - Check if the subtype for vdev is P2P
 * @vdev: Virtual device
 *
 * Return: true if the vdev is of subtype P2P
 *	   false if the vdev is of any other subtype
 */
static inline bool dp_is_vdev_subtype_p2p(struct dp_vdev *vdev)
{
	if (vdev->subtype == wlan_op_subtype_p2p_device ||
	    vdev->subtype == wlan_op_subtype_p2p_cli ||
	    vdev->subtype == wlan_op_subtype_p2p_go)
		return true;

	return false;
}

/*
 * dp_peer_setup_get_reo_hash() - get reo dest ring and hash values for a peer
 * @vdev: Datapath VDEV handle
 * @reo_dest: pointer to default reo_dest ring for vdev to be populated
 * @hash_based: pointer to hash value (enabled/disabled) to be populated
 *
 * If IPA is enabled in ini, for SAP mode, disable hash based
 * steering, use default reo_dst ring for RX. Use config values for other modes.
 * Return: None
 */
static void dp_peer_setup_get_reo_hash(struct dp_vdev *vdev,
				       enum cdp_host_reo_dest_ring *reo_dest,
				       bool *hash_based)
{
	struct dp_soc *soc;
	struct dp_pdev *pdev;

	pdev = vdev->pdev;
	soc = pdev->soc;

	dp_vdev_get_default_reo_hash(vdev, reo_dest, hash_based);

	/* For P2P-GO interfaces we do not need to change the REO
	 * configuration even if IPA config is enabled
	 */
	if (dp_is_vdev_subtype_p2p(vdev))
		return;

	/*
	 * If IPA is enabled, disable hash-based flow steering and set
	 * reo_dest_ring_4 as the REO ring to receive packets on.
	 * IPA is configured to reap reo_dest_ring_4.
	 *
	 * Note - REO DST indexes are from 0 - 3, while cdp_host_reo_dest_ring
	 * value enum value is from 1 - 4.
	 * Hence, *reo_dest = IPA_REO_DEST_RING_IDX + 1
	 */
	if (wlan_cfg_is_ipa_enabled(soc->wlan_cfg_ctx)) {
		if (vdev->opmode == wlan_op_mode_ap) {
			*reo_dest = IPA_REO_DEST_RING_IDX + 1;
			*hash_based = 0;
		} else if (vdev->opmode == wlan_op_mode_sta &&
			   dp_ipa_is_mdm_platform()) {
			*reo_dest = IPA_REO_DEST_RING_IDX + 1;
		}
	}
}

#else

/*
 * dp_peer_setup_get_reo_hash() - get reo dest ring and hash values for a peer
 * @vdev: Datapath VDEV handle
 * @reo_dest: pointer to default reo_dest ring for vdev to be populated
 * @hash_based: pointer to hash value (enabled/disabled) to be populated
 *
 * Use system config values for hash based steering.
 * Return: None
 */

static void dp_peer_setup_get_reo_hash(struct dp_vdev *vdev,
				       enum cdp_host_reo_dest_ring *reo_dest,
				       bool *hash_based)
{
	dp_vdev_get_default_reo_hash(vdev, reo_dest, hash_based);
}
#endif /* IPA_OFFLOAD */

/*
 * dp_peer_setup_wifi3() - initialize the peer
 * @soc_hdl: soc handle object
 * @vdev_id : vdev_id of vdev object
 * @peer_mac: Peer's mac address
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_peer_setup_wifi3(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		    uint8_t *peer_mac)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_pdev *pdev;
	bool hash_based = 0;
	enum cdp_host_reo_dest_ring reo_dest;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_vdev *vdev =
			dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);
	struct dp_peer *peer =
			dp_peer_find_hash_find(soc, peer_mac, 0, vdev_id);

	if (!vdev || !peer || peer->delete_in_progress) {
		status = QDF_STATUS_E_FAILURE;
		goto fail;
	}

	pdev = vdev->pdev;
	dp_peer_setup_get_reo_hash(vdev, &reo_dest, &hash_based);

	dp_info("pdev: %d vdev :%d opmode:%u hash-based-steering:%d default-reo_dest:%u",
		pdev->pdev_id, vdev->vdev_id,
		vdev->opmode, hash_based, reo_dest);


	/*
	 * There are corner cases where the AD1 = AD2 = "VAPs address"
	 * i.e both the devices have same MAC address. In these
	 * cases we want such pkts to be processed in NULL Q handler
	 * which is REO2TCL ring. for this reason we should
	 * not setup reo_queues and default route for bss_peer.
	 */
	if (peer->bss_peer && vdev->opmode == wlan_op_mode_ap) {
		status = QDF_STATUS_E_FAILURE;
		goto fail;
	}

	if (soc->cdp_soc.ol_ops->peer_set_default_routing) {
		/* TODO: Check the destination ring number to be passed to FW */
		soc->cdp_soc.ol_ops->peer_set_default_routing(
				soc->ctrl_psoc,
				peer->vdev->pdev->pdev_id,
				peer->mac_addr.raw,
				peer->vdev->vdev_id, hash_based, reo_dest);
	}

	qdf_atomic_set(&peer->is_default_route_set, 1);

	dp_peer_rx_init(pdev, peer);
	dp_peer_tx_init(pdev, peer);

	dp_peer_ppdu_delayed_ba_init(peer);

fail:
	if (peer)
		dp_peer_unref_delete(peer);
	return status;
}

/*
 * dp_cp_peer_del_resp_handler - Handle the peer delete response
 * @soc_hdl: Datapath SOC handle
 * @vdev_id: id of virtual device object
 * @mac_addr: Mac address of the peer
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_cp_peer_del_resp_handler(struct cdp_soc_t *soc_hdl,
					      uint8_t vdev_id,
					      uint8_t *mac_addr)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_ast_entry  *ast_entry = NULL;
	txrx_ast_free_cb cb = NULL;
	void *cookie;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&soc->ast_lock);

	if (soc->ast_override_support)
		ast_entry =
			dp_peer_ast_hash_find_by_pdevid(soc, mac_addr,
							vdev->pdev->pdev_id);
	else
		ast_entry = dp_peer_ast_hash_find_soc(soc, mac_addr);

	/* in case of qwrap we have multiple BSS peers
	 * with same mac address
	 *
	 * AST entry for this mac address will be created
	 * only for one peer hence it will be NULL here
	 */
	if (!ast_entry || ast_entry->peer || !ast_entry->delete_in_progress) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return QDF_STATUS_E_FAILURE;
	}

	if (ast_entry->is_mapped)
		soc->ast_table[ast_entry->ast_idx] = NULL;

	DP_STATS_INC(soc, ast.deleted, 1);
	dp_peer_ast_hash_remove(soc, ast_entry);

	cb = ast_entry->callback;
	cookie = ast_entry->cookie;
	ast_entry->callback = NULL;
	ast_entry->cookie = NULL;

	soc->num_ast_entries--;
	qdf_spin_unlock_bh(&soc->ast_lock);

	if (cb) {
		cb(soc->ctrl_psoc,
		   dp_soc_to_cdp_soc(soc),
		   cookie,
		   CDP_TXRX_AST_DELETED);
	}
	qdf_mem_free(ast_entry);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_ba_aging_timeout() - set ba aging timeout per AC
 * @txrx_soc: cdp soc handle
 * @ac: Access category
 * @value: timeout value in millisec
 *
 * Return: void
 */
static void dp_set_ba_aging_timeout(struct cdp_soc_t *txrx_soc,
				    uint8_t ac, uint32_t value)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	hal_set_ba_aging_timeout(soc->hal_soc, ac, value);
}

/*
 * dp_get_ba_aging_timeout() - get ba aging timeout per AC
 * @txrx_soc: cdp soc handle
 * @ac: access category
 * @value: timeout value in millisec
 *
 * Return: void
 */
static void dp_get_ba_aging_timeout(struct cdp_soc_t *txrx_soc,
				    uint8_t ac, uint32_t *value)
{
	struct dp_soc *soc = (struct dp_soc *)txrx_soc;

	hal_get_ba_aging_timeout(soc->hal_soc, ac, value);
}

/*
 * dp_set_pdev_reo_dest() - set the reo destination ring for this pdev
 * @txrx_soc: cdp soc handle
 * @pdev_id: id of physical device object
 * @val: reo destination ring index (1 - 4)
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_set_pdev_reo_dest(struct cdp_soc_t *txrx_soc, uint8_t pdev_id,
		     enum cdp_host_reo_dest_ring val)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)txrx_soc,
						   pdev_id);

	if (pdev) {
		pdev->reo_dest = val;
		return QDF_STATUS_SUCCESS;
	}

	return QDF_STATUS_E_FAILURE;
}

/*
 * dp_get_pdev_reo_dest() - get the reo destination for this pdev
 * @txrx_soc: cdp soc handle
 * @pdev_id: id of physical device object
 *
 * Return: reo destination ring index
 */
static enum cdp_host_reo_dest_ring
dp_get_pdev_reo_dest(struct cdp_soc_t *txrx_soc, uint8_t pdev_id)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)txrx_soc,
						   pdev_id);

	if (pdev)
		return pdev->reo_dest;
	else
		return cdp_host_reo_dest_ring_unknown;
}

#ifdef ATH_SUPPORT_NAC
/*
 * dp_set_filter_neigh_peers() - set filter neighbour peers for smart mesh
 * @pdev_handle: device object
 * @val: value to be set
 *
 * Return: void
 */
static int dp_set_filter_neigh_peers(struct dp_pdev *pdev,
				     bool val)
{
	/* Enable/Disable smart mesh filtering. This flag will be checked
	 * during rx processing to check if packets are from NAC clients.
	 */
	pdev->filter_neighbour_peers = val;
	return 0;
}
#else
static int dp_set_filter_neigh_peers(struct dp_pdev *pdev,
				     bool val)
{
	return 0;
}
#endif /* ATH_SUPPORT_NAC */

#if defined(ATH_SUPPORT_NAC_RSSI) || defined(ATH_SUPPORT_NAC)
/*
 * dp_update_filter_neighbour_peers() - set neighbour peers(nac clients)
 * address for smart mesh filtering
 * @txrx_soc: cdp soc handle
 * @vdev_id: id of virtual device object
 * @cmd: Add/Del command
 * @macaddr: nac client mac address
 *
 * Return: success/failure
 */
static int dp_update_filter_neighbour_peers(struct cdp_soc_t *soc,
					    uint8_t vdev_id,
					    uint32_t cmd, uint8_t *macaddr)
{
	struct dp_pdev *pdev;
	struct dp_neighbour_peer *peer = NULL;
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev || !macaddr)
		goto fail0;

	pdev = vdev->pdev;

	if (!pdev)
		goto fail0;

	/* Store address of NAC (neighbour peer) which will be checked
	 * against TA of received packets.
	 */
	if (cmd == DP_NAC_PARAM_ADD) {
		peer = (struct dp_neighbour_peer *) qdf_mem_malloc(
				sizeof(*peer));

		if (!peer) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				FL("DP neighbour peer node memory allocation failed"));
			goto fail0;
		}

		qdf_mem_copy(&peer->neighbour_peers_macaddr.raw[0],
			macaddr, QDF_MAC_ADDR_SIZE);
		peer->vdev = vdev;

		qdf_spin_lock_bh(&pdev->neighbour_peer_mutex);

		/* add this neighbour peer into the list */
		TAILQ_INSERT_TAIL(&pdev->neighbour_peers_list, peer,
				neighbour_peer_list_elem);
		qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);

		/* first neighbour */
		if (!pdev->neighbour_peers_added) {
			QDF_STATUS status = QDF_STATUS_SUCCESS;

			pdev->neighbour_peers_added = true;
			dp_mon_filter_setup_smart_monitor(pdev);
			status = dp_mon_filter_update(pdev);
			if (status != QDF_STATUS_SUCCESS) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL("smart mon filter setup failed"));
				dp_mon_filter_reset_smart_monitor(pdev);
				pdev->neighbour_peers_added = false;
			}
		}
		return 1;

	} else if (cmd == DP_NAC_PARAM_DEL) {
		qdf_spin_lock_bh(&pdev->neighbour_peer_mutex);
		TAILQ_FOREACH(peer, &pdev->neighbour_peers_list,
				neighbour_peer_list_elem) {
			if (!qdf_mem_cmp(&peer->neighbour_peers_macaddr.raw[0],
				macaddr, QDF_MAC_ADDR_SIZE)) {
				/* delete this peer from the list */
				TAILQ_REMOVE(&pdev->neighbour_peers_list,
					peer, neighbour_peer_list_elem);
				qdf_mem_free(peer);
				break;
			}
		}
		/* last neighbour deleted */
		if (TAILQ_EMPTY(&pdev->neighbour_peers_list)) {
			QDF_STATUS status = QDF_STATUS_SUCCESS;

			pdev->neighbour_peers_added = false;
			dp_mon_filter_reset_smart_monitor(pdev);
			status = dp_mon_filter_update(pdev);
			if (status != QDF_STATUS_SUCCESS) {
				QDF_TRACE(QDF_MODULE_ID_DP,
					  QDF_TRACE_LEVEL_ERROR,
					  FL("smart mon filter clear failed"));
			}

		}

		qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);
		return 1;

	}

fail0:
	return 0;
}
#endif /* ATH_SUPPORT_NAC_RSSI || ATH_SUPPORT_NAC */

/*
 * dp_get_sec_type() - Get the security type
 * @soc: soc handle
 * @vdev_id: id of dp handle
 * @peer_mac: mac of datapath PEER handle
 * @sec_idx:    Security id (mcast, ucast)
 *
 * return sec_type: Security type
 */
static int dp_get_sec_type(struct cdp_soc_t *soc, uint8_t vdev_id,
			   uint8_t *peer_mac, uint8_t sec_idx)
{
	int sec_type = 0;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc,
						       peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Peer is NULL!\n", __func__);
		goto fail;
	}

	sec_type = peer->security[sec_idx].sec_type;
fail:
	if (peer)
		dp_peer_unref_delete(peer);
	return sec_type;
}

/*
 * dp_peer_authorize() - authorize txrx peer
 * @soc: soc handle
 * @vdev_id: id of dp handle
 * @peer_mac: mac of datapath PEER handle
 * @authorize
 *
 */
static QDF_STATUS
dp_peer_authorize(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		  uint8_t *peer_mac, uint32_t authorize)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_peer *peer = dp_peer_find_hash_find(soc,
						      peer_mac,
						      0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Peer is NULL!\n", __func__);
		status = QDF_STATUS_E_FAILURE;
	} else {
		qdf_spin_lock_bh(&soc->peer_ref_mutex);
		peer->authorize = authorize ? 1 : 0;
		qdf_spin_unlock_bh(&soc->peer_ref_mutex);
	}

	if (peer)
		dp_peer_unref_delete(peer);

	return status;
}

/*
 * dp_vdev_reset_peer() - Update peer related member in vdev
			  as peer is going to free
 * @vdev: datapath vdev handle
 * @peer: dataptah peer handle
 *
 * Return: None
 */
static void dp_vdev_reset_peer(struct dp_vdev *vdev,
			       struct dp_peer *peer)
{
	struct dp_peer *bss_peer = NULL;

	if (!vdev) {
		dp_err("vdev is NULL");
	} else {
		if (vdev->vap_bss_peer == peer) {
			vdev->vap_bss_peer = NULL;
			qdf_mem_zero(vdev->vap_bss_peer_mac_addr,
				     QDF_MAC_ADDR_SIZE);
		}

		if (vdev && vdev->vap_bss_peer) {
		    bss_peer = vdev->vap_bss_peer;
		    DP_UPDATE_STATS(vdev, peer);
		}
	}
}

/*
 * dp_peer_release_mem() - free dp peer handle memory
 * @soc: dataptah soc handle
 * @pdev: datapath pdev handle
 * @peer: datapath peer handle
 * @vdev_opmode: Vdev operation mode
 * @vdev_mac_addr: Vdev Mac address
 *
 * Return: None
 */
static void dp_peer_release_mem(struct dp_soc *soc,
				struct dp_pdev *pdev,
				struct dp_peer *peer,
				enum wlan_op_mode vdev_opmode,
				uint8_t *vdev_mac_addr)
{
	if (soc->cdp_soc.ol_ops->peer_unref_delete)
		soc->cdp_soc.ol_ops->peer_unref_delete(
				soc->ctrl_psoc,
				pdev->pdev_id,
				peer->mac_addr.raw, vdev_mac_addr,
				vdev_opmode);

	/*
	 * Peer AST list hast to be empty here
	 */
	DP_AST_ASSERT(TAILQ_EMPTY(&peer->ast_entry_list));

	qdf_mem_free(peer);
}

/**
 * dp_delete_pending_vdev() - check and process vdev delete
 * @pdev: DP specific pdev pointer
 * @vdev: DP specific vdev pointer
 * @vdev_id: vdev id corresponding to vdev
 *
 * This API does following:
 * 1) It releases tx flow pools buffers as vdev is
 *    going down and no peers are associated.
 * 2) It also detaches vdev before cleaning vdev (struct dp_vdev) memory
 */
static void dp_delete_pending_vdev(struct dp_pdev *pdev, struct dp_vdev *vdev,
				   uint8_t vdev_id)
{
	ol_txrx_vdev_delete_cb vdev_delete_cb = NULL;
	void *vdev_delete_context = NULL;

	vdev_delete_cb = vdev->delete.callback;
	vdev_delete_context = vdev->delete.context;

	dp_info("deleting vdev object %pK ("QDF_MAC_ADDR_FMT")- its last peer is done",
		vdev, QDF_MAC_ADDR_REF(vdev->mac_addr.raw));
	/* all peers are gone, go ahead and delete it */
	dp_tx_flow_pool_unmap_handler(pdev, vdev_id,
			FLOW_TYPE_VDEV, vdev_id);
	dp_tx_vdev_detach(vdev);

	pdev->soc->vdev_id_map[vdev_id] = NULL;

	if (wlan_op_mode_monitor == vdev->opmode) {
		pdev->monitor_vdev = NULL;
	} else {
		qdf_spin_lock_bh(&pdev->vdev_list_lock);
		TAILQ_REMOVE(&pdev->vdev_list, vdev, vdev_list_elem);
		qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	}

	dp_info("deleting vdev object %pK ("QDF_MAC_ADDR_FMT")",
		vdev, QDF_MAC_ADDR_REF(vdev->mac_addr.raw));
	qdf_mem_free(vdev);
	vdev = NULL;

	if (vdev_delete_cb)
		vdev_delete_cb(vdev_delete_context);
}

/*
 * dp_peer_unref_delete() - unref and delete peer
 * @peer_handle:		Datapath peer handle
 *
 */
void dp_peer_unref_delete(struct dp_peer *peer)
{
	struct dp_vdev *vdev = peer->vdev;
	struct dp_pdev *pdev = vdev->pdev;
	struct dp_soc *soc = pdev->soc;
	struct dp_peer *tmppeer;
	int found = 0;
	uint16_t peer_id;
	uint16_t vdev_id;
	bool vdev_delete = false;
	struct cdp_peer_cookie peer_cookie;
	enum wlan_op_mode vdev_opmode;
	uint8_t vdev_mac_addr[QDF_MAC_ADDR_SIZE];

	/*
	 * Hold the lock all the way from checking if the peer ref count
	 * is zero until the peer references are removed from the hash
	 * table and vdev list (if the peer ref count is zero).
	 * This protects against a new HL tx operation starting to use the
	 * peer object just after this function concludes it's done being used.
	 * Furthermore, the lock needs to be held while checking whether the
	 * vdev's list of peers is empty, to make sure that list is not modified
	 * concurrently with the empty check.
	 */
	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	if (qdf_atomic_dec_and_test(&peer->ref_cnt)) {
		peer_id = peer->peer_ids[0];
		vdev_id = vdev->vdev_id;

		/*
		 * Make sure that the reference to the peer in
		 * peer object map is removed
		 */
		if (peer_id != HTT_INVALID_PEER)
			soc->peer_id_to_obj_map[peer_id] = NULL;

		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "Deleting peer %pK ("QDF_MAC_ADDR_FMT")", peer,
			  QDF_MAC_ADDR_REF(peer->mac_addr.raw));

		/* remove the reference to the peer from the hash table */
		dp_peer_find_hash_remove(soc, peer);

		qdf_spin_lock_bh(&soc->ast_lock);
		if (peer->self_ast_entry) {
			dp_peer_del_ast(soc, peer->self_ast_entry);
		}
		qdf_spin_unlock_bh(&soc->ast_lock);

		TAILQ_FOREACH(tmppeer, &peer->vdev->peer_list, peer_list_elem) {
			if (tmppeer == peer) {
				found = 1;
				break;
			}
		}

		if (found) {
			TAILQ_REMOVE(&peer->vdev->peer_list, peer,
				peer_list_elem);
		} else {
			/*Ignoring the remove operation as peer not found*/
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
				  "peer:%pK not found in vdev:%pK peerlist:%pK",
				  peer, vdev, &peer->vdev->peer_list);
		}

		/* send peer destroy event to upper layer */
		qdf_mem_copy(peer_cookie.mac_addr, peer->mac_addr.raw,
			     QDF_MAC_ADDR_SIZE);
		peer_cookie.ctx = NULL;
		peer_cookie.ctx = (struct cdp_stats_cookie *)
					peer->wlanstats_ctx;
#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
		dp_wdi_event_handler(WDI_EVENT_PEER_DESTROY,
				     pdev->soc,
				     (void *)&peer_cookie,
				     peer->peer_ids[0],
				     WDI_NO_VAL,
				     pdev->pdev_id);
#endif
		peer->wlanstats_ctx = NULL;

		/* cleanup the peer data */
		dp_peer_cleanup(vdev, peer, false);
		/* reset this peer related info in vdev */
		dp_vdev_reset_peer(vdev, peer);
		/* save vdev related member in case vdev freed */
		vdev_opmode = vdev->opmode;
		qdf_mem_copy(vdev_mac_addr, vdev->mac_addr.raw,
			     QDF_MAC_ADDR_SIZE);
		/*
		 * check whether the parent vdev is pending for deleting
		 * and no peers left.
		 */
		if (vdev->delete.pending && TAILQ_EMPTY(&vdev->peer_list))
			vdev_delete = true;
		/*
		 * Now that there are no references to the peer, we can
		 * release the peer reference lock.
		 */
		qdf_spin_unlock_bh(&soc->peer_ref_mutex);

		/*
		 * Invoke soc.ol_ops->peer_unref_delete out of
		 * peer_ref_mutex in case deadlock issue.
		 */
		dp_peer_release_mem(soc, pdev, peer,
				    vdev_opmode,
				    vdev_mac_addr);
		/*
		 * Delete the vdev if it's waiting all peer deleted
		 * and it's chance now.
		 */
		if (vdev_delete)
			dp_delete_pending_vdev(pdev, vdev, vdev_id);

	} else {
		qdf_spin_unlock_bh(&soc->peer_ref_mutex);
	}
}

#ifdef PEER_CACHE_RX_PKTS
static inline void dp_peer_rx_bufq_resources_deinit(struct dp_peer *peer)
{
	qdf_list_destroy(&peer->bufq_info.cached_bufq);
	qdf_spinlock_destroy(&peer->bufq_info.bufq_lock);
}
#else
static inline void dp_peer_rx_bufq_resources_deinit(struct dp_peer *peer)
{
}
#endif

/*
 * dp_peer_detach_wifi3() – Detach txrx peer
 * @soc_hdl: soc handle
 * @vdev_id: id of dp handle
 * @peer_mac: mac of datapath PEER handle
 * @bitmap: bitmap indicating special handling of request.
 *
 */
static QDF_STATUS dp_peer_delete_wifi3(struct cdp_soc_t *soc_hdl,
				       uint8_t vdev_id,
				       uint8_t *peer_mac, uint32_t bitmap)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_peer *peer = dp_peer_find_hash_find(soc, peer_mac,
						      0, vdev_id);

	/* Peer can be null for monitor vap mac address */
	if (!peer) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Invalid peer\n", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	if (!peer->valid) {
		dp_peer_unref_delete(peer);
		dp_err("Invalid peer: "QDF_MAC_ADDR_FMT,
			QDF_MAC_ADDR_REF(peer_mac));
		return QDF_STATUS_E_ALREADY;
	}

	peer->valid = 0;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO_HIGH,
		FL("peer %pK ("QDF_MAC_ADDR_FMT")"),  peer,
		  QDF_MAC_ADDR_REF(peer->mac_addr.raw));

	dp_local_peer_id_free(peer->vdev->pdev, peer);

	/* Drop all rx packets before deleting peer */
	dp_clear_peer_internal(soc, peer);

	dp_peer_rx_bufq_resources_deinit(peer);

	qdf_spinlock_destroy(&peer->peer_info_lock);
	dp_peer_multipass_list_remove(peer);

	/*
	 * Remove the reference added during peer_attach.
	 * The peer will still be left allocated until the
	 * PEER_UNMAP message arrives to remove the other
	 * reference, added by the PEER_MAP message.
	 */
	dp_peer_unref_delete(peer);
	/*
	 * Remove the reference taken above
	 */
	dp_peer_unref_delete(peer);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_vdev_mac_addr_wifi3() – Detach txrx peer
 * @soc_hdl: Datapath soc handle
 * @vdev_id: virtual interface id
 *
 * Return: MAC address on success, NULL on failure.
 *
 */
static uint8 *dp_get_vdev_mac_addr_wifi3(struct cdp_soc_t *soc_hdl,
					 uint8_t vdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return NULL;

	return vdev->mac_addr.raw;
}

/*
 * dp_vdev_set_wds() - Enable per packet stats
 * @soc: DP soc handle
 * @vdev_id: id of DP VDEV handle
 * @val: value
 *
 * Return: none
 */
static int dp_vdev_set_wds(struct cdp_soc_t *soc, uint8_t vdev_id, uint32_t val)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	vdev->wds_enabled = val;
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_mon_vdev_from_pdev_wifi3() - Get vdev id of monitor mode
 * @soc_hdl: datapath soc handle
 * @pdev_id: physical device instance id
 *
 * Return: virtual interface id
 */
static uint8_t dp_get_mon_vdev_from_pdev_wifi3(struct cdp_soc_t *soc_hdl,
					       uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (qdf_unlikely(!pdev))
		return -EINVAL;

	return pdev->monitor_vdev->vdev_id;
}

static int dp_get_opmode(struct cdp_soc_t *soc_hdl, uint8_t vdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev) {
		dp_err("vdev for id %d is NULL", vdev_id);
		return -EINVAL;
	}

	return vdev->opmode;
}

/**
 * dp_get_os_rx_handles_from_vdev_wifi3() - Get os rx handles for a vdev
 * @soc_hdl: ol_txrx_soc_handle handle
 * @vdev_id: vdev id for which os rx handles are needed
 * @stack_fn_p: pointer to stack function pointer
 * @osif_handle_p: pointer to ol_osif_vdev_handle
 *
 * Return: void
 */
static
void dp_get_os_rx_handles_from_vdev_wifi3(struct cdp_soc_t *soc_hdl,
					  uint8_t vdev_id,
					  ol_txrx_rx_fp *stack_fn_p,
					  ol_osif_vdev_handle *osif_vdev_p)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return;

	*stack_fn_p = vdev->osif_rx_stack;
	*osif_vdev_p = vdev->osif_vdev;
}

/**
 * dp_get_ctrl_pdev_from_vdev() - Get control pdev of vdev
 * @soc_hdl: datapath soc handle
 * @vdev_id: virtual device/interface id
 *
 * Return: Handle to control pdev
 */
static struct cdp_cfg *dp_get_ctrl_pdev_from_vdev_wifi3(
						struct cdp_soc_t *soc_hdl,
						uint8_t vdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);
	struct dp_pdev *pdev;

	if (!vdev || !vdev->pdev)
		return NULL;

	pdev = vdev->pdev;
	return (struct cdp_cfg *)pdev->wlan_cfg_ctx;
}

/**
 * dp_monitor_mode_ring_config() - Send the tlv config to fw for monitor buffer
 *                                 ring based on target
 * @soc: soc handle
 * @mac_for_pdev: WIN- pdev_id, MCL- mac id
 * @pdev: physical device handle
 * @ring_num: mac id
 * @htt_tlv_filter: tlv filter
 *
 * Return: zero on success, non-zero on failure
 */
static inline
QDF_STATUS dp_monitor_mode_ring_config(struct dp_soc *soc, uint8_t mac_for_pdev,
				       struct dp_pdev *pdev, uint8_t ring_num,
				       struct htt_rx_ring_tlv_filter htt_tlv_filter)
{
	QDF_STATUS status;

	if (soc->wlan_cfg_ctx->rxdma1_enable)
		status = htt_h2t_rx_ring_cfg(soc->htt_handle, mac_for_pdev,
					     soc->rxdma_mon_buf_ring[ring_num]
					     .hal_srng,
					     RXDMA_MONITOR_BUF,
					     RX_MONITOR_BUFFER_SIZE,
					     &htt_tlv_filter);
	else
		status = htt_h2t_rx_ring_cfg(soc->htt_handle, mac_for_pdev,
					     pdev->rx_mac_buf_ring[ring_num]
					     .hal_srng,
					     RXDMA_BUF, RX_DATA_BUFFER_SIZE,
					     &htt_tlv_filter);

	return status;
}

static inline void
dp_pdev_disable_mcopy_code(struct dp_pdev *pdev)
{
	pdev->mcopy_mode = 0;
	pdev->monitor_configured = false;
	pdev->monitor_vdev = NULL;
	qdf_nbuf_queue_free(&pdev->rx_ppdu_buf_q);
}

/**
 * dp_reset_monitor_mode() - Disable monitor mode
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of datapath PDEV handle
 *
 * Return: QDF_STATUS
 */
QDF_STATUS dp_reset_monitor_mode(struct cdp_soc_t *soc_hdl,
				 uint8_t pdev_id,
				 uint8_t special_monitor)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&pdev->mon_lock);

	pdev->monitor_vdev = NULL;
	pdev->monitor_configured = false;

	/*
	 * Lite monitor mode, smart monitor mode and monitor
	 * mode uses this APIs to filter reset and mode disable
	 */
	if (pdev->mcopy_mode) {
#if defined(FEATURE_PERPKT_INFO)
		dp_pdev_disable_mcopy_code(pdev);
		dp_mon_filter_reset_mcopy_mode(pdev);
#endif /* FEATURE_PERPKT_INFO */
	} else if (special_monitor) {
#if defined(ATH_SUPPORT_NAC)
		dp_mon_filter_reset_smart_monitor(pdev);
#endif /* ATH_SUPPORT_NAC */
	} else {
		dp_mon_filter_reset_mon_mode(pdev);
	}

	status = dp_mon_filter_update(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Failed to reset monitor filters"));
	}

	qdf_spin_unlock_bh(&pdev->mon_lock);
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_get_tx_pending() - read pending tx
 * @pdev_handle: Datapath PDEV handle
 *
 * Return: outstanding tx
 */
static uint32_t dp_get_tx_pending(struct cdp_pdev *pdev_handle)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_handle;

	return qdf_atomic_read(&pdev->num_tx_outstanding);
}

/**
 * dp_get_peer_mac_from_peer_id() - get peer mac
 * @pdev_handle: Datapath PDEV handle
 * @peer_id: Peer ID
 * @peer_mac: MAC addr of PEER
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_get_peer_mac_from_peer_id(struct cdp_soc_t *soc,
					       uint32_t peer_id,
					       uint8_t *peer_mac)
{
	struct dp_peer *peer;

	if (soc && peer_mac) {
		peer = dp_peer_find_by_id((struct dp_soc *)soc,
					  (uint16_t)peer_id);
		if (peer) {
			qdf_mem_copy(peer_mac, peer->mac_addr.raw,
				     QDF_MAC_ADDR_SIZE);
			dp_peer_unref_del_find_by_id(peer);
			return QDF_STATUS_SUCCESS;
		}
	}

	return QDF_STATUS_E_FAILURE;
}

/**
 * dp_vdev_set_monitor_mode() - Set DP VDEV to monitor mode
 * @vdev_handle: Datapath VDEV handle
 * @smart_monitor: Flag to denote if its smart monitor mode
 *
 * Return: 0 on success, not 0 on failure
 */
static QDF_STATUS dp_vdev_set_monitor_mode(struct cdp_soc_t *soc,
					   uint8_t vdev_id,
					   uint8_t special_monitor)
{
	struct dp_pdev *pdev;
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	pdev = vdev->pdev;
	pdev->monitor_vdev = vdev;
	QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_WARN,
		  "pdev=%pK, pdev_id=%d, soc=%pK vdev=%pK\n",
		  pdev, pdev->pdev_id, pdev->soc, vdev);

	/*
	 * do not configure monitor buf ring and filter for smart and
	 * lite monitor
	 * for smart monitor filters are added along with first NAC
	 * for lite monitor required configuration done through
	 * dp_set_pdev_param
	 */
	if (special_monitor)
		return QDF_STATUS_SUCCESS;

	/*Check if current pdev's monitor_vdev exists */
	if (pdev->monitor_configured) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			  "monitor vap already created vdev=%pK\n", vdev);
		return QDF_STATUS_E_RESOURCES;
	}

	pdev->monitor_configured = true;
	dp_mon_buf_delayed_replenish(pdev);

	dp_mon_filter_setup_mon_mode(pdev);
	status = dp_mon_filter_update(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Failed to reset monitor filters"));
		dp_mon_filter_reset_mon_mode(pdev);
		pdev->monitor_configured = false;
		pdev->monitor_vdev = NULL;
	}

	return status;
}

/**
 * dp_pdev_set_advance_monitor_filter() - Set DP PDEV monitor filter
 * @soc: soc handle
 * @pdev_id: id of Datapath PDEV handle
 * @filter_val: Flag to select Filter for monitor mode
 * Return: 0 on success, not 0 on failure
 */
static QDF_STATUS
dp_pdev_set_advance_monitor_filter(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
				   struct cdp_monitor_filter *filter_val)
{
	/* Many monitor VAPs can exists in a system but only one can be up at
	 * anytime
	 */
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_vdev *vdev;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	vdev = pdev->monitor_vdev;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_WARN,
		"pdev=%pK, pdev_id=%d, soc=%pK vdev=%pK",
		pdev, pdev_id, soc, vdev);

	/*Check if current pdev's monitor_vdev exists */
	if (!pdev->monitor_vdev) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			"vdev=%pK", vdev);
		qdf_assert(vdev);
	}

	/* update filter mode, type in pdev structure */
	pdev->mon_filter_mode = filter_val->mode;
	pdev->fp_mgmt_filter = filter_val->fp_mgmt;
	pdev->fp_ctrl_filter = filter_val->fp_ctrl;
	pdev->fp_data_filter = filter_val->fp_data;
	pdev->mo_mgmt_filter = filter_val->mo_mgmt;
	pdev->mo_ctrl_filter = filter_val->mo_ctrl;
	pdev->mo_data_filter = filter_val->mo_data;

	dp_mon_filter_setup_mon_mode(pdev);
	status = dp_mon_filter_update(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Failed to set filter for advance mon mode"));
		dp_mon_filter_reset_mon_mode(pdev);
	}

	return status;
}

/**
 * dp_deliver_tx_mgmt() - Deliver mgmt frame for tx capture
 * @cdp_soc : data path soc handle
 * @pdev_id : pdev_id
 * @nbuf: Management frame buffer
 */
static QDF_STATUS
dp_deliver_tx_mgmt(struct cdp_soc_t *cdp_soc, uint8_t pdev_id, qdf_nbuf_t nbuf)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)cdp_soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	dp_deliver_mgmt_frm(pdev, nbuf);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_set_bsscolor() - sets bsscolor for tx capture
 * @pdev: Datapath PDEV handle
 * @bsscolor: new bsscolor
 */
static void
dp_mon_set_bsscolor(struct dp_pdev *pdev, uint8_t bsscolor)
{
	pdev->rx_mon_recv_status.bsscolor = bsscolor;
}

/**
 * dp_pdev_get_filter_ucast_data() - get DP PDEV monitor ucast filter
 * @soc : data path soc handle
 * @pdev_id : pdev_id
 * Return: true on ucast filter flag set
 */
static bool dp_pdev_get_filter_ucast_data(struct cdp_pdev *pdev_handle)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_handle;

	if ((pdev->fp_data_filter & FILTER_DATA_UCAST) ||
	    (pdev->mo_data_filter & FILTER_DATA_UCAST))
		return true;

	return false;
}

/**
 * dp_pdev_get_filter_mcast_data() - get DP PDEV monitor mcast filter
 * @pdev_handle: Datapath PDEV handle
 * Return: true on mcast filter flag set
 */
static bool dp_pdev_get_filter_mcast_data(struct cdp_pdev *pdev_handle)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_handle;

	if ((pdev->fp_data_filter & FILTER_DATA_MCAST) ||
	    (pdev->mo_data_filter & FILTER_DATA_MCAST))
		return true;

	return false;
}

/**
 * dp_pdev_get_filter_non_data() - get DP PDEV monitor non_data filter
 * @pdev_handle: Datapath PDEV handle
 * Return: true on non data filter flag set
 */
static bool dp_pdev_get_filter_non_data(struct cdp_pdev *pdev_handle)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_handle;

	if ((pdev->fp_mgmt_filter & FILTER_MGMT_ALL) ||
	    (pdev->mo_mgmt_filter & FILTER_MGMT_ALL)) {
		if ((pdev->fp_ctrl_filter & FILTER_CTRL_ALL) ||
		    (pdev->mo_ctrl_filter & FILTER_CTRL_ALL)) {
			return true;
		}
	}

	return false;
}

#ifdef MESH_MODE_SUPPORT
void dp_peer_set_mesh_mode(struct cdp_vdev *vdev_hdl, uint32_t val)
{
	struct dp_vdev *vdev = (struct dp_vdev *)vdev_hdl;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
		FL("val %d"), val);
	vdev->mesh_vdev = val;
}

/*
 * dp_peer_set_mesh_rx_filter() - to set the mesh rx filter
 * @vdev_hdl: virtual device object
 * @val: value to be set
 *
 * Return: void
 */
void dp_peer_set_mesh_rx_filter(struct cdp_vdev *vdev_hdl, uint32_t val)
{
	struct dp_vdev *vdev = (struct dp_vdev *)vdev_hdl;

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
		FL("val %d"), val);
	vdev->mesh_rx_filter = val;
}
#endif

#ifdef VDEV_PEER_PROTOCOL_COUNT
static void dp_enable_vdev_peer_protocol_count(struct cdp_soc_t *soc,
					       int8_t vdev_id,
					       bool enable)
{
	struct dp_vdev *vdev;

	vdev = dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						  vdev_id);
	dp_info("enable %d vdev_id %d", enable, vdev_id);
	vdev->peer_protocol_count_track = enable;
}

static void dp_enable_vdev_peer_protocol_drop_mask(struct cdp_soc_t *soc,
						   int8_t vdev_id,
						   int drop_mask)
{
	struct dp_vdev *vdev;

	vdev = dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						  vdev_id);
	dp_info("drop_mask %d vdev_id %d", drop_mask, vdev_id);
	vdev->peer_protocol_count_dropmask = drop_mask;
}

static int dp_is_vdev_peer_protocol_count_enabled(struct cdp_soc_t *soc,
						  int8_t vdev_id)
{
	struct dp_vdev *vdev;

	vdev = dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						  vdev_id);
	dp_info("enable %d vdev_id %d", vdev->peer_protocol_count_track,
		vdev_id);
	return vdev->peer_protocol_count_track;
}

static int dp_get_vdev_peer_protocol_drop_mask(struct cdp_soc_t *soc,
					       int8_t vdev_id)
{
	struct dp_vdev *vdev;

	vdev = dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						  vdev_id);
	dp_info("drop_mask %d vdev_id %d", vdev->peer_protocol_count_dropmask,
		vdev_id);
	return vdev->peer_protocol_count_dropmask;
}

#endif

bool dp_check_pdev_exists(struct dp_soc *soc, struct dp_pdev *data)
{
	uint8_t pdev_count;

	for (pdev_count = 0; pdev_count < MAX_PDEV_CNT; pdev_count++) {
		if (soc->pdev_list[pdev_count] &&
		    soc->pdev_list[pdev_count] == data)
			return true;
	}
	return false;
}

/**
 * dp_rx_bar_stats_cb(): BAR received stats callback
 * @soc: SOC handle
 * @cb_ctxt: Call back context
 * @reo_status: Reo status
 *
 * return: void
 */
void dp_rx_bar_stats_cb(struct dp_soc *soc, void *cb_ctxt,
	union hal_reo_status *reo_status)
{
	struct dp_pdev *pdev = (struct dp_pdev *)cb_ctxt;
	struct hal_reo_queue_status *queue_status = &(reo_status->queue_status);

	if (!dp_check_pdev_exists(soc, pdev)) {
		dp_err_rl("pdev doesn't exist");
		return;
	}

	if (!qdf_atomic_read(&soc->cmn_init_done))
		return;

	if (queue_status->header.status != HAL_REO_CMD_SUCCESS) {
		DP_PRINT_STATS("REO stats failure %d",
			       queue_status->header.status);
		qdf_atomic_set(&(pdev->stats_cmd_complete), 1);
		return;
	}

	pdev->stats.rx.bar_recv_cnt += queue_status->bar_rcvd_cnt;
	qdf_atomic_set(&(pdev->stats_cmd_complete), 1);

}

/**
 * dp_aggregate_vdev_stats(): Consolidate stats at VDEV level
 * @vdev: DP VDEV handle
 *
 * return: void
 */
void dp_aggregate_vdev_stats(struct dp_vdev *vdev,
			     struct cdp_vdev_stats *vdev_stats)
{
	struct dp_peer *peer = NULL;
	struct dp_soc *soc = NULL;

	if (!vdev || !vdev->pdev)
		return;

	soc = vdev->pdev->soc;

	qdf_mem_copy(vdev_stats, &vdev->stats, sizeof(vdev->stats));

	TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem)
		dp_update_vdev_stats(vdev_stats, peer);

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
	dp_wdi_event_handler(WDI_EVENT_UPDATE_DP_STATS, vdev->pdev->soc,
			     vdev_stats, vdev->vdev_id,
			     UPDATE_VDEV_STATS, vdev->pdev->pdev_id);
#endif
}

void dp_aggregate_pdev_stats(struct dp_pdev *pdev)
{
	struct dp_vdev *vdev = NULL;
	struct dp_soc *soc;
	struct cdp_vdev_stats *vdev_stats =
			qdf_mem_malloc(sizeof(struct cdp_vdev_stats));

	if (!vdev_stats) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "DP alloc failure - unable to get alloc vdev stats");
		return;
	}

	qdf_mem_zero(&pdev->stats.tx, sizeof(pdev->stats.tx));
	qdf_mem_zero(&pdev->stats.rx, sizeof(pdev->stats.rx));
	qdf_mem_zero(&pdev->stats.tx_i, sizeof(pdev->stats.tx_i));

	if (pdev->mcopy_mode)
		DP_UPDATE_STATS(pdev, pdev->invalid_peer);

	soc = pdev->soc;
	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {

		dp_aggregate_vdev_stats(vdev, vdev_stats);
		dp_update_pdev_stats(pdev, vdev_stats);
		dp_update_pdev_ingress_stats(pdev, vdev);
	}
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);
	qdf_mem_free(vdev_stats);

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
	dp_wdi_event_handler(WDI_EVENT_UPDATE_DP_STATS, pdev->soc, &pdev->stats,
			     pdev->pdev_id, UPDATE_PDEV_STATS, pdev->pdev_id);
#endif
}

/**
 * dp_vdev_getstats() - get vdev packet level stats
 * @vdev_handle: Datapath VDEV handle
 * @stats: cdp network device stats structure
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_vdev_getstats(struct cdp_vdev *vdev_handle,
				   struct cdp_dev_stats *stats)
{
	struct dp_vdev *vdev = (struct dp_vdev *)vdev_handle;
	struct dp_pdev *pdev;
	struct dp_soc *soc;
	struct cdp_vdev_stats *vdev_stats;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	pdev = vdev->pdev;
	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	soc = pdev->soc;

	vdev_stats = qdf_mem_malloc(sizeof(struct cdp_vdev_stats));

	if (!vdev_stats) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "DP alloc failure - unable to get alloc vdev stats");
		return QDF_STATUS_E_FAILURE;
	}

	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	dp_aggregate_vdev_stats(vdev, vdev_stats);
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	stats->tx_packets = vdev_stats->tx_i.rcvd.num;
	stats->tx_bytes = vdev_stats->tx_i.rcvd.bytes;

	stats->tx_errors = vdev_stats->tx.tx_failed +
		vdev_stats->tx_i.dropped.dropped_pkt.num;
	stats->tx_dropped = stats->tx_errors;

	stats->rx_packets = vdev_stats->rx.unicast.num +
		vdev_stats->rx.multicast.num +
		vdev_stats->rx.bcast.num;
	stats->rx_bytes = vdev_stats->rx.unicast.bytes +
		vdev_stats->rx.multicast.bytes +
		vdev_stats->rx.bcast.bytes;

	qdf_mem_free(vdev_stats);

	return QDF_STATUS_SUCCESS;
}


/**
 * dp_pdev_getstats() - get pdev packet level stats
 * @pdev_handle: Datapath PDEV handle
 * @stats: cdp network device stats structure
 *
 * Return: QDF_STATUS
 */
static void dp_pdev_getstats(struct cdp_pdev *pdev_handle,
			     struct cdp_dev_stats *stats)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_handle;

	dp_aggregate_pdev_stats(pdev);

	stats->tx_packets = pdev->stats.tx_i.rcvd.num;
	stats->tx_bytes = pdev->stats.tx_i.rcvd.bytes;

	stats->tx_errors = pdev->stats.tx.tx_failed +
		pdev->stats.tx_i.dropped.dropped_pkt.num;
	stats->tx_dropped = stats->tx_errors;

	stats->rx_packets = pdev->stats.rx.unicast.num +
		pdev->stats.rx.multicast.num +
		pdev->stats.rx.bcast.num;
	stats->rx_bytes = pdev->stats.rx.unicast.bytes +
		pdev->stats.rx.multicast.bytes +
		pdev->stats.rx.bcast.bytes;
	stats->rx_errors = pdev->stats.err.desc_alloc_fail +
		pdev->stats.err.ip_csum_err +
		pdev->stats.err.tcp_udp_csum_err +
		pdev->stats.rx.err.mic_err +
		pdev->stats.rx.err.decrypt_err +
		pdev->stats.err.rxdma_error +
		pdev->stats.err.reo_error;
	stats->rx_dropped = pdev->stats.dropped.msdu_not_done +
		pdev->stats.dropped.mec +
		pdev->stats.dropped.mesh_filter +
		pdev->stats.dropped.wifi_parse +
		pdev->stats.dropped.mon_rx_drop +
		pdev->stats.dropped.mon_radiotap_update_err;
}

/**
 * dp_get_device_stats() - get interface level packet stats
 * @soc: soc handle
 * @id : vdev_id or pdev_id based on type
 * @stats: cdp network device stats structure
 * @type: device type pdev/vdev
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_get_device_stats(struct cdp_soc_t *soc, uint8_t id,
				      struct cdp_dev_stats *stats,
				      uint8_t type)
{
	switch (type) {
	case UPDATE_VDEV_STATS:
		return dp_vdev_getstats(
			(struct cdp_vdev *)dp_get_vdev_from_soc_vdev_id_wifi3(
			 (struct dp_soc *)soc, id), stats);
	case UPDATE_PDEV_STATS:
		{
			struct dp_pdev *pdev =
				dp_get_pdev_from_soc_pdev_id_wifi3(
						(struct dp_soc *)soc,
						 id);
			if (pdev) {
				dp_pdev_getstats((struct cdp_pdev *)pdev,
						 stats);
				return QDF_STATUS_SUCCESS;
			}
		}
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			"apstats cannot be updated for this input "
			"type %d", type);
		break;
	}

	return QDF_STATUS_E_FAILURE;
}

const
char *dp_srng_get_str_from_hal_ring_type(enum hal_ring_type ring_type)
{
	switch (ring_type) {
	case REO_DST:
		return "Reo_dst";
	case REO_EXCEPTION:
		return "Reo_exception";
	case REO_CMD:
		return "Reo_cmd";
	case REO_REINJECT:
		return "Reo_reinject";
	case REO_STATUS:
		return "Reo_status";
	case WBM2SW_RELEASE:
		return "wbm2sw_release";
	case TCL_DATA:
		return "tcl_data";
	case TCL_CMD:
		return "tcl_cmd";
	case TCL_STATUS:
		return "tcl_status";
	case SW2WBM_RELEASE:
		return "sw2wbm_release";
	case RXDMA_BUF:
		return "Rxdma_buf";
	case RXDMA_DST:
		return "Rxdma_dst";
	case RXDMA_MONITOR_BUF:
		return "Rxdma_monitor_buf";
	case RXDMA_MONITOR_DESC:
		return "Rxdma_monitor_desc";
	case RXDMA_MONITOR_STATUS:
		return "Rxdma_monitor_status";
	default:
		dp_err("Invalid ring type");
		break;
	}
	return "Invalid";
}

/*
 * dp_print_napi_stats(): NAPI stats
 * @soc - soc handle
 */
void dp_print_napi_stats(struct dp_soc *soc)
{
	hif_print_napi_stats(soc->hif_handle);
}

/**
 * dp_txrx_host_stats_clr(): Reinitialize the txrx stats
 * @vdev: DP_VDEV handle
 *
 * Return: QDF_STATUS
 */
static inline QDF_STATUS
dp_txrx_host_stats_clr(struct dp_vdev *vdev)
{
	struct dp_peer *peer = NULL;

	if (!vdev || !vdev->pdev)
		return QDF_STATUS_E_FAILURE;

	DP_STATS_CLR(vdev->pdev);
	DP_STATS_CLR(vdev->pdev->soc);
	DP_STATS_CLR(vdev);

	hif_clear_napi_stats(vdev->pdev->soc->hif_handle);

	TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem) {
		if (!peer)
			return QDF_STATUS_E_FAILURE;
		DP_STATS_CLR(peer);

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
		dp_wdi_event_handler(WDI_EVENT_UPDATE_DP_STATS, vdev->pdev->soc,
				     &peer->stats,  peer->peer_ids[0],
				     UPDATE_PEER_STATS, vdev->pdev->pdev_id);
#endif
	}

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
	dp_wdi_event_handler(WDI_EVENT_UPDATE_DP_STATS, vdev->pdev->soc,
			     &vdev->stats,  vdev->vdev_id,
			     UPDATE_VDEV_STATS, vdev->pdev->pdev_id);
#endif
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_host_peer_stats()- function to print peer stats
 * @soc: dp_soc handle
 * @mac_addr: mac address of the peer
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_get_host_peer_stats(struct cdp_soc_t *soc, uint8_t *mac_addr)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc,
						      mac_addr, 0,
						      DP_VDEV_ALL);
	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			  "%s: Invalid peer\n", __func__);
		status = QDF_STATUS_E_FAILURE;
		goto fail;
	}

	dp_print_peer_stats(peer);
	dp_peer_rxtid_stats(peer, dp_rx_tid_stats_cb, NULL);
fail:
	if (peer)
		dp_peer_unref_delete(peer);

	return status;
}

/**
 * dp_txrx_stats_help() - Helper function for Txrx_Stats
 *
 * Return: None
 */
static void dp_txrx_stats_help(void)
{
	dp_info("Command: iwpriv wlan0 txrx_stats <stats_option> <mac_id>");
	dp_info("stats_option:");
	dp_info("  1 -- HTT Tx Statistics");
	dp_info("  2 -- HTT Rx Statistics");
	dp_info("  3 -- HTT Tx HW Queue Statistics");
	dp_info("  4 -- HTT Tx HW Sched Statistics");
	dp_info("  5 -- HTT Error Statistics");
	dp_info("  6 -- HTT TQM Statistics");
	dp_info("  7 -- HTT TQM CMDQ Statistics");
	dp_info("  8 -- HTT TX_DE_CMN Statistics");
	dp_info("  9 -- HTT Tx Rate Statistics");
	dp_info(" 10 -- HTT Rx Rate Statistics");
	dp_info(" 11 -- HTT Peer Statistics");
	dp_info(" 12 -- HTT Tx SelfGen Statistics");
	dp_info(" 13 -- HTT Tx MU HWQ Statistics");
	dp_info(" 14 -- HTT RING_IF_INFO Statistics");
	dp_info(" 15 -- HTT SRNG Statistics");
	dp_info(" 16 -- HTT SFM Info Statistics");
	dp_info(" 17 -- HTT PDEV_TX_MU_MIMO_SCHED INFO Statistics");
	dp_info(" 18 -- HTT Peer List Details");
	dp_info(" 20 -- Clear Host Statistics");
	dp_info(" 21 -- Host Rx Rate Statistics");
	dp_info(" 22 -- Host Tx Rate Statistics");
	dp_info(" 23 -- Host Tx Statistics");
	dp_info(" 24 -- Host Rx Statistics");
	dp_info(" 25 -- Host AST Statistics");
	dp_info(" 26 -- Host SRNG PTR Statistics");
	dp_info(" 27 -- Host Mon Statistics");
	dp_info(" 28 -- Host REO Queue Statistics");
	dp_info(" 29 -- Host Soc cfg param Statistics");
	dp_info(" 30 -- Host pdev cfg param Statistics");
	dp_info(" 31 -- Host FISA stats");
	dp_info(" 32 -- Host Register Work stats");
}

/**
 * dp_print_host_stats()- Function to print the stats aggregated at host
 * @vdev_handle: DP_VDEV handle
 * @type: host stats type
 *
 * Return: 0 on success, print error message in case of failure
 */
static int
dp_print_host_stats(struct dp_vdev *vdev,
		    struct cdp_txrx_stats_req *req)
{
	struct dp_pdev *pdev = (struct dp_pdev *)vdev->pdev;
	enum cdp_host_txrx_stats type =
			dp_stats_mapping_table[req->stats][STATS_HOST];

	dp_aggregate_pdev_stats(pdev);

	switch (type) {
	case TXRX_CLEAR_STATS:
		dp_txrx_host_stats_clr(vdev);
		break;
	case TXRX_RX_RATE_STATS:
		dp_print_rx_rates(vdev);
		break;
	case TXRX_TX_RATE_STATS:
		dp_print_tx_rates(vdev);
		break;
	case TXRX_TX_HOST_STATS:
		dp_print_pdev_tx_stats(pdev);
		dp_print_soc_tx_stats(pdev->soc);
		break;
	case TXRX_RX_HOST_STATS:
		dp_print_pdev_rx_stats(pdev);
		dp_print_soc_rx_stats(pdev->soc);
		break;
	case TXRX_AST_STATS:
		dp_print_ast_stats(pdev->soc);
		dp_print_peer_table(vdev);
		break;
	case TXRX_SRNG_PTR_STATS:
		dp_print_ring_stats(pdev);
		break;
	case TXRX_RX_MON_STATS:
		dp_print_pdev_rx_mon_stats(pdev);
		break;
	case TXRX_REO_QUEUE_STATS:
		dp_get_host_peer_stats((struct cdp_soc_t *)pdev->soc,
				       req->peer_addr);
		break;
	case TXRX_SOC_CFG_PARAMS:
		dp_print_soc_cfg_params(pdev->soc);
		break;
	case TXRX_PDEV_CFG_PARAMS:
		dp_print_pdev_cfg_params(pdev);
		break;
	case TXRX_NAPI_STATS:
		dp_print_napi_stats(pdev->soc);
		break;
	case TXRX_SOC_INTERRUPT_STATS:
		dp_print_soc_interrupt_stats(pdev->soc);
		break;
	case TXRX_SOC_FSE_STATS:
		dp_rx_dump_fisa_table(pdev->soc);
		break;
	case TXRX_HAL_REG_WRITE_STATS:
		hal_dump_reg_write_stats(pdev->soc->hal_soc);
		hal_dump_reg_write_srng_stats(pdev->soc->hal_soc);
		break;
	default:
		dp_info("Wrong Input For TxRx Host Stats");
		dp_txrx_stats_help();
		break;
	}
	return 0;
}

/*
 * is_ppdu_txrx_capture_enabled() - API to check both pktlog and debug_sniffer
 *                              modes are enabled or not.
 * @dp_pdev: dp pdev handle.
 *
 * Return: bool
 */
static inline bool is_ppdu_txrx_capture_enabled(struct dp_pdev *pdev)
{
	if (!pdev->pktlog_ppdu_stats && !pdev->tx_sniffer_enable &&
	    !pdev->mcopy_mode)
		return true;
	else
		return false;
}

/*
 *dp_set_bpr_enable() - API to enable/disable bpr feature
 *@pdev_handle: DP_PDEV handle.
 *@val: Provided value.
 *
 *Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS
dp_set_bpr_enable(struct dp_pdev *pdev, int val)
{
	switch (val) {
	case CDP_BPR_DISABLE:
		pdev->bpr_enable = CDP_BPR_DISABLE;
		if (!pdev->pktlog_ppdu_stats && !pdev->enhanced_stats_en &&
		    !pdev->tx_sniffer_enable && !pdev->mcopy_mode) {
			dp_h2t_cfg_stats_msg_send(pdev, 0, pdev->pdev_id);
		} else if (pdev->enhanced_stats_en &&
			   !pdev->tx_sniffer_enable && !pdev->mcopy_mode &&
			   !pdev->pktlog_ppdu_stats) {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_ENH_STATS,
						  pdev->pdev_id);
		}
		break;
	case CDP_BPR_ENABLE:
		pdev->bpr_enable = CDP_BPR_ENABLE;
		if (!pdev->enhanced_stats_en && !pdev->tx_sniffer_enable &&
		    !pdev->mcopy_mode && !pdev->pktlog_ppdu_stats) {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_BPR,
						  pdev->pdev_id);
		} else if (pdev->enhanced_stats_en &&
			   !pdev->tx_sniffer_enable && !pdev->mcopy_mode &&
			   !pdev->pktlog_ppdu_stats) {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_BPR_ENH,
						  pdev->pdev_id);
		} else if (pdev->pktlog_ppdu_stats) {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_BPR_PKTLOG,
						  pdev->pdev_id);
		}
		break;
	default:
		break;
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_pdev_tid_stats_ingress_inc
 * @pdev: pdev handle
 * @val: increase in value
 *
 * Return: void
 */
static void
dp_pdev_tid_stats_ingress_inc(struct dp_pdev *pdev, uint32_t val)
{
	pdev->stats.tid_stats.ingress_stack += val;
}

/*
 * dp_pdev_tid_stats_osif_drop
 * @pdev: pdev handle
 * @val: increase in value
 *
 * Return: void
 */
static void
dp_pdev_tid_stats_osif_drop(struct dp_pdev *pdev, uint32_t val)
{
	pdev->stats.tid_stats.osif_drop += val;
}


/*
 * dp_config_debug_sniffer()- API to enable/disable debug sniffer
 * @pdev: DP_PDEV handle
 * @val: user provided value
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS
dp_config_debug_sniffer(struct dp_pdev *pdev, int val)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	/*
	 * Note: The mirror copy mode cannot co-exist with any other
	 * monitor modes. Hence disabling the filter for this mode will
	 * reset the monitor destination ring filters.
	 */
	if (pdev->mcopy_mode) {
#ifdef FEATURE_PERPKT_INFO
		dp_pdev_disable_mcopy_code(pdev);
		dp_mon_filter_reset_mcopy_mode(pdev);
		status = dp_mon_filter_update(pdev);
		if (status != QDF_STATUS_SUCCESS) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  FL("Failed to reset AM copy mode filters"));
		}
#endif /* FEATURE_PERPKT_INFO */
	}

	switch (val) {
	case 0:
		pdev->tx_sniffer_enable = 0;
		pdev->monitor_configured = false;

		/*
		 * We don't need to reset the Rx monitor status ring  or call
		 * the API dp_ppdu_ring_reset() if all debug sniffer mode is
		 * disabled. The Rx monitor status ring will be disabled when
		 * the last mode using the monitor status ring get disabled.
		 */
		if (!pdev->pktlog_ppdu_stats && !pdev->enhanced_stats_en &&
		    !pdev->bpr_enable) {
			dp_h2t_cfg_stats_msg_send(pdev, 0, pdev->pdev_id);
		} else if (pdev->enhanced_stats_en && !pdev->bpr_enable) {
			dp_h2t_cfg_stats_msg_send(pdev,
				DP_PPDU_STATS_CFG_ENH_STATS, pdev->pdev_id);
		} else if (!pdev->enhanced_stats_en && pdev->bpr_enable) {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_BPR_ENH,
						  pdev->pdev_id);
		} else {
			dp_h2t_cfg_stats_msg_send(pdev,
						  DP_PPDU_STATS_CFG_BPR,
						  pdev->pdev_id);
		}
		break;

	case 1:
		pdev->tx_sniffer_enable = 1;
		pdev->monitor_configured = false;

		if (!pdev->pktlog_ppdu_stats)
			dp_h2t_cfg_stats_msg_send(pdev,
				DP_PPDU_STATS_CFG_SNIFFER, pdev->pdev_id);
		break;
	case 2:
		if (pdev->monitor_vdev) {
			status = QDF_STATUS_E_RESOURCES;
			break;
		}

#ifdef FEATURE_PERPKT_INFO
		pdev->mcopy_mode = 1;
		pdev->tx_sniffer_enable = 0;
		pdev->monitor_configured = true;

		/*
		 * Setup the M copy mode filter.
		 */
		dp_mon_filter_setup_mcopy_mode(pdev);
		status = dp_mon_filter_update(pdev);
		if (status != QDF_STATUS_SUCCESS) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  FL("Failed to set M_copy mode filters"));
			dp_mon_filter_reset_mcopy_mode(pdev);
			dp_pdev_disable_mcopy_code(pdev);
			return status;
		}

		if (!pdev->pktlog_ppdu_stats)
			dp_h2t_cfg_stats_msg_send(pdev,
				DP_PPDU_STATS_CFG_SNIFFER, pdev->pdev_id);
#endif /* FEATURE_PERPKT_INFO */
		break;

	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Invalid value");
		break;
	}
	return status;
}

#ifdef FEATURE_PERPKT_INFO
/*
 * dp_enable_enhanced_stats()- API to enable enhanced statistcs
 * @soc_handle: DP_SOC handle
 * @pdev_id: id of DP_PDEV handle
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_enable_enhanced_stats(struct cdp_soc_t *soc, uint8_t pdev_id)
{
	struct dp_pdev *pdev = NULL;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						  pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	if (pdev->enhanced_stats_en == 0)
		dp_cal_client_timer_start(pdev->cal_client_ctx);

	pdev->enhanced_stats_en = 1;

	dp_mon_filter_setup_enhanced_stats(pdev);
	status = dp_mon_filter_update(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Failed to set enhanced mode filters"));
		dp_mon_filter_reset_enhanced_stats(pdev);
		dp_cal_client_timer_stop(pdev->cal_client_ctx);
		pdev->enhanced_stats_en = 0;
		return QDF_STATUS_E_FAILURE;
	}

	if (is_ppdu_txrx_capture_enabled(pdev) && !pdev->bpr_enable) {
		dp_h2t_cfg_stats_msg_send(pdev, DP_PPDU_STATS_CFG_ENH_STATS, pdev->pdev_id);
	} else if (is_ppdu_txrx_capture_enabled(pdev) && pdev->bpr_enable) {
		dp_h2t_cfg_stats_msg_send(pdev,
					  DP_PPDU_STATS_CFG_BPR_ENH,
					  pdev->pdev_id);
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_disable_enhanced_stats()- API to disable enhanced statistcs
 *
 * @param soc - the soc handle
 * @param pdev_id - pdev_id of pdev
 * @return - QDF_STATUS
 */
static QDF_STATUS
dp_disable_enhanced_stats(struct cdp_soc_t *soc, uint8_t pdev_id)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	if (pdev->enhanced_stats_en == 1)
		dp_cal_client_timer_stop(pdev->cal_client_ctx);

	pdev->enhanced_stats_en = 0;

	if (is_ppdu_txrx_capture_enabled(pdev) && !pdev->bpr_enable) {
		dp_h2t_cfg_stats_msg_send(pdev, 0, pdev->pdev_id);
	} else if (is_ppdu_txrx_capture_enabled(pdev) && pdev->bpr_enable) {
		dp_h2t_cfg_stats_msg_send(pdev,
					  DP_PPDU_STATS_CFG_BPR,
					  pdev->pdev_id);
	}

	dp_mon_filter_reset_enhanced_stats(pdev);
	if (dp_mon_filter_update(pdev) != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Failed to reset enhanced mode filters"));
	}

	return QDF_STATUS_SUCCESS;
}
#endif /* FEATURE_PERPKT_INFO */

/*
 * dp_get_fw_peer_stats()- function to print peer stats
 * @soc: soc handle
 * @pdev_id : id of the pdev handle
 * @mac_addr: mac address of the peer
 * @cap: Type of htt stats requested
 * @is_wait: if set, wait on completion from firmware response
 *
 * Currently Supporting only MAC ID based requests Only
 *	1: HTT_PEER_STATS_REQ_MODE_NO_QUERY
 *	2: HTT_PEER_STATS_REQ_MODE_QUERY_TQM
 *	3: HTT_PEER_STATS_REQ_MODE_FLUSH_TQM
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_get_fw_peer_stats(struct cdp_soc_t *soc, uint8_t pdev_id,
		     uint8_t *mac_addr,
		     uint32_t cap, uint32_t is_wait)
{
	int i;
	uint32_t config_param0 = 0;
	uint32_t config_param1 = 0;
	uint32_t config_param2 = 0;
	uint32_t config_param3 = 0;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	HTT_DBG_EXT_STATS_PEER_INFO_IS_MAC_ADDR_SET(config_param0, 1);
	config_param0 |= (1 << (cap + 1));

	for (i = 0; i < HTT_PEER_STATS_MAX_TLV; i++) {
		config_param1 |= (1 << i);
	}

	config_param2 |= (mac_addr[0] & 0x000000ff);
	config_param2 |= ((mac_addr[1] << 8) & 0x0000ff00);
	config_param2 |= ((mac_addr[2] << 16) & 0x00ff0000);
	config_param2 |= ((mac_addr[3] << 24) & 0xff000000);

	config_param3 |= (mac_addr[4] & 0x000000ff);
	config_param3 |= ((mac_addr[5] << 8) & 0x0000ff00);

	if (is_wait) {
		qdf_event_reset(&pdev->fw_peer_stats_event);
		dp_h2t_ext_stats_msg_send(pdev, HTT_DBG_EXT_STATS_PEER_INFO,
					  config_param0, config_param1,
					  config_param2, config_param3,
					  0, 1, 0);
		qdf_wait_single_event(&pdev->fw_peer_stats_event,
				      DP_FW_PEER_STATS_CMP_TIMEOUT_MSEC);
	} else {
		dp_h2t_ext_stats_msg_send(pdev, HTT_DBG_EXT_STATS_PEER_INFO,
					  config_param0, config_param1,
					  config_param2, config_param3,
					  0, 0, 0);
	}

	return QDF_STATUS_SUCCESS;

}

/* This struct definition will be removed from here
 * once it get added in FW headers*/
struct httstats_cmd_req {
    uint32_t    config_param0;
    uint32_t    config_param1;
    uint32_t    config_param2;
    uint32_t    config_param3;
    int cookie;
    u_int8_t    stats_id;
};

/*
 * dp_get_htt_stats: function to process the httstas request
 * @soc: DP soc handle
 * @pdev_id: id of pdev handle
 * @data: pointer to request data
 * @data_len: length for request data
 *
 * return: QDF_STATUS
 */
static QDF_STATUS
dp_get_htt_stats(struct cdp_soc_t *soc, uint8_t pdev_id, void *data,
		 uint32_t data_len)
{
	struct httstats_cmd_req *req = (struct httstats_cmd_req *)data;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	QDF_ASSERT(data_len == sizeof(struct httstats_cmd_req));
	dp_h2t_ext_stats_msg_send(pdev, req->stats_id,
				req->config_param0, req->config_param1,
				req->config_param2, req->config_param3,
				req->cookie, 0, 0);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_set_pdev_tidmap_prty_wifi3(): update tidmap priority in pdev
 * @pdev: DP_PDEV handle
 * @prio: tidmap priority value passed by the user
 *
 * Return: QDF_STATUS_SUCCESS on success
 */
static QDF_STATUS dp_set_pdev_tidmap_prty_wifi3(struct dp_pdev *pdev,
						uint8_t prio)
{
	struct dp_soc *soc = pdev->soc;

	soc->tidmap_prty = prio;

	hal_tx_set_tidmap_prty(soc->hal_soc, prio);
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_peer_param: function to get parameters in peer
 * @cdp_soc: DP soc handle
 * @vdev_id: id of vdev handle
 * @peer_mac: peer mac address
 * @param: parameter type to be set
 * @val : address of buffer
 *
 * Return: val
 */
static QDF_STATUS dp_get_peer_param(struct cdp_soc_t *cdp_soc,  uint8_t vdev_id,
				    uint8_t *peer_mac,
				    enum cdp_peer_param_type param,
				    cdp_config_param_type *val)
{
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_peer_param: function to set parameters in peer
 * @cdp_soc: DP soc handle
 * @vdev_id: id of vdev handle
 * @peer_mac: peer mac address
 * @param: parameter type to be set
 * @val: value of parameter to be set
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS dp_set_peer_param(struct cdp_soc_t *cdp_soc,  uint8_t vdev_id,
				    uint8_t *peer_mac,
				    enum cdp_peer_param_type param,
				    cdp_config_param_type val)
{
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)cdp_soc,
						      peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress)
		goto fail;

	switch (param) {
	case CDP_CONFIG_NAWDS:
		peer->nawds_enabled = val.cdp_peer_param_nawds;
		break;
	case CDP_CONFIG_NAC:
		peer->nac = !!(val.cdp_peer_param_nac);
		break;
	default:
		break;
	}

fail:
	if (peer)
		dp_peer_unref_delete(peer);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_pdev_param: function to get parameters from pdev
 * @cdp_soc: DP soc handle
 * @pdev_id: id of pdev handle
 * @param: parameter type to be get
 * @value : buffer for value
 *
 * Return: status
 */
static QDF_STATUS dp_get_pdev_param(struct cdp_soc_t *cdp_soc, uint8_t pdev_id,
				    enum cdp_pdev_param_type param,
				    cdp_config_param_type *val)
{
	struct cdp_pdev *pdev = (struct cdp_pdev *)
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)cdp_soc,
						   pdev_id);
	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	switch (param) {
	case CDP_CONFIG_VOW:
		val->cdp_pdev_param_cfg_vow =
				((struct dp_pdev *)pdev)->delay_stats_flag;
		break;
	case CDP_TX_PENDING:
		val->cdp_pdev_param_tx_pending = dp_get_tx_pending(pdev);
		break;
	case CDP_FILTER_MCAST_DATA:
		val->cdp_pdev_param_fltr_mcast =
					dp_pdev_get_filter_mcast_data(pdev);
		break;
	case CDP_FILTER_NO_DATA:
		val->cdp_pdev_param_fltr_none =
					dp_pdev_get_filter_non_data(pdev);
		break;
	case CDP_FILTER_UCAST_DATA:
		val->cdp_pdev_param_fltr_ucast =
					dp_pdev_get_filter_ucast_data(pdev);
		break;
	default:
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_pdev_param: function to set parameters in pdev
 * @cdp_soc: DP soc handle
 * @pdev_id: id of pdev handle
 * @param: parameter type to be set
 * @val: value of parameter to be set
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS dp_set_pdev_param(struct cdp_soc_t *cdp_soc, uint8_t pdev_id,
				    enum cdp_pdev_param_type param,
				    cdp_config_param_type val)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)cdp_soc,
						   pdev_id);
	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	switch (param) {
	case CDP_CONFIG_TX_CAPTURE:
		return dp_config_debug_sniffer(pdev,
					       val.cdp_pdev_param_tx_capture);
	case CDP_CONFIG_DEBUG_SNIFFER:
		return dp_config_debug_sniffer(pdev,
					       val.cdp_pdev_param_dbg_snf);
	case CDP_CONFIG_BPR_ENABLE:
		return dp_set_bpr_enable(pdev, val.cdp_pdev_param_bpr_enable);
	case CDP_CONFIG_PRIMARY_RADIO:
		pdev->is_primary = val.cdp_pdev_param_primary_radio;
		break;
	case CDP_CONFIG_CAPTURE_LATENCY:
		pdev->latency_capture_enable = val.cdp_pdev_param_cptr_latcy;
		break;
	case CDP_INGRESS_STATS:
		dp_pdev_tid_stats_ingress_inc(pdev,
					      val.cdp_pdev_param_ingrs_stats);
		break;
	case CDP_OSIF_DROP:
		dp_pdev_tid_stats_osif_drop(pdev,
					    val.cdp_pdev_param_osif_drop);
		break;
	case CDP_CONFIG_ENH_RX_CAPTURE:
		return dp_config_enh_rx_capture(pdev,
						val.cdp_pdev_param_en_rx_cap);
	case CDP_CONFIG_ENH_TX_CAPTURE:
		return dp_config_enh_tx_capture(pdev,
						val.cdp_pdev_param_en_tx_cap);
	case CDP_CONFIG_HMMC_TID_OVERRIDE:
		pdev->hmmc_tid_override_en = val.cdp_pdev_param_hmmc_tid_ovrd;
		break;
	case CDP_CONFIG_HMMC_TID_VALUE:
		pdev->hmmc_tid = val.cdp_pdev_param_hmmc_tid;
		break;
	case CDP_CHAN_NOISE_FLOOR:
		pdev->chan_noise_floor = val.cdp_pdev_param_chn_noise_flr;
		break;
	case CDP_TIDMAP_PRTY:
		dp_set_pdev_tidmap_prty_wifi3(pdev,
					      val.cdp_pdev_param_tidmap_prty);
		break;
	case CDP_FILTER_NEIGH_PEERS:
		dp_set_filter_neigh_peers(pdev,
					  val.cdp_pdev_param_fltr_neigh_peers);
		break;
	case CDP_MONITOR_CHANNEL:
		pdev->mon_chan_num = val.cdp_pdev_param_monitor_chan;
		break;
	case CDP_MONITOR_FREQUENCY:
		pdev->mon_chan_freq = val.cdp_pdev_param_mon_freq;
		break;
	case CDP_CONFIG_BSS_COLOR:
		dp_mon_set_bsscolor(pdev, val.cdp_pdev_param_bss_color);
		break;
	default:
		return QDF_STATUS_E_INVAL;
	}
	return QDF_STATUS_SUCCESS;
}

/*
 * dp_calculate_delay_stats: function to get rx delay stats
 * @cdp_soc: DP soc handle
 * @vdev_id: id of DP vdev handle
 * @nbuf: skb
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_calculate_delay_stats(struct cdp_soc_t *cdp_soc, uint8_t vdev_id,
			 qdf_nbuf_t nbuf)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)cdp_soc,
						   vdev_id);
	if (vdev) {
		dp_rx_compute_delay(vdev, nbuf);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_vdev_param: function to get parameters from vdev
 * @cdp_soc : DP soc handle
 * @vdev_id: id of DP vdev handle
 * @param: parameter type to get value
 * @val: buffer address
 *
 * return: status
 */
static QDF_STATUS dp_get_vdev_param(struct cdp_soc_t *cdp_soc, uint8_t vdev_id,
				    enum cdp_vdev_param_type param,
				    cdp_config_param_type *val)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)cdp_soc,
						   vdev_id);
	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	switch (param) {
	case CDP_ENABLE_WDS:
		val->cdp_vdev_param_wds = vdev->wds_enabled;
		break;
	case CDP_ENABLE_MEC:
		val->cdp_vdev_param_mec = vdev->mec_enabled;
		break;
	case CDP_ENABLE_DA_WAR:
		val->cdp_vdev_param_da_war = vdev->pdev->soc->da_war_enabled;
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "param value %d is wrong\n",
			  param);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_vdev_param: function to set parameters in vdev
 * @cdp_soc : DP soc handle
 * @vdev_id: id of DP vdev handle
 * @param: parameter type to get value
 * @val: value
 *
 * return: QDF_STATUS
 */
static QDF_STATUS
dp_set_vdev_param(struct cdp_soc_t *cdp_soc, uint8_t vdev_id,
		  enum cdp_vdev_param_type param, cdp_config_param_type val)
{
	struct dp_soc *dsoc = (struct dp_soc *)cdp_soc;
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3(dsoc, vdev_id);
	uint32_t var = 0;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	switch (param) {
	case CDP_ENABLE_WDS:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "wds_enable %d for vdev(%pK) id(%d)\n",
			  val.cdp_vdev_param_wds, vdev, vdev->vdev_id);
		vdev->wds_enabled = val.cdp_vdev_param_wds;
		break;
	case CDP_ENABLE_MEC:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "mec_enable %d for vdev(%pK) id(%d)\n",
			  val.cdp_vdev_param_mec, vdev, vdev->vdev_id);
		vdev->mec_enabled = val.cdp_vdev_param_mec;
		break;
	case CDP_ENABLE_DA_WAR:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "da_war_enable %d for vdev(%pK) id(%d)\n",
			  val.cdp_vdev_param_da_war, vdev, vdev->vdev_id);
		vdev->pdev->soc->da_war_enabled = val.cdp_vdev_param_da_war;
		dp_wds_flush_ast_table_wifi3(((struct cdp_soc_t *)
					     vdev->pdev->soc));
		break;
	case CDP_ENABLE_NAWDS:
		vdev->nawds_enabled = val.cdp_vdev_param_nawds;
		break;
	case CDP_ENABLE_MCAST_EN:
		vdev->mcast_enhancement_en = val.cdp_vdev_param_mcast_en;
		break;
	case CDP_ENABLE_PROXYSTA:
		vdev->proxysta_vdev = val.cdp_vdev_param_proxysta;
		break;
	case CDP_UPDATE_TDLS_FLAGS:
		vdev->tdls_link_connected = val.cdp_vdev_param_tdls_flags;
		break;
	case CDP_CFG_WDS_AGING_TIMER:
		var = val.cdp_vdev_param_aging_tmr;
		if (!var)
			qdf_timer_stop(&vdev->pdev->soc->ast_aging_timer);
		else if (var != vdev->wds_aging_timer_val)
			qdf_timer_mod(&vdev->pdev->soc->ast_aging_timer, var);

		vdev->wds_aging_timer_val = var;
		break;
	case CDP_ENABLE_AP_BRIDGE:
		if (wlan_op_mode_sta != vdev->opmode)
			vdev->ap_bridge_enabled = val.cdp_vdev_param_ap_brdg_en;
		else
			vdev->ap_bridge_enabled = false;
		break;
	case CDP_ENABLE_CIPHER:
		vdev->sec_type = val.cdp_vdev_param_cipher_en;
		break;
	case CDP_ENABLE_QWRAP_ISOLATION:
		vdev->isolation_vdev = val.cdp_vdev_param_qwrap_isolation;
		break;
	case CDP_UPDATE_MULTIPASS:
		vdev->multipass_en = val.cdp_vdev_param_update_multipass;
		break;
	case CDP_TX_ENCAP_TYPE:
		vdev->tx_encap_type = val.cdp_vdev_param_tx_encap;
		break;
	case CDP_RX_DECAP_TYPE:
		vdev->rx_decap_type = val.cdp_vdev_param_rx_decap;
		break;
	case CDP_TID_VDEV_PRTY:
		vdev->tidmap_prty = val.cdp_vdev_param_tidmap_prty;
		break;
	case CDP_TIDMAP_TBL_ID:
		vdev->tidmap_tbl_id = val.cdp_vdev_param_tidmap_tbl_id;
		break;
#ifdef MESH_MODE_SUPPORT
	case CDP_MESH_RX_FILTER:
		dp_peer_set_mesh_rx_filter((struct cdp_vdev *)vdev,
					   val.cdp_vdev_param_mesh_rx_filter);
		break;
	case CDP_MESH_MODE:
		dp_peer_set_mesh_mode((struct cdp_vdev *)vdev,
				      val.cdp_vdev_param_mesh_mode);
		break;
#endif
	case CDP_ENABLE_CSUM:
		dp_info("vdev_id %d enable Checksum %d", vdev_id,
			val.cdp_enable_tx_checksum);
		vdev->csum_enabled = val.cdp_enable_tx_checksum;
		break;
	default:
		break;
	}

	dp_tx_vdev_update_search_flags((struct dp_vdev *)vdev);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_psoc_param: function to set parameters in psoc
 * @cdp_soc : DP soc handle
 * @param: parameter type to be set
 * @val: value of parameter to be set
 *
 * return: QDF_STATUS
 */
static QDF_STATUS
dp_set_psoc_param(struct cdp_soc_t *cdp_soc,
		  enum cdp_psoc_param_type param, cdp_config_param_type val)
{
	struct dp_soc *soc = (struct dp_soc *)cdp_soc;
	struct wlan_cfg_dp_soc_ctxt *wlan_cfg_ctx = soc->wlan_cfg_ctx;

	switch (param) {
	case CDP_ENABLE_RATE_STATS:
		soc->wlanstats_enabled = val.cdp_psoc_param_en_rate_stats;
		break;
	case CDP_SET_NSS_CFG:
		wlan_cfg_set_dp_soc_nss_cfg(wlan_cfg_ctx,
					    val.cdp_psoc_param_en_nss_cfg);
		/*
		 * TODO: masked out based on the per offloaded radio
		 */
		switch (val.cdp_psoc_param_en_nss_cfg) {
		case dp_nss_cfg_default:
			break;
		case dp_nss_cfg_first_radio:
		/*
		 * This configuration is valid for single band radio which
		 * is also NSS offload.
		 */
		case dp_nss_cfg_dbdc:
		case dp_nss_cfg_dbtc:
			wlan_cfg_set_num_tx_desc_pool(wlan_cfg_ctx, 0);
			wlan_cfg_set_num_tx_ext_desc_pool(wlan_cfg_ctx, 0);
			wlan_cfg_set_num_tx_desc(wlan_cfg_ctx, 0);
			wlan_cfg_set_num_tx_ext_desc(wlan_cfg_ctx, 0);
			break;
		default:
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  "Invalid offload config %d",
				  val.cdp_psoc_param_en_nss_cfg);
		}

		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			  FL("nss-wifi<0> nss config is enabled"));
		break;

	default:
		break;
	}

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_get_psoc_param: function to get parameters in soc
 * @cdp_soc : DP soc handle
 * @param: parameter type to be set
 * @val: address of buffer
 *
 * return: status
 */
static QDF_STATUS dp_get_psoc_param(struct cdp_soc_t *cdp_soc,
				    enum cdp_psoc_param_type param,
				    cdp_config_param_type *val)
{
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_peer_update_pkt_capture_params: Set Rx & Tx Capture flags for a peer
 * @soc: DP_SOC handle
 * @pdev_id: id of DP_PDEV handle
 * @is_rx_pkt_cap_enable: enable/disable Rx packet capture in monitor mode
 * @is_tx_pkt_cap_enable: enable/disable Tx packet capture in monitor mode
 * @peer_mac: MAC address for which the above need to be enabled/disabled
 *
 * Return: Success if Rx & Tx capture is enabled for peer, false otherwise
 */
QDF_STATUS
dp_peer_update_pkt_capture_params(ol_txrx_soc_handle soc,
				  uint8_t pdev_id,
				  bool is_rx_pkt_cap_enable,
				  bool is_tx_pkt_cap_enable,
				  uint8_t *peer_mac)
{
	struct dp_peer *peer;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	peer = (struct dp_peer *)dp_find_peer_by_addr((struct cdp_pdev *)pdev,
						      peer_mac);
	if (!peer) {
		dp_err("Invalid Peer");
		return QDF_STATUS_E_FAILURE;
	}

	dp_peer_set_rx_capture_enabled(peer, is_rx_pkt_cap_enable);
	dp_peer_set_tx_capture_enabled(peer, is_tx_pkt_cap_enable);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_set_vdev_dscp_tid_map_wifi3(): Update Map ID selected for particular vdev
 * @soc: DP_SOC handle
 * @vdev_id: id of DP_VDEV handle
 * @map_id:ID of map that needs to be updated
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_set_vdev_dscp_tid_map_wifi3(ol_txrx_soc_handle soc,
						 uint8_t vdev_id,
						 uint8_t map_id)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	if (vdev) {
		vdev->dscp_tid_map_id = map_id;
		return QDF_STATUS_SUCCESS;
	}

	return QDF_STATUS_E_FAILURE;
}

#ifdef DP_RATETABLE_SUPPORT
static int dp_txrx_get_ratekbps(int preamb, int mcs,
				int htflag, int gintval)
{
	uint32_t rix;
	uint16_t ratecode;

	return dp_getrateindex((uint32_t)gintval, (uint16_t)mcs, 1,
			       (uint8_t)preamb, 1, &rix, &ratecode);
}
#else
static int dp_txrx_get_ratekbps(int preamb, int mcs,
				int htflag, int gintval)
{
	return 0;
}
#endif

/* dp_txrx_get_pdev_stats - Returns cdp_pdev_stats
 * @soc: DP soc handle
 * @pdev_id: id of DP pdev handle
 * @pdev_stats: buffer to copy to
 *
 * return : status success/failure
 */
static QDF_STATUS
dp_txrx_get_pdev_stats(struct cdp_soc_t *soc, uint8_t pdev_id,
		       struct cdp_pdev_stats *pdev_stats)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	dp_aggregate_pdev_stats(pdev);

	qdf_mem_copy(pdev_stats, &pdev->stats, sizeof(struct cdp_pdev_stats));
	return QDF_STATUS_SUCCESS;
}

/* dp_txrx_update_vdev_me_stats(): Update vdev ME stats sent from CDP
 * @vdev: DP vdev handle
 * @buf: buffer containing specific stats structure
 *
 * Returns: void
 */
static void dp_txrx_update_vdev_me_stats(struct dp_vdev *vdev,
					 void *buf)
{
	struct cdp_tx_ingress_stats *host_stats = NULL;

	if (!buf) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "Invalid host stats buf");
		return;
	}
	host_stats = (struct cdp_tx_ingress_stats *)buf;

	DP_STATS_INC_PKT(vdev, tx_i.mcast_en.mcast_pkt,
			 host_stats->mcast_en.mcast_pkt.num,
			 host_stats->mcast_en.mcast_pkt.bytes);
	DP_STATS_INC(vdev, tx_i.mcast_en.dropped_map_error,
		     host_stats->mcast_en.dropped_map_error);
	DP_STATS_INC(vdev, tx_i.mcast_en.dropped_self_mac,
		     host_stats->mcast_en.dropped_self_mac);
	DP_STATS_INC(vdev, tx_i.mcast_en.dropped_send_fail,
		     host_stats->mcast_en.dropped_send_fail);
	DP_STATS_INC(vdev, tx_i.mcast_en.ucast,
		     host_stats->mcast_en.ucast);
	DP_STATS_INC(vdev, tx_i.mcast_en.fail_seg_alloc,
		     host_stats->mcast_en.fail_seg_alloc);
	DP_STATS_INC(vdev, tx_i.mcast_en.clone_fail,
		     host_stats->mcast_en.clone_fail);
}

/* dp_txrx_update_vdev_host_stats(): Update stats sent through CDP
 * @soc: DP soc handle
 * @vdev_id: id of DP vdev handle
 * @buf: buffer containing specific stats structure
 * @stats_id: stats type
 *
 * Returns: QDF_STATUS
 */
static QDF_STATUS dp_txrx_update_vdev_host_stats(struct cdp_soc_t *soc,
						 uint8_t vdev_id,
						 void *buf,
						 uint16_t stats_id)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "Invalid vdev handle");
		return QDF_STATUS_E_FAILURE;
	}

	switch (stats_id) {
	case DP_VDEV_STATS_PKT_CNT_ONLY:
		break;
	case DP_VDEV_STATS_TX_ME:
		dp_txrx_update_vdev_me_stats(vdev, buf);
		break;
	default:
		qdf_info("Invalid stats_id %d", stats_id);
		break;
	}

	return QDF_STATUS_SUCCESS;
}

/* dp_txrx_get_peer_stats - will return cdp_peer_stats
 * @soc: soc handle
 * @vdev_id: id of vdev handle
 * @peer_mac: mac of DP_PEER handle
 * @peer_stats: buffer to copy to
 * return : status success/failure
 */
static QDF_STATUS
dp_txrx_get_peer_stats(struct cdp_soc_t *soc, uint8_t vdev_id,
		       uint8_t *peer_mac, struct cdp_peer_stats *peer_stats)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc,
						       peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		status = QDF_STATUS_E_FAILURE;
	} else
		qdf_mem_copy(peer_stats, &peer->stats,
			     sizeof(struct cdp_peer_stats));

	if (peer)
		dp_peer_unref_delete(peer);

	return status;
}

/* dp_txrx_get_peer_stats_param - will return specified cdp_peer_stats
 * @param soc - soc handle
 * @param vdev_id - vdev_id of vdev object
 * @param peer_mac - mac address of the peer
 * @param type - enum of required stats
 * @param buf - buffer to hold the value
 * return : status success/failure
 */
static QDF_STATUS
dp_txrx_get_peer_stats_param(struct cdp_soc_t *soc, uint8_t vdev_id,
			     uint8_t *peer_mac, enum cdp_peer_stats_type type,
			     cdp_peer_stats_param_t *buf)
{
	QDF_STATUS ret = QDF_STATUS_SUCCESS;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc,
						      peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "Invalid Peer for Mac "QDF_MAC_ADDR_FMT,
			  QDF_MAC_ADDR_REF(peer_mac));
		ret = QDF_STATUS_E_FAILURE;
	} else if (type < cdp_peer_stats_max) {
		switch (type) {
		case cdp_peer_tx_ucast:
			buf->tx_ucast = peer->stats.tx.ucast;
			break;
		case cdp_peer_tx_mcast:
			buf->tx_mcast = peer->stats.tx.mcast;
			break;
		case cdp_peer_tx_rate:
			buf->tx_rate = peer->stats.tx.tx_rate;
			break;
		case cdp_peer_tx_last_tx_rate:
			buf->last_tx_rate = peer->stats.tx.last_tx_rate;
			break;
		case cdp_peer_tx_inactive_time:
			buf->tx_inactive_time = peer->stats.tx.inactive_time;
			break;
		case cdp_peer_tx_ratecode:
			buf->tx_ratecode = peer->stats.tx.tx_ratecode;
			break;
		case cdp_peer_tx_flags:
			buf->tx_flags = peer->stats.tx.tx_flags;
			break;
		case cdp_peer_tx_power:
			buf->tx_power = peer->stats.tx.tx_power;
			break;
		case cdp_peer_rx_rate:
			buf->rx_rate = peer->stats.rx.rx_rate;
			break;
		case cdp_peer_rx_last_rx_rate:
			buf->last_rx_rate = peer->stats.rx.last_rx_rate;
			break;
		case cdp_peer_rx_ratecode:
			buf->rx_ratecode = peer->stats.rx.rx_ratecode;
			break;
		case cdp_peer_rx_ucast:
			buf->rx_ucast = peer->stats.rx.unicast;
			break;
		case cdp_peer_rx_flags:
			buf->rx_flags = peer->stats.rx.rx_flags;
			break;
		case cdp_peer_rx_avg_rssi:
			buf->rx_avg_rssi = peer->stats.rx.avg_rssi;
			break;
		default:
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				  "Invalid value");
			ret = QDF_STATUS_E_FAILURE;
			break;
		}
	} else {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "Invalid value");
		ret = QDF_STATUS_E_FAILURE;
	}

	if (peer)
		dp_peer_unref_delete(peer);

	return ret;
}

/* dp_txrx_reset_peer_stats - reset cdp_peer_stats for particular peer
 * @soc: soc handle
 * @vdev_id: id of vdev handle
 * @peer_mac: mac of DP_PEER handle
 *
 * return : QDF_STATUS
 */
static QDF_STATUS
dp_txrx_reset_peer_stats(struct cdp_soc_t *soc, uint8_t vdev_id,
			 uint8_t *peer_mac)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_peer *peer = dp_peer_find_hash_find((struct dp_soc *)soc,
						       peer_mac, 0, vdev_id);

	if (!peer || peer->delete_in_progress) {
		status = QDF_STATUS_E_FAILURE;
		goto fail;
	}

	qdf_mem_zero(&peer->stats, sizeof(peer->stats));

fail:
	if (peer)
		dp_peer_unref_delete(peer);

	return status;
}

/* dp_txrx_get_vdev_stats - Update buffer with cdp_vdev_stats
 * @vdev_handle: DP_VDEV handle
 * @buf: buffer for vdev stats
 *
 * return : int
 */
static int dp_txrx_get_vdev_stats(struct cdp_soc_t *soc, uint8_t vdev_id,
				   void *buf, bool is_aggregate)
{
	struct cdp_vdev_stats *vdev_stats;
	struct dp_pdev *pdev;
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev)
		return 1;

	pdev = vdev->pdev;
	if (!pdev)
		return 1;

	vdev_stats = (struct cdp_vdev_stats *)buf;

	if (is_aggregate) {
		qdf_spin_lock_bh(&((struct dp_soc *)soc)->peer_ref_mutex);
		dp_aggregate_vdev_stats(vdev, buf);
		qdf_spin_unlock_bh(&((struct dp_soc *)soc)->peer_ref_mutex);
	} else {
		qdf_mem_copy(vdev_stats, &vdev->stats, sizeof(vdev->stats));
	}

	return 0;
}

/*
 * dp_get_total_per(): get total per
 * @soc: DP soc handle
 * @pdev_id: id of DP_PDEV handle
 *
 * Return: % error rate using retries per packet and success packets
 */
static int dp_get_total_per(struct cdp_soc_t *soc, uint8_t pdev_id)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return 0;

	dp_aggregate_pdev_stats(pdev);
	if ((pdev->stats.tx.tx_success.num + pdev->stats.tx.retries) == 0)
		return 0;
	return ((pdev->stats.tx.retries * 100) /
		((pdev->stats.tx.tx_success.num) + (pdev->stats.tx.retries)));
}

/*
 * dp_txrx_stats_publish(): publish pdev stats into a buffer
 * @soc: DP soc handle
 * @pdev_id: id of DP_PDEV handle
 * @buf: to hold pdev_stats
 *
 * Return: int
 */
static int
dp_txrx_stats_publish(struct cdp_soc_t *soc, uint8_t pdev_id,
		      struct cdp_stats_extd *buf)
{
	struct cdp_txrx_stats_req req = {0,};
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return TXRX_STATS_LEVEL_OFF;

	dp_aggregate_pdev_stats(pdev);
	req.stats = (enum cdp_stats)HTT_DBG_EXT_STATS_PDEV_TX;
	req.cookie_val = 1;
	dp_h2t_ext_stats_msg_send(pdev, req.stats, req.param0,
				req.param1, req.param2, req.param3, 0,
				req.cookie_val, 0);

	msleep(DP_MAX_SLEEP_TIME);

	req.stats = (enum cdp_stats)HTT_DBG_EXT_STATS_PDEV_RX;
	req.cookie_val = 1;
	dp_h2t_ext_stats_msg_send(pdev, req.stats, req.param0,
				req.param1, req.param2, req.param3, 0,
				req.cookie_val, 0);

	msleep(DP_MAX_SLEEP_TIME);
	qdf_mem_copy(buf, &pdev->stats, sizeof(struct cdp_pdev_stats));

	return TXRX_STATS_LEVEL;
}

/**
 * dp_set_pdev_dscp_tid_map_wifi3(): update dscp tid map in pdev
 * @soc: soc handle
 * @pdev_id: id of DP_PDEV handle
 * @map_id: ID of map that needs to be updated
 * @tos: index value in map
 * @tid: tid value passed by the user
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_set_pdev_dscp_tid_map_wifi3(struct cdp_soc_t *soc_handle,
			       uint8_t pdev_id,
			       uint8_t map_id,
			       uint8_t tos, uint8_t tid)
{
	uint8_t dscp;
	struct dp_soc *soc = (struct dp_soc *)soc_handle;
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	dscp = (tos >> DP_IP_DSCP_SHIFT) & DP_IP_DSCP_MASK;
	pdev->dscp_tid_map[map_id][dscp] = tid;

	if (map_id < soc->num_hw_dscp_tid_map)
		hal_tx_update_dscp_tid(soc->hal_soc, tid,
				       map_id, dscp);
	else
		return QDF_STATUS_E_FAILURE;

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_fw_stats_process(): Process TxRX FW stats request
 * @vdev_handle: DP VDEV handle
 * @req: stats request
 *
 * return: int
 */
static int dp_fw_stats_process(struct dp_vdev *vdev,
			       struct cdp_txrx_stats_req *req)
{
	struct dp_pdev *pdev = NULL;
	uint32_t stats = req->stats;
	uint8_t mac_id = req->mac_id;

	if (!vdev) {
		DP_TRACE(NONE, "VDEV not found");
		return 1;
	}
	pdev = vdev->pdev;

	/*
	 * For HTT_DBG_EXT_STATS_RESET command, FW need to config
	 * from param0 to param3 according to below rule:
	 *
	 * PARAM:
	 *   - config_param0 : start_offset (stats type)
	 *   - config_param1 : stats bmask from start offset
	 *   - config_param2 : stats bmask from start offset + 32
	 *   - config_param3 : stats bmask from start offset + 64
	 */
	if (req->stats == CDP_TXRX_STATS_0) {
		req->param0 = HTT_DBG_EXT_STATS_PDEV_TX;
		req->param1 = 0xFFFFFFFF;
		req->param2 = 0xFFFFFFFF;
		req->param3 = 0xFFFFFFFF;
	} else if (req->stats == (uint8_t)HTT_DBG_EXT_STATS_PDEV_TX_MU) {
		req->param0 = HTT_DBG_EXT_STATS_SET_VDEV_MASK(vdev->vdev_id);
	}

	return dp_h2t_ext_stats_msg_send(pdev, stats, req->param0,
				req->param1, req->param2, req->param3,
				0, 0, mac_id);
}

/**
 * dp_txrx_stats_request - function to map to firmware and host stats
 * @soc: soc handle
 * @vdev_id: virtual device ID
 * @req: stats request
 *
 * Return: QDF_STATUS
 */
static
QDF_STATUS dp_txrx_stats_request(struct cdp_soc_t *soc_handle,
				 uint8_t vdev_id,
				 struct cdp_txrx_stats_req *req)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_handle);
	int host_stats;
	int fw_stats;
	enum cdp_stats stats;
	int num_stats;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc,
								  vdev_id);

	if (!vdev || !req) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				"Invalid vdev/req instance");
		return QDF_STATUS_E_INVAL;
	}

	if (req->mac_id >= WLAN_CFG_MAC_PER_TARGET) {
		dp_err("Invalid mac id request");
		return QDF_STATUS_E_INVAL;
	}

	stats = req->stats;
	if (stats >= CDP_TXRX_MAX_STATS)
		return QDF_STATUS_E_INVAL;

	/*
	 * DP_CURR_FW_STATS_AVAIL: no of FW stats currently available
	 *			has to be updated if new FW HTT stats added
	 */
	if (stats > CDP_TXRX_STATS_HTT_MAX)
		stats = stats + DP_CURR_FW_STATS_AVAIL - DP_HTT_DBG_EXT_STATS_MAX;

	num_stats  = QDF_ARRAY_SIZE(dp_stats_mapping_table);

	if (stats >= num_stats) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "%s: Invalid stats option: %d", __func__, stats);
		return QDF_STATUS_E_INVAL;
	}

	req->stats = stats;
	fw_stats = dp_stats_mapping_table[stats][STATS_FW];
	host_stats = dp_stats_mapping_table[stats][STATS_HOST];

	dp_info("stats: %u fw_stats_type: %d host_stats: %d",
		stats, fw_stats, host_stats);

	if (fw_stats != TXRX_FW_STATS_INVALID) {
		/* update request with FW stats type */
		req->stats = fw_stats;
		return dp_fw_stats_process(vdev, req);
	}

	if ((host_stats != TXRX_HOST_STATS_INVALID) &&
			(host_stats <= TXRX_HOST_STATS_MAX))
		return dp_print_host_stats(vdev, req);
	else
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
				"Wrong Input for TxRx Stats");

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_txrx_dump_stats() -  Dump statistics
 * @value - Statistics option
 */
static QDF_STATUS dp_txrx_dump_stats(struct cdp_soc_t *psoc, uint16_t value,
				     enum qdf_stats_verbosity_level level)
{
	struct dp_soc *soc =
		(struct dp_soc *)psoc;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!soc) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"%s: soc is NULL", __func__);
		return QDF_STATUS_E_INVAL;
	}

	switch (value) {
	case CDP_TXRX_PATH_STATS:
		dp_txrx_path_stats(soc);
		dp_print_soc_interrupt_stats(soc);
		hal_dump_reg_write_stats(soc->hal_soc);
		break;

	case CDP_RX_RING_STATS:
		dp_print_per_ring_stats(soc);
		break;

	case CDP_TXRX_TSO_STATS:
		dp_print_tso_stats(soc, level);
		break;

	case CDP_DUMP_TX_FLOW_POOL_INFO:
		if (level == QDF_STATS_VERBOSITY_LEVEL_HIGH)
			cdp_dump_flow_pool_info((struct cdp_soc_t *)soc);
		break;

	case CDP_DP_NAPI_STATS:
		dp_print_napi_stats(soc);
		break;

	case CDP_TXRX_DESC_STATS:
		/* TODO: NOT IMPLEMENTED */
		break;

	case CDP_DP_RX_FISA_STATS:
		dp_rx_dump_fisa_stats(soc);
		break;

	default:
		status = QDF_STATUS_E_INVAL;
		break;
	}

	return status;

}

/**
 * dp_txrx_clear_dump_stats() - clear dumpStats
 * @soc- soc handle
 * @value - stats option
 *
 * Return: 0 - Success, non-zero - failure
 */
static
QDF_STATUS dp_txrx_clear_dump_stats(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
				    uint8_t value)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!soc) {
		dp_err("%s: soc is NULL", __func__);
		return QDF_STATUS_E_INVAL;
	}

	switch (value) {
	case CDP_TXRX_TSO_STATS:
		dp_txrx_clear_tso_stats(soc);
		break;

	default:
		status = QDF_STATUS_E_INVAL;
		break;
	}

	return status;
}

#ifdef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * dp_update_flow_control_parameters() - API to store datapath
 *                            config parameters
 * @soc: soc handle
 * @cfg: ini parameter handle
 *
 * Return: void
 */
static inline
void dp_update_flow_control_parameters(struct dp_soc *soc,
				struct cdp_config_params *params)
{
	soc->wlan_cfg_ctx->tx_flow_stop_queue_threshold =
					params->tx_flow_stop_queue_threshold;
	soc->wlan_cfg_ctx->tx_flow_start_queue_offset =
					params->tx_flow_start_queue_offset;
}
#else
static inline
void dp_update_flow_control_parameters(struct dp_soc *soc,
				struct cdp_config_params *params)
{
}
#endif

#ifdef WLAN_FEATURE_RX_SOFTIRQ_TIME_LIMIT
/* Max packet limit for TX Comp packet loop (dp_tx_comp_handler) */
#define DP_TX_COMP_LOOP_PKT_LIMIT_MAX 1024

/* Max packet limit for RX REAP Loop (dp_rx_process) */
#define DP_RX_REAP_LOOP_PKT_LIMIT_MAX 1024

static
void dp_update_rx_soft_irq_limit_params(struct dp_soc *soc,
					struct cdp_config_params *params)
{
	soc->wlan_cfg_ctx->tx_comp_loop_pkt_limit =
				params->tx_comp_loop_pkt_limit;

	if (params->tx_comp_loop_pkt_limit < DP_TX_COMP_LOOP_PKT_LIMIT_MAX)
		soc->wlan_cfg_ctx->tx_comp_enable_eol_data_check = true;
	else
		soc->wlan_cfg_ctx->tx_comp_enable_eol_data_check = false;

	soc->wlan_cfg_ctx->rx_reap_loop_pkt_limit =
				params->rx_reap_loop_pkt_limit;

	if (params->rx_reap_loop_pkt_limit < DP_RX_REAP_LOOP_PKT_LIMIT_MAX)
		soc->wlan_cfg_ctx->rx_enable_eol_data_check = true;
	else
		soc->wlan_cfg_ctx->rx_enable_eol_data_check = false;

	soc->wlan_cfg_ctx->rx_hp_oos_update_limit =
				params->rx_hp_oos_update_limit;

	dp_info("tx_comp_loop_pkt_limit %u tx_comp_enable_eol_data_check %u rx_reap_loop_pkt_limit %u rx_enable_eol_data_check %u rx_hp_oos_update_limit %u",
		soc->wlan_cfg_ctx->tx_comp_loop_pkt_limit,
		soc->wlan_cfg_ctx->tx_comp_enable_eol_data_check,
		soc->wlan_cfg_ctx->rx_reap_loop_pkt_limit,
		soc->wlan_cfg_ctx->rx_enable_eol_data_check,
		soc->wlan_cfg_ctx->rx_hp_oos_update_limit);
}
#else
static inline
void dp_update_rx_soft_irq_limit_params(struct dp_soc *soc,
					struct cdp_config_params *params)
{ }
#endif /* WLAN_FEATURE_RX_SOFTIRQ_TIME_LIMIT */

/**
 * dp_update_config_parameters() - API to store datapath
 *                            config parameters
 * @soc: soc handle
 * @cfg: ini parameter handle
 *
 * Return: status
 */
static
QDF_STATUS dp_update_config_parameters(struct cdp_soc *psoc,
				struct cdp_config_params *params)
{
	struct dp_soc *soc = (struct dp_soc *)psoc;

	if (!(soc)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				"%s: Invalid handle", __func__);
		return QDF_STATUS_E_INVAL;
	}

	soc->wlan_cfg_ctx->tso_enabled = params->tso_enable;
	soc->wlan_cfg_ctx->lro_enabled = params->lro_enable;
	soc->wlan_cfg_ctx->rx_hash = params->flow_steering_enable;
	soc->wlan_cfg_ctx->p2p_tcp_udp_checksumoffload =
				params->p2p_tcp_udp_checksumoffload;
	soc->wlan_cfg_ctx->nan_tcp_udp_checksumoffload =
				params->nan_tcp_udp_checksumoffload;
	soc->wlan_cfg_ctx->tcp_udp_checksumoffload =
				params->tcp_udp_checksumoffload;
	soc->wlan_cfg_ctx->napi_enabled = params->napi_enable;
	soc->wlan_cfg_ctx->ipa_enabled = params->ipa_enable;
	soc->wlan_cfg_ctx->gro_enabled = params->gro_enable;

	dp_update_rx_soft_irq_limit_params(soc, params);
	dp_update_flow_control_parameters(soc, params);

	return QDF_STATUS_SUCCESS;
}

static struct cdp_wds_ops dp_ops_wds = {
	.vdev_set_wds = dp_vdev_set_wds,
#ifdef WDS_VENDOR_EXTENSION
	.txrx_set_wds_rx_policy = dp_txrx_set_wds_rx_policy,
	.txrx_wds_peer_tx_policy_update = dp_txrx_peer_wds_tx_policy_update,
#endif
};

/*
 * dp_txrx_data_tx_cb_set(): set the callback for non standard tx
 * @soc_hdl - datapath soc handle
 * @vdev_id - virtual interface id
 * @callback - callback function
 * @ctxt: callback context
 *
 */
static void
dp_txrx_data_tx_cb_set(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		       ol_txrx_data_tx_cb callback, void *ctxt)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);

	if (!vdev)
		return;

	vdev->tx_non_std_data_callback.func = callback;
	vdev->tx_non_std_data_callback.ctxt = ctxt;
}

/**
 * dp_pdev_get_dp_txrx_handle() - get dp handle from pdev
 * @soc: datapath soc handle
 * @pdev_id: id of datapath pdev handle
 *
 * Return: opaque pointer to dp txrx handle
 */
static void *dp_pdev_get_dp_txrx_handle(struct cdp_soc_t *soc, uint8_t pdev_id)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	if (qdf_unlikely(!pdev))
		return NULL;

	return pdev->dp_txrx_handle;
}

/**
 * dp_pdev_set_dp_txrx_handle() - set dp handle in pdev
 * @soc: datapath soc handle
 * @pdev_id: id of datapath pdev handle
 * @dp_txrx_hdl: opaque pointer for dp_txrx_handle
 *
 * Return: void
 */
static void
dp_pdev_set_dp_txrx_handle(struct cdp_soc_t *soc, uint8_t pdev_id,
			   void *dp_txrx_hdl)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return;

	pdev->dp_txrx_handle = dp_txrx_hdl;
}

/**
 * dp_vdev_get_dp_ext_handle() - get dp handle from vdev
 * @soc: datapath soc handle
 * @vdev_id: vdev id
 *
 * Return: opaque pointer to dp txrx handle
 */
static void *dp_vdev_get_dp_ext_handle(ol_txrx_soc_handle soc, uint8_t vdev_id)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev)
		return NULL;

	return vdev->vdev_dp_ext_handle;
}

/**
 * dp_vdev_set_dp_ext_handle() - set dp handle in vdev
 * @soc: datapath soc handle
 * @vdev_id: vdev id
 * @size: size of advance dp handle
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_vdev_set_dp_ext_handle(ol_txrx_soc_handle soc, uint8_t vdev_id,
			  uint16_t size)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	void *dp_ext_handle;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	dp_ext_handle = qdf_mem_malloc(size);

	if (!dp_ext_handle)
		return QDF_STATUS_E_FAILURE;

	vdev->vdev_dp_ext_handle = dp_ext_handle;
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_soc_get_dp_txrx_handle() - get context for external-dp from dp soc
 * @soc_handle: datapath soc handle
 *
 * Return: opaque pointer to external dp (non-core DP)
 */
static void *dp_soc_get_dp_txrx_handle(struct cdp_soc *soc_handle)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;

	return soc->external_txrx_handle;
}

/**
 * dp_soc_set_dp_txrx_handle() - set external dp handle in soc
 * @soc_handle: datapath soc handle
 * @txrx_handle: opaque pointer to external dp (non-core DP)
 *
 * Return: void
 */
static void
dp_soc_set_dp_txrx_handle(struct cdp_soc *soc_handle, void *txrx_handle)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;

	soc->external_txrx_handle = txrx_handle;
}

/**
 * dp_soc_map_pdev_to_lmac() - Save pdev_id to lmac_id mapping
 * @soc_hdl: datapath soc handle
 * @pdev_id: id of the datapath pdev handle
 * @lmac_id: lmac id
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_soc_map_pdev_to_lmac
	(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
	 uint32_t lmac_id)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;

	wlan_cfg_set_hw_mac_idx(soc->wlan_cfg_ctx,
				pdev_id,
				lmac_id);

	/*Set host PDEV ID for lmac_id*/
	wlan_cfg_set_pdev_idx(soc->wlan_cfg_ctx,
			      pdev_id,
			      lmac_id);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_soc_handle_pdev_mode_change() - Update pdev to lmac mapping
 * @soc_hdl: datapath soc handle
 * @pdev_id: id of the datapath pdev handle
 * @lmac_id: lmac id
 *
 * In the event of a dynamic mode change, update the pdev to lmac mapping
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_soc_handle_pdev_mode_change
	(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
	 uint32_t lmac_id)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_vdev *vdev = NULL;
	uint8_t hw_pdev_id, mac_id;
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc,
								  pdev_id);
	int nss_config = wlan_cfg_get_dp_soc_nss_cfg(soc->wlan_cfg_ctx);

	if (qdf_unlikely(!pdev))
		return QDF_STATUS_E_FAILURE;

	pdev->lmac_id = lmac_id;
	dp_info(" mode change %d %d\n", pdev->pdev_id, pdev->lmac_id);

	/*Set host PDEV ID for lmac_id*/
	wlan_cfg_set_pdev_idx(soc->wlan_cfg_ctx,
			      pdev->pdev_id,
			      lmac_id);

	hw_pdev_id =
		dp_get_target_pdev_id_for_host_pdev_id(soc,
						       pdev->pdev_id);

	/*
	 * When NSS offload is enabled, send pdev_id->lmac_id
	 * and pdev_id to hw_pdev_id to NSS FW
	 */
	if (nss_config) {
		mac_id = pdev->lmac_id;
		if (soc->cdp_soc.ol_ops->pdev_update_lmac_n_target_pdev_id)
			soc->cdp_soc.ol_ops->
				pdev_update_lmac_n_target_pdev_id(
				soc->ctrl_psoc,
				&pdev_id, &mac_id, &hw_pdev_id);
	}

	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {
		HTT_TX_TCL_METADATA_PDEV_ID_SET(vdev->htt_tcl_metadata,
						hw_pdev_id);
	}
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_soc_set_pdev_status_down() - set pdev down/up status
 * @soc: datapath soc handle
 * @pdev_id: id of datapath pdev handle
 * @is_pdev_down: pdev down/up status
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_soc_set_pdev_status_down(struct cdp_soc_t *soc, uint8_t pdev_id,
			    bool is_pdev_down)
{
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);
	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	pdev->is_pdev_down = is_pdev_down;
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_get_cfg_capabilities() - get dp capabilities
 * @soc_handle: datapath soc handle
 * @dp_caps: enum for dp capabilities
 *
 * Return: bool to determine if dp caps is enabled
 */
static bool
dp_get_cfg_capabilities(struct cdp_soc_t *soc_handle,
			enum cdp_capabilities dp_caps)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;

	return wlan_cfg_get_dp_caps(soc->wlan_cfg_ctx, dp_caps);
}

#ifdef FEATURE_AST
static QDF_STATUS
dp_peer_teardown_wifi3(struct cdp_soc_t *soc_hdl, uint8_t vdev_id,
		       uint8_t *peer_mac)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct dp_peer *peer =
			dp_peer_find_hash_find(soc, peer_mac, 0, vdev_id);

	/* Peer can be null for monitor vap mac address */
	if (!peer) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			  "%s: Invalid peer\n", __func__);
		return QDF_STATUS_E_FAILURE;
	}
	/*
	 * For BSS peer, new peer is not created on alloc_node if the
	 * peer with same address already exists , instead refcnt is
	 * increased for existing peer. Correspondingly in delete path,
	 * only refcnt is decreased; and peer is only deleted , when all
	 * references are deleted. So delete_in_progress should not be set
	 * for bss_peer, unless only 3 reference remains (peer map reference,
	 * peer hash table reference and above local reference).
	 */
	if (peer->bss_peer && (qdf_atomic_read(&peer->ref_cnt) > 3)) {
		status =  QDF_STATUS_E_FAILURE;
		goto fail;
	}

	qdf_spin_lock_bh(&soc->ast_lock);
	peer->delete_in_progress = true;
	dp_peer_delete_ast_entries(soc, peer);
	qdf_spin_unlock_bh(&soc->ast_lock);

fail:
	if (peer)
		dp_peer_unref_delete(peer);
	return status;
}
#endif

#ifdef ATH_SUPPORT_NAC_RSSI
/**
 * dp_vdev_get_neighbour_rssi(): Store RSSI for configured NAC
 * @soc_hdl: DP soc handle
 * @vdev_id: id of DP vdev handle
 * @mac_addr: neighbour mac
 * @rssi: rssi value
 *
 * Return: 0 for success. nonzero for failure.
 */
static QDF_STATUS  dp_vdev_get_neighbour_rssi(struct cdp_soc_t *soc,
					      uint8_t vdev_id,
					      char *mac_addr,
					      uint8_t *rssi)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	struct dp_pdev *pdev;
	struct dp_neighbour_peer *peer = NULL;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (!vdev)
		return status;

	pdev = vdev->pdev;
	*rssi = 0;
	qdf_spin_lock_bh(&pdev->neighbour_peer_mutex);
	TAILQ_FOREACH(peer, &pdev->neighbour_peers_list,
		      neighbour_peer_list_elem) {
		if (qdf_mem_cmp(&peer->neighbour_peers_macaddr.raw[0],
				mac_addr, QDF_MAC_ADDR_SIZE) == 0) {
			*rssi = peer->rssi;
			status = QDF_STATUS_SUCCESS;
			break;
		}
	}
	qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);
	return status;
}

static QDF_STATUS
dp_config_for_nac_rssi(struct cdp_soc_t *cdp_soc,
		       uint8_t vdev_id,
		       enum cdp_nac_param_cmd cmd, char *bssid,
		       char *client_macaddr,
		       uint8_t chan_num)
{
	struct dp_soc *soc = (struct dp_soc *)cdp_soc;
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3(soc,
						   vdev_id);
	struct dp_pdev *pdev;

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	pdev = (struct dp_pdev *)vdev->pdev;
	pdev->nac_rssi_filtering = 1;
	/* Store address of NAC (neighbour peer) which will be checked
	 * against TA of received packets.
	 */

	if (cmd == CDP_NAC_PARAM_ADD) {
		dp_update_filter_neighbour_peers(cdp_soc, vdev->vdev_id,
						 DP_NAC_PARAM_ADD,
						 (uint8_t *)client_macaddr);
	} else if (cmd == CDP_NAC_PARAM_DEL) {
		dp_update_filter_neighbour_peers(cdp_soc, vdev->vdev_id,
						 DP_NAC_PARAM_DEL,
						 (uint8_t *)client_macaddr);
	}

	if (soc->cdp_soc.ol_ops->config_bssid_in_fw_for_nac_rssi)
		soc->cdp_soc.ol_ops->config_bssid_in_fw_for_nac_rssi
			(soc->ctrl_psoc, pdev->pdev_id,
			 vdev->vdev_id, cmd, bssid, client_macaddr);

	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * dp_enable_peer_based_pktlog() - Set Flag for peer based filtering
 * for pktlog
 * @soc: cdp_soc handle
 * @pdev_id: id of dp pdev handle
 * @mac_addr: Peer mac address
 * @enb_dsb: Enable or disable peer based filtering
 *
 * Return: QDF_STATUS
 */
static int
dp_enable_peer_based_pktlog(struct cdp_soc_t *soc, uint8_t pdev_id,
			    uint8_t *mac_addr, uint8_t enb_dsb)
{
	struct dp_peer *peer;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev) {
		dp_err("Invalid Pdev for pdev_id %d", pdev_id);
		return QDF_STATUS_E_FAILURE;
	}

	peer = (struct dp_peer *)dp_find_peer_by_addr((struct cdp_pdev *)pdev,
						      mac_addr);

	if (!peer) {
		dp_err("Invalid Peer");
		return QDF_STATUS_E_FAILURE;
	}

	peer->peer_based_pktlog_filter = enb_dsb;
	pdev->dp_peer_based_pktlog = enb_dsb;

	return QDF_STATUS_SUCCESS;
}

#ifndef WLAN_SUPPORT_RX_TAG_STATISTICS
/**
 * dp_dump_pdev_rx_protocol_tag_stats - dump the number of packets tagged for
 * given protocol type (RX_PROTOCOL_TAG_ALL indicates for all protocol)
 * @soc: cdp_soc handle
 * @pdev_id: id of cdp_pdev handle
 * @protocol_type: protocol type for which stats should be displayed
 *
 * Return: none
 */
static inline void
dp_dump_pdev_rx_protocol_tag_stats(struct cdp_soc_t  *soc, uint8_t pdev_id,
				   uint16_t protocol_type)
{
}
#endif /* WLAN_SUPPORT_RX_TAG_STATISTICS */

#ifndef WLAN_SUPPORT_RX_PROTOCOL_TYPE_TAG
/**
 * dp_update_pdev_rx_protocol_tag - Add/remove a protocol tag that should be
 * applied to the desired protocol type packets
 * @soc: soc handle
 * @pdev_id: id of cdp_pdev handle
 * @enable_rx_protocol_tag - bitmask that indicates what protocol types
 * are enabled for tagging. zero indicates disable feature, non-zero indicates
 * enable feature
 * @protocol_type: new protocol type for which the tag is being added
 * @tag: user configured tag for the new protocol
 *
 * Return: Success
 */
static inline QDF_STATUS
dp_update_pdev_rx_protocol_tag(struct cdp_soc_t  *soc, uint8_t pdev_id,
			       uint32_t enable_rx_protocol_tag,
			       uint16_t protocol_type,
			       uint16_t tag)
{
	return QDF_STATUS_SUCCESS;
}
#endif /* WLAN_SUPPORT_RX_PROTOCOL_TYPE_TAG */

#ifndef WLAN_SUPPORT_RX_FLOW_TAG
/**
 * dp_set_rx_flow_tag - add/delete a flow
 * @soc: soc handle
 * @pdev_id: id of cdp_pdev handle
 * @flow_info: flow tuple that is to be added to/deleted from flow search table
 *
 * Return: Success
 */
static inline QDF_STATUS
dp_set_rx_flow_tag(struct cdp_soc_t *cdp_soc, uint8_t pdev_id,
		   struct cdp_rx_flow_info *flow_info)
{
	return QDF_STATUS_SUCCESS;
}
/**
 * dp_dump_rx_flow_tag_stats - dump the number of packets tagged for
 * given flow 5-tuple
 * @cdp_soc: soc handle
 * @pdev_id: id of cdp_pdev handle
 * @flow_info: flow 5-tuple for which stats should be displayed
 *
 * Return: Success
 */
static inline QDF_STATUS
dp_dump_rx_flow_tag_stats(struct cdp_soc_t *cdp_soc, uint8_t pdev_id,
			  struct cdp_rx_flow_info *flow_info)
{
	return QDF_STATUS_SUCCESS;
}
#endif /* WLAN_SUPPORT_RX_FLOW_TAG */

static QDF_STATUS dp_peer_map_attach_wifi3(struct cdp_soc_t  *soc_hdl,
					   uint32_t max_peers,
					   uint32_t max_ast_index,
					   bool peer_map_unmap_v2)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;

	soc->max_peers = max_peers;

	qdf_print ("%s max_peers %u, max_ast_index: %u\n",
		   __func__, max_peers, max_ast_index);
	wlan_cfg_set_max_ast_idx(soc->wlan_cfg_ctx, max_ast_index);

	if (dp_peer_find_attach(soc))
		return QDF_STATUS_E_FAILURE;

	soc->is_peer_map_unmap_v2 = peer_map_unmap_v2;

	return QDF_STATUS_SUCCESS;
}

static void dp_soc_set_rate_stats_ctx(struct cdp_soc_t *soc_handle,
				      void *stats_ctx)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;

	soc->rate_stats_ctx = (struct cdp_soc_rate_stats_ctx *)stats_ctx;
}

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
static QDF_STATUS dp_flush_rate_stats_req(struct cdp_soc_t *soc_hdl,
					  uint8_t pdev_id)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_vdev *vdev = NULL;
	struct dp_peer *peer = NULL;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {
		TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem) {
			if (peer && !peer->bss_peer)
				dp_wdi_event_handler(
					WDI_EVENT_FLUSH_RATE_STATS_REQ,
					soc, peer->wlanstats_ctx,
					peer->peer_ids[0],
					WDI_NO_VAL, pdev_id);
		}
	}
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	return QDF_STATUS_SUCCESS;
}
#else
static inline QDF_STATUS
dp_flush_rate_stats_req(struct cdp_soc_t *soc_hdl,
			uint8_t pdev_id)
{
	return QDF_STATUS_SUCCESS;
}
#endif

#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
static QDF_STATUS dp_peer_flush_rate_stats(struct cdp_soc_t *soc,
					   uint8_t pdev_id,
					   void *buf)
{
	 dp_wdi_event_handler(WDI_EVENT_PEER_FLUSH_RATE_STATS,
			      (struct dp_soc *)soc, buf, HTT_INVALID_PEER,
			      WDI_NO_VAL, pdev_id);
	return QDF_STATUS_SUCCESS;
}
#else
static inline QDF_STATUS
dp_peer_flush_rate_stats(struct cdp_soc_t *soc,
			 uint8_t pdev_id,
			 void *buf)
{
	return QDF_STATUS_SUCCESS;
}
#endif

static void *dp_soc_get_rate_stats_ctx(struct cdp_soc_t *soc_handle)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;

	return soc->rate_stats_ctx;
}

/*
 * dp_get_cfg() - get dp cfg
 * @soc: cdp soc handle
 * @cfg: cfg enum
 *
 * Return: cfg value
 */
static uint32_t dp_get_cfg(struct cdp_soc_t *soc, enum cdp_dp_cfg cfg)
{
	struct dp_soc *dpsoc = (struct dp_soc *)soc;
	uint32_t value = 0;

	switch (cfg) {
	case cfg_dp_enable_data_stall:
		value = dpsoc->wlan_cfg_ctx->enable_data_stall_detection;
		break;
	case cfg_dp_enable_p2p_ip_tcp_udp_checksum_offload:
		value = dpsoc->wlan_cfg_ctx->p2p_tcp_udp_checksumoffload;
		break;
	case cfg_dp_enable_nan_ip_tcp_udp_checksum_offload:
		value = dpsoc->wlan_cfg_ctx->nan_tcp_udp_checksumoffload;
		break;
	case cfg_dp_enable_ip_tcp_udp_checksum_offload:
		value = dpsoc->wlan_cfg_ctx->tcp_udp_checksumoffload;
		break;
	case cfg_dp_disable_legacy_mode_csum_offload:
		value = dpsoc->wlan_cfg_ctx->
					legacy_mode_checksumoffload_disable;
		break;
	case cfg_dp_tso_enable:
		value = dpsoc->wlan_cfg_ctx->tso_enabled;
		break;
	case cfg_dp_lro_enable:
		value = dpsoc->wlan_cfg_ctx->lro_enabled;
		break;
	case cfg_dp_gro_enable:
		value = dpsoc->wlan_cfg_ctx->gro_enabled;
		break;
	case cfg_dp_tx_flow_start_queue_offset:
		value = dpsoc->wlan_cfg_ctx->tx_flow_start_queue_offset;
		break;
	case cfg_dp_tx_flow_stop_queue_threshold:
		value = dpsoc->wlan_cfg_ctx->tx_flow_stop_queue_threshold;
		break;
	case cfg_dp_disable_intra_bss_fwd:
		value = dpsoc->wlan_cfg_ctx->disable_intra_bss_fwd;
		break;
	case cfg_dp_pktlog_buffer_size:
		value = dpsoc->wlan_cfg_ctx->pktlog_buffer_size;
		break;
	case cfg_dp_wow_check_rx_pending:
		value = dpsoc->wlan_cfg_ctx->wow_check_rx_pending_enable;
		break;
	default:
		value =  0;
	}

	return value;
}

#ifdef PEER_FLOW_CONTROL
/**
 * dp_tx_flow_ctrl_configure_pdev() - Configure flow control params
 * @soc_handle: datapath soc handle
 * @pdev_id: id of datapath pdev handle
 * @param: ol ath params
 * @value: value of the flag
 * @buff: Buffer to be passed
 *
 * Implemented this function same as legacy function. In legacy code, single
 * function is used to display stats and update pdev params.
 *
 * Return: 0 for success. nonzero for failure.
 */
static uint32_t dp_tx_flow_ctrl_configure_pdev(struct cdp_soc_t *soc_handle,
					       uint8_t pdev_id,
					       enum _ol_ath_param_t param,
					       uint32_t value, void *buff)
{
	struct dp_soc *soc = (struct dp_soc *)soc_handle;
	struct dp_pdev *pdev =
		dp_get_pdev_from_soc_pdev_id_wifi3((struct dp_soc *)soc,
						   pdev_id);

	if (qdf_unlikely(!pdev))
		return 1;

	soc = pdev->soc;
	if (!soc)
		return 1;

	switch (param) {
#ifdef QCA_ENH_V3_STATS_SUPPORT
	case OL_ATH_PARAM_VIDEO_DELAY_STATS_FC:
		if (value)
			pdev->delay_stats_flag = true;
		else
			pdev->delay_stats_flag = false;
		break;
	case OL_ATH_PARAM_VIDEO_STATS_FC:
		qdf_print("------- TID Stats ------\n");
		dp_pdev_print_tid_stats(pdev);
		qdf_print("------ Delay Stats ------\n");
		dp_pdev_print_delay_stats(pdev);
		break;
#endif
	case OL_ATH_PARAM_TOTAL_Q_SIZE:
		{
			uint32_t tx_min, tx_max;

			tx_min = wlan_cfg_get_min_tx_desc(soc->wlan_cfg_ctx);
			tx_max = wlan_cfg_get_num_tx_desc(soc->wlan_cfg_ctx);

			if (!buff) {
				if ((value >= tx_min) && (value <= tx_max)) {
					pdev->num_tx_allowed = value;
				} else {
					QDF_TRACE(QDF_MODULE_ID_DP,
						  QDF_TRACE_LEVEL_INFO,
						  "Failed to update num_tx_allowed, Q_min = %d Q_max = %d",
						  tx_min, tx_max);
					break;
				}
			} else {
				*(int *)buff = pdev->num_tx_allowed;
			}
		}
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			  "%s: not handled param %d ", __func__, param);
		break;
	}

	return 0;
}
#endif

/**
 * dp_set_pdev_pcp_tid_map_wifi3(): update pcp tid map in pdev
 * @psoc: dp soc handle
 * @pdev_id: id of DP_PDEV handle
 * @pcp: pcp value
 * @tid: tid value passed by the user
 *
 * Return: QDF_STATUS_SUCCESS on success
 */
static QDF_STATUS dp_set_pdev_pcp_tid_map_wifi3(ol_txrx_soc_handle psoc,
						uint8_t pdev_id,
						uint8_t pcp, uint8_t tid)
{
	struct dp_soc *soc = (struct dp_soc *)psoc;

	soc->pcp_tid_map[pcp] = tid;

	hal_tx_update_pcp_tid_map(soc->hal_soc, pcp, tid);
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_set_vdev_pcp_tid_map_wifi3(): update pcp tid map in vdev
 * @soc: DP soc handle
 * @vdev_id: id of DP_VDEV handle
 * @pcp: pcp value
 * @tid: tid value passed by the user
 *
 * Return: QDF_STATUS_SUCCESS on success
 */
static QDF_STATUS dp_set_vdev_pcp_tid_map_wifi3(struct cdp_soc_t *soc,
						uint8_t vdev_id,
						uint8_t pcp, uint8_t tid)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);

	if (!vdev)
		return QDF_STATUS_E_FAILURE;

	vdev->pcp_tid_map[pcp] = tid;

	return QDF_STATUS_SUCCESS;
}

static struct cdp_cmn_ops dp_ops_cmn = {
	.txrx_soc_attach_target = dp_soc_attach_target_wifi3,
	.txrx_vdev_attach = dp_vdev_attach_wifi3,
	.txrx_vdev_detach = dp_vdev_detach_wifi3,
	.txrx_pdev_attach = dp_pdev_attach_wifi3,
	.txrx_pdev_detach = dp_pdev_detach_wifi3,
	.txrx_pdev_deinit = dp_pdev_deinit_wifi3,
	.txrx_peer_create = dp_peer_create_wifi3,
	.txrx_peer_setup = dp_peer_setup_wifi3,
#ifdef FEATURE_AST
	.txrx_peer_teardown = dp_peer_teardown_wifi3,
#else
	.txrx_peer_teardown = NULL,
#endif
	.txrx_peer_add_ast = dp_peer_add_ast_wifi3,
	.txrx_peer_update_ast = dp_peer_update_ast_wifi3,
	.txrx_peer_get_ast_info_by_soc = dp_peer_get_ast_info_by_soc_wifi3,
	.txrx_peer_get_ast_info_by_pdev =
		dp_peer_get_ast_info_by_pdevid_wifi3,
	.txrx_peer_ast_delete_by_soc =
		dp_peer_ast_entry_del_by_soc,
	.txrx_peer_ast_delete_by_pdev =
		dp_peer_ast_entry_del_by_pdev,
	.txrx_peer_delete = dp_peer_delete_wifi3,
	.txrx_vdev_register = dp_vdev_register_wifi3,
	.txrx_soc_detach = dp_soc_detach_wifi3,
	.txrx_soc_deinit = dp_soc_deinit_wifi3,
	.txrx_soc_init = dp_soc_init_wifi3,
	.txrx_tso_soc_attach = dp_tso_soc_attach,
	.txrx_tso_soc_detach = dp_tso_soc_detach,
	.txrx_get_vdev_mac_addr = dp_get_vdev_mac_addr_wifi3,
	.txrx_get_mon_vdev_from_pdev = dp_get_mon_vdev_from_pdev_wifi3,
	.txrx_get_ctrl_pdev_from_vdev = dp_get_ctrl_pdev_from_vdev_wifi3,
	.txrx_ath_getstats = dp_get_device_stats,
	.addba_requestprocess = dp_addba_requestprocess_wifi3,
	.addba_responsesetup = dp_addba_responsesetup_wifi3,
	.addba_resp_tx_completion = dp_addba_resp_tx_completion_wifi3,
	.delba_process = dp_delba_process_wifi3,
	.set_addba_response = dp_set_addba_response,
	.flush_cache_rx_queue = NULL,
	/* TODO: get API's for dscp-tid need to be added*/
	.set_vdev_dscp_tid_map = dp_set_vdev_dscp_tid_map_wifi3,
	.set_pdev_dscp_tid_map = dp_set_pdev_dscp_tid_map_wifi3,
	.txrx_get_total_per = dp_get_total_per,
	.txrx_stats_request = dp_txrx_stats_request,
	.txrx_set_monitor_mode = dp_vdev_set_monitor_mode,
	.txrx_get_peer_mac_from_peer_id = dp_get_peer_mac_from_peer_id,
	.display_stats = dp_txrx_dump_stats,
	.txrx_intr_attach = dp_soc_interrupt_attach_wrapper,
	.txrx_intr_detach = dp_soc_interrupt_detach,
	.set_pn_check = dp_set_pn_check_wifi3,
	.set_key_sec_type = dp_set_key_sec_type_wifi3,
	.update_config_parameters = dp_update_config_parameters,
	/* TODO: Add other functions */
	.txrx_data_tx_cb_set = dp_txrx_data_tx_cb_set,
	.get_dp_txrx_handle = dp_pdev_get_dp_txrx_handle,
	.set_dp_txrx_handle = dp_pdev_set_dp_txrx_handle,
	.get_vdev_dp_ext_txrx_handle = dp_vdev_get_dp_ext_handle,
	.set_vdev_dp_ext_txrx_handle = dp_vdev_set_dp_ext_handle,
	.get_soc_dp_txrx_handle = dp_soc_get_dp_txrx_handle,
	.set_soc_dp_txrx_handle = dp_soc_set_dp_txrx_handle,
	.map_pdev_to_lmac = dp_soc_map_pdev_to_lmac,
	.handle_mode_change = dp_soc_handle_pdev_mode_change,
	.set_pdev_status_down = dp_soc_set_pdev_status_down,
	.txrx_set_ba_aging_timeout = dp_set_ba_aging_timeout,
	.txrx_get_ba_aging_timeout = dp_get_ba_aging_timeout,
	.tx_send = dp_tx_send,
	.txrx_peer_reset_ast = dp_wds_reset_ast_wifi3,
	.txrx_peer_reset_ast_table = dp_wds_reset_ast_table_wifi3,
	.txrx_peer_flush_ast_table = dp_wds_flush_ast_table_wifi3,
	.txrx_peer_map_attach = dp_peer_map_attach_wifi3,
	.txrx_get_os_rx_handles_from_vdev =
					dp_get_os_rx_handles_from_vdev_wifi3,
	.delba_tx_completion = dp_delba_tx_completion_wifi3,
	.get_dp_capabilities = dp_get_cfg_capabilities,
	.txrx_get_cfg = dp_get_cfg,
	.set_rate_stats_ctx = dp_soc_set_rate_stats_ctx,
	.get_rate_stats_ctx = dp_soc_get_rate_stats_ctx,
	.txrx_peer_flush_rate_stats = dp_peer_flush_rate_stats,
	.txrx_flush_rate_stats_request = dp_flush_rate_stats_req,

	.set_pdev_pcp_tid_map = dp_set_pdev_pcp_tid_map_wifi3,
	.set_vdev_pcp_tid_map = dp_set_vdev_pcp_tid_map_wifi3,

	.txrx_cp_peer_del_response = dp_cp_peer_del_resp_handler,
#ifdef QCA_MULTIPASS_SUPPORT
	.set_vlan_groupkey = dp_set_vlan_groupkey,
#endif
	.get_peer_mac_list = dp_get_peer_mac_list,
	.tx_send_exc = dp_tx_send_exception,
};

static struct cdp_ctrl_ops dp_ops_ctrl = {
	.txrx_peer_authorize = dp_peer_authorize,
#ifdef VDEV_PEER_PROTOCOL_COUNT
	.txrx_enable_peer_protocol_count = dp_enable_vdev_peer_protocol_count,
	.txrx_set_peer_protocol_drop_mask =
		dp_enable_vdev_peer_protocol_drop_mask,
	.txrx_is_peer_protocol_count_enabled =
		dp_is_vdev_peer_protocol_count_enabled,
	.txrx_get_peer_protocol_drop_mask = dp_get_vdev_peer_protocol_drop_mask,
#endif
	.txrx_set_vdev_param = dp_set_vdev_param,
	.txrx_set_psoc_param = dp_set_psoc_param,
	.txrx_get_psoc_param = dp_get_psoc_param,
	.txrx_set_pdev_reo_dest = dp_set_pdev_reo_dest,
	.txrx_get_pdev_reo_dest = dp_get_pdev_reo_dest,
#if defined(ATH_SUPPORT_NAC_RSSI) || defined(ATH_SUPPORT_NAC)
	.txrx_update_filter_neighbour_peers =
		dp_update_filter_neighbour_peers,
#endif /* ATH_SUPPORT_NAC_RSSI || ATH_SUPPORT_NAC */
	.txrx_get_sec_type = dp_get_sec_type,
	.txrx_wdi_event_sub = dp_wdi_event_sub,
	.txrx_wdi_event_unsub = dp_wdi_event_unsub,
#ifdef WDI_EVENT_ENABLE
	.txrx_get_pldev = dp_get_pldev,
#endif
	.txrx_set_pdev_param = dp_set_pdev_param,
	.txrx_get_pdev_param = dp_get_pdev_param,
	.txrx_set_peer_param = dp_set_peer_param,
	.txrx_get_peer_param = dp_get_peer_param,
#ifdef VDEV_PEER_PROTOCOL_COUNT
	.txrx_peer_protocol_cnt = dp_peer_stats_update_protocol_cnt,
#endif
#ifdef ATH_SUPPORT_NAC_RSSI
	.txrx_vdev_config_for_nac_rssi = dp_config_for_nac_rssi,
	.txrx_vdev_get_neighbour_rssi = dp_vdev_get_neighbour_rssi,
#endif
	.set_key = dp_set_michael_key,
	.txrx_get_vdev_param = dp_get_vdev_param,
	.enable_peer_based_pktlog = dp_enable_peer_based_pktlog,
	.calculate_delay_stats = dp_calculate_delay_stats,
#ifdef WLAN_SUPPORT_RX_PROTOCOL_TYPE_TAG
	.txrx_update_pdev_rx_protocol_tag = dp_update_pdev_rx_protocol_tag,
#ifdef WLAN_SUPPORT_RX_TAG_STATISTICS
	.txrx_dump_pdev_rx_protocol_tag_stats =
				dp_dump_pdev_rx_protocol_tag_stats,
#endif /* WLAN_SUPPORT_RX_TAG_STATISTICS */
#endif /* WLAN_SUPPORT_RX_PROTOCOL_TYPE_TAG */
#ifdef WLAN_SUPPORT_RX_FLOW_TAG
	.txrx_set_rx_flow_tag = dp_set_rx_flow_tag,
	.txrx_dump_rx_flow_tag_stats = dp_dump_rx_flow_tag_stats,
#endif /* WLAN_SUPPORT_RX_FLOW_TAG */
#ifdef QCA_MULTIPASS_SUPPORT
	.txrx_peer_set_vlan_id = dp_peer_set_vlan_id,
#endif /*QCA_MULTIPASS_SUPPORT*/
#if defined(WLAN_TX_PKT_CAPTURE_ENH) || defined(WLAN_RX_PKT_CAPTURE_ENH)
	.txrx_update_peer_pkt_capture_params =
		 dp_peer_update_pkt_capture_params,
#endif /* WLAN_TX_PKT_CAPTURE_ENH || WLAN_RX_PKT_CAPTURE_ENH */
};

static struct cdp_me_ops dp_ops_me = {
#ifdef ATH_SUPPORT_IQUE
	.tx_me_alloc_descriptor = dp_tx_me_alloc_descriptor,
	.tx_me_free_descriptor = dp_tx_me_free_descriptor,
	.tx_me_convert_ucast = dp_tx_me_send_convert_ucast,
#endif
};

static struct cdp_mon_ops dp_ops_mon = {
	.txrx_reset_monitor_mode = dp_reset_monitor_mode,
	/* Added support for HK advance filter */
	.txrx_set_advance_monitor_filter = dp_pdev_set_advance_monitor_filter,
	.txrx_deliver_tx_mgmt = dp_deliver_tx_mgmt,
};

static struct cdp_host_stats_ops dp_ops_host_stats = {
	.txrx_per_peer_stats = dp_get_host_peer_stats,
	.get_fw_peer_stats = dp_get_fw_peer_stats,
	.get_htt_stats = dp_get_htt_stats,
#ifdef FEATURE_PERPKT_INFO
	.txrx_enable_enhanced_stats = dp_enable_enhanced_stats,
	.txrx_disable_enhanced_stats = dp_disable_enhanced_stats,
#endif /* FEATURE_PERPKT_INFO */
	.txrx_stats_publish = dp_txrx_stats_publish,
	.txrx_get_vdev_stats  = dp_txrx_get_vdev_stats,
	.txrx_get_peer_stats = dp_txrx_get_peer_stats,
	.txrx_get_peer_stats_param = dp_txrx_get_peer_stats_param,
	.txrx_reset_peer_stats = dp_txrx_reset_peer_stats,
	.txrx_get_pdev_stats = dp_txrx_get_pdev_stats,
	.txrx_get_ratekbps = dp_txrx_get_ratekbps,
	.txrx_update_vdev_stats = dp_txrx_update_vdev_host_stats,
	/* TODO */
};

static struct cdp_raw_ops dp_ops_raw = {
	/* TODO */
};

#ifdef PEER_FLOW_CONTROL
static struct cdp_pflow_ops dp_ops_pflow = {
	dp_tx_flow_ctrl_configure_pdev,
};
#endif /* CONFIG_WIN */

#if defined(WLAN_CFR_ENABLE) && defined(WLAN_ENH_CFR_ENABLE)
static struct cdp_cfr_ops dp_ops_cfr = {
	.txrx_cfr_filter = dp_cfr_filter,
	.txrx_get_cfr_rcc = dp_get_cfr_rcc,
	.txrx_set_cfr_rcc = dp_set_cfr_rcc,
	.txrx_get_cfr_dbg_stats = dp_get_cfr_dbg_stats,
	.txrx_clear_cfr_dbg_stats = dp_clear_cfr_dbg_stats,
	.txrx_enable_mon_reap_timer = dp_enable_mon_reap_timer,
};
#endif

#ifdef FEATURE_RUNTIME_PM
/**
 * dp_flush_ring_hptp() - Update ring shadow
 *			  register HP/TP address when runtime
 *                        resume
 * @opaque_soc: DP soc context
 *
 * Return: None
 */
static
void dp_flush_ring_hptp(struct dp_soc *soc, hal_ring_handle_t hal_srng)
{
	if (hal_srng && hal_srng_get_clear_event(hal_srng,
						 HAL_SRNG_FLUSH_EVENT)) {
		/* Acquire the lock */
		hal_srng_access_start(soc->hal_soc, hal_srng);

		hal_srng_access_end(soc->hal_soc, hal_srng);

		hal_srng_set_flush_last_ts(hal_srng);
		dp_debug("flushed");
	}
}

/**
 * dp_runtime_suspend() - ensure DP is ready to runtime suspend
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 *
 * DP is ready to runtime suspend if there are no pending TX packets.
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_runtime_suspend(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev;
	uint8_t i;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev is NULL");
		return QDF_STATUS_E_INVAL;
	}

	/* Abort if there are any pending TX packets */
	if (dp_get_tx_pending(dp_pdev_to_cdp_pdev(pdev)) > 0) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			  FL("Abort suspend due to pending TX packets"));

		/* perform a force flush if tx is pending */
		for (i = 0; i < soc->num_tcl_data_rings; i++) {
#ifdef IPA_OFFLOAD
			if (i == IPA_TCL_DATA_RING_IDX)
				continue;
#endif
			hal_srng_set_event(soc->tcl_data_ring[i].hal_srng,
					   HAL_SRNG_FLUSH_EVENT);
			dp_flush_ring_hptp(soc, soc->tcl_data_ring[i].hal_srng);
		}

		return QDF_STATUS_E_AGAIN;
	}

	if (dp_runtime_get_refcount(soc)) {
		dp_info("refcount: %d", dp_runtime_get_refcount(soc));

		return QDF_STATUS_E_AGAIN;
	}

	if (soc->intr_mode == DP_INTR_POLL)
		qdf_timer_stop(&soc->int_timer);

	return QDF_STATUS_SUCCESS;
}

#define DP_FLUSH_WAIT_CNT 10
#define DP_RUNTIME_SUSPEND_WAIT_MS 10
/**
 * dp_runtime_resume() - ensure DP is ready to runtime resume
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 *
 * Resume DP for runtime PM.
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS dp_runtime_resume(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	int i, suspend_wait = 0;

	if (soc->intr_mode == DP_INTR_POLL)
		qdf_timer_mod(&soc->int_timer, DP_INTR_POLL_TIMER_MS);

	/*
	 * Wait until dp runtime refcount becomes zero or time out, then flush
	 * pending tx for runtime suspend.
	 */
	while (dp_runtime_get_refcount(soc) &&
	       suspend_wait < DP_FLUSH_WAIT_CNT) {
		qdf_sleep(DP_RUNTIME_SUSPEND_WAIT_MS);
		suspend_wait++;
	}

	for (i = 0; i < MAX_TCL_DATA_RINGS; i++) {
		dp_flush_ring_hptp(soc, soc->tcl_data_ring[i].hal_srng);
	}

	dp_flush_ring_hptp(soc, soc->reo_cmd_ring.hal_srng);

	return QDF_STATUS_SUCCESS;
}
#endif /* FEATURE_RUNTIME_PM */

/**
 * dp_tx_get_success_ack_stats() - get tx success completion count
 * @soc_hdl: Datapath soc handle
 * @vdevid: vdev identifier
 *
 * Return: tx success ack count
 */
static uint32_t dp_tx_get_success_ack_stats(struct cdp_soc_t *soc_hdl,
					    uint8_t vdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct cdp_vdev_stats *vdev_stats = NULL;
	uint32_t tx_success;
	struct dp_vdev *vdev =
		(struct dp_vdev *)dp_get_vdev_from_soc_vdev_id_wifi3(soc,
								     vdev_id);

	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  FL("Invalid vdev id %d"), vdev_id);
		return 0;
	}

	vdev_stats = qdf_mem_malloc_atomic(sizeof(struct cdp_vdev_stats));
	if (!vdev_stats) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "DP alloc failure - unable to get alloc vdev stats");
		return 0;
	}

	qdf_spin_lock_bh(&soc->peer_ref_mutex);
	dp_aggregate_vdev_stats(vdev, vdev_stats);
	qdf_spin_unlock_bh(&soc->peer_ref_mutex);

	tx_success = vdev_stats->tx.tx_success.num;
	qdf_mem_free(vdev_stats);

	return tx_success;
}

#ifdef WLAN_SUPPORT_DATA_STALL
/**
 * dp_register_data_stall_detect_cb() - register data stall callback
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @data_stall_detect_callback: data stall callback function
 *
 * Return: QDF_STATUS Enumeration
 */
static
QDF_STATUS dp_register_data_stall_detect_cb(
			struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			data_stall_detect_cb data_stall_detect_callback)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev NULL!");
		return QDF_STATUS_E_INVAL;
	}

	pdev->data_stall_detect_callback = data_stall_detect_callback;
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_deregister_data_stall_detect_cb() - de-register data stall callback
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @data_stall_detect_callback: data stall callback function
 *
 * Return: QDF_STATUS Enumeration
 */
static
QDF_STATUS dp_deregister_data_stall_detect_cb(
			struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			data_stall_detect_cb data_stall_detect_callback)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev NULL!");
		return QDF_STATUS_E_INVAL;
	}

	pdev->data_stall_detect_callback = NULL;
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_txrx_post_data_stall_event() - post data stall event
 * @soc_hdl: Datapath soc handle
 * @indicator: Module triggering data stall
 * @data_stall_type: data stall event type
 * @pdev_id: pdev id
 * @vdev_id_bitmap: vdev id bitmap
 * @recovery_type: data stall recovery type
 *
 * Return: None
 */
static void
dp_txrx_post_data_stall_event(struct cdp_soc_t *soc_hdl,
			      enum data_stall_log_event_indicator indicator,
			      enum data_stall_log_event_type data_stall_type,
			      uint32_t pdev_id, uint32_t vdev_id_bitmap,
			      enum data_stall_log_recovery_type recovery_type)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct data_stall_event_info data_stall_info;
	struct dp_pdev *pdev;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev NULL!");
		return;
	}

	if (!pdev->data_stall_detect_callback) {
		dp_err("data stall cb not registered!");
		return;
	}

	dp_info("data_stall_type: %x pdev_id: %d",
		data_stall_type, pdev_id);

	data_stall_info.indicator = indicator;
	data_stall_info.data_stall_type = data_stall_type;
	data_stall_info.vdev_id_bitmap = vdev_id_bitmap;
	data_stall_info.pdev_id = pdev_id;
	data_stall_info.recovery_type = recovery_type;

	pdev->data_stall_detect_callback(&data_stall_info);
}
#endif /* WLAN_SUPPORT_DATA_STALL */

#ifdef DP_PEER_EXTENDED_API
/**
 * dp_peer_get_ref_find_by_addr - get peer with addr by ref count inc
 * @dev: physical device instance
 * @peer_mac_addr: peer mac address
 * @debug_id: to track enum peer access
 *
 * Return: peer instance pointer
 */
static void *
dp_peer_get_ref_find_by_addr(struct cdp_pdev *dev, uint8_t *peer_mac_addr,
			     enum peer_debug_id_type debug_id)
{
	struct dp_pdev *pdev = (struct dp_pdev *)dev;
	struct dp_peer *peer;

	peer = dp_peer_find_hash_find(pdev->soc, peer_mac_addr, 0, DP_VDEV_ALL);

	if (!peer)
		return NULL;

	if (peer->delete_in_progress) {
		dp_err("Peer deletion in progress");
		dp_peer_unref_delete(peer);
		return NULL;
	}

	dp_info_rl("peer %pK mac: "QDF_MAC_ADDR_FMT, peer,
		   QDF_MAC_ADDR_REF(peer->mac_addr.raw));

	return peer;
}
#endif /* DP_PEER_EXTENDED_API */

#ifdef WLAN_FEATURE_STATS_EXT
/* rx hw stats event wait timeout in ms */
#define DP_REO_STATUS_STATS_TIMEOUT 1500
/**
 * dp_txrx_ext_stats_request - request dp txrx extended stats request
 * @soc_hdl: soc handle
 * @pdev_id: pdev id
 * @req: stats request
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
dp_txrx_ext_stats_request(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			  struct cdp_txrx_ext_stats *req)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (!pdev) {
		dp_err("pdev is null");
		return QDF_STATUS_E_INVAL;
	}

	dp_aggregate_pdev_stats(pdev);

	req->tx_msdu_enqueue = pdev->stats.tx_i.processed.num;
	req->tx_msdu_overflow = pdev->stats.tx_i.dropped.ring_full;
	req->rx_mpdu_received = soc->ext_stats.rx_mpdu_received;
	req->rx_mpdu_delivered = soc->ext_stats.rx_mpdu_received;
	req->rx_mpdu_missed = soc->ext_stats.rx_mpdu_missed;
	/* only count error source from RXDMA */
	req->rx_mpdu_error = pdev->stats.err.rxdma_error;

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_rx_hw_stats_cb - request rx hw stats response callback
 * @soc: soc handle
 * @cb_ctxt: callback context
 * @reo_status: reo command response status
 *
 * Return: None
 */
static void dp_rx_hw_stats_cb(struct dp_soc *soc, void *cb_ctxt,
			      union hal_reo_status *reo_status)
{
	struct dp_req_rx_hw_stats_t *rx_hw_stats = cb_ctxt;
	struct hal_reo_queue_status *queue_status = &reo_status->queue_status;
	bool is_query_timeout;

	qdf_spin_lock_bh(&soc->rx_hw_stats_lock);
	is_query_timeout = rx_hw_stats->is_query_timeout;
	/* free the cb_ctxt if all pending tid stats query is received */
	if (qdf_atomic_dec_and_test(&rx_hw_stats->pending_tid_stats_cnt)) {
		if (!is_query_timeout) {
			qdf_event_set(&soc->rx_hw_stats_event);
			soc->is_last_stats_ctx_init = false;
		}

		qdf_mem_free(rx_hw_stats);
	}

	if (queue_status->header.status != HAL_REO_CMD_SUCCESS) {
		dp_info("REO stats failure %d",
			queue_status->header.status);
		qdf_spin_unlock_bh(&soc->rx_hw_stats_lock);
		return;
	}

	if (!is_query_timeout) {
		soc->ext_stats.rx_mpdu_received +=
					queue_status->mpdu_frms_cnt;
		soc->ext_stats.rx_mpdu_missed +=
					queue_status->hole_cnt;
	}
	qdf_spin_unlock_bh(&soc->rx_hw_stats_lock);
}

/**
 * dp_request_rx_hw_stats - request rx hardware stats
 * @soc_hdl: soc handle
 * @vdev_id: vdev id
 *
 * Return: None
 */
static QDF_STATUS
dp_request_rx_hw_stats(struct cdp_soc_t *soc_hdl, uint8_t vdev_id)
{
	struct dp_soc *soc = (struct dp_soc *)soc_hdl;
	struct dp_vdev *vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc, vdev_id);
	struct dp_peer *peer;
	QDF_STATUS status;
	struct dp_req_rx_hw_stats_t *rx_hw_stats;
	int rx_stats_sent_cnt = 0;
	uint32_t last_rx_mpdu_received;
	uint32_t last_rx_mpdu_missed;

	if (!vdev) {
		dp_err("vdev is null for vdev_id: %u", vdev_id);
		return QDF_STATUS_E_INVAL;
	}

	peer = dp_peer_get_ref_find_by_addr((struct cdp_pdev *)vdev->pdev,
					    vdev->vap_bss_peer_mac_addr, 0);

	if (!peer) {
		dp_err("Peer is NULL");
		return QDF_STATUS_E_INVAL;
	}

	rx_hw_stats = qdf_mem_malloc(sizeof(*rx_hw_stats));

	if (!rx_hw_stats) {
		dp_err("malloc failed for hw stats structure");
		dp_peer_unref_delete(peer);
		return QDF_STATUS_E_NOMEM;
	}

	qdf_event_reset(&soc->rx_hw_stats_event);
	qdf_spin_lock_bh(&soc->rx_hw_stats_lock);
	/* save the last soc cumulative stats and reset it to 0 */
	last_rx_mpdu_received = soc->ext_stats.rx_mpdu_received;
	last_rx_mpdu_missed = soc->ext_stats.rx_mpdu_missed;
	soc->ext_stats.rx_mpdu_received = 0;
	soc->ext_stats.rx_mpdu_missed = 0;

	rx_stats_sent_cnt =
		dp_peer_rxtid_stats(peer, dp_rx_hw_stats_cb, rx_hw_stats);
	if (!rx_stats_sent_cnt) {
		dp_err("no tid stats sent successfully");
		qdf_mem_free(rx_hw_stats);
		qdf_spin_unlock_bh(&soc->rx_hw_stats_lock);
		dp_peer_unref_delete(peer);
		return QDF_STATUS_E_INVAL;
	}
	qdf_atomic_set(&rx_hw_stats->pending_tid_stats_cnt,
		       rx_stats_sent_cnt);
	rx_hw_stats->is_query_timeout = false;
	soc->is_last_stats_ctx_init = true;
	qdf_spin_unlock_bh(&soc->rx_hw_stats_lock);

	status = qdf_wait_single_event(&soc->rx_hw_stats_event,
				       DP_REO_STATUS_STATS_TIMEOUT);

	qdf_spin_lock_bh(&soc->rx_hw_stats_lock);
	if (status != QDF_STATUS_SUCCESS) {
		dp_info("rx hw stats event timeout");
		if (soc->is_last_stats_ctx_init)
			rx_hw_stats->is_query_timeout = true;
		/**
		 * If query timeout happened, use the last saved stats
		 * for this time query.
		 */
		soc->ext_stats.rx_mpdu_received = last_rx_mpdu_received;
		soc->ext_stats.rx_mpdu_missed = last_rx_mpdu_missed;
	}
	qdf_spin_unlock_bh(&soc->rx_hw_stats_lock);
	dp_peer_unref_delete(peer);

	return status;
}
#endif /* WLAN_FEATURE_STATS_EXT */

#ifdef DP_PEER_EXTENDED_API
static struct cdp_misc_ops dp_ops_misc = {
#ifdef FEATURE_WLAN_TDLS
	.tx_non_std = dp_tx_non_std,
#endif /* FEATURE_WLAN_TDLS */
	.get_opmode = dp_get_opmode,
#ifdef FEATURE_RUNTIME_PM
	.runtime_suspend = dp_runtime_suspend,
	.runtime_resume = dp_runtime_resume,
#endif /* FEATURE_RUNTIME_PM */
	.pkt_log_init = dp_pkt_log_init,
	.pkt_log_con_service = dp_pkt_log_con_service,
	.get_num_rx_contexts = dp_get_num_rx_contexts,
	.get_tx_ack_stats = dp_tx_get_success_ack_stats,
#ifdef WLAN_SUPPORT_DATA_STALL
	.txrx_data_stall_cb_register = dp_register_data_stall_detect_cb,
	.txrx_data_stall_cb_deregister = dp_deregister_data_stall_detect_cb,
	.txrx_post_data_stall_event = dp_txrx_post_data_stall_event,
#endif

#ifdef WLAN_FEATURE_STATS_EXT
	.txrx_ext_stats_request = dp_txrx_ext_stats_request,
	.request_rx_hw_stats = dp_request_rx_hw_stats,
#endif /* WLAN_FEATURE_STATS_EXT */
};
#endif

#ifdef DP_FLOW_CTL
static struct cdp_flowctl_ops dp_ops_flowctl = {
	/* WIFI 3.0 DP implement as required. */
#ifdef QCA_LL_TX_FLOW_CONTROL_V2
	.flow_pool_map_handler = dp_tx_flow_pool_map,
	.flow_pool_unmap_handler = dp_tx_flow_pool_unmap,
	.register_pause_cb = dp_txrx_register_pause_cb,
	.dump_flow_pool_info = dp_tx_dump_flow_pool_info,
	.tx_desc_thresh_reached = dp_tx_desc_thresh_reached,
#endif /* QCA_LL_TX_FLOW_CONTROL_V2 */
};

static struct cdp_lflowctl_ops dp_ops_l_flowctl = {
	/* WIFI 3.0 DP NOT IMPLEMENTED YET */
};
#endif

#ifdef IPA_OFFLOAD
static struct cdp_ipa_ops dp_ops_ipa = {
	.ipa_get_resource = dp_ipa_get_resource,
	.ipa_set_doorbell_paddr = dp_ipa_set_doorbell_paddr,
	.ipa_op_response = dp_ipa_op_response,
	.ipa_register_op_cb = dp_ipa_register_op_cb,
	.ipa_get_stat = dp_ipa_get_stat,
	.ipa_tx_data_frame = dp_tx_send_ipa_data_frame,
	.ipa_enable_autonomy = dp_ipa_enable_autonomy,
	.ipa_disable_autonomy = dp_ipa_disable_autonomy,
	.ipa_setup = dp_ipa_setup,
	.ipa_cleanup = dp_ipa_cleanup,
	.ipa_setup_iface = dp_ipa_setup_iface,
	.ipa_cleanup_iface = dp_ipa_cleanup_iface,
	.ipa_enable_pipes = dp_ipa_enable_pipes,
	.ipa_disable_pipes = dp_ipa_disable_pipes,
	.ipa_set_perf_level = dp_ipa_set_perf_level,
	.ipa_rx_intrabss_fwd = dp_ipa_rx_intrabss_fwd
};
#endif

#ifdef DP_POWER_SAVE
static QDF_STATUS dp_bus_suspend(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	int timeout = SUSPEND_DRAIN_WAIT;
	int drain_wait_delay = 50; /* 50 ms */

	if (qdf_unlikely(!pdev)) {
		dp_err("pdev is NULL");
		return QDF_STATUS_E_INVAL;
	}

	/* Abort if there are any pending TX packets */
	while (dp_get_tx_pending((struct cdp_pdev *)pdev) > 0) {
		qdf_sleep(drain_wait_delay);
		if (timeout <= 0) {
			dp_err("TX frames are pending, abort suspend");
			return QDF_STATUS_E_TIMEOUT;
		}
		timeout = timeout - drain_wait_delay;
	}

	if (soc->intr_mode == DP_INTR_POLL)
		qdf_timer_stop(&soc->int_timer);

	/* Stop monitor reap timer and reap any pending frames in ring */
	if (((pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) ||
	     dp_is_enable_reap_timer_non_pkt(pdev)) &&
	    soc->reap_timer_init) {
		qdf_timer_sync_cancel(&soc->mon_reap_timer);
		dp_service_mon_rings(soc, DP_MON_REAP_BUDGET);
	}

	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS dp_bus_resume(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (qdf_unlikely(!pdev)) {
		dp_err("pdev is NULL");
		return QDF_STATUS_E_INVAL;
	}

	if (soc->intr_mode == DP_INTR_POLL)
		qdf_timer_mod(&soc->int_timer, DP_INTR_POLL_TIMER_MS);

	/* Start monitor reap timer */
	if (((pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) ||
	     dp_is_enable_reap_timer_non_pkt(pdev)) &&
	    soc->reap_timer_init)
		qdf_timer_mod(&soc->mon_reap_timer,
			      DP_INTR_POLL_TIMER_MS);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_process_wow_ack_rsp() - process wow ack response
 * @soc_hdl: datapath soc handle
 * @pdev_id: data path pdev handle id
 *
 * Return: none
 */
static void dp_process_wow_ack_rsp(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (qdf_unlikely(!pdev)) {
		dp_err("pdev is NULL");
		return;
	}

	/*
	 * As part of wow enable FW disables the mon status ring and in wow ack
	 * response from FW reap mon status ring to make sure no packets pending
	 * in the ring.
	 */
	if (((pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) ||
	     dp_is_enable_reap_timer_non_pkt(pdev)) &&
	    soc->reap_timer_init) {
		dp_service_mon_rings(soc, DP_MON_REAP_BUDGET);
	}
}

/**
 * dp_process_target_suspend_req() - process target suspend request
 * @soc_hdl: datapath soc handle
 * @pdev_id: data path pdev handle id
 *
 * Return: none
 */
static void dp_process_target_suspend_req(struct cdp_soc_t *soc_hdl,
					  uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (qdf_unlikely(!pdev)) {
		dp_err("pdev is NULL");
		return;
	}

	/* Stop monitor reap timer and reap any pending frames in ring */
	if (((pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) ||
	     dp_is_enable_reap_timer_non_pkt(pdev)) &&
	    soc->reap_timer_init) {
		qdf_timer_sync_cancel(&soc->mon_reap_timer);
		dp_service_mon_rings(soc, DP_MON_REAP_BUDGET);
	}
}

static struct cdp_bus_ops dp_ops_bus = {
	.bus_suspend = dp_bus_suspend,
	.bus_resume = dp_bus_resume,
	.process_wow_ack_rsp = dp_process_wow_ack_rsp,
	.process_target_suspend_req = dp_process_target_suspend_req
};
#endif

#ifdef DP_FLOW_CTL
static struct cdp_throttle_ops dp_ops_throttle = {
	/* WIFI 3.0 DP NOT IMPLEMENTED YET */
};

static struct cdp_cfg_ops dp_ops_cfg = {
	/* WIFI 3.0 DP NOT IMPLEMENTED YET */
};
#endif

#ifdef DP_PEER_EXTENDED_API
static struct cdp_ocb_ops dp_ops_ocb = {
	/* WIFI 3.0 DP NOT IMPLEMENTED YET */
};

static struct cdp_mob_stats_ops dp_ops_mob_stats = {
	.clear_stats = dp_txrx_clear_dump_stats,
};

static struct cdp_peer_ops dp_ops_peer = {
	.register_peer = dp_register_peer,
	.clear_peer = dp_clear_peer,
	.find_peer_exist = dp_find_peer_exist,
	.find_peer_exist_on_vdev = dp_find_peer_exist_on_vdev,
	.find_peer_exist_on_other_vdev = dp_find_peer_exist_on_other_vdev,
	.peer_state_update = dp_peer_state_update,
	.get_vdevid = dp_get_vdevid,
	.get_vdev_by_peer_addr = dp_get_vdev_by_peer_addr,
	.peer_get_peer_mac_addr = dp_peer_get_peer_mac_addr,
	.get_peer_state = dp_get_peer_state,
	.peer_flush_frags = dp_peer_flush_frags,
};
#endif

static struct cdp_ops dp_txrx_ops = {
	.cmn_drv_ops = &dp_ops_cmn,
	.ctrl_ops = &dp_ops_ctrl,
	.me_ops = &dp_ops_me,
	.mon_ops = &dp_ops_mon,
	.host_stats_ops = &dp_ops_host_stats,
	.wds_ops = &dp_ops_wds,
	.raw_ops = &dp_ops_raw,
#ifdef PEER_FLOW_CONTROL
	.pflow_ops = &dp_ops_pflow,
#endif /* PEER_FLOW_CONTROL */
#ifdef DP_PEER_EXTENDED_API
	.misc_ops = &dp_ops_misc,
	.ocb_ops = &dp_ops_ocb,
	.peer_ops = &dp_ops_peer,
	.mob_stats_ops = &dp_ops_mob_stats,
#endif
#ifdef DP_FLOW_CTL
	.cfg_ops = &dp_ops_cfg,
	.flowctl_ops = &dp_ops_flowctl,
	.l_flowctl_ops = &dp_ops_l_flowctl,
	.throttle_ops = &dp_ops_throttle,
#endif
#ifdef IPA_OFFLOAD
	.ipa_ops = &dp_ops_ipa,
#endif
#ifdef DP_POWER_SAVE
	.bus_ops = &dp_ops_bus,
#endif
#if defined(WLAN_CFR_ENABLE) && defined(WLAN_ENH_CFR_ENABLE)
	.cfr_ops = &dp_ops_cfr,
#endif
};

/*
 * dp_soc_set_txrx_ring_map()
 * @dp_soc: DP handler for soc
 *
 * Return: Void
 */
void dp_soc_set_txrx_ring_map(struct dp_soc *soc)
{
	uint32_t i;
	for (i = 0; i < WLAN_CFG_INT_NUM_CONTEXTS; i++) {
		soc->tx_ring_map[i] = dp_cpu_ring_map[DP_NSS_DEFAULT_MAP][i];
	}
}

#if defined(QCA_WIFI_QCA8074) || defined(QCA_WIFI_QCA6018)

#ifndef QCA_MEM_ATTACH_ON_WIFI3

/**
 * dp_soc_attach_wifi3() - Attach txrx SOC
 * @ctrl_psoc: Opaque SOC handle from control plane
 * @htc_handle: Opaque HTC handle
 * @hif_handle: Opaque HIF handle
 * @qdf_osdev: QDF device
 * @ol_ops: Offload Operations
 * @device_id: Device ID
 *
 * Return: DP SOC handle on success, NULL on failure
 */
struct cdp_soc_t *
dp_soc_attach_wifi3(struct cdp_ctrl_objmgr_psoc *ctrl_psoc,
		    struct hif_opaque_softc *hif_handle,
		    HTC_HANDLE htc_handle, qdf_device_t qdf_osdev,
		    struct ol_if_ops *ol_ops, uint16_t device_id)
{
	struct dp_soc *dp_soc =  NULL;

	dp_soc = dp_soc_attach(ctrl_psoc, htc_handle, qdf_osdev,
			       ol_ops, device_id);
	if (!dp_soc)
		return NULL;

	if (!dp_soc_init(dp_soc, htc_handle, hif_handle))
		return NULL;

	return dp_soc_to_cdp_soc_t(dp_soc);
}
#else

/**
 * dp_soc_attach_wifi3() - Attach txrx SOC
 * @ctrl_psoc: Opaque SOC handle from control plane
 * @htc_handle: Opaque HTC handle
 * @hif_handle: Opaque HIF handle
 * @qdf_osdev: QDF device
 * @ol_ops: Offload Operations
 * @device_id: Device ID
 *
 * Return: DP SOC handle on success, NULL on failure
 */
struct cdp_soc_t *
dp_soc_attach_wifi3(struct cdp_ctrl_objmgr_psoc *ctrl_psoc,
		    struct hif_opaque_softc *hif_handle,
		    HTC_HANDLE htc_handle, qdf_device_t qdf_osdev,
		    struct ol_if_ops *ol_ops, uint16_t device_id)
{
	struct dp_soc *dp_soc = NULL;

	dp_soc = dp_soc_attach(ctrl_psoc, htc_handle, qdf_osdev,
			       ol_ops, device_id);
	return dp_soc_to_cdp_soc_t(dp_soc);
}

#endif

/**
 * dp_soc_attach() - Attach txrx SOC
 * @ctrl_psoc: Opaque SOC handle from control plane
 * @htc_handle: Opaque HTC handle
 * @qdf_osdev: QDF device
 * @ol_ops: Offload Operations
 * @device_id: Device ID
 *
 * Return: DP SOC handle on success, NULL on failure
 */
static struct dp_soc *
dp_soc_attach(struct cdp_ctrl_objmgr_psoc *ctrl_psoc,
	      HTC_HANDLE htc_handle, qdf_device_t qdf_osdev,
	      struct ol_if_ops *ol_ops, uint16_t device_id)
{
	int int_ctx;
	struct dp_soc *soc =  NULL;
	struct htt_soc *htt_soc;

	soc = qdf_mem_malloc(sizeof(*soc));

	if (!soc) {
		dp_err("DP SOC memory allocation failed");
		goto fail0;
	}

	int_ctx = 0;
	soc->device_id = device_id;
	soc->cdp_soc.ops = &dp_txrx_ops;
	soc->cdp_soc.ol_ops = ol_ops;
	soc->ctrl_psoc = ctrl_psoc;
	soc->osdev = qdf_osdev;
	soc->num_hw_dscp_tid_map = HAL_MAX_HW_DSCP_TID_MAPS;

	dp_soc_rx_history_attach(soc);
	wlan_set_srng_cfg(&soc->wlan_srng_cfg);
	qdf_mem_zero(&soc->vdev_id_map, sizeof(soc->vdev_id_map));

	soc->wlan_cfg_ctx = wlan_cfg_soc_attach(soc->ctrl_psoc);
	if (!soc->wlan_cfg_ctx) {
		dp_err("wlan_cfg_ctx failed\n");
		goto fail1;
	}

	dp_soc_set_interrupt_mode(soc);

	htt_soc = htt_soc_attach(soc, htc_handle);

	if (!htt_soc)
		goto fail1;

	soc->htt_handle = htt_soc;

	if (htt_soc_htc_prealloc(htt_soc) != QDF_STATUS_SUCCESS)
		goto fail2;

	return soc;
fail2:
	htt_soc_detach(htt_soc);
fail1:
	dp_soc_rx_history_detach(soc);
	qdf_mem_free(soc);
fail0:
	return NULL;
}

/**
 * dp_soc_init() - Initialize txrx SOC
 * @dp_soc: Opaque DP SOC handle
 * @htc_handle: Opaque HTC handle
 * @hif_handle: Opaque HIF handle
 *
 * Return: DP SOC handle on success, NULL on failure
 */
void *dp_soc_init(struct dp_soc *soc, HTC_HANDLE htc_handle,
		  struct hif_opaque_softc *hif_handle)
{
	int target_type;
	struct htt_soc *htt_soc = (struct htt_soc *)soc->htt_handle;
	bool is_monitor_mode = false;

	htt_set_htc_handle(htt_soc, htc_handle);
	soc->hif_handle = hif_handle;

	soc->hal_soc = hif_get_hal_handle(soc->hif_handle);
	if (!soc->hal_soc)
		return NULL;

	htt_soc_initialize(soc->htt_handle, soc->ctrl_psoc,
			   htt_get_htc_handle(htt_soc),
			   soc->hal_soc, soc->osdev);
	target_type = hal_get_target_type(soc->hal_soc);
	switch (target_type) {
	case TARGET_TYPE_QCA6290:
		wlan_cfg_set_reo_dst_ring_size(soc->wlan_cfg_ctx,
					       REO_DST_RING_SIZE_QCA6290);
		soc->ast_override_support = 1;
		soc->da_war_enabled = false;
		break;
#if defined(QCA_WIFI_QCA6390) || defined(QCA_WIFI_QCA6490) || \
    defined(QCA_WIFI_QCA6750)
	case TARGET_TYPE_QCA6390:
	case TARGET_TYPE_QCA6490:
	case TARGET_TYPE_QCA6750:
		wlan_cfg_set_reo_dst_ring_size(soc->wlan_cfg_ctx,
					       REO_DST_RING_SIZE_QCA6290);
		wlan_cfg_set_raw_mode_war(soc->wlan_cfg_ctx, true);
		soc->ast_override_support = 1;
		if (soc->cdp_soc.ol_ops->get_con_mode &&
		    soc->cdp_soc.ol_ops->get_con_mode() ==
		    QDF_GLOBAL_MONITOR_MODE) {
			int int_ctx;

			for (int_ctx = 0; int_ctx < WLAN_CFG_INT_NUM_CONTEXTS; int_ctx++) {
				soc->wlan_cfg_ctx->int_rx_ring_mask[int_ctx] = 0;
				soc->wlan_cfg_ctx->int_rxdma2host_ring_mask[int_ctx] = 0;
			}
		}
		soc->wlan_cfg_ctx->rxdma1_enable = 0;
		break;
#endif /* QCA_WIFI_QCA6390 || QCA_WIFI_QCA6490 || QCA_WIFI_QCA6750 */

	case TARGET_TYPE_QCA8074:
		wlan_cfg_set_reo_dst_ring_size(soc->wlan_cfg_ctx,
					       REO_DST_RING_SIZE_QCA8074);
		wlan_cfg_set_raw_mode_war(soc->wlan_cfg_ctx, true);
		soc->da_war_enabled = true;
		soc->is_rx_fse_full_cache_invalidate_war_enabled = true;
		break;
	case TARGET_TYPE_QCA8074V2:
	case TARGET_TYPE_QCA6018:
		wlan_cfg_set_reo_dst_ring_size(soc->wlan_cfg_ctx,
					       REO_DST_RING_SIZE_QCA8074);
		wlan_cfg_set_raw_mode_war(soc->wlan_cfg_ctx, false);
		soc->hw_nac_monitor_support = 1;
		soc->ast_override_support = 1;
		soc->per_tid_basize_max_tid = 8;
		soc->num_hw_dscp_tid_map = HAL_MAX_HW_DSCP_TID_V2_MAPS;
		soc->da_war_enabled = false;
		soc->is_rx_fse_full_cache_invalidate_war_enabled = true;
		break;
	case TARGET_TYPE_QCN9000:
		wlan_cfg_set_reo_dst_ring_size(soc->wlan_cfg_ctx,
					       REO_DST_RING_SIZE_QCN9000);
		soc->ast_override_support = 1;
		soc->da_war_enabled = false;
		wlan_cfg_set_raw_mode_war(soc->wlan_cfg_ctx, false);
		soc->hw_nac_monitor_support = 1;
		soc->per_tid_basize_max_tid = 8;
		soc->num_hw_dscp_tid_map = HAL_MAX_HW_DSCP_TID_V2_MAPS;
		soc->lmac_polled_mode = 1;
		break;
	default:
		qdf_print("%s: Unknown tgt type %d\n", __func__, target_type);
		qdf_assert_always(0);
		break;
	}

	dp_soc_set_interrupt_mode(soc);
	if (soc->cdp_soc.ol_ops->get_con_mode &&
	    soc->cdp_soc.ol_ops->get_con_mode() ==
	    QDF_GLOBAL_MONITOR_MODE)
		is_monitor_mode = true;

	wlan_cfg_fill_interrupt_mask(soc->wlan_cfg_ctx, soc->intr_mode,
				     is_monitor_mode);
	wlan_cfg_set_rx_hash(soc->wlan_cfg_ctx,
			     cfg_get(soc->ctrl_psoc, CFG_DP_RX_HASH));
	soc->cce_disable = false;

	qdf_atomic_init(&soc->num_tx_outstanding);
	soc->num_tx_allowed =
		wlan_cfg_get_dp_soc_tx_device_limit(soc->wlan_cfg_ctx);

	if (soc->cdp_soc.ol_ops->get_dp_cfg_param) {
		int ret = soc->cdp_soc.ol_ops->get_dp_cfg_param(soc->ctrl_psoc,
				CDP_CFG_MAX_PEER_ID);

		if (ret != -EINVAL) {
			wlan_cfg_set_max_peer_id(soc->wlan_cfg_ctx, ret);
		}

		ret = soc->cdp_soc.ol_ops->get_dp_cfg_param(soc->ctrl_psoc,
				CDP_CFG_CCE_DISABLE);
		if (ret == 1)
			soc->cce_disable = true;
	}

	qdf_spinlock_create(&soc->peer_ref_mutex);
	qdf_spinlock_create(&soc->ast_lock);

	qdf_spinlock_create(&soc->reo_desc_freelist_lock);
	qdf_list_create(&soc->reo_desc_freelist, REO_DESC_FREELIST_SIZE);
	INIT_RX_HW_STATS_LOCK(soc);

	/* fill the tx/rx cpu ring map*/
	dp_soc_set_txrx_ring_map(soc);

	qdf_spinlock_create(&soc->htt_stats.lock);
	/* initialize work queue for stats processing */
	qdf_create_work(0, &soc->htt_stats.work, htt_t2h_stats_handler, soc);

	return soc;

}

/**
 * dp_soc_init_wifi3() - Initialize txrx SOC
 * @soc: Opaque DP SOC handle
 * @ctrl_psoc: Opaque SOC handle from control plane(Unused)
 * @hif_handle: Opaque HIF handle
 * @htc_handle: Opaque HTC handle
 * @qdf_osdev: QDF device (Unused)
 * @ol_ops: Offload Operations (Unused)
 * @device_id: Device ID (Unused)
 *
 * Return: DP SOC handle on success, NULL on failure
 */
void *dp_soc_init_wifi3(struct cdp_soc_t *soc,
			struct cdp_ctrl_objmgr_psoc *ctrl_psoc,
			struct hif_opaque_softc *hif_handle,
			HTC_HANDLE htc_handle, qdf_device_t qdf_osdev,
			struct ol_if_ops *ol_ops, uint16_t device_id)
{
	return dp_soc_init((struct dp_soc *)soc, htc_handle, hif_handle);
}

#endif

/*
 * dp_get_pdev_for_mac_id() -  Return pdev for mac_id
 *
 * @soc: handle to DP soc
 * @mac_id: MAC id
 *
 * Return: Return pdev corresponding to MAC
 */
void *dp_get_pdev_for_mac_id(struct dp_soc *soc, uint32_t mac_id)
{
	if (wlan_cfg_per_pdev_lmac_ring(soc->wlan_cfg_ctx))
		return soc->pdev_list[mac_id];

	/* Typically for MCL as there only 1 PDEV*/
	return soc->pdev_list[0];
}

/*
 * dp_is_hw_dbs_enable() - Procedure to check if DBS is supported
 * @soc:		DP SoC context
 * @max_mac_rings:	No of MAC rings
 *
 * Return: None
 */
void dp_is_hw_dbs_enable(struct dp_soc *soc,
				int *max_mac_rings)
{
	bool dbs_enable = false;
	if (soc->cdp_soc.ol_ops->is_hw_dbs_2x2_capable)
		dbs_enable = soc->cdp_soc.ol_ops->
		is_hw_dbs_2x2_capable((void *)soc->ctrl_psoc);

	*max_mac_rings = (dbs_enable)?(*max_mac_rings):1;
}

#if defined(WLAN_CFR_ENABLE) && defined(WLAN_ENH_CFR_ENABLE)
/*
 * dp_cfr_filter() -  Configure HOST RX monitor status ring for CFR
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @enable: Enable/Disable CFR
 * @filter_val: Flag to select Filter for monitor mode
 */
static void dp_cfr_filter(struct cdp_soc_t *soc_hdl,
			  uint8_t pdev_id,
			  bool enable,
			  struct cdp_monitor_filter *filter_val)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = NULL;
	struct htt_rx_ring_tlv_filter htt_tlv_filter = {0};
	int max_mac_rings;
	uint8_t mac_id = 0;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev is NULL");
		return;
	}

	if (pdev->monitor_vdev) {
		dp_info("No action is needed since monitor mode is enabled\n");
		return;
	}
	soc = pdev->soc;
	pdev->cfr_rcc_mode = false;
	max_mac_rings = wlan_cfg_get_num_mac_rings(pdev->wlan_cfg_ctx);
	dp_is_hw_dbs_enable(soc, &max_mac_rings);

	dp_debug("Max_mac_rings %d", max_mac_rings);
	dp_info("enable : %d, mode: 0x%x", enable, filter_val->mode);

	if (enable) {
		pdev->cfr_rcc_mode = true;

		htt_tlv_filter.ppdu_start = 1;
		htt_tlv_filter.ppdu_end = 1;
		htt_tlv_filter.ppdu_end_user_stats = 1;
		htt_tlv_filter.ppdu_end_user_stats_ext = 1;
		htt_tlv_filter.ppdu_end_status_done = 1;
		htt_tlv_filter.mpdu_start = 1;
		htt_tlv_filter.offset_valid = false;

		htt_tlv_filter.enable_fp =
			(filter_val->mode & MON_FILTER_PASS) ? 1 : 0;
		htt_tlv_filter.enable_md = 0;
		htt_tlv_filter.enable_mo =
			(filter_val->mode & MON_FILTER_OTHER) ? 1 : 0;
		htt_tlv_filter.fp_mgmt_filter = filter_val->fp_mgmt;
		htt_tlv_filter.fp_ctrl_filter = filter_val->fp_ctrl;
		htt_tlv_filter.fp_data_filter = filter_val->fp_data;
		htt_tlv_filter.mo_mgmt_filter = filter_val->mo_mgmt;
		htt_tlv_filter.mo_ctrl_filter = filter_val->mo_ctrl;
		htt_tlv_filter.mo_data_filter = filter_val->mo_data;
	}

	for (mac_id = 0; mac_id < max_mac_rings; mac_id++) {
		int mac_for_pdev =
			dp_get_mac_id_for_pdev(mac_id,
					       pdev->pdev_id);

		htt_h2t_rx_ring_cfg(soc->htt_handle,
				    mac_for_pdev,
				    soc->rxdma_mon_status_ring[mac_id]
				    .hal_srng,
				    RXDMA_MONITOR_STATUS,
				    RX_DATA_BUFFER_SIZE,
				    &htt_tlv_filter);
	}
}

/**
 * dp_get_cfr_rcc() - get cfr rcc config
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of objmgr pdev
 *
 * Return: true/false based on cfr mode setting
 */
static
bool dp_get_cfr_rcc(struct cdp_soc_t *soc_hdl, uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = NULL;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev is NULL");
		return false;
	}

	return pdev->cfr_rcc_mode;
}

/**
 * dp_set_cfr_rcc() - enable/disable cfr rcc config
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of objmgr pdev
 * @enable: Enable/Disable cfr rcc mode
 *
 * Return: none
 */
static
void dp_set_cfr_rcc(struct cdp_soc_t *soc_hdl, uint8_t pdev_id, bool enable)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = NULL;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev is NULL");
		return;
	}

	pdev->cfr_rcc_mode = enable;
}

/*
 * dp_get_cfr_dbg_stats - Get the debug statistics for CFR
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 * @cfr_rcc_stats: CFR RCC debug statistics buffer
 *
 * Return: none
 */
static inline void
dp_get_cfr_dbg_stats(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
		     struct cdp_cfr_rcc_stats *cfr_rcc_stats)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (!pdev) {
		dp_err("Invalid pdev");
		return;
	}

	qdf_mem_copy(cfr_rcc_stats, &pdev->stats.rcc,
		     sizeof(struct cdp_cfr_rcc_stats));
}

/*
 * dp_clear_cfr_dbg_stats - Clear debug statistics for CFR
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of data path pdev handle
 *
 * Return: none
 */
static void dp_clear_cfr_dbg_stats(struct cdp_soc_t *soc_hdl,
				   uint8_t pdev_id)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);

	if (!pdev) {
		dp_err("dp pdev is NULL");
		return;
	}

	qdf_mem_zero(&pdev->stats.rcc, sizeof(pdev->stats.rcc));
}

/*
 * dp_enable_mon_reap_timer() - enable/disable reap timer
 * @soc_hdl: Datapath soc handle
 * @pdev_id: id of objmgr pdev
 * @enable: Enable/Disable reap timer of monitor status ring
 *
 * Return: none
 */
static void
dp_enable_mon_reap_timer(struct cdp_soc_t *soc_hdl, uint8_t pdev_id,
			 bool enable)
{
	struct dp_soc *soc = cdp_soc_t_to_dp_soc(soc_hdl);
	struct dp_pdev *pdev = NULL;

	pdev = dp_get_pdev_from_soc_pdev_id_wifi3(soc, pdev_id);
	if (!pdev) {
		dp_err("pdev is NULL");
		return;
	}

	pdev->enable_reap_timer_non_pkt = enable;
	if (pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) {
		dp_debug("pktlog enabled %d", pdev->rx_pktlog_mode);
		return;
	}

	if (!soc->reap_timer_init) {
		dp_err("reap timer not init");
		return;
	}

	if (enable)
		qdf_timer_mod(&soc->mon_reap_timer,
			      DP_INTR_POLL_TIMER_MS);
	else
		qdf_timer_sync_cancel(&soc->mon_reap_timer);
}
#endif

/*
 * dp_is_enable_reap_timer_non_pkt() - check if mon reap timer is
 * enabled by non-pkt log or not
 * @pdev: point to dp pdev
 *
 * Return: true if mon reap timer is enabled by non-pkt log
 */
static bool dp_is_enable_reap_timer_non_pkt(struct dp_pdev *pdev)
{
	if (!pdev) {
		dp_err("null pdev");
		return false;
	}

	return pdev->enable_reap_timer_non_pkt;
}

/*
* dp_is_soc_reinit() - Check if soc reinit is true
* @soc: DP SoC context
*
* Return: true or false
*/
bool dp_is_soc_reinit(struct dp_soc *soc)
{
	return soc->dp_soc_reinit;
}

/*
* dp_set_pktlog_wifi3() - attach txrx vdev
* @pdev: Datapath PDEV handle
* @event: which event's notifications are being subscribed to
* @enable: WDI event subscribe or not. (True or False)
*
* Return: Success, NULL on failure
*/
#ifdef WDI_EVENT_ENABLE
int dp_set_pktlog_wifi3(struct dp_pdev *pdev, uint32_t event,
		bool enable)
{
	struct dp_soc *soc = NULL;
	int max_mac_rings = wlan_cfg_get_num_mac_rings
					(pdev->wlan_cfg_ctx);
	uint8_t mac_id = 0;

	soc = pdev->soc;
	dp_is_hw_dbs_enable(soc, &max_mac_rings);

	QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_DEBUG,
			FL("Max_mac_rings %d "),
			max_mac_rings);

	if (enable) {
		switch (event) {
		case WDI_EVENT_RX_DESC:
			if (pdev->monitor_vdev) {
				/* Nothing needs to be done if monitor mode is
				 * enabled
				 */
				return 0;
			}

			if (pdev->rx_pktlog_mode != DP_RX_PKTLOG_FULL) {
				pdev->rx_pktlog_mode = DP_RX_PKTLOG_FULL;
				dp_mon_filter_setup_rx_pkt_log_full(pdev);
				if (dp_mon_filter_update(pdev) !=
						QDF_STATUS_SUCCESS) {
					QDF_TRACE(QDF_MODULE_ID_DP,
						  QDF_TRACE_LEVEL_ERROR,
						  FL("Pktlog full filters set failed"));
					dp_mon_filter_reset_rx_pkt_log_full(pdev);
					pdev->rx_pktlog_mode = DP_RX_PKTLOG_DISABLED;
					return 0;
				}

				if (soc->reap_timer_init &&
				    (!dp_is_enable_reap_timer_non_pkt(pdev)))
					qdf_timer_mod(&soc->mon_reap_timer,
					DP_INTR_POLL_TIMER_MS);
			}
			break;

		case WDI_EVENT_LITE_RX:
			if (pdev->monitor_vdev) {
				/* Nothing needs to be done if monitor mode is
				 * enabled
				 */
				return 0;
			}
			if (pdev->rx_pktlog_mode != DP_RX_PKTLOG_LITE) {
				pdev->rx_pktlog_mode = DP_RX_PKTLOG_LITE;

				/*
				 * Set the packet log lite mode filter.
				 */
				dp_mon_filter_setup_rx_pkt_log_lite(pdev);
				if (dp_mon_filter_update(pdev) != QDF_STATUS_SUCCESS) {
					QDF_TRACE(QDF_MODULE_ID_DP,
						  QDF_TRACE_LEVEL_ERROR,
						  FL("Pktlog lite filters set failed"));
					dp_mon_filter_reset_rx_pkt_log_lite(pdev);
					pdev->rx_pktlog_mode =
						DP_RX_PKTLOG_DISABLED;
					return 0;
				}

				if (soc->reap_timer_init &&
				    (!dp_is_enable_reap_timer_non_pkt(pdev)))
					qdf_timer_mod(&soc->mon_reap_timer,
					DP_INTR_POLL_TIMER_MS);
			}
			break;

		case WDI_EVENT_LITE_T2H:
			if (pdev->monitor_vdev) {
				/* Nothing needs to be done if monitor mode is
				 * enabled
				 */
				return 0;
			}

			for (mac_id = 0; mac_id < max_mac_rings; mac_id++) {
				int mac_for_pdev = dp_get_mac_id_for_pdev(
							mac_id,	pdev->pdev_id);

				pdev->pktlog_ppdu_stats = true;
				dp_h2t_cfg_stats_msg_send(pdev,
					DP_PPDU_TXLITE_STATS_BITMASK_CFG,
					mac_for_pdev);
			}
			break;

		default:
			/* Nothing needs to be done for other pktlog types */
			break;
		}
	} else {
		switch (event) {
		case WDI_EVENT_RX_DESC:
		case WDI_EVENT_LITE_RX:
			if (pdev->monitor_vdev) {
				/* Nothing needs to be done if monitor mode is
				 * enabled
				 */
				return 0;
			}
			if (pdev->rx_pktlog_mode != DP_RX_PKTLOG_DISABLED) {
				pdev->rx_pktlog_mode = DP_RX_PKTLOG_DISABLED;
				dp_mon_filter_reset_rx_pkt_log_full(pdev);
				if (dp_mon_filter_update(pdev) !=
						QDF_STATUS_SUCCESS) {
					QDF_TRACE(QDF_MODULE_ID_DP,
						  QDF_TRACE_LEVEL_ERROR,
						  FL("Pktlog filters reset failed"));
					return 0;
				}

				dp_mon_filter_reset_rx_pkt_log_lite(pdev);
				if (dp_mon_filter_update(pdev) !=
						QDF_STATUS_SUCCESS) {
					QDF_TRACE(QDF_MODULE_ID_DP,
						  QDF_TRACE_LEVEL_ERROR,
						  FL("Pktlog filters reset failed"));
					return 0;
				}

				if (soc->reap_timer_init &&
				    (!dp_is_enable_reap_timer_non_pkt(pdev)))
					qdf_timer_stop(&soc->mon_reap_timer);
			}
			break;
		case WDI_EVENT_LITE_T2H:
			if (pdev->monitor_vdev) {
				/* Nothing needs to be done if monitor mode is
				 * enabled
				 */
				return 0;
			}
			/* To disable HTT_H2T_MSG_TYPE_PPDU_STATS_CFG in FW
			 * passing value 0. Once these macros will define in htt
			 * header file will use proper macros
			*/
			for (mac_id = 0; mac_id < max_mac_rings; mac_id++) {
				int mac_for_pdev =
						dp_get_mac_id_for_pdev(mac_id,
								pdev->pdev_id);

				pdev->pktlog_ppdu_stats = false;
				if (!pdev->enhanced_stats_en && !pdev->tx_sniffer_enable && !pdev->mcopy_mode) {
					dp_h2t_cfg_stats_msg_send(pdev, 0,
								mac_for_pdev);
				} else if (pdev->tx_sniffer_enable || pdev->mcopy_mode) {
					dp_h2t_cfg_stats_msg_send(pdev, DP_PPDU_STATS_CFG_SNIFFER,
								mac_for_pdev);
				} else if (pdev->enhanced_stats_en) {
					dp_h2t_cfg_stats_msg_send(pdev, DP_PPDU_STATS_CFG_ENH_STATS,
								mac_for_pdev);
				}
			}

			break;
		default:
			/* Nothing needs to be done for other pktlog types */
			break;
		}
	}
	return 0;
}
#endif

/**
 * dp_bucket_index() - Return index from array
 *
 * @delay: delay measured
 * @array: array used to index corresponding delay
 *
 * Return: index
 */
static uint8_t dp_bucket_index(uint32_t delay, uint16_t *array)
{
	uint8_t i = CDP_DELAY_BUCKET_0;

	for (; i < CDP_DELAY_BUCKET_MAX - 1; i++) {
		if (delay >= array[i] && delay <= array[i + 1])
			return i;
	}

	return (CDP_DELAY_BUCKET_MAX - 1);
}

/**
 * dp_fill_delay_buckets() - Fill delay statistics bucket for each
 *				type of delay
 *
 * @pdev: pdev handle
 * @delay: delay in ms
 * @tid: tid value
 * @mode: type of tx delay mode
 * @ring_id: ring number
 * Return: pointer to cdp_delay_stats structure
 */
static struct cdp_delay_stats *
dp_fill_delay_buckets(struct dp_pdev *pdev, uint32_t delay,
		      uint8_t tid, uint8_t mode, uint8_t ring_id)
{
	uint8_t delay_index = 0;
	struct cdp_tid_tx_stats *tstats =
		&pdev->stats.tid_stats.tid_tx_stats[ring_id][tid];
	struct cdp_tid_rx_stats *rstats =
		&pdev->stats.tid_stats.tid_rx_stats[ring_id][tid];
	/*
	 * cdp_fw_to_hw_delay_range
	 * Fw to hw delay ranges in milliseconds
	 */
	uint16_t cdp_fw_to_hw_delay[CDP_DELAY_BUCKET_MAX] = {
		0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 250, 500};

	/*
	 * cdp_sw_enq_delay_range
	 * Software enqueue delay ranges in milliseconds
	 */
	uint16_t cdp_sw_enq_delay[CDP_DELAY_BUCKET_MAX] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

	/*
	 * cdp_intfrm_delay_range
	 * Interframe delay ranges in milliseconds
	 */
	uint16_t cdp_intfrm_delay[CDP_DELAY_BUCKET_MAX] = {
		0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60};

	/*
	 * Update delay stats in proper bucket
	 */
	switch (mode) {
	/* Software Enqueue delay ranges */
	case CDP_DELAY_STATS_SW_ENQ:

		delay_index = dp_bucket_index(delay, cdp_sw_enq_delay);
		tstats->swq_delay.delay_bucket[delay_index]++;
		return &tstats->swq_delay;

	/* Tx Completion delay ranges */
	case CDP_DELAY_STATS_FW_HW_TRANSMIT:

		delay_index = dp_bucket_index(delay, cdp_fw_to_hw_delay);
		tstats->hwtx_delay.delay_bucket[delay_index]++;
		return &tstats->hwtx_delay;

	/* Interframe tx delay ranges */
	case CDP_DELAY_STATS_TX_INTERFRAME:

		delay_index = dp_bucket_index(delay, cdp_intfrm_delay);
		tstats->intfrm_delay.delay_bucket[delay_index]++;
		return &tstats->intfrm_delay;

	/* Interframe rx delay ranges */
	case CDP_DELAY_STATS_RX_INTERFRAME:

		delay_index = dp_bucket_index(delay, cdp_intfrm_delay);
		rstats->intfrm_delay.delay_bucket[delay_index]++;
		return &rstats->intfrm_delay;

	/* Ring reap to indication to network stack */
	case CDP_DELAY_STATS_REAP_STACK:

		delay_index = dp_bucket_index(delay, cdp_intfrm_delay);
		rstats->to_stack_delay.delay_bucket[delay_index]++;
		return &rstats->to_stack_delay;
	default:
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "%s Incorrect delay mode: %d", __func__, mode);
	}

	return NULL;
}

/**
 * dp_update_delay_stats() - Update delay statistics in structure
 *				and fill min, max and avg delay
 *
 * @pdev: pdev handle
 * @delay: delay in ms
 * @tid: tid value
 * @mode: type of tx delay mode
 * @ring id: ring number
 * Return: none
 */
void dp_update_delay_stats(struct dp_pdev *pdev, uint32_t delay,
			   uint8_t tid, uint8_t mode, uint8_t ring_id)
{
	struct cdp_delay_stats *dstats = NULL;

	/*
	 * Delay ranges are different for different delay modes
	 * Get the correct index to update delay bucket
	 */
	dstats = dp_fill_delay_buckets(pdev, delay, tid, mode, ring_id);
	if (qdf_unlikely(!dstats))
		return;

	if (delay != 0) {
		/*
		 * Compute minimum,average and maximum
		 * delay
		 */
		if (delay < dstats->min_delay)
			dstats->min_delay = delay;

		if (delay > dstats->max_delay)
			dstats->max_delay = delay;

		/*
		 * Average over delay measured till now
		 */
		if (!dstats->avg_delay)
			dstats->avg_delay = delay;
		else
			dstats->avg_delay = ((delay + dstats->avg_delay) / 2);
	}
}

/**
 * dp_get_peer_mac_list(): function to get peer mac list of vdev
 * @soc: Datapath soc handle
 * @vdev_id: vdev id
 * @newmac: Table of the clients mac
 * @mac_cnt: No. of MACs required
 *
 * return: no of clients
 */
uint16_t dp_get_peer_mac_list(ol_txrx_soc_handle soc, uint8_t vdev_id,
			      u_int8_t newmac[][QDF_MAC_ADDR_SIZE],
			      u_int16_t mac_cnt)
{
	struct dp_vdev *vdev =
		dp_get_vdev_from_soc_vdev_id_wifi3((struct dp_soc *)soc,
						   vdev_id);
	struct dp_soc *dp_soc = (struct dp_soc *)soc;
	struct dp_peer *peer;
	uint16_t new_mac_cnt = 0;

	if (!vdev)
		return new_mac_cnt;

	qdf_spin_lock_bh(&dp_soc->peer_ref_mutex);
	TAILQ_FOREACH(peer, &vdev->peer_list, peer_list_elem) {
		if (peer->bss_peer)
			continue;
		if (new_mac_cnt < mac_cnt) {
			WLAN_ADDR_COPY(newmac[new_mac_cnt], peer->mac_addr.raw);
			new_mac_cnt++;
		}
	}
	qdf_spin_unlock_bh(&dp_soc->peer_ref_mutex);
	return new_mac_cnt;
}
