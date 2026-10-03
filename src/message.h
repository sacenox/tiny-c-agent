// cage messages: the provider-neutral transcript format.
//
// Provider adapters translate these to and from each wire protocol. The agent
// loop and the session store share this shape.

#ifndef CAGE_MESSAGE_H
#define CAGE_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

typedef enum {
    CAGE_ROLE_USER,
    CAGE_ROLE_ASSISTANT,
    CAGE_ROLE_TOOL,   // a tool result
} cage_role;

typedef enum {
    CAGE_BLOCK_TEXT,
    CAGE_BLOCK_THINKING,
    CAGE_BLOCK_TOOL_CALL,
    CAGE_BLOCK_IMAGE,
} cage_block_type;

typedef struct {
    cage_block_type type;
    char *text;          // TEXT, THINKING
    char *tool_call_id;  // TOOL_CALL
    char *tool_name;     // TOOL_CALL
    char *arguments;     // TOOL_CALL, a JSON object string
    char *media_type;    // IMAGE, e.g. "image/png"
    char *data;          // IMAGE, base64
} cage_block;

typedef struct {
    cage_role role;
    cage_block *blocks;
    size_t blocks_len;
    char *tool_call_id;   // TOOL: the call this result answers
    bool is_error;        // TOOL
    char *stop_reason;    // ASSISTANT: stop, length, toolUse, error, aborted
    char *error_message;  // ASSISTANT, when stop_reason is error
    long tokens_in;       // ASSISTANT usage
    long tokens_out;
    long tokens_total;
} cage_message;

cage_message cage_message_user(const char *text);
cage_message cage_message_tool(const char *tool_call_id, const char *text, bool is_error);

// Appends a zeroed block and returns it for the caller to fill.
cage_block *cage_message_add_block(cage_message *m, cage_block_type type);

// True when the message carries at least one image block.
bool cage_message_has_image(const cage_message *m);

// Concatenates the message's text blocks (TEXT only) into one malloc'd string.
char *cage_message_text(const cage_message *m);

void cage_message_free(cage_message *m);

// Serializes the message to its provider-neutral JSON object.
yyjson_mut_val *cage_message_to_val(yyjson_mut_doc *doc, const cage_message *m);
char *cage_message_to_json(const cage_message *m);

#endif
