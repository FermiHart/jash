// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/chain_read.h>
#include <core/adopt.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char* jash_repo_key(void) { return "-"; }

int main(void) {
    char* tiny = malloc(8); assert(tiny);
    char* argv[8];
    assert(jash_chain_tokenize_nul("012345678901234567890123456789", tiny, 8, argv, 8) == -1);
    free(tiny);
    char guarded[12]; memset(guarded, '!', sizeof(guarded));
    assert(jash_chain_tokenize_nul("01234567", guarded + 1, 8, argv, 8) == -1);
    assert(guarded[0] == '!' && guarded[9] == '!');
    assert(jash_chain_tokenize_nul("0123456", guarded + 1, 8, argv, 8) == 1);
    assert(!strcmp(argv[0], "0123456") && guarded[9] == '!');
    char workspace[64];
    assert(jash_chain_tokenize_nul("one 'two three' \"four five\"", workspace, sizeof(workspace), argv, 8) == 3);
    assert(!strcmp(argv[0], "one") && !strcmp(argv[1], "two three") && !strcmp(argv[2], "four five"));
    assert(jash_chain_tokenize_nul("one two three", workspace, sizeof(workspace), argv, 2) == -1);
    assert(jash_chain_tokenize_nul("one two", workspace, sizeof(workspace), argv, 2) == 2);
    assert(jash_chain_tokenize_nul("", workspace, 1, argv, 8) == 0);
    assert(jash_chain_tokenize_nul("x", workspace, 1, argv, 8) == -1);
    assert(jash_chain_tokenize_nul("'unterminated", workspace, sizeof(workspace), argv, 8) == 1);
    assert(!strcmp(argv[0], "unterminated"));
    return 0;
}
