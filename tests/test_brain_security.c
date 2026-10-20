#include "../c/core/jash_brain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int malloc_calls;
static int fail_on_call;

void* __real_malloc(size_t size);
void* __wrap_malloc(size_t size) {
    malloc_calls++;
    if (malloc_calls == fail_on_call) return NULL;
    return __real_malloc(size);
}

int main(void) {
    char dir[] = "/tmp/jash-brain-security-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    if (jash_brain_init(dir) != 0) return 2;
    if (jash_brain_store_ephemeral("k", "old", "oldtag") != 0) return 1;

    char val[700];
    char tag[100];
    memset(val, 'v', sizeof(val) - 1); val[sizeof(val) - 1] = '\0';
    memset(tag, 't', sizeof(tag) - 1); tag[sizeof(tag) - 1] = '\0';

    malloc_calls = 0;
    fail_on_call = 2;   /* val heap succeeds, tag heap fails */
    if (jash_brain_store_ephemeral("k", val, tag) != -1) {
        fprintf(stderr, "forced OOM was not reported\n");
        return 1;
    }

    const jash_brain_fact_t* fact = jash_brain_recall("k");
    if (!fact || fact->val_uses_heap || strcmp(fact->val, "old") != 0) {
        fprintf(stderr, "partial store corrupted the existing fact\n");
        return 1;
    }
    jash_brain_cleanup();
    return 0;
}
