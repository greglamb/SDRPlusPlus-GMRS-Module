"""Synthetic end-to-end checks. Run: python3 test_dcsdecode.py"""
import os
import tempfile

import numpy as np
from scipy import signal
from scipy.io import wavfile

import dcsdecode as dd

FS = 48000
rng = np.random.default_rng(1)


def voice_like(n):
    # band-limited noise bursts, roughly speech-shaped
    v = rng.normal(size=n)
    sos = signal.butter(4, [300, 2500], btype="band", fs=FS, output="sos")
    v = signal.sosfilt(sos, v)
    env = (np.sin(2 * np.pi * 3 * np.arange(n) / FS) > 0.3).astype(float)
    return 0.3 * v * env


def dcs_signal(code, seconds, rate=134.4, inverted=False, level=0.08):
    bits = np.array(dd.dcs_word(code), dtype=float)
    if inverted:
        bits = 1 - bits
    n = int(seconds * FS)
    idx = np.floor(np.arange(n) / FS * rate).astype(int) % 23
    nrz = 2 * bits[idx] - 1
    sos = signal.butter(2, 180, fs=FS, output="sos")   # transmitter LPF
    return level * signal.sosfilt(sos, nrz)


def ctcss_signal(hz, seconds, level=0.08):
    t = np.arange(int(seconds * FS)) / FS
    return level * np.sin(2 * np.pi * hz * t)


def open_squelch(seconds, level=0.5):
    # FM-demod noise rises with frequency: differentiated white noise
    return level * np.diff(rng.normal(size=int(seconds * FS) + 1))


def harmonic_voice(seconds, f0lo, f0hi, level=0.3):
    """Crude voiced speech: wandering fundamental + harmonics, syllable AM."""
    t = np.arange(int(seconds * FS)) / FS
    f0 = f0lo + (f0hi - f0lo) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t + rng.uniform(0, 6)))
    ph = 2 * np.pi * np.cumsum(f0) / FS
    v = sum(np.sin(k * ph + rng.uniform(0, 6)) / k for k in range(1, 12))
    syl = (np.sin(2 * np.pi * 4 * t + rng.uniform(0, 6)) > -0.2).astype(float)
    return level * v * syl


def write(path, x):
    wavfile.write(path, FS, (np.clip(x, -1, 1) * 32767).astype(np.int16))


def top(path, **kw):
    """(kind, label) of the dominant code in the file, plus the transmissions."""
    _, _, txs = dd.analyze(path, invert=kw.get("invert", False))
    s = dd.roll_up(txs)
    return s[0][0] if s else ("none", ""), txs


