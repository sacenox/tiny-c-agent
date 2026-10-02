// cage host functions: read, edit, and bash, exposed as codemode globals.
//
// They are the seam between a script and its capabilities. `bash` streams its
// combined output through the runtime's output callback and honors the run's
// cancel flag.

#ifndef CAGE_HOST_H
#define CAGE_HOST_H

#include "tool.h"

// Registers read, edit, and bash on the runtime. The runtime outlives the
// calls.
void cage_hosts_register(cage_runtime *rt);

#endif
