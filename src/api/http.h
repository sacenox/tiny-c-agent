// Internal HTTP streaming transport shared by the streaming API adapters.
//
// Performs a POST and splits the server-sent event body into `data:` payloads.
// The caller owns the URL, body, and header list.

#ifndef CAGE_API_HTTP_H
#define CAGE_API_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>

// Called for each SSE `data:` payload. See cage_sse_fn.
typedef void (*cage_http_data_fn)(const char *data, size_t len, void *user);

typedef struct {
    const char *url;
    const char *body;
    struct curl_slist *headers;   // caller builds and frees
    const volatile bool *cancel;  // may be NULL
    cage_http_data_fn on_data;
    void *user;

    // Set by the call.
    long status;   // HTTP status, 0 when none arrived
    bool aborted;  // the cancel flag was observed
    char *error;   // malloc'd message on transport or HTTP error, else NULL
} cage_http_request;

// Runs the request. Returns 0 when the transfer completed (the status may still
// be an error, with error set), and -1 on a transport failure (also with error
// set).
int cage_http_stream(cage_http_request *req);

typedef enum {
    CAGE_API_OK,               // the caller should finalize
    CAGE_API_ABORTED,          // the cancel flag was observed
    CAGE_API_STREAM_ERROR,     // the stream reported an error (see stream_error)
    CAGE_API_TRANSPORT_ERROR,  // the transfer failed; *err is set
} cage_api_result;

// Builds the standard headers (Content-Type, Accept, an optional auth line
// "auth_prefix + api_key", and the NULL-terminated `extra` headers) and runs a
// streaming POST. `stream_error` is the adapter's recorded stream error, or
// NULL. Takes ownership of url and body.
cage_api_result cage_api_post(const char *url, const char *body,
                              const char *auth_prefix, const char *api_key,
                              const char *const *extra,
                              const volatile bool *cancel,
                              cage_http_data_fn on_data, void *user,
                              const char *stream_error, char **err);

#endif
