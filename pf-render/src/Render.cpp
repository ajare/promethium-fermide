#include <array>
#include <cassert>
#include <cfloat>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "core/Defines.h"
#include "core/Background.h"
#include "core/Facade.h"
#include "core/World.h"
#include "PlayerTransportSelectorDrawing.h"
#include "core/AirlockTransit.h"
#include "core/ChamberTransit.h"
#include "core/Location.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/AccessPanel.h"
#include "core/WalkwaySectorObject.h"
#include "core/Button.h"
#include "core/Marker.h"

#include "Render.h"
#include "AgentPathForRendering.h"
#include "SectorTileset.h"
#include "AtlasInstall.h"
#include "ObjectTileset.h"
#include "WorldDrawList.h"
#include "UISettings.h"


extern UISettings gUISettings;
extern core::Agent* gHoveredAgent, *gSelectedAgent;
extern std::shared_ptr<const core::Vertex> gSelectedVertex;
extern std::shared_ptr<const core::Sector> gSelectedSector;
extern std::shared_ptr<const core::SectorObject> gHoveredSectorObject, gSelectedSectorObject;

//
// The World currently being rendered. A clear Window composites the
// Backgrounds behind it from the back Layer's cell grid (#37, and #36 made
// the Window's single back Sector non-authoritative for such a span), and
// the renderSector -> renderSectorObjects -> renderWindow chain which
// reaches renderWindowClear carries no World pointer. A RenderWorldScope
// installs it for exactly one render and restores the previous World when it
// goes out of scope. renderWorld() opens one per call; a headless caller that
// drives the Sector-level passes directly opens its own when those passes need
// the World, instead of leaving it to process-global state.
//
static std::shared_ptr<const core::World> gRenderWorld;

RenderWorldScope::RenderWorldScope(std::shared_ptr<const core::World> world)
	: mPrevious(std::move(gRenderWorld))
{
	gRenderWorld = std::move(world);
}

RenderWorldScope::~RenderWorldScope()
{
	gRenderWorld = std::move(mPrevious);
}

// Text is measured and tessellated through the font metrics the application
// installed (#999). The producer names a face and a size; it never names a
// font, never asks ImGui which font is current, and holds no icon-font global.

using namespace std;
// The Marker glyph the icon face is asked for, from the shared renderer's own
// text vocabulary (#1003) - no icon-font header from the editor's UI layer is
// included here.
using pf::render::MarkerIconGlyph;

PackedColourValue ForeLocationColour = packColour(192, 192, 255);
PackedColourValue BackLocationColour = packColour(224, 224, 255);
PackedColourValue LadderColour = packColour(128, 128, 192);
PackedColourValue LiftColour = packColour(128, 128, 192);
PackedColourValue ShuttleColour = packColour(128, 128, 192);
PackedColourValue StairwellColour = packColour(128, 128, 192);
PackedColourValue VertexColour = packColour(255, 128, 0);
PackedColourValue EdgeColour = packColour(255, 128, 0);
PackedColourValue InterLayerEdgeColour = packColour(255, 255, 64);
PackedColourValue SelectedColour = packColour(255, 255, 0);

#define RENDER_SECTOR_OBJECTS_BEHIND 1
#define RENDER_SECTOR_OBJECTS_INFRONT 2

void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, PackedColourValue colour, WorldDrawList* drawList);
void renderSectorDepthContent(shared_ptr<const core::Sector> sector, WorldDrawList* drawList);

// Laid over the cells one Door stands in, after its leaves. Defined with the
// doorframe vocabulary further down this file and declared here because the
// threshold pass is where the frame goes down (#1073). The frame is Sector
// artwork, so the pass hands it the sheet it frames against rather than
// letting it ask the install which sheet that is (#1088).
void renderDoorframe(std::shared_ptr<const core::Door> const& door,
	pf::render::InstalledSectorSheet const* sheet, WorldDrawList* drawList);

// Laid over the cells one Window stands in, after its glass. Defined with
// the windowframe vocabulary further down this file and declared here
// because the threshold pass is where the frame goes down (#1075), and
// handed the drawn sheet for the same reason the doorframe is (#1088).
void renderWindowframe(std::shared_ptr<const core::Window> const& window,
	pf::render::InstalledSectorSheet const* sheet, WorldDrawList* drawList);

// The Sector sheet a pass draws from: the sheet the install registry is
// drawing (#1087). Every Sector draw helper below is handed this handle
// rather than going to the registry for itself, so no draw path can ask a
// tile question of a sheet it is not drawing (#1088).
//
// This is the one place a Sector pass decides which sheet it draws. A
// later slice resolves that from the Sector's own Layer Image set; every
// helper under here keeps taking the handle it was handed, so the answer
// changes at this one call and nowhere else.
pf::render::InstalledSectorSheet const* sectorDrawSheet()
{
	return pf::render::sectorSheetRegistry().drawing();
}

void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	std::vector<TransitAperture> const& apertures, WorldDrawList* drawList);

void renderStaircase(shared_ptr<const core::Staircase> staircase, WorldDrawList* drawList);

void transformPosition(core::Vector2& p)
{
	p.x *= CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
	p.y *= CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
	p.y = gUISettings.worldViewportY + gUISettings.worldViewportHeight - p.y;

	p.x += gUISettings.worldViewportX + gUISettings.xOffset;
	p.y -= gUISettings.yOffset;
}


void transformPosition(float& x, float& y)
{
	core::Vector2 p{ x, y };

	transformPosition(p);

	x = p.x;
	y = p.y;
}


// The same world-to-screen transform, applied in place to a shared point. The
// producer carries rectangles in pf::render::Vec2 now, so the pass does not
// have to drop back to the domain point type to move a corner (#998).
void transformPosition(Vec2& point)
{
	core::Vector2 p{ point.x, point.y };

	transformPosition(p);

	point.x = p.x;
	point.y = p.y;
}


void renderSelectedQueues(shared_ptr<const core::World> const& world, int layer,
	WorldDrawList* drawList)
{
	shared_ptr<const core::Object> selectedObject;
	if (gSelectedSectorObject)
	{
		selectedObject = gSelectedSectorObject->_getObject();
	}
	else if (gSelectedSector)
	{
		switch (gSelectedSector->getType())
		{
		case core::SectorType::Ladder:
			selectedObject = static_pointer_cast<const core::LadderTransit>(gSelectedSector)->getLadder();
			break;
		case core::SectorType::Lift:
			selectedObject = static_pointer_cast<const core::LiftTransit>(gSelectedSector)->getLift();
			break;
		case core::SectorType::Shuttle:
			selectedObject = static_pointer_cast<const core::ShuttleTransit>(gSelectedSector)->getShuttle();
			break;
		case core::SectorType::Stairwell:
			selectedObject = static_pointer_cast<const core::StairwellTransit>(gSelectedSector)->getStairwell();
			break;
		case core::SectorType::Staircase:
			selectedObject = static_pointer_cast<const core::StaircaseTransit>(gSelectedSector)->getStaircase();
			break;
		default:
			break;
		}
	}

	// Lift-owned landing Doors have their own threshold resource containing the
	// physical queue geometry. World::getTraversalResourceId() deliberately
	// resolves them to the Lift coordinator for scheduling, so select the landing
	// resource directly when rendering that Door's queue lane.
	core::TraversalResourceId resourceId;
	auto schedulingResourceId = world->getTraversalResourceId(selectedObject.get());
	if (gSelectedSectorObject
		&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door)
	{
		resourceId = static_pointer_cast<const core::DoorSectorObject>(
			gSelectedSectorObject)->getDoor()->getTraversalResourceId();
	}
	else resourceId = schedulingResourceId;
	if (!resourceId) return;
	auto const& snapshot = world->getSimulationSnapshotView();
	auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
		[resourceId](auto const& candidate) { return candidate.id == resourceId; });
	if (resource == snapshot.traversalResources.end()) return;

	const PackedColourValue laneColour = packColour(0, 210, 255, 210);
	const PackedColourValue occupiedColour = packColour(255, 170, 0, 230);
	const float slotRadius = max(5.0f,
		CORE_RESOURCE_SLOT_WIDTH * CORE_CELL_WIDTH_PIXELS * 0.35f)
		* gUISettings.worldZoom;

	// Door queue geometry belongs to one approach layer, so only show the visible
	// side. A selected Ladder is different: its two approach lanes are its lower
	// and upper ends, and both must remain visible even when the Ladder itself is
	// represented by a Back-layer transit.
	for (auto const& lane : resource->queueLanes)
	{
		if (!lane.sector) continue;
		auto sector = world->getSector((uint32_t)lane.sector.value - 1);
		if (!sector || (!resource->isLadder
			&& sector->getLayerIndex() != (uint32_t)layer)) continue;
		auto start = lane.origin;
		auto end = lane.origin + lane.direction * lane.extent;
		transformPosition(start);
		transformPosition(end);
		drawList->AddLine({ start.x, start.y }, { end.x, end.y }, laneColour, 3.0f);
	}

	// Capacity and transport queues do not necessarily have external physical
	// slots. Mark their waiting agents in place, preserving the resource's order.
	vector<core::TraversalRequestId> queuedRequests;
	auto appendUnique = [&](core::TraversalRequestId request)
	{
		if (request && find(queuedRequests.begin(), queuedRequests.end(), request) == queuedRequests.end())
			queuedRequests.push_back(request);
	};
	for (auto const& lane : resource->queueLanes)
		for (auto request : lane.queue) appendUnique(request);
	for (auto request : resource->admissionQueue) appendUnique(request);
	for (auto const& zone : resource->shuttleAccessZones)
		for (auto request : zone.queue) appendUnique(request);
	for (auto request : resource->liftConfirmationQueue) appendUnique(request);

	for (uint32_t rank = 0; rank < queuedRequests.size(); ++rank)
	{
		auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& candidate) { return candidate.id == queuedRequests[rank]; });
		if (request == snapshot.traversalRequests.end()) continue;
		auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
			[&](auto const& candidate) { return candidate.id == request->owner; });
		if (agent == snapshot.agents.end() || !agent->sectorId) continue;
		auto sector = world->getSector((uint32_t)agent->sectorId.value - 1);
		if (!sector || sector->getLayerIndex() != (uint32_t)layer) continue;
		auto const* entity = world->lookupAgent(agent->id).entity;
		if (!entity) continue;
		auto point = agent->globalPosition
			+ core::Vector2{ entity->getWidth() * 0.5f, entity->getHeight() * 0.5f };
		transformPosition(point);
		drawList->AddCircle({ point.x, point.y }, slotRadius + 3.0f, occupiedColour, 0, 3.0f);
		auto label = string("Q") + to_string(rank + 1);
		drawList->AddText({ point.x + slotRadius + 5.0f, point.y - 7.0f }, occupiedColour, label.c_str());
	}
}

void renderGrid(shared_ptr<const core::World> const& world, PackedColourValue const& colour,
	float width, WorldDrawList* drawList)
{
	core::Vector2 topLeft{ 0.0f, (float)world->getLevelsHigh() };
	core::Vector2 bottomRight{ (float)world->getCellsWide(), 0.0f };
	transformPosition(topLeft);
	transformPosition(bottomRight);

	// Include both outer edges, not just the cell separators. Building the grid
	// from world coordinates also keeps its far-right and bottom lines present
	// when the canvas is larger than the world.
	for (uint32_t x = 0; x <= world->getCellsWide(); ++x)
	{
		float screenX = topLeft.x
			+ x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		drawList->AddLine({ screenX, topLeft.y }, { screenX, bottomRight.y }, colour, width);
	}

	for (uint32_t y = 0; y <= world->getLevelsHigh(); ++y)
	{
		float screenY = bottomRight.y
			- y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
		drawList->AddLine({ topLeft.x, screenY }, { bottomRight.x, screenY }, colour, width);
	}
}


bool isMarkerPathTarget(core::World const& world, core::Vertex const& vertex)
{
	auto marker = dynamic_pointer_cast<core::Marker>(vertex.getObject());
	auto sector = vertex.getSector();
	return marker && world.lookupMarker(marker->getId()) == marker && sector
		&& sector->getIndex() < world.getNumSectors() && world.getSector(sector->getIndex()) == sector;
}

shared_ptr<const core::Vertex> markerPathTargetAtPosition(core::World const& world,
	uint32_t layer, core::Vector2 position, float radius)
{
	if (!(radius > 0)) return {};
	shared_ptr<const core::Vertex> closest;
	float distance = radius * radius;
	if (auto graph = world.getGraph())
		for (auto const& vertex : graph->getVertices())
			if (isMarkerPathTarget(world, *vertex) && vertex->getSector()->getLayerIndex() == layer)
			{
				auto candidateDistance = position.distanceToSq(vertex->getPosition());
				if (candidateDistance < distance) { distance = candidateDistance; closest = vertex; }
			}
	return closest;
}

void renderGraph(shared_ptr<const core::Graph> graph, shared_ptr<const core::World> world,
	WorldDrawList* drawList, bool markerTargetsOnly,
	std::optional<core::Vector2> cursorWorldPosition)
{
	if (!gUISettings.renderGraph || !drawList)
	{
		return;
	}

	shared_ptr<core::Path> path = gSelectedAgent ? gSelectedAgent->getPath() : nullptr;

	auto layer = (uint32_t)gUISettings.visibleLayer;

	auto const& vertices = graph->getVertices();
	auto const& edges = graph->getEdges();

	shared_ptr<const core::Vertex> closestVertex{ nullptr };

	if (gUISettings.highlightNearestVertex && cursorWorldPosition)
	{
		auto const& mousePos = *cursorWorldPosition;
		auto sector = world->getSectorAtPosition(layer, mousePos.x, mousePos.y);
	
		if (sector)
		{
			closestVertex = markerTargetsOnly
				? markerPathTargetAtPosition(*world, layer, mousePos, FLT_MAX)
				: graph->getClosestVertexInSector(sector.get(), mousePos);
		}
	}

	// Increase the y position slightly so we can see floors, force bridges, etc
	const float yBump = -2;
	float lineThickness = -yBump + 1;

	if (!markerTargetsOnly) for (auto const& edge : edges)
	{
		auto v0 = edge->getVertex(0);
		auto v1 = edge->getVertex(1);

		auto pos0 = v0->getPosition();
		auto pos1 = v1->getPosition();

		transformPosition(pos0);
		transformPosition(pos1);

		pos0.y += yBump;
		pos1.y += yBump;

		bool edgeIsInPath{ false };

		if (path)
		{
			auto edgeInPath = find_if(path->nodes.begin(), path->nodes.end(), [edge](auto node)
			{
				return node.edge && node.edge->sameAs(edge);
			});

			edgeIsInPath = edgeInPath != path->nodes.end();
		}

		auto interLayerEdgeColour = edgeIsInPath ? packColour(255, 0, 0) : InterLayerEdgeColour;
		auto ladderMountColour = edgeIsInPath ? packColour(255, 0, 0) : packColour(0, 192, 255);
		auto liftMountColour = edgeIsInPath ? packColour(255, 0, 0) : packColour(0, 192, 255);
		auto edgeColour = edgeIsInPath ? packColour(255, 0, 0) : EdgeColour;
		auto edgeThickness = edgeIsInPath ? 5.0f : lineThickness;

		if (edge->isInterLayer())
		{
			// If Edge is intra-Layer, then it's guaranteed that both Vertices have the same X/Y position.
			core::Vector2 posl = pos0, posr = pos0, posu = pos0;
			float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

			posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
			posl.x -= sideSize;
			posr.x += sideSize;

			drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, interLayerEdgeColour);
		}
		else if (edge->getType() == core::EdgeType::LadderMount)
		{
			if (v0->getSector()->getLayerIndex() == layer || v1->getSector()->getLayerIndex() == layer)
			{
				core::Vector2 posl = pos0, posr = pos0, posu = pos0;
				float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

				posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
				posl.x -= sideSize;
				posr.x += sideSize;

				drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, ladderMountColour);
			}
		}
		else if (edge->getType() == core::EdgeType::LiftMount)
		{
			if (v0->getSector()->getLayerIndex() == layer || v1->getSector()->getLayerIndex() == layer)
			{
				core::Vector2 posl = pos0, posr = pos0, posu = pos0;
				float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

				posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
				posl.x -= sideSize;
				posr.x += sideSize;

				drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, liftMountColour);
			}
		}

		if (v0->getSector()->getLayerIndex() == layer && v1->getSector()->getLayerIndex() == layer)
		{
			drawList->AddLine({ pos0.x, pos0.y }, { pos1.x, pos1.y }, edgeColour, edgeThickness);
		}
	}

	for (auto vertex : vertices)
	{
		if (vertex->getSector()->getLayerIndex() != layer
			|| (markerTargetsOnly && !isMarkerPathTarget(*world, *vertex)))
		{
			continue;
		}

		auto pos = vertex->getPosition();

		transformPosition(pos.x, pos.y);

		pos.y += yBump;

		bool vertexIsInPath{ false };

		if (path)
		{
			auto vertexNodeIt = find_if(path->nodes.begin(), path->nodes.end(), [vertex](auto node)
			{
				return node.targetVertex->sameAs(vertex);
			});

			if (vertexNodeIt != path->nodes.end())
			{
				vertexIsInPath = true;
			}
		}

		auto vertexColour = vertexIsInPath ? packColour(255, 0, 0) : VertexColour;
		
		float outlineSize{ 2 };
		if (gSelectedVertex && vertex->sameAs(gSelectedVertex))
		{
			vertexColour = packColour(255, 0, 0);

			drawList->AddRect(
				{ pos.x - RENDER_VERTEX_SIZE - outlineSize, pos.y - RENDER_VERTEX_SIZE - outlineSize },
				{ pos.x + RENDER_VERTEX_SIZE + outlineSize, pos.y + RENDER_VERTEX_SIZE + outlineSize },
				vertexColour
			);

			outlineSize += 2;
		}
		if (vertex == closestVertex)
		{
			vertexColour = packColour(255, 128, 0);

			drawList->AddRect(
				{ pos.x - RENDER_VERTEX_SIZE - outlineSize, pos.y - RENDER_VERTEX_SIZE - outlineSize },
				{ pos.x + RENDER_VERTEX_SIZE + outlineSize, pos.y + RENDER_VERTEX_SIZE + outlineSize },
				vertexColour
			);

			outlineSize += 2;
		}

		drawList->AddCircleFilled({ pos.x, pos.y }, RENDER_VERTEX_SIZE, vertexColour);

	}
}


