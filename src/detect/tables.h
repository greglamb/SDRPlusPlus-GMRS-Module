#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Greg Lamb
// Tone and code tables shared by the detectors, the UI and the tests.
// Numbering follows consumer FRS/GMRS radios (Motorola Talkabout, Midland,
// Cobra, Rocky Talkie): CTCSS 1..38 in EIA order, DCS 39..121 in the 83-code
// list. Verified against the Rocky Talkie Mountain Radio manual.
#include <array>
#include <cstdint>

namespace frsmon {

// EIA 38-tone list == privacy codes 1..38
inline constexpr std::array<float, 38> CTCSS_38 = {
    67.0f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f, 94.8f, 97.4f,
    100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f,
    136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 162.2f, 167.9f, 173.8f, 179.9f,
    186.2f, 192.8f, 203.5f, 210.7f, 218.1f, 225.7f, 233.6f, 241.8f, 250.3f};

// Extra tones found on 50-tone radios (no consumer privacy-code number)
inline constexpr std::array<float, 12> CTCSS_EXTRA = {
    69.3f, 159.8f, 165.5f, 171.3f, 177.3f, 183.5f, 189.9f, 196.6f, 199.5f,
    206.5f, 229.1f, 254.1f};

// All 104 standard DCS codes (octal values)
inline constexpr std::array<int, 104> DCS_104 = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053,
    0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115, 0116, 0122,
    0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162,
    0165, 0172, 0174, 0205, 0212, 0223, 0225, 0226, 0243, 0244,
    0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271,
    0274, 0306, 0311, 0315, 0325, 0331, 0332, 0343, 0346, 0351,
    0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432,
    0445, 0446, 0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503,
    0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
    0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731,
    0732, 0734, 0743, 0754};

// 83-code consumer list == privacy codes 39..121 (order matters)
inline constexpr std::array<int, 83> DCS_83 = {
    0023, 0025, 0026, 0031, 0032, 0043, 0047, 0051, 0054, 0065,
    0071, 0072, 0073, 0074, 0114, 0115, 0116, 0125, 0131, 0132,
    0134, 0143, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205,
    0223, 0226, 0243, 0244, 0245, 0251, 0261, 0263, 0265, 0271,
    0306, 0311, 0315, 0331, 0343, 0346, 0351, 0364, 0365, 0371,
    0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446, 0465, 0466,
    0503, 0506, 0516, 0532, 0546, 0565, 0606, 0612, 0624, 0627,
    0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732,
    0734, 0743, 0754};

// privacy-code number (39..121) for an octal DCS code, 0 if not in the list
inline int dcsPrivacyCode(int code) {
    for (size_t i = 0; i < DCS_83.size(); i++) {
        if (DCS_83[i] == code) { return 39 + (int)i; }
    }
    return 0;
}

// privacy-code number (1..38) for a tone, 0 if not in the 38-tone list
inline int ctcssPrivacyCode(float hz) {
    for (size_t i = 0; i < CTCSS_38.size(); i++) {
        if (CTCSS_38[i] == hz) { return 1 + (int)i; }
    }
    return 0;
}

// ---------------------------------------------------------------- Golay ---

// 23 bits of the DCS word for an octal code, bit i = i-th transmitted bit.
// 12 data bits = 9-bit code + fixed '100' (bit 11 set), then 11 check bits of
// the (23,12) Golay code with generator x^11+x^10+x^6+x^5+x^4+x^2+1.
// Verified against the standard N/I inversion pairs (023<->047, ...).
inline uint32_t dcsWord(int code) {
    const uint32_t poly = 0xC75;
    uint32_t data = (uint32_t)code | 0x800;
    uint32_t reg = data << 11;
    for (int i = 22; i >= 11; i--) {
        if (reg & (1u << i)) { reg ^= poly << (i - 11); }
    }
    return data | ((reg & 0x7FF) << 12);
}

inline uint32_t rotl23(uint32_t w, int n) {
    n %= 23;
    return ((w << n) | (w >> (23 - n))) & 0x7FFFFF;
}

// The code whose normal word is this code's word inverted (DxxxI), 0 if none.
inline int dcsInverse(int code) {
    uint32_t inv = (~dcsWord(code)) & 0x7FFFFF;
    for (int d : DCS_104) {
        uint32_t w = dcsWord(d);
        for (int r = 0; r < 23; r++) {
            if (rotl23(w, r) == inv) { return d; }
        }
    }
    return 0;
}

} // namespace frsmon
