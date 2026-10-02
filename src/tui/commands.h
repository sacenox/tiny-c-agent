// cage TUI commands: slash commands the editor submits.

#ifndef CAGE_TUI_COMMANDS_H
#define CAGE_TUI_COMMANDS_H

#include <stddef.h>

// What a command may reach. The TUI provides the callbacks.
typedef struct {
    void *user;                              // opaque, the TUI
    void (*write)(void *user, char *line);   // commits a styled line, taking ownership
    void (*new_session)(void *user);         // starts a fresh session
} cage_command_ctx;

typedef struct {
    const char *name;         // no leading slash, lowercase
    const char *description;  // one line, shown by /help
    void (*run)(cage_command_ctx *ctx, const char *args);
} cage_command;

// `/name` for a known name, else NULL. `*args` receives the text after it.
const cage_command *cage_find_command(const char *text, const char **args);

// Tab completion for a half-typed command name; NULL leaves the draft alone.
char *cage_complete_command(const char *draft);

#endif
