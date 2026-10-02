#include "resolve.h"

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

int cage_resolve_model(const cage_config *cfg, cage_model *out, char **err) {
    if (err) *err = NULL;
    if (!cfg->provider || !cfg->provider[0])
        return fail(err, "no provider configured (set \"provider\")");
    if (!cfg->model || !cfg->model[0])
        return fail(err, "no model configured (set \"model\")");

    memset(out, 0, sizeof *out);
    out->provider = cfg->provider;
    out->model = cfg->model;

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
