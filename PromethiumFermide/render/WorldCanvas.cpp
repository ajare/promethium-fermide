#include "WorldCanvas.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/Vertex.h"
#include "core/World.h"

#include "MppFontMetrics.h"
#include "Render.h"

// The viewport the shared producer reads. `UISettings` is the shared
// renderer's own viewport and display-option struct - the same one the
// editor drives and the headless checks drive - so the game sets the fields
// it cares about and leaves the producer with one source of truth rather
// than a second, parallel settings type it would have to be kept in step
// with (#1002).
//
// The selection globals sit with it for the same reason. The game has no
// selection and no editor cursor, so each one answers "nothing selected,
// nothing hovered" and the producer draws no selection chrome - the same
// shape the headless link's stubs take. They are process globals because
// that is how the shared producer reaches them; nothing in the game's own
// code sets them.
UISettings gUISettings;

core::Agent* gHoveredAgent{ nullptr };
core::Agent* gSelectedAgent{ nullptr };
std::shared_ptr<const core::Vertex> gSelectedVertex;
std::shared_ptr<const core::Sector> gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject;
std::shared_ptr<const core::SectorObject> gSelectedSectorObject;

namespace pf::application::render {
namespace {

std::unique_ptr<MppFontMetrics> gFontMetrics;
IFontMetrics const* gPreviousMetrics{ nullptr };

// The game's camera, into the viewport the producer reads. Fitted to the
// World's Layer-0 content when there is no Player to follow; centred on the
// Player, at the Player's own Layer, when there is one.
void applyCamera(core::World const& world, float width, float height)
{
	gUISettings = UISettings{};
	gUISettings.worldViewportWidth = width;
	gUISettings.worldViewportHeight = height;
	// The game has no editor chrome: no grid, no graph overlay, no
	// next-Layer wireframe x-ray, no nearest-vertex highlight. Markers and
	// the Agent queue/planning badges are World information and do show, the
	// same way they show for the author.
	gUISettings.renderGrid = false;
	gUISettings.renderGraph = false;
	gUISettings.renderNextLayerWireframe = false;
	gUISettings.highlightNearestVertex = false;
	gUISettings.renderMarkers = true;
	gUISettings.renderAgentDebug = true;

	auto const sectors = world.getSectors(0);
	if (!sectors.empty())
	{
		core::Vector2 minimum, maximum;
		sectors.front()->getBounds(minimum, maximum);
		for (auto const& sector : sectors)
		{
			core::Vector2 low, high;
			sector->getBounds(low, high);
			minimum.x = std::min(minimum.x, low.x);
			minimum.y = std::min(minimum.y, low.y);
			maximum.x = std::max(maximum.x, high.x);
			maximum.y = std::max(maximum.y, high.y);
		}
		float const contentWidth = std::max(1.0f,
			(maximum.x - minimum.x) * CORE_CELL_WIDTH_PIXELS);
		float const contentHeight = std::max(1.0f,
			(maximum.y - minimum.y) * CORE_LEVEL_HEIGHT_PIXELS);
		gUISettings.worldZoom = std::max(0.001f, std::min({ 1.0f,
			width * 0.9f / contentWidth, height * 0.9f / contentHeight }));
		gUISettings.xOffset = (width - contentWidth * gUISettings.worldZoom) * 0.5f
			- minimum.x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		gUISettings.yOffset = (height - contentHeight * gUISettings.worldZoom) * 0.5f
			- minimum.y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
	}

	auto const player = world.lookupAgent(world.getPlayerControlledAgent());
	if (player.entity && player.entity->getSector())
	{
		auto const* agent = player.entity;
		auto centre = agent->getGlobalPosition();
		centre.y += agent->getHeight() * 0.5f;
		gUISettings.visibleLayer = static_cast<int>(agent->getSector()->getLayerIndex());
		gUISettings.worldZoom = 1.0f;
		gUISettings.xOffset = width * 0.5f - centre.x * CORE_CELL_WIDTH_PIXELS;
		gUISettings.yOffset = height * 0.5f - centre.y * CORE_LEVEL_HEIGHT_PIXELS;
	}
}

// The producer takes the World it draws as a shared handle so the render
// pass can hold one for the span of the call. The game's World is owned by
// its WorldRuntime and outlives every call made here, so this handle is a
// view, not a second owner: it deliberately extends nothing.
std::shared_ptr<const core::World> viewedWorld(core::World const& world)
{
	return std::shared_ptr<const core::World>(&world, [](core::World const*) {});
}

} // namespace

void createWorldCanvas(WorldRenderStackHost const& host)
{
	// The game's text answers go in before the stack is built, so the first
	// frame has a face to measure with and never falls back to a font nobody
	// chose (#999).
	if (!gFontMetrics) gFontMetrics = std::make_unique<MppFontMetrics>();
	gPreviousMetrics = installFontMetrics(gFontMetrics.get());
	try
	{
		createWorldRenderStack(host);
	}
	catch (...)
	{
		installFontMetrics(gPreviousMetrics);
		gPreviousMetrics = nullptr;
		gFontMetrics.reset();
		throw;
	}
}

void destroyWorldCanvas()
{
	destroyWorldRenderStack();
	if (gFontMetrics)
	{
		installFontMetrics(gPreviousMetrics);
		gPreviousMetrics = nullptr;
		gFontMetrics.reset();
	}
}

bool worldCanvasActive()
{
	return worldRenderStackActive();
}

WorldDrawList drawWorld(core::World const& world, float width, float height)
{
	WorldDrawList commands(WorldDrawList::ClipRectangle{ { 0, 0 }, { width, height } });
	if (width <= 0 || height <= 0 || world.getLayerCount() == 0) return commands;
	applyCamera(world, width, height);
	renderWorld(viewedWorld(world), &commands);
	return commands;
}

void renderWorldCanvas(core::World const& world, float width, float height)
{
	if (!worldRenderStackActive()) return;
	// The World chooses its own artwork; the shared renderer resolves and
	// installs it, and reports a bad selection rather than throwing over it.
	// A selection this set already has is a no-op, so calling every frame is
	// how the game follows a live artwork change without owning the switch.
	selectWorldSectorImageSet(world.sectorImageSetResourceName());
	selectWorldAgentImageSet(world.agentImageSetResourceName());
	selectWorldFurnitureImageSet(world.furnitureImageSetResourceName());
	auto const commands = drawWorld(world, width, height);
	if (commands.commands().empty()) return;
	renderWorldCommandsToHostTarget(commands, { 0.0f, 0.0f },
		{ width, height });
}

UISettings const& worldCanvasViewport()
{
	return gUISettings;
}

} // namespace pf::application::render
