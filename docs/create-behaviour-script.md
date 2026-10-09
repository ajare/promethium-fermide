# Create a Behaviour script

An **Agent behaviour** directs an Agent's intent: where to go, how to react to
movement outcomes, and how long to wait. Each assigned Agent runs its own Lua
coroutine, with independent locals and closure state. Its physical capabilities
come from a separate [Agent-type script](create-agent-script.md).

Behaviours use **Host API v3**, not Agent-type API v2. A Behaviour module returns
an API version and a `factory(configuration)` that returns a function accepting
`context`. Do not return a callback table.

## 1. Create a Behaviour package

Save and pause the World. Use **World → Behaviours** to create an adjacent
`*.behaviours` directory. For `station.world.yaml`, this is normally
`station.behaviours/`.

Create makes `behaviours.yaml` with a unique registry UUID. Keep that generated
UUID; do not copy another registry's UUID. For a new package, add this definition
while retaining its generated UUID:

```yaml
version: 1
uuid: YOUR-GENERATED-UUID  # retain the real UUID created by the editor
revision: 1
nextBehaviourId: 2
behaviours:
  - id: 1
    name: Two-stop patrol
    revision: 1
    source: two-stop-patrol.lua
    schema:
      - name: first
        type: marker
      - name: second
        type: marker
      - name: dwellTicks
        type: duration
```

The UUID line above is a placeholder, not a valid UUID to paste literally.
For an existing package, allocate an unused ID and advance `nextBehaviourId`;
never reuse a deleted ID or replace the existing manifest wholesale.

Configuration belongs to each assigned Agent. Marker fields are selected by name
in the editor but retain stable Marker identity. A `duration` is in simulation
ticks: 60 ticks equal one simulation second. Configure `dwellTicks` to a positive
whole number, such as 180 for three seconds.

## 2. Write the coroutine and event handling

Save this as `station.behaviours/two-stop-patrol.lua`:

```lua
local host = require("promethium.v3")

return {
    api_version = host.api_version,
    factory = function(configuration)
        return function(context)
            assert(configuration.dwellTicks >= 1,
                "dwellTicks must be positive")
            local stops = { configuration.first, configuration.second }
            local index = 1

            while true do
                local destination = stops[index]
                local request = context.move_to(destination)
                if not request.accepted then
                    context.log("Movement request refused: " .. request.status)
                    return -- finish rather than repeatedly polling a refusal
                end

                local arrived = false
                while not arrived do
                    local event = wait()
                    if event.destination == destination then
                        if event.type == "destination_reached" then
                            arrived = true
                        elseif event.type == "route_lost" then
                            context.log("Route lost: " .. event.reason)
                            return
                        elseif event.type == "movement_cancelled" then
                            context.log("Movement cancelled: " .. event.reason)
                            return
                        elseif event.type == "action_failed" then
                            context.log("Action failed: " .. event.reason)
                            return
                        end
                    elseif event.type == "activated" then
                        context.log("Patrol reactivated")
                    end
                    -- Unrelated events are consumed; wait for the next one.
                end

                sleep(configuration.dwellTicks)
                index = 3 - index -- alternate 1 and 2
            end
        end
    end,
}
```

Select two distinct, reachable Markers for this example. It deliberately stops
on refusal or failed/cancelled movement rather than retrying indefinitely. A
more elaborate behaviour can choose another destination after route loss.

### What suspends execution?

**`context.move_to(destination)` does not suspend.** It stages a request and
immediately returns an immutable result with `accepted` and `status`. Omitted
Action means Idle. Successful staged command batches apply after the boundary's
resumes; acceptance is not physical arrival.

**`wait()` suspends until the next semantic event**, then returns its immutable
event table. It is equivalent to `context.wait()`. The local variables and stack
position survive the suspension. Events arrive in sequence order and are
consumed once; `wait()` does not mean “wait until arrival” unless your loop checks
for `destination_reached`. There is no automatic per-tick polling. Queued events
can cause multiple resumes at one boundary, within runtime budgets.

**`sleep(ticks)` suspends for pure simulation time** and returns no values. It is
equivalent to `context.sleep(ticks)`. Its argument must be a positive whole number.
A sleep started at tick N expires at N + ticks. Events do not interrupt sleep;
they remain queued for subsequent waits.

In this example, the Agent requests the first stop, yields until arrival, sleeps,
requests the second stop, and repeats. While the script is suspended, physical
movement and the rest of the simulation continue.

### Events to handle

| Event type | Meaning |
| --- | --- |
| `destination_reached` | Physical arrival and successful completion of the selected synchronous Action, including Idle |
| `route_lost` | Destination has no valid Path; `reason` explains the loss |
| `movement_cancelled` | Movement cancelled or superseded; includes `reason` |
| `action_failed` | Selected Action refused or failed; includes `reason` |
| `interaction_completed` / `interaction_failed` | Interaction outcome; includes opaque interaction identity and result/reason |
| `activated` | The same instance has been reactivated; its body has not restarted |

Movement events include opaque `destination`, `action`, `result`, `tick` and
`sequence`. Match destination handles when waiting for a particular goal. The
context's snapshot fields describe startup observations, not live World state;
use events for subsequent outcomes. Context command functions remain usable
after waits.

## 3. Reload, assign and run

1. Use **Reload registry** while all loaded dependent Worlds are paused.
2. Confirm the Behaviour is Loaded, not Error, in the registry panel.
3. Assign **Two-stop patrol** to an Agent and configure `first`, `second` and
   `dwellTicks` using the generated fields.
4. Resume simulation and observe the Agent alternate stops with the dwell delay.

Module loading and factory preflight validate the contract without running the
returned coroutine body. Body errors appear during simulation, so successful
registry loading is not proof that every runtime path succeeds.

## Rules and lifecycle

- Issue **at most one movement intent per resume**, counting cancellation too.
  A second is a programming error and discards that resume's staged commands.
  Yield with `wait()` or `sleep()` before issuing another intent.
- Returning finishes this instance and installs the default no-op; it does not
  remove the authored assignment or automatically restart it. Final successful
  commands still apply, and already-issued movement may continue.
- Pause preserves the coroutine and freezes simulation time. Deactivation
  prevents resumes, freezes remaining sleep and clears pending events.
  Reactivation queues `activated` without restarting the coroutine or interrupting
  remaining sleep.
- Reset, load and registry reload reconstruct execution from authored
  configuration; suspended stacks and private Lua state are never saved.
- Use `context.random_integer(min, max)` or `context.random_number()` for
  deterministic per-Agent randomness, not `math.random`.
- The sandbox has instruction, memory, command, resume and pending-event limits.
  An endless loop without a yield is an error, not a background thread. Runtime
  failures publish diagnostics and pause interactive simulation; headless runs fail.

For manifest edits, preserve IDs and allocator high-water marks. Source-path or
schema changes require a higher Behaviour revision. Changes to declared helper
modules require a higher package revision. Source paths must stay inside the
package; helper imports must be declared in its manifest.

## Reference

- [All editable Agent properties](agent-properties.md)
- [Full Behaviour package and Host API contract](agent-behaviour-packages.md)
- [Bundled random-marker wander example](../resources/test-worlds/new-world.behaviours/random-marker-wander.lua)
- [ADR 0021: coroutine execution](adr/0021-run-installed-agent-behaviours-as-coroutines.md)
- [Create an Agent script](create-agent-script.md)
