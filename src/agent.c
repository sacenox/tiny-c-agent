// The agent loop: one turn runs the provider, executes tool calls, and appends
// every message to the transcript and the session.

#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <yyjson.h>

#include "diff.h"
#include "util.h"

// Per-turn state passed to the provider and tool callbacks.
typedef struct {
    tiny_agent *a;
    bool streaming;
} turn_state;

// ---- event helpers --------------------------------------------------------

static void emit(tiny_agent *a, const tiny_event *e) {
    if (a->on_event) a->on_event(e, a->user);
}

static void emit_phase(tiny_agent *a, tiny_phase phase, const char *detail) {
    tiny_event e = {.type = TINY_EVENT_PHASE};
    e.u.phase.phase = phase;
    e.u.phase.detail = detail;
    emit(a, &e);
}

static void emit_error(tiny_agent *a, const char *msg) {
    tiny_event e = {.type = TINY_EVENT_ERROR};
    e.u.error.message = msg ? msg : "";
    emit(a, &e);
}

static void emit_simple(tiny_agent *a, tiny_event_type type) {
    tiny_event e = {.type = type};
    emit(a, &e);
}

// ---- transcript and session -----------------------------------------------

// Appends the message to the transcript and, when configured, to the session.
// On a persistence failure the turn must stop, so this reports the error.
static int push_message(tiny_agent *a, tiny_message *m, char **err) {
    a->messages = realloc(a->messages, (a->messages_len + 1) * sizeof *a->messages);
    if (!a->messages) abort();
    a->messages[a->messages_len++] = *m;
    if (!a->session) return 0;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *record = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, record);
    yyjson_mut_obj_add_strcpy(doc, record, "type", "message");
    yyjson_mut_obj_add_val(doc, record, "message", tiny_message_to_val(doc, m));
    char *serr = NULL;
    int rc = tiny_session_append(a->session, record, &serr);
    yyjson_mut_doc_free(doc);
    if (rc != 0) {
        *err = serr ? serr : xstrdup("session append failed");
        return -1;
    }
    return 0;
}

static int push_user(tiny_agent *a, const char *text, char **err) {
    tiny_message m = tiny_message_user(text);
    return push_message(a, &m, err);
}

// ---- interruption ---------------------------------------------------------

// Checks the cancel flag and, at a step boundary, a pause request. On pause it
// blocks for steering. Returns false when the turn was cancelled.
static bool pause_step(tiny_agent *a, char **steer) {
    *steer = NULL;
    if (a->cancel && *a->cancel) return false;
    if (!a->is_pause_requested || !a->is_pause_requested(a->user)) return true;
    if (a->clear_pause) a->clear_pause(a->user);
    emit_phase(a, TINY_PHASE_PAUSING, NULL);
    char *s = a->request_steering ? a->request_steering(a->user) : NULL;
    emit_phase(a, TINY_PHASE_IDLE, NULL);
    if (a->cancel && *a->cancel) {
        free(s);
        return false;
    }
    *steer = s;
    return true;
}

// ---- provider and tool callbacks ------------------------------------------

static void on_stream(const tiny_stream_event *ev, void *user) {
    turn_state *st = user;
    if (!st->streaming) {
        st->streaming = true;
        emit_phase(st->a, TINY_PHASE_STREAMING, NULL);
    }
    tiny_event e = {0};
    switch (ev->type) {
    case TINY_STREAM_TEXT_DELTA:
        e.type = TINY_EVENT_TEXT;
        e.u.text.delta = ev->delta;
        break;
    case TINY_STREAM_REASONING_DELTA:
        e.type = TINY_EVENT_REASONING;
        e.u.reasoning.delta = ev->delta;
        break;
    case TINY_STREAM_TOOL_CALL_START:
        e.type = TINY_EVENT_TOOL_CALL_START;
        e.u.tool_call_start.name = ev->tool_name;
        break;
    case TINY_STREAM_TOOL_CALL:
        e.type = TINY_EVENT_TOOL_CALL;
        e.u.tool_call.name = ev->tool_name;
        e.u.tool_call.arguments = ev->tool_arguments;
        break;
    default:
        return;
    }
    emit(st->a, &e);
}

static void on_tool_output(const char *chunk, void *user) {
    turn_state *st = user;
    tiny_event e = {.type = TINY_EVENT_TOOL_OUTPUT};
    e.u.tool_output.chunk = chunk;
    emit(st->a, &e);
}

static tiny_tool_result run_tool(tiny_agent *a, turn_state *st, const tiny_block *b) {
    const char *name = b->tool_name ? b->tool_name : "";
    const char *args = b->arguments ? b->arguments : "{}";

    // Only bash can write anywhere under the working directory, so a diff of
    // the tree around it is the only way to report what changed. `edit` reports
    // its own diff from the content it already read.
    if (strcmp(name, "bash") != 0)
        return tiny_tool_run(name, args, a->model.supports_images, on_tool_output, st, a->cancel);

    // The tool may write anywhere under the working directory, so a diff of the
    // tree around it is the only way to report what changed.
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    struct tiny_snapshot *before = tiny_snapshot_capture(NULL);
    tiny_tool_result r =
        tiny_tool_run(name, args, a->model.supports_images, on_tool_output, st, a->cancel);
    struct tiny_snapshot *after = tiny_snapshot_capture(&start);
    r.diffs = tiny_snapshot_diff(before, after, &r.diffs_len);
    tiny_snapshot_free(before);
    tiny_snapshot_free(after);
    return r;
}

