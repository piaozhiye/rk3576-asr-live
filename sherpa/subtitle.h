// SPDX-License-Identifier: Apache-2.0
//
// Subtitle commit policy for a streaming recogniser.
//
// A streaming recogniser returns the entire transcript so far on every decode,
// and it is allowed to revise words it produced earlier.  Two naive policies
// both fail:
//
//   * print every result  -> the same sentence scrolls over and over
//   * print text[committed:] -> prints revisions backwards, because a new
//     result is not always an extension of the old one; if the recogniser
//     shortens its output, that slice is the middle of a sentence nobody said
//
// So: text is only ever committed at an endpoint, when the utterance is over
// and the recogniser has stopped changing its mind.  Between endpoints the
// caller can display the uncommitted tail, which is what makes a live view look
// live without pretending to a finality it does not have.

#ifndef SUBTITLE_H
#define SUBTITLE_H

#include <stddef.h>

typedef struct {
    size_t committed;   // bytes of transcript already emitted as final lines
} SubtitleState;

void subtitle_init(SubtitleState *st);

// Emit the text produced since the last commit as one finished line, and only
// when the recogniser says the utterance ended (is_endpoint).  Returns the
// length written to out, NUL-terminated and truncated to out_sz, or 0 if
// nothing was committed -- including when the new result is shorter than what
// is already committed, which means the recogniser revised backwards and there
// is nothing safe to print.
int subtitle_commit(SubtitleState *st, const char *text,
                    char *out, size_t out_sz, int is_endpoint);

// The part of text that has not been committed yet, for a provisional display.
// Returns out (NUL-terminated, truncated) so the caller can print it directly.
const char *subtitle_tail(const SubtitleState *st, const char *text,
                          char *out, size_t out_sz);

// As subtitle_tail, but only when the tail differs from the one last reported,
// and never for an empty tail.  Returns the length written to out, or 0 when
// there is nothing new to show -- so the caller can print unconditionally and
// let this decide.
//
// The caller is driven at the capture rate, about 31 times a second, but the
// recogniser's text only moves when it decodes.  Printing every call put one
// sentence on screen 120 times over: 7247 lines for 114 distinct strings.
// *prev_len is the caller's scratch for the last reported length, and must be
// zeroed before the first call.  out holds the previous value on entry and is
// overwritten with the new one.
int subtitle_tail_changed(SubtitleState *st, const char *text,
                          char *out, size_t out_sz, size_t *prev_len);

// Build the byte string that redraws the provisional line in place, so a
// growing transcript is one line updating rather than a new line per revision.
// Returns the length written to out, always NUL-terminated.
//
// out receives "\r\033[K" (return to column 0, erase to end of line) followed
// by the text.  The erase is what makes a shrinking revision work: without it
// the tail of the previous, longer line would stay on screen.
//
// prev_len is the length last drawn, and the line is padded with spaces out to
// it when the new text is shorter.  Pass 0 for the first draw.  An empty text
// still emits the clear, so a finished utterance does not leave stale words
// frozen on screen.
int subtitle_render(char *out, size_t out_sz, const char *text, size_t prev_len);

// Display columns text occupies on a terminal-style screen.  UTF-8 continuation
// bytes are skipped, so a han character counts once; its width is taken as two
// cells, which is what a fixed-width font uses for CJK.
size_t subtitle_columns(const char *s);

// Break text into rows of at most max_cols display columns, at most max_rows
// rows, writing them into out separated by '\n'.  Breaks land on character
// boundaries.  A run of characters wider than max_cols is still broken rather
// than dropped.  Returns the number of rows written, or 0 if nothing fit.
int subtitle_wrap(char *out, size_t out_sz, const char *text,
                  size_t max_cols, int max_rows);

#endif  // SUBTITLE_H
