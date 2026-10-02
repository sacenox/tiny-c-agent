// cage agent: the loop that binds provider, session, and tool.

#ifndef CAGE_AGENT_H
#define CAGE_AGENT_H

#include <stdbool.h>

#include "event.h"
#include "message.h"
#include "provider.h"
#include "session.h"
#include "tool.h"

typedef struct {
    // Model and request shape.
    cage_model model;
    const char *system_prompt;
    const cage_tool *tools;
    size_t tools_len;

    // Tool execution. Required once the model asks for a tool.
    cage_runtime *runtime;

    // Persistence. NULL disables session logging.
    cage_session *session;

    // Projection. Both projections receive the same event stream.
    cage_event_fn on_event;
    void *user;

    // Interruption. All optional; NULL means the feature is absent.
    bool (*is_pause_requested)(void *user);
    void (*clear_pause)(void *user);
    char *(*request_steering)(void *user);  // returns malloc'd text, or NULL
    const volatile bool *cancel;

    // Transcript, owned by the agent and kept across turns.
    cage_message *messages;
    size_t messages_len;
} cage_agent;

// Appends `input` as a user message, then loops until the model stops without
// tool calls. Every outcome is reported as an event: complete, cancelled, or
// error. The caller owns `model`, `runtime`, `session`, and the tools.
void cage_agent_turn(cage_agent *a, const char *input);

// Frees the transcript. Does not free the runtime, session, or model strings.
void cage_agent_free(cage_agent *a);

#endif
