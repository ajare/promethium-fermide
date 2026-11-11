#include <array>
#include <cassert>
#include <cfloat>
#include <algorithm>
#include <chrono>
#include <optional>
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
// The value lives in Render.h as LightsOffTint so the headless checks can read
// the same colour the viewport paints with.
PackedColourValue LightsOffColour = packColour(LightsOffTint.r, LightsOffTint.g, LightsOffTint.b);
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


void renderDoorOpenUp(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	bounds1 = bounds0;
	// The leaf travels the Door's authored physical height. Door timing scales
	// with this distance so regular and tall OpenUp leaves have the same speed.
	bounds1.y += door->getOpenPercentage() * door->getSize().y;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = packColour(64, 192, 255);
		if (!drawObjectSprite("door", drawList, {bounds1.x, bounds1.y}, {bounds2.x, bounds2.y},
			White, {0, door->getOpenPercentage()}, {1, 1}))
			drawList->AddRectFilled({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, packColour(0, 0, 0));
	}
}


void renderDoorOpenLeft(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	// The leaf slides toward decreasing world X: its right edge tracks the
	// open percentage, so the aperture is vacated from the right edge and the
	// back Sector is revealed right-to-left.
	bounds1 = bounds2;
	bounds1.x -= door->getOpenPercentage() * (bounds2.x - bounds0.x);

	// At 100% open the leaf has slid clean out of the aperture, so it paints
	// neither fill nor outline - otherwise the wireframe pass would emit a
	// stroked zero-width rectangle at the left jamb.
	bool const leafVisible = bounds1.x > bounds0.x;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = packColour(64, 192, 255);
		if (leafVisible && !drawObjectSprite("door", drawList,
			{bounds0.x, bounds0.y}, {bounds1.x, bounds2.y}, White,
			{door->getOpenPercentage(), 0}, {1, 1}))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// The back Sector shows only through the vacated region: from the leaf's
		// right edge to the aperture's right edge, over the full aperture height.
		// ImGui clipping expects ascending Y coordinates, but we have flipped
		// them for rendering.
		drawList->PushClipRect({ bounds1.x, bounds2.y }, { bounds2.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Only the remaining leaf is outlined; the aperture carries no solid fill.
		if (leafVisible)
			drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds2.y }, packColour(0, 0, 0));
	}
}


