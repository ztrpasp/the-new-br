#ifndef __BR_SEND_PKT_H__
#define __BR_SEND_PKT_H__

#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include "br_conf.h"
#include "log.h"
#include "common.h"

/**
 * @brief Send burst of packets on an output interface.
 * Packet ownership will be transferred to this function.
 * This function should only be called in run-to-completion mode.
 * 
 * @post \p pkt_list->len is set to zero.
 * 
 * @param pkt_list array of packets to be sent
 * @param port_id ID of the port to sent packets
 * @param queue_id ID of the queue of the port \p port_id
 */
static inline void rtc_tx_burst(struct pkt_list *pkt_list, uint16_t port_id, uint16_t queue_id)
{
	static int cnt = 0;

	struct rte_mbuf **pkts = pkt_list->pkts;
	uint16_t n_pkts = pkt_list->len;
	uint16_t n_sent = rte_eth_tx_burst(port_id, queue_id, pkts, n_pkts);
	if (unlikely(n_sent < n_pkts)) {
		uint16_t n_unsent = n_pkts - n_sent;
		cnt += n_unsent;
		// BR_INFO("\r                   \r# of pkts unsent: %d, ", cnt);
		BR_INFO("# of pkts unsent: %d\n", cnt);
		for (int i = n_sent; i < n_pkts; i++) {
			rte_pktmbuf_free(pkts[i]);
		}
	}
	pkt_list->len = 0;
}

/**
 * @brief Enqueue a single packet, and send burst if queue is filled.
 * The ownership of \p pkt is transferred to this function.
 * This function should only be called in run-to-completion mode.
 * 
 * @param br_conf pointer to BR's configuration struct
 * @param pkt pointer to the packet mbuf to be sent
 */
static inline void rtc_send_single_packet(struct br_conf *br_conf, struct rte_mbuf *pkt)
{
	uint32_t lcore_id = rte_lcore_id();
	uint16_t port_id = pkt->port;
	struct tx_qconf *tx_qconf = &br_conf->rtc_qconf[lcore_id].tx_qconf;
	uint16_t queue_id = tx_qconf->queue_id[port_id];
	struct pkt_list *pkt_list = &tx_qconf->tx_mbufs[port_id];
	// assert pkt_list->len < MAX_PKT_BURST 
	pkt_list->pkts[pkt_list->len++] = pkt;
	if (unlikely(pkt_list->len >= MAX_PKT_BURST)) {
		rtc_tx_burst(pkt_list, port_id, queue_id);
	}
}

/**
 * @brief Send burst of packets on an output interface.
 * Packet ownership will be transferred to this function.
 * This function should only be called in pipeline0 mode.
 * 
 * @param pkts pointer to the packet mbuf array to be sent
 * @param n_pkts number of packets in the mbuf array
 * @param port_id ID of the port to sent packets
 * @param queue_id ID of the queue of the port \p port_id
 * @return uint16_t the number of unsent packets
 */
static inline uint16_t p0_tx_burst(struct rte_mbuf **pkts, int n_pkts, 
	uint16_t port_id, uint16_t queue_id)
{
	static int cnt = 0;

	uint16_t n_sent = rte_eth_tx_burst(port_id, queue_id, pkts, n_pkts);
	uint16_t n_unsent = n_pkts - n_sent;
	if (unlikely(n_unsent != 0)) {
		cnt += n_unsent;
		// BR_INFO("\r                   \r# of pkts unsent: %d", cnt);
		BR_INFO("# of pkts unsent: %d\n", cnt);
		for (int i = n_sent; i < n_pkts; i++) {
			rte_pktmbuf_free(pkts[i]);
		}
	}
	return n_unsent;
}

/**
 * @brief Enqueue a single packet to the ring of the lcore on which this function 
 * is called. The ownership of \p pkt is transferred to this function.
 * This function should only be called in pipeline0 mode.
 * 
 * @param br_conf pointer to BR's configuration struct
 * @param pkt pointer to the packet mbuf to be sent
 * @return int returns \p FAILED if the ring is full else \p OK
 */
static inline int p0_send_single_packet(struct br_conf *br_conf, struct rte_mbuf *pkt)
{
	uint32_t lcore_id = rte_lcore_id();
	struct worker_qconf *worker_qconf = &br_conf->p0_qconf.workers_qconf[lcore_id];

	struct pkt_list *pkt_list = &worker_qconf->tx_mbufs;
	pkt_list->pkts[pkt_list->len++] = pkt;
	if (unlikely(pkt_list->len >= MAX_PKT_BURST)) {
		int ret = 0;
		int n_tries = 0;
		do {
			ret = rte_ring_sp_enqueue_bulk(worker_qconf->tx_ring, 
				(void **)pkt_list->pkts, pkt_list->len, NULL);
			if (ret == 0) {
				BR_LOGF("tx ring for lcore %d full, n_tries: %d\r\n", lcore_id, ++n_tries);
			}
		} while (ret == 0 && !br_conf->force_quit);
		pkt_list->len = 0;
	}

	return OK;
}

/**
 * @brief Enqueue a single packet, and send burst if queue is filled.
 * The ownership of \p pkt is transferred to this function.
 * This function can be called in run-to-completion or pipeline0 mode.
 * 
 * @param br_conf pointer to BR's configuration struct
 * @param pkt pointer to the packet mbuf to be sent
 * @return int returns \p FAILED if mode is undefined else \p OK.
 */
static inline int send_single_packet(struct br_conf *br_conf, struct rte_mbuf *pkt)
{
	int ret = OK;
	if (br_conf->mode == RUN_TO_COMPLETION) {
		rtc_send_single_packet(br_conf, pkt);
	} else if (br_conf->mode == PIPELINE0) {
		p0_send_single_packet(br_conf, pkt);
	} else {
		BR_LOGF("unsupported br_mode\n");
		rte_pktmbuf_free(pkt);
		ret = FAILED;
	}

	return ret;
}

#endif