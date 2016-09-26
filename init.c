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
#include <string.h>

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
#include <rte_string_fns.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_lpm.h>
#include <rte_lpm6.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>

#include "main.h"

struct app_params app = {
    /* Ports*/
    .n_ports = 1,
    .port_rx_ring_size = 128,
    .port_tx_ring_size = 32,

    /* switch buffer */
    .buff_size_pkts = 256,
    .mean_pkt_size = 1500,
    .buff_occu_pkts = 0,
    .buff_occu_bytes = 0,
    .get_threshold = qlen_threshold_equal_division,

    /* Rings */
    .ring_rx_size = 128,
    /* Notes: this value can be changed in function app_init_rings()*/
    .ring_tx_size = 128,

    /* Buffer pool */
    .pool_buffer_size = 2048 + RTE_PKTMBUF_HEADROOM,
    /* Notes: this value can be changed in function app_init_mbuf_pools*/
    .pool_size = 32 * 1024 - 1,
    .pool_cache_size = 256,

    /* Burst sizes */
    .burst_size_rx_read = 64,
    .burst_size_rx_write = 64,
    .burst_size_worker_read = 1,
    .burst_size_worker_write = 1,
    .burst_size_tx_read = 1,
    .burst_size_tx_write = 1,

    /* forwarding things */
    .ft_name = "Forwarding Table",
    .l2_hash = NULL,
};

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .split_hdr_size = 0,
        .header_split   = 0, /* Header Split disabled */
        .hw_ip_checksum = 1, /* IP checksum offload enabled */
        .hw_vlan_filter = 0, /* VLAN filtering disabled */
        .jumbo_frame    = 0, /* Jumbo Frame Support disabled */
        .hw_strip_crc   = 0, /* CRC stripped by hardware */
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_IP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

static struct rte_eth_rxconf rx_conf = {
    .rx_thresh = {
        .pthresh = 8,
        .hthresh = 8,
        .wthresh = 4,
    },
    .rx_free_thresh = 64,
    .rx_drop_en = 0,
};

static struct rte_eth_txconf tx_conf = {
    .tx_thresh = {
        .pthresh = 36,
        .hthresh = 0,
        .wthresh = 0,
    },
    .tx_free_thresh = 0,
    .tx_rs_thresh = 0,
};

/*
 * Change an arbitary value to a power of 2.
 * E.g., 0x5 --> 0x8, 0x36 --> 0x40
 */
static uint32_t
topower2(uint32_t x) {
    uint32_t result = 0x1;
    while (((-result) & x) != 0) {
        result = result << 1;
    }
    return result;
}


static void
app_init_locks(void) {
    /*uint32_t i;
    rte_rwlock_init(&app.lock_bocu);
    for (i = 0; i < app.n_ports; i++) {
        rte_rwlock_init(&app.lock_qlen[i]);
    }*/
    rte_spinlock_init(&app.lock_buff);
}

static void
app_init_mbuf_pools(void)
{
    /* Init the buffer pool */
    RTE_LOG(INFO, SWITCH, "Creating the mbuf pool ...\n");
    uint32_t temp_pool_size = (topower2(app.buff_size_pkts) << 2) - 1;
    app.pool_size = (app.pool_size < temp_pool_size ? temp_pool_size : app.pool_size);
    app.pool = rte_pktmbuf_pool_create("mempool", app.pool_size,
        app.pool_cache_size, 0, app.pool_buffer_size, rte_socket_id());
    if (app.pool == NULL)
        rte_panic("Cannot create mbuf pool\n");
}

