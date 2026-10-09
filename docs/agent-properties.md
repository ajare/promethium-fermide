# Agent properties

This is the complete reference for the **19 editable Agent properties** declared
by `AgentPropertyType` in `include/core/AgentTag.h`. Each can be authored directly
on an Agent or supplied by an Agent tag. They are distinct from the physical
baseline declared by an [Agent-type script](create-agent-script.md), and from
runtime state such as Pose, destination and movement progress.

## Editing and inheritance

Pause the simulation, select an Agent, and use **Individual properties →
Properties...** to add or remove an override. Edit reusable tag properties in the
Tags panel. The effective value follows this order:

1. Individual Agent property.
2. Inherited Agent tag property (or its persisted sample).
3. Default shown below. Mobility, Object usage and Object usage distance instead
   fall back to their independently frozen Agent-type script defaults.

Removing an individual override reveals the inherited value without removing
the tag or discarding its sample. Two assigned tags cannot supply the same
property, even if an individual override would hide the conflict. Adding a
property to a tag also checks for conflicts among its assigned Agents.

A property **namespace** is only an editor grouping, not part of its name or
serialized key. Colour, Walk speed modifier, Height modifier, Object usage and Object usage distance are in the
unlabelled group; all other properties are in **Pathing**.

### Values versus tag ranges

Individual numeric properties are concrete values. For the 13 numeric modifier/planning properties, tags specify a minimum/maximum range. Both
endpoints must be finite, within the property's allowed bounds, and ordered
minimum ≤ maximum. Equal endpoints produce a fixed value.

Each assigned Agent receives an independent uniform sample from the tag range.
The sample, source tag and property revision are persisted; simulation ticks,
ordinary load and Reset do not repeatedly draw a new value. Assigning a tag or
changing its range creates the relevant new samples. An individual override
hides, rather than replaces, the underlying tag sample.

Colour, Escalator walking chance, Object usage, Object usage distance,
Permission adherence and Mobility profile are
shared values on tags, not sampled ranges. A tag's intrinsic pastel **display
Colour** colours its UI chip; it is separate from the optional Colour property
that colours its Agents.

## Complete property catalogue

Ranges are inclusive. Defaults mean the effective fallback when no individual
or inherited property supplies a value, not necessarily the value inserted by
an editor widget when adding an override. Numeric values must be finite except an ignored Object usage distance under None.

| Property | Serialized `type` | Type / allowed values | Default | Effect |
| --- | --- | --- | --- | --- |
| Object usage | `objectUsage` | Arms or None | Frozen script mode | Ability to operate objects, independent of Access permission and Mobility |
| Object usage distance | `objectUsageDistance` | Concrete World-unit distance; finite and positive for effective Arms | Frozen script distance | Physical arm length, narrowed by Interaction point geometry; ignored under None |
| Colour | `colour` | RGB bytes, each 0–255; no alpha | RGB (179, 77, 77), `#B34D4D` | Ordinary Agent rendering tint; no simulation effect |
| Walk speed modifier | `walkSpeedModifier` | Number, 0.8–1.2 | 1 | Multiplies the Agent type's walking speed |
| Height modifier | `heightModifier` | Number, 0.7–1 | 1 | Multiplies Standing height and derived pose heights; affects appearance and physical fit |
| Escalator walking chance | `escalatorWalkingChance` | Probability, 0–1 | 0 | Chance of walking rather than standing on a moving Escalator |
| Stair speed modifier | `stairSpeedModifier` | Number, 0.5–1.5 | 1 | Multiplies stationary Staircase/Stairwell ascent and descent speeds |
| Ladder speed modifier | `ladderSpeedModifier` | Number, 0.5–1.5 | 1 | Multiplies Ladder climbing speed |
| Interaction aversion | `interactionAversion` | Number, 0–3 | 1 | Weights inconvenience of interactions in perceived route cost |
| Effort aversion | `effortAversion` | Number, 0–3 | 1 | Weights physical effort in perceived route cost |
| Waiting aversion | `waitingAversion` | Number, 0.5–3 | 1 | Weights known and expected waiting in perceived route cost |
| Crowd aversion | `crowdAversion` | Number, 0–3 | 1 | Weights crowding discomfort in perceived route cost |
| Risk aversion | `riskAversion` | Number, 0–3 | 1 | Weights risk-bearing traversals in perceived route cost |
| Route familiarity | `routeFamiliarity` | Number, 0–1 | 0.5 | Reduces uncertainty premiums and stable perception variation |
| Route persistence | `routePersistence` | Number, 0–1 | 0.15 | Sets the proportional improvement needed to replace a still-valid Path voluntarily |
| Minimum route planning time | `minimumRoutePlanningTime` | Seconds, 0.1–10 | 1 second | Lower endpoint for a Route planning episode's duration |
| Maximum route planning time | `maximumRoutePlanningTime` | Seconds, 0.1–10 | 3 seconds | Upper endpoint for a Route planning episode's duration |
| Permission adherence | `permissionAdherence` | Boolean | `true` | Willingness to decline usable resources whose applicable operation permissions are unsatisfied |
| Mobility profile | `mobilityProfile` | Complete nine-entry profile; see below | Frozen Agent-type script profile | Hard traversal constraints and last-resort routing rules |

