# mRTOS - MSP430FR5994 target build.
# Requires TI's msp430-gcc toolchain + device support files:
#   https://www.ti.com/tool/MSP430-GCC-OPENSOURCE
# Point GCC_DIR / SUPPORT_DIR at your installation.

# Toolchain resolution, in priority order:
#   1. GCC_DIR/SUPPORT_DIR given on the command line
#   2. MSP430_GCC_DIR/MSP430_SUPPORT_DIR from the environment
#   3. newest install under ~/toolchains (the doc/VALIDATION.md layout)
#   4. /opt/ti/msp430-gcc
ifndef MSP430_GCC_DIR
MSP430_GCC_DIR := $(lastword $(sort $(wildcard $(HOME)/toolchains/msp430-gcc-*_linux64)))
endif
ifneq ($(MSP430_GCC_DIR),)
GCC_DIR     ?= $(MSP430_GCC_DIR)
endif
GCC_DIR     ?= /opt/ti/msp430-gcc
ifndef MSP430_SUPPORT_DIR
MSP430_SUPPORT_DIR := $(wildcard $(HOME)/toolchains/msp430-gcc-support-files/include)
endif
ifneq ($(MSP430_SUPPORT_DIR),)
SUPPORT_DIR ?= $(MSP430_SUPPORT_DIR)
endif
SUPPORT_DIR ?= $(GCC_DIR)/include

CC      = $(GCC_DIR)/bin/msp430-elf-gcc
MCU     = msp430fr5994

CFLAGS  = -mmcu=$(MCU) -mhwmult=f5series -Os -g -Wall -Wextra \
          -ffunction-sections -fdata-sections \
          -I$(SUPPORT_DIR) -Ikernel -Iport/msp430fr59xx -Iapp
# -mmcu only once on the combined compile+link line, or the device
# linker script is included twice.
LDFLAGS = -L$(SUPPORT_DIR) -Wl,--gc-sections

SRCS = kernel/mrtos.c port/msp430fr59xx/port.c app/main_fr5994.c

mrtos_fr5994.elf: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)
	$(GCC_DIR)/bin/msp430-elf-size $@

# Cycle-exact benchmark image: same bench/bench.c as the simulator,
# built for the FR5994 with the TA1/GPIO markers (-DBENCH_TARGET).
BENCH_SRCS = kernel/mrtos.c port/msp430fr59xx/port.c bench/bench.c

bench_fr5994.elf: $(BENCH_SRCS)
	$(CC) $(CFLAGS) -DBENCH_TARGET $(LDFLAGS) -o $@ $(BENCH_SRCS)
	$(GCC_DIR)/bin/msp430-elf-size $@

# Idle-dominated "sensor node" image: one task, 1 s real-time deadline,
# tickless LPM3 idle in between. The power/determinism demo (POWER.md S3).
IDLE_SRCS = kernel/mrtos.c port/msp430fr59xx/port.c app/main_idle_demo.c

idle_demo.elf: $(IDLE_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IDLE_SRCS)
	$(GCC_DIR)/bin/msp430-elf-size $@

clean:
	rm -f mrtos_fr5994.elf bench_fr5994.elf idle_demo.elf

# ------------------------------------------------------------------ #
# On-target run & debug (LaunchPad MSP-EXP430FR5994, onboard eZ-FET). #
# One-time setup: tools/setup_debug_tools.sh; full guide: doc/DEBUG.md #
# ------------------------------------------------------------------ #
MSPDEBUG_DIR ?= $(HOME)/toolchains/mspdebug
# TI debug stack (libmsp430.so): required for reliable FRAM writes on
# the FR5994 - mspdebug's builtin ezfet driver reads fine but its 2013
# chip database makes FRAM *writes* silently fail. tilib is the default
# whenever the stack is installed (sudo, see doc/DEBUG.md).
TI_DLL_DIR   ?= $(firstword $(wildcard /opt/ti/msp430-gcc-full/bin) $(wildcard $(HOME)/ti/msp430-gcc-full/bin))
ifneq ($(TI_DLL_DIR),)
MSPDEBUG_DRV ?= tilib
endif
MSPDEBUG_DRV ?= ezfet
MSPDEBUG     ?= LD_LIBRARY_PATH=$(TI_DLL_DIR):$(MSPDEBUG_DIR)/lib $(MSPDEBUG_DIR)/mspdebug
GDB          ?= MSP430_GCC_DIR=$(GCC_DIR) tools/msp430-gdb.sh
GDB_PORT     ?= 2000

