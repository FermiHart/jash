// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jef_verify — standalone CLI that verifies jash attestation artifacts using
// ONLY libjef (no jash). Demonstrates third-party verification.
//
//   jef_verify chain <chain.log>
//   jef_verify proof <session.jashproof | .dsse>
//   jef_verify proof-pinned <pubkey-hex> <session.jashproof | .dsse>
//   jef_verify job-receipt <pubkey-hex> <start|action|terminal> <request> <response> <receipt>
//   jef_verify maintenance-receipt <pubkey-hex> <request> <response> <receipt>
//   jef_verify maintenance-intent <pubkey-hex> <request> <receipt>
//   jef_verify maintenance-decision <pubkey-hex> <request> <receipt>
//   jef_verify job-spec <pubkey-hex> <spec-path> <start-request> <start-response> <start-receipt> <terminal-request> <terminal-response> <terminal-receipt>

#define _GNU_SOURCE
#include "jef_verify.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static int decimal_long(const char* text, long min, long max, long* out) {
    if (!text || !*text) return -1;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++)
        if (!isdigit(*p)) return -1;
    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (errno == ERANGE || !end || *end || value < min || value > max)
        return -1;
    *out = value;
    return 0;
}

static int sealed_parent_fd(pid_t parent, const char* text, int optional) {
    if (optional && strcmp(text, "-") == 0) return -2;
#if defined(__linux__) && defined(F_GET_SEALS)
    long source = -1;
    if (decimal_long(text, 0, INT_MAX, &source) != 0) return -1;
    char path[96];
    int n = snprintf(path, sizeof path, "/proc/%ld/fd/%ld",
                     (long)parent, source);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        seals < 0 || (seals & required) != required) {
        close(fd);
        return -1;
    }
    return fd;
#else
    (void)parent; (void)text;
    return -1;
#endif
}

static int runtime_server(const char* pubkey, const char* parent_text) {
#if !defined(__linux__) || !defined(F_GET_SEALS)
    (void)pubkey; (void)parent_text;
    fprintf(stderr, "runtime-server requires Linux memfd seals and procfs\n");
    return 2;
#else
    if (strlen(pubkey) != 64) return 2;
    for (const unsigned char* p = (const unsigned char*)pubkey; *p; p++)
        if (!isxdigit(*p)) return 2;
    long parent_long = -1;
    if (decimal_long(parent_text, 2, INT_MAX, &parent_long) != 0 ||
        (pid_t)parent_long != getppid()) {
        fprintf(stderr, "runtime-server parent identity mismatch\n");
        return 2;
    }
    pid_t parent = (pid_t)parent_long;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) {
        fprintf(stderr, "runtime-server cannot bind parent lifetime\n");
        return 2;
    }
    printf("JEFV\t1\tREADY\t%ld\tsealed-procfd\n", (long)getpid());
    fflush(stdout);

    char line[1024];
    for (;;) {
        size_t length = 0;
        int ch = EOF;
        int oversized = 0;
        int raw_nul = 0;
        while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
            if (ch == '\0') raw_nul = 1;
            if (length + 1 < sizeof line) line[length++] = (char)ch;
            else oversized = 1;
        }
        if (ch == EOF && length == 0 && !oversized && !raw_nul) break;
        if (ch == EOF || oversized || raw_nul) {
            printf("R\t-\tERROR\n");
            fflush(stdout);
            continue;
        }
        line[length] = '\0';
        if (getppid() != parent) return 2;
        char* fields[8];
        int count = 0;
        int extra_fields = 0;
        char* cursor = line;
        while (count < 8) {
            fields[count++] = cursor;
            char* tab = strchr(cursor, '\t');
            if (!tab) break;
            if (count == 8) {
                extra_fields = 1;
                break;
            }
            *tab = '\0';
            cursor = tab + 1;
        }
        long sequence = -1;
        if (count != 8 || extra_fields ||
            strcmp(fields[0], "V") != 0 ||
            decimal_long(fields[1], 0, LONG_MAX, &sequence) != 0 ||
            !fields[7][0] || strlen(fields[7]) >= 80 ||
            strstr(fields[7], "..") || strchr(fields[7], '/')) {
            printf("R\t-\tERROR\n");
            fflush(stdout);
            continue;
        }

        int fds[5] = {-1, -1, -1, -1, -1};
        int bad = 0;
        for (int i = 0; i < 5; i++) {
            fds[i] = sealed_parent_fd(parent, fields[i + 2], i >= 3);
            if (fds[i] == -1) bad = 1;
        }
        if (bad) {
            for (int i = 0; i < 5; i++) if (fds[i] >= 0) close(fds[i]);
            printf("R\t%ld\tERROR\n", sequence);
            fflush(stdout);
            continue;
        }

        char paths[3][64];
        for (int i = 0; i < 3; i++) {
            if (fds[i] < 0) continue;
            snprintf(paths[i], sizeof paths[i], "/proc/self/fd/%d", fds[i]);
        }
        int result = jef_verify_runtime_receipt_with_streams(
            paths[2], paths[1], paths[0], pubkey, fields[7],
            fds[3], fds[4]);
        for (int i = 0; i < 5; i++) if (fds[i] >= 0) close(fds[i]);
        printf("R\t%ld\t%s\n", sequence,
               result == 0 ? "VALID" : result > 0 ? "INVALID" : "ERROR");
        fflush(stdout);
    }
    return ferror(stdin) ? 2 : 0;