namespace
{
// A normalised rectangle in screen pixels.
//
// Every member carries its own zero, so a rect that was never assigned is
// an empty rect - `empty()` says so, and nothing is drawn under it - rather
// than four indeterminate floats that would clip whatever the stack
// happened to be holding where they sat (#1068).
struct ScreenRect
{
	float x0{0.0f};
	float y0{0.0f};
	float x1{0.0f};
	float y1{0.0f};

	bool empty() const
	{
		return x1 <= x0 || y1 <= y0;
	}
};

// The compile-time half of that rule. With the initialisers in place the
// default constructor is not the trivial one that leaves the members
// indeterminate; take them off and this assert is the message (#1068).
static_assert(!std::is_trivially_default_constructible_v<ScreenRect>,
	"ScreenRect members must carry a default initialiser: an unset rect has to "
	"read as empty, not as indeterminate memory");

// A normalised rectangle in World units. The render transform flips y, so
// a rect is normalised on the way in and projected on the way out.
//
// Defaulted to the zero rect for the same reason as ScreenRect, and for the
// same consequence: a rect no opening style arm wrote reads as empty rather
// than as indeterminate memory, and an empty rect draws nothing (#1068).
struct WorldRect
{
	float x0{0.0f};
	float y0{0.0f};
	float x1{0.0f};
	float y1{0.0f};

	bool empty() const
	{
		return x1 <= x0 || y1 <= y0;
	}

	// Whether this rect still covers more than `tolerance` of `o` on every
	// axis. The tolerance is what makes a leaf that has travelled exactly
	// to the jamb read as clean out: the arithmetic that lands an inner
	// edge exactly on a jamb arrives a hair either side of it, and a hair
	// is far under a tenth of a pixel at any zoom a World is drawn at.
	// Without it a fully open Door paints a leaf sliver nobody asked for.
	bool overlaps(WorldRect const& o, float tolerance) const
	{
		return x1 > o.x0 + tolerance && x0 < o.x1 - tolerance
			&& y1 > o.y0 + tolerance && y0 < o.y1 - tolerance;
	}

	WorldRect clippedTo(WorldRect const& o) const
	{
		return WorldRect{max(x0, o.x0), max(y0, o.y0), min(x1, o.x1), min(y1, o.y1)};
	}

	ScreenRect projected() const
	{
		auto a = core::Vector2{x0, y0};
		auto b = core::Vector2{x1, y1};
		transformPosition(a);
		transformPosition(b);
		return ScreenRect{min(a.x, b.x), min(a.y, b.y), max(a.x, b.x), max(a.y, b.y)};
	}
};

// The same compile-time guard ScreenRect carries: the rects a plan holds are
// WorldRects, so the style that never wrote one hands the draw an empty rect
// - which draws nothing - instead of four indeterminate floats (#1068).
static_assert(!std::is_trivially_default_constructible_v<WorldRect>,
	"WorldRect members must carry a default initialiser: an unset rect has to "
	"read as empty, not as indeterminate memory");

// A leaf whose remaining overlap with the doorway is no wider than this is
// treated as clean out of it. See WorldRect::overlaps.
constexpr float kLeafCullTolerance = 1e-4f;

// One Door leaf: a full-cell tile drawn at its cell rect, translated by
// `travel` and clipped to the doorway aperture. `uv0`/`uv1` select the
// source band, which is the whole tile except for the two halves a
// 1-wide OpenApart Door cuts out of a single L0 (#1056).
struct DoorLeaf
{
	// Which leaf family this leaf wears. A 1-wide OpenApart Door wears
	// both halves of L0, so this is the tile the leaf draws from rather
	// than the side of the doorway it sits on.
	bool rightLeaf;
	// The cell rect the leaf is drawn at, before the travel.
	WorldRect cell;
	// The band of the tile the leaf wears, as fractions of the tile.
	core::Vector2 uv0;
	core::Vector2 uv1;
	// The animation vector: how far the leaf has slid from its cell.
	core::Vector2 travel;
	// Which of the leaf's vertical edges faces the Door's centre.
	// OpenApart draws its inner edges as visible seams, and which edge
	// that is belongs to the leaf rather than to the travel - at 0% open
	// the travel is zero and its sign says nothing at all.
	bool innerEdgeIsLeft;

	// Where the leaf sits once it has slid.
	WorldRect slid() const
	{
		return WorldRect{cell.x0 + travel.x, cell.y0 + travel.y,
			cell.x1 + travel.x, cell.y1 + travel.y};
	}
};

// What a Door's opening style contributes to the one draw: the leaves to
// draw and the doorway they clip to. Everything else - the clip itself,
// the back Sector drawn through it, the frame laid over it, the wireframe
// pass - is shared (#1056).
//
// There is no vacated region here. The leaves used to go down first and the
// back Sector was then clipped to whatever the leaves had left, which made
// the reveal a fact the style had to state. The Sector behind is now drawn
// across the whole aperture before the leaves, so the leaves cover what they
// cover and no style has to say how much of the doorway that is (#1073).
struct DoorLeafPlan
{
	WorldRect aperture;
	std::vector<DoorLeaf> leaves;
	// OpenApart draws each leaf's inner edge as a visible seam; the other
	// styles have no inner edge to draw.
	bool seamPerLeaf = false;
};

DoorLeafPlan planDoorLeaves(shared_ptr<const core::Door> const& door)
{
	core::Vector2 ap0, ap1;
	door->getFullShape(ap0, ap1);

	auto const openPct = door->getOpenPercentage();
	auto const cellsWide = door->getCellsWide();
	auto const cellsWideF = static_cast<float>(cellsWide);

	auto const apLeft = min(ap0.x, ap1.x);
	auto const apBottom = min(ap0.y, ap1.y);
	auto const apertureWidth = max(ap0.x, ap1.x) - apLeft;
	auto const apertureHeight = max(ap0.y, ap1.y) - apBottom;

	// The aperture is the Door's own shape: inset from the cell by
	// CORE_DOOR_X_INSET on each x side, and standing on the cell's floor.
	// The cell rect a leaf is drawn at is therefore the aperture widened
	// back out by that inset and carried up to a full cell.
	auto const cellX0 = apLeft - CORE_DOOR_X_INSET;
	auto const cellY0 = apBottom;

	DoorLeafPlan plan;
	plan.aperture = WorldRect{apLeft, apBottom, apLeft + apertureWidth,
		apBottom + apertureHeight};

	// The leaf is drawn at its whole cell - one World cell wide and one
	// Level tall - whatever the doorway's own height. The clip, not the
	// drawn size, is what keeps it inside the doorway, which is what lets
	// one tile serve every Door width and every height scale.
	auto addLeaf = [&](bool rightLeaf, float x0, float x1,
		core::Vector2 uvA, core::Vector2 uvB, core::Vector2 travel,
		bool innerEdgeIsLeft)
	{
		plan.leaves.push_back(DoorLeaf{rightLeaf, {x0, cellY0, x1, cellY0 + 1.0f},
			uvA, uvB, travel, innerEdgeIsLeft});
	};

	// The whole-cell leaf one of the Door's own cells wears. The left cell
	// wears L0 and every other cell R0, so a 2-wide Door draws the two
	// cells of one joined leaf.
	auto cellLeaf = [&](uint32_t index, core::Vector2 travel)
	{
		auto const x0 = cellX0 + static_cast<float>(index);
		addLeaf(index != 0, x0, x0 + 1.0f,
			{0.0f, 0.0f}, {1.0f, 1.0f}, travel, false);
	};

	switch (door->getOpenStyle())
	{
	case core::Door::OpenStyle::OpenUp:
	{
		// The joined leaf rises out of the aperture. Its bottom edge
		// starts on the cell floor, which is also the aperture's bottom
		// edge, so travelling the aperture height clears it exactly.
		auto const travel = openPct * apertureHeight;
		for (uint32_t i = 0; i < cellsWide; ++i)
			cellLeaf(i, {0.0f, travel});
		break;
	}

	case core::Door::OpenStyle::OpenLeft:
	case core::Door::OpenStyle::OpenRight:
	{
		// The joined leaf slides sideways out of the doorway. Its
		// trailing edge - the edge last to leave the doorway, and so the
		// edge the visible coverage is made of - is what the animation
		// is pinned by (#1061, Option A):
		//
		//   p = 0  on the far jamb, so the reveal begins the instant the
		//          Door moves and the leaf never reads as frozen;
		//   p = 1  on the near jamb, so a fully open Door leaves nothing
		//          painted across either jamb.
		//
		// Between the pins it moves at a uniform speed, so the visible
		// edge covers exactly one aperture width over the Door's
		// open/close time, which is the travel #1052 states. The leaf's
		// own cell puts its trailing edge one inset outside the far jamb,
		// so that inset is the offset the journey starts from rather than
		// extra distance. Dropping it would leave the leaf's last inset
		// painted across the far jamb of a Door that reads as fully open;
		// spending it at the start instead - travelling from the cell edge
		// - would burn the first inset behind the far jamb, under the
		// clip, and stall the visible edge for it.
		//
		// What the shift costs is where the closed leaf sits on its tile:
		// a 1-wide OpenLeft Door sees u[0.2,1.0] of L0 where the source
		// crop saw u[0.1,0.9], and the matching OpenRight Door sees the
		// mirror u[0.0,0.8]. L0 is symmetric about u = 0.5 for the
		// 1-wide OpenApart split, so the two bands are mirror images and
		// each still holds the symmetry axis.
		auto const travel = CORE_DOOR_X_INSET + openPct * apertureWidth;
		auto const sign = door->getOpenStyle() == core::Door::OpenStyle::OpenLeft
			? -1.0f : 1.0f;

		for (uint32_t i = 0; i < cellsWide; ++i)
			cellLeaf(i, {sign * travel, 0.0f});
		break;
	}

	case core::Door::OpenStyle::OpenApart:
	{
		// Two equal leaves split at the aperture centre and slide
		// outward, so the gap they leave is always centred. Each travels
		// half an aperture width, which takes a leaf's inner edge from
		// the centre to its own jamb exactly.
		auto const halfWidth = apertureWidth * 0.5f;
		auto const centre = cellX0 + cellsWideF * 0.5f;
		auto const travel = openPct * halfWidth;
		plan.seamPerLeaf = true;

		// Split the cell span at the centre. A cell the centre cuts
		// through becomes two half-cell leaves wearing the two halves of
		// L0 - the 1-wide case, and the reason L0's artwork has to be
		// symmetric about u = 0.5.
		for (uint32_t i = 0; i < cellsWide; ++i)
		{
			auto const x0 = cellX0 + static_cast<float>(i);
			auto const x1 = x0 + 1.0f;
			if (x1 <= centre)
				addLeaf(false, x0, x1, {0.0f, 0.0f}, {1.0f, 1.0f},
					{-travel, 0.0f}, false);
			else if (x0 >= centre)
				addLeaf(true, x0, x1, {0.0f, 0.0f}, {1.0f, 1.0f},
					{travel, 0.0f}, true);
			else
			{
				addLeaf(false, x0, centre, {0.0f, 0.0f}, {0.5f, 1.0f},
					{-travel, 0.0f}, false);
				addLeaf(false, centre, x1, {0.5f, 0.0f}, {1.0f, 1.0f},
					{travel, 0.0f}, true);
			}
		}
		break;
	}

	default:
		// A style with no arm here used to reach the draw with a `reveal`
		// holding whatever the stack had in it, and clip the back Sector to
		// that. The rects default to empty now, so the omission cannot paint
		// garbage; this is the loud half, so a missing arm says so at the
		// frame it lands on instead of drawing a Door with no leaves. The
		// style is reported by value, because this arm is only ever reached
		// by a style this file does not name (#1068).
		throw std::logic_error("Door opening style #"
			+ std::to_string(static_cast<int>(door->getOpenStyle()))
			+ " has no leaf plan in planDoorLeaves");
	}

	return plan;
}
} // namespace

