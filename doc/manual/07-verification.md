# Chapter 7 — Verification

How this kernel is tested, why each stage exists, what each stage can
and cannot prove, and the reasoning behind the individual test
scenarios. The executable counterpart of this chapter is
[doc/VALIDATION.md](../VALIDATION.md); the runner is
`tools/run_tests.py` (§6.7).

## 7.1 The three-stage strategy

| Stage | Executes on | Proves | Cannot prove |
|---|---|---|---|
| 1. Host (POSIX port) | native gcc, Linux | all kernel *logic*: scheduling order, object semantics, timeouts, races at kernel-call granularity | anything about the MSP430: ISA, codegen, context switch, interrupts |
| 2. ISA simulator (`msp430sim` port) | `msp430-elf-run` | the same logic compiled by **msp430-gcc at `-Os`** on a **16-bit machine**, plus a real `PUSHM.A/POPM.A` context switch | peripherals, interrupts, asynchronous preemption, electrical reality |
| 3. Target (FR5994 LaunchPad) | silicon | interrupt-driven preemption, Timer_A tick, ISR-safe API under load, LPM0, FRAM | — |

Stages 1–2 are fully automated (CTest; 6 + 5 binaries, 51 + 45
checks). Stage 3 is a manual checklist (§7.7) because it requires
hardware, instruments, and human observation.

The order matters: each stage is strictly cheaper to run and strictly
easier to debug than the next. A logic bug caught on host costs a
`printf`; the same bug on target costs a JTAG session.

## 7.2 The test framework (`test/test_common.h`)

Conventional unit-test frameworks assume *they* own `main()` and the
flow of control. Under an RTOS the scheduler owns control, so the
suite inverts the pattern:

- **A controller task at priority 7** (the maximum) runs the test
  script. Because nothing outranks it, it executes deterministically
  whenever it is ready, and *chooses* when to let workers run by
  blocking (`mrtos_sleep`) — every sleep is a deliberate "scheduling
  window".
- **Worker tasks log single characters** into a shared event buffer
  (`ev('a')`). Assertions then compare the buffer against the exact
  expected interleaving (`CHECK_SEQ(mark, "lhLH")`). A sequence check
  is much stronger than a counter: it verifies *order*, which is what
  a scheduler is for.
- `CHECK(cond, msg)` prints TAP-like `ok/FAIL` lines; the process
  exits 0 only if every check passed, so each binary plugs into CTest
  with no adapter.
- The framework needs only `printf` and `exit` — which is exactly what
  lets the identical source run under the POSIX port, the ISA
  simulator, and (in principle) a UART-equipped target.

Stack sizing is the one port-dependent knob: ucontext needs 64 KiB
stacks (the host libc runs on them), the simulator port needs 512 B.
`TEST_STK_WORDS` defaults to 8192 and a port may override it by
defining `PORT_TEST_STACK_WORDS` in its `port.h`.

## 7.3 Stage 1: host testing on the POSIX abstraction

### 7.3.1 How the POSIX port creates a deterministic RTOS

Each task is a `ucontext_t`; `port_yield` is `mrtos_sched_pick()` +
`swapcontext`. The crucial property is **where time comes from**:
`port_idle()` — the body of the idle task — calls `mrtos_tick()` once
per iteration. Therefore:

> **Ticks advance only when every other task is blocked.**

This makes the host port *cooperative* and the entire suite
**deterministic**: there is no preemption at arbitrary instructions,
context switches happen only at kernel-call boundaries, and a given
test program produces the identical event sequence on every run. Race
scenarios don't need to be "won" statistically — they are *staged*
move by move (§7.4.2).

Two consequences shape every test:

1. **Busy-waiting deadlocks the suite.** A spinning task never lets
   idle run, so time never advances and nothing sleeping ever wakes.
   All test tasks must block. The port converts a genuine deadlock
   (everything blocked, nothing runnable but idle, forever) into exit
   code 2 after 200 000 ticks, so CTest reports a failure instead of
   hanging.
2. **Asynchronous behavior is out of scope by construction** — that
   is stage 3's job (§7.7).

