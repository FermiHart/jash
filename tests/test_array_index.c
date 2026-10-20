#include <core/env.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int members(jash_state_t* s, const char* base, uint32_t* probes) {
    int count = 0;
    jash_env_array_iter_t iter;
    jash_env_array_iter_init(&iter, s, base);
    for (int i; (i = jash_env_array_iter_next(&iter)) >= 0; ) {
        const char* key = NULL;
        if (!jash_env_at(s, (uint32_t)i, &key, NULL) || !key) return -1;
        if (++count > JASH_MAX_ENV) return -1;
    }
    if (probes) *probes = iter.probes;
    return count;
}

static int fail(const char* message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static unsigned array_bucket(const char* base) {
    unsigned hash = 2166136261u;
    while (*base) {
        hash ^= (unsigned char)*base++;
        hash *= 16777619u;
    }
    return hash % JASH_ENV_ARRAY_HASH_SIZE;
}

int main(void) {
    jash_state_t* s = calloc(1, sizeof(*s));
    if (!s) return fail("state allocation");
    for (int i = 0; i < JASH_ENV_HASH_SIZE; i++) s->env_hash[i] = -1;
    jash_env_array_index_init(s);

    char key[64];
    for (int i = 0; i < 1800; i++) {
        snprintf(key, sizeof(key), "scalar_%d", i);
        if (jash_env_set(s, key, "x") != 0) return fail("scalar setup");
    }
    if (jash_env_set(s, "arr[9]", "nine") != 0 ||
        jash_env_set(s, "arr[1]", "one") != 0 ||
        jash_env_set(s, "arr[key]", "assoc") != 0 ||
        jash_env_set(s, "other[0]", "other") != 0)
        return fail("array setup");

    uint32_t probes = 0;
    if (members(s, "arr", &probes) != 3 || probes > 4 ||
        members(s, "other", NULL) != 1)
        return fail("index included scalars or lost array members");

    int seen[JASH_ENV_ARRAY_HASH_SIZE];
    for (int i = 0; i < JASH_ENV_ARRAY_HASH_SIZE; i++) seen[i] = -1;
    int first = -1, second = -1;
    for (int i = 0; i < 10000 && second < 0; i++) {
        char base[32];
        snprintf(base, sizeof(base), "collision_%d", i);
        unsigned bucket = array_bucket(base);
        if (seen[bucket] >= 0) {
            first = seen[bucket];
            second = i;
        } else {
            seen[bucket] = i;
        }
    }
    char base_a[32], base_b[32], key_a[64], key_b[64];
    snprintf(base_a, sizeof(base_a), "collision_%d", first);
    snprintf(base_b, sizeof(base_b), "collision_%d", second);
    snprintf(key_a, sizeof(key_a), "%s[x]", base_a);
    snprintf(key_b, sizeof(key_b), "%s[y]", base_b);
    if (first < 0 || second < 0 || jash_env_set(s, key_a, "a") != 0 ||
        jash_env_set(s, key_b, "b") != 0 || members(s, base_a, NULL) != 1 ||
        members(s, base_b, NULL) != 1)
        return fail("colliding bases were not filtered exactly");

    /* Unsetting an early scalar moves the final array member into its slot. */
    if (jash_env_unset(s, "scalar_0") != 0 || members(s, "other", NULL) != 1 ||
        strcmp(jash_env_get(s, "other[0]"), "other") != 0)
        return fail("swap-delete did not relink moved array member");

    if (jash_env_unset(s, "arr[1]") != 0 || members(s, "arr", NULL) != 2)
        return fail("element unset left stale membership");
    if (jash_env_append(s, "arr[9]", "!") != 0 || members(s, "arr", NULL) != 2 ||
        strcmp(jash_env_get(s, "arr[9]"), "nine!") != 0)
        return fail("value update duplicated membership");

    const char* batch_keys[] = {"arr[42]", "batch[k]"};
    const char* batch_vals[] = {"forty-two", "batched"};
    if (jash_env_apply_batch(s, batch_keys, batch_vals, 2) != 0 ||
        members(s, "arr", NULL) != 3 || members(s, "batch", NULL) != 1)
        return fail("batch replacement lost index membership");

    jash_env_array_index_rebuild(s);
    if (members(s, "arr", NULL) != 3 || members(s, "other", NULL) != 1 ||
        members(s, "batch", NULL) != 1)
        return fail("rebuild changed membership");

    for (uint32_t i = 0; i < s->env_count; i++) free(s->env[i].heap_val);
    free(s);
    puts("PASS: array index tracks only members across set/unset/swap/rebuild");
    return 0;
}
