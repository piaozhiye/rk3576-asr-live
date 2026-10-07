# rk3576-asr-live — RK3576 板端免提中文实时识别 + 视频实时字幕

RK3576 上开箱即用的中文实时 ASR:说完停约 0.6 秒出字、命令词 100% 逐字(实测)、
~0.3 秒解码(RTF ~0.08)、全简体、零空输出。跑在 NPU 上,板端自持,不依赖网络和 PC。
视频播放时,字幕条实时上屏:开口 ~0.5 s 出草稿,NPU 第二遍 ~0.3–0.7 s 后替换为
带标点的精修行,长句不截断,140 s 连续语音零漂移零丢帧(全部实测)。

## 组成

- `sherpa/sherpa_live.c` — 主程序(~1500 行):ALSA 采集 → silero VAD / 流式端点 →
  双遍识别(CPU 流式草稿 + NPU 异步精修)→ 终端 + 字幕条。采集/解码/PASS2 三线程。
- `sherpa/lib/` — 三个预编译 aarch64 共享库(已入库,clone 即可编译):
  `libsherpa-onnx-c-api.so`、`libonnxruntime.so`(k2-fsa/sherpa-onnx v1.13.8
  rknn 构建)、`librknnrt.so`(Rockchip RKNPU 运行时)。
- `sherpa/c-api.h` / `sherpa/lib/silero_vad.onnx` — vendored 头文件与 VAD 模型。
- `sherpa/subtitle.c · pass2.c · window_plan.c` — 纯逻辑模块(提交策略/采纳策略/
  窗口规划),带单元测试,脱离板子可测。
- `tools/` — `supervisor.sh`(编排,pidfile 互斥)、`play_sub.sh`(播放,含低延迟
  参数)、`S50asrchain`(开机自启)、`term-map.txt`(术语校正)、`asrtap/`(ALSA
  抽头插件:仅提供预编译 .so(aarch64、armhf、x86_64;源码不公开)+install.sh)、
  `sub-overlay.c`(字幕条)、
  `perf_lock.sh`/`S45perf_lock`(定频)、`install_stream_model.sh`(流式模型安装)。
- `setup_models.sh` — 一键下载 paraformer 窗口模型(体积原因不入库)。
- `sherpa/compare3.py` / `sherpa/vadbench.c` — 测量复现工具。
- `sherpa/THIRD-PARTY-NOTICES.md` — 第三方组件许可(已核验:Apache-2.0 / MIT /
  BSD,可商用)。
- `tts_test/` — 10 句中文命令词的 TTS 测试集与期望词表,无人值守回归。

## 快速开始

### 1. 前提

- aarch64 交叉工具链(`aarch64-linux-gnu-gcc` 在 PATH,或 buildroot 工具链树;
  buildroot 工具链的 libmpfr 坑 build.sh 已自动处理)
- 共享库与头文件已在 `sherpa/lib/` 与 `sherpa/c-api.h`,无需额外下载
- **模型**:运行 `./setup_models.sh` 一键下载(超 GitHub 单文件限制,不入库):

| 板上目录 | 包名 | 磁盘 | 用途 |
| --- | --- | --- | --- |
| `model/`(默认) | `sherpa-onnx-rk3576-10-seconds-paraformer-zh-2025-10-07` | 454 MB | 第二遍 10 s 窗(NPU) |
| `model-20s/` | `sherpa-onnx-rk3576-20-seconds-paraformer-zh-2025-10-07` | 468 MB | 第二遍长句路由(NPU) |
| `model-streaming/` | `tools/install_stream_model.sh` 安装 | 232 MB | 流式草稿(CPU) |
| `model-punct/` | sherpa-onnx ct-transformer int8 | 78 MB | 标点恢复(CPU) |
| `model-sv30/`(可选) | `sherpa-onnx-rk3576-30-seconds-sense-voice-…-2024-07-17` | 527 MB | SenseVoice 第二遍(自带标点,多语种) |

  VAD 模型已 vendored(`sherpa/lib/silero_vad.onnx`)。运行库合计约 78 MB,
  全套磁盘约 1.3 GB。板上 `librknnrt.so` 勿随意升级(2.3/2.4 有崩溃报告)。

### 2. 编译

