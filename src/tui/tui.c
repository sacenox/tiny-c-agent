#include "tui.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <yyjson.h>

#include "agent.h"
#include "catalog.h"
#include "commands.h"
#include "complete.h"
#include "editor.h"
#include "highlight.h"
#include "message.h"
#include "resolve.h"
#include "session.h"
#include "stream.h"
#include "term.h"
#include "theme.h"
#include "usage.h"

#define SPINNER_MS 120
#define MAX_BODY_ROWS 12
#define ELIDED_HEAD 4
#define ELIDED_TAIL 4

static const char *const SPINNER[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

static const char *const BODY_PREFIX = " | ";
static const char *const ERROR_PREFIX = " ! ";

static volatile sig_atomic_t g_exit;
static volatile sig_atomic_t g_resize;

static void on_signal(int sig) {
    if (sig == SIGWINCH) g_resize = 1;
    else g_exit = 1;
}

// ---- small helpers ---------------------------------------------------------

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static void write_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        s += w;
        n -= (size_t)w;
    }
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

typedef struct {
    cage_body_line *v;
    size_t n;
    size_t cap;
} blist;

static void bl_push(blist *l, char *text, cage_fg_fn style, const char *bg) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
        if (!l->v) abort();
    }
    l->v[l->n++] = (cage_body_line){text, style, bg};
}

static void free_rows(char **rows, size_t n) {
    for (size_t i = 0; i < n; i++) free(rows[i]);
    free(rows);
}

// ---- tui state -------------------------------------------------------------

typedef struct {
    const cage_tui_config *c;
    cage_model model;
    cage_agent agent;
    cage_session *session;

    int in_fd;
    int out_fd;
    struct termios orig;
    bool raw;
    cage_key_parser *parser;

    int width;
    int height;

    cage_editor *editor;
    cage_mdstream reply;
    cage_tailstream activity;
    blist pending_calls;

    tui_buf scroll;
    bool wrote;
    bool last_blank;
    bool separator;

    int live_rows;
    int live_up;

    // Deadlines for completing a held ESC or escape sequence, in monotonic
    // seconds. Data arriving before a deadline replaces it.
    bool esc_pending;
    bool seq_pending;
    double esc_deadline;
    double seq_deadline;

    bool active;
    volatile bool cancel;
    _Atomic bool pause_requested;
    bool paused;
    bool steering_waiting;
    char *steering_text;
    cage_phase phase;
    char *detail;
    char *writing_tool;
    long ctx_used;
    long context_window;
    struct timespec turn_start;
    int frame;
    char *streamed;
    bool closed;

    // A pending command choice: the next submitted line picks one of the
    // numbered options. `choice_provider` carries the provider just chosen
    // into the chained model list.
    bool choice_active;
    cage_choose choice_kind;
    char *choice_provider;
    char **choice_ids;
    char **choice_labels;
    size_t choice_n;

    pthread_mutex_t lock;
    pthread_cond_t cv;
    pthread_t worker;
    bool worker_live;
} tui;

static double elapsed(const tui *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double s = (double)(now.tv_sec - t->turn_start.tv_sec) +
               (double)(now.tv_nsec - t->turn_start.tv_nsec) / 1e9;
    return s < 0 ? 0 : s;
}

// Monotonic seconds, for key-parser deadlines.
static double monotonic_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

// ---- rendering -------------------------------------------------------------

// Wraps plain text, then styles each row. Styling after the break lets a
// continuation row inherit its source line's style.
static char **render_rows(const cage_body_line *lines, size_t n, int width, size_t *out_n) {
    size_t cap = 16, m = 0;
    char **out = xmalloc(cap * sizeof *out);
    for (size_t i = 0; i < n; i++) {
        char *clean = cage_sanitize(lines[i].text);
        char *expanded = cage_expand_tabs(clean, 4);
        size_t nrows;
        char **rows = cage_wrap_line(expanded, width, &nrows);
        free(clean);
        free(expanded);
        for (size_t r = 0; r < nrows; r++) {
            if (m == cap) {
                cap *= 2;
                out = realloc(out, cap * sizeof *out);
                if (!out) abort();
            }
            if (rows[r][0] == '\0') {
                out[m++] = rows[r];
                continue;
            }
            tui_buf b;
            buf_init(&b);
            const char *bg = lines[i].bg ? lines[i].bg : cage_normal_bg();
            sgr_bg(&b, bg);
            if (lines[i].style) lines[i].style(&b, rows[r]);
            else buf_adds(&b, rows[r]);
            sgr_bg(&b, bg);
            out[m++] = buf_steal(&b);
            buf_free(&b);
            free(rows[r]);
        }
        free(rows);
    }
    *out_n = m;
    return out;
}

// render_rows plus the display-only elision for long tool bodies.
static char **body_rows(const cage_body_line *lines, size_t n, int width, size_t *out_n) {
    size_t nrows;
    char **rows = render_rows(lines, n, width, &nrows);
    if (nrows <= MAX_BODY_ROWS) {
        *out_n = nrows;
        return rows;
    }
    size_t hidden = nrows - ELIDED_HEAD - ELIDED_TAIL;
    size_t m = ELIDED_HEAD + 1 + ELIDED_TAIL;
    char **out = xmalloc(m * sizeof *out);
    size_t k = 0;
    for (size_t i = 0; i < ELIDED_HEAD; i++) out[k++] = rows[i], rows[i] = NULL;
    tui_buf b;
    buf_init(&b);
    char note[64];
    snprintf(note, sizeof note, "... %zu lines not shown ...", hidden);
    fg_dim(&b, note);
    out[k++] = buf_steal(&b);
    buf_free(&b);
    for (size_t i = nrows - ELIDED_TAIL; i < nrows; i++) out[k++] = rows[i], rows[i] = NULL;
    free_rows(rows, nrows);
    *out_n = m;
    return out;
}

static void append_painted(tui_buf *b, const char *row) {
    sgr_plain(b);
    buf_adds(b, row);
    buf_adds(b, "\x1b[K");
}

// ---- scrollback ------------------------------------------------------------

static void tui_render_locked(tui *t);

static void push(tui *t, const char *row) {
    bool blank = row[0] == '\0' || (t->separator && t->wrote);
    t->separator = false;
    if (blank && t->wrote && !t->last_blank) {
        append_painted(&t->scroll, "");
        buf_adds(&t->scroll, "\r\n");
        t->last_blank = true;
    }
    if (row[0] != '\0') {
        append_painted(&t->scroll, row);
        buf_adds(&t->scroll, "\r\n");
        t->wrote = true;
        t->last_blank = false;
    }
}

