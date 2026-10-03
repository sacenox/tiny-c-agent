#include "theme.h"

#include <stdio.h>
#include <string.h>

// The TokyoNight `night` slots the TUI uses, from folke/tokyonight.nvim.
static const tiny_palette TOKYO_NIGHT = {
    .bg = "#1a1b26",
    .fg = "#c0caf5",
    .fg_dark = "#a9b1d6",
    .comment = "#565f89",
    .terminal_black = "#414868",
    .blue = "#7aa2f7",
    .blue1 = "#2ac3de",
    .blue5 = "#89ddff",
    .green = "#9ece6a",
    .green1 = "#73daca",
    .magenta = "#bb9af7",
    .orange = "#ff9e64",
    .purple = "#9d7cd8",
    .red = "#f7768e",
    .teal = "#1abc9c",
    .yellow = "#e0af68",
};

static const char DIFF_ADD[] = "#243e4a";
static const char DIFF_DELETE[] = "#4a272f";

static const tiny_palette *g_pal = &TOKYO_NIGHT;

void tiny_theme_use(tiny_theme theme) {
    // `auto` will derive the palette from the host terminal; until then it
    // shares the TokyoNight palette so no call site branches on the theme.
    (void)theme;
    g_pal = &TOKYO_NIGHT;
}

const char *tiny_normal_fg(void) { return g_pal->fg; }
const char *tiny_normal_bg(void) { return g_pal->bg; }
const char *tiny_diff_add(void) { return DIFF_ADD; }
const char *tiny_diff_delete(void) { return DIFF_DELETE; }

// `r;g;b` for one `#rrggbb`.
static void channels(tui_buf *b, const char *hex) {
    unsigned r = 0, g = 0, bl = 0;
    if (hex && hex[0] == '#' && strlen(hex) >= 7) {
        sscanf(hex + 1, "%2x%2x%2x", &r, &g, &bl);
    }
    buf_addf(b, "%u;%u;%u", r, g, bl);
}

void sgr_fg(tui_buf *b, const char *hex) {
    buf_adds(b, "\x1b[38;2;");
    channels(b, hex);
    buf_addc(b, 'm');
}

void sgr_bg(tui_buf *b, const char *hex) {
    buf_adds(b, "\x1b[48;2;");
    channels(b, hex);
    buf_addc(b, 'm');
}

void sgr_plain(tui_buf *b) {
    buf_adds(b, "\x1b[22;23;24;38;2;");
    channels(b, tiny_normal_fg());
    buf_adds(b, ";48;2;");
    channels(b, tiny_normal_bg());
    buf_addc(b, 'm');
}

void sgr_style(tui_buf *b, const tiny_style *s) {
    buf_adds(b, "\x1b[");
    if (s->bold) buf_adds(b, "1;");
    if (s->italic) buf_adds(b, "3;");
    if (s->underline) buf_adds(b, "4;");
    buf_adds(b, "38;2;");
    channels(b, s->fg ? s->fg : tiny_normal_fg());
    buf_adds(b, ";48;2;");
    channels(b, s->bg ? s->bg : tiny_normal_bg());
    buf_addc(b, 'm');
}

static void fg(tui_buf *b, const char *hex, const char *text) {
    sgr_fg(b, hex);
    buf_adds(b, text);
    sgr_fg(b, tiny_normal_fg());
}

void fg_dim(tui_buf *b, const char *text) { fg(b, g_pal->comment, text); }
void fg_red(tui_buf *b, const char *text) { fg(b, g_pal->red, text); }
void fg_green(tui_buf *b, const char *text) { fg(b, g_pal->green, text); }
void fg_yellow(tui_buf *b, const char *text) { fg(b, g_pal->yellow, text); }
void fg_cyan(tui_buf *b, const char *text) { fg(b, g_pal->blue, text); }
void fg_blue(tui_buf *b, const char *text) { fg(b, g_pal->blue, text); }
void fg_teal(tui_buf *b, const char *text) { fg(b, g_pal->teal, text); }

// One entry per capture name; `fg`/`bg` reference the palette by tag so the
// table survives a palette swap at `tiny_theme_use` time.
typedef struct {
    const char *name;
    tiny_style style;
} style_entry;

