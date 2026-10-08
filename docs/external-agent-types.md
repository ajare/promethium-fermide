# Import external Agent types (#506)

See [Lua Agent-type authoring](lua-agent-types.md) for all eleven baseline fields,
sandbox limits, immutable identity, preview isolation and reconstruction semantics.

In **Agent creation settings**, enter the path to a `.agent.lua` file and choose
**Import Agent type**. Successful import selects that definition in the existing
**Agent type** combobox. Its display name and stable type ID remain distinct;
existing Agents have read-only type identity. Placement and previews use the same
isolated constructor and eleven-field physical-baseline validation as bundled
Agent types.

Import reads only the selected regular file. It neither scans neighbouring
files, writes scripts, nor changes `resources/Resources.yaml`. Invalid Lua,
unsupported capabilities, invalid identities, constructor failures, invalid
baselines, and budget exhaustion report a diagnostic with the source path.
Competing definitions with the same type ID are refused. A failed import does
not enter Willpower or the selector; a failed placement also rolls back a new,
unused World type registration and leaves no Agent or authored edit.

## Resource references and reopening

Bundled definitions keep their manifest Resource names. Imported definitions use
an `external-agent-<hex>` Resource name whose hex payload is the canonical absolute
source path's generic-path bytes. This is a single Willpower-compatible name
(no directory separators), not a second manifest or mutable path cache. The
existing `resolveCatalogSource("AgentType", name)` seam decodes explicit external
references when the manifest does not supply the name. The application lazily
creates and validates the managed resource when reopening such a World.

YAML and binary Worlds persist that Resource name together with the stable type
ID, never the baseline or Lua state. Files outside the World directory, including
paths with spaces, therefore reopen in a new application session as long as the
original source remains available at its canonical path. Moving a World alone
works; moving or removing the referenced script requires restoring its source
path. Explicit missing, invalid, or mismatched resources fail rather than becoming
Human. Headless document loading resolves the same references without graphics.

Reimporting an already accepted source is idempotent, **not hot reload**. Its
accepted source remains unchanged in that application session and existing Agents
retain frozen baselines. A later fresh session/document load reads the source
again and constructs fresh isolated instances.

## Headless coverage

The Editor smoke module owns:

- `agentTypesExternalImportPlacesAndReopens`: managed import, selector inventory,
  preview/placement agreement, no directory scan, YAML/binary save and fresh-session
  reopen, headless reopen, missing/invalid dependencies, and no hot reload.
- `agentTypesExternalImportRefusesWithoutRegistration`: malformed Lua, missing or
  invalid baseline, duplicate identity, forbidden capability, constructor errors,
  instruction budget, corrected retry, and unchanged source files.
- `agentTypesExternalPlacementRollsBackRegistration`: World-budget constructor
  failure leaves no registration, Agent, or authored mutation; competing World
  definitions cannot silently substitute for the imported resource.

These exercise the production CPU-only application resource/import seam and
public World/document/editor placement workflows; no native dialogs or graphics
contexts are used.