// Draws a Door through one parameterised draw for all four opening styles
// (#1056).
//
// A leaf is a full-cell tile drawn at its cell rect, translated by the
// style's animation vector and clipped to the doorway aperture. The clip
// is the doorway itself, so a leaf cannot paint outside it however far it
// travels; the translation, and not a source crop taken from the leaf's
// own edge, is what animates it. That is what lets one tile serve both
// widths - the clip picks the band, so a 2-wide Door reads as one joined
// two-cell leaf rather than one stretched tile - and what lets the closed
// leaf sit wherever its style wants on that band. The side-sliding styles
// want it one inset further in than the aperture, which is what #1061
// pays for a reveal that starts at p = 0: a 1-wide OpenLeft Door sees
// u[0.2,1.0] of L0 where the source crop saw u[0.1,0.9], and the
// matching OpenRight Door sees the mirror u[0.0,0.8].
//
// The style contributes only the leaves to draw and the region they
// vacate; see planDoorLeaves.
void renderDoorLeaves(shared_ptr<const core::Door> door, uint32_t layer,
	LayerRenderStyle style, WorldDrawList* drawList)
{
	auto const plan = planDoorLeaves(door);
	auto const aperture = plan.aperture.projected();

	auto const doorColour = packColour(64, 192, 255);
	auto const seamColour = packColour(0, 0, 0);

	// A leaf paints only while some part of it is still inside the
	// doorway. Once it has slid clean out it emits no geometry at all, so
	// a fully open Door leaves nothing for the wireframe pass to stroke a
	// zero-width outline of.
	auto const drawnLeaves = [&](auto&& use)
	{
		for (auto const& leaf : plan.leaves)
		{
			if (!leaf.slid().overlaps(plan.aperture, kLeafCullTolerance))
				continue;
			use(leaf, leaf.slid().projected());
		}
	};

	if (style == LayerRenderStyle::Solid)
	{
		// The three things a Door puts over its wall, in the order they go
		// down. The wall's own tile is already there - the Sector surface
		// pass drew it across this Door's cells as it did across every other
		// cell of that wall - and what the Door adds is the space behind,
		// the leaves over it, and the frame over both (#1073).
		drawList->AddDrawCmd();

		// 1. The space behind, clipped to the doorway.
		//
		// Clipped to the whole aperture rather than to the strip the leaves
		// have vacated, because the leaves are drawn after it and cover what
		// they cover. That is the whole of the reason the reveal stops being
		// a thing this draw computes: a closed Door paints its leaves over
		// the back Sector, an opening Door slides them off it, and the
		// doorway is the one clip both want. The aperture is also the clip
		// the recursion needs - a Door in the Sector behind is seen through
		// this one and carries its own aperture down the stack - and a
		// nested threshold clipped to its parent's vacated strip would go
		// missing wherever the parent's leaves happened to still lie.
		//
		// A Door is authored on the front Layer of its pair, so index 1 is
		// the Sector on the Layer directly behind.
		//
		// Skipped while the Door is closed, which is not a change of order
		// but a cull inside it: a closed Door's leaves stand across the
		// whole doorway, so every pixel of that back-Sector draw is painted
		// over before it reaches the screen. The old draw got the same saving
		// for free by clipping the back Sector to the vacated strip, which
		// was empty when nothing was vacated; the new order does not compute
		// a strip, so the saving has to be asked for. It is worth asking:
		// a closed Door is the common case, and without this every one of
		// them would render the Sector behind it on every frame to put it
		// under a leaf.
		drawList->PushClipRect({aperture.x0, aperture.y0},
			{aperture.x1, aperture.y1}, true);
		auto const backSector = door->isClosed()
			? nullptr : door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer),
				LayerRenderStyle::Aperture, false, BackLocationColour, drawList);
		}
		drawList->PopClipRect();
		drawList->AddDrawCmd();

		// 2. The leaves, over the space behind and under the same clip.
		// The clip is the doorway, so no leaf can paint over the wall its
		// collision stops in, however far it has travelled.
		drawList->PushClipRect({aperture.x0, aperture.y0},
			{aperture.x1, aperture.y1}, true);

		drawnLeaves([&](DoorLeaf const& leaf, ScreenRect const& r)
		{
			// The leaf family a Door wears is chosen by what the Door
			// *does*, not by the wall it sits in (#1057): a Lift landing
			// set in a Corridor wall still draws the lift leaf, because
			// the Lift owns the Door. A Shuttle hatch is authored one
			// cell wide, so its family has no right-hand leaf and the
			// hatch tile stands in for both roles rather than reaching
			// across families for art nobody drew.
			//
			// Among ordinary Doors the height class then picks the pair
			// (#1058). A Tall Door's aperture is taller than the regular
			// leaf is drawn for, so the regular panel reads as a short
			// door with dead wall above it inside the doorway; the tall
			// pair is authored for that aperture. Ownership is asked first:
			// a transport landing keeps its own art whatever its height
			// happens to read, so the two rules cannot trade places and
			// quietly re-skin a lift as a tall room door.
			//
			// One statement rather than a chain of assignments, because the
			// choice is one question asked once, and because every arm of it
			// has to stay within reach of the call that draws what it picked.
			char const* tile = door->isLiftOwned()
				? (leaf.rightLeaf ? "door-lift-R0" : "door-lift-L0")
				: door->isShuttleOwned() ? "door-shuttle-L0"
				: door->getHeight() == core::Door::Height::Tall
				? (leaf.rightLeaf ? "door-tall-R0" : "door-tall-L0")
				: (leaf.rightLeaf ? "door-R0" : "door-L0");
			if (!drawObjectSprite(tile, drawList, {r.x0, r.y0}, {r.x1, r.y1},
				Vec2{leaf.uv0.x, leaf.uv0.y},
				Vec2{leaf.uv1.x, leaf.uv1.y}))
				drawList->AddRectFilled({r.x0, r.y0}, {r.x1, r.y1}, doorColour);

			if (plan.seamPerLeaf)
			{
				// The inner edge is the centre seam while closed and the
				// facing edge of the two leaves while part open. It stays
				// a drawn primitive rather than trusting two tile edges
				// to land on the same pixel.
				auto const inner = leaf.innerEdgeIsLeft ? r.x0 : r.x1;
				drawList->AddLine({inner, aperture.y0}, {inner, aperture.y1},
					seamColour);
			}
		});

		drawList->PopClipRect();
		drawList->AddDrawCmd();

		// 3. The frame, over the wall and the leaf edges alike. Last, so
		// its moulding is what the eye reads as the edge of the opening
		// whatever the leaves have done underneath it, and so a frame whose
		// hole does not match its opening costs a band of wall inside the
		// moulding rather than the see-through hole it was when the frame
		// stood in for the wall (#1062, retired by #1073).
		renderDoorframe(door, sectorDrawSheet(), drawList);
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddDrawCmd();
		drawList->PushClipRect({aperture.x0, aperture.y0},
			{aperture.x1, aperture.y1}, true);

		drawnLeaves([&](DoorLeaf const&, ScreenRect const& r)
		{
			drawList->AddRect({r.x0, r.y0}, {r.x1, r.y1}, packColour(0, 0, 0));
		});

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
}


// Keep the condition badge above the threshold, leaving the frozen leaf visible.
template<typename Device>
static void renderBrokenWarning(Device const& device, WorldDrawList* drawList, bool currentShape = false)
{
	if (!device.isBroken()) return;
	core::Vector2 low, high;
	if (currentShape) device.getCurrentShape(low, high);
	else device.getFullShape(low, high);
	core::Vector2 anchor{ (low.x + high.x) * 0.5f, high.y };
	transformPosition(anchor);
	drawList->AddTriangleFilled(Vec2(anchor.x, anchor.y - 16.0f),
		Vec2(anchor.x - 7.0f, anchor.y - 3.0f),
		Vec2(anchor.x + 7.0f, anchor.y - 3.0f), packColour(255, 166, 26, 255));
	auto const ink = packColour(30, 20, 0, 255);
	drawList->AddLine(Vec2(anchor.x, anchor.y - 12.0f),
		Vec2(anchor.x, anchor.y - 8.0f), ink, 2.0f);
	drawList->AddRectFilled(Vec2(anchor.x - 1.0f, anchor.y - 6.0f),
		Vec2(anchor.x + 1.0f, anchor.y - 4.0f), ink);
}

