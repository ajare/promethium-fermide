# Render World canvases with MPP and manage application assets as Willpower resources

Status: accepted (amended)

Ticket #1003 of the shared-renderer unification spec (#995) re-homes the ownership
clause below. The owner of the MPP render system, the MPP resource manager and the
Willpower application resource manager is the shared renderer module, `pf-render`,
not an editor rendering service: that service no longer exists, and the editor and
the shipped game now take the same ownership and the same lifecycle from the module.
Every other decision here stands unchanged - MPP performs the drawing, Dear ImGui
composites the offscreen target, thick lines remain native line primitives and are
not expanded into triangles, there are no project-specific tileset resource
classes, and there are no parallel atlas metadata caches.

Ticket #1014 amends the second rejection below. Render semantics - a tile's
tintable flag, its render offset and its drawn size - now live one layer above the
resource, in the authored tileset file that produces it, rather than being derived
by the renderer from a tile's name or from a domain object's geometry. That is a
genuine change to "tintability and repetition are rendering semantics rather than
resource kinds": the semantics are still not a resource kind, but they are no
longer held nowhere at all. What the rejection was actually guarding against -
duplicate authorities that can diverge - is prevented by generating the `ImageSet`
entries from the authored tileset rather than maintaining both by hand. The
`ImageSet` remains the runtime pixel-region contract and the only source of a
drawn region; the install seam `installAtlasTiles()` pairs the two explicitly and
refuses a tile one side declares that the other does not know.

Dear ImGui remains the editor's immediate-mode user-interface layer, but World canvases are rendered by Massive Poly Pusher (MPP) into an offscreen render texture that ImGui presents as an image. A CPU-side `WorldDrawList` records ordered canvas primitives independently of either rendering API. The MPP renderer consumes that stream using native triangle and line batches, MPP text, clipping, and Willpower-managed atlas textures. Thick lines and outlines remain native line primitives and use the supported OpenGL line-width range; they are not expanded into triangle geometry.

The shared renderer module, `pf-render`, owns the MPP render system, MPP resource manager, and Willpower application resource manager, and destroys them before the OpenGL context. It is the project's one MPP consumer: neither application keeps a renderer of its own. Where a host already owns that infrastructure - the shipped game's launcher creates the window, the render system and both resource managers before the application module is loaded - the module draws through the host's stack instead of creating a second one beside it, and neither creates nor destroys what it was handed. `resources/Resources.yaml` is the application asset manifest. Sector and object atlases are Willpower `Image` and `ImageSet` resources. Worlds, Agent tag registries, Agent behaviour registries, and individual Lua modules have application `Resource` subclasses; dependencies in the manifest connect bundled Worlds to their registries and behaviour registries to their Lua modules. Interactive user documents may be registered programmatically instead of rewriting the bundled manifest.

## Considered options

- **Continue drawing World canvases directly with `ImDrawList`.** Rejected because it keeps simulation visualization tied to the UI backend and prevents a coherent MPP scene and resource lifecycle.
- **Expand thick lines and outlines into triangles.** Rejected because lines are a distinct primitive in the render plan and MPP supports native line batches. Platform line-width limits are handled by clamping to the graphics driver's advertised range.
- **Introduce project-specific tile-set resource classes.** Rejected because Willpower `ImageSet` already provides named atlas rectangles and UV coordinates. Tintability and repetition are rendering semantics rather than resource kinds.
- **Keep atlas metadata and registry ownership in parallel global caches.** Rejected because duplicate authorities permit the rendered or edited object to diverge from the resource-manager object.
- **Move the whole editor UI to MPP.** Rejected because Dear ImGui remains appropriate for menus, panels, controls, docking, and presentation of the offscreen World texture.

## Consequences

World rendering can be tested at the stable CPU command-stream boundary without depending on ImGui's internal vertex tessellation. Draw-order barriers and clip rectangles are explicit. MPP performs the GPU drawing and owns the offscreen target, while ImGui only composites that target into the viewport. Application assets fail during shared-renderer startup when required manifests, render infrastructure, or atlas resources are unavailable.

Registry resources own the loaded registry instances used to resolve a World resource. In accordance with ADR 0008, an unavailable or refused Agent behaviour registry remains recoverable World dependency state rather than malformed World data; tag-registry incompatibility remains a World-load failure. Lua remains sandboxed and registry-local as specified by ADR 0008—the resource layer adds managed script assets and lifecycle without changing script capabilities or execution semantics.
