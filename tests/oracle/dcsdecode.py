#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Greg Lamb
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy>=1.22", "scipy>=1.8"]
# ///
"""
dcsdecode — find CTCSS tones and DCS codes in demodulated NFM audio.

Feed it a WAV recorded from an SDR's NFM demodulator (SDR++ "Recorder" in
audio mode, GQRX, SDR#, rtl_fm piped through sox, ...). It finds each
key-down (carrier quieting vs. open-squelch hiss), looks inside each one for
a single sub-audible tone (CTCSS) or a repeating 23-bit Golay word at
~134.4 bps (DCS), and lists every transmission with a timestamp plus the
"privacy code" number used by Motorola / Midland / Cobra / Rocky Talkie
style FRS radios. Different codes in the same file are listed separately.

    uv run dcsdecode.py recording.wav
    python3 dcsdecode.py recording.wav --segment 7-12

Caveats
  * Keep the SDR's audio high-pass / "remove PL tone" filter OFF, or there
    will be nothing to decode.
  * DCS has a polarity. An SDR demodulator with the opposite sign convention
    reports the inverted code (e.g. 261 <-> 732). Both are printed; try the
    first one, then the other. Use --invert if you know your demod is flipped.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, asdict, field
from functools import lru_cache
from typing import Optional

import numpy as np
from scipy import signal
from scipy.io import wavfile
from scipy.ndimage import uniform_filter1d

FSD = 2400           # internal sample rate; everything of interest is < 300 Hz
BAND = (2.0, 300.0)  # sub-audible band we keep
MIN_TX = 0.3         # ignore carrier blips shorter than this (s)
MIN_DECODE = 1.0     # need at least this much carrier to decode (s)
WINDOW = 3.0         # analysis window inside a transmission (s)

# ---------------------------------------------------------------- tables ---

# EIA 38-tone CTCSS list == privacy codes 1..38 on consumer FRS/GMRS radios
CTCSS_38 = [67.0, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4,
            100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8,
            136.5, 141.3, 146.2, 151.4, 156.7, 162.2, 167.9, 173.8, 179.9,
            186.2, 192.8, 203.5, 210.7, 218.1, 225.7, 233.6, 241.8, 250.3]
# extra tones found on 50-tone radios (no consumer privacy-code number)
CTCSS_EXTRA = [69.3, 159.8, 165.5, 171.3, 177.3, 183.5, 189.9, 196.6, 199.5,
               206.5, 229.1, 254.1]
CTCSS_ALL = sorted(CTCSS_38 + CTCSS_EXTRA)

# the full 104 "standard" DCS codes (octal)
DCS_104 = [
    0o023, 0o025, 0o026, 0o031, 0o032, 0o036, 0o043, 0o047, 0o051, 0o053,
    0o054, 0o065, 0o071, 0o072, 0o073, 0o074, 0o114, 0o115, 0o116, 0o122,
    0o125, 0o131, 0o132, 0o134, 0o143, 0o145, 0o152, 0o155, 0o156, 0o162,
    0o165, 0o172, 0o174, 0o205, 0o212, 0o223, 0o225, 0o226, 0o243, 0o244,
    0o245, 0o246, 0o251, 0o252, 0o255, 0o261, 0o263, 0o265, 0o266, 0o271,
    0o274, 0o306, 0o311, 0o315, 0o325, 0o331, 0o332, 0o343, 0o346, 0o351,
    0o356, 0o364, 0o365, 0o371, 0o411, 0o412, 0o413, 0o423, 0o431, 0o432,
    0o445, 0o446, 0o452, 0o454, 0o455, 0o462, 0o464, 0o465, 0o466, 0o503,
    0o506, 0o516, 0o523, 0o526, 0o532, 0o546, 0o565, 0o606, 0o612, 0o624,
    0o627, 0o631, 0o632, 0o654, 0o662, 0o664, 0o703, 0o712, 0o723, 0o731,
    0o732, 0o734, 0o743, 0o754,
]
# 83-code subset == privacy codes 39..121 (Motorola Talkabout, Midland,
# Cobra, Rocky Talkie). Order matters.
DCS_83 = [
    0o023, 0o025, 0o026, 0o031, 0o032, 0o043, 0o047, 0o051, 0o054, 0o065,
    0o071, 0o072, 0o073, 0o074, 0o114, 0o115, 0o116, 0o125, 0o131, 0o132,
    0o134, 0o143, 0o152, 0o155, 0o156, 0o162, 0o165, 0o172, 0o174, 0o205,
    0o223, 0o226, 0o243, 0o244, 0o245, 0o251, 0o261, 0o263, 0o265, 0o271,
    0o306, 0o311, 0o315, 0o331, 0o343, 0o346, 0o351, 0o364, 0o365, 0o371,
    0o411, 0o412, 0o413, 0o423, 0o431, 0o432, 0o445, 0o446, 0o465, 0o466,
    0o503, 0o506, 0o516, 0o532, 0o546, 0o565, 0o606, 0o612, 0o624, 0o627,
    0o631, 0o632, 0o654, 0o662, 0o664, 0o703, 0o712, 0o723, 0o731, 0o732,
    0o734, 0o743, 0o754,
]
assert len(DCS_104) == 104 and len(DCS_83) == 83
PRIVACY_OF_DCS = {c: 39 + i for i, c in enumerate(DCS_83)}

# ------------------------------------------------------------ DCS words ---

_GOLAY_POLY = 0xC75  # x^11+x^10+x^6+x^5+x^4+x^2+1


def dcs_word(code: int) -> list[int]:
    """23 bits in transmission order (LSB first) for an octal DCS code.

    12 data bits = 9-bit code + fixed '100' (bit 11 set), then 11 Golay check
    bits. Verified against the standard N/I inversion pairs (023<->047 etc.).
    """
    data = code | 0x800
    reg = data << 11
    for i in range(22, 10, -1):
        if reg & (1 << i):
            reg ^= _GOLAY_POLY << (i - 11)
    cw = data | ((reg & 0x7FF) << 12)
    return [(cw >> i) & 1 for i in range(23)]


def _rotations(bits):
    return {tuple(bits[i:] + bits[:i]) for i in range(23)}


@lru_cache(maxsize=None)
def dcs_inverse(code: int) -> Optional[int]:
    """The code whose normal word equals this code's word inverted (DxxxI)."""
    inv = [1 - b for b in dcs_word(code)]
    rots = _rotations(inv)
    for d in DCS_104:
        if tuple(dcs_word(d)) in rots:
            return d
    return None


