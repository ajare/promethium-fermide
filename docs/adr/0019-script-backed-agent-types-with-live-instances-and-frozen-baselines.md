# Script-backed Agent types with live instances and frozen baselines

Status: accepted (amended)

Each Agent type is defined by one bundled or imported `.agent.lua` resource: a type object carrying a stable type ID, a display name, and a `new()` constructor that returns a fresh instance with a complete physical baseline. The host validates and freezes that baseline before publishing the Agent, while the Agent retains an isolated live Lua instance for its private state and methods. Human uses this script-backed path; there is no compiled Human subtype, physical definition, or Human-only factory/lookup adapter. Headless creation and legacy migration embed the bundled resource at build time directly from its authored source, rather than maintaining a second definition.

## Considered options

- **Keep a compiled physical subtype per Agent type.** Rejected because authors cannot define another type without recompiling, and the baseline would again be split between C++ and any script.
- **Share one executed module between Agent instances.** Rejected for the same reason as ADR 0008: mutable module upvalues would silently become shared Agent state. Source may be shared; each instance executes its module graph in a fresh private environment.
- **Copy the baseline back into Lua and re-read it every tick.** Rejected because simulation consumers need stable physical observations; a live mutation could invalidate ongoing movement or clearance mid-run.
- **Serialize the live Lua instance or its private state.** Rejected: closures, userdata, cycles, and implementation-version details are unsafe and unstable. Load, Reset, and restoration of a deleted Agent run `new()` again from the resolved source. Ordinary paused topology replay and document undo/redo preserve the live object of each surviving Agent; this is not serialization of private state.
- **Expose a cross-script method API or automatic callbacks on the instance.** Deferred. This slice retains the live instance only for private state and methods; no general invocation API is added, so ADR 0008's behaviour-instance semantics are not silently changed.

## Script-default Mobility (#512, #513, #515)

The constructor also supplies a required complete `mobility_profile`. Its nine
entries and three permitted uses are validated and frozen before publication
alongside the physical baseline. Effective Mobility is complete replacement in
the order individual Agent property → inherited Agent tag property → frozen
script default, never entry-wise merging or a live Lua query (ADR 0011).

The same lifetime boundaries apply to both defaults: surviving Agents preserve
them across paused structural edits and history replay; Load, Reset and deleted-
Agent restoration resolve and validate fresh defaults, including when masked by
an override. Ordinary creation/paste uses the World-registered source. Documents,
clipboard and history carry authored overrides and tag assignments, not script
Mobility snapshots materialised as individual properties. Failed fresh validation
leaves the current World and applicable history unchanged.

## Consequences

An Agent owns two deliberately separated things: its authored type identity (stable ID, display name, resolved resource reference) and its frozen physical baseline, which simulation consumers read; and an opaque live-instance handle owned by the World's Agent-type runtime, whose destructor releases the Lua instance when the Agent dies. The baseline is validated field-by-field (all eleven named numeric fields required; dimensions, reach, and speeds finite and positive after conversion to simulation floats; ratios finite and in `(0, 1]`) and copied out of Lua, so later mutations of the instance cannot change ongoing simulation. The World owns one sandboxed Lua state per the ADR 0008 isolation and budget model, with the same forbidden-capability restrictions and no filesystem, process, native-module, debug, or shared-mutable-module access.

Legacy Human records without a resource reference resolve to the bundled `human.agent.lua`; an explicit missing, invalid, or mismatched resource reference fails clearly and never falls back to Human. Type identity is immutable after creation; there is no conversion or hot reload. Display names are presentation, not stable identity: a revised display name is accepted on reconstruction. The historical `type` wire slot now also writes the stable ID, and readers still accept older records containing a display name there when `typeId` supplies identity. Persisted documents record the stable type ID and resource reference without embedding Lua state, and Reset/load/deleted-Agent restoration construct fresh instances from the currently resolved source while surviving instances retain their frozen baselines.

Structural replay carries opaque instance ownership alongside the frozen baseline and immutable type identity. Document history matches a survivor by stable authored Agent ID plus type ID and resource identity against the current World, not against a live object retained in a history snapshot. Snapshots contain authored data only: deleting an Agent releases its old instance, and undoing that deletion constructs a fresh one. A failed history restoration leaves the current World and history unchanged. Handles retain their originating sandbox state (including allocator and module-loader storage), so replacing or destroying the old World cannot leave dangling Lua references; fresh constructions use the replacement World's own runtime. No unrelated World imports another World's instances.

This Agent-type lifetime policy is distinct from ADR 0008's Agent **behaviour** instance policy. Behaviour reconstruction from authored configuration on undo, load, and Reset still follows ADR 0008; this change does not carry behaviour Lua state or transfer movement-intent ownership.
