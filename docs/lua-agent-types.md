# Lua Agent-type authoring and integrated migration (#510)

> **API v2 declarations (#521) and shared fit/selection/movement (#522) are implemented.**
> Supported poses and ordered automatic choices are validated and frozen per lifetime.
> Room/Door motion, placement, routing and admission consume the shared fit result.
> Furniture eligibility and full lifecycle/edit enforcement remain follow-ups (#523–#524).
> See [selection validation](agent-pose-selection-validation.md).
> See [Agent pose capabilities](agent-pose-capabilities.md) and [ADR 0020](adr/0020-declare-agent-pose-capabilities-and-furniture-pose-requirements.md).

An Agent type is one managed `.agent.lua` resource returning a type table. Human
is the only bundled production type; Scout and Standing Robot are regression
fixtures, not additional production types. Human's physical authority is
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
            reach = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            poses = {
                standing = {}, sitting = { height_ratio = 0.6 }, lying = {},
                crouching = { height_ratio = 0.6 }, crawling = { height_ratio = 0.3 },
            },
            automatic_poses = {
                room_movement = {
                    { pose = "standing", speed_ratio = 1 },
                    { pose = "crouching", speed_ratio = 1 },
                    { pose = "crawling", speed_ratio = 1 },
                },
                door_crossing = {
                    { pose = "standing", speed_ratio = 1 },
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

## The seven required non-pose physical fields

| Lua field | Meaning | Validation |
| --- | --- | --- |
| `width` | Bodily width, World units; bounds and placement | Finite, positive |
| `standing_height` | Standing height, World units | Finite, positive |
| `reach` | Interaction reach, World units | Finite, positive |
| `walk_speed` | Walking speed, World units/second | Finite, positive |
| `climb_speed` | Ladder speed, World units/second | Finite, positive |
| `stair_ascent_speed` | Stationary stair ascent, World units/second | Finite, positive |
| `stair_descent_speed` | Stationary stair descent, World units/second | Finite, positive |

All fields must be actual Lua numbers, finite and representable as positive
simulation floats. Numeric strings, NaN, infinities, zero, negatives, float
overflow/underflow, and out-of-range ratios are refused with field diagnostics.
Lying retains the existing policy of exchanging effective Standing height and
width. Individual physical modifiers and persisted Agent tag samples still
apply, with individual-over-tag precedence. Permissions, route preferences, shared-resource slot geometry and Escalator belt
policies have not moved into type scripts.

## Required pose declarations and v1 migration

`poses` contains only supported canonical keys: `standing`, `sitting`, `lying`,
`crouching`, `crawling`. Standing is mandatory. Standing and Lying have empty
parameter tables; Lying's vertical extent is bodily width. Sitting, Crouching and
Crawling require exactly `height_ratio`, an actual finite Lua number in `(0, 1]`
that converts to a finite positive simulation float. Their vertical extent is
that ratio times effective Standing height. Width and rendering orientation retain
existing policy. Unsupported poses have no envelope or default ratio.

`automatic_poses` requires exactly `room_movement` and `door_crossing`. Each is a
nonempty dense ordered array beginning with Standing, without duplicates, naming
only declared Standing/Crouching/Crawling poses. Each entry has exactly `pose`
and `speed_ratio`; speed ratios follow the same numeric validation as height
ratios. Supporting a pose does not require listing it in either context.
Capabilities, ratios and orders cannot be replaced by properties or tags; Height
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
poses = { standing = {} },
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
not a persisted default from an earlier lifetime. Bundled Human and Scout retain
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
