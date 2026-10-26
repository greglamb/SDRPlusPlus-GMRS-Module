#pragma once
// FRS/GMRS channel plan. Two blocks 5 MHz apart; each is a contiguous
// 12.5 kHz comb, which is what lets one wide VFO per block cover it.
//
//   462 block: ch 1-7 (interstitial) + ch 15-22 (GMRS main = repeater OUTPUTS)
//   467 block: ch 8-14 (interstitial, 0.5 W) + repeater INPUTS 15R-22R
//
// Internally the repeater inputs are numbered 23..30 (= 15R..22R, the way
// most radios label them); use frsChannelLabel() for display.
#include <array>
#include <string>
#include <vector>

namespace frsmon {

struct FrsChannel {
    int number;        // 1..22 simplex, 23..30 repeater inputs (15R..22R)
    double freqHz;
    const char* group;
    bool wide;         // 20 kHz GMRS channel (else 12.5 kHz)
    int block;         // 0 = 462 MHz block, 1 = 467 MHz block
};

inline constexpr const char* GRP_INTER = "FRS/GMRS 1-7 (2 W FRS, 5 W GMRS)";
inline constexpr const char* GRP_LOW   = "FRS/GMRS 8-14 (0.5 W)";
inline constexpr const char* GRP_MAIN  = "GMRS main / repeater output";
inline constexpr const char* GRP_RPTIN = "GMRS repeater input (+5 MHz)";

inline constexpr std::array<FrsChannel, 30> FRS_CHANNELS = {{
    {1, 462562500.0, GRP_INTER, false, 0}, {2, 462587500.0, GRP_INTER, false, 0}, {3, 462612500.0, GRP_INTER, false, 0},
    {4, 462637500.0, GRP_INTER, false, 0}, {5, 462662500.0, GRP_INTER, false, 0}, {6, 462687500.0, GRP_INTER, false, 0},
    {7, 462712500.0, GRP_INTER, false, 0},
    {8, 467562500.0, GRP_LOW, false, 1}, {9, 467587500.0, GRP_LOW, false, 1}, {10, 467612500.0, GRP_LOW, false, 1},
    {11, 467637500.0, GRP_LOW, false, 1}, {12, 467662500.0, GRP_LOW, false, 1}, {13, 467687500.0, GRP_LOW, false, 1},
    {14, 467712500.0, GRP_LOW, false, 1},
    {15, 462550000.0, GRP_MAIN, true, 0}, {16, 462575000.0, GRP_MAIN, true, 0}, {17, 462600000.0, GRP_MAIN, true, 0},
    {18, 462625000.0, GRP_MAIN, true, 0}, {19, 462650000.0, GRP_MAIN, true, 0}, {20, 462675000.0, GRP_MAIN, true, 0},
    {21, 462700000.0, GRP_MAIN, true, 0}, {22, 462725000.0, GRP_MAIN, true, 0},
    // repeater inputs, 15R..22R
    {23, 467550000.0, GRP_RPTIN, true, 1}, {24, 467575000.0, GRP_RPTIN, true, 1}, {25, 467600000.0, GRP_RPTIN, true, 1},
    {26, 467625000.0, GRP_RPTIN, true, 1}, {27, 467650000.0, GRP_RPTIN, true, 1}, {28, 467675000.0, GRP_RPTIN, true, 1},
    {29, 467700000.0, GRP_RPTIN, true, 1}, {30, 467725000.0, GRP_RPTIN, true, 1},
}};

constexpr int FRS_CHANNEL_COUNT = 30;
constexpr int FRS_FIRST_RPT_IN = 23;

struct FrsBlock {
    const char* name;
    double centerHz;
    double spanHz;              // comb extent, edge to edge
    std::vector<int> channels;  // channel numbers, ascending frequency
};

inline const std::vector<FrsBlock>& frsBlocks() {
    static const std::vector<FrsBlock> blocks = {
        {"462 MHz: ch 1-7, 15-22 (rptr out)", 462637500.0, 175000.0, {15, 1, 16, 2, 17, 3, 18, 4, 19, 5, 20, 6, 21, 7, 22}},
        {"467 MHz: ch 8-14, 15R-22R (rptr in)", 467637500.0, 175000.0, {23, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30}},
    };
    return blocks;
}

inline bool frsValidChannel(int number) { return number >= 1 && number <= FRS_CHANNEL_COUNT; }
inline const FrsChannel& frsChannel(int number) { return FRS_CHANNELS[number - 1]; }
inline int frsBlockOf(int number) { return frsChannel(number).block; }
inline bool frsIsRepeaterInput(int number) { return number >= FRS_FIRST_RPT_IN; }

// "17" for simplex, "17R" for the repeater input paired with output ch 17.
inline std::string frsChannelLabel(int number) {
    if (frsIsRepeaterInput(number)) { return std::to_string(number - 8) + "R"; }
    return std::to_string(number);
}

// Repeater pairing: output ch 15..22 <-> input 23..30 (+5 MHz). -1 if none.
inline int frsRepeaterInputOf(int outCh) { return (outCh >= 15 && outCh <= 22) ? outCh + 8 : -1; }
inline int frsRepeaterOutputOf(int inCh) { return frsIsRepeaterInput(inCh) ? inCh - 8 : -1; }

// Row of a channel within its block's channel list, -1 if not in that block.
inline int frsRowOf(int blockIdx, int number) {
    const auto& ch = frsBlocks()[blockIdx].channels;
    for (int i = 0; i < (int)ch.size(); i++) { if (ch[i] == number) { return i; } }
    return -1;
}

} // namespace frsmon
