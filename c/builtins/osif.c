/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   builtins/osif.c — `osif` builtin: capabilities, conformance, port report
 *
 *   The user-visible side of OSIF 2.0 (ADR-0017). Three subcommands:
 *
 *     osif caps     summary line + cap bitmap
 *     osif check    runs every probe, prints PASS/FAIL/SKIP per provider
 *     osif report   writes Docs/ports/<target>-conformance.md
 *
 *   This builtin is also the ENGINE for `qa osif`: same probes,
 *   structured output for the QA aggregator.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "builtin.h"
#include "discovery_output.h"
#include "osif_port.h"
#include <core/query_cancel.h>
#include <core/argv.h>
#include "osif/osinfo.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* Probe result codes. */
#define PROBE_PASS  0
#define PROBE_FAIL  1
#define PROBE_SKIP  2  /* capability not advertised — honest, not a failure */

typedef struct {
    const char* cap_name;
    unsigned    cap_bit;
    const char* probe_name;
    int       (*run)(char* msg, size_t msz);
} probe_t;

/* ── Individual probes ───────────────────────────────────────────────────── */

static int probe_sysid(char* msg, size_t msz) {
    osif_sysid_t s;
    if (bear_osinfo_sysid(&s) != 0) {
        snprintf(msg, msz, "bear_osinfo_sysid() failed");
        return PROBE_FAIL;
    }
    if (!s.sysname[0]) {
        snprintf(msg, msz, "sysname empty");
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "%s %s %s", s.sysname, s.release, s.machine);
    return PROBE_PASS;
}

static int counted_pids = 0;
static int self_seen   = 0;
static int proc_cb_count(const osif_procinfo_t* info, void* user) {
    (void)user;
    counted_pids++;
    if (info->pid == getpid()) self_seen = 1;
    return 0;
}

static int probe_proc(char* msg, size_t msz) {
    /* Self lookup must work and our own comm must be non-empty. */
    osif_procinfo_t me;
    if (bear_osinfo_proc_get(getpid(), &me) != 0) {
        snprintf(msg, msz, "proc_get(self) failed");
        return PROBE_FAIL;
    }
    if (!me.comm[0]) {
        snprintf(msg, msz, "self comm empty");
        return PROBE_FAIL;
    }
    /* Iterator must visit at least pid 1 and ourselves. */
    counted_pids = 0; self_seen = 0;
    if (bear_osinfo_proc_iter(proc_cb_count, NULL) != 0) {
        snprintf(msg, msz, "proc_iter failed");
        return PROBE_FAIL;
    }
    if (!self_seen) {
        snprintf(msg, msz, "iter did not include self (%d procs seen)",
                 counted_pids);
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "%d procs visible, self=%s rss=%luK",
             counted_pids, me.comm, me.rss_kb);
    return PROBE_PASS;
}

static int probe_meminfo(char* msg, size_t msz) {
    osif_meminfo_t m;
    if (bear_osinfo_meminfo(&m) != 0) {
        snprintf(msg, msz, "meminfo failed");
        return PROBE_FAIL;
    }
    if (m.total_kb == 0) {
        snprintf(msg, msz, "total=0 — suspect");
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "%lu MB total, %lu MB available",
             m.total_kb / 1024, m.available_kb / 1024);
    return PROBE_PASS;
}

static int mount_root_seen = 0;
static int mount_cb_count(const osif_mount_t* mnt, void* user) {
    int* n = (int*)user;
    (*n)++;
    if (strcmp(mnt->mountpoint, "/") == 0) mount_root_seen = 1;
    return 0;
}

static int probe_mounts(char* msg, size_t msz) {
    int n = 0;
    mount_root_seen = 0;
    if (bear_osinfo_mounts(mount_cb_count, &n) != 0) {
        snprintf(msg, msz, "mounts iter failed");
        return PROBE_FAIL;
    }
    if (!mount_root_seen) {
        snprintf(msg, msz, "root '/' not visible (%d mounts)", n);
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "%d real mounts, root visible", n);
    return PROBE_PASS;
}

static int probe_netstat(char* msg, size_t msz) {
    osif_netstat_t ns;
    if (bear_osinfo_netstat(&ns) != 0) {
        snprintf(msg, msz, "netstat failed");
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "rx=%llu tx=%llu bytes",
             (unsigned long long)ns.rx_bytes,
             (unsigned long long)ns.tx_bytes);
    return PROBE_PASS;
}

