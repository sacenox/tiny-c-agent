#include "sse.h"

#include <stdlib.h>
#include <string.h>

static void add(cage_sse *s, const char *p, size_t n) {
    if (s->pending_len + n + 1 > s->pending_cap) {
        size_t cap = s->pending_cap ? s->pending_cap : 128;
        while (cap < s->pending_len + n + 1) cap *= 2;
        s->pending = realloc(s->pending, cap);
        if (!s->pending) abort();
        s->pending_cap = cap;
    }
    memcpy(s->pending + s->pending_len, p, n);
    s->pending_len += n;
    s->pending[s->pending_len] = '\0';
}

void cage_sse_init(cage_sse *s) {
    s->pending = NULL;
    s->pending_len = 0;
    s->pending_cap = 0;
}

void cage_sse_free(cage_sse *s) {
    free(s->pending);
    cage_sse_init(s);
}

static void handle_line(cage_sse_fn fn, void *user, const char *line, size_t len) {
    if (len < 5 || memcmp(line, "data:", 5) != 0) return;
    const char *p = line + 5;
    const char *end = line + len;
    while (p < end && *p == ' ') p++;
    if (end - p == 6 && memcmp(p, "[DONE]", 6) == 0) return;
    if (end > p) fn(p, (size_t)(end - p), user);
}

void cage_sse_feed(cage_sse *s, const char *data, size_t n, cage_sse_fn fn, void *user) {
    add(s, data, n);
    size_t start = 0;
    for (size_t i = 0; i < s->pending_len; i++) {
        if (s->pending[i] != '\n') continue;
        size_t end = i;
        if (end > start && s->pending[end - 1] == '\r') end--;
        handle_line(fn, user, s->pending + start, end - start);
        start = i + 1;
    }
    if (start > 0) {
        memmove(s->pending, s->pending + start, s->pending_len - start);
        s->pending_len -= start;
        s->pending[s->pending_len] = '\0';
    }
}
