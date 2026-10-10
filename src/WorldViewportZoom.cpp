#include "WorldViewportZoom.h"
#include "UISettings.h"
#include "core/Defines.h"

extern UISettings gUISettings;

core::Vector2 getMouseWorldPosition()
{
	auto mouseScreenPos = ImGui::GetMousePos();
	return {
		(mouseScreenPos.x - gUISettings.worldViewportX - gUISettings.xOffset) / (CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom),
		(gUISettings.worldViewportY + gUISettings.worldViewportHeight - mouseScreenPos.y - gUISettings.yOffset) / (CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom)
	};
}
