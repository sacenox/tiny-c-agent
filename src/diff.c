// Working-tree snapshots and unified diffs.

#include "diff.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define MAX_FILE_BYTES (1u << 20)
#define MAX_TOTAL_BYTES (32u << 20)
#define BINARY_SNIFF 8192
#define CONTEXT 3
#define MAX_TRACE_BYTES (64u << 20)

// ---- small helpers --------------------------------------------------------


// ---- file states and cache ------------------------------------------------

enum { FS_TEXT, FS_BINARY, FS_LARGE, FS_LINK, FS_UNTRACKED };

typedef struct {
    size_t size;
    struct timespec mtime;
    ino_t ino;
    int kind;
    char *content;  // NULL unless FS_TEXT
    int refs;
} file_state;

// A captured file. `rel` and `abs` are owned.
typedef struct {
    char *rel;
    char *abs;
    size_t size;
    struct timespec mtime;
    ino_t ino;
    bool link;
} found;

typedef struct {
    found *items;
    size_t len;
    size_t cap;
} found_list;

static void found_add(found_list *l, char *rel, char *abs, const struct stat *st, bool link) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = realloc(l->items, l->cap * sizeof *l->items);
        if (!l->items) abort();
    }
    l->items[l->len++] = (found){
        .rel = rel,
        .abs = abs,
        .size = (size_t)st->st_size,
        .mtime = st->st_mtim,
        .ino = st->st_ino,
        .link = link,
    };
}

static const char *const IGNORED_DIRS[] = {
    ".git", ".hg", ".svn", "node_modules", ".venv", "venv",
    "__pycache__", ".mypy_cache", ".pytest_cache",
};

static bool ignored_dir(const char *name) {
    for (size_t i = 0; i < sizeof IGNORED_DIRS / sizeof *IGNORED_DIRS; i++)
        if (strcmp(name, IGNORED_DIRS[i]) == 0) return true;
    return false;
}

static void walk_dir(const char *dir, const char *relbase, found_list *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        size_t fl = strlen(dir) + strlen(de->d_name) + 2;
        char *full = xmalloc(fl);
        snprintf(full, fl, "%s/%s", dir, de->d_name);
        size_t rl = strlen(relbase) + strlen(de->d_name) + 2;
        char *rel = xmalloc(rl);
        if (relbase[0]) snprintf(rel, rl, "%s/%s", relbase, de->d_name);
        else snprintf(rel, rl, "%s", de->d_name);

        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!ignored_dir(de->d_name)) walk_dir(full, rel, out);
            } else if (S_ISLNK(st.st_mode)) {
                found_add(out, rel, full, &st, true);
                rel = NULL;
                full = NULL;
            } else if (S_ISREG(st.st_mode)) {
                found_add(out, rel, full, &st, false);
                rel = NULL;
                full = NULL;
            }
        }
        free(rel);
        free(full);
    }
    closedir(d);
}

static bool same_stat(const file_state *s, const found *f) {
    return s->size == f->size && s->ino == f->ino &&
           s->mtime.tv_sec == f->mtime.tv_sec && s->mtime.tv_nsec == f->mtime.tv_nsec;
}

static bool mtime_at_or_after(const found *f, const struct timespec *since) {
    if (f->mtime.tv_sec != since->tv_sec) return f->mtime.tv_sec > since->tv_sec;
    return f->mtime.tv_nsec >= since->tv_nsec;
}

