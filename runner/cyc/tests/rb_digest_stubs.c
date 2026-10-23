/* The digest test has no machine or session; those entry points must not run. */
#include "cyc_state.h"
#include <stdlib.h>
#include <stdint.h>
uint8_t g_logical_input[4];
bool cyc_state_save(uint8_t **out,size_t *len,char *err,size_t err_len) {
    (void)out;(void)len;(void)err;(void)err_len;abort();
}
bool cyc_state_load(const uint8_t *buf,size_t len,char *err,size_t err_len) {
    (void)buf;(void)len;(void)err;(void)err_len;abort();
}
void cyc_session_state_loaded(void) { abort(); }
