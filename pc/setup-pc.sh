#!/usr/bin/env bash
# setup-pc.sh -- one-shot bootstrap for the PC live-subtitle chain on a
# fresh clone.  Downloads the prebuilt x86_64 sherpa-onnx libs and the CPU
# models (~650 MB total), builds sherpa_live_pc with the host gcc, and
# writes pc/alsa.conf (self-contained: the run scripts export
# ALSA_CONFIG_PATH to it, so ~/.asoundrc is never touched).
#
# The asrtap ALSA plugins: prebuilt .so for three arches are committed
# under tools/asrtap/{aarch64,armhf,x86_64}/.  The plugin source is NOT
# shipped in this repo -- only the compiled modules.
#
# usage: ./pc/setup-pc.sh [--slave <alsa_pcm>]
#   --slave   which PCM the tap passes audio through (your speakers).
#             Default: "default"; on a raw-ALSA box find yours with aplay -l,
#             e.g. --slave plughw:1,0
#
# System packages first (root, once):
#   sudo apt install build-essential libasound2-dev ffmpeg python3-gi \
#        gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
#        gstreamer1.0-plugins-bad gstreamer1.0-libav fonts-noto-cjk
# (plugins-bad+libav: h264/aac decode incl. VAAPI; base: textoverlay)
set -euo pipefail
cd "$(dirname "$0")/.."

SLAVE=default
while [ $# -gt 0 ]; do
    case "$1" in
        --slave) SLAVE=${2:?}; shift 2 ;;
        --build-plugins) echo "--build-plugins: plugin source is not shipped in this repo (prebuilt only)" >&2; exit 1 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

log() { printf '\n== %s ==\n' "$*"; }

# ---- 0. toolchain sanity (fail early, not after 600 MB of downloads) ----
log "checking tools"
for c in gcc curl tar python3 ffmpeg; do
    command -v "$c" >/dev/null || { echo "ERROR: $c not found (see the apt line in the header)"; exit 1; }
done
python3 - << 'EOF' || { echo "ERROR: python3 gi/Gst missing: sudo apt install python3-gi gstreamer1.0-plugins-base"; exit 1; }
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst
Gst.init(None)   # without init the registry is empty and find() fails
for e in ("textoverlay", "autovideosink", "alsasink"):
    if not Gst.ElementFactory.find(e):
        raise SystemExit(f"gstreamer element missing: {e}")
EOF
echo "tools OK"

# ---- 1. asrtap plugins: use the committed prebuilts ------------------
log "asrtap ALSA plugins (x86_64 prebuilt in tools/asrtap/x86_64/)"
for f in tools/asrtap/x86_64/libasound_module_pcm_asrtap.so \
         tools/asrtap/x86_64/libasound_module_pcm_asrtap_c.so; do
    [ -f "$f" ] || { echo "ERROR: $f missing"; exit 1; }
done
echo "plugins OK"

# ---- 2. x86_64 sherpa-onnx runtime (v1.13.8, same version as the board) --
log "sherpa-onnx v1.13.8 linux-x64 libs -> pc/libs/"
LIBDIR=pc/libs/sherpa-onnx-v1.13.8-linux-x64-shared-lib/lib
if [ ! -f "$LIBDIR/libsherpa-onnx-c-api.so" ]; then
    mkdir -p pc/libs
    curl -fL --retry 3 -o /tmp/sherpa-x64.tar.bz2 \
        https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-linux-x64-shared-lib.tar.bz2
    tar xjf /tmp/sherpa-x64.tar.bz2 -C pc/libs
    rm /tmp/sherpa-x64.tar.bz2
fi
[ -f "$LIBDIR/libsherpa-onnx-c-api.so" ] || { echo "ERROR: libs did not land"; exit 1; }
echo "libs OK"

