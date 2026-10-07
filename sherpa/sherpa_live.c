// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
// sherpa_live -- hands-free live ASR: ALSA capture -> silero VAD (auto
// segmentation) -> sherpa-onnx offline paraformer (rknn/NPU) -> text.
//
// No keypresses: speech segments are emitted automatically when the VAD sees
// min_silence after min_speech.  Every decode window is capped at WINDOW_MAX
// (18 s) so it always fits the rknn model's 20-second window -- see
// window_plan.c for why that needs enforcing here rather than in the VAD
// config, and test_window_plan.c for the policy's tests.
//
// Head recovery (v5, measured 2026-09-28): on this user's mic audio silero's
// trigger lags the true onset by 0.14-0.33 s typically and up to 1.2 s
// (measured over the 20-phrase set; thresholds 0.2-0.5 do not move it), so
// sherpa's own segments eat the first syllable.  seg->start is in sherpa's
// INTERNAL popped-buffer coordinates, not ours -- slicing our ring against it
// grabs audio from wrong stream positions (v2's "北京北京时间"), and anchoring
// on Detected() edges misaligns once a noise blip or max-speech split emits
// an extra segment (v4 lost "一" twice that way).  v5 anchors on the emission
// point, which is ours by construction: a segment surfacing from the queue
// means its trailing silence just confirmed, so the window is
//   [total - 0.6 s tail - seg_len - 1.2 s headroom, total - 0.6 s).
// One subtraction, no state to misalign.  Validated end to end on the user's
// own 150 s recording: 20/20 phrases verbatim through this exact path
// (vs 18/20 for trigger-anchored v4, whose two misses were "一二三四五").
// Paraformer is robust to the extra leading silence (verified: 0.3/0.8 s
// silence and 0.5 s ambient prefixes all decode verbatim).
//
// usage: sherpa_live <model_dir> <vad.onnx> <alsa_device> [seconds]
//   model_dir contains encoder.rknn predictor.rknn decoder.rknn tokens.txt
//   seconds   0 = run until Ctrl-C (default)

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c-api.h"
#include "subtitle.h"
#include "pass2.h"
#include "window_plan.h"

#define RING_CAP   (16000 * 25)         // 25 s of raw history
#define STALL_MS   3000                // no samples for this long -> reopen device
#define REOPEN_MAX 10                   // give up after this many reopens
#define QUEUE_MAX  8                    // decoded-outstanding speech windows

// Window geometry, handed to window_plan() for every segment.  HEAD and TAIL
// are the emission-anchored allowances described at the top of this file; MAX
// is the hard ceiling on a decode window.
//
// MAX is 18 s, not 20 s: the 20 s model truncates past 333 LFR frames
// (~19.98 s), so a fitting segment decodes speech+HEAD and an over-long one
// is cut to exactly MAX -- both bounded by MAX, both under the model's
// window.  MAX also has to fit inside RING_CAP, or a cut window would reach
// back past the ring and silently lose its head.  18 < 25, with margin.
//
// This ceiling exists because vc.silero_vad.max_speech_duration is advisory
// only -- per c-api.h it "temporarily uses a higher threshold to encourage a
// split", which continuous speech (a news read with no pause long enough to
// close a segment) simply outlasts.
#define WINDOW_HEAD (16000 * 1.2)       // headroom prepended to a fitting segment
#define WINDOW_TAIL (16000 * 0.6)       // trailing silence trimmed from a fitting one
#define WINDOW_MAX  (16000 * 18)        // hard ceiling on one decode window

// Latency cap: the longest the viewer waits for a subtitle line.
//
// Measured 2026-09-28 on a narration clip, subtitle latency tracked segment
// length almost exactly -- 5.4 s segment gave 6.8 s of latency, 18.0 s gave
// 24.8 s -- because a window is only emitted once the VAD hands over its
// segment, and the VAD cannot close a segment that has no pause in it.  Capping
// WINDOW_MAX does not help that: the segment still has to land first.
//
// So the capture loop carries a second, independent trigger that fires on
// elapsed audio and never consults the VAD.  With it, latency is bounded by
// LAT_CAP + decode time (~0.5 s) whatever the content is, at the cost of
// occasionally cutting a line mid-sentence.  2.5 s puts the worst case near
// 3 s; the industry reference for VAD-batched transcription is 0.5-2 s.
//
// HEAD and LAT_CAP are overridable from the environment so the two can be
// swept against scored audio without a rebuild per data point.  WINDOW_MAX is
// deliberately NOT tunable: it exists to keep a window inside the model's
// window, and tuning it below the model's limit is just a different bug.
//
// Defaults come from a sweep on 2026-09-28 (SHERPA_WINDOW_HEAD x
// SHERPA_LAT_CAP against the scored 10-phrase TTS set, plus latency on a
// narration clip):
//
//   head  cap   lines  TTS CER   narration latency
//   1.2   2.5     20   49.02%       --
//   0.6   2.5     19   19.61%      5.22 s
//   0.3   2.5     19   23.53%       --
//   0.6   3.0     19   23.53%      5.67 s
//   0.6   4.0     10    1.96%      5.67 s   <- chosen
//
// HEAD matters more than it looks: at 1.2 s adjacent windows overlap so heavily
// that the recogniser re-decodes the same audio slightly differently each time
// ("打开客厅的灯" / "我打开客厅的灯"), and the overlap cannot be stripped from
// the transcript.  That is user-visible duplication, not a scoring artefact.
// At 0.6 s the overlap merges cleanly and CER matches the VAD-only baseline.
//
// CAP trades against it: a 2.5 s slice cuts ordinary sentences in half
// (CER 19.6%), while 4.0 s covers a typical line whole.  Shrinking the cap
// buys only 0.45 s of latency for a 10x CER loss -- and this architecture
// cannot get below ~5 s regardless, for the reason above.
#define WINDOW_HEAD_TUNE (16000 * 06 / 10)   // 0.6 s context allowance
#define LAT_CAP_TUNE     (16000 * 4)         // 4.0 s slice

static uint64_t env_samples(const char *name, uint64_t fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    double secs = atof(v);
    if (secs <= 0.0) return fallback;
    return (uint64_t)(secs * 16000.0);
}

// Capture and decode MUST be separate threads.  Measured 2026-09-28: decoding a
// segment blocks 0.3-0.7 s (NPU), while the ALSA buffer is only 8192 frames =
// 0.512 s -- the old comment claiming "8 s of headroom" was off by 16x.  Sharing
// one loop therefore drained the buffer on every segment: the pipeline fell
// behind real time by 6.2% (logged as lag=), end-to-end latency grew
// monotonically 1.1 s -> 11 s over 160 s, and rapid speech would overrun and
// lose audio outright.  The capture thread now never blocks on decode; segments
// wait in the queue instead.

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    float buf[RING_CAP];
    uint64_t base;    // absolute index of buf[0]
    uint64_t count;   // valid samples, == RING_CAP once full
} Ring;

// ---------------------------------------------------------------- streaming
// A streaming recogniser is fed samples as they arrive and returns the whole
// transcript so far on every decode, so it needs a different hand-off from the
// offline path: not finished windows off a queue, but a continuous stream of
// audio.  Samples go capture -> SQueue -> decode, and the decode thread owns
// the online stream exclusively, because AcceptWaveform and DecodeStream on one
// stream are not documented as thread-safe.

// Sample queue geometry.  Each slot is one 32 ms capture period, so SLOTS is
// how much audio may be buffered ahead of the decoder.  It was 8 (~0.26 s) and
// the decoder needed longer than that per period at times, so the queue filled,
// sq_push dropped, and the stream reached the model with holes in its timeline:
// measured on a 252 s lecture the decoder was fed 18.6 s of audio in 32 s of
// wall time, and the text drifted and duplicated ("你是穷并并清清楚楚真真的的")
// while the same audio through the offline bench was clean.  A dropped sample
// is not a lost word, it is a corrupted position.
//
// 48 slots is 1.5 s of slack, which is several times the measured per-period
// decode cost on the board, and it is bounded memory: 48 * 512 * 4 B = 96 kB.
// SQ_SLOTS_MAX caps the array (set by SHERPA_SQ_SLOTS at init, clamped
// below); a PC that also runs the player, video decode and the second pass
// on one CPU can burst past 1.5 s (measured 2026-10-07: 61 chunks dropped
// over 150 s of playback -- each drop is a hole in the timeline and the
// streaming model then decodes from wrong positions), so there the queue is
// widened and a stall costs draft latency instead of timeline holes.
#define SQ_SLOTS_MAX 512
#define SQ_SLOTS_DEFAULT 48

// RMS gate for the streaming path, in float amplitude.  Deliberately low: it
// exists to stop the recogniser hallucinating on silence, not to detect speech,
// so the offline path's silero VAD is the right tool for that and this is not.
// 0.004 is about -48 dBFS, which is below quiet speech in a film mix but above
// digital silence.
#define STREAM_GATE_RMS 0.004

// Leading-silence threshold for the second pass, in float amplitude.  0.02 is
// about -34 dBFS per sample: speech in this material runs near -28 dBFS and the
// gaps between sentences fall well below it.  See pass2_trim_lead for the
// measurement that made this necessary.
#define STREAM_TRIM_RMS 0.02f

typedef struct {
    float  *buf[SQ_SLOTS_MAX];
    int     n[SQ_SLOTS_MAX];
    int     slots;                   // capacity in use (env-tunable)
    int     head, count;
    uint64_t dropped;
    pthread_mutex_t lock;
    pthread_cond_t  ready;
    int     closing;
} SQueue;

static void sq_init(SQueue *q) {
    memset(q, 0, sizeof(*q));
    q->slots = SQ_SLOTS_DEFAULT;
    // See SQ_SLOTS_MAX above for why a host may want more slack.
    const char *v = getenv("SHERPA_SQ_SLOTS");
    if (v && *v) {
        int n = atoi(v);
        if (n >= 8 && n <= SQ_SLOTS_MAX) q->slots = n;
    }
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->ready, NULL);
}

