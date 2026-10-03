// tiny working-tree diffs: what a tool run changed on disk.
//
// The changes are display-only. A snapshot records the working tree; diffing
// the snapshots around a run yields the changed files, each a unified patch or
// a note for content that cannot be tracked. A process-wide cache keyed by the
// stat triple (size, mtime, ino) means a later snapshot re-reads only files
// whose stat moved.

#ifndef TINY_DIFF_H
#define TINY_DIFF_H

#include <stddef.h>
#include <time.h>

#include "tool.h"

struct tiny_snapshot;

// Captures the working tree under the current directory. Files modified at or
// after `force_since` (realtime, may be NULL) are re-read even if their stat is
// unchanged, covering edits within one mtime tick.
struct tiny_snapshot *tiny_snapshot_capture(const struct timespec *force_since);
void tiny_snapshot_free(struct tiny_snapshot *s);

// The files that differ between two snapshots, sorted by path. The caller owns
// the result; free it with tiny_file_diffs_free.
tiny_file_diff *tiny_snapshot_diff(const struct tiny_snapshot *before,
                                   const struct tiny_snapshot *after, size_t *len);
void tiny_file_diffs_free(tiny_file_diff *diffs, size_t len);

#endif
