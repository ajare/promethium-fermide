# Random Marker Wander

`random-marker-wander.lua` implements an independent wandering loop for each assigned Agent:

1. Choose a random Marker other than the last arrival.
2. Move there using the World's normal routing.
3. On arrival, wait 180 simulation ticks (3 seconds), then choose again.
4. If routing fails, try the other candidates without replacement. If none work, remain idle without polling.

Random choices use the host's deterministic per-Agent random stream. Rejected movement commands are retried on a later tick, respecting the one-movement-command-per-callback rule. No behaviour configuration or World document is changed by running the script itself.

## Use

`new-world.world` references this package, and all 320 Agents are assigned **Random Marker Wander**. Resume the simulation to run it.

For another World, while paused, select this package's `behaviours.yaml` in **World → Behaviours**, then assign **Random Marker Wander** to the desired Agents.

The optional `markers` configuration list defaults to all 81 Markers present in `new-world.world` when this package was created. The current Lua API does not enumerate World Markers, so this is a configured snapshot, not a live query. Update the list when adding/removing Markers or using a different World. An explicitly empty list idles immediately. If the only Marker is the last arrival, the Agent idles after its arrival wait because there is no different destination.

Once all candidates have failed, the instance stays idle; restart/reset the behaviour to try again after changing connectivity. A successful trip starts a fresh candidate pool, so previously unreachable Markers can be tried on later trips.
