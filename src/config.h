// tiny config: schema, defaults, and loading/merging of config files.

#ifndef TINY_CONFIG_H
#define TINY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TINY_THEME_TOKYO_NIGHT,
    TINY_THEME_AUTO,
} tiny_theme;

typedef enum {
    TINY_API_OPENAI_COMPLETIONS,
    TINY_API_OPENAI_RESPONSES,
    TINY_API_ANTHROPIC_MESSAGES,
    TINY_API_GOOGLE_GENERATIVE_AI,
} tiny_api;

typedef struct {
    char *id;
    char *name;      // NULL when the config omits it; callers fall back to id.
    char *base_url;
    tiny_api api;
    char **models;
    size_t models_len;
} tiny_provider;

typedef struct {
    tiny_theme theme;
    char *provider;       // NULL when unset.
    char *model;          // NULL when unset.
    char *effort;         // NULL when unset; otherwise off/minimal/low/medium/high/xhigh/max.
    char *system_prompt;  // NULL when unset.
    bool discover_agent_files;
    char **skills_dirs;
    size_t skills_dirs_len;
    char *sessions_dir;   // Always resolved.
    tiny_provider *custom_providers;
    size_t custom_providers_len;
} tiny_config;

// Loads the config. The global config is always read; when cli_path is not NULL
// its keys override the global ones. Returns NULL on failure and, if err is not
// NULL, stores a malloc'd message there (the caller frees it).
tiny_config *tiny_config_load(const char *cli_path, char **err);

// Merges the given keys into the global config file, leaving other keys
// untouched. A NULL argument leaves that key unchanged. Written atomically.
// Returns 0, or -1 with *err set.
int tiny_config_save(const char *provider, const char *model, const char *effort, char **err);

void tiny_config_free(tiny_config *cfg);

#endif
