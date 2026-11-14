#include "WorldViewportZoom.h"
#include "UISettings.h"
#include "WorldToScreen.h"
#include "core/Defines.h"

extern UISettings gUISettings;

// The inverse of the World-to-screen transform renderWorld() applies: the
// caller hands over the cursor it is holding, and this says which point of
// the World sits under it at the current viewport placement and zoom. Read
// off the same shared mapping the forward step uses, so the two cannot drift
// apart (#1100).
core::Vector2 worldPositionAtCursor(core::Vector2 screenPosition)
{
	return pf::render::worldToScreen(gUISettings).toWorld(
		{screenPosition.x, screenPosition.y});
}
