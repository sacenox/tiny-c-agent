#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#define READ_FLAGS (YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS)

// A config value the user has not written yet. The partial form keeps every
// field optional so a local config only overrides the keys it sets.
typedef struct {
    int theme;                  // -1 unset, otherwise a cage_theme.
    char *provider;
    char *model;
    char *effort;
    char *system_prompt;
    int discover_agent_files;   // -1 unset, otherwise a bool.
    char **skills_dirs;
    size_t skills_dirs_len;
    bool skills_dirs_set;
    char *sessions_dir;
    cage_provider *custom_providers;
    size_t custom_providers_len;
    bool custom_providers_set;
} partial;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) abort();
    return p;
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) abort();
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

// Builds "a/b", avoiding a doubled separator when a already ends in one.
static char *path_join(const char *a, const char *b) {
    size_t al = strlen(a);
    bool slash = al > 0 && a[al - 1] == '/';
    size_t n = al + strlen(b) + (slash ? 1 : 2);
    char *p = xmalloc(n);
    snprintf(p, n, slash ? "%s%s" : "%s/%s", a, b);
    return p;
}

static int fail(char **err, const char *path, const char *fmt, ...) {
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    size_t n = strlen(path) + strlen(detail) + 16;
    char *msg = xmalloc(n);
    snprintf(msg, n, "config %s: %s", path, detail);
    free(*err);
    *err = msg;
    return -1;
}

static void push_str(char ***arr, size_t *len, char *s) {
    *arr = xrealloc(*arr, (*len + 1) * sizeof **arr);
    (*arr)[*len] = s;
    (*len)++;
}

static char *config_file_path(void) {
    const char *base = getenv("XDG_CONFIG_HOME");
    char *dir;
    if (base && base[0]) {
        dir = path_join(base, "cage");
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return NULL;
        dir = path_join(home, ".config/cage");
    }
    char *p = path_join(dir, "config.json");
    free(dir);
    return p;
}

static char *default_sessions_dir(void) {
    const char *state = getenv("XDG_STATE_HOME");
    if (state && state[0]) return path_join(state, "cage/sessions");
    const char *home = getenv("HOME");
    if (home && home[0]) return path_join(home, ".local/state/cage/sessions");
    return xstrdup("sessions");
}

static bool valid_effort(const char *s) {
    static const char *const values[] = {
        "off", "minimal", "low", "medium", "high", "xhigh", "max",
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        if (strcmp(s, values[i]) == 0) return true;
    return false;
}

static bool parse_api(const char *s, cage_api *out) {
    if (strcmp(s, "openai-completions") == 0) *out = CAGE_API_OPENAI_COMPLETIONS;
    else if (strcmp(s, "openai-responses") == 0) *out = CAGE_API_OPENAI_RESPONSES;
    else if (strcmp(s, "anthropic-messages") == 0) *out = CAGE_API_ANTHROPIC_MESSAGES;
    else if (strcmp(s, "google-generative-ai") == 0) *out = CAGE_API_GOOGLE_GENERATIVE_AI;
    else return false;
    return true;
}

// Copies an optional string field. Absent leaves *out untouched.
static int opt_str(yyjson_val *obj, const char *key, char **out,
                   const char *path, char **err) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v) return 0;
    if (!yyjson_is_str(v)) return fail(err, path, "\"%s\" must be a string", key);
    *out = xstrdup(yyjson_get_str(v));
    return 0;
}

static int opt_bool(yyjson_val *obj, const char *key, int *out,
                    const char *path, char **err) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v) return 0;
    if (!yyjson_is_bool(v)) return fail(err, path, "\"%s\" must be a boolean", key);
    *out = yyjson_get_bool(v);
    return 0;
}

static int parse_str_array(yyjson_val *v, const char *key,
                           char ***out, size_t *out_len,
                           const char *path, char **err) {
    if (!yyjson_is_arr(v)) return fail(err, path, "\"%s\" must be an array", key);
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(v, idx, max, e) {
        if (!yyjson_is_str(e))
            return fail(err, path, "\"%s[%zu]\" must be a string", key, idx);
        push_str(out, out_len, xstrdup(yyjson_get_str(e)));
    }
    return 0;
}

static void provider_free(cage_provider *p) {
    free(p->id);
    free(p->name);
    free(p->base_url);
    for (size_t i = 0; i < p->models_len; i++) free(p->models[i]);
    free(p->models);
    memset(p, 0, sizeof *p);
}