```sh
./build.sh              # 产出 sherpa_live + tools/sub-overlay(aarch64,零环境)
./build.sh --test       # 宿主机三套单元测试(不依赖板子)
./build.sh --native     # 板上有 gcc 时板内编译
```

### 3. 部署与运行(USB 麦克风独立识别)

```sh
adb push sherpa_live setup_models.sh <板上目录>/ && adb shell chmod +x <板上目录>/sherpa_live
# 模型(宿主机执行 setup_models.sh 后 push,见上表)

# 实时识别(说完停 ~0.6 s 出字;Ctrl-C 停;末尾加秒数可限时)
# asr_mic = USB 麦克风节点;视频字幕链路用的则是 asr_capture
adb shell 'cd <板上目录> && export LD_LIBRARY_PATH=$PWD/sherpa/lib:$PWD:/usr/lib && \
  ./sherpa_live ./model ./silero_vad.onnx asr_mic'
```

参数:`sherpa_live <model_dir> <vad.onnx> <alsa设备> [秒数] [原始pcm转储]`。
输出行:`[序号] t=出字时刻 win=窗口区间 lag=落后量 q=解码积压`。

## 在 PC 上跑(无需板子)

同一份源码在 x86_64 上跑完整链路:asrtap 插件(预编译 .so,三架构 aarch64/armhf/x86_64 入库于 tools/asrtap/)、第二遍换 SenseVoice
ONNX(CPU)、字幕直接烧进 GStreamer 播放器画面。一条命令:

```sh
./pc/run-live.sh <视频> [秒数]    # 视频窗口 + 实时字幕,详见 pc/README-PC.md
```

## 视频字幕链路(播放 → 实时字幕上屏)

```
播放器 → asrtap_auto (ALSA 播放插件:透传不干扰 + 下混 16k 单声道)
       → 共享内存环 /asrtap_ring (单写单读,写满覆盖保最新,识别器晚起不断流)
       → asr_capture (普通 ALSA 捕获节点) → sherpa_live
           ├─ CPU 流式 paraformer:草稿,开口 ~0.5 s 上屏
           └─ NPU 第二遍(独立线程,异步):精修 + 标点 + 术语校正,替换草稿行
       → /tmp/asr_sub → sub-overlay (wayland 字幕条,屏幕顶部 1080×320)
```

日常使用三条命令(板上 `/data/rknn-test/sherpa` 已部署就绪):

```sh
# 1) 启动整条链 —— 已装开机自启(/etc/init.d/S50asrchain),重启后一般可跳过直接播放;
#    只有手动 stop 过才需要重新拉起(任一进程挂掉 5 秒内自动重启):
adb shell 'cd /data/rknn-test/sherpa && nohup ./supervisor.sh start > /tmp/sup.log 2>&1 &'

# 2) 播放任意视频/音频(字幕条在屏幕顶部,视频在下)
adb shell 'play-sub /data/rknn-test/sherpa/mp2.mp4'

# 3) 收工(识别器、字幕条、守护循环一起停)
adb shell 'cd /data/rknn-test/sherpa && ./supervisor.sh stop'
```

日志解读(`adb shell tail -f /tmp/live.log`):

| 行 | 含义 |
| --- | --- |
| 终端原位刷新的草稿 | 流式实时结果,逐字增长 |
| `2nd pass 12.0 s in 632 ms [20s]: adopted …` | NPU 精修耗时、路由(长句走 20 s 模型)与采纳判定 |
| `[7->2] +0.34s -> 修正文本` | 第 7 句的替换行(带标点,术语已校正) |
| `[session] audio resumed` | 暂停/换片后恢复,流式上下文已重置(首句不丢) |
| 退出汇总 `stream: read X, fed Y, difference Z` | Z 恒定 + dropped 0 = 健康;Z 增长 = 丢音频 |

一次性部署(新板子从零复现):

| 步骤 | 内容 |
| --- | --- |
| 1 | `./build.sh && adb push sherpa_live <板上目录>/` |
| 2 | 模型四套(见上表)+ 可选 sv30 |
| 3 | sherpa 运行库:`sherpa/lib/` 三件套 → 板上 `sherpa-onnx-v1.13.8-rknn-linux-aarch64-shared/lib/` |
| 4 | `tools/sub-overlay`(build.sh 已产出)+ `supervisor.sh` + `play_sub.sh`(板上 `/usr/bin/play-sub`)+ `S50asrchain` → `/etc/init.d/` |
| 5 | ALSA 抽头插件:`tools/asrtap/install.sh` 一键装(幂等,.so+asound.conf 三段) |
| 6 | 定频:`tools/perf_lock.sh`(测量前必做;开机自启用 `tools/S45perf_lock`) |

