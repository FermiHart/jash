// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Sigil Defender: an idle easter egg where Bear protects the hash chain.

#include "bear_egg.h"
#include <core/env.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#ifdef JASH_TINALINUX
#include <osif/tina/surface.h>
#endif

#define DEFENDER_MIN_COLS 32
#define DEFENDER_MAX_LANE 72
#define DEFENDER_HEIGHT 8
#define DEFENDER_LANES 3
#define BEAR_WIDTH 7
#define LEVEL_SEALS 4

#define C_RESET   "\033[0m"
#define C_DIM     "\033[90m"
#define C_CYAN    "\033[1;36m"
#define C_YELLOW  "\033[1;33m"
#define C_GREEN   "\033[1;32m"
#define C_RED     "\033[1;31m"
#define C_MAGENTA "\033[1;35m"
#define C_BLUE    "\033[1;34m"
#define C_WHITE   "\033[1;37m"

static int streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int parse_int(const char* s, int fallback) {
    if (!s || !*s) return fallback;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v > 0 ? v : fallback;
}

static int use_ansi(jash_state_t* s) {
    const char* term = jash_env_get(s, "TERM");
    return !(term && streq(term, "dumb"));
}

static int use_color(jash_state_t* s) {
    return use_ansi(s) && !jash_env_get(s, "NO_COLOR");
}

int jash_bear_egg_enabled(jash_state_t* s) {
    const char* env = jash_env_get(s, "JASH_BEAR_EGG");
    return !(env && (streq(env, "0") || streq(env, "off")));
}

long jash_bear_egg_idle_ms(jash_state_t* s) {
    return (long)parse_int(jash_env_get(s, "JASH_BEAR_IDLE_SEC"), 45) * 1000L;
}

void jash_bear_egg_render_ghost(jash_state_t* s, jash_writer_t w, void* ctx) {
    if (!w || !jash_bear_egg_enabled(s)) return;
    const char* cyan = use_color(s) ? C_CYAN : "";
    const char* yellow = use_color(s) ? C_YELLOW : "";
    const char* reset = use_color(s) ? C_RESET : "";

    jash_writes(w, ctx, "\r\n");
    jash_writes(w, ctx, yellow);
    jash_writes(w, ctx, "   [BEAR]");
    jash_writes(w, ctx, cyan);
    jash_writes(w, ctx, "  *-- sigil grid online\r\n");
    jash_writes(w, ctx, "   Space starts Sigil Defender\r\n");
    jash_writes(w, ctx, "   Enter/Esc dismisses");
    jash_writes(w, ctx, reset);
    jash_writes(w, ctx, "\r\n");
}

typedef struct {
    const char* label;
    int points;
    const char* color;
} threat_t;

static const threat_t THREATS[] = {
    {"<bad hash>", 1, C_RED},
    {"<fork()>", 2, C_MAGENTA},
    {"<malformed>", 2, C_YELLOW},
    {"<rm -rf />", 3, C_RED},
    {"<OOM>", 2, C_MAGENTA},
    {"<404>", 1, C_BLUE},
    {"<rootkit>", 4, C_RED},
    {"<race>", 3, C_YELLOW},
};
#define THREAT_COUNT ((int)(sizeof(THREATS) / sizeof(THREATS[0])))

typedef struct {
    int score;
    int combo;
    int sealed;
    int links;
    int level;
    int aim_lane;
    int shot_lane;
    int shot_pos;
    int threat_lane;
    int threat_pos;
    int threat_idx;
    int threat_hp;
    int threat_age;
    int jump_cooldown;
    int phase_cooldown;
    int phase_frames;
    int tick;
    int fire_frames;
    int hit_frames;
    int armor_frames;
    int jump_frames;
    int aim_frames;
    int level_frames;
    int phase_visible;
    int game_over;
} defender_state_t;

typedef struct {
    int rows;
    int cols;
    int lane_width;
    int start_row;
    int ansi;
    int color;
} defender_view_t;

