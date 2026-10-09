# Lua Agent-type authoring and integrated migration (#510)

> **API v2 declarations (#521) and shared fit/selection/movement (#522) are implemented.**
> Supported poses and automatic choices are validated and frozen per lifetime; movement always prefers the tallest fitting allowed pose.
> Room/Door motion, placement, routing and admission consume the shared fit result.
> Furniture eligibility and lifecycle/edit enforcement are integrated (#523–#524).
> See [selection validation](agent-pose-selection-validation.md) and
> [lifecycle validation](agent-pose-lifecycle-validation.md).
> See [Agent pose capabilities](agent-pose-capabilities.md) and [ADR 0020](adr/0020-declare-agent-pose-capabilities-and-furniture-pose-requirements.md).

An Agent type is one managed `.agent.lua` resource returning a type table. Human
and Android are bundled alongside Cleaning Bot. Android supports only Standing,
using `resources/test-worlds/android.agent.lua`. Cleaning Bot (`CleaningBot`,
`cleaning-bot.agent.lua`) is a compact wheeled Agent with a 0.4-unit width,
0.15-unit Standing height, 0.3-unit/s walking speed and its own
`cleaning-bot-standing` tile. It supports only Standing in Rooms and Doors.
Its default Mobility permits Door only: stairs, stairwells, escalators, ladders,
lifts, platform lifts, shuttles and Buttons are forbidden. It can traverse regular
manual/automatic Doors, but not button-operated Doors or Bulkhead Doors, and
cannot operate Access panels. This uses the existing button-required gate, not a
Door-subtype ban; a non-button Bulkhead Door follows ordinary Door eligibility.
As with other script defaults, authored Mobility overrides retain their normal
precedence. The type adds no autonomous cleaning behaviour.

Human's physical authority is
`resources/test-worlds/human.agent.lua`. The headless embed is generated from that
file during the build; there is no compiled Human subtype or second definition.

```lua
return {
    api_version = 2,
    type_id = "Example", -- stable identity, not a presentation label
    display_name = "Example Agent",
    new = function()
        return {
            width = 0.4,
            standing_height = 0.45,
            object_usage = "arms",
            object_usage_distance = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            poses = {
                standing = { image_tile = "human-standing" },
                sitting = { image_tile = "human-sitting", height_ratio = 0.6 },
                lying = { image_tile = "human-lying", height_ratio = 26 / 72, width_ratio = 72 / 26 },
                crouching = { image_tile = "human-crouching", height_ratio = 0.6 },
                crawling = { image_tile = "human-crawling", height_ratio = 0.3 },
            },
            automatic_poses = {
                room_movement = {
                    { pose = "standing", speed_ratio = 1 },
                    { pose = "crouching", speed_ratio = 1 },
                    { pose = "crawling", speed_ratio = 1 },
                },
                door_crossing = {
                    { pose = "standing", speed_ratio = 1 },
                    { pose = "crouching", speed_ratio = 1 },
                    { pose = "crawling", speed_ratio = 0.5 },
                },
            },
            mobility_profile = {
                staircase = "can_use", escalator = "can_use", stairwell = "can_use",
                ladder = "can_use", lift = "can_use", platform_lift = "can_use",
                shuttle = "can_use", door = "can_use", buttons = "can_use",
            },
            private_state = {}, -- retained privately, never persisted
        }
    end,
}
```

`type_id` is 1–128 ASCII letters, digits, underscores or hyphens. `display_name`
is non-empty, at most 128 bytes, and contains no control characters. `new` must
be a Lua function returning an instance table; missing fields never inherit
Human defaults. A constructor error publishes no partial Agent.

## Non-pose physical fields

| Lua field | Meaning | Validation |
| --- | --- | --- |
| `width` | Bodily width, World units; bounds and placement | Finite, positive |
| `standing_height` | Standing height, World units | Finite, positive |
| `object_usage_distance` | Arm length or maximum remote range, World units; ignored for None | Finite, positive for Arms/Remote control; Remote control omission defaults to 1 |
| `walk_speed` | Walking speed, World units/second | Finite, positive |
| `climb_speed` | Ladder speed, World units/second | Finite, positive |
| `stair_ascent_speed` | Stationary stair ascent, World units/second | Finite, positive |
| `stair_descent_speed` | Stationary stair descent, World units/second | Finite, positive |

All required numeric fields must be actual Lua numbers, finite and representable as positive
simulation floats. Numeric strings, NaN, infinities, zero, negatives, float
overflow/underflow, and out-of-range ratios are refused with field diagnostics.
Pose envelopes use declared height and width ratios rather than exchanging
Standing dimensions for Lying. Individual physical modifiers and persisted Agent tag samples still
apply, with individual-over-tag precedence. Permissions, route preferences, shared-resource slot geometry and Escalator belt
policies have not moved into type scripts.

## Object usage and arm length (#535)

`object_usage` accepts the actual Lua strings `"arms"`, `"none"` and `"remote_control"`; omission defaults to
Arms for compatibility. See [Remote operation](remote-button-operation.md) for physical Buttons, ordinary
manual Doors and onboard Lift/Platform lift destination selection, including
World authoring examples. Onboard Shuttle selectors remain unsupported.
In Arms mode, `object_usage_distance` is independently frozen and observed, not
inferred from the usage mode. It must be a concrete finite, strictly positive
simulation float; None's unused observation is described below.
The legacy `reach` field is an input alias for this distance and also defaults to
Arms. Declare at most one of `reach` and `object_usage_distance`: declaring both
is rejected, even with equal values. Remote control refuses legacy `reach`; use `object_usage_distance` or omit it for the 1-unit remote script default. Missing both is an error for Arms. Human uses Arms
with 0.25 units; Cleaning Bot uses Arms with 0.1 units. Android demonstrates the
compatible legacy declaration.

These are frozen defaults, not compulsory effective values. Object usage and its
concrete distance can be inherited independently from Agent tags or overridden
individually (#537, #538), using individual → tag → frozen default precedence.
Removing a property reveals the underlying source without changing the other field;
shared edits preflight all loaded affected Agents. See [Agent properties](agent-properties.md).

Physical eligibility is shared through World: the operator must be within both
its effective arm length and the Interaction point's authored reach. Point geometry
is unchanged and zero reach remains valid (requiring coincident positions).
Ordinary requests still walk to a physical control when outside range; owned
BoothWindow shutters and Dumbwaiter buttons still require range at request time.
Physical Buttons, onboard selectors, Access panels and shutter operations retain
Location/side, height, activation, Mobility, Access permission and Broken checks,
press durations and queues. Onboard selectors retain their passenger-local
placement. Manual ordinary Door opening also requires arm length at the source
threshold; a short-armed queue head approaches it without changing FIFO order,
crossing bands, reservations or admission. Already-open Doors require no arm
operation, and automatic presence sensing is unaffected.

Both observations have the existing frozen-baseline lifetime policy: creation,
paste, reopen, Reset and deleted-Agent restoration validate fresh defaults;
surviving structural edits and history replay retain them. Live Lua mutations
cannot replace the copied defaults. Documents, clipboard and history do not
serialize private Lua state or materialize defaults as individual properties.

### None usage (#536)

A type can replace the usage/distance entries in the complete example with:

```lua
object_usage = "none",
-- No object_usage_distance or reach is needed.
```

None ignores the distance field (including any supplied value); the host freezes
its unused distance observation as zero. Other physical fields and the complete
Mobility/pose declarations remain required. Usage is validated before publication;
unknown spellings, wrong types and embedded NULs are rejected atomically. Omitted
usage still means Arms and needs a positive distance; there is no implicit None
fallback for incomplete legacy resources.

None refuses ordinary manual Door opening, every physical Button/Interaction
request, transport landing calls and onboard selections, Access panels and
BoothWindow shutters. It cannot press a passing Button or become a traversal's
preparation operator. Routes requiring its own operation are hard exclusions,
not expensive choices or waits at impossible controls. Ordinary automatic Doors,
automatic Bulkhead Door presence sensors and automated Chambers remain usable.
Administrative/device-internal commands are not Agent operations.

Operation-free passage is separate from inability to operate. A locally observed
open Door or extended Force Bridge/Ladder may be used under existing permissions,
Permission adherence, Mobility, clearance and admission rules. A None passenger
may join a locally boardable Lift, Platform lift or Shuttle only when the desired
journey was already accepted; remote live requests are not revealed. Boarding
rechecks this condition, and None never creates the missing selection. Expired
or incompatible assistance produces normal replanning/Route loss rather than
stranded pending work. Accepted destinations, admitted crossings, Airlock exits
and committed transport exits retain their existing completion guarantees.
Permission adherence tests authorization, not whether the Agent has arms; None
receives no authorization exception and cannot bypass Buttons Mobility.

None follows the same fresh/surviving lifetime policy as Arms. Switching a script
back to Arms requires its complete valid distance before constructing a new
lifetime; it does not alter a surviving Agent or persist a runtime default as an
override.

### Verification

Release CTest `object-usage-*` checks use public World operations and real ticks.
`agentTypesNone*` covers distance omission/ignoring, direct refusals, route loss
and automatic alternatives, open-Door authorization/adherence/Mobility and
admitted-crossing completion, extended/retracted bridges, automatic Bulkhead
Doors and Chambers, shared Airlocks and all three transport kinds, coordination
cleanup, fresh YAML/binary reopen and Reset, and structural/history/clipboard/
deleted-Agent lifetime boundaries. Existing invalid-baseline checks retain atomic
validation and legacy omission coverage.

The Arms checks use the same seams
for legacy/new declarations, independent snapshots, short/long arm lengths,
unchanged point geometry (including zero reach), owned shutters and Access
panels, manual Door queue approach, and onboard Lift selection. Invalid usage,
competing aliases and invalid distances extend `agentTypesInvalidBaselinesRejected`,
including no partial Agent, events or document changes. Existing revision/load/
Reset and editor structural-history/clipboard/deleted-Agent checks now assert
frozen Arms observations and fresh distances at their lifetime boundaries.

## Required pose declarations and v1 migration

`poses` contains only supported canonical keys: `standing`, `sitting`, `lying`,
`crouching`, `crawling`. Standing is mandatory. Every supported pose requires
`image_tile`: an ObjectAtlas image name, a nonempty actual Lua string of at most
128 bytes with no NUL or control characters. Standing allows only this field
and uses the type's base dimensions. All other supported poses, including Lying,
require `height_ratio`, an actual finite Lua number that converts
to a finite positive simulation float. Sitting and Lying allow `(0, 1]`; Crouching and
Crawling require `(0, 1)`, and Crawling must be strictly lower than Crouching when
both are supported. Equality after float conversion is also rejected. Their vertical extent is
that ratio times effective Standing height. Non-Standing poses may also declare
`width_ratio`, defaulting to 1. It must be a finite positive simulation float
and produce a finite positive bodily width; values greater than 1 are allowed.
Width is the base type width times this ratio. Human's Lying ratios are `26/72`
and `72/26`, matching its 72x26 Lying versus 26x72 Standing tiles.
Unsupported poses have no envelope, default ratio or tile.
Tile names are frozen for each lifetime, like height ratios; the renderer selects
the current pose's tile as authored, with no pose-specific rotation, mirroring or
squashing. Tile dimensions relative to the type's Standing tile determine artwork
size; ordinary Height scaling, tint and Furniture offsets remain. Height ratios
are physical, not rendering transforms. Human has five dedicated `human-*`
tiles, with lowered/horizontal stances baked into the atlas by
`scripts/pack_human_pose_tiles.py`. Other types may share artwork, but must
supply already-posed images for distinct appearances. If artwork is unavailable,
the existing glyph fallback remains; core validation does not resolve the atlas.
Existing API-v2 definitions must add `image_tile` to every supported pose.

`automatic_poses` requires exactly `room_movement` and `door_crossing`. Each is a
nonempty dense ordered array beginning with Standing, without duplicates, naming
only declared Standing/Crouching/Crawling poses. Each entry has exactly `pose`
and `speed_ratio`; speed ratios must be finite positive simulation floats in
`(0, 1]`. Both contexts try Standing, then Crouching, then Crawling, selecting the
tallest allowed pose that fits, regardless of array order or speed. Supporting a
pose does not require listing it in either context.
Capabilities, ratios and context choices cannot be replaced by properties or tags; Height
still modifies effective dimensions. Surrounding private instance fields remain
allowed; unknown fields within the pose contract are rejected.

External API v1 resources are refused with explicit migration guidance. Change
`api_version` to 2, replace `sitting_height_ratio`, `crouching_height_ratio`, and
`crawling_height_ratio` with supported `poses` entries, and replace
`crawling_speed_ratio` with the relevant context choice's `speed_ratio`. Supply
both automatic contexts; do not declare unsupported poses. The migrated Human
example above preserves historical geometry, Room order/speeds and half-speed
Crawling Door crossings. A Standing-only type needs only:

```lua
poses = { standing = { image_tile = "agent" } },
automatic_poses = {
    room_movement = {{ pose = "standing", speed_ratio = 1 }},
    door_crossing = {{ pose = "standing", speed_ratio = 1 }},
},
```

There is no external v1 adapter or implicit all-poses fallback. Legacy Human
World records still resolve the revised bundled Human resource without a World
schema change. Persistence does not store capabilities, orders or runtime poses.

## Required default Mobility profile

`mobility_profile` is a required complete table on the instance returned by
`new()`. It must contain exactly the nine entries shown above: `staircase`,
`escalator`, `stairwell`, `ladder`, `lift`, `platform_lift`, `shuttle`, `door`,
and `buttons`. Each value is exactly `"can_use"`, `"cannot_use"`, or
`"only_if_no_other_option"`. Missing, unknown, malformed, or invalid entries
refuse construction with the resource and entry in the diagnostic.

The host copies and freezes this complete profile with the physical baseline
before publishing the Agent. It is the fallback when neither an individual
Mobility profile nor an inherited tag profile applies; individual → tag → script
default is complete replacement, not per-entry merging. The script default is
not persisted as an individual property or queried from Lua after construction.
Removing an override exposes the next source. New tag profiles continue to begin
with every entry `can_use`; adding an individual profile in the editor snapshots
the Agent's current effective profile. As with every individual Agent property,
adding, editing, or removing that snapshot requires a paused simulation.

## Live object versus frozen baseline

Each World owns a budgeted sandbox runtime; every Agent executes its type module
in a fresh private environment and retains its own live instance, including
private tables, closures and methods. Source can be shared; mutable module
upvalues cannot. Before publication the host validates and freezes non-pose
fields, supported poses and automatic choices into the physical baseline used by movement, poses, clearance, route
estimates, rendering, bounds and placement. Later Lua changes cannot alter that
baseline. Type ID/resource identity cannot be changed on an existing Agent.

The instance returned by `new()` must not define a `behaviour` member, even a
function or a non-function value. Preflight and live construction reject it as
reserved for the runtime-owned Installed behaviour. This is a host-mediated
association in the separate behaviour runtime, not a Lua closure stored on the
type instance. The reservation does not bump the Agent-type API version; it does
not let type scripts install default behaviours. See
[ADR 0021](adr/0021-run-installed-agent-behaviours-as-coroutines.md) and the
[Host API v3 behaviour guide](agent-behaviour-packages.md).

There are no automatic callbacks, method calls from behaviours or Actions, or
cross-script invocation API. Agent behaviours retain movement-intent ownership;
Actions and Furniture use retain their existing responsibilities (ADRs 0008 and
0018). The retained methods are private and unscheduled in this slice.

The sandbox prohibits filesystem, process, native-module, package, debug and
unrestricted loading capabilities; it exposes no mutable domain objects.
Module evaluation and constructors have execution and allocation budgets. Budget
exhaustion is terminal even inside `pcall` or `xpcall`; ordinary caught script
exceptions do not gain unsafe capabilities. The default live World runtime is
64 MiB with 100,000 instructions per call; preflight uses a separate 2 MiB /
100,000-instruction scratch context. Source is limited to 1 MiB.

## Resources, selection and previews

Startup loads manifest-registered resources through Willpower under ADR 0010.
Human is selected by default. Valid arbitrary types need no C++ subtype;
invalid resources are unavailable for selection. Existing-Agent inspection is
read-only. Creation previews run the same constructor/validation in an isolated,
bounded scratch context; they do not create a World Agent, consume its private
state, or share mutable state with placement. Drop clamping uses the selected
baseline width, not Human width. Failed previews prevent placement.

See [external Agent types](external-agent-types.md) for explicit file import,
managed resource names, diagnostics and portability. Bundled references use
manifest names; imports use a canonical-path-encoded Resource name. No directory
scanning, generated scripts, file overwrites or parallel path cache is added.
Competing resources declaring one type ID within a World are refused.

## Persistence, revisions and lifetime boundaries

YAML/binary Worlds and clipboard/history persist authored type ID and resource
reference plus existing Agent data, never baseline snapshots or arbitrary Lua
state. The historical `type` wire slot writes the stable ID too; old display-name
records remain readable when `typeId` supplies identity. Display names may change
in a resolved revision without changing identity or invalidating old documents.
Legacy Human records with omitted identity/resource resolve bundled Human;
explicit missing, invalid or mismatched dependencies fail, never become Human.

| Operation | Instance, physical baseline and default Mobility |
| --- | --- |
| Ordinary creation | Fresh constructor from the World-registered source |
| Preview | Independent scratch constructor; no World publication |
| On-disk edit or repeat import | Existing instances unchanged; no hot reload |
| Load or Reset | Fresh constructor from the currently resolved resource revision |
| Ordinary paused topology edits | Preserve surviving live instances and frozen baselines |
| Structural undo/redo with a surviving authored Agent | Preserve by stable Agent ID and type/resource identity |
| Delete/cut | Ends that instance lifetime; history stores authored data only |
| Undo deletion or redo placement | Fresh constructor from the currently resolved revision |

The frozen-default policy applies equally to the physical baseline (including
supported poses, envelope parameters and ordered context choices) and the complete
script Mobility profile. An on-disk revision cannot change a surviving Agent's
traversal constraints, including through a paused edit or structural undo/redo.
Fresh lifetimes validate and freeze the currently resolved definition's Mobility
profile even when an individual or tag profile masks it. Ordinary creation/paste
into a World with the type already registered uses that World's registered source;
copying into a new World resolves its dependency before fresh construction.

Reset preserves authored properties, tags, samples and identity. YAML and binary
save/load, clipboard and history carry authored individual profiles and tag
assignments, not script defaults materialised as overrides. Removing an override
therefore reveals the applicable tag or the current lifetime's frozen default,
not a persisted default from an earlier lifetime. Bundled Human and Android retain
all–Can use defaults, including legacy Human documents.

Failed Reset or history reconstruction (including invalid `mobility_profile`)
leaves the current World/history unchanged and reports the resource and offending
field. An override does not exempt a fresh constructor from validation. Surviving
Agent-type lifetime preservation is explicitly different from Agent **behaviour**
reconstruction under ADR 0008. Opaque instance handles retain their owning runtime
safely across World replacement and application resource teardown (ADR 0019).

## Integration evidence

See [API v2 declaration validation](agent-pose-declarations-validation.md) for
#521 coverage and final Release results, and [scripted Agent integration](scripted-agent-integration.md)
for the historical #510 build/test results and public-seam coverage. No Debug or unmodified submodule
tests are part of this migration's validation contract.
