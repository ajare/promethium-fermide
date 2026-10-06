#include "LocationPlan.h"
#include "AgentPathForRendering.h"
#include <string>
#include <algorithm>
#include <set>
#include <map>
#include <cmath>
#include <iterator>
#include <vector>
#include "core/Edge.h"
#include "core/Marker.h"
#include "core/Agent.h"

namespace
{
	constexpr float GraphMargin = 8, VertexRadius = 3;
	constexpr ImU32 VertexColours[]{IM_COL32(150, 245, 255, 255), IM_COL32(105, 230, 140, 255), IM_COL32(255, 190, 60, 255)};
	struct PlanVertex
	{
		std::shared_ptr<const core::Vertex> vertex;
		std::string name;
		unsigned category;
	};

	bool onPlan(core::Vertex const& vertex, core::Sector const& location, uint32_t worldLevel)
	{
		auto sector = vertex.getSector();
		return sector && sector->getIndex() == location.getIndex()
			&& sector->getLayerIndex() == location.getLayerIndex()
			&& vertex.getPosition().y == worldLevel;
	}

	// Vertices have no intrinsic Local depth. Project a shared vertex once at
	// each incident edge depth; an isolated vertex uses the neutral depth 0.
	std::set<int> vertexDepths(core::Vertex const& vertex)
	{
		std::set<int> depths;
		for (auto const& edge : vertex.getEdges()) depths.insert(edge->getLocalDepth());
		if (depths.empty()) depths.insert(0);
		return depths;
	}

	std::vector<PlanVertex> planVertices(core::World const& world, core::Sector const& location, uint32_t worldLevel)
	{
		std::map<std::string, std::pair<std::string, bool>> authored;
		if (auto catalogue = world.furnitureCatalogue())
			for (auto const& instance : world.furniture())
			{
				if (instance.sector != location.getIndex() || location.getCellY() + instance.y != worldLevel) continue;
				if (auto definition = catalogue->definition(instance.definitionKey))
					for (auto const& vertex : definition->vertices)
						authored.emplace("furniture:" + std::to_string(instance.id) + ":" + vertex.key,
							std::pair{instance.name + " / " + vertex.key, vertex.external});
			}
		std::vector<PlanVertex> result;
		if (auto graph = world.getGraph())
			for (auto const& vertex : graph->getVertices())
				if (onPlan(*vertex, location, worldLevel))
				{
					auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
					auto entry = authored.find(vertex->getTopologyKey());
					result.push_back({vertex, marker ? marker->getName()
						: entry != authored.end() ? entry->second.first : vertex->getDescription(),
						marker ? 2u : entry != authored.end() && entry->second.second ? 1u : 0u});
				}
		// Coincident ports paint over ordinary anchors; usable points paint last.
		std::stable_sort(result.begin(), result.end(), [](auto const& a, auto const& b) { return a.category < b.category; });
		return result;
	}

	ImVec2 vertexScreen(core::Vertex const& vertex, core::Sector const& location,
		ImVec2 position, ImVec2 size, uint32_t depthRows, int depth)
	{
		return {position.x + 48 + (vertex.getPosition().x - location.getCellX()) * (size.x - 60) / location.getCellsWide(),
			position.y + size.y - 28 - static_cast<float>(depth) * (size.y - 36) / std::max(depthRows, 4u)};
	}
}

std::optional<std::string> locationPlanVertexNameAtPosition(core::World const& world,
	core::Sector const& location, uint32_t worldLevel, ImVec2 mouse,
	ImVec2 position, ImVec2 size, uint32_t depthRows)
{
	if (size.x <= 64 || size.y <= 40 || !location.getCellsWide()
		|| mouse.x < std::max(position.x, position.x + 48 - GraphMargin)
		|| mouse.x >= std::min(position.x + size.x, position.x + size.x - 12 + GraphMargin)
		|| mouse.y < position.y || mouse.y >= std::min(position.y + size.y, position.y + size.y - 28 + GraphMargin)) return {};
	auto vertices = planVertices(world, location, worldLevel);
	for (auto it = vertices.rbegin(); it != vertices.rend(); ++it)
	{
		auto depths = vertexDepths(*it->vertex);
		for (auto depth = depths.rbegin(); depth != depths.rend(); ++depth)
		{
			auto p = vertexScreen(*it->vertex, location, position, size, depthRows, *depth);
			float dx = p.x - mouse.x, dy = p.y - mouse.y;
			if (dx * dx + dy * dy <= (VertexRadius + 2) * (VertexRadius + 2)) return it->name;
		}
	}
	return {};
}

