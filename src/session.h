// cage sessions: append-only JSONL, one record per line, fsynced on write.

#ifndef CAGE_SESSION_H
#define CAGE_SESSION_H

#include <yyjson.h>

typedef struct cage_session cage_session;

// Creates the session directory and its session.jsonl, then writes the header.
// The title seeds the directory name. Returns NULL and sets *err (malloc'd,
// caller frees) on failure.
cage_session *cage_session_open(const char *sessions_dir, const char *cwd,
                                const char *title, char **err);

// The session directory name.
const char *cage_session_id(const cage_session *s);

// Serializes one record as a line and appends it, then fsyncs. The value must
// belong to a document that outlives the call. Returns 0, or -1 with *err set.
// Committed lines are never rewritten or deleted.
int cage_session_append(cage_session *s, yyjson_mut_val *record, char **err);

void cage_session_close(cage_session *s);

#endif
