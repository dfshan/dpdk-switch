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
