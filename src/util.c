#include "util.h"
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) abort();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) abort();
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void buf_init(cage_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(cage_buf *b) {
    free(b->data);
    buf_init(b);
}

void buf_clear(cage_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void buf_reserve(cage_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_add(cage_buf *b, const void *p, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_adds(cage_buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

void buf_addc(cage_buf *b, char c) {
    buf_add(b, &c, 1);
}

void buf_addf(cage_buf *b, const char *fmt, ...) {
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

char *buf_steal(cage_buf *b) {
    if (!b->data) return xstrdup("");
    char *d = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return d;
}

void cage_set_error(char **err, const char *fmt, ...) {
    if (!err) return;
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    free(*err);
    *err = xstrdup(detail);
}

int cage_fail(char **err, const char *fmt, ...) {
    if (err) {
        char detail[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
        free(*err);
        *err = xstrdup(detail);
    }
    return -1;
}

int cage_mkdirs(const char *path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *cage_path_join(const char *a, const char *b) {
    size_t al = strlen(a);
    bool slash = al > 0 && a[al - 1] == '/';
    size_t n = al + strlen(b) + (slash ? 1 : 2);
    char *p = xmalloc(n);
    snprintf(p, n, slash ? "%s%s" : "%s/%s", a, b);
    return p;
}

char **cage_split_lines(const char *s, size_t *count) {
    size_t cap = 8, n = 0;
    char **out = xmalloc(cap * sizeof *out);
    const char *p = s;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        if (n == cap) {
            cap *= 2;
            out = xrealloc(out, cap * sizeof *out);
        }
        out[n++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    *count = n;
    return out;
}

void cage_strv_free(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

char *cage_trim_end(const char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    char *out = xmalloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char *cage_trim(const char *s) {
    while (is_space(*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && is_space(s[n - 1])) n--;
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

void cage_set_str(char **dst, const char *src) {
    free(*dst);
    *dst = xstrdup(src ? src : "");
}

int cage_write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

void cage_strv_push(char ***v, size_t *n, size_t *cap, char *s) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = xrealloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = s;
}

char *cage_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *s = cage_read_stream(f);
    fclose(f);
    return s;
}

char *cage_read_stream(FILE *f) {
    cage_buf b;
    buf_init(&b);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, n);
    return buf_steal(&b);
}

char *cage_url_endpoint(const char *base, const char *path) {
    if (!base || !base[0]) return NULL;
    size_t n = strlen(base);
    bool slash = base[n - 1] == '/';
    size_t size = n + strlen(path) + 2;
    char *url = xmalloc(size);
    snprintf(url, size, slash ? "%s%s" : "%s/%s", base, path);
    return url;
}
