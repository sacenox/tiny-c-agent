// cage events: the single output stream of the agent loop.
//
// The loop emits these. The headless projection and the TUI both consume them
// and own no agent semantics. String payloads are borrowed and valid only for
// the duration of the callback.

#ifndef CAGE_EVENT_H
#define CAGE_EVENT_H

#include <stdbool.h>

typedef enum {
    CAGE_PHASE_PREPARING,
    CAGE_PHASE_WAITING_MODEL,
    CAGE_PHASE_STREAMING,
    CAGE_PHASE_RUNNING_TOOL,
    CAGE_PHASE_PAUSING,
    CAGE_PHASE_IDLE,
} cage_phase;

typedef enum {
    CAGE_EVENT_PHASE,
    CAGE_EVENT_TEXT,        // assistant text delta
    CAGE_EVENT_REASONING,   // reasoning delta
    CAGE_EVENT_TOOL_CALL,   // the model requested the tool; arguments is JSON
    CAGE_EVENT_TOOL_OUTPUT, // stdout/stdout-like chunk from the tool
    CAGE_EVENT_TOOL_RESULT, // the tool finished
    CAGE_EVENT_MESSAGE,     // a complete assistant message, serialized as JSON
    CAGE_EVENT_NO_MODEL,
    CAGE_EVENT_ERROR,       // provider or harness error, surfaced as-is
    CAGE_EVENT_CANCELLED,
    CAGE_EVENT_COMPLETE,
} cage_event_type;

typedef struct {
    cage_event_type type;
    union {
        struct { cage_phase phase; const char *detail; } phase;
        struct { const char *delta; } text;
        struct { const char *delta; } reasoning;
        struct { const char *name; const char *arguments; } tool_call;
        struct { const char *chunk; } tool_output;
        struct { const char *name; const char *text; bool is_error; } tool_result;
        struct { const char *json; } message;
        struct { const char *message; } error;
    } u;
} cage_event;

typedef void (*cage_event_fn)(const cage_event *event, void *user);

#endif
