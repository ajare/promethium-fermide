#include "WorldCanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/Vertex.h"
#include "core/World.h"

#include <willpower/common/Logger.h>

#include "MppFontMetrics.h"
#include "Render.h"
#include "WorldViewportZoom.h"

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

// Where a refused atlas selection is written, and the last thing written
// there. The selection runs every frame, so without remembering what was
// already reported one bad atlas would fill the log at frame rate.
wp::Logger* gAtlasReport{ nullptr };
std::string gAtlasReported;

// The frame's report: the game's own logger, once per distinct refusal.
void reportAtlasSelections(std::vector<WorldAtlasSelection> const& selections)
{
	if (gAtlasReport) reportWorldAtlasSelections(*gAtlasReport, selections,
		gAtlasReported);
}

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
		auto const viewLayer = agent->getSector()->getLayerIndex();
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		gUISettings.worldZoom = 1.0f;
		// The renderer lifts everything on the Layer the player stands on by
		// that Layer's accumulated floor offset on the way out (#1101). The
		// camera follows a point of that lifted picture, so it has to carry
		// the same lift into its own centring: leave it out and the Player
		// draws one offset off the canvas centre, and a Layer lifted by a
		// Level puts the Agent the whole game is watching off the top of the
		// window (#1106).
		//
		// The lift is read through the shared query the editor's cursor
		// inverts (#1105), at the zoom this branch pins, so the game cannot
		// scale the number differently from the picture the author aimed at.
		// With every floor depth zero the term is zero and the centring is the
		// arithmetic it always was.
		gUISettings.xOffset = width * 0.5f - centre.x * CORE_CELL_WIDTH_PIXELS;
		gUISettings.yOffset = height * 0.5f - centre.y * CORE_LEVEL_HEIGHT_PIXELS
			- pf::render::layerFloorLiftPx(world, viewLayer,
				gUISettings.worldZoom);
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

std::string worldAtlasReport(std::vector<WorldAtlasSelection> const& selections)
{
	// Each diagnostic already names its own atlas and its own reason, so
	// the report is the refusals joined rather than a restatement of them.
	std::string report;
	for (auto const& selection : selections)
	{
		if (selection.diagnostic.empty()) continue;
		if (!report.empty()) report += "\n";
		report += selection.diagnostic;
	}
	return report;
}

bool reportWorldAtlasSelections(wp::Logger& report,
	std::vector<WorldAtlasSelection> const& selections, std::string& lastReported)
{
	auto const message = worldAtlasReport(selections);
	// Nothing refused. Clearing what was last written is what keeps a later
	// failure audible: hold on to it and a World that recovers, then takes
	// the same bad atlas again, says nothing the second time - which is
	// the time somebody is looking for it.
	if (message.empty()) { lastReported.clear(); return false; }
	if (message == lastReported) return false;
	lastReported = message;
	report.error(message);
	return true;
}

std::array<WorldAtlasSlot, 4> const& worldAtlasSlots()
{
	// One row per viewport slot, each bound to the World's own selection
	// for that slot and to the renderer entry that installs it. The Objects
	// row is the one this ticket adds: the renderer's Objects entry is the
	// one that runs the Objects contract, so binding this row to it is
	// what makes a World's Objects choice - and its refusal - reach the
	// shipped game rather than only the editor (#1026).
	static std::array<WorldAtlasSlot, 4> const slots{{
		{"Sector",
			[](core::World const& world) -> std::string const&
			{ return world.sectorImageSetResourceName(); },
			&selectWorldSectorImageSet},
		{"Objects",
			[](core::World const& world) -> std::string const&
			{ return world.objectImageSetResourceName(); },
			&selectWorldObjectImageSet},
		{"Agent",
			[](core::World const& world) -> std::string const&
			{ return world.agentImageSetResourceName(); },
			&selectWorldAgentImageSet},
		{"Furniture",
			[](core::World const& world) -> std::string const&
			{ return world.furnitureImageSetResourceName(); },
			&selectWorldFurnitureImageSet},
	}};
	return slots;
}

