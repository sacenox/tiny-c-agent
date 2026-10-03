// OpenAI-compatible chat completions: the wire format behind Opencode Zen/Go
// and every customProvider with api "openai-completions".

#include "api.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "http.h"
#include "sse.h"
#include "util.h"

// ---- request building -----------------------------------------------------

static void add_message(yyjson_mut_doc *doc, yyjson_mut_val *arr, const tiny_message *m) {
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(arr, o);

    if (m->role == TINY_ROLE_TOOL) {
        yyjson_mut_obj_add_strcpy(doc, o, "role", "tool");
        yyjson_mut_obj_add_strcpy(doc, o, "tool_call_id", m->tool_call_id ? m->tool_call_id : "");
        char *t = tiny_message_text(m);
        if (t[0] == '\0' && tiny_message_has_image(m)) {
            free(t);
            t = xstrdup("(see attached image)");
        }
        yyjson_mut_obj_add_strcpy(doc, o, "content", t);
        free(t);
        return;
    }
    if (m->role == TINY_ROLE_USER) {
        yyjson_mut_obj_add_strcpy(doc, o, "role", "user");
        char *t = tiny_message_text(m);
        yyjson_mut_obj_add_strcpy(doc, o, "content", t);
        free(t);
        return;
    }

    yyjson_mut_obj_add_strcpy(doc, o, "role", "assistant");
    char *t = tiny_message_text(m);
    yyjson_mut_obj_add_strcpy(doc, o, "content", t);
    free(t);

    bool has_calls = false;
    yyjson_mut_val *calls = yyjson_mut_arr(doc);
    for (size_t i = 0; i < m->blocks_len; i++) {
        const tiny_block *b = &m->blocks[i];
        if (b->type != TINY_BLOCK_TOOL_CALL) continue;
        has_calls = true;
        yyjson_mut_val *tc = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(calls, tc);
        yyjson_mut_obj_add_strcpy(doc, tc, "id", b->tool_call_id ? b->tool_call_id : "");
        yyjson_mut_obj_add_strcpy(doc, tc, "type", "function");
        yyjson_mut_val *fn = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, tc, "function", fn);
        yyjson_mut_obj_add_strcpy(doc, fn, "name", b->tool_name ? b->tool_name : "");
        yyjson_mut_obj_add_strcpy(doc, fn, "arguments", b->arguments ? b->arguments : "{}");
    }
    if (has_calls) yyjson_mut_obj_add_val(doc, o, "tool_calls", calls);
}

static void add_tools(yyjson_mut_doc *doc, yyjson_mut_val *root, const tiny_context *ctx) {
    if (ctx->tools_len == 0) return;
    yyjson_mut_val *tools = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    for (size_t i = 0; i < ctx->tools_len; i++) {
        const tiny_tool *t = &ctx->tools[i];
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(tools, tool);
        yyjson_mut_obj_add_strcpy(doc, tool, "type", "function");
        yyjson_mut_val *fn = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, tool, "function", fn);
        tiny_json_tool(doc, fn, t, "parameters");
    }
}

static char *build_request(const tiny_model *model, const tiny_context *ctx,
                           const tiny_stream_opts *opts) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "model", model->model ? model->model : "");
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    yyjson_mut_val *so = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, so, "include_usage", true);
    yyjson_mut_obj_add_val(doc, root, "stream_options", so);

    if (opts && opts->effort && strcmp(opts->effort, "off") != 0)
        yyjson_mut_obj_add_strcpy(doc, root, "reasoning_effort", opts->effort);
    if (opts && opts->max_tokens > 0)
        yyjson_mut_obj_add_int(doc, root, "max_tokens", opts->max_tokens);

    yyjson_mut_val *messages = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", messages);
    if (ctx->system_prompt && ctx->system_prompt[0]) {
        yyjson_mut_val *sys = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(messages, sys);
        yyjson_mut_obj_add_strcpy(doc, sys, "role", "system");
        yyjson_mut_obj_add_strcpy(doc, sys, "content", ctx->system_prompt);
    }
    for (size_t i = 0; i < ctx->messages_len; ) {
        if (ctx->messages[i].role == TINY_ROLE_TOOL) {
            // A run of tool results lands together. Any images among them are
            // sent as one following user message, because a tool message's
            // content is a string and the provider rejects a user message
            // between a call and its results.
            size_t j = i;
            bool has_images = false;
            for (; j < ctx->messages_len && ctx->messages[j].role == TINY_ROLE_TOOL; j++) {
                add_message(doc, messages, &ctx->messages[j]);
                if (tiny_message_has_image(&ctx->messages[j])) has_images = true;
            }
            if (has_images) {
                yyjson_mut_val *um = yyjson_mut_obj(doc);
                yyjson_mut_arr_add_val(messages, um);
                yyjson_mut_obj_add_strcpy(doc, um, "role", "user");
                yyjson_mut_val *content = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, um, "content", content);
                yyjson_mut_val *head = yyjson_mut_obj(doc);
                yyjson_mut_arr_add_val(content, head);
                yyjson_mut_obj_add_strcpy(doc, head, "type", "text");
                yyjson_mut_obj_add_strcpy(doc, head, "text", "Attached image(s) from tool result:");
                for (size_t m = i; m < j; m++) {
                    const tiny_message *msg = &ctx->messages[m];
                    for (size_t k = 0; k < msg->blocks_len; k++) {
                        const tiny_block *b = &msg->blocks[k];
                        if (b->type != TINY_BLOCK_IMAGE) continue;
                        yyjson_mut_val *iu = yyjson_mut_obj(doc);
                        yyjson_mut_arr_add_val(content, iu);
                        yyjson_mut_obj_add_strcpy(doc, iu, "type", "image_url");
                        yyjson_mut_val *url = yyjson_mut_obj(doc);
                        yyjson_mut_obj_add_val(doc, iu, "image_url", url);
                        const char *mt = b->media_type ? b->media_type : "";
                        const char *bs = b->data ? b->data : "";
                        size_t need = strlen(mt) + strlen(bs) + 32;
                        char *u = xmalloc(need);
                        snprintf(u, need, "data:%s;base64,%s", mt, bs);
                        yyjson_mut_obj_add_strcpy(doc, url, "url", u);
                        free(u);
                    }
                }
            }
            i = j;
        } else {
            add_message(doc, messages, &ctx->messages[i]);
            i++;
        }
    }

    add_tools(doc, root, ctx);

    size_t len = 0;
    char *body = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    return body;
}