static int probe_uptime(char* msg, size_t msz) {
    unsigned long s = 0;
    if (bear_osinfo_uptime(&s) != 0) {
        snprintf(msg, msz, "uptime failed");
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "%lu seconds", s);
    return PROBE_PASS;
}

static int probe_loadavg(char* msg, size_t msz) {
    double l[3];
    if (bear_osinfo_loadavg(l) != 0) {
        snprintf(msg, msz, "loadavg failed");
        return PROBE_FAIL;
    }
#ifdef JASH_BLIBC
    /* blibc's snprintf has no floating-point formatter. */
    unsigned long centi[3];
    for (int i = 0; i < 3; i++) {
        if (l[i] < 0 || l[i] > 1000000) return PROBE_FAIL;
        centi[i] = (unsigned long)(l[i] * 100.0 + 0.5);
    }
    snprintf(msg, msz, "%lu.%02lu %lu.%02lu %lu.%02lu",
             centi[0] / 100, centi[0] % 100,
             centi[1] / 100, centi[1] % 100,
             centi[2] / 100, centi[2] % 100);
#else
    snprintf(msg, msz, "%.2f %.2f %.2f", l[0], l[1], l[2]);
#endif
    return PROBE_PASS;
}

static int probe_spawn(char* msg, size_t msz) {
    const char* argv[] = { "/bin/echo", "osif_spawn_probe", NULL };
    char out[128] = "";
    int ec = -1;
    /* 10s hard timeout: under TCG emulation a cold-cache fork+exec of
     * /bin/echo page-faults its way past 2s. Same convention as QA. */
    int rc = bear_osinfo_spawn_capture(argv, NULL, out, sizeof(out), 10000, &ec);
    if (rc != 0 || ec != 0) {
        snprintf(msg, msz, "spawn rc=%d ec=%d", rc, ec);
        return PROBE_FAIL;
    }
    if (!strstr(out, "osif_spawn_probe")) {
        snprintf(msg, msz, "spawn captured wrong output");
        return PROBE_FAIL;
    }
    snprintf(msg, msz, "echo round-trip OK (%d bytes)", (int)strlen(out));
    return PROBE_PASS;
}

/* The contract: spec line + probe per provider. Kept in declaration order
 * so the conformance report reads top-to-bottom like the spec file. */
static const probe_t k_probes[] = {
    { "SYSID",   OSIF_CAP_SYSID,   "sysid",     probe_sysid   },
    { "PROC",    OSIF_CAP_PROC,    "proc_iter", probe_proc    },
    { "MEM",     OSIF_CAP_MEM,     "meminfo",   probe_meminfo },
    { "MOUNTS",  OSIF_CAP_MOUNTS,  "mounts",    probe_mounts  },
    { "NET",     OSIF_CAP_NET,     "netstat",   probe_netstat },
    { "UPTIME",  OSIF_CAP_UPTIME,  "uptime",    probe_uptime  },
    { "LOADAVG", OSIF_CAP_LOADAVG, "loadavg",   probe_loadavg },
    { "SPAWN",   OSIF_CAP_SPAWN,   "spawn",     probe_spawn   },
};
/* ICE by construction (sizeof ratio): usable as an array bound.
 * INV-880: the canonical report buffer counts probes statically. */
enum { k_nprobes = sizeof(k_probes) / sizeof(k_probes[0]) };

typedef struct {
    unsigned caps;
    int codes[k_nprobes];
    char details[k_nprobes][256];
    int passes, failures, skips;
} osif_probe_snapshot_t;

static void osif_probe_snapshot(osif_probe_snapshot_t* result, int probing) {
    memset(result, 0, sizeof *result); result->caps = bear_osinfo_caps();
    for (int i = 0; i < k_nprobes; i++) {
        int advertised = (result->caps & k_probes[i].cap_bit) != 0;
        int code = !advertised ? PROBE_SKIP : probing ?
            k_probes[i].run(result->details[i], sizeof result->details[i]) : PROBE_PASS;
        if (!advertised) (void)snprintf(result->details[i], sizeof result->details[i], "capability not advertised");
        result->codes[i] = code;
        if (code == PROBE_PASS) result->passes++;
        else if (code == PROBE_SKIP) result->skips++;
        else result->failures++;
    }
}

