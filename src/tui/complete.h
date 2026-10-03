// tiny TUI completion: path completion and the shared prefix helper.

#ifndef TINY_TUI_COMPLETE_H
#define TINY_TUI_COMPLETE_H

// Case-sensitive longest common prefix. Both inputs must be non-NULL.
char *tiny_common_prefix(const char *a, const char *b);

// Completes `word` as a file path. Returns malloc'd text, or NULL when there is
// nothing more to add.
char *tiny_complete_path(const char *word, const char *cwd);

#endif