static file_state *build_state(const found *f, bool over_budget) {
    file_state *s = xcalloc(1, sizeof *s);
    s->size = f->size;
    s->mtime = f->mtime;
    s->ino = f->ino;
    if (f->link) {
        s->kind = FS_LINK;
        return s;
    }
    if (f->size > MAX_FILE_BYTES) {
        s->kind = FS_LARGE;
        return s;
    }
    if (over_budget) {
        s->kind = FS_UNTRACKED;
        return s;
    }
    int fd = open(f->abs, O_RDONLY);
    if (fd < 0) {
        s->kind = FS_UNTRACKED;
        return s;
    }
    char *data = xmalloc(f->size + 1);
    size_t off = 0;
    while (off < f->size) {
        ssize_t n = read(fd, data + off, f->size - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    data[off] = '\0';
    size_t sniff = off < BINARY_SNIFF ? off : BINARY_SNIFF;
    for (size_t i = 0; i < sniff; i++) {
        if (data[i] == '\0') {
            free(data);
            s->kind = FS_BINARY;
            return s;
        }
    }
    s->kind = FS_TEXT;
    s->content = data;
    s->size = off;
    return s;
}

static void release(file_state *s) {
    if (--s->refs > 0) return;
    free(s->content);
    free(s);
}

typedef struct {
    char *path;
    file_state *state;
} cache_entry;

static cache_entry *g_cache;
static size_t g_cache_len;

static int cmp_cache(const void *a, const void *b) {
    const cache_entry *x = a, *y = b;
    return strcmp(x->path, y->path);
}

// The cache is kept sorted by path so lookups stay logarithmic.
static file_state *cache_find(const char *path) {
    size_t lo = 0, hi = g_cache_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(path, g_cache[mid].path);
        if (c == 0) return g_cache[mid].state;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

struct snapshot_entry {
    char *path;
    file_state *state;
};

struct tiny_snapshot {
    struct snapshot_entry *entries;
    size_t len;
};

static int cmp_entry(const void *a, const void *b) {
    const struct snapshot_entry *x = a, *y = b;
    return strcmp(x->path, y->path);
}

struct tiny_snapshot *tiny_snapshot_capture(const struct timespec *force_since) {
    found_list files = {0};
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    walk_dir(cwd, "", &files);

    struct tiny_snapshot *snap = xcalloc(1, sizeof *snap);
    snap->entries = xmalloc((files.len ? files.len : 1) * sizeof *snap->entries);
    cache_entry *new_cache = xmalloc((files.len ? files.len : 1) * sizeof *new_cache);
    size_t new_len = 0;
    size_t total = 0;

    for (size_t i = 0; i < files.len; i++) {
        found *f = &files.items[i];
        file_state *old = cache_find(f->rel);
        bool forced = force_since && mtime_at_or_after(f, force_since);
        bool reuse = old && !f->link && !forced && same_stat(old, f);
        file_state *s = reuse ? old : build_state(f, total >= MAX_TOTAL_BYTES);

        s->refs++;  // the new cache holds a reference
        new_cache[new_len++] = (cache_entry){.path = xstrdup(f->rel), .state = s};
        s->refs++;  // the snapshot holds a reference
        snap->entries[snap->len++] = (struct snapshot_entry){.path = xstrdup(f->rel), .state = s};
        if (s->kind == FS_TEXT) total += s->size;
    }

    for (size_t i = 0; i < g_cache_len; i++) {
        release(g_cache[i].state);
        free(g_cache[i].path);
    }
    free(g_cache);
    qsort(new_cache, new_len, sizeof *new_cache, cmp_cache);
    g_cache = new_cache;
    g_cache_len = new_len;

    for (size_t i = 0; i < files.len; i++) {
        free(files.items[i].rel);
        free(files.items[i].abs);
    }
    free(files.items);

    qsort(snap->entries, snap->len, sizeof *snap->entries, cmp_entry);
    return snap;
}

void tiny_snapshot_free(struct tiny_snapshot *s) {
    if (!s) return;
    for (size_t i = 0; i < s->len; i++) {
        release(s->entries[i].state);
        free(s->entries[i].path);
    }
    free(s->entries);
    free(s);
}

// ---- unified diff ---------------------------------------------------------

typedef struct {
    const char *p;
    size_t len;
    bool nl;  // the line ended with a newline
} line;

static line *split_lines(const char *s, size_t n, size_t *count) {
    size_t cap = 16, cnt = 0;
    line *ls = xmalloc(cap * sizeof *ls);
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && s[i] != '\n') i++;
        ls[cnt].p = s + start;
        ls[cnt].len = i - start;
        ls[cnt].nl = i < n;
        if (i < n) i++;
        if (++cnt == cap) {
            cap *= 2;
            ls = realloc(ls, cap * sizeof *ls);
            if (!ls) abort();
        }
    }
    *count = cnt;
    return ls;
}

static bool line_eq(const line *a, const line *b) {
    return a->len == b->len && memcmp(a->p, b->p, a->len) == 0;
}

enum { OP_EQ, OP_DEL, OP_INS };

typedef struct {
    int kind;
    line l;
} op;

// Myers O(ND) diff of two line arrays, producing an edit script in order.
static op *myers(const line *a, size_t n, const line *b, size_t m, size_t *out_len) {
    if (n == 0 && m == 0) {
        *out_len = 0;
        return NULL;
    }
    size_t cap = n + m;
    op *ops = xmalloc((cap ? cap : 1) * sizeof *ops);
    size_t cnt = 0;

    if (n == 0) {
        for (size_t i = 0; i < m; i++) ops[cnt++] = (op){OP_INS, b[i]};
        *out_len = cnt;
        return ops;
    }
    if (m == 0) {
        for (size_t i = 0; i < n; i++) ops[cnt++] = (op){OP_DEL, a[i]};
        *out_len = cnt;
        return ops;
    }

    int max = (int)(n + m);
    size_t vsize = (size_t)(2 * max + 3);
    int offset = max + 1;
    int *v = xcalloc(vsize, sizeof *v);
    v[offset + 1] = 0;

    int **trace = NULL;
    size_t trace_len = 0, trace_cap = 0;
    size_t trace_bytes = 0;
    int found_d = -1;

    for (int d = 0; d <= max; d++) {
        if (trace_bytes + vsize * sizeof *v > MAX_TRACE_BYTES) break;
        if (trace_len == trace_cap) {
            trace_cap = trace_cap ? trace_cap * 2 : 16;
            trace = realloc(trace, trace_cap * sizeof *trace);
            if (!trace) abort();
        }
        int *snap = xmalloc(vsize * sizeof *snap);
        memcpy(snap, v, vsize * sizeof *snap);
        trace[trace_len++] = snap;
        trace_bytes += vsize * sizeof *snap;

        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[k - 1 + offset] < v[k + 1 + offset]))
                x = v[k + 1 + offset];
            else
                x = v[k - 1 + offset] + 1;
            int y = x - k;
            while (x < (int)n && y < (int)m && line_eq(&a[x], &b[y])) {
                x++;
                y++;
            }
            v[k + offset] = x;
            if (x >= (int)n && y >= (int)m) {
                found_d = d;
                break;
            }
        }
        if (found_d >= 0) break;
    }

    if (found_d < 0) {
        // Trace budget exceeded: fall back to a coarse replacement.
        for (size_t i = 0; i < n; i++) ops[cnt++] = (op){OP_DEL, a[i]};
        for (size_t i = 0; i < m; i++) ops[cnt++] = (op){OP_INS, b[i]};
        for (size_t i = 0; i < trace_len; i++) free(trace[i]);
        free(trace);
        free(v);
        *out_len = cnt;
        return ops;
    }

    int x = (int)n, y = (int)m;
    for (int d = found_d; d >= 0; d--) {
        int *vt = trace[d];
        int k = x - y;
        int prev_k;
        if (k == -d || (k != d && vt[k - 1 + offset] < vt[k + 1 + offset]))
            prev_k = k + 1;
        else
            prev_k = k - 1;
        int prev_x = vt[prev_k + offset];
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            ops[cnt++] = (op){OP_EQ, a[x - 1]};
            x--;
            y--;
        }
        if (d > 0) {
            if (x == prev_x) {
                ops[cnt++] = (op){OP_INS, b[y - 1]};
                y--;
            } else {
                ops[cnt++] = (op){OP_DEL, a[x - 1]};
                x--;
            }
        }
    }

    for (size_t i = 0; i < trace_len; i++) free(trace[i]);
    free(trace);
    free(v);

    for (size_t i = 0, j = cnt - 1; i < j; i++, j--) {
        op tmp = ops[i];
        ops[i] = ops[j];
        ops[j] = tmp;
    }
    *out_len = cnt;
    return ops;
}

