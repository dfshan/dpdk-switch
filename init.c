#include "main.h"

struct app_params app = {
    /* Ports*/
    .n_ports = 1,
    .port_rx_ring_size = 128,
    .port_tx_ring_size = 32,

    /* switch buffer */
    .shared_memory = 0,
    .buff_size_bytes = 256,
    .buff_bytes_in = 0,
    .buff_pkts_in = 0,
    .buff_bytes_out = 0,
    .buff_pkts_out = 0,
    .log_qlen = 0,
    .qlen_file = NULL,
    .get_threshold = qlen_threshold_equal_division,
    .dt_shift_alpha = 0,
	.qlen_start_cycle = 0,

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

	.ecn_enable = 0,
	.ecn_thresh_kb = 0,
    .tx_rate_mbps = 0,
	.bucket_size = 3200,

    .cedm_enable = 0,
    .cedm_ws = 0.0,
    .cedm_thresh2_kb = 0,
};

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .split_hdr_size = 0,
        .header_split   = 0, /* Header Split disabled */
        .hw_ip_checksum = 1, /* IP checksum offload enabled */
        .hw_vlan_filter = 0, /* VLAN filtering disabled */
        .jumbo_frame    = 0, /* Jumbo Frame Support disabled */
        .hw_strip_crc   = 1, /* CRC stripped by hardware */
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
app_init_mbuf_pools(void) {
    /* Init the buffer pool */
    RTE_LOG(INFO, SWITCH, "Creating the mbuf pool ...\n");
    uint32_t temp_pool_size = (topower2(app.buff_size_bytes / MEAN_PKT_SIZE) << 2) - 1;
    app.pool_size = (app.pool_size < temp_pool_size ? temp_pool_size : app.pool_size);
    app.pool = rte_pktmbuf_pool_create("mempool", app.pool_size,
        app.pool_cache_size, 0, app.pool_buffer_size, rte_socket_id());
    if (app.pool == NULL)
        rte_panic("Cannot create mbuf pool\n");
}

static void
app_init_rings(void) {
    uint32_t i;

    app.ring_rx_size = (topower2(app.buff_size_bytes / MEAN_PKT_SIZE) << 2);
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

    app.ring_tx_size = (topower2(app.buff_size_bytes / MEAN_PKT_SIZE) << 2);
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
        app.qlen_bytes_in[i] = app.qlen_pkts_in[i] = 0;
        app.qlen_bytes_out[i] = app.qlen_pkts_out[i] = 0;
        app.prev_qlen_bytes[i] = app.prev_dequeue_time[i] = 0;
        app.cedm_avg_slope[i] = 0;
    }

}

static void
app_ports_check_link(void) {
    uint32_t all_ports_up, i;

    all_ports_up = 1;

    for (i = 0; i < app.n_ports; i++) {
        struct rte_eth_link link;
        uint8_t port;

        port = (uint8_t) app.ports[i];
        memset(&link, 0, sizeof(link));
        rte_eth_link_get_nowait(port, &link);
        RTE_LOG(INFO, SWITCH, "Port %u (%u Gbps) %s\n",
            port,
            link.link_speed / 1000,
            link.link_status ? "UP" : "DOWN");

        if (link.link_status == ETH_LINK_DOWN)
            all_ports_up = 0;
    }

    if (all_ports_up == 0) {
        RTE_LOG(WARNING, SWITCH, "%s: Some NIC ports are DOWN\n", __func__);
        // rte_panic("Some NIC ports are DOWN\n");
    }
}

static void
app_init_ports(void) {
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
    sleep(1);
    app_ports_check_link();
}


int app_init_forwarding_table(const char* tname) {
    size_t name_len = strlen(tname);
    if (name_len > MAX_NAME_LEN) {
        RTE_LOG(
            ERR, HASH,
            "%s: ERROR when init forward table: table name too long\n",
            __func__
        );
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
app_init(void) {
    app_init_mbuf_pools();
    app_init_rings();
    app_init_ports();
    app_init_forwarding_table("forwarding table");
    app_init_locks();

    RTE_LOG(INFO, SWITCH, "Initialization completed\n");
}