static int parse_provider(yyjson_val *v, size_t idx, const char *path,
                          cage_provider *out, char **err) {
    if (!yyjson_is_obj(v))
        return fail(err, path, "\"customProviders[%zu]\" must be an object", idx);
    if (opt_str(v, "id", &out->id, path, err) != 0) return -1;
    if (!out->id || !out->id[0])
        return fail(err, path, "\"customProviders[%zu].id\" must be a non-empty string", idx);
    if (opt_str(v, "name", &out->name, path, err) != 0) return -1;
    if (opt_str(v, "baseUrl", &out->base_url, path, err) != 0) return -1;
    if (!out->base_url || !out->base_url[0])
        return fail(err, path, "\"customProviders[%zu].baseUrl\" must be a non-empty string", idx);

    yyjson_val *api = yyjson_obj_get(v, "api");
    if (!api || !yyjson_is_str(api))
        return fail(err, path, "\"customProviders[%zu].api\" must be a string", idx);
    if (!parse_api(yyjson_get_str(api), &out->api))
        return fail(err, path, "\"customProviders[%zu].api\" is unknown", idx);

    yyjson_val *models = yyjson_obj_get(v, "models");
    if (!models || !yyjson_is_arr(models) || yyjson_arr_size(models) == 0)
        return fail(err, path, "\"customProviders[%zu].models\" must be a non-empty array", idx);
    if (parse_str_array(models, "models", &out->models, &out->models_len, path, err) != 0)
        return -1;
    return 0;
}

static int parse_partial(yyjson_val *root, const char *path, partial *p, char **err) {
    if (!yyjson_is_obj(root))
        return fail(err, path, "root must be an object");

    yyjson_val *v = yyjson_obj_get(root, "theme");
    if (v) {
        if (!yyjson_is_str(v)) return fail(err, path, "\"theme\" must be a string");
        const char *s = yyjson_get_str(v);
        if (strcmp(s, "tokyo-night") == 0) p->theme = CAGE_THEME_TOKYO_NIGHT;
        else if (strcmp(s, "auto") == 0) p->theme = CAGE_THEME_AUTO;
        else return fail(err, path, "unknown theme \"%s\"", s);
    }

    if (opt_str(root, "provider", &p->provider, path, err) != 0) return -1;
    if (opt_str(root, "model", &p->model, path, err) != 0) return -1;
    if (opt_str(root, "systemPrompt", &p->system_prompt, path, err) != 0) return -1;
    if (opt_str(root, "sessionsDir", &p->sessions_dir, path, err) != 0) return -1;
    if (opt_bool(root, "discoverAgentFiles", &p->discover_agent_files, path, err) != 0) return -1;

    v = yyjson_obj_get(root, "effort");
    if (v) {
        if (!yyjson_is_str(v)) return fail(err, path, "\"effort\" must be a string");
        const char *s = yyjson_get_str(v);
        if (!valid_effort(s)) return fail(err, path, "unknown effort \"%s\"", s);
        p->effort = xstrdup(s);
    }

    v = yyjson_obj_get(root, "skillsDirs");
    if (v) {
        p->skills_dirs_set = true;
        if (parse_str_array(v, "skillsDirs", &p->skills_dirs, &p->skills_dirs_len, path, err) != 0)
            return -1;
    }

    v = yyjson_obj_get(root, "customProviders");
    if (v) {
        p->custom_providers_set = true;
        if (!yyjson_is_arr(v)) return fail(err, path, "\"customProviders\" must be an array");
        p->custom_providers = xcalloc(yyjson_arr_size(v), sizeof *p->custom_providers);
        size_t idx, max;
        yyjson_val *e;
        yyjson_arr_foreach(v, idx, max, e) {
            if (parse_provider(e, idx, path, &p->custom_providers[p->custom_providers_len], err) != 0)
                return -1;
            p->custom_providers_len++;
        }
    }
    return 0;
}

static int load_into(const char *path, bool required, partial *p, char **err) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!required && errno == ENOENT) return 0;
        return fail(err, path, "%s", strerror(errno));
    }
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) < 0) {
        fclose(f);
        return fail(err, path, "cannot read file");
    }
    long size = ftell(f);
    rewind(f);
    char *buf = xmalloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    yyjson_doc *doc = yyjson_read(buf, got, READ_FLAGS);
    free(buf);
    if (!doc) return fail(err, path, "invalid JSON");
    int rc = parse_partial(yyjson_doc_get_root(doc), path, p, err);
    yyjson_doc_free(doc);
    return rc;
}

