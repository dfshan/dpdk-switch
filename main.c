#include <signal.h>

#include "main.h"

volatile bool force_quit;

static void
signal_handler(int signum) {
    switch (signum) {
    case SIGTERM:
    case SIGINT:
        RTE_LOG(
            INFO, SWITCH,
            "%s: Receive %d signal, prepare to exit...\n",
            __func__, signum
        );
        force_quit = true;
        break;
    }
}

static void
app_quit(void) {
    uint8_t i;
    /* close ports */
    for (i = 0; i < app.n_ports; i++) {
        uint8_t port = (uint8_t) app.ports[i];
        RTE_LOG(
            INFO, SWITCH,
            "%s: Closing NIC port %u ...\n",
            __func__, port
        );
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
    }
    /* free resources */
    /*if (app_cfg.cfg != NULL) {
        cfg_free(app_cfg.cfg);
    }
    if (app_cfg.bm_policy != NULL) {
        free(app_cfg.bm_policy);
    }
    if (app_cfg.qlen_fname != NULL) {
        free(app_cfg.qlen_fname);
    }*/
    /* close files */
    if (app.log_qlen) {
        fclose(app.qlen_file);
    }
    printf("App quit. Bye...\n");
}

int
main(int argc, char **argv) {
    uint32_t lcore;
    int ret;

    /* Init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        return -1;
    argc -= ret;
    argv += ret;

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Parse application arguments (after the EAL ones) */
    ret = app_parse_args(argc, argv);
    if (ret < 0) {
        app_print_usage();
        return -1;
    }

    /* Init */
    app_init();

    app.start_cycle = rte_get_tsc_cycles();
    /* Launch per-lcore init on every lcore */
    rte_eal_mp_remote_launch(app_lcore_main_loop, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        if (rte_eal_wait_lcore(lcore) < 0) {
            return -1;
        }
    }

    app_quit();
    fflush(stdout);

    return 0;
}

int
app_lcore_main_loop(__attribute__((unused)) void *arg) {
    unsigned lcore, i;

    lcore = rte_lcore_id();

    if (lcore == app.core_rx) {
        app_main_loop_rx();
        return 0;
    }

    if (lcore == app.core_worker) {
        app_main_loop_worker();
        return 0;
    }

    if (app.n_lcores >= 2+app.n_ports) {
        for (i = 0; i < app.n_ports; i++) {
            if (lcore == app.core_tx[i]) {
                app_main_loop_tx_each_port(i);
                return 0;
            }
        }
    } else {
        if (lcore == app.core_tx[0]) {
            app_main_loop_tx();
            return 0;
        }
    }

    return 0;
}
