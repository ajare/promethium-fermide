#include "LocationPlan.h"
#include "core/SectorType.h"
#include <algorithm>
#include <string>

namespace
{
	bool ownsLocation(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::Sector> const& location)
	{
		return world && location && core::isLocationLike(location->getType())
			&& location->getIndex() < world->getNumSectors()
			&& world->getSector(location->getIndex()) == location;
	}
}

bool LocationPlan::open(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::Sector> const& location, uint32_t worldLevel)
{
	if (!ownsLocation(world, location)) return false;
	mWorld = world; mLocation = location;
	mWorldLevel = std::clamp(worldLevel, location->getCellY(),
		location->getCellY() + location->getLevelsHigh() - 1);
	mOpen = true; mFocus = true;
	return true;
}

void LocationPlan::close()
{
	mOpen = false; mFocus = false; mWorld.reset(); mLocation.reset();
}

std::shared_ptr<const core::Sector> LocationPlan::target(std::shared_ptr<core::World> const& world)
{
	auto location = mLocation.lock();
	if (!mOpen || mWorld.lock() != world || !ownsLocation(world, location))
	{
		close(); return {};
	}
	return location;
}

void LocationPlan::renderSelectionAction(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::Sector> const& selection, uint32_t worldLevel)
{
	if (ownsLocation(world, selection) && ImGui::Button("Location plan"))
		open(world, selection, worldLevel);
}

void LocationPlan::render(std::shared_ptr<core::World> const& world, Presenter const& present)
{
	auto location = target(world);
	if (!location) return;
	ImGui::SetNextWindowSize({600, 360}, ImGuiCond_FirstUseEver);
	if (mFocus) { ImGui::SetNextWindowFocus(); mFocus = false; }
	if (ImGui::Begin("Location plan", &mOpen))
	{
		ImGui::Text("Location: %s", location->getName().empty()
			? location->getDescription().c_str() : location->getName().c_str());
		ImGui::Text("Layer: %u", location->getLayerIndex());
		auto levelLabel = std::to_string(mWorldLevel);
		if (ImGui::BeginCombo("Level", levelLabel.c_str()))
		{
			for (uint32_t offset = 0; offset < location->getLevelsHigh(); ++offset)
			{
				auto level = location->getCellY() + offset;
				auto label = std::to_string(level);
				if (ImGui::Selectable(label.c_str(), level == mWorldLevel)) mWorldLevel = level;
				if (level == mWorldLevel) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::TextUnformatted("World X / Local depth (ordering)");
		auto position = ImGui::GetCursorScreenPos();
		auto size = ImGui::GetContentRegionAvail();
		if (size.x > 0 && size.y > 0)
		{
			ImGui::InvisibleButton("##LocationPlanViewport", size);
			WorldDrawList commands({position, {position.x + size.x, position.y + size.y}});
			renderLocationPlanGrid(commands, *location, position, size);
			present(commands, position, size);
		}
	}
	ImGui::End();
	if (!mOpen) close();
}