static void commit_lines(tui *t, cage_body_line *lines, size_t n) {
    size_t nrows;
    char **rows = render_rows(lines, n, t->width > 0 ? t->width : 1, &nrows);
    for (size_t i = 0; i < nrows; i++) push(t, rows[i]);
    free_rows(rows, nrows);
}

static void commit_user(tui *t, const char *text) {
    t->separator = true;
    char *copy = xstrdup(text);
    size_t cap = 8, n = 0;
    cage_body_line *lines = xmalloc(cap * sizeof *lines);
    for (char *p = copy;;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (n == cap) {
            cap *= 2;
            lines = realloc(lines, cap * sizeof *lines);
            if (!lines) abort();
        }
        lines[n++] = (cage_body_line){xstrdup(p), fg_blue, NULL};
        if (!nl) break;
        p = nl + 1;
    }
    free(copy);
    commit_lines(t, lines, n);
    for (size_t i = 0; i < n; i++) free(lines[i].text);
    free(lines);
    t->separator = true;
}

// ---- tool result projection ------------------------------------------------

static bool edit_header(const char *line) {
    return starts_with(line, "Index: ") || starts_with(line, "--- ") ||
           starts_with(line, "+++ ") ||
           (starts_with(line, "===") && line[3] == '=');
}

static cage_body_line diff_line(const char *line) {
    if (starts_with(line, "@@")) return (cage_body_line){xstrdup(line), fg_cyan, NULL};
    if (starts_with(line, "+")) return (cage_body_line){xstrdup(line), fg_green, cage_diff_add()};
    if (starts_with(line, "-")) return (cage_body_line){xstrdup(line), fg_red, cage_diff_delete()};
    if (starts_with(line, "\\ No newline")) return (cage_body_line){xstrdup(line), fg_dim, NULL};
    if (starts_with(line, " ")) return (cage_body_line){xstrdup(line), fg_dim, NULL};
    return (cage_body_line){xstrdup(line), NULL, NULL};
}

// Splits a tool result into its display lines, rewriting the trailing exit code
// for bash and stripping the repeated diff header for edit.
static cage_body_line *result_lines(const char *name, const char *text, bool is_error, size_t *out_n) {
    // trimEnd
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    char *copy = xmalloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';

    size_t cap = 8, n = 0;
    char **raw = xmalloc(cap * sizeof *raw);
    for (char *p = copy;;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (n == cap) {
            cap *= 2;
            raw = realloc(raw, cap * sizeof *raw);
            if (!raw) abort();
        }
        raw[n++] = p;
        if (!nl) break;
        p = nl + 1;
    }

    blist out = {0};
    bool diff = false;
    size_t start = 0;
    if (strcmp(name, "bash") == 0 && n > 0) {
        const char *last = raw[n - 1];
        if (starts_with(last, "exit code: ")) {
            const char *code = last + strlen("exit code: ");
            n--;
            if (is_error) {
                tui_buf b;
                buf_init(&b);
                buf_adds(&b, "exit ");
                buf_adds(&b, code);
                char *tmp = b.data ? buf_steal(&b) : xstrdup("");
                buf_free(&b);
                bl_push(&out, xstrdup(tmp), fg_red, NULL);
                free(tmp);
            }
        }
    } else if (strcmp(name, "edit") == 0 && n > 0) {
        if (starts_with(raw[0], "edited ") || starts_with(raw[0], "created ")) {
            start = 1;
            while (start < n && edit_header(raw[start])) start++;
            diff = true;
        }
    }
    for (size_t i = start; i < n; i++) {
        if (diff) {
            cage_body_line bl = diff_line(raw[i]);
            bl_push(&out, bl.text, bl.style, bl.bg);
        } else {
            bl_push(&out, xstrdup(raw[i]), NULL, NULL);
        }
    }
    free(copy);
    free(raw);
    *out_n = out.n;
    return out.v;
}

static cage_body_line *diff_rows(const cage_file_diff *diffs, size_t ndiffs, size_t *out_n) {
    blist out = {0};
    for (size_t d = 0; d < ndiffs; d++) {
        bl_push(&out, xstrdup(diffs[d].path), fg_cyan, NULL);
        if (diffs[d].patch) {
            size_t len = strlen(diffs[d].patch);
            while (len > 0 && (diffs[d].patch[len - 1] == '\n' || diffs[d].patch[len - 1] == '\r')) len--;
            char *copy = xmalloc(len + 1);
            memcpy(copy, diffs[d].patch, len);
            copy[len] = '\0';
            size_t cap = 8, n = 0;
            char **lines = xmalloc(cap * sizeof *lines);
            for (char *p = copy;;) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (n == cap) {
                    cap *= 2;
                    lines = realloc(lines, cap * sizeof *lines);
                    if (!lines) abort();
                }
                lines[n++] = p;
                if (!nl) break;
                p = nl + 1;
            }
            size_t start = 0;
            while (start < n && edit_header(lines[start])) start++;
            for (size_t i = start; i < n; i++) {
                cage_body_line bl = diff_line(lines[i]);
                bl_push(&out, bl.text, bl.style, bl.bg);
            }
            free(lines);
            free(copy);
        } else if (diffs[d].note) {
            bl_push(&out, xstrdup(diffs[d].note), fg_dim, NULL);
        }
    }
    *out_n = out.n;
    return out.v;
}

