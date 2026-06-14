/*
 * mRTOS micro-benchmark.
 *
 * Measures the kernel's primitive costs between two marker functions:
 *
 *   bench_mark_a();  <operation under test>  ...  bench_mark_b();
 *
 * The markers carry no timing logic of their own beyond a single timer
 * read, which keeps the measured path pristine. How an interval becomes
 * a number depends on where it runs:
 *
 *   - GNU MSP430 simulator (default build): markers are empty; tools/
 *     bench.py drives msp430-elf-gdb (target sim), breaks on both and
 *     counts single-stepped instructions in between. Deterministic to
 *     the instruction. Not cycle-accurate (1-6 cycles/instruction; FRAM
 *     wait states not modeled).
 *   - MSP430FR5994 target (-DBENCH_TARGET): markers read a free-running
 *     Timer_A (TA1) at SMCLK = 8 MHz, so 1 count = 1 CPU cycle = 125 ns.
 *     The minimum delta per metric (over many samples) is kept in
 *     bench_min[], read out via the debugger (make bench-read). A GPIO
 *     (P1.2) toggles in step for a logic-analyzer cross-check.
 *
 * Intervals may span a context switch (mark_a in the signaler, mark_b
 * first thing in the woken task), so "signal-to-wake" includes the full
 * hand-off. On target YIELD/SEM_WAKE bracket a *real* preemptive switch
 * (the cooperative sim cannot), making those numbers richer there.
 *
 * The metric sequence and sample count are a contract with tools/
 * bench.py: BENCH_N_INTERVALS = metrics x samples, in bench_id order.
 */
#include "mrtos.h"

#ifdef BENCH_TARGET
#include <msp430.h>
#define SAMPLES        32u    /* min over many rejects tick-ISR hits */
#define STK_WORDS      64u    /* FR5994 SRAM is 4 KB: keep stacks lean */
#else
#include <stdlib.h>
#define SAMPLES        3
#define STK_WORDS      256u
#endif
#define N_SLEEPERS     8

/* Metric ids, reported by tools/bench.py in this order. */
enum {
    BM_BASELINE = 0,   /* empty interval: marker overhead, subtracted */
    BM_YIELD,          /* one-way switch between equal-prio tasks     */
    BM_SEM_WAKE,       /* sem_give -> higher-prio waiter running      */
    BM_QSEND,          /* queue_send, poll, no waiter                 */
    BM_QRECV,          /* queue_recv, poll, data present              */
    BM_MUTEX,          /* uncontended lock + unlock                   */
    BM_TICK0,          /* mrtos_tick, empty delay list                */
    BM_TICK8,          /* mrtos_tick, 8 sleeping tasks (O(1) check)   */
    BM_COUNT
};

volatile uint8_t bench_id;

#ifdef BENCH_TARGET
volatile uint16_t bench_min[BM_COUNT];   /* min cycles per metric     */
volatile uint8_t  bench_done;            /* debugger polls this       */
static uint16_t   bench_t0;

void __attribute__((noinline)) bench_mark_a(void)
{
    bench_t0 = TA1R;
    P1OUT |= BIT2;                        /* LA marker high            */
}
void __attribute__((noinline)) bench_mark_b(void)
{
    uint16_t d = (uint16_t)(TA1R - bench_t0);
    P1OUT &= ~BIT2;
    if (d < bench_min[bench_id])
        bench_min[bench_id] = d;
}

/* mrtos_tick() is the ISR body; calling it from preemptible task
 * context would race the real TA0 tick ISR. Measure it as it actually
 * runs: with interrupts masked. (Other metrics rely on min to reject
 * the occasional tick that lands mid-interval.) */
#define BENCH_TICK_GUARD_ENTER  uint16_t _k = port_irq_save()
#define BENCH_TICK_GUARD_EXIT   port_irq_restore(_k)
#else
void __attribute__((noinline)) bench_mark_a(void) { __asm__ volatile("nop"); }
void __attribute__((noinline)) bench_mark_b(void) { __asm__ volatile("nop"); }
#define BENCH_TICK_GUARD_ENTER  ((void)0)
#define BENCH_TICK_GUARD_EXIT   ((void)0)
#endif

#ifdef BENCH_TARGET
/* Busy-wait n TA1 counts (1 count = 125 ns at SMCLK = 8 MHz). */
static void bench_wait(uint16_t counts)
{
    uint16_t t = TA1R;
    while ((uint16_t)(TA1R - t) < counts) { }
}

/* Index marker emitted on P1.2 before each metric's measurement burst:
 * a long low gap, then `idx` short blips, then a short gap. On the logic
 * analyzer the blips read as `idx` narrow spikes (1 = baseline, 2 =
 * yield, ... 8 = tick_8), so each burst is self-identifying and the
 * back-to-back metrics no longer smear together. Markers only - never
 * touch bench_min. */
static void bench_sep(uint8_t idx)
{
    P1OUT &= ~BIT2;
    bench_wait(2000);                     /* ~250 us: clear gap from prev */
    for (uint8_t i = 0; i < idx; i++) {
        P1OUT |= BIT2;  bench_wait(40);   /* ~5 us blip */
        P1OUT &= ~BIT2; bench_wait(40);   /* ~5 us gap  */
    }
    bench_wait(400);                      /* ~50 us before the burst */
}
#else
#define bench_sep(idx) ((void)0)
#endif

