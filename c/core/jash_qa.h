// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_qa.h — Self-Healing QA Engine for Jash
// The first AI-driven QA system built into a shell.

#ifndef JASH_QA_H
#define JASH_QA_H

#include <theme/io.h>
#include <core/state.h>

// Run a full QA scan: source analysis + test generation + execution + report
void jash_qa_run(jash_writer_t w, void *ctx, const char *src_dir);
void jash_qa_scan(jash_writer_t w, void *ctx, const char *src_dir);
void jash_qa_test(jash_writer_t w, void *ctx);
void jash_qa_aggressive(jash_writer_t w, void *ctx, const char *jash_bin);
void jash_qa_status(jash_writer_t w, void *ctx);

#endif // JASH_QA_H