static void commit_tool_result(tui *t, const char *name, const char *text, bool is_error,
                               const cage_file_diff *diffs, size_t ndiffs) {
    t->separator = true;
    if (t->pending_calls.n > 0) {
        cage_body_line call = t->pending_calls.v[0];
        memmove(t->pending_calls.v, t->pending_calls.v + 1, (t->pending_calls.n - 1) * sizeof *t->pending_calls.v);
        t->pending_calls.n--;
        commit_lines(t, &call, 1);
        free(call.text);
    }
    int width = t->width - (int)strlen(BODY_PREFIX);
    if (width < 1) width = 1;
    size_t nlines;
    cage_body_line *lines = result_lines(name, text, is_error, &nlines);
    size_t nrows;
    char **rows = (strcmp(name, "edit") == 0) ? render_rows(lines, nlines, width, &nrows)
                                              : body_rows(lines, nlines, width, &nrows);
    tui_buf bodychrome;
    buf_init(&bodychrome);
    fg_dim(&bodychrome, BODY_PREFIX);
    tui_buf errchrome;
    buf_init(&errchrome);
    fg_red(&errchrome, ERROR_PREFIX);
    for (size_t i = 0; i < nrows; i++) {
        const char *prefix = (is_error && i == nrows - 1) ? errchrome.data : bodychrome.data;
        tui_buf row;
        buf_init(&row);
        buf_adds(&row, prefix);
        buf_adds(&row, rows[i]);
        if (row.data) push(t, row.data);
        buf_free(&row);
    }
    free_rows(rows, nrows);
    buf_free(&bodychrome);
    buf_free(&errchrome);
    for (size_t i = 0; i < nlines; i++) free(lines[i].text);
    free(lines);

    if (diffs && ndiffs > 0) {
        size_t dn;
        cage_body_line *dlines = diff_rows(diffs, ndiffs, &dn);
        size_t dr;
        char **drows = render_rows(dlines, dn, width, &dr);
        tui_buf chrome;
        buf_init(&chrome);
        fg_dim(&chrome, BODY_PREFIX);
        for (size_t i = 0; i < dr; i++) {
            tui_buf row;
            buf_init(&row);
            buf_adds(&row, chrome.data);
            buf_adds(&row, drows[i]);
            if (row.data) push(t, row.data);
            buf_free(&row);
        }
        buf_free(&chrome);
        free_rows(drows, dr);
        for (size_t i = 0; i < dn; i++) free(dlines[i].text);
        free(dlines);
    }
    t->separator = true;
}

static void flush_calls(tui *t) {
    for (size_t i = 0; i < t->pending_calls.n; i++) {
        t->separator = true;
        commit_lines(t, &t->pending_calls.v[i], 1);
    }
    for (size_t i = 0; i < t->pending_calls.n; i++) free(t->pending_calls.v[i].text);
    t->pending_calls.n = 0;
}

// ---- call line -------------------------------------------------------------

static char *call_summary(const char *name, const char *arguments) {
    yyjson_doc *doc = yyjson_read(arguments ? arguments : "{}", arguments ? strlen(arguments) : 2, 0);
    const char *key = (strcmp(name, "bash") == 0)  ? "command"
                      : (strcmp(name, "edit") == 0 || strcmp(name, "read") == 0) ? "path"
                                                                                 : NULL;
    const char *value = NULL;
    if (doc && key) {
        yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(doc), key);
        if (yyjson_is_str(v)) value = yyjson_get_str(v);
    }
    tui_buf b;
    buf_init(&b);
    if (value) {
        // Newlines and tabs collapse to a single space.
        for (const char *p = value; *p; p++) {
            if (*p == '\n' || *p == '\t') {
                if (b.len > 0 && b.data[b.len - 1] != ' ') buf_addc(&b, ' ');
            } else {
                buf_addc(&b, *p);
            }
        }
    } else {
        buf_adds(&b, arguments ? arguments : "");
    }
    if (doc) yyjson_doc_free(doc);
    // trim
    while (b.len > 0 && (b.data[b.len - 1] == ' ')) b.data[--b.len] = '\0';
    return b.data ? buf_steal(&b) : xstrdup("");
}

// ---- status ----------------------------------------------------------------

static char *status_line(tui *t) {
    char *usage = cage_context_usage_line(t->ctx_used, t->context_window, 0);
    if (t->paused || t->phase == CAGE_PHASE_PAUSING) {
        tui_buf b;
        buf_init(&b);
        fg_dim(&b, "paused - type steering, Enter to submit");
        buf_adds(&b, " · ");
        buf_adds(&b, usage);
        free(usage);
        return b.data ? buf_steal(&b) : xstrdup("");
    }
    if (!t->active || t->phase == CAGE_PHASE_IDLE) return usage;

    const char *label;
    char buf[128];
    switch (t->phase) {
    case CAGE_PHASE_PREPARING: label = "preparing"; break;
    case CAGE_PHASE_WAITING_MODEL: label = "waiting for provider"; break;
    case CAGE_PHASE_STREAMING:
        if (t->writing_tool) {
            snprintf(buf, sizeof buf, "writing %s", t->writing_tool);
            label = buf;
        } else {
            label = "streaming";
        }
        break;
    case CAGE_PHASE_RUNNING_TOOL:
        snprintf(buf, sizeof buf, "running %s", t->detail ? t->detail : "tool");
        label = buf;
        break;
    default: label = "idle"; break;
    }
    tui_buf phase;
    buf_init(&phase);
    char text[192];
    snprintf(text, sizeof text, "%s %s · %ds", SPINNER[t->frame % (int)(sizeof SPINNER / sizeof SPINNER[0])],
             label, (int)elapsed(t));
    fg_dim(&phase, text);

    tui_buf out;
    buf_init(&out);
    buf_adds(&out, phase.data ? phase.data : "");
    buf_adds(&out, " · ");
    buf_adds(&out, usage);
    buf_free(&phase);
    free(usage);
    return out.data ? buf_steal(&out) : xstrdup("");
}

// ---- live region -----------------------------------------------------------

static void live_clear(tui *t, tui_buf *out) {
    if (t->live_rows == 0) return;
    if (t->live_up > 0) buf_addf(out, "\x1b[%dB", t->live_up);
    if (t->live_rows > 1) buf_addf(out, "\x1b[%dA", t->live_rows - 1);
    buf_adds(out, "\r");
    sgr_plain(out);
    buf_adds(out, "\x1b[J");
    t->live_rows = 0;
    t->live_up = 0;
}

static void live_draw(tui *t, tui_buf *out, char **lines, size_t n, int cursor_row, int cursor_col,
                      const char *above) {
    live_clear(t, out);
    buf_adds(out, above);
    for (size_t i = 0; i < n; i++) {
        if (i > 0) buf_adds(out, "\r\n");
        buf_adds(out, lines[i]);
    }
    int up = (int)n - 1 - cursor_row;
    if (up > 0) buf_addf(out, "\x1b[%dA", up);
    buf_adds(out, "\r");
    if (cursor_col > 0) buf_addf(out, "\x1b[%dG", cursor_col + 1);
    t->live_rows = (int)n;
    t->live_up = up;
}