static const struct { const char* cap; const char* doc; } osif_lessons[] = {
    { "sysid",
      "  Contract: fill 5×65-char struct (sysname/nodename/release/version/machine).\r\n"
      "  Must never block; called many times per prompt by uname & banner.\r\n"
      "  Linux/macOS reference: uname(2). MINIX: bear_uname() (blibc).\r\n"
      "  ENOSYS fallback consumer: builtin uname prints OSIF_TARGET only.\r\n" },
    { "proc",
      "  Contract: proc_get(pid) and proc_iter(cb).\r\n"
      "  state MUST be normalized to Linux letters: R S D Z T I '?'.\r\n"
      "  rss_kb/vsz_kb in KiB, NOT pages — call sysconf(_SC_PAGESIZE).\r\n"
      "  Linux: parse /proc/[pid]/stat (mind the comm-with-parens trap).\r\n"
      "  macOS: proc_pidinfo + libproc.h. BSD: sysctl KERN_PROC2.\r\n" },
    { "mem",
      "  Contract: total_kb is physical RAM. available_kb is what's\r\n"
      "  reclaimable for a new process (Linux MemAvailable, Darwin\r\n"
      "  vm.free_count + vm.inactive_count). free_kb is strict free.\r\n" },
    { "mounts",
      "  Contract: callback receives only REAL filesystems — pseudo FSes\r\n"
      "  (proc/sysfs/tmpfs/cgroup...) MUST be filtered by the backend.\r\n"
      "  total/free/avail in BYTES, not blocks.\r\n" },
    { "net",
      "  Contract: sum of rx/tx bytes across PHYSICAL interfaces only.\r\n"
      "  Exclude lo/lo0/docker*/br-*/veth*/utun*/awdl* in the backend —\r\n"
      "  consumers expect a usable wire-traffic number.\r\n" },
    { "uptime",
      "  Contract: monotonic seconds since boot.\r\n"
      "  Linux: sysinfo().uptime. macOS: time(NULL) - kern.boottime.\r\n" },
    { "loadavg",
      "  Contract: 1/5/15-minute averages. POSIX has getloadavg(3).\r\n"
      "  Targets without scheduler stats: return ENOSYS (statusbar hides).\r\n" },
    { "spawn",
      "  Contract: execv-based; NEVER /bin/sh. timeout enforced with\r\n"
      "  SIGKILL. Stdout+stderr merged into one buffer. Implemented\r\n"
      "  ONCE in posix_common.c — only freestanding targets reimplement.\r\n" },
    { NULL, NULL }
};

/* ── Output helpers ──────────────────────────────────────────────────────── */

static void caps_summary(jash_writer_t w, void* ctx) {
    unsigned caps = bear_osinfo_caps();
    int total = k_nprobes;
    int have  = 0;
    for (int i = 0; i < k_nprobes; i++)
        if (caps & k_probes[i].cap_bit) have++;
    jash_writef(w, ctx, "OSIF target=\033[1;36m%s\033[0m caps=%d/%d  ",
                bear_osinfo_target(), have, total);
    for (int i = 0; i < k_nprobes; i++) {
        int on = (caps & k_probes[i].cap_bit) != 0;
        jash_writef(w, ctx, "%s%s\033[0m ",
                    on ? "\033[32m+" : "\033[90m-",
                    k_probes[i].cap_name);
    }
    jash_writes(w, ctx, "\r\n");
}

/* Run all probes, fill counts. write=1 prints per-probe lines. */
static void run_all(jash_writer_t w, void* ctx, int verbose,
                    int* pass, int* fail, int* skip) {
    osif_probe_snapshot_t snapshot; osif_probe_snapshot(&snapshot, 1);
    *pass = snapshot.passes; *fail = snapshot.failures; *skip = snapshot.skips;
    for (int i = 0; i < k_nprobes; i++) {
        int res = snapshot.codes[i]; const char* msg = snapshot.details[i];
        if (verbose) {
            const char* tag =
                res == PROBE_PASS ? "\033[32mPASS\033[0m" :
                res == PROBE_FAIL ? "\033[31mFAIL\033[0m" :
                                    "\033[90mSKIP\033[0m";
            jash_writef(w, ctx, "  %s  %-10s %-12s %s\r\n",
                        tag, k_probes[i].cap_name, k_probes[i].probe_name, msg);
        }
    }
}