#endif
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <chain|proof> <path> | "
                        "proof-pinned <pubkey-hex> <path> | "
                        "runtime-receipt <pubkey-hex> <request> <response> "
                        "<receipt> [receipt-ref] | "
                         "maintenance-receipt <pubkey-hex> <request> <response> <receipt> | "
                         "maintenance-intent <pubkey-hex> <request> <receipt> | "
                         "maintenance-decision <pubkey-hex> <request> <receipt> | "
                         "artifact-receipt <pubkey-hex> <request> <artifact> <receipt> | "
                        "job-receipt <pubkey-hex> <start|action|terminal> <request> "
                        "<response> <receipt> [receipt-ref] | "
                        "job-spec <pubkey-hex> <spec-path> <start-request> "
                        "<start-response> <start-receipt> <terminal-request> "
                        "<terminal-response> <terminal-receipt> | "
                        "runtime-server <pubkey-hex> <parent-pid>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "runtime-server") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s runtime-server <pubkey-hex> "
                            "<parent-pid>\n", argv[0]);
            return 2;
        }
        return runtime_server(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "chain") == 0) {
        unsigned long long n = 0;
        int r = jef_verify_chain(argv[2], &n);
        if (r == 0) { printf("chain OK — %llu entries verified\n", n); return 0; }
        if (r < 0)  { printf("chain: cannot open %s\n", argv[2]); return 2; }
        printf("chain BROKEN\n"); return 1;
    }
    if (strcmp(argv[1], "proof") == 0) {
        int r = jef_verify_proof(argv[2]);
        if (r == 0) { printf("proof VALID (ed25519, pubkey-only)\n"); return 0; }
        if (r < 0)  { printf("proof: cannot open %s\n", argv[2]); return 2; }
        printf("proof INVALID\n"); return 1;
    }
    if (strcmp(argv[1], "proof-pinned") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s proof-pinned <pubkey-hex> <path>\n", argv[0]);
            return 2;
        }
        int r = jef_verify_proof_pinned(argv[3], argv[2]);
        if (r == 0) { printf("proof VALID (ed25519, pinned pubkey)\n"); return 0; }
        if (r < 0)  { printf("proof: cannot verify %s\n", argv[3]); return 2; }
        printf("proof INVALID OR FOREIGN\n"); return 1;
    }
    if (strcmp(argv[1], "artifact-receipt") == 0) {
        if (argc != 6) { fprintf(stderr, "usage: %s artifact-receipt <pubkey-hex> <request> <artifact> <receipt>\n", argv[0]); return 2; }
        int result = jef_verify_artifact_receipt(argv[5], argv[3], argv[4], argv[2]);
        puts(result == 0 ? "artifact receipt VALID and content-bound" : "artifact receipt INVALID, FOREIGN, OR UNBOUND");
        return result == 0 ? 0 : result < 0 ? 2 : 1;
    }
    if (strcmp(argv[1], "maintenance-decision") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: %s maintenance-decision <pubkey-hex> <request> <receipt>\n", argv[0]); return 2; }
        int admitted = 0;
        int result = jef_verify_maintenance_decision(argv[4], argv[3], argv[2], &admitted);
        puts(result == 0 ? (admitted ? "maintenance decision VALID: admitted (not completion)" : "maintenance decision VALID: closed") : "maintenance decision INVALID, FOREIGN, OR UNBOUND");
        return result == 0 ? 0 : result < 0 ? 2 : 1;
    }
    if (strcmp(argv[1], "maintenance-intent") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: %s maintenance-intent <pubkey-hex> <request> <receipt>\n", argv[0]); return 2; }
        int result = jef_verify_maintenance_intent(argv[4], argv[3], argv[2]);
        puts(result == 0 ? "maintenance intent VALID and bound (not completion)" : "maintenance intent INVALID, FOREIGN, OR UNBOUND");
        return result == 0 ? 0 : result < 0 ? 2 : 1;
    }
    if (strcmp(argv[1], "maintenance-receipt") == 0) {
        if (argc != 6) { fprintf(stderr, "usage: %s maintenance-receipt <pubkey-hex> <request> <response> <receipt>\n", argv[0]); return 2; }
        int result = jef_verify_maintenance_receipt(argv[5], argv[4], argv[3], argv[2]);
        puts(result == 0 ? "maintenance receipt VALID and bound" : "maintenance receipt INVALID, FOREIGN, OR UNBOUND");
        return result == 0 ? 0 : result < 0 ? 2 : 1;
    }
    if (strcmp(argv[1], "runtime-receipt") == 0) {
        if (argc != 6 && argc != 7) {
            fprintf(stderr, "usage: %s runtime-receipt <pubkey-hex> "
                            "<request> <response> <receipt> "
                            "[receipt-ref]\n", argv[0]);
            return 2;
        }
        int r = jef_verify_runtime_receipt(
            argv[5], argv[4], argv[3], argv[2], argc == 7 ? argv[6] : NULL
        );
        if (r == 0) { printf("runtime receipt VALID and bound\n"); return 0; }
        if (r < 0)  { printf("runtime receipt: cannot read artifact\n"); return 2; }
        printf("runtime receipt INVALID, FOREIGN, OR UNBOUND\n"); return 1;
    }
    if (strcmp(argv[1], "job-spec") == 0) {
        if (argc != 10) {
            fprintf(stderr, "usage: %s job-spec <pubkey-hex> <spec-path> "
                            "<start-request> <start-response> <start-receipt> "
                            "<terminal-request> <terminal-response> "
                            "<terminal-receipt>\n", argv[0]);
            return 2;
        }
        int r = jef_verify_job_spec(argv[2], argv[3], argv[4], argv[5],
                                    argv[6], argv[7], argv[8], argv[9]);
        if (r == 0) { printf("job spec VALID and provenance-bound (not a success assertion)\n"); return 0; }
        if (r == -2) { printf("job spec: malformed expected pubkey\n"); return 2; }
        if (r < 0) { printf("job spec: cannot read artifact or size limit exceeded\n"); return 2; }
        printf("job spec INVALID, FOREIGN, OR UNBOUND\n"); return 1;
    }
    if (strcmp(argv[1], "job-receipt") == 0) {
        if (argc != 7 && argc != 8) {
            fprintf(stderr, "usage: %s job-receipt <pubkey-hex> "
                            "<start|action|terminal> <request> <response> "
                            "<receipt> [receipt-ref]\n", argv[0]);
            return 2;
        }
        int r = jef_verify_job_receipt(
            argv[6], argv[5], argv[4], argv[2], argv[3],
            argc == 8 ? argv[7] : NULL);
        if (r == 0) { printf("job receipt VALID and bound\n"); return 0; }
        if (r < 0)  { printf("job receipt: cannot read artifact\n"); return 2; }
        printf("job receipt INVALID, FOREIGN, OR UNBOUND\n"); return 1;
    }
    fprintf(stderr, "unknown verb '%s'\n", argv[1]);
    return 2;
}
