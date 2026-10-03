// tiny TUI context readout: how full the next request's window would be.

#ifndef TINY_TUI_USAGE_H
#define TINY_TUI_USAGE_H

// `812`, `12.3k`, `1.24M` — trailing zeros trimmed. Returns malloc'd.
char *tiny_format_tokens(long n);

// The context row: dim, yellow as the window nears, red once the next request
// could not carry a maximum-size reply. `window` <= 0 shows only the count.
// Returns malloc'd, styled text.
char *tiny_context_usage_line(long used, long window, long max_tokens);

#endif
