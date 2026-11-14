#include "WorldViewportZoom.h"
#include "UISettings.h"
#include "WorldToScreen.h"
#include "core/Defines.h"
#include "core/World.h"

extern UISettings gUISettings;

// The inverse of the World-to-screen transform renderWorld() applies: the
// caller hands over the cursor it is holding, and this says which point of
// the World sits under it at the current viewport placement and zoom. Read
// off the same shared mapping the forward step uses, so the two cannot drift
// apart (#1100).
//
// No floor lift is installed here, because no Layer is named. A cursor
// aimed at a Layer the floor depth has shifted wants
// `pf::render::worldToScreenForLayer` read backward (#1105).
core::Vector2 worldPositionAtCursor(core::Vector2 screenPosition)
{
	return pf::render::worldToScreen(gUISettings).toWorld(
		{screenPosition.x, screenPosition.y});
}

namespace pf::render
{
	uint32_t viewLayerFor(core::World const& world, int viewLayer)
	{
		auto const layerCount = world.getLayerCount();
		if (layerCount == 0) return 0;
		// The same clamp the draw pass applies to its own setting, so the
		// Layer picked is the Layer drawn even where the setting has run off
		// one end of the range.
		return static_cast<uint32_t>(std::clamp(viewLayer, 0,
			static_cast<int>(layerCount) - 1));
	}

	float layerFloorLiftPx(core::World const& world, uint32_t layerIndex, float zoom)
	{
		// Asked of the World rather than of the frame in progress: the picker
		// runs outside the render pass, and the World is the one thing both
		// are looking at. `layerFloorOffsetPx` answers through the whole
		// staircase, so the picker and the drawer cannot sum it differently.
		return floorLiftScreenPx(world.layerFloorOffsetPx(layerIndex), zoom);
	}

	WorldToScreen worldToScreenForLayer(UISettings const& settings,
		core::World const& world, uint32_t layerIndex, float zoom)
	{
		return worldToScreenAt(settings, zoom)
			.withFloorLift(layerFloorLiftPx(world, layerIndex, zoom));
	}
}
