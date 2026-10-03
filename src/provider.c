#include "provider.h"

#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "api.h"
#include "util.h"

void tiny_json_tool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const tiny_tool *t,
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

void tiny_emit_delta(tiny_stream_fn emit, void *user, tiny_stream_type type, const char *delta) {
    if (!emit) return;
    tiny_stream_event ev = {.type = type, .delta = delta};
    emit(&ev, user);
}

void tiny_emit_tool_start(tiny_stream_fn emit, void *user, const char *id, const char *name) {
    if (!emit) return;
    tiny_stream_event ev = {
        .type = TINY_STREAM_TOOL_CALL_START,
        .tool_call_id = id,
        .tool_name = name,
    };
    emit(&ev, user);
}

void tiny_add_text_block(tiny_message *out, tiny_block_type type, const char *text) {
    if (!text || !text[0]) return;
    tiny_block *b = tiny_message_add_block(out, type);
    b->text = xstrdup(text);
}

void tiny_add_tool_block(tiny_message *out, const char *id, const char *name,
                         const char *args, tiny_stream_fn emit, void *user) {
    tiny_block *b = tiny_message_add_block(out, TINY_BLOCK_TOOL_CALL);
    b->tool_call_id = xstrdup(id ? id : "");
    b->tool_name = xstrdup(name ? name : "");
    b->arguments = xstrdup(args ? args : "{}");
    if (!emit) return;
    tiny_stream_event ev = {
        .type = TINY_STREAM_TOOL_CALL,
        .tool_call_id = b->tool_call_id,
        .tool_name = b->tool_name,
        .tool_arguments = b->arguments,
    };
    emit(&ev, user);
}

int tiny_provider_stream(const tiny_model *model, const tiny_context *ctx,
                         const tiny_stream_opts *opts,
                         tiny_stream_fn emit, void *user,
                         const volatile bool *cancel,
                         tiny_message *out, char **err) {
    switch (model->api) {
    case TINY_API_OPENAI_COMPLETIONS:
        return tiny_openai_completions_stream(model, ctx, opts, emit, user, cancel, out, err);
    case TINY_API_OPENAI_RESPONSES:
        return tiny_openai_responses_stream(model, ctx, opts, emit, user, cancel, out, err);
    case TINY_API_ANTHROPIC_MESSAGES:
        return tiny_anthropic_messages_stream(model, ctx, opts, emit, user, cancel, out, err);
    case TINY_API_GOOGLE_GENERATIVE_AI:
        return tiny_google_generative_ai_stream(model, ctx, opts, emit, user, cancel, out, err);
    }
    *err = strdup("provider: unsupported api");
    return -1;
}
