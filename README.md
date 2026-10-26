# FRS/GMRS Monitor for SDR++

A module for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) that
watches **every FRS/GMRS channel at once** and shows, live, who is talking and
which privacy code (CTCSS tone or DCS code) they are using — as the "privacy
code" number your radio shows, plus the real tone/code. Click a channel to
listen, or let the scanner park the radio on whoever keys up.

```
Ch  Level     Code              Who     Last
17  ████████  141.3 Hz (22)     Family  now      <- busy, code confirmed
3   ░░░░░░░░  D261N (75)        Kids    23s      <- held: last code heard on ch 3
19  ░░░░░░░░  100.0 Hz (12)             2m
```

* **Board** – one row per channel: carrier level, code (yellow `?` = tentative,
  green = confirmed, grey = held from the last transmission), preset name,
  last heard. Hover a row for details (measured tone, DCS correlation, the
  inverted-polarity reading, AFC offset).
* **Channel keys** – a button for every channel: 1–7, 8–14, 15–22 and the eight
  repeater inputs 15R–22R. Click one and the stock *Radio* module is tuned
  there. Busy keys turn green; the key the Radio sits on is blue.
* **Scanner** – parks the Radio on whatever keys up, stays through the
  transmission plus a hang time, then resumes — silently, no squelch fiddling.
  Scan one block or hop between both.
* **Log** – every key-down with time, channel, code, duration, peak level and
  preset name; CSV export.
* **Presets** – name a channel + code pair ("Kids: ch 3 DCS 261"); matches show
  in the *Who* column and can flash the row.

## Install (macOS, Apple Silicon)

