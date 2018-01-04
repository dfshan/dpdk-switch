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
