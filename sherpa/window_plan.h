// SPDX-License-Identifier: Apache-2.0
//
// Window planning, factored out of the sherpa_live capture loop so the
// segment-length policy can be tested on a host with no board, no NPU and no
// ALSA device.  The function is pure: it maps one VAD segment to the span of
// ring samples to decode, and says whether the segment had to be cut short.

#ifndef WINDOW_PLAN_H
#define WINDOW_PLAN_H

#include <stdint.h>

// One decode window, as half-open ring-absolute sample indices [from, end).
typedef struct {
    uint64_t from;  // inclusive
    uint64_t end;   // exclusive
    // 1 when the segment was longer than max_window and had to be cut.  The
    // caller must then clear the VAD so the remainder is re-detected as a new
    // segment instead of growing without bound.
    int split;
} WindowPlan;

// Plan the decode window for a single detected segment.
//
//   run_start  ring-absolute sample index where the CURRENT SPEECH RUN began,
//              tracked by the caller in our own coordinates.  Deliberately
//              NOT seg->start: that is in the VAD's internal popped-buffer
//              coordinates, and slicing the ring against it grabs audio from
//              the wrong stream positions.
//   seg_len    segment length in samples
//   head       headroom prepended to a segment that fits
//   tail       trailing silence trimmed from a segment that fits
//   ring_base  oldest sample still resident in the ring
//   total_fed  total samples fed to the VAD so far
//   max_window hard ceiling on the returned span, in samples
//   out        receives the plan; only written when the return value is > 0
//
// Returns the number of samples to decode, or 0 if the segment yields nothing
// decodable (entirely older than the ring).  The sub-0.3 s blip filter stays
// with the caller, which has always owned it.
//
// Every returned span is <= max_window: a longer segment is cut at
// max_window and flagged, never silently handed to a model window too small
// to hold it.  After a cut, the caller advances its run pointer to
// out->end and resets the VAD, so the audio past the cut is re-detected as a
// fresh segment rather than dropped.
uint64_t window_plan(uint64_t run_start, uint64_t seg_len,
                     uint64_t head, uint64_t tail,
                     uint64_t ring_base, uint64_t total_fed,
                     uint64_t max_window,
                     WindowPlan *out);

// Latency-cap window, the second emission trigger.
//
// window_plan() can only fire once the VAD hands over a segment, so however
// tightly the window size is capped, latency stays proportional to how long
// that segment takes to close.  Measured on a narration clip: 5.4 s segment
// gave 6.8 s of latency, 18.0 s gave 24.8 s.  This trigger ignores the VAD
// entirely and fires on elapsed audio, so latency is bounded by cap + decode
// time whatever the content is.
//
//   last_emit  stream index where the previous window ended
//   total_fed  newest stream index
//   cap        slice length; also the hard ceiling on the returned span
//   head       leading context pulled in from before last_emit
//   ring_base  oldest sample still resident in the ring
//   out        receives the plan; only written when the return value is > 0
//
// Returns the number of samples to decode, or 0 when the cap has not elapsed
// and the caller should stay quiet.
uint64_t plan_latency_window(uint64_t last_emit, uint64_t total_fed,
                             uint64_t cap, uint64_t head, uint64_t ring_base,
                             WindowPlan *out);

// Keep a VAD-triggered window from re-transcribing audio the latency cap has
// already emitted.
//
// The two triggers overlap by design -- the cap guarantees a latency bound, the
// VAD provides natural sentence boundaries -- but a segment that lands after
// the cap has already passed covers audio that was transcribed some slices ago.
// Handing it to the decoder verbatim re-emits whole paragraphs.  So a VAD
// window may reach back at most `head` before `run_start`, and never behind
// the ring; a window entirely at or before `run_start` is dropped.
//
//   wp         window to clamp, modified in place
//   run_start  stream index up to which audio has already been emitted
//   head       context allowance before run_start
//   ring_base  oldest sample still resident in the ring
//
// Returns the resulting sample count, or 0 if the window is now empty.
uint64_t clamp_window_start(WindowPlan *wp, uint64_t run_start,
                            uint64_t head, uint64_t ring_base);

#endif  // WINDOW_PLAN_H