static void terminal_view(jash_state_t* s, defender_view_t* view) {
    struct winsize ws;
    view->rows = 24;
    view->cols = 80;
#ifdef JASH_TINALINUX
    if (jash_tina_surface_native_output()) {
        unsigned columns = 0, rows = 0;
        if (jash_tina_surface_viewport(&columns, &rows) == 0) {
            view->rows = (int)rows;
            view->cols = (int)columns;
        }
    } else
#endif
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) view->rows = ws.ws_row;
        if (ws.ws_col > 0) view->cols = ws.ws_col;
    }
    view->ansi = use_ansi(s) && view->rows >= DEFENDER_HEIGHT &&
                 view->cols >= DEFENDER_MIN_COLS;
    view->color = view->ansi && use_color(s);
    view->lane_width = view->cols - 2;
    if (view->lane_width < 1) view->lane_width = 1;
    if (view->lane_width > DEFENDER_MAX_LANE)
        view->lane_width = DEFENDER_MAX_LANE;
    view->start_row = view->rows > DEFENDER_HEIGHT
        ? (view->rows - DEFENDER_HEIGHT) / 2 + 1 : 1;
    if (view->start_row + DEFENDER_HEIGHT - 1 > view->rows)
        view->start_row = view->rows - DEFENDER_HEIGHT + 1;
    if (view->start_row < 1) view->start_row = 1;
}

static const char* level_name(int level) {
    if (level == 1) return "INTEGRITY";
    if (level == 2) return "JUMPERS";
    if (level == 3) return "PHASE SHIFT";
    if (level == 4) return "OVERCLOCK";
    if (level == 5) return "ARMORED";
    return "CHAOS";
}

static const char* lane_name(int lane) {
    static const char* names[DEFENDER_LANES] = {"upper", "middle", "lower"};
    if (lane < 0 || lane >= DEFENDER_LANES) return "?";
    return names[lane];
}

static const char* paint(const defender_view_t* view, const char* color) {
    return view->color ? color : "";
}

static void put_text(char* line, int width, int pos, const char* text) {
    if (pos < 0) {
        text -= pos;
        pos = 0;
    }
    for (int i = 0; text[i] && pos + i < width; i++)
        line[pos + i] = text[i];
}

static void cursor_to(jash_writer_t w, void* ctx, int row, int col) {
    char control[40];
    int n = snprintf(control, sizeof control, "\033[%d;%dH", row, col);
    w(ctx, control, (size_t)n);
}

static void write_row(jash_writer_t w, void* ctx, const defender_view_t* view,
                      int row, const char* color, const char* text) {
    cursor_to(w, ctx, row, 1);
    jash_writes(w, ctx, "\033[2K");
    jash_writes(w, ctx, paint(view, color));
    size_t len = strlen(text);
    if (len > (size_t)view->cols) len = (size_t)view->cols;
    w(ctx, text, len);
    jash_writes(w, ctx, paint(view, C_RESET));
}

static void write_at(jash_writer_t w, void* ctx, const defender_view_t* view,
                     int row, int col, const char* color, const char* text) {
    if (col < 1 || col > view->cols) return;
    cursor_to(w, ctx, row, col);
    jash_writes(w, ctx, paint(view, color));
    size_t len = strlen(text);
    size_t room = (size_t)(view->cols - col + 1);
    if (len > room) len = room;
    w(ctx, text, len);
    jash_writes(w, ctx, paint(view, C_RESET));
}

static void spawn_threat(defender_state_t* game, const defender_view_t* view) {
    game->threat_idx = (game->sealed + game->level - 1) % THREAT_COUNT;
    game->threat_lane = (game->sealed * 2 + game->level) % DEFENDER_LANES;
    game->threat_hp = 1;
    if (game->level >= 5 && (game->sealed + 1) % 3 == 0)
        game->threat_hp = 2;
    if (game->level >= 6 && (game->sealed + 1) % 10 == 0)
        game->threat_hp = 3;
    game->threat_age = 0;
    game->phase_visible = 1;
    game->phase_frames = 0;
    game->phase_cooldown = -1;
    game->jump_cooldown = game->level >= 2
        ? 6 + (game->sealed + game->threat_idx) % 5 : -1;
    /* Only some threats phase, once each. The staggered cooldown makes the
     * disappearance surprising without turning it into a constant blink. */
    if (game->level >= 3 &&
        (game->sealed + game->threat_idx) % 3 == 0)
        game->phase_cooldown = 9 +
            (game->sealed * 5 + game->threat_idx) % 8;
    int width = (int)strlen(THREATS[game->threat_idx].label);
    game->threat_pos = view->lane_width - width;
    if (game->threat_pos <= BEAR_WIDTH + 4)
        game->threat_pos = BEAR_WIDTH + 4;
}