// Non-blocking push: the capture thread must never wait on the decoder, or a
// slow decode becomes an audio dropout.  A full queue means the decoder is not
// keeping up, which is worth counting but not worth stalling playback for.
static void sq_push(SQueue *q, const float *s, int n) {
    pthread_mutex_lock(&q->lock);
    if (q->count == q->slots) {
        // Loud on purpose.  A dropped period is a hole in the stream's timeline,
        // and the recogniser then decodes from the wrong position, so the text
        // drifts and repeats.  That is far more damaging than the couple of
        // milliseconds of latency a stall would cost, and it used to be silent.
        if (q->dropped == 0)
            fprintf(stderr, "sample queue full (%d slots) -- DROPPING audio, "
                            "stream timing will be corrupted\n", q->slots);
        ++q->dropped;
        pthread_mutex_unlock(&q->lock);
        return;
    }
    int slot = (q->head + q->count) % q->slots;
    q->buf[slot] = malloc(n * sizeof(float));
    if (!q->buf[slot]) { pthread_mutex_unlock(&q->lock); return; }
    memcpy(q->buf[slot], s, n * sizeof(float));
    q->n[slot] = n;
    ++q->count;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->lock);
}

// Returns 1 and hands over ownership of the buffer, or 0 when closing and empty.
static int sq_pop(SQueue *q, float **out, int *n) {
    pthread_mutex_lock(&q->lock);
    while (!q->count && !q->closing)
        pthread_cond_wait(&q->ready, &q->lock);
    if (!q->count) { pthread_mutex_unlock(&q->lock); return 0; }
    *out = q->buf[q->head];
    *n = q->n[q->head];
    q->head = (q->head + 1) % q->slots;
    --q->count;
    pthread_mutex_unlock(&q->lock);
    return 1;
}

static void sq_close(SQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->closing = 1;
    pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->lock);
}

static void sq_free(SQueue *q) {
    for (int i = 0; i < q->count; ++i) {
        int slot = (q->head + i) % q->slots;
        free(q->buf[slot]);
    }
    q->head = q->count = 0;
}
// One finished speech window, handed from the capture thread to the decode
// thread.  The audio is copied out of the ring here so the decode thread never
// touches the ring and the two threads share nothing but this queue.
typedef struct {
    float  *audio;
    size_t  n;
    double  win_start;  // capture-relative seconds
    double  win_end;
    uint64_t fed_at;    // total_fed at emission, for the lag field
    int     q_at_push;  // queue depth the moment this was enqueued
} Job;

typedef struct {
    Job     slot[QUEUE_MAX];
    int     head, count;
    uint64_t dropped;
    int     peak;       // deepest the queue ever got, i.e. real decode backlog
    pthread_mutex_t lock;
    pthread_cond_t  ready;
    int     closing;
} Queue;

typedef struct {
    const char        *device;
    const SherpaOnnxVoiceActivityDetector *vad;
    FILE              *dump;
    Queue             *queue;
    double             t_listen;
    double             deadline;
    int                reopens;
    int                forced_splits;   // segments cut at WINDOW_MAX
    int                latency_slices;  // windows emitted by the latency cap
    int                gated;           // slices skipped: no speech present
    SQueue            *sq;              // streaming mode; NULL = offline
    // Second pass.  The ring keeps recent history so the offline model gets a
    // whole utterance instead of the newest block; the capture thread fills it,
    // the decode thread reads it at an endpoint.  NULL means the second pass is
    // off, which is also the only case where the ring is not allocated.
    Ring              *ring;
    uint64_t          *ring_fed;        // absolute count behind the ring
    pthread_mutex_t   *ring_lock;
    volatile uint64_t  stream_read;     // samples read from the device
    uint64_t           stream_voiced;   // chunks that carried speech
    uint64_t           stream_silent;   // chunks forwarded anyway, but silent
    // Set by the capture thread when data arrives after a dry period (no
    // playback running = a new session, or a pause+resume).  The decode
    // thread resets the streaming stream before feeding the new audio: the
    // pending context belongs to a BROKEN audio timeline, and its endpoint
    // commit-on-new-silence is exactly what swallowed the head of the new
    // first sentence (那下边呢我们就要定量计算 lost, remainder kept --
    // measured 2026-09-30).
    volatile int       new_session;
} CaptureArgs;

static void queue_init(Queue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->ready, NULL);
}

// Push, or drop the oldest when full.  Dropping the oldest keeps the newest
// speech -- a stale transcript is worth less than the current one -- and the
// count is reported at exit so a lossy run is never silent about it.
static void queue_push(Queue *q, Job j) {
    pthread_mutex_lock(&q->lock);
    if (q->count == QUEUE_MAX) {
        // Drop the oldest slot and advance head; the vacated slot is written
        // by the push below, so no shifting is needed.
        free(q->slot[q->head].audio);
        q->head = (q->head + 1) % QUEUE_MAX;
        --q->count;
        ++q->dropped;
    }
    q->slot[(q->head + q->count) % QUEUE_MAX] = j;
    ++q->count;
    if (q->count > q->peak) q->peak = q->count;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->lock);
}

// Returns 0 and leaves *out untouched when the queue is closing and drained.
static int queue_pop(Queue *q, Job *out) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closing) pthread_cond_wait(&q->ready, &q->lock);
    if (q->count == 0) { pthread_mutex_unlock(&q->lock); return 0; }
    *out = q->slot[q->head];
    q->head = (q->head + 1) % QUEUE_MAX;
    --q->count;
    pthread_mutex_unlock(&q->lock);
    return 1;
}

static void queue_close(Queue *q) {
    pthread_mutex_lock(&q->lock);
    q->closing = 1;
    pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->lock);
}

static int queue_count(Queue *q) {
    pthread_mutex_lock(&q->lock);
    int n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

static void ring_push(Ring *r, const float *s, int n) {
    for (int i = 0; i < n; ++i) {
        r->buf[(r->base + r->count) % RING_CAP] = s[i];
        if (r->count < RING_CAP) r->count++;
        else r->base++;
    }
}

static float ring_at(const Ring *r, uint64_t abs) {
    return r->buf[abs % RING_CAP];
}

// Copy one planned window out of the ring and hand it to the decode queue.
// The audio is copied here so the decode thread never touches the ring and the
// two threads share nothing but the queue.  Returns the sample count queued,
// or 0 if the window was empty or the allocation failed.
static uint64_t push_window(CaptureArgs *ca, const Ring *ring,
                            const WindowPlan *wp, uint64_t total_fed) {
    if (wp->end <= wp->from) return 0;
    Job j;
    memset(&j, 0, sizeof(j));
    j.n = (size_t)(wp->end - wp->from);
    j.audio = (float *)malloc(j.n * sizeof(float));
    if (!j.audio) return 0;
    for (size_t i = 0; i < j.n; ++i)
        j.audio[i] = ring_at(ring, wp->from + i);
    j.win_start = wp->from / 16000.0;
    j.win_end   = wp->end / 16000.0;
    j.fed_at    = total_fed;
    // Depth before this push, so q= reports how far behind decoding was when
    // the segment was cut -- the one number that says whether audio is at risk.
    j.q_at_push = queue_count(ca->queue);
    queue_push(ca->queue, j);
    return wp->end - wp->from;
}

static snd_pcm_t *open_capture(const char *device, int period) {
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) { fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(err)); return NULL; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    unsigned int rate = 16000;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);
    snd_pcm_uframes_t frames = period;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &frames, NULL);
    // Capture buffer sized to the tap ring's capacity (65536 frames = 4.096 s).
    // The old value here was 512*16 = 8192 frames, believed to be "8 s" --
    // ALSA buffer units are FRAMES, so it was really 0.512 s (the 16x
    // arithmetic error caught in review).  That mattered little for a USB
    // mic, but over asr_capture it is fatal: the ioplug can hand the
    // framework up to 1 s of ring backlog per pointer call, and any backlog
    // over 0.512 s makes the pointer jump a whole buffer -> the framework
    // flags an overrun and the stream enters a stop/recover death spiral
    // (measured 2026-09-30: a reader attached 3 s into playback got exactly
    // one 4.00 s window and then starved forever).  Matching the ring depth
    // means any backlog the ring can legally hold fits the buffer.
    snd_pcm_uframes_t bufsz = 65536;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsz);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
    if (rate != 16000) {
        fprintf(stderr, "device %s gave %u Hz, want 16000\n", device, rate);
        snd_pcm_close(pcm); return NULL;
    }
    // Non-blocking, so the capture loop below owns the clock.  A blocking
    // snd_pcm_readi on this USB mic can sleep in the kernel indefinitely once
    // the device stalls (measured 2026-09-28: 40 s of zero samples, process in
    // wchan=do_sys_poll), which pins the mic, makes the seconds deadline
    // unreachable and blocks Ctrl-C until data happens to arrive.  -EAGAIN plus
    // snd_pcm_wait keeps the loop responsive and lets the stall watchdog run.
    snd_pcm_nonblock(pcm, 1);
    snd_pcm_prepare(pcm);
    return pcm;
}

// Async second-pass job + worker context (file scope: the worker runs on its
// own thread, main() builds and owns the context).
typedef struct {
    float *audio;
    int n;
    char first[1024];
} P2Job;
typedef struct {
    P2Job slot;
    int busy;                 // 0 = free, 1 = a job is waiting
    pthread_mutex_t lock;
    pthread_cond_t ready;
    int closing;
    pthread_t thr;
    const SherpaOnnxOfflineRecognizer *frec;
    const SherpaOnnxOfflineRecognizer *frec_long;  // 20 s model, or NULL
    int long_at;              // spans longer than this (samples) use frec_long
    int sv_backend;           // pass2 is SenseVoice: its text is already
                              // punctuated (ITN), skip the punct model
    const char *sub_path;
    const SherpaOnnxOfflinePunctuation *punct;
    int lines;                // corrected-line counter for the [n->2] log tag
    int adopted, rejected;    // worker-local counters, reported at shutdown
    // Shared with the decode thread (atomics, no lock): the decode side
    // holds drafts off the overlay while a correction is in flight or
    // dwelling; the worker stamps final_us when the corrected line is
    // actually on screen.
    int64_t final_us;         // last correction written to overlay (us)
    int64_t enq_us;           // when the newest job was enqueued (us)
    int pending;              // 1 = a job is enqueued and not yet written
} P2Ctx;