`supervisor.sh` 已设全部 `SHERPA_*` 环境变量,无需手工 export。

## 硬件与系统环境

| 项 | 值 |
| --- | --- |
| SoC | RK3576:4×Cortex-A53 @2.0 GHz + 4×Cortex-A72 @2.2 GHz |
| NPU | RKNPU 双核(驱动 v0.9.8),实测第二遍仅用 Core0 |
| 内存 | 8 GB(识别链峰值 3.34 GiB = 42%,见下) |
| 显示 | weston(wayland),**无 layer-shell**;竖屏 1080×1920 |
| 系统 | buildroot;SD 卡根文件系统(**大文件 push 会静默截断**,装模型用 md5 校验脚本) |
| 音频 | 板载 es8388(hw:0,0)/HDMI/DP;USB 麦以卡名 `Audio` 寻址 |

## 内部结构(改代码前读这节)

**线程模型**(实测 10 线程):main(解码线程,流式解码驱动/草稿上屏/PASS2 入队)、
capture(ALSA 读 + silero VAD,仅 ~0.1% CPU)、流式 ORT worker×3、标点 ONNX 池×3、
p2_worker(NPU 第二遍→采纳→标点→术语→替换上屏;NPU 干活,CPU ≈0)。

**双遍分工**:CPU 流式 paraformer 出实时草稿;句终后 NPU 离线 paraformer 精修
(>11 s 句子自动路由 20 s 模型),采纳策略拒绝空/前缀/过短结果;修正行经标点模型
(SenseVoice 后端则用其原生标点)与术语校正后**替换**草稿行并驻留 2 s。
标注集实测:纯流式 CER 40% → 两遍 0%。

**为什么这些值是这些值**(都有实测依据,勿凭感觉改):rule1=1.2 s 匹配真实句间停顿
(rule2 的 0.4 s 会切碎句子);rule3=15 s 由 20 s 二遍模型兜底(10 s 模型只读 ~12 s,
实测 10/12/14/16/18/20 s 输出 87/96/恒 96 字);流式 4 线程(1 线程中位时延 7.95 s
且单调漂移,4 线程 1.49 s);PASS2 必须异步(同步每句停流 ~300 ms,100 s 累计漂移
+25 s)。

## 资源消耗实测(2026-09-30,定频,mp2 连续语音 100 s,64 采样点)

| 资源 | 空闲(等待播放) | 连续语音期(含视频解码) |
| --- | --- | --- |
| CPU 全芯片 | 0.6%(0.05 核) | 34.4%(2.75 核;sherpa 线程组约 1.5–2 核) |
| NPU | 0% | 仅 Core0,瞬时 23–24%,平均占空比 ~3%(每句 ~0.3 s) |
| 内存 | sherpa_live 峰值 **3.34 GiB**(paraformer 双上下文,42% RAM;仅 10 s 单模型 2.13 GiB) | 同左 |
| 磁盘 | 全套模型+运行库 ≈ 1.3 GB(明细见模型表) | — |
| 字幕条 | 3–8 MB RSS,空闲 0% CPU | 渲染仅随草稿变化 |

时延:开口 → 草稿上屏 ~0.5 s(playbin 预滚已关);句终 → 修正替换 +0.3–0.7 s
(SenseVoice 后端 ~0.95 s);140 s 连续语音累计漂移 0.00 s、0 丢帧。

## 调参入口

环境变量(supervisor 已设全;单独跑时需自 export):

