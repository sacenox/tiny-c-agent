#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

struct cage_session {
    int fd;
    char id[128];
    char path[PATH_MAX + 16];
};

static void stamp_now(char out[32]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y%m%d-%H%M%S", &tm);
}

static void iso_now(char out[32]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void short_id(char out[7]) {
    unsigned char b[6] = {0};
    size_t got = 0;
    while (got < sizeof b) {
        ssize_t n = getrandom(b + got, sizeof b - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)n;
    }
    static const char alnum[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < 6; i++) out[i] = alnum[b[i] % 36];
    out[6] = '\0';
}

// Lowercases text into a dash-separated slug, collapsing runs and trimming.
static void slugify(const char *text, char *out, size_t cap) {
    size_t n = 0;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int c = *p;
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            dash = n > 0;
            continue;
        }
        if (dash && n + 1 < cap) out[n++] = '-';
        dash = false;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n == 0) snprintf(out, cap, "session");
}

// Writes one line and fsyncs, so a committed record survives a crash.
static int write_record(cage_session *s, const char *line, size_t len, char **err) {
    if (cage_write_all(s->fd, line, len) != 0 || cage_write_all(s->fd, "\n", 1) != 0)
        return cage_fail(err, "session %s: write failed: %s", s->path, strerror(errno));
    if (fsync(s->fd) != 0)
        return cage_fail(err, "session %s: fsync failed: %s", s->path, strerror(errno));
    return 0;
}

cage_session *cage_session_open(const char *sessions_dir, const char *cwd,
                                const char *title, char **err) {
    if (err) *err = NULL;

    char slug[48];
    slugify(title ? title : "", slug, sizeof slug);
    if (cage_mkdirs(sessions_dir) != 0) {
        cage_fail(err, "session: cannot create %s: %s", sessions_dir, strerror(errno));
        return NULL;
    }

    char id[128];
    char dir[PATH_MAX];
    bool created = false;
    for (int attempt = 0; attempt < 16 && !created; attempt++) {
        char sid[7];
        short_id(sid);
        char stamp[32];
        stamp_now(stamp);
        snprintf(id, sizeof id, "%s-%s-%s", stamp, slug, sid);
        snprintf(dir, sizeof dir, "%s/%s", sessions_dir, id);
        if (mkdir(dir, 0755) == 0) created = true;
        else if (errno != EEXIST) {
            cage_fail(err, "session: cannot create %s: %s", dir, strerror(errno));
            return NULL;
        }
    }
    if (!created) {
        cage_fail(err, "session: could not create a unique directory in %s", sessions_dir);
        return NULL;
    }

    char log_path[PATH_MAX + 16];
    snprintf(log_path, sizeof log_path, "%s/session.jsonl", dir);
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        cage_fail(err, "session: cannot open %s: %s", log_path, strerror(errno));
        return NULL;
    }

    cage_session *s = xmalloc(sizeof *s);
    s->fd = fd;
    snprintf(s->id, sizeof s->id, "%s", id);
    snprintf(s->path, sizeof s->path, "%s", log_path);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    char created_at[32];
    iso_now(created_at);
    yyjson_mut_obj_add_str(doc, obj, "type", "session");
    yyjson_mut_obj_add_int(doc, obj, "version", 1);
    yyjson_mut_obj_add_str(doc, obj, "id", id);
    yyjson_mut_obj_add_str(doc, obj, "cwd", cwd ? cwd : "");
    yyjson_mut_obj_add_str(doc, obj, "createdAt", created_at);
    yyjson_mut_obj_add_str(doc, obj, "title", slug);
    size_t len = 0;
    char *line = yyjson_mut_val_write(obj, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!line) {
        cage_fail(err, "session %s: cannot serialize header", s->path);
        close(fd);
        free(s);
        return NULL;
    }

    int rc = write_record(s, line, len, err);
    free(line);
    if (rc != 0) {
        close(fd);
        free(s);
        return NULL;
    }
    return s;
}

const char *cage_session_id(const cage_session *s) {
    return s->id;
}

int cage_session_append(cage_session *s, yyjson_mut_val *record, char **err) {
    if (err) *err = NULL;
    size_t len = 0;
    char *line = yyjson_mut_val_write(record, 0, &len);
    if (!line) return cage_fail(err, "session %s: cannot serialize record", s->path);
    int rc = write_record(s, line, len, err);
    free(line);
    return rc;
}

void cage_session_close(cage_session *s) {
    if (!s) return;
    close(s->fd);
    free(s);
}