// One draw serves every opening. The leaves, the frame and the selection
// outline are each drawn as their own artwork or their own primitive;
// nothing in the pass multiplies a colour into a tile.
void renderDoor(shared_ptr<const core::Door> door, uint32_t layer,
	LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	// One draw serves every opening style: the style is read inside the
	// plan, so there is no per-style dispatch left here (#1056).
	renderDoorLeaves(door, layer, style, drawList);

	if (selected)
	{
		core::Vector2 bounds0, bounds1;
		door->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, packColour(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
	// Owned transport Doors share failure, but the warning belongs to the
	// whole transport and follows its car rather than each stationary landing.
	if (door->isBreakable()) renderBrokenWarning(*door, drawList);
}


namespace
{
// Draws a tile at its authored render offset and drawn size from a World-unit
// anchor. The anchor is where the object's own geometry starts; the artwork
// goes where the authored tileset says it goes, which need not be where the
// object is. Returns false when the tile is not installed or declares no
// drawn size, so the caller keeps its primitive fallback (#1014).
bool drawOffsetTile(char const* name, core::Vector2 worldAnchor,
	WorldDrawList* drawList)
{
	auto const artwork = objectTileArtwork(name);
	if (!artwork || artwork->drawnSize.x <= 0.0f || artwork->drawnSize.y <= 0.0f)
		return false;
	core::Vector2 from{ worldAnchor.x + artwork->offset.x,
		worldAnchor.y + artwork->offset.y };
	core::Vector2 to{ from.x + artwork->drawnSize.x, from.y + artwork->drawnSize.y };
	transformPosition(from);
	transformPosition(to);
	return drawObjectSprite(name, drawList,
		{ min(from.x, to.x), min(from.y, to.y) },
		{ max(from.x, to.x), max(from.y, to.y) });
}

// Draws the part of a tile's artwork a leaf has not yet retracted into its
// top edge: the top `1 - progress` of the World-unit extent `fullMin` to
// `fullMax`. The source crop follows the retraction, so the artwork slides
// out of view through the top edge instead of squashing into the strip
// that is left below it. `progress` is the device's own open state, not a
// frame counter, which is what keeps a half-open leaf exactly half open
// however long it took to get there (#1019).
//
// Returns false when the tile is not installed, so the caller keeps its
// primitive fill.
bool drawRetractedTile(char const* name, core::Vector2 fullMin,
	core::Vector2 fullMax, float progress,
	WorldDrawList* drawList)
{
	if (!objectSpriteSize(name)) return false;
	auto const fraction = std::min(std::max(progress, 0.0f), 1.0f);
	core::Vector2 from{ fullMin.x, fullMin.y + (fullMax.y - fullMin.y) * fraction };
	core::Vector2 to{ fullMax.x, fullMax.y };
	transformPosition(from);
	transformPosition(to);
	return drawObjectSprite(name, drawList,
		{ min(from.x, to.x), min(from.y, to.y) },
		{ max(from.x, to.x), max(from.y, to.y) },
		{ 0.0f, fraction }, { 1.0f, 1.0f });
}

// The Bulkhead Door's artwork is the slab itself. The tile declares the
// slab's own 0.2 x 0.7 drawn size at its declared offset from the door's
// anchor, which is the bottom-left of the door's physical geometry - and
// that geometry straddles the boundary between the two Locations the door
// separates, 0.1 into each. So the artwork crosses the cell boundary
// exactly where the collision does, and is drawn at the slab's aspect
// ratio rather than the cell's 2:5 (#1019).
//
// The slab opens vertically from the floor, so what is visible is what is
// still down: the visible height is the declared extent less the live open
// state, which is why a door half open shows exactly half open.
bool drawBulkheadDoorSlab(shared_ptr<const core::BulkheadDoor> const& door,
	WorldDrawList* drawList)
{
	auto const artwork = objectTileArtwork("bulkhead-door");
	if (!artwork || artwork->drawnSize.x <= 0.0f || artwork->drawnSize.y <= 0.0f)
		return false;
	core::Vector2 full0, full1;
	door->getFullShape(full0, full1);
	core::Vector2 const extentMin{ full0.x + artwork->offset.x,
		full0.y + artwork->offset.y };
	core::Vector2 const extentMax{ extentMin.x + artwork->drawnSize.x,
		extentMin.y + artwork->drawnSize.y };
	return drawRetractedTile("bulkhead-door", extentMin, extentMax,
		door->getOpenPercentage(), drawList);
}
}

void renderBulkheadDoor(shared_ptr<const core::BulkheadDoor> door, uint32_t /* layer */, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	core::Vector2 bounds0, bounds1;

	door->getCurrentShape(bounds0, bounds1);

	auto doorColour = packColour(128, 192, 182);
	if (style == LayerRenderStyle::Wireframe)
	{
		transformPosition(bounds0);
		transformPosition(bounds1);
		drawList->AddRect({ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) },
			{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) }, packColour(0, 0, 0));
	}
	else if (!drawBulkheadDoorSlab(door, drawList))
	{
		transformPosition(bounds0);
		transformPosition(bounds1);
		drawList->AddRectFilled({ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) },
			{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) }, doorColour);
	}
	if (selected)
	{
		core::Vector2 full0, full1;
		door->getFullShape(full0, full1);
		transformPosition(full0); transformPosition(full1);
		auto const topLeft = Vec2{ min(full0.x, full1.x), min(full0.y, full1.y) };
		auto const bottomRight = Vec2{ max(full0.x, full1.x), max(full0.y, full1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, packColour(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
	renderBrokenWarning(*door, drawList);
}


void renderWindowClear(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	window->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, packColour(0, 0, 0));
		return;
	}

	// A Window is authored on the front Layer of its pair, so index 1 is the
	// Sector on the Layer directly behind.
	auto backSector = window->getBackSector();
	auto const backLayer = core::layerBehind(layer);

	// The aperture's rect in world (cell) units, before the screen transform.
	core::Vector2 shapeMin, shapeMax;
	window->getFullShape(shapeMin, shapeMax);
	Vec2 const worldMin{ shapeMin.x, shapeMin.y };
	Vec2 const worldMax{ shapeMax.x, shapeMax.y };

	drawList->AddDrawCmd();

	// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering.
	drawList->PushClipRect({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, true);

	// What the glass shows is derived from the back Layer's cell grid, not from
	// the Window's single back Sector (#36 made that reference
	// non-authoritative). Each Background the aperture looks into is drawn
	// clipped to the intersection of the Window rect and that Background's own
	// rect, so the seam between two Backgrounds lands exactly on the cell
	// boundary between them - no bleed past it, no seam line across it - and
	// stays pinned to the world as the viewport scrolls.
	auto const regions = gRenderWorld
		? backgroundApertureRegions(*gRenderWorld, backLayer, worldMin, worldMax)
		: std::vector<BackgroundApertureRegion>{};

	if (!regions.empty())
	{
		// Cells the span looks through which hold no Background contribute no
		// region; black stands in behind the glass there, as it does for a
		// Window with no back Sector at all. The opaque fills below cover it
		// wherever a Background is present.
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, packColour(0, 0, 0));

		for (auto const& region : regions)
		{
			auto clip0 = region.min;
			auto clip1 = region.max;
			transformPosition(clip0);
			transformPosition(clip1);

			drawList->AddDrawCmd();
			drawList->PushClipRect(
				{ min(clip0.x, clip1.x), min(clip0.y, clip1.y) },
				{ max(clip0.x, clip1.x), max(clip0.y, clip1.y) }, true);

			// The Background behind the glass is drawn as its own artwork,
			// clipped to this stretch of the aperture; the region clip trims
			// the draw, and nothing tints it (#34).
			renderSector(region.background, backLayer, LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else if (backSector)
	{
		// The back Sector is drawn the way a back Sector is always drawn:
		// its own artwork, under this aperture's clip. It carries no colour
		// of its own into the glass because it carries no colour at all.
		renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
			BackLocationColour, drawList);
	}
	else
	{
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, packColour(0, 0, 0));
	}

	drawList->PopClipRect();
	drawList->AddDrawCmd();
}


void renderWindowFrosted(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	renderWindowClear(window, layer, style, selected, drawList);
}


void renderWindowTinted(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	renderWindowClear(window, layer, style, selected, drawList);
}


void renderBoothWindow(shared_ptr<const core::BoothWindow> booth, uint32_t layer,
	LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Wireframe)
	{
		renderWindowClear(booth, layer, style, false, drawList);
		return;
	}
	if (!isDrawnSolid(style)) return;
	core::Vector2 from, to;
	booth->getFullShape(from, to);
	transformPosition(from); transformPosition(to);
	Vec2 topLeft{min(from.x, to.x), min(from.y, to.y)};
	Vec2 bottomRight{max(from.x, to.x), max(from.y, to.y)};
	// These insets are the transparent centre of the supplied 52 x 48 frame.
	float const width = bottomRight.x - topLeft.x, height = bottomRight.y - topLeft.y;
	Vec2 innerMin{topLeft.x + width * 5.0f / 52.0f, topLeft.y + height * 5.0f / 48.0f};
	Vec2 innerMax{bottomRight.x - width * 5.0f / 52.0f, bottomRight.y - height * 5.0f / 48.0f};
	float const travel = (innerMax.y - innerMin.y) * booth->getProgress();
	if (booth->getProgress() > 0.0f)
	{
		drawList->PushClipRect({innerMin.x, innerMax.y - travel}, innerMax, true);
		renderWindowClear(booth, layer, style, false, drawList);
		drawList->PopClipRect();
	}
	// Only the shutter translates. Its clip intersects any enclosing aperture.
	drawList->PushClipRect(innerMin, innerMax, true);
	Vec2 shutterMin{innerMin.x, innerMin.y - travel};
	Vec2 shutterMax{innerMax.x, innerMax.y - travel};
	if (!drawObjectSprite("booth-window-shutter", drawList, shutterMin, shutterMax))
		drawList->AddRectFilled(shutterMin, shutterMax, packColour(90, 106, 116));
	drawList->PopClipRect();
	if (!drawObjectSprite("booth-window-open", drawList, topLeft, bottomRight))
		drawList->AddRect(topLeft, bottomRight, packColour(148, 162, 170), 0.0f, 0, 2.0f);
}

// The glass one Window's pane wears, laid down by repetition (#1079).
//
// One tile per glass Style, repeated across the pane the way a Sector
// surface repeats across a wall, rather than one pane-sized image
// stretched over whatever rectangle the Window happens to be. A tall
// window repeats the tile rather than pulling it four times out of shape;
// a wide window repeats it rather than smearing it sideways.
//
// The repeat is one quad per tile and not a wrapped texture coordinate.
// The atlas is a single texture shared by every tile in the set, so
// wrap-mode sampling cannot be enabled for one region without bleeding the
// neighbouring tiles' artwork in. Each whole tile emits one quad sampling
// the full tile, which is the same shape the Sector surface's per-cell
// loop already uses.
//
// A cut tile samples only the fraction that fits. A pane's dimensions are
// never a whole multiple of the tile - a 1x1 pane is 0.8 x 0.3 World
// units against a 0.25 x 0.25 tile - so its last column and its last row
// are cut. Each is drawn at its remaining size sampling only the matching
// fraction of the tile, so the pattern stays at one artwork pixel to one
// screen pixel and the cut reads as a cut rather than as a whole tile
// squeezed into the sliver that was left.
//
// The repeat is anchored to the pane's bottom-left, not to the World
// grid: the pattern belongs to the pane, so two windows side by side each
// start their pattern at their own edge rather than forming one sheet
// across the wall. The cut therefore always falls on the pane's far edges -
// its right and its top - and never somewhere inside it.
//
// Returns false when the installed atlas carries no tile of that name, in
// which case nothing was drawn. That is the missing-tile answer every tile
// gives: no quad, never the missing-art colour, and the space behind the
// aperture keeps showing through the glass's absence instead of a hole
// being punched where the pane should have been.
bool drawGlassRepeating(std::string const& tile, Vec2 paneMin, Vec2 paneMax,
	WorldDrawList* drawList)
{
	auto const artwork = objectTileArtwork(tile);
	if (!artwork || artwork->drawnSize.x <= 0.0f || artwork->drawnSize.y <= 0.0f)
		return false;

	// The tile's screen extent, read through the same transform the pane's
	// own corners went through rather than restated from the cell metrics,
	// so the repeat cannot drift from the pane about how a World unit maps
	// to a screen pixel at this zoom. The transform flips y, which is why
	// the step is read as an absolute extent.
	core::Vector2 origin{}, step{ artwork->drawnSize.x, artwork->drawnSize.y };
	transformPosition(origin);
	transformPosition(step);
	auto const tileWidth = std::abs(step.x - origin.x);
	auto const tileHeight = std::abs(step.y - origin.y);
	if (tileWidth <= 0.0f || tileHeight <= 0.0f) return true;

	float const left = min(paneMin.x, paneMax.x);
	float const right = max(paneMin.x, paneMax.x);
	float const top = min(paneMin.y, paneMax.y);
	// Screen y grows downward, so the pane's World bottom - the anchor the
	// repeat is built from - is its *largest* y.
	float const bottom = max(paneMin.y, paneMax.y);
	// Anything thinner than this is the transform's own rounding rather than
	// a tile, and a sub-pixel quad sampling a sub-pixel slice of artwork
	// says nothing about the pattern either way.
	constexpr float kSliver = 1e-3f;
	if (right - left <= kSliver || bottom - top <= kSliver) return true;

	for (int column = 0;; ++column)
	{
		// Each edge is multiplied out fresh rather than accumulated, so a
		// long span cannot drift a pixel sideways by the end of it.
		float const x0 = left + static_cast<float>(column) * tileWidth;
		if (x0 >= right - kSliver) break;
		float const x1 = min(x0 + tileWidth, right);
		float const across = (x1 - x0) / tileWidth;

		for (int row = 0;; ++row)
		{
			// Rows stack upward from the anchor, so a row's *lower* screen y
			// is its top edge and its larger y is the edge it was laid from.
			float const lower = bottom - static_cast<float>(row) * tileHeight;
			if (lower <= top + kSliver) break;
			float const upper = max(lower - tileHeight, top);
			float const down = (lower - upper) / tileHeight;

			// The tile's bottom edge sits on the pane's bottom, so a cut row
			// loses the tile's *top*: it samples from 1 - down to 1. A cut
			// column loses the tile's right, so it samples from 0 to `across`.
			drawObjectSprite(tile.c_str(), drawList, { x0, upper }, { x1, lower },
				Vec2{ 0.0f, 1.0f - down }, Vec2{ across, 1.0f });
		}
	}
	return true;
}

// What a pane's glass resolved to: the tile to wear, and how it is laid
// down. `tile` is empty when the installed Object set carries neither
// name, which is the missing-tile answer every tile gives - no quad, never
// the missing-art colour, and the space behind the aperture keeps showing
// through the glass's absence instead of a hole being punched where the
// pane should have been.
struct GlassDraw
{
	std::string tile;
	// True when the tile repeats across the pane; false when it is one quad
	// stretched over the whole pane.
	bool repeats{ false };
};

// The glass one Window's pane wears, resolved against the installed Object
// set (#1083).
//
// A version 2 set carries `glass-<style>`: one small tile repeated across
// the pane (#1079). A version 1 set carries no such name. What it carries
// is the pane-sized `window-<style>` art every World drew before the
// repeating tile existed - art authored to be stretched over one whole
// pane, at the pane's own extent. #1079 stopped asking for those names,
// and every set that only had them was left drawing no glass at all: the
// frame still framed, the space behind still showed, and the pane was
// simply missing. That is a finished sheet's artwork turned into dead
// pixels, not an unfinished sheet being filled one tile at a time, and it
// is the case this settles rather than leaves as a side effect.
//
// The answer chosen is to keep the fallback reachable, the way #1063 kept
// the Door palette slot's fallback reachable under the same version 1
// theme sets and the way `carriageTileOrLegacy` keeps a Shuttle carriage
// drawing its own art under a set that predates the role names. The
// alternative - dropping `window-clear` / `-frosted` / `-tinted` from the
// version 1 required-tile contract - would have made the silence explicit
// at the contract but would have changed the picture under those themes
// for nothing.
//
// The resolution is a named call rather than a second conditional at the
// draw, so the two authored vocabularies are one decision made in one
// place, and so the Objects atlas contract reads the second name as what
// it is: the version 1 arm of a fallback, which the version 1 set is
// still required to carry.
GlassDraw glassTileOrLegacy(std::string const& repeating,
	std::string const& stretched)
{
	if (objectTileArtwork(repeating)) return { repeating, true };
	if (objectTileArtwork(stretched)) return { stretched, false };
	return {};
}

// Lays a resolved glass down. A repeating tile fills the pane by
// repetition; a stretched one is one quad over the pane's whole rect
// sampling the whole tile, which is the picture the pane-sized art drew
// before the repeating tile existed (#1083).
void drawGlassOrLegacy(GlassDraw const& glass, Vec2 paneMin, Vec2 paneMax,
	WorldDrawList* drawList)
{
	if (glass.repeats)
	{
		drawGlassRepeating(glass.tile, paneMin, paneMax, drawList);
		return;
	}
	if (!glass.tile.empty())
		drawObjectSprite(glass.tile.c_str(), drawList, paneMin, paneMax);
}

void renderWindow(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto windowStyle = window->getStyle();

	bool const booth = window->isBoothWindow();
	if (booth) renderBoothWindow(static_pointer_cast<const core::BoothWindow>(window), layer, style, drawList);
	else switch (windowStyle)
	{
	case core::Window::Style::Clear:
		renderWindowClear(window, layer, style, selected, drawList);
		break;

	case core::Window::Style::Frosted:
		renderWindowFrosted(window, layer, style, selected, drawList);
		break;

	case core::Window::Style::Tinted:
		renderWindowTinted(window, layer, style, selected, drawList);
		break;
	}

	if (!booth && style == LayerRenderStyle::Solid && hasObjectTileset())
	{
		core::Vector2 from, to;
		window->getFullShape(from, to);
		transformPosition(from);
		transformPosition(to);
		// The Style picks the tile and nothing else. The pane's geometry, its
		// aperture and its clip are identical across all three, exactly as
		// they were when one pane-sized tile carried the whole opening
		// (#1079). A set that carries no `glass-` tile by that name - a
		// version 1 set, which is what the custom theme atlases are - falls
		// back to the pane-sized `window-` art the same pane drew before the
		// repeating tile existed, so those themes keep drawing glass instead
		// of an empty opening (#1083).
		auto const glass = windowStyle == core::Window::Style::Clear
			? glassTileOrLegacy("glass-clear", "window-clear")
			: windowStyle == core::Window::Style::Frosted
			? glassTileOrLegacy("glass-frosted", "window-frosted")
			: glassTileOrLegacy("glass-tinted", "window-tinted");
		drawGlassOrLegacy(glass, { from.x, from.y }, { to.x, to.y }, drawList);
	}

	// The frame, over the glass and over the wall alike. Last, so its
	// moulding is what reads as the edge of the opening whatever the
	// glass underneath it is doing (#1075). The BoothWindow is refused
	// inside the frame pass rather than here, so that no caller reaching
	// the frame by another route can put a second moulding on a hatch
	// that already owns one.
	if (isDrawnSolid(style))
	{
		renderWindowframe(window, sectorDrawSheet(), drawList);
		drawList->AddDrawCmd();
	}

	if (selected)
	{
		core::Vector2 bounds0, bounds1;
		window->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, packColour(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
}


void renderPhysicalControl(shared_ptr<const core::Button> button, uint32_t /* layer */, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;
	button->getFullShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);

	// A Button whose controlled object is Broken renders red, never the enabled
	// green, so a failed threshold is visible from its control.
	auto const broken = gRenderWorld
		&& gRenderWorld->isControlTargetBroken(button->getInteractionPointId());
	auto colour = broken ? packColour(255, 0, 0)
		: button->isEnabled() ? packColour(0, 255, 128) : packColour(192, 128, 128);
	auto point = gRenderWorld ? gRenderWorld->lookupInteractionPoint(button->getInteractionPointId()).entity : nullptr;
	auto unit = point ? gRenderWorld->lookupDumbwaiter(point->getDumbwaiterOwner()) : nullptr;
	if (unit && !broken)
	{
		auto stop = unit->getLandingButton(0) == button->getInteractionPointId() ? 0u : 1u;
		colour = unit->isBusy() ? packColour(220, 80, 80)
			: unit->getButtonState(stop) == core::DumbwaiterButtonState::Here
				? packColour(80, 200, 120) : packColour(200, 160, 80);
	}

	// A Button seen from the Layer its controlling threshold was authored on is
	// filled solid; from every other Layer it contributes the outline only,
	// exactly like the Door it stands beside.
	if (style == LayerRenderStyle::Solid)
	{
		if (unit || broken || !drawObjectSprite(button->isEnabled() ? "button-enabled" : "button-disabled",
			drawList, {bounds0.x, bounds0.y}, {bounds1.x, bounds1.y}))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	if (selected)
	{
		Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
}


void renderWalkway(shared_ptr<const core::Walkway> walkway, uint32_t /* layer */, LayerRenderStyle style,
	bool selected, WorldDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;
	core::Vector2 bounds0, bounds1;

	walkway->getFullShape(bounds0, bounds1);

	// A Walkway is authored `Object(cellX, cellY, 1.0, 0.0)`: a deliberately
	// zero height so it never obstructs pathing. Its visible slab belongs to
	// the artwork, not to the object - the tile declares an offset that hangs
	// it below the floor line of the cell it was authored in, and the
	// physical shape stays exactly what it is (#1014).
	core::Vector2 const anchor{ bounds0.x, bounds0.y };

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = selected ? SelectedColour : packColour(64, 64, 64);
	if (drawOffsetTile("walkway", anchor, drawList)) return;
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, colour,
		selected ? 3.0f : 2.0f);
}


void renderMarker(shared_ptr<const core::Marker> marker, uint32_t /* layer */, LayerRenderStyle style,
	bool selected, WorldDrawList* drawList)
{
	if (!gUISettings.renderMarkers || style != LayerRenderStyle::Solid) return;
	auto point = marker->getPosition();
	point.x += marker->getOffset();
	point.y += MarkerFloorLift;
	transformPosition(point);

	auto const& metrics = WorldDrawList::fontMetrics();
	// The icon face measured at its own size: the height that comes back is
	// the face's pixel size, which is what the icon is scaled from. The
	// arithmetic is the one the producer used when it held the font itself,
	// so the drawn icon keeps its exact centring and extents.
	auto const natural = metrics.measure({ WorldFont::Icons, 0.0f }, MarkerIconGlyph);
	auto const fontSize = natural.y * MarkerIconSize * gUISettings.worldZoom
		/ max(max(natural.x, natural.y), 1.0f);
	auto const size = metrics.measure({ WorldFont::Icons, fontSize }, MarkerIconGlyph);
	Vec2 topLeft{ point.x - size.x * 0.5f, point.y - size.y };
	if (selected)
		drawList->AddRect({ topLeft.x - 3.0f, topLeft.y - 3.0f },
			{ topLeft.x + size.x + 3.0f, topLeft.y + size.y + 3.0f },
			SelectedColour, 2.0f, 0, 2.0f);
	// The Marker's icon is its artwork. With no tile to draw it, the glyph
	// itself is drawn as text, which is a vector run and may carry a colour.
	if (!drawObjectSprite("marker", drawList, topLeft,
		{topLeft.x + size.x, topLeft.y + size.y}))
		drawList->AddText({ WorldFont::Icons, fontSize }, topLeft,
			packColour(251, 188, 4), MarkerIconGlyph);
}


void renderFurnitureMarkers(shared_ptr<const core::Sector> const& sector, uint32_t layer,
	LayerRenderStyle style, WorldDrawList* drawList)
{
	if (!gRenderWorld || style != LayerRenderStyle::Solid) return;
	for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		if (auto object = std::dynamic_pointer_cast<const core::MarkerSectorObject>(sector->getObject(index));
			object && gRenderWorld->isFurnitureMarker(object->getMarker()->getId()))
			renderMarker(object->getMarker(), layer, style, object == gSelectedSectorObject, drawList);
}

void renderForceBridge(shared_ptr<const core::ForceBridge> forceBridge, uint32_t /* layer */,
	LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	// The Force Bridge stays a primitive line, deliberately. Its span is
	// continuous and arbitrary - any fraction of a cell over any gap - so a
	// fixed tile would be stretched wrong at most lengths it gets used at,
	// and the 2px line is honest about being an energy field. It is given no
	// tile in ObjectAtlasNew, and the loader refuses one (#1019).
	if (style != LayerRenderStyle::Solid)
	{
		renderBrokenWarning(*forceBridge, drawList);
		return;
	}
	core::Vector2 bounds0, bounds1;
	if (selected)
	{
		forceBridge->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y },
			SelectedColour, 5.0f);
	}
	forceBridge->getCurrentShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y },
		packColour(0, 255, 0), selected ? 3.0f : 2.0f);
	renderBrokenWarning(*forceBridge, drawList);
}


void renderLadder(shared_ptr<const core::Ladder> ladder, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	ladder->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LadderColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	renderBrokenWarning(*ladder, drawList);
}


void renderLift(shared_ptr<const core::Lift> lift, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	lift->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LiftColour;
	if (!drawObjectSprite("lift-car", drawList,
		{ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }))
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	renderBrokenWarning(*lift, drawList, true);
}


void renderPlatformLift(shared_ptr<const core::LiftSectorObject> const& platformLift,
	uint32_t layer, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	// The moving platform is only a thin slab, so also show the full authored
	// shaft occupied by its stops. Keep this outline subdued so Walkways and the
	// platform itself remain the dominant geometry.
	core::Vector2 bounds0, bounds1;
	platformLift->getBounds(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);
	Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
	Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
	drawList->AddRect(topLeft, bottomRight, packColour(128, 128, 192, 72));

	core::Vector2 car0, car1;
	platformLift->getLift()->getCurrentShape(car0, car1);
	transformPosition(car0);
	transformPosition(car1);
	if (!drawObjectSprite("platform-lift", drawList, {car0.x, car0.y}, {car1.x, car1.y}))
		renderLift(platformLift->getLift(), layer, style, selected, drawList);
}


