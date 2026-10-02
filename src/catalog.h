// Model metadata generated from models.dev.

#ifndef CAGE_CATALOG_H
#define CAGE_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

// One entry of the compiled-in catalog, describing a model a built-in provider
// serves. The table itself lives in src/catalog.generated.c; regenerate it with
// `make catalog` when models.dev changes.
typedef struct {
    const char *provider;       // built-in provider id
    const char *id;             // model id sent on the wire
    const char *name;           // human-readable name
    bool supports_images;       // whether the model accepts image input
    long context_limit;         // context window in tokens, 0 when unknown
    const char *const *effort;  // accepted reasoning_effort values; NULL when the
    size_t effort_len;          // model exposes no effort option
} cage_model_info;

// Looks up provider/model. Returns NULL when the catalog has no entry, and the
// caller then falls back to its assumed defaults.
const cage_model_info *cage_catalog_lookup(const char *provider, const char *model);

#endif
