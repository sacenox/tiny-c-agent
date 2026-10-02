// cage TUI completion: path completion and the shared prefix helper.

#ifndef CAGE_TUI_COMPLETE_H
#define CAGE_TUI_COMPLETE_H

// Case-sensitive longest common prefix. Both inputs must be non-NULL.
char *cage_common_prefix(const char *a, const char *b);

// Completes `word` as a file path. Returns malloc'd text, or NULL when there is
// nothing more to add.
char *cage_complete_path(const char *word, const char *cwd);

#endif
