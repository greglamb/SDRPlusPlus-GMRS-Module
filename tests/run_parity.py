#!/usr/bin/env python3
"""Parity test: the C++ streaming detector vs. the Python oracle (dcsdecode.py).

Builds a corpus of synthetic WAVs (every DCS code normal + some inverted, every
CTCSS tone, each under a loud voice; noise-only and voice-only negatives; a
mixed multi-code file; a short-key-down file), plus any real recordings in
tests/corpus/*.wav that have a matching .spans file. For each carrier span the
C++ binary must report the same label as the Python, and the Python must match
the truth.

    python3 tests/run_parity.py [--bin build-tools/parity_test] [--quick]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "oracle"))
import dcsdecode as dd  # noqa: E402
import test_dcsdecode as gen  # noqa: E402

FS = gen.FS
rng = np.random.default_rng(11)


def write(path, x):
    wavfile.write(path, FS, (np.clip(x, -1, 1) * 32767).astype(np.int16))


def python_labels(path, spans):
    """Run the oracle's classifier over the given spans (no key-down detection)."""
    fs, x = dd.load_wav(path)
    y = dd.to_subaudible(x, fs)
    out = []
    for t0, t1 in spans:
        tx = dd.classify_transmission(y, t0, t1, False)
        out.append(tx.label if tx.kind in ("DCS", "CTCSS") else "")
    return out


def cpp_labels(binary, path, spans, chunk=1024):
    with tempfile.NamedTemporaryFile("w", suffix=".spans", delete=False) as f:
        for t0, t1 in spans:
            f.write(f"{t0:.3f} {t1:.3f}\n")
        spath = f.name
    try:
        res = subprocess.run([binary, path, spath, "--chunk", str(chunk)], capture_output=True, text=True, check=True)
    finally:
        os.unlink(spath)
    rows = [json.loads(l) for l in res.stdout.splitlines() if l.strip()]
    return [r["label"] for r in rows], rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(HERE, "..", "build-tools", "parity_test"))
    ap.add_argument("--quick", action="store_true", help="subset of codes/tones")
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="frsmon_corpus_")
    cases = []  # (name, path, spans, truth_labels)

    # --- synthetic: DCS codes under voice (normal polarity) ---
    codes = list(dd.DCS_104) if not args.quick else [0o023, 0o131, 0o261, 0o445, 0o754]
    for code in codes:
        p = os.path.join(tmp, f"dcs_{code:03o}.wav")
        write(p, gen.dcs_signal(code, 4) + 1.0 * gen.harmonic_voice(4, 90, 180) + 0.01 * rng.normal(size=4 * FS))
        cases.append((f"DCS {code:03o} + male voice", p, [(0.0, 4.0)], [f"D{code:03o}N"]))
    # --- inverted polarity: expect the inverse code ---
    for code in (0o023, 0o261, 0o732, 0o754):
        p = os.path.join(tmp, f"dcsinv_{code:03o}.wav")
        write(p, gen.dcs_signal(code, 4, inverted=True) + gen.voice_like(4 * FS))
        cases.append((f"DCS {code:03o} inverted", p, [(0.0, 4.0)], [f"D{dd.dcs_inverse(code):03o}N"]))
    # --- off-nominal bit rate ---
    p = os.path.join(tmp, "dcs_rate.wav"); write(p, gen.dcs_signal(0o445, 4, rate=133.9) + gen.voice_like(4 * FS))
    cases.append(("DCS 445 at 133.9 bps", p, [(0.0, 4.0)], ["D445N"]))
    # --- CTCSS tones under voice ---
    tones = list(dd.CTCSS_38) if not args.quick else [67.0, 100.0, 136.5, 250.3]
    for hz in tones:
        p = os.path.join(tmp, f"ctcss_{hz}.wav")
        write(p, gen.ctcss_signal(hz, 4) + 1.0 * gen.harmonic_voice(4, 90, 180) + 0.01 * rng.normal(size=4 * FS))
        cases.append((f"CTCSS {hz} + male voice", p, [(0.0, 4.0)], [f"{hz:.1f} Hz"]))
    # --- negatives ---
    for i in range(3):
        p = os.path.join(tmp, f"noise_{i}.wav"); write(p, gen.open_squelch(4))
        cases.append((f"open squelch noise {i}", p, [(0.0, 4.0)], [""]))
        p = os.path.join(tmp, f"voice_m_{i}.wav"); write(p, gen.harmonic_voice(4, 90, 180) + 0.01 * rng.normal(size=4 * FS))
        cases.append((f"male voice only {i}", p, [(0.0, 4.0)], [""]))
        p = os.path.join(tmp, f"voice_c_{i}.wav"); write(p, gen.harmonic_voice(4, 220, 380) + 0.01 * rng.normal(size=4 * FS))
        cases.append((f"child voice only {i}", p, [(0.0, 4.0)], [""]))
    # --- mixed: two users on different codes, a code-less carrier, an inverted one ---
    parts = [gen.open_squelch(2.0),
             gen.dcs_signal(0o131, 5) + gen.voice_like(5 * FS),
             gen.open_squelch(1.5),
             gen.ctcss_signal(123.0, 4) + gen.voice_like(4 * FS),
             gen.open_squelch(1.0),
             gen.voice_like(3 * FS) + 0.002 * rng.normal(size=3 * FS),
             gen.open_squelch(2.0),
             gen.dcs_signal(0o131, 2.5, inverted=True) + gen.voice_like(int(2.5 * FS)),
             gen.open_squelch(1.0)]
    p = os.path.join(tmp, "mixed.wav"); write(p, np.concatenate(parts))
    cases.append(("mixed file", p, [(2.0, 7.0), (8.5, 12.5), (13.5, 16.5), (18.5, 21.0)],
                  ["D131N", "123.0 Hz", "", f"D{dd.dcs_inverse(0o131):03o}N"]))
    # --- short key-downs: 1.2 s DCS and 1.2 s CTCSS ---
    parts = [gen.open_squelch(1.0), gen.dcs_signal(0o054, 1.2) + gen.voice_like(int(1.2 * FS)), gen.open_squelch(1.0),
             gen.ctcss_signal(88.5, 1.2) + gen.voice_like(int(1.2 * FS)), gen.open_squelch(1.0)]
    p = os.path.join(tmp, "short.wav"); write(p, np.concatenate(parts))
    cases.append(("short key-downs", p, [(1.0, 2.2), (3.2, 4.4)], ["D054N", "88.5 Hz"]))

    # --- real recordings with .spans files ---
    corpus = os.path.join(HERE, "corpus")
    if os.path.isdir(corpus):
        for fn in sorted(os.listdir(corpus)):
            if fn.endswith(".wav") and os.path.exists(os.path.join(corpus, fn[:-4] + ".spans")):
                spans = [tuple(map(float, l.split())) for l in open(os.path.join(corpus, fn[:-4] + ".spans")) if l.strip()]
                cases.append((f"real: {fn}", os.path.join(corpus, fn), spans, None))

    fails = 0
    for name, path, spans, truth in cases:
        py = python_labels(path, spans)
        cpp, rows = cpp_labels(args.bin, path, spans)
        ok_py = truth is None or py == truth
        ok_cpp = cpp == (truth if truth is not None else py)
        if not (ok_py and ok_cpp):
            fails += 1
            print(f"FAIL {name}\n     truth {truth}\n     python {py}\n     c++    {cpp}")
        elif len(cases) <= 40 or truth is None:
            lat = [f"{r['t_confirmed']:.2f}" for r in rows]
            print(f"ok   {name}: {cpp}  confirmed at {lat} s")
    print(f"\n{len(cases)} cases, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
