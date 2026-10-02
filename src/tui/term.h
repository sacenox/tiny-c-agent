// cage TUI terminal: display width, sanitising, wrapping, and key decoding.
//
// The same text rules mini-coder uses: widths count cells, SGR sequences count
// zero, and everything else the terminal could act on is dropped before the
// text is measured, so a row's width is its visible width.

#ifndef CAGE_TUI_TERM_H
#define CAGE_TUI_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decodes one UTF-8 code point; returns its byte length, or 1 with *cp = the
// raw byte on malformed input.
int cage_utf8_decode(const char *s, size_t len, uint32_t *cp);
// Encodes a code point and returns its byte length.
int cage_utf8_encode(uint32_t cp, char out[4]);
// Cells one code point occupies.
int cage_char_width(uint32_t cp);

int cage_display_width(const char *text);

// Drops control bytes and every escape sequence but SGR. Returns malloc'd.
char *cage_sanitize(const char *text);
// Expands tabs to `size`-cell stops. Returns malloc'd.
char *cage_expand_tabs(const char *text, int size);
// Splits one logical line into rows no wider than `width`. Returns a malloc'd
// array of malloc'd rows; *count receives the row count.
char **cage_wrap_line(const char *text, int width, size_t *count);

typedef enum {
    CAGE_KEY_TEXT,
    CAGE_KEY_SUBMIT,
    CAGE_KEY_NEWLINE,
    CAGE_KEY_BACKSPACE,
    CAGE_KEY_DELETE,
    CAGE_KEY_WORD_BACK,
    CAGE_KEY_LEFT,
    CAGE_KEY_RIGHT,
    CAGE_KEY_WORD_LEFT,
    CAGE_KEY_WORD_RIGHT,
    CAGE_KEY_UP,
    CAGE_KEY_DOWN,
    CAGE_KEY_HOME,
    CAGE_KEY_END,
    CAGE_KEY_DOC_START,
    CAGE_KEY_DOC_END,
    CAGE_KEY_TAB,
    CAGE_KEY_ESCAPE,
    CAGE_KEY_INTERRUPT,
    CAGE_KEY_EOF,
} cage_key_type;

typedef struct {
    cage_key_type type;
    char *text;  // TEXT only, malloc'd
} cage_key;

typedef void (*cage_key_fn)(const cage_key *key, void *user);

typedef struct cage_key_parser cage_key_parser;

cage_key_parser *cage_key_parser_new(void);
void cage_key_parser_free(cage_key_parser *p);

// Consumes bytes, invoking `emit` for each complete key. Partial sequences are
// held until more bytes arrive.
void cage_key_parser_feed(cage_key_parser *p, const char *bytes, size_t n,
                          cage_key_fn emit, void *user);

// What the buffer may still complete: 0 nothing, 1 a lone ESC, 2 a sequence.
int cage_key_parser_pending(cage_key_parser *p);
// Emits the held lone ESC (after the 30ms timeout), if any.
void cage_key_parser_flush_escape(cage_key_parser *p, cage_key_fn emit, void *user);
// Drops an incomplete sequence (after the 150ms timeout).
void cage_key_parser_flush_sequence(cage_key_parser *p);

#endif
