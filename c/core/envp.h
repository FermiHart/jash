// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Checked environment admission shared by foreground, pipeline and background exec.
#ifndef JASH_CORE_ENVP_H
#define JASH_CORE_ENVP_H

#include <core/env.h>
#include <errno.h>
#include <string.h>

#define JASH_ENVP_MAX 512
#define JASH_ENVP_BYTES 65536
#define JASH_ENVP_LIMIT_MESSAGE \
    "jash: environment exceeds execution limit (512 entries or 65536 bytes)\n"

/* The complete vector must fit, including every '=' and string terminator.
 * On failure no prefix is usable: clear the vector and return E2BIG before
 * exec. Parser expansion/redirect preparation may precede this admission;
 * callers retain responsibility for those observations and cleanup. */
static inline int jash_envp_build(char* envp[], unsigned max_envc,
                                  char* buf, unsigned buf_cap,
                                  const jash_state_t* s,
                                  unsigned* count_out, unsigned* bytes_out) {
    unsigned envc = 0, blen = 0;
    uint32_t n = jash_env_count(s);
    for (uint32_t i = 0; i < n; i++) {
        const char *key = NULL, *value = NULL;
        if (!jash_env_at(s, i, &key, &value)) continue;
        size_t kl = strlen(key), vl = strlen(value);
        size_t remaining = buf_cap - blen;
        if (envc >= max_envc || kl > remaining || remaining - kl < 2 ||
            vl > remaining - kl - 2) {
            envp[0] = NULL;
            if (count_out) *count_out = 0;
            if (bytes_out) *bytes_out = 0;
            errno = E2BIG;
            return -1;
        }
        envp[envc++] = buf + blen;
        memcpy(buf + blen, key, kl); blen += (unsigned)kl;
        buf[blen++] = '=';
        memcpy(buf + blen, value, vl); blen += (unsigned)vl;
        buf[blen++] = '\0';
    }
    envp[envc] = NULL;
    if (count_out) *count_out = envc;
    if (bytes_out) *bytes_out = blen;
    return 0;
}

#endif