std::vector<WorldAtlasSelection> applyWorldAtlasSelections(core::World const& world)
{
	std::vector<WorldAtlasSelection> selections;
	auto const& slots = worldAtlasSlots();
	selections.reserve(slots.size());
	for (auto const& slot : slots)
	{
		auto const& name = slot.selected(world);
		selections.push_back({slot.slot, name, slot.install(name)});
	}
	return selections;
}

std::vector<WorldLayerSectorSheet> applyWorldLayerSectorSheets(
	core::World const& world, LayerSectorSheetInstall const& install)
{
	std::vector<WorldLayerSectorSheet> sheets;
	std::set<std::string> takenUp;
	for (std::uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
	{
		auto const& binding = world.layerSectorImageSet(layer);
		// No binding is the inherit state, and an empty name asks for
		// nothing either: neither is taken up, and the Layer draws the
		// World's own sheet the Sector slot has already installed.
		if (!binding || binding->empty()) continue;
		// The World's own set is already in the registry under that name,
		// installed by the Sector slot as the sheet that draws. Putting it
		// through the alongside path again would replace that entry with a
		// copy of itself while the slot still believes it owns it.
		if (*binding == world.sectorImageSetResourceName()) continue;
		// Two Layers bound to one sheet take it up once. The renderer
		// caches per name so a second call would cost nothing, but the
		// sheet is one thing and this list says what the game took up.
		if (takenUp.insert(*binding).second)
			sheets.push_back({layer, *binding, install(*binding)});
	}
	return sheets;
}

WorldArtworkSync syncWorldArtwork(core::World const& world,
	LayerSectorSheetInstall const& install)
{
	WorldArtworkSync sync;
	// The World's own sheet goes through first. Both halves of this sync
	// skip a Layer bound to the World's own set, and the renderer makes
	// its skip against the Sector slot's current name - neither of which
	// reads right before that sheet is installed and drawn (#1094).
	sync.selections = applyWorldAtlasSelections(world);
	sync.layerSheets = applyWorldLayerSectorSheets(world, install);
	return sync;
}

void createWorldCanvas(WorldRenderStackHost const& host, wp::Logger* report)
{
	gAtlasReport = report;
	gAtlasReported.clear();
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
		gAtlasReport = nullptr;
		throw;
	}
}

void destroyWorldCanvas()
{
	destroyWorldRenderStack();
	gAtlasReport = nullptr;
	gAtlasReported.clear();
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
	// installs it, and reports a bad selection rather than throwing over
	// it. A selection this set already has is a no-op, so calling every
	// frame is how the game follows a live artwork change without owning
	// the switch. All four slots go every frame: leaving one out is how the
	// game came to draw the old Objects sheet while the World asked for
	// the new one, and the Objects slot is the one whose refusal
	// decides whether a Door's artwork sits inside its collision (#1026).
	//
	// The four slots are the World's own artwork. A Layer bound to its own
	// Sector sheet is a fifth thing the frame has to take up, and until it
	// did the shipped game drew the World's sheet everywhere no matter what
	// the Layers bound: the binding resolved through a registry nothing
	// on the game side had ever put the Layer's sheet into (#1094). The
	// install is cached per distinct name in the renderer, so this costs
	// the Layer walk and no disk read on a frame that changed nothing. A
	// Layer sheet the install refuses is not reported here: the game has no
	// artwork panel to show it in, and the load step already said so when
	// it could not take the sheet up at all. What the canvas does with a
	// sheet that resolved at load but paired badly is paint magenta, which
	// is the same visible answer every other refused atlas gets.
	auto const sync = syncWorldArtwork(world, &installWorldLayerSectorImageSet);
	reportAtlasSelections(sync.selections);
	// A Layer that stopped binding its sheet takes the sheet down with it
	// (#1097). The names the sync just installed are the ones still bound;
	// every sheet this stack holds under any other name is artwork nothing
	// points at, and until here it sat held until the stack was destroyed.
	//
	// This is the quiescent point the release needs: nothing of this frame
	// has been recorded or submitted yet, and the previous frame is already
	// presented. The render stack drops the batches binding a released sheet
	// as it releases it, so nothing is ever let go of under a live batch.
	std::set<std::string> stillBound;
	for (auto const& sheet : sync.layerSheets) stillBound.insert(sheet.name);
	releaseUnboundWorldLayerSectorSheets(stillBound);
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