static void merge_str(char **dst, char **src) {
    if (!*src) return;
    free(*dst);
    *dst = *src;
    *src = NULL;
}

static void merge_partial(partial *dst, partial *src) {
    if (src->theme != -1) dst->theme = src->theme;
    merge_str(&dst->provider, &src->provider);
    merge_str(&dst->model, &src->model);
    merge_str(&dst->effort, &src->effort);
    merge_str(&dst->system_prompt, &src->system_prompt);
    merge_str(&dst->sessions_dir, &src->sessions_dir);
    if (src->discover_agent_files != -1) dst->discover_agent_files = src->discover_agent_files;
    if (src->skills_dirs_set) {
        for (size_t i = 0; i < dst->skills_dirs_len; i++) free(dst->skills_dirs[i]);
        free(dst->skills_dirs);
        dst->skills_dirs = src->skills_dirs;
        dst->skills_dirs_len = src->skills_dirs_len;
        src->skills_dirs = NULL;
        src->skills_dirs_len = 0;
    }
    if (src->custom_providers_set) {
        for (size_t i = 0; i < dst->custom_providers_len; i++) provider_free(&dst->custom_providers[i]);
        free(dst->custom_providers);
        dst->custom_providers = src->custom_providers;
        dst->custom_providers_len = src->custom_providers_len;
        src->custom_providers = NULL;
        src->custom_providers_len = 0;
        src->custom_providers_set = false;
    }
}

static void partial_free(partial *p) {
    free(p->provider);
    free(p->model);
    free(p->effort);
    free(p->system_prompt);
    free(p->sessions_dir);
    for (size_t i = 0; i < p->skills_dirs_len; i++) free(p->skills_dirs[i]);
    free(p->skills_dirs);
    for (size_t i = 0; i < p->custom_providers_len; i++) provider_free(&p->custom_providers[i]);
    free(p->custom_providers);
}

static void config_free_members(cage_config *cfg) {
    free(cfg->provider);
    free(cfg->model);
    free(cfg->effort);
    free(cfg->system_prompt);
    free(cfg->sessions_dir);
    for (size_t i = 0; i < cfg->skills_dirs_len; i++) free(cfg->skills_dirs[i]);
    free(cfg->skills_dirs);
    for (size_t i = 0; i < cfg->custom_providers_len; i++) provider_free(&cfg->custom_providers[i]);
    free(cfg->custom_providers);
}

cage_config *cage_config_load(const char *cli_path, char **err) {
    if (err) *err = NULL;

    partial global = {.theme = -1, .discover_agent_files = -1};
    partial local = {.theme = -1, .discover_agent_files = -1};
    char *global_path = config_file_path();

    int rc = load_into(global_path, false, &global, err);
    free(global_path);
    if (rc == 0 && cli_path) rc = load_into(cli_path, true, &local, err);
    if (rc != 0) {
        partial_free(&global);
        partial_free(&local);
        return NULL;
    }
    merge_partial(&global, &local);

    cage_config *cfg = xcalloc(1, sizeof *cfg);
    cfg->theme = global.theme == -1 ? CAGE_THEME_TOKYO_NIGHT : (cage_theme)global.theme;
    cfg->provider = global.provider;
    cfg->model = global.model;
    cfg->effort = global.effort;
    cfg->system_prompt = global.system_prompt;
    cfg->discover_agent_files = global.discover_agent_files == -1 ? true : global.discover_agent_files;
    cfg->skills_dirs = global.skills_dirs;
    cfg->skills_dirs_len = global.skills_dirs_len;
    cfg->sessions_dir = global.sessions_dir ? global.sessions_dir : default_sessions_dir();
    cfg->custom_providers = global.custom_providers;
    cfg->custom_providers_len = global.custom_providers_len;

    global.provider = global.model = global.effort = NULL;
    global.system_prompt = global.sessions_dir = NULL;
    global.skills_dirs = NULL;
    global.skills_dirs_len = 0;
    global.custom_providers = NULL;
    global.custom_providers_len = 0;

    partial_free(&global);
    partial_free(&local);
    return cfg;
}

void cage_config_free(cage_config *cfg) {
    if (!cfg) return;
    config_free_members(cfg);
    free(cfg);
}