// The tile one Shuttle carriage cell wears: the role-named tile when the
// installed atlas carries one, otherwise the left/middle/right tile that
// same cell drew before the naming convention existed. The fallback is the
// same shape as the Sector surface fallback further down - a set with no
// role tiles keeps drawing its own carriage art instead of dropping to the
// flat fill - so adopting the convention is an upgrade an older sheet
// survives rather than a break (#1021).
char const* carriageTileOrLegacy(char const* role, char const* legacy)
{
	return objectSpriteSize(role) ? role : legacy;
}


void renderShuttle(shared_ptr<const core::Shuttle> shuttle, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	shuttle->getCurrentShape(bounds0, bounds1);

	auto const numCars = shuttle->getNumCars();
	auto const carWidth = shuttle->getCarWidth();
	auto const colour = ShuttleColour;

	auto drawPart = [&](char const* sprite, core::Vector2 part0, core::Vector2 part1)
	{
		transformPosition(part0);
		transformPosition(part1);
		if (!drawObjectSprite(sprite, drawList, { part0.x, part0.y }, { part1.x, part1.y }))
			drawList->AddRectFilled({ part0.x, part0.y }, { part1.x, part1.y }, colour);
	};

	for (uint32_t car = 0; car < numCars; ++car)
	{
		// The carriage is composed from its role tiles, not from art cut per
		// width: the first cell is the left end, the last is the right end,
		// and every cell between reuses the middle. A two-cell carriage has
		// no middle to draw, a four-cell carriage draws the same middle
		// twice. Widening a carriage buys cells, never tiles (#1021).
		auto const carX = bounds0.x + car * (carWidth + 1);
		for (uint32_t cell = 0; cell < carWidth; ++cell)
		{
			auto const* sprite = cell == 0
				? carriageTileOrLegacy("shuttle-L0", "shuttle-car-left")
				: cell + 1 == carWidth
				? carriageTileOrLegacy("shuttle-R0", "shuttle-car-right")
				: carriageTileOrLegacy("shuttle-M0", "shuttle-car-middle");
			drawPart(sprite, { carX + cell, bounds0.y },
				{ carX + cell + 1.0f, bounds1.y });
		}

		if (car + 1 < numCars)
		{
			// The coupling sits in the one-cell gap between carriages, which
			// belongs to neither of them. That is why its tile keeps a bare
			// name: it is not a cell position on a carriage, and a role name
			// would invite composing it into a carriage's own cell count.
			auto const connectorX = carX + carWidth;
			drawPart(carriageTileOrLegacy("shuttle-connector", "shuttle-car-connector"),
				{ connectorX, bounds0.y }, { connectorX + 1.0f, bounds1.y });
		}
	}
	renderBrokenWarning(*shuttle, drawList, true);
}


void renderStairwell(shared_ptr<const core::Stairwell> stairwell, uint32_t /* layer */, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 worldMin, worldMax;
	stairwell->getCurrentShape(worldMin, worldMax);

	auto screenMin = worldMin;
	auto screenMax = worldMax;
	transformPosition(screenMin);
	transformPosition(screenMax);
	Vec2 topLeft{ min(screenMin.x, screenMax.x), min(screenMin.y, screenMax.y) };
	Vec2 bottomRight{ max(screenMin.x, screenMax.x), max(screenMin.y, screenMax.y) };
	drawList->AddRectFilled(topLeft, bottomRight, packColour(255, 255, 255));

	auto toScreen = [&](core::Vector2 point)
	{
		point += worldMin;
		transformPosition(point);
		return Vec2{ point.x, point.y };
	};

	// Stairwell::getLevelPath is also used to place the Graph vertices. Build one
	// continuous polyline so adjacent flights share their exact level endpoint.
	vector<Vec2> pathPoints;
	if (stairwell->getLevelsHigh() > 1)
		pathPoints.reserve(1 + (stairwell->getLevelsHigh() - 1) * 3);
	for (uint32_t level = 0; level + 1 < stairwell->getLevelsHigh(); ++level)
	{
		auto path = stairwell->getLevelPath(level);
		if (pathPoints.empty()) pathPoints.push_back(toScreen(path[0]));
		pathPoints.push_back(toScreen(path[1]));
		pathPoints.push_back(toScreen(path[2]));
		pathPoints.push_back(toScreen(path[3]));
	}
	for (uint32_t level = 0; level < stairwell->getLevelsHigh(); ++level)
		for (auto const& platform : stairwell->getLandingPaths(level))
		{
			array<Vec2, 2> points{toScreen(platform[0]), toScreen(platform[1])};
			drawList->AddPolyline(points.data(), 2, packColour(223, 223, 223), DrawFlagsNone, 10.0f);
			drawList->AddPolyline(points.data(), 2, packColour(0, 0, 0), DrawFlagsNone, 4.0f);
		}
	if (pathPoints.size() >= 2)
	{
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			packColour(223, 223, 223), DrawFlagsNone, 10.0f);
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			packColour(0, 0, 0), DrawFlagsNone, 4.0f);
	}
	// Outline each permanently open doorway both from inside the Stairwell
	// and through the landing aperture. The aperture pass already clips each
	// box to its landing; these frames never reveal the fore Layer recursively.
	if (isDrawnSolid(style))
		for (uint32_t level = 0; level < stairwell->getLevelsHigh(); ++level)
		{
			auto low = toScreen({1.f - CORE_STAIRWELL_PASSAGE_WIDTH * .5f, float(level)});
			auto high = toScreen({1.f + CORE_STAIRWELL_PASSAGE_WIDTH * .5f,
				float(level) + CORE_STAIRWELL_DOORWAY_HEIGHT});
			drawList->AddRect({low.x, high.y}, {high.x, low.y}, packColour(0, 0, 0));
		}
}


void renderStaircase(shared_ptr<const core::Staircase> staircase, WorldDrawList* drawList)
{
	auto const path = staircase->getPath();
	core::Vector2 origin, ignored;
	staircase->getCurrentShape(origin, ignored);
	uint32_t const count = staircase->getStepCount();
	auto toScreen = [&](float x, float y)
	{
		core::Vector2 point{ origin.x + x, origin.y + y };
		transformPosition(point);
		return Vec2{ point.x, point.y };
	};
	auto drawSteps = [&](auto const& segments, PackedColourValue colour, float thickness)
	{
		for (auto const& segment : segments)
			drawList->AddPolyline(segment.data(), (int)segment.size(), colour,
				DrawFlagsNone, thickness);
	};

	if (staircase->isEscalator())
	{
		// Each L-shaped tread/riser advances along the incline at the Escalator's
		// world speed, then wraps to the opposite endpoint.
		vector<array<Vec2, 3>> segments;
		segments.reserve(count);
		float const step = 1.0f / (float)count;
		float const phase = staircase->getAnimationPhase();
		for (uint32_t i = 0; i < count; ++i)
		{
			float t = fmod((float)i * step + phase, 1.0f);
			if (t < 0.0f) t += 1.0f;
			float const previous = max(0.0f, t - step);
			float const x0 = path[0].x + (path[1].x - path[0].x) * previous;
			float const x1 = path[0].x + (path[1].x - path[0].x) * t;
			float const y0 = path[0].y + (path[1].y - path[0].y) * previous;
			float const y1 = path[0].y + (path[1].y - path[0].y) * t;
			segments.push_back({ toScreen(x0, y0), toScreen(x1, y0), toScreen(x1, y1) });
		}
		drawSteps(segments, packColour(32, 32, 32, 255), 10.0f);
		drawSteps(segments, packColour(220, 220, 220, 255), 6.0f);
		renderBrokenWarning(*staircase, drawList);
		return;
	}

	vector<Vec2> points;
	points.reserve(count * 2 + 1);
	points.push_back(toScreen(path[0].x, path[0].y));
	for (uint32_t i = 0; i < count; ++i)
	{
		float const t0 = (float)i / (float)count;
		float const t1 = (float)(i + 1) / (float)count;
		float const x1 = path[0].x + (path[1].x - path[0].x) * t1;
		float const y0 = path[0].y + (path[1].y - path[0].y) * t0;
		float const y1 = path[0].y + (path[1].y - path[0].y) * t1;
		points.push_back(toScreen(x1, y0));
		points.push_back(toScreen(x1, y1));
	}
	drawList->AddPolyline(points.data(), (int)points.size(), packColour(32, 32, 32, 255),
		DrawFlagsNone, 10.0f);
	drawList->AddPolyline(points.data(), (int)points.size(), packColour(220, 220, 220, 255),
		DrawFlagsNone, 6.0f);
}

void renderSelected(shared_ptr<const core::Object> object, int /* layer */, bool /* visibleLayer */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	object->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);
	
	bounds0.x -= 2;
	bounds0.y -= 2;
	bounds1.x += 2;
	bounds1.y += 2;

	auto colour = SelectedColour;
	drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderSectorObjects(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, int flags, WorldDrawList* drawList)
{
	// Sort so that Ladders and Lifts are rendered first, as these need to be behind everything else.
	auto sortedObjects = sector->getSortedObjects([](auto obj1, auto obj2)
	{
		if (obj1->getObjectType() == core::SectorObjectType::Ladder || obj1->getObjectType() == core::SectorObjectType::Lift)
		{
			if (obj2->getObjectType() == core::SectorObjectType::Ladder || obj2->getObjectType() == core::SectorObjectType::Lift)
			{
				if (obj1->getCellX() == obj1->getCellY())
				{
					return obj1->getCellY() < obj2->getCellY();
				}
				else
				{
					return obj1->getCellX() < obj2->getCellX();
				}
			}
			else
			{
				return true;
			}
		}
		else
		{
			return false;
		}
	});

	// A Door or Window is authored on the front Layer of its pair, but the same
	// object is registered on the Sector on the Layer behind as well. Its aperture
	// is only drawn while rendering from the Layer it is authored on; from the
	// Layer behind it is drawn as an outline.
	auto thresholdStyle = [&](shared_ptr<const core::Sector> authoredSector)
	{
		return isDrawnSolid(style) && sector == authoredSector
			? LayerRenderStyle::Solid
			: LayerRenderStyle::Wireframe;
	};

	for (auto object : sortedObjects)
	{
		auto selected = object == gSelectedSectorObject;
		switch (object->getObjectType())
		{
		case core::SectorObjectType::BulkheadDoor:
			// Like ordinary Door outlines, same-Layer thresholds and their
			// condition warning remain visible in the wireframe pass.
			if (flags & (style == LayerRenderStyle::Wireframe
				? RENDER_SECTOR_OBJECTS_BEHIND : RENDER_SECTOR_OBJECTS_INFRONT))
			{
				renderBulkheadDoor(static_pointer_cast<const core::BulkheadDoorSectorObject>(object)->getDoor(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Door:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				if (style == LayerRenderStyle::Aperture && door->getFrontSector() != sector) break;
				renderDoor(door, layer, thresholdStyle(door->getFrontSector()), selected, drawList);
			}
			break;


		case core::SectorObjectType::ForceBridge:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderForceBridge(static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Ladder:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLadder(static_pointer_cast<const core::LadderSectorObject>(object)->getLadder(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Lift:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderPlatformLift(static_pointer_cast<const core::LiftSectorObject>(object),
					layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::InteractionPoint:
			// Physical controls draw with the thresholds, not the in-front objects:
			// a Door Button renders exactly like its Door - solid on the Layer the
			// Door was authored on, an outline from every other Layer - so the
			// Button on the far side of a Door is never solid and shows through in
			// the wireframe overlay pass. Ordinary controls carry no threshold
			// Layer and stay solid wherever their own Layer is drawn, but a
			// non-solid pass contributes outlines only, never a fill.
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto button = static_pointer_cast<const core::Button>(object->_getObject());
				auto const thresholdLayer = button->getThresholdLayer();
				auto controlStyle = thresholdLayer == ~0u || thresholdLayer == layer
					? LayerRenderStyle::Solid : LayerRenderStyle::Wireframe;
				if (style != LayerRenderStyle::Solid) controlStyle = LayerRenderStyle::Wireframe;
				renderPhysicalControl(button, layer, controlStyle, selected, drawList);
			}
			break;

		case core::SectorObjectType::AccessPanel:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto panel = static_pointer_cast<const core::AccessPanelSectorObject>(object)->getPanel();
				core::Vector2 min, max; panel->getFullShape(min, max);
				bool degenerate = panel->getSize().x == 0 || panel->getSize().y == 0;
				if (degenerate) panel->getSelectionShape(min, max);
				// The panel's authored rect in World units before the screen
				// transform: its authored width, its authored height, sitting its
				// authored bottom offset above the cell floor. The artwork is drawn
				// into that and not into a cell, because an Access panel's geometry
				// is authored per panel and the tile cannot second-guess it (#1019).
				core::Vector2 const authoredMin = min, authoredMax = max;
				transformPosition(min); transformPosition(max);
				if (!degenerate && isDrawnSolid(style))
					drawList->AddRectFilled({min.x, max.y}, {max.x, min.y}, packColour(22, 30, 40, 255));
				if (panel->getProgress() > 0)
				{
					// An inset cross distinguishes Open even in wireframe and zero-area indicators.
					drawList->AddLine({min.x, max.y}, {max.x, min.y}, packColour(100, 210, 180, 255), 2);
					drawList->AddLine({min.x, min.y}, {max.x, max.y}, packColour(100, 210, 180, 255), 2);
				}
				// OpenUp: the leaf retracts into the top edge, revealing the bottom first.
				if (!degenerate && panel->getProgress() < 1)
				{
					auto bottom = min.y + (max.y - min.y) * panel->getProgress();
					if (isDrawnSolid(style))
					{
						// The leaf draws its artwork over the part of the authored
						// rect it still covers, cropped to match, so the art slides
						// into the top edge with the live progress instead of being
						// squashed into the strip left below it (#1019).
						if (!drawRetractedTile("access-panel", authoredMin, authoredMax,
							panel->getProgress(), drawList))
							drawList->AddRectFilled({min.x, max.y}, {max.x, bottom}, packColour(78, 92, 110, 255));
					}
					if (panel->getProgress() > 0)
						drawList->AddRect({min.x, max.y}, {max.x, bottom}, packColour(165, 180, 198, 255), 0, 0, 2);
				}
				drawList->AddRect({min.x, max.y}, {max.x, min.y},
					selected ? packColour(251, 188, 4, 255) : packColour(165, 180, 198, 255), 0, 0, 2);
			}
			break;

		case core::SectorObjectType::Marker:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
				// Owned Markers are editor overlays, drawn after Furniture artwork.
				if (!gRenderWorld || !gRenderWorld->isFurnitureMarker(marker->getId()))
					renderMarker(marker, layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Walkway:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderWalkway(static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::BoothWindow:
		case core::SectorObjectType::Window:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto window = static_pointer_cast<const core::WindowSectorObject>(object)->getWindow();
				if (style == LayerRenderStyle::Aperture && window->getFrontSector() != sector) break;
				renderWindow(window, layer, thresholdStyle(window->getFrontSector()), selected, drawList);
			}
			break;

		default:
			break;
		}
	}
}


//
// Clipped transits from the Layer behind draw over the selected Layer's Locations.
// Redraw what the selected Layer owns and must keep in front of them: its own
// thresholds first, so a closed Door still occludes the Transit standing behind
// it; then its physical controls; then its Agents, so no control can be painted
// in front of an Agent.
//
void renderThresholdsControlsAndAgentsAboveTransit(vector<shared_ptr<const core::Sector>> const& sectors,
	uint32_t layer, WorldDrawList* drawList)
{
	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::Door)
				continue;

			// Only a Door authored on this Layer opens into the Layer behind, and so
			// covers something that Layer draws. The same Door object registered on the
			// Sector behind is seen from the other side and is not occluder here.
			auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			if (door->getFrontSector() != sector)
				continue;

			renderDoor(door, layer, LayerRenderStyle::Solid,
				object == gSelectedSectorObject, drawList);
		}
	}

	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			auto button = static_pointer_cast<const core::Button>(object->_getObject());
			auto const thresholdLayer = button->getThresholdLayer();
			auto const controlStyle = thresholdLayer == ~0u || thresholdLayer == layer
				? LayerRenderStyle::Solid : LayerRenderStyle::Wireframe;
			renderPhysicalControl(button, layer, controlStyle,
				object == gSelectedSectorObject, drawList);
		}
	}

	// Controls are redrawn above clipped transits. Restore the sector's Agents
	// afterwards so no physical control can be painted in front of them.
	for (auto const& sector : sectors)
	{
		// Chamber occupants were already drawn below their processing overlay.
		// This foreground restoration must not repaint them above the white quad.
		if (auto chamber = dynamic_pointer_cast<const core::ChamberTransit>(sector);
			chamber && chamber->getDecontaminationOpacity() > 0) continue;
		renderSectorDepthContent(sector, drawList);
		renderFurnitureMarkers(sector, layer, LayerRenderStyle::Solid, drawList);
	}
}