### 7.3.2 Unit suites — what each one pins down

**`test_unit_task`** — the scheduler's contract:
creation order follows priority (`bca` from creating prios 1,5,3);
returned tasks read `MRTOS_TASK_SUSPENDED`; priority 9 clamps to 7;
`sleep(10)` blocks 10–11 ticks; `mrtos_now()` is monotonic;
`sleep(0)` consumes no tick (pure yield); two equal-priority tasks
alternate strictly under `yield` (`cycycy`); `MRTOS_MS` arithmetic.

**`test_unit_sem`** — token conservation in every direction:
initial count consumed without blocking, then poll fails; 5 gives
against `limit=2` cap at exactly 2 retrievable tokens; a blocked taker
wakes on give *and the count stays 0* (direct hand-off, §1.9); timeout
returns −1 after exactly ~5 ticks *and the wait list is empty
afterwards* (cleanup); three waiters at prios 1/3/5 wake in order
`5,3,1` (priority order, not FIFO); two waiters at the same prio wake
in block order (FIFO within a level); a give after a waiter timed out
goes to the count, not into the void.

**`test_unit_mutex`** — the error matrix plus both PI paths:
lock/unlock round-trip with owner field checks; unlock-when-unowned
and recursive-lock both return `MRTOS_ERR_OWNER`; poll on an owned
mutex fails fast; a waiter timeout leaves the owner *owning* and the
wait list clean, sequence `oXO` proving the timeout fired before the
owner woke; PI on a **READY** owner (prio 1 boosted to 5, demoted at
unlock, hand-off sequence `oOh`); PI on a **BLOCKED** owner — the
owner is parked on a semaphore when boosted, exercising the wait-list
re-sort branch of `prio_raise()` that no other scenario reaches; three
waiters at prios 2/4/3 are handed the mutex in order `2(prio4), 3,
1` — sequence `oO231`.

**`test_unit_queue`** — data integrity:
poll on empty/full fails without disturbing contents; FIFO preserved
across ring wraparound (fill, half-drain, refill, verify 2,3,4,5);
copy-out semantics (clobber the source after `send`, receive the
original value); a multi-field struct item arrives bit-identical;
recv timeout ~3 ticks; a sender blocked on a full queue times out with
the queue still intact (`count==4`, original items unharmed); a
blocked sender completes when a drainer frees a slot, and the drainer
observes all 4 originals plus the late item, in order.

### 7.3.3 A bug the framework itself caught

The first integration run segfaulted: scenario 2a reused a helper's
TCB for the next scenario while the previous task was still alive
(preempted inside `sem_give`, still in the ready list).
`mrtos_task_create`'s `memset` shredded a live list node. The fix is a
2-tick grace sleep — and the incident is why the TCB-reuse rule is
documented prominently (§1.12, §5.8): the kernel genuinely does not
tolerate it, by design.

## 7.4 Stage 1b: integration suite design

`test_integration.c` — concurrency scenarios that cross object
boundaries. Each scenario is reproduced here with its reasoning,
because the *why* is not obvious from the assertions.

### 7.4.1 Producer/consumer stress (2×2 through a 4-slot queue)

Two producers (prio 3) push 20 tagged items each (`tag | seq`) through
a 4-slot queue drained by two consumers (prio 2); producers sleep
every 4th item to stir the interleaving. Assertions:

- exactly 40 items delivered (no loss, no duplication),
- per-producer sequence numbers strictly increasing across whichever
  consumer got them (FIFO survives contention),
- both streams complete, and the queue ends fully quiesced
  (`count==0`, both wait lists NULL).

This exercises the full blocking matrix — senders blocking on full,
receivers on empty, wakes in both directions, Mesa re-checks under
real contention — in one scenario.

### 7.4.2 Same-tick race: timeout vs. give (the "exactly one token" invariant)

What happens when a semaphore is given on *precisely* the tick a
waiter's timeout expires? The cooperative port lets us stage this
exactly: the controller takes with `timeout=5` while a helper sleeps 5
ticks and then gives. Both events land on tick N.

