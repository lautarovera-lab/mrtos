# mRTOS validation plan

Two stages: an automated host suite that anyone can run in minutes, and a
manual on-target checklist for the LaunchPad MSP-EXP430FR5994. A release
is "validated" when **all** automated checks pass and the on-target
checklist is green on real silicon.

## 1. Automated suite (host)

Prerequisites: gcc, cmake ≥ 3.20, [uv](https://docs.astral.sh/uv/).

```sh
uv run tools/run_tests.py            # host suite only
uv run tools/run_tests.py --sim      # + same suite on the MSP430 ISA (simulator)
uv run tools/run_tests.py --target   # + MSP430FR5994 cross-build
```

Toolchain for `--target` (public TI mirror, no registration):

```sh
tools/setup_toolchain.sh
```

With this layout (`~/toolchains/`), every tool in the repo
auto-detects the toolchain — no environment variables required. Only
for non-standard locations: export `MSP430_GCC_DIR` /
`MSP430_SUPPORT_DIR` (or `source tools/env.sh`).

### What the suite covers

| Binary | Scope |
|---|---|
| `test_unit_task` | creation/priority order, prio clamping, task retirement, sleep accuracy, `now()` monotonicity, `sleep(0)`≡yield, equal-prio yield alternation, `MRTOS_MS` |
| `test_unit_sem` | initial count, poll take, limit saturation, block/give, exact timeout, wake by priority, FIFO within a priority, timeout cleanup |
| `test_unit_mutex` | uncontended lock, `ERR_OWNER` paths (recursion, foreign unlock), poll fail, waiter timeout leaves owner intact, PI on READY owner, PI on BLOCKED owner (wait-list re-sort), multi-waiter hand-off in priority order |
| `test_unit_queue` | poll on empty/full, FIFO across wraparound, copy semantics, struct items, recv/send timeouts, blocked sender completion |
| `test_unit_stack` | stack painting, high-water marking (`mrtos_stack_unused`), overflow absence; on the simulator the marks are genuine MSP430 frame sizes |
| `test_integration` | 2-producer/2-consumer stress (no loss, no duplication, per-stream order), same-tick timeout-vs-give race (exactly-one-token invariant), Mesa wakeup steal + re-block, priority inversion bounded by inheritance |
| `test_kernel` | original 12-assertion smoke test |

Pass criterion: `ctest` reports 6/6; the runner exits 0.

### Simulator stage (`--sim`)

QEMU has no MSP430 support; the practical simulator is the GNU one
(`msp430-elf-run`), which ships inside TI's msp430-gcc. It emulates the
CPU/CPUX instruction set but **no peripherals or interrupts**, so the
fr59xx port can't run on it. Instead, `port/msp430sim/` is a cooperative
port (like POSIX) whose context switch is **real CPUX assembly**
(`PUSHM.A`/`POPM.A`, SP parked in the TCB — the same register protocol as
the fr59xx ISRs). Running the suite there validates what the host can't:

- the kernel compiled by msp430-gcc at `-Os` (16-bit `int`, 16-bit
  pointers, real integer promotion/overflow behavior);
- 20-bit register save/restore and stack-frame layout on the actual ISA.

Build quirk, documented in `port/msp430sim/port.c`: the simulator aborts
if SP ever drops below the heap break (which starts at `_end`), so task
stacks live in a fixed pool at 0xA000–0xEFFF rather than in the
caller-supplied `.bss` arrays.

### Micro-benchmark (`tools/bench.py`)

`bench/bench.c` brackets each kernel primitive between two empty marker
functions; `tools/bench.py` drives `msp430-elf-gdb`'s built-in
simulator to count single-stepped instructions per interval.
Deterministic to the instruction (ideal for CI regression tracking),
**not** cycle-accurate (1–6 cycles per MSP430 instruction, FRAM wait
states unmodeled). Reference values at `-Os`, msp430-gcc 9.3.1.11:

| Metric | Instructions |
|---|---|
| one-way context switch (yield) | ~52 |
| sem_give → waiter running (full hand-off) | ~132 |
| queue send / recv (poll) | ~77 |
| mutex lock + unlock (uncontended) | ~73 |
| tick, no sleepers | ~45 |
| tick, 8 sleepers | ~56 (O(1) delta list: not 8×) |

On target the same `bench/bench.c` becomes **cycle-exact** (below).

### On-target benchmark (cycle-exact)

`bench/bench.c` built with `-DBENCH_TARGET` (`bench_fr5994.elf`) turns
the markers into a free-running **TA1** read (SMCLK = 8 MHz, so 1 count
= 1 CPU cycle = 125 ns) plus a **P1.2** GPIO toggle. The minimum delta
per metric over 32 samples lands in `bench_min[]`, read out via the
debugger. Procedure at the bench:

```sh
make bench-target     # program + run; bench computes and parks
make bench-read       # attach, read bench_min[], print cycles + us
```

`bench-read` only *reads* memory (never resumes), so the tilib
reset-on-resume quirk leaves the parked results intact.

Two target-specific points: **min, not median** — on silicon the 1 kHz
tick ISR occasionally lands inside an interval and inflates that
sample; the minimum rejects it (the `mrtos_tick` metrics additionally
run with interrupts masked, since calling the ISR body from task
context would otherwise race the real tick). And **YIELD / SEM_WAKE
bracket a real preemptive context switch** here (the cooperative sim
cannot), so those numbers are the true hand-off cost.

Cross-check: a logic analyzer on **P1.2** sees one high pulse per
sample; its width should match the timer figure for that metric.

Results (`-Os`, msp430-gcc 9.3.1.11, MCLK = SMCLK = 8 MHz,
2026-06-13; baseline 17 cycles subtracted):

| Metric | Cycles | bench µs | LA µs | sim insns |
|---|---|---|---|---|
| one-way context switch (yield) | 202 | 25.25 | 27.67 | 52 |
| sem_give → waiter running | 367 | 45.88 | 48.42 | 132 |
| queue send (poll) | 158 | 19.75 | 22.12 | 78 |
| queue recv (poll) | 157 | 19.63 | 22.00 | 79 |
| mutex lock + unlock | 289 | 36.13 | 38.62 | 73 |
| tick, no sleepers | 118 | 14.75 | 17.08 | 45 |
| tick, 8 sleepers | 139 | 17.38 | 19.75 | 56 |

> Baseline (marker overhead, subtracted from every row): 17 cycles ≈ 2.13 µs; LA cross-check 2.25 µs.

**LA cross-check** (Saleae on P1.2, 24 MS/s, 2026-06-13): the logic
analyzer independently confirms the timer figures. Measured pulse
widths — baseline 2.25 µs (the marker overhead itself), **yield 29 µs**,
**sem_give→waiter 50 µs** — match `bench µs` to within a few µs. The LA
reads slightly high because the kernel records the **minimum** of 32
samples while the wire shows a *typical* pulse, and the 1 kHz tick ISR
(~15 µs) widens some samples (the spread you see across a burst — the
narrowest pulse lands on `bench µs`). Pulse *period* = ON + inter-sample
gap (e.g. yield 58.75 µs = 29 µs + ~30 µs for the peer to yield back).
Only yield and sem_wake are individually resolvable on the one-shot
signal: `q_send`/`q_recv`/`mutex`/`tick_0` run back-to-back with no gap
and smear into one mixed burst (the `—` rows). A per-metric replay mode
(one clean pulse each, gap-separated) would fill them — see git history
if enabled.

Reading these: cycles run ~2–4× the sim instruction counts — expected
(MSP430 ops take 1–6 cycles and FRAM adds wait states). YIELD and
SEM_WAKE carry a **real preemptive context switch** (the TA0CCR1
software-yield ISR), which the cooperative sim never exercises, so
their ratio is highest. MUTEX includes the unconditional re-evaluation
`port_yield()` in `mrtos_mutex_unlock()` — a full self-switch
round-trip even with no waiter. Most importantly, **tick, 8 sleepers
(139) is only +21 cycles over tick, no sleepers (118), not 8×** — the
delta-list O(1) tick, confirmed on silicon.

### Known coverage gap (by design)

Both the POSIX port and the simulator port are **cooperative**: ticks
advance only while idle runs, so asynchronous preemption (a tick landing
between two arbitrary instructions) never happens off-target. Races that
depend on it — ISR-driven `sem_give`, tick-vs-pended-yield ordering,
time-slice preemption of a CPU-bound task — are only exercised on target
(section 2, T4–T6). Also not simulated: Timer_A, LPM behavior, FRAM wait
states.

## 2. On-target checklist (MSP-EXP430FR5994)

Flash & run: `make run` (see `doc/DEBUG.md` for one-time setup;
`tools/check_board.sh` diagnoses the USB/probe chain step by step).

| # | Test | Procedure | Pass criterion |
|---|---|---|---|
| T1 | Boot + tick | Power the board | LED1 (P1.0) blinks steadily at 1 Hz (toggle every 500 ms; verify a few cycles against a watch) |
| T2 | Tick accuracy | Scope/logic analyzer on P1.0 | Period 1000 ms ± 1 %; jitter < 1 tick (1 ms) |
| T3 | ISR → semaphore | Press S1 (P5.6) repeatedly, vary cadence | LED2 (P1.1) toggles once per press; no missed/double toggles (30 ms debounce) |
| T4 | ISR-safe API under load | Hold S1 bursts while LED1 blinks | No hang, no blink disturbance — exercises `sem_give` from PORT5 ISR + deferred yield after RETI |
| T5 | Queue under preemption | Debugger: watch `cons_checksum` | Strictly increasing by n(n+1)/2 pattern (sum of 0,1,2,…); producer/consumer at equal prio time-slice correctly |
| T6 | Soak | Leave running ≥ 1 h with periodic S1 presses | Still responsive; LED1 still 1 Hz; `cons_checksum` still advancing |
| T7 | Stack guard | Test build: a task corrupts its own `stack_base[0]` then busy-spins; debugger breaks on the hook (see note) | Execution traps in `mrtos_stack_overflow_hook` with `t->name` identifying the task |
| T8 | Low power | **EnergyTrace**: `make energy DUR=30`, no button activity (restarts the target — run after T6); plot with `uv run tools/plot_energy.py` (LED-off phase = MCU baseline) | Average current consistent with LPM0 idle (CPU mostly asleep, wakes 1000×/s for tick); record the value — it is the baseline for the tickless comparison |

Procedure notes from the first bench run (2026-06-12):

- **The debug stub resets the chip** on first resume after (re)attach
  (tilib), so state poked from gdb before boot is wiped by
  re-initialization, and post-mortem state can't be read by attaching
  after the fact. Drive T7 with a self-corrupting test build plus a
  hardware breakpoint on the hook.
- **The guard check covers the *running* task only** (each tick checks
  `mrtos_cur`). Corrupting the guard of a mostly-sleeping task never
  triggers; the test task must stay busy after corrupting itself.
- **T8 uses EnergyTrace, not an ammeter.** The FR5994 LaunchPad's
  eZ-FET implements EnergyTrace++ — a software-controlled DC-DC
  converter whose charge pulses are *counted*, so the energy integral
  captures even sub-µs spikes that any sampling ammeter would miss;
  current is derived from energy over time (nA-class average
  resolution). `make energy` drives it from the CLI (energytrace-util
  + libmsp430). The ++ part (per-peripheral and LPM state tracing from
  inside the chip) is only exposed by CCS's GUI — worth a look when
  optimizing the tickless port, not needed for the T8 number.

### First bench results — 2026-06-12, msp430-gcc 9.3.1.11, LaunchPad rev ?

| # | Result | Measurement |
|---|---|---|
| T1 | **PASS** | LED1 1 Hz, verified against a clock |
| T2 | **PASS** | period 1.00661–1.00663 s (LA): +0.66 % offset = DCO tolerance (tick math exact); **jitter ≈ 20 µs/1 s ≈ 20 ppm, 50× under the 1-tick bound**; half-period 503.31–503.34 ms; unchanged under S1 storms |
| T3 | **PASS** | after the release-bounce fix: 1 press = 1 toggle at any cadence (first build double-toggled — release bounces; fixed in app) |
| T4 | **PASS** | aggressive S1 bursts: no hang, LED1 timing unaffected on the LA |
| T5 | **PASS** | `cons_checksum` = 1035 at `tick_count` = 4500 — exactly n(n+1)/2 for 46 sends at 100 ms; tick count exact to the tick |
| T6 | **PASS** | > 1 h running: LED1 steady, S1 responsive |
| T7 | **PASS** | hook hit with `t = tcb_blink` ("blink"), `guard[0]=0, guard[1]=0x5afe` observed in the debugger |
| T8 | **PASS** | EnergyTrace 30 s / 114k samples: **MCU baseline 277 µA** (LED-off phase) — consistent with LPM0 floor + 1 kHz tick. Whole-board avg 1806 µA is LED1-dominated (LED ≈ 2.9 mA when lit, 50 % duty). Profile archived: [doc/results/2026-06-13/t8-energy-profile.png](results/2026-06-13/t8-energy-profile.png) (raw trace gzipped alongside). **277 µA is the tickless-comparison baseline** |

Bonus data — stack high-water marks on silicon (96-word stacks, incl.
guards): blink 34, ui 36, prod 35, cons 41 words used. Consistent with
the simulator's context-save floor (26 words); ~55 words of headroom
everywhere.

## 3. Build configurations validated

| Configuration | Command | Status checked by |
|---|---|---|
| Host, Debug | `cmake -B build-host && ctest --test-dir build-host` | runner |
| Simulator, `-Os` | `cmake --toolchain cmake/msp430sim.cmake …` + CTest via `msp430-elf-run` | runner (`--sim`) |
| Target, `-Os` | `cmake --toolchain cmake/msp430fr5994.cmake …` | runner (`--target`), SRAM bound asserted |
| Legacy Makefiles | `make -f Makefile.host run` · `make GCC_DIR=…` | manual, kept for non-CMake consumers |