/* Tier letter: A = perfect, B = >=75% pass, C = >=50%, D otherwise. */
static char tier_letter(int pass, int total) {
    if (total <= 0) return 'D';
    int pct = (pass * 100) / total;
    if (pct == 100) return 'A';
    if (pct >= 75)  return 'B';
    if (pct >= 50)  return 'C';
    return 'D';
}

static void write_score_line(jash_writer_t w, void* ctx,
                             int p, int f, int s) {
    int total = k_nprobes;
    char tier = tier_letter(p, total);
    const char* tier_color =
        tier == 'A' ? "\033[1;32m" :
        tier == 'B' ? "\033[1;33m" :
        tier == 'C' ? "\033[1;35m" : "\033[1;31m";
    jash_writef(w, ctx,
        "Tier %s%c\033[0m — %d/%d pass · %d fail · %d skip · target=%s\r\n",
        tier_color, tier, p, total, f, s, bear_osinfo_target());
}

#include "osif_report.inc"

/* ── Public dispatcher ───────────────────────────────────────────────────── */

static int osif_text(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    const char* p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    if (!*p || strncmp(p, "caps", 4) == 0) {
        caps_summary(w, ctx);
        return 0;
    }

    if (strncmp(p, "check", 5) == 0) {
        caps_summary(w, ctx);
        int pa = 0, fa = 0, sk = 0;
        run_all(w, ctx, 1, &pa, &fa, &sk);
        write_score_line(w, ctx, pa, fa, sk);
        return fa == 0 ? 0 : 1;
    }

    if (strncmp(p, "score", 5) == 0) {
        int pa = 0, fa = 0, sk = 0;
        run_all(w, ctx, 0, &pa, &fa, &sk);
        write_score_line(w, ctx, pa, fa, sk);
        return fa == 0 ? 0 : 1;
    }

    if (strncmp(p, "report", 6) == 0) {
        return write_report(w, ctx);
    }

    if (strncmp(p, "port", 4) == 0) {
        return jash_b_osif_port(s, w, ctx, p);
    }

    if (strncmp(p, "matrix", 6) == 0) {
        /* Aggregate ALL committed port-conformance reports from
         * Docs/ports/<target>-conformance.md into one table.
         * This is the WHOLE PRODUCT in one screen: where jash runs, how well. */
        DIR* d = opendir("Docs/ports");
        if (!d) {
            jash_writes(w, ctx,
                "osif: no port reports yet — run 'osif report' here\r\n"
                "      or `cd <other-port> && jash -c 'osif report'`\r\n");
            return 1;
        }
        jash_writef(w, ctx,
            "\033[1;36m═══ JASH OSIF PORT MATRIX ═══\033[0m\r\n"
            "%-12s %-6s %-10s %s\r\n", "TARGET", "TIER", "PASS/TOTAL", "REPORT");
        struct dirent* ent;
        int found = 0;
        while ((ent = readdir(d)) != NULL) {
            const char* nm = ent->d_name;
            const char* suffix = strstr(nm, "-conformance.md");
            if (!suffix) continue;
            char target_name[64];
            size_t tlen = (size_t)(suffix - nm);
            if (tlen >= sizeof(target_name)) tlen = sizeof(target_name) - 1;
            memcpy(target_name, nm, tlen);
            target_name[tlen] = '\0';

            /* Pull tier + pass/total from the markdown header. */
            char path[256];
            snprintf(path, sizeof(path), "Docs/ports/%s", nm);
            FILE* f = fopen(path, "r");
            char tier_ch = '?';
            int pa = 0, tot = 8;
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "**Tier:** %c — %d/%d", &tier_ch, &pa, &tot) >= 3)
                        break;
                }
                fclose(f);
            }
            const char* col =
                tier_ch == 'A' ? "\033[1;32m" :
                tier_ch == 'B' ? "\033[1;33m" :
                tier_ch == 'C' ? "\033[1;35m" : "\033[1;31m";
            jash_writef(w, ctx, "%-12s %s%c\033[0m      %d/%-8d %s\r\n",
                        target_name, col, tier_ch, pa, tot, path);
            found++;
        }
        closedir(d);
        if (!found) {
            jash_writes(w, ctx, "  (no reports found — none committed yet)\r\n");
        }
        return 0;
    }

    if (strncmp(p, "explain", 7) == 0) {
        /* Pedagogical mode for the porting agent: explain what each
         * provider's contract is, with verified implementations as
         * worked examples. The agent reads this INSTEAD of the
         * Linux/macOS source files — keeps the porting context tight. */
        const char* arg = p + 7;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (!*arg) {
            jash_writes(w, ctx,
                "usage: osif explain <provider>\r\n"
                "  providers: sysid proc mem mounts net uptime loadavg spawn\r\n");
            return 2;
        }
        for (int i = 0; osif_lessons[i].cap; i++) {
            if (strncmp(arg, osif_lessons[i].cap, strlen(osif_lessons[i].cap)) == 0) {
                jash_writef(w, ctx,
                    "\033[1;36m═══ provider: %s ═══\033[0m\r\n%s",
                    osif_lessons[i].cap, osif_lessons[i].doc);
                return 0;
            }
        }
        jash_writef(w, ctx, "osif: unknown provider '%s'\r\n", arg);
        return 2;
    }

    jash_writes(w, ctx,
        "usage: osif {caps|check|score|report|port <newos>|matrix|explain <p>}\r\n"
        "  caps    — capability summary\r\n"
        "  check   — run every probe, verbose PASS/FAIL/SKIP\r\n"
        "  score   — tier letter + counts, machine-friendly\r\n"
        "  report  — write Docs/ports/<target>-conformance.md\r\n"
        "  port    — scaffold c/osif/<newos>/ for a new port (ADR-0017)\r\n"
        "  matrix  — aggregate all committed port reports into one table\r\n"
        "  explain — teach a porting agent the contract of one provider\r\n");
    return 0;
}

