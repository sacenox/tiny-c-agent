// cage TUI stream renderers: assistant text and reasoning, projected onto lines.
//
// MarkdownStream holds a fenced code block until its close, and a run of prose
// until its block ends, then colors the whole block. TailStream keeps only the
// incomplete tail, dimmed.

#ifndef CAGE_TUI_STREAM_H
#define CAGE_TUI_STREAM_H

#include <stddef.h>

#include "theme.h"

typedef struct {
    char *text;            // malloc'd, may carry SGR
    cage_fg_fn style;      // NULL when unstyled
    const char *bg;        // row background, or NULL for Normal
} cage_body_line;

void cage_body_lines_free(cage_body_line *lines, size_t count);

// A growable list of body lines, used while assembling a block.
typedef struct {
    cage_body_line *v;
    size_t n;
    size_t cap;
} cage_blist;

void bl_push(cage_blist *l, char *text, cage_fg_fn style, const char *bg);

// Hands the list's array to the caller (NULL when empty) and resets the count.
cage_body_line *bl_take(cage_blist *l, size_t *count);

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
} cage_mdstream;

void cage_mdstream_init(cage_mdstream *s);
void cage_mdstream_free(cage_mdstream *s);
// feed/pending/flush return a heap array the caller frees with
// cage_body_lines_free; *count receives its length.
cage_body_line *cage_mdstream_feed(cage_mdstream *s, const char *delta, size_t *count);
cage_body_line *cage_mdstream_pending(cage_mdstream *s, size_t *count);
cage_body_line *cage_mdstream_flush(cage_mdstream *s, size_t *count);
void cage_mdstream_reset(cage_mdstream *s);

typedef struct {
    char *rest;
} cage_tailstream;

void cage_tailstream_init(cage_tailstream *s);
void cage_tailstream_free(cage_tailstream *s);
cage_body_line *cage_tailstream_feed(cage_tailstream *s, const char *delta, size_t *count);
cage_body_line *cage_tailstream_pending(cage_tailstream *s, size_t *count);
void cage_tailstream_reset(cage_tailstream *s);

#endif
