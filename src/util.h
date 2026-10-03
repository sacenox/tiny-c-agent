// Small shared helpers: allocation that aborts on failure, a growable byte
// buffer, and formatted error storage. Used across the whole program.

#ifndef TINY_UTIL_H
#define TINY_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} tiny_buf;

// The TUI assembles styled output with the same buffer.
typedef tiny_buf tui_buf;

void buf_init(tiny_buf *b);
void buf_free(tiny_buf *b);
void buf_clear(tiny_buf *b);
void buf_reserve(tiny_buf *b, size_t extra);
void buf_add(tiny_buf *b, const void *p, size_t n);
void buf_adds(tiny_buf *b, const char *s);
void buf_addc(tiny_buf *b, char c);
void buf_addf(tiny_buf *b, const char *fmt, ...);
// Hands the buffer's string to the caller and resets the buffer. Never NULL.
char *buf_steal(tiny_buf *b);

// Stores a formatted, malloc'd message into *err, freeing the previous one.
void tiny_set_error(char **err, const char *fmt, ...);

// tiny_set_error plus a -1 return, for the common error path.
int tiny_fail(char **err, const char *fmt, ...);

// Creates every component of path, like mkdir -p. Returns 0, or -1 with errno.
int tiny_mkdirs(const char *path);

// Builds "a/b", avoiding a doubled separator when a already ends in one.
char *tiny_path_join(const char *a, const char *b);

// Splits `s` on newlines into `*out` (malloc'd array of malloc'd strings) and
// returns the count. The trailing empty part is included, matching a plain
// split. Free with tiny_strv_free.
char **tiny_split_lines(const char *s, size_t *count);

// Frees an array returned by tiny_split_lines.
void tiny_strv_free(char **v, size_t n);

// A malloc'd copy of `s` with trailing newlines and carriage returns removed.
char *tiny_trim_end(const char *s);

// A malloc'd copy of `s` with leading and trailing ASCII whitespace removed.
char *tiny_trim(const char *s);

// Reads a whole file. Returns NULL when it cannot be opened or read; errno is
// left set as the failing call set it.
char *tiny_read_file(const char *path);

// Reads all of `f` into one malloc'd string.
char *tiny_read_stream(FILE *f);

// Writes all of `buf` to `fd`, retrying on EINTR. Returns 0, or -1 with errno.
int tiny_write_all(int fd, const char *buf, size_t len);

// Sets *dst to a copy of `src` (or ""), freeing the previous value.
void tiny_set_str(char **dst, const char *src);

// Appends `s` to a growable string array, growing `*cap` as needed.
void tiny_strv_push(char ***v, size_t *n, size_t *cap, char *s);

// Builds an endpoint URL from a base and a path, avoiding a doubled separator.
// Returns NULL when `base` is NULL or empty.
char *tiny_url_endpoint(const char *base, const char *path);

#endif
