/*
 * cyc_app.c - application layer for games built on the cycle-accurate backend (see cyc_app.h).
 *
 *   main()           launcher (recomp-ui) or command line -> mods -> cyc_host_main (cyc_host.c, built with main renamed)
 *   cyc_sdl_main()   the interactive host: window, audio, keyboard/gamepad, in-game menu, save states, cheat hooks
 *
 * Keys: the ones in keybinds.ini for the pad, Esc = menu (pauses), F1-F12 = load slot, Shift+F1-F12 = save slot,
 * Tab (hold) = fast forward, F12 with Ctrl = screenshot, Alt+Enter = fullscreen.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "cyc_app.h"
#include "cyc_core.h"
#include "cyc_ext.h"
#include "cyc_png.h"
#include "cyc_recomp.h"
#include "cyc_run.h"

#include "config.h"
#include "controller.h"
#include "crc32.h"
#include "keybinds.h"
#include "mod_runtime.h"
#include "runtime_ui_host.h"

#ifdef RECOMP_LAUNCHER
#include "launcher_profile.h"
#include "recomp_launcher.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

/* cyc_host.c, compiled with main renamed. */
int  cyc_host_main(int argc, char **argv);
void write_miss_log(const char *path);
extern uint32_t *cyc_run_miss;

static char g_exe_dir[512] = "./";

/* ------------------------------------------------------------------------------------------------------------------ */
/* ROM selection                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------------ */

static void exe_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s%s", g_exe_dir, name); }

static void rom_cfg_read(char *out, int n) {
    char p[600];
    exe_path(p, sizeof p, "rom.cfg");
    FILE *f = fopen(p, "r");
    out[0] = 0;
    if (!f) return;
    if (!fgets(out, n, f)) out[0] = 0;
    fclose(f);
    size_t len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
}

static void rom_cfg_write(const char *rom) {
    char p[600];
    exe_path(p, sizeof p, "rom.cfg");
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom);
    fclose(f);
}

/* CRC32 of the ROM after its iNES header: the same value for iNES 1.0 and NES 2.0 headers of one dump. */
static int rom_ok(const char *path, uint32_t expected) {
    if (!expected) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *d = sz > 16 ? (uint8_t *)malloc((size_t)sz) : NULL;
    int ok = 0;
    if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz) ok = crc32_compute(d + 16, (size_t)sz - 16) == expected;
    free(d);
    fclose(f);
    return ok;
}

static int pick_rom(char *out, int n) {
#ifdef _WIN32
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    out[0] = 0;
    o.lStructSize = sizeof o;
    o.lpstrFilter = "NES ROM (*.nes)\0*.nes\0All files (*.*)\0*.*\0";
    o.lpstrFile = out;
    o.nMaxFile = (DWORD)n;
    o.lpstrTitle = "Select the ROM";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&o) ? 1 : 0;
#else
    (void)out;
    (void)n;
    fprintf(stderr, "No ROM given: pass its path as the first argument.\n");
    return 0;
#endif
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* main                                                                                                                */
/* ------------------------------------------------------------------------------------------------------------------ */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const CycAppGame *game = cyc_app_game();
    nesrecomp_exe_dir(g_exe_dir, sizeof g_exe_dir);

    {
        char root[600];
        exe_path(root, sizeof root, "mods");
        if (!nes_mod_runtime_initialize_c(root, game->game_id, game->rom_crc32)) {
            fprintf(stderr, "[Mods] cannot initialize the catalog: %s\n", nes_mod_runtime_last_error_c());
            return 1;
        }
    }
    config_load(config_path());
    keybinds_init(argv[0]);

    static char rom_path[600];
    int resolved = 0;
    int positional = argc >= 2 && argv[1][0] != '-';
    int force_launcher = argc >= 2 && strcmp(argv[1], "--launcher") == 0;
    int cli_flags = argc >= 2 && argv[1][0] == '-' && !force_launcher;   /* headless/diagnostic runs skip the GUI */

