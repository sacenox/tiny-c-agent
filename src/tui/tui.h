// tiny TUI: the interactive projection of the agent event stream.
//
// The agent turn runs on a worker thread so the render stays responsive; the
// main thread owns the terminal, reads keys, and draws. Every render and every
// state change happens under one lock.

#ifndef TINY_TUI_TUI_H
#define TINY_TUI_TUI_H

#include "config.h"
#include "provider.h"
#include "resources.h"
#include "tool.h"

typedef struct {
    tiny_config *cfg;
    tiny_model model;
    const tiny_resources *resources;
    const char *cwd;
} tiny_tui_config;

// Runs the interactive loop until the user exits. Returns the process exit
// code. Owns no caller resources.
int tiny_tui_run(const tiny_tui_config *c);

#endif