The kernel's documented resolution (§1.6): the tick handler processes
the expiry first, so the waiter sees `MRTOS_ERR_TIMEOUT`; the give
then finds no waiters and goes to the count. The assertion is the
*invariant*, not the specific outcome: **(taker got OK) XOR (count ==
1)** — one token, never zero, never two. A companion scenario gives
one tick *before* the deadline and requires the opposite outcome
(taker OK, count 0). Together they bracket the boundary.

### 7.4.3 Mesa wakeup steal

Validates §1.11's re-check loop end to end: a low-prio task blocks on
`recv`; the controller sends (waking it) but — being higher priority —
immediately polls the item away before the waiter runs. Assertions:
the waiter transparently re-blocks **inside the same `recv` call**
(state reads BLOCKED again), and when a second item arrives it
receives *that* one — having called `recv` exactly once. The internal
loop is invisible to the caller; this proves it.

### 7.4.4 Priority-inversion bounding

The classic Mars-Pathfinder scenario: low (prio 1) holds the mutex,
high (prio 5) blocks on it, mid (prio 3) is an unrelated periodic
interferer. Designing this on a cooperative port required care, and
the reasoning is instructive:

- While the owner *sleeps* holding the mutex, mid legitimately runs —
  that is not inversion (high is waiting on low's hold, not on mid).
- The discriminating moment is the **release tick**, where low and mid
  become ready simultaneously. *With* PI, low runs at prio 5, unlocks,
  and hands off to high before mid gets the CPU. *Without* PI, mid
  (3 > 1) would run first — unbounded inversion in miniature.

Assertions: low's effective priority reads 5 while high is blocked;
high's acquisition latency is bounded by low's hold time alone; and
exactly **three** `m` events precede `H` in the trace (the fourth `m`
of the release tick must come after). A fourth early `m` is precisely
the inversion PI exists to prevent, so this check fails on a kernel
with the boost removed.

## 7.5 Stage 2: why simulate, and what was investigated

### 7.5.1 What host testing structurally cannot see

The host is a 64-bit machine: `int` is 32 bits, pointers are 64. The
kernel does 16-bit delta arithmetic, packs bitmaps in `uint8_t`, and
relies on integer promotions behaving the same when `int` is 16 bits —
the exact class of bug that compiles cleanly on both and only
malfunctions on one. Plus: the host runs gcc-x86 codegen, not
msp430-gcc at `-Os`, and never executes a single MSP430 instruction.

### 7.5.2 Simulator survey (the investigation, with conclusions)

| Option | Verdict | Detail |
|---|---|---|
| **QEMU** | ✗ | No MSP430 target in mainline QEMU at all; only abandoned out-of-tree academic forks. |
| **Renode** | ✗ | No MSP430 support (ARM/RISC-V-centric). |
| **MSPSim** (Contiki) | ✗ | Emulates old F1xx/F2xx parts; no FR59xx, incomplete CPUX. |
| **mspdebug `sim`** | ✗ for this kernel | Has a Timer_A peripheral model (interesting!) but implements only the plain 16-bit core — it cannot execute the CPUX `PUSHM.A/POPM.A` instructions the context switch is built on. |
| **GNU simulator (`msp430-elf-run`)** | ✓ | Ships *inside* TI's msp430-gcc. Emulates the CPU/CPUX instruction set, provides `printf`/`exit` via the `-msim` syscall layer. **No peripherals, no interrupt injection.** Verified empirically to execute `PUSHM.A`/`POPM.A` correctly before committing to it. |

Conclusion: full-system emulation of the FR5994 (with Timer_A
interrupts driving the real port) is not available anywhere. But
ISA-level simulation is — and it covers precisely the gap stage 1
leaves (§7.5.1), if the kernel is given a port that doesn't need
interrupts.

### 7.5.3 The `msp430sim` port

A third port (§3.3.2), cooperative like POSIX, but whose context
switch is genuine CPUX assembly using the same register protocol as
the hardware port: `PUSHM.A #12, R15`, SP parked at TCB offset 0,
`POPM.A`, return into the new context. Running the suite there
executes, on the real ISA:

- every kernel path, compiled by msp430-elf-gcc at `-Os`, with 16-bit
  `int`/pointers — 45 checks, all passing;
- thousands of real 20-bit context save/restores across fabricated and
  live frames — validating the save-area layout and the SP-in-TCB
  protocol the hardware port shares.

What it deliberately does *not* validate: the hardware interrupt
frame (`PC[15:0]` / `SR|PC[19:16]` packing), `RETI`, Timer_A and the
CCR1 yield trick, LPM0 — those have no simulator to run on and remain
stage 3 items.

### 7.5.4 Simulator quirks worth knowing

1. **The stack-bound watchdog.** `msp430-elf-run` aborts
   ("STACK OVERFLOW, minimum SP value …") whenever SP drops below the
   program's heap break — and the break starts at `_end`, the top of
   `.bss`. An RTOS switching SP into static stack arrays violates this
   instantly. Workarounds investigated: overriding libsim's
   `sbrk`/`_sbrk_heap` (fragile — the bound tracks the break
   dynamically), linker-script surgery to place stacks above `_end`
   (the sim script computes `_end` after every RAM section, so an
   inserted section just moves `_end` up). The robust fix: the sim
   port carves task stacks from a **fixed pool at 0xA000–0xEFFF** —
   above any realistic break, below the startup stack at 0xFFC0 — and
   ignores the caller's arrays. The port asserts the pool stays clear
   of `_end` and of exhaustion at every allocation.
2. **CTest integration is free.** The toolchain file sets
   `CMAKE_CROSSCOMPILING_EMULATOR` to `msp430-elf-run`, and CTest
   transparently wraps every test binary — `ctest --test-dir
   build-sim` looks identical to a native run.
3. The original smoke test (`test_kernel.c`) hardcodes 64 KiB stacks
   and is excluded from the sim build; the five framework-based suites
   run unmodified.

## 7.6 Stage 3: on-target checklist

Automated stages end where interrupts begin. The manual checklist
(authoritative copy in [VALIDATION.md](../VALIDATION.md), T1–T8)
covers, per item: boot + tick liveness (LED1 at 1 Hz), tick accuracy
on a scope (±1 %), the ISR→semaphore path via S1 (T3), the deferred
yield under ISR load (T4), queue traffic under genuine time-slice
preemption observed through `cons_checksum` (T5), a one-hour soak
(T6), the stack-guard trap (T7), tickless LPM3 idle current (T8), and
the sleep-race / wake-accounting proof under a synthetic storm (T8b).

T4 deserves emphasis: it is the *only* line in the entire validation
program that exercises `mrtos_sem_give()` from a real ISR with the
pended-yield-after-`RETI` mechanism — the architectural keystone of
§1.7. Everything else about that mechanism is design review (SLAU367)
plus the race analysis comments in the port.

## 7.7 Coverage map — the honest summary

| Kernel behavior | Host | Sim | Target |
|---|:-:|:-:|:-:|
| Priority scheduling, RR, yield | ● | ● | ○ |
| Sleep / delta list / timeouts | ● | ● | ○ |
| Semaphore semantics incl. hand-off | ● | ● | ○ |
| Mutex + both PI paths + hand-off order | ● | ● | ○ |
| Queue FIFO/wrap/copy/Mesa | ● | ● | ○ |
| Same-tick race resolution | ● | ● | — |
| 16-bit int/pointer correctness, `-Os` codegen | — | ● | ○ |
| CPUX context save/restore protocol | — | ● | ○ |
| Hardware interrupt frame / `RETI` | — | — | **T1–T5** |
| Asynchronous preemption, ISR-safe API | — | — | **T3–T5** |
| Time-slice preemption of CPU-bound tasks | — | — | **T5** |
| Tick accuracy, LPM0, stack guard | — | — | **T2/T8/T7** |

● = verified by execution, automated. ○ = additionally exercised on
target by the demo app. — = not applicable / not covered there.

The two cooperative stages share one blind spot, stated once more
because it is the most important sentence in this chapter: **no
automated stage ever delivers an interrupt at an arbitrary instruction
boundary.** Until T1–T8 pass on silicon, the interrupt-driven half of
the port is design-reviewed, not tested.
