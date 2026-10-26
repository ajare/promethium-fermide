# Power Bus breakers (#884)

A rated Power Bus trips when actual through-power is strictly above its rating
for `tripPersistenceSeconds` continuously. Equality and below-rating intervals
reset the pending timer; rating zero disables overload protection. Zero
persistence isolates before any trial allocation is committed. Positive windows
split integration at the protection boundary, including inside coarse Station
steps. Electrical and material accounts before that boundary remain real.

A Bus Trip latches independently of settings recovery. Its sources and batteries
cannot supply that circuit, its batteries cannot charge from it, and unsupplied
loads remain explicit unmet demand (including unmet critical floors). A generator
with an isolated landing follows normal shutdown and decay-heat accounting,
including on its other landings. Other source and battery landings remain usable.
Transfer and Shared consumers retain their existing supply-mode rules.

Headless clients use `World::resetStationBreaker(busId, diagnostic)` or
`StationSystemsRuntime::breakerReset(busId, diagnostic)`. Unknown buses,
untripped buses, disabled runtimes and unhealthy runtimes reject the command
without mutation. Reset clears the latch and pending persistence; a continuing
overload must qualify again for a full window. Like other World edits, headless
editors can bracket the command with the document snapshot/history API. The
Station Flow marker and inspector do this automatically using the document history.

The always-on `busBreakerTrip` Critical Alert uses the callback registry, not a
Monitor. Its payload includes bus identity, through-power at trip, rating and
simulation time. Only BreakerReset clears it, with clear reason `BreakerReset`.
Monitor IDs beginning `breaker:` are reserved so authored rules cannot impersonate
or disable protection announcements. Physical replay retains breaker Alert
lifecycle state, including across document history; Monitor observation still
starts fresh at load as before. No runtime latch is written into authored settings.

The diagram shows a dead-coloured bus with a **Latched trip - Reset breaker**
marker; the selected bus also offers **BreakerReset** in the inspector. These
controls operate the applied runtime, not an unapplied configuration draft.
Snapshot trip flags describe current breaker state; power rates describe the last
integration interval (duration-weighted when a trip splits that interval), not a
retroactive deletion of electricity delivered before the breaker opened.

Coverage: `station-busBreakers` exercises runtime/snapshot timing, recovery, latch,
reset, re-trip, Critical Alerts, isolation, generator shutdown, and coarse-step
accounts. `station-editor-bus-breaker` exercises CPU-only ImGui styling and both
reset controls, autonomous-trip YAML/binary save/load, and reset undo/redo.
