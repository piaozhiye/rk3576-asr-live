#!/usr/bin/env bash
# run-demo.sh -- PC (x86_64) 复跑实时字幕演示，完整板端链路:
#   aplay -D asrtap_auto (扬声器出声 + 下混抽头) -> /dev/shm/asrtap_ring
#   -> asr_capture -> sherpa_live_pc (CPU 流式草稿 + SenseVoice 第二遍)
#
# 前提: 已跑 ./pc/setup-pc.sh(库/模型/插件/pc/alsa.conf 一次到位)
#
# 用法: ./pc/run-demo.sh [媒体文件] [秒数]
#   默认素材 = 仓库自带 tts_test/long_test.wav(30.7 s,十句中文 TTS,
#   无版权问题) -- 不再自动挑本机的 zhtest.mp4(那是第三方广告素材);
#   要放它就显式写: ./pc/run-demo.sh sherpa/zhtest.mp4 150
set -euo pipefail
cd "$(dirname "$0")/.."

MEDIA=${1:-tts_test/long_test.wav}
[ -f "$MEDIA" ] || { echo "no such media: $MEDIA"; exit 1; }
SECS=${2:-0}   # 0 = 整段

WAV=pc/demo-$(basename "${MEDIA%.*}").wav
if [ ! -s "$WAV" ]; then
    if [ "$SECS" -gt 0 ] 2>/dev/null; then
        ffmpeg -v error -y -t "$SECS" -i "$MEDIA" \
            -vn -ar 48000 -ac 2 -c:a pcm_s16le "$WAV"
    else
        ffmpeg -v error -y -i "$MEDIA" \
            -vn -ar 48000 -ac 2 -c:a pcm_s16le "$WAV"
    fi
fi
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$WAV" | cut -d. -f1)

export LD_LIBRARY_PATH=$PWD/pc/libs/sherpa-onnx-v1.13.8-linux-x64-shared-lib/lib
[ -f pc/alsa.conf ] || { echo "先跑 ./pc/setup-pc.sh（详见 pc/README-PC.md）"; exit 1; }
export ALSA_CONFIG_PATH=$PWD/pc/alsa.conf
export SHERPA_STREAM_DIR=$PWD/pc/model-streaming
export SHERPA_OFFLINE_DIR=$PWD/pc/model-sv
export SHERPA_SUBTITLE_FIFO=/tmp/asr_sub
export SHERPA_PUNCT_DIR=$PWD/pc/model-punct
export SHERPA_DRAFT_PUNCT=1
export SHERPA_SQ_SLOTS=384
export SHERPA_TERM_MAP=$PWD/pc/term-map.txt

# 字幕流(板上 sub-overlay 消费的同一条 FIFO)落一份到 pc/subtitles.log
rm -f /tmp/asr_sub pc/subtitles.log pc/demo.log
mkfifo /tmp/asr_sub
cat /tmp/asr_sub > pc/subtitles.log 2>/dev/null &
READER=$!

# 识别器先起:等它打出 streaming mode 再播放(模型加载约 5-15 s)。
# 输出 tee 到终端:字幕草稿实时滚屏,不再"只有声音、屏幕上什么都没有"。
./pc/sherpa_live_pc pc/model-streaming sherpa/lib/silero_vad.onnx \
    asr_capture $((DUR + 45)) > >(tee pc/demo.log) 2>&1 &
ASR=$!
for i in $(seq 1 40); do
    grep -q "streaming mode" pc/demo.log && break
    sleep 1
done

echo "== 播放 ${MEDIA}(约 ${DUR}s,扬声器出声) =="
aplay -D asrtap_auto "$WAV" || true
sleep 8                                # 等最后一句提交 + 第二遍完成
kill -INT $ASR 2>/dev/null || true
wait $ASR 2>/dev/null || true
sleep 1; kill $READER 2>/dev/null || true

echo
echo "== 精修行(第二遍采纳) =="
grep "^\[[0-9]*->2\]" pc/demo.log
echo
echo "== 统计 =="
grep -E "first text|stream: read|second pass:" pc/demo.log | tail -4
