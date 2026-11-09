#include "WorldViewportZoom.h"
#include "UISettings.h"
#include "core/Defines.h"

extern UISettings gUISettings;

// The inverse of the World-to-screen transform renderWorld() applies: the
// caller hands over the cursor it is holding, and this says which point of
// the World sits under it at the current viewport placement and zoom.
core::Vector2 worldPositionAtCursor(core::Vector2 screenPosition)
{
	return {
		(screenPosition.x - gUISettings.worldViewportX - gUISettings.xOffset) / (CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom),
		(gUISettings.worldViewportY + gUISettings.worldViewportHeight - screenPosition.y - gUISettings.yOffset) / (CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom)
	};
}
