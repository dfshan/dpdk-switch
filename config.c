#include "main.h"

// struct app_params app;

static const char usage[] = "./<app name> [EAL options] -- -p PORTMASK\n";

void
app_print_usage(void) {
    printf("USAGE: %s", usage);
}

static int
app_parse_port_mask(const char *arg) {
    char *end = NULL;
    uint64_t port_mask;
    uint32_t i;

    if (arg[0] == '\0')
        return -1;

    port_mask = strtoul(arg, &end, 16);
    if ((end == NULL) || (*end != '\0'))
        return -2;

    if (port_mask == 0) {
        RTE_LOG(
            ERR, SWITCH,
            "%s: no port specified\n",
            __func__
        );
        return -3;
    }

    app.n_ports = 0;
    for (i = 0; i < 64; i++) {
        if ((port_mask & (1LLU << i)) == 0)
            continue;

        if (app.n_ports >= APP_MAX_PORTS) {
            RTE_LOG(
                ERR, SWITCH,
                "%s: # of ports (%u) is larger than maximum supported port number (%u)\n",
                __func__, app.n_ports, APP_MAX_PORTS
            );
            return -4;
        }

        app.ports[app.n_ports] = i;
        app.n_ports++;
    }

    if (!rte_is_power_of_2(app.n_ports)) {
        RTE_LOG(
            WARNING, SWITCH,
            "%s: # of ports (%u) is not power of 2\n",
            __func__, app.n_ports
        );
        return -5;
    }

    return 0;
}


