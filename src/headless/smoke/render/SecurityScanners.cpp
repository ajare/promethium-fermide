#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/SecurityScannerTransit.h"
#include "core/YamlSerializer.h"
#include "imgui/IconsFontAwesome5.h"
#include <cmath>
#include <set>

extern UISettings gUISettings;
extern std::shared_ptr<const core::Sector> gSelectedSector;

namespace
{
	void beams(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = { 1600, 720 };
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.visibleLayer = 0; gUISettings.worldViewportWidth = 1600;
		gUISettings.worldViewportHeight = 720; gUISettings.worldZoom = 1;
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		for (uint32_t width : { 1u, 2u, 5u })
			for (bool direction : { false, true })
			{
				auto world = std::make_shared<core::World>("Scanner beams", width + 8, 2);
				uint32_t ends[] = { world->addRoom("Left", 0, 0, 0, 3, 1), world->addCorridor(0, 0, width + 3, 3, 1) };
				auto index = world->addSecurityScanner(0, 0, 3, width, direction);
				auto marker = world->addSectorMarker(ends[direction ? 1 : 0], 0, 1.5f);
				world->finishBuild();
				auto id = world->createAgent("Traveller", ends[direction ? 0 : 1], 0, 1.5f);
				auto actor = world->lookupAgent(id).entity;
				actor->setPath(world->getGraph()->calculatePath(actor,
					world->getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
				auto inspect = [&](std::shared_ptr<core::World> const& rendered) {
					auto scanner = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(rendered->getSector(index));
					WorldDrawList drawing({ { 0, 0 }, { 1600, 720 } }); renderWorld(rendered, &drawing);
					std::vector<WorldDrawList::Line> found;
					bool agentDrawn = false;
					for (auto const& command : drawing.commands())
					{
						if (auto text = std::get_if<WorldDrawList::Text>(&command))
							if (text->value == ICON_FA_MALE && text->colour == agentRenderColour(*rendered->lookupAgent(id).entity, false)) agentDrawn = true;
						if (auto line = std::get_if<WorldDrawList::Line>(&command))
							if (line->colour == IM_COL32(255, 0, 0, 128))
							{
								require(agentDrawn, "Scan beam painted before occupant");
								require(line->thickness == 4.0f, "Scan beam thickness changed");
								found.push_back(*line);
							}
					}
					if (scanner->getPhase() != core::SecurityScannerPhase::Scanning)
					{
						require(found.empty(), "Scan beams visible outside Scanning");
						return found;
					}
					require(found.size() == 2, "Scan did not draw both translucent red beams");
					core::Vector2 low, high; scanner->getBounds(low, high);
					float left = low.x * CORE_CELL_WIDTH_PIXELS, right = high.x * CORE_CELL_WIDTH_PIXELS;
					float ceiling = 720 - high.y * CORE_LEVEL_HEIGHT_PIXELS, floor = 720 - low.y * CORE_LEVEL_HEIGHT_PIXELS;
					float p = scanner->getScanProgress();
					float sweep = p <= 0.5f ? 2 * p : 2 * (1 - p);
					auto near = [](float a, float b) { return std::abs(a - b) < 0.001f; };
					require(near(found[0].from.x, left) && near(found[0].to.x, right)
						&& near(found[0].from.y, ceiling + sweep * (floor - ceiling))
						&& near(found[0].to.y, found[0].from.y), "Horizontal beam span/sweep incorrect");
					require(near(found[1].from.y, ceiling) && near(found[1].to.y, floor)
						&& near(found[1].from.x, left + sweep * (right - left))
						&& near(found[1].to.x, found[1].from.x), "Vertical beam span/sweep incorrect");
					return found;
				};
				std::set<std::string> phases;
				bool start = false, outbound = false, midpoint = false, returning = false, finish = false, completion = false;
				for (unsigned tick = 0; tick < 2400; ++tick)
				{
					inspect(world); phases.insert(chamber->getPhaseName());
					if (chamber->getPhase() == core::SecurityScannerPhase::Scanning)
					{
						float p = chamber->getScanProgress();
						start = start || p == 0;
						outbound = outbound || (p > 0.2f && p < 0.3f);
						returning = returning || (p > 0.7f && p < 0.8f);
						finish = finish || p > 0.99f;
						if (!midpoint && std::abs(p - 0.5f) < 0.001f)
						{
							midpoint = true;
							auto before = inspect(world); world->pauseSimulation(); world->advanceTicks(100);
							auto after = inspect(world);
							require(before.size() == after.size() && before[0].from.y == after[0].from.y
								&& before[1].from.x == after[1].from.x, "Paused beam positions moved");
							core::SerializationWorkData work; work.markSerializedUnmodified = false;
							auto output = core::YamlSerializer::toString(); world->serialize(*output, work); output->serialize();
							auto input = core::YamlSerializer::fromString(output->getSerializedString()); input->deserialize();
							auto loaded = std::make_shared<core::World>("Loaded", 1, 1);
							require(loaded->deserialize(*input, work), "Beam load fixture refused"); inspect(loaded);
							require(world->resumeSimulation(), "Beam pause resume refused");
						}
					}
					if (chamber->getPhase() == core::SecurityScannerPhase::PostPause)
					{ completion = chamber->getScanProgress() == 1; }
					if (completion && chamber->getPhase() == core::SecurityScannerPhase::Idle) break;
					world->advanceTick();
				}
				require(start && outbound && midpoint && returning && finish && completion && phases.size() == 11,
					"Beam journey omitted sweep samples or non-scanning phases");
				world->resetSimulation();
				chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
				require(world->resumeSimulation(), "Beam reset resume refused");
				for (unsigned tick = 0; tick < 2000 && chamber->getPhase() != core::SecurityScannerPhase::Scanning; ++tick) world->advanceTick();
				require(chamber->getPhase() == core::SecurityScannerPhase::Scanning, "Reset beam fixture did not scan");
				inspect(world); world->resetSimulation(); inspect(world);
			}
		ImGui::EndFrame();
	}

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
	checks.push_back({ "securityScanners/beamSweeps", isolated<beams> });
}
