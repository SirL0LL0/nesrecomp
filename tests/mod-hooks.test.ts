import { describe, expect, it } from "vitest";
import { RomBuilder } from "./helpers/rom-builder";
import { recompile } from "./helpers/recompile";

// C000 branches or falls through into a separately callable C004 remainder.
// This is the native shape used by SMB's climbing/flagpole collision paths.
function rom() {
  return new RomBuilder().org(0xc000).lda(1).bne(0xc004)
    .lda(2).sta(0x0200).rts().vectors(0xc000,0xc000,0xc000)
    .writeTemp("internal-mod-hook.nes");
}
const config = `[game]
output_prefix = "internal-mod-hook"
[functions]
fixed = [0xC000, 0xC004]
[[mod_function_hook]]
addr = 0xC004
`;

describe("native branch mod hooks", () => {
  it("gates the instruction label, including branch and fallthrough entries", () => {
    const { fullC } = recompile(rom(), config + "include_internal = true\n");
    expect(fullC).toMatch(/label_C004:;[^]*?if \(nes_mod_function_entry\(0xC004u\)\) \{ \/\* trusted native branch entry/);
    expect(fullC).not.toContain("if (nes_mod_function_entry(0xC004u)) return;");
    expect(fullC).toContain("goto label_C004;");
  });
  it("preserves the default public-entry-only gate", () => {
    const { fullC } = recompile(rom(), config);
    expect(fullC).toContain("if (nes_mod_function_entry(0xC004u)) return;");
    expect(fullC).not.toContain("trusted native branch entry");
  });
});
