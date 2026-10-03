#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "catalog.h"
#include "complete.h"
#include "resolve.h"
#include "theme.h"

// The keybindings the TUI accepts, in the order /help prints them.
static const char *const KEYBINDINGS[][2] = {
    {"Enter", "submit"},
    {"Shift+Enter", "newline (Ctrl+J also works)"},
    {"Esc", "pause the turn at the next step boundary"},
    {"Ctrl+C", "cancel the turn"},
    {"Ctrl+D", "exit on an empty draft"},
    {"Tab", "complete command or path"},
};

// The built-in providers, in the order /provider lists them.
static const struct {
    const char *id;
    const char *name;
} BUILTINS[] = {
    {"opencode-zen", "OpenCode Zen"},
    {"opencode-go", "OpenCode Go"},
};

// The effort values the config accepts, in ascending order.
static const char *const EFFORTS[] = {
    "off", "minimal", "low", "medium", "high", "xhigh", "max",
};

static void run_help(tiny_command_ctx *ctx, const char *args);
static void run_new(tiny_command_ctx *ctx, const char *args);
static void run_provider(tiny_command_ctx *ctx, const char *args);
static void run_model(tiny_command_ctx *ctx, const char *args);
static void run_thinking(tiny_command_ctx *ctx, const char *args);

static const tiny_command COMMANDS[] = {
    {"help", "list commands and keybindings", run_help},
    {"new", "start a new session", run_new},
    {"provider", "choose the provider and model", run_provider},
    {"model", "choose a model for the current provider", run_model},
    {"thinking", "set the effort level", run_thinking},
};

static void emit_str(tiny_command_ctx *ctx, const char *s) {
    ctx->write(ctx->user, strdup(s));
}

// One aligned `key  description` block; the key column is dimmed.
static char *keyed(const char *key, const char *description, size_t width) {
    tui_buf b;
    buf_init(&b);
    buf_adds(&b, "  ");
    tui_buf pad;
    buf_init(&pad);
    buf_adds(&pad, key);
    while (pad.len < width) buf_addc(&pad, ' ');
    fg_dim(&b, pad.data ? pad.data : "");
    buf_free(&pad);
    buf_adds(&b, "  ");
    buf_adds(&b, description);
    return buf_steal(&b);
}

static void run_help(tiny_command_ctx *ctx, const char *args) {
    (void)args;
    size_t width = 0;
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        char name[64];
        snprintf(name, sizeof name, "/%s", COMMANDS[i].name);
        if (strlen(name) > width) width = strlen(name);
    }
    for (size_t i = 0; i < sizeof KEYBINDINGS / sizeof KEYBINDINGS[0]; i++)
        if (strlen(KEYBINDINGS[i][0]) > width) width = strlen(KEYBINDINGS[i][0]);

    emit_str(ctx, "commands");
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        char name[64];
        snprintf(name, sizeof name, "/%s", COMMANDS[i].name);
        ctx->write(ctx->user, keyed(name, COMMANDS[i].description, width));
    }
    emit_str(ctx, "");
    emit_str(ctx, "keybindings");
    for (size_t i = 0; i < sizeof KEYBINDINGS / sizeof KEYBINDINGS[0]; i++)
        ctx->write(ctx->user, keyed(KEYBINDINGS[i][0], KEYBINDINGS[i][1], width));
}

static void run_new(tiny_command_ctx *ctx, const char *args) {
    (void)args;
    ctx->new_session(ctx->user);
    emit_str(ctx, "new session");
}

static const tiny_provider *find_custom(const tiny_config *cfg, const char *id) {
    for (size_t i = 0; i < cfg->custom_providers_len; i++)
        if (strcmp(cfg->custom_providers[i].id, id) == 0) return &cfg->custom_providers[i];
    return NULL;
}

static bool is_builtin(const char *id) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].id, id) == 0) return true;
    return false;
}

// The first model advertised for a provider, or NULL when it has none.
static const char *first_model(const tiny_config *cfg, const char *provider) {
    const tiny_provider *custom = find_custom(cfg, provider);
    if (custom) return custom->models_len > 0 ? custom->models[0] : NULL;
    const char *canonical = tiny_provider_canonical(provider);
    for (size_t i = 0; i < tiny_catalog_count(); i++) {
        const tiny_model_info *e = tiny_catalog_get(i);
        if (strcmp(e->provider, canonical) == 0) return e->id;
    }
    return NULL;
}

void tiny_choice_free(tiny_choice *c) {
    strv_free(&c->ids);
    strv_free(&c->labels);
}

static void push_option(tiny_choice *c, const char *id, const char *label) {
    strv_push(&c->ids, strdup(id));
    strv_push(&c->labels, strdup(label && label[0] ? label : id));
}

tiny_choice tiny_choose_list(const tiny_config *cfg, tiny_choose what,
                             const char *provider) {
    tiny_choice c = {0};

    if (what == TINY_CHOOSE_PROVIDER) {
        for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
            push_option(&c, BUILTINS[i].id, BUILTINS[i].name);
        for (size_t i = 0; i < cfg->custom_providers_len; i++) {
            const tiny_provider *p = &cfg->custom_providers[i];
            push_option(&c, p->id, p->name);
        }
    } else if (what == TINY_CHOOSE_MODEL) {
        const char *prov = provider ? provider : cfg->provider;
        if (prov) {
            const tiny_provider *custom = find_custom(cfg, prov);
            if (custom) {
                for (size_t i = 0; i < custom->models_len; i++)
                    push_option(&c, custom->models[i], custom->models[i]);
            } else {
                const char *canonical = tiny_provider_canonical(prov);
                for (size_t i = 0; i < tiny_catalog_count(); i++) {
                    const tiny_model_info *e = tiny_catalog_get(i);
                    if (strcmp(e->provider, canonical) == 0)
                        push_option(&c, e->id, e->name);
                }
            }
        }
    } else {
        const tiny_model_info *info = cfg->provider && cfg->model
            ? tiny_catalog_lookup(tiny_provider_canonical(cfg->provider), cfg->model)
            : NULL;
        if (info) {
            for (size_t i = 0; i < info->effort_len; i++) {
                const char *v = strcmp(info->effort[i], "none") == 0 ? "off" : info->effort[i];
                push_option(&c, v, v);
            }
        }
    }

    return c;
}

