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
    uint32_t threshold = 0;
    uint32_t qlen_enque = qlen_bytes + pkt->pkt_len;
    uint32_t buff_occu_bytes = 0;
    mark_pkt = (app.ecn_enable && qlen_bytes >= (app.ecn_thresh_kb<<10));
    /*Check whether buffer overflows after enqueue*/
    if (app.shared_memory) {
        buff_occu_bytes = get_buff_occu_bytes();
        threshold = app.get_threshold(dst_port);
        if (qlen_enque > threshold) {
            ret = -1;
        } else if (buff_occu_bytes + pkt->pkt_len > app.buff_size_bytes) {
            ret = -2;
        }
    } else if (qlen_enque > app.buff_size_per_port_bytes) {
        ret = -2;
    }
    if (ret == 0 && mark_pkt) {
        /* do ecn marking */
        mark_ret = mark_packet_with_ecn(pkt);
        if (mark_ret < 0) {
            ret = -3;
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
