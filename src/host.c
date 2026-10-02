// The read, edit, and bash capabilities, exposed as codemode globals.

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <yyjson.h>

#define MAX_TEXT 100000
#define MAX_IMAGE_B64 (5u * 1024 * 1024)
#define MAX_READ (16u * 1024 * 1024)
#define MAX_HEAD 10000
#define MAX_TAIL 6000
#define TRUNCATED "\n\n... output truncated ...\n\n"

// ---- small helpers --------------------------------------------------------

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

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf;

static void buf_add(buf *b, const char *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = realloc(b->data, cap);
        if (!b->data) abort();
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_addz(buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

static cage_host_result ok_json(char *json) {
    return (cage_host_result){.json = json};
}

static cage_host_result err(const char *fmt, ...) {
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    return (cage_host_result){.error = xstrdup(detail)};
}

// A JSON string value; `s` must be valid UTF-8 and NUL-terminated.
static char *json_string(const char *s) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_strcpy(doc, s));
    char *out = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return out;
}

// nth argument of the JSON array the runtime passes to a host function.
static yyjson_val *arg_at(yyjson_doc *doc, size_t i) {
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) return NULL;
    return yyjson_arr_get(root, i);
}

// ---- UTF-8 ----------------------------------------------------------------

// Validates one sequence at `p` (n bytes remain), returning its length.
static bool utf8_seq(const unsigned char *p, size_t n, size_t *len) {
    unsigned char c = p[0];
    if (c >= 0xC2 && c <= 0xDF) {
        if (n >= 2 && (p[1] & 0xC0) == 0x80) return (*len = 2, true);
    } else if (c == 0xE0) {
        if (n >= 3 && p[1] >= 0xA0 && p[1] <= 0xBF && (p[2] & 0xC0) == 0x80) return (*len = 3, true);
    } else if (c >= 0xE1 && c <= 0xEC) {
        if (n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) return (*len = 3, true);
    } else if (c == 0xED) {
        if (n >= 3 && p[1] >= 0x80 && p[1] <= 0x9F && (p[2] & 0xC0) == 0x80) return (*len = 3, true);
    } else if (c >= 0xEE && c <= 0xEF) {
        if (n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) return (*len = 3, true);
    } else if (c == 0xF0) {
        if (n >= 4 && p[1] >= 0x90 && p[1] <= 0xBF && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
            return (*len = 4, true);
    } else if (c >= 0xF1 && c <= 0xF3) {
        if (n >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
            return (*len = 4, true);
    } else if (c == 0xF4) {
        if (n >= 4 && p[1] >= 0x80 && p[1] <= 0x8F && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
            return (*len = 4, true);
    }
    return false;
}

// Replaces invalid byte sequences with U+FFFD, so the text survives the JSON
// and session round trips. Mirrors a UTF-8 decode with replacement.
static char *utf8_clean(const char *in, size_t n) {
    buf b = {0};
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0;
    while (i < n) {
        size_t len;
        if (p[i] < 0x80) {
            buf_add(&b, (const char *)&p[i], 1);
            i += 1;
        } else if (utf8_seq(p + i, n - i, &len)) {
            buf_add(&b, (const char *)(p + i), len);
            i += len;
        } else {
            buf_add(&b, "\xef\xbf\xbd", 3);
            i += 1;
        }
    }
    if (!b.data) buf_add(&b, "", 1);
    return b.data;
}

// ---- file helpers ---------------------------------------------------------

// Reads up to MAX_READ bytes. Returns 0, or -1 with errno set.
static int slurp(const char *path, buf *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return -1;
    }
    for (;;) {
        if (out->len >= MAX_READ) break;
        char tmp[65536];
        size_t want = sizeof tmp;
        if (out->len + want > MAX_READ) want = MAX_READ - out->len;
        ssize_t n = read(fd, tmp, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        if (n == 0) break;
        buf_add(out, tmp, (size_t)n);
    }
    close(fd);
    return 0;
}

static int spill(const char *path, const char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) return -1;
    return 0;
}

static bool find_sub(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t *at) {
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

static char *base64_encode(const unsigned char *data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = xmalloc(4 * ((len + 2) / 3) + 1);
    size_t i = 0, o = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = table[(v >> 6) & 63];
        out[o++] = table[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8;
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = table[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static const char *image_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    const char *e = dot + 1;
    if (strcasecmp(e, "png") == 0) return "image/png";
    if (strcasecmp(e, "jpg") == 0 || strcasecmp(e, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(e, "webp") == 0) return "image/webp";
    return NULL;
}

// ---- read -----------------------------------------------------------------

static cage_host_result host_read(const char *args_json, void *user) {
    cage_runtime *rt = user;
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    yyjson_val *a = arg_at(doc, 0);
    if (!yyjson_is_str(a)) {
        yyjson_doc_free(doc);
        return err("read: path must be a string");
    }
    const char *path = yyjson_get_str(a);

    buf data = {0};
    if (slurp(path, &data) != 0) {
        cage_host_result r = err("read failed: %s: %s", path, strerror(errno));
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }

    const char *mime = image_mime(path);
    if (mime) {
        if (!cage_runtime_supports_images(rt)) {
            cage_host_result r = err("read failed: %s is an image and the current model does not accept image input", path);
            free(data.data);
            yyjson_doc_free(doc);
            return r;
        }
        char *b64 = base64_encode((const unsigned char *)data.data, data.len);
        if (strlen(b64) > MAX_IMAGE_B64) {
            cage_host_result r = err("read failed: %s encodes to %zu bytes, over the %u byte image limit",
                                     path, strlen(b64), MAX_IMAGE_B64);
            free(b64);
            free(data.data);
            yyjson_doc_free(doc);
            return r;
        }
        char msg[1024];
        snprintf(msg, sizeof msg, "read %s (%s, %zu bytes)", path, mime, data.len);
        cage_runtime_add_image(rt, mime, b64);
        free(b64);
        free(data.data);
        yyjson_doc_free(doc);
        return ok_json(json_string(msg));
    }

    size_t sniff = data.len < 8000 ? data.len : 8000;
    if (sniff > 0 && memchr(data.data, 0, sniff)) {
        cage_host_result r = err("read failed: %s is binary; use bash (file, xxd)", path);
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }

    size_t take = data.len > MAX_TEXT ? MAX_TEXT : data.len;
    char *clean = utf8_clean(data.data ? data.data : "", take);
    if (data.len > MAX_TEXT) {
        buf out = {0};
        buf_addz(&out, clean);
        char marker[256];
        snprintf(marker, sizeof marker, "\n\n... truncated ... %s is %zu characters\n", path, data.len);
        buf_addz(&out, marker);
        free(clean);
        clean = out.data;
    }
    free(data.data);
    yyjson_doc_free(doc);
    return ok_json(json_string(clean));
}

// ---- edit -----------------------------------------------------------------

static cage_host_result host_edit(const char *args_json, void *user) {
    (void)user;
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    yyjson_val *pa = arg_at(doc, 0), *oa = arg_at(doc, 1), *na = arg_at(doc, 2);
    if (!yyjson_is_str(pa) || !yyjson_is_str(oa) || !yyjson_is_str(na)) {
        yyjson_doc_free(doc);
        return err("edit: path, oldText and newText must be strings");
    }
    const char *path = yyjson_get_str(pa);
    const char *old = yyjson_get_str(oa);
    size_t old_len = yyjson_get_len(oa);
    const char *nw = yyjson_get_str(na);
    size_t new_len = yyjson_get_len(na);

    if (old_len == 0) {
        if (access(path, F_OK) == 0) {
            cage_host_result r = err("edit failed: %s already exists", path);
            yyjson_doc_free(doc);
            return r;
        }
        if (spill(path, nw, new_len) != 0) {
            cage_host_result r = err("edit failed: %s: %s", path, strerror(errno));
            yyjson_doc_free(doc);
            return r;
        }
        char msg[1024];
        snprintf(msg, sizeof msg, "created %s", path);
        yyjson_doc_free(doc);
        return ok_json(json_string(msg));
    }

    buf data = {0};
    if (slurp(path, &data) != 0) {
        cage_host_result r = err("edit failed: %s: %s", path, strerror(errno));
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }

    size_t at;
    if (!find_sub(data.data, data.len, old, old_len, &at)) {
        cage_host_result r = err("edit failed: oldText not found in %s", path);
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }
    size_t second;
    if (find_sub(data.data + at + 1, data.len - at - 1, old, old_len, &second)) {
        cage_host_result r = err("edit failed: oldText matches more than once in %s", path);
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }

    buf out = {0};
    buf_add(&out, data.data, at);
    buf_add(&out, nw, new_len);
    buf_add(&out, data.data + at + old_len, data.len - at - old_len);
    if (spill(path, out.data, out.len) != 0) {
        cage_host_result r = err("edit failed: %s: %s", path, strerror(errno));
        free(out.data);
        free(data.data);
        yyjson_doc_free(doc);
        return r;
    }

    char msg[1024];
    snprintf(msg, sizeof msg, "edited %s", path);
    free(out.data);
    free(data.data);
    yyjson_doc_free(doc);
    return ok_json(json_string(msg));
}

// ---- bash -----------------------------------------------------------------

// Keeps only the last MAX_TAIL bytes, flagging that a gap was dropped.
static void keep_tail(buf *t, bool *truncated) {
    if (t->len <= MAX_TAIL) return;
    memmove(t->data, t->data + t->len - MAX_TAIL, MAX_TAIL);
    t->len = MAX_TAIL;
    t->data[t->len] = '\0';
    *truncated = true;
}

static cage_host_result host_bash(const char *args_json, void *user) {
    cage_runtime *rt = user;
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    yyjson_val *a = arg_at(doc, 0);
    if (!yyjson_is_str(a)) {
        yyjson_doc_free(doc);
        return err("bash: command must be a string");
    }
    const char *command = yyjson_get_str(a);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        cage_host_result r = err("bash: %s", strerror(errno));
        yyjson_doc_free(doc);
        return r;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        cage_host_result r = err("bash: fork: %s", strerror(errno));
        yyjson_doc_free(doc);
        return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(127);
        close(pipefd[0]);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execlp("bash", "bash", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    setpgid(pid, pid);

    buf head = {0}, tail = {0};
    bool truncated = false;
    bool cancelled = false;

    char chunk[8192];
    for (;;) {
        if (cage_runtime_cancelled(rt)) {
            cancelled = true;
            break;
        }
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        ssize_t n = read(pipefd[0], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        chunk[n] = '\0';
        cage_runtime_output(rt, chunk);
        if (head.len < MAX_HEAD) {
            size_t take = MAX_HEAD - head.len;
            if ((size_t)n < take) take = (size_t)n;
            buf_add(&head, chunk, take);
            if (take < (size_t)n) {
                buf_add(&tail, chunk + take, (size_t)n - take);
                keep_tail(&tail, &truncated);
            }
        } else {
            buf_add(&tail, chunk, (size_t)n);
            keep_tail(&tail, &truncated);
        }
    }
    close(pipefd[0]);

    int status = 0;
    if (cancelled) {
        kill(-pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 30; i++) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                reaped = true;
                break;
            }
            usleep(100000);
        }
        if (!reaped) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    int code;
    if (cancelled) code = 143;
    else if (WIFEXITED(status)) code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    else code = 0;

    buf body = {0};
    buf_add(&body, head.data ? head.data : "", head.len);
    if (truncated) buf_addz(&body, TRUNCATED);
    buf_add(&body, tail.data ? tail.data : "", tail.len);

    char *clean = utf8_clean(body.data ? body.data : "", body.len);
    yyjson_mut_doc *jd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(jd);
    yyjson_mut_doc_set_root(jd, o);
    yyjson_mut_obj_add_strcpy(jd, o, "output", clean);
    yyjson_mut_obj_add_int(jd, o, "exitCode", code);
    char *json = yyjson_mut_write(jd, 0, NULL);
    yyjson_mut_doc_free(jd);
    free(clean);
    free(body.data);
    free(head.data);
    free(tail.data);
    yyjson_doc_free(doc);
    return ok_json(json);
}

void cage_hosts_register(cage_runtime *rt) {
    cage_runtime_register(rt, "read", host_read, rt);
    cage_runtime_register(rt, "edit", host_edit, rt);
    cage_runtime_register(rt, "bash", host_bash, rt);
}
