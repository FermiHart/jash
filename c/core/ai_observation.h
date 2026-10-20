// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_AI_OBSERVATION_H
#define JASH_CORE_AI_OBSERVATION_H

#include <stddef.h>
#include <stdint.h>

/* Read-only, bounded projection of the native filesystem stream. On success
 * out contains exactly one NUL-terminated JSON observation, with an ordered
 * content digest and explicit completeness/freshness claims. Failure leaves
 * out empty. Neither the caller nor a provider selects a shell command. */
int jash_ai_observe_ls(const char* root, char* out, size_t capacity);
/* Optional admitted equality predicate on the native `type` record field.
 * The resulting sealed JTV1 ref contains only matching rows. */
int jash_ai_observe_ls_filtered(const char* root, const char* type,
                                char* out, size_t capacity);
/* Chained native record predicates. has_uid differentiates uid=0 from absent. */
int jash_ai_observe_ls_where(const char* root, const char* type,
                             int has_uid, uint64_t uid,
                             char* out, size_t capacity);
/* Count the same bounded native source without materializing a JTV1 ref. */
int jash_ai_count_ls_where(const char* root, const char* type,
                           int has_uid, uint64_t uid,
                           char* out, size_t capacity);

/* Process/session-local immutable observation ref. A new successful
 * observation replaces the preceding ref. Read verifies filesystem freshness
 * before exposing a bounded base64 page; no durable artifact authority is
 * granted. Foreign, replaced and stale refs fail explicitly. */
int jash_ai_observation_read_json(const char* ref, size_t offset,
                                  size_t limit, char* out, size_t capacity);
/* Optional handoff to an independently authorized data-plane client. Returns
 * a caller-owned, CLOEXEC, independent read-only FD for the sealed snapshot.
 * This does not perform data.import/promote or grant data:create/data:manage. */
int jash_ai_observation_reader_fd(const char* ref, uint64_t* size);
void jash_ai_observation_forget(void);

/* Compiled, provider-independent discovery of admitted typed observations. */
int jash_ai_observation_describe(char* out, size_t capacity);

#endif
