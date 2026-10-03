#include "editor.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "util.h"

#define TAB 4

struct cage_editor {
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
        int b = cage_utf8_decode(s + i, len - i, &cp);
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
        int b = cage_utf8_decode(s + i, len - i, &cp);
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
        int b = cage_utf8_decode(s + i, len - i, &cp);
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

cage_editor *cage_editor_new(void) {
    cage_editor *e = calloc(1, sizeof *e);
    if (!e) abort();
    e->nlines = 1;
    e->lines = xmalloc(sizeof *e->lines);
    e->lines[0] = xstrdup("");
    e->width = 80;
    e->text_dirty = true;
    return e;
}

void cage_editor_free(cage_editor *e) {
    if (!e) return;
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    free(e->lines);
    free(e->text);
    free(e);
}

const char *cage_editor_text(cage_editor *e) {
    if (!e->text_dirty) return e->text;
    tui_buf b;
    buf_init(&b);
    for (size_t i = 0; i < e->nlines; i++) {
        if (i) buf_addc(&b, '\n');
        buf_adds(&b, e->lines[i]);
    }
    free(e->text);
    e->text = buf_steal(&b);
    buf_free(&b);
    e->text_dirty = false;
    return e->text;
}

void cage_editor_clear(cage_editor *e) {
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    e->nlines = 1;
    e->lines = realloc(e->lines, sizeof *e->lines);
    e->lines[0] = xstrdup("");
    e->row = 0;
    e->col = 0;
    e->scroll = 0;
    e->text_dirty = true;
}

void cage_editor_set_text(cage_editor *e, const char *text) {
    for (size_t i = 0; i < e->nlines; i++) free(e->lines[i]);
    // Split on newline.
    size_t cap = 8, n = 0;
    char **lines = xmalloc(cap * sizeof *lines);
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        if (n == cap) {
            cap *= 2;
            lines = realloc(lines, cap * sizeof *lines);
            if (!lines) abort();
        }
        lines[n++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    free(e->lines);
    e->lines = lines;
    e->nlines = n;
    e->row = n - 1;
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
        int b = cage_utf8_decode(line + i, len - i, &cp);
        if (b <= 0) break;
        if (cp == '\t') col += TAB - (col % TAB);
        else col += cage_char_width(cp);
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

static void caret(cage_editor *e, int *row_out, int *col_out) {
    char *line = xstrdup(e->lines[e->row]);
    char *expanded = cage_expand_tabs(line, TAB);
    size_t nrows;
    char **chunks = cage_wrap_line(expanded, e->width, &nrows);
    size_t ncells;
    int *cells = cells_of(line, &ncells);
    int cell = cells[e->col < ncells ? e->col : ncells - 1];
    free(cells);
    int row = cell / e->width;
    if (row >= (int)nrows) {
        *row_out = (int)nrows - 1;
        *col_out = cage_display_width(chunks[nrows - 1]);
    } else {
        *row_out = row;
        *col_out = cell - row * e->width;
    }
    for (size_t i = 0; i < nrows; i++) free(chunks[i]);
    free(chunks);
    free(expanded);
    free(line);
}

// Replaces the code points [start, end) of a line with `insert`. A copy of
// `line` is passed because the caller may hold it.
static void line_splice(cage_editor *e, const char *line, size_t start, size_t end, const char *insert) {
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
    buf_free(&b);
    e->text_dirty = true;
}

static void insert_text(cage_editor *e, const char *text) {
    // Split `text` on newlines.
    size_t nparts = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') nparts++;

    if (nparts == 1) {
        line_splice(e, e->lines[e->row], e->col, e->col, text);
        e->col += cp_count(text);
        return;
    }

    char *line = e->lines[e->row];
    char *before = cp_slice(line, 0, e->col);
    char *after = cp_slice(line, e->col, cp_count(line));

    // Build the replacement lines.
    size_t n;
    char **parts = cage_split_lines(text, &n);

    tui_buf head;
    buf_init(&head);
    buf_adds(&head, before);
    buf_adds(&head, parts[0]);
    tui_buf tail;
    buf_init(&tail);
    buf_adds(&tail, parts[n - 1]);
    buf_adds(&tail, after);
    size_t caret_col = cp_count(parts[n - 1]);
    free(parts[0]);
    free(parts[n - 1]);

    // New line array: [0, row) + head + middle parts + tail + (row, end).
    char **nl = xmalloc((e->nlines + 1) * sizeof *nl);
    size_t k = 0;
    for (size_t i = 0; i < e->row; i++) nl[k++] = e->lines[i];
    nl[k++] = buf_steal(&head);
    buf_free(&head);
    for (size_t i = 1; i + 1 < n; i++) nl[k++] = parts[i];
    nl[k++] = buf_steal(&tail);
    buf_free(&tail);
    for (size_t i = e->row + 1; i < e->nlines; i++) nl[k++] = e->lines[i];
    free(line);
    free(e->lines);
    e->lines = nl;
    e->nlines = k;
    e->row += n - 1;
    e->col = caret_col;
    free(parts);
    free(before);
    free(after);
    e->text_dirty = true;
}

static void backspace(cage_editor *e) {
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
        buf_free(&b);
        free(e->lines[e->row]);
        for (size_t i = e->row; i + 1 < e->nlines; i++) e->lines[i] = e->lines[i + 1];
        e->nlines--;
        e->row--;
        e->col = prev;
        e->text_dirty = true;
    }
}

static void delete_forward(cage_editor *e) {
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
        buf_free(&b);
        free(e->lines[e->row + 1]);
        for (size_t i = e->row + 1; i + 1 < e->nlines; i++) e->lines[i] = e->lines[i + 1];
        e->nlines--;
        e->text_dirty = true;
    }
}

cage_edit_result cage_editor_handle(cage_editor *e, const cage_key *key) {
    switch (key->type) {
    case CAGE_KEY_SUBMIT:
        return CAGE_EDIT_SUBMIT;
    case CAGE_KEY_NEWLINE:
        insert_text(e, "\n");
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_TEXT:
        insert_text(e, key->text ? key->text : "");
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_BACKSPACE:
        backspace(e);
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_DELETE:
        delete_forward(e);
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_WORD_BACK: {
        size_t start = word_start(e->lines[e->row], e->col);
        line_splice(e, e->lines[e->row], start, e->col, "");
        e->col = start;
        return CAGE_EDIT_CHANGED;
    }
    case CAGE_KEY_LEFT:
        if (e->col > 0) e->col--;
        else if (e->row > 0) {
            e->row--;
            e->col = cp_count(e->lines[e->row]);
        }
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_RIGHT:
        if (e->col < cp_count(e->lines[e->row])) e->col++;
        else if (e->row + 1 < e->nlines) {
            e->row++;
            e->col = 0;
        }
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_WORD_LEFT: {
        size_t start = word_start(e->lines[e->row], e->col);
        if (start != e->col) {
            e->col = start;
            return CAGE_EDIT_CHANGED;
        }
        if (e->row == 0) return CAGE_EDIT_CHANGED;
        e->row--;
        size_t prev = cp_count(e->lines[e->row]);
        e->col = word_start(e->lines[e->row], prev);
        return CAGE_EDIT_CHANGED;
    }
    case CAGE_KEY_WORD_RIGHT: {
        size_t end = word_end(e->lines[e->row], e->col);
        if (end != e->col) {
            e->col = end;
            return CAGE_EDIT_CHANGED;
        }
        if (e->row + 1 >= e->nlines) return CAGE_EDIT_CHANGED;
        e->row++;
        e->col = word_end(e->lines[e->row], 0);
        return CAGE_EDIT_CHANGED;
    }
    case CAGE_KEY_UP: {
        int crow, ccol;
        caret(e, &crow, &ccol);
        if (crow > 0) {
            size_t n;
            int *cells = cells_of(e->lines[e->row], &n);
            int cell = cells[e->col < n ? e->col : n - 1] - e->width;
            free(cells);
            e->col = col_at_cell(e->lines[e->row], cell);
            return CAGE_EDIT_CHANGED;
        }
        if (e->row == 0) return CAGE_EDIT_CHANGED;
        e->row--;
        char *expanded = cage_expand_tabs(e->lines[e->row], TAB);
        size_t nrows;
        char **chunks = cage_wrap_line(expanded, e->width, &nrows);
        int last_row = (int)nrows - 1;
        for (size_t i = 0; i < nrows; i++) free(chunks[i]);
        free(chunks);
        free(expanded);
        e->col = col_at_cell(e->lines[e->row], last_row * e->width + ccol);
        return CAGE_EDIT_CHANGED;
    }
    case CAGE_KEY_DOWN: {
        int crow, ccol;
        caret(e, &crow, &ccol);
        char *expanded = cage_expand_tabs(e->lines[e->row], TAB);
        size_t nrows;
        char **chunks = cage_wrap_line(expanded, e->width, &nrows);
        for (size_t i = 0; i < nrows; i++) free(chunks[i]);
        free(chunks);
        free(expanded);
        if (crow < (int)nrows - 1) {
            size_t n;
            int *cells = cells_of(e->lines[e->row], &n);
            int cell = cells[e->col < n ? e->col : n - 1] + e->width;
            free(cells);
            e->col = col_at_cell(e->lines[e->row], cell);
            return CAGE_EDIT_CHANGED;
        }
        if (e->row + 1 >= e->nlines) return CAGE_EDIT_CHANGED;
        e->row++;
        e->col = col_at_cell(e->lines[e->row], ccol);
        return CAGE_EDIT_CHANGED;
    }
    case CAGE_KEY_HOME:
        e->col = 0;
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_END:
        e->col = cp_count(e->lines[e->row]);
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_DOC_START:
        e->row = 0;
        e->col = 0;
        return CAGE_EDIT_CHANGED;
    case CAGE_KEY_DOC_END:
        e->row = e->nlines - 1;
        e->col = cp_count(e->lines[e->row]);
        return CAGE_EDIT_CHANGED;
    default:
        return CAGE_EDIT_NONE;
    }
}

bool cage_editor_complete_word(cage_editor *e, char *(*step)(const char *word, void *user), void *user) {
    char *line = e->lines[e->row];
    size_t start = word_start(line, e->col);
    if (start == e->col) return false;
    // The character before the caret must not be whitespace.
    size_t off = cp_off(line, e->col - 1);
    uint32_t cp;
    cage_utf8_decode(line + off, strlen(line) - off, &cp);
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

cage_editor_view cage_editor_render(cage_editor *e, int width, int max_rows) {
    e->width = width > 0 ? width : 1;
    size_t cap = 16, n = 0;
    char **rows = xmalloc(cap * sizeof *rows);
    int cursor_row = 0, cursor_col = 0;
    for (size_t line = 0; line < e->nlines; line++) {
        char *disp = xstrdup(e->lines[line]);
        char *expanded = cage_expand_tabs(disp, TAB);
        size_t nchunks;
        char **chunks = cage_wrap_line(expanded, e->width, &nchunks);
        free(expanded);
        free(disp);
        if (line == e->row) {
            int cr, cc;
            caret(e, &cr, &cc);
            cursor_row = (int)n + cr;
            cursor_col = cc;
        }
        for (size_t i = 0; i < nchunks; i++) {
            if (n == cap) {
                cap *= 2;
                rows = realloc(rows, cap * sizeof *rows);
                if (!rows) abort();
            }
            rows[n++] = chunks[i];
        }
        free(chunks);
    }

    int view = max_rows > 0 ? max_rows : 1;
    if (cursor_row < e->scroll) e->scroll = cursor_row;
    else if (cursor_row >= e->scroll + view) e->scroll = cursor_row - view + 1;
    int max_scroll = (int)n - view;
    if (max_scroll < 0) max_scroll = 0;
    if (e->scroll > max_scroll) e->scroll = max_scroll;
    if (e->scroll < 0) e->scroll = 0;

    size_t shown = n > (size_t)e->scroll ? n - (size_t)e->scroll : 0;
    if (shown > (size_t)view) shown = (size_t)view;
    char **out = xmalloc((shown ? shown : 1) * sizeof *out);
    // Free rows scrolled off and after the window.
    for (size_t i = 0; i < n; i++) {
        if (i < (size_t)e->scroll || i >= (size_t)e->scroll + shown) free(rows[i]);
        else out[i - (size_t)e->scroll] = rows[i];
    }
    free(rows);

    cage_editor_view v = {
        .rows = out,
        .nrows = shown,
        .cursor_row = cursor_row - e->scroll,
        .cursor_col = cursor_col,
    };
    return v;
}

