#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>

#include <mpp/Scene.h>

#include "DrawTypes.h"

class WorldDrawList;

namespace mpp
{
	class RenderSystem;
	class ResourceManager;
}

namespace wp::application::resourcesystem
{
	class ResourceManager;
}

//
// The shared renderer's MPP render stack (#1001, ADR 0010 as amended by the
// pf-render unification spec #995).
//
// This is the project's one MPP consumer. It owns the MPP logger, the MPP
// render system, the MPP resource manager, the Willpower application resource
// manager, the MPP scene, the offscreen canvas targets, and the triangle and
// line batch renderers with their data providers that turn a recorded
// `WorldDrawList` into pixels. Thick lines stay native line primitives: a
// segment's width is clamped to the driver's advertised aliased line-width
// range and is never expanded into triangle geometry.
//
// Lifecycle ordering is unforgiving. The stack is created against an
// already-current OpenGL context and must be destroyed while that context is
// still current - every GPU object it holds is created and released inside
// this module's lifetime, and nothing outside it may outlive the context.
//
// The command stream itself stays a pure CPU translation unit (WorldDrawList
// has no GL or MPP include). The GL-context requirement is confined to this
// seam, which is what keeps the headless Render smoke module cheap.
//

// Which offscreen canvas a command stream is submitted to. Both are composited
// by the caller; the shared renderer never presents one itself.
enum class WorldCanvas { World, LocationPlan };

// The rendered offscreen target, as an opaque handle. A caller composites it
// its own way: the editor hands it to ImGui as a texture id, another host
// could install it in its own scene. The shared renderer never reads it back,
// so nothing about the host's presentation leaks in here.
using OffscreenTargetId = std::uint64_t;

// The render infrastructure a host that already owns it lends to the shared
// renderer (#1002).
//
// The editor creates its own stack and hands the shared renderer nothing but a
// resource directory. The shipped game is different: its launcher creates the
// window, the MPP render system, the MPP and Willpower resource managers and
// the scene before the application module is even loaded, and the game is
// handed those objects by its state framework. A host-bound shared renderer
// draws through them and never owns them - it does not create them, resize them
// or destroy them, and it does not create a second render system beside the
// host's. Because the host's presentation is the window itself rather than an
// offscreen texture it composites, a host-bound stack submits into whatever
// render target the host has bound when the frame is drawn.
struct WorldRenderStackHost
{
	mpp::RenderSystem& renderSystem;
	mpp::ResourceManager& renderResources;
	wp::application::resourcesystem::ResourceManager& resources;
	mpp::ScenePtr scene;
};

// Creates the shared renderer over a host-owned stack. The four viewport
// atlases are installed from the host's resource manager: `ObjectAtlas` is
// mandatory, and a missing or foreign Sector, Agent or Furniture atlas is
// reported through the corresponding `selectWorld*ImageSet` diagnostic and
// drawn as magenta rather than failing the host's start-up. The Objects
// atlas carries a further check: a set that declares render semantics must
// contain every required Objects tile at the drawn size and offset the
// renderer draws it at, or the selection is refused (#1022).
//
// Throws `std::logic_error` if a stack already exists.
void createWorldRenderStack(WorldRenderStackHost const& host);

// Renders one CPU command stream into the host's currently-bound render
// target. Valid only for a host-bound stack; throws `std::runtime_error`
// otherwise, because silently drawing into an offscreen texture a host-bound
// caller never looks at would hide the mistake.
void renderWorldCommandsToHostTarget(WorldDrawList const& commands,
	pf::render::Vec2 canvasPosition, pf::render::Vec2 canvasSize);

