// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "core/ai_source_page.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "source page gate line %d: %s (errno %d)\n", \
            __LINE__, #x, errno); return 1; \
} } while (0)

int main(void) {
    char base[] = "/tmp/jash-ai-source-XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    int cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd >= 0 && chdir(base) == 0);
    int fd = open("raw.c", O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    const unsigned char bytes[] = {'a', 0, 0xff, 'Z'};
    CHECK(write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
    CHECK(close(fd) == 0);
    char out[4096];
    CHECK(jash_ai_source_page("raw.c", 0, 4, out, sizeof(out)) == 0);
    CHECK(strstr(out, "\"schema\":\"jash.source-page.v1\"") != NULL);
    CHECK(strstr(out, "\"text\":null") != NULL);
    CHECK(strstr(out, "\"data\":{\"encoding\":\"base64\",\"data\":\"YQD/Wg==\"}") != NULL);
    CHECK(strstr(out, "\"eof\":true") != NULL);
    CHECK(jash_ai_source_page("raw.c", 4, 4, out, sizeof(out)) == 0);
    CHECK(strstr(out, "\"bytes_returned\":0") != NULL);
    CHECK(jash_ai_source_page("raw.c", 5, 4, out, sizeof(out)) < 0);
    CHECK(errno == ERANGE && out[0] == '\0');
    CHECK(symlink("raw.c", "link.c") == 0);
    CHECK(jash_ai_source_page("link.c", 0, 4, out, sizeof(out)) < 0);
    CHECK(out[0] == '\0');
    CHECK(jash_ai_source_page("../raw.c", 0, 4, out, sizeof(out)) < 0);
    CHECK(out[0] == '\0');
    CHECK(jash_ai_source_page("raw.c", 0, 0, out, sizeof(out)) < 0);
    CHECK(out[0] == '\0');
    CHECK(jash_ai_source_page("raw.c", 0, 1025, out, sizeof(out)) < 0);
    CHECK(errno == EINVAL && out[0] == '\0');
    CHECK(mkfifo("pipe.h", 0600) == 0);
    CHECK(jash_ai_source_page("pipe.h", 0, 4, out, sizeof(out)) < 0);
    CHECK(errno == EINVAL && out[0] == '\0');
    CHECK(unlink("pipe.h") == 0);
    CHECK(unlink("link.c") == 0 && unlink("raw.c") == 0);
    CHECK(fchdir(cwd) == 0 && close(cwd) == 0 && rmdir(base) == 0);
    puts("ai source page: ok");
    return 0;
}
