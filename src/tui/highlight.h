// tiny TUI syntax highlighting, over a committed block, once.
//
// Grammars come from tree-sitter; queries ship as data and are read from the
// usual nvim / tree-sitter query directories. Only a capture name reaches the
// palette. When no grammar or query is available, the text passes through
// unchanged.

#ifndef TINY_TUI_HIGHLIGHT_H
#define TINY_TUI_HIGHLIGHT_H

// Colors a committed block of Markdown. Returns malloc'd text.
char *tiny_highlight_markdown(const char *text);

// Colors a fenced code block's body by its fence info string; returns the input
// unchanged (a malloc'd copy) when the language is unknown.
char *tiny_highlight_code(const char *info, const char *text);

#endif
