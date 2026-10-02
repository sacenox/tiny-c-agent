// The agent loop: one turn runs the provider, executes tool calls, and appends
// every message to the transcript and the session.

#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <yyjson.h>

#include "diff.h"

// Per-turn state passed to the provider and tool callbacks.
typedef struct {
    cage_agent *a;
    bool streaming;
} turn_state;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf;

static void buf_add(buf *b, const char *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = realloc(b->data, cap);
        if (!b->data) abort();
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

// ---- event helpers --------------------------------------------------------

static void emit(cage_agent *a, const cage_event *e) {
    if (a->on_event) a->on_event(e, a->user);
}

static void emit_phase(cage_agent *a, cage_phase phase, const char *detail) {
    cage_event e = {.type = CAGE_EVENT_PHASE};
    e.u.phase.phase = phase;
    e.u.phase.detail = detail;
    emit(a, &e);
}

static void emit_error(cage_agent *a, const char *msg) {
    cage_event e = {.type = CAGE_EVENT_ERROR};
    e.u.error.message = msg ? msg : "";
    emit(a, &e);
}

static void emit_simple(cage_agent *a, cage_event_type type) {
    cage_event e = {.type = type};
    emit(a, &e);
}

// ---- transcript and session -----------------------------------------------

// Appends the message to the transcript and, when configured, to the session.
// On a persistence failure the turn must stop, so this reports the error.
static int push_message(cage_agent *a, cage_message *m, char **err) {
    a->messages = realloc(a->messages, (a->messages_len + 1) * sizeof *a->messages);
    if (!a->messages) abort();
    a->messages[a->messages_len++] = *m;
    if (!a->session) return 0;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *record = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, record);
    yyjson_mut_obj_add_strcpy(doc, record, "type", "message");
    yyjson_mut_obj_add_val(doc, record, "message", cage_message_to_val(doc, m));
    char *serr = NULL;
    int rc = cage_session_append(a->session, record, &serr);
    yyjson_mut_doc_free(doc);
    if (rc != 0) {
        *err = serr ? serr : xstrdup("session append failed");
        return -1;
    }
    return 0;
}

static int push_user(cage_agent *a, const char *text, char **err) {
    cage_message m = cage_message_user(text);
    return push_message(a, &m, err);
}

// ---- interruption ---------------------------------------------------------

// Checks the cancel flag and, at a step boundary, a pause request. On pause it
// blocks for steering. Returns false when the turn was cancelled.
static bool pause_step(cage_agent *a, char **steer) {
    *steer = NULL;
    if (a->cancel && *a->cancel) return false;
    if (!a->is_pause_requested || !a->is_pause_requested(a->user)) return true;
    if (a->clear_pause) a->clear_pause(a->user);
    emit_phase(a, CAGE_PHASE_PAUSING, NULL);
    char *s = a->request_steering ? a->request_steering(a->user) : NULL;
    emit_phase(a, CAGE_PHASE_IDLE, NULL);
    if (a->cancel && *a->cancel) {
        free(s);
        return false;
    }
    *steer = s;
    return true;
}

// ---- provider and tool callbacks ------------------------------------------

static void on_stream(const cage_stream_event *ev, void *user) {
    turn_state *st = user;
    if (!st->streaming) {
        st->streaming = true;
        emit_phase(st->a, CAGE_PHASE_STREAMING, NULL);
    }
    cage_event e = {0};
    switch (ev->type) {
    case CAGE_STREAM_TEXT_DELTA:
        e.type = CAGE_EVENT_TEXT;
        e.u.text.delta = ev->delta;
        break;
    case CAGE_STREAM_REASONING_DELTA:
        e.type = CAGE_EVENT_REASONING;
        e.u.reasoning.delta = ev->delta;
        break;
    case CAGE_STREAM_TOOL_CALL:
        e.type = CAGE_EVENT_TOOL_CALL;
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
    cage_event e = {.type = CAGE_EVENT_TOOL_OUTPUT};
    e.u.tool_output.chunk = chunk;
    emit(st->a, &e);
}

static cage_tool_result run_tool(cage_agent *a, turn_state *st, const cage_block *b) {
    cage_tool_result r = {0};
    if (!a->runtime) {
        r.text = xstrdup("codemode: no runtime configured");
        r.is_error = true;
        return r;
    }
    if (!b->tool_name || strcmp(b->tool_name, cage_codemode_tool().name) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "codemode: unknown tool \"%s\"", b->tool_name ? b->tool_name : "");
        r.text = xstrdup(msg);
        r.is_error = true;
        return r;
    }

    const char *args = b->arguments ? b->arguments : "{}";
    yyjson_doc *doc = yyjson_read(args, strlen(args), 0);
    const char *code = NULL;
    if (doc) {
        yyjson_val *c = yyjson_obj_get(yyjson_doc_get_root(doc), "code");
        if (yyjson_is_str(c)) code = yyjson_get_str(c);
    }
    if (!code) {
        yyjson_doc_free(doc);
        r.text = xstrdup("codemode: missing \"code\" argument");
        r.is_error = true;
        return r;
    }

    // The run may write anywhere under the working directory, so a diff of the
    // tree around it is the only way to report what changed.
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    struct cage_snapshot *before = cage_snapshot_capture(NULL);
    r = cage_runtime_run(a->runtime, code, on_tool_output, st, a->cancel);
    struct cage_snapshot *after = cage_snapshot_capture(&start);
    r.diffs = cage_snapshot_diff(before, after, &r.diffs_len);
    cage_snapshot_free(before);
    cage_snapshot_free(after);

    yyjson_doc_free(doc);
    return r;
}

