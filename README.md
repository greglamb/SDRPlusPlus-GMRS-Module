# FRS/GMRS Monitor for SDR++

A module for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) (Alexandre Rouma's cross-platform SDR software) that watches **every FRS/GMRS channel at once** and shows, live,
who is talking and which privacy code (CTCSS tone or DCS code) they are using —
as the consumer "privacy code" number your radio shows, plus the real tone/code.

```
Ch  Level     Code              Who     Last
17  ████████  141.3 Hz (22)     Family  now      <- busy, code confirmed
3   ░░░░░░░░  D261N (75)        Kids    23s      <- held: last code heard on ch 3
19  ░░░░░░░░  100.0 Hz (12)             2m
```

* **Board** – one row per channel of the block; carrier level, code (yellow `?` =
  tentative, green = confirmed, grey = held from the last transmission), preset
  name, last-heard time. Hover a row for details (measured tone, DCS
  correlation, the inverted-polarity reading, AFC offset).
* **Channel keys** – a button for every channel: 1–7, 8–14, 15–22 and the eight
  repeater inputs 15R–22R. Click one and the stock *Radio* module is tuned there
  (NFM, 12.5 or 20 kHz). Keys in the other 462/467 block are dimmed; clicking one
  re-centres the SDR on that block. Busy keys turn green, the key the Radio is
  parked on is blue.
* **Scanner** – *Scan* parks the Radio on whatever channel keys up, stays through
  the transmission plus a hang time, then resumes. Scan *this block* or *both
  blocks* (the SDR hops 462↔467 while idle). Right-click a channel to skip it;
  watched presets win when several channels are busy.
* **Log** – every key-down with time, channel, code, duration, peak level and
  preset name; CSV export.
* **Presets** – name a channel + code pair ("Kids: ch 3 DCS 261"); matches show
  in the *Who* column and can flash the row.

The detector is the same algorithm as the `dcsdecode` Python tool (DCS by
fold-and-correlate against all 104 codes, CTCSS by percentile spectrum), ported
to C++ and verified sample-for-sample against it (`tests/run_parity.py`).

## Requirements

* [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) built from source, **v1.3.x master**. SDR++ has no stable plugin ABI:
  a module must be compiled against the exact SDR++ build it will run in, so
  this module is built *inside* the SDR++ tree (see below). It does not load
  into the stock nightly `.app`/`.exe`.
* An SDR that does ≥ 250 kS/s (RTL-SDR, Airspy, HackRF, SDRplay …). The module
  needs a 200 kHz-wide VFO to see a whole block.
* FFTW3 single precision (`fftw3f`) — SDR++ already needs FFTW, so it's there.

## Prebuilt macOS app (Apple Silicon)

Check the repo's **Releases** page first — each release is SDR++ (built from
[AlexandreRouma/SDRPlusPlus](https://github.com/AlexandreRouma/SDRPlusPlus)
master, unmodified) with this module baked in.

To make a new one, run the **Build macOS (Apple Silicon)** workflow under the
*Actions* tab (*Run workflow* → pick the SDR++ ref, default `master`, and a
`release_tag` such as `v0.2.0`); pushing a `v*` tag does the same. Leave the
tag empty for a plain build, whose zip is only a run artifact
(`SDR++-macos-arm64`). Either way, unzip and, because the app is unsigned, run once:

```sh
xattr -dr com.apple.quarantine SDR++.app
```

It includes the RTL-SDR, Airspy, Airspy HF+, HackRF and File sources and the
PortAudio sink. Then enable the module as in *Enable it* below.

## Build

### 1. Get SDR++ and wire the module in

```sh
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
git clone https://github.com/greglamb/SDRPlusPlus-GMRS-Plugin.git sdrpp-frsgmrs-monitor
sh sdrpp-frsgmrs-monitor/tools/install_in_tree.sh SDRPlusPlus
```

The script symlinks the module into `SDRPlusPlus/decoder_modules/frsgmrs_monitor`,
adds `OPT_BUILD_FRSGMRS_MONITOR` (ON) to SDR++'s root `CMakeLists.txt`, and adds
the module to `make_macos_bundle.sh`. It's idempotent. (Use `--copy` instead of
a symlink if you prefer.)

### 2. macOS

```sh
brew install cmake pkg-config fftw glfw volk zstd rtl-sdr airspy hackrf   # the usual SDR++ deps
cd SDRPlusPlus
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_FRSGMRS_MONITOR=ON
cmake --build build -j
sh make_macos_bundle.sh build ./SDR++.app
open ./SDR++.app
```

### 2. Linux

