#!/usr/bin/env python3
"""Checks the C pipeline (lib/ppg) against scipy and against tools/procesamiento.py.

Input is a recording made with the pipeline in CSV mode, either on the board
(tools/capture.py) or on the PC (tools/ppg_host.c):
    <base>.csv          one row per sample  (n, red, ir, red_ac, ... , finger)
    <base>_results.csv  one row per second  (n, finger, spo2, ..., r, pi, ...)

Three checks, per stretch with the finger on:
  1. Filters: recomputes the AC, the DC and the display trace (ir_view) with
     scipy.signal.sosfilt (same coefficients, same priming) and compares them with
     what the C code logged.
  2. R and SpO2: recomputes R over the same window the C code uses and compares it
     with the logged one; checks that SpO2 is SPO2_TABLE[round(R * 100)].
  3. Against procesamiento.py: R, SpO2 and heart rate of the offline script
     (zero-phase filtfilt over the whole stretch, FFT for the heart rate) next to the
     median of what the real-time code reported.

Parameters (sample rate, filters, window) are read from include/config.h.

Requirements: pip install numpy scipy pandas

Usage, from firmware/pulsox:
    python tools/validate_ppg.py <base> [<base> ...]
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.signal import butter, filtfilt, sosfilt, sosfilt_zi

ROOT = Path(__file__).resolve().parent.parent
AC_TOL = 0.01        # counts: the stream prints the AC with 3 decimals, in float
DC_TOL = 0.02        # counts: the DC (~1e5) is a float32 printed with 2 decimals
R_TOL = 5e-5
SKIP_S = 5.0         # offline comparison: ignore the start of the stretch (filtfilt edges, settling)


def define(text: str, name: str) -> float:
    m = re.search(rf"^#define\s+{name}\s+([0-9.]+)f?\b", text, re.MULTILINE)
    if not m:
        sys.exit(f"config.h: no encuentro {name}")
    return float(m.group(1))


class Params:
    def __init__(self):
        cfg = (ROOT / "include" / "config.h").read_text(encoding="utf-8")
        self.fs = define(cfg, "SENSOR_FS_HZ")
        order = int(define(cfg, "FILTER_ORDER"))
        low, high = define(cfg, "BAND_LOW_HZ"), define(cfg, "BAND_HIGH_HZ")
        self.bp = butter(order, [low, high], btype="band", fs=self.fs, output="sos")
        self.lp = butter(order, define(cfg, "DC_CUTOFF_HZ"), btype="low", fs=self.fs, output="sos")
        self.view = butter(int(define(cfg, "VIEW_FILTER_ORDER")),
                           [define(cfg, "VIEW_BAND_LOW_HZ"), define(cfg, "VIEW_BAND_HIGH_HZ")],
                           btype="band", fs=self.fs, output="sos")
        self.order, self.low, self.high = order, low, high
        self.dc_cut = define(cfg, "DC_CUTOFF_HZ")
        self.settle = int(define(cfg, "SETTLE_S") * self.fs)
        self.window = int(define(cfg, "SPO2_WINDOW_S") * self.fs)
        tbl = (ROOT / "lib" / "ppg" / "spo2_table.h").read_text(encoding="utf-8")
        body = re.search(r"SPO2_TABLE\[SPO2_TABLE_SIZE\]\s*=\s*\{(.*?)\};", tbl, re.S).group(1)
        self.table = np.array([int(v) for v in re.findall(r"\d+", body)])


def causal(sos, x):
    """sosfilt primed the way ppg_processor.c primes its filters."""
    return sosfilt(sos, x, zi=sosfilt_zi(sos) * x[0])[0]


def offline(p: Params, red, ir):
    """What tools/procesamiento.py computes for a stretch (sections 3 and 4 of the script)."""
    fs = p.fs
    b, a = butter(p.order, [p.low / (fs / 2), p.high / (fs / 2)], btype="band")
    bl, al = butter(p.order, p.dc_cut / (fs / 2), btype="low")
    ra, ia = filtfilt(b, a, red), filtfilt(b, a, ir)
    rd, idc = filtfilt(bl, al, red), filtfilt(bl, al, ir)
    r = (np.sqrt(np.mean(ra**2)) / np.mean(rd)) / (np.sqrt(np.mean(ia**2)) / np.mean(idc))
    spo2 = min(100.0, -45.060 * r**2 + 30.354 * r + 94.845)
    x = ir - ir.mean()
    mag, freq = np.abs(np.fft.rfft(x)), np.fft.rfftfreq(len(x), 1 / fs)
    band = (freq >= 0.8) & (freq <= 3.0)
    return r, spo2, freq[band][np.argmax(mag[band])] * 60


def stretches(finger):
    """(start, end) index pairs of the runs with the finger on."""
    f = np.concatenate(([0], np.asarray(finger, dtype=int), [0]))
    d = np.diff(f)
    return list(zip(np.where(d == 1)[0], np.where(d == -1)[0]))


def check(base: Path, p: Params) -> bool:
    d = pd.read_csv(base.with_name(base.name + ".csv"))
    r = pd.read_csv(base.with_name(base.name + "_results.csv"))
    print(f"\n=== {base.name}: {len(d)} muestras, {len(r)} resultados ===")
    runs = stretches(d["finger"])
    if not runs:
        print("  sin dedo en toda la captura")
        print(f"  filas con spo2_valid: {int(r['spo2_valid'].sum())} (se esperan 0)")
        return int(r["spo2_valid"].sum()) == 0

    ok = True
    pos = {int(n): i for i, n in enumerate(d["n"])}
    for s, e in runs:
        red, ir = d["red"].values[s:e].astype(float), d["ir"].values[s:e].astype(float)
        ref = {"red_ac": -causal(p.bp, red), "ir_ac": -causal(p.bp, ir),
               "red_dc": causal(p.lp, red), "ir_dc": causal(p.lp, ir),
               "ir_view": -causal(p.view, ir)}
        err_ac = max(np.abs(d[k].values[s:e] - ref[k]).max() for k in ("red_ac", "ir_ac", "ir_view"))
        err_dc = max(np.abs(d[k].values[s:e] - ref[k]).max() for k in ("red_dc", "ir_dc"))
        good = err_ac < AC_TOL and err_dc < DC_TOL
        ok &= good
        print(f"  dedo en muestras {s}-{e} ({(e - s) / p.fs:.0f} s)")
        print(f"    1. filtros vs scipy   : error AC {err_ac:.2e}, DC {err_dc:.2e} -> {'OK' if good else 'FALLA'}")

        # 2. R over the window the C code uses: the last `window` samples after the settling.
        rows = r[(r["n"].map(pos).between(s, e - 1)) & (r["r"] > 0)]
        worst_r, bad_table = 0.0, 0
        for _, row in rows.iterrows():
            i = pos[int(row["n"])] - s
            w = slice(i - p.window + 1, i + 1)
            rr = (np.sqrt(np.mean(ref["red_ac"][w] ** 2)) / np.mean(ref["red_dc"][w])) / \
                 (np.sqrt(np.mean(ref["ir_ac"][w] ** 2)) / np.mean(ref["ir_dc"][w]))
            worst_r = max(worst_r, abs(rr - row["r"]))
            if row["spo2_valid"] and row["spo2"] != p.table[int(round(row["r"] * 100 + 1e-9))]:
                bad_table += 1
        good = worst_r < R_TOL and bad_table == 0
        ok &= good
        print(f"    2. R y SpO2 vs scipy  : {len(rows)} lecturas, max |dR| {worst_r:.1e}, "
              f"SpO2 != tabla en {bad_table} -> {'OK' if good else 'FALLA'}")

        # 3. Offline script vs real time.
        a, b = p.settle + int(SKIP_S * p.fs), e - s
        if b - a < 10 * p.fs:
            print("    3. vs procesamiento.py: tramo demasiado corto")
            continue
        r_off, sp_off, hr_off = offline(p, red[a:b], ir[a:b])
        v = r[(r["n"].map(pos).between(s + a, e - 1)) & r["spo2_valid"].astype(bool)]
        vb = r[(r["n"].map(pos).between(s + a, e - 1)) & r["bpm_valid"].astype(bool)]
        if len(v):
            print(f"    3. vs procesamiento.py: R {r_off:.3f} -> tiempo real {v['r'].median():.3f} "
                  f"(min {v['r'].min():.3f}, max {v['r'].max():.3f}) | SpO2 {sp_off:.1f} -> "
                  f"{v['spo2'].median():.0f} (min {v['spo2'].min():.0f}, max {v['spo2'].max():.0f})")
        else:
            print(f"    3. vs procesamiento.py: R {r_off:.3f}, SpO2 {sp_off:.1f}; el tiempo real "
                  f"no dio ningun SpO2 valido en este tramo")
        hr_rt = f"{vb['bpm'].median():.1f} (min {vb['bpm'].min():.1f}, max {vb['bpm'].max():.1f})" \
            if len(vb) else "sin BPM valido"
        print(f"       frecuencia cardiaca: FFT {hr_off:.1f} -> tiempo real {hr_rt}")
        print(f"       validos: SpO2 {int(r['spo2_valid'].sum())}/{len(r)} s, BPM {int(r['bpm_valid'].sum())}/{len(r)} s")
    return ok


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base", nargs="+", type=Path, help="path without .csv / _results.csv")
    args = ap.parse_args()
    p = Params()
    dev = np.abs(p.table - np.round(-45.060 * (np.arange(len(p.table)) / 100) ** 2
                                    + 30.354 * (np.arange(len(p.table)) / 100) + 94.845)).max()
    print(f"tabla SpO2: {len(p.table)} valores, difiere de la cuadratica redondeada en {dev:.0f} como maximo")
    results = [check(b, p) for b in args.base]
    print("\nRESULTADO:", "todo OK" if all(results) else "HAY FALLAS")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
