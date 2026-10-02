// Internal contract between the provider dispatcher and each API adapter.

#ifndef CAGE_API_H
#define CAGE_API_H

#include "provider.h"

int cage_openai_completions_stream(const cage_model *model, const cage_context *ctx,
                                   const cage_stream_opts *opts,
                                   cage_stream_fn emit, void *user,
                                   const volatile bool *cancel,
                                   cage_message *out, char **err);

#endif
