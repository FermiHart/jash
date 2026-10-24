// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — startup banner

#include "banner.h"
#include "colors.h"
#include <string.h>
#include <sys/utsname.h>

/* Form bounded banner records before calling the output writer. Formatting
 * tokens are not output records: another process may write between two
 * callbacks. Keep this staging local to one banner invocation; prompts,
 * command output and the generic formatter retain their existing behavior.
 * Long lines are streamed in bounded spans without clipping any bytes. */
#define BANNER_RECORD_BYTES 192u
struct banner_writer {
    jash_writer_t output;
    void* context;
    char bytes[BANNER_RECORD_BYTES];
    size_t used;
    long error;
};

static long banner_flush(struct banner_writer* b) {
    if (b->error < 0) return b->error;
    size_t sent = 0;
    while (sent < b->used) {
        size_t remaining = b->used - sent;
        long n = b->output(b->context, b->bytes + sent, remaining);
        if (n <= 0 || (size_t)n > remaining) {
            /* A void banner API cannot report delivery success. Stop after
             * a failed/nonprogressing writer instead of dropping a suffix
             * and emitting later records as though it had been delivered. */
            b->error = n < 0 ? n : -1;
            return b->error;
        }
        sent += (size_t)n;
    }
    b->used = 0;
    return (long)sent;
}

static long banner_collect(void* context, const char* text, size_t n) {
    struct banner_writer* b = context;
    if (b->error < 0) return b->error;
    for (size_t i = 0; i < n; i++) {
        b->bytes[b->used++] = text[i];
        if (text[i] == '\n' || b->used == sizeof(b->bytes)) {
            long result = banner_flush(b);
            if (result < 0) return result;
        }
    }
    return (long)n;
}

/* JASH logo — block unicode characters */
static const char* const JASH_LOGO[] = {
    "",
    "    ██╗ █████╗ ███████╗██╗  ██╗",
    "    ██║██╔══██╗██╔════╝██║  ██║",
    "    ██║███████║███████╗███████║",
    "██  ██║██╔══██║╚════██║██╔══██║",
    "█████╔╝██║  ██║███████║██║  ██║",
    "╚════╝ ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝",
    "",
    0
};

static const char* get_os_name(void) {
    static struct utsname u;
    if (uname(&u) == 0) return u.sysname;
    return "Unknown";
}

static const char* get_arch(void) {
    static struct utsname u;
    if (uname(&u) == 0) return u.machine;
    return "";
}

void jash_banner_short(jash_writer_t w, void* ctx) {
    if (!w) return;
    struct banner_writer record = { .output = w, .context = ctx };
    jash_writef(banner_collect, &record,
        "%sjash %s%s%s %s%s%s\n",
        J_ROLE_PROMPT, J_BOLD, JASH_VERSION, J_RESET,
        J_ROLE_ACCENT, JASH_CODENAME, J_RESET);
    (void)banner_flush(&record);
}

void jash_banner_render(jash_writer_t w, void* ctx, long pid) {
    if (!w) return;
    struct banner_writer record = { .output = w, .context = ctx };
    w = banner_collect;
    ctx = &record;

    jash_writes(w, ctx, "\n");

    /* Logo */
    jash_writes(w, ctx, J_ROLE_BANNER J_BOLD);
    for (int i = 0; JASH_LOGO[i]; i++) {
        jash_writes(w, ctx, JASH_LOGO[i]);
        jash_writes(w, ctx, "\n");
    }
    jash_writes(w, ctx, J_RESET);

    /* 0.1α Subsumption · Darwin x86_64 */
    jash_writef(w, ctx,
        "  %s%s%s%s %sα%s %s%s%s  %s·%s  %s%s%s %s%s%s\n",
        J_ROLE_BANNER, J_BOLD, JASH_VERSION, J_RESET,
        J_ROLE_ACCENT, J_RESET,
        J_ROLE_OK, JASH_CODENAME, J_RESET,
        J_ROLE_MUTED, J_RESET,
        J_ROLE_PROMPT, get_os_name(), J_RESET,
        J_ROLE_MUTED, get_arch(), J_RESET);

    /* Separator */
    jash_writef(w, ctx, "  %s", J_ROLE_MUTED);
    for (int i = 0; i < 38; i++) jash_writes(w, ctx, "─");
    jash_writef(w, ctx, "%s\n", J_RESET);

    /* Type help for commands  Pid 12345 */
    jash_writef(w, ctx,
        "  %sType%s %shelp%s %sfor commands%s  %sPid %s%ld%s\n",
        J_ROLE_MUTED, J_RESET,
        J_BOLD, J_RESET,
        J_ROLE_MUTED, J_RESET,
        J_ROLE_MUTED, J_RESET, pid, J_RESET);

    jash_writes(w, ctx, "\n");
    (void)banner_flush(&record);
}
