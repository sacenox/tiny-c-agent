#include "provider.h"

#include <stdlib.h>
#include <string.h>

#include "api.h"

int cage_provider_stream(const cage_model *model, const cage_context *ctx,
                         const cage_stream_opts *opts,
                         cage_stream_fn emit, void *user,
                         const volatile bool *cancel,
                         cage_message *out, char **err) {
    switch (model->api) {
    case CAGE_API_OPENAI_COMPLETIONS:
        return cage_openai_completions_stream(model, ctx, opts, emit, user, cancel, out, err);
    case CAGE_API_OPENAI_RESPONSES:
        return cage_openai_responses_stream(model, ctx, opts, emit, user, cancel, out, err);
    case CAGE_API_ANTHROPIC_MESSAGES:
        return cage_anthropic_messages_stream(model, ctx, opts, emit, user, cancel, out, err);
    case CAGE_API_GOOGLE_GENERATIVE_AI:
        return cage_google_generative_ai_stream(model, ctx, opts, emit, user, cancel, out, err);
    }
    *err = strdup("provider: unsupported api");
    return -1;
}
