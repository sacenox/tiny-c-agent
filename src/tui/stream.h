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

// An owned, growable list of owned body lines. Free with bl_free.
typedef struct {
    tiny_body_line *v;
    size_t n;
    size_t cap;
} tiny_blist;

void bl_push(tiny_blist *l, char *text, tiny_fg_fn style, const char *bg);
void bl_free(tiny_blist *l);

typedef struct {
    char *rest;
    char *fence_marker;
    char *fence_info;
    tiny_strv fence_lines;
    tiny_strv prose;
} tiny_mdstream;

void tiny_mdstream_init(tiny_mdstream *s);
void tiny_mdstream_free(tiny_mdstream *s);
// feed/pending/flush return a list the caller frees with bl_free.
tiny_blist tiny_mdstream_feed(tiny_mdstream *s, const char *delta);
tiny_blist tiny_mdstream_pending(tiny_mdstream *s);
tiny_blist tiny_mdstream_flush(tiny_mdstream *s);
void tiny_mdstream_reset(tiny_mdstream *s);

typedef struct {
    char *rest;
} tiny_tailstream;

void tiny_tailstream_init(tiny_tailstream *s);
void tiny_tailstream_free(tiny_tailstream *s);
tiny_blist tiny_tailstream_feed(tiny_tailstream *s, const char *delta);
tiny_blist tiny_tailstream_pending(tiny_tailstream *s);
void tiny_tailstream_reset(tiny_tailstream *s);

#endif
