#include "headless.h"

#include <stdlib.h>
#include <string.h>

static const char *phase_name(tiny_phase p) {
    switch (p) {
    case TINY_PHASE_PREPARING: return "preparing";
    case TINY_PHASE_WAITING_MODEL: return "waiting for model";
    case TINY_PHASE_STREAMING: return "streaming";
    case TINY_PHASE_RUNNING_TOOL: return "running tool";
    case TINY_PHASE_PAUSING: return "pausing";
    case TINY_PHASE_IDLE: return "idle";
    }
    return "?";
}

void tiny_headless_init(tiny_headless *h, FILE *out, FILE *err) {
    memset(h, 0, sizeof *h);
    h->out = out;
    h->err = err;
}

static void text_add(tiny_headless *h, const char *s, size_t n) {
    if (h->text_len + n + 1 > h->text_cap) {
        size_t cap = h->text_cap ? h->text_cap : 256;
        while (cap < h->text_len + n + 1) cap *= 2;
        h->text = realloc(h->text, cap);
        if (!h->text) abort();
        h->text_cap = cap;
    }
    memcpy(h->text + h->text_len, s, n);
    h->text_len += n;
    h->text[h->text_len] = '\0';
}

void tiny_headless_event(const tiny_event *e, void *user) {
    tiny_headless *h = user;
    switch (e->type) {
    case TINY_EVENT_PHASE:
        // A new step starts a fresh assistant message.
        if (e->u.phase.phase == TINY_PHASE_PREPARING) h->text_len = 0;
        if (e->u.phase.phase == TINY_PHASE_RUNNING_TOOL) break;  // shown as [tool]
        fprintf(h->err, "[status] %s\n", phase_name(e->u.phase.phase));
        break;
    case TINY_EVENT_TEXT:
        text_add(h, e->u.text.delta, strlen(e->u.text.delta));
        break;
    case TINY_EVENT_TOOL_CALL:
        fprintf(h->err, "[tool] %s\n", e->u.tool_call.name);
        break;
    case TINY_EVENT_TOOL_OUTPUT:
        fputs(e->u.tool_output.chunk, h->err);
        fflush(h->err);
        break;
    case TINY_EVENT_TOOL_RESULT:
        if (e->u.tool_result.is_error)
            fprintf(h->err, "[tool error] %s\n", e->u.tool_result.name);
        break;
    case TINY_EVENT_NO_MODEL:
        h->no_model = true;
        fprintf(h->err, "[error] no model configured\n");
        break;
    case TINY_EVENT_ERROR:
        h->failed = true;
        fprintf(h->err, "[error] %s\n", e->u.error.message);
        break;
    case TINY_EVENT_CANCELLED:
        h->cancelled = true;
        fprintf(h->err, "[cancelled]\n");
        break;
    case TINY_EVENT_MESSAGE:
    case TINY_EVENT_REASONING:
    case TINY_EVENT_COMPLETE:
        break;
    }
}

int tiny_headless_finish(tiny_headless *h) {
    if (!h->failed && !h->cancelled && !h->no_model && h->text_len > 0) {
        fwrite(h->text, 1, h->text_len, h->out);
        if (h->text[h->text_len - 1] != '\n') fputc('\n', h->out);
        fflush(h->out);
    }
    return (h->failed || h->cancelled || h->no_model) ? 1 : 0;
}

void tiny_headless_free(tiny_headless *h) {
    free(h->text);
    h->text = NULL;
    h->text_len = h->text_cap = 0;
}
