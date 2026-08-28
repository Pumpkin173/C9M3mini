/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "txcount.pio.h"

#define DATA_WORDS 100

int main(__unused int argc, __unused const char **argv) {
    uint32_t databuf[DATA_WORDS];
    bool use_dma = true;
    uint gpio = 2;
    int ret = 0;
    int i, j;
    stdio_init_all();

    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &txcount_program);
    uint32_t last_count = 0;
    printf("Loaded program at %d, using sm %d, gpio %d\n", offset, sm, gpio);

    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, 256, 1);

    pio_gpio_init(pio, gpio);
    pio_sm_set_consecutive_pindirs(pio, sm, gpio, 1, true);
    pio_sm_config c = txcount_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, gpio);
    sm_config_set_clkdiv(&c, 1);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    for (i = 1; i <= DATA_WORDS; i++) {
        uint32_t count;
        int diff;
        printf("Iter %d:\n", i);
        if (use_dma) {
            ret = pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, i * sizeof(databuf[0]), databuf);
            if (ret)
               break;
        } else {
            for (j = 0; j < i; j++)
            {
                pio_sm_put_blocking(pio, sm, databuf[i]);
            }
        }

        while (!pio_sm_is_rx_fifo_empty(pio, sm))
            pio_sm_get(pio, sm);
        pio_sm_put_blocking(pio, sm, 0);
        count = pio_sm_get_blocking(pio, sm);
        diff = -(count + 1 - last_count);
        if (diff != i) {
            printf("%d -> %d\n", i, diff);
            //return -1;
        }
        last_count = count;
    }

    if (ret)
        printf("* error %d\n", ret);
    return ret;
}
