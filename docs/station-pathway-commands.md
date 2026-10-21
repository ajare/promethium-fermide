# Scheduled Pathway commands (#787)

`StationSystemsSettings::pathwayCommands` holds one-off `PathwayCommandSettings`
entries: `id`, `pathwayId`, `openFraction`, `occurrenceSeconds`, and `priority`.
The existing paused `World::configureStationSystems` seam validates the complete
configuration atomically. Command IDs must be unique and non-empty (at most
`MaxStationStoreIdLength` characters); at most `MaxStationPathwayCommands` (256)
commands may be authored. Openness must be finite and within [0, 1], time finite
and non-negative, and the target must name an authored Atmosphere pathway.
Refusals name the command and invalid field/reference; they do not clamp values
or mutate the current run.

Commands use the shared Station occurrence collector: occurrence time ascending,
priority descending, then declaration order. The last applied command wins.
They split integration at their authored time, including inside a coarse Station
step, and a command at an endpoint affects only the following positive interval.
A zero-time command affects the first interval. Existing material-event kind
ordering is unchanged; commands follow material events at equal time/priority
(they do not consume or supply material).

`StationAtmospherePathwaySnapshot` carries runtime `openFraction`, plus
`lastCommandId` (empty before the first successful command) and
`lastCommandSeconds`. Signed gas flow rates cover the entire last Station step,
including every event-split interval. Cumulative exchange and command state are
rolled back with the rest of a failed ledger transaction.

Reset discards all live openness, command provenance and pending occurrence
state, restores each authored initial openness, and rebuilds the authored
schedule from simulation time zero. Authored commands therefore replay in a new
run; they are not deleted from the scenario. Reset preserves the in-memory
Station configuration independently of the document encoding. Pathway/command
document persistence is separately scoped to #791; no live command API or editor
authoring UI is added here.

Fast checks: `pathwayCommands`, `pathwayCommandOrdering`,
`pathwayCommandValidation`, and `pathwayCommandReset`. Command landing and gas
response are also checked through the existing `--step-ratio` runner at 1, 7,
and 60 in `validation-fast`, against physical elapsed-time expectations rather
than tick counts.
