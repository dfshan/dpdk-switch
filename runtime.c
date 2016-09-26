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
#include <rte_per_lcore.h>
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

    for (i = 0; ; i = ((i + 1) & (app.n_ports - 1))) {
        uint16_t n_mbufs;

        n_mbufs = rte_eth_rx_burst(
            app.ports[i],
            0,
            app.mbuf_rx.array,
            app.burst_size_rx_read);
        if (n_mbufs >= app.burst_size_rx_read) {
            RTE_LOG(DEBUG, SWITCH, "%s: receive %u packets from port %u\n", __func__, n_mbufs, app.ports[i]);
        }

        if (n_mbufs == 0)
            continue;

        do {
            ret = rte_ring_sp_enqueue_bulk(
                app.rings_rx[i],
                (void **) app.mbuf_rx.array,
                n_mbufs);
        } while (ret < 0);
    }
}

uint32_t
packet_enqueue(uint32_t dst_port, struct rte_mbuf *pkt) {
    int ret = 0;
    /*Check whether buffer overflows after enqueue*/
    rte_spinlock_lock(&app.lock_buff);
    uint32_t threshold = app.get_threshold(dst_port);
#if QUE_IN_BYTES == 1
    uint32_t qlen_enque = app.qlen_bytes[dst_port] + pkt->pkt_len;
    if (qlen_enque > threshold) {
        ret = -1;
    }
    else if (app.buff_occu_bytes + pkt->pkt_len > app.buff_size_pkts * app.mean_pkt_size) {
        ret = -2;
    } else {
        ret = 0;
    }
#else
    if (app.qlen_pkts[dst_port] + 1 > threshold) {
        ret = -1;
    } else if (app.buff_occu_pkts > app.buff_size_pkts) {
        ret = -2;
    } else {
        ret = 0;
    }
#endif
    if (ret == 0) {
        int enque_ret = rte_ring_sp_enqueue(
            app.rings_tx[dst_port],
            pkt
        );
        if (enque_ret != 0) {
            RTE_LOG(ERR, SWITCH, "%s: packet cannot enqueue in port %u", __func__, app.ports[dst_port]);
        }
        app.qlen_bytes[dst_port] += pkt->pkt_len;
        app.qlen_pkts[dst_port] ++;
        app.buff_occu_bytes += pkt->pkt_len;
        app.buff_occu_pkts ++;
        if (app.log_qlen && pkt->pkt_len > app.mean_pkt_size) {
            fprintf(
                app.qlen_file,
                "%-10lu %-8u %-8u %-8u %-8u %-8u\n",
                rte_get_tsc_cycles() - app.start_cycle,
                dst_port,
                app.qlen_pkts[dst_port],
                app.qlen_bytes[dst_port],
                app.buff_occu_pkts,
                app.buff_occu_bytes
            );
        }
    } else {
        rte_pktmbuf_free(pkt);
    }
    rte_spinlock_unlock(&app.lock_buff);
    switch (ret) {
    case 0:
        RTE_LOG(DEBUG, SWITCH, "%s: packet enqueue to port %u\n", __func__, app.ports[dst_port]);
        break;
    case -1:
        RTE_LOG(DEBUG, SWITCH, "%s: Packet dropped due to queue length > threshold\n", __func__);
        break;
    case -2:
        RTE_LOG(DEBUG, SWITCH, "%s: Packet dropped due to buffer overflow\n", __func__);
    }
    return ret;
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

    worker_mbuf = rte_malloc_socket(NULL, sizeof(struct app_mbuf_array),
            RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (worker_mbuf == NULL)
        rte_panic("Worker thread: cannot allocate buffer space\n");

    for (i = 0; ; i = ((i + 1) & (app.n_ports - 1))) {
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
            RTE_LOG(DEBUG, SWITCH, "%s: forward packet to %d\n", __func__, app.ports[dst_port]);
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
app_main_loop_tx(void) {
    uint32_t i, j;
    struct rte_mbuf* pkt;

    RTE_LOG(INFO, SWITCH, "Core %u is doing TX\n", rte_lcore_id());

    for (i = 0; ; i = ((i + 1) & (app.n_ports - 1))) {
        uint16_t n_mbufs, n_pkts;
        int ret;

        n_mbufs = app.mbuf_tx[i].n_mbufs;

        rte_spinlock_lock(&app.lock_buff);
        ret = rte_ring_sc_dequeue_bulk(
            app.rings_tx[i],
            (void **) &app.mbuf_tx[i].array[n_mbufs],
            app.burst_size_tx_read);

        if (ret == -ENOENT) {
            rte_spinlock_unlock(&app.lock_buff);
            continue;
        }

        for (j = 0; j < app.burst_size_tx_read; j++) {
            pkt = app.mbuf_tx[i].array[n_mbufs+j];
            app.qlen_bytes[i] -= pkt->pkt_len;
            app.qlen_pkts[i] --;
            app.buff_occu_bytes -= pkt->pkt_len;
            app.buff_occu_pkts --;
        }
        rte_spinlock_unlock(&app.lock_buff);

        n_mbufs += app.burst_size_tx_read;

        RTE_LOG(DEBUG, SWITCH, "%s: port %u receive %u packets\n", __func__, app.ports[i], n_mbufs);

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
                n_mbufs);
            k += n_pkts;
        } while (n_pkts < n_mbufs);

        app.mbuf_tx[i].n_mbufs = 0;
    }
}
