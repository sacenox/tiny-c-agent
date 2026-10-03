#include "editor.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define TAB 4

struct tiny_editor {
    char **lines;
    size_t nlines;
    size_t row;
    size_t col;     // code point index within the row
    int scroll;
    int width;
    char *text;     // cached join, valid until the next change
    bool text_dirty;
};

static size_t cp_count(const char *s) {
    size_t n = 0, len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int b = tiny_utf8_decode(s + i, len - i, &cp);
        if (b <= 0) break;
        i += (size_t)b;
        n++;
    }
    return n;
}

// Byte offset of code point index `idx`, clamped to the end.
static size_t cp_off(const char *s, size_t idx) {
    size_t i = 0, n = 0, len = strlen(s);
    while (n < idx && i < len) {
        uint32_t cp;
        int b = tiny_utf8_decode(s + i, len - i, &cp);
        if (b <= 0) break;
        i += (size_t)b;
        n++;
    }
    return i;
}

// The code points [a, b) as a new string.
static char *cp_slice(const char *s, size_t a, size_t b) {
    size_t start = cp_off(s, a);
    size_t end = cp_off(s, b);
    size_t n = end > start ? end - start : 0;
    char *out = xmalloc(n + 1);
    memcpy(out, s + start, n);
    out[n] = '\0';
    return out;
}

static bool cp_is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f';
}

// Decodes `s` into a malloc'd array of code points; *n receives the count.
static uint32_t *cps_of(const char *s, size_t *n_out) {
    size_t len = strlen(s), cap = 8, n = 0;
    uint32_t *cps = xmalloc(cap * sizeof *cps);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int b = tiny_utf8_decode(s + i, len - i, &cp);
        if (b <= 0) break;
        if (n == cap) {
            cap *= 2;
            cps = xrealloc(cps, cap * sizeof *cps);
        }
        cps[n++] = cp;
        i += (size_t)b;
    }
    *n_out = n;
    return cps;
}

// First code point index of the word before `col`.
static size_t word_start(const char *s, size_t col) {
    size_t n;
    uint32_t *cps = cps_of(s, &n);
    size_t i = col;
    while (i > 0 && cp_is_space(cps[i - 1])) i--;
    while (i > 0 && !cp_is_space(cps[i - 1])) i--;
    free(cps);
    return i;
}

// One past the word at or after `col`.
static size_t word_end(const char *s, size_t col) {
    size_t n;
    uint32_t *cps = cps_of(s, &n);
    size_t i = col;
    while (i < n && cp_is_space(cps[i])) i++;
    while (i < n && !cp_is_space(cps[i])) i++;
    free(cps);
    return i;
}

tiny_editor *tiny_editor_new(void) {
    tiny_editor *e = calloc(1, sizeof *e);
    if (!e) abort();
    e->nlines = 1;
    e->lines = xmalloc(sizeof *e->lines);
    e->lines[0] = xstrdup("");
    e->width = 80;
    e->text_dirty = true;
    return e;
}

void tiny_editor_free(tiny_editor *e) {
    if (!e) return;
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    free(e->lines);
    free(e->text);
    free(e);
}

const char *tiny_editor_text(tiny_editor *e) {
    if (!e->text_dirty) return e->text;
    tui_buf b;
    buf_init(&b);
    for (size_t i = 0; i < e->nlines; i++) {
        if (i) buf_addc(&b, '\n');
        buf_adds(&b, e->lines[i]);
    }
    free(e->text);
    e->text = buf_steal(&b);
    e->text_dirty = false;
    return e->text;
}

void tiny_editor_clear(tiny_editor *e) {
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    e->nlines = 1;
    e->lines = realloc(e->lines, sizeof *e->lines);
    e->lines[0] = xstrdup("");
    e->row = 0;
    e->col = 0;
    e->scroll = 0;
    e->text_dirty = true;
}

void tiny_editor_set_text(tiny_editor *e, const char *text) {
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    tiny_strv lines = tiny_split_lines(text);
    free(e->lines);
    e->lines = lines.v;
    e->nlines = lines.n;
    e->row = lines.n - 1;
    e->col = cp_count(e->lines[e->row]);
    e->scroll = 0;
    e->text_dirty = true;
}