void renderLocationPlanPreview(WorldDrawList& commands, core::Sector const& location,
	core::FurnitureDefinition const& definition, float x, int depth, bool valid,
	ImVec2 position, ImVec2 size, uint32_t depthRows)
{
	if (size.x <= 64 || size.y <= 40 || !location.getCellsWide()) return;
	float left = position.x + 48, right = position.x + size.x - 12;
	float top = position.y + 8, bottom = position.y + size.y - 28;
	float cellWidth = (right - left) / location.getCellsWide();
	float rowHeight = (bottom - top) / std::max(depthRows, 4u);
	ImVec2 min{left + (x + definition.minX) * cellWidth, bottom - (float(depth) + 1) * rowHeight};
	ImVec2 max{left + (x + definition.maxX) * cellWidth, bottom - float(depth) * rowHeight};
	commands.PushClipRect({left, top}, {right, bottom}, true);
	commands.AddRectFilled(min, max, valid ? IM_COL32(80, 200, 120, 65) : IM_COL32(244, 67, 54, 65));
	commands.AddRect(min, max, valid ? IM_COL32(80, 200, 120, 255) : IM_COL32(244, 67, 54, 255), 0, 0, 2);
	commands.PushClipRect(min, max, true);
	commands.AddText({min.x + 4, min.y + 3}, IM_COL32_WHITE, definition.label.c_str());
	commands.PopClipRect();
	commands.PopClipRect();
}

uint32_t locationPlanDepthRows(core::World const& world, core::Sector const& location,
	uint32_t worldLevel)
{
	uint32_t rows = 4;
	if (auto graph = world.getGraph())
		for (auto const& vertex : graph->getVertices())
			if (onPlan(*vertex, location, worldLevel))
				for (auto depth : vertexDepths(*vertex))
					if (depth >= 0) rows = std::max(rows, static_cast<uint32_t>(depth) + 2);
	auto catalogue = world.furnitureCatalogue();
	if (!catalogue) return rows;
	for (auto const& instance : world.furniture())
	{
		if (instance.sector != location.getIndex() || location.getCellY() + instance.y != worldLevel) continue;
		auto definition = catalogue->definition(instance.definitionKey);
		if (!definition) continue;
		rows = std::max(rows, static_cast<uint32_t>(instance.localDepth) + 2);
		// Graph resolves an absent offset to fixed depth 0, not instance depth.
		for (auto const& edge : definition->edges)
		{
			auto depth = edge.depthOffset ? int64_t{instance.localDepth} + *edge.depthOffset : 0;
			if (depth >= 0) rows = std::max(rows, static_cast<uint32_t>(depth) + 2);
		}
	}
	return rows;
}

