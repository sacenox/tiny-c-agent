// OpenAI Responses API streaming adapter.

#include "api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "http.h"

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

static int fail(char **err, const char *fmt, ...) {
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    *err = xstrdup(detail);
    return -1;
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

static char *join_text(const cage_message *m) {
    buf b = {0};
    for (size_t i = 0; i < m->blocks_len; i++)
        if (m->blocks[i].type == CAGE_BLOCK_TEXT && m->blocks[i].text)
            buf_add(&b, m->blocks[i].text, strlen(m->blocks[i].text));
    buf_add(&b, "", 0);
    return b.data ? b.data : xstrdup("");
}

// ---- request --------------------------------------------------------------

static void add_input(yyjson_mut_doc *doc, yyjson_mut_val *arr, const cage_message *m) {
    if (m->role == CAGE_ROLE_TOOL) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(arr, o);
        yyjson_mut_obj_add_strcpy(doc, o, "type", "function_call_output");
        yyjson_mut_obj_add_strcpy(doc, o, "call_id", m->tool_call_id ? m->tool_call_id : "");
        char *t = join_text(m);
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
        yyjson_mut_obj_add_strcpy(doc, tool, "name", t->name);
        yyjson_mut_obj_add_strcpy(doc, tool, "description", t->description ? t->description : "");
        const char *params = t->parameters ? t->parameters : "{}";
        yyjson_doc *pdoc = yyjson_read(params, strlen(params), 0);
        if (pdoc) {
            yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(pdoc));
            yyjson_mut_obj_add_val(doc, tool, "parameters", copy);
            yyjson_doc_free(pdoc);
        }
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
    buf args;
} call_acc;