// Display column of every code point boundary in `line`, tabs expanded.
static int *cells_of(const char *line, size_t *n_out) {
    size_t len = strlen(line);
    size_t cap = 16, n = 0;
    int *out = xmalloc(cap * sizeof *out);
    out[n++] = 0;
    int col = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int b = tiny_utf8_decode(line + i, len - i, &cp);
        if (b <= 0) break;
        if (cp == '\t') col += TAB - (col % TAB);
        else col += tiny_char_width(cp);
        if (n == cap) {
            cap *= 2;
            out = realloc(out, cap * sizeof *out);
            if (!out) abort();
        }
        out[n++] = col;
        i += (size_t)b;
    }
    *n_out = n;
    return out;
}

// The caret column nearest `cell`, never past the end of the line.
static size_t col_at_cell(const char *line, int cell) {
    size_t n;
    int *cells = cells_of(line, &n);
    size_t i = n - 1;
    while (i > 0 && cells[i] > cell) i--;
    free(cells);
    return i;
}

static void caret(tiny_editor *e, int *row_out, int *col_out) {
    char *line = xstrdup(e->lines[e->row]);
    char *expanded = tiny_expand_tabs(line, TAB);
    tiny_strv chunks = tiny_wrap_line(expanded, e->width);
    size_t ncells;
    int *cells = cells_of(line, &ncells);
    int cell = cells[e->col < ncells ? e->col : ncells - 1];
    free(cells);
    int row = cell / e->width;
    if (row >= (int)chunks.n) {
        *row_out = (int)chunks.n - 1;
        *col_out = tiny_display_width(chunks.v[chunks.n - 1]);
    } else {
        *row_out = row;
        *col_out = cell - row * e->width;
    }
    strv_free(&chunks);
    free(expanded);
    free(line);
}

// Replaces the code points [start, end) of a line with `insert`. A copy of
// `line` is passed because the caller may hold it.
static void line_splice(tiny_editor *e, const char *line, size_t start, size_t end, const char *insert) {
    char *before = cp_slice(line, 0, start);
    char *after = cp_slice(line, end, cp_count(line));
    tui_buf b;
    buf_init(&b);
    buf_adds(&b, before);
    buf_adds(&b, insert);
    buf_adds(&b, after);
    free(before);
    free(after);
    free(e->lines[e->row]);
    e->lines[e->row] = buf_steal(&b);
    e->text_dirty = true;
}

static void insert_text(tiny_editor *e, const char *text) {
    if (!strchr(text, '\n')) {
        line_splice(e, e->lines[e->row], e->col, e->col, text);
        e->col += cp_count(text);
        return;
    }

    char *line = e->lines[e->row];
    char *before = cp_slice(line, 0, e->col);
    char *after = cp_slice(line, e->col, cp_count(line));

    // Build the replacement lines.
    tiny_strv parts = tiny_split_lines(text);
    size_t n = parts.n;

    tui_buf head;
    buf_init(&head);
    buf_adds(&head, before);
    buf_adds(&head, parts.v[0]);
    tui_buf tail;
    buf_init(&tail);
    buf_adds(&tail, parts.v[n - 1]);
    buf_adds(&tail, after);
    size_t caret_col = cp_count(parts.v[n - 1]);
    free(parts.v[0]);
    free(parts.v[n - 1]);

    // New line array: [0, row) + head + middle parts + tail + (row, end).
    char **nl = xmalloc((e->nlines + 1) * sizeof *nl);
    size_t k = 0;
    for (size_t i = 0; i < e->row; i++) nl[k++] = e->lines[i];
    nl[k++] = buf_steal(&head);
    for (size_t i = 1; i + 1 < n; i++) nl[k++] = parts.v[i];
    nl[k++] = buf_steal(&tail);
    for (size_t i = e->row + 1; i < e->nlines; i++) nl[k++] = e->lines[i];
    free(line);
    free(e->lines);
    e->lines = nl;
    e->nlines = k;
    e->row += n - 1;
    e->col = caret_col;
    free(parts.v);
    free(before);
    free(after);
    e->text_dirty = true;
}

