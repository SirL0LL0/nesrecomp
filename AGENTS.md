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