void renderDoorOpenRight(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	// The leaf slides toward increasing world X: its left edge tracks the open
	// percentage, so the aperture is vacated from the left edge and the back
	// Sector is revealed left-to-right.
	bounds1 = bounds0;
	bounds1.x += door->getOpenPercentage() * (bounds2.x - bounds0.x);

	// At 100% open the leaf has slid clean out of the aperture, so it paints
	// neither fill nor outline - otherwise the wireframe pass would emit a
	// stroked zero-width rectangle at the right jamb.
	bool const leafVisible = bounds1.x < bounds2.x;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = packColour(64, 192, 255);
		if (leafVisible && !drawObjectSprite("door", drawList,
			{bounds1.x, bounds0.y}, {bounds2.x, bounds2.y}, White,
			{0, 0}, {1 - door->getOpenPercentage(), 1}))
			drawList->AddRectFilled({ bounds1.x, bounds0.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// The back Sector shows only through the vacated region: from the
		// aperture's left edge to the leaf's left edge, over the full aperture
		// height. ImGui clipping expects ascending Y coordinates, but we have
		// flipped them for rendering.
		drawList->PushClipRect({ bounds0.x, bounds2.y }, { bounds1.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Only the remaining leaf is outlined; the aperture carries no solid fill.
		if (leafVisible)
			drawList->AddRect({ bounds1.x, bounds0.y }, { bounds2.x, bounds2.y }, packColour(0, 0, 0));
	}
}


void renderDoorOpenApart(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool /* selected */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds2;

	door->getFullShape(bounds0, bounds2);

	// Two equal leaves split at the aperture midpoint and slide outward: each
	// travels half an aperture width over the open percentage, so the gap they
	// leave is always centred and becomes the whole aperture at 100% open.
	// Fractional midpoints are valid, so any Door width is supported.
	auto const worldX0 = bounds0.x;
	auto const worldX2 = bounds2.x;
	auto const halfWidth = (worldX2 - worldX0) * 0.5f;
	auto const midX = worldX0 + halfWidth;
	auto const travel = door->getOpenPercentage() * halfWidth;

	auto const leftLeafX0 = midX - halfWidth - travel;
	auto const leftLeafX1 = midX - travel;
	auto const rightLeafX0 = midX + travel;
	auto const rightLeafX1 = midX + halfWidth + travel;

	// A leaf paints only while some part of it is still inside the aperture; at
	// 100% open both have slid clean out and the Door draws no leaf geometry.
	bool const leftLeafVisible = leftLeafX1 > worldX0;
	bool const rightLeafVisible = rightLeafX0 < worldX2;

	transformPosition(bounds0);
	transformPosition(bounds2);

	// The transform flips Y for rendering, so bounds2 carries the aperture's top
	// and bounds0 its bottom.  X stays linear in world units, which lets the leaf
	// edges be projected without re-running the whole transform.
	auto const apertureLeft = min(bounds0.x, bounds2.x);
	auto const apertureRight = max(bounds0.x, bounds2.x);
	auto const apertureTop = min(bounds0.y, bounds2.y);
	auto const apertureBottom = max(bounds0.y, bounds2.y);
	auto const screenX = [&](float worldX)
	{
		return apertureLeft + (worldX - worldX0) * CORE_CELL_WIDTH_PIXELS
			* gUISettings.worldZoom;
	};

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = packColour(64, 192, 255);
		auto seamColour = packColour(0, 0, 0);

		// The leaves slide out of the Door, so their fill is clipped to the full
		// aperture and never paints over the surrounding Location.
		drawList->PushClipRect({ apertureLeft, apertureTop }, { apertureRight, apertureBottom }, true);

		if (leftLeafVisible)
		{
			if (!drawObjectSprite("door", drawList, {screenX(leftLeafX0), apertureTop},
				{screenX(leftLeafX1), apertureBottom}, White, {0, 0}, {0.5f, 1}))
				drawList->AddRectFilled({ screenX(leftLeafX0), apertureTop },
					{ screenX(leftLeafX1), apertureBottom }, doorColour);
			// The inner edge is the centre seam while closed and the facing edge of
			// the two leaves while the Door is part open.
			drawList->AddLine({ screenX(leftLeafX1), apertureTop },
				{ screenX(leftLeafX1), apertureBottom }, seamColour);
		}

		if (rightLeafVisible)
		{
			if (!drawObjectSprite("door", drawList, {screenX(rightLeafX0), apertureTop},
				{screenX(rightLeafX1), apertureBottom}, White, {0.5f, 0}, {1, 1}))
				drawList->AddRectFilled({ screenX(rightLeafX0), apertureTop },
					{ screenX(rightLeafX1), apertureBottom }, doorColour);
			drawList->AddLine({ screenX(rightLeafX0), apertureTop },
				{ screenX(rightLeafX0), apertureBottom }, seamColour);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();

		// The back Sector shows only through the centred gap between the two
		// leaves: it grows symmetrically from the midpoint and fills the whole
		// aperture at 100% open.  ImGui clipping expects ascending Y coordinates,
		// but we have flipped them for rendering.
		drawList->PushClipRect({ screenX(leftLeafX1), apertureTop },
			{ screenX(rightLeafX0), apertureBottom }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Both remaining leaves are outlined separately, clipped to the aperture,
		// and the aperture carries no solid fill.
		drawList->PushClipRect({ apertureLeft, apertureTop }, { apertureRight, apertureBottom }, true);

		if (leftLeafVisible)
			drawList->AddRect({ screenX(leftLeafX0), apertureTop },
				{ screenX(leftLeafX1), apertureBottom }, packColour(0, 0, 0));

		if (rightLeafVisible)
			drawList->AddRect({ screenX(rightLeafX0), apertureTop },
				{ screenX(rightLeafX1), apertureBottom }, packColour(0, 0, 0));

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

void renderDoor(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool selected, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto openStyle = door->getOpenStyle();

	switch (openStyle)
	{
	case core::Door::OpenStyle::OpenUp:
		renderDoorOpenUp(door, layer, style, selected, drawList);
		break;

	case core::Door::OpenStyle::OpenLeft:
		renderDoorOpenLeft(door, layer, style, selected, drawList);
		break;

	case core::Door::OpenStyle::OpenRight:
		renderDoorOpenRight(door, layer, style, selected, drawList);
		break;

	case core::Door::OpenStyle::OpenApart:
		renderDoorOpenApart(door, layer, style, selected, drawList);
		break;
	}

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
	PackedColourValue tint, WorldDrawList* drawList)
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
		{ max(from.x, to.x), max(from.y, to.y) }, tint);
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
	core::Vector2 fullMax, float progress, PackedColourValue tint,
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
		{ max(from.x, to.x), max(from.y, to.y) }, tint,
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
	PackedColourValue tint, WorldDrawList* drawList)
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
		door->getOpenPercentage(), tint, drawList);
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
	else if (!drawBulkheadDoorSlab(door, doorColour, drawList))
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

			// The Background fills with its own colour (apertureFillColour,
			// #34); the region clip trims that fill to this stretch of the
			// aperture.
			renderSector(region.background, backLayer, LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else if (backSector)
	{
		// The glass shows the Background's own colour, not the generic
		// back-layer tint (#34). A back Sector that carries no colour of its
		// own keeps the tint.
		auto const ownColour = apertureFillColour(*backSector);
		auto const apertureColour = ownColour.value_or(BackLocationColour);

		renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
			apertureColour, drawList);
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
		auto const sprite = windowStyle == core::Window::Style::Clear ? "window-clear"
			: windowStyle == core::Window::Style::Frosted ? "window-frosted" : "window-tinted";
		drawObjectSprite(sprite, drawList, {from.x, from.y}, {to.x, to.y});
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
	if (drawOffsetTile("walkway", anchor, colour, drawList)) return;
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
	if (!drawObjectSprite("marker", drawList, topLeft,
		{topLeft.x + size.x, topLeft.y + size.y}, packColour(251, 188, 4)))
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
							panel->getProgress(), packColour(78, 92, 110), drawList))
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


PackedColourValue agentRenderColour(core::Agent const& agent, bool selected)
{
	auto const colour = selected
		? core::SelectedAgentColour : agent.getEffectiveColour().value;
	return packColour(colour.r, colour.g, colour.b);
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

	auto const colour = agentRenderColour(*agent, gSelectedAgent == agent);
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
			{minimum.x + size.x, minimum.y + size.y}, colour);
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
std::string sectorSurfaceOrBorrowed(char const* own, char const* borrowed)
{
	return hasSectorTileSurface(own) ? std::string{ own } : std::string{ borrowed };
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
std::string newFamilySurfaceOrBorrowed(char const* own, char const* borrowed)
{
	return sectorTilesetIsNewFamily() && hasSectorTileSurface(own)
		? std::string{ own }
		: std::string{ borrowed };
}

// The resolver the Sector surface loop asks for one cell's tile (#1039).
//
// Every cell answers the Sector type's own base tile, which is all a cell
// can answer today: nothing records a per-cell choice yet, so nothing here
// draws differently from the one-name repeat it replaces. The resolver is
// asked anyway, per cell, because the resolver is where the answer grows
// and the loop is where the answer must not change. The loop reproduces the
// floor-anchored crop and the culling the #1037 contract pins, and a
// variant is meant to be a different picture and not a different geometry.
// When a Sector starts carrying a variant on a cell, the per-cell answer
// appears here - the cell's own entry where it has one, the base tile where
// it has none - and the loop that asks is untouched.
SectorSurfaceTileResolver sectorSurfaceTileResolver(std::string baseTile)
{
	return [baseTile = std::move(baseTile)](std::uint32_t, std::uint32_t)
		{ return baseTile; };
}

void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, PackedColourValue colour, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	// A Background is drawn with its own raw colour. The lights-off tint is
	// bypassed: a Background has no lights to switch, and tinting it would
	// silently override the colour the user picked and make the picker
	// untrustworthy. A Facade follows the same rendering rule (ADR 0003):
	// its user-picked colour is authoritative even though, unlike a
	// Background, its lights-off state still means what it means for the
	// agents and objects inside it.
	auto const sectorType = sector->getType();
	auto const ownColour = rendersAsFlatColour(sectorType);

	if (!bypassesLightsOffTint(sectorType) && !sector->areLightsOn())
	{
		colour = LightsOffColour;
	}

	// Render sector area
	core::Vector2 bounds0, bounds1;

	sector->getBounds(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	// The Sector's surface fill. A Sector that carries no colour of its own
	// takes the Layer's. A flat-colour Sector fills with its own colour here,
	// where the generic fill sits - before the BEHIND object pass - so a Door
	// or Window authored on a Facade keeps its aperture instead of being
	// painted over by the flat fill (#49). The wireframe overlay contributes
	// the outline only, never a fill (ADR 0002).
	if (ownColour)
	{
		auto const surface = flatSurfaceColour(*sector);
		assert(surface.has_value());
		auto const fill = *surface;

		if (shouldFillBackground(style))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
		else if (shouldOutlineBackground(style))
			drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		std::string kind;
		switch (sectorType)
		{
		case core::SectorType::Location: kind = static_pointer_cast<const core::Location>(sector)->isCorridor() ? "corridor" : "room"; break;
		case core::SectorType::Airlock: kind = sectorSurfaceOrBorrowed("airlock", "corridor"); break;
		case core::SectorType::Chamber: kind = sectorSurfaceOrBorrowed("chamber", "corridor"); break;
		case core::SectorType::Ladder: kind = "ladder"; break;
		case core::SectorType::Lift: kind = "lift"; break;
		case core::SectorType::Dumbwaiter: kind = sectorSurfaceOrBorrowed("dumbwaiter", "lift"); break;
		// The Shuttle's rail corridor is static architecture and wears its
		// own tile in the new family; carriage images are rendered separately
		// at the Shuttle's current position, on top of it (#1017). On the
		// old family it still borrows the Lift's shaft, as it always did
		// while the `shuttle` region sat undrawn (#1027).
		case core::SectorType::Shuttle: kind = newFamilySurfaceOrBorrowed("shuttle", "lift"); break;
		case core::SectorType::Stairwell: kind = "stairwell"; break;
		// Staircase steps are procedural and render on top of their Sector
		// surface, which in the new family is the Staircase's own art rather
		// than a borrowed Stairwell shaft tile (#1017). On the old family it
		// is still the plain shaft: the same rule the deleted comment stated,
		// kept where it belongs - on the family that has no Staircase art of
		// its own (#1027).
		case core::SectorType::Staircase: kind = newFamilySurfaceOrBorrowed("staircase", "stairwell"); break;
		default: break;
		}
		// One tile per World cell (#1039). The loop that lays the cells out
		// lives with the atlas, because the cell size, the crop and the
		// culling it reproduces are the atlas's business. Which tile each of
		// those cells wears is asked through the resolver, because that is
		// the Sector's. Today the two answers are the same everywhere; asking
		// them separately is what lets one of them stop being the same
		// without this call changing again.
		if (!drawSectorTileSurfacePerCell(drawList, {bounds0.x, bounds1.y},
			{bounds1.x, bounds0.y}, colour, gUISettings.worldZoom,
			sectorSurfaceTileResolver(std::move(kind))))
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
	// the flat fill emitted above, before the thresholds, so nothing here can
	// paint over an aperture (#49).
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
	// A Background has no floor, ceiling or walls - its colour is the whole
	// surface, and a per-sector black border would break the seamless surface
	// adjacent Backgrounds are meant to form. A Facade skips them for the
	// same flat-surface reason: its perimeter is open, so there are no walls
	// to draw, and the flat colour carries its extent (ADR 0003).
	if (renderEdges && !ownColour)
	{
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary("ceiling", drawList,
			{bounds0.x, bounds1.y}, {bounds1.x, bounds1.y + 2.0f}))
			drawList->AddLine({ bounds0.x, bounds1.y }, { bounds1.x, bounds1.y }, packColour(0, 0, 0), 2.0f);

		// Render floor
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary("floor", drawList,
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
			RENDER_SECTOR_OBJECTS_BEHIND | RENDER_SECTOR_OBJECTS_INFRONT, drawList);
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