typedef struct {
    buf text;
    buf reasoning;
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

static void emit_delta(resp_state *st, cage_stream_type type, const char *delta) {
    if (!st->emit) return;
    cage_stream_event ev = {.type = type, .delta = delta};
    st->emit(&ev, st->user);
}

static void grow_calls(resp_state *st, size_t count) {
    if (count <= st->calls_len) return;
    st->calls = realloc(st->calls, count * sizeof *st->calls);
    if (!st->calls) abort();
    memset(&st->calls[st->calls_len], 0, (count - st->calls_len) * sizeof *st->calls);
    st->calls_len = count;
}

static void set_str(char **dst, const char *src) {
    free(*dst);
    *dst = xstrdup(src ? src : "");
}

static void handle_data(resp_state *st, const char *json, size_t len) {
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
        if (yyjson_is_str(m)) set_str(&st->error, yyjson_get_str(m));
        else if (yyjson_is_obj(e)) {
            yyjson_val *em = yyjson_obj_get(e, "message");
            if (yyjson_is_str(em)) set_str(&st->error, yyjson_get_str(em));
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
            if (yyjson_is_str(id)) set_str(&a->call_id, yyjson_get_str(id));
            if (yyjson_is_str(nm)) set_str(&a->name, yyjson_get_str(nm));
        }
    } else if (strcmp(type, "response.output_text.delta") == 0 ||
               strcmp(type, "response.refusal.delta") == 0) {
        yyjson_val *d = yyjson_obj_get(root, "delta");
        if (yyjson_is_str(d) && yyjson_get_str(d)[0]) {
            const char *s = yyjson_get_str(d);
            buf_add(&st->text, s, strlen(s));
            emit_delta(st, CAGE_STREAM_TEXT_DELTA, s);
        }
    } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
               strcmp(type, "response.reasoning_text.delta") == 0) {
        yyjson_val *d = yyjson_obj_get(root, "delta");
        if (yyjson_is_str(d) && yyjson_get_str(d)[0]) {
            const char *s = yyjson_get_str(d);
            buf_add(&st->reasoning, s, strlen(s));
            emit_delta(st, CAGE_STREAM_REASONING_DELTA, s);
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
                if (yyjson_is_str(id)) set_str(&a->call_id, yyjson_get_str(id));
                if (yyjson_is_str(nm)) set_str(&a->name, yyjson_get_str(nm));
                if (yyjson_is_str(ar) && !a->args.len)
                    buf_add(&a->args, yyjson_get_str(ar), strlen(yyjson_get_str(ar)));
            }
        }
    } else if (strcmp(type, "response.completed") == 0 ||
               strcmp(type, "response.incomplete") == 0) {
        yyjson_val *resp = yyjson_obj_get(root, "response");
        if (yyjson_is_obj(resp)) {
            yyjson_val *sv = yyjson_obj_get(resp, "status");
            if (yyjson_is_str(sv)) set_str(&st->status, yyjson_get_str(sv));
            yyjson_val *det = yyjson_obj_get(resp, "incomplete_details");
            if (yyjson_is_obj(det)) {
                yyjson_val *r = yyjson_obj_get(det, "reason");
                if (yyjson_is_str(r)) set_str(&st->incomplete, yyjson_get_str(r));
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
        set_str(&st->error, yyjson_is_str(m) ? yyjson_get_str(m) : "response failed");
    }
    yyjson_doc_free(doc);
}

static void on_data(const char *json, size_t len, void *user) {
    handle_data(user, json, len);
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
    if (st->reasoning.len > 0) {
        cage_block *b = cage_message_add_block(out, CAGE_BLOCK_THINKING);
        b->text = xstrdup(st->reasoning.data);
    }
    if (st->text.len > 0) {
        cage_block *b = cage_message_add_block(out, CAGE_BLOCK_TEXT);
        b->text = xstrdup(st->text.data);
    }
    for (size_t i = 0; i < st->calls_len; i++) {
        call_acc *a = &st->calls[i];
        if (!a->name) continue;
        cage_block *b = cage_message_add_block(out, CAGE_BLOCK_TOOL_CALL);
        b->tool_call_id = xstrdup(a->call_id ? a->call_id : "");
        b->tool_name = xstrdup(a->name);
        b->arguments = xstrdup(a->args.len ? a->args.data : "{}");
        if (st->emit) {
            cage_stream_event ev = {
                .type = CAGE_STREAM_TOOL_CALL,
                .tool_call_id = b->tool_call_id,
                .tool_name = b->tool_name,
                .tool_arguments = b->arguments,
            };
            st->emit(&ev, st->user);
        }
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

static char *endpoint_url(const char *base) {
    if (!base || !base[0]) return NULL;
    size_t n = strlen(base);
    const char *path = "responses";
    bool slash = base[n - 1] == '/';
    size_t size = n + strlen(path) + 2;
    char *url = xmalloc(size);
    snprintf(url, size, slash ? "%s%s" : "%s/%s", base, path);
    return url;
}

int cage_openai_responses_stream(const cage_model *model, const cage_context *ctx,
                                 const cage_stream_opts *opts,
                                 cage_stream_fn emit, void *user,
                                 const volatile bool *cancel,
                                 cage_message *out, char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    out->role = CAGE_ROLE_ASSISTANT;

    char *url = endpoint_url(model->base_url);
    if (!url) return fail(err, "provider: no base url");
    char *body = build_request(model, ctx, opts);
    if (!body) {
        free(url);
        return fail(err, "provider: cannot build request");
    }

    resp_state st = {0};
    st.emit = emit;
    st.user = user;

    char *auth = NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    if (model->api_key && model->api_key[0]) {
        size_t n = strlen(model->api_key) + 24;
        auth = xmalloc(n);
        snprintf(auth, n, "Authorization: Bearer %s", model->api_key);
        headers = curl_slist_append(headers, auth);
    }

    cage_http_request req = {
        .url = url,
        .body = body,
        .headers = headers,
        .cancel = cancel,
        .on_data = on_data,
        .user = &st,
    };
    cage_http_stream(&req);

    int rc = 0;
    if (req.aborted) {
        out->stop_reason = xstrdup("aborted");
    } else if (req.error) {
        *err = req.error;
        req.error = NULL;
        rc = -1;
    } else if (st.error) {
        out->stop_reason = xstrdup("error");
        out->error_message = xstrdup(st.error);
    } else {
        finalize(&st, out);
    }

    curl_slist_free_all(headers);
    free(auth);
    free(req.error);
    free(url);
    free(body);
    free_state(&st);
    return rc;
}