static void reset_game(defender_state_t* game, const defender_view_t* view) {
    memset(game, 0, sizeof *game);
    game->links = 3;
    game->level = 1;
    game->aim_lane = 1;
    game->shot_lane = 1;
    game->shot_pos = -1;
    spawn_threat(game, view);
}

static void aim(defender_state_t* game, int delta) {
    game->aim_lane += delta;
    if (game->aim_lane < 0) game->aim_lane = 0;
    if (game->aim_lane >= DEFENDER_LANES) game->aim_lane = DEFENDER_LANES - 1;
    game->aim_frames = 5;
}

static void fire_sigil(defender_state_t* game) {
    game->fire_frames = 5;
    if (game->shot_pos < 0) {
        game->shot_pos = BEAR_WIDTH;
        game->shot_lane = game->aim_lane;
    }
}

static void seal_or_crack(defender_state_t* game, const defender_view_t* view) {
    game->shot_pos = -1;
    game->threat_hp--;
    game->hit_frames = 7;
    if (game->threat_hp > 0) {
        game->armor_frames = 7;
        return;
    }

    const threat_t* threat = &THREATS[game->threat_idx];
    int old_level = game->level;
    game->combo++;
    game->score += threat->points * game->combo * 10 * game->level;
    game->sealed++;
    game->level = 1 + game->sealed / LEVEL_SEALS;
    if (game->level > 9) game->level = 9;
    if (game->level > old_level) game->level_frames = 14;
    spawn_threat(game, view);
}

static void advance_game(defender_state_t* game, const defender_view_t* view) {
    if (game->game_over) return;

    game->tick++;
    game->threat_age++;

    game->phase_visible = 1;
    if (game->phase_frames > 0) {
        game->phase_visible = 0;
        game->phase_frames--;
        if (game->phase_frames == 0 && game->jump_cooldown < 4)
            game->jump_cooldown = 4;
    } else if (game->phase_cooldown > 0) {
        game->phase_cooldown--;
        if (game->phase_cooldown == 0 &&
            game->threat_pos > BEAR_WIDTH + 8) {
            /* This frame is already invisible; two remaining frames make a
             * short three-frame surprise instead of a recurring blackout. */
            game->phase_frames = 2;
            game->phase_cooldown = -1;
            game->phase_visible = 0;
        }
    }

    /* Jump time advances only while visible. A phased threat therefore always
     * returns on the same lane and remains targetable before its next jump. */
    if (game->phase_visible && game->level >= 2 && game->jump_cooldown > 0) {
        game->jump_cooldown--;
        if (game->jump_cooldown == 0) {
            int direction = ((game->sealed + game->threat_age) & 1) ? 1 : 2;
            game->threat_lane = (game->threat_lane + direction) % DEFENDER_LANES;
            game->jump_frames = 6;
            int delay = 9 - game->level;
            game->jump_cooldown = delay < 4 ? 4 : delay;
        }
    }

    if (game->shot_pos >= 0) game->shot_pos += game->level >= 6 ? 4 : 3;
    int step = 1;
    if (game->level >= 4 && game->tick % 2 == 0) step++;
    if (game->level >= 7) step++;
    game->threat_pos -= step;

    if (game->shot_pos >= 0 && game->shot_lane == game->threat_lane &&
        game->phase_visible && game->shot_pos >= game->threat_pos) {
        seal_or_crack(game, view);
    } else {
        if (game->shot_pos >= view->lane_width) game->shot_pos = -1;
        if (game->threat_pos <= BEAR_WIDTH - 1 && game->phase_visible) {
            game->links--;
            game->combo = 0;
            game->shot_pos = -1;
            game->hit_frames = 0;
            if (game->links <= 0)
                game->game_over = 1;
            else
                spawn_threat(game, view);
        }
    }

    if (game->fire_frames > 0) game->fire_frames--;
    if (game->hit_frames > 0) game->hit_frames--;
    if (game->armor_frames > 0) game->armor_frames--;
    if (game->jump_frames > 0) game->jump_frames--;
    if (game->aim_frames > 0) game->aim_frames--;
    if (game->level_frames > 0) game->level_frames--;
}