# Attach the LaunchPad's eZ-FET to WSL2 (one command, no Windows
# terminal, no admin for the recurring case). No-op on native Linux or
# when already attached, so it is safe as a prerequisite. See
# tools/attach_board.sh and doc/DEBUG.md.
attach:
	@tools/attach_board.sh

# Program the board and leave it halted under the debugger's control.
flash: attach mrtos_fr5994.elf
	$(MSPDEBUG) $(MSPDEBUG_DRV) "prog mrtos_fr5994.elf"

# Program, reset and let it run free (mspdebug releases on exit).
run: attach mrtos_fr5994.elf
	$(MSPDEBUG) $(MSPDEBUG_DRV) "prog mrtos_fr5994.elf" "reset"

# Same, for the idle-dominated sensor-node demo.
idle-run: attach idle_demo.elf
	$(MSPDEBUG) $(MSPDEBUG_DRV) "prog idle_demo.elf" "reset"

# GDB server on :$(GDB_PORT). Programs first, then serves until ^C
# (gdb_loop keeps it alive across client reconnects).
gdbserver: attach mrtos_fr5994.elf
	$(MSPDEBUG) $(MSPDEBUG_DRV) "prog mrtos_fr5994.elf" \
	    "opt gdb_loop true" "gdb $(GDB_PORT)"

# CLI debug session against a running 'make gdbserver'.
gdb: mrtos_fr5994.elf
	$(GDB) mrtos_fr5994.elf -ex "target remote :$(GDB_PORT)"

# GUI debug: open VSCode here; F5 runs the preconfigured launch
# (.vscode/launch.json starts its own gdbserver task).
debug:
	code .
	@echo "VSCode launched - press F5 (Debug on LaunchPad)."

# EnergyTrace capture (T8): charge-pulse counting in the eZ-FET's
# DC-DC converter - integrates even sub-us spikes into the energy
# total by construction. NOTE: starting a capture restarts the target.
ENERGYTRACE ?= LD_LIBRARY_PATH=$(TI_DLL_DIR) $(HOME)/toolchains/energytrace/energytrace
DUR ?= 10
energy: attach
	$(ENERGYTRACE) $(DUR) | tee energy.csv | awk '!/^#/ { n++; i += $$2; e = $$4; t = $$1 } \
	    END { printf "samples=%d  avg_current=%.1f uA  energy=%.3f mJ  over %.2f s\n", \
	          n, i/n*1e6, e*1e3, t }'
	@echo "raw trace in energy.csv (time/current/voltage/energy)"

# ------------------------------------------------------------------ #
# On-target micro-benchmark (cycle-exact). See doc/VALIDATION.md.     #
#   make bench-target   program + run (computes, parks with results)  #
#   make bench-read     attach, read bench_min[], print cycles + us   #
# bench-read only READS memory (never resumes), so the tilib          #
# reset-on-resume quirk does not disturb the parked results.          #
# ------------------------------------------------------------------ #
BENCH_METRICS = baseline yield sem_wake q_send q_recv mutex tick_0 tick_8
BENCH_SRV_LOG = /tmp/mrtos-bench-srv.log

bench-target: attach bench_fr5994.elf
	$(MSPDEBUG) $(MSPDEBUG_DRV) "prog bench_fr5994.elf" "reset"

bench-read: attach bench_fr5994.elf
	@$(MSPDEBUG) $(MSPDEBUG_DRV) "gdb $(GDB_PORT)" >$(BENCH_SRV_LOG) 2>&1 & \
	  srv=$$!; \
	  while ! grep -q "Bound to port" $(BENCH_SRV_LOG) 2>/dev/null; do \
	      sleep 0.3; kill -0 $$srv 2>/dev/null || { cat $(BENCH_SRV_LOG); exit 1; }; \
	  done; \
	  $(GDB) bench_fr5994.elf -batch -ex "target remote :$(GDB_PORT)" \
	      -ex "print/d bench_done" -ex "print/d bench_min" 2>/dev/null \
	      | sed -n 's/.*{\([0-9, ]*\)}.*/\1/p' | tr ',' ' ' \
	      | awk -v metrics="$(BENCH_METRICS)" '{ \
	          n = split(metrics, m, " "); base = $$1; \
	          printf "%-10s %8s %8s %9s\n", "metric", "cycles", "net", "us"; \
	          for (i = 1; i <= NF; i++) { \
	              net = (i == 1) ? 0 : $$i - base; if (net < 0) net = 0; \
	              printf "%-10s %8d %8d %9.3f\n", m[i], $$i, net, net/8.0; \
	          } }'; \
	  kill $$srv 2>/dev/null; wait $$srv 2>/dev/null || true

.PHONY: clean attach flash run idle-run gdbserver gdb debug energy bench-target bench-read