#ifdef RECOMP_LAUNCHER
    if (!getenv("NESRECOMP_NO_LAUNCHER") && !positional && !cli_flags && (!g_nes_config.skip_launcher || force_launcher)) {
        char init_rom[600];
        rom_cfg_read(init_rom, sizeof init_rom);

        RecompLauncherCSettings ls;
        memset(&ls, 0, sizeof ls);
        ls.window_scale = g_nes_config.window_scale;
        ls.fullscreen = g_nes_config.fullscreen;
        ls.integer_scale = g_nes_config.integer_scale;
        ls.linear_filter = g_nes_config.linear_filter;
        ls.renderer = g_nes_config.renderer;
        ls.widescreen = 0;
        ls.enable_audio = 1;
        ls.volume = g_nes_config.volume;
        ls.player_src[0] = g_nes_config.player_src[0];
        ls.player_src[1] = g_nes_config.player_src[1];
        ls.deadzone[0] = g_nes_config.deadzone[0];
        ls.deadzone[1] = g_nes_config.deadzone[1];
        ls.skip_launcher = g_nes_config.skip_launcher;

        RecompLauncherCGameInfo gi;
        memset(&gi, 0, sizeof gi);
        launcher_profile_apply("nes", &gi);
        gi.name = game->name;
        gi.region = "NTSC-U (USA)";
        gi.expected_crc = game->expected_crc;
        gi.has_expected_crc = game->expected_crc != 0;
        gi.num_players = game->players;
        gi.widescreen_supported = 0;
        gi.hdpack_supported = 0;
        gi.config_path = config_path();
        static char keybinds_file[600];
        exe_path(keybinds_file, sizeof keybinds_file, "keybinds.ini");
        gi.keybinds_path = keybinds_file;
        gi.mods = nes_mod_runtime_launcher_provider_c();

        char title[128];
        snprintf(title, sizeof title, "%s - Launcher", game->name);
        int act = recomp_launcher_run_window(title, &ls, &gi, ".", init_rom, rom_path, sizeof rom_path);
        if (act == 1) return 0;   /* window closed */
        if (act == 0) {
            g_nes_config.window_scale = ls.window_scale;
            g_nes_config.fullscreen = ls.fullscreen;
            g_nes_config.integer_scale = ls.integer_scale;
            g_nes_config.linear_filter = ls.linear_filter;
            g_nes_config.renderer = ls.renderer;
            g_nes_config.volume = ls.volume;
            g_nes_config.player_src[0] = ls.player_src[0];
            g_nes_config.player_src[1] = ls.player_src[1];
            g_nes_config.deadzone[0] = ls.deadzone[0];
            g_nes_config.deadzone[1] = ls.deadzone[1];
            g_nes_config.skip_launcher = ls.skip_launcher;
            config_save(config_path());
            if (rom_path[0]) {
                rom_cfg_write(rom_path);
                resolved = 1;
            }
        }
    }
#else
    (void)force_launcher;
#endif

    if (!resolved) {
        if (positional) {
            snprintf(rom_path, sizeof rom_path, "%s", argv[1]);
            if (!rom_ok(rom_path, game->expected_crc))
                fprintf(stderr, "[Launcher] warning: '%s' does not have the expected CRC32; continuing\n", rom_path);
        } else {
            rom_cfg_read(rom_path, sizeof rom_path);
            while (!rom_path[0] || !rom_ok(rom_path, game->expected_crc)) {
                rom_path[0] = 0;
                if (!pick_rom(rom_path, sizeof rom_path)) return 1;
            }
            rom_cfg_write(rom_path);
        }
    }

    if (!nes_mod_runtime_commit_c(rom_path)) {
        fprintf(stderr, "[Mods] cannot start: %s\n", nes_mod_runtime_last_error_c());
        return 1;
    }
    nes_mod_runtime_activate_plugins_c();

    /* cyc_host_main(<exe> <rom> [options]): the rest of the command line is passed on. */
    char *av[64];
    int ac = 0;
    av[ac++] = argv[0];
    av[ac++] = rom_path;
    for (int i = positional ? 2 : 1; i < argc && ac < 63; i++) {
        if (strcmp(argv[i], "--launcher") == 0) continue;
        av[ac++] = argv[i];
    }
    av[ac] = NULL;
    return cyc_host_main(ac, av);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Interactive host                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------------ */

#define AUDIO_RATE 48000

static char s_miss_out[600];
static int s_miss_done;

/* Also runs when the menu's Exit (or closing the window in the menu) calls exit() instead of returning. */
static void write_miss_once(void) {
    if (s_miss_done || !cyc_run_miss || !s_miss_out[0]) return;
    s_miss_done = 1;
    write_miss_log(s_miss_out);
}

/* Statistics overlay text: which machine ran the last second, and how much of the CPU time each part took. */
typedef struct {
    Uint64 mark;
    uint64_t cycles, native, rom, ram, other;
    double fps, p_native, p_rom, p_ram, p_other;
    int frames;
    size_t miss_total, miss_last_total;   /* distinct ROM addresses ever seen on the interpreter (cyc_run_miss) */
    char text[512];
} Stats;