static void build_track(char* line, int width, int active) {
    memset(line, ' ', (size_t)width);
    line[width] = '\0';
    put_text(line, width, 0, active ? "[BEAR]>" : "      |");
    for (int i = BEAR_WIDTH; i < width; i++)
        line[i] = (i % 4 == 0) ? ':' : '-';
}

static const char* message_color(const defender_state_t* game) {
    if (game->game_over) return C_RED;
    if (game->level_frames > 0) return C_MAGENTA;
    if (!game->phase_visible) return C_BLUE;
    if (game->armor_frames > 0) return C_YELLOW;
    if (game->hit_frames > 0) return C_GREEN;
    if (game->jump_frames > 0) return C_MAGENTA;
    return C_CYAN;
}

static void game_message(char* message, size_t cap, const defender_state_t* game) {
    if (game->game_over) {
        snprintf(message, cap, "CHAIN BROKEN | Space restarts | Enter/Esc exits");
    } else if (game->level_frames > 0) {
        snprintf(message, cap, "LEVEL %02d // %s", game->level,
                 level_name(game->level));
    } else if (!game->phase_visible) {
        snprintf(message, cap, "PHASE SHIFT // target vanished");
    } else if (game->armor_frames > 0) {
        snprintf(message, cap, "ARMOR CRACKED // %d layer%s left", game->threat_hp,
                 game->threat_hp == 1 ? "" : "s");
    } else if (game->hit_frames > 0) {
        snprintf(message, cap, "LINK SEALED // combo x%d", game->combo);
    } else if (game->jump_frames > 0) {
        snprintf(message, cap, "TARGET JUMPED // reacquire %s lane",
                 lane_name(game->threat_lane));
    } else if (game->aim_frames > 0) {
        snprintf(message, cap, "AIM %s", lane_name(game->aim_lane));
    } else if (game->fire_frames > 0) {
        snprintf(message, cap, "SIGIL FIRED  *");
    } else {
        snprintf(message, cap, "LOCK %s // threat armor:%d",
                 lane_name(game->threat_lane), game->threat_hp);
    }
}

static void draw_frame(jash_writer_t w, void* ctx, const defender_view_t* view,
                       const defender_state_t* game) {
    char title[96];
    char track[DEFENDER_MAX_LANE + 1];
    char chain[96];
    char status[96];
    char message[128];

    snprintf(title, sizeof title, "[ SIGIL DEFENDER ]  LEVEL %02d  %s",
             game->level, level_name(game->level));
    write_row(w, ctx, view, view->start_row, C_CYAN, title);

    for (int lane = 0; lane < DEFENDER_LANES; lane++) {
        int row = view->start_row + 1 + lane;
        build_track(track, view->lane_width, lane == game->aim_lane);
        write_row(w, ctx, view, row, C_DIM, track);
        if (lane == game->aim_lane)
            write_at(w, ctx, view, row, 1, C_YELLOW, "[BEAR]>");
        if (game->shot_pos >= 0 && lane == game->shot_lane)
            write_at(w, ctx, view, row, game->shot_pos + 1, C_CYAN, "*");
        if (!game->game_over && game->phase_visible && lane == game->threat_lane)
            write_at(w, ctx, view, row, game->threat_pos + 1,
                     THREATS[game->threat_idx].color,
                     THREATS[game->threat_idx].label);
    }

    int chain_width = view->lane_width - 18;
    if (chain_width < 6) chain_width = 6;
    if (chain_width > 24) chain_width = 24;
    int filled = game->links * chain_width / 3;
    int pos = snprintf(chain, sizeof chain, "CHAIN [");
    for (int i = 0; i < chain_width && pos + 2 < (int)sizeof chain; i++)
        chain[pos++] = i < filled ? '=' : '.';
    snprintf(chain + pos, sizeof chain - (size_t)pos, "] links:%d", game->links);
    const char* chain_color = game->links >= 3 ? C_GREEN :
                              game->links == 2 ? C_YELLOW : C_RED;
    write_row(w, ctx, view, view->start_row + 4, chain_color, chain);

    snprintf(status, sizeof status,
             "SCORE %06d  COMBO x%d  SEALED %d  TARGET HP %d",
             game->score, game->combo, game->sealed, game->threat_hp);
    write_row(w, ctx, view, view->start_row + 5, C_MAGENTA, status);

    game_message(message, sizeof message, game);
    write_row(w, ctx, view, view->start_row + 6, message_color(game), message);
    write_row(w, ctx, view, view->start_row + 7, C_WHITE,
              "W/S or arrows aim | Space fires | Enter/Esc exits");
}