#define FG(tag) tag
static const style_entry STYLES[] = {
    {"comment", {FG("#565f89"), NULL, false, false, false}},
    {"constant", {FG("#ff9e64"), NULL, false, false, false}},
    {"constant.builtin", {FG("#2ac3de"), NULL, false, false, false}},
    {"constructor", {FG("#bb9af7"), NULL, false, false, false}},
    {"escape", {FG("#bb9af7"), NULL, false, false, false}},
    {"function", {FG("#7aa2f7"), NULL, false, false, false}},
    {"function.builtin", {FG("#2ac3de"), NULL, false, false, false}},
    {"keyword", {FG("#9d7cd8"), NULL, false, false, false}},
    {"number", {FG("#ff9e64"), NULL, false, false, false}},
    {"operator", {FG("#89ddff"), NULL, false, false, false}},
    {"property", {FG("#73daca"), NULL, false, false, false}},
    {"punctuation.bracket", {FG("#a9b1d6"), NULL, false, false, false}},
    {"punctuation.delimiter", {FG("#89ddff"), NULL, false, false, false}},
    {"punctuation.special", {FG("#89ddff"), NULL, false, false, false}},
    {"string", {FG("#9ece6a"), NULL, false, false, false}},
    {"string.special", {FG("#2ac3de"), NULL, false, false, false}},
    {"type", {FG("#2ac3de"), NULL, false, false, false}},
    {"type.builtin", {FG("#27a1b9"), NULL, false, false, false}},
    {"variable", {FG("#c0caf5"), NULL, false, false, false}},
    {"variable.builtin", {FG("#f7768e"), NULL, false, false, false}},
    {"variable.parameter", {FG("#e0af68"), NULL, false, false, false}},
    {"text.emphasis", {NULL, NULL, false, true, false}},
    {"text.strong", {NULL, NULL, true, false, false}},
    {"text.literal", {FG("#9ece6a"), NULL, false, false, false}},
    {"text.uri", {NULL, NULL, false, false, true}},
    {"text.reference", {FG("#2ac3de"), NULL, false, false, false}},
    // The `@markup` names used by nvim's current markdown queries.
    {"markup.heading", {NULL, NULL, true, false, false}},
    {"markup.strong", {NULL, NULL, true, false, false}},
    {"markup.emphasis", {NULL, NULL, false, true, false}},
    {"markup.raw", {FG("#9ece6a"), NULL, false, false, false}},
    {"markup.link", {FG("#2ac3de"), NULL, false, false, false}},
    {"markup.link.url", {NULL, NULL, false, false, true}},
    {"markup.list", {FG("#e0af68"), NULL, false, false, false}},
    {"markup.quote", {FG("#565f89"), NULL, false, false, false}},
};
#undef FG

const tiny_style *tiny_capture_style(const char *capture) {
    for (const char *name = capture;;) {
        for (size_t i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++)
            if (strcmp(STYLES[i].name, name) == 0) return &STYLES[i].style;
        const char *dot = strrchr(name, '.');
        if (!dot) return NULL;
        static _Thread_local char buf[128];
        size_t n = (size_t)(dot - name);
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, name, n);
        buf[n] = '\0';
        name = buf;
    }
}

// Headings take their level's colour from TokyoNight's rainbow, over a 10% tint.
static const tiny_style HEADINGS[] = {
    {"#7aa2f7", "#24293b", true, false, false},
    {"#e0af68", "#2e2a2d", true, false, false},
    {"#9ece6a", "#272d2d", true, false, false},
    {"#1abc9c", "#1a2b32", true, false, false},
    {"#bb9af7", "#2a283b", true, false, false},
    {"#9d7cd8", "#272538", true, false, false},
    {"#ff9e64", "#31282c", true, false, false},
    {"#f7768e", "#302430", true, false, false},
};

const tiny_style *tiny_heading_style(int level) {
    if (level < 1) level = 1;
    if (level > (int)(sizeof HEADINGS / sizeof HEADINGS[0]))
        level = (int)(sizeof HEADINGS / sizeof HEADINGS[0]);
    return &HEADINGS[level - 1];
}

static const tiny_style INLINE_CODE = {"#7aa2f7", "#414868", false, false, false};
const tiny_style *tiny_inline_code_style(void) { return &INLINE_CODE; }