void renderLocationPlanGrid(WorldDrawList& commands, core::Sector const& location,
	ImVec2 position, ImVec2 size, uint32_t depthRows, core::World const* world, uint32_t worldLevel,
	uint64_t selectedId, core::Agent const* selectedAgent)
{
	depthRows = std::max(depthRows, 4u);
	if (size.x <= 64 || size.y <= 40 || !location.getCellsWide()) return;
	commands.PushClipRect(position, {position.x + size.x, position.y + size.y}, true);
	commands.AddRectFilled(position, {position.x + size.x, position.y + size.y}, IM_COL32(24, 24, 28, 255));
	float const left = position.x + 48, right = position.x + size.x - 12;
	float const top = position.y + 8, bottom = position.y + size.y - 28;
	float const cellWidth = (right - left) / location.getCellsWide();
	float const rowHeight = (bottom - top) / depthRows;
	if (world && worldLevel >= location.getCellY()
		&& worldLevel < location.getCellY() + location.getLevelsHigh())
	{
		auto layer = world->getLayer(location.getLayerIndex());
		for (uint32_t x = 0; x < location.getCellsWide(); ++x)
		{
			auto const& cell = layer->getCellDefinition(location.getCellX() + x, worldLevel);
			if (cell.sectorIndex != location.getIndex() || (cell.floorType != core::CellFloorType::Ground
				&& cell.floorType != core::CellFloorType::Walkway))
				commands.AddRectFilled({left + x * cellWidth, top}, {left + (x + 1) * cellWidth, bottom},
					IM_COL32(65, 40, 40, 255));
		}
	}
	for (uint32_t x = 0; x <= location.getCellsWide(); ++x)
	{
		float const screenX = left + x * cellWidth;
		commands.AddLine({screenX, top}, {screenX, bottom}, IM_COL32(95, 95, 105, 255));
		// Keep large Locations readable without changing the World-X mapping.
		if (x == 0 || x == location.getCellsWide() || cellWidth >= 32)
		{
			auto label = std::to_string(location.getCellX() + x);
			commands.AddText({screenX - 4, bottom + 6}, IM_COL32_WHITE, label.c_str());
		}
	}
	// Very large authored depths remain in the mapping without generating billions
	// of indistinguishable lines/labels in a small viewport.
	auto const step = static_cast<uint32_t>(std::clamp(std::ceil(16.0 / rowHeight), 1.0, static_cast<double>(depthRows)));
	for (uint64_t depth = 0; depth <= depthRows; depth = std::min(depth + step, uint64_t{depthRows}))
	{
		float const y = bottom - depth * rowHeight;
		commands.AddLine({left, y}, {right, y}, IM_COL32(95, 95, 105, 255));
		if (depth < depthRows)
		{
			auto label = std::to_string(depth);
			commands.AddText({position.x + 16, y - 6}, IM_COL32_WHITE, label.c_str());
		}
		if (depth == depthRows) break;
	}
	// Contents are clipped to the grid interior as well as the caller viewport.
	commands.PushClipRect({left, top}, {right, bottom}, true);
	if (world && world->furnitureCatalogue())
	{
		auto catalogue = world->furnitureCatalogue();
		for (auto const& instance : world->furniture())
		{
			if (instance.sector != location.getIndex() || location.getCellY() + instance.y != worldLevel) continue;
			auto definition = catalogue->definition(instance.definitionKey);
			if (!definition) continue;
			float const x0 = left + (instance.x + definition->minX) * cellWidth;
			float const x1 = left + (instance.x + definition->maxX) * cellWidth;
			float const y0 = bottom - (static_cast<float>(instance.localDepth) + 1) * rowHeight;
			float const y1 = bottom - instance.localDepth * rowHeight;
			commands.AddRectFilled({x0, y0}, {x1, y1}, IM_COL32(55, 90, 120, 255));
			commands.AddRect({x0, y0}, {x1, y1}, instance.id == selectedId
				? IM_COL32(251, 188, 4, 255) : IM_COL32(140, 190, 220, 255));
			// Keep a long label from covering adjacent footprints.
			commands.PushClipRect({x0, y0}, {x1, y1}, true);
			commands.AddText({x0 + 4, y0 + 3}, IM_COL32_WHITE, instance.name.c_str());
			commands.PopClipRect();
		}
	}
	commands.PopClipRect();
	// Leave breathing room outside the grid so depth-0 edges and boundary
	// vertices retain their full stroke/radius. The outer viewport/caller clip
	// still bounds the overlay; Furniture and input keep their existing bounds.
	commands.PushClipRect({left - GraphMargin, top - GraphMargin},
		{right + GraphMargin, bottom + GraphMargin}, true);
	// Draw the live graph above footprints, never reconstructing catalogue
	// connectivity or joining coincident points. Only edges wholly on this
	// Location/Level are shown; boundary vertices remain visible.
	if (world) if (auto graph = world->getGraph())
	{
		auto vertices = planVertices(*world, location, worldLevel);
		auto screen = [&](core::Vertex const& vertex, int depth)
		{
			return vertexScreen(vertex, location, position, size, depthRows, depth);
		};
		for (auto const& edge : graph->getEdges())
		{
			auto from = edge->getVertex(0), to = edge->getVertex(1);
			if (!from || !to || !onPlan(*from, location, worldLevel) || !onPlan(*to, location, worldLevel)) continue;
			commands.AddLine(screen(*from, edge->getLocalDepth()), screen(*to, edge->getLocalDepth()),
				IM_COL32(80, 210, 220, 255), 3);
		}
		// These dotted guides join projections of the same graph vertex, not
		// graph edges or distinct vertices that happen to share a name/position.
		// Consecutive depths form one chain; keep the dots behind all glyphs.
		for (auto const& entry : vertices)
		{
			auto depths = vertexDepths(*entry.vertex);
			for (auto it = depths.begin(); it != depths.end(); ++it)
			{
				auto next = std::next(it);
				if (next == depths.end()) break;
				auto a = screen(*entry.vertex, *next), b = screen(*entry.vertex, *it);
				// Limit recording to visible pixels even if a caller supplies
				// fewer rows than the live graph needs. Leave the glyphs clear.
				auto clipMin = commands.GetClipRectMin(), clipMax = commands.GetClipRectMax();
				if (a.x < clipMin.x || a.x > clipMax.x) continue;
				float first = std::max(a.y + VertexRadius + 2, clipMin.y + 1);
				float last = std::min(b.y - VertexRadius - 2, clipMax.y - 1);
				for (float y = first; y <= last; y += 5)
					commands.AddCircleFilled({a.x, y}, 1, IM_COL32(180, 180, 180, 200), 6);
			}
		}
		// Usable points belong to the Furniture's instance-depth row, whereas
		// their routing vertices are projected at incident edge depths. Resolve
		// each link by owned Marker identity, never by proximity or a display name.
		std::map<core::MarkerId, core::Vertex const*> usableVertices;
		for (auto const& vertex : graph->getVertices())
			if (onPlan(*vertex, location, worldLevel))
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()))
					usableVertices.emplace(marker->getId(), vertex.get());
		if (auto catalogue = world->furnitureCatalogue())
			for (auto const& instance : world->furniture())
			{
				if (instance.sector != location.getIndex() || location.getCellY() + instance.y != worldLevel) continue;
				auto definition = catalogue->definition(instance.definitionKey);
				if (!definition) continue;
				for (auto const& point : definition->usablePoints)
				{
					ImVec2 centre{left + (instance.x + point.x) * cellWidth,
						bottom - (static_cast<float>(instance.localDepth) + .5f) * rowHeight};
					auto destination = std::find_if(instance.destinations.begin(), instance.destinations.end(),
						[&](auto const& entry) { return entry.key == point.key; });
					if (destination != instance.destinations.end())
						if (auto vertex = usableVertices.find(destination->marker); vertex != usableVertices.end())
							for (auto depth : vertexDepths(*vertex->second))
							{
								auto from = screen(*vertex->second, depth);
								if (from.x != centre.x || from.y != centre.y)
									commands.AddLine(from, centre, IM_COL32(255, 190, 60, 255), 3);
							}
					commands.AddCircle(centre, 6, IM_COL32(255, 190, 60, 255), 16);
				}
			}
		for (auto const& entry : vertices)
			for (auto depth : vertexDepths(*entry.vertex))
				commands.AddCircleFilled(screen(*entry.vertex, depth), VertexRadius, VertexColours[entry.category], 8);

		// Highlight only the selected Path, not all incident projections of its
		// vertices. Ordinary Markers and floor anchors participate just like ports.
		if (auto path = agentPathForRendering(world, selectedAgent); path && !path->nodes.empty())
		{
			constexpr auto outline = IM_COL32(32, 32, 32, 220);
			constexpr auto colour = IM_COL32(255, 196, 0, 240);
			auto segment = [&](ImVec2 from, ImVec2 to)
			{
				commands.AddLine(from, to, outline, 5);
				commands.AddLine(from, to, colour, 2.5f);
			};
			auto depthAt = [&](size_t index)
			{
				auto const& node = path->nodes[index];
				return node.edge ? node.edge->getLocalDepth() : 0;
			};
			auto const& first = path->nodes.front().targetVertex;
			auto agentSector = selectedAgent->getSector();
			auto agentPosition = selectedAgent->getGlobalPosition();
			if (first && onPlan(*first, location, worldLevel) && agentSector
				&& agentSector == &location && agentPosition.y == worldLevel)
			{
				ImVec2 from{left + (agentPosition.x - location.getCellX()) * cellWidth,
					bottom - selectedAgent->getLocalDepth() * rowHeight};
				segment(from, screen(*first, depthAt(0)));
			}
			for (size_t i = 0; i < path->nodes.size(); ++i)
			{
				auto const& vertex = path->nodes[i].targetVertex;
				if (!vertex || !onPlan(*vertex, location, worldLevel)) continue;
				auto p = screen(*vertex, depthAt(i));
				if (i + 1 < path->nodes.size())
				{
					auto const& next = path->nodes[i + 1];
					if (next.edge && next.targetVertex && onPlan(*next.targetVertex, location, worldLevel))
					{
						auto outgoing = screen(*vertex, next.edge->getLocalDepth());
						if (p.y != outgoing.y) segment(p, outgoing);
						segment(outgoing, screen(*next.targetVertex, next.edge->getLocalDepth()));
					}
				}
			}
			// Paint glyphs last so coincident graph vertices cannot hide the route.
			for (size_t i = 0; i < path->nodes.size(); ++i)
			{
				auto const& vertex = path->nodes[i].targetVertex;
				if (!vertex || !onPlan(*vertex, location, worldLevel)) continue;
				std::set<int> depths{depthAt(i)};
				if (i + 1 < path->nodes.size())
				{
					auto const& next = path->nodes[i + 1];
					if (next.edge && next.targetVertex && onPlan(*next.targetVertex, location, worldLevel))
						depths.insert(next.edge->getLocalDepth());
				}
				for (auto depth : depths)
				{
					auto p = screen(*vertex, depth);
					commands.AddCircleFilled(p, 5, outline, 8);
					commands.AddCircleFilled(p, VertexRadius, colour, 8);
				}
			}
		}
	}
	commands.PopClipRect();
	commands.PopClipRect();
}
