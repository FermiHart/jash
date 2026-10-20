// Regression test: phase-shift must not teleport a threat between lanes or
// repeat continuously. Include the implementation to exercise its pure state
// transition helpers without exposing game internals in the production API.
#include "../c/easter/bear_egg.c"

const char* jash_env_get(const jash_state_t* state, const char* key) {
    (void)state;
    (void)key;
    return NULL;
}

long jash_writes(jash_writer_t writer, void* ctx, const char* text) {
    return writer ? writer(ctx, text, strlen(text)) : -1;
}

int main(void) {
    defender_view_t view = {
        .rows = 24, .cols = 80, .lane_width = 64,
        .start_row = 8, .ansi = 1, .color = 1,
    };
    defender_state_t game;
    reset_game(&game, &view);
    game.level = 3;
    game.threat_lane = 2;
    game.threat_pos = 1000;

    /* Force phase and jump to become due together. Phase wins, and neither the
     * invisible frames nor the first visible frames may move the lane. */
    game.phase_cooldown = 1;
    game.jump_cooldown = 1;
    int original_lane = game.threat_lane;
    advance_game(&game, &view);
    if (game.phase_visible || game.threat_lane != original_lane) return 1;

    int invisible_runs = 1;
    int was_visible = 0;
    int visible_grace = 0;
    for (int i = 0; i < 80; i++) {
        advance_game(&game, &view);
        if (!game.phase_visible && was_visible) invisible_runs++;
        if (game.phase_visible && !was_visible) visible_grace = 3;
        if (visible_grace > 0) {
            if (game.threat_lane != original_lane) return 2;
            visible_grace--;
        }
        was_visible = game.phase_visible;
    }
    if (invisible_runs != 1) return 3;
    return 0;
}
