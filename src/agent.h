// tiny agent: the loop that binds provider, session, and tool.

#ifndef TINY_AGENT_H
#define TINY_AGENT_H

#include <stdbool.h>

#include "event.h"
#include "message.h"
#include "provider.h"
#include "session.h"

typedef struct {
    // Model and request shape.
    tiny_model model;
    const char *system_prompt;
    const tiny_tool *tools;
    size_t tools_len;

    // Persistence. NULL disables session logging.
    tiny_session *session;

    // Projection. Both projections receive the same event stream.
    tiny_event_fn on_event;
    void *user;

    // Interruption. All optional; NULL means the feature is absent.
    bool (*is_pause_requested)(void *user);
    void (*clear_pause)(void *user);
    char *(*request_steering)(void *user);  // returns malloc'd text, or NULL
    const volatile bool *cancel;

    // Transcript, owned by the agent and kept across turns.
    tiny_message *messages;
    size_t messages_len;
} tiny_agent;

// Appends `input` as a user message, then loops until the model stops without
// tool calls. Every outcome is reported as an event: complete, cancelled, or
// error. The caller owns `model`, `runtime`, `session`, and the tools.
void tiny_agent_turn(tiny_agent *a, const char *input);

// Frees the transcript. Does not free the runtime, session, or model strings.
void tiny_agent_free(tiny_agent *a);

#endif
