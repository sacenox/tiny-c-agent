// cage TUI: a growable byte buffer, used to assemble styled output.

#ifndef CAGE_TUI_BUF_H
#define CAGE_TUI_BUF_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} tui_buf;

static inline void buf_init(tui_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static inline void buf_free(tui_buf *b) {
    free(b->data);
    buf_init(b);
}

static inline void buf_clear(tui_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static inline void buf_reserve(tui_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = realloc(b->data, cap);
    if (!b->data) abort();
    b->cap = cap;
}

static inline void buf_add(tui_buf *b, const char *s, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static inline void buf_adds(tui_buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

static inline void buf_addc(tui_buf *b, char c) {
    buf_add(b, &c, 1);
}

static inline void buf_addf(tui_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    b->len += (size_t)n;
    va_end(ap2);
}

// Hands the string to the caller and resets the buffer.
static inline char *buf_steal(tui_buf *b) {
    char *d = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return d;
}

#endif
