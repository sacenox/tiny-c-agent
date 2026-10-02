// cage resources: the system prompt and the files it is built from.

#ifndef CAGE_RESOURCES_H
#define CAGE_RESOURCES_H

#include <stddef.h>

#include "config.h"

// One discovered AGENTS.md/CLAUDE.md.
typedef struct {
    char *path;
} cage_agent_file;

// One SKILL.md, with the name and description taken from its frontmatter.
typedef struct {
    char *name;
    char *description;
    char *path;
} cage_skill;

typedef struct {
    char *system_prompt;           // always set, possibly empty
    cage_agent_file *agent_files;
    size_t agent_files_len;
    cage_skill *skills;
    size_t skills_len;
} cage_resources;

// Builds the system prompt from the configured prompt file, the discovered
// SKILL.md files, and the discovered agent files, in that order. `cwd` is the
// working directory searched for agent files. Returns NULL with *err set on
// failure (an unreadable configured prompt file).
cage_resources *cage_resources_build(const cage_config *cfg, const char *cwd, char **err);

void cage_resources_free(cage_resources *r);

#endif
