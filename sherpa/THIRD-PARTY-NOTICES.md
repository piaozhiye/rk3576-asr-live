# 第三方组件与许可声明（THIRD-PARTY NOTICES）

本目录（`sherpa/`）构建的板端实时 ASR 栈使用以下第三方开源组件。
许可信息核验日期 2026-09-28，来源链接见文末。

| 组件 | 用途 | 许可证 | 本仓库中的形态 |
| --- | --- | --- | --- |
| [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) v1.13.8（rknn 构建） | 推理运行时 + C API | **Apache-2.0** | 不入库（预编译共享库已 vendored 于 `sherpa/lib/`）；`c-api.h` 为其头文件副本（Copyright (c) 2023 Xiaomi Corporation 等，文件头保留） |
| Paraformer 模型（川版 2025-10-07 / 标准版 2023-03-28，rk3576 预转换） | 语音识别 | **Apache-2.0**（上游 [ASLP-lab/WSChuan-ASR](https://huggingface.co/ASLP-lab/WSChuan-ASR) HuggingFace 标注 apache-2.0；底模阿里 FunASR paraformer-large 同为 Apache-2.0） | 不入库，从 sherpa-onnx `asr-models` release 下载（见 README 的包名与 md5） |
| SenseVoice 模型（rk3576 预转换包） | 可选识别后端 | Apache-2.0（阿里通义 FunAudioLLM 发布；**以模型发布页当前标注为准**，引用前请复核） | 不入库，同上 |
| [silero VAD](https://github.com/snakers4/silero-vad) v5 | 语音活动检测 | **MIT**（Copyright (c) 2020-present Silero） | 模型不入库（`silero_vad_v5.onnx` 从 sherpa-onnx release 获取，md5 见根 README） |

各许可证文本见官方仓库。本项目对这些组件的使用与分发遵循上述条款；
分发本目录代码时须一并保留本声明与各文件版权头。

whisper（OpenAI，MIT）仅曾作为对比基线，本仓库不含其代码或权重。
