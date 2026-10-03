#include "term.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

// ---- UTF-8 -----------------------------------------------------------------

int cage_utf8_decode(const char *s, size_t len, uint32_t *cp) {
    if (len == 0) {
        *cp = 0;
        return 0;
    }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int n;
    uint32_t v;
    if ((c & 0xe0) == 0xc0) {
        n = 2;
        v = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
        n = 3;
        v = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
        n = 4;
        v = c & 0x07;
    } else {
        *cp = c;
        return 1;
    }
    if ((size_t)n > len) {
        *cp = c;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xc0) != 0x80) {
            *cp = c;
            return 1;
        }
        v = (v << 6) | (cc & 0x3f);
    }
    *cp = v;
    return n;
}

int cage_utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static bool is_combining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f);
}

int cage_char_width(uint32_t cp) {
    if (cp < 32 || (cp >= 0x7f && cp < 0xa0)) return 0;
    if (cp == 0x200b || cp == 0x200c || cp == 0x200d || cp == 0xfeff) return 0;
    if (cp >= 0xfe00 && cp <= 0xfe0f) return 0;
    if (cp >= 0xe0100 && cp <= 0xe01ef) return 0;
    if (is_combining(cp)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) ||
        (cp >= 0x3041 && cp <= 0x33ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
        (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
        (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd)) {
        return 2;
    }
    return 1;
}

int cage_display_width(const char *text) {
    int width = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int n = cage_utf8_decode(text + i, len - i, &cp);
        if (n <= 0) break;
        width += cage_char_width(cp);
        i += (size_t)n;
    }
    return width;
}

// ---- sanitize and tab expansion -------------------------------------------

// SGR: `ESC [ [0-9;]* m`, zero cells wide.
static bool is_sgr(const char *s, size_t len, size_t *seq_len) {
    if (len < 3 || s[0] != '\x1b' || s[1] != '[') return false;
    size_t i = 2;
    while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) i++;
    if (i < len && s[i] == 'm') {
        *seq_len = i + 1;
        return true;
    }
    return false;
}

char *cage_sanitize(const char *text) {
    tui_buf out;
    buf_init(&out);
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0x1b) {
            size_t sgr;
            if (is_sgr(text + i, len - i, &sgr)) {
                buf_add(&out, text + i, sgr);
                i += sgr;
                continue;
            }
            // A non-SGR escape: drop the whole sequence. CSI runs to its final
            // byte; every other escape consumes one following byte.
            if (i + 1 < len && text[i + 1] == '[') {
                size_t j = i + 2;
                while (j < len && !(text[j] >= 0x40 && text[j] <= 0x7e)) j++;
                i = j < len ? j + 1 : len;
            } else {
                i += (i + 1 < len) ? 2 : 1;
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) {
            i++;
            continue;
        }
        uint32_t cp;
        int n = cage_utf8_decode(text + i, len - i, &cp);
        if (n <= 0) break;
        buf_add(&out, text + i, (size_t)n);
        i += (size_t)n;
    }
    if (!out.data) return strdup("");
    return buf_steal(&out);
}

char *cage_expand_tabs(const char *text, int size) {
    if (!strchr(text, '\t')) return strdup(text);
    tui_buf out;
    buf_init(&out);
    size_t len = strlen(text);
    int column = 0;
    for (size_t i = 0; i < len;) {
        size_t sgr;
        if (is_sgr(text + i, len - i, &sgr)) {
            buf_add(&out, text + i, sgr);
            i += sgr;
            continue;
        }
        uint32_t cp;
        int n = cage_utf8_decode(text + i, len - i, &cp);
        if (n <= 0) break;
        if (cp == '\t') {
            int spaces = size - (column % size);
            for (int k = 0; k < spaces; k++) buf_addc(&out, ' ');
            column += spaces;
        } else {
            buf_add(&out, text + i, (size_t)n);
            column += cage_char_width(cp);
        }
        i += (size_t)n;
    }
    if (!out.data) return strdup("");
    return buf_steal(&out);
}

char **cage_wrap_line(const char *text, int width, size_t *count) {
    size_t cap = 8, n = 0;
    char **rows = malloc(cap * sizeof *rows);
    tui_buf cur;
    buf_init(&cur);
    int used = 0;
    if (width <= 0 || text[0] == '\0') {
        if (cap == 0) cap = 1;
        rows[0] = strdup("");
        *count = 1;
        buf_free(&cur);
        return rows;
    }
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        if (text[i] == '\x1b') {
            size_t sgr;
            if (is_sgr(text + i, len - i, &sgr)) {
                buf_add(&cur, text + i, sgr);
                i += sgr;
                continue;
            }
        }
        uint32_t cp;
        int nb = cage_utf8_decode(text + i, len - i, &cp);
        if (nb <= 0) break;
        int w = cage_char_width(cp);
        if (used + w > width && cur.len > 0) {
            if (n == cap) {
                cap *= 2;
                rows = realloc(rows, cap * sizeof *rows);
            }
            rows[n++] = buf_steal(&cur);
            buf_init(&cur);
            used = 0;
        }
        buf_add(&cur, text + i, (size_t)nb);
        used += w;
        i += (size_t)nb;
    }
    if (n == cap) {
        cap += 1;
        rows = realloc(rows, cap * sizeof *rows);
    }
    rows[n++] = buf_steal(&cur);
    *count = n;
    return rows;
}

