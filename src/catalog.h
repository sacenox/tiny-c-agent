// Model metadata generated from models.dev.

#ifndef TINY_CATALOG_H
#define TINY_CATALOG_H

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
    long max_output;            // maximum reply tokens, 0 when unknown
    const char *const *effort;  // accepted reasoning_effort values; NULL when the
    size_t effort_len;          // model exposes no effort option
} tiny_model_info;

// Looks up provider/model. Returns NULL when the catalog has no entry, and the
// caller then falls back to its assumed defaults.
const tiny_model_info *tiny_catalog_lookup(const char *provider, const char *model);

// Iteration over the compiled-in table, for listing a provider's models.
size_t tiny_catalog_count(void);
const tiny_model_info *tiny_catalog_get(size_t index);

// Clamps a desired effort to what `info` accepts, preferring the next higher
// level when the exact value is unavailable. Returns a level from the entry's
// own list, or NULL when the desired effort is NULL. When `info` is NULL the
// desired value is returned unchanged (unknown model).
const char *tiny_catalog_clamp_effort(const tiny_model_info *info, const char *desired);

#endif
