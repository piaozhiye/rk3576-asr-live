// SPDX-License-Identifier: Apache-2.0
//
// THROWAWAY DIAGNOSTIC -- not part of the shipped pipeline.
//
// Question this answers: can the RK3576 run a *streaming* ASR model on the
// NPU, and what does it cost?  sherpa_live.c only ever builds an offline
// recognizer, so for continuous narration it cannot emit a subtitle until the
// VAD closes a segment (~26 s of news audio).  Before rewriting that pipeline
// for the online API, measure the streaming model on the real device.
//
// Reads a raw s16le 16 kHz mono stream (a dump, or the asrtap FIFO) and
// prints the partial result as it grows, so first-token latency and RTF are
// both visible.  CPU time is reported next to wall time: if the NPU is doing
// the work, CPU stays far below wall.
//
// Build: see build.sh --native, or cross-compile and push.
// Usage: ./sherpa_stream_bench <model_dir> <raw_s16le_16k_mono.pcm> [provider] [kind]
//   kind: "transducer" (default) or "paraformer" (online paraformer, encoder+decoder)
//   provider defaults to rknn; pass cpu to run the same weights through
//   onnxruntime instead, which isolates a bad RKNN conversion from a bug
//   in this harness or in the audio.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c-api.h"

#define CHUNK 512                      // 32 ms, same as the capture thread
#define REPORT_EVERY_S 2.0             // print growth at this cadence

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <model_dir> <raw_s16le_16k_mono.pcm> [provider]\n", argv[0]);
        return 1;
    }
    const char *model_dir = argv[1];
    const char *pcm_path = argv[2];
    const char *provider = argc > 3 ? argv[3] : "rknn";
    const char *kind = argc > 4 ? argv[4] : "transducer";
    const int use_paraformer = strcmp(kind, "paraformer") == 0;
    const int use_rknn = strcmp(provider, "rknn") == 0;


    char enc[512], dec[512], joi[512], tok[512];
    if (use_paraformer) {
        // The upstream online-paraformer release names files by quantisation,
        // not by training epoch, and has no joiner.  The decoder stays on
        // onnxruntime even when the provider is rknn: the encoder runs once
        // per 600 ms chunk and carries the FLOPs, the decoder runs once per
        // emitted token and its graph has 32 cache tensors with a token axis
        // that rknn-toolkit2 cannot make static.  So only the encoder is
        // converted, and mixing providers here is the deliberate design, not
        // an accident.
        if (use_rknn) {
            snprintf(enc, sizeof(enc), "%s/encoder.rknn", model_dir);
            snprintf(dec, sizeof(dec), "%s/decoder.int8.onnx", model_dir);
        } else {
            snprintf(enc, sizeof(enc), "%s/encoder.int8.onnx", model_dir);
            snprintf(dec, sizeof(dec), "%s/decoder.int8.onnx", model_dir);
        }
        snprintf(joi, sizeof(joi), "%s/joiner%s", model_dir, use_rknn ? ".rknn" : ".onnx");
    } else if (use_rknn) {
        // The pre-converted RKNN bundle uses short names.
        snprintf(enc, sizeof(enc), "%s/encoder.rknn", model_dir);
        snprintf(dec, sizeof(dec), "%s/decoder.rknn", model_dir);
        snprintf(joi, sizeof(joi), "%s/joiner.rknn", model_dir);
    } else {
        // The upstream ONNX release uses training-derived names; int8 is the
        // variant worth comparing, since that is what RKNN was built from.
        snprintf(enc, sizeof(enc), "%s/encoder-epoch-99-avg-1.int8.onnx", model_dir);
        snprintf(dec, sizeof(dec), "%s/decoder-epoch-99-avg-1.int8.onnx", model_dir);
        snprintf(joi, sizeof(joi), "%s/joiner-epoch-99-avg-1.int8.onnx", model_dir);
    }
    snprintf(tok, sizeof(tok), "%s/tokens.txt", model_dir);

    double t0 = now_s();
    SherpaOnnxOnlineRecognizerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (use_paraformer) {
        cfg.model_config.paraformer.encoder = enc;
        cfg.model_config.paraformer.decoder = dec;
    } else {
        cfg.model_config.transducer.encoder = enc;
        cfg.model_config.transducer.decoder = dec;
        cfg.model_config.transducer.joiner = joi;
    }
    // Meeting-caption behaviour: let the endpoint rules mark where an
    // utterance ended, so the caller can flush and start a new turn.
    cfg.enable_endpoint = 1;
    cfg.rule1_min_trailing_silence = 0.8f;
    cfg.rule2_min_trailing_silence = 0.5f;
    cfg.rule3_min_utterance_length = 0.0f;
    cfg.model_config.tokens = tok;
    cfg.model_config.num_threads = 1;
    cfg.model_config.provider = provider;
    cfg.decoding_method = "greedy_search";
    const SherpaOnnxOnlineRecognizer *rec = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (!rec) { fprintf(stderr, "online recognizer init FAILED (rknn)\n"); return 1; }
    printf("online recognizer ready (%s, %s) in %.2f s\n    enc=%s\n",
           use_paraformer ? "paraformer" : "transducer", provider, now_s() - t0, enc);

    const SherpaOnnxOnlineStream *stream = SherpaOnnxCreateOnlineStream(rec);
    int endpoints = 0;
    if (!stream) { fprintf(stderr, "stream init FAILED\n"); return 1; }

    FILE *f = fopen(pcm_path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", pcm_path); return 1; }

    int16_t ibuf[CHUNK];
    float buf[CHUNK];
    double audio_pos = 0.0;        // seconds of audio consumed
    double first_text_at = -1.0;   // wall time at which text first appeared
    double wall0 = now_s();
    int done = 0;

    while (!done) {
        size_t got = fread(ibuf, sizeof(int16_t), CHUNK, f);
        for (size_t i = 0; i < got; ++i) buf[i] = (float)ibuf[i] / 32768.0f;
        if (got == 0) {
            SherpaOnnxOnlineStreamInputFinished(stream);
            done = 1;
        } else {
            SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, buf, (int)got);
        }
        audio_pos += got / 16000.0;

        while (SherpaOnnxIsOnlineStreamReady(rec, stream))
            SherpaOnnxDecodeOnlineStream(rec, stream);

        const SherpaOnnxOnlineRecognizerResult *r =
            SherpaOnnxGetOnlineStreamResult(rec, stream);
        if (r) {
            if (first_text_at < 0.0 && r->text && r->text[0]) {
                first_text_at = now_s() - wall0;
                printf("  [first text at audio %.2fs / wall %.2fs] %s\n",
                       audio_pos, first_text_at, r->text);
            } else if ((int)(audio_pos / REPORT_EVERY_S) !=
                       (int)((audio_pos - got / 16000.0) / REPORT_EVERY_S)) {
                printf("  [audio %.2fs] %s\n", audio_pos,
                       r->text && r->text[0] ? r->text : "(empty)");
            }
            SherpaOnnxDestroyOnlineRecognizerResult((SherpaOnnxOnlineRecognizerResult *)r);
        }
    }

    // Flush whatever the tail still owes us.
    while (SherpaOnnxIsOnlineStreamReady(rec, stream))
        SherpaOnnxDecodeOnlineStream(rec, stream);
    const SherpaOnnxOnlineRecognizerResult *fin = SherpaOnnxGetOnlineStreamResult(rec, stream);
    double wall = now_s() - wall0;
    if (fin) {
        printf("\nFINAL: %s\n", fin->text ? fin->text : "(empty)");
        SherpaOnnxDestroyOnlineRecognizerResult((SherpaOnnxOnlineRecognizerResult *)fin);
    }
    printf("\naudio %.2f s in wall %.2f s  -> RTF %.3f\n", audio_pos, wall, wall / audio_pos);
    printf("endpoints fired: %d\n", endpoints);
    if (first_text_at >= 0.0)
        printf("first text after %.2f s of wall time\n", first_text_at);
    else
        printf("no text produced at all\n");

    fclose(f);
    SherpaOnnxDestroyOnlineStream((SherpaOnnxOnlineStream *)stream);
    SherpaOnnxDestroyOnlineRecognizer(rec);
    return 0;
}
