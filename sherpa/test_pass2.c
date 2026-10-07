// SPDX-License-Identifier: Apache-2.0
//
// Tests for the two-pass adoption policy.
//
// A streaming recogniser gives a provisional line; a second, non-streaming
// recogniser re-decodes the same audio and may produce a better one.  What to
// do with the second result is not obvious, and the answer here is measured
// rather than assumed -- see the notes in pass2.c for the transcripts both
// models produce from the same 100 s lecture.
//
// The three rules this file pins down:
//
//   1. An empty second result never replaces anything.  sherpa-onnx's own
//      two-pass product server (two-pass-wss.py) guards this; the teaching
//      example (two-pass-speech-recognition-from-microphone.py) does not.
//   2. A second result that lost text never replaces anything.  A shorter
//      second result means the non-streaming model dropped words, which is the
//      documented failure mode of attention decoders on long audio.  Measuring
//      by character count catches it without needing a reference transcript.
//   3. Otherwise the second result wins, including when it merely re-breaks the
//      same words into different lines.  That is the whole point of the second
//      pass: the streaming model's segment boundaries are the thing being
//      fixed, and a second result that keeps the same characters is evidence
//      that the improvement is in the segmentation, not in the words.
//
// Build and run (host, no board, no model):
//   gcc -O2 -Wall -o /tmp/tp2 sherpa/test_pass2.c sherpa/pass2.c
//   /tmp/tp2

#include <stdio.h>
#include <string.h>

#include "pass2.h"

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

static void check_adopt(Pass2Decision d, int want, const char *what) {
    char buf[256];
    snprintf(buf, sizeof(buf), "expected adopt=%d, got adopt=%d (%s)",
             want, d.adopt, d.reason);
    check(d.adopt == want, what, buf);
}

