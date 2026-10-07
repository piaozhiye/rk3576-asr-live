#!/usr/bin/env bash
# run-live.sh -- PC 实时字幕完整演示:视频窗口里实时烧字幕 + 扬声器出声。
#
#   sherpa_live_pc (草稿+精修) → /tmp/asr_sub
#   live-sub-player.py (playbin + textoverlay 实时刷字, 音频走 asrtap_auto)
#
# 用法: ./pc/run-live.sh [媒体文件] [播放秒数]   # 默认 sherpa/zhtest.mp4 120s
set -euo pipefail
cd "$(dirname "$0")/.."

MEDIA=${1:-sherpa/zhtest.mp4}
SECS=${2:-120}

export LD_LIBRARY_PATH=$PWD/pc/libs/sherpa-onnx-v1.13.8-linux-x64-shared-lib/lib
# Self-contained ALSA config (written by pc/setup-pc.sh): the tap pcms and
# the x86_64 plugin .so paths live inside this checkout, so a second clone
# on the same machine does not fight over ~/.asoundrc.
[ -f pc/alsa.conf ] || { echo "先跑 ./pc/setup-pc.sh（详见 pc/README-PC.md）"; exit 1; }
export ALSA_CONFIG_PATH=$PWD/pc/alsa.conf
export SHERPA_STREAM_DIR=$PWD/pc/model-streaming
export SHERPA_OFFLINE_DIR=$PWD/pc/model-sv
export SHERPA_SUBTITLE_FIFO=/tmp/asr_sub
export SHERPA_TERM_MAP=$PWD/pc/term-map.txt
export SHERPA_PUNCT_DIR=$PWD/pc/model-punct   # 标点模型
export SHERPA_DRAFT_PUNCT=1                   # 草稿也上标点(板端默认关,见 sherpa_live.c)
export SHERPA_SQ_SLOTS=384                    # 12 s 缓冲:PC 上视频解码与推理抢 CPU,抗突发

# 1. 环里先建会话(1 s 静音),识别器才能打开 asr_capture(板上由 supervisor 重试兜底)
rm -f /dev/shm/asrtap_ring
aplay -q -D asrtap_auto -f S16_LE -r 48000 -c 2 -d 1 /dev/zero

# 2. 识别器(草稿 CPU 流式 + SenseVoice 第二遍精修)
#    注意:FIFO 只能有播放器一个读者。曾经用 `cat /tmp/asr_sub` 同时记录,
#    POSIX FIFO 每次写入只投递给一个读者,字幕行被两边随机分抢,播放器
#    漏行导致画面长时间停在同一句(2026-10-07 实测)。字幕流由播放器
#    自己 tee 到 pc/subtitles.log。
rm -f /tmp/asr_sub pc/demo.log pc/subtitles.log
mkfifo /tmp/asr_sub
./pc/sherpa_live_pc pc/model-streaming sherpa/lib/silero_vad.onnx \
    asr_capture $((SECS + 45)) > pc/demo.log 2>&1 &
ASR=$!
for i in $(seq 1 40); do grep -q "streaming mode" pc/demo.log && break; sleep 1; done

# 3. 播放器窗口:字幕实时叠在视频上,音频经抽头链出声。
#    nice/ionice:视频解码让路给识别器 -- 同优先级时软解码曾把采集队列
#    挤丢 2.4 s 音频(VAAPI 硬解 + 降优先级双保险)。
nice -n 10 ionice -c 3 python3 pc/live-sub-player.py "$MEDIA" /tmp/asr_sub "$SECS" || true

# 4. 收尾:等最后一句提交,停识别器
sleep 8
kill -INT $ASR 2>/dev/null || true
wait $ASR 2>/dev/null || true

echo; echo "== 精修字幕(第二遍) =="
grep "^\[[0-9]*->2\]" pc/demo.log || true
