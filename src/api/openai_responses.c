// OpenAI Responses API streaming adapter.

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "http.h"
#include "util.h"

// ---- request --------------------------------------------------------------

static void add_input(yyjson_mut_doc *doc, yyjson_mut_val *arr, const cage_message *m) {
    if (m->role == CAGE_ROLE_TOOL) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(arr, o);
        yyjson_mut_obj_add_strcpy(doc, o, "type", "function_call_output");
        yyjson_mut_obj_add_strcpy(doc, o, "call_id", m->tool_call_id ? m->tool_call_id : "");
        char *t = cage_message_text(m);
        yyjson_mut_obj_add_strcpy(doc, o, "output", t);
        free(t);
        return;
    }
    if (m->role == CAGE_ROLE_USER) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(arr, o);
        yyjson_mut_obj_add_strcpy(doc, o, "role", "user");
        yyjson_mut_val *content = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, o, "content", content);
        for (size_t i = 0; i < m->blocks_len; i++) {
            const cage_block *b = &m->blocks[i];
            if (b->type == CAGE_BLOCK_TEXT) {
                yyjson_mut_val *p = yyjson_mut_obj(doc);
                yyjson_mut_arr_add_val(content, p);
                yyjson_mut_obj_add_strcpy(doc, p, "type", "input_text");
                yyjson_mut_obj_add_strcpy(doc, p, "text", b->text ? b->text : "");
            } else if (b->type == CAGE_BLOCK_IMAGE) {
                yyjson_mut_val *p = yyjson_mut_obj(doc);
                yyjson_mut_arr_add_val(content, p);
                yyjson_mut_obj_add_strcpy(doc, p, "type", "input_image");
                yyjson_mut_obj_add_strcpy(doc, p, "detail", "auto");
                size_t need = strlen(b->media_type ? b->media_type : "") +
                              strlen(b->data ? b->data : "") + 32;
                char *u = xmalloc(need);
                snprintf(u, need, "data:%s;base64,%s", b->media_type ? b->media_type : "",
                         b->data ? b->data : "");
                yyjson_mut_obj_add_strcpy(doc, p, "image_url", u);
                free(u);
            }
        }
        return;
    }

    // Assistant: text as a message item, each tool call as its own item.
    for (size_t i = 0; i < m->blocks_len; i++) {
        const cage_block *b = &m->blocks[i];
        if (b->type == CAGE_BLOCK_TEXT) {
            yyjson_mut_val *o = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(arr, o);
            yyjson_mut_obj_add_strcpy(doc, o, "type", "message");
            yyjson_mut_obj_add_strcpy(doc, o, "role", "assistant");
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, o, "content", content);
            yyjson_mut_val *p = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(content, p);
            yyjson_mut_obj_add_strcpy(doc, p, "type", "output_text");
            yyjson_mut_obj_add_strcpy(doc, p, "text", b->text ? b->text : "");
        } else if (b->type == CAGE_BLOCK_TOOL_CALL) {
            yyjson_mut_val *o = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(arr, o);
            yyjson_mut_obj_add_strcpy(doc, o, "type", "function_call");
            yyjson_mut_obj_add_strcpy(doc, o, "call_id", b->tool_call_id ? b->tool_call_id : "");
            yyjson_mut_obj_add_strcpy(doc, o, "name", b->tool_name ? b->tool_name : "");
            yyjson_mut_obj_add_strcpy(doc, o, "arguments", b->arguments ? b->arguments : "{}");
        }
    }
}

static void add_tools(yyjson_mut_doc *doc, yyjson_mut_val *root, const cage_context *ctx) {
    if (ctx->tools_len == 0) return;
    yyjson_mut_val *tools = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    for (size_t i = 0; i < ctx->tools_len; i++) {
        const cage_tool *t = &ctx->tools[i];
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(tools, tool);
        yyjson_mut_obj_add_strcpy(doc, tool, "type", "function");
        cage_json_tool(doc, tool, t, "parameters");
    }
}