The editor currently inserts **0.5** when adding an individual Route persistence
property; that is an explicit override, not the absent-property default of 0.15.
Adding an individual Mobility profile snapshots the current effective profile;
a newly added tag profile starts with all entries **Can use**.

## Independent Object usage inheritance (#537, #538)

Mode and distance resolve independently: individual override → inherited tag value →
frozen script default. Different tags may supply mode and distance, but two assigned
tags cannot supply the same field, even when masked by an individual override. The Selection panel shows both effective values and their
sources, and labels distance as ignored under None. Only Arms and None are exposed;
Remote control is not implemented. Adding an override starts from its current effective
value, not a sampled range. Removal leaves the other override and script baseline intact.

Every edit and removal is paused-only and undoable. Arms requires the resulting effective
distance to be finite and strictly positive, even when removing an override or changing
back from None. None ignores distance, including zero or negative authored values. A
script-default None Agent has canonical zero frozen distance, so enabling Arms requires
an independent positive distance property first, either individual or inherited.
Shared tag additions, edits, removals and reloads preflight every affected Agent in every
loaded dependent World. All those Worlds must be paused. A refusal preserves definitions,
revisions, assignments, Worlds and history; removing a property must also leave a valid
effective configuration. Refused edits are atomic.

Tag Object usage distance is concrete and deterministic, never a range or sample. The
Tags panel inserts Arms and 0.25 for new fields, not each Agent's script default. For
example, a tag can supply `objectUsage: none` while a second supplies distance 0; adding
Arms to an affected Agent then refuses until its effective distance becomes positive.
A mode-only tag or individual override never silently chooses a different distance.

Registry schema 15 persists these fields as ordinary revision-bearing properties:

```yaml
properties:
  - type: objectUsage
    revision: 1
    value: arms
  - type: objectUsageDistance
    revision: 2
    value: 0.6
```

Removing the distance property reveals each Agent's independently frozen distance;
removing mode reveals its frozen mode. Individual overrides keep taking precedence.

Capability changes cancel now-ineligible pending operations and reconsider Paths through
the existing planning rules; admitted crossings and accepted journeys finish safely.
Arms remains physical: short arms approach controls and never activate them remotely.

World schema 62 (YAML and binary), clipboard and history carry only optional authored
overrides, never snapshots of live Lua defaults. Reset, load, deletion restoration and
cross-World paste validate fresh script defaults plus the overrides and inherited fields; ordinary structural
and surviving history replay retain the Agent's frozen defaults. Legacy Agents without
overrides continue to use their script defaults.

## Appearance and physical movement

### Colour

The Colour property supplies an opaque RGB tint. Selection replaces the ordinary
colour with the selection highlight, RGB (251, 188, 4). Changing Colour does not
change physical capabilities, route choice or permissions.

### Walk speed modifier

Effective walking speed is:

```text
Agent-type walk_speed × Walk speed modifier
```

It affects walking motion and its route estimates, including the walking
contribution on a moving Escalator. It is not a universal multiplier: Ladder and
stationary stair speeds have their own modifiers, and device speeds remain
independent. Script-declared pose/context speed ratios apply where appropriate.

### Height modifier

