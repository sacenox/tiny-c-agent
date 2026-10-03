#include "highlight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

#include <tree_sitter/tree-sitter-javascript.h>
#include <tree_sitter/tree-sitter-bash.h>
#include <tree_sitter/tree-sitter-markdown.h>
#include <tree_sitter/tree-sitter-markdown-inline.h>

#include "util.h"
#include "theme.h"
#include "util.h"

typedef struct {
    TSParser *parser;
    TSQuery *query;
} syntax;

// Reads a highlights query for `lang` from the usual locations, or NULL.
static char *read_query(const char *lang) {
    const char *env = getenv("CAGE_QUERIES_DIR");
    const char *home = getenv("HOME");
    char path[4096];
    if (env && env[0]) {
        snprintf(path, sizeof path, "%s/%s/highlights.scm", env, lang);
        char *q = cage_read_file(path);
        if (q) return q;
    }
    const char *tmpl[] = {
        "/usr/share/tree-sitter/queries/%s/highlights.scm",
        "/usr/share/nvim/runtime/queries/%s/highlights.scm",
        "%s/.local/share/nvim/lazy/nvim-treesitter/runtime/queries/%s/highlights.scm",
    };
    for (size_t i = 0; i < sizeof tmpl / sizeof tmpl[0]; i++) {
        if (i == 2) {
            if (!home || !home[0]) break;
            snprintf(path, sizeof path, tmpl[i], home, lang);
        } else {
            snprintf(path, sizeof path, tmpl[i], lang);
        }
        char *q = cage_read_file(path);
        if (q) return q;
    }
    return NULL;
}

static syntax *syntax_open(const TSLanguage *lang, const char *const *langs, size_t nlangs) {
    if (!lang) return NULL;
    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, lang)) {
        ts_parser_delete(parser);
        return NULL;
    }
    tui_buf src;
    buf_init(&src);
    bool any = false;
    for (size_t i = 0; i < nlangs; i++) {
        char *q = read_query(langs[i]);
        if (!q) continue;
        if (any) buf_addc(&src, '\n');
        buf_adds(&src, q);
        any = true;
        free(q);
    }
    if (!any) {
        buf_free(&src);
        ts_parser_delete(parser);
        return NULL;
    }
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    TSQuery *query = ts_query_new(lang, src.data, (uint32_t)src.len, &error_offset, &error_type);
    buf_free(&src);
    if (!query) {
        ts_parser_delete(parser);
        return NULL;
    }
    syntax *s = malloc(sizeof *s);
    if (!s) abort();
    s->parser = parser;
    s->query = query;
    return s;
}

// Lazily built grammars; `seen` records that we tried, so a missing grammar is
// not re-probed on every block.
static syntax *JS;
static syntax *BASH;
static syntax *MD;
static syntax *MD_INLINE;
static bool js_seen, bash_seen, md_seen, md_inline_seen;

static syntax *grammar_js(void) {
    static const char *langs[] = {"javascript"};
    if (!js_seen) {
        js_seen = true;
        JS = syntax_open(tree_sitter_javascript(), langs, 1);
    }
    return JS;
}

static syntax *grammar_bash(void) {
    static const char *langs[] = {"bash"};
    if (!bash_seen) {
        bash_seen = true;
        BASH = syntax_open(tree_sitter_bash(), langs, 1);
    }
    return BASH;
}

static syntax *grammar_md(void) {
    static const char *langs[] = {"markdown"};
    if (!md_seen) {
        md_seen = true;
        MD = syntax_open(tree_sitter_markdown(), langs, 1);
    }
    return MD;
}

static syntax *grammar_md_inline(void) {
    static const char *langs[] = {"markdown_inline"};
    if (!md_inline_seen) {
        md_inline_seen = true;
        MD_INLINE = syntax_open(tree_sitter_markdown_inline(), langs, 1);
    }
    return MD_INLINE;
}

// ---- spans -----------------------------------------------------------------

typedef struct {
    size_t start;
    size_t end;
    const cage_style *style;
} span;

typedef struct {
    span *v;
    size_t n;
    size_t cap;
} spanlist;

static void span_push(spanlist *l, size_t start, size_t end, const cage_style *style) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
        if (!l->v) abort();
    }
    l->v[l->n++] = (span){start, end, style};
}