// ---- key decoding ----------------------------------------------------------

#define PASTE_START "\x1b[200~"
#define PASTE_END "\x1b[201~"

#define SHIFT 1
#define ALT 2
#define CTRL 4
#define MODS (SHIFT | ALT | CTRL)

enum {
    K_ENTER = 13,
    K_ESCAPE = 27,
    K_TAB = 9,
    K_BACKSPACE = 127,
    K_DELETE = 57349,
    K_LEFT = 57350,
    K_RIGHT = 57351,
    K_UP = 57352,
    K_DOWN = 57353,
    K_HOME = 57356,
    K_END = 57357,
};

// Returns the key type for a code/modifier pair, or -1 when unmapped.
static int key_for(long code, int mods) {
    if (code == K_ENTER) return (mods == ALT || mods == SHIFT) ? CAGE_KEY_NEWLINE : (mods == 0 ? CAGE_KEY_SUBMIT : -1);
    if (code == K_ESCAPE) return mods == 0 ? CAGE_KEY_ESCAPE : -1;
    if (code == K_TAB) return mods == 0 ? CAGE_KEY_TAB : -1;
    if (code == K_BACKSPACE) return mods == CTRL ? CAGE_KEY_WORD_BACK : (mods == 0 ? CAGE_KEY_BACKSPACE : -1);
    if (code == K_DELETE) return mods == 0 ? CAGE_KEY_DELETE : -1;
    if (code == K_LEFT) return mods == CTRL ? CAGE_KEY_WORD_LEFT : (mods == 0 ? CAGE_KEY_LEFT : -1);
    if (code == K_RIGHT) return mods == CTRL ? CAGE_KEY_WORD_RIGHT : (mods == 0 ? CAGE_KEY_RIGHT : -1);
    if (code == K_UP) return mods == 0 ? CAGE_KEY_UP : -1;
    if (code == K_DOWN) return mods == 0 ? CAGE_KEY_DOWN : -1;
    if (code == K_HOME) return mods == CTRL ? CAGE_KEY_DOC_START : (mods == 0 ? CAGE_KEY_HOME : -1);
    if (code == K_END) return mods == CTRL ? CAGE_KEY_DOC_END : (mods == 0 ? CAGE_KEY_END : -1);
    if (code == 97 && mods == CTRL) return CAGE_KEY_HOME;       // Ctrl+A
    if (code == 98 && mods == ALT) return CAGE_KEY_WORD_BACK;   // Alt+B
    if (code == 99 && mods == CTRL) return CAGE_KEY_INTERRUPT;  // Ctrl+C
    if (code == 100 && mods == CTRL) return CAGE_KEY_EOF;       // Ctrl+D
    if (code == 101 && mods == CTRL) return CAGE_KEY_END;       // Ctrl+E
    if (code == 106 && mods == CTRL) return CAGE_KEY_NEWLINE;   // Ctrl+J
    if (code == 119 && mods == CTRL) return CAGE_KEY_WORD_BACK; // Ctrl+W
    return -1;
}

struct cage_key_parser {
    char *buf;
    size_t len;
    size_t cap;
    bool pasting;
};

cage_key_parser *cage_key_parser_new(void) {
    cage_key_parser *p = calloc(1, sizeof *p);
    if (!p) abort();
    return p;
}

void cage_key_parser_free(cage_key_parser *p) {
    if (!p) return;
    free(p->buf);
    free(p);
}

static void pb_append(cage_key_parser *p, const char *bytes, size_t n) {
    if (p->len + n + 1 > p->cap) {
        size_t cap = p->cap ? p->cap : 64;
        while (cap < p->len + n + 1) cap *= 2;
        p->buf = realloc(p->buf, cap);
        if (!p->buf) abort();
        p->cap = cap;
    }
    memcpy(p->buf + p->len, bytes, n);
    p->len += n;
    p->buf[p->len] = '\0';
}

static void pb_drop(cage_key_parser *p, size_t n) {
    if (n >= p->len) {
        p->len = 0;
        if (p->buf) p->buf[0] = '\0';
        return;
    }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
    p->buf[p->len] = '\0';
}