// ---- streaming state ------------------------------------------------------

typedef struct {
    char *id;
    char *name;
    tiny_buf args;
    bool started;
} tool_acc;

typedef struct {
    tiny_buf text;
    tiny_buf reasoning;
    tool_acc *calls;
    size_t calls_len;
    char *finish_reason;
    long tokens_in;
    long tokens_out;
    long tokens_total;
    char *error;
    tiny_stream_fn emit;
    void *user;
} sse_state;

static void grow_calls(sse_state *st, size_t count) {
    if (count <= st->calls_len) return;
    st->calls = realloc(st->calls, count * sizeof *st->calls);
    if (!st->calls) abort();
    memset(&st->calls[st->calls_len], 0, (count - st->calls_len) * sizeof *st->calls);
    st->calls_len = count;
}

static void handle_delta(sse_state *st, yyjson_val *delta) {
    yyjson_val *content = yyjson_obj_get(delta, "content");
    if (yyjson_is_str(content) && yyjson_get_str(content)[0]) {
        const char *s = yyjson_get_str(content);
        buf_add(&st->text, s, strlen(s));
        tiny_emit_delta(st->emit, st->user, TINY_STREAM_TEXT_DELTA, s);
    }

    static const char *const reasoning_fields[] = {"reasoning_content", "reasoning", "reasoning_text"};
    for (size_t i = 0; i < sizeof reasoning_fields / sizeof reasoning_fields[0]; i++) {
        yyjson_val *r = yyjson_obj_get(delta, reasoning_fields[i]);
        if (!yyjson_is_str(r) || !yyjson_get_str(r)[0]) continue;
        const char *s = yyjson_get_str(r);
        buf_add(&st->reasoning, s, strlen(s));
        tiny_emit_delta(st->emit, st->user, TINY_STREAM_REASONING_DELTA, s);
        break;
    }

    yyjson_val *tcs = yyjson_obj_get(delta, "tool_calls");
    if (!yyjson_is_arr(tcs)) return;
    size_t idx, max;
    yyjson_val *tc;
    yyjson_arr_foreach(tcs, idx, max, tc) {
        yyjson_val *iv = yyjson_obj_get(tc, "index");
        size_t index = yyjson_is_int(iv) ? (size_t)yyjson_get_int(iv) : st->calls_len;
        grow_calls(st, index + 1);
        tool_acc *a = &st->calls[index];
        yyjson_val *id = yyjson_obj_get(tc, "id");
        if (yyjson_is_str(id)) {
            free(a->id);
            a->id = xstrdup(yyjson_get_str(id));
        }
        yyjson_val *fn = yyjson_obj_get(tc, "function");
        if (!yyjson_is_obj(fn)) continue;
        yyjson_val *name = yyjson_obj_get(fn, "name");
        if (yyjson_is_str(name)) {
            free(a->name);
            a->name = xstrdup(yyjson_get_str(name));
        }
        if (!a->started && a->name && a->name[0]) {
            a->started = true;
            tiny_emit_tool_start(st->emit, st->user, a->id, a->name);
        }
        yyjson_val *args = yyjson_obj_get(fn, "arguments");
        if (yyjson_is_str(args)) {
            const char *s = yyjson_get_str(args);
            buf_add(&a->args, s, strlen(s));
        }
    }
}