/* Count of non-zero entries in cyc_run_miss (a full scan; called at most once a second, see stats_update). */
static size_t miss_distinct_count(void) {
    if (!cyc_run_miss) return 0;
    size_t n = cyc_run_miss_slots(), count = 0;
    for (size_t i = 0; i < n; i++) count += cyc_run_miss[i] != 0;
    return count;
}

static void stats_update(Stats *s, uint64_t frame, Uint64 now, Uint64 freq) {
    s->frames++;
    if (now - s->mark >= freq) {
        double secs = (double)(now - s->mark) / (double)freq;
        uint64_t c = cyc_cycle_count();
        uint64_t dc = c - s->cycles;
        double k = dc ? 100.0 / (double)dc : 0.0;
        s->fps = s->frames / secs;
        s->p_native = (double)(cyc_run_native_cycles - s->native) * k;
        s->p_rom = (double)(cyc_run_interp_rom_cycles - s->rom) * k;
        s->p_ram = (double)(cyc_run_interp_ram_cycles - s->ram) * k;
        s->p_other = (double)(cyc_run_interp_other_cycles - s->other) * k;
        s->cycles = c;
        s->native = cyc_run_native_cycles;
        s->rom = cyc_run_interp_rom_cycles;
        s->ram = cyc_run_interp_ram_cycles;
        s->other = cyc_run_interp_other_cycles;
        s->mark = now;
        s->frames = 0;
        if (cyc_run_miss) {
            s->miss_last_total = s->miss_total;
            s->miss_total = miss_distinct_count();
        }
    }
    uint64_t tot = cyc_cycle_count();
    double tp = tot ? 100.0 * (double)cyc_run_native_cycles / (double)tot : 0.0;
    int len = snprintf(s->text, sizeof s->text,
             "%.1f fps   frame %llu\n"
             "Esecuzione: %s\n"
             "CPU, ultimo secondo:\n"
             "  codice compilato   %5.1f%%\n"
             "  interprete (ROM)   %5.1f%%   codice non compilato\n"
             "  interprete (RAM)   %5.1f%%   codice in RAM\n"
             "  interprete (altro) %5.1f%%\n"
             "Compilato dall'avvio: %.1f%%",
             s->fps, (unsigned long long)frame, cyc_run_native ? "compilato + interprete" : "solo interprete",
             s->p_native, s->p_rom, s->p_ram, s->p_other, tp);
    if (cyc_run_miss && len > 0 && (size_t)len < sizeof s->text) {
        /* Indirizzi ROM incontrati per la prima volta sull'interprete (miss.on): materia prima per crescere il codice
         * nativo e il disassemblato (tools/seeds_to_coverage.py). Non sono "funzioni": e' il conteggio grezzo di
         * indirizzi distinti, la disassemblazione in funzioni resta un'analisi statica separata. */
        snprintf(s->text + len, sizeof(s->text) - (size_t)len,
                 "\nIndirizzi nuovi (miss.on): %zu   (+%zu nell'ultimo secondo)",
                 s->miss_total, s->miss_total > s->miss_last_total ? s->miss_total - s->miss_last_total : 0);
    }
}

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;

static void slot_path(int slot, char *out, size_t n) { snprintf(out, n, "%ssavestates/slot%02d.sav", g_exe_dir, slot); }

static void state_save(int slot) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%ssavestates", g_exe_dir);
    MKDIR(dir);
    slot_path(slot, p, sizeof p);
    printf("[state] save slot %d: %s\n", slot, cyc_state_save(p) ? "ok" : "FAILED");
}

static void state_load(int slot) {
    char p[700];
    slot_path(slot, p, sizeof p);
    printf("[state] load slot %d: %s\n", slot, cyc_state_load(p) ? "ok" : "FAILED (missing or from another build)");
}

static int slot_from_key(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return (int)(sc - SDL_SCANCODE_F1) + 1;
    return 0;
}

static void draw_game(void) {
    if (s_tex) SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
}

