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

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>

#ifndef APP_MBUF_ARRAY_SIZE
#define APP_MBUF_ARRAY_SIZE 256
#endif

#define RTE_LOGTYPE_SWITCH RTE_LOGTYPE_USER1

#undef RTE_LOG_LEVEL
#define RTE_LOG_LEVEL RTE_LOG_DEBUG

#define FORWARD_ENTRY 10 // # of forwarding table entries
#define MAX_NAME_LEN 100
#define VALID_TIME INT_MAX // valid time (in ms) for a forwarding item

struct app_mbuf_array {
	struct rte_mbuf *array[APP_MBUF_ARRAY_SIZE];
	uint16_t n_mbufs;
};

#ifndef APP_MAX_PORTS
#define APP_MAX_PORTS 4
#endif

struct app_fwd_table_item {
	uint8_t port_id;
	struct timeval timestamp; // the time when the item is added
};

struct app_params {
	/* CPU cores */
	uint32_t core_rx;
	uint32_t core_worker;
	uint32_t core_tx;

	/* Ports*/
	uint32_t ports[APP_MAX_PORTS];
	uint32_t n_ports;
	uint32_t port_rx_ring_size;
	uint32_t port_tx_ring_size;

	/* Rings */
	struct rte_ring *rings_rx[APP_MAX_PORTS];
	struct rte_ring *rings_tx[APP_MAX_PORTS];
	uint32_t ring_rx_size;
	uint32_t ring_tx_size;

	/* Internal buffers */
	struct app_mbuf_array mbuf_rx;
	struct app_mbuf_array mbuf_tx[APP_MAX_PORTS];

	/* Buffer pool */
	struct rte_mempool *pool;
	uint32_t pool_buffer_size;
	uint32_t pool_size;
	uint32_t pool_cache_size;

	/* Burst sizes */
	uint32_t burst_size_rx_read;
	uint32_t burst_size_rx_write;
	uint32_t burst_size_worker_read;
	uint32_t burst_size_worker_write;
	uint32_t burst_size_tx_read;
	uint32_t burst_size_tx_write;

	/* App behavior */
	// uint32_t pipeline_type;
    
    /* things about forwarding table */
    struct app_fwd_table_item fwd_table[FORWARD_ENTRY];
    char ft_name[MAX_NAME_LEN]; /* forward table name */
    struct rte_hash* l2_hash;
} __rte_cache_aligned;

extern struct app_params app;

int app_parse_args(int argc, char **argv);
void app_print_usage(void);
void app_init(void);
int app_lcore_main_loop(void *arg);

void app_main_loop_rx(void);
void app_main_loop_worker(void);
void app_main_loop_tx(void);

/*
 * Initialize forwarding table.
 * Return 0 when OK, -1 when there is error.
 */
int app_init_forwarding_table(const char *tname);

/*
 * Return 0 when OK, -1 when there is error.
 */
int app_l2_learning(const struct ether_addr* srcaddr, uint8_t port);

/*
 * Return port id to forward (broadcast if negative)
 */
int app_l2_lookup(const struct ether_addr* addr);


#define APP_FLUSH 0
#ifndef APP_FLUSH
#define APP_FLUSH 0x3FF
#endif

#define APP_METADATA_OFFSET(offset) (sizeof(struct rte_mbuf) + (offset))

#endif /* _MAIN_H_ */
