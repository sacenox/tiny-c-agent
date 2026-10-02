// cage config: schema, defaults, and loading/merging of config files.

#ifndef CAGE_CONFIG_H
#define CAGE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CAGE_THEME_TOKYO_NIGHT,
    CAGE_THEME_AUTO,
} cage_theme;

typedef enum {
    CAGE_API_OPENAI_COMPLETIONS,
    CAGE_API_OPENAI_RESPONSES,
    CAGE_API_ANTHROPIC_MESSAGES,
    CAGE_API_GOOGLE_GENERATIVE_AI,
} cage_api;

typedef struct {
    char *id;
    char *name;      // NULL when the config omits it; callers fall back to id.
    char *base_url;
    cage_api api;
    char **models;
    size_t models_len;
} cage_provider;

typedef struct {
    cage_theme theme;
    char *provider;       // NULL when unset.
    char *model;          // NULL when unset.
    char *effort;         // NULL when unset; otherwise off/minimal/low/medium/high/xhigh/max.
    char *system_prompt;  // NULL when unset.
    bool discover_agent_files;
    char **skills_dirs;
    size_t skills_dirs_len;
    char *sessions_dir;   // Always resolved.
    cage_provider *custom_providers;
    size_t custom_providers_len;
} cage_config;

// Loads the config. The global config is always read; when cli_path is not NULL
// its keys override the global ones. Returns NULL on failure and, if err is not
// NULL, stores a malloc'd message there (the caller frees it).
cage_config *cage_config_load(const char *cli_path, char **err);

// Merges the given keys into the global config file, leaving other keys
// untouched. A NULL argument leaves that key unchanged. Written atomically.
// Returns 0, or -1 with *err set.
int cage_config_save(const char *provider, const char *model, const char *effort, char **err);

void cage_config_free(cage_config *cfg);

#endif
