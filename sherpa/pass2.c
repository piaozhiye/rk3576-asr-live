// SPDX-License-Identifier: Apache-2.0
//
// Adoption policy for a two-pass recogniser.
//
// A streaming model gives a provisional line as soon as speech is recognised.
// A second, non-streaming model re-decodes the same audio and may produce a
// better line.  Whether to adopt that second result is the whole content of
// this file, and the policy is set by measurement on this board rather than
// by copying an example.
//
// What the two models actually produce from the same 100 s Chinese lecture
// (audio captured through the tap, both models given identical input):
//
//                          streaming (CPU)          non-streaming (NPU)
//   暂稳态                   暂稳态                   暂稳态
//   三要素                   三要素                   三要素
//   稳态时无穷                稳态微无穷   (wrong)      稳态微无穷   (wrong)
//   过渡                     过渡       (right)       过度         (wrong)
//   终止时间                  终止时间   (right)       终旨时间      (wrong)
//   segmentation             one 100 s blob            8 clean lines
//
// So the second pass is better at one thing and worse at another.  It breaks
// sentences correctly, which is the whole reason to run it, and it corrupts
// individual characters, which is why blindly replacing the text would be a
// regression on exactly the words that were already right.
//
// sherpa-onnx's own two-pass examples do replace unconditionally in the
// teaching version, and guard only against an empty result in the product
// server (two-pass-wss.py).  Those examples pair a streaming zipformer with a
// non-streaming paraformer, a combination the project documents as the second
// pass being the more accurate one.  The pairing here is different -- two
// independently trained paraformer models that were never trained together --
// and the table above is what that pairing does.  Hence two rules, not one:
//
//   1. Never adopt an empty result.  Same as the product server.
//   2. Never adopt a result with fewer characters than the first.  A second
//      result that lost text is the documented failure mode of a non-streaming
//      attention decoder on long audio, and character count detects it without
//      a reference transcript.
//
// Everything else is adopted.  Equal or longer length means nothing was lost,
// and the character-level differences are then a matter of opinion that this
// code has no way to adjudicate -- refusing to adopt on any difference would
// reduce the second pass to a segmenter that can never correct a word.

#include "pass2.h"

#include <stdio.h>
#include <string.h>

// Characters worth displaying, not bytes.  A han character is three bytes, so
// a byte count would call a five-character line shorter than a two-character
// one.  Space is excluded too, which makes "  \t  " count as zero rather than
// four and keeps a model that spaces its output differently from looking
// shorter than one that does not.  Control characters go with it: the
// streaming model emits CR and TAB as separators, so a result made of those is
// empty as far as a reader is concerned.
//
// UTF-8 continuation bytes are skipped rather than counted: they only ever
// occur inside a character, so counting them separately would double-count
// every han character, and skipping them is what makes this a character count.
static size_t visible_len(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if ((*p & 0xC0) == 0x80) continue;   // interior of a character
        if (*p == ' ') continue;             // spacing is not content
        if (*p < 0x20 || *p == 0x7F) continue;  // separator, not content
        ++n;
    }
    return n;
}

// Is `short` the beginning of `long`, comparing content characters only?  The
// comparison has to be by character and must skip separators, because the two
// models space their output differently: "rc" against "r c" is the same start.
static int is_leading_prefix(const char *shortstr, const char *longstr) {
    if (!shortstr || !longstr) return 0;
    while (*shortstr && *longstr) {
        while (*shortstr == ' ' || (*(const unsigned char *)shortstr < 0x20 &&
                                    *(const unsigned char *)shortstr != 0x1b))
            ++shortstr;
        while (*longstr == ' ' || (*(const unsigned char *)longstr < 0x20 &&
                                   *(const unsigned char *)longstr != 0x1b))
            ++longstr;
        if (!*shortstr) return 1;              // short ran out: it is a prefix
        if (!*longstr) return 0;              // long ran out first: not a prefix
        if (*shortstr != *longstr) return 0;
        ++shortstr;
        ++longstr;
    }
    // Trailing separators on the short side still count as a prefix.
    while (*shortstr == ' ') ++shortstr;
    return *shortstr == '\0';
}

Pass2Decision pass2_decide(const char *first, const char *second) {
    Pass2Decision d;
    d.adopt = 0;
    snprintf(d.reason, sizeof(d.reason), "%s", "kept first pass");

    const size_t a = visible_len(first);
    const size_t b = visible_len(second);

    if (b == 0) {
        snprintf(d.reason, sizeof(d.reason),
                 "second pass returned nothing (first has %zu chars)", a);
        return d;
    }

    // A second result that is shorter is not automatically one that lost text.
    //
    // Measured cause, and it is not a property of the model: WSChuan paraformer
    // attends to the beginning of its input, so leading silence suppresses what
    // follows.  Given a 20 s window it returned 36 characters when the window
    // started with 8 s of silence, and 96 characters from the same 20 s with the
    // silence removed -- identical audio, identical model, a 2.7x difference.
    // The first pass has no such problem because its context grows with the
    // utterance.
    //
    // So a second result that is a leading substring of the first has very
    // likely been truncated by the window rather than by the decoder, and the
    // first pass's text is the better of the two.  A second result that is
    // shorter but NOT a leading substring really did drop something in the
    // middle, which is the case that has to be refused.
    if (b < a) {
        if (is_leading_prefix(second, first)) {
            snprintf(d.reason, sizeof(d.reason),
                     "second pass is a leading prefix (%zu of %zu chars), "
                     "the window was cut short, not the decoder", b, a);
            return d;
        }
        // Close enough to adopt.  With the window trimmed to what the offline
        // model can actually read, what is left is usually a boundary word and
        // the length moves by one or two characters; the truncated case that
        // motivated this rule lost 20-35%, nowhere near here.  A fifth is
        // deliberately generous and is a judgement about this model pair, not
        // a general truth about second passes.
        if (b * 5 >= a * 4) {
            d.adopt = 1;
            snprintf(d.reason, sizeof(d.reason),
                     "adopted second pass (%zu vs %zu chars, within a fifth)", b, a);
            return d;
        }
        snprintf(d.reason, sizeof(d.reason),
                 "second pass is much shorter and not a prefix (%zu vs %zu chars), "
                 "so it lost text", b, a);
        return d;
    }

    d.adopt = 1;
    snprintf(d.reason, sizeof(d.reason),
             "adopted second pass (%zu chars vs %zu)", b, a);
    return d;
}

int pass2_render(char *out, size_t out_sz, const Pass2Decision *d,
                 const char *first, const char *second) {
    if (out_sz == 0) return 0;
    const char *src = (d && d->adopt && second && *second) ? second : first;
    if (!src) { out[0] = '\0'; return 0; }
    size_t n = strlen(src);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    return (int)n;
}

int pass2_audio_span(uint64_t fed, uint64_t ring_base,
                     uint64_t *from, uint64_t *to) {
    if (fed == 0 || ring_base >= fed) return 0;   // nothing worth decoding
    *from = ring_base;
    *to = fed;
    return *to > *from;
}

int pass2_trim_lead(const float *samples, int n, float threshold) {
    if (!samples || n <= 0) return 0;
    for (int i = 0; i < n; ++i) {
        const float v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > threshold) return i;
    }
    return n;   // all quiet: the caller is told, rather than handed nothing
}
