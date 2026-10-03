#include "provider.h"

#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "api.h"
#include "util.h"

void cage_json_tool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const cage_tool *t,
                    const char *params_key) {
    yyjson_mut_obj_add_strcpy(doc, obj, "name", t->name);
    yyjson_mut_obj_add_strcpy(doc, obj, "description", t->description ? t->description : "");
    const char *params = t->parameters ? t->parameters : "{}";
    yyjson_doc *pdoc = yyjson_read(params, strlen(params), 0);
    if (!pdoc) return;
    yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(pdoc));
    yyjson_mut_obj_add_val(doc, obj, params_key, copy);
    yyjson_doc_free(pdoc);
}

void cage_emit_delta(cage_stream_fn emit, void *user, cage_stream_type type, const char *delta) {
    if (!emit) return;
    cage_stream_event ev = {.type = type, .delta = delta};
    emit(&ev, user);
}

void cage_add_text_block(cage_message *out, cage_block_type type, const char *text) {
    if (!text || !text[0]) return;
    cage_block *b = cage_message_add_block(out, type);
    b->text = xstrdup(text);
}

void cage_add_tool_block(cage_message *out, const char *id, const char *name,
                         const char *args, cage_stream_fn emit, void *user) {
    cage_block *b = cage_message_add_block(out, CAGE_BLOCK_TOOL_CALL);
    b->tool_call_id = xstrdup(id ? id : "");
    b->tool_name = xstrdup(name ? name : "");
    b->arguments = xstrdup(args ? args : "{}");
    if (!emit) return;
    cage_stream_event ev = {
        .type = CAGE_STREAM_TOOL_CALL,
        .tool_call_id = b->tool_call_id,
        .tool_name = b->tool_name,
        .tool_arguments = b->arguments,
    };
    emit(&ev, user);
}

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
