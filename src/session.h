// tiny sessions: append-only JSONL, one record per line, fsynced on write.

#ifndef TINY_SESSION_H
#define TINY_SESSION_H

#include <yyjson.h>

typedef struct tiny_session tiny_session;

// Creates the session directory and its session.jsonl, then writes the header.
// The title seeds the directory name. Returns NULL and sets *err (malloc'd,
// caller frees) on failure.
tiny_session *tiny_session_open(const char *sessions_dir, const char *cwd,
                                const char *title, char **err);

// The session directory name.
const char *tiny_session_id(const tiny_session *s);

// Serializes one record as a line and appends it, then fsyncs. The value must
// belong to a document that outlives the call. Returns 0, or -1 with *err set.
// Committed lines are never rewritten or deleted.
int tiny_session_append(tiny_session *s, yyjson_mut_val *record, char **err);

void tiny_session_close(tiny_session *s);

#endif