static int
app_read_config_file(const char *fname) {
    struct app_configs app_cfg = {
        .shared_memory = cfg_false,
        .buffer_size_kb = -1,
        .dt_shift_alpha = -1,
        .bm_policy = NULL,
        .qlen_fname = NULL,
        .log_qlen = cfg_false,
        .log_qlen_port = -1,
        .ecn_enable = cfg_false,
        .ecn_thresh_kb = -1,
        .tx_rate_mbps = -1,
        .bucket_size = -1,
        .cedm_enable = cfg_false,
        .cedm_ws = -1,
        .cedm_k2_kb = -1,
        .cfg = NULL
    };
    cfg_opt_t opts[] = {
        CFG_SIMPLE_BOOL("shared_memory", &app_cfg.shared_memory),
        CFG_SIMPLE_INT("buffer_size", &app_cfg.buffer_size_kb),
        CFG_SIMPLE_STR("buffer_management_policy", &app_cfg.bm_policy),
        CFG_SIMPLE_INT("dt_shift_alpha", &app_cfg.dt_shift_alpha),
        CFG_SIMPLE_BOOL("log_queue_length", &app_cfg.log_qlen),
        CFG_SIMPLE_INT("log_queue_length_port", &app_cfg.log_qlen_port),
        CFG_SIMPLE_STR("queue_length_file", &app_cfg.qlen_fname),
        CFG_SIMPLE_BOOL("ecn_enable", &app_cfg.ecn_enable),
        CFG_SIMPLE_INT("ecn_threshold", &app_cfg.ecn_thresh_kb),
        CFG_SIMPLE_INT("tx_rate_mbps", &app_cfg.tx_rate_mbps),
        CFG_SIMPLE_INT("bucket_size", &app_cfg.bucket_size),
        CFG_SIMPLE_BOOL("cedm_enable", &app_cfg.cedm_enable),
        CFG_SIMPLE_FLOAT("cedm_ws", &app_cfg.cedm_ws),
        CFG_SIMPLE_INT("cedm_k2_kb", &app_cfg.cedm_k2_kb),
        CFG_END()
    };
    app_cfg.cfg = cfg_init(opts, 0);
    if (cfg_parse(app_cfg.cfg, fname) == CFG_FILE_ERROR) {
        RTE_LOG(
            ERR, SWITCH,
            "%s: Configuration file '%s' cannot open for reading.\n",
            __func__, fname
        );
        if (app_cfg.cfg != NULL) {
            cfg_free(app_cfg.cfg);
        }
        if (app_cfg.bm_policy != NULL) {
            free(app_cfg.bm_policy);
        }
        if (app_cfg.qlen_fname != NULL) {
            free(app_cfg.qlen_fname);
        }
        return 1;
    }
    app.buff_size_bytes = (app_cfg.buffer_size_kb > 0 ? (app_cfg.buffer_size_kb<<10) : app.buff_size_bytes);
    if (app_cfg.shared_memory) {
        app.shared_memory = 1;
        if (!strcmp(app_cfg.bm_policy, "Equal Division")) {
            app.get_threshold = qlen_threshold_equal_division;
            RTE_LOG(
                INFO, SWITCH,
                "%s: shared memory enabled, bm_policy: %s, buffer_size: %uB=%uKiB\n",
                __func__,
                app_cfg.bm_policy,
                app.buff_size_bytes,
                app.buff_size_bytes/1024
            );
        } else if (!strcmp(app_cfg.bm_policy, "Dynamic Threshold")
                || !strcmp(app_cfg.bm_policy, "DT")) {
            RTE_LOG(
                INFO, SWITCH,
                "%s: shared memory enabled, bm_policy: Dynamic Threshold,\
                buffer_size: %uB=%uKiB, dt_shift_alpha: %u\n",
                __func__,
                app.buff_size_bytes,
                app.buff_size_bytes/1024,
                app.dt_shift_alpha
            );
            app.get_threshold = qlen_threshold_dt;
            app.dt_shift_alpha = (app_cfg.dt_shift_alpha >= 0 ? app_cfg.dt_shift_alpha : app.dt_shift_alpha);
        } else {
            RTE_LOG(
                ERR, SWITCH,
                "%s: Unsupported buffer management policy: %s, disable shared memory.\n",
                __func__, app_cfg.bm_policy
            );
            app.shared_memory = 0;
        }
    }
    if (app_cfg.log_qlen) {
        if (app_cfg.qlen_fname == NULL) {
            RTE_LOG(
                ERR, SWITCH,
                "%s: Enable queue length log, but log file name is not specified.\n",
                __func__
            );
        } else {
            app.qlen_file = fopen(app_cfg.qlen_fname, "w");
            if (app.qlen_file == NULL) {
                perror("Open file error:");
                RTE_LOG(
                    ERR, SWITCH,
                    "%s: Cannot open queue length log file '%s'\n",
                    __func__, app_cfg.qlen_fname
                );
            } else {
                app.log_qlen = 1;
                if (app_cfg.log_qlen_port >= 0 && app_cfg.log_qlen_port < app.n_ports) {
                    app.log_qlen_port = app_cfg.log_qlen_port;
                } else {
                    app.log_qlen_port = app.n_ports;
                    RTE_LOG(
                        WARNING, SWITCH,
                        "%s: The log queue length port (%ld) is invalid. \
                        Queue length logging is enabled for all ports.",
                        __func__, app_cfg.log_qlen_port
                    );
                }
            }
        }
    }
    if (app.log_qlen) {
        if (app.log_qlen_port >= 0 && app.log_qlen_port < app.n_ports) {
            RTE_LOG(
                INFO, SWITCH,
                "%s: Queue length logging is enabled for port %u. Logging is dumped into file %s\n",
                __func__, app.log_qlen_port, app_cfg.qlen_fname
            );
        } else {
            RTE_LOG(
                WARNING, SWITCH,
                "%s: Queue length logging is enabled for all ports. \
                Logging is dumped into file %s\n",
                __func__, app_cfg.qlen_fname
            );
        }
    }
    if (app_cfg.ecn_enable && app_cfg.ecn_thresh_kb >= 0) {
        app.ecn_enable = 1;
        app.ecn_thresh_kb = app_cfg.ecn_thresh_kb;
    } else {
        app.ecn_enable = 0;
        app.ecn_thresh_kb = 0;
    }
    app.tx_rate_mbps = (app_cfg.tx_rate_mbps >= 0 ? app_cfg.tx_rate_mbps : 0);
    app.bucket_size = (app_cfg.bucket_size > ETHER_MIN_LEN ? app_cfg.bucket_size: app.bucket_size);
    if (app_cfg.bucket_size < ETHER_MAX_LEN) {
        RTE_LOG(
            WARNING, SWITCH,
            "%s: TBF bucket size (given %ldB) is smaller than MTU(%uB)\n",
            __func__, app_cfg.bucket_size, ETHER_MAX_LEN
        );
    }
    if (app.ecn_enable && app_cfg.cedm_enable) {
        app.cedm_enable = 1;
        if (app_cfg.cedm_k2_kb > app.ecn_thresh_kb) {
            app.cedm_thresh2_kb = app_cfg.cedm_k2_kb;
        } else {
            app.cedm_thresh2_kb = (5 * app.ecn_thresh_kb + 125) >> 2;
        }
        app.cedm_ws = app_cfg.cedm_ws;
    } else {
        app.cedm_enable = 0;
    }
    cfg_free(app_cfg.cfg);
    free(app_cfg.bm_policy);
    free(app_cfg.qlen_fname);
    return 0;
}

