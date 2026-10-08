# Agent-type API v2 declarations — #521

Implemented only the declaration/frozen-data slice of #520. API v2 requires
supported canonical poses and dense ordered Room/Door choices with validated
height/speed ratios. Human preserves all five poses, geometry, context orders
and existing crossing speeds. Scout and inline builders are migrated; the
manifest includes a Standing-only Robot regression resource. Unknown/retired
pose fields and external v1 scripts are refused with migration diagnostics.

The frozen baseline carries declarations across existing survivor replay/history
boundaries. Creation, preview, load, Reset and deleted-Agent restoration validate
fresh definitions through the existing runtime. Persistence still records authored
identity/properties, not poses, definitions or Lua state; no schema change.
Capability/envelope observations explicitly distinguish absent poses. Existing
consumers read the new baseline without introducing unsupported-pose geometry.
Full context selection, no-fit movement and Furniture/lifecycle enforcement are
not implemented here; see #522–#524.

## Focused production-seam coverage

- `agentTypesPoseDeclarations`: bundled Human orders/speeds, real Standing-only
  Lua constructor, optional unsupported envelopes, effective Height, supported
  but non-automatic poses, canonical Lying geometry, YAML/binary identities.
- `agentTypesInvalidPoseDeclarations`: malformed/missing/unknown capabilities,
  ratio numeric types/ranges/float conversion, missing/empty/sparse/duplicate/
  unsupported/unknown choices, unknown fields and API versions; independent
  preview/creation refusal with resource/field diagnostics and no publication.
- Expanded revised-definition/Reset/load/history tests verify survivor freeze,
  fresh deletion restoration, invalid pose-contract rollback, authored
  preservation, legacy Human resolution and no serialized pose snapshots.
- Existing constructor isolation, execution/allocation budgets, preview caching,
  import/registration rollback, clearance, editor placement and lifetime tests
  continue to exercise the migrated real Lua resources/builders.

The final inventory checks exposed pre-existing omissions for script Mobility,
preview caches and the Mobility editor fixture. The affected Agent/Editor CLI
inventories and ownership manifest now include those existing checks as well as
the new declaration checks.

## Final Linux Release validation

Incremental inner-loop builds selected `pf-smoke-agent` and `pf-smoke-editor`.
Their functional and affected ownership/CLI/exhaustive contract checks passed
before the final integrated pass.

Final default inventory build (core, editor, headless modules, tools and compile
contracts), using the existing Release tree:

```sh
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh \
  --build-dir build-linux --config Release --lane final --build-only \
  --run-timeout 1800 all
```

Passed, exit 0. Local build evidence:
`build-linux/.pf-validation/run-5c59bcd30d0642089f6aba17224a0a5e/`.

Final repository-owned exhaustive inventory:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build-linux -C Release \
  -E '^willpower_' -j 8 --output-on-failure
```

**74/74 passed, zero failures, 157.88 seconds.** Includes all module functional,
CLI, concurrency and exhaustive contracts, ownership, harness/tooling, persistence,
rendering and controlled graphics startup coverage. No dialogs or display were
used; unmodified submodule tests were excluded. `git diff --check` passed.
No Debug or native Windows validation is claimed or required by this ticket.
