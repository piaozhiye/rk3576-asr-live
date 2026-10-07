// SPDX-License-Identifier: Apache-2.0
//
// Window planning, extracted from the sherpa_live capture loop so the
// segment-length policy is testable on a host.  See window_plan.h.

#include "window_plan.h"

uint64_t window_plan(uint64_t run_start, uint64_t seg_len,
                     uint64_t head, uint64_t tail,
                     uint64_t ring_base, uint64_t total_fed,
                     uint64_t max_window,
                     WindowPlan *out) {
    WindowPlan w;
    w.split = 0;

    // A segment that will not fit is cut at the ceiling rather than handed to
    // a model window too small to hold it.  max_speech_duration in the VAD
    // config does NOT bound this: per c-api.h it only raises the threshold
    // "to encourage a split", so continuous speech (a news read with no pause
    // long enough to close a segment) grows without limit.
    if (seg_len + head > max_window) {
        // Take the head of the segment, up to exactly max_window, and ask the
        // caller to clear the VAD.  Everything after the cut stays in the
        // stream and is re-detected as a fresh segment, so no audio is lost --
        // which a "keep the newest max_window" policy would not manage.
        uint64_t from = run_start;
        uint64_t end = run_start + max_window;
        if (from < ring_base) from = ring_base;   // head aged out of the ring
        if (end > total_fed) end = total_fed;
        w.from = from;
        w.end = end;
        w.split = 1;
        if (out) *out = w;
        return end > from ? end - from : 0;
    }

    // Emission-anchored window: end sits `tail` before the newest sample, and
    // the window reaches back far enough to cover the segment plus headroom.
    uint64_t end = total_fed > tail ? total_fed - tail : 0;
    uint64_t from = end > seg_len + head ? end - seg_len - head : 0;
    if (from < ring_base) from = ring_base;
    if (end > total_fed) end = total_fed;

    w.from = from;
    w.end = end;
    if (out) *out = w;
    return end > from ? end - from : 0;
}

uint64_t plan_latency_window(uint64_t last_emit, uint64_t total_fed,
                             uint64_t cap, uint64_t head, uint64_t ring_base,
                             WindowPlan *out) {
    WindowPlan w;
    w.split = 0;

    // Nothing to do until the slice has actually elapsed.
    if (total_fed < last_emit || total_fed - last_emit < cap) {
        if (out) *out = w;
        return 0;
    }

    // Cover the audio since the last window, plus a little context in front
    // of it: paraformer decodes a bare 2.5 s window poorly, and the VAD path
    // has always prepended headroom for the same reason.  The overlap is
    // deliberate -- the caller advances last_emit to w.end, so context is
    // re-decoded rather than skipped.
    uint64_t from = last_emit > head ? last_emit - head : 0;
    if (from < ring_base) from = ring_base;
    uint64_t end = total_fed;

    // The cap bounds the span, so a long run cannot turn into one huge
    // window; the remainder is picked up by the next slice.
    if (end > from + cap) end = from + cap;

    w.from = from;
    w.end = end;
    if (out) *out = w;
    return end > from ? end - from : 0;
}

uint64_t clamp_window_start(WindowPlan *wp, uint64_t run_start,
                            uint64_t head, uint64_t ring_base) {
    // No new audio at all: the segment ends at or before what has already
    // been emitted, so the whole window is a duplicate.  The head allowance
    // below attaches context to real new audio; with none, it would just
    // re-transcribe 1.2 s of stale audio as a phantom subtitle line.
    if (wp->end <= run_start) {
        wp->from = wp->end;
        return 0;
    }
    // Otherwise the earliest this window may start is `head` before what has
    // already been emitted -- that much overlap is the context allowance, and
    // reusing it is the point.  ring_base only floors it.
    uint64_t earliest = run_start > head ? run_start - head : 0;
    if (earliest < ring_base) earliest = ring_base;
    if (wp->from < earliest) wp->from = earliest;
    return wp->end > wp->from ? wp->end - wp->from : 0;
}
