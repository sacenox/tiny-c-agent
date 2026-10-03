// Internal contract between the provider dispatcher and each API adapter.

#ifndef CAGE_API_H
#define CAGE_API_H

#include <yyjson.h>

#include "provider.h"

// Adds a tool's "name", "description", and schema (under `params_key`) to `obj`.
// Shared by every adapter's tool serialization.
void cage_json_tool(yyjson_mut_doc *doc, yyjson_mut_val *obj, const cage_tool *t,
                    const char *params_key);

// Emits one delta to the adapter's stream callback, if any.
void cage_emit_delta(cage_stream_fn emit, void *user, cage_stream_type type, const char *delta);

// Appends a text/thinking block when `text` is non-NULL and non-empty.
void cage_add_text_block(cage_message *out, cage_block_type type, const char *text);

// Appends a tool-call block (defaulting empty arguments to "{}") and emits the
// corresponding CAGE_STREAM_TOOL_CALL event.
void cage_add_tool_block(cage_message *out, const char *id, const char *name,
                         const char *args, cage_stream_fn emit, void *user);

int cage_openai_completions_stream(const cage_model *model, const cage_context *ctx,
                                   const cage_stream_opts *opts,
                                   cage_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   cage_message *out, char **err);

int cage_openai_responses_stream(const cage_model *model, const cage_context *ctx,
                                 const cage_stream_opts *opts,
                                 cage_stream_fn emit, void *user,
                                 const volatile bool *cancel,
                                 cage_message *out, char **err);

int cage_anthropic_messages_stream(const cage_model *model, const cage_context *ctx,
                                   const cage_stream_opts *opts,
                                   cage_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   cage_message *out, char **err);

int cage_google_generative_ai_stream(const cage_model *model, const cage_context *ctx,
                                     const cage_stream_opts *opts,
                                     cage_stream_fn emit, void *user,
                                     const volatile bool *cancel,
                                     cage_message *out, char **err);

#endif
