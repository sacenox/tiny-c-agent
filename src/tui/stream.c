#include "stream.h"

#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "highlight.h"

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

static void strv_push(char ***v, size_t *n, size_t *cap, char *s) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = realloc(*v, *cap * sizeof **v);
        if (!*v) abort();
    }
    (*v)[(*n)++] = s;
}

static void strv_clear(char **v, size_t *n) {
    for (size_t i = 0; i < *n; i++) free(v[i]);
    *n = 0;
}

static void strv_free(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

// Splits `s` on '\n'; the trailing empty part is included, like JS split.
static char **split_lines(const char *s, size_t *count) {
    size_t cap = 8, n = 0;
    char **out = xmalloc(cap * sizeof *out);
    const char *p = s;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        strv_push(&out, &n, &cap, line);
        if (!nl) break;
        p = nl + 1;
    }
    *count = n;
    return out;
}

// ---- body line lists -------------------------------------------------------

typedef struct {
    cage_body_line *v;
    size_t n;
    size_t cap;
} bodylist;

static void body_push(bodylist *l, char *text, cage_fg_fn style, const char *bg) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
        if (!l->v) abort();
    }
    l->v[l->n++] = (cage_body_line){text, style, bg};
}

static cage_body_line *body_take(bodylist *l, size_t *count) {
    *count = l->n;
    if (l->n == 0) {
        free(l->v);
        return NULL;
    }
    return l->v;
}

void cage_body_lines_free(cage_body_line *lines, size_t count) {
    for (size_t i = 0; i < count; i++) free(lines[i].text);
    free(lines);
}

// ---- fence parsing ---------------------------------------------------------

// `^ {0,3}(`{3,}|~{3,})\s*(\S*)`: returns the marker and info when it matches.
static bool fence_open(const char *line, char **marker, char **info) {
    size_t i = 0;
    while (i < 3 && line[i] == ' ') i++;
    char c = line[i];
    if (c != '`' && c != '~') return false;
    size_t start = i;
    while (line[i] == c) i++;
    if (i - start < 3) return false;
    char *m = xmalloc(i - start + 1);
    memcpy(m, line + start, i - start);
    m[i - start] = '\0';
    while (line[i] == ' ' || line[i] == '\t') i++;
    size_t s = i;
    while (line[i] && line[i] != ' ' && line[i] != '\t') i++;
    char *inf = xmalloc(i - s + 1);
    memcpy(inf, line + s, i - s);
    inf[i - s] = '\0';
    *marker = m;
    *info = inf;
    return true;
}

// `^ {0,3}(`{3,}|~{3,})\s*$`: the marker run, or NULL.
static char *fence_close(const char *line) {
    size_t i = 0;
    while (i < 3 && line[i] == ' ') i++;
    char c = line[i];
    if (c != '`' && c != '~') return NULL;
    size_t start = i;
    while (line[i] == c) i++;
    if (i - start < 3) return NULL;
    size_t j = i;
    while (line[j] == ' ' || line[j] == '\t') j++;
    if (line[j] != '\0') return NULL;
    char *m = xmalloc(i - start + 1);
    memcpy(m, line + start, i - start);
    m[i - start] = '\0';
    return m;
}

// ---- markdown stream -------------------------------------------------------

void cage_mdstream_init(cage_mdstream *s) {
    memset(s, 0, sizeof *s);
    s->rest = xstrdup("");
}

void cage_mdstream_free(cage_mdstream *s) {
    free(s->rest);
    free(s->fence_marker);
    free(s->fence_info);
    strv_clear(s->fence_lines, &s->fence_n);
    free(s->fence_lines);
    strv_clear(s->prose, &s->prose_n);
    free(s->prose);
    memset(s, 0, sizeof *s);
}

void cage_mdstream_reset(cage_mdstream *s) {
    free(s->rest);
    s->rest = xstrdup("");
    free(s->fence_marker);
    s->fence_marker = NULL;
    free(s->fence_info);
    s->fence_info = NULL;
    strv_clear(s->fence_lines, &s->fence_n);
    strv_clear(s->prose, &s->prose_n);
}

// Emits the held prose as one highlighted block.
static void release_prose(cage_mdstream *s, const char *tail, bodylist *out) {
    char **prose = s->prose;
    size_t n = s->prose_n;
    s->prose = NULL;
    s->prose_n = 0;
    s->prose_cap = 0;

    tui_buf joined;
    buf_init(&joined);
    for (size_t i = 0; i < n; i++) {
        if (i) buf_addc(&joined, '\n');
        buf_adds(&joined, prose[i]);
    }
    if (tail[0]) {
        if (n) buf_addc(&joined, '\n');
        buf_adds(&joined, tail);
    }
    strv_free(prose, n);
    if (joined.len == 0) {
        buf_free(&joined);
        return;
    }
    // A block is only a block once its last line ends, so the text needs a
    // terminating newline; the closing reset belongs to the last line.
    buf_addc(&joined, '\n');
    char *colored = cage_highlight_markdown(joined.data);
    buf_free(&joined);
    size_t nlines;
    char **lines = split_lines(colored, &nlines);
    // The split adds a trailing line holding the closing reset; attach it.
    if (nlines > 1) {
        char *reset = lines[nlines - 1];
        tui_buf last;
        buf_init(&last);
        buf_adds(&last, lines[nlines - 2]);
        buf_adds(&last, reset);
        free(lines[nlines - 2]);
        free(reset);
        lines[nlines - 2] = buf_steal(&last);
        buf_free(&last);
        nlines--;
    }
    for (size_t i = 0; i < nlines; i++) body_push(out, lines[i], NULL, NULL);
    free(lines);
    free(colored);
}