static void emit_type(cage_key_fn emit, void *user, cage_key_type t) {
    cage_key k = {.type = t, .text = NULL};
    emit(&k, user);
}

static void emit_text(cage_key_fn emit, void *user, const char *text) {
    cage_key k = {.type = CAGE_KEY_TEXT, .text = (char *)text};
    emit(&k, user);
}

// Normalizes CRLF and CR to LF, returning a malloc'd copy.
static char *normalize_paste(const char *text, size_t n) {
    tui_buf out;
    buf_init(&out);
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r') {
            buf_addc(&out, '\n');
            if (i + 1 < n && text[i + 1] == '\n') i++;
        } else {
            buf_addc(&out, text[i]);
        }
    }
    if (!out.data) return strdup("");
    return buf_steal(&out);
}

static int mods_of(const char *field, size_t n) {
    if (n == 0) return 0;
    long raw = strtol(field, NULL, 10);
    return raw > 0 ? (int)((raw - 1) & MODS) : 0;
}

static void parse_escape(cage_key_parser *p, cage_key_fn emit, void *user, bool *progress);

// `params final`: the legacy `CSI 1;5D` form and SS3's bare `A`.
static int legacy_chord(char final, const char *params, size_t plen, long *code, int *mods) {
    // Split params on ';': first field selects the key for `~`, second is mods.
    char first[16] = {0};
    char second[16] = {0};
    size_t fi = 0, si = 0;
    size_t i = 0;
    for (; i < plen && params[i] != ';'; i++)
        if (fi < sizeof first - 1) first[fi++] = params[i];
    if (i < plen) {
        i++;
        for (; i < plen; i++)
            if (si < sizeof second - 1) second[si++] = params[i];
    }
    if (final == '~') {
        long n = strtol(first, NULL, 10);
        long c = -1;
        if (n == 1 || n == 7) c = K_HOME;
        else if (n == 3) c = K_DELETE;
        else if (n == 4 || n == 8) c = K_END;
        if (c < 0) return -1;
        *code = c;
    } else {
        switch (final) {
        case 'A': *code = K_UP; break;
        case 'B': *code = K_DOWN; break;
        case 'C': *code = K_RIGHT; break;
        case 'D': *code = K_LEFT; break;
        case 'H': *code = K_HOME; break;
        case 'F': *code = K_END; break;
        default: return -1;
        }
    }
    *mods = mods_of(second, strlen(second));
    return 0;
}

// `code[:shifted[:base]];mods[:event];text`
static int csi_u_chord(const char *params, size_t plen, long *code, int *mods) {
    char first[24] = {0};
    char second[24] = {0};
    size_t fi = 0, si = 0, i = 0;
    for (; i < plen; i++) {
        if (params[i] == ':' || params[i] == ';') break;
        if (fi < sizeof first - 1) first[fi++] = params[i];
    }
    while (i < plen && params[i] != ';') i++;
    if (i < plen) {
        i++;
        for (; i < plen; i++) {
            if (params[i] == ':' || params[i] == ';') break;
            if (si < sizeof second - 1) second[si++] = params[i];
        }
    }
    if (fi == 0) return -1;
    long c = strtol(first, NULL, 10);
    if (c >= 57344) return -1;  // functional keys never arrive as CSI-u
    *code = c;
    *mods = mods_of(second, strlen(second));
    return 0;
}

// OSC ends at BEL or ST; DCS/APC/PM/SOS end at ST. Skipped whole.
static bool skip_string(cage_key_parser *p, bool bel) {
    size_t st = SIZE_MAX, bell = SIZE_MAX;
    for (size_t i = 2; i + 1 < p->len; i++)
        if (p->buf[i] == '\x1b' && p->buf[i + 1] == '\\') {
            st = i;
            break;
        }
    if (bel) {
        for (size_t i = 2; i < p->len; i++)
            if (p->buf[i] == '\x07') {
                bell = i;
                break;
            }
    }
    size_t end;
    if (bell != SIZE_MAX && (st == SIZE_MAX || bell < st)) end = bell + 1;
    else if (st != SIZE_MAX) end = st + 2;
    else return false;
    pb_drop(p, end);
    return true;
}

static bool parse_csi(cage_key_parser *p, cage_key_fn emit, void *user) {
    size_t i = 2;
    while (i < p->len) {
        unsigned char c = (unsigned char)p->buf[i];
        if (c >= 0x40 && c <= 0x7e) break;
        i++;
    }
    if (i >= p->len) return false;
    char final = p->buf[i];
    char params[256];
    size_t plen = i - 2;
    if (plen >= sizeof params) plen = sizeof params - 1;
    memcpy(params, p->buf + 2, plen);
    params[plen] = '\0';
    pb_drop(p, i + 1);
    long code = -1;
    int mods = 0;
    int rc = final == 'u' ? csi_u_chord(params, plen, &code, &mods)
                          : legacy_chord(final, params, plen, &code, &mods);
    if (rc == 0) {
        int type = key_for(code, mods);
        if (type >= 0) emit_type(emit, user, (cage_key_type)type);
    }
    return true;
}

