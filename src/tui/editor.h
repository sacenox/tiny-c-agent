// tiny TUI line editor: multi-line, wrapped, with word motion and completion.

#ifndef TINY_TUI_EDITOR_H
#define TINY_TUI_EDITOR_H

#include <stdbool.h>
#include <stddef.h>

#include "term.h"

typedef struct tiny_editor tiny_editor;

tiny_editor *tiny_editor_new(void);
void tiny_editor_free(tiny_editor *e);

// The draft as one string, lines joined by newline. Owned by the editor.
const char *tiny_editor_text(tiny_editor *e);

void tiny_editor_clear(tiny_editor *e);
void tiny_editor_set_text(tiny_editor *e, const char *text);

typedef enum {
    TINY_EDIT_NONE,
    TINY_EDIT_CHANGED,
    TINY_EDIT_SUBMIT,
} tiny_edit_result;

tiny_edit_result tiny_editor_handle(tiny_editor *e, const tiny_key *key);

// Applies `step` to the word ending at the caret, replacing it; false when
// there is no word before the caret.
bool tiny_editor_complete_word(tiny_editor *e, char *(*step)(const char *word, void *user), void *user);

typedef struct {
    char **rows;
    size_t nrows;
    int cursor_row;
    int cursor_col;
} tiny_editor_view;

// Clipped to `max_rows`, scrolled only as far as the caret requires.
tiny_editor_view tiny_editor_render(tiny_editor *e, int width, int max_rows);

#endif