static void backspace(tiny_editor *e) {
    if (e->col > 0) {
        line_splice(e, e->lines[e->row], e->col - 1, e->col, "");
        e->col--;
        return;
    }
    if (e->row > 0) {
        size_t prev = cp_count(e->lines[e->row - 1]);
        tui_buf b;
        buf_init(&b);
        buf_adds(&b, e->lines[e->row - 1]);
        buf_adds(&b, e->lines[e->row]);
        free(e->lines[e->row - 1]);
        e->lines[e->row - 1] = buf_steal(&b);
        free(e->lines[e->row]);
        for (size_t i = e->row; i + 1 < e->nlines; i++) e->lines[i] = e->lines[i + 1];
        e->nlines--;
        e->row--;
        e->col = prev;
        e->text_dirty = true;
    }
}

static void delete_forward(tiny_editor *e) {
    size_t n = cp_count(e->lines[e->row]);
    if (e->col < n) {
        line_splice(e, e->lines[e->row], e->col, e->col + 1, "");
        return;
    }
    if (e->row + 1 < e->nlines) {
        tui_buf b;
        buf_init(&b);
        buf_adds(&b, e->lines[e->row]);
        buf_adds(&b, e->lines[e->row + 1]);
        free(e->lines[e->row]);
        e->lines[e->row] = buf_steal(&b);
        free(e->lines[e->row + 1]);
        for (size_t i = e->row + 1; i + 1 < e->nlines; i++) e->lines[i] = e->lines[i + 1];
        e->nlines--;
        e->text_dirty = true;
    }
}

tiny_edit_result tiny_editor_handle(tiny_editor *e, const tiny_key *key) {
    switch (key->type) {
    case TINY_KEY_SUBMIT:
        return TINY_EDIT_SUBMIT;
    case TINY_KEY_NEWLINE:
        insert_text(e, "\n");
        return TINY_EDIT_CHANGED;
    case TINY_KEY_TEXT:
        insert_text(e, key->text ? key->text : "");
        return TINY_EDIT_CHANGED;
    case TINY_KEY_BACKSPACE:
        backspace(e);
        return TINY_EDIT_CHANGED;
    case TINY_KEY_DELETE:
        delete_forward(e);
        return TINY_EDIT_CHANGED;
    case TINY_KEY_WORD_BACK: {
        size_t start = word_start(e->lines[e->row], e->col);
        line_splice(e, e->lines[e->row], start, e->col, "");
        e->col = start;
        return TINY_EDIT_CHANGED;
    }
    case TINY_KEY_LEFT:
        if (e->col > 0) e->col--;
        else if (e->row > 0) {
            e->row--;
            e->col = cp_count(e->lines[e->row]);
        }
        return TINY_EDIT_CHANGED;
    case TINY_KEY_RIGHT:
        if (e->col < cp_count(e->lines[e->row])) e->col++;
        else if (e->row + 1 < e->nlines) {
            e->row++;
            e->col = 0;
        }
        return TINY_EDIT_CHANGED;
    case TINY_KEY_WORD_LEFT: {
        size_t start = word_start(e->lines[e->row], e->col);
        if (start != e->col) {
            e->col = start;
            return TINY_EDIT_CHANGED;
        }
        if (e->row == 0) return TINY_EDIT_CHANGED;
        e->row--;
        size_t prev = cp_count(e->lines[e->row]);
        e->col = word_start(e->lines[e->row], prev);
        return TINY_EDIT_CHANGED;
    }
    case TINY_KEY_WORD_RIGHT: {
        size_t end = word_end(e->lines[e->row], e->col);
        if (end != e->col) {
            e->col = end;
            return TINY_EDIT_CHANGED;
        }
        if (e->row + 1 >= e->nlines) return TINY_EDIT_CHANGED;
        e->row++;
        e->col = word_end(e->lines[e->row], 0);
        return TINY_EDIT_CHANGED;
    }
    case TINY_KEY_UP: {
        int crow, ccol;
        caret(e, &crow, &ccol);
        if (crow > 0) {
            size_t n;
            int *cells = cells_of(e->lines[e->row], &n);
            int cell = cells[e->col < n ? e->col : n - 1] - e->width;
            free(cells);
            e->col = col_at_cell(e->lines[e->row], cell);
            return TINY_EDIT_CHANGED;
        }
        if (e->row == 0) return TINY_EDIT_CHANGED;
        e->row--;
        char *expanded = tiny_expand_tabs(e->lines[e->row], TAB);
        tiny_strv chunks = tiny_wrap_line(expanded, e->width);
        free(expanded);
        e->col = col_at_cell(e->lines[e->row], ((int)chunks.n - 1) * e->width + ccol);
        strv_free(&chunks);
        return TINY_EDIT_CHANGED;
    }
    case TINY_KEY_DOWN: {
        int crow, ccol;
        caret(e, &crow, &ccol);
        char *expanded = tiny_expand_tabs(e->lines[e->row], TAB);
        tiny_strv chunks = tiny_wrap_line(expanded, e->width);
        free(expanded);
        if (crow < (int)chunks.n - 1) {
            size_t n;
            int *cells = cells_of(e->lines[e->row], &n);
            int cell = cells[e->col < n ? e->col : n - 1] + e->width;
            free(cells);
            e->col = col_at_cell(e->lines[e->row], cell);
            strv_free(&chunks);
            return TINY_EDIT_CHANGED;
        }
        strv_free(&chunks);
        if (e->row + 1 >= e->nlines) return TINY_EDIT_CHANGED;
        e->row++;
        e->col = col_at_cell(e->lines[e->row], ccol);
        return TINY_EDIT_CHANGED;
    }
    case TINY_KEY_HOME:
        e->col = 0;
        return TINY_EDIT_CHANGED;
    case TINY_KEY_END:
        e->col = cp_count(e->lines[e->row]);
        return TINY_EDIT_CHANGED;
    case TINY_KEY_DOC_START:
        e->row = 0;
        e->col = 0;
        return TINY_EDIT_CHANGED;
    case TINY_KEY_DOC_END:
        e->row = e->nlines - 1;
        e->col = cp_count(e->lines[e->row]);
        return TINY_EDIT_CHANGED;
    default:
        return TINY_EDIT_NONE;
    }
}