// Async second-pass worker.  Takes the newest queued utterance, runs the
// NPU offline decode, applies the adopt policy, then prints and (if the
// overlay is up) writes the corrected line.  Running this inline stopped the
// streaming decode ~300 ms per sentence, which compounded into subtitle
// latency drifting +25 s over 100 s of continuous speech (measured 2026-09-30);
// on its own thread the streaming path keeps decoding during the NPU pass.
// One worker, one slot: a newer utterance replaces an unconsumed older one --
// the newest correction is the one the viewer needs.  Exits when closing.
// Domain-term correction table (SHERPA_TERM_MAP, one "wrong=>right" per
// line, applied to both drafts and corrected finals).  Neither ASR backend
// reliably emits this lecture's key term: paraformer writes 暂稳态 in one
// sentence and 站稳态 in the next (the speaker's 暂/站 are near-homophones);
// SenseVoice writes 站稳态/散稳态 throughout (measured 2026-09-30).  A tiny
// string table fixes a KNOWN term deterministically, which no model swap
// delivered.  UTF-8-safe: replacement happens on byte-string boundaries.
#define TERM_MAP_MAX 32
typedef struct { char from[64]; char to[64]; int flen, tlen; } TermMap;
static TermMap g_terms[TERM_MAP_MAX];
static int g_nterms;

static void term_map_load(const char *path)
{
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[256];
    while (g_nterms < TERM_MAP_MAX && fgets(buf, sizeof buf, f)) {
        if (buf[0] == '#') continue;           // comment
        char *sep = strstr(buf, "=>");
        if (!sep) continue;
        *sep = '\0';
        char *to = sep + 2;
        char *nl = strpbrk(to, "\r\n"); if (nl) *nl = '\0';
        char *fl = buf; while (*fl == ' ') ++fl;
        char *fe = fl + strlen(fl);
        while (fe > fl && fe[-1] == ' ') *--fe = '\0';
        if (!*fl || !*to) continue;
        const size_t flen = strlen(fl), tlen = strlen(to);
        if (flen >= sizeof(g_terms[g_nterms].from) ||
            tlen >= sizeof(g_terms[g_nterms].to)) {
            fprintf(stderr, "term map: skipping overlong rule in %s\n", path);
            continue;
        }
        memcpy(g_terms[g_nterms].from, fl, flen + 1);
        memcpy(g_terms[g_nterms].to, to, tlen + 1);
        g_terms[g_nterms].flen = (int)flen;
        g_terms[g_nterms].tlen = (int)tlen;
        ++g_nterms;
    }
    fclose(f);
    fprintf(stderr, "term map: %d rule(s) from %s\n", g_nterms, path);
}

static void term_map_apply(char *s, size_t cap)
{
    for (int t = 0; t < g_nterms; ++t) {
        // All occurrences, not just the first: a long line can carry the
        // same wrong term twice (a sentence mentioning 暂稳态 twice).
        for (char *at = strstr(s, g_terms[t].from); at;
             at = strstr(at + g_terms[t].tlen, g_terms[t].from)) {
            size_t head = (size_t)(at - s);
            size_t tail = strlen(at + g_terms[t].flen);
            if (head + (size_t)g_terms[t].tlen + tail >= cap) break;
            memmove(at + g_terms[t].tlen, at + g_terms[t].flen, tail + 1);
            memcpy(at, g_terms[t].to, (size_t)g_terms[t].tlen);
        }
    }
}

// Serialises punctuation calls: the second-pass worker punctuates finals
// while (opt-in) the decode thread punctuates drafts, and the C wrapper's
// thread-safety is not documented.  onnxruntime inference itself is
// thread-safe, so this is belt and braces, not a correctness fix.
static pthread_mutex_t g_punct_lock = PTHREAD_MUTEX_INITIALIZER;

static void *p2_worker(void *arg) {
    P2Ctx *p = (P2Ctx *)arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->busy && !p->closing)
            pthread_cond_wait(&p->ready, &p->lock);
        if (!p->busy) { pthread_mutex_unlock(&p->lock); break; }
        float *audio = p->slot.audio;
        int n = p->slot.n;
        char first[1024];
        snprintf(first, sizeof(first), "%s", p->slot.first);
        p->slot.audio = NULL;
        p->busy = 0;
        pthread_mutex_unlock(&p->lock);

        char line[1024];
        snprintf(line, sizeof(line), "%s", first);
        char second[1024] = "";
        // Route by span length: the 10 s model reads ~12 s of audio and no
        // more, so a longer span goes to the 20 s twin (when loaded).  Same
        // dispatch idea as the offline path's long_threshold.
        const SherpaOnnxOfflineRecognizer *use =
            (p->frec_long && n > p->long_at) ? p->frec_long : p->frec;
        const double t2 = now_s();
        const SherpaOnnxOfflineStream *fs = SherpaOnnxCreateOfflineStream(use);
        if (fs) {
            // const float *, not int16: raw PCM bytes feed the model values
            // around 1e37 and it answers with noise.
            SherpaOnnxAcceptWaveformOffline(fs, 16000, audio, n);
            SherpaOnnxDecodeOfflineStream(use, fs);
            const SherpaOnnxOfflineRecognizerResult *fr =
                SherpaOnnxGetOfflineStreamResult(fs);
            if (fr && fr->text) snprintf(second, sizeof(second), "%s", fr->text);
            if (fr)
                SherpaOnnxDestroyOfflineRecognizerResult(
                    (SherpaOnnxOfflineRecognizerResult *)fr);
            SherpaOnnxDestroyOfflineStream(fs);
        }
        const double ms = (now_s() - t2) * 1000.0;
        Pass2Decision d = pass2_decide(line, second);
        if (d.adopt) { snprintf(line, sizeof(line), "%s", second); ++p->adopted; }
        else ++p->rejected;
        printf("      2nd pass %.1f s in %.0f ms%s: %s\n",
               n / 16000.0, ms,
               (use == p->frec_long && p->frec_long) ? " [20s]" : "", d.reason);
        if (getenv("SHERPA_P2_VERBOSE"))
            printf("            1st=[%s]\n            2nd=[%s]\n", first, second);
        // Punctuation on the adopted final only (drafts stay raw).
        // SenseVoice punctuates natively (ITN); applying ct-transformer on
        // top duplicated every mark ("。。" -- measured 2026-09-30).
        if (p->punct && !p->sv_backend) {
            pthread_mutex_lock(&g_punct_lock);
            const char *pt = SherpaOfflinePunctuationAddPunct(p->punct, line);
            if (pt && *pt) snprintf(line, sizeof(line), "%s", pt);
            if (pt) SherpaOfflinePunctuationFreeText(pt);
            pthread_mutex_unlock(&g_punct_lock);
        }
        term_map_apply(line, sizeof line);
        printf("[%d->2] +%.2fs -> %s\n", ++p->lines, ms / 1000.0, line);
        fflush(stdout);

        // Overlay: the corrected line REPLACES the draft on screen.  Open
        // non-blocking per write; a missing reader costs nothing.
        int sfd = p->sub_path ? open(p->sub_path, O_WRONLY | O_NONBLOCK) : -1;
        if (sfd >= 0) {
            FILE *sf = fdopen(sfd, "w");
            if (sf) {
                fputs(line, sf); fputc('\n', sf); fflush(sf);
                fclose(sf);
            } else close(sfd);
        }
        // The line is on screen (or there is no overlay): stamp the dwell
        // clock and clear in-flight -- order matters, the decode thread
        // reads both.
        __atomic_store_n(&p->final_us, (int64_t)(now_s() * 1e6), __ATOMIC_RELEASE);
        __atomic_store_n(&p->pending, 0, __ATOMIC_RELEASE);
        free(audio);
    }
    return NULL;
}

