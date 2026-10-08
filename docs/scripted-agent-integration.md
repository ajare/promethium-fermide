# Scripted Agent integration verification (#510)

The final migration removes the compiled Human adapter and Human-only factory
and baseline lookup forms. Human's authored `.agent.lua` resource is the sole
physical authority; the build generates the headless embed from that file.
Production query Agents now construct from a resolved definition, and default
permission-bearing World creation uses the same World-local scripted path as
typed creation. Selected-type width also drives creation drop bounds.

The existing Human-default Agent-less routing and UI policies remain intact,
using the resource-derived baseline. Shared-resource slot geometry, permissions,
Mobility, modifier precedence, behaviour ownership and Furniture/Action policy
are unchanged. No callbacks, cross-script methods, hot reload, Lua-state
persistence, scanning, generated scripts or additional production types were
introduced.

## Public-seam coverage

The Agent and Editor modules cover these contracts on the final source state:

| Contract | Representative checks |
| --- | --- |
| Historical Human floats and all eleven fields | `agentTypesScriptedHumanIdentity`, `agentTypesBundledDefinitionMatchesResource` |
| Arbitrary type, effective dimensions/speeds/bounds/clearance | `agentTypesGenericScriptBackedType`, `agentTypesFixturePhysicalOutcomes` |
| Startup/resource selection, isolated preview, selected-width drop clamping | `agentTypesExternalImportRefusesWithoutRegistration`, `agentTypesExternalImportPlacesAndReopens`, `agentTypesEditorSelectionPreviewAgreesWithPlacement` |
| External import, no file writes/scanning, fresh-session dependencies | `agentTypesExternalImportPlacesAndReopens`, `agentTypesExternalPlacementRollsBackRegistration` |
| YAML/binary save/load and explicit missing/mismatched resource refusal | `agentTypesFixturePersistenceRoundTrip`, `agentTypesLoadingRefusesMismatchedOrMissingResource` |
| Legacy World records omitting type, type ID and resource | `agentTypesLegacyHumanWithoutResource` |
| Legacy clipboard, copied identity, tags/samples/properties, paste/cut/history | `agentTypesScriptedClipboardRefusalAndLegacy`, `agentTypesScriptedClipboardAndDeletionHistory` |
| Reset, revised baselines/display names, authored preservation, atomic failure | `agentTypesResetRevisionAndAuthoredData`, `agentTypesResetFailureIsAtomic`, `agentTypesManagedResourceRevisionOnResetAndLoad` |
| Surviving-instance preservation, deleted-instance reconstruction, teardown | `agentTypesTopologyReplayPreservesLiveInstances`, `agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents`, `agentTypesResetReleasesReplacedInstances` |
| Duplicate identity refusal | `agentTypesDuplicateTypeIdRejected`, `agentTypesCompetingTypeIdsRejectedOnLoad` |
| Private module-state isolation | `agentTypesInstancesAreIsolated`, `agentTypesLoadingConstructsFreshInstances` |

Adversarial baseline coverage now actually constructs the registered invalid
type and requires a resource/field diagnostic. The old fixture incorrectly
requested unregistered case names and its nominal missing-field case omitted no
field; those false positives are removed. Every field now covers omission,
NaN/infinities, zero/negative values, non-numeric values and float underflow or
overflow; ratios also cover values above one and accept the inclusive upper
boundary. Malformed Lua, absent/invalid constructors, non-table instances, module
and constructor exceptions, forbidden capabilities and allocation/execution
exhaustion are covered. `pcall`/`xpcall` cannot swallow terminal budgets, and
subsequent valid creation recovers without partial publication.

Validation rejects finite Lua doubles that cannot become finite positive
simulation floats. Persistence uses stable identity rather than a revision's
presentation name, allowing revised display names without changing authored
type/resource identity or rejecting old documents.

## Final Linux Release results

Reused the existing GUI-enabled `build-linux` Release tree. Inner-loop builds
selected only Agent/Editor targets and their dependencies; focused Agent/Editor
CTest feedback preceded the final integrated pass. All checks are headless:
Editor/Render use CPU seams, and graphics startup uses controlled failure without
opening a window or native dialog.

Final default inventory build (editor, core, headless modules, standalone tools
and compile contracts):

```sh
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh \
  --build-dir build-linux --config Release --lane final --build-only \
  --run-timeout 1800 all
```

Passed, exit 0. Build evidence is retained locally at
`build-linux/.pf-validation/run-5245a37c6dc74a06bf28c7444d1496fc/`.

Final repository-owned exhaustive CTest inventory:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build-linux -C Release \
  -E '^willpower_' -j 8 --output-on-failure
```

**72/72 passed, zero failures, 157.54 seconds.** This includes all thirteen smoke
modules, their exhaustive contracts, relevant lifecycle/clearance integration,
CLI/concurrency, persistence, rendering/bounds, routing/transport policies,
controlled startup, ownership, harness, tooling and validation-supervisor
contracts. The 45 unmodified Willpower dependency tests were explicitly excluded
as required by `AGENTS.md` and #510; no Debug or native Windows validation is
claimed. CTest evidence remains in `build-linux/Testing/Temporary/LastTest.log`.

`git diff --check` passed on the completed change. Documentation and the glossary
agree with ADR 0019; see [Lua Agent-type authoring](lua-agent-types.md) for the
final authoring and lifetime contract.
