// tiny TUI theme: the TokyoNight `night` palette and the SGR writer.
//
// Every colour the TUI writes comes from the active palette, foreground and
// background alike, so no row falls back to the host terminal's own colours.
// Truecolor only.

#ifndef TINY_TUI_THEME_H
#define TINY_TUI_THEME_H

#include <stdbool.h>

#include "util.h"
#include "config.h"

typedef struct {
    const char *bg;
    const char *fg;
    const char *fg_dark;
    const char *comment;
    const char *terminal_black;
    const char *blue;
    const char *blue1;
    const char *blue5;
    const char *green;
    const char *green1;
    const char *magenta;
    const char *orange;
    const char *purple;
    const char *red;
    const char *teal;
    const char *yellow;
} tiny_palette;

// A capture style: a foreground, a background, and the text attributes.
typedef struct {
    const char *fg;
    const char *bg;
    bool bold;
    bool italic;
    bool underline;
} tiny_style;

// Selects the active palette. Must be called before any drawing.
void tiny_theme_use(tiny_theme theme);

const char *tiny_normal_fg(void);
const char *tiny_normal_bg(void);
const char *tiny_diff_add(void);
const char *tiny_diff_delete(void);

// One `38;2;r;g;b` / `48;2;r;g;b`, and attributes off then `Normal`.
void sgr_fg(tui_buf *b, const char *hex);
void sgr_bg(tui_buf *b, const char *hex);
void sgr_plain(tui_buf *b);

// A style's own attributes with the defaults spelled out, so a span that names
// no colour never inherits the one before it.
void sgr_style(tui_buf *b, const tiny_style *s);

// Inline fg styling: writes `sgrFg(colour)`, text, `sgrFg(Normal)`, so the row's
// background survives.
typedef void (*tiny_fg_fn)(tui_buf *b, const char *text);
void fg_dim(tui_buf *b, const char *text);
void fg_red(tui_buf *b, const char *text);
void fg_green(tui_buf *b, const char *text);
void fg_yellow(tui_buf *b, const char *text);
void fg_cyan(tui_buf *b, const char *text);
void fg_blue(tui_buf *b, const char *text);
void fg_teal(tui_buf *b, const char *text);

// Capture name to colour, mapped the way tokyonight.nvim maps captures. A
// dotted name falls back to its parent; NULL when nothing matches.
const tiny_style *tiny_capture_style(const char *capture);

// A heading level (1-based, clamped to 8) with its rainbow tint, and inline code.
const tiny_style *tiny_heading_style(int level);
const tiny_style *tiny_inline_code_style(void);

#endif
