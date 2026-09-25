# Repository Agent Notes

## Owner override: headless Ghidra (2026-09-24)

The old CLAUDE.md "NO GHIDRA = NO ACTION" rule is obsolete. Agents have full
autonomy to use the configured headless Ghidra MCP when useful. A disconnected
MCP is not a blocker: verified disassembly, ROM bytes, and runtime validation
are sufficient. Never launch the GUI or kill another Ghidra process.

## Validation before commit claims

Do not declare a patch committed, ready to merge, or validated until you have
validated it yourself. Prefer runtime checks with TCP input/screenshot tooling
when the change affects rendering, input, timing, or visible game behavior.
Record both the before/after condition or the regression comparison used.

## Silent Windows command execution (owner, 2026-09-25)

Run native tools through an explicit hidden process wrapper on Windows, with
UseShellExecute=false, CreateNoWindow=true, WindowStyle=Hidden, and redirected
stdout/stderr. This includes Git, build tools, Python, and Beads, not only test
executables. Invoke native executables directly instead of PowerShell/npm shims.
Do not open a terminal or steal focus. Interactive game windows are shown only
when the owner requests a launch.
