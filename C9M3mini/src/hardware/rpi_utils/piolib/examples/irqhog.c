/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"

int main(__unused int argc, __unused  const char **argv) {
    int pio_irq;
    int ret = 0;

    stdio_init_all();

    PIO pio = pio0;

    pio_irq = pio_get_irq_num(pio, 0);
    printf("pio_irq = %d\n", pio_irq);

    while (1)
        sleep_ms(1000);

    if (ret)
        printf("* error %d\n", ret);
    return ret;
}