static void tui_render_locked(tui *t) {
    if (t->closed) return;
    int width = t->width > 0 ? t->width : 80;
    int height = (t->height > 1 ? t->height - 1 : 1);

    char *status = status_line(t);
    size_t nstatus;
    char **status_rows = cage_wrap_line(status, width, &nstatus);
    free(status);

    size_t ninf;
    cage_body_line *infl = cage_mdstream_pending(&t->reply, &ninf);
    if (ninf == 0) {
        cage_body_lines_free(infl, ninf);
        infl = cage_tailstream_pending(&t->activity, &ninf);
    }
    size_t nrows;
    char **rows = render_rows(infl, ninf, width, &nrows);
    cage_body_lines_free(infl, ninf);

    int keep = height - (int)nstatus - 1;
    if (keep < 0) keep = 0;
    if (keep > (int)nrows) keep = (int)nrows;
    size_t body_start = nrows - (size_t)keep;

    int ed_max = height - (int)nstatus - keep;
    if (ed_max < 1) ed_max = 1;
    cage_editor_view ev = cage_editor_render(t->editor, width, ed_max);

    size_t total = nstatus + (size_t)keep + ev.nrows;
    char **lines = xmalloc((total ? total : 1) * sizeof *lines);
    size_t k = 0;
    for (size_t i = 0; i < (size_t)keep; i++) lines[k++] = rows[body_start + i];
    for (size_t i = 0; i < nstatus; i++) lines[k++] = status_rows[i];
    for (size_t i = 0; i < ev.nrows; i++) lines[k++] = ev.rows[i];

    int cursor_row = (int)nstatus + keep + ev.cursor_row;
    if (cursor_row >= (int)total) cursor_row = (int)total - 1;

    // Every row is painted the same way; the live region adds no attributes.
    for (size_t i = 0; i < total; i++) {
        tui_buf painted;
        buf_init(&painted);
        append_painted(&painted, lines[i]);
        free(lines[i]);
        lines[i] = buf_steal(&painted);
        buf_free(&painted);
    }

    char *scroll = t->scroll.data ? buf_steal(&t->scroll) : xstrdup("");
    buf_init(&t->scroll);
    bool reanchor = scroll[0] != '\0' && t->live_rows >= t->height - 1;
    tui_buf above;
    buf_init(&above);
    buf_adds(&above, scroll);
    if (reanchor) buf_adds(&above, "\r\n");

    tui_buf frame;
    buf_init(&frame);
    live_draw(t, &frame, lines, total, cursor_row, ev.cursor_col, above.data ? above.data : "");
    write_all(t->out_fd, frame.data, frame.len);
    buf_free(&frame);
    buf_free(&above);
    free(scroll);

    free_rows(lines, total);
    // The body rows past the kept tail, the status array, and the editor rows
    // were all moved into `lines` and freed with it.
    for (size_t i = 0; i < body_start; i++) free(rows[i]);
    free(rows);
    free(status_rows);
    free(ev.rows);
}

// ---- events ----------------------------------------------------------------

static void activity_reset(tui *t) {
    cage_tailstream_reset(&t->activity);
}

static void end_turn(tui *t, const char *line) {
    t->phase = CAGE_PHASE_IDLE;
    free(t->detail);
    t->detail = NULL;
    free(t->writing_tool);
    t->writing_tool = NULL;
    t->paused = false;
    activity_reset(t);
    size_t n;
    cage_body_line *lines = cage_mdstream_flush(&t->reply, &n);
    commit_lines(t, lines, n);
    cage_body_lines_free(lines, n);
    flush_calls(t);
    t->separator = true;
    push(t, line);
}

// Extracts concatenated assistant text and total tokens from a message JSON.
static void message_text_usage(const char *json, char **text_out, long *total_out) {
    *text_out = NULL;
    *total_out = -1;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    tui_buf b;
    buf_init(&b);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (yyjson_is_arr(content)) {
        size_t idx, max;
        yyjson_val *block;
        yyjson_arr_foreach(content, idx, max, block) {
            yyjson_val *type = yyjson_obj_get(block, "type");
            if (yyjson_is_str(type) && strcmp(yyjson_get_str(type), "text") == 0) {
                yyjson_val *txt = yyjson_obj_get(block, "text");
                if (yyjson_is_str(txt)) buf_adds(&b, yyjson_get_str(txt));
            }
        }
    }
    yyjson_val *usage = yyjson_obj_get(root, "usage");
    if (yyjson_is_obj(usage)) {
        yyjson_val *total = yyjson_obj_get(usage, "total");
        if (yyjson_is_int(total)) *total_out = (long)yyjson_get_int(total);
    }
    yyjson_doc_free(doc);
    *text_out = b.data ? buf_steal(&b) : xstrdup("");
    buf_free(&b);
}

static void commit_message(tui *t, const char *json) {
    activity_reset(t);
    size_t n;
    cage_body_line *lines = cage_mdstream_flush(&t->reply, &n);
    commit_lines(t, lines, n);
    cage_body_lines_free(lines, n);

    char *text;
    long total;
    message_text_usage(json, &text, &total);
    if (total >= 0) t->ctx_used = total;

    // trimEnd for the contains check
    size_t tl = strlen(text);
    while (tl > 0 && (text[tl - 1] == '\n' || text[tl - 1] == '\r')) text[--tl] = '\0';
    size_t nt = 0;
    while (text[nt] == ' ' || text[nt] == '\t' || text[nt] == '\n') nt++;
    if (text[nt] != '\0' && !strstr(t->streamed, text)) {
        size_t fn;
        cage_body_line *fed = cage_mdstream_feed(&t->reply, text, &fn);
        commit_lines(t, fed, fn);
        cage_body_lines_free(fed, fn);
        size_t mn;
        cage_body_line *more = cage_mdstream_flush(&t->reply, &mn);
        commit_lines(t, more, mn);
        cage_body_lines_free(more, mn);
    }
    free(text);
    if (t->streamed) t->streamed[0] = '\0';
}

