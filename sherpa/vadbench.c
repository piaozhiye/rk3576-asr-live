// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
// Measure where the live capture loop's wall-clock goes on the board.
//
// The log field lag= showed the pipeline falling behind real time by ~6.2% with
// no decode backlog, so the cost is in the per-32ms path: readi, int16->float,
// silero VAD inference, ring copy.  This bench times each stage on the device
// itself, because host numbers do not transfer (the same silero graph is ~0.12ms
// on x86 and the board is a different ISA and cache story).
//
// It also reports the honest end-to-end question: can the loop sustain 512
// frames (32 ms) every 32 ms, i.e. real time with zero drift?

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c-api.h"

#define PERIOD 512
#define ROUNDS 3000

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *model_dir = argc > 1 ? argv[1] : "./model";
    const char *vad_path  = argc > 2 ? argv[2] : "./silero_vad.onnx";
    const char *device    = argc > 3 ? argv[3] : "asr_mic";

    char para[1024], tokens[1024];
    snprintf(para, sizeof(para), "%s/encoder.rknn,%s/predictor.rknn,%s/decoder.rknn",
             model_dir, model_dir, model_dir);
    snprintf(tokens, sizeof(tokens), "%s/tokens.txt", model_dir);

    printf("=== 1) 识别器初始化耗时 ===\n");
    double t0 = now_s();
    SherpaOnnxOfflineRecognizerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model_config.paraformer.model = para;
    cfg.model_config.tokens = tokens;
    cfg.model_config.num_threads = 1;
    cfg.model_config.provider = "rknn";
    const SherpaOnnxOfflineRecognizer *rec = SherpaOnnxCreateOfflineRecognizer(&cfg);
    if (!rec) { fprintf(stderr, "recognizer init failed\n"); return 1; }
    printf("  SherpaOnnxCreateOfflineRecognizer  %.3f s\n", now_s() - t0);

    printf("\n=== 2) VAD 初始化耗时 ===\n");
    t0 = now_s();
    SherpaOnnxVadModelConfig vc;
    memset(&vc, 0, sizeof(vc));
    vc.silero_vad.model = vad_path;
    vc.silero_vad.threshold = 0.2f;
    vc.silero_vad.min_silence_duration = 0.6f;
    vc.silero_vad.min_speech_duration = 0.3f;
    vc.silero_vad.max_speech_duration = 18.0f;
    vc.silero_vad.window_size = PERIOD;
    vc.sample_rate = 16000;
    vc.num_threads = 1;
    vc.provider = "cpu";
    const SherpaOnnxVoiceActivityDetector *vad =
        SherpaOnnxCreateVoiceActivityDetector(&vc, 30.0f);
    if (!vad) { fprintf(stderr, "vad init failed\n"); return 1; }
    printf("  SherpaOnnxCreateVoiceActivityDetector  %.3f s\n", now_s() - t0);

    printf("\n=== 3) 打开采集设备 ===\n");
    t0 = now_s();
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) { fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(err)); return 1; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    unsigned int rate = 16000;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);
    snd_pcm_uframes_t frames = PERIOD, bufsz = PERIOD * 16;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &frames, NULL);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsz);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err)); return 1;
    }
    snd_pcm_nonblock(pcm, 1);
    snd_pcm_prepare(pcm);
    double t_open = now_s() - t0;
    snd_pcm_uframes_t actual_buf = 0, actual_per = 0;
    snd_pcm_hw_params_get_buffer_size(hw, &actual_buf);
    snd_pcm_hw_params_get_period_size(hw, &actual_per, NULL);
    printf("  open+hw_params  %.3f s\n", t_open);
    printf("  实际 period = %lu 帧 (%.1f ms), buffer = %lu 帧 (%.3f 秒)\n",
           (unsigned long)actual_per, actual_per / 16000.0 * 1000,
           (unsigned long)actual_buf, actual_buf / 16000.0);
    printf("  捕获格式: s16le %u Hz mono\n", rate);

    printf("\n=== 4) 纯读采集 %d 轮（不喂 VAD）===\n", ROUNDS);
    int16_t ibuf[PERIOD];
    float fbuf[PERIOD];
    uint64_t total = 0;
    int rounds_done = 0, stalls = 0;
    double t_read = 0.0, t_cvt = 0.0, t_wait = 0.0;
    double d0;
    for (int i = 0; i < ROUNDS; ++i) {
        d0 = now_s();
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, ibuf, PERIOD);
        if (got == -EAGAIN) { t_wait += now_s() - d0; ++stalls; snd_pcm_wait(pcm, 20); continue; }
        if (got < 0) {
            if (snd_pcm_recover(pcm, got, 1) < 0) { fprintf(stderr, "fatal %s\n", snd_strerror(got)); break; }
            continue;
        }
        t_read += now_s() - d0;
        total += got;
        ++rounds_done;
        d0 = now_s();
        for (int k = 0; k < got; ++k) fbuf[k] = (float)ibuf[k] / 32768.0f;
        t_cvt += now_s() - d0;
        if (rounds_done >= ROUNDS) break;
        d0 = now_s();
        snd_pcm_wait(pcm, 20);
        t_wait += now_s() - d0;
    }
    printf("  完成 %d 轮, 读到 %lu 样本 (%.2f 秒音频), EAGAIN %d 次\n",
           rounds_done, (unsigned long)total, total / 16000.0, stalls);
    if (rounds_done > 0) {
        printf("  readi    %7.3f ms/轮\n", t_read / rounds_done * 1000);
        printf("  int16->f %6.3f ms/轮\n", t_cvt / rounds_done * 1000);
        printf("  wait     %7.3f ms/轮  (含阻塞等待, 正常)\n", t_wait / rounds_done * 1000);
    }

    printf("\n=== 5) 只喂 VAD（合成静音, 不解码）%d 轮 ===\n", ROUNDS);
    for (int i = 0; i < PERIOD; ++i) fbuf[i] = 0.001f * sinf(0.01f * (float)i);
    d0 = now_s();
    for (int i = 0; i < ROUNDS; ++i) {
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, fbuf, PERIOD);
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
            const SherpaOnnxSpeechSegment *seg = SherpaOnnxVoiceActivityDetectorFront(vad);
            SherpaOnnxDestroySpeechSegment((SherpaOnnxSpeechSegment *)seg);
            SherpaOnnxVoiceActivityDetectorPop(vad);
        }
    }
    double t_vad = now_s() - d0;
    printf("  AcceptWaveform  %7.3f ms/轮  (x31.25 轮/秒 = %.1f%% 单核)\n",
           t_vad / ROUNDS * 1000, t_vad / ROUNDS * 31.25 * 100);

    printf("\n=== 6) 实时能力判定 ===\n");
    double per_round_ms = t_read / ROUNDS * 1000 + t_cvt / ROUNDS * 1000 + t_vad / ROUNDS * 1000;
    printf("  每 32ms 窗口的非等待开销: %.3f ms  (预算 32.000 ms)\n", per_round_ms);
    printf("  余量: %.3f ms  -> %s\n", 32.0 - per_round_ms,
           (32.0 - per_round_ms) > 0 ? "够实时" : "不够, 会持续落后");
    if (per_round_ms > 32.0) {
        printf("  落后速率: %.2f%%  (每秒欠 %.3f 秒)\n",
               (per_round_ms - 32.0) / 32.0 * 100, (per_round_ms - 32.0) / 1000.0);
    }

    snd_pcm_close(pcm);
    SherpaOnnxDestroyVoiceActivityDetector(vad);
    SherpaOnnxDestroyOfflineRecognizer(rec);
    return 0;
}