static void add_range(char *dst, size_t cap, size_t start, size_t count) {
    if (count == 0) snprintf(dst, cap, "%zu,0", start - 1);
    else snprintf(dst, cap, "%zu,%zu", start, count);
}

char *tiny_unified_diff(const char *path, const char *a, size_t alen,
                        const char *b, size_t blen) {
    size_t na, nb;
    line *la = split_lines(a, alen, &na);
    line *lb = split_lines(b, blen, &nb);
    size_t on;
    op *ops = myers(la, na, lb, nb, &on);

    size_t *opref = xmalloc((on + 1) * sizeof *opref);
    size_t *npref = xmalloc((on + 1) * sizeof *npref);
    opref[0] = npref[0] = 0;
    for (size_t i = 0; i < on; i++) {
        opref[i + 1] = opref[i] + (ops[i].kind == OP_EQ || ops[i].kind == OP_DEL);
        npref[i + 1] = npref[i] + (ops[i].kind == OP_EQ || ops[i].kind == OP_INS);
    }

    tiny_buf out = {0};
    buf_adds(&out, "Index: ");
    buf_adds(&out, path);
    buf_adds(&out, "\n===================================================================\n--- ");
    buf_adds(&out, path);
    buf_adds(&out, "\n+++ ");
    buf_adds(&out, path);
    buf_adds(&out, "\n");

    size_t i = 0;
    while (i < on) {
        while (i < on && ops[i].kind == OP_EQ) i++;
        if (i >= on) break;
        size_t block_start = i;
        size_t block_end = i;
        while (block_end < on && ops[block_end].kind != OP_EQ) block_end++;
        size_t last = block_end;
        for (;;) {
            size_t g = last;
            while (g < on && ops[g].kind == OP_EQ) g++;
            if (g >= on) break;
            if (g - last > 2 * CONTEXT) break;
            size_t b2 = g;
            while (b2 < on && ops[b2].kind != OP_EQ) b2++;
            last = b2;
        }
        size_t hs = block_start >= CONTEXT ? block_start - CONTEXT : 0;
        size_t he = last + CONTEXT <= on ? last + CONTEXT : on;

        char r1[32], r2[32], hdr[96];
        add_range(r1, sizeof r1, opref[hs] + 1, opref[he] - opref[hs]);
        add_range(r2, sizeof r2, npref[hs] + 1, npref[he] - npref[hs]);
        snprintf(hdr, sizeof hdr, "@@ -%s +%s @@\n", r1, r2);
        buf_adds(&out, hdr);

        for (size_t k = hs; k < he; k++) {
            char prefix = ops[k].kind == OP_DEL ? '-' : ops[k].kind == OP_INS ? '+' : ' ';
            buf_add(&out, &prefix, 1);
            buf_add(&out, ops[k].l.p, ops[k].l.len);
            buf_adds(&out, "\n");
            if (!ops[k].l.nl) buf_adds(&out, "\\ No newline at end of file\n");
        }
        i = he;
    }

    free(opref);
    free(npref);
    free(ops);
    free(la);
    free(lb);
    return out.data;
}