static void apply_display(void) {
    if (!s_win || !s_ren) return;
    SDL_ScaleMode mode = g_nes_config.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
    if (s_tex) SDL_SetTextureScaleMode(s_tex, mode);
    SDL_RenderSetIntegerScale(s_ren, g_nes_config.integer_scale ? SDL_TRUE : SDL_FALSE);
    Uint32 fs = SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN;
    if (g_nes_config.fullscreen && !fs)
        SDL_SetWindowFullscreen(s_win, g_nes_config.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (!g_nes_config.fullscreen && fs)
        SDL_SetWindowFullscreen(s_win, 0);
    if (!g_nes_config.fullscreen) {
        int scale = g_nes_config.window_scale < 1 ? 1 : g_nes_config.window_scale;
        int w = 0, h = 0;
        SDL_GetWindowSize(s_win, &w, &h);
        if (w != 256 * scale || h != 240 * scale) SDL_SetWindowSize(s_win, 256 * scale, 240 * scale);
    }
}

static uint8_t read_pad(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int src = g_nes_config.player_src[0];
    uint8_t b = (uint8_t)((src == 1 ? keybinds_read_player(keys, 1) : 0) | (src == 2 ? controller_read_player(1) : 0));
    /* The NES pad cannot press opposite directions at once. */
    if ((b & 0x0C) == 0x0C) b &= (uint8_t)~0x0C;
    if ((b & 0x03) == 0x03) b &= (uint8_t)~0x03;
    return b;
}

int cyc_sdl_main(const char *title, int scale) {
    const CycAppGame *game = cyc_app_game();
    (void)title;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (g_nes_config.window_scale > 0) scale = g_nes_config.window_scale;
    if (scale < 1) scale = 1;

    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (g_nes_config.fullscreen) flags |= g_nes_config.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_win = SDL_CreateWindow(game->name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * scale, 240 * scale, flags);
    if (s_win) s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (s_win && !s_ren) s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (s_ren) s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!s_tex) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(s_ren, 256, 240);
    apply_display();

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev && cyc_audio_enable(have.freq)) SDL_PauseAudioDevice(dev, 0);
    else if (!dev) fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());

    controller_init();

    NesRuntimeUiHost host;
    memset(&host, 0, sizeof host);
    host.window = s_win;
    host.renderer = s_ren;
    host.draw_game = draw_game;
    host.apply_display = apply_display;
    host.save_state = state_save;
    host.load_state = state_load;
    host.title = game->name;
    host.subtitle = "NES";
    if (!nes_runtime_ui_init(&host)) fprintf(stderr, "[RuntimeUI] not available; Esc will quit\n");
    if (getenv("CYC_TEST_OVERLAY")) nes_runtime_ui_set_overlay(1);   /* scripted checks: start with the overlay on */

    /* Live interpreter-miss log for growing the native coverage: create a file named miss.on next to the exe. */
    char miss_flag[600];
    exe_path(miss_flag, sizeof miss_flag, "miss.on");
    exe_path(s_miss_out, sizeof s_miss_out, "cycle_seeds_played.txt");
    FILE *mf = fopen(miss_flag, "rb");
    int want_miss = mf != NULL;
    if (mf) fclose(mf);
    if (want_miss) {
        cyc_run_miss = (uint32_t *)calloc(cyc_run_miss_slots(), sizeof(uint32_t));
        printf("[miss] recording interpreted ROM addresses -> %s\n", s_miss_out);
        atexit(write_miss_once);
    }

    if (game->on_init) game->on_init();

    const double frame_seconds = 1.0 / 60.0988;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next = SDL_GetPerformanceCounter();
    uint64_t frame = 0;
    Stats stats;
    memset(&stats, 0, sizeof stats);
    stats.mark = SDL_GetPerformanceCounter();
    int shot = 0;
    int running = 1;
    const char *tm = getenv("CYC_TEST_MENU_AT");
    long test_menu_at = tm ? atol(tm) : -1;
    const char *tq = getenv("CYC_TEST_QUIT_AT");      /* CYC_TEST_QUIT_AT=<frame>: exit cleanly (scripted checks) */
    long test_quit_at = tq ? atol(tq) : -1;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            controller_handle_event(&ev);
            if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) running = 0;
            if (nes_runtime_ui_active()) {
                if (nes_runtime_ui_event(&ev)) { next = SDL_GetPerformanceCounter(); continue; }
            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                running = 0;
            }
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keymod mod = ev.key.keysym.mod;
                int slot = slot_from_key(ev.key.keysym.scancode);
                if (slot == 11 && (mod & KMOD_CTRL)) {   /* Ctrl+F11: statistics overlay */
                    nes_runtime_ui_set_overlay(!nes_runtime_ui_overlay_enabled());
                } else if (slot && (mod & KMOD_CTRL) && slot == 12) {
                    char name[700];
                    snprintf(name, sizeof name, "%scyc_shot_%04d.png", g_exe_dir, shot++);
                    if (cyc_write_png(name, cyc_frame_argb(), 256, 240)) printf("saved %s\n", name);
                } else if (slot) {
                    if (mod & KMOD_SHIFT) state_save(slot);
                    else state_load(slot);
                    next = SDL_GetPerformanceCounter();
                }
                if (ev.key.keysym.sym == SDLK_RETURN && (mod & KMOD_ALT)) {
                    Uint32 fs = SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(s_win, fs ? 0 : (g_nes_config.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN
                                                                                          : SDL_WINDOW_FULLSCREEN_DESKTOP));
                }
            }
        }
        if (!running) break;

        if (test_quit_at >= 0 && (long)frame >= test_quit_at) exit(0);   /* like the menu's Exit: exit(), not a return */
        if (test_menu_at >= 0 && (long)frame == test_menu_at) {   /* CYC_TEST_MENU_AT=<frame>: open the menu like Esc */
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_KEYDOWN;
            e.key.keysym.sym = SDLK_ESCAPE;
            e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
            SDL_PushEvent(&e);
        }
        cyc_set_controller(0, read_pad());
        cyc_set_controller(1, 0);   /* one-player game: nothing plugged in port 2 */
        if (game->on_frame) game->on_frame(frame);
        cyc_run_frame();
        frame++;

        const int fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] != 0;
        int16_t pcm[4096];
        size_t n;
        int vol = g_nes_config.volume < 0 ? 0 : g_nes_config.volume > 100 ? 100 : g_nes_config.volume;
        while ((n = cyc_audio_read(pcm, 4096)) > 0) {
            if (vol != 100)
                for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[i] * vol) / 100);
            /* Keep latency bounded: drop a frame's audio if ~100 ms are already queued. */
            if (dev && !fast && SDL_GetQueuedAudioSize(dev) < (Uint32)(have.freq / 10) * 2)
                SDL_QueueAudio(dev, pcm, (Uint32)(n * sizeof(int16_t)));
        }

        SDL_UpdateTexture(s_tex, NULL, cyc_frame_argb(), 256 * 4);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        stats_update(&stats, frame, SDL_GetPerformanceCounter(), freq);
        nes_runtime_ui_draw_overlay(stats.text);
        SDL_RenderPresent(s_ren);

        Uint64 now = SDL_GetPerformanceCounter();
        if (fast) { next = now; continue; }
        next += (Uint64)(frame_seconds * (double)freq);
        if (next > now) {
            Uint32 ms = (Uint32)((next - now) * 1000 / freq);
            if (ms > 1) SDL_Delay(ms - 1);
            while (SDL_GetPerformanceCounter() < next) {}
        } else if (now - next > freq / 4) {
            next = now;   /* fell behind: do not try to catch up */
        }
    }

    write_miss_once();
    nes_runtime_ui_shutdown();
    controller_shutdown();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Scripted (headless) runs: the game's hooks, and state save/load at chosen frames for testing.                       */