static void on_event(const cage_event *e, void *user) {
    tui *t = user;
    if (t->closed) return;
    pthread_mutex_lock(&t->lock);
    switch (e->type) {
    case CAGE_EVENT_PHASE:
        t->phase = e->u.phase.phase;
        free(t->detail);
        t->detail = e->u.phase.detail ? xstrdup(e->u.phase.detail) : NULL;
        if (e->u.phase.phase == CAGE_PHASE_PAUSING) t->paused = true;
        break;
    case CAGE_EVENT_TEXT: {
        activity_reset(t);
        if (t->streamed) {
            size_t a = strlen(t->streamed), b = strlen(e->u.text.delta);
            t->streamed = realloc(t->streamed, a + b + 1);
            if (!t->streamed) abort();
            memcpy(t->streamed + a, e->u.text.delta, b + 1);
        }
        size_t n;
        cage_body_line *lines = cage_mdstream_feed(&t->reply, e->u.text.delta, &n);
        commit_lines(t, lines, n);
        cage_body_lines_free(lines, n);
        break;
    }
    case CAGE_EVENT_REASONING: {
        size_t np;
        cage_body_line *p = cage_mdstream_pending(&t->reply, &np);
        bool empty = (np == 0);
        cage_body_lines_free(p, np);
        if (empty) {
            size_t z;
            cage_tailstream_feed(&t->activity, e->u.reasoning.delta, &z);
        }
        break;
    }
    case CAGE_EVENT_TOOL_CALL: {
        free(t->writing_tool);
        t->writing_tool = NULL;
        size_t n;
        cage_body_line *lines = cage_mdstream_flush(&t->reply, &n);
        commit_lines(t, lines, n);
        cage_body_lines_free(lines, n);
        activity_reset(t);
        tui_buf b;
        buf_init(&b);
        tui_buf head;
        buf_init(&head);
        char name[128];
        snprintf(name, sizeof name, "-> %s", e->u.tool_call.name);
        fg_teal(&head, name);
        buf_adds(&b, head.data ? head.data : "");
        buf_free(&head);
        buf_adds(&b, "  ");
        char *summary = call_summary(e->u.tool_call.name, e->u.tool_call.arguments);
        buf_adds(&b, summary);
        free(summary);
        bl_push(&t->pending_calls, b.data ? buf_steal(&b) : xstrdup(""), NULL, NULL);
        buf_free(&b);
        break;
    }
    case CAGE_EVENT_TOOL_OUTPUT:
        // Tool output is not streamed live; the result carries it.
        break;
    case CAGE_EVENT_TOOL_RESULT:
        activity_reset(t);
        commit_tool_result(t, e->u.tool_result.name, e->u.tool_result.text, e->u.tool_result.is_error,
                           e->u.tool_result.diffs, e->u.tool_result.diffs_len);
        break;
    case CAGE_EVENT_MESSAGE:
        commit_message(t, e->u.message.json);
        break;
    case CAGE_EVENT_ERROR: {
        tui_buf b;
        buf_init(&b);
        tui_buf p;
        buf_init(&p);
        buf_adds(&p, "! ");
        buf_adds(&p, e->u.error.message);
        fg_red(&b, p.data ? p.data : "");
        buf_free(&p);
        char *line = b.data ? buf_steal(&b) : xstrdup("");
        buf_free(&b);
        end_turn(t, line);
        free(line);
        break;
    }
    case CAGE_EVENT_NO_MODEL:
        end_turn(t, "\x1b[38;2;247;118;142m! no model configured\x1b[39m");
        break;
    case CAGE_EVENT_CANCELLED: {
        tui_buf b;
        buf_init(&b);
        fg_red(&b, "! cancelled");
        char *line = b.data ? buf_steal(&b) : xstrdup("");
        buf_free(&b);
        end_turn(t, line);
        free(line);
        break;
    }
    case CAGE_EVENT_COMPLETE: {
        tui_buf b;
        buf_init(&b);
        char text[64];
        snprintf(text, sizeof text, "[complete · %ds]", (int)elapsed(t));
        fg_dim(&b, text);
        char *line = b.data ? buf_steal(&b) : xstrdup("");
        buf_free(&b);
        end_turn(t, line);
        free(line);
        break;
    }
    }
    tui_render_locked(t);
    pthread_mutex_unlock(&t->lock);
}

// ---- interruption callbacks ------------------------------------------------

static bool is_pause_requested(void *user) {
    tui *t = user;
    return t->pause_requested;
}

static void clear_pause(void *user) {
    tui *t = user;
    t->pause_requested = false;
}

static char *request_steering(void *user) {
    tui *t = user;
    pthread_mutex_lock(&t->lock);
    t->paused = true;
    t->steering_waiting = true;
    if (t->steering_text) {
        free(t->steering_text);
        t->steering_text = NULL;
    }
    tui_render_locked(t);
    while (t->steering_waiting && !t->cancel && !t->closed) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&t->cv, &t->lock, &ts);
    }
    char *s = t->steering_text;
    t->steering_text = NULL;
    t->steering_waiting = false;
    t->paused = false;
    tui_render_locked(t);
    pthread_mutex_unlock(&t->lock);
    return s ? s : xstrdup("");
}

// ---- command execution -----------------------------------------------------

// Command callbacks run under the TUI lock, from `submit`.
static void command_write(void *user, char *line) {
    tui *t = user;
    t->separator = true;
    cage_body_line bl = {line, NULL, NULL};
    size_t nrows;
    char **rows = render_rows(&bl, 1, t->width > 0 ? t->width : 1, &nrows);
    for (size_t i = 0; i < nrows; i++) push(t, rows[i]);
    free_rows(rows, nrows);
    t->separator = true;
}

static void command_new_session(void *user) {
    tui *t = user;
    if (t->session) cage_session_close(t->session);
    char title[256];
    const char *base = strrchr(t->c->cwd, '/');
    base = base ? base + 1 : t->c->cwd;
    snprintf(title, sizeof title, "%s", base[0] ? base : "session");
    char *err = NULL;
    t->session = cage_session_open(t->c->cfg->sessions_dir, t->c->cwd, title, &err);
    free(err);
    t->agent.session = t->session;
    for (size_t i = 0; i < t->agent.messages_len; i++) cage_message_free(&t->agent.messages[i]);
    free(t->agent.messages);
    t->agent.messages = NULL;
    t->agent.messages_len = 0;
    t->ctx_used = 0;
}

// The one-line model banner: "cage · provider/model · effort".
static char *model_line(tui *t) {
    tui_buf b;
    buf_init(&b);
    buf_adds(&b, "cage · ");
    buf_adds(&b, t->model.provider ? t->model.provider : "no provider");
    buf_addc(&b, '/');
    buf_adds(&b, t->model.model ? t->model.model : "no model");
    if (t->model.effort) {
        buf_adds(&b, " · ");
        buf_adds(&b, t->model.effort);
    }
    return b.data ? buf_steal(&b) : xstrdup("");
}

