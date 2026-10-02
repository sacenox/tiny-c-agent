// cage providers: the uniform streaming contract of every API format.
//
// Each adapter under src/api/ implements one wire format behind
// cage_provider_stream(). Callers pass a resolved model, the transcript, and a
// cancel flag; the adapter emits deltas and returns the final assistant message.

#ifndef CAGE_PROVIDER_H
#define CAGE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "message.h"

typedef struct {
    const char *provider;  // provider id, echoed into the response metadata
    const char *model;     // model id sent on the wire
    const char *base_url;  // API root, without the endpoint path
    cage_api api;
    const char *api_key;   // NULL when the endpoint needs no key
    // The effort actually sent, after clamping the configured desire to what
    // the model accepts. NULL lets the provider decide.
    const char *effort;
    // Whether the model accepts image input. Gates whether `read` attaches
    // images. The generated catalog will set this per model.
    bool supports_images;
    // Providers that route by conversation send this header with the session
    // id. NULL when the provider does not need one.
    const char *session_header;
} cage_model;

typedef struct {
    const char *name;
    const char *description;
    const char *parameters;  // a JSON Schema object string
} cage_tool;

typedef struct {
    const char *system_prompt;  // NULL or empty to omit
    const cage_tool *tools;
    size_t tools_len;
    const cage_message *messages;
    size_t messages_len;
} cage_context;

typedef struct {
    const char *effort;      // NULL to let the provider decide; off/…/max otherwise
    long max_tokens;         // 0 to omit
    const char *session_id;  // NULL to omit
} cage_stream_opts;

typedef enum {
    CAGE_STREAM_TEXT_DELTA,
    CAGE_STREAM_REASONING_DELTA,
    CAGE_STREAM_TOOL_CALL,
} cage_stream_type;

typedef struct {
    cage_stream_type type;
    const char *delta;          // delta events, borrowed for the callback
    const char *tool_call_id;   // TOOL_CALL
    const char *tool_name;      // TOOL_CALL
    const char *tool_arguments; // TOOL_CALL, a JSON object string
} cage_stream_event;

typedef void (*cage_stream_fn)(const cage_stream_event *event, void *user);

// Runs one streaming request. Deltas go through emit; out receives the final
// assistant message (blocks, stop_reason, usage). Returns 0 when the request
// completed, even if the model stopped with "error" or "aborted" (recorded in
// out->stop_reason), and -1 with *err set on a transport or protocol failure.
// The cancel flag is polled for prompt aborts and may be NULL.
int cage_provider_stream(const cage_model *model, const cage_context *ctx,
                         const cage_stream_opts *opts,
                         cage_stream_fn emit, void *user,
                         const volatile bool *cancel,
                         cage_message *out, char **err);

#endif
