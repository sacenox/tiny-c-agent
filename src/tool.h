// cage tools: the single codemode tool and the JS runtime that executes it.
//
// Instead of one tool per capability, the model writes a JavaScript program.
// Capabilities are exposed to the script as global functions registered by the
// caller (this is the seam where MCP servers will plug in). Sandboxing is not a
// feature yet; the embedded runtime is what makes it possible later.

#ifndef CAGE_TOOL_H
#define CAGE_TOOL_H

#include <stdbool.h>

#include "provider.h"

typedef struct {
    char *text;      // the tool result text; always set
    bool is_error;
} cage_tool_result;

typedef struct cage_runtime cage_runtime;

typedef void (*cage_tool_output_fn)(const char *chunk, void *user);

// A host function callable from the script. `args_json` is a JSON array of the
// call arguments; the returned string is the JSON value for the call result
// (or NULL for null). The string is owned by the runtime.
typedef char *(*cage_host_fn)(const char *args_json, void *user);

// The single tool the model calls.
cage_tool cage_codemode_tool(void);

cage_runtime *cage_runtime_new(void);
void cage_runtime_free(cage_runtime *rt);

// Exposes `fn` as a global function named `name` in the runtime.
void cage_runtime_register(cage_runtime *rt, const char *name, cage_host_fn fn, void *user);

// Runs `script` as an async program. Output chunks go to `out`; the program's
// return value becomes the result text. `cancel` is polled by the interpreter
// and may be NULL.
cage_tool_result cage_runtime_run(cage_runtime *rt, const char *script,
                                  cage_tool_output_fn out, void *out_user,
                                  const volatile bool *cancel);

#endif
