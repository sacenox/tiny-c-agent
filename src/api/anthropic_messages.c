// Anthropic Messages API streaming adapter.

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "http.h"
#include "util.h"

#define ANTHROPIC_VERSION "2023-06-01"

// ---- request --------------------------------------------------------------

static void add_image_block(yyjson_mut_doc *doc, yyjson_mut_val *content,
                            const tiny_block *b) {
    yyjson_mut_val *img = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(content, img);
    yyjson_mut_obj_add_strcpy(doc, img, "type", "image");
    yyjson_mut_val *src = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, img, "source", src);
    yyjson_mut_obj_add_strcpy(doc, src, "type", "base64");
    yyjson_mut_obj_add_strcpy(doc, src, "media_type", b->media_type ? b->media_type : "");
    yyjson_mut_obj_add_strcpy(doc, src, "data", b->data ? b->data : "");
}

static void add_tool_result(yyjson_mut_doc *doc, yyjson_mut_val *messages, const tiny_message *m) {
    yyjson_mut_val *um = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(messages, um);
    yyjson_mut_obj_add_strcpy(doc, um, "role", "user");
    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, um, "content", content);

    yyjson_mut_val *tr = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(content, tr);
    yyjson_mut_obj_add_strcpy(doc, tr, "type", "tool_result");
    yyjson_mut_obj_add_strcpy(doc, tr, "tool_use_id", m->tool_call_id ? m->tool_call_id : "");
    if (m->is_error) yyjson_mut_obj_add_bool(doc, tr, "is_error", true);

    bool has_images = tiny_message_has_image(m);
    if (!has_images) {
        tiny_buf t = {0};
        for (size_t i = 0; i < m->blocks_len; i++)
            if (m->blocks[i].type == TINY_BLOCK_TEXT && m->blocks[i].text)
                buf_add(&t, m->blocks[i].text, strlen(m->blocks[i].text));
        yyjson_mut_obj_add_strcpy(doc, tr, "content", t.data ? t.data : "");
        free(t.data);
        return;
    }
    yyjson_mut_val *blocks = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, tr, "content", blocks);
    for (size_t i = 0; i < m->blocks_len; i++) {
        const tiny_block *b = &m->blocks[i];
        if (b->type == TINY_BLOCK_TEXT) {
            yyjson_mut_val *tx = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(blocks, tx);
            yyjson_mut_obj_add_strcpy(doc, tx, "type", "text");
            yyjson_mut_obj_add_strcpy(doc, tx, "text", b->text ? b->text : "");
        } else if (b->type == TINY_BLOCK_IMAGE) {
            add_image_block(doc, blocks, b);
        }
    }
}

static void add_message(yyjson_mut_doc *doc, yyjson_mut_val *messages, const tiny_message *m) {
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(messages, o);
    yyjson_mut_obj_add_strcpy(doc, o, "role", m->role == TINY_ROLE_ASSISTANT ? "assistant" : "user");
    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, o, "content", content);
    for (size_t i = 0; i < m->blocks_len; i++) {
        const tiny_block *b = &m->blocks[i];
        if (b->type == TINY_BLOCK_TEXT) {
            yyjson_mut_val *tx = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(content, tx);
            yyjson_mut_obj_add_strcpy(doc, tx, "type", "text");
            yyjson_mut_obj_add_strcpy(doc, tx, "text", b->text ? b->text : "");
        } else if (b->type == TINY_BLOCK_IMAGE) {
            add_image_block(doc, content, b);
        } else if (b->type == TINY_BLOCK_TOOL_CALL) {
            yyjson_mut_val *tu = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(content, tu);
            yyjson_mut_obj_add_strcpy(doc, tu, "type", "tool_use");
            yyjson_mut_obj_add_strcpy(doc, tu, "id", b->tool_call_id ? b->tool_call_id : "");
            yyjson_mut_obj_add_strcpy(doc, tu, "name", b->tool_name ? b->tool_name : "");
            yyjson_doc *adoc = b->arguments ? yyjson_read(b->arguments, strlen(b->arguments), 0) : NULL;
            if (adoc && yyjson_is_obj(yyjson_doc_get_root(adoc))) {
                yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(adoc));
                yyjson_mut_obj_add_val(doc, tu, "input", copy);
            } else {
                yyjson_mut_val *empty = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, tu, "input", empty);
            }
            if (adoc) yyjson_doc_free(adoc);
        }
    }
}

