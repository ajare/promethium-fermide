#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
#include <set>

extern UISettings gUISettings;
extern std::shared_ptr<const core::Sector> gSelectedSector;

namespace
{
	void chambers(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = { 1600, 720 };
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.visibleLayer = 0; gUISettings.worldViewportWidth = 1600;
		gUISettings.worldViewportHeight = 720; gUISettings.worldZoom = 1;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		gUISettings.selectionMode = UISettings::SelectionMode::Sector;
		for (uint32_t width : { 1u, 3u })
			for (bool direction : { false, true })
			{
				auto world = std::make_shared<core::World>("Scanner drawing", 12, 2);
				world->addRoom("Left", 0, 0, 0, 2, 1); world->addCorridor(0, 0, 2 + width, 2, 1);
				auto index = world->addSecurityScanner(0, 0, 2, width, direction); world->finishBuild();
				gSelectedSector = world->getSector(index);
				WorldDrawList drawing({ { 0, 0 }, { 1600, 720 } }); renderWorld(world, &drawing);
				std::set<float> doors; uint32_t buttons = 0, arrows = 0;
				bool surface = false, capacity = false, selected = false;
				for (auto const& command : drawing.commands())
				{
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
					{
						float low = triangle->positions[0].x, high = low;
						for (auto point : triangle->positions) { low = std::min(low, point.x); high = std::max(high, point.x); }
						if (triangle->colour == ImU32(ImColor(128, 192, 182))) doors.insert((low + high) * 0.5f);
						if (triangle->colour == ImU32(ImColor(0, 255, 128))) ++buttons;
						if (low == 2 * CORE_CELL_WIDTH_PIXELS && high == (2 + width) * CORE_CELL_WIDTH_PIXELS) surface = true;
					}
					if (auto line = std::get_if<WorldDrawList::Line>(&command))
					{
						if (line->colour == ImU32(ImColor(255, 255, 0)) && line->thickness == 3) selected = true;
						if (line->colour == IM_COL32_WHITE && line->thickness == 2)
						{
							++arrows;
							if (line->from.y == line->to.y)
								require((line->to.x > line->from.x) == direction, "Scanner arrow points against authored direction");
						}
					}
					if (auto text = std::get_if<WorldDrawList::Text>(&command))
						if (text->value == "Capacity: 1") capacity = true;
				}
				require(surface && doors.size() == 2 && buttons == 0 && arrows == 3 && capacity && selected,
					"Scanner render omitted surface/Doors/direction/capacity/selection or added buttons");
			}
		gSelectedSector.reset(); ImGui::EndFrame();
	}
}

void render_smoke::registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/chamberCommandStream", isolated<chambers> });
}