# --------------------------------------------------------------- audio ---

def load_wav(path: str):
    fs, data = wavfile.read(path)
    if np.issubdtype(data.dtype, np.integer):
        data = data.astype(np.float64) / np.iinfo(data.dtype).max
    else:
        data = data.astype(np.float64)
    if data.ndim > 1:
        data = data.mean(axis=1)
    return int(fs), data


def to_subaudible(x: np.ndarray, fs: int) -> np.ndarray:
    """Band-limit to the sub-audible band and resample to FSD."""
    sos = signal.butter(4, BAND, btype="band", fs=fs, output="sos")
    y = signal.sosfiltfilt(sos, x)
    g = np.gcd(fs, FSD)
    return signal.resample_poly(y, FSD // g, fs // g)


QUIETING_DB = 10.0   # a carrier must quiet the hiss band by at least this much


def carrier_segments(x: np.ndarray, fs: int, quieting_db: float = QUIETING_DB):
    """Find key-downs: (t0, t1) spans where a carrier is present.

    Open squelch on an NFM demodulator is loud, rising-with-frequency hiss;
    a carrier quiets it. The loudest the hiss band ever gets is the
    open-squelch level; any frame at least `quieting_db` below that has a
    carrier. Digital silence (recordings made with the SDR's own squelch
    closed) is treated as no carrier too.
    """
    frame = int(0.05 * fs)
    nfr = len(x) // frame
    hi_lo = 2500.0
    hi_hi = min(0.45 * fs, 8000.0)
    if hi_hi - hi_lo < 500:                     # low sample-rate recording
        hi_lo, hi_hi = 0.25 * fs, 0.45 * fs
    sos = signal.butter(4, [hi_lo, hi_hi], btype="band", fs=fs, output="sos")
    h = signal.sosfiltfilt(sos, x)[:nfr * frame].reshape(nfr, frame)
    xf = x[:nfr * frame].reshape(nfr, frame)
    hiss = 10 * np.log10((h ** 2).mean(axis=1) + 1e-12)
    full = 10 * np.log10((xf ** 2).mean(axis=1) + 1e-12)
    hiss = uniform_filter1d(hiss, 3)
    not_silent = full > -70
    if not_silent.any():
        live = hiss[not_silent]
        open_level = float(np.percentile(live, 99.5))
        if open_level - np.percentile(live, 5) >= 15:   # file has both states
            carrier = hiss < open_level - quieting_db
        else:                                           # whole file is one state
            carrier = np.ones(nfr, dtype=bool)
    else:
        carrier = np.zeros(nfr, dtype=bool)
    carrier &= not_silent
    # merge short gaps, drop short blips
    segs, start = [], None
    gap_frames = int(0.25 / 0.05)
    for i, on in enumerate(np.append(carrier, False)):
        if on and start is None:
            start = i
        elif not on and start is not None:
            if segs and start - segs[-1][1] <= gap_frames:
                segs[-1][1] = i
            else:
                segs.append([start, i])
            start = None
    out = [(a * 0.05, b * 0.05) for a, b in segs if (b - a) * 0.05 >= MIN_TX]
    return out


# ------------------------------------------------------------ detectors ---

DCS_BAND = (2.0, 150.0)   # DCS energy sits below ~134 Hz; this keeps voice out
SPB = 10                   # samples per bit in the folded (word-synchronous) domain
WL = 23 * SPB              # samples per 23-bit word

# detection thresholds, set from measurements: real transmissions (even with a
# kid yelling over the code) score corr>=0.76, margin>=0.19, pf>=0.14; open
# squelch noise and voice-only carriers never exceeded 0.58 / 0.08 / 0.09.
DCS_MIN_CORR, DCS_MIN_MARGIN, DCS_MIN_PF = 0.75, 0.15, 0.10


def _lowpass_dcs(seg: np.ndarray) -> np.ndarray:
    sos = signal.butter(4, DCS_BAND, btype="band", fs=FSD, output="sos")
    return signal.sosfiltfilt(sos, seg)


@lru_cache(maxsize=None)
def _word_template(code: int, rate: float) -> np.ndarray:
    """One band-limited period of the code's NRZ waveform, SPB samples/bit."""
    fsr = SPB * rate
    bits = np.repeat([1.0 if b else -1.0 for b in dcs_word(code)], SPB)
    sos = signal.butter(4, DCS_BAND, btype="band", fs=fsr, output="sos")
    p = signal.sosfiltfilt(sos, np.tile(bits, 7))[3 * WL:4 * WL]
    return p / np.linalg.norm(p)


def _fold(seg: np.ndarray, rate: float):
    """Average the segment word-by-word at the given bit rate.

    Anything periodic in the word (the DCS code) adds up; voice and noise
    average down. Returns the folded word and the fraction of the segment's
    energy that was periodic.
    """
    fsr = SPB * rate
    nwords = int(len(seg) / FSD * fsr // WL)
    if nwords < 2:
        return None, 0.0, nwords
    t = np.arange(nwords * WL) / fsr
    r = np.interp(t, np.arange(len(seg)) / FSD, seg).reshape(nwords, WL)
    f = r.mean(axis=0)
    pf = nwords * np.sum(f ** 2) / (np.sum(r ** 2) + 1e-20)
    return f, float(pf), nwords


def _circ_ncc(a: np.ndarray, b: np.ndarray) -> float:
    cc = np.fft.irfft(np.fft.rfft(a) * np.conj(np.fft.rfft(b)), len(a))
    cc /= (np.linalg.norm(a) * np.linalg.norm(b) + 1e-20)
    i = int(np.argmax(np.abs(cc)))
    return float(cc[i])


def _estimate_dcs_rate(seg: np.ndarray) -> float:
    """Harmonic-sum search for the comb fundamental (= bitrate / 23)."""
    n = len(seg)
    nfft = 1 << int(np.ceil(np.log2(n * 8)))
    spec = np.abs(np.fft.rfft(seg * np.hanning(n), nfft))
    f = np.fft.rfftfreq(nfft, 1 / FSD)
    db = 20 * np.log10(spec + 1e-12)
    best, best_f0 = -np.inf, 134.4 / 23
    ks = np.arange(1, 21)
    for f0 in np.arange(5.70, 5.96, 0.005):
        idx = np.round(ks * f0 / (f[1] - f[0])).astype(int)
        s = db[idx].sum()
        if s > best:
            best, best_f0 = s, f0
    return 23 * best_f0


def detect_dcs(seg: np.ndarray):
    """Best DCS match for one window of 2-300 Hz audio (any length >= ~1 s).

    Folds the window at the word period for a handful of candidate bit rates
    around the comb-estimated one, correlates the folded word circularly
    against all 104 codewords, and keeps the strongest. `margin` is the gap
    to the best *different* code (a code and its inverse count as the same).
    """
    seg = _lowpass_dcs(seg - seg.mean())
    if not np.any(seg):
        return None
    r0 = round(_estimate_dcs_rate(seg) / 0.15) * 0.15   # quantize -> cache hits
    best = None
    for rate in (r0 - 0.3, r0 - 0.15, r0, r0 + 0.15, r0 + 0.3):
        rate = round(rate, 3)
        f, pf, nwords = _fold(seg, rate)
        if f is None or not np.any(f):
            continue
        scored = []
        for code in DCS_104:
            c = _circ_ncc(f, _word_template(code, rate))
            scored.append((abs(c), c, code))
        scored.sort(reverse=True)
        top = scored[0]
        inv = dcs_inverse(top[2])
        runner = next((s for s in scored[1:] if s[2] != inv), scored[1])
        cand = {"code": top[2], "corr": top[1], "inverted": top[1] < 0,
                "margin": top[0] - runner[0], "pf": pf, "rate": rate, "words": nwords}
        if best is None or abs(cand["corr"]) * cand["pf"] ** 0.5 > abs(best["corr"]) * best["pf"] ** 0.5:
            best = cand
    return best


def _spectrum_db(seg: np.ndarray, nfft: int):
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
    return 20 * np.log10(spec + 1e-12)


def detect_ctcss(seg: np.ndarray):
    """Look for one steady sub-audible tone.

    Voice wanders in pitch and comes in syllables; a CTCSS tone does neither.
    So the spectrum used for the peak search is a low percentile across 1 s
    sub-windows: voice energy drops out, a steady tone survives. The full
    window is then used for a fine frequency estimate.
    """
    seg = seg - seg.mean()
    n = len(seg)
    sub = min(n, FSD // 2)                # 0.5 s sub-windows, 0.25 s hop
    hop = max(1, sub // 2)
    nfft = 1 << int(np.ceil(np.log2(sub * 16)))
    f = np.fft.rfftfreq(nfft, 1 / FSD)
    m = (f >= 55) & (f <= 265)
    specs = [_spectrum_db(seg[s:s + sub], nfft)[m] for s in range(0, n - sub + 1, hop)]
    db = np.percentile(np.array(specs), 10, axis=0) if len(specs) > 1 else specs[0]
    f = f[m]
    floor = float(np.median(db))
    i = int(np.argmax(db))
    peak_hz, peak_db = float(f[i]), float(db[i])
    mask = np.abs(f - peak_hz) > 6.0                    # outside the peak's own main lobe
    for k in (2, 3):                                    # and not its harmonics
        mask &= np.abs(f - k * peak_hz) > 3.0
    second_db = float(db[mask].max()) if mask.any() else floor
    # DCS comb guard: a DCS word puts many lines at multiples of ~5.84 Hz, all
    # within ~20 dB of the strongest. A tone has none (besides its skirt/harmonics).
    comb = False
    k0 = max(1, round(peak_hz / (134.4 / 23)))
    f0 = peak_hz / k0
    if 5.70 <= f0 <= 5.96:
        lines = 0
        for k in range(10, 46):
            if abs(k - k0) <= 2 or k in (2 * k0, 3 * k0):
                continue
            w = np.abs(f - k * f0) <= 0.4
            if w.any() and db[w].max() >= peak_db - 20 and db[w].max() - floor >= 10:
                lines += 1
        comb = lines >= 4
    # refine the frequency on the full window (finer bins), near the coarse peak
    nfft_full = 1 << int(np.ceil(np.log2(n * 16)))
    ff = np.fft.rfftfreq(nfft_full, 1 / FSD)
    mm = np.abs(ff - peak_hz) <= 2.0
    full = _spectrum_db(seg, nfft_full)[mm]
    fine_hz = float(ff[mm][int(np.argmax(full))])
    nearest = min(CTCSS_ALL, key=lambda t: abs(t - fine_hz))
    return {"hz": fine_hz, "prominence": peak_db - floor,
            "dominance": peak_db - second_db, "nearest": nearest,
            "err": abs(nearest - fine_hz), "comb": comb}


# measured: steady tone under a voice 12 dB louder -> prominence >= 23, dominance
# >= 13; voice-only / noise windows never exceeded 14 / 2.5.
CTCSS_MIN_PROM, CTCSS_MIN_DOM = 18.0, 8.0


@dataclass
class Window:
    kind: str                      # "DCS" | "CTCSS" | "none"
    label: str = ""
    confidence: float = 0.0
    dcs_code: Optional[int] = None
    dcs_alt: Optional[int] = None
    rate: Optional[float] = None
    ctcss_hz: Optional[float] = None


def classify_window(seg: np.ndarray, invert: bool) -> Window:
    d = detect_dcs(seg)
    c = detect_ctcss(seg)
    if d and abs(d["corr"]) >= DCS_MIN_CORR and d["margin"] >= DCS_MIN_MARGIN and d["pf"] >= DCS_MIN_PF:
        flipped = d["inverted"] != invert
        code = d["code"]
        shown = dcs_inverse(code) if flipped else code
        other = code if flipped else dcs_inverse(code)
        return Window("DCS", f"D{shown:03o}N", round(abs(d["corr"]), 3),
                      dcs_code=shown, dcs_alt=other, rate=round(d["rate"], 2))
    if (c["prominence"] >= CTCSS_MIN_PROM and c["dominance"] >= CTCSS_MIN_DOM
            and c["err"] <= 1.0 and not c["comb"]):
        return Window("CTCSS", f"{c['nearest']:.1f} Hz", round(c["prominence"], 1),
                      ctcss_hz=c["nearest"])
    return Window("none")


# -------------------------------------------------------- transmissions ---

@dataclass
class Transmission:
    t0: float
    t1: float
    kind: str                      # "DCS" | "CTCSS" | "none" | "short"
    label: str = ""
    privacy_code: Optional[int] = None
    confidence: float = 0.0
    dcs_code: Optional[str] = None
    dcs_alt_code: Optional[str] = None
    dcs_alt_privacy_code: Optional[int] = None
    ctcss_hz: Optional[float] = None
    note: str = ""
    windows: list = field(default_factory=list, repr=False)

    @property
    def duration(self) -> float:
        return self.t1 - self.t0


def privacy_of(w: Window) -> Optional[int]:
    if w.kind == "CTCSS" and w.ctcss_hz in CTCSS_38:
        return CTCSS_38.index(w.ctcss_hz) + 1
    if w.kind == "DCS":
        return PRIVACY_OF_DCS.get(w.dcs_code)
    return None


def classify_transmission(y: np.ndarray, t0: float, t1: float, invert: bool) -> Transmission:
    dur = t1 - t0
    if dur < MIN_DECODE:
        return Transmission(t0, t1, "short", note=f"only {dur:.1f} s, need {MIN_DECODE:.0f} s")
    a, b = int(t0 * FSD), int(t1 * FSD)
    n = int(min(WINDOW, dur) * FSD)
    hop = max(int(n / 2), 1)
    wins = []
    s = a
    while s + n <= b:
        wins.append(classify_window(y[s:s + n], invert))
        if s + n == b:
            break
        s = min(s + hop, b - n)
    votes = {}
    for w in wins:
        if w.kind != "none":
            votes.setdefault((w.kind, w.label), []).append(w)
    if not votes:
        return Transmission(t0, t1, "none", note="carrier, no sub-audible code (or voice only)",
                            windows=wins)
    (kind, label), ws = max(votes.items(), key=lambda kv: sum(w.confidence for w in kv[1]))
    best = max(ws, key=lambda w: w.confidence)
    tx = Transmission(t0, t1, kind, label, privacy_of(best), best.confidence, windows=wins)
    if kind == "DCS":
        tx.dcs_code = f"{best.dcs_code:03o}"
        tx.dcs_alt_code = f"{best.dcs_alt:03o}"
        tx.dcs_alt_privacy_code = PRIVACY_OF_DCS.get(best.dcs_alt)
    else:
        tx.ctcss_hz = best.ctcss_hz
    others = [k for k in votes if k != (kind, label)]
    if others:
        tx.note = "mixed: also " + ", ".join(f"{k} {l}" for k, l in others)
    elif len(ws) < len(wins):
        tx.note = f"code in {len(ws)}/{len(wins)} windows"
    return tx


def analyze(path: str, invert: bool = False, segment=None, sliding: bool = False):
    fs, x = load_wav(path)
    duration = len(x) / fs
    if segment:
        x = x[int(segment[0] * fs):int(segment[1] * fs)]
        off = segment[0]
    else:
        off = 0.0
    y = to_subaudible(x, fs)
    if sliding:
        spans = [(s, min(s + WINDOW, len(y) / FSD)) for s in np.arange(0, len(y) / FSD - MIN_DECODE, WINDOW / 2)]
    else:
        spans = carrier_segments(x, fs)
    txs = []
    for t0, t1 in spans:
        tx = classify_transmission(y, t0, t1, invert)
        tx.t0 += off; tx.t1 += off
        txs.append(tx)
    return fs, duration, txs


def roll_up(txs):
    seen = {}
    for tx in txs:
        if tx.kind in ("DCS", "CTCSS"):
            e = seen.setdefault((tx.kind, tx.label), {"count": 0, "seconds": 0.0, "tx": tx})
            e["count"] += 1
            e["seconds"] += tx.duration
    return sorted(seen.items(), key=lambda kv: -kv[1]["seconds"])


# ----------------------------------------------------------------- main ---

def mmss(t: float) -> str:
    return f"{int(t // 60)}:{t % 60:04.1f}"


def privacy_text(tx: Transmission) -> str:
    if tx.privacy_code:
        return f"privacy {tx.privacy_code}"
    if tx.kind == "CTCSS":
        return "50-tone only"
    if tx.kind == "DCS":
        return "not in 83 list"
    return ""


def main(argv=None):
    doc = __doc__.split("\n\n")
    ap = argparse.ArgumentParser(prog="dcsdecode", description=doc[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(doc[3:]))
    ap.add_argument("wav", help="demodulated NFM audio (WAV)")
    ap.add_argument("--segment", metavar="T0-T1", help="only analyze this time range, seconds")
    ap.add_argument("--invert", action="store_true", help="assume the demodulator's polarity is inverted")
    ap.add_argument("--sliding", action="store_true",
                    help="skip key-down detection; just slide a window over the whole file")
    ap.add_argument("--all", action="store_true", help="also list key-downs that carried no code")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args(argv)

    seg = tuple(float(v) for v in args.segment.split("-")) if args.segment else None
    fs, dur, txs = analyze(args.wav, args.invert, seg, args.sliding)
    summary = roll_up(txs)

    if args.json:
        out = {
            "file": args.wav, "sample_rate": fs, "duration_s": round(dur, 2),
            "transmissions": [
                {k: v for k, v in asdict(tx).items() if k != "windows"} | {"duration_s": round(tx.duration, 2)}
                for tx in txs
            ],
            "codes_seen": [
                {"kind": k, "label": l, "privacy_code": e["tx"].privacy_code,
                 "transmissions": e["count"], "seconds": round(e["seconds"], 1),
                 "dcs_code": e["tx"].dcs_code, "dcs_alt_code": e["tx"].dcs_alt_code,
                 "dcs_alt_privacy_code": e["tx"].dcs_alt_privacy_code}
                for (k, l), e in summary
            ],
        }
        print(json.dumps(out, indent=2))
        return 0 if summary else 1

    n_key = len(txs)
    print(f"{args.wav}: {fs} Hz, {dur:.1f} s, {n_key} key-down{'s' if n_key != 1 else ''}")
    print(f"{'start':>7} {'end':>7} {'dur':>6}  {'code':18} {'privacy':12} {'conf':>5}  note")
    for tx in txs:
        if tx.kind in ("none", "short") and not args.all:
            continue
        label = f"{tx.kind} {tx.label}" if tx.kind in ("DCS", "CTCSS") else ("(no code)" if tx.kind == "none" else "(too short)")
        conf = f"{tx.confidence:5.2f}" if tx.kind in ("DCS", "CTCSS") else "     "
        print(f"{mmss(tx.t0):>7} {mmss(tx.t1):>7} {tx.duration:5.1f}s  {label:18} {privacy_text(tx):12} {conf}  {tx.note}")
    hidden = sum(1 for tx in txs if tx.kind in ("none", "short"))
    if hidden and not args.all:
        print(f"        ({hidden} key-down{'s' if hidden != 1 else ''} with no decodable code hidden; --all to show)")
    print()
    if not summary:
        print("No CTCSS tone or DCS code found. Check that the SDR's audio high-pass /")
        print("PL-removal filter is off and that the recording contains a transmission.")
        return 1
    print("Codes seen:")
    for (kind, label), e in summary:
        tx = e["tx"]
        line = f"  {kind} {label:10} -> {privacy_text(tx):14} {e['count']} transmission{'s' if e['count'] != 1 else ''}, {e['seconds']:.1f} s"
        if kind == "DCS":
            alt = f"privacy {tx.dcs_alt_privacy_code}" if tx.dcs_alt_privacy_code else "not in 83 list"
            line += f"   [if demod inverted: D{tx.dcs_alt_code}N -> {alt}]"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
