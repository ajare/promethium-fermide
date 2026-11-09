# Object atlas

`resources/textures/objects.png` is a 320x640 RGBA atlas. Its uniform cells are
64x160 pixels: the same 1x1 world-unit footprint used by sector rendering, not
64x64 screen pixels. Unoccupied cell space is transparent. Small objects do not
expand to fill their cell.

`objects.tileset.yaml` records each sprite's cell, local content rectangle and
whether its RGB can be tinted. Content UVs map to the renderer's existing physical
bounds; alpha padding never changes collision, selection or visual sizes.
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
