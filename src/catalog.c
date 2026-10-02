#include "catalog.h"

#include <string.h>

extern const cage_model_info cage_catalog_entries[];
extern const size_t cage_catalog_entries_len;

const cage_model_info *cage_catalog_lookup(const char *provider, const char *model) {
    if (!provider || !model) return NULL;
    for (size_t i = 0; i < cage_catalog_entries_len; i++) {
        const cage_model_info *e = &cage_catalog_entries[i];
        if (strcmp(e->provider, provider) == 0 && strcmp(e->id, model) == 0) return e;
    }
    return NULL;
}
