/*
 * runtime_ui_host.cpp -- in-game menu host for the NES runner (see runtime_ui_host.h).
 *
 * Rendering follows recomp-ui docs/RUNTIME_UI.md for SDL_Renderer hosts: the ImGui frame is drawn at the renderer
 * OUTPUT size, so the game's SDL logical size is dropped for that draw and restored afterwards.
 */
#include "runtime_ui_host.h"
#include "recomp_runtime_ui.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "config.h"
}

namespace {

NesRuntimeUiHost s_host;
RecompRuntimeUi *s_ui = nullptr;
bool s_imgui_owned = false;
bool s_ready = false;
int s_state_slot = 1;

#define KEY_EXIT "system.exit"

const RecompRuntimeUiItem s_extra_items[] = {
    { KEY_EXIT, "System", "Exit game", "Close the game and return to the desktop.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr },
};

int is_key(const RecompRuntimeUiItem *item, const char *key) {
    return item && item->key && std::strcmp(item->key, key) == 0;
}

int get_value(void *, const RecompRuntimeUiItem *item, int *out) {
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN))       *out = g_nes_config.fullscreen != 0;
    else if (is_key(item, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) *out = g_nes_config.window_scale;
    else if (is_key(item, RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE)) *out = g_nes_config.integer_scale != 0;
    else if (is_key(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) *out = g_nes_config.linear_filter != 0;
    else if (is_key(item, RECOMP_RUNTIME_UI_KEY_VOLUME))       *out = g_nes_config.volume;
    else return 0;   /* unknown key: fail loudly rather than invent a value */
    return 1;
}

int set_value(void *, const RecompRuntimeUiItem *item, int value) {
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) {
        /* keep an exclusive-fullscreen choice from the launcher (2) when it is switched on again */
        g_nes_config.fullscreen = value ? (g_nes_config.fullscreen ? g_nes_config.fullscreen : 1) : 0;
    } else if (is_key(item, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) {
        g_nes_config.window_scale = value < 1 ? 1 : value;
    } else if (is_key(item, RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE)) {
        g_nes_config.integer_scale = value != 0;
    } else if (is_key(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) {
        g_nes_config.linear_filter = value != 0;
    } else if (is_key(item, RECOMP_RUNTIME_UI_KEY_VOLUME)) {
        g_nes_config.volume = value < 0 ? 0 : value > 100 ? 100 : value;   /* read by the mixer every frame */
    } else {
        return 0;
    }
    if (s_host.apply_display) s_host.apply_display();
    return 1;
}

int run_action(void *, const RecompRuntimeUiItem *item) {
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_RESUME)) {
        recomp_runtime_ui_close(s_ui);
        return 1;
    }
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_SAVE_STATE)) {
        if (s_host.save_state) s_host.save_state(s_state_slot);
        return s_host.save_state != nullptr;
    }
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_LOAD_STATE)) {
        if (s_host.load_state) s_host.load_state(s_state_slot);
        recomp_runtime_ui_close(s_ui);
        return s_host.load_state != nullptr;
    }
    if (is_key(item, KEY_EXIT)) {
        std::fprintf(stderr, "[RunnerExit] Exit from the in-game menu\n");
        std::exit(0);
    }
    return 0;
}

int is_enabled(void *, const RecompRuntimeUiItem *item) {
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_SAVE_STATE)) return s_host.save_state != nullptr;
    if (is_key(item, RECOMP_RUNTIME_UI_KEY_LOAD_STATE)) return s_host.load_state != nullptr;
    return 1;
}

void save_settings(void *) {
    config_save(config_path());
}

RecompRuntimeUiInput map_key(SDL_Keycode k, int *ok) {
    *ok = 1;
    switch (k) {
    case SDLK_UP:    return RECOMP_RUNTIME_UI_INPUT_UP;
    case SDLK_DOWN:  return RECOMP_RUNTIME_UI_INPUT_DOWN;
    case SDLK_LEFT:  return RECOMP_RUNTIME_UI_INPUT_LEFT;
    case SDLK_RIGHT: return RECOMP_RUNTIME_UI_INPUT_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_z: return RECOMP_RUNTIME_UI_INPUT_ACCEPT;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_x: return RECOMP_RUNTIME_UI_INPUT_BACK;
    default: *ok = 0; return RECOMP_RUNTIME_UI_INPUT_BACK;
    }
}

RecompRuntimeUiInput map_button(Uint8 b, int *ok) {
    *ok = 1;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    return RECOMP_RUNTIME_UI_INPUT_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return RECOMP_RUNTIME_UI_INPUT_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return RECOMP_RUNTIME_UI_INPUT_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RECOMP_RUNTIME_UI_INPUT_RIGHT;
    case SDL_CONTROLLER_BUTTON_A:          return RECOMP_RUNTIME_UI_INPUT_ACCEPT;
    case SDL_CONTROLLER_BUTTON_B:          return RECOMP_RUNTIME_UI_INPUT_BACK;
    case SDL_CONTROLLER_BUTTON_GUIDE:      return RECOMP_RUNTIME_UI_INPUT_TOGGLE;
    default: *ok = 0; return RECOMP_RUNTIME_UI_INPUT_BACK;
    }
}

