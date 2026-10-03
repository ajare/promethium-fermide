# Authored BoothWindows (#334)

BoothWindow is a fixed one-cell-wide, one-Level-high service aperture authored on
Layer L, with its back side on L+1. Both sides require an occupiable Room,
Corridor, or Facade and stable walkable Floor on the aperture's Level (Ground or
Walkway, not an extensible Force Bridge). Empty space, Backgrounds, Transits,
the back-most Layer, unsupported states, and malformed footprints are refused
before mutation.

The palette has a distinct BoothWindow slot. Selection offers only the authored
Initially Open checkbox, with Closed as the default; there are no resizing,
glass-style, Broken-condition, crossing, or physical control-panel controls.
Move, delete, copy/cut/paste, history, structural replay, and reset use normal
World document workflows. Edits that remove either Location or a supporting
Walkway remove the invalidated aperture and both approaches.

The two approaches are centred at x+0.5 on the walking Level, one per side,
connected only to that side's walking topology. Neither shutter state creates
a cross-Layer edge or a Traversal resource. Window base APIs reject glass states,
glass styles, and traversal configuration for BoothWindow. Animated shutter
operation and Agent-operated invisible Interaction points are deliberately left
to dependent tickets.

## Persistence

World schema **43** adds a distinct `boothWindow` construction record, appended to
the construction-kind enumeration to preserve legacy numeric kind values. YAML
and binary share this authored schema and atomic document validation:

```yaml
- type: boothWindow
  layer: 0
  y: 0
  x: 3
  cellsWide: 1
  levelsHigh: 1
  initialState: closed
```

`initialState` is `closed` or `open`. Traversability, glass style, and Broken
fields are unsupported, even when their values appear neutral. Older schema
versions cannot carry BoothWindow records. Ordinary Window and Door records
remain unchanged, and documents without BoothWindows remain compatible.

## Static presentation and checks

Geometry matches an ordinary one-cell Window: 0.1 horizontal insets, 0.2 vertical
offset, and 0.3 visible height. The ObjectAtlas ImageSet supplies
`booth-window-open` and `booth-window-closed`. Open reveals the Layer behind
through normal recursive, intersecting aperture clips and a transparent frame
centre. Closed omits the aperture pass and fills the centre with its shutter.
No Button tile or physical panel is drawn.

The procedural tiles occupy the previously unused object-atlas cell [4,1].
`python3 scripts/pack_booth_window_tiles.py` (Pillow) regenerates their pixels
without changing other atlas regions; ImageSet and tileset definitions retain
their distinct rectangles.

Focused headless module checks:

- World: `boothWindows/placementAndTopology`, `boothWindows/atomicRefusal`,
  `boothWindows/lifecycle`, `boothWindows/simultaneousAdjacentPairs`.
- Persistence: `boothWindows/authoredRoundTripAndMalformedRecords`.
- Render: `boothWindows/staticPresentationAndNestedClipping`.
- Editor: `boothWindows/historyAndClipboard`.
- Existing `sector-tileset`: verifies transparent Open and opaque Closed centres.

Use the [Linux validation procedure](linux-smoke-validation.md) for complete
Release/Debug validation. All feature checks are CPU-only and non-interactive.