namespace
{
	void drawAgentDebugBadge(WorldDrawList* drawList, char const* symbol,
		PackedColourValue boxColour, float centreX, float side, float gap, float& bottom)
	{
		Vec2 const minimum{ centreX - side * 0.5f, bottom - side };
		Vec2 const maximum{ centreX + side * 0.5f, bottom };
		drawList->AddRectFilled(minimum, maximum, boxColour);
		auto const textSize = WorldDrawList::fontMetrics()
			.measure({ WorldFont::Default, 0.0f }, symbol);
		drawList->AddText({ centreX - textSize.x * 0.5f,
			minimum.y + (side - textSize.y) * 0.5f }, White, symbol);
		bottom = minimum.y - gap;
	}
}

void renderAgent(core::Agent const* agent, WorldDrawList* drawList)
{
	auto bounds = agent->getBounds();

	core::Vector2 pos0, pos1;

	bounds.getCurrentShape(pos0, pos1);
	pos0.x += agent->getPoseRenderXOffset();
	pos1.x += agent->getPoseRenderXOffset();
	pos0.y += agent->getPoseRenderYOffset();
	pos1.y += agent->getPoseRenderYOffset();

	transformPosition(pos0);
	transformPosition(pos1);

	float availableWidth = pos1.x - pos0.x;
	auto const tileSize = agentSpriteSize(agent->getPoseImageTile().c_str());
	auto const standingTileSize = agentSpriteSize(
		agent->getPhysicalBaseline().poses.at(core::Pose::Standing).imageTile.c_str());
	if (tileSize && standingTileSize)
	{
		float const standardHeight = agent->getPhysicalBaseline().standingHeight
			* CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
		float const standardWidth = agent->getPhysicalBaseline().width
			* CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		auto const scale = min(standardWidth / standingTileSize->x, standardHeight / standingTileSize->y)
			* agent->getEffectiveHeightModifier().value;
		Vec2 size{tileSize->x * scale, tileSize->y * scale};
		Vec2 minimum{(pos0.x + pos1.x - size.x) * .5f, pos0.y - size.y};
		drawAgentSprite(agent->getPoseImageTile().c_str(), drawList, minimum,
			{minimum.x + size.x, minimum.y + size.y});
		// Selection is an outline drawn over the sprite rather than a colour
		// multiplied into it: the Agent's artwork is the same picture whether
		// or not it is selected, and the outline is what says it is.
		if (gSelectedAgent == agent)
			drawList->AddRect(minimum, {minimum.x + size.x, minimum.y + size.y},
				SelectedColour, 0.0f, 0, 2.0f);
	}
	else drawList->AddRectFilled({pos0.x, pos1.y}, {pos1.x, pos0.y}, packColour(255, 0, 255, 255));

	auto const planning = agent->getState() == core::Agent::State::RoutePlanning;
	if (!gUISettings.renderAgentDebug) return;

	auto const inQueue = agent->isInQueue();
	if (!inQueue && !planning) return;

	// Badge width follows the rendered Agent body, with screen-space limits that
	// keep punctuation legible at low zoom and unobtrusive at high zoom.
	auto const side = clamp(availableWidth, 14.0f, 24.0f);
	auto const gap = clamp(side * 0.12f, 1.0f, 3.0f);
	auto const centreX = (pos0.x + pos1.x) * 0.5f;
	float badgeBottom = min(pos0.y, pos1.y) - gap;
	if (inQueue)
		drawAgentDebugBadge(drawList, "!",
			packColour(230, 126, 34, 255), centreX, side, gap, badgeBottom);
	if (planning)
		drawAgentDebugBadge(drawList, "?",
			packColour(96, 96, 96, 255), centreX, side, gap, badgeBottom);
}


std::shared_ptr<core::Path> agentPathForRendering(core::World const* world, core::Agent const* agent)
{
	if (!agent) return nullptr;
	auto path = agent->getPath();
	if ((!path || path->nodes.empty()) && world && world->isSimulationPaused()
		&& world->isTraversalTopologyValid())
	{
		core::World::TopologyPathIntent intent;
		if (world->getPausedPathIntent(*agent, intent)
			&& intent.destinationSector
			&& intent.destinationSector.value <= world->getNumSectors())
		{
			auto const destinationSector = world->getSector(
				static_cast<uint32_t>(intent.destinationSector.value - 1));
			auto const graph = world->getGraph();
			auto const destination = graph && destinationSector
				? graph->getClosestVertexInSector(
					destinationSector.get(), intent.destinationPosition)
				: nullptr;
			if (destination) path = graph->calculatePath(agent, destination);
		}
	}
	return path;
}

void renderSelectedAgentPath(core::World const* world, WorldDrawList* drawList)
{
	if (!drawList || !gUISettings.renderAgentDebug || !gSelectedAgent) return;
	auto path = agentPathForRendering(world, gSelectedAgent);
	if (!path || path->nodes.empty()) return;

	auto const visibleLayer = static_cast<uint32_t>(max(gUISettings.visibleLayer, 0));
	auto const outlineColour = packColour(32, 32, 32, 220);
	auto const pathColour = packColour(255, 196, 0, 240);

	auto positionOnVisibleLayer = [visibleLayer](
		shared_ptr<const core::Vertex> const& vertex, core::Vector2& position)
	{
		if (!vertex || !vertex->getSector()
			|| vertex->getSector()->getLayerIndex() != visibleLayer) return false;
		position = vertex->getPosition();
		transformPosition(position);
		return true;
	};
	auto drawSegment = [drawList, outlineColour, pathColour](
		core::Vector2 const& from, core::Vector2 const& to)
	{
		drawList->AddLine({ from.x, from.y }, { to.x, to.y }, outlineColour, 5.0f);
		drawList->AddLine({ from.x, from.y }, { to.x, to.y }, pathColour, 2.5f);
	};

	core::Vector2 firstPosition;
	if (gSelectedAgent->getSector()
		&& gSelectedAgent->getSector()->getLayerIndex() == visibleLayer
		&& positionOnVisibleLayer(path->nodes.front().targetVertex, firstPosition))
	{
		auto agentPosition = gSelectedAgent->getGlobalPosition();
		transformPosition(agentPosition);
		drawSegment(agentPosition, firstPosition);
	}

	for (size_t index = 0; index < path->nodes.size(); ++index)
	{
		core::Vector2 position;
		if (!positionOnVisibleLayer(path->nodes[index].targetVertex, position)) continue;
		drawList->AddCircleFilled({ position.x, position.y }, 5.0f, outlineColour);
		drawList->AddCircleFilled({ position.x, position.y }, 3.0f, pathColour);

		if (index + 1 >= path->nodes.size()) continue;
		core::Vector2 nextPosition;
		if (positionOnVisibleLayer(path->nodes[index + 1].targetVertex, nextPosition))
			drawSegment(position, nextPosition);
	}
}

void renderSectorDepthContent(shared_ptr<const core::Sector> sector, WorldDrawList* drawList)
{
	// One ordered content pass, under the caller's existing Layer/aperture clip.
	struct Content { int depth; core::FurnitureInstance const* furniture; core::Agent const* agent; };
	vector<Content> content;
	if (gRenderWorld && gRenderWorld->furnitureCatalogue())
		for (auto const& instance : gRenderWorld->furniture())
			if (instance.sector == sector->getIndex()) content.push_back({ instance.localDepth, &instance, nullptr });
	for (auto agent : sector->getAgents()) content.push_back({ agent->getLocalDepth(), nullptr, agent });
	stable_sort(content.begin(), content.end(), [](auto const& a, auto const& b) {
		if (a.depth != b.depth) return a.depth > b.depth;
		return a.furniture && !b.furniture;
	});
	for (auto const& entry : content)
	{
		if (entry.agent) { renderAgent(entry.agent, drawList); continue; }
		auto const& instance = *entry.furniture;
		auto const& definition = *gRenderWorld->furnitureCatalogue()->definition(instance.definitionKey);
		for (auto const& tile : definition.tiles)
		{
			core::Vector2 top{ sector->getCellX() + instance.x + tile.x, sector->getCellY() + instance.y + tile.y + 1 };
			core::Vector2 bottom{ top.x + 1, top.y - 1 };
			transformPosition(top); transformPosition(bottom);
			if (!drawFurnitureSprite(tile.image.c_str(), drawList, { top.x, top.y }, { bottom.x, bottom.y }))
				drawList->AddRectFilled({top.x, top.y}, {bottom.x, bottom.y}, packColour(255, 0, 255, 255));
		}
	}
}

// The surface a Sector type fills with: its own full-cell tile when the
// installed atlas carries one, otherwise the shared surface the type
// borrowed before its own artwork existed. The fallback is what keeps an
// older atlas - one with no Airlock tile of its own - drawing Corridor art
// rather than the missing-tile magenta (#1017).
//
// Presence is a trustworthy test only for a surface the old family never
// declared. The old sheet declares `shuttle` and `staircase` as dormant
// regions - cut out of the draft, never drawn - so those two ask the
// family instead, below (#1027).
std::string sectorSurfaceOrBorrowed(pf::render::InstalledSectorSheet const* sheet,
	char const* own, char const* borrowed)
{
	return hasSectorTileSurface(sheet, own) ? std::string{ own }
		: std::string{ borrowed };
}

// The surface of a Sector type whose own tile is a New-family thing: the
// Shuttle's and the Staircase's. In the New family both wear it. In the
// Old family both borrow the Lift's and the Stairwell's shafts, which is
// what every existing World has always drawn - the old set's `shuttle`
// and `staircase` regions were declared but never drawn, because the
// Shuttle's rail corridor and the Staircase's steps are procedural and a
// second fixed image underneath them fights the real one.
//
// The gate is the family the tile was authored to be drawn in, not the
// presence of the name. The name has been in the old sheet since the day
// it was written, so a repoint keyed on presence alone reads those dormant
// regions as own art and repaints every World on `SectorAtlas` (#1027).
std::string newFamilySurfaceOrBorrowed(pf::render::InstalledSectorSheet const* sheet,
	char const* own, char const* borrowed)
{
	return sectorTilesetIsNewFamily(sheet) && hasSectorTileSurface(sheet, own)
		? std::string{ own }
		: std::string{ borrowed };
}

// The widest opening a doorframe is authored for. Two cells is the whole
// of the vocabulary - `L0` and `R0` - and a wider opening has no frame to
// wear, so it draws its wall's plain tile rather than a frame that stops
// short of the gap.
constexpr std::uint32_t kMaxFramedDoorCellsWide = 2;

// The doorframe tile one cell of one opening wears (#1054, #1073).
//
// `doorframe-<width><height>-<role>`, where the width is how many cells
// the opening spans, the height is `r`egular or `t`all, and the role is
// `L0` for the opening's left cell and `R0` for its right one. A 1-wide
// opening has only an `L0`.
//
// The wall is not in the name because the wall is not in the artwork. A
// frame is laid over the wall's own tile rather than traded for it, so
// what the frame has to say is settled by the opening it frames and by
// nothing else: the same six tiles frame a Room, a Corridor and a Facade
// alike. Keying a frame by its wall was what the frame *replacing* the
// wall's tile needed - the frame had to carry the wall's look - and it
// bought three copies of artwork that could not tell the walls apart.
std::string doorframeTileName(std::uint32_t cellsWide,
	core::Door::Height height, bool rightCell)
{
	return "doorframe-" + std::to_string(cellsWide)
		+ (height == core::Door::Height::Tall ? "t" : "r") + "-"
		+ (rightCell ? "R0" : "L0");
}

// The doorframe over the cells one Door stands in (#1073).
//
// The frame is the last thing a Door draws: over the wall's own tile,
// which the Sector surface pass laid across these cells as it did across
// every other cell of that wall, and over the leaves. Its hole is
// transparent, so what shows through it is the leaf where the leaf still
// is and the space behind where the leaf has gone; its moulding is
// opaque, so it covers the wall around the opening and the leaf's edge
// wherever the two overlap.
//
// Drawn through the Sector surface's own per-cell loop over the Door's
// cell span, so the frame takes the same floor-anchored grid, the same
// crop and the same culling as the tile beneath it. Only the picture a
// cell wears changes, never the geometry - the rule #1037 pinned for a
// Sector surface, arriving at a frame through the same door #1059 opened
// for the Facade's.
//
// A frame the installed atlas does not carry is no frame at all: the cell
// resolves to no tile, draws nothing, and the wall's own tile - already
// down - is all there is to see. That is what lets a World drawn on an
// atlas written before frames existed carry on drawing exactly what it
// drew before.
void renderDoorframe(std::shared_ptr<const core::Door> const& door,
	pf::render::InstalledSectorSheet const* sheet, WorldDrawList* drawList)
{
	auto const cellsWide = door->getCellsWide();
	if (cellsWide == 0 || cellsWide > kMaxFramedDoorCellsWide) return;

	// The cell span the Door stands in: its aperture widened back out by
	// the inset it stands in from the cell's x edges, and carried up to a
	// whole cell. The same span the leaves are drawn at, because the same
	// Door owns both.
	core::Vector2 ap0, ap1;
	door->getFullShape(ap0, ap1);
	WorldRect const cells{
		min(ap0.x, ap1.x) - CORE_DOOR_X_INSET,
		min(ap0.y, ap1.y),
		min(ap0.x, ap1.x) - CORE_DOOR_X_INSET + static_cast<float>(cellsWide),
		min(ap0.y, ap1.y) + 1.0f};
	// Trimmed to the wall it frames. A Door's cell is a whole cell and a
	// wall is not: a Room is authored 0.9 of a cell tall, so an untrimmed
	// frame would stand a tenth of a cell above the ceiling it is moulding
	// and draw moulding over the viewport. The wall's own tile is cropped
	// to its height by the same loop, so trimming here lands the frame on
	// that tile at the same crop rather than above it.
	auto const wall = door->getFrontSector();
	if (!wall) return;
	core::Vector2 wallLo{}, wallHi{};
	wall->getBounds(wallLo, wallHi);
	auto const area = cells.clippedTo(WorldRect{
		min(wallLo.x, wallHi.x), min(wallLo.y, wallHi.y),
		max(wallLo.x, wallHi.x), max(wallLo.y, wallHi.y) });
	if (area.empty()) return;
	auto const span = area.projected();

	drawSectorTileSurfacePerCell(sheet, drawList, { span.x0, span.y0 },
		{ span.x1, span.y1 }, gUISettings.worldZoom,
		[sheet, door, cellsWide](std::uint32_t column, std::uint32_t) -> std::string
		{
			auto const name = doorframeTileName(cellsWide, door->getHeight(),
				column != 0);
			// Asked of the sheet being drawn rather than trusted. A name with
			// no tile behind it answers the empty string, which the loop reads
			// as "no tile asked for" and draws nothing - deliberately not the
			// missing-art case, which is a name asked and not carried and
			// answers in magenta over a wall that needed no art.
			return hasSectorTileOverlay(sheet, name) ? name : std::string{};
		});
}

