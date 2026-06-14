/* MSP430FR5994 application configuration */
#ifndef MRTOS_CONFIG_H
#define MRTOS_CONFIG_H

/* 1024 Hz (= ACLK 32768 / 32, exact): the tick runs from the LFXT
 * crystal, so MRTOS_MS stays integer-exact and the time base inherits
 * the crystal's ~20 ppm accuracy instead of the DCO's ~%. */
#define MRTOS_CFG_TICK_HZ          1024u
#define MRTOS_CFG_SLICE_TICKS      10u
#define MRTOS_CFG_IDLE_STACK_WORDS 64u
#define MRTOS_CFG_STACK_CHECK      1

/* MUST match what board_init() programs into the clock system: the
 * port derives the tick timer reload from ACLK, so a mismatch skews
 * the kernel time base. Compile-time checks in the port enforce range
 * and exact divisibility, not agreement with the hardware. */
#define PORT_CFG_SMCLK_HZ          8000000UL
#define PORT_CFG_ACLK_HZ           32768UL
#define PORT_CFG_ISR_STACK_WORDS   96u

#endif
