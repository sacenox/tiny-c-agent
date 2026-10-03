// tiny headless projection.
//
// Statuses and tool activity go to stderr; the final assistant text goes to
// stdout, so the output can be piped. Tool bodies (the call arguments) are
// never printed; tool output is.

#ifndef TINY_HEADLESS_H
#define TINY_HEADLESS_H

#include <stdbool.h>
#include <stdio.h>

#include "event.h"

typedef struct {
    FILE *out;
    FILE *err;
    char *text;        // accumulated text of the current assistant message
    size_t text_len;
    size_t text_cap;
    bool failed;
    bool cancelled;
    bool no_model;
} tiny_headless;

void tiny_headless_init(tiny_headless *h, FILE *out, FILE *err);

// Suitable as an agent's on_event callback, with `user` set to the headless.
void tiny_headless_event(const tiny_event *event, void *user);

// Prints the final text and returns the process exit code.
int tiny_headless_finish(tiny_headless *h);

void tiny_headless_free(tiny_headless *h);

#endif
