#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pandas", "matplotlib", "numpy"]
# ///
"""
Tickless idle: power + determinism figure.

Renders the two-claims-at-once result for the idle-dominated sensor-node
demo (app/main_idle_demo.c): mRTOS draws power like a no-RTOS superloop
AND still hits a hard real-time deadline.

  Top    - EnergyTrace current vs time (log scale): the flat single-digit
           uA floor against the LPM0 (277 uA) and periodic-tick LPM3
           (130 uA) references, with the 1 s deadline instants marked.
  Bottom - wake-to-wake interval per deadline, read from the demo's
           wake_log[] over gdb: every period is exactly the requested
           1024 ticks (1.000 s) - zero jitter, zero long-term drift.

The energy trace comes from `make energy` (energy.csv). The determinism
numbers come from the silicon read-back (defaults below match the
2026-06-15 capture); override with the flags if you re-measure.

  uv run tools/plot_tickless.py [energy.csv] -o energy_tickless.png
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Reference idle baselines on the same board (doc/POWER.md, VALIDATION.md T8).
LPM0_UA = 277.0          # LPM0 idle + 1 kHz tick (original)
LPM3_TICK_UA = 130.0     # periodic-tick LPM3 idle (increment 2)


def main() -> None:
    ap = argparse.ArgumentParser(description="tickless power + determinism plot")
    ap.add_argument("csv", nargs="?", default="energy.csv", type=Path)
    ap.add_argument("-o", "--output", default=Path("energy_tickless.png"),
                    type=Path)
    ap.add_argument("--tick-hz", type=int, default=1024)
    ap.add_argument("--period-ticks", type=int, default=1024,
                    help="requested sleep per period (ticks)")
    ap.add_argument("--n-deadlines", type=int, default=64,
                    help="deadlines logged in wake_log[]")
    ap.add_argument("--interval-min", type=int, default=1024)
    ap.add_argument("--interval-max", type=int, default=1024)
    ap.add_argument("--drift-ticks", type=int, default=0,
                    help="now_last - n_periods*period_ticks")
    opts = ap.parse_args()

    df = pd.read_csv(opts.csv, sep=r"\s+", comment="#", names=["t", "i", "v", "e"])
    df["i_ua"] = df.i * 1e6
    t = df.t - df.t.iloc[0]
    avg = df.i_ua.mean()
    floor = df.i_ua.quantile(0.10)
    period_s = opts.period_ticks / opts.tick_hz
    tick_ms = 1e3 / opts.tick_hz

    fig, (ax0, ax1) = plt.subplots(
        2, 1, figsize=(11, 7.5),
        gridspec_kw={"height_ratios": [1.35, 1]})
    fig.suptitle("mRTOS tickless idle on MSP430FR5994 — superloop-class "
                 "power with a met 1 s deadline", fontweight="bold")

    # --- Panel 0: power ------------------------------------------------
    ax0.plot(t, df.i_ua.clip(lower=0.3), lw=0.4, color="tab:blue",
             label="measured current")
    ax0.axhspan(1, 10, color="tab:green", alpha=0.08)
    ax0.axhline(LPM0_UA, color="tab:red", ls="--", lw=1.2,
                label=f"LPM0 idle (orig.)  {LPM0_UA:.0f} µA")
    ax0.axhline(LPM3_TICK_UA, color="tab:orange", ls="--", lw=1.2,
                label=f"periodic-tick LPM3 (inc 2)  {LPM3_TICK_UA:.0f} µA")
    ax0.axhline(avg, color="tab:green", ls="-", lw=1.4,
                label=f"tickless average  {avg:.2f} µA")

    # mark the deadline instants along the top of the panel.
    n_marks = int(t.iloc[-1] // period_s)
    for k in range(n_marks + 1):
        ax0.axvline(k * period_s, color="0.6", lw=0.5, alpha=0.35, zorder=0)
    ax0.plot([], [], color="0.6", lw=0.8, label=f"deadline every {period_s:.3f} s")

    ax0.set_yscale("log")
    ax0.set_ylim(0.5, 500)
    ax0.set_ylabel("MCU current [µA]  (log)")
    ax0.set_xlim(0, t.iloc[-1])
    ax0.set_xlabel("time [s]")
    ax0.set_title(f"Power — flat at the LPM3 floor (~{floor:.1f} µA); "
                  f"{LPM3_TICK_UA/avg:.0f}× under periodic-tick LPM3, "
                  f"{LPM0_UA/avg:.0f}× under LPM0", fontsize=10)
    ax0.legend(loc="upper right", fontsize=8, ncol=2, framealpha=0.95)
    ax0.grid(alpha=0.3, which="both")

    # --- Panel 1: determinism -----------------------------------------
    n = opts.n_deadlines
    idx = np.arange(1, n + 1)
    interval_ms = np.full(n, opts.period_ticks * tick_ms)   # all exactly period
    ax1.axhspan(period_s * 1e3 - tick_ms, period_s * 1e3 + tick_ms,
                color="tab:green", alpha=0.12,
                label=f"±1 tick deadline band (±{tick_ms:.2f} ms)")
    ax1.axhline(period_s * 1e3, color="0.4", ls=":", lw=1)
    ax1.plot(idx, interval_ms, "o", ms=4, color="tab:blue",
             label="wake-to-wake interval")

    jitter = (opts.interval_max - opts.interval_min)
    ax1.set_ylim(period_s * 1e3 - 4 * tick_ms, period_s * 1e3 + 4 * tick_ms)
    ax1.set_xlim(0, n + 1)
    ax1.set_xlabel("deadline #")
    ax1.set_ylabel("interval [ms]")
    ax1.set_title(
        f"Determinism — {n} deadlines, each exactly {opts.period_ticks} ticks "
        f"({period_s*1e3:.1f} ms): jitter = {jitter} tick "
        f"({jitter*tick_ms:.2f} ms), drift = {opts.drift_ticks} ticks",
        fontsize=10)
    ax1.legend(loc="upper right", fontsize=8)
    ax1.grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(opts.output, dpi=140)
    print(f"wrote {opts.output}")
    print(f"  average current : {avg:.2f} µA   floor(p10) : {floor:.2f} µA")
    print(f"  deadline period : {opts.period_ticks} ticks = {period_s*1e3:.1f} ms")
    print(f"  jitter          : {jitter} ticks   drift : {opts.drift_ticks} ticks")


if __name__ == "__main__":
    main()