static void spans_of(syntax *s, TSNode node, size_t offset, spanlist *out) {
    TSQueryCursor *cur = ts_query_cursor_new();
    ts_query_cursor_exec(cur, s->query, node);
    TSQueryMatch m;
    while (ts_query_cursor_next_match(cur, &m)) {
        for (uint16_t i = 0; i < m.capture_count; i++) {
            TSQueryCapture cap = m.captures[i];
            uint32_t len = 0;
            const char *name = ts_query_capture_name_for_id(s->query, cap.index, &len);
            char namebuf[128];
            if (len >= sizeof namebuf) len = sizeof namebuf - 1;
            memcpy(namebuf, name, len);
            namebuf[len] = '\0';
            const cage_style *style = cage_capture_style(namebuf);
            if (!style) continue;
            span_push(out, offset + ts_node_start_byte(cap.node), offset + ts_node_end_byte(cap.node), style);
        }
    }
    ts_query_cursor_delete(cur);
}

// Descendant nodes whose type is one of `types`, appended to `out`.
static void collect_types(TSNode n, const char *const *types, size_t ntypes,
                          TSNode **out, size_t *len, size_t *cap) {
    const char *t = ts_node_type(n);
    for (size_t i = 0; i < ntypes; i++) {
        if (strcmp(t, types[i]) == 0) {
            if (*len == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *out = realloc(*out, *cap * sizeof **out);
                if (!*out) abort();
            }
            (*out)[(*len)++] = n;
            break;
        }
    }
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++)
        collect_types(ts_node_child(n, i), types, ntypes, out, len, cap);
}

// Parses `text` and hands the caller the tree to free once every node from it
// is done being used.
static TSNode parse(syntax *s, const char *text, size_t len, TSTree **tree_out) {
    TSTree *tree = ts_parser_parse_string(s->parser, NULL, text, (uint32_t)len);
    *tree_out = tree;
    return ts_tree_root_node(tree);
}

static void sgr_of(tui_buf *b, const cage_style *style) {
    if (style) sgr_style(b, style);
    else sgr_plain(b);
}

// Writes text[at, end) with the innermost active span's colour, re-asserting it
// after each newline, and advances `at`.
static void paint_emit(tui_buf *out, const char *text, size_t text_len, size_t *at, size_t end,
                       const span *const *active, size_t nactive, bool *plain) {
    if (end <= *at) return;
    const cage_style *style = nactive ? active[nactive - 1]->style : NULL;
    if (!style) {
        if (!*plain) sgr_plain(out);
        *plain = true;
    } else {
        sgr_of(out, style);
        *plain = false;
    }
    for (size_t i = *at; i < end && i < text_len; i++) {
        buf_addc(out, text[i]);
        if (text[i] == '\n' && style) sgr_of(out, style);
    }
    *at = end;
}

// Wraps every span in its colour, innermost wins, text outside falls back to
// Normal. A span may cross a line break and re-asserts itself after each one.
static char *paint(const char *text, span *spans, size_t nspans, size_t text_len) {
    typedef struct {
        size_t at;
        bool open;
        size_t idx;
    } event;

    event *ev = malloc((nspans ? nspans : 1) * 2 * sizeof *ev);
    if (!ev) abort();
    size_t nev = 0;
    for (size_t i = 0; i < nspans; i++) {
        if (spans[i].end <= spans[i].start) continue;
        ev[nev++] = (event){spans[i].start, true, i};
        ev[nev++] = (event){spans[i].end, false, i};
    }
    // Insertion sort by position, closing before opening at the same offset.
    for (size_t i = 1; i < nev; i++) {
        event key = ev[i];
        size_t j = i;
        while (j > 0 &&
               (ev[j - 1].at > key.at || (ev[j - 1].at == key.at && ev[j - 1].open && !key.open))) {
            ev[j] = ev[j - 1];
            j--;
        }
        ev[j] = key;
    }

    const span *active[64];
    size_t nactive = 0;
    tui_buf out;
    buf_init(&out);
    bool plain = true;
    size_t at = 0;

    for (size_t e = 0; e < nev; e++) {
        paint_emit(&out, text, text_len, &at, ev[e].at, active, nactive, &plain);
        if (ev[e].open) {
            if (nactive < sizeof active / sizeof active[0]) active[nactive++] = &spans[ev[e].idx];
        } else {
            for (size_t i = 0; i < nactive; i++) {
                if (active[i] == &spans[ev[e].idx]) {
                    memmove(&active[i], &active[i + 1], (nactive - i - 1) * sizeof active[0]);
                    nactive--;
                    break;
                }
            }
        }
    }
    paint_emit(&out, text, text_len, &at, text_len, active, nactive, &plain);
    free(ev);
    if (!plain) sgr_plain(&out);
    if (!out.data) return strdup("");
    return buf_steal(&out);
}

