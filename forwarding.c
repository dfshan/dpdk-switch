#include "main.h"

int
app_l2_learning(const struct ether_addr* srcaddr, uint8_t port) {
    if (app.l2_hash == NULL) {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR hash table is not initialized.\n",
            __func__
        );
        return -1;
    }
    int index = rte_hash_lookup(app.l2_hash, srcaddr);
    if (index == -EINVAL) {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR the parameters are invalid when lookup hash table\n",
            __func__
        );
    } else if (index == -ENOENT) {
        int new_ind = rte_hash_add_key(app.l2_hash, srcaddr);
        app.fwd_table[new_ind].port_id = port;
        app.fwd_table[new_ind].timestamp = rte_get_tsc_cycles();
        /* gettimeofday(&app.fwd_table[new_ind].timestamp, NULL); */
        RTE_LOG(
            INFO, HASH,
            "%s: new item in forwarding table:"
            " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
            " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
            " --> %d\n",
            __func__,
            srcaddr->addr_bytes[0], srcaddr->addr_bytes[1],
            srcaddr->addr_bytes[2], srcaddr->addr_bytes[3],
            srcaddr->addr_bytes[4], srcaddr->addr_bytes[5],
            app.ports[port]
        );
    } else if (index < 0 || index >= FORWARD_ENTRY) {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR invalid table entry found in hash table: %d\n",
            __func__, index
        );
        return -1;
    } else {
        int old_port = app.fwd_table[index].port_id;
        app.fwd_table[index].port_id = port;
        app.fwd_table[index].timestamp = rte_get_tsc_cycles();
        /*gettimeofday(&app.fwd_table[index].timestamp, NULL);*/
        if (old_port != port) {
            RTE_LOG(
                INFO, HASH,
                "%s: Update item in forwarding table:"
                " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
                " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
                "--> %d (previous: %d)\n",
                __func__,
                srcaddr->addr_bytes[0], srcaddr->addr_bytes[1],
                srcaddr->addr_bytes[2], srcaddr->addr_bytes[3],
                srcaddr->addr_bytes[4], srcaddr->addr_bytes[5],
                app.ports[port], app.ports[old_port]
            );
        }
    }
    return 0;
}

int
app_l2_lookup(const struct ether_addr* addr) {
    int index = rte_hash_lookup(app.l2_hash, addr);
    if (index >= 0 && index < FORWARD_ENTRY) {
        uint64_t now_time = rte_get_tsc_cycles();
        uint64_t interval = now_time - app.fwd_table[index].timestamp;
        if (interval <= app.fwd_item_valid_time) {
            return app.fwd_table[index].port_id;
        } else {
            RTE_LOG(
                INFO, HASH,
                "%s: Fowllowing item is outdated, delete it from forwarding table:"
                " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
                " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
                "--> %d\n",
                __func__,
                addr->addr_bytes[0], addr->addr_bytes[1],
                addr->addr_bytes[2], addr->addr_bytes[3],
                addr->addr_bytes[4], addr->addr_bytes[5],
                app.ports[app.fwd_table[index].port_id]
            );
            rte_hash_del_key(app.l2_hash, addr);
            return -1;
        }
        /*struct timeval now_time, intv_time;
        gettimeofday(&now_time, NULL);
        timersub(&now_time, &app.fwd_table[index].timestamp, &intv_time);
        long intv_time_us = intv_time.tv_sec * 1000 * 1000 + intv_time.tv_usec;
        if (intv_time_us / 1000 < VALID_TIME) {
            return app.fwd_table[index].port_id;
        } else {
            rte_hash_del_key(app.l2_hash, addr);
            return -1;
        }*/
    }
    return -1;
}

void
app_main_loop_forwarding(void) {
    struct app_mbuf_array *worker_mbuf;
    struct ether_hdr *eth;
    struct rte_mbuf* new_pkt;
    uint32_t i, j;
    int dst_port;

    RTE_LOG(INFO, SWITCH, "Core %u is doing forwarding\n",
        rte_lcore_id());

    app.cpu_freq[rte_lcore_id()] = rte_get_tsc_hz();
    app.fwd_item_valid_time = app.cpu_freq[rte_lcore_id()] / 1000 * VALID_TIME;

    if (app.log_qlen) {
        fprintf(
            app.qlen_file,
            "# %-10s %-8s %-8s %-8s %s\n",
            "<Time (in s)>",
            "<Port id>",
            "<Qlen in Bytes>",
            "<Buffer occupancy in Bytes>",
            "<avg slope>"
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
