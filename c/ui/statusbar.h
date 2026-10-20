// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ui/statusbar — Living Dashboard with multiple modes

#ifndef JASH_UI_STATUSBAR_H
#define JASH_UI_STATUSBAR_H

#include <core/state.h>
#include <theme/io.h>

/* Frame buffer: holds the COMPOSED full-width frame (SGR escapes included),
 * not just visible columns — sized for wide terminals with dense themes. */
#define JASH_STATUSBAR_MAX_WIDTH  4096
#define JASH_STATUSBAR_MAX_HEIGHT 2

/* Status bar modes */
typedef enum {
    SB_MODE_COMPUTER = 0,  /* CPU, RAM, disk, network, battery, uptime */
    SB_MODE_DEV,           /* Git, language, build, project info */
    SB_MODE_AI,            /* AI agent status, context, learning */
    SB_MODE_COGNITIVE,     /* LLM mesh status, intent, risk (ADR-0022) */
    SB_MODE_FOCUS,         /* Minimal: time + battery only */
    SB_MODE_PERSONAL,      /* Weather, news, stocks (web-configurable) */
    SB_MODE_DEBUG,         /* Env vars, jobs, PIDs, internals */
    SB_MODE_COUNT
} jash_sb_mode_t;

/* Status bar state */
typedef struct {
    int enabled;
    int dirty;
    int rendered;
    uint64_t last_update_ns;
    char cached_line[JASH_STATUSBAR_MAX_WIDTH];
    uint32_t cached_len;
    jash_sb_mode_t mode;           /* Current display mode */
    int auto_switch;               /* 1 = auto-switch based on context */
    char cached_git_branch[64];
    int cached_has_git;
    int cached_git_status;
    uint64_t last_git_probe_ns;
    uint64_t last_net_probe_ns;
    uint64_t last_battery_probe_ns;
} jash_statusbar_t;

/* Section registration for modules (ADR-0022 Cognitive Statusbar) */
#define JASH_STATUSBAR_MAX_SECTIONS 8

/* Forward declare sb_seg_t for callback */
typedef struct sb_seg sb_seg_t;

/* Section callback type — modules register these to add custom status */
typedef void (*jash_statusbar_section_fn)(sb_seg_t* seg, const void* state);

/* Setup/teardown */
void jash_statusbar_setup_terminal(void);
void jash_statusbar_teardown_terminal(void);

/* Init/config */
void jash_statusbar_init(jash_state_t* s);
int  jash_statusbar_enabled(const jash_state_t* s);
int  jash_statusbar_region_active(void);
void jash_statusbar_set_enabled(jash_state_t* s, int enabled);

/* Mode control */
void jash_statusbar_set_mode(jash_sb_mode_t mode);
jash_sb_mode_t jash_statusbar_get_mode(void);
void jash_statusbar_toggle_mode(void);
void jash_statusbar_set_auto_switch(int on);
const char* jash_statusbar_mode_name(jash_sb_mode_t mode);

/* Section registration (for modules to add custom status) */
int jash_statusbar_register_section(jash_statusbar_section_fn fn);
void jash_statusbar_unregister_section(int id);

/* Render */
void jash_statusbar_render(jash_writer_t w, void* ctx, const jash_state_t* s);
void jash_statusbar_mark_dirty(void);
void jash_statusbar_clear(jash_writer_t w, void* ctx);

/* Signals */
int jash_statusbar_sigwinch_pending(void);
int jash_statusbar_sigterm_pending(void);
void jash_statusbar_clear_sigwinch(void);
void jash_statusbar_clear_sigterm(void);

/* AI indicator (when statusbar disabled) */
void render_ai_mode_indicator(void);
/* Sigil indicator (when statusbar disabled) */
void render_sigil_indicator(void);

#endif