/*   CYC_SAVE_AT=<frame>:<file>   save the machine before that frame runs                                            */
/*   CYC_LOAD_AT=<frame>:<file>   load a state before that frame runs                                                */
/* ------------------------------------------------------------------------------------------------------------------ */

static long s_save_at = -1, s_load_at = -1;
static char s_save_file[600], s_load_file[600];

static int parse_at(const char *env, long *frame, char *file, size_t n) {
    const char *v = getenv(env);
    if (!v) return 0;
    char *end;
    long f = strtol(v, &end, 10);
    if (end == v || *end != ':') return 0;
    *frame = f;
    snprintf(file, n, "%s", end + 1);
    return 1;
}

void cyc_app_headless_init(void) {
    const CycAppGame *game = cyc_app_game();
    parse_at("CYC_SAVE_AT", &s_save_at, s_save_file, sizeof s_save_file);
    parse_at("CYC_LOAD_AT", &s_load_at, s_load_file, sizeof s_load_file);
    if (game->on_init) game->on_init();
}

void cyc_app_headless_frame(long frame) {
    const CycAppGame *game = cyc_app_game();
    if (frame == s_load_at) printf("[state] load %s at frame %ld: %s\n", s_load_file, frame, cyc_state_load(s_load_file) ? "ok" : "FAILED");
    if (game->on_frame) game->on_frame((uint64_t)frame);
    if (frame == s_save_at) printf("[state] save %s at frame %ld: %s\n", s_save_file, frame, cyc_state_save(s_save_file) ? "ok" : "FAILED");
}