# ---- 3. models (~560 MB; huggingface.co first, hf-mirror.com fallback for
#         networks where hf.co is unreachable) ----------------------------
fetch() { # fetch <relpath> <repo> <file>
    local dest=$1 repo=$2 f=$3
    [ -s "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    # --connect-timeout: when huggingface.co is unreachable (e.g. DNS
    # hijack on this network) fail over to the mirror in seconds, not
    # after minutes of TCP timeouts per file.
    curl -fsSL --connect-timeout 15 --retry 2 -o "$dest" \
        "https://huggingface.co/$repo/resolve/main/$f" \
     || curl -fsSL --connect-timeout 15 --retry 3 -o "$dest" \
        "https://hf-mirror.com/$repo/resolve/main/$f" \
     || { echo "ERROR: could not fetch $f"; exit 1; }
    echo "  $dest  $(wc -c < "$dest") bytes"
}
log "streaming paraformer (draft) -> pc/model-streaming/"
fetch pc/model-streaming/encoder.int8.onnx \
      csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en encoder.int8.onnx
fetch pc/model-streaming/decoder.int8.onnx \
      csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en decoder.int8.onnx
fetch pc/model-streaming/tokens.txt \
      csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en tokens.txt
log "SenseVoice int8 (second pass) -> pc/model-sv/"
fetch pc/model-sv/model.int8.onnx \
      csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17 model.int8.onnx
fetch pc/model-sv/tokens.txt \
      csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17 tokens.txt
log "punctuation ct-transformer int8 -> pc/model-punct/"
# The official repo ships fp32 only; this mirror holds the int8 export.
fetch pc/model-punct/model.int8.onnx \
      lorneluo/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8 model.int8.onnx

# ---- 4. build sherpa_live_pc (host gcc; no cross toolchain involved) ----
log "building pc/sherpa_live_pc"
gcc -O2 -Wall -Isherpa -o pc/sherpa_live_pc \
    sherpa/sherpa_live.c sherpa/window_plan.c sherpa/subtitle.c sherpa/pass2.c \
    -L"$LIBDIR" -lsherpa-onnx-c-api -lonnxruntime -lasound -lm -lpthread

# ---- 5. self-contained ALSA config: plugins + pcms with absolute paths --
# No --slave given: pick the first analog playback card aplay -l reports
# (on a pipewire/pulse desktop "default" would also work; on raw ALSA it
# often points at silent HDMI, so analog is the safer guess).
if [ "$SLAVE" = default ]; then
    line=$(aplay -l 2>/dev/null | grep -m1 "Analog" || true)
    if echo "$line" | grep -q "card [0-9]*.*device [0-9]*"; then
        c=$(echo "$line" | sed 's/.*card \([0-9]*\).*/\1/')
        d=$(echo "$line" | sed 's/.*device \([0-9]*\).*/\1/')
        SLAVE="plughw:$c,$d"
        echo "(auto-detected speakers: $SLAVE)"
    fi
fi
log "writing pc/alsa.conf (slave: $SLAVE)"
cat > pc/alsa.conf << EOF
# generated by pc/setup-pc.sh -- self-contained; the run scripts export
# ALSA_CONFIG_PATH here so nothing is written to /etc/asound.conf or
# ~/.asoundrc.  Rerun setup-pc.sh to change the slave.
#
# The system config is included FIRST: name shortcuts the slave may use
# (default / plughw:N,M / dmix / ...) are defined there, not in libc.
</usr/share/alsa/alsa.conf>
pcm.asrtap {
    type asrtap
    slave.pcm "$SLAVE"
}
pcm.asrtap_auto {
    type plug
    slave {
        pcm asrtap
        rate 48000
    }
}
pcm.asr_capture {
    type asrtap_c
    start_backlog 1
}
# alsa-lib resolves pcm plugin types through absolute paths on Ubuntu, so
# LD_LIBRARY_PATH does not work -- point lib at the committed prebuilts.
pcm_type.asrtap {
    lib "$PWD/tools/asrtap/x86_64/libasound_module_pcm_asrtap.so"
}
pcm_type.asrtap_c {
    lib "$PWD/tools/asrtap/x86_64/libasound_module_pcm_asrtap_c.so"
}
EOF

# ---- 6. verify the whole chain opens (1 s of silence through the tap) ---
log "verifying: asrtap_auto (speakers) -> ring -> asr_capture"
if ALSA_CONFIG_PATH=$PWD/pc/alsa.conf aplay -q -D asrtap_auto \
        -f S16_LE -r 48000 -c 2 -d 1 /dev/zero 2>/tmp/setup-aplay.err \
   && [ -e /dev/shm/asrtap_ring ]; then
    echo "chain OK"
else
    echo "WARN: tap chain did not open -- wrong --slave? (aplay -l to list)"
    cat /tmp/setup-aplay.err
fi

cat << EOF

done.  run it:

    ./pc/run-live.sh <any video> [seconds]

artifacts (gitignored): pc/libs pc/model-* pc/alsa.conf pc/sherpa_live_pc
EOF
