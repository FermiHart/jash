// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !chdir(argv[1]) && !setenv("HOME", argv[1], 1));
    srv_job_t job = {0}; strcpy(job.job_id, "j-0123456789abcdef0123456789abcdef");
    assert(!srv_job_archive_write(&job, "{\"id\":\"observed\"}", "{\"terminal\":true}\n"));
    char bytes[SRV_JOB_ARCHIVE_MAX]; const char *request, *response;
    assert(!srv_job_archive_read(job.job_id, bytes, &request, &response));
    assert(!strcmp(request, "{\"id\":\"observed\"}") && !strcmp(response, "{\"terminal\":true}\n"));
    assert(srv_job_archive_write(&job, "changed", "changed") != 0); /* no clobber */
    assert(!srv_job_archive_read(job.job_id, bytes, &request, &response));
    assert(!strcmp(request, "{\"id\":\"observed\"}"));
    assert(srv_job_archive_read("../../escape", bytes, &request, &response) != 0);
    char path[256]; snprintf(path, sizeof(path), ".jash/agent_job_results/%s.result", job.job_id);
    int fd = open(path, O_RDWR); assert(fd >= 0);
    srv_job_archive_t header; assert(read(fd, &header, sizeof(header)) == sizeof(header));
    header.request_len = UINT32_MAX;
    assert(pwrite(fd, &header, sizeof(header), 0) == sizeof(header));
    assert(srv_job_archive_read(job.job_id, bytes, &request, &response) != 0);
    assert(!ftruncate(fd, 1024 * 1024));
    assert(srv_job_archive_read(job.job_id, bytes, &request, &response) != 0);
    close(fd);
    return 0;
}
