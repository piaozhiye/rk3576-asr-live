#!/usr/bin/env bash
# build.sh -- cross-compile sherpa_live for the RK3576 board.
#
# All three prebuilt aarch64 libraries are vendored in sherpa/lib/:
#   libsherpa-onnx-c-api.so + libonnxruntime.so (sherpa-onnx v1.13.8 rknn build)
#   librknnrt.so (Rockchip RKNPU runtime 2.2.0)
# Override via env if you want different copies:
#   SHERPA_LIB_DIR / RKNNRT_LIB
# and an aarch64 cross toolchain on PATH (aarch64-linux-gnu-gcc or the
# buildroot one; see TOOLCHAIN below).
#
# Usage: ./build.sh            # cross-compile -> sherpa_live
#        ./build.sh --test     # build the host unit tests
#        ./build.sh --native   # compile on the board itself (gcc + libs there)
set -euo pipefail

cd "$(dirname "$0")"

OUT=sherpa_live
# Array, not a string: the sources are passed as separate arguments to gcc.
SRC=(sherpa/sherpa_live.c sherpa/window_plan.c sherpa/subtitle.c sherpa/pass2.c)
INC=(-Isherpa)

# ---- locate a cross toolchain ------------------------------------------
find_cc() {
    for cc in aarch64-linux-gnu-gcc aarch64-none-linux-gnu-gcc; do
        command -v "$cc" >/dev/null 2>&1 && { echo "$cc"; return; }
    done
    # buildroot-style trees
    for d in /opt/*gcc-arm* /opt/*aarch64* ~/projects/toolchains/*; do
        [ -x "$d/bin/aarch64-buildroot-linux-gnu-gcc" ] && { echo "$d/bin/aarch64-buildroot-linux-gnu-gcc"; return; }
    done
    echo ""
}

# ---- host unit tests (no board, no NPU, no ALSA) ----------------------
# window_plan holds the segment-length policy, so it is testable on the host.
if [ "${1:-}" = "--test" ]; then
    CC_HOST=${HOSTCC:-gcc}
    "$CC_HOST" -O2 -Wall -o "${TEST_OUT:-sherpa/test_window_plan}" \
        sherpa/test_window_plan.c sherpa/window_plan.c
    "$CC_HOST" -O2 -Wall -o sherpa/test_subtitle \
        sherpa/test_subtitle.c sherpa/subtitle.c
    "$CC_HOST" -O2 -Wall -o sherpa/test_pass2 \
        sherpa/test_pass2.c sherpa/pass2.c
    echo "run: sherpa/test_window_plan && sherpa/test_subtitle && sherpa/test_pass2"
    exit 0
fi

# ---- native build on the board -----------------------------------------
if [ "${1:-}" = "--native" ]; then
    echo "== native build on the board =="
    CC=${CC:-gcc}
    SHERPA_LIB_DIR=${SHERPA_LIB_DIR:-sherpa/lib}
    $CC -O2 -Wall -o "$OUT" "${SRC[@]}" "${INC[@]}" \
        -L"$SHERPA_LIB_DIR" -lsherpa-onnx-c-api -lonnxruntime \
        -L"${RKNNRT_LIB_DIR:-/usr/lib}" -l:librknnrt.so -lasound -lm -lpthread
    echo "OK: ./$OUT"
    exit 0
fi

# ---- cross build on the host -------------------------------------------
CC=${CC:-$(find_cc)}
[ -z "$CC" ] && { echo "ERROR: no aarch64 cross gcc on PATH; set CC=... (see header)"; exit 1; }
echo "== cross build with $CC =="

# buildroot toolchains ship their own libmpfr/libgmp for cc1; the driver
# needs them on LD_LIBRARY_PATH or cc1 dies with "cannot open libmpfr.so.4".
CC_ROOT=$(dirname "$(dirname "$(readlink -f "$CC")")")
if [ -d "$CC_ROOT/lib" ] && ls "$CC_ROOT"/lib/libmpfr* >/dev/null 2>&1; then
    export LD_LIBRARY_PATH="$CC_ROOT/lib:$CC_ROOT/libexec/gcc"*/9.3.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi

SHERPA_LIB_DIR=${SHERPA_LIB_DIR:-sherpa/lib}
RKNNRT_LIB=${RKNNRT_LIB:-sherpa/lib/librknnrt.so}
[ -f "$SHERPA_LIB_DIR/libsherpa-onnx-c-api.so" ] || { echo "ERROR: $SHERPA_LIB_DIR/libsherpa-onnx-c-api.so missing"; exit 1; }
[ -f "$RKNNRT_LIB" ] || { echo "ERROR: $RKNNRT_LIB missing"; exit 1; }
RKNN_DIR=$(dirname "$(readlink -f "$RKNNRT_LIB")")

$CC -O2 -Wall -o "$OUT" "${SRC[@]}" "${INC[@]}" \
    -L"$SHERPA_LIB_DIR" -lsherpa-onnx-c-api -lonnxruntime \
    -L"$RKNN_DIR" -l:librknnrt.so \
    -lasound -lm -lpthread

file "$OUT" | grep -q aarch64 && echo "OK: ./$OUT (aarch64)" || { echo "ERROR: output is not aarch64"; exit 1; }

# ---- sub-overlay (wayland subtitle band) --------------------------------
# The xdg-shell protocol header AND code are scanner output, not library
# content: without the generated .c the link fails on xdg_*_interface, and
# without the generated .h the include fails.  Both are derived in a temp
# dir so the tree stays clean.
OVL_OUT=${OVL_OUT:-tools/sub-overlay}
SYSROOT=$(dirname "$(dirname "$CC")")/aarch64-buildroot-linux-gnu/sysroot
SCANNER="$CC_ROOT/bin/wayland-scanner"
XDG_XML="$SYSROOT/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
if [ -x "$SCANNER" ] && [ -f "$XDG_XML" ]; then
    echo "== building $OVL_OUT =="
    OVLGEN=$(mktemp -d)
    "$SCANNER" client-header "$XDG_XML" "$OVLGEN/xdg-shell-client-protocol.h"
    "$SCANNER" private-code "$XDG_XML" "$OVLGEN/xdg-shell-protocol.c"
    $CC -O2 -Wall -I sherpa -I "$OVLGEN" -I "$SYSROOT/usr/include" \
        -I "$SYSROOT/usr/include/freetype2" \
        -o "$OVL_OUT" tools/sub-overlay.c sherpa/subtitle.c \
        "$OVLGEN/xdg-shell-protocol.c" \
        -L"$SYSROOT/usr/lib" -lwayland-client -lfreetype \
        2> >(grep -v "警告\|warning\|附注\|note:" >&2) || true
    rm -rf "$OVLGEN"
    if file "$OVL_OUT" | grep -q aarch64; then
        echo "OK: $OVL_OUT (aarch64)"
    else
        echo "WARN: $OVL_OUT not built (missing wayland/freetype in sysroot?)"
    fi
else
    echo "WARN: wayland-scanner or xdg-shell.xml not found under \$CC_ROOT; skipping $OVL_OUT"
fi

echo
echo "deploy: adb push $OUT <board_dir>/ && chmod +x"