def main():
    tmp = tempfile.mkdtemp()
    fails = 0

    def check(name, got, want):
        nonlocal fails
        ok = got == want
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}: got {got}, want {want}")

    # 1. pure open-squelch noise -> nothing
    p = os.path.join(tmp, "noise.wav"); write(p, open_squelch(8))
    check("noise only", top(p)[0], ("none", ""))

    # 2. every one of the 104 DCS codes, normal polarity, with voice on top
    bad = []
    for code in dd.DCS_104:
        p = os.path.join(tmp, f"d{code:03o}.wav")
        x = dcs_signal(code, 4) + voice_like(4 * FS) + 0.02 * rng.normal(size=4 * FS)
        write(p, x)
        got, _ = top(p)
        if got != ("DCS", f"D{code:03o}N"):
            bad.append((f"{code:03o}", got))
    check("all 104 DCS codes (normal)", bad, [])

    # 3. inverted polarity on a few codes -> reports the inverse code
    bad = []
    for code in (0o023, 0o261, 0o732, 0o754):
        p = os.path.join(tmp, f"i{code:03o}.wav")
        write(p, dcs_signal(code, 4, inverted=True) + voice_like(4 * FS))
        got, _ = top(p)
        want = ("DCS", f"D{dd.dcs_inverse(code):03o}N")
        if got != want:
            bad.append((f"{code:03o}", got, want))
        got_inv, _ = top(p, invert=True)
        if got_inv != ("DCS", f"D{code:03o}N"):
            bad.append((f"{code:03o} --invert", got_inv))
    check("inverted DCS + --invert flag", bad, [])

    # 4. off-nominal bit rate (cheap crystal) still decodes
    p = os.path.join(tmp, "rate.wav"); write(p, dcs_signal(0o445, 4, rate=133.9) + voice_like(4 * FS))
    check("DCS at 133.9 bps", top(p)[0], ("DCS", "D445N"))

    # 5. every CTCSS tone, with voice
    bad = []
    for hz in dd.CTCSS_38:
        p = os.path.join(tmp, f"c{hz}.wav")
        write(p, ctcss_signal(hz, 4) + voice_like(4 * FS) + 0.02 * rng.normal(size=4 * FS))
        got, _ = top(p)
        if got != ("CTCSS", f"{hz:.1f} Hz"):
            bad.append((hz, got))
    check("all 38 CTCSS tones", bad, [])

    # 6. two users on different codes + one with no code, separated by open squelch:
    #    expect four key-downs with the right labels and timestamps
    parts = [open_squelch(2.0),
             dcs_signal(0o131, 5) + voice_like(5 * FS),            # 2.0 - 7.0
             open_squelch(1.5),
             ctcss_signal(123.0, 4) + voice_like(4 * FS),          # 8.5 - 12.5
             open_squelch(1.0),
             voice_like(3 * FS) + 0.002 * rng.normal(size=3 * FS),  # 13.5 - 16.5 carrier, no code
             open_squelch(2.0),
             dcs_signal(0o131, 2.5, inverted=True) + voice_like(int(2.5 * FS)),  # 18.5 - 21.0 -> D364N
             open_squelch(1.0)]
    p = os.path.join(tmp, "mixed.wav"); write(p, np.concatenate(parts))
    _, txs = top(p)
    want = [(2.0, 7.0, "DCS", "D131N"), (8.5, 12.5, "CTCSS", "123.0 Hz"), (13.5, 16.5, "none", ""),
            (18.5, 21.0, "DCS", f"D{dd.dcs_inverse(0o131):03o}N")]
    got = [(t.t0, t.t1, t.kind, t.label) for t in txs]
    ok = len(got) == len(want) and all(
        abs(g[0] - w[0]) <= 0.3 and abs(g[1] - w[1]) <= 0.3 and g[2:] == w[2:] for g, w in zip(got, want))
    check("mixed file: key-downs, labels, timestamps (+-0.3 s)", ok, True)
    if not ok:
        print("       got:", [(round(a, 2), round(b, 2), k, l) for a, b, k, l in got])
    check("mixed file: roll-up lists both codes",
          [k for k, _ in dd.roll_up(txs)],
          [("DCS", "D131N"), ("CTCSS", "123.0 Hz"), ("DCS", f"D{dd.dcs_inverse(0o131):03o}N")])

    # 7. DCS under a loud adult male voice (fundamental inside the DCS band)
    bad = []
    for code in (0o023, 0o261, 0o445, 0o754):
        p = os.path.join(tmp, f"m{code:03o}.wav")
        write(p, dcs_signal(code, 4) + 1.2 * harmonic_voice(4, 90, 180))
        got, _ = top(p)
        if got != ("DCS", f"D{code:03o}N"):
            bad.append((f"{code:03o}", got))
    check("DCS under loud male voice", bad, [])

    # 8. voice-only carriers must not produce a code
    bad = []
    for name, (lo, hi) in {"male": (90, 180), "child": (220, 380)}.items():
        for i in range(3):
            p = os.path.join(tmp, f"v{name}{i}.wav")
            write(p, harmonic_voice(4, lo, hi) + 0.01 * rng.normal(size=4 * FS))
            got, _ = top(p)
            if got != ("none", ""):
                bad.append((name, got))
    check("voice-only carriers give no code", bad, [])

    # 9. CTCSS under a loud male voice
    bad = []
    for hz in (67.0, 100.0, 136.5, 250.3):
        p = os.path.join(tmp, f"cm{hz}.wav")
        write(p, ctcss_signal(hz, 4) + 1.2 * harmonic_voice(4, 90, 180))
        got, _ = top(p)
        if got != ("CTCSS", f"{hz:.1f} Hz"):
            bad.append((hz, got))
    check("CTCSS under loud male voice", bad, [])

    # 10. DCS comb must not be mistaken for a CTCSS tone (some codes have a strong
    #     single line, e.g. 76 Hz) - classify() prefers DCS, but the CTCSS gate alone
    #     should reject it too.
    leaks = []
    for code in dd.DCS_104:
        y = dd.to_subaudible(dcs_signal(code, 3) + voice_like(3 * FS), FS)
        c = dd.detect_ctcss(y)
        if c["prominence"] >= dd.CTCSS_MIN_PROM and c["dominance"] >= dd.CTCSS_MIN_DOM and not c["comb"]:
            leaks.append(f"{code:03o}")
    check("CTCSS gate rejects every DCS comb", leaks, [])
    # ...and a real tone that happens to sit on a comb multiple (110.9 ~ 19 x 5.84) still passes
    y = dd.to_subaudible(ctcss_signal(110.9, 3) + voice_like(3 * FS), FS)
    check("110.9 Hz tone not mistaken for a comb", dd.detect_ctcss(y)["comb"], False)

    # 11. recording made with the SDR squelch closed: digital silence between key-downs
    x = np.concatenate([np.zeros(2 * FS), dcs_signal(0o054, 3) + voice_like(3 * FS),
                        np.zeros(2 * FS), ctcss_signal(88.5, 3), np.zeros(FS)])
    p = os.path.join(tmp, "squelched.wav"); write(p, x)
    _, txs = top(p)
    check("squelched recording splits on silence",
          [(round(t.t0), round(t.t1), t.kind, t.label) for t in txs],
          [(2, 5, "DCS", "D054N"), (7, 10, "CTCSS", "88.5 Hz")])

    print("\nALL PASS" if not fails else f"\n{fails} FAILED")
    return fails


if __name__ == "__main__":
    raise SystemExit(main())
