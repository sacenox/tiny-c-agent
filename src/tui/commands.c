#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
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

static void run_help(cage_command_ctx *ctx, const char *args);
static void run_new(cage_command_ctx *ctx, const char *args);
static void run_provider(cage_command_ctx *ctx, const char *args);
static void run_model(cage_command_ctx *ctx, const char *args);
static void run_thinking(cage_command_ctx *ctx, const char *args);

static const cage_command COMMANDS[] = {
    {"help", "list commands and keybindings", run_help},
    {"new", "start a new session", run_new},
    {"provider", "choose the provider and model", run_provider},
    {"model", "choose a model for the current provider", run_model},
    {"thinking", "set the effort level", run_thinking},
};

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

// Hands the buffer's text to the writer and resets it.
static void emit(cage_command_ctx *ctx, tui_buf *b) {
    ctx->write(ctx->user, b->data ? buf_steal(b) : strdup(""));
    buf_free(b);
}

static void emit_str(cage_command_ctx *ctx, const char *s) {
    ctx->write(ctx->user, strdup(s));
}

// A malloc'd copy with surrounding whitespace removed.
static char *trimmed(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
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
    return b.data ? buf_steal(&b) : strdup("");
}

static void run_help(cage_command_ctx *ctx, const char *args) {
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

static void run_new(cage_command_ctx *ctx, const char *args) {
    (void)args;
    ctx->new_session(ctx->user);
    emit_str(ctx, "new session");
}

static const cage_provider *find_custom(const cage_config *cfg, const char *id) {
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
static const char *first_model(const cage_config *cfg, const char *provider) {
    const cage_provider *custom = find_custom(cfg, provider);
    if (custom) return custom->models_len > 0 ? custom->models[0] : NULL;
    const char *canonical = cage_provider_canonical(provider);
    for (size_t i = 0; i < cage_catalog_count(); i++) {
        const cage_model_info *e = cage_catalog_get(i);
        if (strcmp(e->provider, canonical) == 0) return e->id;
    }
    return NULL;
}

static void listed(cage_command_ctx *ctx, const char *label) {
    emit_str(ctx, label);
}

static void run_provider(cage_command_ctx *ctx, const char *args) {
    char *id = trimmed(args);
    if (id[0] == '\0') {
        free(id);
        listed(ctx, "providers");
        for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) {
            tui_buf b;
            buf_init(&b);
            buf_adds(&b, ctx->cfg->provider && strcmp(ctx->cfg->provider, BUILTINS[i].id) == 0 ? "* " : "  ");
            buf_adds(&b, BUILTINS[i].id);
            buf_adds(&b, "  ");
            buf_adds(&b, BUILTINS[i].name);
            emit(ctx, &b);
        }
        for (size_t i = 0; i < ctx->cfg->custom_providers_len; i++) {
            const cage_provider *p = &ctx->cfg->custom_providers[i];
            tui_buf b;
            buf_init(&b);
            buf_adds(&b, ctx->cfg->provider && strcmp(ctx->cfg->provider, p->id) == 0 ? "* " : "  ");
            buf_adds(&b, p->id);
            buf_adds(&b, "  ");
            buf_adds(&b, p->name ? p->name : p->id);
            emit(ctx, &b);
        }
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

static void list_models(cage_command_ctx *ctx) {
    const char *provider = ctx->cfg->provider ? ctx->cfg->provider : "";
    const cage_provider *custom = find_custom(ctx->cfg, provider);
    if (custom) {
        for (size_t i = 0; i < custom->models_len; i++) {
            tui_buf b;
            buf_init(&b);
            buf_adds(&b, ctx->cfg->model && strcmp(ctx->cfg->model, custom->models[i]) == 0 ? "* " : "  ");
            buf_adds(&b, custom->models[i]);
            emit(ctx, &b);
        }
        return;
    }
    const char *canonical = cage_provider_canonical(provider);
    for (size_t i = 0; i < cage_catalog_count(); i++) {
        const cage_model_info *e = cage_catalog_get(i);
        if (strcmp(e->provider, canonical) != 0) continue;
        tui_buf b;
        buf_init(&b);
        buf_adds(&b, ctx->cfg->model && strcmp(ctx->cfg->model, e->id) == 0 ? "* " : "  ");
        buf_adds(&b, e->id);
        buf_adds(&b, "  ");
        buf_adds(&b, e->name);
        emit(ctx, &b);
    }
}

static void run_model(cage_command_ctx *ctx, const char *args) {
    char *id = trimmed(args);
    if (id[0] == '\0') {
        free(id);
        char label[256];
        snprintf(label, sizeof label, "models for %s", ctx->cfg->provider ? ctx->cfg->provider : "");
        listed(ctx, label);
        list_models(ctx);
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

static void run_thinking(cage_command_ctx *ctx, const char *args) {
    char *level = trimmed(args);
    if (level[0] == '\0') {
        free(level);
        const cage_model_info *info = ctx->cfg->provider && ctx->cfg->model
            ? cage_catalog_lookup(cage_provider_canonical(ctx->cfg->provider), ctx->cfg->model)
            : NULL;
        if (!info || info->effort_len == 0) {
            emit_str(ctx, "no configurable effort for this model");
            return;
        }
        listed(ctx, "effort");
        for (size_t i = 0; i < info->effort_len; i++) {
            const char *v = strcmp(info->effort[i], "none") == 0 ? "off" : info->effort[i];
            tui_buf b;
            buf_init(&b);
            buf_adds(&b, "  ");
            buf_adds(&b, v);
            emit(ctx, &b);
        }
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

const cage_command *cage_find_command(const char *text, const char **args) {
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

char *cage_complete_command(const char *draft) {
    if (draft[0] != '/') return NULL;
    for (const char *p = draft; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n') return NULL;
    const char *typed = draft + 1;
    size_t tn = strlen(typed);
    const cage_command *match = NULL;
    size_t nmatch = 0;
    char *shared = NULL;
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strncmp(COMMANDS[i].name, typed, tn) != 0) continue;
        nmatch++;
        if (!match) match = &COMMANDS[i];
        if (!shared) shared = strdup(COMMANDS[i].name);
        else {
            char *next = cage_common_prefix(shared, COMMANDS[i].name);
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
        buf_free(&b);
    } else if (nmatch > 1 && strlen(shared) > tn) {
        tui_buf b;
        buf_init(&b);
        buf_addc(&b, '/');
        buf_adds(&b, shared);
        result = b.data ? buf_steal(&b) : NULL;
        buf_free(&b);
    }
    free(shared);
    return result;
}
