// tiny messages: the provider-neutral transcript format.
//
// Provider adapters translate these to and from each wire protocol. The agent
// loop and the session store share this shape.

#ifndef TINY_MESSAGE_H
#define TINY_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

typedef enum {
    TINY_ROLE_USER,
    TINY_ROLE_ASSISTANT,
    TINY_ROLE_TOOL,   // a tool result
} tiny_role;

typedef enum {
    TINY_BLOCK_TEXT,
    TINY_BLOCK_THINKING,
    TINY_BLOCK_TOOL_CALL,
    TINY_BLOCK_IMAGE,
} tiny_block_type;

typedef struct {
    tiny_block_type type;
    char *text;          // TEXT, THINKING
    char *tool_call_id;  // TOOL_CALL
    char *tool_name;     // TOOL_CALL
    char *arguments;     // TOOL_CALL, a JSON object string
    char *media_type;    // IMAGE, e.g. "image/png"
    char *data;          // IMAGE, base64
} tiny_block;

typedef struct {
    tiny_role role;
    tiny_block *blocks;
    size_t blocks_len;
    char *tool_call_id;   // TOOL: the call this result answers
    bool is_error;        // TOOL
    char *stop_reason;    // ASSISTANT: stop, length, toolUse, error, aborted
    char *error_message;  // ASSISTANT, when stop_reason is error
    long tokens_in;       // ASSISTANT usage
    long tokens_out;
    long tokens_total;
} tiny_message;

tiny_message tiny_message_user(const char *text);
tiny_message tiny_message_tool(const char *tool_call_id, const char *text, bool is_error);

// Appends a zeroed block and returns it for the caller to fill.
tiny_block *tiny_message_add_block(tiny_message *m, tiny_block_type type);

// True when the message carries at least one image block.
bool tiny_message_has_image(const tiny_message *m);

// Concatenates the message's text blocks (TEXT only) into one malloc'd string.
char *tiny_message_text(const tiny_message *m);

void tiny_message_free(tiny_message *m);

// Serializes the message to its provider-neutral JSON object.
yyjson_mut_val *tiny_message_to_val(yyjson_mut_doc *doc, const tiny_message *m);
char *tiny_message_to_json(const tiny_message *m);

#endif
