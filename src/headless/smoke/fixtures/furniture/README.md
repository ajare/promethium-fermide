# Independent Furniture regression fixtures (#466)

These native Lua catalogues preserve the pre-#465 regression geometry, artwork,
UUIDs, definition/usable-point keys and dependent World identities. They are
independent of editable teaching content in `resources/test-worlds`.
Worlds remain YAML; intended seated/lying journeys now select `use-furniture`.
Geometry-only catalogues intentionally have no use callbacks. Their omitted
Actions still arrive Standing, without occupancy.

`furniture/bundledLua` compares teaching definitions and placements with these
regression baselines (teaching content adds use to geometry-only seating), loads
all dependent Worlds, checks their intended explicit use or default Idle, and
round-trips both World formats. Other domain-owned checks preserve routing, Local
depth, composition, support/overlap, reconciliation, history and ownership contracts.

Generated variants use `support/CatalogueSource.h` to compose native Lua source
before the fixture's `return catalogue`. The helper neither evaluates Lua nor
converts YAML: production catalogue/document loading validates every variant.
The separate `sit.furniture.lua` fixture has a seated point and a plain point;
its definition-owned callback deliberately does nothing for the plain point,
rather than silently assigning the former seat activity to every point.

No YAML Furniture catalogue inputs or builders remain. Production legacy-loader
and closed-enum removal are separately scoped to #467.
