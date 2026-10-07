#!/usr/bin/env bash
# setup_models.sh -- download the rk3576 ASR models to ./models/ for board
# deployment.  Models are NOT stored in git (encoder.rknn 335 MB exceeds the
# GitHub 100 MB per-file limit); silero_vad.onnx (2.3 MB) IS vendored in
# sherpa/lib/ and this script only downloads the two paraformer windows.
#
# Usage: ./setup_models.sh [output_dir]   (default ./models)
set -euo pipefail

OUT=${1:-models}
BASE=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models
mkdir -p "$OUT"
cd "$OUT"

fetch() {
    local pkg=$1
    if [ -d "$pkg" ]; then echo "skip: $pkg (already present)"; return; fi
    echo "== downloading $pkg =="
    curl -L --fail --retry 3 -o "$pkg.tar.bz2" "$BASE/$pkg.tar.bz2"
    tar xjf "$pkg.tar.bz2"
    rm "$pkg.tar.bz2"
}

fetch sherpa-onnx-rk3576-10-seconds-paraformer-zh-2025-10-07
fetch sherpa-onnx-rk3576-20-seconds-paraformer-zh-2025-10-07

echo
echo "board layout (matches the README run commands):"
echo "  adb push sherpa_live <dir>/"
echo "  adb push $OUT/sherpa-onnx-rk3576-10-seconds-paraformer-zh-2025-10-07 <dir>/model"
echo "  adb push $OUT/sherpa-onnx-rk3576-20-seconds-paraformer-zh-2025-10-07 <dir>/model-20s"
echo "  adb push sherpa/lib/silero_vad.onnx <dir>/"
