/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_lpm.h>
#include <rte_lpm6.h>
#include <rte_malloc.h>

#include "main.h"

void
app_main_loop_rx(void) {
    uint32_t i;
    int ret;

    RTE_LOG(INFO, SWITCH, "Core %u is doing RX\n", rte_lcore_id());

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
    for (i = 0; !force_quit ; i = ((i + 1) & (app.n_ports - 1))) {
        uint16_t n_mbufs;

        n_mbufs = rte_eth_rx_burst(
            app.ports[i],
            0,
            app.mbuf_rx.array,
            app.burst_size_rx_read);
        if (n_mbufs >= app.burst_size_rx_read) {
            RTE_LOG(
                DEBUG, SWITCH,
                "%s: receive %u packets from port %u\n",
                __func__, n_mbufs, app.ports[i]
            );
        }

        if (n_mbufs == 0)
            continue;

        do {
            ret = rte_ring_sp_enqueue_bulk(
                app.rings_rx[i],
                (void **) app.mbuf_rx.array,
                n_mbufs, NULL);
        } while (ret == 0);
    }
}

void
app_main_loop_worker(void) {
    struct app_mbuf_array *worker_mbuf;
    struct ether_hdr *eth;
    struct rte_mbuf* new_pkt;
    uint32_t i, j;
    int dst_port;

    RTE_LOG(INFO, SWITCH, "Core %u is doing work (no pipeline)\n",
        rte_lcore_id());

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
    app.fwd_item_valid_time = app.cpu_freq[rte_lcore_id()] / 1000 * VALID_TIME;

    if (app.log_qlen) {
        fprintf(
            app.qlen_file,
            "# %-10s %-8s %-8s %-8s\n",
            "<Time (in s)>",
            "<Port id>",
            "<Qlen in Bytes>",
            "<Buffer occupancy in Bytes>"
        );
        fflush(app.qlen_file);
    }
    worker_mbuf = rte_malloc_socket(NULL, sizeof(struct app_mbuf_array),
            RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (worker_mbuf == NULL)
        rte_panic("Worker thread: cannot allocate buffer space\n");

    for (i = 0; !force_quit; i = ((i + 1) & (app.n_ports - 1))) {
        int ret;

        /*ret = rte_ring_sc_dequeue_bulk(
            app.rings_rx[i],
            (void **) worker_mbuf->array,
            app.burst_size_worker_read);*/
        ret = rte_ring_sc_dequeue(
            app.rings_rx[i],
            (void **) worker_mbuf->array);

        if (ret == -ENOENT)
            continue;

        // l2 learning
        eth = rte_pktmbuf_mtod(worker_mbuf->array[0], struct ether_hdr*);
        app_l2_learning(&(eth->s_addr), i);

        // l2 forward
        dst_port = app_l2_lookup(&(eth->d_addr));
        if (dst_port < 0) { /* broadcast */
            RTE_LOG(DEBUG, SWITCH, "%s: broadcast packets\n", __func__);
            for (j = 0; j < app.n_ports; j++) {
                if (j == i) {
                    continue;
                } else if (j == (i ^ 1)) {
                    packet_enqueue(j, worker_mbuf->array[0]);
                } else {
                    new_pkt = rte_pktmbuf_clone(worker_mbuf->array[0], app.pool);
                    packet_enqueue(j, new_pkt);
                    /*rte_ring_sp_enqueue(
                        app.rings_tx[j],
                        new_pkt
                    );*/
                }
            }
        } else {
            RTE_LOG(
                DEBUG, SWITCH,
                "%s: forward packet to %d\n",
                __func__, app.ports[dst_port]
            );
            packet_enqueue(dst_port, worker_mbuf->array[0]);
            /*rte_ring_sp_enqueue(
                app.rings_tx[dst_port],
                worker_mbuf->array[0]
            );*/
        }

        /*do {
            ret = rte_ring_sp_enqueue_bulk(
                app.rings_tx[i ^ 1],
                (void **) worker_mbuf->array,
                app.burst_size_worker_write);
        } while (ret < 0);*/
    }
}

void
app_main_loop_tx_each_port(uint32_t port_id) {
	uint32_t i = port_id;
    struct rte_mbuf* pkt;
    /* next time allowed to transmit packets */
	uint64_t current_time = rte_get_tsc_cycles();
	uint64_t token;
	uint64_t prev_time;

    RTE_LOG(INFO, SWITCH, "Core %u is doing TX for port %u\n", rte_lcore_id(), port_id);

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
	token = app.bucket_size;
    prev_time = current_time;
    while(!force_quit) {
        uint16_t n_mbufs, n_pkts;
        int ret;

        n_mbufs = app.mbuf_tx[i].n_mbufs;

		current_time = rte_get_tsc_cycles();
        if (app.tx_rate_mbps > 0) {
			// tbf: generate tokens
			token += (uint64_t) app.tx_rate_mbps * 1e6 / app.cpu_freq[app.core_tx[i]] / 8;
			token *= (current_time - prev_time);
			token = MIN(token, (app.bucket_size<<1));
			prev_time = current_time;
			if (token < app.bucket_size) {
				continue;
			}
        }
        ret = rte_ring_sc_dequeue(
            app.rings_tx[i],
            (void **) &app.mbuf_tx[i].array[n_mbufs]);

        if (ret == -ENOENT) { /* no packets in tx ring */
            continue;
        }

        pkt = app.mbuf_tx[i].array[n_mbufs];
		app.qlen_bytes_out[i] += pkt->pkt_len;
		app.qlen_pkts_out[i] ++;
        if (app.tx_rate_mbps > 0) {
			token -= pkt->pkt_len;
		}

        n_mbufs ++;

        RTE_LOG(
            DEBUG, SWITCH,
            "%s: port %u receive %u packets\n",
            __func__, app.ports[i], n_mbufs
        );

        if (n_mbufs < app.burst_size_tx_write) {
            app.mbuf_tx[i].n_mbufs = n_mbufs;
            continue;
        }

        uint16_t k = 0;
        do {
            n_pkts = rte_eth_tx_burst(
                app.ports[i],
                0,
                &app.mbuf_tx[i].array[k],
                n_mbufs - k);
            k += n_pkts;
            if (k < n_mbufs) {
                RTE_LOG(
                    DEBUG, SWITCH,
                    "%s: Transmit ring is full in port %u\n",
                    __func__, app.ports[i]
                );
            }
        } while (k < n_mbufs);

        app.mbuf_tx[i].n_mbufs = 0;
    }
}

void
app_main_loop_tx(void) {
    uint32_t i;
    struct rte_mbuf* pkt;
    /* next time allowed to transmit packets */
	uint64_t current_time = rte_get_tsc_cycles();
	uint64_t token[APP_MAX_PORTS];
	uint64_t prev_time[APP_MAX_PORTS];

    RTE_LOG(INFO, SWITCH, "Core %u is doing TX\n", rte_lcore_id());

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
    for (i = 0; i < app.n_ports; i++) {
		token[i] = app.bucket_size;
        prev_time[i] = current_time;
    }
    for (i = 0; !force_quit; i = ((i + 1) & (app.n_ports - 1))) {
        uint16_t n_mbufs, n_pkts;
        int ret;

        n_mbufs = app.mbuf_tx[i].n_mbufs;

		current_time = rte_get_tsc_cycles();
        if (app.tx_rate_mbps > 0) {
			// tbf: generate tokens
			token[i] += (uint64_t) app.tx_rate_mbps * (current_time - prev_time[i]) * 1e6 / app.cpu_freq[app.core_tx[i]] / 8;
			token[i] = MIN(token[i], (app.bucket_size<<1));
			prev_time[i] = current_time;
			if (token[i] < app.bucket_size) {
				continue;
			}
        }
        ret = rte_ring_sc_dequeue(
            app.rings_tx[i],
            (void **) &app.mbuf_tx[i].array[n_mbufs]);

        if (ret == -ENOENT) { /* no packets in tx ring */
            continue;
        }

        pkt = app.mbuf_tx[i].array[n_mbufs];
		app.qlen_bytes_out[i] += pkt->pkt_len;
		app.qlen_pkts_out[i] ++;
        if (app.tx_rate_mbps > 0) {
			token[i] -= pkt->pkt_len;
		}

        n_mbufs ++;

        RTE_LOG(
            DEBUG, SWITCH,
            "%s: port %u receive %u packets\n",
            __func__, app.ports[i], n_mbufs
        );

        if (n_mbufs < app.burst_size_tx_write) {
            app.mbuf_tx[i].n_mbufs = n_mbufs;
            continue;
        }

        uint16_t k = 0;
        do {
            n_pkts = rte_eth_tx_burst(
                app.ports[i],
                0,
                &app.mbuf_tx[i].array[k],
                n_mbufs - k);
            k += n_pkts;
            if (k < n_mbufs) {
                RTE_LOG(
                    DEBUG, SWITCH,
                    "%s: Transmit ring is full in port %u\n",
                    __func__, app.ports[i]
                );
            }
        } while (k < n_mbufs);

        app.mbuf_tx[i].n_mbufs = 0;
    }
}

static int mark_packet_with_ecn(struct rte_mbuf *pkt) {
	struct ipv4_hdr *iphdr;
	uint16_t cksum;
	if (RTE_ETH_IS_IPV4_HDR(pkt->packet_type)) {
		iphdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, sizeof(struct ether_hdr));
		if ((iphdr->type_of_service & 0x03) != 0) {
			iphdr->type_of_service |= 0x3;
			iphdr->hdr_checksum = 0;
			cksum = rte_ipv4_cksum(iphdr);
			iphdr->hdr_checksum = cksum;
		} else {
			return -2;
		}
		return 0;
	} else {
		return -1;
	}
}

