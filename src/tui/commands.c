#include "commands.h"

#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "complete.h"
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
    size_t ncmd = 2;
    const char *names[][2] = {
        {"/help", "list commands and keybindings"},
        {"/new", "start a new session"},
    };
    size_t width = 0;
    for (size_t i = 0; i < ncmd; i++)
        if (strlen(names[i][0]) > width) width = strlen(names[i][0]);
    for (size_t i = 0; i < sizeof KEYBINDINGS / sizeof KEYBINDINGS[0]; i++)
        if (strlen(KEYBINDINGS[i][0]) > width) width = strlen(KEYBINDINGS[i][0]);

    ctx->write(ctx->user, strdup("commands"));
    for (size_t i = 0; i < ncmd; i++)
        ctx->write(ctx->user, keyed(names[i][0], names[i][1], width));
    ctx->write(ctx->user, strdup(""));
    ctx->write(ctx->user, strdup("keybindings"));
    for (size_t i = 0; i < sizeof KEYBINDINGS / sizeof KEYBINDINGS[0]; i++)
        ctx->write(ctx->user, keyed(KEYBINDINGS[i][0], KEYBINDINGS[i][1], width));
}

static void run_new(cage_command_ctx *ctx, const char *args) {
    (void)args;
    ctx->new_session(ctx->user);
    ctx->write(ctx->user, strdup("new session"));
}

static const cage_command COMMANDS[] = {
    {"help", "list commands and keybindings", run_help},
    {"new", "start a new session", run_new},
};

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
