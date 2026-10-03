// tiny providers: the uniform streaming contract of every API format.
//
// Each adapter under src/api/ implements one wire format behind
// tiny_provider_stream(). Callers pass a resolved model, the transcript, and a
// cancel flag; the adapter emits deltas and returns the final assistant message.

#ifndef TINY_PROVIDER_H
#define TINY_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "message.h"

typedef struct {
    const char *provider;  // provider id, echoed into the response metadata
    const char *model;     // model id sent on the wire
    const char *base_url;  // API root, without the endpoint path
    tiny_api api;
    const char *api_key;   // NULL when the endpoint needs no key
    // The effort actually sent, after clamping the configured desire to what
    // the model accepts. NULL lets the provider decide.
    const char *effort;
    // Whether the model accepts image input. Gates whether `read` attaches
    // images. The generated catalog will set this per model.
    bool supports_images;
    // The largest reply the model accepts, from the catalog. 0 when unknown.
    // The context readout uses it to tell when the next request could not fit.
    long max_tokens;
    // Providers that route by conversation send this header with the session
    // id. NULL when the provider does not need one.
    const char *session_header;
} tiny_model;

typedef struct {
    const char *name;
    const char *description;
    const char *parameters;  // a JSON Schema object string
} tiny_tool;

typedef struct {
    const char *system_prompt;  // NULL or empty to omit
    const tiny_tool *tools;
    size_t tools_len;
    const tiny_message *messages;
    size_t messages_len;
} tiny_context;

typedef struct {
    const char *effort;      // NULL to let the provider decide; off/…/max otherwise
    long max_tokens;         // 0 to omit
    const char *session_id;  // NULL to omit
} tiny_stream_opts;

typedef enum {
    TINY_STREAM_TEXT_DELTA,
    TINY_STREAM_REASONING_DELTA,
    TINY_STREAM_TOOL_CALL_START, // a tool call block began; name is known
    TINY_STREAM_TOOL_CALL,       // a tool call block finished
} tiny_stream_type;

typedef struct {
    tiny_stream_type type;
    const char *delta;          // delta events, borrowed for the callback
    const char *tool_call_id;   // TOOL_CALL
    const char *tool_name;      // TOOL_CALL
    const char *tool_arguments; // TOOL_CALL, a JSON object string
} tiny_stream_event;

typedef void (*tiny_stream_fn)(const tiny_stream_event *event, void *user);

// Runs one streaming request. Deltas go through emit; out receives the final
// assistant message (blocks, stop_reason, usage). Returns 0 when the request
// completed, even if the model stopped with "error" or "aborted" (recorded in
// out->stop_reason), and -1 with *err set on a transport or protocol failure.
// The cancel flag is polled for prompt aborts and may be NULL.
int tiny_provider_stream(const tiny_model *model, const tiny_context *ctx,
                         const tiny_stream_opts *opts,
                         tiny_stream_fn emit, void *user,
                         const volatile bool *cancel,
                         tiny_message *out, char **err);

#endif
