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

size_t cage_catalog_count(void) { return cage_catalog_entries_len; }

const cage_model_info *cage_catalog_get(size_t index) {
    if (index >= cage_catalog_entries_len) return NULL;
    return &cage_catalog_entries[index];
}

// Rank of an effort value on the off..max ladder. Both "off" and the catalog's
// "none" sit at 0. Returns -1 for an unknown value.
static int effort_rank(const char *s) {
    static const char *const ladder[] = {
        "off", "none", "minimal", "low", "medium", "high", "xhigh", "max",
    };
    for (int i = 0; i < (int)(sizeof ladder / sizeof ladder[0]); i++)
        if (strcmp(ladder[i], s) == 0) return i <= 1 ? 0 : i - 1;
    return -1;
}

// The catalog uses models.dev's "none"; the config and wire use "off".
static const char *norm_effort(const char *v) {
    return strcmp(v, "none") == 0 ? "off" : v;
}

const char *cage_catalog_clamp_effort(const cage_model_info *info, const char *desired) {
    if (!desired) return NULL;
    if (!info) return desired;
    for (size_t i = 0; i < info->effort_len; i++)
        if (strcmp(info->effort[i], desired) == 0) return norm_effort(info->effort[i]);

    int want = effort_rank(desired);
    if (want < 0) return info->effort_len ? norm_effort(info->effort[0]) : NULL;
    // Prefer the next higher accepted level, then the next lower.
    for (int rank = want; rank <= 6; rank++)
        for (size_t i = 0; i < info->effort_len; i++)
            if (effort_rank(info->effort[i]) == rank) return norm_effort(info->effort[i]);
    for (int rank = want - 1; rank >= 0; rank--)
        for (size_t i = 0; i < info->effort_len; i++)
            if (effort_rank(info->effort[i]) == rank) return norm_effort(info->effort[i]);
    return NULL;
}
