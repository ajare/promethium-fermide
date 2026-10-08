# Agent-type reconstruction on Reset (#508)

Simulation Reset constructs fresh isolated Agent Lua instances while preserving
stable Agent IDs, type IDs, selected resource references, individual properties,
Agent tag assignments and persisted samples. The existing authored-data reset
pipeline still restores initial positions, activation, permissions and Path
intent; ordinary paused structural/history preservation remains separate.

Before changing the live World, Reset resolves each used resource once and runs
all Agent constructors in a fresh World-local runtime with the original runtime's
instruction and allocation limits. Only a complete successful candidate set is
installed. Reconstruction borrows those exact validated instances during authored
replay, rather than calling constructors again after destructive reset. A failed
resource, identity check, constructor, baseline validation or budget check leaves
the old instances, topology, simulation tick, pause and dirty state untouched.
Diagnostics name the affected Agent, type and resource. Unused types need not
resolve successfully to reset unrelated Agents.

An existing Agent always retains its frozen physical baseline despite on-disk
script edits. Intentional Reset and document load resolve the current managed
revision and validate its constructor defaults. Application resource resolution
reads into a private candidate without replacing the startup/import resource or
changing the selector inventory. Reimport remains idempotent: no hot reload,
automatic callback or cross-script method API is introduced. Explicit missing,
invalid or mismatched managed resources are refused, not replaced with Human.
Directly attached in-memory definitions without a managed source retain their
registered source; standalone bundled Human still works without a filesystem
resource resolver.

World documents continue to contain authored data only. Lua private tables,
closures, VM state and frozen baseline snapshots are neither serialized nor
carried across Reset/load. Source definitions are owned values; opaque live
handles retain their sandbox until their final release, independently of the
application resource manager's lifetime. Replacing the runtime avoids charging
both old and replacement instances against one World's live allocation budget.

## Headless Release coverage

The Agent module covers revised external sources through YAML/binary load and
Reset, true module-upvalue isolation, unchanged individual-over-tag properties
and exact tag samples, frozen existing baselines, constructor/resource/identity
and baseline failures, per-call and aggregate allocation-budget failure atomicity,
and repeated reconstruction with retained private allocations.

The Editor module covers same-session managed-resource revisions through public
placement, YAML/binary document load and Reset, refused invalid revision without
partial mutation, stable resource inventory, and safe World use/reconstruction
after application resource teardown. Existing history/topology coverage continues
to verify the separate surviving-instance contract.

Validation uses incremental Release builds and CTest only, with displays unset.
The final project inventory excludes unmodified submodule tests as required by
`AGENTS.md`; Windows and Debug validation are not claimed.
