#include "resources.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

// ---- small helpers --------------------------------------------------------

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// ---- skills ---------------------------------------------------------------

// Pulls `name` and `description` from a `---`-fenced frontmatter block. Both
// outputs are malloc'd and NULL when absent.
static void frontmatter(const char *text, char **name, char **description) {
    *name = *description = NULL;
    if (strncmp(text, "---", 3) != 0) return;
    const char *p = text + 3;
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    const char *end = strstr(p, "\n---");
    if (!end) return;
    for (const char *line = p; line < end;) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t len = nl ? (size_t)(nl - line) : (size_t)(end - line);
        const char *colon = memchr(line, ':', len);
        if (colon) {
            size_t klen = (size_t)(colon - line);
            while (klen > 0 && line[klen - 1] == ' ') klen--;
            const char *v = colon + 1;
            size_t vlen = len - (size_t)(v - line);
            while (vlen > 0 && (*v == ' ' || *v == '\t')) v++, vlen--;
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\r')) vlen--;
            if (vlen >= 2 && (v[0] == '"' || v[0] == '\'') && v[vlen - 1] == v[0]) v++, vlen -= 2;
            if (klen == 4 && strncmp(line, "name", 4) == 0) {
                free(*name);
                *name = xmalloc(vlen + 1);
                memcpy(*name, v, vlen);
                (*name)[vlen] = '\0';
            } else if (klen == 11 && strncmp(line, "description", 11) == 0) {
                free(*description);
                *description = xmalloc(vlen + 1);
                memcpy(*description, v, vlen);
                (*description)[vlen] = '\0';
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
}

static void push_skill(tiny_resources *r, tiny_skill s) {
    r->skills = realloc(r->skills, (r->skills_len + 1) * sizeof *r->skills);
    if (!r->skills) abort();
    r->skills[r->skills_len++] = s;
}

static void discover_skills(tiny_resources *r, const tiny_config *cfg) {
    for (size_t i = 0; i < cfg->skills_dirs_len; i++) {
        DIR *d = opendir(cfg->skills_dirs[i]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char *dir = tiny_path_join(cfg->skills_dirs[i], ent->d_name);
            char *path = tiny_path_join(dir, "SKILL.md");
            free(dir);
            if (file_exists(path)) {
                char *text = tiny_read_file(path);
                if (text) {
                    char *name = NULL, *desc = NULL;
                    frontmatter(text, &name, &desc);
                    if (name)
                        push_skill(r, (tiny_skill){name, desc ? desc : xstrdup(""), path});
                    else
                        free(desc);
                    free(text);
                    if (name) continue;
                }
            }
            free(path);
        }
        closedir(d);
    }
}

// ---- agent files ----------------------------------------------------------

static void push_agent_file(tiny_resources *r, char *path) {
    r->agent_files = realloc(r->agent_files, (r->agent_files_len + 1) * sizeof *r->agent_files);
    if (!r->agent_files) abort();
    r->agent_files[r->agent_files_len++].path = path;
}

static void discover_agent_files(tiny_resources *r, const char *cwd) {
    static const char *const names[] = {"AGENTS.md", "CLAUDE.md"};
    char *roots[2] = {NULL, NULL};
    const char *home = getenv("HOME");
    if (home && home[0]) roots[0] = tiny_path_join(home, ".agents");
    roots[1] = xstrdup(cwd ? cwd : ".");
    for (size_t ri = 0; ri < 2; ri++) {
        if (!roots[ri]) continue;
        for (size_t ni = 0; ni < sizeof names / sizeof names[0]; ni++) {
            char *path = tiny_path_join(roots[ri], names[ni]);
            if (file_exists(path)) push_agent_file(r, path);
            else free(path);
        }
        free(roots[ri]);
    }
}

// ---- build ----------------------------------------------------------------

tiny_resources *tiny_resources_build(const tiny_config *cfg, const char *cwd, char **err) {
    if (err) *err = NULL;
    tiny_resources *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);

    tiny_buf prompt = {0};
    if (cfg->system_prompt && cfg->system_prompt[0]) {
        char *text = tiny_read_file(cfg->system_prompt);
        if (!text) {
            size_t n = strlen(cfg->system_prompt) + 32;
            *err = xmalloc(n);
            if (*err) snprintf(*err, n, "cannot read %s", cfg->system_prompt);
            tiny_resources_free(r);
            free(prompt.data);
            return NULL;
        }
        char *trimmed = tiny_trim(text);
        buf_adds(&prompt, trimmed);
        free(trimmed);
        free(text);
    }

    if (cfg->skills_dirs_len > 0) {
        discover_skills(r, cfg);
        if (r->skills_len > 0) {
            if (prompt.len) buf_adds(&prompt, "\n\n");
            buf_adds(&prompt, "## Skills\n\n");
            for (size_t i = 0; i < r->skills_len; i++) {
                buf_adds(&prompt, "- ");
                buf_adds(&prompt, r->skills[i].name);
                buf_adds(&prompt, ": ");
                buf_adds(&prompt, r->skills[i].description);
                buf_adds(&prompt, " (");
                buf_adds(&prompt, r->skills[i].path);
                buf_adds(&prompt, ")\n");
            }
        }
    }

    if (cfg->discover_agent_files) {
        discover_agent_files(r, cwd);
        for (size_t i = 0; i < r->agent_files_len; i++) {
            char *text = tiny_read_file(r->agent_files[i].path);
            if (!text) continue;
            char *trimmed = tiny_trim(text);
            if (prompt.len) buf_adds(&prompt, "\n\n");
            buf_adds(&prompt, "## ");
            buf_adds(&prompt, r->agent_files[i].path);
            buf_adds(&prompt, "\n\n");
            buf_adds(&prompt, trimmed);
            free(trimmed);
            free(text);
        }
    }

    r->system_prompt = prompt.data ? prompt.data : xstrdup("");
    return r;
}

void tiny_resources_free(tiny_resources *r) {
    if (!r) return;
    free(r->system_prompt);
    for (size_t i = 0; i < r->agent_files_len; i++) free(r->agent_files[i].path);
    free(r->agent_files);
    for (size_t i = 0; i < r->skills_len; i++) {
        free(r->skills[i].name);
        free(r->skills[i].description);
        free(r->skills[i].path);
    }
    free(r->skills);
    free(r);
}