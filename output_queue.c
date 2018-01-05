#include "main.h"

uint32_t
qlen_threshold_equal_division(uint32_t port_id) {
    port_id = port_id << 1; /* prevent warning */
    uint32_t result = app.buff_size_bytes / app.n_ports;
    return result;
}

uint32_t
qlen_threshold_dt(uint32_t port_id) {
    port_id = port_id << 1; /* prevent warning */
    return ((app.buff_size_bytes - get_buff_occu_bytes()) << app.dt_shift_alpha);
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

static int ipv4_set_ecn_bit(struct ipv4_hdr *iphdr, uint8_t bit) {
    uint16_t cksum;
    uint8_t tos = iphdr->type_of_service;
    iphdr->type_of_service = (tos &  0xfc) + (bit & 3);
    iphdr->hdr_checksum = 0;
    cksum = rte_ipv4_cksum(iphdr);
    iphdr->hdr_checksum = cksum;
    return 0;
}

static int mark_packet_with_ecn(struct rte_mbuf *pkt) {
    struct ipv4_hdr *iphdr;
    if (RTE_ETH_IS_IPV4_HDR(pkt->packet_type)) {
        iphdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, sizeof(struct ether_hdr));
        if ((iphdr->type_of_service & 0x03) != 0) {
            ipv4_set_ecn_bit(iphdr, 3);
        } else {
            return -2;
        }
        return 0;
    } else {
        return -1;
    }
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
    if (ret == 0 && pkt->pkt_len > 100 && mark_pkt) {
        if (app.cedm_enable) {
            cedm_before_enqueue(dst_port, pkt);
        } else {
            /* do ecn marking */
            mark_ret = mark_packet_with_ecn(pkt);
            if (mark_ret < 0) {
                ret = -3;
            }
            /* end */
        }
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
                "%-12.6f %-8u %-8u %-8u %f\n",
                (float) (rte_get_tsc_cycles() - app.qlen_start_cycle) / app.cpu_freq[rte_lcore_id()],
                app.ports[dst_port],
                qlen_bytes,
                buff_occu_bytes,
                app.cedm_avg_slope[dst_port]
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

void cedm_before_enqueue(uint32_t port, struct rte_mbuf *pkt) {
    uint8_t tos_ecn;
    uint32_t qlen_bytes;
    double avg_slope;
    struct ipv4_hdr *iphdr = NULL;
    if (RTE_ETH_IS_IPV4_HDR(pkt->packet_type)) {
        iphdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, sizeof(struct ether_hdr));
    } else {
        return ;
    }
    tos_ecn = (iphdr->type_of_service & 3);
    if (tos_ecn == 0) {
        return ;
    }
    qlen_bytes = get_qlen_bytes(port);
    avg_slope = app.cedm_avg_slope[port];
    if (qlen_bytes >= (app.cedm_thresh2_kb<<10)) {
        ipv4_set_ecn_bit(iphdr, 3);
    } if (qlen_bytes >= (app.ecn_thresh_kb<<10) && avg_slope >= 0) {
        ipv4_set_ecn_bit(iphdr, 1);
    }
}

void cedm_after_dequeue(uint32_t port, struct rte_mbuf *pkt) {
    uint32_t qlen_bytes;
    int32_t qlen_diff;
    uint64_t current_time, intvl;
    uint8_t tos_ecn;
    double slope, avg_slope;
    struct ipv4_hdr *iphdr = NULL;
    current_time = rte_get_tsc_cycles();
    qlen_bytes = get_qlen_bytes(port);
    avg_slope = app.cedm_avg_slope[port];
    if (pkt->pkt_len > MEAN_PKT_SIZE) {
        if (unlikely(app.prev_dequeue_time[port] == 0)) {
            app.prev_dequeue_time[port] = current_time;
            app.prev_qlen_bytes[port] = qlen_bytes;
            app.cedm_avg_slope[port] = 0;
            return ;
        }
        /* Update slope */
        qlen_diff = qlen_bytes - app.prev_qlen_bytes[port];
        intvl = current_time - app.prev_dequeue_time[port];
        slope = qlen_diff;
        slope = slope / intvl;
        avg_slope = (1.0 - app.cedm_ws) * avg_slope + app.cedm_ws * slope;
        app.cedm_avg_slope[port] = avg_slope;
        app.prev_dequeue_time[port] = current_time;
        app.prev_qlen_bytes[port] = qlen_bytes;
    }

    if (RTE_ETH_IS_IPV4_HDR(pkt->packet_type)) {
        iphdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, sizeof(struct ether_hdr));
    } else {
        return ;
    }
    tos_ecn = (iphdr->type_of_service & 3);
    if (tos_ecn == 0) {
        return ;
    }
    if (qlen_bytes >= (app.cedm_thresh2_kb<<10)) {
        ipv4_set_ecn_bit(iphdr, 3);
    } else if (tos_ecn == 1) {
        if (qlen_bytes >= (app.ecn_thresh_kb<<10) && avg_slope >= 0) {
            ipv4_set_ecn_bit(iphdr, 3);
        } else {
            ipv4_set_ecn_bit(iphdr, 2);
        }
    }
}