// The windowframe tile one cell of one opening wears (#1075).
//
// `windowframe-<vertical role><horizontal role>`, vertical first so that
// `BL` reads as "bottom-left" the way it is said. The two roles say
// where the cell sits in the opening's span and say nothing else: not the
// glass style, not the Window's State, not the wall it is set into. Each
// of those would be a new axis multiplying fifteen tiles by something,
// and none of them is in the artwork.
//
// The Furniture role vocabulary - `<piece>-<role><level>` - cannot carry
// this, and that is deliberate rather than an oversight. Its trailing
// digit is the height level *within the footprint*, and a windowframe's
// vertical role is end-versus-middle, not a level index: the bottom cell
// of a one-level window needs a hole band and the bottom cell of a
// three-level window needs a hole running to the cell top, and both are
// level 0. And `L`/`M`/`R` has no "inset on both sides" case, which is
// exactly what a one-column window's cell is - different art from a
// two-column window's left cell, and not a thing the piece-role shape can
// name. Forcing the shape would make one convention mean two different
// things in two different files.
//
// The all-middle cell - middle in both directions - is all glass with no
// moulding at all, so it asks for no tile rather than wearing a
// transparent one. The empty string is the same answer a missing tile
// gives, and for the same reason: there is nothing there to draw.
std::string windowframeTileName(std::uint32_t cellsWide,
	std::uint32_t levelsHigh, std::uint32_t column, std::uint32_t level)
{
	auto const horizontal = cellsWide == 1 ? 'S'
		: column == 0 ? 'L'
		: column + 1 == cellsWide ? 'R' : 'M';
	auto const vertical = levelsHigh == 1 ? 'S'
		: level + 1 == levelsHigh ? 'T'
		: level == 0 ? 'B' : 'M';
	if (vertical == 'M' && horizontal == 'M') return {};
	return std::string{ "windowframe-" } + vertical + horizontal;
}

// The windowframe over the cells one Window stands in (#1075).
//
// The frame is the last thing a Window draws: over the wall's own tile,
// which the Sector surface pass laid across these cells as it did across
// every other cell of that wall, and over the glass. Its hole is
// transparent, so what shows through it is the glass and, past the glass,
// the space behind; its moulding is opaque, so it covers the seam where
// the two meet and the edge of the pane wherever the two overlap.
//
// Drawn through the Sector surface's own per-cell loop over the Window's
// cell span, so the frame takes the same floor-anchored grid, the same
// crop and the same culling as the tile beneath it. Only the picture a
// cell wears changes, never the geometry - the rule #1037 pinned for a
// Sector surface, arriving at a frame through the same door #1059 opened
// for the Facade's and #1073 opened for the doorframe.
//
// A frame the installed atlas does not carry is no frame at all: the cell
// resolves to no tile, draws nothing, and the wall's own tile - already
// down - is all there is to see. That is what lets the role grid be
// filled one tile at a time: a World asking for a `windowframe-BL` that
// has not been drawn yet keeps drawing the wall it drew before rather
// than punching a hole in it.
void renderWindowframe(std::shared_ptr<const core::Window> const& window,
	pf::render::InstalledSectorSheet const* sheet, WorldDrawList* drawList)
{
	// A BoothWindow wears no windowframe. It *is* a 1x1 Window - a
	// subclass, constructed as one cell by one level - so a derivation
	// from the opening that did not gate on it would land a second
	// moulding over every service hatch. The frame it already owns is not
	// interchangeable with this one either: the shutter's clip is derived
	// from that frame's inner rect, and the whole closed-versus-open read
	// depends on the shutter and the frame staying in lockstep. Migrating
	// the hatch into the windowframe system is its own decision with the
	// shutter's clip inside it; until that decision is made this condition
	// is what keeps the two frames from meeting, and it is named so that
	// whoever is tempted to unify them later finds the reason here rather
	// than having to rediscover it from a doubled moulding.
	if (window->isBoothWindow()) return;

	auto const cellsWide = window->getCellsWide();
	auto const levelsHigh = window->getLevelsHigh();
	if (cellsWide == 0 || levelsHigh == 0) return;

	// The cell span the Window stands in: its glass rect widened back out
	// by the inset it stands in from the cell's x edges and stood down
	// from the glass's own offset onto the span's floor line. The same
	// span the glass is drawn across, with the inset taken back and the
	// offset given back, because the frame moulds the whole cell rather
	// than the pane.
	core::Vector2 glass0, glass1;
	window->getFullShape(glass0, glass1);
	auto const glassX = min(glass0.x, glass1.x);
	auto const glassY = min(glass0.y, glass1.y);
	WorldRect const cells{
		glassX - CORE_WINDOW_X_INSET,
		glassY - CORE_WINDOW_Y_OFFSET,
		glassX - CORE_WINDOW_X_INSET + static_cast<float>(cellsWide),
		glassY - CORE_WINDOW_Y_OFFSET + static_cast<float>(levelsHigh) };
	// Trimmed to the wall it frames. A Window's cell is a whole cell and a
	// wall is not: a Room is authored 0.9 of a cell tall, so an untrimmed
	// frame would stand a tenth of a cell of moulding above the ceiling it
	// is moulding. The wall's own tile is cropped to its height by the
	// same loop, so trimming here lands the frame on that tile at the same
	// crop rather than above it.
	auto const wall = window->getFrontSector();
	if (!wall) return;
	core::Vector2 wallLo{}, wallHi{};
	wall->getBounds(wallLo, wallHi);
	auto const area = cells.clippedTo(WorldRect{
		min(wallLo.x, wallHi.x), min(wallLo.y, wallHi.y),
		max(wallLo.x, wallHi.x), max(wallLo.y, wallHi.y) });
	if (area.empty()) return;
	auto const span = area.projected();

	drawSectorTileSurfacePerCell(sheet, drawList, { span.x0, span.y0 },
		{ span.x1, span.y1 }, gUISettings.worldZoom,
		[sheet, cellsWide, levelsHigh](std::uint32_t column, std::uint32_t level) -> std::string
		{
			auto const name = windowframeTileName(cellsWide, levelsHigh, column,
				level);
			// Asked of the sheet being drawn rather than trusted, exactly as
			// the doorframe asks. A name with no tile behind it - and the
			// all-middle cell's empty name - answers the empty string, which
			// the loop reads as "no tile asked for" and draws nothing.
			return hasSectorTileOverlay(sheet, name) ? name : std::string{};
		});
}

// The tile one cell wears when nothing outranks it - the variant it was
// recorded with, or the base (#1039, #1040, #1050).
//
// Stated as a function of the two names and the Sector rather than left
// inline in the resolver, so the rule is one function with one answer
// rather than one per caller.
std::string sectorSurfaceVariantTile(pf::render::InstalledSectorSheet const* sheet,
	std::string const& own,
	std::string const& base, core::Sector const& sector,
	std::uint32_t column, std::uint32_t level)
{
	auto const variant = sector.surfaceVariantAt(column, level);
	if (!variant) return base;
	auto const tile = own + "-V" + std::to_string(*variant);
	return hasSectorTileSurface(sheet, tile) ? tile : base;
}

// The resolver the Sector surface loop asks for one cell's tile (#1039).
//
// A cell answers the variant it was recorded as wearing, and the base tile
// everywhere else. That is the whole of what #1040 adds to the seam #1039
// left: the loop is untouched, because the loop was written so that this
// question could grow an answer without it.
//
// The variant is asked of the Sector's own family, never of the name the
// draw ended up with (#1050). Two questions meet at this seam and only one
// of them owns the variant. Which family a Sector belongs to is what
// `sectorSurfaceKind` answers, and no artwork installed changes it: an
// Airlock next door to a Corridor is still an Airlock. Which tile it draws
// is the borrow rule's answer, and is entirely a fact about the sheet.
// Deriving the variant from the drawn name agrees with both for every type
// that owns its tile - which is every type the shipped atlas declares - and
// goes wrong the moment a Sector borrows: an Airlock with no `airlock` tile
// on a sheet carrying `corridor-V2` asked for that, found it, and wore a
// Corridor's variant. That is neither the tile the document recorded nor
// the base the fallback rule answers with; it is a third thing, and the
// kind of wrong an author reads as unfinished art rather than a bug.
// Asking the own family makes the leak unrepresentable: a borrowing Sector
// has no own-family variant tile to find, so its variant cells fall back to
// the borrowed base their plain neighbours are already drawing. The borrow
// rule keeps deciding the base and nothing else.
//
// The variant's tile name follows the `-Vn` derivation the Sector tileset's
// family rule established (#1038), and it is asked of the installed atlas
// before it is worn. A recorded variant the installed atlas does not carry
// draws the base rather than the missing-art magenta: the author picked a
// number from a family the artwork they are looking at has no row for, and
// the honest picture of that is the Sector's own base tile, not a hole.
// The same rule is what makes a World repointed at the version 1 family -
// which declares no families at all, and so carries no `-Vn` tile under any
// name - draw base on every variant cell while keeping its entries, so
// repointing it back at a family that has them restores the picture with no
// data touched (#1040).
//
// A base tile that is itself missing still draws magenta: falling back from
// a variant to a base that is not there is not falling back to something,
// and the missing-art case belongs to the draw, not to the resolution.
//
// No doorframe arrives through here. A framed cell used to be answered the
// frame instead of the wall's tile, which is what made the frame have to
// carry the wall's look and made a frame whose hole did not match its
// opening a hole in the wall (#1062). The frame is drawn over the wall's
// tile by the Door that owns it instead, so the resolver's answer is the
// wall's tile on a door cell exactly as it is on every other cell, and a
// Door is still a reversible placement for nothing was ever written over
// the variant its author painted (#1073).
SectorSurfaceTileResolver sectorSurfaceTileResolver(
	pf::render::InstalledSectorSheet const* sheet,
	std::string ownTile,
	std::string baseTile, std::shared_ptr<const core::Sector> sector)
{
	return [sheet, own = std::move(ownTile), base = std::move(baseTile), sector](
		std::uint32_t column, std::uint32_t level) -> std::string
	{
		return sectorSurfaceVariantTile(sheet, own, base, *sector, column, level);
	};
}