static void app_finish_config(void) {
    uint64_t max_tx_rate_mbps = (((uint64_t)1<<(68-RATE_SCALE)) / 1e6);
    if (!app.shared_memory) {
        app.buff_size_per_port_bytes = app.buff_size_bytes / app.n_ports;
        RTE_LOG(
            INFO, SWITCH,
            "%s: shared memory disabled, each port has %uB/%uKiB buffer.\n",
            __func__,
            app.buff_size_per_port_bytes,
            app.buff_size_per_port_bytes / 1024
        );
    }
    if (app.ecn_enable) {
        RTE_LOG(
            INFO, SWITCH,
            "%s: ECN marking is enabled, ECN threshold=%uKiB.\n",
            __func__, app.ecn_thresh_kb
        );
    }
    if (app.tx_rate_mbps > max_tx_rate_mbps) {
        RTE_LOG(
            ERR, SWITCH,
            "%s: tx rate must be smaller than %luMbps to prevent integer overflow\n",
            __func__,
            max_tx_rate_mbps
        );
        app.tx_rate_mbps = 0;
    }
    RTE_LOG(
        INFO, SWITCH,
        "%s: tx_rate: %luMbps, tbf bucket size=%uB\n",
        __func__, app.tx_rate_mbps, app.bucket_size
    );
    if (app.cedm_enable) {
        RTE_LOG(
            INFO, SWITCH,
            "%s: CEDM is enabled, K1=%uKiB, K2=%uKiB, ws=%.6f.\n",
            __func__, app.ecn_thresh_kb, app.cedm_thresh2_kb, app.cedm_ws
        );
    }
}

int
app_parse_args(int argc, char **argv) {
    int opt, ret;
    char **argvopt;
    int option_index;
    char *prgname = argv[0];
    static struct option lgopts[] = {
        {"none", 0, 0, 0},
    };
    uint32_t lcores[RTE_MAX_LCORE], n_lcores, lcore_id, i;

    /* Non-EAL args */
    argvopt = argv;

    while ((opt = getopt_long(argc, argvopt, "p:",
            lgopts, &option_index)) != EOF) {
        switch (opt) {
        case 'p':
            if (app_parse_port_mask(optarg) < 0) {
                return -1;
            }
            break;

        default:
            return -1;
        }
    }

    /* EAL args */
    n_lcores = 0;
    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        if (rte_lcore_is_enabled(lcore_id) == 0)
            continue;

        lcores[n_lcores] = lcore_id;
        n_lcores++;
    }

    //if (n_lcores != 2+app.n_ports) {
    if (n_lcores < 3) {
        RTE_LOG(ERR, SWITCH, "# of cores must be larger than 3.\n");
        return -1;
    }

    app.core_rx = lcores[0];
    app.core_worker = lcores[1];
    for (i = 0; i < n_lcores ; i++) {
        app.core_tx[i] = lcores[i+2];
    }
    app.n_lcores = n_lcores;
    if (optind >= 0)
        argv[optind - 1] = prgname;

    ret = optind - 1;
    optind = 1; /* reset getopt lib */
    app_read_config_file("switch.conf");
    app_finish_config();
    return ret;
}
