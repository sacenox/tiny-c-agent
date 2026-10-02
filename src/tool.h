// cage tools: the capabilities the model may call.
//
// Three plain functions: `read` returns a file's text (or an image), `edit`
// replaces text in a file, and `bash` runs a shell command. Each result carries
// text, an error flag, optional images, and display-only file diffs.

#ifndef CAGE_TOOL_H
#define CAGE_TOOL_H

#include <stdbool.h>
#include <stddef.h>

#include "provider.h"

// One image a tool attached to its result.
typedef struct {
    char *media_type;  // e.g. "image/png"
    char *data;        // base64 payload
} cage_image;

// One changed file under the working directory, display-only: it never reaches
// the model. `patch` is a unified diff; it is NULL when the content cannot be
// tracked, in which case `note` explains why.
typedef struct {
    char *path;   // relative to the working directory
    char *patch;  // unified diff body, or NULL
    char *note;   // reason when no patch exists, or NULL
} cage_file_diff;

typedef struct {
    char *text;          // the tool result text; always set
    bool is_error;
    cage_image *images;  // owned by the caller
    size_t images_len;
    cage_file_diff *diffs;  // owned by the caller; display-only
    size_t diffs_len;
} cage_tool_result;

// A tool's streamed output (bash's stdout/stderr), forwarded to the projection.
typedef void (*cage_tool_output_fn)(const char *chunk, void *user);

// The tools the model may call. `*len` receives the count. `supports_images`
// gates whether read's description mentions image input. The array is static
// and must not be freed.
const cage_tool *cage_tools(bool supports_images, size_t *len);

// Executes `name` with `args`, a JSON object string. An unknown tool name yields
// an error result. `out` forwards streamed output and may be NULL. `cancel` is
// polled by bash and may be NULL.
cage_tool_result cage_tool_run(const char *name, const char *args, bool supports_images,
                               cage_tool_output_fn out, void *user, const volatile bool *cancel);

#endif
