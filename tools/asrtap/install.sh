#!/bin/sh
# Install the asrtap ALSA plugins + asound.conf entries onto the board.
# Idempotent: safe to re-run.  Usage:  ./install.sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "== pushing plugins to /usr/lib/alsa-lib/"
adb push "$DIR/aarch64/libasound_module_pcm_asrtap.so" \
         "$DIR/aarch64/libasound_module_pcm_asrtap_c.so" /usr/lib/alsa-lib/

echo "== asound.conf entries (idempotent)"
adb shell '
CONF=/etc/asound.conf
grep -q "pcm.asrtap " $CONF 2>/dev/null || {
    cp $CONF ${CONF}.bak-asrtap 2>/dev/null || true
    cat >> $CONF << "EOF"

# --- ASR tap: tee playback PCM into a shm ring for live subtitles (asrtap) ---
pcm.asrtap {
    type asrtap
    slave.pcm "hw:0,0"
}
pcm.asrtap_auto {
    type plug
    slave {
        pcm "asrtap"
        rate 48000
    }
}
pcm.asr_capture {
    type asrtap_c
    start_backlog 1
}
EOF
    echo "asound.conf: tap entries appended (backup at ${CONF}.bak-asrtap)"
}
grep -c "pcm.asrtap\|pcm.asr_capture" $CONF
'

echo "== sanity"
# NOTE: this board's aplay -L does NOT enumerate custom pcm names (they
# still work), so verify functionally.  The arecord probe MUST carry an
# explicit timeout: on a DRY ring a naive reader waits for data forever
# (its -d is data duration, not wall clock), and without format flags it
# negotiates 8k/U8 which the node rejects.
adb shell '
n=$(grep -c "^pcm\.asrtap \|^pcm\.asrtap_auto \|^pcm\.asr_capture " /etc/asound.conf)
echo "asound.conf entries: $n/3"
p=$(timeout 3 arecord -D asr_capture -f S16_LE -r 16000 -c 1 -d 1 /dev/null 2>&1 | head -1)
case "$p" in
    "") echo "asr_capture: opens and streams OK";;
    *not\ found*) echo "asr_capture: node OK (ring not created yet -- run any playback once)";;
    *) echo "asr_capture: $p";;
esac'
echo "done.  players: audio-sink=\"alsasink device=asrtap_auto\"; recogniser opens: asr_capture"
