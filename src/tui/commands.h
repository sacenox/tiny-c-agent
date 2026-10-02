// cage TUI commands: slash commands the editor submits.

#ifndef CAGE_TUI_COMMANDS_H
#define CAGE_TUI_COMMANDS_H

#include <stddef.h>

#include "config.h"

// The option sets a command asks the TUI to present.
typedef enum {
    CAGE_CHOOSE_PROVIDER,
    CAGE_CHOOSE_MODEL,
    CAGE_CHOOSE_EFFORT,
} cage_choose;

// What a command may reach. The TUI provides the callbacks.
typedef struct {
    void *user;                              // opaque, the TUI
    void (*write)(void *user, char *line);   // commits a styled line, taking ownership
    void (*new_session)(void *user);         // starts a fresh session
    // Presents `what` as a numbered list; the TUI routes the next submitted
    // line back as the choice.
    void (*choose)(void *user, cage_choose what);
    const cage_config *cfg;                  // the live config, for listing
    // Applies a provider/model/effort change to the running session and
    // persists it. A NULL argument leaves that field unchanged. Returns 0, or
    // -1 with *err set.
    int (*apply)(void *user, const char *provider, const char *model,
                 const char *effort, char **err);
} cage_command_ctx;

// Builds the options a chooser presents as malloc'd parallel arrays: `*ids`
// are the values applied, `*labels` are shown. `provider` overrides
// cfg->provider for CAGE_CHOOSE_MODEL. Returns the count, 0 when there is
// nothing to offer; the caller frees each string and both arrays.
size_t cage_choose_list(const cage_config *cfg, cage_choose what,
                        const char *provider, char ***ids, char ***labels);

// The one-line heading above a chooser's options, malloc'd. `provider`
// overrides cfg->provider for CAGE_CHOOSE_MODEL, naming the provider chosen.
char *cage_choose_title(const cage_config *cfg, cage_choose what,
                        const char *provider);

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
