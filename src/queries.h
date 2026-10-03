// Compiled-in tree-sitter highlight queries.
//
// The text lives in src/queries.generated.c, generated from vendor/queries by
// tools/gen_queries.py. Keeping them in the binary avoids any runtime lookup
// path.

#ifndef TINY_QUERIES_H
#define TINY_QUERIES_H

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *lang;
    const char *text;
} tiny_query;

// Returns the highlights query for `lang`, or NULL when none is compiled in.
const char *tiny_query_for(const char *lang);

#endif
