# PC (x86_64) 上跑实时字幕

板端链路原样跑在 PC 上,无需板子。同一份 `sherpa_live.c` 源码,识别在 CPU
(NPU 部分:第二遍换成 SenseVoice ONNX int8,CPU provider),字幕直接烧进
GStreamer 播放器画面。

```
live-sub-player.py (playbin: h264/aac 解码)
  ├─ 视频 → textoverlay(字幕实时刷新) → 窗口
  └─ 音频 → alsasink device=asrtap_auto        ← 扬声器出声
              └→ asrtap 插件(x86 编译) 下混 16k 单声道 → /dev/shm/asrtap_ring
                   → asr_capture → sherpa_live_pc
                       ├─ 流式 paraformer 草稿(逐字上屏,带标点)
                       └─ SenseVoice 第二遍(异步精修,替换草稿行)
                           ↘ /tmp/asr_sub → textoverlay
```

## 快速开始

```sh
./pc/run-live.sh pc/sample.mp4    # 开箱视频演示:仓库自带示例片(30s TTS,自制无版权问题)
./pc/run-live.sh <视频> [秒数]    # 或你自己的任意视频,如:
./pc/run-live.sh ~/videos/xxx.mp4 150
./pc/run-demo.sh                  # 纯音频版:终端实时滚屏字幕(默认同一 TTS 样例)
./pc/run-demo.sh <媒体> [秒数]    # 或指定任意音频/视频取其音轨
```

## 首次准备(全新 clone,三步)

**1. 系统依赖**(root,一次性;脚本本身不需要 root):

```sh
sudo apt install build-essential libasound2-dev ffmpeg python3-gi \
     gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
     gstreamer1.0-plugins-bad gstreamer1.0-libav fonts-noto-cjk
```

(plugins-bad+libav 提供 h264/aac 解码含 VAAPI;base 提供 textoverlay)

**2. 一键引导** `pc/setup-pc.sh`:下载 x86_64 库与 CPU 模型(~650 MB,
huggingface.co 优先、hf-mirror.com 兜底)、直接使用**已入库的预编译插件**
`tools/asrtap/x86_64/*.so`(插件源码不入库,仅预编译 .so)、编译
`pc/sherpa_live_pc`、写自包含 `pc/alsa.conf`——**不碰 /etc/asound.conf
和 ~/.asoundrc**,运行脚本通过 `ALSA_CONFIG_PATH` 指向它:

```sh
./pc/setup-pc.sh                     # 声卡自动探测(取第一个 Analog 输出)
./pc/setup-pc.sh --slave plughw:1,0  # 或手动指定(aplay -l 查扬声器)
```

**3. 下载内容一览**(setup 已封装,这里备查):

| 内容 | 位置 | 来源 |
|---|---|---|
| sherpa-onnx v1.13.8 x64 库 | `pc/libs/.../lib` | GitHub release `sherpa-onnx-v1.13.8-linux-x64-shared-lib.tar.bz2` |
| 流式 paraformer(草稿) | `pc/model-streaming/` | `csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en` |
| SenseVoice int8(第二遍) | `pc/model-sv/` | `csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17` |
| 标点 ct-transformer int8 | `pc/model-punct/` | `lorneluo/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`(官方仓库只有 fp32) |
| asrtap ALSA 插件 | `tools/asrtap/x86_64/` | **预编译入库**;aarch64/armhf 版在同目录 `tools/asrtap/{aarch64,armhf}/`,源码不入库 |

## 环境变量(run-live.sh 已全部设好)

| 变量 | 本机值 | 说明 |
|---|---|---|
| `SHERPA_STREAM_DIR` | pc/model-streaming | 草稿模型;设了它就跳过 rknn/NPU 初始化 |
| `SHERPA_OFFLINE_DIR` | pc/model-sv | 第二遍;目录里有 `model.int8.onnx` 即走 CPU SenseVoice |
| `SHERPA_PUNCT_DIR` | pc/model-punct | 标点模型;精修行默认过 |
| `SHERPA_DRAFT_PUNCT` | 1 | 草稿也上标点(每次文本变化一次推理;板端 A82 余量小,默认关) |
| `SHERPA_SQ_SLOTS` | 384 (12 s) | 采集队列深度;板端默认 48,PC 与视频解码抢 CPU 必须开大,否则丢块=时间轴空洞=字幕错乱 |
| `SHERPA_TERM_MAP` | pc/term-map.txt | 术语纠正,一行 `错=>对` |
| `SHERPA_SUBTITLE_FIFO` | /tmp/asr_sub | 字幕流,播放器消费 |

字幕样式在 `pc/live-sub-player.py` 的 `apply_font()`/textoverlay 属性处:
当前电影风(白字黑边无底框,字号=视频高 1/30)。

## 本机踩过的坑(按严重程度)

1. **丢帧出乱字**:视频软解码(avdec_h264)与 onnxruntime 抢 CPU,采集队列
   (1.5 s)爆,丢的每块都是时间轴空洞,流式模型从错误位置解码——重复字、
   结巴、幻觉英文。三层修复缺一不可:第二遍预热(sherpa_live.c,冷会话首次
   推理能停顿数秒)+ `SHERPA_SQ_SLOTS=384` + 播放器 `nice -n 10`(播放器
   里已内置 VAAPI 硬解优先)。修复后实测 read=fed、0 dropped。
2. **插件加载失败** `Cannot open shared library …alsa-lib/…`:见上,用
   `pcm_type.*.lib` 显式指绝对路径。
3. hf-mirror 上标点模型官方仓只有 fp32,int8 在 lorneluo 镜像仓。

## 与板上的差异

| | 板 (RK3576) | PC |
|---|---|---|
| 第二遍 | paraformer/SenseVoice **rknn/NPU**(0.26-0.33 s/句) | SenseVoice **ONNX int8 CPU**(0.6-1.8 s/句,异步不挡草稿) |
| 字幕上屏 | sub-overlay(wayland 独占字幕条) | textoverlay 烧在视频里 |
| 编排 | supervisor.sh + S50asrchain | pc/run-live.sh |
| 采集队列 | 48(1.5 s,NPU 专用够用) | 384(12 s,与视频解码共存) |
