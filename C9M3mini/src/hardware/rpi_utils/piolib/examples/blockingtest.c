/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "blockingtest.pio.h"

#define LOOP_ITERATIONS 300
#define IRQ_ITERATIONS  20
#define IRQ_FLAG        4

static PIO pio;
static int sm_loop;
static int sm_irq;
static int sm_trigger;

static atomic_int errors;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *fmt, ...) {
    va_list args;
    pthread_mutex_lock(&log_lock);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* Pushes values into the loopback state machine as fast as it can, pausing
 * occasionally so the consumer sees an empty FIFO and has to block for data. */
static void *loop_producer_thread(void *arg) {
    (void)arg;
    pio_select(pio);
    for (uint32_t i = 0; i < LOOP_ITERATIONS; i++) {
        pio_sm_put_blocking(pio, sm_loop, i);
        if ((i % 7) == 0)
            usleep(rand() % 3000);
    }
    return NULL;
}

/* Pulls the values back out, checking that FIFO ordering survived the trip,
 * pausing occasionally so the producer sees a full FIFO and has to block. */
static void *loop_consumer_thread(void *arg) {
    (void)arg;
    pio_select(pio);
    for (uint32_t i = 0; i < LOOP_ITERATIONS; i++) {
        uint32_t value = pio_sm_get_blocking(pio, sm_loop);
        if (value != i) {
            log_line("loop: expected %u, got %u\n", i, value);
            atomic_fetch_add(&errors, 1);
        }
        if ((i % 5) == 0)
            usleep(rand() % 4000);
    }
    return NULL;
}

/* Sits on sm_irq, which is permanently stalled on "wait 1 irq". Each call
 * only returns once irq_trigger_thread() has set the flag it is waiting on. */
static void *irq_consumer_thread(void *arg) {
    (void)arg;
    pio_select(pio);
    for (int i = 0; i < IRQ_ITERATIONS; i++) {
        pio_sm_get_blocking(pio, sm_irq);
        log_line("irq: wait %d released\n", i);
    }
    return NULL;
}

/* Delays, then injects an "irq set" instruction into sm_trigger - an
 * unrelated, idling state machine - using the blocking exec call. This
 * proves that pio_sm_exec_wait_blocking() only returns once the injected
 * instruction has actually retired, and that doing so on one state machine
 * correctly wakes another that is blocked in "wait 1 irq". */
static void *irq_trigger_thread(void *arg) {
    (void)arg;
    pio_select(pio);
    uint set_instr = pio_encode_irq_set(false, IRQ_FLAG);
    for (int i = 0; i < IRQ_ITERATIONS; i++) {
        usleep(50000 + (rand() % 150000));
        pio_sm_exec_wait_blocking(pio, sm_trigger, set_instr);
        log_line("irq: trigger %d fired\n", i);
    }
    return NULL;
}

int main(__unused int argc, __unused const char **argv) {
    pthread_t loop_producer, loop_consumer, irq_trigger, irq_consumer;

    stdio_init_all();
    srand(time(NULL));

    pio = pio0;

    sm_loop = pio_claim_unused_sm(pio, true);
    uint offset_loop = pio_add_program(pio, &blocking_loop_program);
    sm_irq = pio_claim_unused_sm(pio, true);
    uint offset_irq = pio_add_program(pio, &blocking_irq_program);
    sm_trigger = pio_claim_unused_sm(pio, true);
    uint offset_trigger = pio_add_program(pio, &blocking_idle_program);

    printf("sm_loop=%d sm_irq=%d sm_trigger=%d\n", sm_loop, sm_irq, sm_trigger);

    /* Run the loopback state machine slowly, so that put_blocking() and
     * get_blocking() are genuinely forced to block on the hardware FIFOs,
     * not just on each other's sleep()s. */
    pio_sm_config c = blocking_loop_program_get_default_config(offset_loop);
    sm_config_set_clkdiv_int_frac(&c, 1000, 0);
    pio_sm_init(pio, sm_loop, offset_loop, &c);
    pio_sm_set_enabled(pio, sm_loop, true);

    c = blocking_irq_program_get_default_config(offset_irq);
    pio_sm_init(pio, sm_irq, offset_irq, &c);
    pio_sm_set_enabled(pio, sm_irq, true);

    c = blocking_idle_program_get_default_config(offset_trigger);
    pio_sm_init(pio, sm_trigger, offset_trigger, &c);
    pio_sm_set_enabled(pio, sm_trigger, true);

    pthread_create(&loop_producer, NULL, loop_producer_thread, NULL);
    pthread_create(&loop_consumer, NULL, loop_consumer_thread, NULL);
    pthread_create(&irq_consumer, NULL, irq_consumer_thread, NULL);
    pthread_create(&irq_trigger, NULL, irq_trigger_thread, NULL);

    pthread_join(loop_producer, NULL);
    pthread_join(loop_consumer, NULL);
    pthread_join(irq_trigger, NULL);
    pthread_join(irq_consumer, NULL);

    pio_sm_set_enabled(pio, sm_loop, false);
    pio_sm_set_enabled(pio, sm_irq, false);
    pio_sm_set_enabled(pio, sm_trigger, false);

    int err = atomic_load(&errors);
    printf(err ? "* FAILED, %d error(s)\n" : "PASSED\n", err);
    return err ? 1 : 0;
}