1. Download the latest zip from the **[Releases](../../releases)** page. Each
   release is SDR++ itself (built from
   [AlexandreRouma/SDRPlusPlus](https://github.com/AlexandreRouma/SDRPlusPlus)
   master — the exact commit is in the release title; the only change is one
   line in the default config that enables this module) with this module
   built in, plus the RTL-SDR, Airspy, Airspy HF+, HackRF and File
   sources and the PortAudio audio sink.
2. Unzip, move `SDR++.app` to Applications. The app is unsigned, so once:
   ```sh
   xattr -dr com.apple.quarantine /Applications/SDR++.app
   ```
3. Open SDR++. On a fresh install the *FRS/GMRS Monitor* section is already
   in the left menu. If you had SDR++ configured before (an existing
   `config.json`), add it once: **Module Manager** → pick `frsgmrs_monitor`,
   give it a name (e.g. `FRS/GMRS Monitor`), press **+**.

Why a whole app and not a drop-in file: SDR++ has no stable module ABI, so a
module only loads into the exact SDR++ build it was compiled with. The
official nightly is rebuilt from master on every push, and the v1.2.1 release
is too old. Shipping SDR++ + module together is how SDR++ modules are
distributed. Other platforms: see *Building from source* below.

**Hardware:** any SDR that does ≥ 250 kS/s (RTL-SDR, Airspy, HackRF,
SDRplay …). The module needs a 200 kHz-wide VFO to see a whole block.

## Quick start

1. Pick your source (RTL-SDR: any rate from 1.024 MS/s up is fine) and press
   **Play**.
2. In *FRS/GMRS Monitor*, press **Center SDR on block**. The status line says
   *block covered* when the module's wide VFO sits over the whole channel comb.
3. Press **Scan**. Whoever keys up next, you'll hear, and the board shows their
   channel and privacy code.

Or skip the clicks entirely — see *Start playing and scanning automatically*.

## Using it

### The board

A row lights green when a carrier is detected. The code appears after ~1 s as
`D261N (75)?` (tentative), turns solid green when two consecutive readings
agree, and stays grey (held) for *Hold last code* minutes after the
transmission ends so you can see who was there. The *Who* column shows the
preset name for that channel + code, *Last* shows when the channel was last
busy (`scan` = the scanner is parked here, `radio` = the Radio is tuned here).

### Blocks

The 22 channels sit in two groups 5 MHz apart, and one SDR covers one at a
time:

* **462 MHz** — channels 1–7 and 15–22. This is where almost everything
  happens: every simplex channel most radios use, and the GMRS *repeater
  outputs*. Default.
* **467 MHz** — channels 8–14 (0.5 W only) and the repeater *inputs* 15R–22R.

Switch with the *Block* combo, by clicking a dimmed key, or let the scanner hop
(*Both blocks*). Each block keeps its own held codes and last-heard times.

### Listening

Click a channel key, double-click a board row, or select a row and press
*Listen*. The *Radio* module is tuned to the channel in NFM (12.5 kHz for 1–14,
20 kHz for the GMRS main/repeater channels). Keys in the other block are
dimmed; clicking one re-centres the SDR there.

### Scanner

*Scan* parks the Radio on the first busy channel (watched presets first), shows
*Stopped on ch 17 141.3 Hz (22) [Family]*, and resumes *Scan hang time* seconds
after the carrier drops. With *Both blocks* the SDR retunes to the other block
every *Scan dwell* seconds while nothing is heard, so all 30 channels are
covered.

While nothing is parked the Radio is muted — the module closes the Radio's
squelch and restores your own squelch setting the moment something keys up —
so scanning is silent instead of FM hiss (*Options → Mute Radio while
scanning* turns that off if you'd rather run the Radio's own squelch).

Right-click a key or row → *Skip in scan* to lock a channel out (shown in
red). Clicking a key or *Listen* stops the scan, like pressing a channel on a
hardware scanner.

### Log, presets, keys

The **Log**, **Presets** and **Keys** checkboxes show the log window, the preset
editor and the channel keys. *Export CSV* writes `frsgmrs_log_<date>.csv` into
the SDR++ config folder. In the presets window, *Add from selected channel*
captures whatever the selected row is showing; the right-click menu on any
channel does the same.

### Start playing and scanning automatically

SDR++'s own command line rejects unknown flags, so the module can't add any —
but it can press Play itself and start the scanner once the SDR is running.
Two switches under **Options** (*Start the SDR when SDR++ opens*, *Start
scanning when the SDR starts*) do it from the saved config; environment
variables do the same from a launcher or shortcut and win over the config:

```sh
# macOS
FRSGMRS_AUTOPLAY=1 FRSGMRS_SCAN=1 FRSGMRS_SCAN_MODE=both open -a SDR++
# Linux
FRSGMRS_AUTOPLAY=1 FRSGMRS_SCAN=1 FRSGMRS_BLOCK=462 sdrpp
```

| Variable | Values | Effect |
|---|---|---|
| `FRSGMRS_AUTOPLAY` | `1` / `0` | press Play ~1.5 s after start-up (`sdrpp --autostart` also works) |
| `FRSGMRS_SCAN` | `1` / `0` | centre the SDR on the block and turn *Scan* on as soon as the SDR is running |
| `FRSGMRS_BLOCK` | `462` / `467` | which block to start on |
| `FRSGMRS_SCAN_MODE` | `this` / `both` | scan this block only, or hop between both |

A dedicated config folder (`sdrpp -r ~/.config/sdrpp-scanner`) with both
Options ticked makes a one-shot "turn on and listen" setup with no variables at
all; the source and gain come from that folder's normal SDR++ config.

## Reading the codes

### Privacy-code numbers

Consumer radios number codes 1–38 (CTCSS tones) and 39–121 (DCS codes, in the
usual Motorola/Midland order). The module shows both the real value and the
number: `141.3 Hz (22)`, `D261N (75)`. Brands occasionally disagree on the DCS
numbering, so check your radio's manual against the real code (`D261N`) when
it matters.

### DCS polarity (check once)

Every DCS code has a "normal" and an "inverted" form (`D261N` reads as `D732N`
inverted; privacy 75 ↔ 118). Which one you see depends on the sign convention
of the SDR's FM demodulator versus your radio's. Key up a radio with a known
DCS code and look at the board:

* reads correctly → nothing to do;
* reads the inverted pair (the row tooltip's *if your radio reads it inverted*
  line matches your radio) → tick **Options → Invert DCS polarity** once; it
  stays in the config.

CTCSS tones have no polarity.

### Repeaters

* GMRS repeaters **transmit on channels 15–22** (462.550–462.725 MHz) and
  **listen on 15R–22R** (467.550–467.725 MHz). What you hear from a repeater is
  its output, so the 462 block already covers "who's on the repeater", and the
  code shown on ch 15–22 while the repeater is keyed is the repeater's *output*
  tone (if it sends one — many do, some don't).
* To use a repeater your radio must transmit its *input* tone, which is not
  necessarily the same as the output tone. The input tone is only visible on
  15R–22R (the 467 block), and only when a user is close enough to be heard
  directly. Scan *Both blocks*, or click `17R` while a conversation is going
  on, to catch it. The right-click menu on any main/repeater channel jumps to
  its pair.
* Published repeater listings (myGMRS.com) give both tones; this lets you
  verify them.

## Options

| Option | What it does |
|---|---|
| Carrier threshold | dB above the per-channel noise floor that counts as a carrier (8 dB default; raise it in a noisy area, lower it for weak toys) |
| AFC | follows radios up to ±3 kHz off frequency (cheap toys often are) |
| Invert DCS polarity | see above |
| Alert on watched presets | flashes the row when a watched preset is heard |
| Hold last code | how long a code stays on the board after the transmission (1–60 min) |
| Start the SDR when SDR++ opens / Start scanning when the SDR starts | start-up automation, see above |
| Mute Radio while scanning | close the Radio squelch between transmissions (default on) |
| Scan hang time | seconds to stay on a channel after its carrier drops |
| Scan dwell per block | seconds to listen on each block before hopping (*Both blocks*) |

Settings, presets and scan skips live in
`<sdrpp config folder>/frsgmrs_monitor_config.json`.

## Known limits

* One block at a time per SDR. The two blocks are 5 MHz apart and a 200 kHz
  VFO can't straddle them; *Both blocks* scanning hops instead.
* A strong 20 kHz GMRS signal on a main channel can splatter into the adjacent
  12.5 kHz interstitial channel and show a weak carrier there; the carrier
  threshold and the *Level* bar make this obvious.
* DCS needs ~1 s of clean carrier before a reading; CTCSS ~0.5 s. Very short
  "chirp" key-downs log as `(no code)`.
* 467 MHz repeater inputs are mobiles and handhelds anywhere in the repeater's
  footprint — you only hear the ones near you.
* The SDR++ File source ignores retuning, so with a recording the 467 block
  shows the 462 recording relabelled. That's the file source, not the module.

## Building from source

SDR++ has no stable module ABI: the module must be compiled against the exact
SDR++ build it will run in, so it is built *inside* the SDR++ source tree.

### 1. Get SDR++ and wire the module in

```sh
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
git clone https://github.com/greglamb/SDRPlusPlus-GMRS-Plugin.git sdrpp-frsgmrs-monitor
sh sdrpp-frsgmrs-monitor/tools/install_in_tree.sh SDRPlusPlus
```

The script symlinks the module into `SDRPlusPlus/decoder_modules/frsgmrs_monitor`,
adds `OPT_BUILD_FRSGMRS_MONITOR` (ON) to SDR++'s root `CMakeLists.txt`, and adds
the module to `make_macos_bundle.sh`. It's idempotent; `--copy` copies instead
of symlinking.

### 2a. macOS

```sh
brew install cmake pkg-config fftw glfw volk zstd librtlsdr airspy hackrf portaudio
cd SDRPlusPlus
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_FRSGMRS_MONITOR=ON \
      -DOPT_BUILD_PLUTOSDR_SOURCE=OFF -DOPT_BUILD_AUDIO_SINK=OFF -DOPT_BUILD_PORTAUDIO_SINK=ON -DUSE_BUNDLE_DEFAULTS=ON
cmake --build build -j
sh make_macos_bundle.sh build ./SDR++.app
open ./SDR++.app
```

### 2b. Linux

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

### Releases (CI)

The **Build macOS (Apple Silicon)** workflow under *Actions* does the macOS
build on a GitHub runner. Pushing a `v*` tag publishes a release with the zip
attached and the bundled SDR++ version/commit in the title; *Run workflow* with
a `release_tag` does the same by hand, and with an empty tag just produces a
run artifact. The `sdrpp_ref` input pins which SDR++ commit to build against.

### Tests

```sh
# detector parity (python3 + numpy/scipy, fftw3f, a C++17 compiler)
cmake -S tools -B build-tools -DCMAKE_BUILD_TYPE=Release && cmake --build build-tools
python3 tests/run_parity.py            # 159 cases; --quick for a subset
python3 -m pytest tests/oracle         # the Python reference decoder's own tests

# whole-module replay: synthesise a busy 462 block, play it through the File source
python3 tests/make_iq.py /tmp/iq       # writes baseband_462637500Hz_*.wav + truth.json
# build SDR++ with -DOPT_BUILD_PACED_NULL_SINK=ON (tools/install_in_tree.sh --with-paced-sink),
# select File source + the "Paced Null" audio sink, Play, and compare the log with truth.json
```

The detector is a C++ port of the `dcsdecode` Python reference (DCS by
fold-and-correlate against all 104 codes, CTCSS by percentile spectrum),
verified sample-for-sample against it by `tests/run_parity.py`.

### Repository layout

```
src/main.cpp              the module: VFO, board, keys, scanner, log, presets
src/channel_bank.h        one DSP sink that demodulates all 15 channels of a block
src/frs_tables.h          channel plan, blocks, repeater pairs, labels
src/detect/               CTCSS/DCS detector (tables, DSP helpers, detector.cpp)
src/radio_iface_compat.h  SDR++ Radio module interface enums (so it builds out-of-tree)
tools/install_in_tree.sh  wires the module into an SDR++ source tree
tools/parity_test.cpp     standalone harness: streams a WAV through the detector
tools/paced_null_sink/    SDR++ sink that paces audio to real time (headless tests)
tests/oracle/dcsdecode.py the Python reference decoder (+ its unit tests)
tests/run_parity.py       C++ vs Python vs ground truth over a synthetic corpus
tests/make_iq.py          synthesises an IQ recording of a busy 462 block
tests/corpus/             a real recording: eKids toy radio, ch 12, DCS 261
.github/workflows/        the macOS build/release workflow
```

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). The module links against and the
releases bundle [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus), © Alexandre
Rouma, also GPL-3.0. `src/radio_iface_compat.h` is a copy of SDR++'s
`radio_interface.h`. Everything else is original.