static void draw_plain_frame(jash_writer_t w, void* ctx, const defender_view_t* view,
                             const defender_state_t* game) {
    char line[160];
    snprintf(line, sizeof line,
             "\rL%d %s | aim:%s target:%s/%s hp:%d score:%d",
             game->level, level_name(game->level), lane_name(game->aim_lane),
             lane_name(game->threat_lane),
             game->phase_visible ? "visible" : "phase", game->threat_hp,
             game->score);
    size_t len = strlen(line);
    if (len > (size_t)view->cols) len = (size_t)view->cols;
    w(ctx, line, len);
}

static int read_arrow(jash_reader_t r, void* rctx, defender_state_t* game) {
    char bracket = 0;
    char direction = 0;
    if (r(rctx, &bracket) != 1 || bracket != '[') return 0;
    if (r(rctx, &direction) != 1) return 0;
    if (direction == 'A') { aim(game, -1); return 1; }
    if (direction == 'B') { aim(game, 1); return 1; }
    return 0;
}

void jash_bear_egg_run(jash_state_t* s, jash_reader_t r, void* rctx,
                       jash_writer_t w, void* wctx) {
    if (!w || !r || !jash_bear_egg_enabled(s)) return;

    defender_view_t view;
    terminal_view(s, &view);
    defender_state_t game;
    reset_game(&game, &view);

    if (view.ansi) {
        jash_writes(w, wctx, "\033[?1049h\033[?25l\033[2J");
        draw_frame(w, wctx, &view, &game);
    } else {
        jash_writes(w, wctx,
                    "\r\nSIGIL DEFENDER | W/S aim | Space fires | Enter/Esc exits\r\n");
        draw_plain_frame(w, wctx, &view, &game);
    }

    for (int frame = 0; frame < 3000; frame++) {
        for (int tries = 0; tries < 8; tries++) {
            char b = 0;
            int rc = r(rctx, &b);
            if (rc != 1) break;
            if (b == ' ') {
                if (game.game_over)
                    reset_game(&game, &view);
                else
                    fire_sigil(&game);
            } else if (b == 'w' || b == 'W') {
                aim(&game, -1);
            } else if (b == 's' || b == 'S') {
                aim(&game, 1);
            } else if (b == 27) {
                if (!read_arrow(r, rctx, &game)) goto game_exit;
            } else if (b == '\r' || b == '\n') {
                goto game_exit;
            }
        }

        advance_game(&game, &view);
        if (view.ansi)
            draw_frame(w, wctx, &view, &game);
        else
            draw_plain_frame(w, wctx, &view, &game);
        usleep(20000);
    }

game_exit:
    if (view.ansi)
        jash_writes(w, wctx, "\033[0m\033[?25h\033[?1049l");
    else
        jash_writes(w, wctx, "\r\n");
}