static char *build_request(const cage_model *model, const cage_context *ctx,
                           const cage_stream_opts *opts) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "model", model->model ? model->model : "");
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    yyjson_mut_obj_add_bool(doc, root, "store", false);
    if (ctx->system_prompt && ctx->system_prompt[0])
        yyjson_mut_obj_add_strcpy(doc, root, "instructions", ctx->system_prompt);
    if (opts && opts->max_tokens > 0)
        yyjson_mut_obj_add_int(doc, root, "max_output_tokens", opts->max_tokens > 16 ? opts->max_tokens : 16);
    if (opts && opts->effort && strcmp(opts->effort, "off") != 0) {
        yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
        yyjson_mut_obj_add_strcpy(doc, reasoning, "effort", opts->effort);
    }

    yyjson_mut_val *input = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", input);
    for (size_t i = 0; i < ctx->messages_len; i++) add_input(doc, input, &ctx->messages[i]);

    add_tools(doc, root, ctx);

    size_t len = 0;
    char *body = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    return body;
}

// ---- streaming state ------------------------------------------------------

typedef struct {
    char *call_id;
    char *name;
    cage_buf args;
} call_acc;

typedef struct {
    cage_buf text;
    cage_buf reasoning;
    call_acc *calls;
    size_t calls_len;
    long tokens_in;
    long tokens_out;
    long tokens_total;
    char *status;
    char *incomplete;
    char *error;
    cage_stream_fn emit;
    void *user;
} resp_state;

static void grow_calls(resp_state *st, size_t count) {
    if (count <= st->calls_len) return;
    st->calls = realloc(st->calls, count * sizeof *st->calls);
    if (!st->calls) abort();
    memset(&st->calls[st->calls_len], 0, (count - st->calls_len) * sizeof *st->calls);
    st->calls_len = count;
}

