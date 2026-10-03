// tiny TUI terminal: display width, sanitising, wrapping, and key decoding.
//
// The same text rules mini-coder uses: widths count cells, SGR sequences count
// zero, and everything else the terminal could act on is dropped before the
// text is measured, so a row's width is its visible width.

#ifndef TINY_TUI_TERM_H
#define TINY_TUI_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

// Decodes one UTF-8 code point; returns its byte length, or 1 with *cp = the
// raw byte on malformed input.
int tiny_utf8_decode(const char *s, size_t len, uint32_t *cp);
// Encodes a code point and returns its byte length.
int tiny_utf8_encode(uint32_t cp, char out[4]);
// Cells one code point occupies.
int tiny_char_width(uint32_t cp);

int tiny_display_width(const char *text);

// Drops control bytes and every escape sequence but SGR. Returns malloc'd.
char *tiny_sanitize(const char *text);
// Expands tabs to `size`-cell stops. Returns malloc'd.
char *tiny_expand_tabs(const char *text, int size);
// Splits one logical line into rows no wider than `width`. Returns an owned
// list of owned rows.
tiny_strv tiny_wrap_line(const char *text, int width);

typedef enum {
    TINY_KEY_TEXT,
    TINY_KEY_SUBMIT,
    TINY_KEY_NEWLINE,
    TINY_KEY_BACKSPACE,
    TINY_KEY_DELETE,
    TINY_KEY_WORD_BACK,
    TINY_KEY_LEFT,
    TINY_KEY_RIGHT,
    TINY_KEY_WORD_LEFT,
    TINY_KEY_WORD_RIGHT,
    TINY_KEY_UP,
    TINY_KEY_DOWN,
    TINY_KEY_HOME,
    TINY_KEY_END,
    TINY_KEY_DOC_START,
    TINY_KEY_DOC_END,
    TINY_KEY_TAB,
    TINY_KEY_ESCAPE,
    TINY_KEY_INTERRUPT,
    TINY_KEY_EOF,
} tiny_key_type;

typedef struct {
    tiny_key_type type;
    char *text;  // TEXT only, malloc'd
} tiny_key;

typedef void (*tiny_key_fn)(const tiny_key *key, void *user);

typedef struct tiny_key_parser tiny_key_parser;

tiny_key_parser *tiny_key_parser_new(void);
void tiny_key_parser_free(tiny_key_parser *p);

// Consumes bytes, invoking `emit` for each complete key. Partial sequences are
// held until more bytes arrive.
void tiny_key_parser_feed(tiny_key_parser *p, const char *bytes, size_t n,
                          tiny_key_fn emit, void *user);

// What the buffer may still complete: 0 nothing, 1 a lone ESC, 2 a sequence.
int tiny_key_parser_pending(tiny_key_parser *p);
// Emits the held lone ESC (after the 30ms timeout), if any.
void tiny_key_parser_flush_escape(tiny_key_parser *p, tiny_key_fn emit, void *user);
// Drops an incomplete sequence (after the 150ms timeout).
void tiny_key_parser_flush_sequence(tiny_key_parser *p);

#endif