static long window_for(const char *provider, const char *model) {
    if (!provider || !model) return 0;
    const cage_model_info *info = cage_catalog_lookup(cage_provider_canonical(provider), model);
    return info ? info->context_limit : 0;
}

// Switches the running session to a provider/model/effort, persists the change,
// and repoints the agent's model. A NULL argument leaves that field unchanged.
static int command_apply(void *user, const char *provider, const char *model,
                         const char *effort, char **err) {
    if (err) *err = NULL;
    tui *t = user;
    cage_config *cfg = t->c->cfg;

    const char *new_provider = provider ? provider : cfg->provider;
    const char *new_model = model ? model : cfg->model;
    const char *new_effort = effort ? effort : cfg->effort;

    // Validate by resolving against a shallow copy before touching anything.
    cage_config probe = *cfg;
    probe.provider = (char *)new_provider;
    probe.model = (char *)new_model;
    probe.effort = (char *)new_effort;
    cage_model resolved;
    if (cage_resolve_model(&probe, &resolved, err) != 0) return -1;

    if (cage_config_save(provider, model, effort, err) != 0) return -1;

    if (provider) {
        free(cfg->provider);
        cfg->provider = xstrdup(provider);
    }
    if (model) {
        free(cfg->model);
        cfg->model = xstrdup(model);
    }
    if (effort) {
        free(cfg->effort);
        cfg->effort = xstrdup(effort);
    }

    if (cage_resolve_model(cfg, &t->model, err) != 0) return -1;
    t->agent.model = t->model;
    t->context_window = window_for(cfg->provider, cfg->model);
    t->agent.tools = cage_tools(t->model.supports_images, &t->agent.tools_len);

    char *line = model_line(t);
    t->separator = true;
    command_write(t, line);
    return 0;
}

// Releases the pending choice and its options.
static void free_choice(tui *t) {
    for (size_t i = 0; i < t->choice_n; i++) {
        free(t->choice_ids[i]);
        free(t->choice_labels[i]);
    }
    free(t->choice_ids);
    free(t->choice_labels);
    free(t->choice_provider);
    t->choice_ids = NULL;
    t->choice_labels = NULL;
    t->choice_provider = NULL;
    t->choice_n = 0;
    t->choice_active = false;
}

// Commits one red line, for errors the choice flow reports.
static void command_error(tui *t, const char *msg) {
    t->separator = true;
    cage_body_line bl = {xstrdup(msg), fg_red, NULL};
    commit_lines(t, &bl, 1);
    free(bl.text);
    t->separator = true;
}

// Presents the options for `what` as a numbered list and arms the choice.
// `provider` overrides the config provider for a model list.
static void start_choose(tui *t, cage_choose what, const char *provider) {
    free_choice(t);
    char **ids = NULL, **labels = NULL;
    size_t n = cage_choose_list(t->c->cfg, what, provider, &ids, &labels);
    if (n == 0) {
        const char *msg = what == CAGE_CHOOSE_MODEL ? "! select a provider first"
                          : what == CAGE_CHOOSE_EFFORT ? "! no configurable effort for this model"
                                                       : "! no providers configured";
        command_error(t, msg);
        return;
    }
    t->choice_active = true;
    t->choice_kind = what;
    t->choice_provider = provider ? xstrdup(provider) : NULL;
    t->choice_ids = ids;
    t->choice_labels = labels;
    t->choice_n = n;

    // The heading and options are one block, so no blank line separates them.
    char *title = cage_choose_title(t->c->cfg, what, provider);
    size_t total = n + 1;
    cage_body_line *lines = xmalloc(total * sizeof *lines);
    lines[0] = (cage_body_line){title, NULL, NULL};
    for (size_t i = 0; i < n; i++) {
        tui_buf b;
        buf_init(&b);
        buf_addf(&b, "  %zu. ", i + 1);
        buf_adds(&b, labels[i]);
        lines[i + 1] = (cage_body_line){b.data ? buf_steal(&b) : xstrdup(""), NULL, NULL};
        buf_free(&b);
    }
    t->separator = true;
    commit_lines(t, lines, total);
    for (size_t i = 0; i < total; i++) free(lines[i].text);
    free(lines);
    t->separator = true;
}

// Turns the submitted line into the pending choice: a 1-based index, an id, or
// a label. An invalid line reports and clears the choice.
static void resolve_choice(tui *t, const char *text) {
    size_t n = t->choice_n;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n' || p[len - 1] == '\r'))
        len--;
    char *word = xmalloc(len + 1);
    memcpy(word, p, len);
    word[len] = '\0';

    const char *chosen = NULL;
    char *end = NULL;
    long idx = strtol(word, &end, 10);
    if (word[0] != '\0' && *end == '\0' && idx >= 1 && (size_t)idx <= n)
        chosen = t->choice_ids[idx - 1];
    if (!chosen) {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(word, t->choice_ids[i]) == 0 || strcmp(word, t->choice_labels[i]) == 0) {
                chosen = t->choice_ids[i];
                break;
            }
        }
    }
    if (!chosen) {
        size_t ml = strlen(word) + 32;
        char *msg = xmalloc(ml);
        snprintf(msg, ml, "! invalid selection: %s", word);
        command_error(t, msg);
        free(msg);
        free(word);
        free_choice(t);
        return;
    }

    cage_choose kind = t->choice_kind;
    char *id = xstrdup(chosen);
    char *provider = t->choice_provider ? xstrdup(t->choice_provider) : NULL;
    free(word);
    free_choice(t);

    if (kind == CAGE_CHOOSE_PROVIDER) {
        start_choose(t, CAGE_CHOOSE_MODEL, id);
        free(id);
        return;
    }

    char *err = NULL;
    int rc = kind == CAGE_CHOOSE_MODEL
        ? command_apply(t, provider, id, NULL, &err)
        : command_apply(t, NULL, NULL, id, &err);
    if (rc != 0) {
        const char *detail = err ? err : "cannot apply the selection";
        size_t ml = strlen(detail) + 4;
        char *msg = xmalloc(ml);
        snprintf(msg, ml, "! %s", detail);
        command_error(t, msg);
        free(msg);
        free(err);
    }
    free(provider);
    free(id);
}