```sh
sudo apt install build-essential cmake pkg-config libfftw3-dev libglfw3-dev libvolk-dev libzstd-dev librtlsdr-dev
cd SDRPlusPlus
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_FRSGMRS_MONITOR=ON
cmake --build build -j
sudo cmake --install build          # or run from build/ with: ./sdrpp -r ../root_dev
```

### Out-of-tree (if you already have a matching SDR++ build)

```sh
cmake -S sdrpp-frsgmrs-monitor -B build-mod -DSDRPP_MODULE_CMAKE=/path/to/SDRPlusPlus/build/sdrpp_module.cmake
cmake --build build-mod
# copy build-mod/frsgmrs_monitor.{so,dylib} next to SDR++'s other modules
```

### 3. Enable it

SDR++ → **Module Manager** → pick `frsgmrs_monitor`, name it (e.g. `FRS/GMRS
Monitor`), press **+**. A new *FRS/GMRS Monitor* section appears in the left menu.

## Using it

1. Pick your source and **Play**. Any sample rate ≥ 250 kS/s works
   (RTL-SDR: 1.024 MS/s or more is fine — the module takes a 250 kS/s slice).
2. **Block**: `462 MHz` (channels 1–7, 15–22 — all the simplex channels most
   radios use, and the GMRS *repeater outputs*) or `467 MHz` (channels 8–14 and
   the repeater *inputs* 15R–22R). One SDR covers one block at a time.
   Press **Center SDR on block** once; the status line says
   *block covered* when the module's wide VFO sits over the whole comb.
3. Watch the board. A row lights green when a carrier is detected; the code
   appears after ~1 s as `D261N (75)?` (tentative), turns solid green when two
   consecutive readings agree, and stays grey (held) for *Hold last code*
   minutes after the transmission ends so you can see who was there.
4. **Listen**: click a channel key, or select a board row and press *Listen*,
   or double-click a row. The *Radio* module is tuned to the channel in NFM
   (12.5 kHz for 1–14, 20 kHz for the GMRS main/repeater channels). Turn up
   the Radio squelch so idle channels are silent.
5. **Scan**: press *Scan*. The scanner parks the Radio on the first busy
   channel (watched presets first), shows *Stopped on ch 17 141.3 Hz (22)*,
   and resumes `Scan hang time` seconds after the carrier drops. With *Both
   blocks* the SDR retunes to the other block every `Scan dwell` seconds while
   nothing is heard, so you cover all 30 channels. Right-click a key or row →
   *Skip in scan* to lock a channel out (shown in red). Clicking a key or
   *Listen* stops the scan, like pressing a channel on a hardware scanner.
6. **Log** / **Presets** / **Keys** checkboxes show the log window, the preset
   editor and the channel keys. *Export CSV* writes
   `frsgmrs_log_<date>.csv` into the SDR++ root (config) folder.

### Start playing and scanning from the command line

SDR++'s own argument parser rejects unknown flags, so a module can't add new
ones — but it doesn't need to. The module can press Play itself and start the
scanner once the SDR is running, controlled by two Options checkboxes or by
environment variables (which win over the config, so a launcher or shortcut
can decide):

```sh
# macOS
FRSGMRS_AUTOPLAY=1 FRSGMRS_SCAN=1 FRSGMRS_SCAN_MODE=both open -a SDR++      # or: FRSGMRS_… ./SDR++.app/Contents/MacOS/sdrpp
# Linux
FRSGMRS_AUTOPLAY=1 FRSGMRS_SCAN=1 FRSGMRS_BLOCK=462 sdrpp
```

| Variable | Values | Effect |
|---|---|---|
| `FRSGMRS_AUTOPLAY` | `1` / `0` | press Play ~1.5 s after start-up (same as `sdrpp --autostart`, which also works) |
| `FRSGMRS_SCAN` | `1` / `0` | centre the SDR on the block and turn *Scan* on as soon as the SDR is running |
| `FRSGMRS_BLOCK` | `462` / `467` | which block to start on |
| `FRSGMRS_SCAN_MODE` | `this` / `both` | scan this block only, or hop between both |

The same two switches live in **Options → Start the SDR when SDR++ opens** and
**Start scanning when the SDR starts**, so a dedicated config root
(`sdrpp -r ~/.config/sdrpp-scanner`) can be a one-shot "turn on and listen"
setup with no environment variables at all. The source, gain and Radio squelch
come from that root's normal SDR++ config.

### Privacy-code numbers

Consumer radios number codes 1–38 (CTCSS tones) and 39–121 (DCS codes, in the
usual Motorola/Midland order). The module shows both the real value and the
number: `141.3 Hz (22)`, `D261N (75)`. Different brands occasionally disagree
on the DCS numbering, so check the table in your radio's manual against the
real code (`D261N`) when it matters.

### Repeaters

