#include "resolve.h"

#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

const char *tiny_provider_canonical(const char *provider) {
    if (provider && strcmp(provider, "opencode-zen") == 0) return "opencode";
    return provider;
}

int tiny_resolve_model(const tiny_config *cfg, tiny_model *out, char **err) {
    if (err) *err = NULL;
    if (!cfg->provider || !cfg->provider[0])
        return tiny_fail(err, "no provider configured (set \"provider\")");
    if (!cfg->model || !cfg->model[0])
        return tiny_fail(err, "no model configured (set \"model\")");

    memset(out, 0, sizeof *out);
    out->provider = cfg->provider;
    out->model = cfg->model;
    // Built-in models carry their capability in the compiled catalog. Custom
    // providers are uncatalogued, so assume image input; a model that rejects
    // images surfaces the provider error as-is.
    const tiny_model_info *info = tiny_catalog_lookup(tiny_provider_canonical(cfg->provider), cfg->model);
    out->supports_images = info ? info->supports_images : true;
    out->max_tokens = info ? info->max_output : 0;
    out->effort = tiny_catalog_clamp_effort(info, cfg->effort);

    if (strcmp(cfg->provider, "opencode-go") == 0) {
        out->base_url = "https://opencode.ai/zen/go/v1";
        out->api = TINY_API_OPENAI_COMPLETIONS;
        out->api_key = getenv("OPENCODE_API_KEY");
        out->session_header = "x-opencode-session";
        return 0;
    }
    if (strcmp(cfg->provider, "opencode") == 0 || strcmp(cfg->provider, "opencode-zen") == 0) {
        out->base_url = "https://opencode.ai/zen/v1";
        out->api = TINY_API_OPENAI_COMPLETIONS;
        out->api_key = getenv("OPENCODE_API_KEY");
        out->session_header = "x-opencode-session";
        return 0;
    }
    for (size_t i = 0; i < cfg->custom_providers_len; i++) {
        const tiny_provider *p = &cfg->custom_providers[i];
        if (strcmp(p->id, cfg->provider) == 0) {
            out->base_url = p->base_url;
            out->api = p->api;
            return 0;
        }
    }
    return tiny_fail(err, "unknown provider \"%s\"", cfg->provider);
}