static void command_choose(void *user, cage_choose what) {
    start_choose(user, what, NULL);
}

static void run_command(tui *t, const cage_command *cmd, const char *args) {
    cage_command_ctx ctx = {
        .user = t,
        .write = command_write,
        .new_session = command_new_session,
        .choose = command_choose,
        .cfg = t->c->cfg,
        .apply = command_apply,
    };
    cmd->run(&ctx, args);
}

// ---- turns -----------------------------------------------------------------

typedef struct {
    tui *t;
    char *text;
} turn_arg;

static void *worker_main(void *arg) {
    turn_arg *a = arg;
    tui *t = a->t;
    char *text = a->text;
    free(a);
    cage_agent_turn(&t->agent, text);
    free(text);
    pthread_mutex_lock(&t->lock);
    t->active = false;
    t->paused = false;
    t->pause_requested = false;
    free(t->streamed);
    t->streamed = NULL;
    tui_render_locked(t);
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

// The user message is appended to the transcript and session by the agent turn,
// which runs on `worker`. commit_user already displayed it.
static void start_turn(tui *t, const char *text) {
    t->active = true;
    t->cancel = false;
    t->pause_requested = false;
    t->paused = false;
    t->phase = CAGE_PHASE_PREPARING;
    free(t->detail);
    t->detail = NULL;
    t->frame = 0;
    clock_gettime(CLOCK_MONOTONIC, &t->turn_start);
    free(t->streamed);
    t->streamed = xstrdup("");

    turn_arg *a = xmalloc(sizeof *a);
    a->t = t;
    a->text = xstrdup(text);
    if (pthread_create(&t->worker, NULL, worker_main, a) != 0) {
        t->active = false;
        free(a->text);
        free(a);
    } else {
        t->worker_live = true;
    }
    tui_render_locked(t);
}

// ---- input -----------------------------------------------------------------

static void submit(tui *t) {
    const char *raw = cage_editor_text(t->editor);
    char *text = xstrdup(raw);
    if (t->active) {
        if (t->paused && t->steering_waiting) {
            free(t->steering_text);
            t->steering_text = xstrdup(text);
            t->steering_waiting = false;
            pthread_cond_broadcast(&t->cv);
            cage_editor_clear(t->editor);
            size_t len = strlen(text);
            size_t s = 0;
            while (s < len && (text[s] == ' ' || text[s] == '\t' || text[s] == '\n')) s++;
            if (text[s] != '\0') commit_user(t, text);
            free(text);
            tui_render_locked(t);
            return;
        }
        free(text);
        return;
    }
    if (t->choice_active) {
        cage_editor_clear(t->editor);
        resolve_choice(t, text);
        free(text);
        tui_render_locked(t);
        return;
    }
    size_t len = strlen(text);
    size_t s = 0;
    while (s < len && (text[s] == ' ' || text[s] == '\t' || text[s] == '\n')) s++;
    if (text[s] == '\0') {
        free(text);
        return;
    }
    const char *args = NULL;
    const cage_command *cmd = cage_find_command(text, &args);
    if (cmd) {
        cage_editor_clear(t->editor);
        char *argcopy = xstrdup(args);
        run_command(t, cmd, argcopy);
        free(argcopy);
        free(text);
        tui_render_locked(t);
        return;
    }
    cage_editor_clear(t->editor);
    commit_user(t, text);
    start_turn(t, text);
    free(text);
}

// Tab completion for a path needs the cwd; a wrapper over the editor.
typedef struct {
    const char *cwd;
} complete_ctx;

static char *complete_step(const char *word, void *user) {
    complete_ctx *c = user;
    return cage_complete_path(word, c->cwd);
}

static void handle_tab(tui *t) {
    const char *draft = cage_editor_text(t->editor);
    char *completed = cage_complete_command(draft);
    if (completed) {
        cage_editor_set_text(t->editor, completed);
        free(completed);
        return;
    }
    complete_ctx cc = {t->c->cwd};
    cage_editor_complete_word(t->editor, complete_step, &cc);
}

static void handle_key(tui *t, const cage_key *key) {
    pthread_mutex_lock(&t->lock);
    switch (key->type) {
    case CAGE_KEY_EOF:
        if (!t->active && cage_editor_text(t->editor)[0] == '\0') t->closed = true;
        break;
    case CAGE_KEY_INTERRUPT:
        if (t->active) {
            t->cancel = true;
            pthread_cond_broadcast(&t->cv);
        } else if (t->choice_active) {
            free_choice(t);
            command_error(t, "! cancelled");
        } else if (cage_editor_text(t->editor)[0] != '\0') {
            cage_editor_clear(t->editor);
        }
        break;
    case CAGE_KEY_ESCAPE:
        if (t->active && !t->paused) t->pause_requested = true;
        break;
    case CAGE_KEY_TAB:
        handle_tab(t);
        break;
    default: {
        cage_edit_result r = cage_editor_handle(t->editor, key);
        if (r == CAGE_EDIT_SUBMIT) submit(t);
        break;
    }
    }
    tui_render_locked(t);
    pthread_mutex_unlock(&t->lock);
}

static void handle_key_cb(const cage_key *key, void *user) {
    handle_key(user, key);
}

// ---- lifecycle -------------------------------------------------------------

static void get_size(tui *t) {
    struct winsize ws;
    if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        t->width = ws.ws_col;
        t->height = ws.ws_row > 0 ? ws.ws_row : 24;
    } else {
        t->width = 80;
        t->height = 24;
    }
}

static void terminal_raw(tui *t) {
    if (tcgetattr(t->in_fd, &t->orig) != 0) return;
    struct termios raw = t->orig;
    cfmakeraw(&raw);
    if (tcsetattr(t->in_fd, TCSAFLUSH, &raw) == 0) t->raw = true;
}

static void terminal_restore(tui *t) {
    if (t->raw) tcsetattr(t->in_fd, TCSAFLUSH, &t->orig);
}

// Pushes one styled line into scrollback.
static void push_line(tui *t, char *text) {
    t->separator = true;
    cage_body_line bl = {text, NULL, NULL};
    size_t nrows;
    char **rows = render_rows(&bl, 1, t->width > 0 ? t->width : 80, &nrows);
    for (size_t i = 0; i < nrows; i++) push(t, rows[i]);
    free_rows(rows, nrows);
    free(bl.text);
    t->separator = true;
}