int main(void) {
    // ---- 1. an empty second result is never adopted -------------------------
    // Not a hypothetical: a non-streaming model fed a short or silent window
    // returns nothing, and adopting that would delete a line the first pass had
    // already got right.
    {
        Pass2Decision d = pass2_decide("那下边呢我们就要定量计算", "");
        check_adopt(d, 0, "an empty second result is not adopted");
        check(strstr(d.reason, "nothing") != NULL,
              "  ...and says why", d.reason);
    }
    {
        // Whitespace only is the same thing: nothing was actually decoded.
        Pass2Decision d = pass2_decide("那下边呢", "  \t ");
        check_adopt(d, 0, "a blank second result is not adopted");
    }
    {
        // So is a result made only of characters the stripper removes.  The
        // recogniser emits CR and TAB as separators, and a line of those is
        // empty as far as a reader is concerned.
        Pass2Decision d = pass2_decide("那下边呢", "\r\n\t");
        check_adopt(d, 0, "a control-only second result is not adopted");
    }

    // ---- 2. a shorter second result is never adopted -----------------------
    // The measured case: on a 100 s lecture the non-streaming model returned
    // "的过渡过程环节" for a window where the streaming model had "这个过渡过程
    // 环节这个程程们看谁对它进行充电充电的起始点都有谁来帮助界定".  Adopting that
    // would have deleted most of the line.
    {
        Pass2Decision d = pass2_decide(
            "这个过渡过程环节这个程程们看谁对它进行充电充电的起始点都有谁来帮助界定",
            "的过渡过程环节");
        check_adopt(d, 0, "a much shorter second result is not adopted");
        check(strstr(d.reason, "shorter") != NULL,
              "  ...and says why", d.reason);
    }
    {
        // Marginally shorter, which is what a real off-by-one looks like
        // rather than the catastrophic case.  Still rejected: a second result
        // that is even slightly behind has lost something, and there is no way
        // to know whether what it lost was noise or a word.
        Pass2Decision d = pass2_decide("那下边呢我们就要定量计算", "那下边呢我们就要定量");
        check_adopt(d, 0, "a slightly shorter second result is not adopted");
    }

    // ---- 3. longer or equal, and the second pass wins -----------------------
    {
        // The real case, at the granularity the display works in.  The first
        // pass committed a whole clause with no break in it; the second pass
        // re-decoded the same audio and put the break earlier.  Same words, a
        // genuinely different line, and that is the improvement the second pass
        // exists to provide.
        const char *first = "那下边呢我们就要定量计算";
        Pass2Decision d = pass2_decide(first, first);
        check_adopt(d, 1, "an identically-worded but re-broken line is adopted");

        char got[512];
        const int n = pass2_render(got, sizeof(got), &d, first, "那下边呢我们就要");
        check(n > 0 && strcmp(got, "那下边呢我们就要") == 0,
              "  ...and the adopted text is the second result's",
              "the wrong text was kept");
    }
    {
        // The other half of render, and the half that a mutation of its
        // decision check slips past: when the decision rejects, the FIRST text
        // must come back.  Rendering the second regardless would be a silent
        // inversion -- the policy would compute correctly and then be ignored.
        const char *first = "这个过渡过程环节这个程程们看谁对它进行充电充电的起始点都有谁来帮助界定";
        const char *second = "的过渡过程环节";
        Pass2Decision d = pass2_decide(first, second);
        char got[512];
        const int n = pass2_render(got, sizeof(got), &d, first, second);
        check(n == (int)strlen(first) && strcmp(got, first) == 0,
              "a rejected decision renders the first pass's text",
              "the second pass was rendered despite rejection");
    }
    {
        // And a NULL second result must not be dereferenced when a decision
        // somehow says adopt.
        char got[64];
        Pass2Decision d;
        d.adopt = 1;
        const int n = pass2_render(got, sizeof(got), &d, "安全的原文", NULL);
        check(n > 0 && strcmp(got, "安全的原文") == 0,
              "a NULL second result falls back to the first",
              "rendered nothing or crashed");
    }
    {
        // And the same comparison across a longer stretch: a first pass that
        // ran two sentences together against a second pass that split them.
        // Character counts differ because the split drops the run-on, so this
        // is NOT adopted -- which is the rule biting, and is correct: a second
        // result that genuinely lost a word is not a re-segmentation.
        Pass2Decision d = pass2_decide(
            "那下边呢我们就要定量计算定量计算就是我们说要准确的计算出来它这个站稳态的输出脉宽",
            "那下边呢我们就要定量计算");
        check_adopt(d, 0,
                    "a second result missing a whole clause is not adopted");
    }
    {
        // Equal length, different characters: the second pass still wins.  This
        // is the case the policy is really about -- on the same lecture the
        // non-streaming model produced 稳态微无穷 and the streaming model
        // produced 稳态微无穷 too, but it also corrected 终旨 to 终止 in some
        // windows.  There is no way to tell those apart without a reference
        // transcript, and refusing to adopt would make the second pass useless.
        Pass2Decision d = pass2_decide("稳态微无穷想知道起点", "稳态微无穷想知道起点");
        check_adopt(d, 1, "an identical second result is adopted");
    }
    {
        // Longer second result, so nothing can have been lost.
        Pass2Decision d = pass2_decide("的过渡过程环节", "这个过渡过程环节这个程程们看谁");
        check_adopt(d, 1, "a longer second result is adopted");
    }

    // ---- 4. counting ignores separators -------------------------------------
    // A second result with different spacing must not look shorter, or a model
    // that spaces its output differently would be rejected every time.
    {
        const char *a = "那下边呢我们就要定量计算";   // 11 characters
        const char *b = "那 下边呢 我们就 要定 量 计算";  // 11 characters, 4 spaces
        Pass2Decision d = pass2_decide(a, b);
        check_adopt(d, 1, "spacing differences do not count as shrinkage");
    }

    // ---- 5. the count is characters, not bytes ------------------------------
    // These are the tests a byte count fails.  Both compare strings of equal
    // character length whose byte lengths differ, so counting bytes gives the
    // wrong answer in both directions.
    {
        // 6 han characters = 18 bytes, against 2 han = 6 bytes.  A byte count
        // would call the second one LONGER and adopt a result that lost
        // two thirds of the line.
        Pass2Decision d = pass2_decide("计算电路打开得到", "计算电路");
        check_adopt(d, 0, "characters, not bytes: 6 han vs 2 han");

        // The other direction, which needs the first string to be the longer
        // one in characters but not in bytes.  5 han = 15 bytes against
        // 2 han + 2 latin = 10 bytes: a byte count would call the second
        // LONGER and adopt it, while by character it has lost three.
        d = pass2_decide("计算电路打开得到", "计算电路 r");
        check_adopt(d, 0, "characters, not bytes: 5 han vs 2 han + latin");
    }
    {
        // Straightforward losses, at a granularity a reader would notice.
        Pass2Decision d = pass2_decide("计算电路打开", "计算电路打");
        check_adopt(d, 0, "a one-character loss is still a loss");

        d = pass2_decide("计算电路打开", "计算电路");
        check_adopt(d, 0, "a two-character loss is still a loss");
    }
    {
        // Latin text counts the same way.  "rc" and "r c" are both two
        // characters -- the space is not content -- so they are equal, not
        // short, and the rule must not mistake spacing for a lost word.
        Pass2Decision d = pass2_decide("rc 电路", "r c 电路");
        check_adopt(d, 1, "resplitting rc as r c is spacing, not a loss");

        // A genuinely dropped letter is still a loss, though: 3 characters
        // against 4.
        d = pass2_decide("rc 电路", "r 电路");
        check_adopt(d, 0, "a dropped latin letter is a loss");
    }
    {
        // The case that separates the three counting rules at once.  Same
        // characters, one extra space: 12 bytes against 13, but 4 characters
        // against 4.
        //
        //   counting bytes      -> 13 > 12, longer, adopted (wrong reason)
        //   counting all chars  -> 5 > 4,  longer, adopted (wrong reason)
        //   counting content     -> equal, adopted (right reason)
        //
        // All three say adopt, so the decision alone cannot tell them apart --
        // which is why the reason string is what the next check looks at.  A
        // count that did not skip the space would report "5 chars vs 4" and
        // claim the second pass gained a character it does not have.
        Pass2Decision d = pass2_decide("计算电路", "计算 电路");
        check_adopt(d, 1, "a space does not make the second pass longer");
        check(strstr(d.reason, "4 chars vs 4") != NULL,
              "  ...and the reported counts exclude it", d.reason);
    }
    {
        // Same shape with a control character, which the streaming model emits
        // as a separator.  "a\rb" is 4 bytes and 4 characters, but only 2 of
        // them are content.
        Pass2Decision d = pass2_decide("计算电路", "计算\r电路");
        check_adopt(d, 1, "a CR separator does not make the second pass longer");
        check(strstr(d.reason, "4 chars vs 4") != NULL,
              "  ...and is not counted as content", d.reason);
    }

    // ---- 5b. a leading substring is a truncated window, not a lost word ----
    // The measured case that changes the policy.  WSChuan paraformer attends to
    // the start of its input, so a window that opens with silence suppresses
    // what follows: 20 s of audio gave 36 characters with 8 s of leading
    // silence and 96 characters with that silence removed.  The first pass has
    // no such problem, because its context grows with the utterance.  So a
    // second result that is exactly the beginning of the first was cut short by
    // the window, and the first pass already said all of it.
    {
        const char *first = "那下边呢我们就要定量计算定量计算就是我们说要准确的计算出来它这个暂稳态的输出脉宽";
        Pass2Decision d = pass2_decide(first, "那下边呢我们就要定量计算");
        check_adopt(d, 0, "a leading substring is not adopted");
        check(strstr(d.reason, "prefix") != NULL, "  ...reported as a prefix",
              d.reason);
    }
    {
        // Spacing differs between the two models, so the prefix comparison has
        // to ignore it.  "rc" against "r c" is the same beginning.
        Pass2Decision d = pass2_decide("rc 电路打开计算", "r c 电路");
        check_adopt(d, 0, "a prefix survives different spacing");
    }
    {
        // Shortened but NOT a prefix: something was dropped from the middle,
        // which is a different failure and still has to be refused.
        Pass2Decision d = pass2_decide("那下边呢我们就要定量计算", "那下边呢电计算");
        check_adopt(d, 0, "a shorter result that diverges is not a prefix");
        check(strstr(d.reason, "much shorter") != NULL,
              "  ...and is reported as a real loss", d.reason);
    }

    // ---- 5c. a near miss is still a second pass worth taking ---------------
    // With the window inside what the offline model can read, the common
    // remaining case is a boundary word.  Measured: the first pass said
    // "充电的过程环节这个过程我们看谁对它进行充电充电的起始点都有谁来帮助界定"
    // and the second "就过程环节这个过程我们看谁对它进行充电充电起起始都有谁来帮助界定"
    // -- 33 against 36 characters, differing at the leading boundary word and by
    // one doubled character, with the body identical.
    //
    // That is not a truncated window, which loses a third of the text.  Refusing
    // it throws away the second pass exactly where it is working, so a result
    // that is nearly the same length is adopted even if shorter.
    {
        Pass2Decision d = pass2_decide(
            "充电的过程环节这个过程我们看谁对它进行充电充电的起始点都有谁来帮助界定",
            "就过程环节这个过程我们看谁对它进行充电充电起起始都有谁来帮助界定");
        check_adopt(d, 1, "a near miss in length is still adopted");

        // How near is near?  A fifth is the line, and it is a judgement about
        // this model pair rather than a general truth: the truncated case that
        // motivated the rule lost 20-35%, far outside it.
        // Not a prefix: the leading character differs, so this exercises the
        // length tolerance rather than the prefix rule.
        Pass2Decision ok = pass2_decide(
            "甲一二三四五六七八九十乙丙丁戊己庚辛壬癸子丑寅卯辰",
            "乙一二三四五六七八九十乙丙丁戊己庚辛壬癸子丑");
        check_adopt(ok, 1, "a 3-character loss is within a fifth and adopted");
        check(strstr(ok.reason, "within a fifth") != NULL,
              "  ...and says it was a close call", ok.reason);

        Pass2Decision too_far = pass2_decide(
            "一二三四五六七八九十甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳",
            "一二三四五六七八九十甲乙");
        check_adopt(too_far, 0, "a 20-character loss on 28 is still refused");
    }

    // ---- 6. choosing which audio the second pass sees -----------------------
    // The second pass must be given the whole utterance, not the most recent
    // block.  These two functions decide that slice, and the decisions are easy
    // to get wrong in ways that look fine: feeding the whole ring re-decodes
    // audio the streaming pass already emitted, and feeding only the newest
    // block gives the offline model a fragment instead of a sentence.
    {
        // A 16 s utterance sitting in a 20 s ring, arriving as three blocks.
        // The second pass is given the utterance, starting at its first sample
        // and ending at the last one fed.
        const uint64_t fed = 16000 * 16;
        const uint64_t ring_base = 16000 * 2;   // 2 s of older audio scrolled off
        uint64_t from = 0, to = 0;
        const int ok = pass2_audio_span(fed, ring_base, &from, &to);
        check(ok, "a span is found for a 16 s utterance in a 20 s ring",
              "no span returned");
        check(from == ring_base, "  ...starting at what the ring still holds",
              "span started inside the utterance");
        check(to == fed, "  ...and ending at the last sample fed",
              "span did not end at the current position");
        check((to - from) == 16000 * 14,
              "  ...clipped to the ring, not reading past it",
              "the span claimed audio the ring has already discarded");
    }
    {
        // The utterance is longer than the ring: everything the ring holds is
        // used, and the loss is reported so it can be logged rather than
        // silently decoding a fragment.
        const uint64_t fed = 16000 * 30;
        const uint64_t ring_base = 16000 * 15;  // only 15 s left
        uint64_t from = 0, to = 0;
        const int ok = pass2_audio_span(fed, ring_base, &from, &to);
        check(ok, "a span is returned even when the utterance overfills the ring",
              "no span returned");
        check(from == ring_base && to == fed,
              "  ...covering exactly what the ring still holds",
              "the span is not the surviving part of the utterance");
    }
    {
        // Nothing to send: no audio has been fed, or the ring has scrolled past
        // everything.  The caller must not hand the offline model an empty
        // buffer, which it answers with a hallucination rather than an error.
        uint64_t from = 1, to = 1;
        check(pass2_audio_span(0, 0, &from, &to) == 0,
              "no audio fed means no span", "a span was invented from nothing");
        from = to = 0;
        check(pass2_audio_span(1000, 2000, &from, &to) == 0,
              "a ring that has scrolled past the audio means no span",
              "a span was returned for audio the ring no longer holds");
    }

    // ---- 7. trimming the leading silence off the second pass's input -------
    // The measured root cause of the whole second pass looking useless.
    // WSChuan paraformer attends to the start of its input, so a window that
    // opens with silence suppresses what follows: 20 s of the same audio gave
    // 36 characters when the window started at 0 (8 s of leading silence) and
    // 96 characters when it started at 5 s.  The first pass does not have this
    // problem because its context grows with the utterance.
    //
    // An endpoint fires on trailing silence, so by construction the audio a
    // second pass is handed often opens with whatever preceded the speech --
    // a pause, a breath, the tail of the previous line.  Dropping the quiet
    // prefix is therefore the fix, and it has to be a measured threshold
    // rather than a fixed number of samples.
    {
        static float q[64];
        // 0.02 is about -34 dBFS per sample.  Speech in this material runs
        // around -28 dBFS and the gap between sentences is well under that, so
        // the split is not delicate.
        // An all-quiet window returns n, not 0: the caller has to be able to
        // tell "no silence to trim" from "there is no speech here at all", and
        // sending the second model pure silence gets a hallucination back.
        const int n1 = pass2_trim_lead(q, 8, 0.02f);
        check(n1 == 8, "an all-quiet window reports that it has no speech",
              "reported as if it began with speech");

        static float sp[8] = {0.0f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f};
        const int n2 = pass2_trim_lead(sp, 8, 0.02f);
        check(n2 == 3, "the quiet prefix before speech is dropped", "wrong offset");

        static float all[8] = {0.4f, 0.3f, 0.2f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f};
        const int n3 = pass2_trim_lead(all, 8, 0.02f);
        check(n3 == 0, "a window that starts loud is left alone", "trimmed anyway");
    }
    {
        // A window that is quiet at the start but loud later must keep the
        // speech, and the result has to be a usable index rather than a copy.
        static float buf[16] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 0.0f};
        const int n = pass2_trim_lead(buf, 16, 0.02f);
        check(n == 5, "the offset points at the first loud sample", "wrong offset");
        check(buf[n] == 0.5f, "  ...and that sample is the loud one",
              "the offset does not point at the speech");
    }
    {
        // Nothing to trim must not shift the pointer, or every window would be
        // reported as starting one sample in.
        static float loud[4] = {0.4f, 0.4f, 0.4f, 0.4f};
        check(pass2_trim_lead(loud, 4, 0.02f) == 0,
              "a loud window keeps offset 0", "offset moved on a loud window");
        // A zero-length window is a caller bug, but must not read anything.
        check(pass2_trim_lead(loud, 0, 0.02f) == 0,
              "a zero-length window is not read", "offset moved on empty input");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
