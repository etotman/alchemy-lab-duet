#!/usr/bin/env bash
#
# Render and validate this firmware's HostLink descriptor — no hardware, no
# flashing.  Catches the failure where a module boots and sounds fine but the
# web programmer hangs at "reconnect and verify firmware".
#
#   tools/descriptor_probe/run.sh                 # the Makefile's default FW
#   tools/descriptor_probe/run.sh duet            # a specific firmware
#   tools/descriptor_probe/run.sh duet --json   # also dump the JSON
#
# Exits non-zero if anything is wrong, so it works as a pre-flash gate.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SDK="$ROOT/lib/alchemy-sdk"
OUT="$ROOT/build/descriptor-probe"      # build/ is gitignored
LIB="$OUT/host/framework/libalchemy_primitives.a"

# Firmware: first non-flag argument, else the Makefile's `FW ?=` default.
FW=""
show_json=0
for arg in "$@"; do
    if [[ "$arg" == "--json" ]]; then show_json=1
    else FW="$arg"
    fi
done
if [[ -z "$FW" ]]; then
    FW="$(sed -n 's/^FW *?= *//p' "$ROOT/Makefile" | head -1)"
fi

FW_DIR="$ROOT/src/$FW"
MIRROR="$HERE/mirrors/$FW.cpp"
[[ -d "$FW_DIR" ]] || { echo "no firmware at src/$FW"; exit 1; }
[[ -f "$MIRROR" ]] || {
    echo "no descriptor mirror at tools/descriptor_probe/mirrors/$FW.cpp."
    echo "Copy an existing one and edit its MIRROR block to match src/$FW."
    exit 1
}
echo "firmware: $FW"
echo

echo "── static checks (reads src/$FW directly) ──"
python3 "$HERE/check.py" static "$FW_DIR"

echo
echo "── building host descriptor layer ──"
mkdir -p "$OUT"
if [[ ! -f "$LIB" ]]; then
    cmake -S "$SDK" -B "$OUT/host" -G Ninja > "$OUT/cmake.log" 2>&1 \
        || { echo "cmake configure failed; see $OUT/cmake.log"; exit 1; }
    cmake --build "$OUT/host" --target alchemy_primitives > "$OUT/build.log" 2>&1 \
        || { echo "cmake build failed; see $OUT/build.log"; exit 1; }
    echo "  built $(basename "$LIB")"
else
    echo "  reusing $(basename "$LIB") (delete $OUT to rebuild)"
fi

# The descriptor layer is board-free; the host build uses the v1 headers and
# the libDaisy stubs.  Sources mirror tests/host/CMakeLists.txt.
clang++ -std=gnu++17 -O0 -g -o "$OUT/probe-$FW" \
    -I"$SDK/framework/include" \
    -I"$SDK/hardware/include" \
    -I"$SDK/hardware/alchemy-lab/v1/include" \
    -I"$SDK/stubs" \
    "$MIRROR" \
    "$SDK"/framework/src/host_link/{descriptor,describe,json_check}.cpp \
    "$SDK"/framework/src/surface/{presets,virtual_knob,pager,settings,button_bank,preset_gesture_ui}.cpp \
    "$SDK"/framework/src/control/param_lock_manager.cpp \
    "$SDK"/hardware/alchemy-lab/v1/src/{alchemy_lab_v1,alchemy_lab_v1_layout,ws2812}.cpp \
    "$LIB"
echo "  compiled mirrors/$FW.cpp"

"$OUT/probe-$FW" > "$OUT/$FW-descriptor.json"

echo
echo "── descriptor checks ──"
python3 "$HERE/check.py" json "$OUT/$FW-descriptor.json"

if (( show_json )); then
    echo
    echo "── descriptor ──"
    python3 -m json.tool "$OUT/$FW-descriptor.json"
fi

echo
echo "descriptor written to $OUT/$FW-descriptor.json"