uint32_t get_qlen_bytes(uint32_t port_id) {
	return app.qlen_bytes_in[port_id] - app.qlen_bytes_out[port_id];
}

uint32_t get_buff_occu_bytes(void) {
	uint32_t i, result = 0;
	for (i = 0; i < app.n_ports; i++) {
		result += (app.qlen_bytes_in[i] - app.qlen_bytes_out[i]);
	}
	return result;
	//return app.buff_bytes_in - app.buff_bytes_out;
}

int packet_enqueue(uint32_t dst_port, struct rte_mbuf *pkt) {
    int ret = 0, mark_pkt = 0, mark_ret;
	uint32_t qlen_bytes = get_qlen_bytes(dst_port);
    /*Check whether buffer overflows after enqueue*/
    uint32_t threshold = app.get_threshold(dst_port);
    uint32_t qlen_enque = qlen_bytes + pkt->pkt_len;
	uint32_t buff_occu_bytes = get_buff_occu_bytes();
	mark_pkt = (app.ecn_enable && qlen_bytes >= (app.ecn_thresh_kb<<10));
    if (qlen_enque > threshold) {
        ret = -1;
    }
    else if (
        buff_occu_bytes + pkt->pkt_len > app.buff_size_bytes
    ) {
        ret = -2;
    } else {
        ret = 0;
		/* do ecn marking */
		if (mark_pkt) {
			mark_ret = mark_packet_with_ecn(pkt);
			if (mark_ret < 0) {
				ret = -3;
			}
		}
		/* end */
    }
    if (ret == 0) {
        int enque_ret = rte_ring_sp_enqueue(
            app.rings_tx[dst_port],
            pkt
        );
        if (enque_ret != 0) {
            RTE_LOG(
                ERR, SWITCH,
                "%s: packet cannot enqueue in port %u",
                __func__, app.ports[dst_port]
            );
        }
		app.qlen_bytes_in[dst_port] += pkt->pkt_len;
		app.qlen_pkts_in[dst_port] ++;
		/*app.buff_bytes_in += pkt->pkt_len;
		app.buff_pkts_in ++;*/
        if (
			app.log_qlen && pkt->pkt_len >= MEAN_PKT_SIZE &&
			(app.log_qlen_port >= app.n_ports || app.log_qlen_port == dst_port)
		) {
			if (app.qlen_start_cycle == 0) {
				app.qlen_start_cycle = rte_get_tsc_cycles();
			}
            fprintf(
                app.qlen_file,
				"%-12.6f %-8u %-8u %-8u\n",
				(float) (rte_get_tsc_cycles() - app.qlen_start_cycle) / app.cpu_freq[rte_lcore_id()],
                app.ports[dst_port],
				qlen_bytes,
                buff_occu_bytes
            );
        }
    } else {
        rte_pktmbuf_free(pkt);
    }
    switch (ret) {
    case 0:
        RTE_LOG(
            DEBUG, SWITCH,
            "%s: packet enqueue to port %u\n",
            __func__, app.ports[dst_port]
        );
        break;
    case -1:
        RTE_LOG(
            DEBUG, SWITCH,
            "%s: Packet dropped due to queue length > threshold\n",
            __func__
        );
        break;
    case -2:
        RTE_LOG(
            DEBUG, SWITCH,
            "%s: Packet dropped due to buffer overflow\n",
            __func__
        );
    case -3:
        RTE_LOG(
            DEBUG, SWITCH,
            "%s: Cannot mark packet with ECN, drop packet\n",
            __func__
        );
    }
    return ret;
}
