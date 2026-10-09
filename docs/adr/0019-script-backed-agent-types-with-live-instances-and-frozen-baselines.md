# Script-backed Agent types with live instances and frozen baselines

Status: accepted (amended)

[ADR 0021](0021-run-installed-agent-behaviours-as-coroutines.md) adds the
host-mediated Installed behaviour association in the separate behaviour runtime
and reserves `behaviour` on Agent-type instances. It changes behaviour execution
to Host API v3 coroutines without merging Lua states, bumping the Agent-type API
or introducing the cross-script method API deferred here.

Approved follow-on [ADR 0020](0020-declare-agent-pose-capabilities-and-furniture-pose-requirements.md)
extends the frozen baseline to supported poses and automatic selection rules under
Agent-type API v2. The declaration and frozen-data migration is implemented in
#521; shared context fit/selection/routing/movement is implemented in #522.
Furniture/lifecycle enforcement remains pending in #523–#524.

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

## Frozen Arms usage and arm length (#535)

Agent-type API v2 additionally accepts `object_usage = "arms"` and a required
`object_usage_distance`. Omitted usage defaults to Arms, and legacy `reach`
declarations resolve to Arms with that distance. `reach` is an input alias only:
declaring both distance fields is rejected even if equal. This avoids competing
physical authorities while retaining old reach-only resources. Distance is a
concrete finite, strictly positive simulation float, independently frozen and
observed alongside usage. #536 adds the working None mode below; Remote control
remains deferred.

Shared World-facing physical-operation eligibility uses arm length, narrowed by
Interaction point geometry (including valid zero reach), without replacing
physical Location/side/height eligibility, Access permissions, Mobility, Broken
checks, durations, queues or traversal admission. Ordinary controls still permit
physical approach; controls requiring reach at request still refuse out-of-range
requests. Manual ordinary Door activation uses arm length at its source endpoint;
short-armed queue heads approach without gaining priority or changing crossing
bands. Automatic sensing and already-usable passage remain independent of Arms.
Onboard selectors retain their existing passenger-local physical placement.

Usage and distance follow the same frozen lifetime and authored-only persistence
rules as other baselines: surviving structural replay/history retain them;
creation, paste, load, Reset and deleted-Agent restoration validate fresh ones.
The host never re-reads them from live Lua. This working slice establishes the
shared physical policy; remote activation is deliberately deferred to later
slices of #534.

## None usage and operation-free routing (#536)

Agent-type API v2 accepts `object_usage = "none"` as a frozen lifetime default.
None ignores distance, which may be omitted; its unused frozen observation is
canonical zero. The dual-alias rejection remains in both modes. Other physical,
pose and Mobility defaults remain fully validated, and omission of usage still
means Arms with a required positive distance. No persistence schema or live Lua
query is added.

The shared World ability gate refuses Agent-operated requests and preparation,
while route feasibility hard-excludes operation-dependent Paths. We deliberately
do not encode None as Buttons Mobility or a permission denial: those would
incorrectly forbid automatic devices or change Permission adherence for
already usable resources. Landing authorization and operation capability are
separate queries; adherence continues to test only authorization.

A None passenger can observe and join an already accepted locally boardable
transport journey but cannot presume remote assistance, call a closed transport,
or supply a missing destination selection. Runtime admission rechecks shared
availability; if selection is impossible after admission, the existing safe-exit
path is used instead of leaving the passenger pending at a selector. Accepted
passenger destinations, admitted crossings and chamber exits are not revoked.
Ordinary automatic Doors, Bulkhead presence sensors and automated Chambers
remain device-owned behavior, not None Agent operations. Existing permissions,
Mobility, queues, capacity, reservations and Broken policies stay authoritative.

None shares the Arms lifetime policy, including fresh validation on paste,
reopen, Reset and deleted-Agent restoration, and retained defaults for surviving
structural/history replay. Documents and clipboard contain authored data only.

## Consequences

An Agent owns two deliberately separated things: its authored type identity (stable ID, display name, resolved resource reference) and its frozen physical baseline, which simulation consumers read; and an opaque live-instance handle owned by the World's Agent-type runtime, whose destructor releases the Lua instance when the Agent dies. The baseline is validated field-by-field (six non-pose numeric fields always required, plus positive Object usage distance for Arms; dimensions and speeds finite and positive after conversion to simulation floats; explicit supported poses and ordered Room/Door choices under API v2, with applicable height and speed ratios finite and in `(0, 1]`) and copied out of Lua, so later mutations of the instance cannot change ongoing simulation. The World owns one sandboxed Lua state per the ADR 0008 isolation and budget model, with the same forbidden-capability restrictions and no filesystem, process, native-module, debug, or shared-mutable-module access.

Legacy Human records without a resource reference resolve to the bundled `human.agent.lua`; an explicit missing, invalid, or mismatched resource reference fails clearly and never falls back to Human. Type identity is immutable after creation; there is no conversion or hot reload. Display names are presentation, not stable identity: a revised display name is accepted on reconstruction. The historical `type` wire slot now also writes the stable ID, and readers still accept older records containing a display name there when `typeId` supplies identity. Persisted documents record the stable type ID and resource reference without embedding Lua state, and Reset/load/deleted-Agent restoration construct fresh instances from the currently resolved source while surviving instances retain their frozen baselines.

Structural replay carries opaque instance ownership alongside the frozen baseline and immutable type identity. Document history matches a survivor by stable authored Agent ID plus type ID and resource identity against the current World, not against a live object retained in a history snapshot. Snapshots contain authored data only: deleting an Agent releases its old instance, and undoing that deletion constructs a fresh one. A failed history restoration leaves the current World and history unchanged. Handles retain their originating sandbox state (including allocator and module-loader storage), so replacing or destroying the old World cannot leave dangling Lua references; fresh constructions use the replacement World's own runtime. No unrelated World imports another World's instances.

This Agent-type lifetime policy is distinct from ADR 0008's Agent **behaviour** instance policy. Behaviour reconstruction from authored configuration on undo, load, and Reset still follows ADR 0008; this change does not carry behaviour Lua state or transfer movement-intent ownership.
