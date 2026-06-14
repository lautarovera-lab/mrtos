/*
 * mRTOS demo - MSP430FR5994 (LaunchPad MSP-EXP430FR5994).
 *
 * Tasks:
 *   blink (p1): LED1 P1.0 toggles every 500 ms.
 *   ui    (p3): toggles LED2 P1.1 on each press of S1 (P5.6), delivered
 *               from the PORT5 ISR through a semaphore.
 *   prod  (p2): pushes a counter into a queue every 100 ms.
 *   cons  (p2): drains the queue; mirrors bit0 of the value on P1.1 is
 *               left to the UI, so it just accumulates into a checksum
 *               observable with a debugger.
 *
 * Board bring-up per TI app guidance: stop WDT first, unlock LOCKLPM5,
 * drive unused GPIO low, DCO at 8 MHz (0 FRAM wait states at <= 8 MHz;
 * NWAITS_1 would be mandatory above 8 MHz).
 */
#include <msp430.h>
#include "mrtos.h"

#define STK_WORDS 96u   /* 192 B: ISR save area is 52 B + task usage */

static mrtos_tcb_t  tcb_blink, tcb_ui, tcb_prod, tcb_cons;
static port_stack_t stk_blink[STK_WORDS], stk_ui[STK_WORDS],
                    stk_prod[STK_WORDS], stk_cons[STK_WORDS];

static mrtos_sem_t   btn_sem;
static mrtos_queue_t q;
static uint16_t      q_storage[8];

volatile uint32_t cons_checksum;   /* watch via debugger */

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
     * timer survives LPM3. LFXIN/LFXOUT = PJ.4/PJ.5. 0 FRAM wait states. */
    PJSEL0 |= BIT4 | BIT5;                  /* PJ.4/PJ.5 -> crystal pins */
    CSCTL0_H = CSKEY_H;                     /* unlock CS                 */
    CSCTL1   = DCOFSEL_3 | DCORSEL;
    CSCTL4  &= ~LFXTOFF;                    /* enable LFXT               */
    do {                                   /* wait out the crystal fault */
        CSCTL5 &= ~LFXTOFFG;
        SFRIFG1 &= ~OFIFG;
    } while (SFRIFG1 & OFIFG);
    CSCTL2   = SELA__LFXTCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3   = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;                           /* lock CS                   */

    /* S1 = P5.6: input, pull-up, falling edge interrupt. */
    P5DIR &= ~BIT6;
    P5REN |=  BIT6;
    P5OUT |=  BIT6;
    P5IES |=  BIT6;
    P5IFG &= ~BIT6;
    P5IE  |=  BIT6;
}

static void task_blink(void *arg)
{
    (void)arg;
    for (;;) {
        P1OUT ^= BIT0;
        mrtos_sleep(MRTOS_MS(500));
    }
}

static void task_ui(void *arg)
{
    (void)arg;
    for (;;) {
        mrtos_sem_take(&btn_sem, MRTOS_FOREVER);
        /* The ISR fires on falling edges - but RELEASING the button
         * bounces through falling edges too. Settle, then accept the
         * event only if the line still reads pressed: release bounces
         * wake us, read high, and are rejected. (Found on silicon:
         * fast press/release sequences double-toggled.) */
        mrtos_sleep(MRTOS_MS(5));
        if ((P5IN & BIT6) == 0)
            P1OUT ^= BIT1;
        mrtos_sleep(MRTOS_MS(25));             /* refractory window     */
        P5IFG &= ~BIT6;
        mrtos_sem_init(&btn_sem, 0, 1);        /* drop queued bounces   */
    }
}

static void task_prod(void *arg)
{
    (void)arg;
    uint16_t n = 0;
    for (;;) {
        mrtos_queue_send(&q, &n, MRTOS_FOREVER);
        n++;
        mrtos_sleep(MRTOS_MS(100));
    }
}

static void task_cons(void *arg)
{
    (void)arg;
    uint16_t v;
    for (;;) {
        mrtos_queue_recv(&q, &v, MRTOS_FOREVER);
        cons_checksum += v;
    }
}

int main(void)
{
    board_init();
    mrtos_init();

    mrtos_sem_init(&btn_sem, 0, 1);
    mrtos_queue_init(&q, q_storage, sizeof(uint16_t), 8);

    mrtos_task_create(&tcb_blink, "blink", task_blink, NULL, 1,
                      stk_blink, STK_WORDS);
    mrtos_task_create(&tcb_ui,    "ui",    task_ui,    NULL, 3,
                      stk_ui,    STK_WORDS);
    mrtos_task_create(&tcb_prod,  "prod",  task_prod,  NULL, 2,
                      stk_prod,  STK_WORDS);
    mrtos_task_create(&tcb_cons,  "cons",  task_cons,  NULL, 2,
                      stk_cons,  STK_WORDS);

    mrtos_start();                             /* never returns */
}

/* Application ISR: runs on the interrupted task's stack (keep it lean).
 * mrtos_sem_give() is ISR-safe; if it wakes a higher-priority task the
 * deferred yield fires right after this RETI. */
__attribute__((interrupt(PORT5_VECTOR)))
void port5_isr(void)
{
    if (P5IFG & BIT6) {
        P5IFG &= ~BIT6;
        mrtos_sem_give(&btn_sem);
    }
}