// ---- the loop -------------------------------------------------------------

void cage_agent_turn(cage_agent *a, const char *input) {
    if (!a->model.model || !a->model.base_url) {
        emit_simple(a, CAGE_EVENT_NO_MODEL);
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
            emit_simple(a, CAGE_EVENT_CANCELLED);
            return;
        }
        if (steer && steer[0] && push_user(a, steer, &perr) != 0) {
            free(steer);
            emit_error(a, perr);
            free(perr);
            return;
        }
        free(steer);

        emit_phase(a, CAGE_PHASE_PREPARING, NULL);

        turn_state st = {.a = a, .streaming = false};
        cage_context ctx = {
            .system_prompt = a->system_prompt,
            .tools = a->tools,
            .tools_len = a->tools_len,
            .messages = a->messages,
            .messages_len = a->messages_len,
        };
        cage_stream_opts opts = {
            .effort = a->model.effort,
            .session_id = a->session ? cage_session_id(a->session) : NULL,
        };

        emit_phase(a, CAGE_PHASE_WAITING_MODEL, NULL);
        cage_message asst = {0};
        char *serr = NULL;
        if (cage_provider_stream(&a->model, &ctx, &opts, on_stream, &st, a->cancel, &asst, &serr) != 0) {
            emit_error(a, serr ? serr : "provider error");
            free(serr);
            cage_message_free(&asst);
            return;
        }

        if (push_message(a, &asst, &perr) != 0) {
            emit_error(a, perr);
            free(perr);
            return;
        }
        char *json = cage_message_to_json(&asst);
        cage_event me = {.type = CAGE_EVENT_MESSAGE};
        me.u.message.json = json ? json : "{}";
        emit(a, &me);
        free(json);

        if (asst.stop_reason && strcmp(asst.stop_reason, "aborted") == 0) {
            emit_simple(a, CAGE_EVENT_CANCELLED);
            return;
        }
        if (asst.stop_reason && strcmp(asst.stop_reason, "error") == 0) {
            emit_error(a, asst.error_message ? asst.error_message : "provider error");
            return;
        }

        size_t calls = 0;
        for (size_t i = 0; i < asst.blocks_len; i++)
            if (asst.blocks[i].type == CAGE_BLOCK_TOOL_CALL) calls++;
        if (calls == 0) {
            emit_simple(a, CAGE_EVENT_COMPLETE);
            return;
        }

        // Steering is held until every result of this assistant turn has landed.
        buf held = {0};
        bool cancelled = false;
        for (size_t i = 0; i < asst.blocks_len && !cancelled; i++) {
            cage_block *b = &asst.blocks[i];
            if (b->type != CAGE_BLOCK_TOOL_CALL) continue;

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

            emit_phase(a, CAGE_PHASE_RUNNING_TOOL, b->tool_name);
            cage_tool_result tr = run_tool(a, &st, b);
            cage_message tm = cage_message_tool(b->tool_call_id, tr.text, tr.is_error);
            free(tr.text);
            for (size_t k = 0; k < tr.images_len; k++) {
                cage_block *ib = cage_message_add_block(&tm, CAGE_BLOCK_IMAGE);
                ib->media_type = xstrdup(tr.images[k].media_type);
                ib->data = xstrdup(tr.images[k].data);
                free(tr.images[k].media_type);
                free(tr.images[k].data);
            }
            free(tr.images);

            if (push_message(a, &tm, &perr) != 0) {
                free(held.data);
                cage_file_diffs_free(tr.diffs, tr.diffs_len);
                emit_error(a, perr);
                free(perr);
                return;
            }
            cage_event re = {.type = CAGE_EVENT_TOOL_RESULT};
            re.u.tool_result.name = b->tool_name;
            re.u.tool_result.text = tm.blocks_len ? tm.blocks[0].text : "";
            re.u.tool_result.is_error = tm.is_error;
            re.u.tool_result.diffs = tr.diffs;
            re.u.tool_result.diffs_len = tr.diffs_len;
            emit(a, &re);
            cage_file_diffs_free(tr.diffs, tr.diffs_len);
        }

        if (cancelled) {
            free(held.data);
            emit_simple(a, CAGE_EVENT_CANCELLED);
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

void cage_agent_free(cage_agent *a) {
    for (size_t i = 0; i < a->messages_len; i++) cage_message_free(&a->messages[i]);
    free(a->messages);
    a->messages = NULL;
    a->messages_len = 0;
}
