/*
 * mRTOS idle-dominated "sensor node" demo - MSP430FR5994.
 *
 * The point of this build: show that mRTOS keeps a hard real-time deadline
 * AND draws power like a no-RTOS superloop at the same time. A single
 * periodic task wakes every 1 s (the deadline), does a tiny bounded
 * "sample + process", pulses P1.2 as a logic-analyzer deadline marker, and
 * sleeps. The kernel suppresses the between-deadline ticks (tickless LPM3
 * idle), so for >99 % of wall time the MCU sits at the LPM3 floor
 * (single-digit uA) - yet the task still runs exactly on the period.
 *
 * Determinism is recorded for read-back over gdb (read these as VARIABLES;
 * an mrtos_now() inferior call returns garbage on a halted LPM target):
 *   wake_count          - periods elapsed
 *   interval_min/max    - wake-to-wake spacing in ticks (1024 = 1.000 s)
 *   wake_log[]          - the first WAKE_LOG_N intervals, in order
 *   now_first/now_last  - with wake_count, checks long-term drift
 *                         (now_last - now_first should == wake_count*1024)
 *
 * Built by `make idle_demo.elf` / flashed by `make idle-run`. This is the
 * mRTOS side of the planned no-RTOS superloop comparison (POWER.md S3).
 */
#include <msp430.h>
#include "mrtos.h"

#define STK_WORDS  128u
#define PERIOD_MS  1000u
#define WAKE_LOG_N 64u
#define WORK_ITERS 32u     /* bounded "sample + process", well under 1 tick */

static mrtos_tcb_t  tcb_sensor;
static port_stack_t stk_sensor[STK_WORDS];

volatile uint32_t wake_count;
volatile uint16_t interval_min = 0xFFFFu, interval_max;
volatile uint16_t wake_log[WAKE_LOG_N];
volatile uint16_t wake_log_n;
volatile uint32_t now_first, now_last;
volatile uint32_t sample_acc;          /* keeps the bounded "work" live */

static void board_init(void)
{
    WDTCTL = WDTPW | WDTHOLD;

    /* All GPIO driven low (LPM hygiene), then unlock. */
    P1OUT = 0; P1DIR = 0xFF;
    P2OUT = 0; P2DIR = 0xFF;
    P3OUT = 0; P3DIR = 0xFF;
    P4OUT = 0; P4DIR = 0xFF;
    P5OUT = 0; P5DIR = 0xFF;
    P6OUT = 0; P6DIR = 0xFF;
    P7OUT = 0; P7DIR = 0xFF;
    P8OUT = 0; P8DIR = 0xFF;
    PJOUT = 0; PJDIR = 0xFF;
    PM5CTL0 &= ~LOCKLPM5;

    /* DCO = 8 MHz (MCLK = SMCLK); ACLK = LFXT 32768 crystal so the tick
     * timer survives LPM3. LFXIN/LFXOUT = PJ.4/PJ.5. */
    PJSEL0 |= BIT4 | BIT5;
    CSCTL0_H = CSKEY_H;
    CSCTL1   = DCOFSEL_3 | DCORSEL;
    CSCTL4  &= ~LFXTOFF;
    do {
        CSCTL5 &= ~LFXTOFFG;
        SFRIFG1 &= ~OFIFG;
    } while (SFRIFG1 & OFIFG);
    CSCTL2   = SELA__LFXTCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3   = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;
}

static void task_sensor(void *arg)
{
    (void)arg;
    uint32_t last = mrtos_now();
    now_first = last;

    for (;;) {
        mrtos_sleep(MRTOS_MS(PERIOD_MS));      /* sleep to the next deadline */

        uint32_t now = mrtos_now();
        uint16_t d   = (uint16_t)(now - last);
        last = now;
        now_last = now;
        if (d < interval_min) interval_min = d;
        if (d > interval_max) interval_max = d;
        if (wake_log_n < WAKE_LOG_N) wake_log[wake_log_n++] = d;
        wake_count++;

        /* Bounded "sample + process", bracketed by a P1.2 marker pulse for a
         * logic analyzer. Kept under one tick so the period stays exactly
         * 1024 ticks; the feedback term (acc >> 3) makes it data-dependent so
         * the compiler cannot fold it to a constant - the active burst is
         * real, it is simply dwarfed by the 1 s LPM3 sleep around it. */
        P1OUT |= BIT2;
        uint32_t acc = sample_acc;
        for (uint16_t i = 0; i < WORK_ITERS; i++)
            acc += (uint32_t)i * 7u + (acc >> 3);
        sample_acc = acc;
        P1OUT &= ~BIT2;
    }
}

int main(void)
{
    board_init();
    mrtos_init();
    mrtos_task_create(&tcb_sensor, "sensor", task_sensor, NULL, 1,
                      stk_sensor, STK_WORDS);
    mrtos_start();                             /* never returns */
}
