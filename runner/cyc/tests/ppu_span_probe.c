/* Differential stimulus for cached native spans versus the general PPU.
 * Drive actual CPU bus cycles, including delayed register accesses and DMA,
 * then expose full machine hashes. The paired executable supplies the oracle.
 */
#include "hw_internal.h"
#include "hw.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void access_bus(uint16_t addr, uint8_t value, bool write)
{
    hw_cycle_start(addr, write ? HW_WRITE : HW_READ);
    if (write) hw_write(addr, value); else (void)hw_read(addr);
    hw_cycle_finish(false);
}

static void stimulus(unsigned i)
{
    /* A long stable render interval, followed by mixed register traffic.
     * Vary the period to hit every CPU/PPU phase instead of one scanline slot.
     */
    unsigned r = i * 1664525u + 1013904223u;
    if (i % 1511 == 0) access_bus((uint16_t)(0x4000 + (i / 1511) % 16), (uint8_t)(r >> 19), true);
    else if (i % 1021 == 0) access_bus(0x2001, (i & 1024) ? 0x1e : 0x00, true);
    else if (i % 509 == 0) access_bus(0x2000, (uint8_t)(r >> 16), true);
    else if (i % 251 == 0) access_bus(0x2005, (uint8_t)(r >> 24), true);
    else if (i % 127 == 0) access_bus(0x2006, (uint8_t)(r >> 19), true);
    else if (i % 61 == 0) access_bus(0x2007, (uint8_t)r, (i & 1) != 0);
    else if (i % 4093 == 0) access_bus(0x4014, 2, true);
    else if (i % 89 == 0) access_bus(0x2004, (uint8_t)(r >> 11), (i & 2) != 0);
    else if (i % 37 == 0) access_bus(0x2002, 0, false);
    else access_bus((uint16_t)(i & 0x7ff), 0, false);
}

static void replay_probe(void)
{
    CycSnapshot *snapshot = cyc_snapshot_new();
    check(snapshot != NULL, "allocate snapshot");
    cyc_snapshot_take(snapshot);
    for (unsigned i = 60001; i < 60314; ++i) stimulus(i);
    uint64_t after = cyc_hw_state_hash();
    uint64_t picture = cyc_mem_state_hash();
    cyc_snapshot_restore(snapshot);
    for (unsigned i = 60001; i < 60314; ++i) stimulus(i);
    check(after == cyc_hw_state_hash() && picture == cyc_mem_state_hash(), "mid-span replay");
    cyc_snapshot_free(snapshot);
}

int main(void)
{
    static uint8_t image[16 + 32768 + 8192];
    memcpy(image, "NES\032", 4); image[4] = 2; image[5] = 1;
    for (size_t i = 16; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 37u + (i >> 3));
    for (unsigned region = 0; region < 2; ++region) {
        for (unsigned align = 0; align < (region ? 5u : 4u); ++align) {
            check(cyc_load_ines(image, sizeof(image)), "load fixture");
            check(cyc_set_region(region ? CYC_REGION_PAL : CYC_REGION_NTSC), "region");
            cyc_power_on((uint8_t)align);
            check(cyc_zapper_attach(2), "attach Zapper");
            cyc_set_zapper(120, 100, true);
            check(cyc_audio_enable(44100), "enable audio");
            access_bus(0x4015, 0x1f, true);
            static const uint8_t registers[16] = {
                0xbf, 0x08, 0x40, 0x20, 0x5f, 0x08, 0x99, 0x20,
                0xff, 0x00, 0x07, 0x28, 0x3f, 0x00, 0x0d, 0x28
            };
            for (unsigned i = 0; i < sizeof(registers); ++i)
                access_bus((uint16_t)(0x4000 + i), registers[i], true);
            access_bus(0x4010, 0x0f, true);
            access_bus(0x4011, 0x40, true);
            access_bus(0x4012, 0x00, true);
            access_bus(0x4013, 0x01, true);
            access_bus(0x4015, 0x1f, true);
            for (unsigned i = 0; i < sizeof(hw.ram); ++i) hw.ram[i] = (uint8_t)(i * 13u);
            access_bus(0x2001, 0x1e, true);
            printf("case region=%u align=%u\n", region, align);
            for (unsigned i = 1; i <= 60000; ++i) {
                stimulus(i);
                if (i % 257 == 0)
                    printf("%u %016llx\n", i, (unsigned long long)cyc_hw_state_hash());
                if (i == 20000) replay_probe();
            }
            printf("picture %016llx\n", (unsigned long long)cyc_mem_state_hash());
            int16_t pcm[8192];
            size_t count = cyc_audio_read(pcm, sizeof(pcm) / sizeof(pcm[0]));
            uint64_t hash = UINT64_C(1469598103934665603);
            for (size_t i = 0; i < count; ++i) {
                hash = (hash ^ (uint8_t)pcm[i]) * UINT64_C(1099511628211);
                hash = (hash ^ (uint8_t)((uint16_t)pcm[i] >> 8)) * UINT64_C(1099511628211);
            }
            printf("pcm %zu %016llx\n", count, (unsigned long long)hash);
        }
    }
    return 0;
}
