// cage working-tree diffs: what a codemode run changed on disk.
//
// The changes are display-only. A snapshot records the working tree; diffing
// the snapshots around a run yields the changed files, each a unified patch or
// a note for content that cannot be tracked. A process-wide cache keyed by the
// stat triple (size, mtime, ino) means a later snapshot re-reads only files
// whose stat moved.

#ifndef CAGE_DIFF_H
#define CAGE_DIFF_H

#include <stddef.h>
#include <time.h>

#include "tool.h"

struct cage_snapshot;

// Captures the working tree under the current directory. Files modified at or
// after `force_since` (realtime, may be NULL) are re-read even if their stat is
// unchanged, covering edits within one mtime tick.
struct cage_snapshot *cage_snapshot_capture(const struct timespec *force_since);
void cage_snapshot_free(struct cage_snapshot *s);

// The files that differ between two snapshots, sorted by path. The caller owns
// the result; free it with cage_file_diffs_free.
cage_file_diff *cage_snapshot_diff(const struct cage_snapshot *before,
                                   const struct cage_snapshot *after, size_t *len);
void cage_file_diffs_free(cage_file_diff *diffs, size_t len);

#endif