static void push_banner(tui *t) {
    push_line(t, model_line(t));
    const cage_resources *r = t->c->resources;
    if (!r) return;
    for (size_t i = 0; i < r->agent_files_len; i++) {
        tui_buf b;
        buf_init(&b);
        buf_adds(&b, "agents · ");
        buf_adds(&b, r->agent_files[i].path);
        push_line(t, b.data ? buf_steal(&b) : xstrdup(""));
        buf_free(&b);
    }
    if (r->skills_len > 0) {
        tui_buf b;
        buf_init(&b);
        buf_adds(&b, "skills · ");
        for (size_t i = 0; i < r->skills_len; i++) {
            if (i > 0) buf_adds(&b, ", ");
            buf_adds(&b, r->skills[i].name);
        }
        push_line(t, b.data ? buf_steal(&b) : xstrdup(""));
        buf_free(&b);
    }
}

int cage_tui_run(const cage_tui_config *c) {
    tui t;
    memset(&t, 0, sizeof t);
    t.c = c;
    t.in_fd = STDIN_FILENO;
    t.out_fd = STDOUT_FILENO;
    t.editor = cage_editor_new();
    cage_mdstream_init(&t.reply);
    cage_tailstream_init(&t.activity);
    buf_init(&t.scroll);
    pthread_mutex_init(&t.lock, NULL);
    pthread_cond_init(&t.cv, NULL);
    t.parser = cage_key_parser_new();

    cage_theme_use(c->cfg->theme);

    t.model = c->model;
    t.context_window = window_for(c->model.provider, c->model.model);

    get_size(&t);

    char title[256];
    const char *base = strrchr(c->cwd, '/');
    base = base ? base + 1 : c->cwd;
    snprintf(title, sizeof title, "%s", base[0] ? base : "session");
    char *err = NULL;
    t.session = cage_session_open(c->cfg->sessions_dir, c->cwd, title, &err);
    if (!t.session) {
        fprintf(stderr, "%s\n", err ? err : "session error");
        free(err);
        cage_editor_free(t.editor);
        return 1;
    }

    size_t tools_len;
    const cage_tool *tools = cage_tools(t.model.supports_images, &tools_len);

    t.agent = (cage_agent){
        .model = t.model,
        .system_prompt = c->resources ? c->resources->system_prompt : "",
        .tools = tools,
        .tools_len = tools_len,
        .session = t.session,
        .on_event = on_event,
        .user = &t,
        .is_pause_requested = is_pause_requested,
        .clear_pause = clear_pause,
        .request_steering = request_steering,
        .cancel = &t.cancel,
    };

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    terminal_raw(&t);
    if (isatty(t.out_fd)) {
        const char *enable = "\x1b[?2004h\x1b[>1u\x1b[?u";
        write_all(t.out_fd, enable, strlen(enable));
    }

    push_banner(&t);
    tui_render_locked(&t);

    while (!t.closed && !g_exit) {
        int timeout = SPINNER_MS;
        double now = monotonic_now();
        if (t.esc_pending) {
            int ms = (int)((t.esc_deadline - now) * 1000.0);
            if (ms < timeout) timeout = ms;
        }
        if (t.seq_pending) {
            int ms = (int)((t.seq_deadline - now) * 1000.0);
            if (ms < timeout) timeout = ms;
        }
        if (timeout < 0) timeout = 0;

        struct pollfd pfd = {.fd = t.in_fd, .events = POLLIN};
        int rc = poll(&pfd, 1, timeout);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t n = read(t.in_fd, buf, sizeof buf);
            if (n > 0) {
                // New data supersedes any held ESC or sequence.
                t.esc_pending = false;
                t.seq_pending = false;
                cage_key_parser_feed(t.parser, buf, (size_t)n, handle_key_cb, &t);
                int pend = cage_key_parser_pending(t.parser);
                if (pend == 1) {
                    t.esc_pending = true;
                    t.esc_deadline = monotonic_now() + 0.030;
                } else if (pend == 2) {
                    t.seq_pending = true;
                    t.seq_deadline = monotonic_now() + 0.150;
                }
            } else if (n == 0) {
                t.closed = true;
            }
        }
        now = monotonic_now();
        if (t.esc_pending && now >= t.esc_deadline) {
            t.esc_pending = false;
            cage_key_parser_flush_escape(t.parser, handle_key_cb, &t);
        }
        if (t.seq_pending && now >= t.seq_deadline) {
            t.seq_pending = false;
            cage_key_parser_flush_sequence(t.parser);
        }
        pthread_mutex_lock(&t.lock);
        if (g_resize) {
            g_resize = 0;
            get_size(&t);
            tui_render_locked(&t);
        } else if (t.active) {
            t.frame++;
            tui_render_locked(&t);
        }
        pthread_mutex_unlock(&t.lock);
    }

    pthread_mutex_lock(&t.lock);
    t.closed = true;
    t.cancel = true;
    pthread_cond_broadcast(&t.cv);
    pthread_mutex_unlock(&t.lock);
    if (t.worker_live) pthread_join(t.worker, NULL);

    // Flush the live region and any pending scrollback.
    pthread_mutex_lock(&t.lock);
    tui_buf out;
    buf_init(&out);
    live_clear(&t, &out);
    if (t.scroll.data) buf_adds(&out, t.scroll.data);
    write_all(t.out_fd, out.data, out.len);
    buf_free(&out);
    buf_init(&t.scroll);
    pthread_mutex_unlock(&t.lock);

    if (isatty(t.out_fd)) {
        const char *disable = "\x1b[<u\x1b[?2004l";
        write_all(t.out_fd, disable, strlen(disable));
    }
    terminal_restore(&t);

    if (t.session) cage_session_close(t.session);
    cage_agent_free(&t.agent);
    cage_editor_free(t.editor);
    cage_mdstream_free(&t.reply);
    cage_tailstream_free(&t.activity);
    for (size_t i = 0; i < t.pending_calls.n; i++) free(t.pending_calls.v[i].text);
    free(t.pending_calls.v);
    free_choice(&t);
    free(t.detail);
    free(t.writing_tool);
    free(t.streamed);
    free(t.steering_text);
    buf_free(&t.scroll);
    cage_key_parser_free(t.parser);
    pthread_mutex_destroy(&t.lock);
    pthread_cond_destroy(&t.cv);
    return 0;
}
