#include "LocationPlan.h"
#include <string>

void renderLocationPlanGrid(WorldDrawList& commands, core::Sector const& location,
	ImVec2 position, ImVec2 size)
{
	if (size.x <= 64 || size.y <= 40 || !location.getCellsWide()) return;
	commands.PushClipRect(position, {position.x + size.x, position.y + size.y}, true);
	commands.AddRectFilled(position, {position.x + size.x, position.y + size.y}, IM_COL32(24, 24, 28, 255));
	float const left = position.x + 48, right = position.x + size.x - 12;
	float const top = position.y + 8, bottom = position.y + size.y - 28;
	float const cellWidth = (right - left) / location.getCellsWide();
	float const rowHeight = (bottom - top) / 4;
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
	for (uint32_t depth = 0; depth <= 4; ++depth)
	{
		float const y = bottom - depth * rowHeight;
		commands.AddLine({left, y}, {right, y}, IM_COL32(95, 95, 105, 255));
		if (depth < 4)
		{
			auto label = std::to_string(depth);
			commands.AddText({position.x + 16, y - rowHeight / 2 - 6}, IM_COL32_WHITE, label.c_str());
		}
	}
	commands.PopClipRect();
}
