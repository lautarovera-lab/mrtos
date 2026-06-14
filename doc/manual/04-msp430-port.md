# Chapter 4 — The MSP430FR59xx Port

`port/msp430fr59xx/port.{h,c}` — the hardware port this kernel was
designed around, with the MSP-EXP430FR5994 LaunchPad as reference
board (`app/main_fr5994.c`).

## 4.1 Context layout

The FR59xx CPU is **CPUX**: registers are 20 bits wide (to address up
to 1 MB), even though the part is used here in the small (16-bit)
memory model. A task's saved context, from high to low addresses:

```
        ┌──────────────────────────────┐  higher addresses
        │ SR | PC[19:16]<<12           │  ┐ hardware interrupt frame,
        │ PC[15:0]                     │  ┘ pushed by the CPU (SLAU367)
        │ R15 (2 words)                │  ┐
        │ ...                          │  │ PUSHM.A #12, R15
        │ R4  (2 words)                │  ┘ 24 words, pushed by software
sp ───► └──────────────────────────────┘  lower addresses
```

- The hardware pushes `PC[15:0]` first, then a word combining the
  status register with the upper 4 PC bits (`PC[19:16]` in bits
  15:12). This packing is exactly per the family user's guide
  (SLAU367) and is what `RETI` expects.
- `PUSHM.A #12, R15` saves R15 down to R4 in one instruction, 2 words
  per register (`.A` = 20-bit width). Total software-saved context:
  48 bytes; with the 4-byte hardware frame, **52 bytes per context**.
- The saved SP is stored at **TCB offset 0** — the assembly does
  `mov.w r1, @r12`-style accesses with no compiler involvement, which
  is why `sp` must stay the first TCB member (§1.3).

`port_stack_init()` fabricates this exact frame for a new task:
synthetic SR with **GIE set** (the task starts with interrupts
enabled), entry address split per the packing rule, and the argument
pre-loaded into the saved **R12 slot** — R12 is the first-argument
register in the MSP430 EABI, so when the context is "restored" the
task shell finds its argument exactly as if it had been called.

## 4.2 The tick

Timer_A0, CCR0 compare, **up mode**, clocked from SMCLK/8:

- SMCLK = 8 MHz (DCO) ÷ 8 → 1 MHz timebase.
- 1 kHz tick → `CCR0 = 999`: the divide is exact, so the tick has no
  systematic drift — only crystal/DCO tolerance applies.
- The CCR0 interrupt (`TIMER0_A0_VECTOR`) runs the context-saving
  prologue, calls `mrtos_tick()`, and resumes whichever task
  `mrtos_cur` then points to.

Changing `MRTOS_CFG_TICK_HZ` means recomputing CCR0 in the port —
the kernel only assumes "something calls `mrtos_tick()` at
`MRTOS_CFG_TICK_HZ`".

## 4.3 Yield: a synthesized PendSV

The MSP430 has no dedicated software-pendable context-switch interrupt
(no Cortex-M PendSV). The port synthesizes one from spare timer
hardware:

- `TA0CCR1` is parked at `0xFFFF`. In up mode the counter never
  reaches it (it resets at CCR0 = 999), so CCR1's interrupt flag can
  **only** be set by software.
- `port_yield()` is therefore one line: set `TA0CCTL1.CCIFG`. The
  interrupt (`TIMER0_A1_VECTOR`) performs the actual switch.

This single mechanism produces the right behavior in all three calling
contexts, with no context-detection code:

| Caller context | What happens |
|---|---|
| Task, interrupts enabled | The interrupt fires immediately — effectively a synchronous switch. |
| Task, inside a critical section | The flag pends; the switch happens the moment `port_irq_restore()` re-enables GIE. The kernel's "yield inside the lock" pattern is safe by construction. |
| Any application ISR | The flag pends; the switch fires right after the outer `RETI`. This is the deferred yield that makes `mrtos_sem_give()` ISR-safe (§1.7). |

A benign race is documented in the port: if the tick fires while a
yield is pended, the tick's own reschedule may make the pended yield
redundant — it then performs a second pick that selects the same task
and resumes. Wasted cycles, no incorrectness.

## 4.4 The two kernel ISRs and the dedicated ISR stack

Both kernel ISRs (tick and yield) are `naked` and follow the same
skeleton:

1. `PUSHM.A #12, R15` — complete the context on the *task's* stack;
2. store SP into `mrtos_cur->sp`;
3. **switch SP to a dedicated ISR stack**;
4. call the C handler (`mrtos_tick()` / pick) — which may retarget
   `mrtos_cur`;
5. reload SP from the (possibly different) `mrtos_cur->sp`;
6. `POPM.A #12, R15`, `RETI`.

The dedicated ISR stack means the C handler's own frame usage (locals,
nested calls) does **not** have to fit in every task's stack — each
task only ever hosts the fixed 52-byte save area. That makes per-task
stack sizing tractable (§4.6).

The `naked` + `__attribute__((interrupt(VECTOR)))` combination is
toolchain-sensitive; it compiles cleanly on msp430-elf-gcc 9.3.1
(verified by the build pipeline, §6.4). The port carries a comment describing the fallback
(move the ISR bodies to a `.S` file and place vectors via
`.section __interrupt_vector_<n>`) should a future toolchain reject it.

## 4.5 Idle and low power (tickless LPM3)

The tick runs from **ACLK (LFXT 32.768 kHz)**, which survives LPM3, at
**1024 Hz** (`32768/32`, 32 counts per tick). `port_idle()` sleeps the
CPU straight to the next pending deadline instead of waking every tick:

1. Read `d = mrtos_next_deadline()` (ticks to the earliest wake; 0 = no
   timeout pending) and the depth cap `mrtos_pm_max_lpm()`.
2. Program `TA0CCR0 = 32·d − 1` **without** `TACLR` — the live counter
   *is* the sub-tick phase, so counting forward to the new limit keeps
   the clock phase-exact. `d` is capped at `0xFFFF/32 = 2047` ticks so
   `TA0CCR0` stays below the parked CCR1 software-yield channel
   (`0xFFFF`); a farther deadline (or no deadline) just re-arms on the
   capped wake. `d = 0` sleeps the cap and waits for a peripheral IRQ.
3. Enter LPM atomically: a single `BIS #(LPMbits|GIE), SR` applies the
   LPM and GIE bits together, so an interrupt that fires between the arm
   and the sleep is taken only *after* the CPU is asleep — no lost wake.
   The depth comes from the pm cap (LPM3 default; never LPM4 — it stops
   ACLK and would freeze the tick).

**Wake reconciliation happens in the timer ISRs**, because an early
peripheral wake switches context to the woken task in-ISR before any
post-sleep idle code could run:

- **Planned wake** (CCR0 reached its limit): the tick handler folds the
  full armed span via `mrtos_tick_advance(span)` and restores the
  periodic `TA0CCR0`. The counter wrapped to a tick boundary, so phase
  is already 0.
- **Early wake** (a peripheral readied a task and pended the CCR1
  yield): the yield handler unwinds from the live counter — whole ticks
  folded, the sub-tick remainder accumulated in a static `phase_carry`
  so the time base cannot creep slow across many short idle entries.

Unlike the old LPM0 idle, idle's saved SR carries `CPUOFF`, so to make
its loop **re-evaluate** the deadline each sleep the timer ISRs clear
the LPM bits in idle's saved frame on any return-to-idle (identified by
a captured idle-TCB pointer). Idle then resumes active, re-reads the
deadline and pm cap, and re-arms — no re-sleeping on a stale SR.

Measured idle floor: **single-digit µA** (≈ 1–3 µA, the LPM3 hardware
floor), down from ~46 µA for a periodic-tick LPM3 idle. See VALIDATION.md
T8/T8b for the on-silicon A/B and the sleep-race proof.

## 4.6 Stack budgeting on this port

Per task, you must budget:

- the task's own worst-case call depth, plus
- **52 bytes** of kernel context save area, plus
- the worst-case frame of any **application ISR** that may interrupt
  the task — application ISRs run on the interrupted task's stack
  (only the two kernel ISRs hop to the ISR stack).

The demo uses 96 words (192 bytes) per task, which fits "blink an LED
and pass words through a queue" with margin. Keep application ISRs
lean, or grow every task stack by their cost.

## 4.7 FR5994 board bring-up (`app/main_fr5994.c`)

The demo performs the family-correct initialization sequence, in
order, each step load-bearing:

1. **Stop the watchdog** (`WDTCTL = WDTPW | WDTHOLD`) — first
   statement; the WDT is live out of reset.
2. **Drive all GPIO low as outputs** — LPM hygiene: floating inputs
   burn current.
3. **Unlock LOCKLPM5** (`PM5CTL0 &= ~LOCKLPM5`) — mandatory on FRAM
   parts: GPIO stays high-impedance after reset until this bit clears.
   Forgetting it is the classic "my code runs but no pin moves" on
   this family.
4. **Clock setup**: DCO 8 MHz (`CSCTL1 = DCOFSEL_3 | DCORSEL`),
   MCLK = SMCLK = DCO, ACLK = VLO, all dividers 1. The CS registers
   are password-protected (`CSCTL0_H = CSKEY_H` … `= 0`).
5. **FRAM wait states**: at ≤ 8 MHz, zero wait states are legal —
   which is why the demo stops at 8 MHz. **Above 8 MHz you must set
   `FRCTL0 = FRCTLPW | NWAITS_1` *before* raising the clock** or the
   part reads garbage from FRAM. This is a hardware constraint, not a
   kernel one (§5.5).
6. **Button S1** = P5.6: input, pull-up, falling-edge interrupt.

Demo task set: `blink` (P1.0, 500 ms, prio 1), `ui` (toggles P1.1 per
S1 press via semaphore from the PORT5 ISR, prio 3, with a 30 ms
debounce window and a semaphore re-init to drop bounces), `prod`/`cons`
(queue pair at prio 2, consumer accumulates `cons_checksum` — a
debugger-visible liveness counter). The PORT5 ISR demonstrates the
intended ISR pattern: clear the flag, `mrtos_sem_give()`, done.

## 4.8 Verified vs. hand-checked, honestly

| Claim | Status |
|---|---|
| Kernel logic (scheduling, objects, timeouts) | executed: host suite + ISA simulator (ch. 7) |
| `PUSHM.A`/`POPM.A` save/restore protocol, SP-in-TCB | executed in the ISA simulator via the structurally identical `msp430sim` switch |
| Compiles/links at `-Os` on msp430-elf-gcc 9.3.1, incl. naked ISRs | verified (build runs in CI path) |
| Hardware interrupt frame fabrication, RETI path, Timer_A config, LPM3 tickless idle + wake reconciliation, PORT5 ISR | **hand-checked against SLAU367 only — requires silicon** (checklist in §7.6 / VALIDATION.md) |