static mrtos_tcb_t  t_ctl, t_peer, t_waiter, t_slp[N_SLEEPERS];
static port_stack_t s_ctl[STK_WORDS], s_peer[STK_WORDS], s_wtr[STK_WORDS],
                    s_slp[N_SLEEPERS][STK_WORDS];

static mrtos_sem_t   sem;
static mrtos_queue_t q;
static uint16_t      qstore[4];

/* Equal-priority peer: completes the yield round-trip. */
static void peer_task(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_b();                /* end of ctl's one-way switch */
        mrtos_yield();
    }
}

/* Higher-priority waiter: preempts the giver on each give. */
static void waiter_task(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < SAMPLES; i++) {
        mrtos_sem_take(&sem, MRTOS_FOREVER);
        bench_mark_b();                /* running again: hand-off done */
    }
}

static void sleeper_task(void *arg)
{
    (void)arg;
    mrtos_sleep(30000);                /* park in the delay list */
}

static void controller(void *arg)
{
    (void)arg;
    uint16_t v = 42;

    bench_id = BM_BASELINE;
    bench_sep(BM_BASELINE + 1);        /* 1 blip */
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_a();
        bench_mark_b();
    }

    bench_id = BM_YIELD;
    mrtos_task_create(&t_peer, "peer", peer_task, NULL, 6,
                      s_peer, STK_WORDS);
    bench_sep(BM_YIELD + 1);           /* 2 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_a();
        mrtos_yield();                 /* peer runs mark_b, yields back */
    }
    mrtos_sleep(1);                    /* let the peer retire */

    bench_id = BM_SEM_WAKE;
    mrtos_sem_init(&sem, 0, 1);
    mrtos_task_create(&t_waiter, "wtr", waiter_task, NULL, 7,
                      s_wtr, STK_WORDS);    /* preempts, blocks on sem */
    bench_sep(BM_SEM_WAKE + 1);        /* 3 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_a();
        mrtos_sem_give(&sem);          /* waiter preempts, marks b */
    }
    mrtos_sleep(1);

    bench_id = BM_QSEND;
    mrtos_queue_init(&q, qstore, sizeof(uint16_t), 4);
    bench_sep(BM_QSEND + 1);           /* 4 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_a();
        mrtos_queue_send(&q, &v, 0);
        bench_mark_b();
        mrtos_queue_recv(&q, &v, 0);   /* drain so the queue never fills */
    }
    bench_id = BM_QRECV;
    mrtos_queue_send(&q, &v, 0);
    bench_sep(BM_QRECV + 1);           /* 5 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        mrtos_queue_send(&q, &v, 0);
        bench_mark_a();
        mrtos_queue_recv(&q, &v, 0);
        bench_mark_b();
    }
    mrtos_queue_recv(&q, &v, 0);

    bench_id = BM_MUTEX;
    static mrtos_mutex_t mtx;
    mrtos_mutex_init(&mtx);
    bench_sep(BM_MUTEX + 1);           /* 6 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        bench_mark_a();
        mrtos_mutex_lock(&mtx, 0);
        mrtos_mutex_unlock(&mtx);
        bench_mark_b();
    }

    /* Tick cost with an empty delay list... */
    bench_id = BM_TICK0;
    bench_sep(BM_TICK0 + 1);           /* 7 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        BENCH_TICK_GUARD_ENTER;
        bench_mark_a();
        mrtos_tick();
        bench_mark_b();
        BENCH_TICK_GUARD_EXIT;
    }

    /* ...and with 8 sleepers parked: must not get more expensive
     * (delta list: only the head is ever decremented). */
    for (int i = 0; i < N_SLEEPERS; i++)
        mrtos_task_create(&t_slp[i], "slp", sleeper_task, NULL, 1,
                          s_slp[i], STK_WORDS);
    mrtos_sleep(1);                    /* let them enter the delay list */
    bench_id = BM_TICK8;
    bench_sep(BM_TICK8 + 1);           /* 8 blips */
    for (unsigned i = 0; i < SAMPLES; i++) {
        BENCH_TICK_GUARD_ENTER;
        bench_mark_a();
        mrtos_tick();
        bench_mark_b();
        BENCH_TICK_GUARD_EXIT;
    }

#ifdef BENCH_TARGET
    bench_done = 1;                    /* results ready for the debugger */
    for (;;)
        __bis_SR_register(LPM0_bits);
#else
    exit(0);
#endif
}

#ifdef BENCH_TARGET
static void bench_board_init(void)
{
    WDTCTL = WDTPW | WDTHOLD;

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

    /* DCO = MCLK = SMCLK = 8 MHz (0 FRAM wait states); ACLK = LFXT
     * 32768 crystal (the kernel tick source). LFXIN/LFXOUT = PJ.4/PJ.5. */
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

    /* TA1 free-running at SMCLK: 1 count = 1 CPU cycle (125 ns). */
    TA1CTL = TASSEL__SMCLK | MC__CONTINUOUS | TACLR;
}
#endif

int main(void)
{
#ifdef BENCH_TARGET
    bench_board_init();
    for (int i = 0; i < BM_COUNT; i++)
        bench_min[i] = 0xFFFFu;
#endif
    mrtos_init();
    mrtos_task_create(&t_ctl, "ctl", controller, NULL, 6,
                      s_ctl, STK_WORDS);
    mrtos_start();
    return 0;
}
