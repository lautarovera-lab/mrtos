# Design review: ultra-low-power and LEA on mRTOS

**Status: design RFC** — analysis and proposed architecture; nothing in
this document is implemented yet except where marked *(done)*.

## 0. What SLAU840 actually says

The trigger for this review was SLAU840A ("MSP430 MCUs Development
Guide Book"), chapter 4.2, read as "TI strongly advises no-RTOS on
MSP430". The exact text, in the **TI-RTOS** bullet:

> *"TI-RTOS is a real-time operating system for TI microcontrollers
> […] The latest version for MSP430 is 2.20.00.06 (22 Jun 2016). Till
> now there is no plan for update. **We strongly advise you to use
> no-RTOS on MSP430.**"*

Context matters: the sentence lives inside the TI-RTOS paragraph and
follows directly from "abandoned since 2016, no updates planned" — it
is primarily *maintenance* advice about TI's own dead port, not a
microarchitectural verdict. FreeRTOS is listed immediately after,
neutrally, as "a market-leading RTOS". SLAU840 nowhere argues power or
LEA against RTOSes.

**However** — the engineering concerns behind the stronger reading are
real, and mRTOS as it stands today *does* fall to one of them. They
deserve the analysis regardless of what TI meant.

## 1. The three real objections, and where mRTOS stands

### 1.1 "A periodic tick destroys LPM3 residency" — TRUE today

The canonical MSP430 application sleeps in **LPM3** (~0.5–1 µA, ACLK
alive) or LPM4, waking only on real events. mRTOS today idles in
**LPM0** with a 1 kHz tick, because the tick timer runs from SMCLK,
which dies in LPM3. Cost breakdown (FR5994, 3 V — ballpark figures, to
be verified at the bench with EnergyTrace/ammeter):

| State | Current |
|---|---|
| LPM3 (superloop idle) | ~1 µA |
| LPM0 floor (DCO+SMCLK alive) | ~100 µA-class |
| + 1000 tick wakes/s (~45 insns + ISR frame each) | a few µA more |

So an idle mRTOS system today burns **~100× more** than an idle
superloop. The tick CPU work is noise; **the LPM0 floor is the
problem**. TI's implicit advice is correct against *this* design.

### 1.2 "Per-task stacks waste scarce SRAM" — partially true

A superloop needs one stack; N tasks need N. On a 4 KB-SRAM part this
is the second-strongest argument. mRTOS mitigations: stacks are
right-sizable from evidence (`mrtos_stack_unused()`, *(done)*; the
context-save floor is 26 words/task), the kernel itself adds only
~1.3 KB including the demo, and — an FRAM-family trick — *stacks of
low-rate tasks can live in FRAM* (slower, but FR5994 has 256 KB of
it). The honest residual cost is real but bounded: ~100–300 B per task
beyond what the same logic would consume as superloop state machines.

### 1.3 "LEA works better baremetal" — FALSE, with discipline

LEA is a vector-math coprocessor with its own 4 KB SRAM window; the
CPU writes a command block and LEA runs **without CPU intervention**,
raising an interrupt on completion. Analysis:

- The *waiting* pattern under mRTOS is strictly better than a
  superloop's: the calling task blocks on a semaphore that the
  LEA-done ISR gives (`mrtos_sem_give` is ISR-safe by design); the CPU
  either runs *other* tasks or sleeps. A superloop either busy-waits
  or sleeps doing nothing — it cannot overlap unrelated work with LEA
  compute. **RTOS wins on LEA utilization.**
- The hazards are resource-sharing ones, all solvable by convention:
  LEA is single-command — serialize access with a mutex; LEA operands
  must live in LEA RAM (linker-placed buffers — the static-allocation
  model fits naturally); **never place task stacks in the LEA RAM
  window** (the FR5994 maps it as usable SRAM when LEA is idle — do
  not be tempted);
- One genuine interaction with power management: LEA completes while
  the CPU sleeps in LPM0, but deeper modes gate the clocks it needs
  (exact floor to be confirmed in the datasheet at the bench). So the
  kernel must not enter LPM3 while LEA runs → §2.3.

## 2. Proposed architecture: best of both worlds

Three pieces, ordered by dependency. The goal stated precisely:
**CPU wakes if and only if there is work** — the superloop power
profile — while keeping preemptive priorities, blocking IPC and
bounded latencies.

### 2.1 Tickless **idle** (not tickless everything)

The tick is suppressed *only while the idle task runs*. While any real
task is runnable, the periodic tick continues unchanged — slicing,
timeouts and `mrtos_now()` behave exactly as today, and none of the
existing semantics or tests change. The pieces:

1. **The next deadline is already O(1)**: it is the delay list's head
   delta — the delta-list design pays off again. New kernel helper:
   `mrtos_next_deadline()` → head delta, or "none" if no task has a
   timeout pending. **(done — kernel half)**
2. **Timer moves to ACLK** (32.768 kHz LFXT crystal on the LaunchPad)
   so it survives LPM3. Natural tick becomes **1024 Hz** (`32768/32`),
   keeping `MRTOS_MS` integer-exact for powers of two; the 4d
   compile-time divisibility guard already enforces sanity. **(done —
   port half, increment 1: TA0 on ACLK, LFXT bring-up in `board_init`,
   verified on silicon — still LPM0 idle.)** The crystal also fixes the
   +0.66 % DCO offset seen in T2.
3. **`port_idle()` becomes**: compute `d = mrtos_next_deadline()`;
   program the compare register `d` ticks ahead (capped at the 16-bit
   horizon — re-arm on wrap, harmless); enter **LPM3**; on wake (timer
   *or any application ISR*), read elapsed ticks from the counter and
   fold them in one call. **(increment 2 done — plain LPM3, tick still
   periodic: measured 130 µA MCU baseline, down from 277 µA, ~2×. The
   per-tick wake — DCO restart + `mrtos_tick` ~1024×/s — dominates, so
   the periodic-tick LPM3 alone is not enough; suppressing the
   between-deadline ticks is increment 3, justified by this number.)**
4. **New kernel entry `mrtos_tick_advance(n)`**: subtract `n` from the
   head delta, pop everything that reaches zero, adjust `tick_count`
   by `n`. This is the only new kernel logic (~25 lines) and is fully
   testable on host and simulator by construction — feed it counts,
   assert wake order and `now()` coherence. **(done — kernel half,
   `test_unit_tickless`)**
5. **No deadline at all** (every task blocked `MRTOS_FOREVER`): arm
   nothing, sleep in LPM3/LPM4 until a peripheral ISR. This is the
   pure event-driven case — *zero* spontaneous wakes, indistinguishable
   from a superloop at the ammeter.

Expected result for an idle-dominated application: average current
drops from ~100 µA-class to **single-digit µA**, tick CPU overhead in
active phases unchanged (45 instructions, measured).

The race to prove on silicon (the classic one): an interrupt that
fires between "decided to sleep" and the LPM entry instruction must
not be lost — on MSP430 this is handled by entering LPM with a single
`BIS SR` whose GIE+CPUOFF bits apply atomically, but it must be
*demonstrated*, not assumed → new on-target checklist items.

### 2.2 SRAM strategy (no code change, documentation + app guidance)

- Budget stacks from `mrtos_stack_unused()` soak data, not guesses.
- Optionally place cold-task stacks in FRAM (`.persistent` section);
  keep hot-task stacks in SRAM. Document the latency trade.
- Never allocate stacks or kernel objects in the LEA RAM window.

### 2.3 Power locks (the LEA/driver veto, ~25 lines) **(done — kernel half)**

A counter-based cap, the embedded-standard pattern:

```c
void mrtos_pm_lock(uint8_t max_lpm);   /* e.g. LEA driver: LPM0 max */
void mrtos_pm_unlock(uint8_t max_lpm);
```

`port_idle()` sleeps at the *deepest mode no one has vetoed*. The LEA
driver pattern then becomes:

```c
mrtos_mutex_lock(&lea_mtx, MRTOS_FOREVER);   /* LEA is single-command */
mrtos_pm_lock(LPM0);                         /* LEA needs clocks      */
<write command block, start LEA>
mrtos_sem_take(&lea_done, timeout);          /* ISR gives on LEADONE  */
mrtos_pm_unlock(LPM0);
mrtos_mutex_unlock(&lea_mtx);
```

CPU sleeps (LPM0) or runs other tasks while LEA computes; deep sleep
resumes the moment the operation retires. This is the "robust LEA
usage" answer: serialized, power-correct, and overlap-capable — none
of which the superloop gives you simultaneously.

## 3. Benchmark plan: mRTOS-tickless vs no-RTOS

The fair fight TI's advice implies, on identical hardware with
identical function: a sensor app (sample every 1 s + button event +
UART burst), implemented twice — superloop+ISRs and mRTOS tickless.

| Metric | Instrument | Win condition for mRTOS |
|---|---|---|
| Average idle current | ammeter / EnergyTrace on 3V3 jumper | within ~10 % of superloop |
| Event → handler latency | logic analyzer, GPIO markers (8 MS/s is ample) | comparable; *bounded* under added load where superloop degrades |
| LEA throughput w/ concurrent UART | LA + counters | mRTOS ≥ superloop (overlap) |
| Code + RAM footprint | size reports (CI already tracks) | honest accounting incl. per-task stacks |

The third row is where the RTOS should *beat* baremetal, not just tie:
under concurrent load the superloop must interleave by hand, the
kernel overlaps by construction.

## 4. Sequencing

1. **2.1 tickless idle** — kernel half **done** (`mrtos_next_deadline`,
   `mrtos_tick_advance`, host+sim tested in `test_unit_tickless`); port
   half next (ACLK timer, LPM3 `port_idle`, on-target checklist items
   for the sleep race and wake accounting).
2. **2.3 power locks** — **done** (`mrtos_pm_lock/unlock/max_lpm`);
   consumed by the port half's `port_idle`.
3. **2.2** — documentation + soak-data stack table in the manual.
4. **§3 benchmark** — at the bench, after T1–T8 pass on the current
   LPM0 build (the comparison needs a working baseline anyway).

T8 of the validation checklist ("current consistent with LPM0 idle")
becomes obsolete on the day 2.1 merges and is replaced by an LPM3
residency criterion.
