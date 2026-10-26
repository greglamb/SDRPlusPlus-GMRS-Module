#!/bin/sh
# Wire this module into an SDR++ source tree so it builds with SDR++ itself.
#
#   tools/install_in_tree.sh /path/to/SDRPlusPlus [--copy] [--with-paced-sink]
#
# - links (or copies, with --copy) the module into decoder_modules/frsgmrs_monitor
# - adds OPT_BUILD_FRSGMRS_MONITOR (default ON) to SDR++'s root CMakeLists.txt
# - teaches make_macos_bundle.sh to put the module into SDR++.app
# - with --with-paced-sink also wires tools/paced_null_sink (headless tests only)
#
# Idempotent: safe to run again after pulling either repo. Works with the
# stock macOS /bin/sh and Linux dash/bash.
set -e

SDRPP=$1
[ -n "$SDRPP" ] || { echo "usage: $0 /path/to/SDRPlusPlus [--copy] [--with-paced-sink]" >&2; exit 2; }
shift
MODE=link; SINK=no
for a in "$@"; do
    case "$a" in
        --copy) MODE=copy ;;
        --with-paced-sink) SINK=yes ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

HERE=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$SDRPP/CMakeLists.txt" ] && [ -d "$SDRPP/decoder_modules" ] || { echo "not an SDR++ source tree: $SDRPP" >&2; exit 1; }

place() {   # place <src dir> <dest dir>
    rm -rf "$2"
    if [ "$MODE" = copy ]; then cp -R "$1" "$2"; else ln -s "$1" "$2"; fi
}

place "$HERE" "$SDRPP/decoder_modules/frsgmrs_monitor"
echo "module -> $SDRPP/decoder_modules/frsgmrs_monitor ($MODE)"

if ! grep -q OPT_BUILD_FRSGMRS_MONITOR "$SDRPP/CMakeLists.txt"; then
    cat >> "$SDRPP/CMakeLists.txt" <<'EOF'

# --- FRS/GMRS Monitor (added by sdrpp-frsgmrs-monitor/tools/install_in_tree.sh)
option(OPT_BUILD_FRSGMRS_MONITOR "Build the FRS/GMRS monitor module" ON)
if (OPT_BUILD_FRSGMRS_MONITOR)
add_subdirectory("decoder_modules/frsgmrs_monitor")
endif (OPT_BUILD_FRSGMRS_MONITOR)
EOF
    echo "CMakeLists.txt: added OPT_BUILD_FRSGMRS_MONITOR"
fi

if [ -f "$SDRPP/make_macos_bundle.sh" ] && ! grep -q frsgmrs_monitor "$SDRPP/make_macos_bundle.sh"; then
    awk '{ print }
         /decoder_modules\/radio\/radio\.dylib/ && !done {
             print "bundle_install_binary $BUNDLE $BUNDLE/Contents/Plugins $BUILD_DIR/decoder_modules/frsgmrs_monitor/frsgmrs_monitor.dylib || true"
             done = 1 }' "$SDRPP/make_macos_bundle.sh" > "$SDRPP/make_macos_bundle.sh.tmp"
    mv "$SDRPP/make_macos_bundle.sh.tmp" "$SDRPP/make_macos_bundle.sh"
    chmod +x "$SDRPP/make_macos_bundle.sh"
    echo "make_macos_bundle.sh: will bundle frsgmrs_monitor.dylib"
fi

if [ "$SINK" = yes ]; then
    place "$HERE/tools/paced_null_sink" "$SDRPP/sink_modules/paced_null_sink"
    if ! grep -q OPT_BUILD_PACED_NULL_SINK "$SDRPP/CMakeLists.txt"; then
        cat >> "$SDRPP/CMakeLists.txt" <<'EOF'

# --- paced null sink, for headless replay tests (sdrpp-frsgmrs-monitor)
option(OPT_BUILD_PACED_NULL_SINK "Build the paced null sink (tests)" OFF)
if (OPT_BUILD_PACED_NULL_SINK)
add_subdirectory("sink_modules/paced_null_sink")
endif (OPT_BUILD_PACED_NULL_SINK)
EOF
    fi
    echo "paced null sink wired (enable with -DOPT_BUILD_PACED_NULL_SINK=ON)"
fi

echo "done. configure SDR++ as usual, e.g.:"
echo "  cmake -S $SDRPP -B $SDRPP/build -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_FRSGMRS_MONITOR=ON"
