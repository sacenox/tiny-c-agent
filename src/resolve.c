#include "resolve.h"

#include "catalog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char **err, const char *fmt, ...) {
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    size_t n = strlen(detail) + 1;
    *err = malloc(n);
    if (*err) memcpy(*err, detail, n);
    return -1;
}

const char *cage_provider_canonical(const char *provider) {
    if (provider && strcmp(provider, "opencode-zen") == 0) return "opencode";
    return provider;
}

int cage_resolve_model(const cage_config *cfg, cage_model *out, char **err) {
    if (err) *err = NULL;
    if (!cfg->provider || !cfg->provider[0])
        return fail(err, "no provider configured (set \"provider\")");
    if (!cfg->model || !cfg->model[0])
        return fail(err, "no model configured (set \"model\")");

    memset(out, 0, sizeof *out);
    out->provider = cfg->provider;
    out->model = cfg->model;
    // Built-in models carry their capability in the compiled catalog. Custom
    // providers are uncatalogued, so assume image input; a model that rejects
    // images surfaces the provider error as-is.
    const cage_model_info *info = cage_catalog_lookup(cage_provider_canonical(cfg->provider), cfg->model);
    out->supports_images = info ? info->supports_images : true;
    out->effort = cage_catalog_clamp_effort(info, cfg->effort);

    if (strcmp(cfg->provider, "opencode-go") == 0) {
        out->base_url = "https://opencode.ai/zen/go/v1";
        out->api = CAGE_API_OPENAI_COMPLETIONS;
        out->api_key = getenv("OPENCODE_API_KEY");
        out->session_header = "x-opencode-session";
        return 0;
    }
    if (strcmp(cfg->provider, "opencode") == 0 || strcmp(cfg->provider, "opencode-zen") == 0) {
        out->base_url = "https://opencode.ai/zen/v1";
        out->api = CAGE_API_OPENAI_COMPLETIONS;
        out->api_key = getenv("OPENCODE_API_KEY");
        out->session_header = "x-opencode-session";
        return 0;
    }
    for (size_t i = 0; i < cfg->custom_providers_len; i++) {
        const cage_provider *p = &cfg->custom_providers[i];
        if (strcmp(p->id, cfg->provider) == 0) {
            out->base_url = p->base_url;
            out->api = p->api;
            return 0;
        }
    }
    return fail(err, "unknown provider \"%s\"", cfg->provider);
}