Effective Standing height is `standing_height × Height modifier`. Other supported
poses derive their height from that value using the type's frozen height ratios.
This changes fit, including ordinary Door clearance, and rendered height. It does
not change the type's supported poses or bodily width, which uses the type's width
and pose width ratio.

Height edits and changes of inherited Height can be refused if the resulting
Agent cannot fit its current supported state. Merely choosing a value within the
numeric range is not sufficient to guarantee a valid placement or edit.

### Stair and Ladder speed modifiers

- Stair speed modifier scales the type's `stair_ascent_speed` and
  `stair_descent_speed` on stationary Staircases and Stairwells. It does not scale
  moving Escalator belt speed. A Broken Escalator is stationary and uses stair
  movement rules.
- Ladder speed modifier scales the type's `climb_speed` for physical climbing and
  route estimates. It does not change reach or authorization to extend a Ladder.

### Escalator walking chance

A value of 0 means always stand; 1 means always choose walking. The host makes
one deterministic draw when an admitted moving-Escalator traversal begins, using
World seed, Agent identity and traversal sequence. It is not redrawn every tick.
Walking adds the effective walking speed to belt speed. Route estimates use
the probability and locally observed congestion, not foreknowledge of the
eventual draw.

## Route preferences

These properties change **perceived route cost**, a seconds-equivalent preference
score, not actual elapsed journey time. They cannot make an infeasible traversal
possible or grant permission. Higher aversion increases the corresponding cost;
zero removes that particular premium where the allowed range includes zero.

- **Interaction aversion**: weights interaction inconvenience, such as Door
  activation, threshold interaction and resource boarding/mounting. It does not
  disable Buttons or alter device activation duration.
- **Effort aversion**: weights physical effort, including stairs, Ladder climbing
  and walking on an Escalator. It does not reduce physical speed.
- **Waiting aversion**: weights both known and expected waits. It does not alter
  queue priority, capacity or device timing.
- **Crowd aversion**: weights crowding estimates, including locally observed
  access-zone density. It does not reveal remote live queues or reserve space.
- **Risk aversion**: weights finite risk premiums, such as those on Ladders and
  exposed Force Bridges. It changes preference, not feasibility or accidents.
- **Route familiarity**: 0 retains the largest uncertainty influence; 1 removes
  the uncertainty premium. Familiarity also reduces stable perception variation,
  but never provides perfect knowledge of remote device state or queues.

### Route persistence

A still-valid Path is replaced voluntarily only if the alternative improves its
perceived cost by **more than** both thresholds:

```text
currentCost - alternativeCost > max(2, currentCost × Route persistence)
```

The absolute threshold is currently 2 seconds-equivalent, defined by the default
route-cost policy. At the default 0.15, a current cost of 100 requires an
improvement greater than 15. Even persistence 0 still requires an improvement
greater than the absolute threshold. Hard Path invalidation bypasses voluntary
persistence checks: persistence is not a lock on a broken route.

### Minimum and Maximum route planning time

Route planning is an active but stationary movement state. Every planning episode
samples an inclusive whole-tick duration between the effective bounds. Bounds
are in **seconds**, rounded upward to ticks (60 ticks/second), unlike Behaviour
`sleep(ticks)`, whose input is already ticks.

If Maximum is lower than Minimum, consumption raises Maximum to Minimum without
rewriting authored values or tag samples. Changing either property affects the
next episode, not an already-running interval. Pause and deactivation freeze the
remaining interval. At expiry the host calculates a Path synchronously; successful
movement starts on the following tick. See [Route planning](route-planning.md).

## Permission adherence

Permission adherence is willingness, not authorization:

- **true**: decline applicable opportunistic use when operation-permission
  requirements are not satisfied, even if the resource is already usable.
- **false**: allow applicable opportunistic use of an already usable resource
  without those operation permissions. This does not allow the Agent to operate
  a protected control.

It applies to ordinary manual/control-operated Doors, extended Force Bridges,
extended extensible Ladders, and applicable transport/Airlock control requirements.
Accepted crossings and committed journeys still complete safely; adherence does
not trap admitted passengers. Independent Location permission requirements still
constrain passage into protected Rooms and Corridors.

Access permissions and Permission sets are separate assignments, not additional
Agent properties in this catalogue. See [ADR 0015](adr/0015-authorize-control-operation-not-resource-passage.md).