bool tiny_editor_complete_word(tiny_editor *e, char *(*step)(const char *word, void *user), void *user) {
    char *line = e->lines[e->row];
    size_t start = word_start(line, e->col);
    if (start == e->col) return false;
    // The character before the caret must not be whitespace.
    size_t off = cp_off(line, e->col - 1);
    uint32_t cp;
    tiny_utf8_decode(line + off, strlen(line) - off, &cp);
    if (cp_is_space(cp)) return false;
    char *word = cp_slice(line, start, e->col);
    char *completed = step(word, user);
    free(word);
    if (!completed) return false;
    line_splice(e, line, start, e->col, completed);
    e->col = start + cp_count(completed);
    free(completed);
    return true;
}

tiny_editor_view tiny_editor_render(tiny_editor *e, int width, int max_rows) {
    e->width = width > 0 ? width : 1;
    tiny_strv rows = {0};
    int cursor_row = 0, cursor_col = 0;
    for (size_t line = 0; line < e->nlines; line++) {
        char *disp = xstrdup(e->lines[line]);
        char *expanded = tiny_expand_tabs(disp, TAB);
        tiny_strv chunks = tiny_wrap_line(expanded, e->width);
        free(expanded);
        free(disp);
        if (line == e->row) {
            int cr, cc;
            caret(e, &cr, &cc);
            cursor_row = (int)rows.n + cr;
            cursor_col = cc;
        }
        for (size_t i = 0; i < chunks.n; i++) strv_push(&rows, chunks.v[i]);
        free(chunks.v);
    }

    int view = max_rows > 0 ? max_rows : 1;
    if (cursor_row < e->scroll) e->scroll = cursor_row;
    else if (cursor_row >= e->scroll + view) e->scroll = cursor_row - view + 1;
    int max_scroll = (int)rows.n - view;
    if (max_scroll < 0) max_scroll = 0;
    if (e->scroll > max_scroll) e->scroll = max_scroll;
    if (e->scroll < 0) e->scroll = 0;

    size_t shown = rows.n > (size_t)e->scroll ? rows.n - (size_t)e->scroll : 0;
    if (shown > (size_t)view) shown = (size_t)view;
    char **out = xmalloc((shown ? shown : 1) * sizeof *out);
    // Free rows scrolled off and after the window.
    for (size_t i = 0; i < rows.n; i++) {
        if (i < (size_t)e->scroll || i >= (size_t)e->scroll + shown) free(rows.v[i]);
        else out[i - (size_t)e->scroll] = rows.v[i];
    }
    free(rows.v);

    tiny_editor_view v = {
        .rows = out,
        .nrows = shown,
        .cursor_row = cursor_row - e->scroll,
        .cursor_col = cursor_col,
    };
    return v;
}

