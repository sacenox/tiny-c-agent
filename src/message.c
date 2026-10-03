#include "message.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

cage_message cage_message_user(const char *text) {
    cage_message m = {0};
    m.role = CAGE_ROLE_USER;
    cage_block *b = cage_message_add_block(&m, CAGE_BLOCK_TEXT);
    b->text = xstrdup(text ? text : "");
    return m;
}

cage_message cage_message_tool(const char *tool_call_id, const char *text, bool is_error) {
    cage_message m = {0};
    m.role = CAGE_ROLE_TOOL;
    m.is_error = is_error;
    m.tool_call_id = xstrdup(tool_call_id ? tool_call_id : "");
    cage_block *b = cage_message_add_block(&m, CAGE_BLOCK_TEXT);
    b->text = xstrdup(text ? text : "");
    return m;
}

cage_block *cage_message_add_block(cage_message *m, cage_block_type type) {
    m->blocks = realloc(m->blocks, (m->blocks_len + 1) * sizeof *m->blocks);
    if (!m->blocks) abort();
    cage_block *b = &m->blocks[m->blocks_len++];
    memset(b, 0, sizeof *b);
    b->type = type;
    return b;
}

bool cage_message_has_image(const cage_message *m) {
    for (size_t i = 0; i < m->blocks_len; i++)
        if (m->blocks[i].type == CAGE_BLOCK_IMAGE) return true;
    return false;
}

char *cage_message_text(const cage_message *m) {
    cage_buf b;
    buf_init(&b);
    for (size_t i = 0; i < m->blocks_len; i++)
        if (m->blocks[i].type == CAGE_BLOCK_TEXT && m->blocks[i].text)
            buf_adds(&b, m->blocks[i].text);
    return buf_steal(&b);
}

void cage_message_free(cage_message *m) {
    if (!m) return;
    for (size_t i = 0; i < m->blocks_len; i++) {
        free(m->blocks[i].text);
        free(m->blocks[i].tool_call_id);
        free(m->blocks[i].tool_name);
        free(m->blocks[i].arguments);
        free(m->blocks[i].media_type);
        free(m->blocks[i].data);
    }
    free(m->blocks);
    free(m->tool_call_id);
    free(m->stop_reason);
    free(m->error_message);
    memset(m, 0, sizeof *m);
}

yyjson_mut_val *cage_message_to_val(yyjson_mut_doc *doc, const cage_message *m) {
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    const char *role = m->role == CAGE_ROLE_USER ? "user"
                     : m->role == CAGE_ROLE_ASSISTANT ? "assistant"
                     : "tool";
    yyjson_mut_obj_add_strcpy(doc, o, "role", role);

    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, o, "content", content);
    for (size_t i = 0; i < m->blocks_len; i++) {
        const cage_block *b = &m->blocks[i];
        yyjson_mut_val *bo = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(content, bo);
        switch (b->type) {
        case CAGE_BLOCK_TEXT:
            yyjson_mut_obj_add_strcpy(doc, bo, "type", "text");
            yyjson_mut_obj_add_strcpy(doc, bo, "text", b->text ? b->text : "");
            break;
        case CAGE_BLOCK_THINKING:
            yyjson_mut_obj_add_strcpy(doc, bo, "type", "thinking");
            yyjson_mut_obj_add_strcpy(doc, bo, "thinking", b->text ? b->text : "");
            break;
        case CAGE_BLOCK_TOOL_CALL:
            yyjson_mut_obj_add_strcpy(doc, bo, "type", "toolCall");
            yyjson_mut_obj_add_strcpy(doc, bo, "id", b->tool_call_id ? b->tool_call_id : "");
            yyjson_mut_obj_add_strcpy(doc, bo, "name", b->tool_name ? b->tool_name : "");
            yyjson_mut_obj_add_strcpy(doc, bo, "arguments", b->arguments ? b->arguments : "{}");
            break;
        case CAGE_BLOCK_IMAGE:
            yyjson_mut_obj_add_strcpy(doc, bo, "type", "image");
            yyjson_mut_obj_add_strcpy(doc, bo, "data", b->data ? b->data : "");
            yyjson_mut_obj_add_strcpy(doc, bo, "mimeType", b->media_type ? b->media_type : "");
            break;
        }
    }

    if (m->role == CAGE_ROLE_TOOL) {
        yyjson_mut_obj_add_strcpy(doc, o, "toolCallId", m->tool_call_id ? m->tool_call_id : "");
        yyjson_mut_obj_add_bool(doc, o, "isError", m->is_error);
    } else if (m->role == CAGE_ROLE_ASSISTANT) {
        if (m->stop_reason) yyjson_mut_obj_add_strcpy(doc, o, "stopReason", m->stop_reason);
        if (m->error_message) yyjson_mut_obj_add_strcpy(doc, o, "errorMessage", m->error_message);
        yyjson_mut_val *usage = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, o, "usage", usage);
        yyjson_mut_obj_add_int(doc, usage, "input", m->tokens_in);
        yyjson_mut_obj_add_int(doc, usage, "output", m->tokens_out);
        yyjson_mut_obj_add_int(doc, usage, "total", m->tokens_total);
    }
    return o;
}

char *cage_message_to_json(const cage_message *m) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, cage_message_to_val(doc, m));
    size_t len = 0;
    char *s = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    return s;
}
