# Script-backed Agent types with live instances and frozen baselines

Status: accepted

Each Agent type is defined by one bundled or imported `.agent.lua` resource: a type object carrying a stable type ID, a display name, and a `new()` constructor that returns a fresh instance with a complete physical baseline. The host validates and freezes that baseline before publishing the Agent, while the Agent retains an isolated live Lua instance for its private state and methods. Human is migrated to this script-backed path; its compiled subtype becomes a compatibility adapter that routes to the same bundled definition rather than a second physical authority.

## Considered options

- **Keep a compiled physical subtype per Agent type.** Rejected because authors cannot define another type without recompiling, and the baseline would again be split between C++ and any script.
- **Share one executed module between Agent instances.** Rejected for the same reason as ADR 0008: mutable module upvalues would silently become shared Agent state. Source may be shared; each instance executes its module graph in a fresh private environment.
- **Copy the baseline back into Lua and re-read it every tick.** Rejected because simulation consumers need stable physical observations; a live mutation could invalidate ongoing movement or clearance mid-run.
- **Serialize the live Lua instance or its private state.** Rejected: closures, userdata, cycles, and implementation-version details are unsafe and unstable. Load, Reset, and reconstruction always run `new()` again from the resolved source.
- **Expose a cross-script method API or automatic callbacks on the instance.** Deferred. This slice retains the live instance only for private state and methods; no general invocation API is added, so ADR 0008's behaviour-instance semantics are not silently changed.

## Consequences

An Agent owns two deliberately separated things: its authored type identity (stable ID, display name, resolved resource reference) and its frozen physical baseline, which simulation consumers read; and an opaque live-instance handle owned by the World's Agent-type runtime, whose destructor releases the Lua instance when the Agent dies. The baseline is validated field-by-field (all eleven named fields required; dimensions, reach, and speeds finite and positive; ratios finite and in `(0, 1]`) and copied out of Lua, so later mutations of the instance cannot change ongoing simulation. The World owns one sandboxed Lua state per the ADR 0008 isolation and budget model, with the same forbidden-capability restrictions and no filesystem, process, native-module, debug, or shared-mutable-module access.

Legacy Human records without a resource reference resolve to the bundled `human.agent.lua`; an explicit missing, invalid, or mismatched resource reference fails clearly and never falls back to Human. Type identity is immutable after creation; there is no conversion or hot reload. Persisted documents record the stable type ID and resource reference without embedding Lua state, and Reset/load/reconstruction construct fresh instances from the currently resolved source while surviving instances retain their frozen baselines.
