// SPDX-License-Identifier: Apache-2.0
//
// Subtitle commit policy for a streaming recogniser.  See subtitle.h for why
// text is only committed at an endpoint.
//
// Two things about the raw text need handling before it can be printed, and
// both were found on the device rather than by reading the code:
//
//   * UTF-8.  One han character is three bytes, and a streaming recogniser
//     rewrites earlier words freely, so a byte offset that was a character
//     start last tick can be mid-character this tick.  Slicing there hands the
//     terminal half a glyph.  Offsets are aligned forward, and clipping to the
//     output buffer is aligned too, so no boundary is ever cut.
//
//   * Control characters.  The model emits CR and TAB as separators.  A literal
//     0x0D in the middle of a subtitle line is invisible in the source and
//     shows up on screen as a stray control sequence.  A subtitle is one line,
//     so these are dropped.

#include "subtitle.h"

#include <stdio.h>
#include <string.h>

void subtitle_init(SubtitleState *st) {
    st->committed = 0;
}

// A byte offset is only usable as "the recogniser has confirmed this much" if
// the new result is at least that long.  Streaming models revise, and a shorter
// result means the earlier text no longer exists -- slicing from committed
// would then print the tail of a sentence that was never spoken.
static int confirmable(const char *text, size_t committed) {
    size_t len = strlen(text);
    return len >= committed;
}

// Pull an offset forward to the next UTF-8 character boundary.  Forward only:
// rewinding would re-print text the caller has already committed.
static size_t utf8_align(const char *text, size_t off, size_t len) {
    while (off < len && ((unsigned char)text[off] & 0xC0) == 0x80)
        ++off;
    return off;
}

// A control character that must not reach the terminal.  UTF-8 continuation
// bytes are NOT filtered here: they are the interior of a character, and
// dropping them turns a han character into three separate garbage bytes.  The
// input is already aligned (utf8_align), so whole characters are copied and
// only real controls are skipped.
// C0 control characters and DEL.  Space (0x20) is deliberately not included:
// the model separates words with it, and dropping it would glue them together.
static int droppable(unsigned char c) {
    return c < 0x20 || c == 0x7F;
}

// Copy text[from, to) into out, dropping control characters and never ending on
// a partial UTF-8 character.  Returns the length written.
static size_t emit(const char *text, size_t from, size_t to,
                   char *out, size_t out_sz) {
    if (out_sz == 0) return 0;
    size_t n = 0;
    size_t i = from;
    while (i < to) {
        // Decode one character so the whole thing is copied or not at all.
        // A half-written character is the mojibake case, and it is not
        // detectable by looking at the last byte: e4 b8 80 followed by a bare e4
        // ends on a lead byte, not a continuation byte, yet it is still broken.
        const unsigned char c0 = (unsigned char)text[i];
        size_t clen = 1;
        if ((c0 & 0xE0) == 0xC0) clen = 2;
        else if ((c0 & 0xF0) == 0xE0) clen = 3;
        else if ((c0 & 0xF8) == 0xF0) clen = 4;
        if (i + clen > to) break;                  // truncated source
        if (n + clen + 1 > out_sz) break;         // no room for it plus NUL
        int ok = 1;
        for (size_t k = 1; k < clen; ++k)
            if (((unsigned char)text[i + k] & 0xC0) != 0x80) ok = 0;
        if (!ok) { i += clen; continue; }          // malformed: drop the char
        for (size_t k = 0; k < clen; ++k)
            if (droppable((unsigned char)text[i + k])) ok = 0;
        if (ok) {
            memcpy(out + n, text + i, clen);
            n += clen;
        }
        i += clen;
    }
    out[n] = '\0';
    return n;
}

int subtitle_commit(SubtitleState *st, const char *text,
                    char *out, size_t out_sz, int is_endpoint) {
    if (out_sz == 0) return 0;
    out[0] = '\0';
    if (!is_endpoint) return 0;
    if (!text || !confirmable(text, st->committed)) return 0;

    size_t len = strlen(text);
    size_t start = utf8_align(text, st->committed, len);
    if (start >= len) return 0;

    size_t n = emit(text, start, len, out, out_sz);
    if (n == 0) return 0;

    // Advance only over text actually emitted, so a line clipped by the buffer
    // is re-emitted after the next endpoint rather than lost.
    size_t end = start;
    for (size_t i = start; i < len; ++i) {
        if (droppable((unsigned char)text[i])) continue;
        if (++end > start + n) break;
    }
    st->committed = utf8_align(text, end, len);
    return (int)n;
}

