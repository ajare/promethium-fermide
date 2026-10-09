# Script-defined Agent poses and Furniture pose requirements

Status: implemented (#521–#524). API v2 declarations, shared fit/selection,
Furniture eligibility, ownership, edit preflight and supported committed completion
are integrated. See [shared selection validation](agent-pose-selection-validation.md)
and [lifecycle validation](agent-pose-lifecycle-validation.md).
See [ADR 0020](adr/0020-declare-agent-pose-capabilities-and-furniture-pose-requirements.md).
The current authoring contracts are documented in [Lua Agent types](lua-agent-types.md)
and [Lua Furniture catalogues](lua-furniture-catalogues.md).

## Purpose

An Agent type must be able to describe which bodily Poses it can perform. Human
supports Standing, Sitting, Lying, Crouching, and Crawling; a basic robot may
support Standing only. Neither low-ceiling movement nor committed-exit recovery
may manufacture an unsupported pose. Furniture must advertise its use and finish
postures so an Agent is offered use only when it can fulfil both.

## Agent-type API v2

The instance returned by `new()` supplies `poses` and `automatic_poses` alongside
its existing non-pose physical fields and complete default Mobility profile.
The host validates and freezes the resulting declarations for the Agent lifetime,
just like its physical baseline. They are not queried from live Lua each tick.

The following is the implemented pose portion of Human's v2 instance; it is not a
complete runnable Agent resource:

```lua
poses = {
    standing = { image_tile = "human-standing" },
    sitting = { image_tile = "human-sitting", height_ratio = 0.6 },
    lying = { image_tile = "human-lying", height_ratio = 26 / 72, width_ratio = 72 / 26 },
    crouching = { image_tile = "human-crouching", height_ratio = 0.6 },
    crawling = { image_tile = "human-crawling", height_ratio = 0.3 },
},
automatic_poses = {
    room_movement = {
        { pose = "standing", speed_ratio = 1.0 },
        { pose = "crouching", speed_ratio = 1.0 },
        { pose = "crawling", speed_ratio = 1.0 },
    },
    door_crossing = {
        { pose = "standing", speed_ratio = 1.0 },
        { pose = "crouching", speed_ratio = 1.0 },
        { pose = "crawling", speed_ratio = 0.5 },
    },
},
```

A standing-only robot's corresponding declarations are:

```lua
poses = { standing = { image_tile = "agent" } },
automatic_poses = {
    room_movement = {{ pose = "standing", speed_ratio = 1.0 }},
    door_crossing = {{ pose = "standing", speed_ratio = 1.0 }},
},
```

### Validation and migration

- The canonical vocabulary remains `standing`, `sitting`, `lying`, `crouching`,
  and `crawling`; no custom identities are introduced. Standing is required.
- Every supported pose requires `image_tile`, a nonempty ObjectAtlas image name
  of at most 128 bytes with no NUL or control characters. It is frozen for the
  lifetime and selected by the renderer for the current pose. Artwork supplies
  the stance; the renderer does not rotate, mirror or squash it based on Pose.
  Human uses dedicated `human-*` tiles with transforms baked into the atlas.
  Native tile dimensions relative to Standing determine visual size, with
  ordinary Height scaling, tint, Furniture offsets and glyph fallback retained.
  Missing or invalid tile declarations are
  diagnosed; core does not resolve graphics resources. Existing v2 scripts must
  add this field to every supported pose.
- Only supported poses are present. Sitting, Lying, Crouching, and Crawling
  require a finite positive `height_ratio`. Sitting and Lying permit `(0, 1]`; Crouching and Crawling
  require `(0, 1)`, with Crawling strictly lower than Crouching when both are
  supported, including after simulation-float conversion. Standing uses the
  base dimensions without configurable ratios. Other poses may also supply
  `width_ratio` (default 1), finite and positive, including values above 1; the
  resulting base-width product must remain finite and positive. A standing-only
  type supplies no dimension ratios.
- Both automatic context lists are required, nonempty ordered dense arrays,
  start with Standing, have no duplicate poses, and reference only supported
  locomotion poses: Standing, Crouching, or Crawling. Supporting a pose does not
  require listing it in every automatic context.
- Every choice supplies a finite `speed_ratio` in `(0, 1]`. Reject malformed
  declarations and unknown fields within the new pose contract with contextual
  diagnostics. Preserve the surrounding instance's private-state contract.
- Replace `sitting_height_ratio`, `crouching_height_ratio`,
  `crawling_height_ratio`, and `crawling_speed_ratio` with these declarations.
  Update the resource's Agent-type `api_version` to 2.
- Migrate bundled Human, repository fixtures, and inline script builders.
  External v1 Agent scripts are refused with migration guidance, not converted
  by silently granting every pose. Legacy World records still resolve bundled
  Human through the existing legacy resource rule.

Capability, envelope parameters, and context choices cannot be overridden by
individual Agent properties or Agent tags. Effective Height modifiers still
change physical fit using existing individual-over-tag precedence.

## Fit and automatic selection

Both Room movement and Door crossing try Standing, Crouching, then Crawling,
skipping poses not allowed in the context and selecting the tallest whose entire
applicable physical envelope fits. Script array order and movement speed do not
change this priority. Selection returns a pose and context speed ratio, or
an explicit no-fit result. Routing, captured route facts, remaining-Path
validation, request/queue gates, permit adoption, actual movement, and duration
estimates must agree on that result.

The physical inputs are:

| Input | Meaning |
| --- | --- |
| Effective Standing height | Frozen base Standing height with existing effective Height modifier |
| Bodily width | Existing type-derived width |
| Pose envelope | Effective Standing height times the pose height ratio, and base bodily width times its width ratio |
| Support elevation | Real support above the Floor/Walkway, not an artwork offset |
| Vertical clearance | Available ceiling/opening height relative to the approach Floor/Walkway |
| Opening width | Available physical aperture width where existing rules constrain it |
| Selection context | Room movement or Door crossing |

Standing's vertical extent is effective Standing height. All other poses use
that height times their declared height ratio. Physical width uses base type
width times the pose width ratio. Human Lying declares height `26/72` and width
`72/26` from its baked artwork dimensions; no hardcoded rotated envelope remains.
Bounds, vertical clearance and applicable opening-width checks use these
physical dimensions. Height modifiers scale Lying's height like other poses.

Physical fit uses the existing `0.00001` World-unit tolerance. Automatic thresholds
are derived from these dimensions and available space, not separately duplicated
absolute heights. No arbitrary World expressions, anticipatory preferences,
hysteresis, preparation delays, or recovery delays are introduced.

Human allows Standing/Crouching/Crawling in both contexts; Android allows only
Standing. Human keeps
ordinary Room speed and ordinary threshold speed for Standing/Crouching, with
half-speed Crawling Door crossings. Scripts may declare choices in another order
after the required Standing entry, but selection always prefers the tallest
allowed fitting pose. Sitting/Lying remain explicit uses or Actions rather
than automatic locomotion. Ratios affect the relevant movement component, not
Lift/Shuttle ride speed, device opening/cycle time, or unrelated modifiers.

No fit excludes a route or refuses new admission through existing Route
planning/Route loss behavior. Placement and relocation also require a fitting
supported Room-movement pose. A robot with Door Mobility still cannot crawl.
Windows and BoothWindows remain non-passenger apertures.

## Pose ownership and lifecycle

| Situation | Pose rule |
| --- | --- |
| Creation, load, Reset, or fresh restoration | Construct and validate fresh definitions; validate placement and resolve environmental posture; do not replay old use callbacks |
| Ordinary movement | Select Room-movement posture from the physical Sector under the Agent; preserve existing non-Room Standing policy |
| Successful pose-setting Marker Action | Supported, physically fitting requested pose takes ownership |
| New destination request or Route planning | Predict departure posture without changing Action/Furniture posture or occupancy |
| Actual departure | End Action/Furniture ownership after preflight; select environmental or admitted-crossing posture, not an unconditional Standing reset |
| Furniture use | Retain declared use posture through planning, failed movement, pause, and deactivation |
| Admitted threshold crossing | Retain selected supported crossing posture until physical completion |
| Crossing completion, cancellation, or ownership release | Resolve the applicable next owner/environment; no unconditional Crawling or Standing assignment |
| Relevant accepted physical edit | Re-evaluate environmental posture; reject edits violating retained posture or active Furniture finish feasibility |

An explicit Action pose survives until a successful replacement pose effect or
actual departure. Ordinary pose effects cannot bypass physical fit. An unsupported
or impossible ordinary Action is an ordinary refusal: reject its entire staged
batch, including occupancy, device, and log effects, without partial publication.
General Marker Action availability is otherwise unchanged; this refactor does
not add speculative execution or general Action requirement metadata.

## Furniture authoring and eligibility

Each definition offering use supplies exactly one `use_pose`, one
`finish_use_pose`, and paired `use()` / `finish_use()` functions. A definition
with no use omits all four. There are no intermediate pose sequences or
instance-specific pose overrides.

An illustrative chair definition fragment is:

```lua
use_pose = "sitting",
finish_use_pose = "standing",
use = function(agent, world, marker)
    world.claim()
end,
finish_use = function(agent, world, marker)
    world.release()
end,
```

A bed uses `use_pose = "lying"` with a declared finish pose. Host staging
applies the declaration atomically with callback effects. Any Furniture callback
`set_pose` call is a script-contract error, even if it names the declared pose;
remove these calls when migrating existing catalogues and fixtures. General
Marker Actions keep `set_pose`.

Expose one production World eligibility query with a useful refusal reason. It
checks both declared poses against the Agent's frozen capabilities and against
the target usable point's geometry:

1. The Agent must be able to stand in the Furniture's Sector: its Standing pose
   must be supported and fit the Sector clearance at the Furniture, however well
   the declared poses themselves would fit.
2. The use envelope must fit with the point's declared support elevation.
3. The finish envelope must fit after release of Furniture support/occupancy.
4. Apply existing usable-point, availability, occupancy, permission, and movement
   rules as appropriate; pose eligibility does not supersede those authorities.

Do not use the Agent's current Room to predict a distant use, and do not execute
callbacks speculatively. The preliminary check is conservative; execution checks
the complete staged result again, including resulting support and occupancy.

Editor choices hide or disable ineligible Use furniture with a reason. Behaviour
and API requests selecting that use refuse before starting movement. Arrival
rechecks before publishing effects. Idle movement to the Marker and unrelated
Actions remain available subject to their existing rules.

Both capability and physical feasibility matter. A robot cannot use a Sitting
chair even if its finish pose is Standing. A Human cannot begin use if its
declared Standing finish is already impossible. There is no automatic substitution
of Crouching for a declared Standing finish.

### Finishing and mutations

While use is active, edits must preserve both the current use posture and the
declared finish posture. Preflight departure, replacement Actions, and forced
finishing before ending use; refusal preserves use, occupancy, document dirty
state, and history where applicable. Furniture move/delete or structural edits
must not first tear down use and only afterward discover impossible cleanup.

Finishing applies the declared finish pose and releases occupancy. Callback
failure still reports the existing script failure, but host guarantees the same
pose/release cleanup. The finishing guarantee relies on preflight and edit guards
maintaining feasibility; it must not fall back to an unsupported or merely
preferred pose. Environmental/crossing selection takes ownership after physical
departure.

## Live changes and commitment exception

Reject authored changes transactionally if their immediate result leaves an
Agent without a valid pose in occupied space. Environment-owned Agents may
select a different supported Room pose; retained Action/Furniture poses cannot
be silently replaced. Future-only passage changes may succeed and trigger Route
planning/Route loss. Relevant changes include ceilings, effective Height,
Furniture support/geometry, and pose definitions resolved for fresh lifetimes.

Validate necessary committed exits before admission. An admitted crossing keeps
its selected supported pose. A committed transport/chamber exit uses a fitting
supported crossing pose where possible; if later conditions make every choice
impossible, preserve the previously accepted supported exit pose under the
existing completion obligation. Record sufficient acceptance information for
that guarantee. Never force Crawling on a standing-only Agent.

This grandfathered completion is an explicit exception to current physical fit,
not a successful fit classification. It applies only to existing commitments;
new admission remains refused. Existing authored-edit guards protect active
crossings and commitments. Pause/deactivation freeze rather than reset them.

## Lifetimes and persistence

Retain ADR 0019's boundaries: surviving Agents preserve frozen pose definitions
through paused topology edits and history replay; creation, load, Reset, and
restoration after deletion construct fresh instances from the applicable resource.
World documents and clipboard/history retain type/resource references and authored
properties, not runtime poses, pose-definition snapshots, or live Lua state.
Fresh validation failure is transactional. No pose-related World schema change
is required merely to store frozen definitions.

## Verification scope

Primary seam: production World workflows with real Lua resources, Marker requests,
fixed-timestep simulation, snapshots, and semantic outcomes. Use existing Agent-type,
Furniture-use, Marker-Action, and mixed-Crawling smoke patterns. No private pose
setter, speculative Lua execution, or test-only selector is needed.

Supplement existing direct/captured route facts for cost agreement, real
editor/document-history seams for availability and atomic edits, and rendering
observations for canonical presentation. The eligibility query is a production
World API shared by consumers, not an independent testing hook.

Cover:

- valid Human and standing-only robot definitions; missing/unknown/malformed poses,
  ratios, lists, and API versions; transactional construction/import/reconstruction;
- exact/tolerance/below-fit cases, effective Height precedence, real versus
  decorative support, width-constrained apertures, and physical Sector boundaries;
- Room/Door choices, all existing threshold families and activation/Broken modes,
  alternatives/Route loss, actual/captured durations, queues, and existing gates;
- literal supported/unsupported/impossible general Actions and batch rollback;
- Furniture capability plus both target-space envelopes, remote-target query,
  Idle arrival, request/arrival recheck, callback contract errors and atomic effects;
- use/finish/departure/replacement, failed movement, callback cleanup, pause,
  deactivation, live changes, forced finishing, edit refusal, dirty state/history;
- supported-pose committed crossings/exits after live changes, including robots;
- save/load, Reset, clipboard/history, survivor freeze versus fresh construction,
  and legacy Human migration with no runtime-pose persistence;
- repository-wide resource/inline fixture migration and canonical rendering.

Build incrementally and run repository-owned CTests or approved Python scripts
only in Release. Do not run tests from unmodified submodules. Reuse available
validation evidence rather than repeatedly executing unchanged tests.

## Delivery and exclusions

Parent specification: [#520](https://github.com/ajare/promethium-fermide/issues/520).
Implementation is split into four dependent issues:

1. [#521 — Agent script contract and frozen definitions](https://github.com/ajare/promethium-fermide/issues/521).
2. [#522 — Shared selection, fit, routing, and movement](https://github.com/ajare/promethium-fermide/issues/522), blocked by #521.
3. [#523 — Furniture declarations and eligibility](https://github.com/ajare/promethium-fermide/issues/523), blocked by #521 and #522.
4. [#524 — Lifecycle, edit safety, and integrated verification](https://github.com/ajare/promethium-fermide/issues/524), blocked by #521, #522, and #523.

Each issue ships focused external-behavior coverage; the final issue checks their
composition. #521 supplies the validated/frozen declaration contract and capability
and envelope observations. Its Agent/Editor coverage includes Human and Standing-only
resources, malformed contracts, preview/import refusal, persistence, revised fresh
lifetimes and surviving topology/history carry. #522 integrates shared fit/selection, placement, routing/admission and context
motion, with focused production World journeys and captured/direct facts.
#523 implements declared Furniture requirements and target-space eligibility.
#524 integrates ownership release, replacement-effect transactions, occupied-space
preflight and exact accepted-exit recording. Its validation is recorded separately.

Out of scope: custom pose identities/shapes, independent per-pose widths, new
fatigue/damage/task systems, live Lua selectors, tag-granted capabilities,
anticipatory thresholds, hysteresis/delays, intermediate Furniture pose sequences,
general Action capability metadata, new seated/lying locomotion, movable Furniture,
and new device/permission/Mobility semantics.
