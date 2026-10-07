// SPDX-License-Identifier: Apache-2.0
//
// Adoption policy for a two-pass recogniser.  See pass2.c for why the policy
// is what it is; this is the interface.

#ifndef PASS2_H
#define PASS2_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int  adopt;      // 1 = use the second-pass text, 0 = keep the first
    char reason[160]; // why, for the log; never NULL
} Pass2Decision;

// Compare the first-pass line with the second-pass result and decide whether
// the second should replace it.  Reasons are reported in d.reason.
Pass2Decision pass2_decide(const char *first, const char *second);

// Write the text that should be displayed for this decision: the second-pass
// text when adopted, otherwise the first.  Returns the length written.
int pass2_render(char *out, size_t out_sz, const Pass2Decision *d,
                 const char *first, const char *second);

// Work out which audio the second pass should decode.
//
// fed is the total number of samples handed to the streaming recogniser and
// ring_base is the absolute index of the oldest sample the ring still holds.
// The second pass gets the whole utterance, from the oldest surviving sample
// to the newest, because an offline model handed only the most recent block
// sees a fragment rather than a sentence.
//
// Returns 1 and sets *from and *to when there is audio worth decoding, 0 when
// there is not: nothing fed yet, or the ring has already scrolled past
// everything.  The caller must not hand the offline model an empty buffer --
// it answers a hallucination rather than an error.
//
// When the utterance is longer than the ring, *from is ring_base and the lost
// part is simply not decoded; the caller can compare fed - *from against what
// it expected if it wants to report that.
int pass2_audio_span(uint64_t fed, uint64_t ring_base,
                     uint64_t *from, uint64_t *to);

// Return the index of the first sample louder than threshold, so the caller can
// start the second pass there.  Returns 0 when the window already begins with
// speech, and n when the whole window is quiet.
//
// This exists because of a measured failure, not a precaution.  WSChuan
// paraformer attends to the beginning of its input, so a window opening with
// silence suppresses what follows: 20 s of the same audio produced 36 characters
// when the window started at 0, where the first 8 s were silent, and 96
// characters when it started at 5 s.  An endpoint fires on trailing silence, so
// the audio handed to the second pass routinely opens with a pause, and without
// this trim the second pass returns a fraction of what the first pass said and
// is rejected every time.
//
// threshold is amplitude, 0 to 1.  The window is not modified; only the returned
// index is used.
int pass2_trim_lead(const float *samples, int n, float threshold);

#endif  // PASS2_H
