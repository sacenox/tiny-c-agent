#include "complete.h"

#include "buf.h"

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *cage_common_prefix(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    char *out = malloc(i + 1);
    if (!out) abort();
    memcpy(out, a, i);
    out[i] = '\0';
    return out;
}

// A word must carry path syntax to be completed: a leading `~`, `./`, `../`,
// `.`/`..`, `/`, or a word segment followed by a slash.
static bool path_like(const char *w) {
    if (w[0] == '~' || w[0] == '/') return true;
    if (w[0] == '.' && (w[1] == '\0' || w[1] == '/' ||
                        (w[1] == '.' && (w[2] == '\0' || w[2] == '/'))))
        return true;
    const char *slash = strchr(w, '/');
    if (slash && slash != w) return true;
    return false;
}

char *cage_complete_path(const char *word, const char *cwd) {
    if (!path_like(word)) return NULL;

    const char *slashp = strrchr(word, '/');
    const char *prefix = slashp ? slashp + 1 : word;
    size_t prefix_len = strlen(prefix);

    // Locate the directory to list.
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    if (word[0] == '~' && home) {
        size_t n = slashp ? (size_t)(slashp - (word + 1)) : strlen(word + 1);
        if (n >= sizeof dir) return NULL;
        snprintf(dir, sizeof dir, "%s%.*s", home, (int)n, word + 1);
    } else if (slashp) {
        size_t n = (size_t)(slashp - word);
        if (n == 0) {
            snprintf(dir, sizeof dir, "/");
        } else if (word[0] == '/') {
            if (n >= sizeof dir) return NULL;
            snprintf(dir, sizeof dir, "%.*s", (int)n, word);
        } else {
            if (n + strlen(cwd) + 2 >= sizeof dir) return NULL;
            snprintf(dir, sizeof dir, "%s/%.*s", cwd, (int)n, word);
        }
    } else {
        snprintf(dir, sizeof dir, "%s", cwd);
    }

    DIR *d = opendir(dir);
    if (!d) return NULL;

    // Candidates: matching names, dirs grown a trailing slash.
    char **cands = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (name[0] == '.' && prefix[0] != '.') continue;
        char path[PATH_MAX];
        bool isdir = false;
        if (snprintf(path, sizeof path, "%s/%s", dir, name) < (int)sizeof path) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) isdir = true;
        }
        size_t nl = strlen(name);
        char *cand = malloc(nl + 2);
        if (!cand) abort();
        memcpy(cand, name, nl);
        if (isdir) cand[nl++] = '/';
        cand[nl] = '\0';
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            cands = realloc(cands, cap * sizeof *cands);
            if (!cands) abort();
        }
        cands[n++] = cand;
    }
    closedir(d);

    char *result = NULL;
    // The bytes before the prefix; equal to the word length minus the prefix.
    size_t keep = strlen(word) - prefix_len;
    if (n == 1) {
        if (strcmp(cands[0], prefix) != 0) {
            tui_buf b;
            buf_init(&b);
            buf_add(&b, word, keep);
            buf_adds(&b, cands[0]);
            result = b.data ? buf_steal(&b) : NULL;
            buf_free(&b);
        }
    } else if (n > 1) {
        char *shared = strdup(cands[0]);
        for (size_t i = 1; i < n; i++) {
            char *next = cage_common_prefix(shared, cands[i]);
            free(shared);
            shared = next;
        }
        if (strlen(shared) > prefix_len) {
            tui_buf b;
            buf_init(&b);
            buf_add(&b, word, keep);
            buf_adds(&b, shared);
            result = b.data ? buf_steal(&b) : NULL;
            buf_free(&b);
        }
        free(shared);
    }
    for (size_t i = 0; i < n; i++) free(cands[i]);
    free(cands);
    return result;
}
