#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/SecurityScannerTransit.h"
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
				uint32_t ends[] = { world->addRoom("Left", 0, 0, 0, 2, 1), world->addCorridor(0, 0, 2 + width, 2, 1) };
				auto index = world->addSecurityScanner(0, 0, 2, width, direction);
				auto marker = world->addSectorMarker(ends[direction ? 1 : 0], 0, 1.0f); world->finishBuild();
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
				auto id = world->createAgent("Traveller", ends[direction ? 0 : 1], 0, 1.0f);
				auto actor = world->lookupAgent(id).entity;
				actor->setPath(world->getGraph()->calculatePath(actor, world->getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
				for (unsigned tick = 0; tick < 2000 && chamber->getPhase() != core::SecurityScannerPhase::Scanning; ++tick) world->advanceTick();
				require(chamber->getPhase() == core::SecurityScannerPhase::Scanning, "Render journey did not reach scan");
				WorldDrawList scanDrawing({ { 0, 0 }, { 1600, 720 } }); renderWorld(world, &scanDrawing);
				bool countdown = false;
				for (auto const& command : scanDrawing.commands())
					if (auto text = std::get_if<WorldDrawList::Text>(&command)) countdown = countdown || text->value == "Scanning: 2.0 s";
				require(countdown, "Canvas omitted live phase/countdown readout");
			}
		gSelectedSector.reset(); ImGui::EndFrame();
	}
}

void render_smoke::registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/chamberCommandStream", isolated<chambers> });
}