// ---- the loop -------------------------------------------------------------

void tiny_agent_turn(tiny_agent *a, const char *input) {
    if (!a->model.model || !a->model.base_url) {
        emit_simple(a, TINY_EVENT_NO_MODEL);
        return;
    }

    char *perr = NULL;
    if (input && input[0] && push_user(a, input, &perr) != 0) {
        emit_error(a, perr);
        free(perr);
        return;
    }

    for (;;) {
        char *steer = NULL;
        if (!pause_step(a, &steer)) {
            emit_simple(a, TINY_EVENT_CANCELLED);
            return;
        }
        if (steer && steer[0] && push_user(a, steer, &perr) != 0) {
            free(steer);
            emit_error(a, perr);
            free(perr);
            return;
        }
        free(steer);

        emit_phase(a, TINY_PHASE_PREPARING, NULL);

        turn_state st = {.a = a, .streaming = false};
        tiny_context ctx = {
            .system_prompt = a->system_prompt,
            .tools = a->tools,
            .tools_len = a->tools_len,
            .messages = a->messages,
            .messages_len = a->messages_len,
        };
        tiny_stream_opts opts = {
            .effort = a->model.effort,
            .session_id = a->session ? tiny_session_id(a->session) : NULL,
        };

        emit_phase(a, TINY_PHASE_WAITING_MODEL, NULL);
        tiny_message asst = {0};
        char *serr = NULL;
        if (tiny_provider_stream(&a->model, &ctx, &opts, on_stream, &st, a->cancel, &asst, &serr) != 0) {
            emit_error(a, serr ? serr : "provider error");
            free(serr);
            tiny_message_free(&asst);
            return;
        }

        if (push_message(a, &asst, &perr) != 0) {
            emit_error(a, perr);
            free(perr);
            return;
        }
        char *json = tiny_message_to_json(&asst);
        tiny_event me = {.type = TINY_EVENT_MESSAGE};
        me.u.message.json = json ? json : "{}";
        emit(a, &me);
        free(json);

        if (asst.stop_reason && strcmp(asst.stop_reason, "aborted") == 0) {
            emit_simple(a, TINY_EVENT_CANCELLED);
            return;
        }
        if (asst.stop_reason && strcmp(asst.stop_reason, "error") == 0) {
            emit_error(a, asst.error_message ? asst.error_message : "provider error");
            return;
        }

        size_t calls = 0;
        for (size_t i = 0; i < asst.blocks_len; i++)
            if (asst.blocks[i].type == TINY_BLOCK_TOOL_CALL) calls++;
        if (calls == 0) {
            emit_simple(a, TINY_EVENT_COMPLETE);
            return;
        }

        // Steering is held until every result of this assistant turn has landed.
        tiny_buf held = {0};
        bool cancelled = false;
        for (size_t i = 0; i < asst.blocks_len && !cancelled; i++) {
            tiny_block *b = &asst.blocks[i];
            if (b->type != TINY_BLOCK_TOOL_CALL) continue;

            char *s2 = NULL;
            if (!pause_step(a, &s2)) {
                free(s2);
                cancelled = true;
                break;
            }
            if (s2 && s2[0]) {
                if (held.len) buf_add(&held, "\n", 1);
                buf_add(&held, s2, strlen(s2));
            }
            free(s2);

            emit_phase(a, TINY_PHASE_RUNNING_TOOL, b->tool_name);
            tiny_tool_result tr = run_tool(a, &st, b);
            tiny_message tm = tiny_message_tool(b->tool_call_id, tr.text, tr.is_error);
            free(tr.text);
            for (size_t k = 0; k < tr.images_len; k++) {
                tiny_block *ib = tiny_message_add_block(&tm, TINY_BLOCK_IMAGE);
                ib->media_type = xstrdup(tr.images[k].media_type);
                ib->data = xstrdup(tr.images[k].data);
                free(tr.images[k].media_type);
                free(tr.images[k].data);
            }
            free(tr.images);

            if (push_message(a, &tm, &perr) != 0) {
                free(held.data);
                tiny_file_diffs_free(tr.diffs, tr.diffs_len);
                emit_error(a, perr);
                free(perr);
                return;
            }
            tiny_event re = {.type = TINY_EVENT_TOOL_RESULT};
            re.u.tool_result.name = b->tool_name;
            re.u.tool_result.text = tm.blocks_len ? tm.blocks[0].text : "";
            re.u.tool_result.is_error = tm.is_error;
            re.u.tool_result.diffs = tr.diffs;
            re.u.tool_result.diffs_len = tr.diffs_len;
            emit(a, &re);
            tiny_file_diffs_free(tr.diffs, tr.diffs_len);
        }

        if (cancelled) {
            free(held.data);
            emit_simple(a, TINY_EVENT_CANCELLED);
            return;
        }
        if (held.len && push_user(a, held.data, &perr) != 0) {
            free(held.data);
            emit_error(a, perr);
            free(perr);
            return;
        }
        free(held.data);
    }
}

void tiny_agent_free(tiny_agent *a) {
    for (size_t i = 0; i < a->messages_len; i++) tiny_message_free(&a->messages[i]);
    free(a->messages);
    a->messages = NULL;
    a->messages_len = 0;
}
