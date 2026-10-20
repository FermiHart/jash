/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* TinaLinux native Surface MMIO /dev endpoint, never built for host Linux. */
#include "surface.h"
#include <linux/tina_surface.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>

/* Temporary, opt-in guest fault capture. Linux x86-64 ucontext begins with
 * five machine words, followed by the kernel sigcontext register array. */
static int debug_fault_fd = -1;
static void debug_fault(int sig, void *info, void *context) {
    (void)info;
    const unsigned long *regs = (const unsigned long *)context + 5;
    char message[160]; unsigned used = 0;
    const char *labels[] = {"JASH_FAULT rip=", " rsp=", " rbp=", " fault="};
    unsigned long values[] = {regs[16], regs[15], regs[10], regs[22]};
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 4; i++) {
        for (const char *p = labels[i]; *p; p++) message[used++] = *p;
        for (int shift = 60; shift >= 0; shift -= 4) message[used++] = hex[(values[i] >> shift) & 15];
    }
    message[used++] = '\n';
    if (debug_fault_fd >= 0) (void)write(debug_fault_fd, message, used);
    _exit(128 + sig);
}

static int native_output;
static int foreground_owner;
static unsigned seat;

int jash_tina_surface_probe(void) {
    struct surface_info_t info = {0};
    native_output = 0;
    foreground_owner = 0;
    if (ioctl(STDOUT_FILENO, SURF_GET_INFO, &info) != 0 ||
        info.magic != SURF_MAGIC || info.version != 2) return 0;
    seat = (unsigned)info.surf_id;
    native_output = (info.capabilities &
        (SURF_CAP_NATIVE_CELLS | SURF_CAP_PRESENTATION | SURF_CAP_INPUT_POLICY)) ==
        (SURF_CAP_NATIVE_CELLS | SURF_CAP_PRESENTATION | SURF_CAP_INPUT_POLICY);
    if (native_output) {
        struct surf_stat_t state = {0};
        if (ioctl(STDOUT_FILENO, SURF_GET_STAT, &state) == 0)
            foreground_owner = state.owner_pid == (int)getpid();
        if (getenv("JASH_TINA_SURFACE_DEBUG"))
            fprintf(stderr, "JASH_SURFACE pid=%d pgrp=%d owner=%d control=%d\n", (int)getpid(), (int)getpgrp(), state.owner_pid, foreground_owner);
        if (getenv("JASH_TINA_SURFACE_DEBUG")) {
            debug_fault_fd = open("/tmp/jash-surface-crash", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (debug_fault_fd >= 0) {
                struct sigaction action = {0};
                action.sa_sigaction = debug_fault;
                action.sa_flags = SA_SIGINFO | SA_RESETHAND;
                sigemptyset(&action.sa_mask);
                (void)sigaction(SIGSEGV, &action, NULL);
            }
        }
    }
    return native_output;
}

int jash_tina_surface_native_output(void) { return native_output; }
int jash_tina_surface_foreground_owner(void) { return foreground_owner; }

int jash_tina_surface_native_input(void) {
    struct surf_endpoint_t endpoint = {0};
    if (!native_output || ioctl(STDIN_FILENO, SURF_GET_ENDPOINT, &endpoint)) return 0;
    return endpoint.version == SURF_ENDPOINT_VERSION &&
        endpoint.size == sizeof(endpoint) && endpoint.surf_id == seat &&
        endpoint.ring == SURF_PROV_RING_IN;
}

static int input_actions(unsigned flags) {
    struct surf_input_policy_v1 policy = {
        .version = SURF_NATIVE_VERSION, .size = sizeof(policy)
    };
    if (!jash_tina_surface_native_input() ||
        ioctl(STDIN_FILENO, SURF_GET_INPUT_POLICY, &policy) ||
        policy.version != SURF_NATIVE_VERSION || policy.size != sizeof(policy)) return -1;
    if (policy.flags == flags) return 0;
    policy.flags = flags;
    return ioctl(STDIN_FILENO, SURF_SET_INPUT_POLICY, &policy);
}

int jash_tina_surface_editor_end(void) { return input_actions(1); }
int jash_tina_surface_foreground(int pgid) {
    struct surf_foreground_t fg = { .pgid = pgid, .reserved = 0 };
    if (!native_output || pgid <= 0) return -1;
    return ioctl(STDOUT_FILENO, SURF_SET_FOREGROUND, &fg);
}
int jash_tina_surface_editor_begin(void) {
    struct surf_presentation_v1 presentation = {
        .version = SURF_NATIVE_VERSION, .size = sizeof(presentation)
    };
    if (input_actions(0) || !native_output ||
        ioctl(STDOUT_FILENO, SURF_GET_PRESENTATION, &presentation) ||
        presentation.version != SURF_NATIVE_VERSION ||
        presentation.size != sizeof(presentation)) return -1;
    if (presentation.flags == SURF_PRESENT_NL_CRLF) return 0;
    presentation.flags = SURF_PRESENT_NL_CRLF;
    return ioctl(STDOUT_FILENO, SURF_SET_PRESENTATION, &presentation);
}

int jash_tina_surface_viewport(unsigned* columns, unsigned* rows) {
    struct surf_viewport_t v = {0};
    if (!native_output || (!columns && !rows) ||
        ioctl(STDOUT_FILENO, SURF_GET_VIEWPORT, &v) ||
        v.version != SURF_VIEWPORT_VERSION || v.size != sizeof(v) ||
        !v.columns || !v.rows) return -1;
    if (columns) *columns = v.columns;
    if (rows) *rows = v.rows;
    return 0;
}

static void emit(unsigned type, const char* data, size_t len) {
    if (!native_output || !data) return;
    struct bloom_cell_t cell = {0};
    cell.type = type;
    if (len > BLOOM_CELL_DATA_MAX) len = BLOOM_CELL_DATA_MAX;
    cell.len = (unsigned)len;
    memcpy(cell.data, data, len);
    (void)ioctl(STDOUT_FILENO, SURF_EMIT_CELL, &cell);
}

void jash_tina_surface_error(const char* data, size_t len) {
    emit(BLOOM_CELL_ERROR, data, len);
}

void jash_tina_surface_progress(unsigned current, unsigned total, const char* label) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%u/%u %s", current, total, label ? label : "");
    if (n > 0) emit(BLOOM_CELL_PROGRESS, buf, (size_t)n);
}