| 变量 | 默认(supervisor) | 作用 |
| --- | --- | --- |
| `SHERPA_STREAM_DIR` | model-streaming | 流式模型;**不设则走离线窗口模式**。目录含 `joiner.rknn` 时按 zipformer transducer 上 NPU(英语实验) |
| `SHERPA_OFFLINE_DIR` | model | 第二遍 10 s NPU 模型;目录含 `model.rknn` 则切 SenseVoice |
| `SHERPA_PASS2_LONG_DIR` | model-20s | >11 s 句子路由的 20 s 二遍模型(paraformer 时) |
| `SHERPA_PUNCT_DIR` | model-punct | 标点模型(SenseVoice 后端自动跳过,其自带标点) |
| `SHERPA_TERM_MAP` | term-map.txt | 术语校正表(`错=>对` 每行,`#` 注释),草稿与修正行都应用 |
| `SHERPA_DWELL` | 2.0 | 修正行驻留秒数 |
| `SHERPA_SUBTITLE_FIFO` | /tmp/asr_sub | 字幕 FIFO |
| `SHERPA_LONG_MODEL_DIR` | (空) | 离线窗口模式的 ≥10 s 路由(麦克风场景可选) |

行为参数(改代码,`sherpa_live.c` 顶部宏):VAD 阈值 0.2、段尾静音 0.6 s、最短语音
0.3 s、切窗头部余量 1.2 s(silero 触发晚于真实起音 0.14–1.2 s)。

**后端选择**:默认 paraformer 双模型(术语更准、修正更快);日常口语/英日韩粤内容
可切 SenseVoice(`SHERPA_OFFLINE_DIR=model-sv30`,注意 rknn 版 SenseVoice 必须
用 30 s 窗导出,短窗版只读开头几秒)。按内容切换,两态都已实测。

## 测试

`tts_test/` 两层(期望词表 `phrases.list`,TTS 男声,非真人):

1. **单句直测**(识别器精度):每句 wav 直接喂识别器。
2. **长音频全链路**(VAD 切段+切窗+识别):`long_test.wav` 过
   `segment_and_score.py`(宿主 onnxruntime,与板上同参数同算法)再上板评分。

```sh
./build.sh --test        # 纯逻辑单测:window_plan / subtitle / pass2
```

实测基线:长音频全链路 10/10 逐字;单句 9/10;命令词多轮实时 100% 逐字;
识别质量验证必须用**生产节奏**(带视频时钟)——fakesink 无同步测试形态会让管线
~8 倍速狂奔、识别全碎,不能用来下结论。

## 已知问题与边界

1. **默认设备路由**:播放器不带 `device=asrtap_auto` 就绕过抽头(待做系统级路由)。
2. **ioplug 空环语义**:朴素读者(裸 `arecord -D asr_capture`)在空环上 100% 忙转;
   本链已用非阻塞+重连规避,根治需改插件 poll 语义。
3. **字幕条布局**依赖 weston 平铺顺序,play-sub 在播放器起来后重启 overlay 保证在顶。
4. 播放结束后字幕**保留最后一行**(不清屏);换片/暂停恢复有会话重置保护。
5. 英语字幕:**当前不可用**。实测 NPU 双语流式 zipformer 解码低于实时(RTF>1,双核仍队列溢出、时序损坏);SenseVoice 第二遍本身支持英语,缺的是跟得上的英语草稿层——候选为 CPU 版双语流式模型,待下轮实验。

## 故障排查(症状 → 原因 → 处理)

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| 播放了但没字幕(重启后) | 未装开机自启或链路被 stop | `supervisor.sh start`;确认 `/etc/init.d/S50asrchain` |
| 有声音但识别不出 | 播放器没走 asrtap_auto | 用 play-sub 或 `audio-sink="alsasink device=asrtap_auto"` |
| 板子越跑越卡 | 残留 `arecord -D asr_capture` 空转(见已知问题 2) | `pkill -9 arecord` |
| tokens.txt 崩溃 std::out_of_range | SD 卡 push 静默截断 | 用 md5 校验脚本装模型,勿信"文件存在" |
| 视频黑屏有声 | 非方像素(PAR≠1:1) | play-sub 已带 `pixel-aspect-ratio=1/1` |
| 字幕条被视频盖住 | weston 平铺顺序 | play-sub 会重启 overlay;手动 `pkill -x sub-overlay` 等拉起 |
| 首句缺开头 | (已修)旧版跳到最新/旧上下文吃头 | 更新到 5cd33ea 之后;确认日志有 `[session]` 行 |
| 术语错(站稳态) | 模型近音词 | `term-map.txt` 加一行 `错=>对` |

## 许可

本项目代码以 **Apache-2.0** 发布(见 `LICENSE`);第三方组件的归属与许可
见 `sherpa/THIRD-PARTY-NOTICES.md`。
