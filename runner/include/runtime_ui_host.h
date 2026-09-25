/*
 * runtime_ui_host.h -- adapter between the NES runner and recomp-ui's in-game menu (recomp_runtime_ui.h).
 *
 * recomp-ui owns the menu model and its ImGui presentation; the host (this runner) owns the event loop, live
 * application of settings, persistence and pause policy (docs/RUNTIME_UI.md in recomp-ui). Opt-in: build with
 * NESRECOMP_ENABLE_RUNTIME_UI=ON (defines NESRECOMP_RUNTIME_UI). Without it the runner behaves exactly as before.
 *
 * Policy: Escape opens the menu (F1..F12 stay the save-state slots) (Escape no longer quits the runner; "Exit" is a menu action), the
 * simulation is paused while it is open (the menu runs a nested loop inside the VBlank callback), gamepad
 * Guide opens it and D-pad/A/B navigate it.
 */
#pragma once
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NesRuntimeUiHost {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    /* Redraw the last presented game frame into the renderer's back buffer (no present). */
    void (*draw_game)(void);
    /* Re-apply display settings held in g_nes_config (scale quality, integer scale, window size, fullscreen). */
    void (*apply_display)(void);
    void (*save_state)(int slot);
    void (*load_state)(int slot);
    const char *title;      /* e.g. the game name */
    const char *subtitle;   /* e.g. "NES" */
} NesRuntimeUiHost;

/* Returns 1 on success. On failure the runner keeps its stock behaviour (Escape quits). */
int nes_runtime_ui_init(const NesRuntimeUiHost *host);
/* Feed every SDL event before the runner's own handling. Returns 1 if the event was consumed. May block in the
 * modal menu loop until the menu closes. */
int nes_runtime_ui_event(const SDL_Event *ev);
int nes_runtime_ui_active(void);   /* 1 once nes_runtime_ui_init succeeded */
void nes_runtime_ui_shutdown(void);

#ifdef __cplusplus
}
#endif