/* Dispatches one event to the menu model. Returns 1 if it belonged to the menu. */
int dispatch(const SDL_Event &ev) {
    int ok = 0;
    switch (ev.type) {
    case SDL_KEYDOWN: {
        RecompRuntimeUiInput in = map_key(ev.key.keysym.sym, &ok);
        return ok ? recomp_runtime_ui_handle_input(s_ui, in, 1, ev.key.repeat != 0) : 0;
    }
    case SDL_KEYUP: {
        RecompRuntimeUiInput in = map_key(ev.key.keysym.sym, &ok);
        return ok ? recomp_runtime_ui_handle_input(s_ui, in, 0, 0) : 0;
    }
    case SDL_CONTROLLERBUTTONDOWN: {
        RecompRuntimeUiInput in = map_button(ev.cbutton.button, &ok);
        return ok ? recomp_runtime_ui_handle_input(s_ui, in, 1, 0) : 0;
    }
    case SDL_CONTROLLERBUTTONUP: {
        RecompRuntimeUiInput in = map_button(ev.cbutton.button, &ok);
        return ok ? recomp_runtime_ui_handle_input(s_ui, in, 0, 0) : 0;
    }
    default: return 0;
    }
}

void draw_frame() {
    SDL_Renderer *r = s_host.renderer;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (s_host.draw_game) s_host.draw_game();

    /* The ImGui frame is laid out in output pixels: drop the game's logical size for this draw. */
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    SDL_RenderSetLogicalSize(r, 0, 0);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(s_ui);
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_RenderSetLogicalSize(r, lw, lh);
    SDL_RenderPresent(r);
}

/* Nested loop: the simulation (which called us from its VBlank callback) stays frozen until the menu closes. */
void run_modal() {
    while (recomp_runtime_ui_is_open(s_ui)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
                std::fprintf(stderr, "[RunnerExit] window closed while the in-game menu was open\n");
                std::exit(0);
            }
            ImGui_ImplSDL2_ProcessEvent(&ev);
            dispatch(ev);
        }
        draw_frame();   /* SDL_RenderPresent is vsynced; that paces the loop */
    }
    /* Leave no key/button state behind that the game would read as a fresh press. */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYUP);
}

}  // namespace

extern "C" int nes_runtime_ui_init(const NesRuntimeUiHost *host) {
    if (s_ready || !host || !host->window || !host->renderer) return 0;
    s_host = *host;

    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        s_imgui_owned = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForSDLRenderer(s_host.window, s_host.renderer) ||
        !ImGui_ImplSDLRenderer2_Init(s_host.renderer)) {
        std::fprintf(stderr, "[RuntimeUI] ImGui backend init failed\n");
        return 0;
    }

    RecompRuntimeUiStandardConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.menu.title = s_host.title ? s_host.title : "Game";
    cfg.menu.subtitle = s_host.subtitle ? s_host.subtitle : "NES";
    cfg.menu.theme = "nes";
    cfg.menu.accept_label = "A / Enter";
    cfg.menu.back_label = "B / Esc";
    cfg.menu.callbacks.get_value = get_value;
    cfg.menu.callbacks.set_value = set_value;
    cfg.menu.callbacks.run_action = run_action;
    cfg.menu.callbacks.is_enabled = is_enabled;
    cfg.menu.callbacks.save = save_settings;
    cfg.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN | RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE |
                   RECOMP_RUNTIME_UI_STANDARD_INTEGER_SCALE | RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER |
                   RECOMP_RUNTIME_UI_STANDARD_VOLUME | RECOMP_RUNTIME_UI_STANDARD_RESUME |
                   RECOMP_RUNTIME_UI_STANDARD_SAVE_STATE | RECOMP_RUNTIME_UI_STANDARD_LOAD_STATE;
    cfg.window_scale_max = 8;
    cfg.extra_items = s_extra_items;
    cfg.extra_item_count = sizeof s_extra_items / sizeof s_extra_items[0];
    s_ui = recomp_runtime_ui_create_standard(&cfg);
    if (!s_ui) {
        std::fprintf(stderr, "[RuntimeUI] recomp_runtime_ui_create_standard failed\n");
        return 0;
    }
    s_ready = true;
    std::fprintf(stderr, "[RuntimeUI] in-game menu ready (Esc)\n");
    return 1;
}

extern "C" int nes_runtime_ui_active(void) { return s_ready ? 1 : 0; }

extern "C" int nes_runtime_ui_event(const SDL_Event *ev) {
    if (!s_ready || !ev) return 0;
    int open_it = 0;
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat &&
        ev->key.keysym.sym == SDLK_ESCAPE)
        open_it = 1;
    if (ev->type == SDL_CONTROLLERBUTTONDOWN && ev->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
        open_it = 1;
    if (!open_it) return 0;
    recomp_runtime_ui_open(s_ui);
    run_modal();
    return 1;
}

extern "C" void nes_runtime_ui_shutdown(void) {
    if (!s_ready) return;
    recomp_runtime_ui_destroy(s_ui);
    s_ui = nullptr;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    if (s_imgui_owned) ImGui::DestroyContext();
    s_ready = false;
}

