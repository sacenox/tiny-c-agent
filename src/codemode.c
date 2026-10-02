// The codemode runtime: an embedded QuickJS that runs a JavaScript program.

#include "tool.h"

#include <quickjs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host {
    char *name;
    cage_host_fn fn;
    void *user;
};

struct cage_runtime {
    JSRuntime *rt;
    JSContext *ctx;
    struct host *hosts;
    size_t hosts_len;
    cage_tool_output_fn out;
    void *out_user;
    const volatile bool *cancel;
};

// ---- small helpers --------------------------------------------------------

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
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

// Returns JSON.stringify for objects and the raw text for strings.
static char *json_or_text(JSContext *ctx, JSValueConst v) {
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        char *r = xstrdup(s ? s : "");
        JS_FreeCString(ctx, s);
        return r;
    }
    JSValue j = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(j) || JS_IsUndefined(j)) {
        JS_FreeValue(ctx, j);
        return xstrdup("");
    }
    const char *s = JS_ToCString(ctx, j);
    char *r = xstrdup(s ? s : "");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, j);
    return r;
}

// Combines an error's name, message, and stack trace.
static char *error_text(JSContext *ctx, JSValueConst v) {
    if (JS_IsError(v)) {
        JSValue name = JS_GetPropertyStr(ctx, v, "name");
        JSValue msg = JS_GetPropertyStr(ctx, v, "message");
        JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
        const char *ns = JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
        const char *ms = JS_IsString(msg) ? JS_ToCString(ctx, msg) : NULL;
        const char *ss = JS_IsString(stack) ? JS_ToCString(ctx, stack) : NULL;

        buf b = {0};
        if (ss && ns && strncmp(ss, ns, strlen(ns)) == 0) {
            buf_add(&b, ss, strlen(ss));  // the stack already carries the header
        } else {
            if (ns) buf_add(&b, ns, strlen(ns));
            if (ms) {
                if (b.len) buf_add(&b, ": ", 2);
                buf_add(&b, ms, strlen(ms));
            }
            if (ss) {
                if (b.len) buf_add(&b, "\n", 1);
                buf_add(&b, ss, strlen(ss));
            }
        }
        if (ns) JS_FreeCString(ctx, ns);
        if (ms) JS_FreeCString(ctx, ms);
        if (ss) JS_FreeCString(ctx, ss);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, msg);
        JS_FreeValue(ctx, stack);
        if (b.data) return b.data;
    }
    const char *s = JS_ToCString(ctx, v);
    if (s) {
        char *r = xstrdup(s);
        JS_FreeCString(ctx, s);
        return r;
    }
    return xstrdup("codemode: error");
}

// ---- interpreter hooks ----------------------------------------------------

static int on_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt;
    cage_runtime *r = opaque;
    return (r->cancel && *r->cancel) ? 1 : 0;
}

static JSValue js_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    cage_runtime *r = JS_GetContextOpaque(ctx);
    buf b = {0};
    for (int i = 0; i < argc; i++) {
        if (i > 0) buf_add(&b, " ", 1);
        char *s = json_or_text(ctx, argv[i]);
        buf_add(&b, s, strlen(s));
        free(s);
    }
    buf_add(&b, "\n", 1);
    if (r->out) r->out(b.data, r->out_user);
    free(b.data);
    return JS_UNDEFINED;
}

static JSValue js_host(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv, int magic, JSValueConst *func_data) {
    (void)this_val;
    (void)func_data;
    cage_runtime *r = JS_GetContextOpaque(ctx);
    size_t index = (size_t)magic;
    if (index >= r->hosts_len) return JS_UNDEFINED;
    struct host *h = &r->hosts[index];

    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_DupValue(ctx, argv[i]));
    JSValue jargs = JS_JSONStringify(ctx, arr, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, arr);
    if (JS_IsException(jargs)) {
        JS_FreeValue(ctx, jargs);
        return JS_EXCEPTION;
    }
    const char *cargs = JS_ToCString(ctx, jargs);
    JS_FreeValue(ctx, jargs);

    char *result = h->fn(cargs ? cargs : "[]", h->user);
    JS_FreeCString(ctx, cargs);
    if (!result) return JS_NULL;

    JSValue parsed = JS_ParseJSON(ctx, result, strlen(result), "<host>");
    free(result);
    return parsed;
}

