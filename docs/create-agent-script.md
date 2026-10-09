# Create an Agent script

An **Agent-type script** defines what an Agent physically is: its stable type
identity, dimensions, movement speeds, supported poses, and default Mobility.
It does **not** run a movement loop. To decide where an Agent goes, assign a
separate [Behaviour script](create-behaviour-script.md).

Agent types use **API v2**; Behaviour scripts use **Host API v3**. These are
separate contracts and separate Lua runtimes.

## 1. Create a `.agent.lua` file

Save this complete example as `courier.agent.lua`. It defines a Standing-only
Courier, using the existing `agent` atlas tile.

```lua
return {
    api_version = 2,
    type_id = "Courier",
    display_name = "Courier",
    new = function()
        -- Each Agent receives a fresh instance table.
        return {
            width = 0.3,
            standing_height = 0.35,
            object_usage = "arms",
            object_usage_distance = 0.4,
            walk_speed = 0.9,
            climb_speed = 0.5,
            stair_ascent_speed = 0.6,
            stair_descent_speed = 0.7,

            poses = {
                standing = { image_tile = "agent" },
            },
            automatic_poses = {
                room_movement = {
                    { pose = "standing", speed_ratio = 1 },
                },
                door_crossing = {
                    { pose = "standing", speed_ratio = 1 },
                },
            },

            mobility_profile = {
                staircase = "can_use",
                escalator = "can_use",
                stairwell = "can_use",
                ladder = "can_use",
                lift = "can_use",
                platform_lift = "can_use",
                shuttle = "can_use",
                door = "can_use",
                buttons = "can_use",
            },

            private_state = {}, -- optional; not saved in the World
        }
    end,
}
```

### Required fields

- `type_id`: stable identity, 1–128 ASCII letters, digits, underscores or hyphens.
  Use a distinct ID for a distinct type; competing resources with the same ID
  are refused within a World.
- `display_name`: presentation label, nonempty, at most 128 bytes, with no control
  characters. It may differ from the type ID.
- `new()`: returns a fresh instance table. No missing field inherits Human defaults.
- `width`, `standing_height`, `object_usage_distance`: dimensions/distances in World units.
- `object_usage`: `"arms"` (also the compatibility default when omitted).
  Distance is required, finite and strictly positive. Legacy `reach` remains an
  alias, but declaring it together with `object_usage_distance` is rejected even
  when equal. See [Object usage and arm length](lua-agent-types.md#object-usage-and-arm-length-535).
- `walk_speed`, `climb_speed`, `stair_ascent_speed`, `stair_descent_speed`:
  speeds in World units per second. All seven physical numbers must be finite
  and positive, including after conversion to simulation floats.
- `poses`: supported canonical poses. Standing is mandatory; other choices are
  Sitting, Lying, Crouching and Crawling, using lowercase Lua keys. Every declared
  pose requires an `image_tile` string naming an ObjectAtlas image.
- `automatic_poses`: both `room_movement` and `door_crossing` are required.
  Each is a nonempty dense array beginning with Standing, without duplicates.
  Only declared Standing/Crouching/Crawling poses may appear; each `speed_ratio`
  must be finite and in `(0, 1]`.
- `mobility_profile`: exactly the nine keys in the example. Values are
  `"can_use"`, `"cannot_use"`, or `"only_if_no_other_option"`.

A Standing-only Agent cannot crouch or crawl through a low opening or perform a
Furniture use requiring Sitting or Lying. To add capabilities, declare their
pose envelopes and, where appropriate, their automatic movement choices. See
[the full pose rules](lua-agent-types.md#required-pose-declarations-and-v1-migration).
The host chooses the tallest allowed pose that fits, not simply the fastest one.

## 2. Import and place the Agent

1. Pause the simulation and open **Agent creation settings**.
2. Enter the path to `courier.agent.lua` and select **Import Agent type**.
3. Select Courier in the **Agent type** combobox and place an Agent.
4. Assign a Behaviour separately if it should move autonomously.

Import and placement validate the script and report field/constructor errors.
Preview uses an isolated constructor, not the future Agent's private instance.
An existing Agent's type cannot be changed.

External scripts are referenced by their canonical absolute source path through
a managed Resource name. Keep the file available at that path when reopening
its World; saving the World does not embed the script. See
[external Agent types](external-agent-types.md) for portability and import details.

## 3. Understand instance lifetime and limits

The host validates and **freezes** physical values, pose capabilities and default
Mobility at construction. Mutating the Lua table later does not change them.
Individual Mobility overrides replace tag profiles, which replace the script
default; these are complete profiles, not per-key merges.

Each Agent has an isolated live Lua instance. Private tables and methods are
allowed, but there are **no automatic callbacks or cross-script method calls**:
Behaviours cannot call methods on the type instance. Do not define a `behaviour`
member; it is reserved for the host-owned Installed behaviour association.

Private Lua state is not persisted. Load, Reset and restoration after deletion
construct fresh instances. Ordinary paused topology edits preserve surviving
instances. Editing or reimporting the file is not hot reload; existing Agents
retain their frozen defaults. Use a fresh application session/document load to
resolve an edited external source again.

Scripts are sandboxed and budgeted; filesystem, process, native-module and debug
access are unavailable. Keep module evaluation and `new()` bounded.

## Reference

- [All editable Agent properties](agent-properties.md)
- [Full Agent-type contract](lua-agent-types.md)
- [Agent pose capabilities](agent-pose-capabilities.md)
- [ADR 0019: live instances and frozen baselines](adr/0019-script-backed-agent-types-with-live-instances-and-frozen-baselines.md)
- [Create a Behaviour script](create-behaviour-script.md)
