#!/bin/sh
# supervisor.sh -- keep the subtitle chain up, in the right order.
#
#   ./supervisor.sh start [seconds]    # overlay -> recogniser, auto-restart
#   ./supervisor.sh stop
#
# Fixes the fragile choreography that produced every silent failure this
# round: fixed start order (overlay first, recogniser second, both mkfifo
# idempotent so order does not actually matter -- but it is deterministic),
# auto-restart when either process dies (zombie exit, SIGPIPE, deadline),
# and a restart counter so a crash loop is visible instead of silent.
#
# The video player is NOT managed here: play-sub starts it, and the tap
# plugin now retries its fifo writer so the recogniser can restart mid-play.

DIR=/data/rknn-test/sherpa
export LD_LIBRARY_PATH="$DIR/sherpa-onnx-v1.13.8-rknn-linux-aarch64-shared/lib:/usr/lib"
export XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0
export SHERPA_STREAM_DIR="$DIR/model-streaming"
# 第二遍默认 paraformer 双模型:术语更准(暂稳态 — SenseVoice 写"站稳态",
# 实测对比 2026-09-30),修正更快(0.3/0.65 s vs 0.95 s)。
# 日常口语/英日韩粤内容可换 SenseVoice(30 s 窗,自带标点):
#   export SHERPA_OFFLINE_DIR="$DIR/model-sv30"   # 并注释掉下面两行
export SHERPA_OFFLINE_DIR="$DIR/model"
export SHERPA_PASS2_LONG_DIR="$DIR/model-20s"
export SHERPA_SUBTITLE_FIFO=/tmp/asr_sub
export SHERPA_PUNCT_DIR="$DIR/model-punct"
export SHERPA_TERM_MAP="$DIR/term-map.txt"
FONT=/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Regular.otf

RECOG_SECS=${1:-0}
[ "$1" = "start" ] && RECOG_SECS=${2:-0}

# Instance control via pidfile + stop sentinel.  No process-NAME matching
# anywhere: on this board every pgrep -f/pkill -f pattern has bitten us
# (patterns match the invoking shell's own cmdline whenever the same line
# also names the script, and background-spawn subshells showed up as extra
# "loops").  A pidfile is exact.
PIDFILE=/tmp/asr_sup.pid
STOPFLAG=/tmp/asr_sup_stop

case "${1:-start}" in
stop)
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    touch "$STOPFLAG"
    pkill -x sherpa_live; pkill -x sub-overlay
    echo "supervisor: stopped"
    exit 0
    ;;
esac

# Already running?  Exact pid check, no name matching.
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "supervisor: already running (pid $(cat "$PIDFILE")), nothing to do"
    exit 0
fi
echo $$ > "$PIDFILE"
rm -f "$STOPFLAG"     # a stale flag from a previous stop would kill this run

r_recog=0; r_ovl=0

while :; do
    [ -f "$STOPFLAG" ] && { rm -f "$PIDFILE"; echo "supervisor: stop flag seen, exiting"; exit 0; }
    # overlay first: it is the display endpoint and cheap to restart
    if ! pgrep -x sub-overlay >/dev/null; then
        rm -f /tmp/asr_sub
        ( cd "$DIR" && nohup ./sub-overlay /tmp/asr_sub "$FONT" \
              >> /tmp/so.log 2>&1 & )
        r_ovl=$((r_ovl + 1))
        echo "supervisor: overlay up (restart #$r_ovl)"
    fi

    # then the recogniser; "not running" includes a clean deadline exit
    if ! pgrep -x sherpa_live >/dev/null; then
        ( cd "$DIR" && nohup ./sherpa_live ./model ./silero_vad.onnx \
              asr_capture "$RECOG_SECS" > /tmp/live.log 2>&1 & )
        r_recog=$((r_recog + 1))
        echo "supervisor: recogniser up (restart #$r_recog, ${RECOG_SECS:-inf}s)"
    fi

    sleep 5
done