// Creates the whole render stack against the current OpenGL context:
//
//   * the MPP logger and the Willpower logger, both written into the build
//     tree so diagnostics do not depend on the launch directory;
//   * the MPP render system, its resource manager and its core resources;
//   * the Willpower application resource manager, into which
//     `registerResourceTypes` installs the application's own Resource
//     subclasses before the manifest is scanned. The application's resource
//     classes live above this module, so the shared renderer owns the manager
//     and the application owns what goes into it;
//   * the `Resources.yaml` application asset manifest, and the
//     `core::ManifestCatalogResolver` that answers catalogue source lookups
//     from it;
//   * the mandatory viewport atlases - Sector, Object, Agent and Furniture -
//     installed as the sector, object, Agent and Furniture tilesets;
//   * the MPP scene.
//
// Failure is controlled, not half-drawn: a missing or unreadable manifest,
// unusable render infrastructure, or a missing/foreign/undecodable atlas
// throws, and the caller reports a broken install.
//
// Throws `std::logic_error` if a stack already exists.
void createWorldRenderStack(std::filesystem::path const& resourceDirectory,
	std::size_t drawableWidth, std::size_t drawableHeight,
	std::function<void(wp::application::resourcesystem::ResourceManager&)> const&
		registerResourceTypes);

// Follows the host window's drawable size. The offscreen canvas targets follow
// the canvas they are drawn into rather than the window, so MPP's window
// dimensions are not what the World canvases are sized by; this stays a
// no-op until a presentation pipeline needs it.
void resizeWorldRenderStack(std::size_t drawableWidth, std::size_t drawableHeight);

// Destroys the whole stack, in the order that releases every GPU object while
// the OpenGL context which created it is still current. Safe to call when no
// stack exists.
void destroyWorldRenderStack();

// True while the stack exists and can render.
bool worldRenderStackActive();

// The Willpower application resource manager the stack created, or nullptr
// when there is no stack. Callers above the renderer read the catalogue
// through this; they never construct their own manager.
wp::application::resourcesystem::ResourceManager* worldRenderResourceManager();

// Renders one CPU command stream into the canvas's offscreen target and
// returns the handle the caller composites. The canvas position and size
// cross on the shared drawing types (#997): the position is the origin the
// recorded geometry is translated by, and the size is the target's extent.
//
// Throws `std::runtime_error` when there is no render stack to submit to.
OffscreenTargetId submitWorldCommands(WorldDrawList const& commands,
	pf::render::Vec2 canvasPosition, pf::render::Vec2 canvasSize,
	WorldCanvas canvas = WorldCanvas::World);

// Installs the selected artwork before canvas commands are recorded. An
// invalid resource name leaves the corresponding tileset empty, which the
// draw helpers render as opaque magenta, and returns the diagnostic instead
// of throwing: a bad selection is recoverable at run time, unlike a missing
// atlas at startup.
//
// All four answer with the same shape, and an empty diagnostic means exactly
// one thing: the selection was accepted. Anything else - a set that is not
// an ImageSet, one that cannot carry the geometry the renderer draws from it,
// or there being no render stack at all - is reported. A caller that shows
// the diagnostic in a panel therefore never mistakes "nothing to say" for a
// selection that worked when the renderer was not there to make it (#1034).
std::string selectWorldSectorImageSet(std::string const& name);
std::string selectWorldObjectImageSet(std::string const& name);
std::string selectWorldAgentImageSet(std::string const& name);
std::string selectWorldFurnitureImageSet(std::string const& name);

