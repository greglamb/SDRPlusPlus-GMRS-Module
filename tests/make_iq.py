#!/usr/bin/env python3
"""Synthesise an IQ recording of the FRS/GMRS 462 MHz block for SDR++'s File
source: several radios keying up on different channels with DCS/CTCSS codes,
voice on top, a weak one, a code-less one, two overlapping. Writes a 16-bit
stereo WAV whose name carries the centre frequency the way SDR++ expects, plus
a truth table (time, channel, code) next to it.

    python3 tests/make_iq.py out_dir [--seconds 40] [--rate 250000]
"""
import argparse
import json
import os
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "oracle"))
import dcsdecode as dd  # noqa: E402

CENTER = 462637500.0
CHAN = {1: 462562500, 2: 462587500, 3: 462612500, 4: 462637500, 5: 462662500, 6: 462687500, 7: 462712500,
        15: 462550000, 16: 462575000, 17: 462600000, 18: 462625000, 19: 462650000, 20: 462675000, 21: 462700000, 22: 462725000}
rng = np.random.default_rng(3)


def voice(n, fs, f0lo, f0hi, level=0.8):
    t = np.arange(n) / fs
    f0 = f0lo + (f0hi - f0lo) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.6 * t + rng.uniform(0, 6)))
    ph = 2 * np.pi * np.cumsum(f0) / fs
    v = sum(np.sin(k * ph + rng.uniform(0, 6)) / k for k in range(1, 10))
    syl = (np.sin(2 * np.pi * 3.5 * t + rng.uniform(0, 6)) > -0.3).astype(float)
    v = v * syl
    sos = signal.butter(4, [300, 3000], btype="band", fs=fs, output="sos")
    v = signal.sosfilt(sos, v)
    return level * v / (np.max(np.abs(v)) + 1e-9)


def dcs_mod(n, fs, code, rate=134.4, inverted=False):
    bits = np.array(dd.dcs_word(code), dtype=float)
    if inverted:
        bits = 1 - bits
    idx = np.floor(np.arange(n) / fs * rate).astype(int) % 23
    nrz = 2 * bits[idx] - 1
    sos = signal.butter(2, 200, fs=fs, output="sos")
    return signal.sosfilt(sos, nrz)


def ctcss_mod(n, fs, hz):
    return np.sin(2 * np.pi * hz * np.arange(n) / fs)


def nfm(mod, fs, dev_hz, offset_hz, amp):
    """FM-modulate `mod` (deviation dev_hz at |mod|=1) onto offset_hz."""
    phase = 2 * np.pi * np.cumsum(dev_hz * mod) / fs
    t = np.arange(len(mod)) / fs
    return amp * np.exp(1j * (2 * np.pi * offset_hz * t + phase))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--rate", type=int, default=250000)
    args = ap.parse_args()
    fs = args.rate
    N = int(args.seconds * fs)
    iq = np.zeros(N, dtype=np.complex64)
    # noise floor
    iq += (rng.normal(size=N) + 1j * rng.normal(size=N)).astype(np.complex64) * 2e-3

    # (channel, t0, t1, kind, value, level_dbfs, voice (lo,hi) or None, inverted)
    plan = [
        (3, 2.0, 8.0, "DCS", 0o261, -20, (220, 380), False),     # kids' radio, child voice
        (3, 12.0, 15.0, "DCS", 0o261, -20, (220, 380), False),
        (17, 5.0, 11.0, "CTCSS", 141.3, -30, (90, 180), False),  # overlaps ch 3, male voice
        (22, 20.0, 26.0, "DCS", 0o023, -45, None, False),        # weak, no voice
        (1, 28.0, 32.0, None, None, -25, (90, 180), False),      # voice only, no code
        (19, 30.0, 38.0, "CTCSS", 100.0, -25, (220, 380), False),
        (7, 34.0, 36.0, "DCS", 0o732, -22, (90, 180), False),    # short 2 s key-down
    ]
    truth = []
    for ch, t0, t1, kind, val, level_db, vrange, inv in plan:
        n = int((t1 - t0) * fs)
        mod = np.zeros(n)
        if vrange:
            mod += voice(n, fs, *vrange)
        if kind == "DCS":
            mod += 0.15 * dcs_mod(n, fs, val, inverted=inv)
        elif kind == "CTCSS":
            mod += 0.15 * ctcss_mod(n, fs, val)
        # key-up/down ramps (10 ms) so the carrier edge is realistic
        ramp = np.ones(n); r = int(0.01 * fs)
        ramp[:r] = np.linspace(0, 1, r); ramp[-r:] = np.linspace(1, 0, r)
        amp = 10 ** (level_db / 20)
        off = CHAN[ch] - CENTER
        s = nfm(mod, fs, 2500.0, off, amp) * ramp
        i0 = int(t0 * fs)
        iq[i0:i0 + n] += s.astype(np.complex64)
        label = ("D%03oN" % (dd.dcs_inverse(val) if inv else val)) if kind == "DCS" else (f"{val:.1f} Hz" if kind == "CTCSS" else "")
        truth.append({"channel": ch, "t0": t0, "t1": t1, "label": label})

    os.makedirs(args.out_dir, exist_ok=True)
    name = f"baseband_{int(CENTER)}Hz_12-00-00_25-10-2026.wav"
    path = os.path.join(args.out_dir, name)
    pcm = np.stack([iq.real, iq.imag], axis=1)
    peak = np.max(np.abs(pcm))
    pcm = (pcm / max(peak, 1e-9) * 0.9 * 32767).astype(np.int16)
    wavfile.write(path, fs, pcm)
    with open(os.path.join(args.out_dir, "truth.json"), "w") as f:
        json.dump({"center_hz": CENTER, "rate": fs, "transmissions": truth}, f, indent=1)
    print(path)
    for t in truth:
        print(f"  ch {t['channel']:2d} {t['t0']:5.1f}-{t['t1']:5.1f}s  {t['label'] or '(no code)'}")


if __name__ == "__main__":
    main()
