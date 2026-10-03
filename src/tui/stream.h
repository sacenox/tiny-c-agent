// tiny TUI stream renderers: assistant text and reasoning, projected onto lines.
//
// MarkdownStream holds a fenced code block until its close, and a run of prose
// until its block ends, then colors the whole block. TailStream keeps only the
// incomplete tail, dimmed.

#ifndef TINY_TUI_STREAM_H
#define TINY_TUI_STREAM_H

#include <stddef.h>

#include "theme.h"

typedef struct {
    char *text;            // malloc'd, may carry SGR
    tiny_fg_fn style;      // NULL when unstyled
    const char *bg;        // row background, or NULL for Normal
} tiny_body_line;

void tiny_body_lines_free(tiny_body_line *lines, size_t count);

// A growable list of body lines, used while assembling a block.
typedef struct {
    tiny_body_line *v;
    size_t n;
    size_t cap;
} tiny_blist;

void bl_push(tiny_blist *l, char *text, tiny_fg_fn style, const char *bg);

// Hands the list's array to the caller (NULL when empty) and resets the count.
tiny_body_line *bl_take(tiny_blist *l, size_t *count);

typedef struct {
    char *rest;
    char *fence_marker;
    char *fence_info;
    char **fence_lines;
    size_t fence_n;
    size_t fence_cap;
    char **prose;
    size_t prose_n;
    size_t prose_cap;
} tiny_mdstream;

void tiny_mdstream_init(tiny_mdstream *s);
void tiny_mdstream_free(tiny_mdstream *s);
// feed/pending/flush return a heap array the caller frees with
// tiny_body_lines_free; *count receives its length.
tiny_body_line *tiny_mdstream_feed(tiny_mdstream *s, const char *delta, size_t *count);
tiny_body_line *tiny_mdstream_pending(tiny_mdstream *s, size_t *count);
tiny_body_line *tiny_mdstream_flush(tiny_mdstream *s, size_t *count);
void tiny_mdstream_reset(tiny_mdstream *s);

typedef struct {
    char *rest;
} tiny_tailstream;

void tiny_tailstream_init(tiny_tailstream *s);
void tiny_tailstream_free(tiny_tailstream *s);
tiny_body_line *tiny_tailstream_feed(tiny_tailstream *s, const char *delta, size_t *count);
tiny_body_line *tiny_tailstream_pending(tiny_tailstream *s, size_t *count);
void tiny_tailstream_reset(tiny_tailstream *s);

#endif
