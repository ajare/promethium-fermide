# Scripted Agent clipboard and history (#507)

Copy/paste carries an Agent's stable type ID and application Resource reference,
not its physical baseline or private Lua instance. It also carries authored
activation, flags, Agent group name, individual properties, Agent tag assignments
and persisted property samples, behaviour configuration, and the existing
World-scoped authorization block. Individual properties continue to override
inherited tag samples; paste previews use that same Height precedence.

Legacy Human payloads may omit the Resource reference and/or type ID under the
existing legacy rules. These resolve to bundled `human.agent.lua`. An explicit
missing resource, mismatched ID, competing definition in the destination World,
or failing constructor is refused, never substituted with Human. Invalid
previews return zero dimensions and a dependency diagnostic. Per-frame previews
use validated startup/import snapshots, not current on-disk edits; invalidating
an on-disk dependency does not hot-reload an accepted preview. Arming a paste
validates identity and its isolated constructor without publishing a World Agent
or history entry; committing revalidates dependencies and rolls back a newly
registered type if creation fails.

History snapshots contain authored documents only. Undoing a paste deletes its
instance; redoing it constructs a fresh one. Cut/delete ends the Agent lifetime,
and undo restoration uses the currently resolved resource and constructor.
Restoration failure leaves the current World and undo/redo stacks unchanged.
Surviving Agents retain their live instances and frozen baselines through the
existing #509 document-history seam, including when a failed candidate World is
discarded. Neither clipboard nor history stores arbitrary Lua tables, closures,
VM state, or embedded baseline snapshots.

Release headless coverage lives in `src/headless/smoke/agent/AgentTypeEditor.cpp`:
`agentTypesScriptedClipboardAndDeletionHistory` proves wire/property/sample
preservation, preview agreement, paste undo/redo, fresh deleted-Agent restoration,
survivor preservation, and restoration failure atomicity.
`agentTypesScriptedClipboardRefusalAndLegacy` proves missing/mismatched identity,
constructor and individual-property refusal, duplicate definition conflicts,
legacy Human compatibility, and absence of partial paste/history mutation.
These run under the public `smoke-editor` CTest owner, alongside existing editor,
resource, tag, and history checks. No dialog, GPU, or private VM test API is used.
