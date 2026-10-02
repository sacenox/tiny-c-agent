// cage TUI line editor: multi-line, wrapped, with word motion and completion.

#ifndef CAGE_TUI_EDITOR_H
#define CAGE_TUI_EDITOR_H

#include <stdbool.h>
#include <stddef.h>

#include "term.h"

typedef struct cage_editor cage_editor;

cage_editor *cage_editor_new(void);
void cage_editor_free(cage_editor *e);

// The draft as one string, lines joined by newline. Owned by the editor.
const char *cage_editor_text(cage_editor *e);

// Renders each character as `*` without changing the underlying text.
void cage_editor_set_masked(cage_editor *e, bool masked);

void cage_editor_clear(cage_editor *e);
void cage_editor_set_text(cage_editor *e, const char *text);

typedef enum {
    CAGE_EDIT_NONE,
    CAGE_EDIT_CHANGED,
    CAGE_EDIT_SUBMIT,
} cage_edit_result;

cage_edit_result cage_editor_handle(cage_editor *e, const cage_key *key);

// Applies `step` to the word ending at the caret, replacing it; false when
// there is no word before the caret.
bool cage_editor_complete_word(cage_editor *e, char *(*step)(const char *word, void *user), void *user);

typedef struct {
    char **rows;
    size_t nrows;
    int cursor_row;
    int cursor_col;
} cage_editor_view;

// Clipped to `max_rows`, scrolled only as far as the caret requires.
cage_editor_view cage_editor_render(cage_editor *e, int width, int max_rows);
void cage_editor_view_free(cage_editor_view *v);

#endif