cage_runtime *cage_runtime_new(void) {
    cage_runtime *r = xcalloc(1, sizeof *r);
    r->rt = JS_NewRuntime();
    r->ctx = JS_NewContext(r->rt);
    JS_SetContextOpaque(r->ctx, r);
    JS_SetInterruptHandler(r->rt, on_interrupt, r);

    JSValue global = JS_GetGlobalObject(r->ctx);
    JSValue console = JS_NewObject(r->ctx);
    JS_SetPropertyStr(r->ctx, console, "log", JS_NewCFunction(r->ctx, js_log, "log", 1));
    JS_SetPropertyStr(r->ctx, console, "error", JS_NewCFunction(r->ctx, js_log, "error", 1));
    JS_SetPropertyStr(r->ctx, global, "console", console);
    JS_FreeValue(r->ctx, global);
    return r;
}

void cage_runtime_register(cage_runtime *r, const char *name, cage_host_fn fn, void *user) {
    size_t index = r->hosts_len;
    r->hosts = realloc(r->hosts, (index + 1) * sizeof *r->hosts);
    if (!r->hosts) abort();
    r->hosts[index] = (struct host){.name = xstrdup(name), .fn = fn, .user = user};
    r->hosts_len = index + 1;

    JSValue f = JS_NewCFunctionData(r->ctx, js_host, 1, (int)index, 0, NULL);
    JSValue global = JS_GetGlobalObject(r->ctx);
    JS_SetPropertyStr(r->ctx, global, name, f);
    JS_FreeValue(r->ctx, global);
}

void cage_runtime_free(cage_runtime *r) {
    if (!r) return;
    for (size_t i = 0; i < r->hosts_len; i++) free(r->hosts[i].name);
    free(r->hosts);
    JS_FreeContext(r->ctx);
    JS_FreeRuntime(r->rt);
    free(r);
}

cage_tool_result cage_runtime_run(cage_runtime *r, const char *script,
                                  cage_tool_output_fn out, void *out_user,
                                  const volatile bool *cancel) {
    cage_tool_result res = {0};
    r->out = out;
    r->out_user = out_user;
    r->cancel = cancel;

    const char *pre = "(async () => {\n";
    const char *post = "\n})()";
    size_t n = strlen(pre) + strlen(script) + strlen(post);
    char *src = xmalloc(n + 1);
    memcpy(src, pre, strlen(pre));
    memcpy(src + strlen(pre), script, strlen(script));
    memcpy(src + strlen(pre) + strlen(script), post, strlen(post) + 1);

    JSValue fn = JS_Eval(r->ctx, src, n, "<codemode>", JS_EVAL_TYPE_GLOBAL);
    free(src);

    if (cancel && *cancel) {
        if (JS_IsException(fn)) JS_FreeValue(r->ctx, JS_GetException(r->ctx));
        res.text = xstrdup("codemode: aborted");
        res.is_error = true;
        return res;
    }
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(r->ctx);
        res.text = error_text(r->ctx, e);
        res.is_error = true;
        JS_FreeValue(r->ctx, e);
        return res;
    }

    JSContext *jc;
    while (JS_ExecutePendingJob(r->rt, &jc) > 0) {}

    if (cancel && *cancel) {
        JS_FreeValue(r->ctx, fn);
        res.text = xstrdup("codemode: aborted");
        res.is_error = true;
        return res;
    }

    switch (JS_PromiseState(r->ctx, fn)) {
    case JS_PROMISE_FULFILLED: {
        JSValue v = JS_PromiseResult(r->ctx, fn);
        res.text = json_or_text(r->ctx, v);
        JS_FreeValue(r->ctx, v);
        break;
    }
    case JS_PROMISE_REJECTED: {
        JSValue v = JS_PromiseResult(r->ctx, fn);
        res.text = error_text(r->ctx, v);
        res.is_error = true;
        JS_FreeValue(r->ctx, v);
        break;
    }
    default:
        res.text = xstrdup("codemode: script did not settle");
        res.is_error = true;
        break;
    }
    JS_FreeValue(r->ctx, fn);
    return res;
}

cage_tool cage_codemode_tool(void) {
    cage_tool t = {
        .name = "codemode",
        .description =
            "Run a JavaScript program. The program may use await. Call the "
            "provided host functions directly as globals, write progress with "
            "console.log, and return the value to report back as the result.",
        .parameters =
            "{\"type\":\"object\","
            "\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"JavaScript to run\"}},"
            "\"required\":[\"code\"],\"additionalProperties\":false}",
    };
    return t;
}
