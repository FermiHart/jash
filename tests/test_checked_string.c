// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include <stdlib.h>
#include <stdio.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

int main(void) {
    struct { unsigned char before; char bytes[4]; unsigned char after; } b = {0xa5, "old", 0x5a};
    REQUIRE(jash_copy_cstr(b.bytes, sizeof b.bytes, "abc") == 0);
    REQUIRE(!strcmp(b.bytes, "abc") && b.before == 0xa5 && b.after == 0x5a);
    errno = 0;
    REQUIRE(jash_copy_cstr(b.bytes, sizeof b.bytes, "abcd") == -1 && errno == E2BIG);
    REQUIRE(!strcmp(b.bytes, "abc") && b.before == 0xa5 && b.after == 0x5a);
    const char unterminated[4] = {'a', 'b', 'c', 'd'};
    REQUIRE(jash_copy_cstr(b.bytes, sizeof b.bytes, unterminated) == -1);
    REQUIRE(!strcmp(b.bytes, "abc"));
    REQUIRE(jash_copy_cstr(b.bytes, 0, "") == -1 && errno == E2BIG);
    REQUIRE(!strcmp(b.bytes, "abc"));
    REQUIRE(jash_copy_cstr(b.bytes, 1, "x") == -1);
    REQUIRE(jash_copy_cstr(b.bytes, 1, "") == 0 && b.bytes[0] == 0);
    REQUIRE(jash_copy_cstr(b.bytes, sizeof b.bytes, "") == 0);
    REQUIRE(jash_copy_cstr(NULL, 4, "a") == -1 && errno == EINVAL);
    REQUIRE(jash_copy_cstr(b.bytes, 4, NULL) == -1 && errno == EINVAL);
    char overlap[16] = "abcdef";
    REQUIRE(jash_copy_cstr(overlap, sizeof overlap, overlap + 2) == 0);
    REQUIRE(!strcmp(overlap, "cdef"));
    REQUIRE(jash_copy_cstr(overlap + 2, sizeof overlap - 2, overlap) == 0);
    REQUIRE(!strcmp(overlap + 2, "cdef"));
    REQUIRE(jash_copy_cstr(overlap, sizeof overlap, overlap) == 0);
    puts("checked string: capacity, nontermination, failure atomicity and overlap PASS");
    return 0;
}