static void release_fence(cage_mdstream *s, const char *closing, bodylist *out) {
    // The opening fence line, colored as markdown.
    char *first = s->fence_lines[0];
    char *first_col = cage_highlight_markdown(first);
    body_push(out, first_col, NULL, NULL);
    size_t n = s->fence_n;
    if (n > 1) {
        tui_buf body;
        buf_init(&body);
        for (size_t i = 1; i < n; i++) {
            if (i > 1) buf_addc(&body, '\n');
            buf_adds(&body, s->fence_lines[i]);
        }
        char *colored = cage_highlight_code(s->fence_info, body.data);
        buf_free(&body);
        size_t nlines;
        char **lines = split_lines(colored, &nlines);
        for (size_t i = 0; i < nlines; i++) body_push(out, lines[i], NULL, NULL);
        free(lines);
        free(colored);
    }
    for (size_t i = 0; i < n; i++) free(s->fence_lines[i]);
    s->fence_n = 0;
    free(s->fence_marker);
    s->fence_marker = NULL;
    free(s->fence_info);
    s->fence_info = NULL;
    if (closing) {
        char *cc = cage_highlight_markdown(closing);
        body_push(out, cc, NULL, NULL);
    }
}

static void commit_line(cage_mdstream *s, const char *line, bodylist *out) {
    if (s->fence_marker) {
        char *close = fence_close(line);
        if (close && close[0] == s->fence_marker[0] &&
            strlen(close) >= strlen(s->fence_marker)) {
            release_fence(s, line, out);
            free(close);
            return;
        }
        free(close);
        strv_push(&s->fence_lines, &s->fence_n, &s->fence_cap, xstrdup(line));
        return;
    }
    char *marker = NULL, *info = NULL;
    if (fence_open(line, &marker, &info)) {
        s->fence_marker = marker;
        s->fence_info = info;
        strv_push(&s->fence_lines, &s->fence_n, &s->fence_cap, xstrdup(line));
        release_prose(s, "", out);
        return;
    }
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    if (*t == '\0') {
        release_prose(s, "", out);
        body_push(out, xstrdup(line), NULL, NULL);
        return;
    }
    strv_push(&s->prose, &s->prose_n, &s->prose_cap, xstrdup(line));
}

cage_body_line *cage_mdstream_feed(cage_mdstream *s, const char *delta, size_t *count) {
    bodylist out = {0};
    size_t rest_len = strlen(s->rest);
    size_t dl = strlen(delta);
    char *joined = xmalloc(rest_len + dl + 1);
    memcpy(joined, s->rest, rest_len);
    memcpy(joined + rest_len, delta, dl + 1);
    free(s->rest);

    size_t nlines;
    char **lines = split_lines(joined, &nlines);
    // The last split part is the still-incomplete tail.
    s->rest = lines[nlines - 1];
    for (size_t i = 0; i + 1 < nlines; i++) {
        commit_line(s, lines[i], &out);
        free(lines[i]);
    }
    free(lines);
    free(joined);
    return body_take(&out, count);
}

cage_body_line *cage_mdstream_pending(cage_mdstream *s, size_t *count) {
    bodylist out = {0};
    char **held = s->fence_marker ? s->fence_lines : s->prose;
    size_t n = s->fence_marker ? s->fence_n : s->prose_n;
    for (size_t i = 0; i < n; i++) body_push(&out, xstrdup(held[i]), NULL, NULL);
    if (s->rest[0]) body_push(&out, xstrdup(s->rest), NULL, NULL);
    return body_take(&out, count);
}

cage_body_line *cage_mdstream_flush(cage_mdstream *s, size_t *count) {
    bodylist out = {0};
    if (!s->fence_marker) {
        release_prose(s, s->rest, &out);
    } else {
        release_fence(s, NULL, &out);
        if (s->rest[0]) {
            char *c = cage_highlight_markdown(s->rest);
            // Split the possibly multi-line remainder.
            size_t nlines;
            char **lines = split_lines(c, &nlines);
            for (size_t i = 0; i < nlines; i++) body_push(&out, lines[i], NULL, NULL);
            free(lines);
            free(c);
        }
    }
    cage_mdstream_reset(s);
    return body_take(&out, count);
}

// ---- tail stream -----------------------------------------------------------

void cage_tailstream_init(cage_tailstream *s) {
    s->rest = xstrdup("");
}

void cage_tailstream_free(cage_tailstream *s) {
    free(s->rest);
    s->rest = NULL;
}

void cage_tailstream_reset(cage_tailstream *s) {
    free(s->rest);
    s->rest = xstrdup("");
}

cage_body_line *cage_tailstream_feed(cage_tailstream *s, const char *delta, size_t *count) {
    size_t rl = strlen(s->rest), dl = strlen(delta);
    char *text = xmalloc(rl + dl + 1);
    memcpy(text, s->rest, rl);
    memcpy(text + rl, delta, dl + 1);
    free(s->rest);
    const char *nl = strrchr(text, '\n');
    if (nl) {
        s->rest = xstrdup(nl + 1);
    } else {
        s->rest = text;
        text = NULL;
    }
    free(text);
    *count = 0;
    return NULL;
}

cage_body_line *cage_tailstream_pending(cage_tailstream *s, size_t *count) {
    if (s->rest[0] == '\0') {
        *count = 0;
        return NULL;
    }
    cage_body_line *v = xmalloc(sizeof *v);
    v[0] = (cage_body_line){xstrdup(s->rest), fg_dim, NULL};
    *count = 1;
    return v;
}

cage_body_line *cage_tailstream_flush(cage_tailstream *s, size_t *count) {
    cage_tailstream_reset(s);
    *count = 0;
    return NULL;
}
