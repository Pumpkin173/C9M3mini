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
#include "irqtest2.pio.h"

int sm;

uint gpio = 4;

static void irqtest2_handler(void *) {
    gpio_set_drive(gpio + 3, DRIVE_HIGH);
    sleep_us(10);
    gpio_set_drive(gpio + 3, DRIVE_LOW);
    printf("%d", sm);
}

int main(int argc, const char **argv) {
    int pio_irq = -1;
    int ret = 0;
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
    pio_irq = pio_get_irq_num(pio, 0);
    uint offset = pio_add_program(pio, &irqtest2_program);
    if (argc == 2)
        gpio = (uint)strtoul(argv[1], NULL, 0);
    else
        gpio += pio_irq * 4;

    gpio_set_fsel(gpio + 2, GPIO_FSEL_INPUT);
    gpio_set_fsel(gpio + 3, GPIO_FSEL_OUTPUT);
    for (i = 0; i < 2; i++) {
        int level = (i == 0) ? DRIVE_HIGH : DRIVE_LOW;
        gpio_set_drive(gpio + 3, level);
        sleep_ms(10);
        if (gpio_get_level(gpio + 2) != level) {
            printf("please connect GPIOs %d and %d\n", gpio + 2, gpio + 3);
            exit(1);
        }
    }

    pio_gpio_init(pio, gpio);
    pio_gpio_init(pio, gpio + 1);
    pio_gpio_init(pio, gpio + 2);
    pio_sm_set_consecutive_pindirs(pio, sm, gpio, 2, true);
    pio_sm_set_consecutive_pindirs(pio, sm, gpio + 2, 1, false);
    pio_sm_config c = irqtest2_program_get_default_config(offset);
    sm_config_set_in_pins(&c, gpio + 2);
    sm_config_set_sideset_pins(&c, gpio);

    pio_sm_init(pio, sm, offset, &c);

    irq_set_handler(pio_irq, irqtest2_handler, NULL);
    pio_set_irqn_source_enabled(pio, 0,
                                pio_interrupt_rel(sm, pis_interrupt0),
                                true);
    irq_set_enabled(pio_irq, true);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_enabled(pio, sm, true);

    for (i = 0; i < 10; i++) {
        printf("\n%d: Iter %d: ", sm, i);
        // sleep_ms(500);
        pio_sm_put_blocking(pio, sm, i);

        while (1) {
            sleep_ms(50);
            if (!pio_sm_is_rx_fifo_empty(pio, sm))
                break;
            printf("!");
            fflush(stdout);
            gpio_set_drive(gpio + 3, DRIVE_HIGH);
            gpio_set_drive(gpio + 3, DRIVE_LOW);
        }
        pio_sm_get_blocking(pio, sm);
    }
    printf("\n");

    pio_sm_set_enabled(pio, sm, false);

    if (pio_irq >= 0) {
        irq_set_enabled(pio_irq, false);
        pio_set_irqn_source_enabled(pio, 0,
                                    pio_interrupt_rel(sm, pis_interrupt0),
                                    false);
    }

    if (ret)
        printf("* %d: error %d\n", sm, ret);
    return ret;
}
