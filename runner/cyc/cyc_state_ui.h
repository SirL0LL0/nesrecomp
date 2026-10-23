/* Desktop snapshot browser and local rewind. The machine serializer stays
 * with the host; rbengine owns history blobs and recomp-net binds previews
 * to the exact saved blob. No online session is started by this module. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CycUiHost;
struct SDL_Renderer;
union SDL_Event;
void cyc_state_ui_init(struct SDL_Renderer *renderer, const struct CycUiHost *host);
void cyc_state_ui_shutdown(void);
void cyc_state_ui_configure(int enabled, int depth, int interval);
void cyc_state_ui_reset(void);
void cyc_state_ui_picture(const uint32_t *pixels, int width, int height);
void cyc_state_ui_frame(long frame);
bool cyc_state_ui_capture_due(long frame);
bool cyc_state_ui_open(int rewind);
void cyc_state_ui_close(void);
bool cyc_state_ui_is_open(void);
void cyc_state_ui_event(const union SDL_Event *event);
void cyc_state_ui_render(void);
void cyc_state_ui_status(char *out, size_t size);
#ifdef __cplusplus
}
#endif