/* ── Public C entry for the QA engine and the AI subsystem ──────────────── */

/* Compact, tag-friendly summary used by `? port status` and qa osif.
 * Format: "TARGET=linux TIER=A CAPS=8/8 PASS=8 FAIL=0 SKIP=0"
 * Returns the tier letter. Never prints. */
char jash_osif_summary(char* out, size_t outsz) {
    osif_probe_snapshot_t snapshot; osif_probe_snapshot(&snapshot, 1);
    int pa = snapshot.passes, fa = snapshot.failures, sk = snapshot.skips;
    char tier = tier_letter(pa, k_nprobes);
    if (out && outsz)
        snprintf(out, outsz,
                 "TARGET=%s TIER=%c CAPS=%d/%d PASS=%d FAIL=%d SKIP=%d",
                 bear_osinfo_target(), tier,
                 pa, k_nprobes, pa, fa, sk);
    return tier;
}


#include "osif_native.inc"

int jash_b_osif_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    osif_arguments_t arguments = osif_arguments(argc, argv);
    if (!arguments.selected) return jash_discovery_text(osif_text, state, writer, context, argc, argv);
    if (arguments.error) return jash_discovery_error(writer, context, "osif", arguments.mode, arguments.error, 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "osif", arguments.mode, "allocation_failed", 125);
    const char* operation = arguments.operation; int status;
    if (!strcmp(operation, "report")) status = osif_native_report(writer, context, arguments.mode, &rows);
    else if (!strcmp(operation, "port")) status = osif_native_port(writer, context, arguments.mode, &rows, arguments.operand);
    else if (!strcmp(operation, "explain")) status = osif_native_explain(writer, context, arguments.mode, &rows, arguments.operand);
    else if (!strcmp(operation, "matrix")) status = osif_native_matrix(state, writer, context, arguments.mode, &rows);
    else {
        int probing = strcmp(operation, "caps") != 0;
        osif_probe_snapshot_t snapshot; osif_probe_snapshot(&snapshot, probing);
        for (int i = 0; i < k_nprobes; i++) if (osif_native_probe(&rows, &snapshot, operation, i, 0)) break;
        status = jash_discovery_emit(writer, context, "osif", arguments.mode, &rows, probing && snapshot.failures ? 1 : 0);
    }
    jash_discovery_rows_free(&rows); return status;
}

int jash_b_osif(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_osif_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