static const char *provider_name(const tiny_config *cfg, const char *id) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].id, id) == 0) return BUILTINS[i].name;
    const tiny_provider *p = find_custom(cfg, id);
    if (p) return p->name ? p->name : p->id;
    return id;
}

char *tiny_choose_title(const tiny_config *cfg, tiny_choose what,
                        const char *provider) {
    if (what == TINY_CHOOSE_PROVIDER) return strdup("Select a provider");
    if (what == TINY_CHOOSE_EFFORT) return strdup("Select a thinking level");
    const char *prov = provider ? provider : cfg->provider;
    if (!prov) return strdup("Select a model");
    const char *name = provider_name(cfg, prov);
    size_t n = strlen(name) + 32;
    char *s = xmalloc(n);
    snprintf(s, n, "Select a model for %s", name);
    return s;
}

static void run_provider(tiny_command_ctx *ctx, const char *args) {
    char *id = tiny_trim(args);
    if (id[0] == '\0') {
        free(id);
        ctx->choose(ctx->user, TINY_CHOOSE_PROVIDER);
        return;
    }
    if (!is_builtin(id) && !find_custom(ctx->cfg, id)) {
        char msg[256];
        snprintf(msg, sizeof msg, "! unknown provider: %s", id);
        emit_str(ctx, msg);
        free(id);
        return;
    }
    const char *model = first_model(ctx->cfg, id);
    if (!model) {
        char msg[256];
        snprintf(msg, sizeof msg, "! no models for provider: %s", id);
        emit_str(ctx, msg);
        free(id);
        return;
    }
    char *err = NULL;
    if (ctx->apply(ctx->user, id, model, NULL, &err) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "! %s", err ? err : "cannot switch provider");
        emit_str(ctx, msg);
        free(err);
    }
    free(id);
}

static void run_model(tiny_command_ctx *ctx, const char *args) {
    char *id = tiny_trim(args);
    if (id[0] == '\0') {
        free(id);
        ctx->choose(ctx->user, TINY_CHOOSE_MODEL);
        return;
    }
    char *err = NULL;
    if (ctx->apply(ctx->user, NULL, id, NULL, &err) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "! %s", err ? err : "cannot switch model");
        emit_str(ctx, msg);
        free(err);
    }
    free(id);
}

static void run_thinking(tiny_command_ctx *ctx, const char *args) {
    char *level = tiny_trim(args);
    if (level[0] == '\0') {
        free(level);
        ctx->choose(ctx->user, TINY_CHOOSE_EFFORT);
        return;
    }
    bool valid = false;
    for (size_t i = 0; i < sizeof EFFORTS / sizeof EFFORTS[0]; i++)
        if (strcmp(EFFORTS[i], level) == 0) valid = true;
    if (!valid) {
        char msg[256];
        snprintf(msg, sizeof msg, "! unknown effort: %s", level);
        emit_str(ctx, msg);
        free(level);
        return;
    }
    char *err = NULL;
    if (ctx->apply(ctx->user, NULL, NULL, level, &err) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "! %s", err ? err : "cannot set effort");
        emit_str(ctx, msg);
        free(err);
    }
    free(level);
}

const tiny_command *tiny_find_command(const char *text, const char **args) {
    if (text[0] != '/') return NULL;
    const char *p = text + 1;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n') n++;
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strlen(COMMANDS[i].name) == n && strncmp(COMMANDS[i].name, p, n) == 0) {
            const char *rest = p + n;
            while (*rest == ' ' || *rest == '\t') rest++;
            *args = rest;
            return &COMMANDS[i];
        }
    }
    return NULL;
}

char *tiny_complete_command(const char *draft) {
    if (draft[0] != '/') return NULL;
    for (const char *p = draft; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n') return NULL;
    const char *typed = draft + 1;
    size_t tn = strlen(typed);
    const tiny_command *match = NULL;
    size_t nmatch = 0;
    char *shared = NULL;
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strncmp(COMMANDS[i].name, typed, tn) != 0) continue;
        nmatch++;
        if (!match) match = &COMMANDS[i];
        if (!shared) shared = strdup(COMMANDS[i].name);
        else {
            char *next = tiny_common_prefix(shared, COMMANDS[i].name);
            free(shared);
            shared = next;
        }
    }
    char *result = NULL;
    if (nmatch == 1) {
        tui_buf b;
        buf_init(&b);
        buf_addc(&b, '/');
        buf_adds(&b, match->name);
        result = b.data ? buf_steal(&b) : NULL;
    } else if (nmatch > 1 && strlen(shared) > tn) {
        tui_buf b;
        buf_init(&b);
        buf_addc(&b, '/');
        buf_adds(&b, shared);
        result = b.data ? buf_steal(&b) : NULL;
    }
    free(shared);
    return result;
}