static void on_data(const char *json, size_t len, void *user) {
    sse_state *st = user;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        if (!st->error) st->error = xstrdup("stream: invalid JSON chunk");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);

    yyjson_val *err = yyjson_obj_get(root, "error");
    if (err) {
        if (!st->error) {
            const char *msg = NULL;
            if (yyjson_is_obj(err)) {
                yyjson_val *m = yyjson_obj_get(err, "message");
                if (yyjson_is_str(m)) msg = yyjson_get_str(m);
            } else if (yyjson_is_str(err)) {
                msg = yyjson_get_str(err);
            }
            st->error = xstrdup(msg ? msg : "stream: provider error");
        }
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *choices = yyjson_obj_get(root, "choices");
    if (yyjson_is_arr(choices) && yyjson_arr_size(choices) > 0) {
        yyjson_val *choice = yyjson_arr_get_first(choices);
        yyjson_val *delta = yyjson_obj_get(choice, "delta");
        if (yyjson_is_obj(delta)) handle_delta(st, delta);
        yyjson_val *fr = yyjson_obj_get(choice, "finish_reason");
        if (yyjson_is_str(fr)) {
            free(st->finish_reason);
            st->finish_reason = xstrdup(yyjson_get_str(fr));
        }
    }

    yyjson_val *usage = yyjson_obj_get(root, "usage");
    if (yyjson_is_obj(usage)) {
        yyjson_val *v;
        if (yyjson_is_num(v = yyjson_obj_get(usage, "prompt_tokens"))) st->tokens_in = (long)yyjson_get_num(v);
        if (yyjson_is_num(v = yyjson_obj_get(usage, "completion_tokens"))) st->tokens_out = (long)yyjson_get_num(v);
        if (yyjson_is_num(v = yyjson_obj_get(usage, "total_tokens"))) st->tokens_total = (long)yyjson_get_num(v);
    }
    yyjson_doc_free(doc);
}

static void sse_free(sse_state *st) {
    buf_free(&st->text);
    buf_free(&st->reasoning);
    for (size_t i = 0; i < st->calls_len; i++) {
        free(st->calls[i].id);
        free(st->calls[i].name);
        buf_free(&st->calls[i].args);
    }
    free(st->calls);
    free(st->finish_reason);
    free(st->error);
}

static void finalize(sse_state *st, tiny_message *out) {
    tiny_add_text_block(out, TINY_BLOCK_THINKING, st->reasoning.data);
    tiny_add_text_block(out, TINY_BLOCK_TEXT, st->text.data);
    for (size_t i = 0; i < st->calls_len; i++) {
        tool_acc *a = &st->calls[i];
        if (!a->name && a->args.len == 0) continue;
        tiny_add_tool_block(out, a->id, a->name, a->args.len ? a->args.data : NULL, st->emit, st->user);
    }
    out->tokens_in = st->tokens_in;
    out->tokens_out = st->tokens_out;
    out->tokens_total = st->tokens_total;
    if (st->calls_len > 0) out->stop_reason = xstrdup("toolUse");
    else if (st->finish_reason && strcmp(st->finish_reason, "length") == 0) out->stop_reason = xstrdup("length");
    else out->stop_reason = xstrdup("stop");
}

// ---- transport ------------------------------------------------------------


int tiny_openai_completions_stream(const tiny_model *model, const tiny_context *ctx,
                                   const tiny_stream_opts *opts,
                                   tiny_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   tiny_message *out, char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    out->role = TINY_ROLE_ASSISTANT;

    char *url = tiny_url_endpoint(model->base_url, "chat/completions");
    if (!url) return tiny_fail(err, "provider: no base url");
    char *body = build_request(model, ctx, opts);
    if (!body) {
        free(url);
        return tiny_fail(err, "provider: cannot build request");
    }

    sse_state st = {0};
    st.emit = emit;
    st.user = user;

    const char *extra[2] = {NULL, NULL};
    char *session = NULL;
    if (model->session_header && model->session_header[0] && opts && opts->session_id && opts->session_id[0]) {
        size_t n = strlen(model->session_header) + strlen(opts->session_id) + 4;
        session = xmalloc(n);
        snprintf(session, n, "%s: %s", model->session_header, opts->session_id);
        extra[0] = session;
    }

    tiny_api_result r = tiny_api_post(url, body, "Authorization: Bearer ",
                                      model->api_key, extra, cancel, on_data, &st, st.error, err);
    free(session);

    int rc;
    if (r == TINY_API_ABORTED) {
        out->stop_reason = xstrdup("aborted");
        rc = 0;
    } else if (r == TINY_API_TRANSPORT_ERROR) {
        rc = -1;
    } else if (r == TINY_API_STREAM_ERROR) {
        out->stop_reason = xstrdup("error");
        out->error_message = xstrdup(st.error);
        rc = 0;
    } else {
        finalize(&st, out);
        rc = 0;
    }
    sse_free(&st);
    return rc;
}
