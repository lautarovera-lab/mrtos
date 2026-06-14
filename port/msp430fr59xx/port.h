/*
 * mRTOS port: MSP430FR59xx family (CPUX), msp430-elf-gcc, SMALL memory model.
 *
 * Port strategy
 * -------------
 * - Context = R4..R15 saved as full 20-bit registers (PUSHM.A/POPM.A) on the
 *   task stack, below the hardware interrupt frame (SR+PC packed per SLAU367:
 *   PC[15:0] pushed first, then SR with PC[19:16] in bits 15:12).
 * - Tick: Timer_A0 CCR0 (TIMER0_A0_VECTOR), up mode, ACLK (LFXT 32768).
 *   ACLK keeps running in LPM3, which is what makes tickless idle work.
 * - Yield: software-pended interrupt on TA0 CCR1 (TIMER0_A1_VECTOR).
 *   TA0CCR1 is parked at 0xFFFF, unreachable in up mode, so its CCIFG can
 *   only be set by software -> behaves like Cortex-M PendSV.
 * - ISR C handlers run on a dedicated ISR stack; each task stack only pays
 *   the 52-byte save area (4 frame + 48 context).
 *
 * Assumptions (checked where possible):
 * - Small code+data model (default -mmcu=...): pointers/PCs fit in 16 bits;
 *   code below 0x10000. Large-model support would change mov.w->mov[x].a
 *   and 'call'->'calla' in port.c.
 * - TI msp430-gcc support files installed (msp430.h, in430 intrinsics,
 *   device linker script).
 */
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stddef.h>

typedef uint16_t port_stack_t;

uint16_t      port_irq_save(void);
void          port_irq_restore(uint16_t key);
void          port_yield(void);
port_stack_t *port_stack_init(port_stack_t *base, size_t words,
                              void (*shell)(void *), void *arg);
void          port_start(void) __attribute__((noreturn));
void          port_idle(void);

/* ------------------------------------------------------------------ */
/* Port configuration defaults (override in mrtos_config.h)             */
/* ------------------------------------------------------------------ */
#ifndef PORT_CFG_SMCLK_HZ
#define PORT_CFG_SMCLK_HZ        8000000UL    /* CPU/peripheral clock (FYI) */
#endif
#ifndef PORT_CFG_ACLK_HZ
#define PORT_CFG_ACLK_HZ         32768UL      /* LFXT crystal; tick source  */
#endif
#ifndef PORT_CFG_ISR_STACK_WORDS
#define PORT_CFG_ISR_STACK_WORDS 96u
#endif

/* The tick timer (TA0) runs from ACLK so it survives LPM3 (tickless
 * idle). 1 ACLK count = 1/PORT_CFG_ACLK_HZ s; board_init() must source
 * ACLK from a stable PORT_CFG_ACLK_HZ oscillator (LFXT on the LaunchPad). */
#define PORT_TICK_CLK_HZ   PORT_CFG_ACLK_HZ

#endif /* PORT_H */
