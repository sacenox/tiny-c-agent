// cage tools: the single codemode tool and the JS runtime that executes it.
//
// Instead of one tool per capability, the model writes a JavaScript program.
// Capabilities are exposed to the script as global functions registered by the
// caller (this is the seam where MCP servers will plug in). Sandboxing is not a
// feature yet; the embedded runtime is what makes it possible later.

#ifndef CAGE_TOOL_H
#define CAGE_TOOL_H

#include <stdbool.h>
#include <stddef.h>

#include "provider.h"

// One image a host function attached to the current run.
typedef struct {
    char *media_type;  // e.g. "image/png"
    char *data;        // base64 payload
} cage_image;

typedef struct {
    char *text;          // the tool result text; always set
    bool is_error;
    cage_image *images;  // owned by the caller
    size_t images_len;
} cage_tool_result;

typedef struct cage_runtime cage_runtime;

typedef void (*cage_tool_output_fn)(const char *chunk, void *user);

// A host function's outcome: a JSON value string, or an error to throw. Both
// fields are owned by the runtime; `error` wins when set.
typedef struct {
    char *json;   // JSON value text, or NULL for null
    char *error;  // when set, a JS Error with this message is thrown
} cage_host_result;

// A host function callable from the script. `args_json` is a JSON array of the
// call arguments; the returned strings are owned by the runtime.
typedef cage_host_result (*cage_host_fn)(const char *args_json, void *user);

// The single tool the model calls.
cage_tool cage_codemode_tool(void);

// `supports_images` is the model's image-input capability, used to gate `read`.
cage_runtime *cage_runtime_new(bool supports_images);
void cage_runtime_free(cage_runtime *rt);

// Exposes `fn` as a global function named `name` in the runtime.
void cage_runtime_register(cage_runtime *rt, const char *name, cage_host_fn fn, void *user);

// Host-side accessors used by the capability implementations. `output` forwards
// a chunk to the run's output callback; `add_image` attaches an image to the
// run's result.
void cage_runtime_output(cage_runtime *rt, const char *chunk);
void cage_runtime_add_image(cage_runtime *rt, const char *media_type, const char *base64);
bool cage_runtime_supports_images(const cage_runtime *rt);
bool cage_runtime_cancelled(const cage_runtime *rt);

// Runs `script` as an async program. Output chunks go to `out`; the program's
// return value becomes the result text. `cancel` is polled by the interpreter
// and may be NULL.
cage_tool_result cage_runtime_run(cage_runtime *rt, const char *script,
                                  cage_tool_output_fn out, void *out_user,
                                  const volatile bool *cancel);

#endif
