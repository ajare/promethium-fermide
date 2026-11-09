# Sector textures

`resources/textures/sectors.tileset.yaml` defines the pixel regions used from
`sector-atlas-draft.png`. Version 1 uses explicit `[x, y, width, height]` rectangles
because the generated draft is not a uniform tile grid. Image dimensions and all
regions are validated before the shared renderer takes the tileset.

There is one sector renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and repeats its regions into the World command
stream; the editor and the shipped game both draw from that same code and see the
same result. The editor keeps no sector rendering of its own, and neither does the
game. See [ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

The renderer repeats a surface every 1x1 world unit, magnified to the World cell's
screen size, cropping partial cells. Shared boundary strips compose centre, end, corner and open-wall
variants rather than baking every combination into separate sprites. Floors and
ceilings appear only at sector bounds. Side strips use the existing wall-span
calculation, including partial-height openings into Corridors. Open spans receive
no strip. Room interiors therefore support arbitrary height without intermediate
floors. The current draft supplies small interior samples; it is not a finished
seamless art set.

Room, Corridor, Ladder, Lift, Shuttle, Stairwell and Staircase surfaces have
separate entries. Backgrounds and Facades intentionally retain their authored
opaque colours and no boundary strips. Vehicles, stair/ladder mechanisms,
thresholds, other objects, Agents and editor overlays retain their existing
rendering. Aperture clipping and render order are unchanged; wireframe mode
remains primitive-based.

CMake copies both assets beside each executable under `textures/`, including
on builds with no C++ changes. Startup resolves that directory relative to the
executable, not the working directory. Invalid or missing assets produce a
controlled startup error. The GL texture is deleted before the GL context.
Headless rendering without an installed texture retains the primitive fallback.

Run `ctest --test-dir build-linux -R 'sector-tileset|smoke-startup|smoke-render' --output-on-failure`
after building the GUI, headless and `pf-sector-tileset-checks` targets.
