/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "txsink.pio.h"

#define DATA_WORDS 16

int main(__unused int argc, __unused const char **argv) {
    uint32_t databuf[DATA_WORDS];
    uint gpio = 2;
    int ret = 0;
    int i;
    stdio_init_all();

    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &txsink_program);
    printf("Loaded program at %d, using sm %d, gpio %d\n", offset, sm, gpio);

    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, 256, 4);

    pio_gpio_init(pio, gpio);
    pio_sm_set_consecutive_pindirs(pio, sm, gpio, 1, true);
    pio_sm_config c = txsink_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, gpio);
    sm_config_set_clkdiv(&c, 1);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    for (i = 0; i < 1000; i++) {
        ret = pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, sizeof(databuf), databuf);
        if (ret)
            break;
    }

    if (ret)
        printf("* error %d\n", ret);
    return ret;
}