// ---- diffs between snapshots ----------------------------------------------

static const char *note_base(int kind) {
    if (kind == FS_BINARY) return "binary file";
    if (kind == FS_LARGE) return "large file";
    return "file not tracked";
}

static char *note_text(int kind, const char *verb) {
    const char *base = note_base(kind);
    char *out = xmalloc(strlen(base) + strlen(verb) + 2);
    sprintf(out, "%s %s", base, verb);
    return out;
}

// Fills `*out` and returns true when the file changed; false means no change.
static bool diff_one(const char *path, const file_state *before,
                     const file_state *after, tiny_file_diff *out) {
    out->path = xstrdup(path);
    out->patch = NULL;
    out->note = NULL;

    if (before == NULL) {
        if (after->kind == FS_LINK) {
            out->note = xstrdup("symlink created");
        } else if (after->content == NULL) {
            out->note = note_text(after->kind, "created");
        } else {
            out->patch = tiny_unified_diff(path, "", 0, after->content, after->size);
        }
        return true;
    }
    if (after == NULL) {
        if (before->kind == FS_LINK) {
            out->note = xstrdup("symlink removed");
        } else if (before->content == NULL) {
            out->note = note_text(before->kind, "deleted");
        } else {
            out->patch = tiny_unified_diff(path, before->content, before->size, "", 0);
        }
        return true;
    }
    if (before->kind == FS_LINK || after->kind == FS_LINK) {
        if (before->ino == after->ino && before->size == after->size &&
            before->mtime.tv_sec == after->mtime.tv_sec &&
            before->mtime.tv_nsec == after->mtime.tv_nsec)
            return false;
        out->note = xstrdup("symlink changed");
        return true;
    }
    if (before->content != NULL && after->content != NULL) {
        if (before->size == after->size && memcmp(before->content, after->content, before->size) == 0)
            return false;
        out->patch = tiny_unified_diff(path, before->content, before->size, after->content, after->size);
        return true;
    }
    if (before->size == after->size && before->ino == after->ino &&
        before->mtime.tv_sec == after->mtime.tv_sec && before->mtime.tv_nsec == after->mtime.tv_nsec)
        return false;
    int kind = after->content == NULL ? after->kind : before->kind;
    out->note = note_text(kind, "changed");
    return true;
}

tiny_file_diff *tiny_snapshot_diff(const struct tiny_snapshot *before,
                                   const struct tiny_snapshot *after, size_t *len) {
    tiny_file_diff *out = NULL;
    size_t cap = 0, n = 0;
    size_t i = 0, j = 0;
    while (i < before->len || j < after->len) {
        const char *path;
        int cmp;
        if (i >= before->len) cmp = 1;
        else if (j >= after->len) cmp = -1;
        else cmp = strcmp(before->entries[i].path, after->entries[j].path);

        tiny_file_diff d;
        bool changed;
        if (cmp < 0) {
            path = before->entries[i].path;
            changed = diff_one(path, before->entries[i].state, NULL, &d);
            i++;
        } else if (cmp > 0) {
            path = after->entries[j].path;
            changed = diff_one(path, NULL, after->entries[j].state, &d);
            j++;
        } else {
            path = before->entries[i].path;
            changed = diff_one(path, before->entries[i].state, after->entries[j].state, &d);
            i++;
            j++;
        }
        if (!changed) {
            free(d.path);
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            out = realloc(out, cap * sizeof *out);
            if (!out) abort();
        }
        out[n++] = d;
    }
    *len = n;
    return out;
}

void tiny_file_diffs_free(tiny_file_diff *diffs, size_t len) {
    if (!diffs) return;
    for (size_t i = 0; i < len; i++) {
        free(diffs[i].path);
        free(diffs[i].patch);
        free(diffs[i].note);
    }
    free(diffs);
}
