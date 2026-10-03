// Google Generative AI (Gemini) streaming adapter.

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "http.h"
#include "util.h"

// ---- request --------------------------------------------------------------

static void add_text_part(yyjson_mut_doc *doc, yyjson_mut_val *parts, const char *text) {
    yyjson_mut_val *p = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(parts, p);
    yyjson_mut_obj_add_strcpy(doc, p, "text", text ? text : "");
}

static void add_function_call(yyjson_mut_doc *doc, yyjson_mut_val *parts, const tiny_block *b) {
    yyjson_mut_val *p = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(parts, p);
    yyjson_mut_val *fc = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, p, "functionCall", fc);
    yyjson_mut_obj_add_strcpy(doc, fc, "name", b->tool_name ? b->tool_name : "");
    yyjson_doc *adoc = b->arguments ? yyjson_read(b->arguments, strlen(b->arguments), 0) : NULL;
    if (adoc && yyjson_is_obj(yyjson_doc_get_root(adoc))) {
        yyjson_mut_obj_add_val(doc, fc, "args", yyjson_val_mut_copy(doc, yyjson_doc_get_root(adoc)));
    } else {
        yyjson_mut_obj_add_val(doc, fc, "args", yyjson_mut_obj(doc));
    }
    if (adoc) yyjson_doc_free(adoc);
}

static void add_message(yyjson_mut_doc *doc, yyjson_mut_val *contents, const tiny_message *m) {
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(contents, o);
    yyjson_mut_obj_add_strcpy(doc, o, "role", m->role == TINY_ROLE_ASSISTANT ? "model" : "user");
    yyjson_mut_val *parts = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, o, "parts", parts);
    for (size_t i = 0; i < m->blocks_len; i++) {
        const tiny_block *b = &m->blocks[i];
        if (b->type == TINY_BLOCK_TEXT) {
            add_text_part(doc, parts, b->text);
        } else if (b->type == TINY_BLOCK_IMAGE) {
            yyjson_mut_val *p = yyjson_mut_obj(doc);
            yyjson_mut_arr_add_val(parts, p);
            yyjson_mut_val *inl = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, p, "inlineData", inl);
            yyjson_mut_obj_add_strcpy(doc, inl, "mimeType", b->media_type ? b->media_type : "");
            yyjson_mut_obj_add_strcpy(doc, inl, "data", b->data ? b->data : "");
        } else if (b->type == TINY_BLOCK_TOOL_CALL) {
            add_function_call(doc, parts, b);
        }
    }
}

// A run of tool results becomes one user turn of functionResponse parts.
static const char *tool_name_for(const tiny_context *ctx, const char *id) {
    for (size_t i = 0; i < ctx->messages_len; i++) {
        const tiny_message *m = &ctx->messages[i];
        for (size_t k = 0; k < m->blocks_len; k++) {
            const tiny_block *b = &m->blocks[k];
            if (b->type == TINY_BLOCK_TOOL_CALL && b->tool_call_id && id &&
                strcmp(b->tool_call_id, id) == 0)
                return b->tool_name ? b->tool_name : "";
        }
    }
    return "";
}

static void add_tool_results(yyjson_mut_doc *doc, yyjson_mut_val *contents,
                             const tiny_context *ctx, size_t from, size_t to) {
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(contents, o);
    yyjson_mut_obj_add_strcpy(doc, o, "role", "user");
    yyjson_mut_val *parts = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, o, "parts", parts);
    for (size_t i = from; i < to; i++) {
        const tiny_message *m = &ctx->messages[i];
        char *t = tiny_message_text(m);
        yyjson_mut_val *p = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(parts, p);
        yyjson_mut_val *fr = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, p, "functionResponse", fr);
        yyjson_mut_obj_add_strcpy(doc, fr, "name", tool_name_for(ctx, m->tool_call_id));
        yyjson_mut_val *resp = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, fr, "response", resp);
        yyjson_mut_obj_add_strcpy(doc, resp, m->is_error ? "error" : "output", t);
        free(t);
    }
}

static void add_tools(yyjson_mut_doc *doc, yyjson_mut_val *root, const tiny_context *ctx) {
    if (ctx->tools_len == 0) return;
    yyjson_mut_val *tools = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    yyjson_mut_val *group = yyjson_mut_obj(doc);
    yyjson_mut_arr_add_val(tools, group);
    yyjson_mut_val *decls = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, group, "functionDeclarations", decls);
    for (size_t i = 0; i < ctx->tools_len; i++) {
        yyjson_mut_val *d = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(decls, d);
        tiny_json_tool(doc, d, &ctx->tools[i], "parametersJsonSchema");
    }
}

