// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
//
// Module: builtins/irq — display serial IRQ statistics
//
// Proves that interrupt-driven serial input is live by reading the
// hardware IRQ counter from the kernel's SURF_GET_INFO response.
// Each keystroke increments the counter inside trap_serial() — this
// command makes the invisible ISR visible.
//
// Host builds (no JASH_TARGET_BEYOND): the builtin exists but refuses,
// same doctrine as the other Beyond-only builtins — the refusal IS the
// documented behavior, not a stub (see main.c surface-detect pattern).

#include "builtin.h"
#include <string.h>
#include <stdio.h>

#ifdef JASH_TARGET_BEYOND
/* Mirror of the kernel's surface_info_t (LiveFS syscall/syscall.h, v2).
 * Kept local on purpose — same pattern as main.c's jash_surface_info_t:
 * jash does not include kernel headers, it speaks the wire layout.
 * irq_count sits at offset 24 (4+4+4+4+2+2+4). */
typedef struct {
    unsigned int       magic;         /* 'SURF' = 0x53555246 */
    unsigned int       version;       /* 2 */
    int                surf_id;
    unsigned int       capabilities;  /* SURF_CAP_* bitmask */
    unsigned short     width, height;
    unsigned int       cell_types;
    unsigned long long irq_count;     /* trap_serial() fire counter */
} jash_irq_surface_info_t;

#define JASH_SURF_GET_INFO_IOCTL 0x5301
#define JASH_SURF_MAGIC_VAL      0x53555246 /* 'SURF' */

/* blibc exposes bear_ioctl with variadic args (routes via syscall 0x67). */
extern int bear_ioctl(int fd, unsigned long request, ...);
#endif

int jash_b_irq(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    (void)args;

#ifndef JASH_TARGET_BEYOND
    static const char msg[] =
        "irq: not running on a Beyond surface (SURF_GET_INFO unavailable)\n";
    w(ctx, msg, (unsigned)(sizeof(msg) - 1));
    return 1;
#else
    jash_irq_surface_info_t info;
    memset(&info, 0, sizeof(info));
    if (bear_ioctl(1, (unsigned long)JASH_SURF_GET_INFO_IOCTL, &info) != 0 ||
        info.magic != JASH_SURF_MAGIC_VAL) {
        static const char msg[] =
            "irq: not running on a Beyond surface (SURF_GET_INFO failed)\n";
        w(ctx, msg, (unsigned)(sizeof(msg) - 1));
        return 1;
    }

    char buf[256];
    int n;

    n = snprintf(buf, sizeof(buf),
        "\n"
        " ── Serial IRQ ──────────────────────────────\n"
        "   trap_serial fires:  %llu\n"
        "   surface capabilities:", (unsigned long long)info.irq_count);

    if (n > 0 && n < (int)sizeof(buf)) w(ctx, buf, (unsigned)n);

    n = snprintf(buf, sizeof(buf), "%s%s%s%s%s\n",
        (info.capabilities & 0x01) ? " READ" : "",
        (info.capabilities & 0x02) ? " WRITE" : "",
        (info.capabilities & 0x04) ? " QUERY" : "",
        (info.capabilities & 0x08) ? " CTRL" : "",
        (info.capabilities & 0x10) ? " FB" : "");
    if (n > 0 && n < (int)sizeof(buf)) w(ctx, buf, (unsigned)n);

    n = snprintf(buf, sizeof(buf),
        "   framebuffer display: %s\n"
        "   surface id:          %d\n"
        "   version:             %u\n"
        "\n"
        " Every keystroke increments the ISR counter above.\n"
        " Type more and run `irq` again — the count will grow.\n"
        "\n",
        (info.capabilities & 0x10) ? "YES (pixels live)" : "no (serial-only)",
        info.surf_id,
        info.version);
    if (n > 0 && n < (int)sizeof(buf)) w(ctx, buf, (unsigned)n);

    return 0;
#endif
}