static void
app_init_rings(void)
{
    uint32_t i;

    for (i = 0; i < app.n_ports; i++) {
        char name[32];

        snprintf(name, sizeof(name), "app_ring_rx_%u", i);

        app.rings_rx[i] = rte_ring_create(
            name,
            app.ring_rx_size,
            rte_socket_id(),
            RING_F_SP_ENQ | RING_F_SC_DEQ);

        if (app.rings_rx[i] == NULL)
            rte_panic("Cannot create RX ring %u\n", i);
    }

    app.ring_tx_size = (topower2(app.buff_size_pkts) << 2);
    for (i = 0; i < app.n_ports; i++) {
        char name[32];

        snprintf(name, sizeof(name), "app_ring_tx_%u", i);

        app.rings_tx[i] = rte_ring_create(
            name,
            app.ring_tx_size,
            rte_socket_id(),
            RING_F_SP_ENQ | RING_F_SC_DEQ);

        if (app.rings_tx[i] == NULL)
            rte_panic("Cannot create TX ring %u\n", i);
        app.qlen_bytes[i] = app.qlen_pkts[i] = 0;
    }

}

static void
app_ports_check_link(void)
{
    uint32_t all_ports_up, i;

    all_ports_up = 1;

    for (i = 0; i < app.n_ports; i++) {
        struct rte_eth_link link;
        uint8_t port;

        port = (uint8_t) app.ports[i];
        memset(&link, 0, sizeof(link));
        rte_eth_link_get(port, &link);
        RTE_LOG(INFO, SWITCH, "Port %u (%u Gbps) %s\n",
            port,
            link.link_speed / 1000,
            link.link_status ? "UP" : "DOWN");

        if (link.link_status == ETH_LINK_DOWN)
            all_ports_up = 0;
    }

    if (all_ports_up == 0)
        rte_panic("Some NIC ports are DOWN\n");
}

static void
app_init_ports(void)
{
    uint32_t i;

    /* Init NIC ports, then start the ports */
    for (i = 0; i < app.n_ports; i++) {
        uint8_t port;
        int ret;

        port = (uint8_t) app.ports[i];
        RTE_LOG(INFO, SWITCH, "Initializing NIC port %u ...\n", port);

        /* Init port */
        ret = rte_eth_dev_configure(
            port,
            1,
            1,
            &port_conf);
        if (ret < 0)
            rte_panic("Cannot init NIC port %u (%d)\n", port, ret);

        rte_eth_promiscuous_enable(port);

        /* Init RX queues */
        ret = rte_eth_rx_queue_setup(
            port,
            0,
            app.port_rx_ring_size,
            rte_eth_dev_socket_id(port),
            &rx_conf,
            app.pool);
        if (ret < 0)
            rte_panic("Cannot init RX for port %u (%d)\n",
                (uint32_t) port, ret);

        /* Init TX queues */
        ret = rte_eth_tx_queue_setup(
            port,
            0,
            app.port_tx_ring_size,
            rte_eth_dev_socket_id(port),
            &tx_conf);
        if (ret < 0)
            rte_panic("Cannot init TX for port %u (%d)\n",
                (uint32_t) port, ret);

        /* Start port */
        ret = rte_eth_dev_start(port);
        if (ret < 0)
            rte_panic("Cannot start port %u (%d)\n", port, ret);
    }

    app_ports_check_link();
}


int app_init_forwarding_table(const char* tname) {
    size_t name_len = strlen(tname);
    if (name_len > MAX_NAME_LEN) {
        RTE_LOG(ERR, HASH, "%s: ERROR when init forward table: table name too long\n", __func__);
        return -1;
    }
    rte_memcpy(app.ft_name, tname, name_len);
    struct rte_hash_parameters hash_params = {
        .name = app.ft_name,
        .entries = FORWARD_ENTRY,
        .key_len = sizeof(struct ether_addr),
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
    };
    app.l2_hash = rte_hash_create(&hash_params);
    if (app.l2_hash == NULL) {
        RTE_LOG(ERR, HASH, "%s: ERROR when create hash table.\n", __func__);
        return -1;
    }
    return 0;
}

void
app_init(void)
{
    app_init_mbuf_pools();
    app_init_rings();
    app_init_ports();
    app_init_forwarding_table("forwarding table");
    app_init_locks();

    RTE_LOG(INFO, SWITCH, "Initialization completed\n");
}
