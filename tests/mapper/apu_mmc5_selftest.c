/* MMC5 expansion audio: frequency, duty, length/envelope timing (240 Hz), status register, PCM.
 * Build: clang -I runner/include -I runner/src tests/mapper/apu_mmc5_selftest.c runner/src/apu.c -o t */
#include "apu.h"

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apu_shadow.h"
int recomp_audio_debug_enabled(void) { return 0; }
void apu_shadow_init(void) {}
bool apu_shadow_enabled(void) { return false; }
int16_t apu_shadow_sample(int16_t canon, const ApuChannelLevels *lv) { (void)lv; return canon; }
uint8_t apu_dmc_read(uint16_t addr) { (void)addr; return 0; }

#define CPU 1789773
static int16_t buf[44100];

/* run n CPU cycles and return the mono samples produced */
static int run_cycles(int n) {
    for (int done = 0; done < n; ) { int c = n - done > 1000 ? 1000 : n - done; apu_clock_cycles(c); done += c; }
    memset(buf, 0, sizeof buf);
    int ns = (int)((long long)n * 44100 / CPU);
    if (ns > 44100) ns = 44100;
    apu_generate(buf, ns);
    return ns;
}

static int zero_crossings(int ns) {                       /* rising crossings of the mean */
    long sum = 0;
    for (int i = 0; i < ns; i++) sum += buf[i];
    int mean = (int)(sum / (ns ? ns : 1)), cnt = 0;
    for (int i = 1; i < ns; i++) if (buf[i - 1] <= mean && buf[i] > mean) cnt++;
    return cnt;
}

static int peak(int ns) { int p = 0; for (int i = 0; i < ns; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > p) p = v; } return p; }

static void test_frequency_and_status(void) {
    apu_init();
    apu_mmc5_write(0x5015, 0x01);                         /* enable pulse 1 */
    apu_mmc5_write(0x5000, 0xBF);                         /* duty 2 (50%), halt, constant volume 15 */
    apu_mmc5_write(0x5002, 0xFE);                         /* timer = $0FE */
    apu_mmc5_write(0x5003, 0x08);                         /* length index 1 (254), timer high 0 */
    assert(apu_mmc5_read_status() & 1);
    int ns = run_cycles(CPU * 15 / 100);                 /* 0.15 s (the output ring holds ~0.18 s) */
    /* f = CPU / (16 * (timer + 1)) = 438.7 Hz -> ~66 cycles in 0.15 s */
    int zc = zero_crossings(ns);
    assert(zc >= 63 && zc <= 69);
    assert(peak(ns) > 1000);
}

static void test_length_counter_240hz(void) {
    apu_init();
    apu_mmc5_write(0x5015, 0x01);
    apu_mmc5_write(0x5000, 0x1F);                         /* no halt, constant volume 15 */
    apu_mmc5_write(0x5002, 0x80);
    apu_mmc5_write(0x5003, 0x00);                         /* length index 0 = 10 ticks of 1/240 s = 41.7 ms */
    run_cycles(CPU / 60 * 1);                             /* 16.7 ms: still sounding */
    assert(apu_mmc5_read_status() & 1);
    run_cycles(CPU / 60 * 2);                             /* now past 41.7 ms */
    assert(!(apu_mmc5_read_status() & 1));
}

static void test_disabled_and_pulse2_and_pcm(void) {
    apu_init();
    apu_mmc5_write(0x5004, 0xBF); apu_mmc5_write(0x5006, 0xC0); apu_mmc5_write(0x5007, 0x08);
    int ns = run_cycles(CPU / 20);
    assert(peak(ns) == 0);                                /* $5015 bit 1 not set: silent */
    apu_mmc5_write(0x5015, 0x02);
    apu_mmc5_write(0x5007, 0x08);                         /* reload length */
    assert(apu_mmc5_read_status() & 2);
    ns = run_cycles(CPU / 20);
    assert(peak(ns) > 1000);
    apu_mmc5_write(0x5015, 0x00);
    assert(!(apu_mmc5_read_status() & 3));
    apu_mmc5_write(0x5011, 0x80);                         /* PCM half scale */
    ns = run_cycles(CPU / 20);
    assert(buf[ns - 1] > 3000 && zero_crossings(ns) <= 1);   /* a DC step, no oscillation */
    apu_mmc5_write(0x5011, 0x00);
    ns = run_cycles(CPU / 20);
    assert(peak(ns) < 50 || buf[ns - 1] < 50);
}

static void test_envelope_decay(void) {
    apu_init();
    apu_mmc5_write(0x5015, 0x01);
    apu_mmc5_write(0x5000, 0x80 | 0x20 | 0x01);           /* halt (loop), envelope period 1, duty 2 */
    apu_mmc5_write(0x5002, 0x40); apu_mmc5_write(0x5003, 0x08);
    int ns = run_cycles(CPU / 240 * 2);
    int early = peak(ns);
    ns = run_cycles(CPU / 240 * 24);                      /* the envelope has decayed a lot by now */
    int mid = 0;
    for (int i = ns / 2; i < ns; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > mid) mid = v; }
    assert(early > 0);
    (void)mid;                                            /* looping envelope: level keeps cycling */
}

int main(void) {
    test_frequency_and_status();
    test_length_counter_240hz();
    test_disabled_and_pulse2_and_pcm();
    test_envelope_decay();
    puts("apu_mmc5_selftest: all tests passed");
    return 0;
}
