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
