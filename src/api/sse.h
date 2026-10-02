// Internal SSE line splitter shared by the streaming API adapters.
//
// Every adapter consumes server-sent events where each event carries a single
// `data:` line of JSON. This splits a byte stream into those payloads.

#ifndef CAGE_API_SSE_H
#define CAGE_API_SSE_H

#include <stddef.h>

// Called once per `data:` payload, with the prefix and trailing newline removed.
// `[DONE]` sentinels and empty payloads are not delivered. The pointer is valid
// only for the call.
typedef void (*cage_sse_fn)(const char *data, size_t len, void *user);

typedef struct {
    char *pending;
    size_t pending_len;
    size_t pending_cap;
} cage_sse;

void cage_sse_init(cage_sse *s);
void cage_sse_free(cage_sse *s);
void cage_sse_feed(cage_sse *s, const char *data, size_t n, cage_sse_fn fn, void *user);

#endif