// Capture thread: read -> int16 to float -> VAD -> slice the window -> queue.
// It never decodes, so nothing here can be held up by the NPU.  Owns the ALSA
// handle, the VAD, the ring and the dump; hands finished windows to the decode
// thread as owned float buffers.
static void *capture_thread(void *arg) {
    CaptureArgs *ca = (CaptureArgs *)arg;
    const char *device = ca->device;
    const int period = 512;
    // The tap node going dry is normal idle, not a wedged device: playback
    // paused or not started simply means the ring has nothing.  Reopening
    // reconnects to the same ring (recovers nothing), and the old give-up
    // path made the recogniser EXIT after ~30 s of idle -- which under the
    // supervisor became a restart loop with a ~13 s dead window each cycle
    // (10 restarts logged during one measurement run, 2026-09-30).  A USB
    // mic that goes dry IS a wedged device (the do_sys_poll 40 s incident),
    // so it keeps the reopen-and-give-up behaviour.
    const int tap_node = strcmp(device, "asr_capture") == 0;
    int idle_logged = 0;
    int16_t buf[period];
    float fbuf[period];
    static Ring ring;                      // 25 s * 4 B, too big for the stack
    memset(&ring, 0, sizeof(ring));
    uint64_t total_fed = 0;
    // Ring-absolute start of the speech run currently being accumulated, in
    // OUR coordinates.  seg->start cannot be used here: it lives in the VAD's
    // internal popped-buffer coordinates (see the header note), so cutting on
    // it skips real audio.  A run ends wherever a window was emitted, and
    // where a run is cut at WINDOW_MAX the next run resumes at the cut.
    uint64_t run_start = 0;
    double last_sample = now_s();

    // Window geometry, tunable for the latency/accuracy trade-off.
    const uint64_t head  = env_samples("SHERPA_WINDOW_HEAD", WINDOW_HEAD_TUNE);
    const uint64_t cap   = env_samples("SHERPA_LAT_CAP",     LAT_CAP_TUNE);

    snd_pcm_t *pcm = open_capture(device, period);
    // sq_close here too: a failed open (e.g. the asr_capture ring does not
    // exist yet because the player has not started) must let the main
    // thread exit, or the process becomes a zombie the supervisor cannot
    // see -- observed 2026-09-30.
    if (!pcm) { sq_close(ca->sq); queue_close(ca->queue); return NULL; }

    while (!g_stop) {
        if (ca->deadline > 0 && now_s() >= ca->deadline) break;
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf, period);
        {
            if (got == -EAGAIN) {
            // No data ready.  Bounded sleep so the deadline and g_stop stay
            // live; if the device has been dry, reopen it.  The tap node
            // reconnects at 0.5 s, the mic at STALL_MS (3 s): each tap
            // reconnect is what picks a dry reader back up after playback
            // starts (the ioplug stops delivering until reopened), so the
            // interval directly caps first-subtitle latency -- average
            // attach lag 1.5 s -> 0.25 s (mp2 measured 8.2 s -> ~5 s wall,
            // ~3 s of which is the player's own mp4 startup).  A reopen is
            // a shm attach, effectively free, and the plugin's per-open
            // logging is verbose-gated.
            const double dry_s = tap_node ? 0.5 : STALL_MS / 1000.0;
            if (now_s() - last_sample > dry_s) {
                // The reopen is not optional for the tap node: the ioplug
                // stops delivering after a long dry period until the handle
                // is reopened (measured 2026-09-30: playback started, the
                // ring's wr counter advanced at 16000 f/s, rd stayed 0).
                // But the give-up below is mic-only: an idle ring means "no
                // playback", which can last all day.
                if (tap_node) {
                    if (!idle_logged) {
                        fprintf(stderr, "tap idle (no playback), reconnecting until audio arrives\n");
                        idle_logged = 1;
                    }
                } else {
                    fprintf(stderr, "capture stalled %.1f s, reopening %s\n",
                            now_s() - last_sample, device);
                }
                snd_pcm_close(pcm);
                pcm = NULL;
                if (!tap_node && ++ca->reopens > REOPEN_MAX) {
                    fprintf(stderr, "giving up after %d reopens\n", ca->reopens);
                    break;
                }
                pcm = open_capture(device, period);
                if (!pcm) break;
                last_sample = now_s();
            }
            snd_pcm_wait(pcm, 20);
            continue;
        }
        if (got < 0) {
            if (snd_pcm_recover(pcm, got, 1) < 0) {
                fprintf(stderr, "alsa fatal: %s\n", snd_strerror(got));
                break;
            }
            last_sample = now_s();
            continue;
        }
        last_sample = now_s();
        // A successful read heals both counters: reopens then count
        // consecutive stalls, not the device's whole lifetime.
        ca->reopens = 0;
        // Data after a dry period = the audio timeline broke (new playback,
        // or pause+resume).  Flag it: the decode thread resets the streaming
        // stream before this chunk, so the stale pending context cannot
        // commit on the new silence and swallow the new sentence's head.
        if (idle_logged) {
            __atomic_store_n(&ca->new_session, 1, __ATOMIC_RELEASE);
            idle_logged = 0;
        }
        }
        for (int i = 0; i < got; ++i) fbuf[i] = (float)buf[i] / 32768.0f;
        if (ca->dump) fwrite(buf, sizeof(int16_t), got, ca->dump);
        if (ca->sq) {
            // Streaming mode: the decoder consumes the audio itself and does
            // its own segmenting, so the window machinery below is unused.
            //
            // Every sample is forwarded, INCLUDING silence.  Dropping quiet
            // chunks instead was tried and is wrong: a streaming model carries
            // state that assumes a continuous timeline, so removing audio punches
            // a hole in its clock and it decodes from the wrong position.  That
            // showed up as drifting and duplicated text ("站稳态" for 暂稳态,
            // "一个一个过渡渡" for 过渡过程) -- worse than the noise it was meant to
            // prevent.  The way to keep silence from being decoded is to let the
            // model see it and reset on the endpoint, not to hide it.
            //
            // What the count is for is diagnosis: if the recogniser spends most
            // of a film being fed non-speech, that is visible rather than
            // mistaken for a rendering problem.
            double sum = 0.0;
            for (int i = 0; i < got; ++i) sum += (double)fbuf[i] * fbuf[i];
            const double rms = got > 0 ? sqrt(sum / got) : 0.0;
            sq_push(ca->sq, fbuf, got);
            if (ca->ring) {
                // sring_fed is the absolute sample count the second pass measures
                // its span against, so it is updated under the same lock as the
                // ring itself; the endpoint resets base against this value.
                pthread_mutex_lock(ca->ring_lock);
                ring_push(ca->ring, fbuf, got);
                *ca->ring_fed += (uint64_t)got;
                pthread_mutex_unlock(ca->ring_lock);
            }
            ca->stream_read += (uint64_t)got;   // capture thread is the only writer
            if (rms >= STREAM_GATE_RMS) ++ca->stream_voiced;
            else ++ca->stream_silent;
            continue;
        }
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(ca->vad, fbuf, got);
        ring_push(&ring, fbuf, got);
        total_fed += got;

        while (!SherpaOnnxVoiceActivityDetectorEmpty(ca->vad)) {
            const SherpaOnnxSpeechSegment *seg =
                SherpaOnnxVoiceActivityDetectorFront(ca->vad);
            int split_here = 0;
            if (seg->n >= (int32_t)(16000 * 0.3f)) {   // drop sub-0.3s blips
                // Emission-anchored window (see header note), with the
                // segment-length policy living in window_plan().
                WindowPlan wp;
                const uint64_t n = window_plan(run_start,
                                               (uint64_t)seg->n,
                                               head, WINDOW_TAIL,
                                               ring.base, total_fed,
                                               WINDOW_MAX, &wp);
                // The latency cap may already have transcribed most of this
                // segment by the time it lands.  Without this clamp the
                // emission-anchored window re-emits whole paragraphs verbatim.
                const uint64_t cn = clamp_window_start(&wp, run_start,
                                                       head, ring.base);
                if (cn > 0) push_window(ca, &ring, &wp, total_fed);
                split_here = wp.split && cn > 0;
                // Whatever happened, the next window starts where this one
                // stopped, so consecutive windows tile the stream instead of
                // leaving a hole.  A blip produced no window, so fall back to
                // the emission anchor to keep the pointer moving.
                run_start = n > 0 ? wp.end
                                  : (total_fed > WINDOW_TAIL ? total_fed - WINDOW_TAIL : 0);
            }
            SherpaOnnxDestroySpeechSegment((SherpaOnnxSpeechSegment *)seg);
            SherpaOnnxVoiceActivityDetectorPop(ca->vad);
            if (split_here) {
                // The segment outran WINDOW_MAX.  Anything still queued belongs
                // to the same over-long run, so drop it and reset the
                // detector: the audio past the cut is re-detected as a fresh
                // segment rather than the run growing without bound.  This has
                // to come after Pop() -- Clear() empties the very queue this
                // loop is draining.
                SherpaOnnxVoiceActivityDetectorClear(ca->vad);
                ++ca->forced_splits;
                fprintf(stderr, "segment outran %.0f s window, cut and reset VAD "
                        "(%d so far)\n", WINDOW_MAX / 16000.0,
                        ca->forced_splits);
                fflush(stderr);
                break;
            }
        }

        // Latency-cap trigger.  Runs whether or not the VAD produced anything,
        // because the whole point is not to wait for the VAD to close a
        // segment: on continuous speech it does not close one for tens of
        // seconds, and the viewer's wait is measured from when the line was
        // spoken, not from when we decided it had ended.
        //
        // It is still gated on the VAD currently seeing speech.  A film is
        // mostly not speech -- measured on G.功夫熊猫3 at 15 min, silero
        // found 3 segments in 55 s of orchestral score -- and decoding music
        // anyway makes paraformer emit hallucination ("好的好的好的", spaced-out
        // Latin letters) as subtitles.  Gating on Detected() separates the two
        // cases that a silence rule cannot: continuous speech keeps the
        // detector inside a segment indefinitely, so the cap still fires,
        // while music never opens one.
        WindowPlan lw;
        const uint64_t ln = plan_latency_window(run_start, total_fed, cap,
                                                head, ring.base, &lw);
        if (ln > 0) {
            if (SherpaOnnxVoiceActivityDetectorDetected(ca->vad)) {
                push_window(ca, &ring, &lw, total_fed);
                run_start = lw.end;
                ++ca->latency_slices;
            } else {
                // Not speech: skip the audio instead of queueing it.  It has
                // to be skipped rather than held, or the next slice would try
                // to catch up on minutes of score.
                run_start = lw.end;
                ++ca->gated;
            }
        }
    }

    if (pcm) snd_pcm_close(pcm);
    // BOTH queues must close here.  queue_close wakes the offline decode
    // thread; without sq_close the streaming loop stays in sq_pop's
    // cond_wait forever -- a deadline run ended as a one-thread zombie in
    // futex, still holding the fifo open so later writers "succeeded" into
    // a buffer nobody read (observed 2026-09-29).
    if (ca->sq) sq_close(ca->sq);
    queue_close(ca->queue);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <model_dir> <vad.onnx> <alsa_device> [seconds]\n", argv[0]);
        return 1;
    }
    // argv is copied into a fixed buffer rather than aliased: the model path is
    // composed into three further snprintf destinations, and an unbounded
    // source would make every one of them a truncation candidate.
    char model_dir[256];
    snprintf(model_dir, sizeof(model_dir), "%s", argv[1]);
    const char *vad_path = argv[2];
    const char *device = argv[3];
    const double seconds = argc > 4 ? atof(argv[4]) : 0.0;
    // optional dump of exactly what the mic fed us, for offline forensics
    FILE *dump = argc > 5 ? fopen(argv[5], "wb") : NULL;

    signal(SIGINT, on_sigint);
    // The subtitle FIFO's reader may come and go; a write with no reader
    // must fail with EPIPE (which the write site retries) instead of
    // raising the default SIGPIPE, which would kill the whole recogniser
    // mid-session -- observed 2026-09-29: the overlay was not running and
    // the process vanished after the first decoded line.
    signal(SIGPIPE, SIG_IGN);

    // The streaming path below returns from inside its own decode loop and
    // never touches the offline recognizer -- creating one there is dead
    // weight (a 1.2 GB NPU model on the board, a guaranteed load failure on
    // any host without the rknn provider, e.g. a PC running the same
    // pipeline on CPU).  Probe SHERPA_STREAM_DIR here so streaming mode
    // skips the whole rknn setup.
    const char *stream_dir = getenv("SHERPA_STREAM_DIR");
    const int streaming = stream_dir && stream_dir[0];

    const SherpaOnnxOfflineRecognizer *rec = NULL;
    const SherpaOnnxOfflineRecognizer *rec_long = NULL;
    // 10 s window, in samples.  A window longer than this is one the default
    // model would truncate, so it goes to the 20 s model instead.
    const size_t long_threshold = (size_t)16000 * 10;
    if (!streaming) {
    SherpaOnnxOfflineRecognizerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    char para[3 * 256 + 64], tokens[256 + 16], sv[256 + 16];
    snprintf(para, sizeof(para), "%s/encoder.rknn,%s/predictor.rknn,%s/decoder.rknn",
             model_dir, model_dir, model_dir);
    snprintf(tokens, sizeof(tokens), "%s/tokens.txt", model_dir);
    cfg.model_config.tokens = tokens;
    cfg.model_config.num_threads = 1;   // 1 = RKNN_NPU_CORE_AUTO
    cfg.model_config.provider = "rknn";
    // Backend switch, so the same capture/VAD/segment/queue path can be measured
    // against a second recognizer.  Everything downstream is unchanged: only
    // which model the recognizer wraps differs, which is what makes the two
    // comparable.  paraformer is a 3-file RKNN triple passed comma-joined;
    // SenseVoice is a single fused model.rknn.
    const char *backend = getenv("SHERPA_BACKEND");
    const int use_sv = backend && strcmp(backend, "sensevoice") == 0;
    if (use_sv) {
        snprintf(sv, sizeof(sv), "%s/model.rknn", model_dir);
        cfg.model_config.sense_voice.model = sv;
        cfg.model_config.sense_voice.language = "auto";
        cfg.model_config.sense_voice.use_itn = 1;   // "8点" -> "八点"
    } else {
        cfg.model_config.paraformer.model = para;
    }
    rec = SherpaOnnxCreateOfflineRecognizer(&cfg);
    if (!rec) { fprintf(stderr, "recognizer init failed\n"); return 1; }

    // Second recognizer for long utterances.  The VAD is allowed to emit a
    // segment up to max_speech_duration (18 s), but the default 10 s window
    // model truncates anything longer and the tail is lost silently -- the user
    // sees the first half of what they said and no indication that the rest
    // went missing.  model-20s is the same model at a 20 s window, so routing
    // long segments to it costs nothing when both are resident.
    //
    // The threshold is the SHORT window (10 s), because that is the model that
    // would truncate.  Routing on the long window instead would only kick in at
    // 19.4 s, which is past the point where the 10 s model has already thrown
    // away half the sentence.  The routed length is the whole window -- the
    // 1.2 s headroom included -- so routing is conservative: a window whose
    // speech alone would just fit 10 s still goes to the 20 s model, which
    // costs a little decode time and can never lose audio.
    // Bounded so the compiler can prove the composed paths below cannot
    // truncate: three model paths plus separators have to fit, and a longer
    // value is a configuration error we want to hear about, not to silently
    // clip into a model path that then fails to load.
    char long_dir[256];
    snprintf(long_dir, sizeof(long_dir), "%s", getenv("SHERPA_LONG_MODEL_DIR")
             ? getenv("SHERPA_LONG_MODEL_DIR") : "");
    if (long_dir[0] && !use_sv) {
        char lp[3 * 256 + 64], lt[256 + 16];
        snprintf(lp, sizeof(lp), "%s/encoder.rknn,%s/predictor.rknn,%s/decoder.rknn",
                 long_dir, long_dir, long_dir);
        snprintf(lt, sizeof(lt), "%s/tokens.txt", long_dir);
        SherpaOnnxOfflineRecognizerConfig lc;
        memset(&lc, 0, sizeof(lc));
        lc.model_config.paraformer.model = lp;
        lc.model_config.tokens = lt;
        lc.model_config.num_threads = 1;
        lc.model_config.provider = "rknn";
        rec_long = SherpaOnnxCreateOfflineRecognizer(&lc);
        if (!rec_long)
            fprintf(stderr, "long-window recognizer unavailable (%s); "
                            "segments over 10 s will be truncated\n", long_dir);
    }
    printf("recognizer ready (%s rknn%s)\n", use_sv ? "sensevoice" : "paraformer",
           rec_long ? " + 20s window" : "");
    fflush(stdout);
    }   // !streaming: rknn recognizer setup ends here

    SherpaOnnxVadModelConfig vc;
    memset(&vc, 0, sizeof(vc));
    vc.silero_vad.model = vad_path;
    vc.silero_vad.threshold = 0.2f;   // 0.4 dropped mid-phrase on one live utterance of 音量太大了 (prob fell to 0.2-0.5 after 0.3 s); 0.2 recovers it with no false segments in a quiet room (session forensics 2026-09-28)
    vc.silero_vad.min_silence_duration = 0.6f;
    vc.silero_vad.min_speech_duration = 0.3f;
    vc.silero_vad.max_speech_duration = 18.0f;
    vc.silero_vad.window_size = 512;
    vc.sample_rate = 16000;
    vc.num_threads = 1;
    vc.provider = "cpu";
    const SherpaOnnxVoiceActivityDetector *vad =
        SherpaOnnxCreateVoiceActivityDetector(&vc, 30.0f);
    if (!vad) { fprintf(stderr, "vad init failed\n"); return 1; }

    // Streaming mode is opt-in and takes a different model directory: the
    // offline models here are .rknn, the online paraformer is ONNX on CPU.
    // Measured on the board: 0.5 s to first text and RTF 0.82, so the NPU is
    // not needed for the live path and stays free for the offline pass.
    // (stream_dir itself is probed above, before the rknn setup.)
    // Optional on-screen overlay sink.  Opened write-only and non-blocking so a
    // missing reader cannot stall the recogniser: the overlay may not be
    // running, and the subtitle path must not hold up decoding when it is not.
    FILE *sub_fifo = NULL;
    const char *sub_path = getenv("SHERPA_SUBTITLE_FIFO");
    // Optional punctuation restoration, applied to FINAL lines only (after
    // the second pass) -- drafts stay raw so the live path gains no latency.
    // Set SHERPA_PUNCT_DIR to a directory holding model.int8.onnx to enable.
    const SherpaOnnxOfflinePunctuation *punct = NULL;
    const char *punct_dir = getenv("SHERPA_PUNCT_DIR");
    if (punct_dir && *punct_dir) {
        SherpaOnnxOfflinePunctuationConfig pc;
        memset(&pc, 0, sizeof(pc));
        char pmodel[1024];
        snprintf(pmodel, sizeof(pmodel), "%s/model.int8.onnx", punct_dir);
        pc.model.ct_transformer = pmodel;
        pc.model.num_threads = 2;
        pc.model.provider = "cpu";
        punct = SherpaOnnxCreateOfflinePunctuation(&pc);
        if (punct) printf("punctuation: %s\n", pmodel);
        else fprintf(stderr, "punctuation: failed to load %s\n", pmodel);
    }
    // Whether drafts get punctuation too (finals always do when a model is
    // loaded) -- see the draft punct site in the decode loop for why the
    // board keeps this off by default.
    const int draft_punct = getenv("SHERPA_DRAFT_PUNCT") != NULL;
    if (sub_path && *sub_path) {
        mkfifo(sub_path, 0666);
        // O_NONBLOCK from the start, not after fopen.  fopen on a FIFO blocks
        // until a reader appears, and it blocked this program before it had even
        // loaded the model: the log stopped after "recognizer ready" and no
        // audio was ever processed.  Non-blocking open fails with ENXIO when
        // there is no reader yet, which is the correct answer here -- the
        // overlay may simply not be running.
        int fd = open(sub_path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            sub_fifo = fdopen(fd, "w");
            if (sub_fifo) {
                printf("subtitle overlay: %s\n", sub_path);
            } else {
                close(fd);
                fprintf(stderr, "subtitle overlay: fdopen failed\n");
            }
        } else if (errno == ENXIO) {
            // No reader yet.  The overlay opens the FIFO for reading later, so
            // retry rather than giving up: the order of the two processes is
            // not something to rely on.
            printf("subtitle overlay: %s not open yet, will retry\n", sub_path);
        } else {
            fprintf(stderr, "subtitle overlay: open %s: %s\n",
                    sub_path, strerror(errno));
        }
    }

    const SherpaOnnxOnlineRecognizer *orec = NULL;
    // Second pass.  NULL unless SHERPA_OFFLINE_DIR names a model that loads:
    // the offline model exists only to revise the streaming one, so the two
    // belong or neither does.  Kept off by default -- see pass2.c for what it
    // costs and what it buys.
    const SherpaOnnxOfflineRecognizer *frec = NULL;
    // Optional 20 s twin for utterances longer than the 10 s model reads.
    const SherpaOnnxOfflineRecognizer *frec_long = NULL;
    int pass2_sv = 0;         // pass2 backend is SenseVoice (model.rknn dir)
    // History for the second pass.  25 s, the same as the offline path's ring;
    // a streaming utterance is capped at 15 s by rule3, so it fits with room to
    // spare.  Only allocated when the second pass is on.
    static Ring sring;
    pthread_mutex_t sring_lock;
    pthread_mutex_init(&sring_lock, NULL);
    uint64_t sring_fed = 0;
    const SherpaOnnxOnlineStream *ostream = NULL;
    static SQueue sq;

    if (streaming) {
        // Streaming model by directory content: encoder.int8.onnx = the
        // zh streaming paraformer on CPU (the default, tuned below);
        // joiner.rknn = a zipformer transducer triple (the bilingual zh-en
        // export runs on the NPU via the rknn provider -- the EN-draft path).
        char enc[512], dec[512], tok[512], jnr[512];
        snprintf(enc, sizeof(enc), "%s/encoder.int8.onnx", stream_dir);
        snprintf(dec, sizeof(dec), "%s/decoder.int8.onnx", stream_dir);
        snprintf(tok, sizeof(tok), "%s/tokens.txt", stream_dir);
        snprintf(jnr, sizeof(jnr), "%s/joiner.rknn", stream_dir);
        struct stat sprobe;
        const int stream_zf = stat(jnr, &sprobe) == 0;
        SherpaOnnxOnlineRecognizerConfig oc;
        memset(&oc, 0, sizeof(oc));
        if (stream_zf) {
            char zenc[512], zdec[512];   // stack: the recognizer is created
                                          // in this same block, like enc/dec
            snprintf(zenc, sizeof(zenc), "%s/encoder.rknn", stream_dir);
            snprintf(zdec, sizeof(zdec), "%s/decoder.rknn", stream_dir);
            oc.model_config.transducer.encoder = zenc;
            oc.model_config.transducer.decoder = zdec;
            oc.model_config.transducer.joiner = jnr;
            oc.model_config.provider = "rknn";
            // -3 = RKNN_NPU_CORE_0_1: both NPU cores.  With 1 (=AUTO, single
            // core) the streaming zipformer and the SenseVoice pass2 contend
            // for Core0 and the draft decode falls behind real time on
            // back-to-back speech (queue overflows, timing corrupts --
            // measured 2026-09-30); two cores restore headroom.
            oc.model_config.num_threads = -3;
        } else {
            oc.model_config.paraformer.encoder = enc;
            oc.model_config.paraformer.decoder = dec;
        }
        oc.model_config.tokens = tok;
        // 4 threads (CPU paraformer only): the streaming decode is the pacing
        // item of the whole subtitle chain.  Measured 2026-09-30 on the 8xA72
        // with the same 100 s of video: 1 thread -> median subtitle latency
        // 7.95 s and a monotonic drift (+15.85 s over the run, RTF ~0.8);
        // 4 threads -> median 1.49 s, first phrase 1.16 s.  The drift does not
        // vanish at 4 threads (the synchronous PASS2 + punctuation each stop
        // the stream ~300 ms per sentence); that is a structural follow-up,
        // not a threading one.  On the NPU zipformer the threads stay at 1.
        if (!stream_zf)
            oc.model_config.num_threads = 4;
        oc.decoding_method = "greedy_search";
        // Endpointing is what turns one growing transcript into separate
        // subtitle lines: without it there is no point at which text is final.
        oc.enable_endpoint = 1;
        // Line length is a subtitle decision, not a model one.  These three
        // rules are an OR, so the shortest one dominates: rule2's 0.4 s of
        // trailing silence was firing on the gaps between words and produced
        // 2 s fragments with doubled text ("计是要的的").  rule1 alone at 1.2 s
        // matches a real pause between sentences, and rule3 caps the worst case
        // so a speaker who never pauses still gets lines.
        oc.rule1_min_trailing_silence = 1.2f;
        oc.rule2_min_trailing_silence = 100.0f;   // effectively off
        // 15 s, not 10: rule3 caps how long an utterance may run before the
        // endpoint fires, and the second pass has to be given the whole
        // utterance.  The 10 s model reads about 12 s of audio and no more
        // (measured: 96 chars at 12 s, exactly 96 at 14/16/18/20 s), so the
        // old 10 s cap existed to keep every utterance inside that limit --
        // which force-cut long sentences mid-word AND sent anything longer to
        // a truncated second pass that the adoption policy then rejected,
        // leaving the uncorrected draft on screen.  Two visible symptoms, one
        // cause.  Utterances longer than 11 s now route to the 20 s
        // second-pass model (SHERPA_PASS2_LONG_DIR), so the cap is bounded by
        // that model and by the 25 s second-pass ring instead.
        oc.rule3_min_utterance_length = 15.0f;
        // rule3 is also what ends an utterance that contains no speech at all:
        // without a committed line the stream context would otherwise grow
        // through a whole film's worth of silence.
        orec = SherpaOnnxCreateOnlineRecognizer(&oc);
        if (!orec) {
            fprintf(stderr, "streaming recognizer init failed (%s)\n", stream_dir);
            return 1;
        }

        const char *off_dir = getenv("SHERPA_OFFLINE_DIR");
        if (off_dir && *off_dir) {
            // The second pass is backend-agnostic by directory content:
            // model.rknn present = SenseVoice (single fused model, reads
            // ~30 s natively, no long twin needed); model.int8.onnx or
            // model.onnx = the same SenseVoice as plain ONNX, which runs on
            // the cpu provider (a PC hosts the whole chain without an NPU);
            // encoder.rknn present = the paraformer triple as one
            // comma-joined string.  The provider has to be named or
            // onnxruntime is handed .rknn files; that fails silently, so it
            // is set explicitly.
            char para[1024], otok[512], svm[512], svo[512], svof[512];
            snprintf(otok, sizeof(otok), "%s/tokens.txt", off_dir);
            snprintf(svm, sizeof(svm), "%s/model.rknn", off_dir);
            snprintf(svo, sizeof(svo), "%s/model.int8.onnx", off_dir);
            snprintf(svof, sizeof(svof), "%s/model.onnx", off_dir);
            struct stat st;
            const int sv_rknn = stat(svm, &st) == 0;
            const int sv_int8 = stat(svo, &st) == 0;
            const int sv_onnx = sv_int8 || stat(svof, &st) == 0;
            // Plain-ONNX SenseVoice is the ONLY cpu path here.  A paraformer
            // rknn TRIPLE has no model.rknn, so "!sv_rknn" must not mean
            // "cpu" -- that hands .rknn files to onnxruntime and silently
            // kills the board's default second pass.
            const int pass2_onnx = sv_onnx && !sv_rknn;
            pass2_sv = sv_rknn || sv_onnx;
            SherpaOnnxOfflineRecognizerConfig fc;
            memset(&fc, 0, sizeof(fc));
            fc.model_config.provider = pass2_onnx ? "cpu" : "rknn";
            fc.model_config.tokens = otok;
            // NPU: one core map.  CPU SenseVoice: threads are real cores,
            // the second pass must not stall the draft behind it.
            fc.model_config.num_threads = pass2_onnx ? 4 : 1;
            if (pass2_sv) {
                fc.model_config.sense_voice.model = sv_rknn ? svm
                    : (sv_int8 ? svo : svof);
                fc.model_config.sense_voice.language = "auto";
                fc.model_config.sense_voice.use_itn = 1;
            } else {
                snprintf(para, sizeof(para),
                         "%s/encoder.rknn,%s/predictor.rknn,%s/decoder.rknn",
                         off_dir, off_dir, off_dir);
                fc.model_config.paraformer.model = para;
            }
            frec = SherpaOnnxCreateOfflineRecognizer(&fc);
            if (frec) {
                sring.base = 0;
                sring.count = 0;
                printf("second pass: %s (%s) from %s\n",
                       pass2_sv ? "sensevoice" : "offline paraformer",
                       pass2_onnx ? "cpu" : "rknn", off_dir);
                // Cold-start warm-up, same reason as the streaming one
                // below: the FIRST inference on a fresh onnxruntime session
                // spins up its thread pool and stalls for seconds, and it
                // would land on the first sentence of the first playback.
                // On a loaded host (PC, video decoding in parallel) that
                // storm overflowed the capture queue and punched holes into
                // the audio timeline (72 dropped 32 ms chunks within the
                // first 15 s, measured 2026-10-07) -- the streaming model
                // then decoded from wrong positions and the drafts came out
                // stuttered and duplicated.  A silent 0.1 s decode pays the
                // cost here, before any audio exists.
                const double tw = now_s();
                const SherpaOnnxOfflineStream *ws =
                    SherpaOnnxCreateOfflineStream(frec);
                if (ws) {
                    enum { WW = 1600 };
                    static float zw[WW];
                    SherpaOnnxAcceptWaveformOffline(ws, 16000, zw, WW);
                    SherpaOnnxDecodeOfflineStream(frec, ws);
                    const SherpaOnnxOfflineRecognizerResult *wr =
                        SherpaOnnxGetOfflineStreamResult(ws);
                    if (wr)
                        SherpaOnnxDestroyOfflineRecognizerResult(
                            (SherpaOnnxOfflineRecognizerResult *)wr);
                    SherpaOnnxDestroyOfflineStream(ws);
                    printf("second pass warm-up in %.2f s\n", now_s() - tw);
                    fflush(stdout);
                }
            } else {
                fprintf(stderr, "second pass: %s failed to load, continuing without it\n",
                        off_dir);
            }
            // The long-window twin, paraformer only: rule3 allows 15 s
            // utterances and the 10 s model above reads ~12 s, so anything
            // longer decodes truncated and gets rejected.  SenseVoice reads
            // ~30 s natively and does not need it.
            const char *lng_dir = getenv("SHERPA_PASS2_LONG_DIR");
            if (frec && !pass2_sv && lng_dir && *lng_dir) {
                char lpara[1024], ltok[512];
                snprintf(lpara, sizeof(lpara),
                         "%s/encoder.rknn,%s/predictor.rknn,%s/decoder.rknn",
                         lng_dir, lng_dir, lng_dir);
                snprintf(ltok, sizeof(ltok), "%s/tokens.txt", lng_dir);
                fc.model_config.paraformer.model = lpara;
                fc.model_config.tokens = ltok;
                frec_long = SherpaOnnxCreateOfflineRecognizer(&fc);
                if (frec_long)
                    printf("second pass long: 20 s paraformer (rknn) from %s\n", lng_dir);
                else
                    fprintf(stderr, "second pass long: %s failed to load, "
                            "long utterances keep their draft\n", lng_dir);
            }
        }
        ostream = SherpaOnnxCreateOnlineStream(orec);
        if (!ostream) { fprintf(stderr, "stream init failed\n"); return 1; }
        sq_init(&sq);
        // Warm-up decode: the FIRST inference on a cold onnxruntime session
        // takes seconds on the A72 (kernel init + thread pool spin-up), and
        // that delay lands on the first sentence of the first playback after
        // every process start (measured 2026-09-30: first playback's first
        // line 8.0 s, every later playback 0.5-0.8 s with identical audio
        // flow -- reader had data at 0.27 s in both cases).  Feed a second
        // of silence, run the decode once, throw it away.
        {
            const double tw = now_s();
            enum { WARM = 16000 };
            static float zeros[WARM];
            SherpaOnnxOnlineStreamAcceptWaveform(ostream, 16000, zeros, WARM);
            while (SherpaOnnxIsOnlineStreamReady(orec, ostream))
                SherpaOnnxDecodeOnlineStream(orec, ostream);
            SherpaOnnxOnlineStreamReset(orec, ostream);
            printf("streaming warm-up in %.2f s\n", now_s() - tw);
        }
        printf("streaming mode: %s from %s\n",
               stream_zf ? "online zipformer transducer (npu)"
                         : "online paraformer (cpu)",
               stream_dir);
    }

    static Queue queue;
    queue_init(&queue);

    const double t_listen = now_s();        // origin for the t= field in the log
    term_map_load(getenv("SHERPA_TERM_MAP"));
    const double deadline = seconds > 0 ? now_s() + seconds : 0.0;

    // Designated, not positional: fields have been added to this struct
    // twice, and a positional list would silently shift them.
    CaptureArgs ca = {
        .device = device, .vad = vad, .dump = dump, .queue = &queue,
        .t_listen = t_listen, .deadline = deadline, .sq = orec ? &sq : NULL,
        // The ring exists only for the second pass, and ring_fed is written by
        // the capture thread under ring_lock and read by the decode thread at an
        // endpoint, so it is a pointer rather than a value.
        .ring = frec ? &sring : NULL, .ring_fed = &sring_fed,
        .ring_lock = &sring_lock,
    };

    pthread_t cap_thr;
    if (pthread_create(&cap_thr, NULL, capture_thread, &ca) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
    printf("listening on %s (Ctrl-C to stop)\n", device); fflush(stdout);

    // P2 worker: NPU offline decode + adopt decision + overlay/log output.
    // Runs detached from the decode thread; the sub_fifo pointer is not
    // shared (the worker opens its own non-blocking connection per line).
    P2Ctx p2;
    memset(&p2, 0, sizeof(p2));
    pthread_mutex_init(&p2.lock, NULL);
    pthread_cond_init(&p2.ready, NULL);
    p2.frec = frec;
    p2.frec_long = frec_long;
    p2.long_at = 11 * 16000;   // the 10 s model reads ~12 s; past that, 20 s twin
    p2.sv_backend = pass2_sv;
    p2.sub_path = sub_path;
    p2.punct = punct;

    // The P2 worker body lives above main(); started here so frec/sub_path
    // are final.
    pthread_create(&p2.thr, NULL, p2_worker, &p2);

    // ---- streaming decode loop ------------------------------------------
    // One owner for the online stream: samples come off the queue, get fed in,
    // and every time the model says it is ready the stream is decoded and the
    // new text printed.  Printing goes through subtitle_commit so a line is
    // only emitted once the endpoint confirms the utterance ended -- a
    // streaming model rewrites earlier words freely, and printing the raw
    // result every tick would scroll the same sentence over and over.
    if (orec) {
        // Opt-in raw dump; see the fprintf below.
        FILE *g_raw_dump = getenv("SHERPA_RAW_DUMP") ? fopen(getenv("SHERPA_RAW_DUMP"), "w") : NULL;
        SubtitleState sub;
        subtitle_init(&sub);
        char line[1024], tail[1024] = "", raw_tail[1024] = "", scr[1200];
        size_t raw_tail_len = 0;  // comparison state before punctuation/terms
        size_t prev_drawn = 0;    // width of the last redraw, for padding
        float *chunk = NULL;
        int n = 0, fed = 0, finals = 0;
        int skipped = 0;                 // spans never handed to the worker
        // How long a corrected line stays alone on screen before the next
        // draft may overwrite it.  2 s: enough to read a sentence, short
        // enough that continuous speech keeps moving.  SHERPA_DWELL overrides.
        const double overlay_dwell = getenv("SHERPA_DWELL")
            ? atof(getenv("SHERPA_DWELL")) : 2.0;
        double t_first = -1.0;
        while (sq_pop(&sq, &chunk, &n)) {
            // Session boundary (data after a dry period): drop the pending
            // stream context BEFORE feeding.  Its endpoint would otherwise
            // fire on the new session's leading silence and the reset would
            // discard whatever of the new sentence had already been fed --
            // the first line of every playback lost its opening words this
            // way (那下边呢我们就要定量计算, measured 2026-09-30).  The
            // second-pass ring restarts from the same boundary so the new
            // utterance's span is not stitched onto the old session's audio.
            if (__atomic_load_n(&ca.new_session, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&ca.new_session, 0, __ATOMIC_RELEASE);
                SherpaOnnxOnlineStreamReset(orec, ostream);
                sub.committed = 0;
                raw_tail_len = 0;
                raw_tail[0] = '\0';
                tail[0] = '\0';
                if (prev_drawn) { fputs("\n", stdout); prev_drawn = 0; }
                if (frec) {
                    pthread_mutex_lock(&sring_lock);
                    sring.base = sring_fed;
                    sring.count = 0;
                    pthread_mutex_unlock(&sring_lock);
                }
                printf("[session] audio resumed: stream context reset\n");
                fflush(stdout);
            }
            SherpaOnnxOnlineStreamAcceptWaveform(ostream, 16000, chunk, n);
            free(chunk);
            chunk = NULL;
            fed += n;
            while (SherpaOnnxIsOnlineStreamReady(orec, ostream))
                SherpaOnnxDecodeOnlineStream(orec, ostream);
            const SherpaOnnxOnlineRecognizerResult *r =
                SherpaOnnxGetOnlineStreamResult(orec, ostream);
            if (!r) continue;
            const int ep = SherpaOnnxOnlineStreamIsEndpoint(orec, ostream);
            const char *txt = r->text ? r->text : "";
            if (*txt && t_first < 0.0) t_first = now_s() - t_listen;
            const int got = subtitle_commit(&sub, txt, line, sizeof(line), ep);
            // Second pass, at the endpoint, on the NPU -- asynchronously: the
            // utterance's audio span is copied out of the ring and handed to
            // the worker, so this thread never waits for the NPU here.
            if (ep && got > 0 && frec) {
                uint64_t a_from = 0, a_to = 0;
                pthread_mutex_lock(&sring_lock);
                const int have = pass2_audio_span(sring_fed, sring.base,
                                                  &a_from, &a_to);
                float *base = NULL;
                if (have) {
                    base = (float *)malloc((size_t)(a_to - a_from) * sizeof(float));
                    if (base)
                        for (uint64_t i = a_from; i < a_to; ++i)
                            base[i - a_from] = ring_at(&sring, i);
                }
                pthread_mutex_unlock(&sring_lock);

                if (base) {
                    const int span_n = (int)(a_to - a_from);
                    const int lead = pass2_trim_lead(base, span_n,
                                                     STREAM_TRIM_RMS);
                    if (lead >= span_n) {
                        // Nothing but silence: nothing to compare, the first
                        // pass stands.
                        free(base);
                        ++skipped;
                    } else {
                        // Keep the allocation base for the worker's free(),
                        // but remove the quiet prefix from the decoded audio.
                        const int speech_n = span_n - lead;
                        if (lead)
                            memmove(base, base + lead,
                                    (size_t)speech_n * sizeof(*base));
                        // Hand the span to the worker; it owns the memory.
                        pthread_mutex_lock(&p2.lock);
                        free(p2.slot.audio);
                        p2.slot.audio = base;
                        p2.slot.n = speech_n;
                        snprintf(p2.slot.first, sizeof(p2.slot.first), "%s", line);
                        p2.busy = 1;
                        __atomic_store_n(&p2.pending, 1, __ATOMIC_RELEASE);
                        __atomic_store_n(&p2.enq_us,
                                         (int64_t)(now_s() * 1e6), __ATOMIC_RELEASE);
                        pthread_cond_signal(&p2.ready);
                        pthread_mutex_unlock(&p2.lock);
                    }
                }
            }
            // Diagnostic: the model's own text, before any slicing or filtering.
            // Garbage on screen has two possible sources -- the model emitted
            // junk, or this file cut it wrongly -- and the only way to tell is
            // to keep the unsliced string.  Opt-in; it is a few MB per minute.
            if (g_raw_dump) {
                fprintf(g_raw_dump, "%.3f ep=%d len=%zu [%s]\n",
                        now_s() - t_listen, ep, strlen(txt), txt);
            }
            // Provisional display, so a long utterance visibly moves even before
            // its endpoint arrives.  Deliberately NOT \r: a carriage return
            // redraws the line in place, and these strings are UTF-8, so a
            // partial code point left on screen from the previous redraw
            // interleaves with the new one and the terminal shows mojibake.
            //
            // This MUST happen before the result is destroyed below.  txt points
            // into r, and r owns the string: reading it afterwards is a
            // use-after-free, which is what printed the "cM?" and "#}|*"
            // garbage.  It looked like the model was emitting junk, because the
            // freed bytes did change from tick to tick -- but the committed
            // lines, which are sliced before the destroy, were always correct.
            // Only when the provisional text has actually moved.  This loop
            // runs at the capture rate, ~31 times a second, while the model
            // only revises when it decodes, so printing unconditionally put
            // one sentence on screen 120 times over -- 7247 lines for 114
            // distinct strings.  The gate is in the subtitle module so it can
            // be tested without a recogniser.
            if (subtitle_tail_changed(&sub, txt, raw_tail,
                                      sizeof(raw_tail), &raw_tail_len) > 0) {
                char display_tail[1024];
                snprintf(display_tail, sizeof(display_tail), "%s", raw_tail);
                // Draft punctuation, opt-in (SHERPA_DRAFT_PUNCT): the finals
                // already carry punctuation (pass2), but drafts otherwise
                // stay raw for the whole utterance and read as unfinished
                // next to them.  The tail gate fires only when the text
                // actually moved (~a dozen times per utterance), so a call
                // per change is affordable on a PC -- but each call is a
                // real inference, and the board's A72 runs the streaming
                // path at RTF 0.82 with little headroom, so there it stays
                // off unless explicitly asked for.
                if (punct && draft_punct) {
                    pthread_mutex_lock(&g_punct_lock);
                    const char *pt = SherpaOfflinePunctuationAddPunct(punct, display_tail);
                    if (pt && *pt) snprintf(display_tail, sizeof(display_tail), "%s", pt);
                    if (pt) SherpaOfflinePunctuationFreeText(pt);
                    pthread_mutex_unlock(&g_punct_lock);
                }
                // Term table applies to drafts too: the wrong term would
                // otherwise sit on screen for the whole utterance until the
                // correction replaces it.
                term_map_apply(display_tail, sizeof display_tail);
                // A raw revision can normalize to the same visible line
                // (for example 站稳态 -> 暂稳态).  Keep the raw comparison
                // separate from the rendered one so neither case redraws
                // the same subtitle on every capture period.
                if (strcmp(display_tail, tail) != 0) {
                    snprintf(tail, sizeof(tail), "%s", display_tail);
                    // Redraw in place.  One line that grows, not a new line
                    // per revision: 14 growing revisions printed one after
                    // another look exactly like the same sentence repeating.
                    const int n = subtitle_render(scr, sizeof(scr), tail, prev_drawn);
                    if (n > 0) { fwrite(scr, 1, n, stdout); prev_drawn = strlen(tail); }
                    fflush(stdout);
                    // Real-time draft to the overlay: the CPU streaming text
                    // goes on screen as it changes; the NPU final replaces it.
                    // Dwell starts when the worker actually displays that
                    // final, and a pending correction also holds the draft
                    // back (up to 3 s in case the job is replaced/dropped).
                    const double since_final =
                        now_s() - __atomic_load_n(&p2.final_us, __ATOMIC_ACQUIRE) / 1e6;
                    const double since_enq =
                        now_s() - __atomic_load_n(&p2.enq_us, __ATOMIC_ACQUIRE) / 1e6;
                    const int in_flight = __atomic_load_n(&p2.pending, __ATOMIC_ACQUIRE);
                    if (since_final > overlay_dwell
                        && (!in_flight || since_enq > 3.0)) {
                        if (!sub_fifo && sub_path)
                            sub_fifo = fdopen(open(sub_path, O_WRONLY | O_NONBLOCK), "w");
                        if (sub_fifo) {
                            if (fputs(tail, sub_fifo) < 0 || fputc('\n', sub_fifo) < 0
                                || fflush(sub_fifo) != 0) {
                                fclose(sub_fifo);
                                sub_fifo = NULL;
                            }
                        }
                    }
                }
            }
            SherpaOnnxDestroyOnlineRecognizerResult(
                (SherpaOnnxOnlineRecognizerResult *)r);
            if (ep) {
                // A new utterance starts: drop the text the model has already
                // confirmed, so the next line is not a continuation of this one.
                SherpaOnnxOnlineStreamReset(orec, ostream);
                sub.committed = 0;
                // The ring restarts here too, and this is not optional.  Left
                // alone it kept accumulating, so the second pass was handed the
                // ring's whole 25 s -- the previous sentence, this one and the
                // silence between -- and answered with only the first of them.
                // Measured: 25 s produced 36 characters where the streaming pass
                // had 72, so every revision was rejected for being shorter.
                if (frec) {
                    pthread_mutex_lock(&sring_lock);
                    sring.base = sring_fed;
                    sring.count = 0;
                    pthread_mutex_unlock(&sring_lock);
                }
                // Forget the provisional line too: the next utterance opens with
                // the same words, and without this the gate would swallow its
                // first line as "unchanged" and the screen would appear to stall.
                raw_tail_len = 0;
                raw_tail[0] = '\0';
                tail[0] = '\0';
                if (prev_drawn) { fputs("\n", stdout); prev_drawn = 0; }
            }
        }
        if (chunk) free(chunk);
        if (g_raw_dump) fclose(g_raw_dump);
        SherpaOnnxOnlineStreamInputFinished(ostream);
        while (SherpaOnnxIsOnlineStreamReady(orec, ostream))
            SherpaOnnxDecodeOnlineStream(orec, ostream);
        const SherpaOnnxOnlineRecognizerResult *fr =
            SherpaOnnxGetOnlineStreamResult(orec, ostream);
        if (fr) {
            if (subtitle_commit(&sub, fr->text ? fr->text : "", line,
                                sizeof(line), 1) > 0)
                printf("[%d] t=%6.2fs fed=%6.2fs -> %s (final)\n",
                       ++finals, now_s() - t_listen, fed / 16000.0, line);
            SherpaOnnxDestroyOnlineRecognizerResult(
                (SherpaOnnxOnlineRecognizerResult *)fr);
        }
        if (t_first > 0.0)
            printf("first text after %.2f s, %d line(s)\n", t_first, finals);
        // Capture-rate check.  read is what the device delivered, fed is what
        // the recogniser consumed: a gap that stays constant is the startup
        // offset between "listening on" and the first audio, but a gap that
        // GROWS means the capture side is losing audio.
        printf("stream: read %.2f s, fed %.2f s, difference %.2f s "
               "(constant = startup offset, growing = audio lost)\n",
               ca.stream_read / 16000.0, fed / 16000.0,
               (ca.stream_read > (uint64_t)fed
                    ? ca.stream_read - (uint64_t)fed : 0) / 16000.0);
        printf("stream: %llu chunks with speech, %llu silent (all forwarded; "
               "gate rms %.4f), %llu chunks DROPPED for being too slow\n",
               (unsigned long long)ca.stream_voiced,
               (unsigned long long)ca.stream_silent, STREAM_GATE_RMS,
               (unsigned long long)sq.dropped);
        sq_free(&sq);
        // Stop the P2 worker before returning: this thread was its only
        // producer, and its counters are only final once it has joined.
        pthread_mutex_lock(&p2.lock);
        p2.closing = 1;
        if (p2.slot.audio) { free(p2.slot.audio); p2.slot.audio = NULL; p2.busy = 0; }
        pthread_cond_broadcast(&p2.ready);
        pthread_mutex_unlock(&p2.lock);
        pthread_join(p2.thr, NULL);
        if (p2.adopted || p2.rejected || skipped)
            printf("second pass: %d adopted, %d rejected, %d skipped (silence)\n",
                   p2.adopted, p2.rejected, skipped);
        SherpaOnnxDestroyOnlineStream((SherpaOnnxOnlineStream *)ostream);
        SherpaOnnxDestroyOnlineRecognizer(orec);
        fflush(stdout);
        return 0;
    }

    // Decode loop: the only thing this thread does.  Because capture runs
    // independently, a slow decode now costs latency, never audio.
    uint64_t recognized = 0;
    uint64_t routed_long = 0;
    Job job;
    while (queue_pop(&queue, &job)) {
        const double t0 = now_s();
        // Route by window length.  The 10 s model silently truncates a longer
        // window, so anything that could overflow it goes to the 20 s model
        // instead; without this the tail of a long utterance just disappears.
        const int use_long = rec_long && (job.n >= long_threshold);
        const SherpaOnnxOfflineRecognizer *use_rec = use_long ? rec_long : rec;
        if (use_long) ++routed_long;
        const SherpaOnnxOfflineStream *stream = SherpaOnnxCreateOfflineStream(use_rec);
        SherpaOnnxAcceptWaveformOffline(stream, 16000, job.audio, job.n);
        SherpaOnnxDecodeOfflineStream(use_rec, stream);
        const SherpaOnnxOfflineRecognizerResult *r = SherpaOnnxGetOfflineStreamResult(stream);
        // t=     wall clock since listening began: when the text became
        //        available, so end-of-speech -> text latency is measurable.
        // win=    the window in capture-relative seconds; indexes the optional
        //        .pcm dump directly, so an offline silero curve or offline
        //        decode lines up against what the live path did.
        // lag=    capture position minus elapsed time.  Should now hover near
        //        zero: if it walks, capture is not keeping up again.
        // q=      segments still queued at push time, i.e. the real decode
        //        backlog -- not the depth at print time, which is always 0 for
        //        a single segment in flight.
        printf("[%llu] t=%6.2fs win=%7.2f-%7.2f (%.2fs speech) lag=%+.2fs q=%d%s"
               " -> %s  (decode %.0f ms)\n",
               (unsigned long long)++recognized,
               now_s() - t_listen,
               job.win_start, job.win_end,
               job.n / 16000.0,
               job.fed_at / 16000.0 - (now_s() - t_listen),
               job.q_at_push,
               use_long ? " [20s]" : "",
               r->text ? r->text : "",
               (now_s() - t0) * 1000.0);
        fflush(stdout);
        SherpaOnnxDestroyOfflineRecognizerResult(r);
        SherpaOnnxDestroyOfflineStream(stream);
        free(job.audio);
    }

    sq_close(&sq);
    pthread_join(cap_thr, NULL);
    if (dump) { fclose(dump); printf("raw mic dumped\n"); }
    // Stop the P2 worker after the decode thread is gone (it is the producer).
    pthread_mutex_lock(&p2.lock);
    p2.closing = 1;
    if (p2.slot.audio) { free(p2.slot.audio); p2.slot.audio = NULL; p2.busy = 0; }
    pthread_cond_broadcast(&p2.ready);
    pthread_mutex_unlock(&p2.lock);
    pthread_join(p2.thr, NULL);
    printf("stopped, %llu segment(s) recognized, %d device reopen(s), "
           "%llu dropped, %llu via 20s window, %d forced split(s), "
           "%d latency slice(s), %d gated, peak queue %d, "
           "pass2 adopted %d / rejected %d\n",
           (unsigned long long)recognized, ca.reopens,
           (unsigned long long)queue.dropped,
           (unsigned long long)routed_long, ca.forced_splits,
           ca.latency_slices, ca.gated, queue.peak,
           p2.adopted, p2.rejected);
    if (rec_long) SherpaOnnxDestroyOfflineRecognizer(rec_long);
    SherpaOnnxDestroyVoiceActivityDetector(vad);
    SherpaOnnxDestroyOfflineRecognizer(rec);
    return 0;
}