static void add_tools(yyjson_mut_doc *doc, yyjson_mut_val *root, const tiny_context *ctx) {
    if (ctx->tools_len == 0) return;
    yyjson_mut_val *tools = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    for (size_t i = 0; i < ctx->tools_len; i++) {
        const tiny_tool *t = &ctx->tools[i];
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(tools, tool);
        tiny_json_tool(doc, tool, t, "input_schema");
    }
}

// Thinking budget per effort level. Anthropic requires max_tokens > budget.
static long budget_for(const char *effort) {
    if (!effort) return 0;
    if (strcmp(effort, "minimal") == 0) return 1024;
    if (strcmp(effort, "low") == 0) return 2048;
    if (strcmp(effort, "medium") == 0) return 4096;
    if (strcmp(effort, "high") == 0) return 8192;
    if (strcmp(effort, "xhigh") == 0) return 16384;
    if (strcmp(effort, "max") == 0) return 32768;
    return 0;
}

static char *build_request(const tiny_model *model, const tiny_context *ctx,
                           const tiny_stream_opts *opts) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    long budget = opts ? budget_for(opts->effort) : 0;
    long max_tokens = opts && opts->max_tokens > 0 ? opts->max_tokens : 8192;
    if (budget > 0 && max_tokens <= budget) max_tokens = budget + 4096;

    yyjson_mut_obj_add_strcpy(doc, root, "model", model->model ? model->model : "");
    yyjson_mut_obj_add_int(doc, root, "max_tokens", max_tokens);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    if (ctx->system_prompt && ctx->system_prompt[0])
        yyjson_mut_obj_add_strcpy(doc, root, "system", ctx->system_prompt);
    if (budget > 0) {
        yyjson_mut_val *thinking = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
        yyjson_mut_obj_add_strcpy(doc, thinking, "type", "enabled");
        yyjson_mut_obj_add_int(doc, thinking, "budget_tokens", budget);
    }

    yyjson_mut_val *messages = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", messages);
    for (size_t i = 0; i < ctx->messages_len;) {
        if (ctx->messages[i].role == TINY_ROLE_TOOL) {
            // All results of one assistant turn share a single user message.
            size_t j = i;
            for (; j < ctx->messages_len && ctx->messages[j].role == TINY_ROLE_TOOL; j++)
                add_tool_result(doc, messages, &ctx->messages[j]);
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

enum { BLK_TEXT, BLK_THINKING, BLK_TOOL };

typedef struct {
    int type;
    tiny_buf text;
    char *id;
    char *name;
    tiny_buf args;
} block_acc;

typedef struct {
    block_acc *blocks;
    size_t blocks_len;
    long tokens_in;
    long tokens_out;
    char *stop_reason;
    char *error;
    tiny_stream_fn emit;
    void *user;
} anth_state;

static block_acc *block_at(anth_state *st, size_t index) {
    if (index >= st->blocks_len) {
        st->blocks = realloc(st->blocks, (index + 1) * sizeof *st->blocks);
        if (!st->blocks) abort();
        memset(&st->blocks[st->blocks_len], 0, (index + 1 - st->blocks_len) * sizeof *st->blocks);
        st->blocks_len = index + 1;
    }
    return &st->blocks[index];
}

static void on_data(const char *json, size_t len, void *user) {
    anth_state *st = user;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        if (!st->error) st->error = xstrdup("stream: invalid JSON chunk");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tv = yyjson_obj_get(root, "type");
    const char *type = yyjson_is_str(tv) ? yyjson_get_str(tv) : "";

    if (strcmp(type, "error") == 0) {
        yyjson_val *err = yyjson_obj_get(root, "error");
        yyjson_val *m = yyjson_is_obj(err) ? yyjson_obj_get(err, "message") : NULL;
        tiny_set_str(&st->error, yyjson_is_str(m) ? yyjson_get_str(m) : "stream: provider error");
    } else if (strcmp(type, "message_start") == 0) {
        yyjson_val *msg = yyjson_obj_get(root, "message");
        yyjson_val *usage = yyjson_is_obj(msg) ? yyjson_obj_get(msg, "usage") : NULL;
        if (yyjson_is_obj(usage)) {
            yyjson_val *v;
            if (yyjson_is_num(v = yyjson_obj_get(usage, "input_tokens"))) st->tokens_in = (long)yyjson_get_num(v);
            if (yyjson_is_num(v = yyjson_obj_get(usage, "output_tokens"))) st->tokens_out = (long)yyjson_get_num(v);
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        yyjson_val *iv = yyjson_obj_get(root, "index");
        if (!yyjson_is_int(iv)) {
            yyjson_doc_free(doc);
            return;
        }
        block_acc *b = block_at(st, (size_t)yyjson_get_int(iv));
        yyjson_val *cb = yyjson_obj_get(root, "content_block");
        yyjson_val *bt = yyjson_is_obj(cb) ? yyjson_obj_get(cb, "type") : NULL;
        const char *bts = yyjson_is_str(bt) ? yyjson_get_str(bt) : "";
        if (strcmp(bts, "tool_use") == 0) {
            b->type = BLK_TOOL;
            yyjson_val *id = yyjson_obj_get(cb, "id");
            yyjson_val *nm = yyjson_obj_get(cb, "name");
            if (yyjson_is_str(id)) tiny_set_str(&b->id, yyjson_get_str(id));
            if (yyjson_is_str(nm)) tiny_set_str(&b->name, yyjson_get_str(nm));
        } else if (strcmp(bts, "thinking") == 0 || strcmp(bts, "redacted_thinking") == 0) {
            b->type = BLK_THINKING;
        } else {
            b->type = BLK_TEXT;
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        yyjson_val *iv = yyjson_obj_get(root, "index");
        yyjson_val *d = yyjson_obj_get(root, "delta");
        if (!yyjson_is_int(iv) || !yyjson_is_obj(d)) {
            yyjson_doc_free(doc);
            return;
        }
        size_t index = (size_t)yyjson_get_int(iv);
        if (index >= st->blocks_len) {
            yyjson_doc_free(doc);
            return;
        }
        block_acc *b = &st->blocks[index];
        yyjson_val *dt = yyjson_obj_get(d, "type");
        const char *dts = yyjson_is_str(dt) ? yyjson_get_str(dt) : "";
        if (strcmp(dts, "text_delta") == 0) {
            yyjson_val *t = yyjson_obj_get(d, "text");
            if (yyjson_is_str(t) && yyjson_get_str(t)[0]) {
                buf_add(&b->text, yyjson_get_str(t), strlen(yyjson_get_str(t)));
                tiny_emit_delta(st->emit, st->user, TINY_STREAM_TEXT_DELTA, yyjson_get_str(t));
            }
        } else if (strcmp(dts, "thinking_delta") == 0) {
            yyjson_val *t = yyjson_obj_get(d, "thinking");
            if (yyjson_is_str(t) && yyjson_get_str(t)[0]) {
                buf_add(&b->text, yyjson_get_str(t), strlen(yyjson_get_str(t)));
                tiny_emit_delta(st->emit, st->user, TINY_STREAM_REASONING_DELTA, yyjson_get_str(t));
            }
        } else if (strcmp(dts, "input_json_delta") == 0) {
            yyjson_val *p = yyjson_obj_get(d, "partial_json");
            if (yyjson_is_str(p)) buf_add(&b->args, yyjson_get_str(p), strlen(yyjson_get_str(p)));
        }
    } else if (strcmp(type, "message_delta") == 0) {
        yyjson_val *d = yyjson_obj_get(root, "delta");
        yyjson_val *sr = yyjson_is_obj(d) ? yyjson_obj_get(d, "stop_reason") : NULL;
        if (yyjson_is_str(sr)) tiny_set_str(&st->stop_reason, yyjson_get_str(sr));
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        if (yyjson_is_obj(usage)) {
            yyjson_val *v;
            if (yyjson_is_num(v = yyjson_obj_get(usage, "output_tokens"))) st->tokens_out = (long)yyjson_get_num(v);
        }
    }
    yyjson_doc_free(doc);
}

static void free_state(anth_state *st) {
    for (size_t i = 0; i < st->blocks_len; i++) {
        free(st->blocks[i].text.data);
        free(st->blocks[i].id);
        free(st->blocks[i].name);
        free(st->blocks[i].args.data);
    }
    free(st->blocks);
    free(st->stop_reason);
    free(st->error);
}

static const char *map_stop(const char *reason, bool has_calls) {
    if (reason && strcmp(reason, "max_tokens") == 0) return "length";
    if (has_calls) return "toolUse";
    return "stop";
}

static void finalize(anth_state *st, tiny_message *out) {
    bool has_calls = false;
    for (size_t i = 0; i < st->blocks_len; i++) {
        block_acc *b = &st->blocks[i];
        if (b->type == BLK_TOOL) has_calls = true;
    }
    for (size_t i = 0; i < st->blocks_len; i++) {
        block_acc *b = &st->blocks[i];
        if (b->type == BLK_THINKING)
            tiny_add_text_block(out, TINY_BLOCK_THINKING, b->text.data);
        else if (b->type == BLK_TEXT)
            tiny_add_text_block(out, TINY_BLOCK_TEXT, b->text.data);
    }
    for (size_t i = 0; i < st->blocks_len; i++) {
        block_acc *b = &st->blocks[i];
        if (b->type != BLK_TOOL) continue;
        tiny_add_tool_block(out, b->id, b->name, b->args.len ? b->args.data : NULL, st->emit, st->user);
    }
    out->tokens_in = st->tokens_in;
    out->tokens_out = st->tokens_out;
    out->tokens_total = st->tokens_in + st->tokens_out;
    out->stop_reason = xstrdup(map_stop(st->stop_reason, has_calls));
}

// ---- transport ------------------------------------------------------------


int tiny_anthropic_messages_stream(const tiny_model *model, const tiny_context *ctx,
                                   const tiny_stream_opts *opts,
                                   tiny_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   tiny_message *out, char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    out->role = TINY_ROLE_ASSISTANT;

    char *url = tiny_url_endpoint(model->base_url, "v1/messages");
    if (!url) return tiny_fail(err, "provider: no base url");
    char *body = build_request(model, ctx, opts);
    if (!body) {
        free(url);
        return tiny_fail(err, "provider: cannot build request");
    }

    anth_state st = {0};
    st.emit = emit;
    st.user = user;

    const char *extra[] = {"anthropic-version: " ANTHROPIC_VERSION, NULL};
    tiny_api_result r = tiny_api_post(url, body, "x-api-key: ", model->api_key,
                                      extra, cancel, on_data, &st, st.error, err);

    int rc = 0;
    if (r == TINY_API_ABORTED) {
        out->stop_reason = xstrdup("aborted");
    } else if (r == TINY_API_TRANSPORT_ERROR) {
        rc = -1;
    } else if (r == TINY_API_STREAM_ERROR) {
        out->stop_reason = xstrdup("error");
        out->error_message = xstrdup(st.error);
    } else {
        finalize(&st, out);
    }
    free_state(&st);
    return rc;
}
