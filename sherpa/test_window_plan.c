// SPDX-License-Identifier: Apache-2.0
//
// Tests for window_plan -- the segment-length policy of the live capture loop.
//
// The bug these lock down: sherpa-onnx's max_speech_duration is advisory
// (c-api.h: "the detector temporarily uses a higher threshold to encourage a
// split"), so continuous news speech never splits and produced windows of
// 21-24 s.  The 20 s model truncated them ("Number of input frames 381 is too
// large. Truncate it to 333 frames"), losing the tail of every long sentence.
//
// Build and run (host, no board needed):
//   gcc -O2 -Wall -o /tmp/t sherpa/test_window_plan.c sherpa/window_plan.c
//   /tmp/t

#include <stdio.h>
#include <string.h>

#include "window_plan.h"

#define SR 16000
#define HEAD   (SR * 1.2)   // headroom prepended to a fitting segment
#define TAIL   (SR * 0.6)   // trailing silence trimmed from a fitting segment
#define MAXWIN (SR * 18)    // ceiling handed to window_plan by the capture loop
#define LAT_CAP (SR * 25 / 10)  // 2.5 s latency slice for the timer trigger

static int failures = 0;
static int checks = 0;

static void check(int ok, const char *what, const char *detail) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("  FAIL  %s\n        %s\n", what, detail);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void check_u64(uint64_t got, uint64_t want, const char *what) {
    char buf[192];
    snprintf(buf, sizeof(buf), "expected %llu, got %llu",
             (unsigned long long)want, (unsigned long long)got);
    check(got == want, what, buf);
}

