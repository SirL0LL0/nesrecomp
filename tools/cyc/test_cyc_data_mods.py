"""ROM-free native/interpreter tests for safe data overlays and banked cheats."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nested_build import Toolchain, add_arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    add_arguments(parser)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    image = bytearray(16 + 32768 + 8192)
    image[:6] = b'NES\x1a\x02\x01'
    code = bytes.fromhex('A9 11 85 00 AD 20 80 85 01 AD 20 C0 85 02 4C 00 80')
    image[16:16 + len(code)] = code
    image[16 + 0x20] = 0x33
    image[16 + 0x4020] = 0x77
    image[16 + 32768 - 6:16 + 32768] = struct.pack('<HHH', 0x8000, 0x8000, 0x8000)
    (out / 'fixture.nes').write_bytes(image)
    (out / 'extras.c').write_text(r'''
#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include "cyc_recomp.h"
#include "hw.h"
#include "mod_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned frame, checks;
static int pre_registered;
NES_MOD_CONSTRUCTOR(before_rom) {
    pre_registered = cyc_mod_set_prg_read_override(0x20, 0x33, 0x88);
}
static unsigned char original[40960], payload[40960], actual[40960];
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"data-mod check %u failed: %s\n",checks,#x); exit(1); } } while (0)
static void enable(void) {
    CHECK(cyc_mod_set_prg_read_override(1, 0x11, 0x42));
    CHECK(cyc_mod_set_prg_read_override(0x20, 0x55, 0x99));
    CHECK(!cyc_mod_set_prg_read_override(0x4020, 0x55, 0x99));
    CHECK(!cyc_mod_set_prg_read_override(0x8000, 0, 1));
}
static void power(void *ctx) {
    (void)ctx;
    CHECK(pre_registered && cyc_mod_rom_read_override_count() == 1);
    cyc_mod_clear_rom_read_overrides();
    CHECK(cyc_mod_copy_cart_payload(NULL, 0) == sizeof original);
    memset(actual, 0xEE, sizeof actual);
    CHECK(cyc_mod_copy_cart_payload(actual, 1) == sizeof actual && actual[0] == 0xEE);
    CHECK(cyc_mod_copy_cart_payload(original, sizeof original) == sizeof original);
    CHECK(cyc_native_prg_code_byte(0));
    CHECK(cyc_native_prg_code_byte(1));
    CHECK(cyc_native_prg_code_byte(5) && cyc_native_prg_code_byte(6));
    CHECK(!cyc_native_prg_code_byte(0x20));
    CHECK(!cyc_native_prg_code_byte(0xFFFFFFFF));
    for (unsigned i = 0; i < 3; i++) {
        memcpy(payload, original, sizeof payload);
        payload[i == 0 ? 0 : i == 1 ? 1 : 6] ^= 1;
        payload[0x20] = 0x55; payload[32768] = 0x66;
        CHECK(!cyc_mod_apply_cart_data_payload(payload, sizeof payload));
        cyc_mod_copy_cart_payload(actual, sizeof actual);
        CHECK(!memcmp(actual, original, sizeof actual));
        CHECK(!hw_prg_modified && !hw_prg_data_modified);
    }
    memcpy(payload, original, sizeof payload);
    payload[0x20] = 0x55; payload[32768] = 0x66;
    CHECK(!cyc_mod_apply_cart_data_payload(payload, sizeof payload - 1));
    CHECK(cyc_mod_isolate_begin());
    CHECK(!cyc_mod_apply_cart_data_payload(payload, sizeof payload));
    cyc_mod_isolate_end();
    CHECK(cyc_mod_apply_cart_data_payload(payload, sizeof payload));
    cyc_mod_copy_cart_payload(actual, sizeof actual);
    CHECK(!memcmp(actual, payload, sizeof actual));
    CHECK(!hw_prg_modified && hw_prg_data_modified);
    enable();
}
static void end(void *ctx) {
    (void)ctx;
    printf("data-mod-result %02X %02X %02X\n",cyc_mod_peek(0),cyc_mod_peek(1),cyc_mod_peek(2));
    if (frame++ == 0) cyc_mod_clear_rom_read_overrides(); else enable();
    if (frame == 3) {
        size_t size;
        CHECK(cyc_mod_prg_data_rw(&size) && size == 32768 && hw_prg_modified);
        CHECK(cyc_mod_apply_cart_data_payload(payload, sizeof payload) && hw_prg_modified);
        printf("data-mod-checks %u\n", checks);
    }
}
static const CycHostExtras extras = {.power_on=power, .frame_end=end};
const CycHostExtras *cyc_host_extras(void) { return &extras; }
''', encoding='utf-8', newline='\n')
    (out / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(DataModTest C CXX)
set(NESRECOMP_HEADLESS ON)
set(NESRECOMP_RECOMP_UI OFF)
include("{root.as_posix()}/runner/cyc/project.cmake")
nesrecomp_add_cycle_game(data_mod_test HEADLESS
    ROM "{(out / 'fixture.nes').as_posix()}"
    RECOMPILER "{args.recompiler.resolve().as_posix()}"
    HOST_EXTRAS "{(out / 'extras.c').as_posix()}")
''', encoding='utf-8', newline='\n')
    toolchain = Toolchain.from_args(args)
    (out / 'logs').mkdir(exist_ok=True)
    toolchain.build(out, out / 'build', out / 'logs', parallel=4)
    exe = toolchain.executable(out / 'build', 'data_mod_test')
    for mode in ([], ['--interp-only']):
        result = subprocess.run([str(exe), str(out / 'fixture.nes'), '--frames', '3', '--no-save', *mode],
                                capture_output=True, text=True, timeout=30,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        name = 'interpreter' if mode else 'native'
        (out / f'{name}.log').write_text(result.stdout + result.stderr, encoding='utf-8', newline='\n')
        assert result.returncode == 0, result.stdout + result.stderr
        actual = re.findall(r'data-mod-result ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2})', result.stdout)
        assert actual == [('42', '99', '77'), ('11', '55', '77'), ('42', '99', '77')], actual
        assert 'data-mod-checks ' in result.stdout
        print(f'{name}: bank/compare isolation, folded data, code/operand refusal, atomic CHR, scoped refusal passed', flush=True)


if __name__ == '__main__':
    main()
