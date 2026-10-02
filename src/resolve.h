// Resolves the configured provider into a concrete endpoint and API.

#ifndef CAGE_RESOLVE_H
#define CAGE_RESOLVE_H

#include "config.h"
#include "provider.h"

// Fills `out` from the config. The strings point into `cfg` or the environment,
// so `cfg` must outlive `out`. Built-in providers use the openai-completions
// API for now; custom providers use the api written in the config.
// Returns 0, or -1 with *err set.
int cage_resolve_model(const cage_config *cfg, cage_model *out, char **err);

#endif
