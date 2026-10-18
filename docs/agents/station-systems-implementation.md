# Station systems implementation boundaries

`include/core/` remains the public API. The split implementation under `src/core/`
is private; CMake's existing `PF_CORE_SOURCES` glob discovers its translation units.

- `StationSystems.cpp`: source loader, protected Lua construction/stepping, runtime
  construction and lifecycle, public history/query forwarding.
- `StationSystemsValidation.cpp`: authored settings and wiring validation.
- `StationSystemsPhysics.cpp`: shared atmosphere, thermal and schedule calculations.
- `StationSystemsForecast.cpp`: the separate declared-profile forecast (not a second
  constrained allocator; see ADR 0023).
- `StationSystemsSnapshot.cpp` and `StationSystemsTrends.cpp`: observation assembly,
  bounded history and endurance.
- `StationSystemsLedger.cpp`: transactional rollback and the explicit tick phase order.
- `StationTick*.cpp`: allocation, audit, commit and event phases, plus their shared
  scratch-state operations.

`StationSystemsImpl.h` holds the runtime's private state layout and a nested `Tick`
scratch object. The scratch object lives for one call to `runLedger`, never crosses
World boundaries, and is not persisted. Former ledger locals live there so phases
can compile independently without templates or type-erased callbacks at phase
boundaries. Explicit initializers execute at their original positions; scratch
containers are default constructed when the tick begins. The former top-level
ledger lambdas are ordinary scratch methods; nested callbacks retain their original
captures. `StationSystemsDetail.h` contains private inline helpers and sandbox state;
`StationSystemsPhysics.h` declares shared private solver types/functions.

The phase calls are sequential, not parallel simulation work. Preserve their order,
priority/declaration-order iteration, audit checks, floating-point operation order,
and post-commit event ordering. A phase failure must propagate immediately through
`runLedger` to `applyLedger`, which retains the existing whole-tick rollback. Do not
publish staged inventory, constituent drift or connection snapshots before the
final commit phase. Public headers and serialization formats need no changes when
moving implementation between these files.
