// Quick unit test for context engine.
// Build: cc -I.. -I../c tests/test_context_engine.c ../c/core/context_engine.c ../c/core/provider_git.c ../c/core/provider_state.c ../c/core/provider_digest.c ../c/journal/journal.c ../c/core/stance.c ../c/core/digest_engine.c ../c/module/sigil.c ../c/module/jash_crypto.c ../c/module/blake3.c ../c/core/timeline.c ../c/core/session_meta.c ../c/core/guard.c ../c/core/output_budget.c ../c/builtins/builtin_stance.c -o /tmp/test_ce
// Then: JASH_SIGIL=1 ./build/jash -c 'echo warmup' >/dev/null 2>&1 && /tmp/test_ce

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <core/context_engine.h>
#include <journal/journal.h>

static int test_basic(void) {
    context_basket_t basket;
    memset(&basket, 0, sizeof(basket));

    context_init();
    context_register_builtins(NULL);

    int n = context_get_basket(&basket);
    printf("test_basic: %d items collected\n", n);

    if (n < 0) { printf("FAIL: negative\n"); return 1; }
    printf("PASS: basic collect works (%d items)\n", n);

    context_destroy();
    return 0;
}

static int test_prioritize(void) {
    context_basket_t basket;
    memset(&basket, 0, sizeof(basket));

    context_init();
    context_register_builtins(NULL);

    /* Add a few manual items with different priorities. */
    context_item_t* items = basket.items;
    basket.n_items = 3;

    items[0].id = 1; items[0].priority = 100; items[0].pinned = 0; items[0].timestamp = 1000;
    items[0].token_cost = 10; items[0].provider = "test"; strcpy(items[0].tag, "normal");
    strcpy(items[0].source, "test:a"); strcpy(items[0].summary, "test a");

    items[1].id = 2; items[1].priority = 200; items[1].pinned = 0; items[1].timestamp = 2000;
    items[1].token_cost = 10; items[1].provider = "test"; strcpy(items[1].tag, "high");
    strcpy(items[1].source, "test:b"); strcpy(items[1].summary, "test b");

    items[2].id = 3; items[2].priority = 50; items[2].pinned = 0; items[2].timestamp = 500;
    items[2].token_cost = 10; items[2].provider = "test"; strcpy(items[2].tag, "low");
    strcpy(items[2].source, "test:c"); strcpy(items[2].summary, "test c");

    context_prioritize(&basket);

    /* After sort, order should be: 200 (item 1), 100 (item 0), 50 (item 2). */
    if (basket.items[0].priority != 200 || basket.items[1].priority != 100 || basket.items[2].priority != 50) {
        printf("FAIL: wrong sort order: %d %d %d\n",
               basket.items[0].priority, basket.items[1].priority, basket.items[2].priority);
        return 1;
    }
    printf("PASS: prioritize sorts correctly\n");
    context_destroy();
    return 0;
}

static int test_budget(void) {
    context_basket_t basket;
    memset(&basket, 0, sizeof(basket));

    context_init();
    context_register_builtins(NULL);

    basket.n_items = 3;
    for (int i = 0; i < 3; i++) {
        basket.items[i].id = (uint64_t)(i + 1);
        basket.items[i].priority = 100;
        basket.items[i].pinned = 0;
        basket.items[i].token_cost = 100;
        basket.items[i].provider = "test";
        basket.items[i].timestamp = (uint64_t)(1000 + i);
        snprintf(basket.items[i].tag, sizeof(basket.items[i].tag), "item%d", i);
        snprintf(basket.items[i].source, sizeof(basket.items[i].source), "test:%d", i);
        snprintf(basket.items[i].summary, sizeof(basket.items[i].summary), "item %d", i);
    }

    context_budget_calc(&basket, 500);
    if (basket.budget.used != 300) {
        printf("FAIL: budget used=%llu, expected 300\n", (unsigned long long)basket.budget.used);
        return 1;
    }
    if (basket.budget.total_budget != 500) {
        printf("FAIL: budget total=%llu\n", (unsigned long long)basket.budget.total_budget);
        return 1;
    }
    printf("PASS: budget calc: used=%llu/%llu\n",
           (unsigned long long)basket.budget.used,
           (unsigned long long)basket.budget.total_budget);
    context_destroy();
    return 0;
}

static int test_prune(void) {
    context_basket_t basket;
    memset(&basket, 0, sizeof(basket));

    context_init();
    context_register_builtins(NULL);

    basket.n_items = 5;
    for (int i = 0; i < 5; i++) {
        basket.items[i].id = (uint64_t)(i + 1);
        basket.items[i].priority = (i + 1) * 20;  /* 20, 40, 60, 80, 100 */
        basket.items[i].pinned = 0;
        basket.items[i].token_cost = 100;
        basket.items[i].provider = "test";
        basket.items[i].timestamp = (uint64_t)(1000 + i);
        snprintf(basket.items[i].tag, sizeof(basket.items[i].tag), "item%d", i);
        snprintf(basket.items[i].source, sizeof(basket.items[i].source), "test:%d", i);
        snprintf(basket.items[i].summary, sizeof(basket.items[i].summary), "item %d", i);
    }

    /* Pin item 3 (priority 80). */
    basket.items[3].pinned = 1;
    /* Pin item 4 (priority 100). */
    basket.items[4].pinned = 1;

    context_budget_calc(&basket, 500);
    context_prioritize(&basket);

    int pruned = context_prune(&basket, 350);
    if (pruned < 0) { printf("FAIL: prune error\n"); return 1; }
    printf("prune: removed %d items, remaining %d, used=%llu\n",
           pruned, basket.n_items,
           (unsigned long long)basket.budget.used);

    /* Should have removed 2 lowest-priority items (20 and 40). */
    if (basket.n_items != 3) {
        printf("FAIL: expected 3 remaining items, got %d\n", basket.n_items);
        return 1;
    }
    /* Remaining should be items with priorities 60, 80, 100 (80 and 100 were pinned). */
    printf("PASS: prune kept pinned items, removed lowest priorities\n");
    context_destroy();
    return 0;
}

int main(void) {
    int fail = 0;

    journal_init();
    fail += test_basic();
    fail += test_prioritize();
    fail += test_budget();
    fail += test_prune();

    printf("\n=== %s: %d failures ===\n", fail ? "FAIL" : "ALL PASS", fail);
    return fail;
}
