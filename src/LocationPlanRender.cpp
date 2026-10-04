#include "LocationPlan.h"
#include <string>
#include <algorithm>
#include <set>
#include <cmath>

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
	ImVec2 position, ImVec2 size, uint32_t depthRows, core::World const* world, uint32_t worldLevel, uint64_t selectedId)
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
	auto const step = static_cast<uint32_t>(std::clamp(std::ceil(16.0 / rowHeight), 1.0, double{depthRows}));
	for (uint64_t depth = 0; depth <= depthRows; depth = std::min(depth + step, uint64_t{depthRows}))
	{
		float const y = bottom - depth * rowHeight;
		commands.AddLine({left, y}, {right, y}, IM_COL32(95, 95, 105, 255));
		if (depth < depthRows)
		{
			auto label = std::to_string(depth);
			commands.AddText({position.x + 16, y - rowHeight / 2 - 6}, IM_COL32_WHITE, label.c_str());
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
			for (auto const& point : definition->usablePoints)
			{
				std::set<int> depths;
				for (auto const& vertex : definition->vertices)
					if (vertex.usablePoint == point.key)
						for (auto const& edge : definition->edges)
							if (edge.from == vertex.key || edge.to == vertex.key)
								depths.insert(edge.depthOffset ? instance.localDepth + *edge.depthOffset : 0);
				if (depths.empty()) depths.insert(instance.localDepth);
				for (auto depth : depths)
					commands.AddCircleFilled({left + (instance.x + point.x) * cellWidth,
						bottom - (static_cast<float>(depth) + 0.5f) * rowHeight},
						3, IM_COL32(255, 210, 90, 255), 8);
			}
		}
	}
	commands.PopClipRect();
	commands.PopClipRect();
}