static long budget_for(const char *effort) {
    if (!effort) return -1;
    if (strcmp(effort, "off") == 0) return 0;
    if (strcmp(effort, "minimal") == 0) return 512;
    if (strcmp(effort, "low") == 0) return 1024;
    if (strcmp(effort, "medium") == 0) return 4096;
    if (strcmp(effort, "high") == 0) return 8192;
    if (strcmp(effort, "xhigh") == 0) return 16384;
    if (strcmp(effort, "max") == 0) return 24576;
    return -1;
}

static char *build_request(const tiny_context *ctx, const tiny_stream_opts *opts) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *contents = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "contents", contents);
    for (size_t i = 0; i < ctx->messages_len;) {
        if (ctx->messages[i].role == TINY_ROLE_TOOL) {
            size_t j = i;
            for (; j < ctx->messages_len && ctx->messages[j].role == TINY_ROLE_TOOL; j++) {}
            add_tool_results(doc, contents, ctx, i, j);
            i = j;
        } else {
            add_message(doc, contents, &ctx->messages[i]);
            i++;
        }
    }

    if (ctx->system_prompt && ctx->system_prompt[0]) {
        yyjson_mut_val *si = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "systemInstruction", si);
        yyjson_mut_val *parts = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, si, "parts", parts);
        add_text_part(doc, parts, ctx->system_prompt);
    }

    add_tools(doc, root, ctx);

    yyjson_mut_val *gen = NULL;
    if (opts && opts->max_tokens > 0) {
        gen = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, gen, "maxOutputTokens", opts->max_tokens);
    }
    long budget = opts ? budget_for(opts->effort) : -1;
    if (budget >= 0) {
        if (!gen) gen = yyjson_mut_obj(doc);
        yyjson_mut_val *tc = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, gen, "thinkingConfig", tc);
        if (budget > 0) yyjson_mut_obj_add_bool(doc, tc, "includeThoughts", true);
        yyjson_mut_obj_add_int(doc, tc, "thinkingBudget", budget);
    }
    if (gen) yyjson_mut_obj_add_val(doc, root, "generationConfig", gen);

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
} gcall;

typedef struct {
    tiny_buf text;
    tiny_buf reasoning;
    gcall *calls;
    size_t calls_len;
    long tokens_in;
    long tokens_out;
    long tokens_total;
    char *finish;
    char *error;
    tiny_stream_fn emit;
    void *user;
} google_state;

static void add_call(google_state *st, const char *name, yyjson_val *args) {
    st->calls = realloc(st->calls, (st->calls_len + 1) * sizeof *st->calls);
    if (!st->calls) abort();
    gcall *c = &st->calls[st->calls_len++];
    memset(c, 0, sizeof *c);
    c->name = xstrdup(name ? name : "");
    char id[64];
    snprintf(id, sizeof id, "%s_%zu", c->name, st->calls_len);
    c->id = xstrdup(id);
    if (args) {
        size_t len = 0;
        char *txt = yyjson_val_write(args, 0, &len);
        if (txt) {
            buf_add(&c->args, txt, len);
            free(txt);
        }
    }
}