// Replaces every span entirely inside [start, end) with one covering span.
static void recolor(spanlist *l, size_t start, size_t end, const cage_style *style) {
    size_t w = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i].start >= start && l->v[i].end <= end) continue;
        l->v[w++] = l->v[i];
    }
    l->n = w;
    span_push(l, start, end, style);
}

static int heading_level(TSNode heading) {
    uint32_t count = ts_node_named_child_count(heading);
    for (uint32_t i = 0; i < count; i++) {
        const char *t = ts_node_type(ts_node_named_child(heading, i));
        const char *p = NULL;
        if (strncmp(t, "atx_h", 5) == 0) p = t + 5;
        else if (strncmp(t, "setext_h", 8) == 0) p = t + 8;
        if (p && *p) return atoi(p);
    }
    return 1;
}

char *cage_highlight_markdown(const char *text) {
    syntax *md = grammar_md();
    if (!md) return strdup(text);
    size_t len = strlen(text);
    TSTree *md_tree = NULL;
    TSNode root = parse(md, text, len, &md_tree);
    spanlist spans = {0};
    spans_of(md, root, 0, &spans);

    syntax *inl = grammar_md_inline();
    TSTree **trees = NULL;
    size_t ntrees = 0, trees_cap = 0;
    if (inl) {
        static const char *inline_types[] = {"inline"};
        TSNode *inlines = NULL;
        size_t ninline = 0, cap = 0;
        collect_types(root, inline_types, 1, &inlines, &ninline, &cap);
        for (size_t i = 0; i < ninline; i++) {
            size_t off = ts_node_start_byte(inlines[i]);
            size_t ilen = ts_node_end_byte(inlines[i]) - off;
            char *sub = malloc(ilen + 1);
            if (!sub) abort();
            memcpy(sub, text + off, ilen);
            sub[ilen] = '\0';
            TSTree *itree = NULL;
            TSNode iroot = parse(inl, sub, ilen, &itree);
            if (ntrees == trees_cap) {
                trees_cap = trees_cap ? trees_cap * 2 : 8;
                trees = realloc(trees, trees_cap * sizeof *trees);
                if (!trees) abort();
            }
            trees[ntrees++] = itree;
            spans_of(inl, iroot, off, &spans);
            static const char *code_types[] = {"code_span"};
            TSNode *codes = NULL;
            size_t ncodes = 0, ccap = 0;
            collect_types(iroot, code_types, 1, &codes, &ncodes, &ccap);
            for (size_t c = 0; c < ncodes; c++)
                recolor(&spans, off + ts_node_start_byte(codes[c]),
                        off + ts_node_end_byte(codes[c]), cage_inline_code_style());
            free(codes);
            free(sub);
        }
        free(inlines);
    }

    static const char *heading_types[] = {"atx_heading", "setext_heading"};
    TSNode *headings = NULL;
    size_t nhead = 0, hcap = 0;
    collect_types(root, heading_types, 2, &headings, &nhead, &hcap);
    for (size_t i = 0; i < nhead; i++)
        recolor(&spans, ts_node_start_byte(headings[i]), ts_node_end_byte(headings[i]),
                cage_heading_style(heading_level(headings[i])));
    free(headings);

    char *out = paint(text, spans.v, spans.n, len);
    free(spans.v);
    for (size_t i = 0; i < ntrees; i++) ts_tree_delete(trees[i]);
    free(trees);
    ts_tree_delete(md_tree);
    return out;
}

char *cage_highlight_code(const char *info, const char *text) {
    syntax *s = NULL;
    char lang[32];
    size_t i = 0;
    while (info[i] && i < sizeof lang - 1 && info[i] != ' ' && info[i] != '\t') {
        lang[i] = (char)(info[i] >= 'A' && info[i] <= 'Z' ? info[i] - 'A' + 'a' : info[i]);
        i++;
    }
    lang[i] = '\0';
    if (strcmp(lang, "js") == 0 || strcmp(lang, "javascript") == 0 || strcmp(lang, "jsx") == 0) s = grammar_js();
    else if (strcmp(lang, "bash") == 0 || strcmp(lang, "sh") == 0 || strcmp(lang, "shell") == 0) s = grammar_bash();
    if (!s) return strdup(text);
    size_t len = strlen(text);
    TSTree *tree = NULL;
    TSNode root = parse(s, text, len, &tree);
    spanlist spans = {0};
    spans_of(s, root, 0, &spans);
    char *out = paint(text, spans.v, spans.n, len);
    free(spans.v);
    ts_tree_delete(tree);
    return out;
}
