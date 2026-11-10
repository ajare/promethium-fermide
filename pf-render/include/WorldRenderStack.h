#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
