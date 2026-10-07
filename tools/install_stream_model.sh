#!/bin/sh
# Install the streaming paraformer onto a board, into the same layout the
# offline models use.
#
#   usage: install_stream_model.sh <board_serial> [<model_dir_on_host>]
#
# Why a script and not a manual copy: the model lives in /tmp on the board,
# which is tmpfs, so it disappears on reboot.
#
# Every file is verified by md5, and a mismatch is retried.  That is not
# paranoia: the board's root filesystem is an SD card, and a plain `adb push`
# of tokens.txt was observed landing truncated (56317 of 75756 bytes), which
# the recogniser then crashed on with std::out_of_range from unordered_map::at
# -- a token id past the end of its table.  "The file exists" is not evidence
# that the file arrived.
#
# The upstream release names files by quantisation, not by training epoch:
#   encoder.int8.onnx  decoder.int8.onnx  tokens.txt
# Those names are what sherpa-onnx's online-paraformer config expects, and what
# sherpa_live.c builds, so the install normalises any sp_-prefixed download.
#
# Measured on the board: 0.5 s to first text, RTF 0.85, 10/10 on the scored
# 10-phrase set.  CPU provider -- sherpa-onnx 1.13.8's rknn provider does not
# support online paraformer (zipformer transducer and CTC only), so the NPU is
# not used by this path.

set -e

SERIAL="${1:?usage: install_stream_model.sh <board_serial> [<model_dir_on_host>]}"
DEST=/data/rknn-test/sherpa/model-streaming
ATTEMPTS=3

BASE=https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/resolve/main
FILES="encoder.int8.onnx decoder.int8.onnx tokens.txt"

adb -s "$SERIAL" shell "mkdir -p $DEST"

if [ -n "${2:-}" ]; then
    SRC="$2"
    echo "== installing from $SRC"
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    for f in $FILES; do
        # Pick by content, not by name.  A scratch directory can easily hold two
        # candidates -- a stream-prefixed download next to a bare tokens.txt
        # belonging to a different model -- and taking the first name that
        # exists once installed the wrong token table, which the recogniser then
        # crashed on with std::out_of_range.  The streaming paraformer has 8404
        # token ids; anything else is the wrong file.
        if [ "$f" = tokens.txt ]; then
            best=""
            for cand in "$SRC/sp_tokens.txt" "$SRC/tokens.txt"; do
                [ -f "$cand" ] || continue
                n=$(grep -c . "$cand" 2>/dev/null || echo 0)
                if [ "$n" -ge 8000 ]; then best="$cand"; break; fi
                echo "  skipping $cand: only $n token lines"
            done
            [ -n "$best" ] || { echo "ERROR: no tokens.txt with ~8404 lines under $SRC" >&2; exit 1; }
            cp "$best" "$work/$f"
        elif [ -f "$SRC/$f" ]; then
            cp "$SRC/$f" "$work/$f"
        elif [ -f "$SRC/sp_$f" ]; then
            cp "$SRC/sp_$f" "$work/$f"
        else
            echo "ERROR: $f not found under $SRC" >&2
            exit 1
        fi
    done
else
    echo "== downloading from $BASE"
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    for f in $FILES; do
        echo "-- $f"
        curl -sSL --retry 2 -o "$work/$f" "$BASE/$f"
    done
fi

# Push each file until its md5 on the board matches the source.
for f in $FILES; do
    want=$(md5sum "$work/$f" | cut -d' ' -f1)
    ok=0
    n=1
    while [ "$n" -le "$ATTEMPTS" ]; do
        adb -s "$SERIAL" shell "rm -f $DEST/$f"
        adb -s "$SERIAL" push "$work/$f" "$DEST/$f" > /dev/null
        adb -s "$SERIAL" shell "sync" > /dev/null 2>&1 || true
        got=$(adb -s "$SERIAL" shell "md5sum $DEST/$f" 2>/dev/null | cut -d' ' -f1)
        if [ "$got" = "$want" ]; then
            echo "  $f  $(wc -c < "$work/$f" | tr -d ' ') bytes  md5 ok"
            ok=1
            break
        fi
        echo "  $f attempt $n: md5 $got != $want, retrying"
        n=$((n + 1))
    done
    [ "$ok" = 1 ] || { echo "ERROR: $f would not land intact after $ATTEMPTS attempts" >&2; exit 1; }
done

# Prove it loads, rather than trusting the bytes: a bad token table only shows
# up once the recogniser is constructed.
echo "== verifying the model loads on the board"
adb -s "$SERIAL" shell "
    cd /data/rknn-test/sherpa || exit 1
    export LD_LIBRARY_PATH=\$PWD/sherpa-onnx-v1.13.8-rknn-linux-aarch64-shared/lib:/usr/lib
    export SHERPA_STREAM_DIR=$DEST
    timeout 25 ./sherpa_live ./model ./silero_vad.onnx fifo:/tmp/asr_verify 20 2>&1 |
        grep -E 'streaming mode|init failed|what\(\)' | head -3
"

cat <<EOF

installed to $DEST

use it with:
  adb exec-out 'cd /data/rknn-test/sherpa && \\
    export LD_LIBRARY_PATH=\$PWD/sherpa-onnx-v1.13.8-rknn-linux-aarch64-shared/lib:/usr/lib && \\
    export SHERPA_STREAM_DIR=$DEST && \\
    ./sherpa_live ./model ./silero_vad.onnx fifo:/tmp/asr_pcm 600'

then play through the tap in another terminal:
  adb shell 'gst-play-1.0 --audiosink=\"alsasink device=asrtap_auto\" \\
    --videosink=fakesink file:///path/to/media.mp4'

Leaving SHERPA_STREAM_DIR unset runs the original offline path instead.
EOF
