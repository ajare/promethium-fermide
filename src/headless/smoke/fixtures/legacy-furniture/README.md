# Frozen regression migration inputs

These files are the exact pre-#465 bundled catalogue/World data. Existing
regressions that edit YAML catalogues now read this private fixture directory,
not editable teaching content. They are not bundled application resources or a
shipped YAML compatibility promise. Fixture builders and final loader removal
remain separately scoped (#466/#467).

The converted bundled catalogues and Worlds live in `resources/test-worlds`.
`furniture/bundledLua` compares their immutable geometry, artwork, keys, UUIDs and
placements against these frozen baselines, deliberately ignoring legacy per-point
actions only after auditing uniformity. The audit found no conflicting per-point
uses. New explicit-use coverage exercises Lua through World/document workflows.
