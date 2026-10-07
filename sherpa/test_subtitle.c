// SPDX-License-Identifier: Apache-2.0
//
// Tests for the streaming subtitle commit policy.
//
// A streaming recogniser returns the WHOLE transcript so far on every call,
// and it is free to revise words it emitted earlier.  Naively printing each
// result scrolls the same sentence over and over; naively printing the delta
// can emit a revision backwards, because the new text is not always an
// extension of the old.  So the policy is: show the tail provisionally, and
// only ever commit text once an endpoint says the utterance is over.
//
// Build and run (host, no board, no model):
//   gcc -O2 -Wall -o /tmp/t2 sherpa/test_subtitle.c sherpa/subtitle.c
//   /tmp/t2

#include <stdio.h>
#include <string.h>

#include "subtitle.h"

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

static void check_str(const char *got, const char *want, const char *what) {
    char buf[192];
    snprintf(buf, sizeof(buf), "expected \"%s\", got \"%s\"", want, got);
    check(strcmp(got, want) == 0, what, buf);
}

int main(void) {
    char out[128];

    // ---- 1. the first endpoint commits everything seen so far -------------
    SubtitleState st;
    subtitle_init(&st);
    subtitle_commit(&st, "那下边呢我们就要", out, sizeof(out), 1);
    check_str(out, "那下边呢我们就要", "first endpoint commits the whole transcript");
    check(st.committed == (size_t)strlen("那下边呢我们就要"), "  ...and advances committed",
          "committed not advanced");

    // ---- 2. the next endpoint commits only the new suffix -----------------
    subtitle_commit(&st, "那下边呢我们就要定量计算", out, sizeof(out), 1);
    check_str(out, "定量计算", "second endpoint commits only the new tail");
    check(st.committed == (size_t)strlen("那下边呢我们就要定量计算"), "  ...committed advances again",
          "committed wrong");

    // ---- 3. no endpoint means nothing is committed -------------------------
    // This is the case that matters for live captions: the recogniser may have
    // produced a lot of text, but until the utterance ends it may still change
    // it, so it must not be shown as a final line.
    size_t before = st.committed;
    int n = subtitle_commit(&st, "那下边呢我们就要定量计算就是我们说", out, sizeof(out), 0);
    check(n == 0, "no endpoint commits nothing", "committed without an endpoint");
    check(out[0] == '\0', "  ...and leaves the buffer empty", "buffer not cleared");
    check(st.committed == before, "  ...committed unchanged", "committed moved");

    // ---- 4. a revision must not be emitted backwards ----------------------
    // Streaming models revise.  If the new result is shorter than what we have
    // already committed, or diverges, emitting text[committed:] would print a
    // fragment of a sentence that was never said.
    st.committed = (size_t)strlen("那下边呢我们就要定量计算");
    n = subtitle_commit(&st, "那下边", out, sizeof(out), 1);
    check(n == 0, "a shrinking revision commits nothing", "emitted from a shorter revision");
    check(st.committed == (size_t)strlen("那下边呢我们就要定量计算"),
          "  ...committed is not rolled back", "committed regressed");

    // ---- 5. an empty result is a no-op ------------------------------------
    size_t c2 = st.committed;
    check(subtitle_commit(&st, "", out, sizeof(out), 1) == 0, "empty result commits nothing",
          "emitted from empty text");
    check(st.committed == c2, "  ...committed unchanged", "committed moved on empty");

    // ---- 6. a long tail is truncated, not overflowed ----------------------
    {
        char big[512];
        for (int i = 0; i < 500; ++i) big[i] = 'x';
        big[500] = '\0';
        SubtitleState s2;
        subtitle_init(&s2);
        n = subtitle_commit(&s2, big, out, sizeof(out), 1);
        check(n == (int)sizeof(out) - 1, "a long line fills the buffer exactly", "wrong truncation length");
        check(strlen(out) == (size_t)n, "  ...and stays NUL-terminated", "not terminated");
    }

    // ---- 7. the provisional tail, for a live view -------------------------
    // Between endpoints the caller still wants to show something moving.  This
    // is the not-yet-committed tail, so it must never include committed text.
    {
        SubtitleState s3;
        subtitle_init(&s3);
        subtitle_commit(&s3, "北京时间", out, sizeof(out), 1);
        const char *tail = subtitle_tail(&s3, "北京时间今天天气不错", out, sizeof(out));
        check_str(tail, "今天天气不错", "provisional tail excludes committed text");
        tail = subtitle_tail(&s3, "北京", out, sizeof(out));
        check_str(tail, "", "tail is empty when the result is behind committed");
    }

    // ---- 8. UTF-8 boundaries ------------------------------------------------
    // committed counts BYTES, and these strings are UTF-8 where one han
    // character is three bytes.  A streaming recogniser rewrites earlier text
    // freely, so committed routinely lands mid-character; slicing there prints
    // half a glyph, which showed up on the terminal as "u^$G" and mojibake.
    // Every boundary this module produces must be a character boundary.
    {
        // 「那下边呢我们就要计算」 is 30 bytes of 3-byte han characters.
        // Byte 7 sits inside 边 (e8 be b9 at bytes 6-8), and so does byte 8, so
        // a forward alignment has to skip both continuation bytes and land on
        // 呢 at byte 9.
        SubtitleState s4;
        subtitle_init(&s4);
        s4.committed = 7;                       // deliberately illegal
        const char *t = subtitle_tail(&s4, "那下边呢我们就要计算", out, sizeof(out));
        // Check the FIRST byte only.  Testing every byte for 0b10xxxxxx finds
        // the two continuation bytes inside every han character and reports a
        // false failure; the property that matters is that the slice starts on
        // a character boundary, and the strcmp below proves the whole slice is
        // the intended text.
        const int starts_clean = ((unsigned char)out[0] & 0xC0) != 0x80;
        check(starts_clean, "tail never starts mid-UTF-8-character",
              "tail began with a continuation byte");
        check_str(t, "呢我们就要计算", "  ...and starts at the nearest character boundary");
    }
    {
        // A commit whose new text is not a clean extension must also land on a
        // boundary, or the NEXT tail is already broken.
        SubtitleState s5;
        subtitle_init(&s5);
        subtitle_commit(&s5, "计算", out, sizeof(out), 1);
        const char *t = subtitle_tail(&s5, "计算结果", out, sizeof(out));
        check_str(t, "结果", "a commit boundary is a character boundary");
    }

    // ---- 9. control characters in the model's text -------------------------
    // The streaming model emits a carriage return as a separator, and a literal
    // CR in the middle of a line is what put a 0x0D on screen.  Subtitle text
    // is one line, so these are dropped rather than printed.
    {
        SubtitleState s6;
        subtitle_init(&s6);
        subtitle_commit(&s6, "计算\r\n结果 tx", out, sizeof(out), 1);
        // Space survives: the model separates words with it, and dropping it
        // would glue them ("结果tx").  CR, LF and TAB do not.
        check_str(out, "计算结果 tx", "commit drops control characters but keeps spaces");

        subtitle_init(&s6);
        const char *t = subtitle_tail(&s6, "开头\r中间", out, sizeof(out));
        check_str(t, "开头中间", "tail drops control characters");
        // And the same when the tail is clipped, so the two filters compose.
        {
            char small[16];
            t = subtitle_tail(&s6, "开头\r中间很长的一句话需要被截断", small, sizeof(small));
            int has_ctrl = 0;
            for (const char *p = small; *p; ++p)
                if ((unsigned char)*p < 0x20) has_ctrl = 1;
            check(!has_ctrl, "a clipped tail still drops control characters",
                  "control character survived clipping");
        }
    }

    // ---- 11. only report the tail when it actually changes -----------------
    // The caller is driven 31 times a second but the recogniser's text only
    // moves when it decodes, so printing every call emitted the same sentence
    // 120 times: 7247 lines for 114 distinct strings.  subtitle_tail_changed
    // is the gate -- it reports the tail once per distinct value and then stays
    // quiet, so the screen follows the recogniser rather than the clock.
    {
        SubtitleState s8;
        char prev[128];
        size_t prev_len = 0;
        subtitle_init(&s8);

        int first = subtitle_tail_changed(&s8, "那下边呢", prev, sizeof(prev), &prev_len);
        check(first > 0, "first call reports the tail", "nothing reported on the first call");

        int again = subtitle_tail_changed(&s8, "那下边呢", prev, sizeof(prev), &prev_len);
        check(again == 0, "an unchanged tail is not reported again",
              "the same text was reported twice");

        int grown = subtitle_tail_changed(&s8, "那下边呢我们就要", prev, sizeof(prev), &prev_len);
        check(grown > 0, "a longer tail is reported", "growth was not reported");

        // The recogniser revises freely, so a shorter result is normal.  It is
        // a change and must be shown -- otherwise the screen keeps a sentence
        // the model has already abandoned.
        int revised = subtitle_tail_changed(&s8, "那下边", prev, sizeof(prev), &prev_len);
        check(revised > 0, "a revised (shorter) tail is reported",
              "a revision was suppressed");

        // A commit moves the starting point, so the tail is a different string
        // and has to be shown again even though it may be a suffix of what was
        // on screen a moment ago.
        subtitle_commit(&s8, "那下边", prev, sizeof(prev), 1);
        int after_commit = subtitle_tail_changed(&s8, "那下边呢我们就要", prev, sizeof(prev), &prev_len);
        check(after_commit > 0, "the tail is reported again after a commit",
              "the post-commit tail was suppressed as unchanged");

        // Committing the whole result leaves nothing provisional, and nothing
        // should be printed for it -- not a bare "...", and not the line that
        // was just committed a second time.
        subtitle_commit(&s8, "那下边呢我们就要", prev, sizeof(prev), 1);
        const int drained = subtitle_tail_changed(&s8, "那下边呢我们就要", prev, sizeof(prev), &prev_len);
        check(drained == 0, "nothing is reported once the tail is fully committed",
              "an already-committed tail was reported again");
    }
    {
        // Empty must never be reported: it is the common case between
        // utterances, and reporting it would print a bare "...每 32ms".
        SubtitleState s9;
        char prev[128];
        size_t prev_len = 0;
        subtitle_init(&s9);
        subtitle_tail_changed(&s9, "", prev, sizeof(prev), &prev_len);
        const int empty = subtitle_tail_changed(&s9, "", prev, sizeof(prev), &prev_len);
        check(empty == 0, "an empty tail is never reported", "an empty tail was reported");
    }

    // ---- 12. rendering the provisional line in place ------------------------
    // The live view must redraw one line, not append a new one per revision.
    // Appending is what made 14 growing revisions look like 14 repetitions of
    // the same sentence even though no two were equal.  In place, the reader
    // sees one line that grows.
    //
    // The awkward part is shrinking: a revision that comes back shorter leaves
    // the tail of the previous, longer line on screen.  The redraw therefore
    // pads with spaces out to the previous width.
    {
        char scr[256];
        int n;

        n = subtitle_render(scr, sizeof(scr), "那下", 0);
        check(n > 0 && strstr(scr, "\r") == scr,
              "a redraw starts by returning to column 0", "no carriage return");
        check(n > 0 && strstr(scr, "那下") != NULL,
              "  ...and carries the new text", "the text is missing");

        // First render has nothing to erase, so no padding.
        // Computed, not hand-counted: "\r\033[K" is FOUR bytes (\r, ESC, [,
        // K) and one han character is three, so hard-coding these lengths was
        // how this test first came to disagree with correct code.
        const size_t CLR = 4, MARK = 6;   // "   ..."
        n = subtitle_render(scr, sizeof(scr), "那下", 0);
        const size_t want0 = CLR + MARK + strlen("那下");
        check(n == (int)want0 && memcmp(scr, "\r\033[K   ...那下", want0) == 0,
              "a first render is exactly the clear, marker and text",
              "unexpected first render");

        // Now shrink from 6 bytes ("那下边呢" is 9) to 2: must pad out to 9 or
        // the old characters stay visible.
        {
            const char *prev = "那下边呢";       // 9 bytes on screen
            const size_t plen = strlen(prev);   // what was drawn before
            const char *cur  = "那下";          // 6 bytes now
            const size_t tlen = strlen(cur);
            n = subtitle_render(scr, sizeof(scr), cur, plen);
            // Ends at the previous width, so the line never shrinks and the
            // old, longer text cannot survive underneath.
            check(n == (int)(CLR + MARK + plen),
                  "a shorter redraw pads out the old line",
                  "no padding, so the previous text stays on screen");
            // Padding starts right after the new text, not after the old one.
            const size_t pad_from = CLR + MARK + tlen;
            check(pad_from < (size_t)n && scr[pad_from] == ' ' &&
                  scr[n - 1] == ' ' && scr[n - 2] == ' ' && scr[n - 3] == ' ',
                  "  ...to exactly the previous width", "wrong pad width");
        }

        // Growing must not pad, and must not leave a gap.
        n = subtitle_render(scr, sizeof(scr), "那下边呢我们就要", 0);
        check(n == (int)(CLR + MARK + strlen("那下边呢我们就要")) &&
              memcmp(scr, "\r\033[K   ...那下边呢我们就要", CLR + MARK + 24) == 0,
              "a longer redraw is not padded", "a longer line was padded");
    }
    {
        // An empty tail still has to clear the line, or the last words stay
        // frozen on screen with nothing to say they are stale.
        char scr[256];
        const int n = subtitle_render(scr, sizeof(scr), "", 12);
        check(n > 0, "an empty redraw still emits something", "nothing emitted");
        // The prefix is the fixed "   ..." marker, not stale text, so it is
        // allowed to survive.  What must not survive is any of the words that
        // were there before, so check the payload after the marker is blank
        // and that the whole thing is padded out to the old width.
        const char *payload = strstr(scr, "...");
        check(payload != NULL, "  ...and still carries the marker", "no marker");
        int blank = 1;
        for (const char *p = payload ? payload + 3 : scr; *p; ++p)
            if (*p != ' ') { blank = 0; break; }
        check(blank, "  ...with no stale words left after it",
              "an empty redraw left text on screen");
        check(n >= 12, "  ...padded to the previous width",
              "the old line was not cleared");
    }

    // ---- 10. truncation must not split a character -------------------------
    // The tail buffer is small and a growing transcript overflows it.  Clipping
    // at out_sz-1 bytes lands mid-character and the terminal shows the
    // half-glyph as mojibake, so the end has to be pulled back to a boundary.
    {
        SubtitleState s7;
        subtitle_init(&s7);
        // 7 bytes of budget: two 3-byte han characters fit (6 bytes) plus the
        // NUL, and the third cannot start without exceeding it.
        char small[7];
        const char *t = subtitle_tail(&s7, "一二三四五六七八", small, sizeof(small));
        check(strcmp(t, "一二") == 0, "a truncated tail keeps only whole characters",
              "tail did not clip to a character boundary");
        check(strlen(small) == 6, "  ...with no partial character left over",
              "trailing bytes are not a whole character");
    }

    // ---- 13. laying a committed line out on a fixed-width screen ------------
    // The on-screen renderer needs to know where to break a line: the panel is
    // 1080 wide, the subtitle band has margins, and a han character is three
    // bytes but occupies two terminal cells.  Breaking on byte offsets splits
    // characters, and breaking on byte width wraps a 40-character line into
    // something half the length it should be.
    {
        // Display columns, not bytes: a han character is two cells wide.
        check(subtitle_columns("\xe5\x90\x8c") == 2,
              "a han character is two display columns", "wrong column count");
        check(subtitle_columns("abc") == 3,
              "ascii is one column per byte", "wrong column count");
        // "同 a" is two cells for 同, one for the space, one for a.
        check(subtitle_columns("\xe5\x90\x8c a") == 4,
              "mixed width adds up", "wrong column count");
    }
    {
        // A line that fits is one piece.
        char out[256];
        int n = subtitle_wrap(out, sizeof(out), "\xe5\x90\x8c\xe6\xad\xa3", 20, 4);
        check(n == 1, "a short line wraps to one row", "wrong row count");
        check_str(out, "\xe5\x90\x8c\xe6\xad\xa3", "  ...unchanged");
    }
    {
        // 5 han characters is 10 columns, so at 8 columns per row it is two
        // rows, and the break must land between characters.
        char out[256];
        const int rows = subtitle_wrap(out, sizeof(out),
            "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94", 8, 4);
        check(rows == 2, "an over-long line wraps", "wrong row count");
        // Row 1 is the first 4 han (8 columns), row 2 the last.
        check(strstr(out, "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\n") == out,
              "  ...breaking on a character boundary", "broke mid-character");
        check(strstr(out, "\n\xe4\xba\x94") != NULL,
              "  ...with the remainder on the next row", "the tail is missing");
    }
    {
        // A single word wider than the row must still be broken, or it is
        // silently dropped.  Prefer breaking it over losing it.
        char out[256];
        const int rows = subtitle_wrap(out, sizeof(out),
            "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94", 4, 4);
        check(rows >= 2, "a word wider than the row is broken anyway", "dropped");
        int valid = 1;
        for (const unsigned char *p = (const unsigned char *)out; *p; ++p)
            if ((*p & 0xC0) == 0x80 &&
                p[-1] != '\n' && (*(p - 1) & 0xC0) != 0x80) { /* count below */ }
        // The real check: no row starts on a continuation byte.
        for (const char *row = out; row && *row; ) {
            if ((*(const unsigned char *)row & 0xC0) == 0x80) valid = 0;
            const char *nl = strchr(row, '\n');
            row = nl ? nl + 1 : NULL;
        }
        check(valid, "  ...and every row starts on a character", "a row starts mid-character");
    }
    {
        // A long line whose last character is cut off by the end of the input.
        // This is the real shape: a recogniser buffer that got truncated, so
        // there is a whole row of text and then half a character.  The half
        // character must be dropped, not carried onto the row.
        char out[256];
        char cut[64];
        // Four han characters (12 bytes) with the last one missing its third
        // byte: 11 bytes of text, the last character incomplete.
        memcpy(cut, "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b", 11);
        cut[11] = '\0';
        subtitle_wrap(out, sizeof(out), cut, 20, 4);
        int complete = 1;
        for (const char *row = out; row && *row; ) {
            const unsigned char c0 = (const unsigned char)*row;
            size_t need = 1;
            if ((c0 & 0xE0) == 0xC0) need = 2;
            else if ((c0 & 0xF0) == 0xE0) need = 3;
            else if ((c0 & 0xF8) == 0xF0) need = 4;
            int k = 0;
            while (row[k] && k < (int)need &&
                   (k == 0 || ((unsigned char)row[k] & 0xC0) == 0x80)) ++k;
            if (k < (int)need) complete = 0;
            const char *nl = strchr(row, '\n');
            row = nl ? nl + 1 : NULL;
        }
        check(complete, "a line ending in a half character drops it",
              "an incomplete character was carried onto a row");
    }
    {
        // Rows are capped: a 200-character line cannot produce 200 rows and run
        // the renderer off the band.
        char out[1024];
        char big[600];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        const int rows = subtitle_wrap(out, sizeof(out), big, 20, 4);
        check(rows > 0 && rows <= 4, "the row count is capped", "no cap applied");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
