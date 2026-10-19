# Equivalent NES cartridge headers

The launcher verifies the PRG+CHR content of a cartridge image. The cycle
runtime additionally checks the board identity used by generated code: fixed
bank reads are only valid for the correct mapper, RAM geometry and wiring.

An older iNES header and a NES 2.0 header can describe the same game and board.
For example, Super Mario Bros. (World) files with CRC32 `3337EC46` and `393A432F`
contain the same PRG+CHR bytes (CRC32 `D445F698`). The former has an iNES
header; the latter has a NES 2.0 multi-region header. Both select NTSC by
default in the cycle runtime.

`nes_cart_identity_compatible` accepts the corresponding existing generated
identities without changing their hash ABI or rewriting input metadata. It
allows the container-version difference and the NTSC/multi-region timing
alias. It preserves mapper 71's format distinction because the current mapper
implementation uses it to select different mirroring-register decoding.

All other identity fields remain checked, including resolved ROM/RAM sizes,
submapper, mirroring, battery, trainer, console and PAL-only/Dendy timing. The
ordinary image/board validation and program-ROM hash checks still run. Explicit
RAM metadata is not discarded to match an ambiguous older dump.

Do not universally strip NES headers: the runtime needs their hardware
description. Parse the image, verify its content and compare the resolved
board with the compiled profile. The compatibility check is shared by all
cycle games; a title does not need its own ROM exception or player toggle.

`runner/cyc/cart_header_test.c` covers both directions of the format/timing
aliases, stable generated identities, every hardware field, explicit MMC3 RAM
geometry and mapper 71's format-dependent behavior.