int subtitle_render(char *out, size_t out_sz, const char *text, size_t prev_len) {
    if (out_sz == 0) return 0;
    // "\r\033[K" is three bytes.  Kept as a literal so the escape cannot be
    // mangled by an editor that eats control characters.
    static const char CLR[] = "\r\033[K";
    int n = snprintf(out, out_sz, "%s   ...%s", CLR, text ? text : "");
    if (n < 0) { out[0] = '\0'; return 0; }
    size_t len = (size_t)n;
    if (len >= out_sz) len = out_sz - 1;      // snprintf reported what it wanted

    // Pad out to what was on screen, or a shorter revision leaves the old text
    // behind.  The two lengths have to be measured over the same span: len
    // includes the clear sequence and the "   ..." marker, while prev_len is
    // the text alone, so the text is len minus the prefix.  Comparing the raw
    // values made a 6-byte line look longer than the 9-byte line it replaced
    // and skipped the padding entirely.
    const size_t prefix = (size_t)(sizeof(CLR) - 1) + 6;   // clear + "   ..."
    const size_t tlen = len > prefix ? len - prefix : 0;
    const size_t want = len + (prev_len > tlen ? prev_len - tlen : 0);
    while (len < want && len + 1 < out_sz) out[len++] = ' ';
    out[len] = '\0';
    return (int)len;
}

int subtitle_tail_changed(SubtitleState *st, const char *text,
                          char *out, size_t out_sz, size_t *prev_len) {
    if (out_sz == 0) return 0;
    // Build the candidate beside the previous value rather than over it: the
    // comparison needs both, and out is where the last one lives.
    char cur[1024];
    const size_t cap = out_sz < sizeof(cur) ? out_sz : sizeof(cur);
    // *out is the caller's scratch, zeroed before the first call.  An empty
    // tail needs no special case: it matches the initial state, so the
    // comparison below suppresses it.  (A separate n == 0 guard was tried and
    // removed -- it was unreachable, since memcmp over zero bytes always
    // compares equal, so the check could only ever be redundant.)
    cur[0] = '\0';
    subtitle_tail(st, text, cur, cap);
    const size_t n = strlen(cur);
    if (*prev_len == n && memcmp(out, cur, n) == 0) return 0;
    memcpy(out, cur, n + 1);
    *prev_len = n;
    return (int)n;
}

const char *subtitle_tail(const SubtitleState *st, const char *text,
                          char *out, size_t out_sz) {
    if (out_sz == 0) return "";
    out[0] = '\0';
    if (!text || !confirmable(text, st->committed)) return out;
    size_t len = strlen(text);
    size_t start = utf8_align(text, st->committed, len);
    if (start >= len) return out;
    emit(text, start, len, out, out_sz);
    return out;
}

// Width of one UTF-8 character in terminal cells.  Continuation bytes and
// invalid lead bytes are one cell each so a malformed string still terminates.
static size_t char_columns(unsigned char c) {
    if (c < 0x80) return 1;                  // ASCII
    if ((c & 0xE0) == 0xC0) return 1;        // 2-byte sequence: treat as 1
    if ((c & 0xF0) == 0xE0) return 2;        // CJK, the common case here
    if ((c & 0xF8) == 0xF0) return 2;        // 4-byte sequence: treat as 2
    return 1;
}

size_t subtitle_columns(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if ((*p & 0xC0) == 0x80) continue;   // inside a character
        n += char_columns(*p);
    }
    return n;
}

int subtitle_wrap(char *out, size_t out_sz, const char *text,
                  size_t max_cols, int max_rows) {
    if (out_sz == 0 || !text || !*text) return 0;
    if (max_cols == 0) max_cols = 1;
    if (max_rows < 1) max_rows = 1;

    size_t n = 0;
    int rows = 1;
    size_t cols = 0;                 // columns on the current row
    const char *p = text;

    while (*p) {
        // Measure one whole character, never a slice of one: breaking between a
        // lead byte and its continuation is what printed half a glyph.
        const unsigned char c0 = (unsigned char)*p;
        size_t clen = 1;
        if ((c0 & 0xE0) == 0xC0) clen = 2;
        else if ((c0 & 0xF0) == 0xE0) clen = 3;
        else if ((c0 & 0xF8) == 0xF0) clen = 4;
        // Verify the sequence really is that long.  Without this a truncated
        // character at the end of the input is copied whole, so the row ends on
        // a lead byte with its continuations missing -- the same shape as the
        // half-glyph this module exists to prevent, arriving from the other
        // direction.
        for (size_t k = 1; k < clen; ++k) {
            if (p[k] == '\0') { clen = k; break; }
            if (((unsigned char)p[k] & 0xC0) != 0x80) { clen = 1; break; }
        }
        const size_t cw = char_columns(c0);

        // Break before this character when it will not fit.  A character wider
        // than the whole row is still placed, on a row of its own, because
        // dropping it loses text and this is a subtitle.
        if (cols + cw > max_cols && cols > 0) {
            if (rows >= max_rows) break;        // the band is full
            if (n + 1 >= out_sz) break;
            out[n++] = '\n';
            ++rows;
            cols = 0;
        }
        if (n + clen >= out_sz) break;
        memcpy(out + n, p, clen);
        n += clen;
        cols += cw;
        p += clen;
    }
    out[n] = '\0';
    return n > 0 ? rows : 0;
}
