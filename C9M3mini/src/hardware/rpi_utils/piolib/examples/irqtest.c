/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpiolib.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "irqtest.pio.h"

int sm;

uint gpio = 4;

static void irqtest_handler(void *) {
    gpio_set_drive(gpio + 3, DRIVE_HIGH);
    gpio_set_drive(gpio + 3, DRIVE_LOW);
    printf("%d!\n", sm);
}

int main(int argc, const char **argv) {
    int ret = 0;
    int pass;
    int i;

    stdio_init_all();

    ret = gpiolib_init();

    if (ret < 0)
    {
        printf("Failed to initialise gpiolib - %d\n", ret);
        return -1;
    }

    ret = gpiolib_mmap();
    if (ret)
    {
        if (ret == EACCES && geteuid())
            printf("Must be root (or group 'gpio' on RPiOS)\n");
        else
            printf("Failed to mmap gpiolib - %s\n", strerror(ret));
        return -1;
    }

    PIO pio = pio0;
    sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &irqtest_program);
    if (argc == 2)
        gpio = (uint)strtoul(argv[1], NULL, 0);

    if (sm == 1)
        gpio += 4;
    pio_gpio_init(pio, gpio);
    pio_gpio_init(pio, gpio + 1);
    pio_gpio_init(pio, gpio + 2);
    gpio_set_fsel(gpio + 3, GPIO_FSEL_OUTPUT);
    pio_sm_set_consecutive_pindirs(pio, sm, gpio, 3, true);
    pio_sm_config c = irqtest_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, gpio);
    sm_config_set_clkdiv_int_frac(&c, 65535, 0);

    for (pass = 1; pass < 2; pass++) {
        int pio_irq = -1;

        pio_sm_init(pio, sm, offset, &c);
        pio_sm_set_enabled(pio, sm, true);

        if (pass == 1) {
            pio_irq = pio_get_irq_num(pio, 0);
            irq_add_shared_handler(pio_irq, irqtest_handler, PICO_DEFAULT_IRQ_PRIORITY);
            pio_set_irqn_source_enabled(pio, 0, pis_interrupt0, true);
            irq_set_enabled(pio_irq, true);
        }

        for (i = 0; i < 10; i++) {
            printf("%d: Iter %d: ", sm, i);
            // sleep_ms(500);
            pio_sm_put_blocking(pio, sm, i);
            // sleep_ms(250);
            pio_sm_put_blocking(pio, sm, 10 - i);
            while (!pio_interrupt_get(pio, pio_interrupt_rel(sm, 0))) {
                //sleep_ms(10);
            }
            while (!pio_interrupt_get(pio, pio_interrupt_rel(sm, 2))) {
                //sleep_ms(10);
            }
            pio_interrupt_clear(pio, pio_interrupt_rel(sm, 2));
        }

        pio_sm_set_enabled(pio, sm, false);

        if (pio_irq >= 0) {
            irq_set_enabled(pio_irq, false);
            pio_set_irqn_source_enabled(pio, 0, pis_interrupt0, false);
        }
    }

    if (ret)
        printf("* %d: error %d\n", sm, ret);
    return ret;
}
