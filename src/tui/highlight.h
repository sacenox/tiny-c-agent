// tiny TUI syntax highlighting, over a committed block, once.
//
// Grammars are compiled in: some come from pkg-config, the rest are vendored
// and built from source. The highlights queries live in vendor/queries and are
// compiled into the binary (src/queries.generated.c). Only a capture name
// reaches the palette. When no grammar is available for a language, the text
// passes through unchanged.

#ifndef TINY_TUI_HIGHLIGHT_H
#define TINY_TUI_HIGHLIGHT_H

// Colors a committed block of Markdown. Returns malloc'd text.
char *tiny_highlight_markdown(const char *text);

// Colors a fenced code block's body by its fence info string; returns the input
// unchanged (a malloc'd copy) when the language is unknown.
char *tiny_highlight_code(const char *info, const char *text);

#endif