int main(void) {
    WindowPlan w;

    // ---- 1. a segment that fits keeps the emission-anchored window --------
    // 2.4 s of speech ending now: window = speech + head, ending at
    // total_fed - tail, exactly as the live loop has always done it.
    uint64_t n = window_plan(32800, SR * 24 / 10, HEAD, TAIL, 0, 100000, MAXWIN, &w);
    check_u64(n, SR * 36 / 10, "fitting segment decodes speech+head");
    check_u64(w.from, 32800, "  ...anchored at total_fed - tail - span");
    check_u64(w.end, 100000 - TAIL, "  ...ending before the trailing silence");
    check(w.split == 0, "fitting segment is not marked split", "split should be 0");

    // ---- 2. THE BUG: a 23 s segment must not be handed over whole -------
    // This is the shape the news clip produced.  Before the fix the span came
    // back at 24.2 s -- past the 20 s model, which silently truncated it.
    n = window_plan(50000, SR * 23, HEAD, TAIL, 0, 500000, MAXWIN, &w);
    check(n <= MAXWIN, "23 s segment is capped at the model ceiling", "span exceeded the ceiling");
    check(w.split == 1, "  ...and is flagged for a VAD reset", "split should be 1");

    // ---- 3. the cut keeps the run's head, so nothing is skipped -----------
    n = window_plan(50000, SR * 23, HEAD, TAIL, 0, 500000, MAXWIN, &w);
    check_u64(w.from, 50000, "capped segment starts at the run start");
    check_u64(w.end, 50000 + MAXWIN, "  ...and runs exactly the ceiling");

    // ---- 3b. THE COORDINATE BUG: consecutive cuts must tile --------------
    // Cutting on the VAD's internal seg->start left 2-5 s holes between
    // windows (28% of a 100 s news clip went undecoded).  Once the caller
    // tracks the run in ring-absolute coordinates, the next window must begin
    // exactly where the last one ended.
    uint64_t prev_end = 0;
    uint64_t pos = 50000;
    int tiled = 1;
    for (int k = 0; k < 4; ++k) {
        n = window_plan(pos, SR * 23, HEAD, TAIL, 0, pos + SR * 23, MAXWIN, &w);
        if (k == 0) {
            if (w.from != 50000) tiled = 0;
        } else if (w.from != prev_end) {
            tiled = 0;
        }
        prev_end = w.end;
        pos = w.end;
    }
    check(tiled, "4 consecutive cuts tile the stream with no gap", "windows did not abut");
    check_u64(prev_end, 50000 + 4 * MAXWIN, "  ...covering exactly 4 windows' worth");

    // ---- 4. arbitrarily long segments stay bounded -----------------------
    n = window_plan(1000, SR * 600, HEAD, TAIL, 0, SR * 600, MAXWIN, &w);
    check_u64(n, MAXWIN, "10 minute segment is still exactly one window");
    check(w.split == 1, "  ...and is flagged", "split should be 1");

    // ---- 5. a stale segment is clamped to the ring, not decoded blind ----
    n = window_plan(32800, SR * 24 / 10, HEAD, TAIL, 40000, 100000, MAXWIN, &w);
    check_u64(w.from, 40000, "request older than the ring is clamped to ring base");
    check_u64(n, (100000 - TAIL) - 40000, "  ...and the span shrinks to match");

    // ---- 6. a capped segment whose head fell out of the ring -------------
    n = window_plan(1000, SR * 40, HEAD, TAIL, 40000, SR * 40, MAXWIN, &w);
    check_u64(w.from, 40000, "capped segment is clamped to ring base too");
    check(n <= MAXWIN, "  ...and still respects the ceiling", "span exceeded the ceiling");

    // ---- 7. a segment that is entirely gone yields nothing ----------------
    n = window_plan(1000, SR * 24 / 10, HEAD, TAIL, 50000, 50000, MAXWIN, &w);
    check_u64(n, 0, "segment entirely older than the ring decodes nothing");

    // ---- 8. THE LATENCY CAP ----------------------------------------------
    // Measured 2026-09-28 on a narration clip: subtitle latency tracked segment
    // length almost exactly (5.4 s segment -> 6.8 s, 18.0 s -> 24.8 s), because
    // a window is only emitted once the VAD closes its segment.  Capping the
    // window SIZE does not bound latency -- the segment still has to land
    // first.  So the capture loop needs a second, independent trigger: a
    // time slice that fires on its own and never waits for the VAD.
    //
    // CAP is that slice; latency is then bounded by CAP + decode time
    // regardless of what the content turns out to be.

    // Below the cap: stay quiet, so short gaps do not force a window.
    n = plan_latency_window(0, SR * 2, LAT_CAP, 0, 0, &w);
    check_u64(n, 0, "no emission before the latency cap is reached");

    // Past the cap: emit one cap-sized slice.  The remainder is deliberately
    // left for the next slice rather than swallowed, which is what stops a
    // long run from turning into one oversized window.
    n = plan_latency_window(0, SR * 5, LAT_CAP, 0, 0, &w);
    check_u64(n, LAT_CAP, "past the cap exactly one cap-sized slice is emitted");
    check_u64(w.from, 0, "  ...starting where the previous window ended");
    check_u64(w.end, LAT_CAP, "  ...stopping at the cap, not at the newest sample");

    // One period short of the cap is still quiet (no off-by-one).
    n = plan_latency_window(0, LAT_CAP - 512, LAT_CAP, 0, 0, &w);
    check_u64(n, 0, "one period short of the cap does not emit");

    // A long run must not produce one huge window: the cap bounds the span.
    n = plan_latency_window(0, SR * 30, LAT_CAP, 0, 0, &w);
    check_u64(n, LAT_CAP, "a 30 s run still emits exactly one cap-sized window");
    check_u64(w.from, 0, "  ...anchored at the previous window's end");

    // Successive slices tile the stream: feeding w.end back in as last_emit
    // continues from exactly where the previous one stopped, so no audio is
    // decoded twice and none is skipped.  This is what keeps the timer
    // trigger from re-transcribing or dropping anything.
    uint64_t cur = 0, tiled_total = 0, slice_end = 0;
    int slices_abut = 1;
    for (int k = 0; k < 6; ++k) {
        n = plan_latency_window(cur, SR * 30, LAT_CAP, 0, 0, &w);
        if (k > 0 && w.from != slice_end) slices_abut = 0;
        slice_end = w.end;
        cur = w.end;
        tiled_total += n;
    }
    check(slices_abut, "successive latency slices tile the stream", "slices did not abut");
    check_u64(tiled_total, 6 * LAT_CAP, "  ...covering exactly six slices");

    // Leading context is pulled in from before the previous window, as the
    // VAD path does, but must never reach behind the ring's live region.
    n = plan_latency_window(SR * 10, SR * 15, LAT_CAP, SR, 0, &w);
    check(n > 0 && w.from < SR * 10, "latency window includes leading context",
          "expected context before the previous window end");
    n = plan_latency_window(SR * 10, SR * 15, LAT_CAP, SR * 20, SR * 5, &w);
    check_u64(w.from, SR * 5, "  ...but never older than the ring base");

    // ---- 9. THE TWO TRIGGERS MUST NOT FIGHT ---------------------------
    // With both a VAD trigger and the latency cap live, a segment that lands
    // late covers audio the cap has already transcribed.  Handing that window
    // to the decoder re-emits whole paragraphs verbatim.  A VAD window may
    // therefore reach back at most `head` before run_start, and nothing
    // already emitted.

    // Segment entirely already emitted: drop it, do not re-transcribe.
    WindowPlan dup = { SR * 10, SR * 20, 0 };
    n = clamp_window_start(&dup, SR * 20, HEAD, 0);
    check_u64(n, 0, "a VAD segment already fully emitted is dropped");

    // Segment straddles run_start: keep only the new part, plus head.
    dup.from = SR * 10; dup.end = SR * 30; dup.split = 0;
    n = clamp_window_start(&dup, SR * 25, HEAD, 0);
    check_u64(dup.from, SR * 25 - HEAD, "a straddling segment keeps only new audio");
    check_u64(n, (SR * 30) - (SR * 25 - HEAD), "  ...and its length shrinks to match");

    // Segment entirely after run_start: untouched, boundaries are natural.
    dup.from = SR * 30; dup.end = SR * 34; dup.split = 0;
    n = clamp_window_start(&dup, SR * 20, HEAD, 0);
    check_u64(dup.from, SR * 30, "a segment beyond run_start is left alone");
    check_u64(n, SR * 4, "  ...with its length unchanged");

    // A run_start near the start of the stream must not underflow the head.
    // With nothing emitted yet there is nothing to clamp against, so a window
    // that is already inside the live region is left exactly as planned.
    dup.from = SR * 10; dup.end = SR * 30; dup.split = 0;
    n = clamp_window_start(&dup, 0, HEAD, 0);
    check_u64(dup.from, SR * 10, "a window is untouched when nothing is emitted yet");
    check_u64(n, SR * 20, "  ...with its length unchanged");

    // The ring only floors the clamp: when the head allowance is available it
    // is used, so context still reaches back before run_start.
    dup.from = SR * 10; dup.end = SR * 30; dup.split = 0;
    n = clamp_window_start(&dup, SR * 25, HEAD, SR * 20);
    check_u64(dup.from, SR * 25 - HEAD, "the head allowance is used when the ring allows it");
    dup.from = SR * 10; dup.end = SR * 30; dup.split = 0;
    n = clamp_window_start(&dup, SR * 25, HEAD, SR * 24);
    check_u64(dup.from, SR * 24, "  ...and the ring base floors it when it cannot");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
