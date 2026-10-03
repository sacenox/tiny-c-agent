// tiny events: the single output stream of the agent loop.
//
// The loop emits these. The headless projection and the TUI both consume them
// and own no agent semantics. String payloads are borrowed and valid only for
// the duration of the callback.

#ifndef TINY_EVENT_H
#define TINY_EVENT_H

#include <stdbool.h>
#include <stddef.h>

#include "tool.h"

typedef enum {
    TINY_PHASE_PREPARING,
    TINY_PHASE_WAITING_MODEL,
    TINY_PHASE_STREAMING,
    TINY_PHASE_RUNNING_TOOL,
    TINY_PHASE_PAUSING,
    TINY_PHASE_IDLE,
} tiny_phase;

typedef enum {
    TINY_EVENT_PHASE,
    TINY_EVENT_TEXT,        // assistant text delta
    TINY_EVENT_REASONING,   // reasoning delta
    TINY_EVENT_TOOL_CALL,   // the model requested the tool; arguments is JSON
    TINY_EVENT_TOOL_OUTPUT, // stdout/stdout-like chunk from the tool
    TINY_EVENT_TOOL_RESULT, // the tool finished
    TINY_EVENT_MESSAGE,     // a complete assistant message, serialized as JSON
    TINY_EVENT_NO_MODEL,
    TINY_EVENT_ERROR,       // provider or harness error, surfaced as-is
    TINY_EVENT_CANCELLED,
    TINY_EVENT_COMPLETE,
} tiny_event_type;

typedef struct {
    tiny_event_type type;
    union {
        struct { tiny_phase phase; const char *detail; } phase;
        struct { const char *delta; } text;
        struct { const char *delta; } reasoning;
        struct { const char *name; const char *arguments; } tool_call;
        struct { const char *chunk; } tool_output;
        struct {
            const char *name;
            const char *text;
            bool is_error;
            const tiny_file_diff *diffs;  // display-only; never reaches the model
            size_t diffs_len;
        } tool_result;
        struct { const char *json; } message;
        struct { const char *message; } error;
    } u;
} tiny_event;

typedef void (*tiny_event_fn)(const tiny_event *event, void *user);

#endif