## Mobility profile

A profile contains **all nine** entries. Individual → tag → script default is
complete replacement, never per-entry merging. Bundled Human's script default is
all Can use; custom Agent types may declare other defaults.

| Entry | Lua script key | Serialized traversal | Scope |
| --- | --- | --- | --- |
| Staircase | `staircase` | `staircase` | Stationary Staircases |
| Escalator | `escalator` | `escalator` | Moving Escalators; Broken stationary Escalators use Staircase classification |
| Stairwell | `stairwell` | `stairwell` | Stairwells |
| Ladder | `ladder` | `ladder` | Ladders |
| Lift | `lift` | `lift` | Lifts |
| Platform lift | `platform_lift` | `platformLift` | Platform lifts |
| Shuttle | `shuttle` | `shuttle` | Shuttles |
| Door | `door` | `door` | Ordinary and Bulkhead Doors |
| Buttons | `buttons` | `buttons` | Operating Interaction points and routes through authored button-requiring resources |

Each entry is one of:

| Editor value | Lua script value | Serialized use | Meaning |
| --- | --- | --- | --- |
| Can use | `can_use` | `canUse` | Consider normally, subject to other constraints |
| Cannot use | `cannot_use` | `cannotUse` | Exclude from routing and refuse at runtime admission |
| Only if no other option | `only_if_no_other_option` | `onlyIfNoOtherOption` | Exclude in the first route search; admit in a second search only if the first finds no Path |

When both a traversal kind and Buttons apply, the most restrictive use wins:
Cannot use, then Only if no other option, then Can use. Button requirements are
classified from authored resource requirements, not momentary open/extended/queue
state. Mobility is independent of Permission adherence: non-adherence cannot
bypass Cannot use. See [ADR 0011](adr/0011-enforce-mobility-profiles-as-routing-constraints.md).

## Persistence example

Within a serialized Agent record, individual overrides use `individualProperties`:

```yaml
individualProperties:
  - type: colour
    r: 80
    g: 160
    b: 220
  - type: walkSpeedModifier
    value: 1.1
  - type: minimumRoutePlanningTime
    value: 0.5
  - type: maximumRoutePlanningTime
    value: 1.5
  - type: permissionAdherence
    value: false
```

All numeric overrides use `value`; Colour uses `r`/`g`/`b`; Mobility uses a
`uses` array with all nine `{ traversal, use }` entries. For example, its Platform
lift entry is `{ traversal: platformLift, use: canUse }`. These serialized names
are not the snake-case names used in Lua type scripts. Prefer the editor for authoring, especially for tag revisions
and sample provenance. Saving persists individual values and inherited samples,
not live destinations, current Pose, planning timers, coroutine stacks or frozen
script baselines.

## What is not an Agent property?

- **Agent type ID/resource and physical baseline**: supplied by the type script
  and frozen per lifetime. See [Agent script authoring](create-agent-script.md)
  and [the complete API v2 contract](lua-agent-types.md).
- **Name, Agent group, Agent tags, permission assignments and Behaviour
  assignment/configuration**: separate authored fields or associations. Groups
  are administrative; tags supply properties.
- **Position, Local depth, Pose, Support elevation, activation, destination,
  selected Action, Path and movement state**: observations or runtime state, not
  extra entries in the property system. Behaviours issue intent through the host
  API rather than writing these as mutable properties.
- **Behaviour private variables**: coroutine/closure state, neither properties
  nor saved data. See [Behaviour script authoring](create-behaviour-script.md).

## Implementation references

- [`include/core/AgentTag.h`](../include/core/AgentTag.h): complete property enum,
  display names/namespaces, bounds and tag defaults.
- [`src/core/AgentTag.cpp`](../src/core/AgentTag.cpp): numeric validation and sampling.
- [`src/core/Agent.cpp`](../src/core/Agent.cpp): effective lookup and serialization.
- [`include/core/RouteCost.h`](../include/core/RouteCost.h): preference weights and
  voluntary Path replacement threshold.
- [ADR 0012: individual overrides](adr/0012-individual-agent-properties-override-tag-properties.md)
- [ADR 0014: perceived cost versus traversal facts](adr/0014-separate-perceived-route-cost-from-traversal-facts.md)
