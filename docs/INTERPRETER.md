# Legacy interpreter bridge

This guide describes the function-level runner in `runner/src/interp.c`, whose
public contract is in [`runner/include/interp.h`](../runner/include/interp.h).
The cycle backend has its own execution model; see the
[cycle runtime guide](../runner/cyc/README.md). Paths below are relative to the
repository root.

## Entry points and stack boundaries

Generated `call_by_address` paths enter `nes_interp_dispatch` for ROM targets
missing from static discovery, or for intentional code in RAM/SRAM. Execution
uses the runner's shared CPU and memory state and the recompiler's shared 6502
decode table. Fix the generator or runner when changing this behavior; never
patch generated C.

The native boundary contract requires `push_all_jsr`: the 6502 RAM stack must
mirror native calls. A handled dispatch returns 1. An interpreted RTS returns
to the existing native caller at the architectural boundary; it must not
redispatch a return address and grow the C stack. Nested interpreted calls use
the guest stack inside the same interpreter island.

ROM fallback defaults on when `push_all_jsr` is enabled; setting
`NESRECOMP_INTERP_FALLBACK=off` disables it. Without that stack precondition,
fallback self-disables. ROM misses honor the configured `LOG_RETURN`, `FATAL`
or `TRAP` policy. Intentional RAM/SRAM code is separate from optional ROM
fallback and remains interpreted when that fallback is disabled.

`nes_interp_interrupt` handles intentional RAM/SRAM vector entries without
recording them as static-discovery misses. `nes_interp_resume` resumes an
explicit stack continuation without interpreting initial stack lifts as an
ordinary return. After a non-local save-state resume discards the old C stack,
`nes_interp_reset_context` clears interpreter-only native bookkeeping. See
[netplay](NETPLAY.md) for the replay model.

## Native collaboration and diagnostics

`NES_INTERP_HANDOFF_ISLAND` is the default: nested calls and tail transfers
stay interpreted until an architectural exit. `SAFE` permits balanced JSR
handoffs to native code while keeping tails interpreted. `LEGACY` permits both
call and tail handoffs. These modes are explicit runtime policy, not evidence
that arbitrary native continuations are safe.

The handled/not-handled dispatch ABI is supplemented by `NesInterpExit`, which
records the exit reason, entry/next PC and entry/exit stack pointer. Outcomes
include return, RTI, native escape, stack escape, BRK and declined execution.

Ordinary dispatch and interrupt calls have per-call instruction watchdogs.
Explicit continuation resumes renew their watchdog on rendered-frame progress.
`NesInterpStats` and per-entry hotspots report interpreted instructions,
watchdog trips, handoffs, suppressed handoffs, declines and policy traps. Inspect
these alongside dispatch logs when promoting discovered paths to generated C;
a running fallback path does not establish static coverage or oracle accuracy.