static void on_data(const char *json, size_t len, void *user) {
    resp_state *st = user;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        if (!st->error) st->error = xstrdup("stream: invalid JSON chunk");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tv = yyjson_obj_get(root, "type");
    const char *type = yyjson_is_str(tv) ? yyjson_get_str(tv) : "";

    if (strcmp(type, "error") == 0) {
        yyjson_val *m = yyjson_obj_get(root, "message");
        yyjson_val *e = yyjson_obj_get(root, "error");
        if (yyjson_is_str(m)) cage_set_str(&st->error, yyjson_get_str(m));
        else if (yyjson_is_obj(e)) {
            yyjson_val *em = yyjson_obj_get(e, "message");
            if (yyjson_is_str(em)) cage_set_str(&st->error, yyjson_get_str(em));
        }
        if (!st->error) st->error = xstrdup("stream: provider error");
    } else if (strcmp(type, "response.output_item.added") == 0) {
        yyjson_val *item = yyjson_obj_get(root, "item");
        yyjson_val *it = yyjson_is_obj(item) ? yyjson_obj_get(item, "type") : NULL;
        if (yyjson_is_str(it) && strcmp(yyjson_get_str(it), "function_call") == 0) {
            yyjson_val *iv = yyjson_obj_get(root, "output_index");
            size_t index = yyjson_is_int(iv) ? (size_t)yyjson_get_int(iv) : st->calls_len;
            grow_calls(st, index + 1);
            call_acc *a = &st->calls[index];
            yyjson_val *id = yyjson_obj_get(item, "call_id");
            yyjson_val *nm = yyjson_obj_get(item, "name");
            if (yyjson_is_str(id)) cage_set_str(&a->call_id, yyjson_get_str(id));
            if (yyjson_is_str(nm)) cage_set_str(&a->name, yyjson_get_str(nm));
        }
    } else if (strcmp(type, "response.output_text.delta") == 0 ||
               strcmp(type, "response.refusal.delta") == 0) {
        yyjson_val *d = yyjson_obj_get(root, "delta");
        if (yyjson_is_str(d) && yyjson_get_str(d)[0]) {
            const char *s = yyjson_get_str(d);
            buf_add(&st->text, s, strlen(s));
            cage_emit_delta(st->emit, st->user, CAGE_STREAM_TEXT_DELTA, s);
        }
    } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
               strcmp(type, "response.reasoning_text.delta") == 0) {
        yyjson_val *d = yyjson_obj_get(root, "delta");
        if (yyjson_is_str(d) && yyjson_get_str(d)[0]) {
            const char *s = yyjson_get_str(d);
            buf_add(&st->reasoning, s, strlen(s));
            cage_emit_delta(st->emit, st->user, CAGE_STREAM_REASONING_DELTA, s);
        }
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        yyjson_val *iv = yyjson_obj_get(root, "output_index");
        size_t index = yyjson_is_int(iv) ? (size_t)yyjson_get_int(iv) : st->calls_len;
        if (index < st->calls_len) {
            yyjson_val *d = yyjson_obj_get(root, "delta");
            if (yyjson_is_str(d)) buf_add(&st->calls[index].args, yyjson_get_str(d), strlen(yyjson_get_str(d)));
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        yyjson_val *iv = yyjson_obj_get(root, "output_index");
        size_t index = yyjson_is_int(iv) ? (size_t)yyjson_get_int(iv) : st->calls_len;
        if (index < st->calls_len) {
            yyjson_val *a = yyjson_obj_get(root, "arguments");
            if (yyjson_is_str(a)) {
                st->calls[index].args.len = 0;
                if (st->calls[index].args.data) st->calls[index].args.data[0] = '\0';
                buf_add(&st->calls[index].args, yyjson_get_str(a), strlen(yyjson_get_str(a)));
            }
        }
    } else if (strcmp(type, "response.output_item.done") == 0) {
        yyjson_val *item = yyjson_obj_get(root, "item");
        yyjson_val *it = yyjson_is_obj(item) ? yyjson_obj_get(item, "type") : NULL;
        if (yyjson_is_str(it) && strcmp(yyjson_get_str(it), "function_call") == 0) {
            yyjson_val *iv = yyjson_obj_get(root, "output_index");
            size_t index = yyjson_is_int(iv) ? (size_t)yyjson_get_int(iv) : st->calls_len;
            if (index < st->calls_len) {
                call_acc *a = &st->calls[index];
                yyjson_val *id = yyjson_obj_get(item, "call_id");
                yyjson_val *nm = yyjson_obj_get(item, "name");
                yyjson_val *ar = yyjson_obj_get(item, "arguments");
                if (yyjson_is_str(id)) cage_set_str(&a->call_id, yyjson_get_str(id));
                if (yyjson_is_str(nm)) cage_set_str(&a->name, yyjson_get_str(nm));
                if (yyjson_is_str(ar) && !a->args.len)
                    buf_add(&a->args, yyjson_get_str(ar), strlen(yyjson_get_str(ar)));
            }
        }
    } else if (strcmp(type, "response.completed") == 0 ||
               strcmp(type, "response.incomplete") == 0) {
        yyjson_val *resp = yyjson_obj_get(root, "response");
        if (yyjson_is_obj(resp)) {
            yyjson_val *sv = yyjson_obj_get(resp, "status");
            if (yyjson_is_str(sv)) cage_set_str(&st->status, yyjson_get_str(sv));
            yyjson_val *det = yyjson_obj_get(resp, "incomplete_details");
            if (yyjson_is_obj(det)) {
                yyjson_val *r = yyjson_obj_get(det, "reason");
                if (yyjson_is_str(r)) cage_set_str(&st->incomplete, yyjson_get_str(r));
            }
            yyjson_val *usage = yyjson_obj_get(resp, "usage");
            if (yyjson_is_obj(usage)) {
                yyjson_val *v;
                if (yyjson_is_num(v = yyjson_obj_get(usage, "input_tokens"))) st->tokens_in = (long)yyjson_get_num(v);
                if (yyjson_is_num(v = yyjson_obj_get(usage, "output_tokens"))) st->tokens_out = (long)yyjson_get_num(v);
                if (yyjson_is_num(v = yyjson_obj_get(usage, "total_tokens"))) st->tokens_total = (long)yyjson_get_num(v);
            }
        }
    } else if (strcmp(type, "response.failed") == 0) {
        yyjson_val *resp = yyjson_obj_get(root, "response");
        yyjson_val *err = yyjson_is_obj(resp) ? yyjson_obj_get(resp, "error") : NULL;
        yyjson_val *m = yyjson_is_obj(err) ? yyjson_obj_get(err, "message") : NULL;
        cage_set_str(&st->error, yyjson_is_str(m) ? yyjson_get_str(m) : "response failed");
    }
    yyjson_doc_free(doc);
}