* GMRS repeaters **transmit on channels 15–22** (462.550–462.725 MHz) and
  **listen on 15R–22R** (467.550–467.725 MHz). What you hear on a repeater is
  its output, so the `462` block already covers "who's on the repeater", and
  the code shown on ch 15–22 while the repeater is keyed is the repeater's
  *output* tone (if it sends one — many do, some send none).
* To use a repeater your radio must transmit the repeater's *input* tone, which
  is not necessarily the same as the output tone. The input tone is only
  visible on 15R–22R, i.e. in the `467` block, and only when a user is close
  enough to you to be heard directly. Scan *Both blocks*, or click the `17R`
  key while a conversation is going on, to catch it. The right-click menu on
  any main/repeater channel jumps to its pair.
* Published repeater listings (myGMRS.com) give both tones; this tool lets you
  verify them.

### DCS polarity calibration (do this once)

DCS has a "normal" and an "inverted" form of every code (e.g. `D261N` reads as
`D732N` inverted, privacy 75 ↔ 118). Which one you see depends on the sign
convention of the SDR's FM demodulator versus your radio's. Key up a radio with
a known DCS code and look at the board:

* reads correctly → nothing to do;
* reads the inverted pair (the tooltip's *if your radio reads it inverted* line
  matches your radio's setting) → tick **Options → Invert DCS polarity** once
  and it stays in the config.

CTCSS tones have no polarity.

### Options

| Option | What it does |
|---|---|
| Carrier threshold | dB above the per-channel noise floor that counts as a carrier (8 dB default; raise it in a noisy area, lower it for weak toys) |
| AFC | follows radios that are up to ±3 kHz off frequency (cheap toys often are) |
| Invert DCS polarity | see above |
| Alert on watched presets | flashes the row and logs when a watched preset is heard |
| Hold last code | how long a code stays on the board after the transmission (1–60 min) |
| Scan hang time | seconds to stay on a channel after its carrier drops |
| Scan dwell per block | seconds to listen on each block before hopping (Both-blocks mode) |
| Start the SDR when SDR++ opens / Start scanning when the SDR starts | start-up automation, see above |

Config lives in `<sdrpp root>/frsgmrs_monitor_config.json` (block, options,
presets, scan skips).

## Known limits

* One block at a time per SDR. The two blocks are 5 MHz apart and a 200 kHz
  VFO can't straddle them; *Both blocks* scanning hops instead.
* A strong 20 kHz GMRS signal on a main channel can splatter into the adjacent
  12.5 kHz interstitial channel and show a weak carrier there; the carrier
  threshold and the *Level* bar make this obvious.
* DCS needs ~1 s of clean carrier before a reading; CTCSS ~0.5 s. Very short
  "chirp" key-downs log as `(no code)`.
* 467 MHz repeater inputs are 50 W mobiles or 5 W handhelds anywhere in the
  repeater's footprint — you only hear the ones near you.
* The SDR++ File source ignores retuning, so with a recording the 467 block
  shows the 462 recording relabelled. That's the file source, not the module.

## Repository layout

```
src/main.cpp              the module: VFO, board, keys, scanner, log, presets
src/channel_bank.h        one DSP sink that demodulates all 15 channels of a block
src/frs_tables.h          channel plan, blocks, repeater pairs, labels
src/detect/               CTCSS/DCS detector (tables, DSP helpers, detector.cpp)
src/radio_iface_compat.h  Radio module interface enums (so it builds out-of-tree)
tools/install_in_tree.sh  wires the module into an SDR++ source tree
tools/parity_test.cpp     standalone harness: streams a WAV through the detector
tools/paced_null_sink/    SDR++ sink that paces audio to real time (headless tests)
tests/oracle/dcsdecode.py the Python reference decoder (+ its unit tests)
tests/run_parity.py       C++ vs Python vs ground truth over a synthetic corpus
tests/make_iq.py          synthesises an IQ recording of a busy 462 block
tests/corpus/             a real recording: eKids toy radio, ch 12, DCS 261
```

## Tests

```sh
# detector parity (needs python3 + numpy/scipy, fftw3f, a C++17 compiler)
cmake -S tools -B build-tools -DCMAKE_BUILD_TYPE=Release && cmake --build build-tools
python3 tests/run_parity.py            # 159 cases; --quick for a subset
python3 -m pytest tests/oracle         # the Python reference's own tests

# whole-module replay: synthesise a block, play it through SDR++'s File source
python3 tests/make_iq.py /tmp/iq       # writes baseband_462637500Hz_*.wav + truth.json
# build SDR++ with -DOPT_BUILD_PACED_NULL_SINK=ON (tools/install_in_tree.sh --with-paced-sink),
# select File source + the "Paced Null" audio sink, press Play and compare the
# log with truth.json (7 transmissions on 6 channels, one code-less).
```
