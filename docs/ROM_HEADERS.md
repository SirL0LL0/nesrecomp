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

The selected game/CLI region qualifies timing aliases. A PAL build also accepts
unspecified legacy timing, explicit PAL and multi-region headers; explicit
NES 2.0 NTSC and Dendy do not match a PAL profile. NTSC builds still reject
PAL-only and Dendy images. This preserves Dr. Mario's separate USA/EU releases.

Known legacy payloads use the pinned Mesen/NRS board database to resolve
MMC1 mirroring, UxROM bus-conflict submappers and MMC3 RAM/mirroring. Explicit
NES 2.0 board descriptions remain authoritative, and unknown payloads retain
their previous defaults. These board-fact corrections require native code to
be regenerated with the updated compiler. They are not hash-only aliases.

A legacy file with an incorrect declared chip size can be recovered only when
its complete payload CRC, actual length and mapper match a database entry.
For example, Yoshi USA payload `E7EAD93B` contains 128 KiB PRG and 32 KiB CHR;
some old headers incorrectly declare 64 KiB CHR. The original bytes are never
rewritten. Unknown/truncated payloads, trainers and explicit NES 2.0 geometry
are not repaired by this fallback. The compiler and runtime share this decoder.

`runner/cyc/cart_header_test.c` covers both directions of the format/timing
aliases, stable generated identities, every hardware field, explicit MMC3 RAM
geometry and mapper 71's format-dependent behavior.