static void free_state(resp_state *st) {
    free(st->text.data);
    free(st->reasoning.data);
    for (size_t i = 0; i < st->calls_len; i++) {
        free(st->calls[i].call_id);
        free(st->calls[i].name);
        free(st->calls[i].args.data);
    }
    free(st->calls);
    free(st->status);
    free(st->incomplete);
    free(st->error);
}

static void finalize(resp_state *st, cage_message *out) {
    cage_add_text_block(out, CAGE_BLOCK_THINKING, st->reasoning.data);
    cage_add_text_block(out, CAGE_BLOCK_TEXT, st->text.data);
    for (size_t i = 0; i < st->calls_len; i++) {
        call_acc *a = &st->calls[i];
        if (!a->name) continue;
        cage_add_tool_block(out, a->call_id, a->name, a->args.len ? a->args.data : NULL, st->emit, st->user);
    }
    out->tokens_in = st->tokens_in;
    out->tokens_out = st->tokens_out;
    out->tokens_total = st->tokens_total;

    bool has_calls = out->blocks_len > 0 && out->blocks[out->blocks_len - 1].type == CAGE_BLOCK_TOOL_CALL;
    for (size_t i = 0; i < out->blocks_len && !has_calls; i++)
        if (out->blocks[i].type == CAGE_BLOCK_TOOL_CALL) has_calls = true;

    if (st->status && strcmp(st->status, "incomplete") == 0) {
        if (st->incomplete && strcmp(st->incomplete, "max_output_tokens") == 0) {
            out->stop_reason = xstrdup("length");
        } else {
            out->stop_reason = xstrdup("error");
            out->error_message = xstrdup(st->incomplete ? st->incomplete : "response incomplete");
        }
    } else if (st->status && (strcmp(st->status, "failed") == 0 || strcmp(st->status, "cancelled") == 0)) {
        out->stop_reason = xstrdup("error");
        out->error_message = xstrdup(st->error ? st->error : "response failed");
    } else if (has_calls) {
        out->stop_reason = xstrdup("toolUse");
    } else {
        out->stop_reason = xstrdup("stop");
    }
}

// ---- transport ------------------------------------------------------------


int cage_openai_responses_stream(const cage_model *model, const cage_context *ctx,
                                 const cage_stream_opts *opts,
                                 cage_stream_fn emit, void *user,
                                 const volatile bool *cancel,
                                 cage_message *out, char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    out->role = CAGE_ROLE_ASSISTANT;

    char *url = cage_url_endpoint(model->base_url, "responses");
    if (!url) return cage_fail(err, "provider: no base url");
    char *body = build_request(model, ctx, opts);
    if (!body) {
        free(url);
        return cage_fail(err, "provider: cannot build request");
    }

    resp_state st = {0};
    st.emit = emit;
    st.user = user;

    cage_api_result r = cage_api_post(url, body, "Authorization: Bearer ",
                                      model->api_key, NULL, cancel, on_data, &st, st.error, err);

    int rc = 0;
    if (r == CAGE_API_ABORTED) {
        out->stop_reason = xstrdup("aborted");
    } else if (r == CAGE_API_TRANSPORT_ERROR) {
        rc = -1;
    } else if (r == CAGE_API_STREAM_ERROR) {
        out->stop_reason = xstrdup("error");
        out->error_message = xstrdup(st.error);
    } else {
        finalize(&st, out);
    }
    free_state(&st);
    return rc;
}
