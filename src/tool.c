// The read, edit, and bash tools.

#include "tool.h"

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

#include "diff.h"
#include "util.h"

#define MAX_TEXT 100000
#define MAX_IMAGE_B64 (5u * 1024 * 1024)
#define MAX_READ (16u * 1024 * 1024)
#define MAX_HEAD 10000
#define MAX_TAIL 6000
#define TRUNCATED "\n\n... output truncated ...\n\n"

// ---- small helpers --------------------------------------------------------

static void set_error(tiny_tool_result *res, const char *fmt, ...) {
    char detail[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    res->text = xstrdup(detail);
    res->is_error = true;
}

// The named member of `args` when it is a string, else NULL.
static const char *arg_str(yyjson_doc *doc, const char *key) {
    if (!doc) return NULL;
    yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(doc), key);
    return yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
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
    tiny_buf b = {0};
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
static int slurp(const char *path, tiny_buf *out) {
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

static void tool_read(const char *args, bool supports_images, tiny_tool_result *res) {
    yyjson_doc *doc = yyjson_read(args, strlen(args), 0);
    const char *path = arg_str(doc, "path");
    if (!path) {
        set_error(res, "read failed: path must be a string");
        yyjson_doc_free(doc);
        return;
    }

    tiny_buf data = {0};
    if (slurp(path, &data) != 0) {
        set_error(res, "read failed: %s: %s", path, strerror(errno));
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    const char *mime = image_mime(path);
    if (mime) {
        if (!supports_images) {
            set_error(res, "read failed: %s is an image and the current model does not accept image input", path);
            free(data.data);
            yyjson_doc_free(doc);
            return;
        }
        char *b64 = base64_encode((const unsigned char *)data.data, data.len);
        if (strlen(b64) > MAX_IMAGE_B64) {
            set_error(res, "read failed: %s encodes to %zu bytes, over the %u byte image limit",
                      path, strlen(b64), MAX_IMAGE_B64);
            free(b64);
            free(data.data);
            yyjson_doc_free(doc);
            return;
        }
        res->text = xmalloc(strlen(path) + strlen(mime) + 64);
        sprintf(res->text, "read %s (%s, %zu bytes)", path, mime, data.len);
        res->images = xmalloc(sizeof *res->images);
        res->images[0] = (tiny_image){.media_type = xstrdup(mime), .data = b64};
        res->images_len = 1;
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    size_t sniff = data.len < 8000 ? data.len : 8000;
    if (sniff > 0 && memchr(data.data, 0, sniff)) {
        set_error(res, "read failed: %s is binary; use bash (file, xxd)", path);
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    size_t take = data.len > MAX_TEXT ? MAX_TEXT : data.len;
    char *clean = utf8_clean(data.data ? data.data : "", take);
    if (data.len > MAX_TEXT) {
        tiny_buf out = {0};
        buf_adds(&out, clean);
        char marker[256];
        snprintf(marker, sizeof marker, "\n\n... truncated ... %s is %zu characters\n", path, data.len);
        buf_adds(&out, marker);
        free(clean);
        clean = out.data;
    }
    free(data.data);
    yyjson_doc_free(doc);
    res->text = clean;
}

// ---- edit -----------------------------------------------------------------

static void tool_edit(const char *args, tiny_tool_result *res) {
    yyjson_doc *doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *pa = root ? yyjson_obj_get(root, "path") : NULL;
    yyjson_val *oa = root ? yyjson_obj_get(root, "oldText") : NULL;
    yyjson_val *na = root ? yyjson_obj_get(root, "newText") : NULL;
    if (!yyjson_is_str(pa) || !yyjson_is_str(oa) || !yyjson_is_str(na)) {
        set_error(res, "edit failed: path, oldText and newText must be strings");
        yyjson_doc_free(doc);
        return;
    }
    const char *path = yyjson_get_str(pa);
    const char *old = yyjson_get_str(oa);
    size_t old_len = yyjson_get_len(oa);
    const char *nw = yyjson_get_str(na);
    size_t new_len = yyjson_get_len(na);

    if (old_len == 0) {
        if (access(path, F_OK) == 0) {
            set_error(res, "edit failed: %s already exists", path);
            yyjson_doc_free(doc);
            return;
        }
        if (spill(path, nw, new_len) != 0) {
            set_error(res, "edit failed: %s: %s", path, strerror(errno));
            yyjson_doc_free(doc);
            return;
        }
        char *patch = tiny_unified_diff(path, "", 0, nw, new_len);
        tui_buf b;
        buf_init(&b);
        buf_addf(&b, "created %s\n", path);
        buf_adds(&b, patch);
        res->text = buf_steal(&b);
        buf_free(&b);
        free(patch);
        yyjson_doc_free(doc);
        return;
    }

    tiny_buf data = {0};
    if (slurp(path, &data) != 0) {
        set_error(res, "edit failed: %s: %s", path, strerror(errno));
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    size_t at;
    if (!find_sub(data.data, data.len, old, old_len, &at)) {
        set_error(res, "edit failed: oldText not found in %s", path);
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }
    size_t second;
    if (find_sub(data.data + at + 1, data.len - at - 1, old, old_len, &second)) {
        set_error(res, "edit failed: oldText matches more than once in %s", path);
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    tiny_buf out = {0};
    buf_add(&out, data.data, at);
    buf_add(&out, nw, new_len);
    buf_add(&out, data.data + at + old_len, data.len - at - old_len);
    if (spill(path, out.data, out.len) != 0) {
        set_error(res, "edit failed: %s: %s", path, strerror(errno));
        free(out.data);
        free(data.data);
        yyjson_doc_free(doc);
        return;
    }

    char *patch = tiny_unified_diff(path, data.data, data.len, out.data, out.len);
    tui_buf b;
    buf_init(&b);
    buf_addf(&b, "edited %s\n", path);
    buf_adds(&b, patch);
    res->text = buf_steal(&b);
    buf_free(&b);
    free(patch);
    free(out.data);
    free(data.data);
    yyjson_doc_free(doc);
}

// ---- bash -----------------------------------------------------------------

// Keeps only the last MAX_TAIL bytes, flagging that a gap was dropped.
static void keep_tail(tiny_buf *t, bool *truncated) {
    if (t->len <= MAX_TAIL) return;
    memmove(t->data, t->data + t->len - MAX_TAIL, MAX_TAIL);
    t->len = MAX_TAIL;
    t->data[t->len] = '\0';
    *truncated = true;
}

static void tool_bash(const char *args, tiny_tool_output_fn out, void *user,
                      const volatile bool *cancel, tiny_tool_result *res) {
    yyjson_doc *doc = yyjson_read(args, strlen(args), 0);
    const char *command = arg_str(doc, "command");
    if (!command) {
        set_error(res, "bash: command must be a string");
        yyjson_doc_free(doc);
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        set_error(res, "bash: %s", strerror(errno));
        yyjson_doc_free(doc);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        set_error(res, "bash: fork: %s", strerror(errno));
        yyjson_doc_free(doc);
        return;
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

    tiny_buf head = {0}, tail = {0};
    bool truncated = false;
    bool cancelled = false;

    char chunk[8192];
    for (;;) {
        if (cancel && *cancel) {
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
        ssize_t n = read(pipefd[0], chunk, sizeof chunk - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        chunk[n] = '\0';
        if (out) out(chunk, user);
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

    tiny_buf body = {0};
    buf_add(&body, head.data ? head.data : "", head.len);
    if (truncated) buf_adds(&body, TRUNCATED);
    buf_add(&body, tail.data ? tail.data : "", tail.len);

    char *clean = utf8_clean(body.data ? body.data : "", body.len);
    tiny_buf text = {0};
    buf_adds(&text, clean);
    if (text.len > 0 && text.data[text.len - 1] != '\n') buf_adds(&text, "\n");
    char code_line[32];
    snprintf(code_line, sizeof code_line, "exit code: %d", code);
    buf_adds(&text, code_line);
    res->text = text.data;
    res->is_error = code != 0;
    free(clean);
    free(body.data);
    free(head.data);
    free(tail.data);
    yyjson_doc_free(doc);
}

// ---- the toolset ----------------------------------------------------------

#define READ_DESCRIPTION \
    "Read a file. Returns its text. Prefer bash for search, ranges, or binary files."
#define READ_IMAGE_DESCRIPTION \
    "Read a file. Returns its text, or the image itself when the file is a png, jpg, or webp. " \
    "Prefer bash for search, ranges, or binary files."
#define EDIT_DESCRIPTION \
    "Edit a file by exact text replacement. oldText must occur exactly once. " \
    "With empty oldText, create a new file (fails if it exists)."
#define BASH_DESCRIPTION "Run a bash command in the current working directory."

#define READ_PARAMS \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}}," \
    "\"required\":[\"path\"],\"additionalProperties\":false}"
#define EDIT_PARAMS \
    "{\"type\":\"object\",\"properties\":{" \
    "\"path\":{\"type\":\"string\",\"description\":\"File path\"}," \
    "\"oldText\":{\"type\":\"string\",\"description\":\"Exact text to replace; empty to create a file\"}," \
    "\"newText\":{\"type\":\"string\",\"description\":\"Replacement text\"}}," \
    "\"required\":[\"path\",\"oldText\",\"newText\"],\"additionalProperties\":false}"
#define BASH_PARAMS \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command to run\"}}," \
    "\"required\":[\"command\"],\"additionalProperties\":false}"

const tiny_tool *tiny_tools(bool supports_images, size_t *len) {
    static const tiny_tool plain[] = {
        {"read", READ_DESCRIPTION, READ_PARAMS},
        {"edit", EDIT_DESCRIPTION, EDIT_PARAMS},
        {"bash", BASH_DESCRIPTION, BASH_PARAMS},
    };
    static const tiny_tool images[] = {
        {"read", READ_IMAGE_DESCRIPTION, READ_PARAMS},
        {"edit", EDIT_DESCRIPTION, EDIT_PARAMS},
        {"bash", BASH_DESCRIPTION, BASH_PARAMS},
    };
    *len = 3;
    return supports_images ? images : plain;
}

tiny_tool_result tiny_tool_run(const char *name, const char *args, bool supports_images,
                               tiny_tool_output_fn out, void *user, const volatile bool *cancel) {
    tiny_tool_result res = {0};
    if (name && strcmp(name, "read") == 0) {
        tool_read(args, supports_images, &res);
    } else if (name && strcmp(name, "edit") == 0) {
        tool_edit(args, &res);
    } else if (name && strcmp(name, "bash") == 0) {
        tool_bash(args, out, user, cancel, &res);
    } else {
        set_error(&res, "unknown tool: %s", name ? name : "");
    }
    return res;
}
