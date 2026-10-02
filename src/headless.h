// cage headless projection.
//
// Statuses and tool activity go to stderr; the final assistant text goes to
// stdout, so the output can be piped. Tool bodies (the call arguments) are
// never printed; tool output is.

#ifndef CAGE_HEADLESS_H
#define CAGE_HEADLESS_H

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
} cage_headless;

void cage_headless_init(cage_headless *h, FILE *out, FILE *err);

// Suitable as an agent's on_event callback, with `user` set to the headless.
void cage_headless_event(const cage_event *event, void *user);

// Prints the final text and returns the process exit code.
int cage_headless_finish(cage_headless *h);

void cage_headless_free(cage_headless *h);

#endif