// Installs one Layer's own Sector sheet under `name`, beside the sheet the
// Sector slot carries, without changing which sheet the draw path draws.
//
// This is what makes a Layer binding visible (#1091). The per-Layer
// resolution added in #1090 already asks the install registry for the name
// a Layer binds; what it could not do is put a sheet there, because every
// install entry point above takes the drawn sheet with it. A Layer bound to
// `name` draws this sheet; every Layer that binds nothing keeps drawing the
// World's selected sheet, exactly as it did before this was called.
//
// The install is done once per distinct name and the answer is kept, so a
// caller may ask for the same sheet every frame and pay for a lookup. A
// reload drops the kept answers, so the next ask installs from disk again.
//
// The answer has the same shape as the four selections above: empty means
// the sheet is installed and resolves, and anything else is the reason it
// is not - a set that is not an ImageSet, one whose tileset cannot be
// paired, or there being no render stack at all. That reason is what the
// Layers table shows on the combo (#1091).
//
// A refused name still leaves a sheet installed under itself. Every failure
// shape - the name missing, the name naming something that is not an Image
// set, a set whose tileset will not pair - puts a sheet in the registry and
// paints that Layer the missing-art magenta, so a binding the author can go
// and fix is visible on the canvas rather than looking like a Layer that
// binds nothing (#1096).
std::string installWorldLayerSectorImageSet(std::string const& name);

// Brings down every Layer Sector sheet the render stack took up that no
// Layer binds any more, and returns how many came down (#1097).
//
// `stillBound` is the set of sheet names a Layer still binds - the names
// the caller has just installed through the entry above. Every sheet the
// stack holds under a name missing from that set is artwork nothing points
// at. Until this existed the stack carried all of them until it was
// destroyed: unbinding a Layer never released the sheet it had been using,
// so a long editor session that cycled the Layers table through the
// manifest's Image sets held every one of them.
//
// The held set was always bounded - by the number of distinct Image set
// names the application ships, which is single digits - so this is a
// correctness gap being closed rather than a leak being stopped. What
// changes is that the stack no longer holds what nothing points at.
//
// The batches binding a released sheet are dropped in the same call, so no
// sheet is released while a live batch still binds its texture; the next
// frame rebuilds whatever it needs. The caller must ask at a point where no
// frame is mid-draw - both hosts ask from their per-frame artwork sync,
// which runs before anything is recorded or submitted and after the
// previous frame has been presented.
//
// A sheet another consumer also holds stays up: this gives back the
// reference the render stack took and no more, the same unload symmetry the
// Map load guard keeps (#1092).
//
// Returns the number of sheets released. Zero means either that nothing was
// unbound or that there is no render stack to release from; unlike the four
// atlas selections above, an empty answer here makes no claim that
// something succeeded, so no diagnostic is needed.
std::size_t releaseUnboundWorldLayerSectorSheets(
	std::set<std::string> const& stillBound);

// Reloads the artwork the stack draws from, off disk, the definitions and
// the images both.
//
// Every distinct installed Sector sheet goes back through it, not just the
// one the Sector slot carries: the walk is over the keyed install registry
// (#1087), which is what says which sheets exist - the World's own, every
// Layer's, and any sheet a recorded Sector batch bound itself to. Each
// sheet's `ImageSet` and its paired `Image` go back through the resource
// manager's refresh - unload, destroy, create, load - and the destroy is the
// load-bearing step: Willpower's `unload` leaves the resource flagged as
// created, and `_createResource` returns early on a created resource, so
// without it `parseData` never runs again and the texture is rebuilt from
// the pixels decoded at startup. With it, the PNG is read afresh. Each
// sheet's authored tileset is read again and re-paired with the pixel
// regions the `ImageSet` declares, and the atlas-set registry is re-parsed
// as well, rather than served from the cache the stack has been installing
// from, so a definition edited outside the editor since it started is what
// the next frame draws. The sheet that was being drawn keeps being drawn:
// a reload changes what every sheet is made of, not which sheet is on
// screen.
//
// The Objects, Agent and Furniture slots are reloaded with them, on the
// same refresh path.
//
// Returns a diagnostic naming every sheet and slot that failed to reload,
// one per line; empty means everything came back. A sheet that failed keeps
// its own diagnostic too, so the Artwork panel still reports it and it
// draws magenta until it is fixed, and the sheets that came back keep
// serving - one bad file does not stop the reload. With no render stack
// there is nothing to reload, and that is reported rather than answered
// with an empty string.
std::string reloadWorldArtwork();