void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, PackedColourValue colour, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	// The sheet every Sector tile in this pass is drawn from, resolved once
	// for the pass and handed to every draw helper below - the surface loop,
	// the boundary strips - so the whole Sector draws off one sheet and no
	// helper goes looking for it (#1088).
	auto const sheet = sectorDrawSheet();

	// A Sector's surface is artwork the installed atlas carries. Every type
	// asks for a tile of its own family and draws it exactly as the sheet
	// paints it - Background and Facade included, which used to fill with a
	// colour the renderer packed from the Sector's own authored triple. No
	// colour is multiplied into a tile on the way to the screen, and a
	// Sector's lights-off state changes no picture: a dark room is a dark
	// tile, authored dark, if it is drawn dark at all.
	auto const sectorType = sector->getType();

	// Render sector area
	core::Vector2 bounds0, bounds1;

	sector->getBounds(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	// The wireframe overlay contributes the outline only, never a fill
	// (ADR 0002). `colour` reaches that outline, and the plain fill drawn
	// when no atlas is installed at all - nothing else in this pass takes
	// a colour.
	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		std::string kind;
		// The family name comes from the one place that says which family a
		// Sector belongs to (#1042), so the editor's swatch strip and the
		// surface actually being drawn cannot disagree about what a Sector is.
		// What each type does when its own tile is missing stays here with the
		// draw, because that is a question about the installed artwork and not
		// about the Sector.
		auto const* own = sectorSurfaceKind(*sector);
		switch (sectorType)
		{
		case core::SectorType::Location:
		case core::SectorType::Ladder:
		case core::SectorType::Lift:
		case core::SectorType::Stairwell:
		// A Background and a Facade own their tile outright. Neither
		// borrows: there is no wall they could be mistaken for, and the
		// tile they ask for is the picture their surface is.
		case core::SectorType::Background:
		case core::SectorType::Facade: kind = own; break;
		case core::SectorType::Airlock:
		case core::SectorType::Chamber: kind = sectorSurfaceOrBorrowed(sheet, own, "corridor"); break;
		case core::SectorType::Dumbwaiter: kind = sectorSurfaceOrBorrowed(sheet, own, "lift"); break;
		// The Shuttle's rail corridor is static architecture and wears its
		// own tile in the new family; carriage images are rendered separately
		// at the Shuttle's current position, on top of it (#1017). On the
		// old family it still borrows the Lift's shaft, as it always did
		// while the `shuttle` region sat undrawn (#1027).
		case core::SectorType::Shuttle: kind = newFamilySurfaceOrBorrowed(sheet, own, "lift"); break;
		// Staircase steps are procedural and render on top of their Sector
		// surface, which in the new family is the Staircase's own art rather
		// than a borrowed Stairwell shaft tile (#1017). On the old family it
		// is still the plain shaft: the same rule the deleted comment stated,
		// kept where it belongs - on the family that has no Staircase art of
		// its own (#1027).
		case core::SectorType::Staircase: kind = newFamilySurfaceOrBorrowed(sheet, own, "stairwell"); break;
		default: break;
		}
		// One tile per World cell (#1039). The loop that lays the cells out
		// lives with the atlas, because the cell size, the crop and the
		// culling it reproduces are the atlas's business. Which tile each of
		// those cells wears is asked through the resolver, because that is
		// the Sector's. Asking them separately is what lets one of them stop
		// being the same without this call changing again, and the two names
		// handed down here already are not the same thing: `own` is which
		// family the Sector belongs to, and `kind` is the base the borrow
		// rule settled on for the installed sheet. The resolver needs each
		// of them for its own question - the recorded variant is derived
		// from `own`, never from `kind`, which is what stops a borrowing
		// Sector wearing the borrowed family's variant (#1050).
		auto const ownFamily = own ? std::string{ own } : std::string{};
		if (!drawSectorTileSurfacePerCell(sheet, drawList,
			{bounds0.x, bounds1.y},
			{bounds1.x, bounds0.y}, gUISettings.worldZoom,
			sectorSurfaceTileResolver(sheet, ownFamily, std::move(kind), sector)))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}

	// Thresholds are normally drawn before the sector-specific geometry so their
	// apertures are not painted over. On a Shuttle's own Layer its Doors are seen
	// from the back and contribute wireframes rather than apertures; defer those
	// outlines until after the filled carriage so the carriage cannot hide them.
	// Apertures may reveal further thresholds, but only those authored on the
	// current Layer: renderSectorObjects skips back-facing thresholds to avoid
	// re-entering the same aperture. Nested scissor rectangles keep the view local.
	auto const backObjectsFollowGeometry = sectorType == core::SectorType::Shuttle
		&& isDrawnSolid(style);
	if (!backObjectsFollowGeometry)
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	auto selected = sector == gSelectedSector;

	// Sector-specific
	switch (sector->getType())
	{
	case core::SectorType::Dumbwaiter:
		if (isDrawnSolid(style))
		{
			auto unit = static_pointer_cast<const core::Dumbwaiter>(sector);
			auto position = unit->getCarPosition();
			core::Vector2 low{position.x + 0.1f, position.y + 0.18f};
			core::Vector2 high{position.x + 0.9f, position.y + 0.55f};
			transformPosition(low); transformPosition(high);
			// Procedural empty car: normal sector recursion supplies depth and clipping.
			drawList->AddRectFilled({low.x, high.y}, {high.x, low.y}, packColour(180, 190, 205, 255));
			drawList->AddRect({low.x, high.y}, {high.x, low.y}, packColour(60, 70, 85, 255), 0, 0, 2);
		}
		break;
	case core::SectorType::Chamber:
	{
		auto chamber = static_pointer_cast<const core::ChamberTransit>(sector);
		if (isDrawnSolid(style))
		{
			float centreX = (bounds0.x + bounds1.x) * 0.5f;
			float centreY = (bounds0.y + bounds1.y) * 0.5f;
			float half = min((bounds1.x - bounds0.x) * 0.3f, 30.0f);
			float direction = chamber->isLeftToRight() ? 1.0f : -1.0f;
			Vec2 tip{ centreX + direction * half, centreY };
			drawList->AddLine({ centreX - direction * half, centreY }, tip, White, 2);
			drawList->AddLine(tip, { tip.x - direction * 8, centreY - 6 }, White, 2);
			drawList->AddLine(tip, { tip.x - direction * 8, centreY + 6 }, White, 2);
			auto capacity = std::format("Capacity: {}", chamber->getJourneyCapacity());
			drawList->AddText({ bounds0.x + 4, bounds1.y + 4 }, White, capacity.c_str());
			auto readout = std::format("{}: {:.1f} s", chamber->getPhaseName(), chamber->getRemainingSeconds());
			drawList->AddText({ bounds0.x + 4, bounds1.y + 20 }, White, readout.c_str());
		}
		break;
	}
	case core::SectorType::Airlock:
	{
		auto chamber = static_pointer_cast<const core::AirlockTransit>(sector);
		if (!chamber->isCycleComplete() && isDrawnSolid(style))
		{
			auto text = std::format("{:.1f} s", chamber->getRemainingCycleSeconds());
			drawList->AddText({ (bounds0.x + bounds1.x) * 0.5f, bounds1.y + 4 }, White, text.c_str());
		}
		break;
	}
	case core::SectorType::Ladder:
		if (shouldRenderLadderGeometry(style))
			renderLadder(static_pointer_cast<const core::LadderTransit>(sector)->getLadder(),
				layer, style, selected, drawList);
		else if (style == LayerRenderStyle::Wireframe)
			renderBrokenWarning(*static_pointer_cast<const core::LadderTransit>(sector)->getLadder(), drawList);
		break;

	case core::SectorType::Lift:
		// Like every other Transit, a wireframe Layer contributes the Sector outline
		// only. Painting the car filled here leaked it over the selected Layer.
		if (isDrawnSolid(style))
			renderLift(static_pointer_cast<const core::LiftTransit>(sector)->getLift(),
				layer, style, selected, drawList);
		else if (style == LayerRenderStyle::Wireframe)
			renderBrokenWarning(*static_pointer_cast<const core::LiftTransit>(sector)->getLift(), drawList, true);
		break;

	case core::SectorType::Shuttle:
		// A wireframe Layer contributes a sector outline only. Its vehicle appears
		// when the selected Layer draws the sector itself, or through an aperture.
		if (isDrawnSolid(style))
			renderShuttle(static_pointer_cast<const core::ShuttleTransit>(sector)->getShuttle(),
				layer, style, selected, drawList);
		else if (style == LayerRenderStyle::Wireframe)
			renderBrokenWarning(*static_pointer_cast<const core::ShuttleTransit>(sector)->getShuttle(), drawList, true);
		break;

	case core::SectorType::Stairwell:
		if (shouldRenderStairwellGeometry(style))
			renderStairwell(static_pointer_cast<const core::StairwellTransit>(sector)->getStairwell(),
				layer, style, selected, drawList);
		break;

	case core::SectorType::Staircase:
		if (isDrawnSolid(style))
			renderStaircase(static_pointer_cast<const core::StaircaseTransit>(sector)->getStaircase(), drawList);
		else if (style == LayerRenderStyle::Wireframe)
			renderBrokenWarning(*static_pointer_cast<const core::StaircaseTransit>(sector)->getStaircase(), drawList);
		break;

	// Background and Facade carry no geometry of their own: their surface is
	// the tile their family's artwork carries, drawn by the surface pass
	// above, before the thresholds, so nothing here can paint over an
	// aperture (#49).
	default:
		break;
	}

	if (backObjectsFollowGeometry)
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	// Condition warnings remain visible on wireframe Layers without painting
	// filled device geometry over the selected Layer.
	if (style == LayerRenderStyle::Wireframe)
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto object = sector->getObject(index);
			if (auto ladder = dynamic_pointer_cast<const core::LadderSectorObject>(object))
				renderBrokenWarning(*ladder->getLadder(), drawList);
			else if (auto bridge = dynamic_pointer_cast<const core::ForceBridgeSectorObject>(object))
				renderBrokenWarning(*bridge->getForceBridge(), drawList);
			else if (auto platform = dynamic_pointer_cast<const core::LiftSectorObject>(object))
				renderBrokenWarning(*platform->getLift(), drawList, true);
		}

	// Objects
	if (isDrawnSolid(style))
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_INFRONT, drawList);
	}

	// Transit occupants obey the same aperture rule as their transit. Door and
	// Window rendering call this with a clip rectangle active from the Layer in
	// front; the wireframe overlay pass must not expose them.
	if (shouldRenderSectorAgents(sector->getType(), style))
		renderSectorDepthContent(sector, drawList);
	renderFurnitureMarkers(sector, layer, style, drawList);

	// Scan overlays follow occupants, and share the production scan clock. The
	// triangular wave is independent of the chamber's authored travel direction.
	if (sector->getType() == core::SectorType::Chamber && isDrawnSolid(style))
	{
		auto chamber = static_pointer_cast<const core::ChamberTransit>(sector);
		if (chamber->getSubtype() == core::ChamberSubtype::SecurityScanner
			&& chamber->getPhase() == core::SecurityScannerPhase::Scanning)
		{
			float progress = chamber->getScanProgress();
			float sweep = 1.0f - std::abs(2.0f * progress - 1.0f);
			float x = bounds0.x + (bounds1.x - bounds0.x) * sweep;
			float y = bounds1.y + (bounds0.y - bounds1.y) * sweep;
			auto colour = packColour(255, 0, 0, 128);
			drawList->AddLine({ bounds0.x, y }, { bounds1.x, y }, colour, 4.0f);
			drawList->AddLine({ x, bounds1.y }, { x, bounds0.y }, colour, 4.0f);
		}
		else if (chamber->getDecontaminationOpacity() > 0)
		{
			auto alpha = (int)std::lround(chamber->getDecontaminationOpacity() * 255.0f);
			drawList->AddRectFilled({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, packColour(255, 255, 255, alpha));
			// Generated Doors belong to neighbouring Sectors and may already have
			// been painted. Restore their visible leaves above the interior effect.
			for (int side = 0; side < 2; ++side)
				renderBulkheadDoor(chamber->getDoor(side), layer, style, false, drawList);
		}
	}

	// Render ceiling
	// A Background has no floor, ceiling or walls - its tile is the whole
	// surface, and a per-sector black border would break the seamless surface
	// adjacent Backgrounds are meant to form. A Facade skips them for the
	// same continuous-surface reason: its perimeter is open, so there are no
	// walls to draw, and its tile carries its extent (ADR 0003).
	auto const edgelessSurface = sectorType == core::SectorType::Background
		|| sectorType == core::SectorType::Facade;
	if (renderEdges && !edgelessSurface)
	{
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary(sheet,
			"ceiling", drawList,
			{bounds0.x, bounds1.y}, {bounds1.x, bounds1.y + 2.0f}))
			drawList->AddLine({ bounds0.x, bounds1.y }, { bounds1.x, bounds1.y }, packColour(0, 0, 0), 2.0f);

		// Render floor
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary(sheet,
			"floor", drawList,
			{bounds0.x, bounds0.y - 2.0f}, {bounds1.x, bounds0.y}))
			drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, packColour(0, 0, 0), 2.0f);

		// Render walls
		//
		// An open end takes away only the stretch of wall the neighbouring
		// Location actually shares, never the whole level: a Room level standing
		// taller than the Corridor it opens into keeps the wall above the
		// Corridor's ceiling, or the two Locations read as open to the void
		// above their own connection.
		//
		// The wireframe overlay passes the viewed Layer through too, so a wall on
		// the Layer behind does not draw across an opening the selected Layer
		// has made - otherwise the removed wall comes back as the Sector behind
		// it, and the connection reads as closed again.
		int const viewLayer{ std::max(0, gUISettings.visibleLayer) };
		float height{ 0.0f };

		for (uint32_t y = 0; y < sector->getLevelsHigh(); ++y)
		{
			auto levelHeight = sector->getLevelHeight(y);

			core::Vector2 wallBounds0, wallBounds1;

			sector->getBounds(wallBounds0, wallBounds1);

			wallBounds0.y += height;
			wallBounds1.y = wallBounds0.y + levelHeight;

			for (int const side : { CORE_SIDE_LEFT, CORE_SIDE_RIGHT })
			{
				auto const x = side == CORE_SIDE_LEFT ? wallBounds0.x : wallBounds1.x;

				for (auto const& span : wallSpansToDraw(gRenderWorld, *sector, y, side, viewLayer))
				{
					core::Vector2 from{ x, span.y0 }, to{ x, span.y1 };

					transformPosition(from);
					transformPosition(to);

					float const left = side == CORE_SIDE_LEFT ? from.x : from.x - 2.0f;
					if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary(
						sheet,
						side == CORE_SIDE_LEFT ? "left" : "right", drawList,
						{left, std::min(from.y, to.y)}, {left + 2.0f, std::max(from.y, to.y)}))
						drawList->AddLine({ from.x, from.y }, { to.x, to.y }, packColour(0, 0, 0), 2.0f);
				}
			}

			height += levelHeight;
		}
	}

	// Selection is an editor overlay. Emit it last so sector-specific fills,
	// passengers, objects, floors, and walls cannot paint over the yellow border.
	// The rule is type-agnostic: a selected Background takes the highlight exactly
	// as a Location does (#35).
	if (sector == gSelectedSector && shouldHighlightSelectedSector(style,
		sector->getLayerIndex(), layer,
		gUISettings.selectionMode == UISettings::SelectionMode::Sector))
	{
		Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 3.0f);
	}
}


void renderLocationContentAboveTransit(shared_ptr<const core::Sector> const& location,
	uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	if (isDrawnSolid(style))
	{
		renderSectorObjects(location, layer, LayerRenderStyle::Solid,
			RENDER_SECTOR_OBJECTS_BEHIND | RENDER_SECTOR_OBJECTS_INFRONT,
			drawList);
	}
	renderSectorDepthContent(location, drawList);
	if (isDrawnSolid(style)) renderFurnitureMarkers(location, layer, LayerRenderStyle::Solid, drawList);
}

//
// Draws one Transit inside a single aperture of the selected Layer. Everything the
// Transit contributes - Sector fill, geometry, and its own Agents - is clipped to
// that aperture by the caller, so a Transit on the Layer behind never fills over
// the Layer in front of it.
//
void renderTransitInAperture(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	TransitAperture const& aperture, WorldDrawList* drawList)
{
	// An aperture always looks through to the Layer behind the selection, so it
	// always carries the back-Layer colour.
	auto const colour = BackLocationColour;

	// A Staircase is drawn as its own polyline across the Location it crosses
	// rather than as a filled Sector, so it never contributes a Sector fill
	// through an aperture.
	if (transit->getType() == core::SectorType::Staircase)
	{
		renderStaircase(
			static_pointer_cast<const core::StaircaseTransit>(transit)->getStaircase(), drawList);
		renderSectorDepthContent(transit, drawList);
		return;
	}

	renderSector(transit, behindLayer, LayerRenderStyle::Aperture, false, colour, drawList);

	// A Ladder sits behind the contents of the Location it lands in, so those
	// contents are redrawn inside the same aperture.
	if (aperture.location && shouldRenderForeContentAfterTransit(transit->getType()))
	{
		renderLocationContentAboveTransit(aperture.location, aperture.location->getLayerIndex(),
			LayerRenderStyle::Aperture, drawList);
	}
}

//
// Draws one Transit of the Layer behind the selection, solid, through each of the
// apertures the selected Layer gives it. A Transit with no aperture is not drawn.
//
void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit,
	uint32_t behindLayer, std::vector<TransitAperture> const& apertures, WorldDrawList* drawList)
{
	static_assert(shouldClipTransitToApertures(LayerRenderStyle::Aperture),
		"this is the clipped pass, so it must only draw a style which clips to apertures");

	if (!transit)
	{
		return;
	}

	for (auto const& aperture : apertures)
	{
		auto bounds0 = aperture.min;
		auto bounds1 = aperture.max;
		transformPosition(bounds0);
		transformPosition(bounds1);

		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) },
			{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) }, true);

		renderTransitInAperture(transit, behindLayer, aperture, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
}


//
// The Sectors of one Layer that the current viewport actually sees.
//
// Every render pass must cull with the same bounds (#58): the visible world
// origin is (-xOffset, -yOffset) - the scrollbars drive both - over the
// viewport's own width and height. Passing a hard-coded Y origin here is what
// made high levels vanish when scrolled into view.
//
std::vector<std::shared_ptr<const core::Sector>> viewportSectors(
	std::shared_ptr<const core::World> const& world, uint32_t layer)
{
	auto const inverseZoom = 1.0f / gUISettings.worldZoom;
	return world->getSectorsInBounds(layer,
		-gUISettings.xOffset * inverseZoom, -gUISettings.yOffset * inverseZoom,
		gUISettings.worldViewportWidth * inverseZoom,
		gUISettings.worldViewportHeight * inverseZoom);
}


//
// Draws every Sector one Layer contributes, in the given style.
//
void renderSectors(shared_ptr<const core::World> world, uint32_t layer, LayerRenderStyle style,
	WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto const sectors = viewportSectors(world, layer);

	auto const colour = style == LayerRenderStyle::Solid ? ForeLocationColour : BackLocationColour;

	for (auto sector : sectors)
	{
		renderSector(sector, layer, style, true, colour, drawList);
	}
}


//
// Draws the Transits of the Layer directly behind the selection, solid, clipped to
// the apertures the selected Layer gives them. The selected Layer's Locations are
// the apertures onto the Layer behind, so a Transit never fills over ground the
// Layer in front of it does not open up. A Transit with a Door or clear Window in
// front of it is drawn by that threshold's own aperture as well.
//
// This pass runs whether or not the wireframe overlay is on: the overlay adds the
// Layer behind's outlines, it is not what makes that Layer visible.
//
void renderBehindLayerTransits(shared_ptr<const core::World> world, uint32_t behindLayer,
	std::vector<std::shared_ptr<const core::Sector>> const& viewSectors, WorldDrawList* drawList)
{
	auto const viewLayer = core::layerInFront(behindLayer);

	auto const transits = viewportSectors(world, behindLayer);

	for (auto const& transit : transits)
	{
		renderTransitThroughApertures(transit, behindLayer,
			transitApertures(transit, viewLayer, viewSectors), drawList);
	}
}


void renderWorld(shared_ptr<const core::World> world, WorldDrawList* drawList)
{
	// Own the World for exactly this call. The scope restores the previous
	// World on every exit, so closing the last document releases it as soon as
	// the viewport stops rendering it and an early return cannot leave it
	// reachable by the Sector-level passes.
	RenderWorldScope renderWorldScope(world);
	if (!drawList) return;

	auto const layerCount = world->getLayerCount();
	auto const viewLayer = static_cast<uint32_t>(clamp(gUISettings.visibleLayer, 0,
		static_cast<int>(layerCount) - 1));

	drawList->PushClipRect({ gUISettings.worldViewportX, gUISettings.worldViewportY },
		{ gUISettings.worldViewportX + gUISettings.worldViewportWidth,
			gUISettings.worldViewportY + gUISettings.worldViewportHeight }, true);

	for (auto const& pass : renderPasses(viewLayer, layerCount, gUISettings.renderNextLayerWireframe))
	{
		switch (pass.style)
		{
		case LayerRenderStyle::Solid:
			// Only the selected Layer is drawn whole. Deeper geometry enters through
			// the clipped aperture paths below or through nested thresholds.
			renderSectors(world, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Aperture:
		{
			auto const frontLayer = core::layerInFront(pass.layer);
			auto const frontSectors = viewportSectors(world, frontLayer);
			renderBehindLayerTransits(world, pass.layer, frontSectors, drawList);
			// Clipped Transits draw over the selected Layer's Locations. Restore
			// that Layer's thresholds, controls, and Agents in front of them.
			renderThresholdsControlsAndAgentsAboveTransit(frontSectors, frontLayer, drawList);
			break;
		}

		case LayerRenderStyle::Wireframe:
			// The wireframe overlay x-rays the Layer directly behind: its whole
			// footprint is outlined over the selection. Outlines only - never a
			// fill, Transit geometry, or the Agents inside them.
			renderSectors(world, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Hidden:
		default:
			break;
		}
	}

	// Queue diagnostics are selection overlays and should remain visible above
	// the selected object and agents.
	renderSelectedQueues(world, viewLayer, drawList);

	if (auto selector = world->getPlayerTransportSelector())
	{
		auto anchor = selector->anchor; transformPosition(anchor);
		pf::drawPlayerTransportSelector(*selector, anchor.x, anchor.y,
			[&](float x0, float y0, float x1, float y1, uint32_t colour) { drawList->AddRectFilled({x0,y0}, {x1,y1}, colour); },
			[&](float x0, float y0, float x1, float y1, uint32_t colour) { drawList->AddRect({x0,y0}, {x1,y1}, colour); });
	}

	// Grid
	if (gUISettings.renderGrid)
	{
		renderGrid(world, packColour(128, 128, 127), 1.0f, drawList);
	}
	drawList->PopClipRect();
}
