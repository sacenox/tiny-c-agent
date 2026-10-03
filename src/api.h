// Internal contract between the provider dispatcher and each API adapter.

#ifndef TINY_API_H
#define TINY_API_H

#include <yyjson.h>

#include "provider.h"

// Adds a tool's "name", "description", and schema (under `params_key`) to `obj`.
// Shared by every adapter's tool serialization.
void tiny_json_tool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const tiny_tool *t,
                    const char *params_key);

// Emits one delta to the adapter's stream callback, if any.
void tiny_emit_delta(tiny_stream_fn emit, void *user, tiny_stream_type type, const char *delta);

// Appends a text/thinking block when `text` is non-NULL and non-empty.
void tiny_add_text_block(tiny_message *out, tiny_block_type type, const char *text);

// Appends a tool-call block (defaulting empty arguments to "{}") and emits the
// corresponding TINY_STREAM_TOOL_CALL event.
void tiny_add_tool_block(tiny_message *out, const char *id, const char *name,
                         const char *args, tiny_stream_fn emit, void *user);

int tiny_openai_completions_stream(const tiny_model *model, const tiny_context *ctx,
                                   const tiny_stream_opts *opts,
                                   tiny_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   tiny_message *out, char **err);

int tiny_openai_responses_stream(const tiny_model *model, const tiny_context *ctx,
                                 const tiny_stream_opts *opts,
                                 tiny_stream_fn emit, void *user,
                                 const volatile bool *cancel,
                                 tiny_message *out, char **err);

int tiny_anthropic_messages_stream(const tiny_model *model, const tiny_context *ctx,
                                   const tiny_stream_opts *opts,
                                   tiny_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   tiny_message *out, char **err);

int tiny_google_generative_ai_stream(const tiny_model *model, const tiny_context *ctx,
                                     const tiny_stream_opts *opts,
                                     tiny_stream_fn emit, void *user,
                                     const volatile bool *cancel,
                                     tiny_message *out, char **err);

#endif
