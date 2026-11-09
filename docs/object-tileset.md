# Object atlas

`resources/textures/objects.png` is a 320x960 RGBA atlas laid out on a uniform
64x160 authored grid: 5 cells across by 6 down, one world unit to a cell rather than
a 64x64 square of screen pixels. The World cell's screen size is a separate fact:
the renderer stretches each resolved region into the World-unit rectangle it is
drawn into, so the artwork magnifies with the cell instead of being re-cut (#1007).
Unoccupied cell space is transparent. Small objects do not expand to fill their
cell.

`objects.tileset.yaml` records each sprite's cell, local content rectangle and
whether its RGB can be tinted. The manifest's `tile_size` is the authored grid:
how the artist laid the image out. The loader requires only that whole authored
cells cover the image with no remainder; it never compares that grid with the
World cell, because a World unit's screen size is a different fact and the
renderer stretches a resolved region into whatever destination it is drawn into
(#1006). A content rectangle that escapes the image, or that does not start
inside its own authored cell, is refused. Content rectangles exclude alpha
padding and may run from their own cell into a neighbour, as the wide Lying tile
does. Alpha padding never changes collision, selection or visual sizes.
The Agent and Marker sprites have grayscale RGB. Agents use the existing resolved
Agent colour, including tag overrides and selection, as the vertex tint carried on
the recorded command. Existing height scaling, horizontal centring and foot
position are preserved.

## Render semantics: offset, drawn size, tintable

A tileset file carries a `version`. Version 1 - what `objects.tileset.yaml` is -
records a cell, a content rectangle and a `tintable` flag. Version 2 adds the two
facts a version 1 renderer had to guess, per tile:

- `offset` - the render offset, in World units, from the tile's anchor. `+x` is
  right and `+y` is up, so a negative `y` hangs artwork below the anchor.
- `drawn_size` - the drawn size, in World units. One World unit of authored art is
  one authored tile, so a drawn size larger than the whole image cannot be drawn
  from it and is refused.

Both are required in a version 2 file and both must be finite. A missing field, a
`.nan` or an `.inf` is a load refusal rather than a silent default, because a
render offset that quietly became zero would put artwork where no author said it
goes. A drawn size that is zero, negative, non-finite, or larger than the image is
refused the same way. A version 1 file declares no render semantics, so the loader
gives it the format's own identity: no offset, and a drawn size of whatever the
content rectangle measures in World units over the authored grid.

`tintable` is the only source of tintability. The C++ rule that used to derive it
by matching a tile's name against `agent`, `human-*`, `robot-standing` and
`cleaning-bot-standing` is gone: a new Agent type can no longer become tintable by
accident, and a tile named without a hint can no longer fail to be.

The offset and the drawn size belong to the artwork and never to the domain object.
CONTEXT.md keeps that line - Support elevation is a physical fact and is explicitly
not a render offset - and putting a visual extent on the object would collapse it.
A Walkway is `Object(cellX, cellY, 1.0, 0.0)`: deliberately zero height so it
never obstructs pathing, routing or support, while its `walkway` tile draws a
24px slab at `offset: [0.0, -0.075]` and `drawn_size: [1.0, 0.075]`, hanging
that far below the floor line of the cell it was authored in.

## ObjectAtlasNew

`resources/textures/objects-new.tileset.yaml` is the version 2 authored tileset
behind the replacement Objects atlas, laid out on the 128x320 World cell at
768x960. It is the tracer-bullet set: the Walkway is its one live tile, and the
rest of the sheet drops in against the same image size. The old `ObjectAtlas` and
`ObjectAtlasImage` stay registered and canonical until nothing references them.

The `ImageSet` entries the application manifest declares for `ObjectAtlasNew` are
generated from the tileset, not authored beside it:

```sh
python3 scripts/generate_object_atlas_new.py --write   # regenerate
python3 scripts/generate_object_atlas_new.py --check   # verify, as CI does
```

The render smoke module checks the committed manifest against the tileset on every
run, so a drift fails the build rather than showing up as artwork in the wrong
place.

## The install seam

`installAtlasTiles()` in `pf-render` is the one place the two authorities meet,
and every viewport atlas installs through it:

- **Pixel regions come from the `ImageSet` resource.** It stays the runtime
  pixel-region contract ADR 0010 names, and it is the only place a drawn region is
  taken from.
- **Render semantics come from the authored tileset** the atlas is asked about.

Which tileset that is is stated in `resources/atlas-sets.yaml`, the authored
atlas-set registry: one entry per atlas, giving the tileset file and the
authored `kind` of that tileset (`sector` or `object`), resolved against the
registry. The install checks the declared kind against the slot it is filling,
so a mismatch is reported rather than silently loading the wrong shape.

The registry is a separate file because the manifest cannot carry the binding -
the vendored Willpower `ImageSet` schema fixes the Options an `ImageSet` is
allowed to carry (`filtering`, `uv-style`, `colour-space`, `wrapping`,
`mipmaps`), and extending the vendored SDK was rejected as a much wider blast
radius than the atlas work. The registry adds the binding without touching the
resource layer: the tileset stays the single authority for what a tile means,
and the registry only says which tileset is asked about for which atlas. The
render smoke module checks that every `ImageSet` in the manifest is covered by
the registry, that every named tileset loads, and that it defines every tile
the atlas declares, so the mapping cannot rot quietly.

A tile the `ImageSet` declares that the tileset never authored is refused, as is a
pixel region escaping the atlas image. Nothing in the install derives a tint from
a name or a drawn size from a domain object's geometry.


There is one object renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and emits the sprite quads into the World
command stream; the editor and the shipped game both draw from that same code and
see the same result. Neither application keeps an object renderer of its own, and
the renderer itself contains no ImGui code - the tint is a packed colour on the
command, not an ImGui vertex type. See
[ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

Supported sprites:

- Door leaf (all four opening styles): source UV cropping preserves sliding
  rather than compressing the sprite. Fully open leaves emit no geometry.
- Enabled/disabled Buttons, at the existing Button bounds.
- Clear, frosted and tinted Windows: back-sector rendering precedes the overlay.
  Clear glass has a transparent centre; tinted glass is translucent.
- BoothWindow frame and distinct inner shutter: fixed ordinary one-cell Window
  geometry. The transparent Open frame stays stationary; the opaque 42x38 shutter
  subregion of the Closed tile translates upwards behind it, clipped to its centre.
  See [BoothWindows](booth-windows.md) for authoring and regeneration.
- Agents and Markers, keeping the existing icon-fit dimensions.
- Platform lift car: the sprite follows the moving slab's actual bounds, not the
  shaft bounds. The editor shaft outline is retained.

Bulkhead Doors and Force Bridges remain entirely primitive-rendered. Other
transport geometry and editor overlays are unchanged. Headless tests without a
texture retain the primitive/font fallback.

The shared renderer loads and validates both the definition and PNG beside the
executable, under `textures/`. CMake deploys them even on builds without C++
changes. Missing or invalid assets cause a controlled startup error, and the GL
texture is released before the context is destroyed.

The original generated art is in `resources/textures/source/objects-generated.png`.
Run `python3 scripts/pack_object_atlas.py` (requires Pillow) to reproduce the packed
PNG and YAML. This crops the irregular generated sheet, normalizes cells, removes
near-transparent noise and neutralizes tintable sprites. The generated art remains
a draft; the packer makes its sizing and alpha layout deterministic.

The `sector-tileset` CTest also checks object quads, inverted screen-Y bounds,
Agent tint, door UV cropping, fully-open leaf suppression and cleanup.
The `render` smoke module's `objectAtlas/*` checks prove the loader contract
itself: an invented atlas on a 32x40 authored grid loads, resolves its regions
in that grid's units and draws them, while a content rectangle escaping its
cell or a grid leaving a remainder in the image is refused on the stated
requirement.