static void on_data(const char *json, size_t len, void *user) {
    google_state *st = user;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        if (!st->error) st->error = xstrdup("stream: invalid JSON chunk");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);

    yyjson_val *err = yyjson_obj_get(root, "error");
    if (yyjson_is_obj(err)) {
        yyjson_val *m = yyjson_obj_get(err, "message");
        if (!st->error) st->error = xstrdup(yyjson_is_str(m) ? yyjson_get_str(m) : "stream: provider error");
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *cands = yyjson_obj_get(root, "candidates");
    if (yyjson_is_arr(cands) && yyjson_arr_size(cands) > 0) {
        yyjson_val *cand = yyjson_arr_get_first(cands);
        yyjson_val *content = yyjson_obj_get(cand, "content");
        yyjson_val *parts = yyjson_is_obj(content) ? yyjson_obj_get(content, "parts") : NULL;
        if (yyjson_is_arr(parts)) {
            size_t idx, max;
            yyjson_val *part;
            yyjson_arr_foreach(parts, idx, max, part) {
                yyjson_val *fc = yyjson_obj_get(part, "functionCall");
                if (yyjson_is_obj(fc)) {
                    yyjson_val *name = yyjson_obj_get(fc, "name");
                    yyjson_val *args = yyjson_obj_get(fc, "args");
                    add_call(st, yyjson_is_str(name) ? yyjson_get_str(name) : "", args);
                    continue;
                }
                yyjson_val *txt = yyjson_obj_get(part, "text");
                if (!yyjson_is_str(txt) || !yyjson_get_str(txt)[0]) continue;
                yyjson_val *thought = yyjson_obj_get(part, "thought");
                const char *s = yyjson_get_str(txt);
                if (yyjson_is_bool(thought) && yyjson_get_bool(thought)) {
                    buf_add(&st->reasoning, s, strlen(s));
                    tiny_emit_delta(st->emit, st->user, TINY_STREAM_REASONING_DELTA, s);
                } else {
                    buf_add(&st->text, s, strlen(s));
                    tiny_emit_delta(st->emit, st->user, TINY_STREAM_TEXT_DELTA, s);
                }
            }
        }
        yyjson_val *fr = yyjson_obj_get(cand, "finishReason");
        if (yyjson_is_str(fr)) {
            free(st->finish);
            st->finish = xstrdup(yyjson_get_str(fr));
        }
    }

    yyjson_val *usage = yyjson_obj_get(root, "usageMetadata");
    if (yyjson_is_obj(usage)) {
        yyjson_val *v;
        if (yyjson_is_num(v = yyjson_obj_get(usage, "promptTokenCount"))) st->tokens_in = (long)yyjson_get_num(v);
        if (yyjson_is_num(v = yyjson_obj_get(usage, "candidatesTokenCount"))) st->tokens_out = (long)yyjson_get_num(v);
        if (yyjson_is_num(v = yyjson_obj_get(usage, "thoughtsTokenCount"))) st->tokens_out += (long)yyjson_get_num(v);
        if (yyjson_is_num(v = yyjson_obj_get(usage, "totalTokenCount"))) st->tokens_total = (long)yyjson_get_num(v);
    }
    yyjson_doc_free(doc);
}

static void free_state(google_state *st) {
    free(st->text.data);
    free(st->reasoning.data);
    for (size_t i = 0; i < st->calls_len; i++) {
        free(st->calls[i].id);
        free(st->calls[i].name);
        free(st->calls[i].args.data);
    }
    free(st->calls);
    free(st->finish);
    free(st->error);
}

static void finalize(google_state *st, tiny_message *out) {
    tiny_add_text_block(out, TINY_BLOCK_THINKING, st->reasoning.data);
    tiny_add_text_block(out, TINY_BLOCK_TEXT, st->text.data);
    for (size_t i = 0; i < st->calls_len; i++) {
        gcall *c = &st->calls[i];
        tiny_add_tool_block(out, c->id, c->name, c->args.len ? c->args.data : NULL, st->emit, st->user);
    }
    out->tokens_in = st->tokens_in;
    out->tokens_out = st->tokens_out;
    out->tokens_total = st->tokens_total ? st->tokens_total : st->tokens_in + st->tokens_out;
    if (st->finish && strcmp(st->finish, "MAX_TOKENS") == 0) out->stop_reason = xstrdup("length");
    else if (st->calls_len > 0) out->stop_reason = xstrdup("toolUse");
    else out->stop_reason = xstrdup("stop");
}

// ---- transport ------------------------------------------------------------

static char *endpoint_url(const char *base, const char *model) {
    if (!base || !base[0] || !model || !model[0]) return NULL;
    const char *id = model;
    const char *prefix = "models/";
    size_t n = strlen(base);
    bool slash = base[n - 1] == '/';
    const char *sep = slash ? "" : "/";
    if (strncmp(id, prefix, 6) == 0) {
        size_t size = n + strlen(sep) + strlen(id) + 32;
        char *url = xmalloc(size);
        snprintf(url, size, "%s%s%s:streamGenerateContent?alt=sse", base, sep, id);
        return url;
    }
    size_t size = n + strlen(sep) + strlen(prefix) + strlen(id) + 40;
    char *url = xmalloc(size);
    snprintf(url, size, "%s%s%s%s:streamGenerateContent?alt=sse", base, sep, prefix, id);
    return url;
}

int tiny_google_generative_ai_stream(const tiny_model *model, const tiny_context *ctx,
                                     const tiny_stream_opts *opts,
                                     tiny_stream_fn emit, void *user,
                                     const volatile bool *cancel,
                                     tiny_message *out, char **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    out->role = TINY_ROLE_ASSISTANT;

    char *url = endpoint_url(model->base_url, model->model);
    if (!url) return tiny_fail(err, "provider: no base url");
    char *body = build_request(ctx, opts);
    if (!body) {
        free(url);
        return tiny_fail(err, "provider: cannot build request");
    }

    google_state st = {0};
    st.emit = emit;
    st.user = user;

    tiny_api_result r = tiny_api_post(url, body, "x-goog-api-key: ", model->api_key,
                                      NULL, cancel, on_data, &st, st.error, err);

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
