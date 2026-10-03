#include "stream.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "highlight.h"

// ---- body line lists -------------------------------------------------------

void bl_push(tiny_blist *l, char *text, tiny_fg_fn style, const char *bg) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = (tiny_body_line){text, style, bg};
}

void bl_free(tiny_blist *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].text);
    free(l->v);
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
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

void tiny_mdstream_init(tiny_mdstream *s) {
    memset(s, 0, sizeof *s);
    s->rest = xstrdup("");
}

void tiny_mdstream_free(tiny_mdstream *s) {
    free(s->rest);
    free(s->fence_marker);
    free(s->fence_info);
    strv_free(&s->fence_lines);
    strv_free(&s->prose);
    memset(s, 0, sizeof *s);
}

void tiny_mdstream_reset(tiny_mdstream *s) {
    free(s->rest);
    s->rest = xstrdup("");
    free(s->fence_marker);
    s->fence_marker = NULL;
    free(s->fence_info);
    s->fence_info = NULL;
    strv_free(&s->fence_lines);
    strv_free(&s->prose);
}

// Emits the held prose as one highlighted block.
static void release_prose(tiny_mdstream *s, const char *tail, tiny_blist *out) {
    tiny_strv prose = s->prose;
    memset(&s->prose, 0, sizeof s->prose);

    tui_buf joined;
    buf_init(&joined);
    for (size_t i = 0; i < prose.n; i++) {
        if (i) buf_addc(&joined, '\n');
        buf_adds(&joined, prose.v[i]);
    }
    if (tail[0]) {
        if (prose.n) buf_addc(&joined, '\n');
        buf_adds(&joined, tail);
    }
    strv_free(&prose);
    if (joined.len == 0) {
        buf_free(&joined);
        return;
    }
    // A block is only a block once its last line ends, so the text needs a
    // terminating newline; the closing reset belongs to the last line.
    buf_addc(&joined, '\n');
    char *colored = tiny_highlight_markdown(joined.data);
    buf_free(&joined);
    tiny_strv lines = tiny_split_lines(colored);
    // The split adds a trailing line holding the closing reset; attach it.
    if (lines.n > 1) {
        char *reset = lines.v[lines.n - 1];
        tui_buf last;
        buf_init(&last);
        buf_adds(&last, lines.v[lines.n - 2]);
        buf_adds(&last, reset);
        free(lines.v[lines.n - 2]);
        free(reset);
        lines.v[lines.n - 2] = buf_steal(&last);
        lines.n--;
    }
    for (size_t i = 0; i < lines.n; i++) bl_push(out, lines.v[i], NULL, NULL);
    free(lines.v);
    free(colored);
}

static void release_fence(tiny_mdstream *s, const char *closing, tiny_blist *out) {
    // The opening fence line, colored as markdown.
    char *first = s->fence_lines.v[0];
    char *first_col = tiny_highlight_markdown(first);
    bl_push(out, first_col, NULL, NULL);
    size_t n = s->fence_lines.n;
    if (n > 1) {
        tui_buf body;
        buf_init(&body);
        for (size_t i = 1; i < n; i++) {
            if (i > 1) buf_addc(&body, '\n');
            buf_adds(&body, s->fence_lines.v[i]);
        }
        char *colored = tiny_highlight_code(s->fence_info, body.data);
        buf_free(&body);
        tiny_strv lines = tiny_split_lines(colored);
        for (size_t i = 0; i < lines.n; i++) bl_push(out, lines.v[i], NULL, NULL);
        free(lines.v);
        free(colored);
    }
    strv_free(&s->fence_lines);
    free(s->fence_marker);
    s->fence_marker = NULL;
    free(s->fence_info);
    s->fence_info = NULL;
    if (closing) {
        char *cc = tiny_highlight_markdown(closing);
        bl_push(out, cc, NULL, NULL);
    }
}

static void commit_line(tiny_mdstream *s, const char *line, tiny_blist *out) {
    if (s->fence_marker) {
        char *close = fence_close(line);
        if (close && close[0] == s->fence_marker[0] &&
            strlen(close) >= strlen(s->fence_marker)) {
            release_fence(s, line, out);
            free(close);
            return;
        }
        free(close);
        strv_push(&s->fence_lines, xstrdup(line));
        return;
    }
    char *marker = NULL, *info = NULL;
    if (fence_open(line, &marker, &info)) {
        s->fence_marker = marker;
        s->fence_info = info;
        strv_push(&s->fence_lines, xstrdup(line));
        release_prose(s, "", out);
        return;
    }
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    if (*t == '\0') {
        release_prose(s, "", out);
        bl_push(out, xstrdup(line), NULL, NULL);
        return;
    }
    strv_push(&s->prose, xstrdup(line));
}

tiny_blist tiny_mdstream_feed(tiny_mdstream *s, const char *delta) {
    tiny_blist out = {0};
    size_t rest_len = strlen(s->rest);
    size_t dl = strlen(delta);
    char *joined = xmalloc(rest_len + dl + 1);
    memcpy(joined, s->rest, rest_len);
    memcpy(joined + rest_len, delta, dl + 1);
    free(s->rest);

    tiny_strv lines = tiny_split_lines(joined);
    // The last split part is the still-incomplete tail.
    s->rest = lines.v[lines.n - 1];
    for (size_t i = 0; i + 1 < lines.n; i++) {
        commit_line(s, lines.v[i], &out);
        free(lines.v[i]);
    }
    free(lines.v);
    free(joined);
    return out;
}

tiny_blist tiny_mdstream_pending(tiny_mdstream *s) {
    tiny_blist out = {0};
    tiny_strv *held = s->fence_marker ? &s->fence_lines : &s->prose;
    for (size_t i = 0; i < held->n; i++) bl_push(&out, xstrdup(held->v[i]), NULL, NULL);
    if (s->rest[0]) bl_push(&out, xstrdup(s->rest), NULL, NULL);
    return out;
}

tiny_blist tiny_mdstream_flush(tiny_mdstream *s) {
    tiny_blist out = {0};
    if (!s->fence_marker) {
        release_prose(s, s->rest, &out);
    } else {
        release_fence(s, NULL, &out);
        if (s->rest[0]) {
            char *c = tiny_highlight_markdown(s->rest);
            // Split the possibly multi-line remainder.
            tiny_strv lines = tiny_split_lines(c);
            for (size_t i = 0; i < lines.n; i++) bl_push(&out, lines.v[i], NULL, NULL);
            free(lines.v);
            free(c);
        }
    }
    tiny_mdstream_reset(s);
    return out;
}

// ---- tail stream -----------------------------------------------------------

void tiny_tailstream_init(tiny_tailstream *s) {
    s->rest = xstrdup("");
}

void tiny_tailstream_free(tiny_tailstream *s) {
    free(s->rest);
    s->rest = NULL;
}

void tiny_tailstream_reset(tiny_tailstream *s) {
    free(s->rest);
    s->rest = xstrdup("");
}

tiny_blist tiny_tailstream_feed(tiny_tailstream *s, const char *delta) {
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
    return (tiny_blist){0};
}

tiny_blist tiny_tailstream_pending(tiny_tailstream *s) {
    tiny_blist out = {0};
    if (s->rest[0] != '\0') bl_push(&out, xstrdup(s->rest), fg_dim, NULL);
    return out;
}