static void parse_escape(cage_key_parser *p, cage_key_fn emit, void *user, bool *progress) {
    *progress = false;
    if (p->len < 2) return;
    char next = p->buf[1];
    if (next == '[' || next == 'O') {
        *progress = parse_csi(p, emit, user);
        return;
    }
    if (next == ']') {
        *progress = skip_string(p, true);
        return;
    }
    if (next == 'P' || next == 'X' || next == '^' || next == '_') {
        *progress = skip_string(p, false);
        return;
    }
    pb_drop(p, 2);
    uint32_t cp;
    cage_utf8_decode(&next, 1, &cp);
    int type = key_for(cp, ALT);
    if (type >= 0) emit_type(emit, user, (cage_key_type)type);
    *progress = true;
}

static void map_point(uint32_t cp, cage_key_fn emit, void *user) {
    if (cp == 0x7f) {
        int t = key_for(K_BACKSPACE, 0);
        if (t >= 0) emit_type(emit, user, (cage_key_type)t);
        return;
    }
    if (cp < 0x20) {
        if (cp == 0x08) {
            int t = key_for(K_BACKSPACE, 0);
            if (t >= 0) emit_type(emit, user, (cage_key_type)t);
        } else if (cp == 0x09) {
            int t = key_for(K_TAB, 0);
            if (t >= 0) emit_type(emit, user, (cage_key_type)t);
        } else if (cp == 0x0d) {
            int t = key_for(K_ENTER, 0);
            if (t >= 0) emit_type(emit, user, (cage_key_type)t);
        } else {
            int t = key_for((long)cp + 0x60, CTRL);
            if (t >= 0) emit_type(emit, user, (cage_key_type)t);
        }
        return;
    }
    char out[4];
    int n = cage_utf8_encode(cp, out);
    char *text = malloc((size_t)n + 1);
    if (!text) abort();
    memcpy(text, out, (size_t)n);
    text[n] = '\0';
    emit_text(emit, user, text);
    free(text);
}

void cage_key_parser_feed(cage_key_parser *p, const char *bytes, size_t n,
                          cage_key_fn emit, void *user) {
    pb_append(p, bytes, n);
    for (;;) {
        if (p->pasting) {
            // Search for the paste end marker.
            size_t end = SIZE_MAX;
            size_t elen = strlen(PASTE_END);
            for (size_t i = 0; i + elen <= p->len; i++)
                if (memcmp(p->buf + i, PASTE_END, elen) == 0) {
                    end = i;
                    break;
                }
            if (end != SIZE_MAX) {
                if (end > 0) {
                    char *text = normalize_paste(p->buf, end);
                    emit_text(emit, user, text);
                    free(text);
                }
                pb_drop(p, end + elen);
                p->pasting = false;
                continue;
            }
            if (p->len > elen) {
                size_t keep = elen - 1;
                size_t take = p->len - keep;
                char *text = normalize_paste(p->buf, take);
                if (text[0]) emit_text(emit, user, text);
                free(text);
                pb_drop(p, take);
            }
            break;
        }

        if (p->len == 0) break;
        size_t slen = strlen(PASTE_START);
        if (p->len >= slen && memcmp(p->buf, PASTE_START, slen) == 0) {
            pb_drop(p, slen);
            p->pasting = true;
            continue;
        }
        if (p->buf[0] == '\x1b') {
            bool progress;
            parse_escape(p, emit, user, &progress);
            if (!progress) break;
            continue;
        }
        uint32_t cp;
        int nb = cage_utf8_decode(p->buf, p->len, &cp);
        if (nb <= 0) break;
        pb_drop(p, (size_t)nb);
        map_point(cp, emit, user);
    }
}

int cage_key_parser_pending(cage_key_parser *p) {
    if (p->pasting) return 0;
    if (p->len == 1 && p->buf[0] == '\x1b') return 1;
    return p->len > 1 ? 2 : 0;
}

void cage_key_parser_flush_escape(cage_key_parser *p, cage_key_fn emit, void *user) {
    if (!(p->len == 1 && p->buf[0] == '\x1b')) return;
    pb_drop(p, 1);
    emit_type(emit, user, CAGE_KEY_ESCAPE);
}

void cage_key_parser_flush_sequence(cage_key_parser *p) {
    pb_drop(p, p->len);
}
