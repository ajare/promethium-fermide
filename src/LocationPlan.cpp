#include "LocationPlan.h"
#include "core/SectorType.h"
#include <algorithm>
#include <string>
#include <cmath>
#include <limits>
#include "PaletteLayout.h"
#include "FurniturePanel.h"

namespace
{
	std::string placementName(core::World const& world, core::FurnitureDefinition const& definition)
	{
		size_t limit = core::Marker::MaxNameBytes;
		for (auto const& point : definition.usablePoints)
		{
			if (point.label.size() + 1 >= core::Marker::MaxNameBytes) return {};
			limit = std::min(limit, core::Marker::MaxNameBytes - point.label.size() - 1);
		}
		for (uint64_t number = 1;; ++number)
		{
			auto suffix = number == 1 ? std::string{} : " " + std::to_string(number);
			if (suffix.size() >= limit) return {};
			auto base = core::Marker::trimName(definition.label);
			// Truncate on a UTF-8 boundary and reserve space for owned Marker labels.
			if (base.size() > limit - suffix.size()) base.resize(limit - suffix.size());
			while (!base.empty() && !core::Marker::nameIsValid(base, nullptr)) base.pop_back();
			auto name = core::Marker::trimName(base) + suffix;
			bool taken = std::any_of(world.furniture().begin(), world.furniture().end(),
				[&](auto const& instance) { return instance.name == name; });
			for (auto id : world.getMarkerIds())
				for (auto const& point : definition.usablePoints)
					taken = taken || world.lookupMarker(id)->getName() == name + " " + point.label;
			if (!taken) return name;
		}
	}

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
	cancelDrag(); mDeleteDiagnostic.clear();
	mWorld = world; mLocation = location; mDepthRows = 4;
	mWorldLevel = std::clamp(worldLevel, location->getCellY(),
		location->getCellY() + location->getLevelsHigh() - 1);
	mOpen = true; mFocus = true;
	return true;
}

void LocationPlan::close()
{
	cancelDrag(); mDeleteDiagnostic.clear(); mPage = 0; mRowCatalogue.reset();
	mOpen = false; mFocus = false; mWorld.reset(); mLocation.reset();
}

std::shared_ptr<const core::Sector> LocationPlan::target(std::shared_ptr<core::World> const& world)
{
	auto location = mLocation;
	// History reconstructs Sectors in the same World. Rebind only an unchanged
	// authored Location at the same index; replacement Worlds and changed targets
	// close instead of silently inspecting an unrelated Sector.
	if (mOpen && world && mWorld.lock() == world && location
		&& !ownsLocation(world, location) && location->getIndex() < world->getNumSectors())
	{
		auto candidate = world->getSector(location->getIndex());
		if (candidate && candidate->getType() == location->getType()
			&& candidate->getName() == location->getName()
			&& candidate->getLayerIndex() == location->getLayerIndex()
			&& candidate->getCellX() == location->getCellX()
			&& candidate->getCellY() == location->getCellY()
			&& candidate->getCellsWide() == location->getCellsWide()
			&& candidate->getLevelsHigh() == location->getLevelsHigh())
		{
			cancelDrag();
			mLocation = location = candidate;
		}
	}
	if (!mOpen || mWorld.lock() != world || !ownsLocation(world, location))
	{
		close(); return {};
	}
	return location;
}

void LocationPlan::cancelDrag()
{
	mDragArmed = mDragging = false; mDragCatalogue.reset(); mDragKey.clear(); mMoveId = 0;
}

bool LocationPlan::isOpen(std::shared_ptr<core::World> const& world)
{
	return static_cast<bool>(target(world));
}

bool LocationPlan::renderPaletteRow(std::shared_ptr<core::World> const& world,
	WorldDrawList& commands, ImVec2 trayTopLeft, bool hovered)
{
	if (!isOpen(world)) return false;
	auto catalogue = world->furnitureCatalogue();
	if (mRowCatalogue.lock() != catalogue)
	{
		mPage = 0; mRowCatalogue = catalogue; cancelDrag();
	}
	auto const& io = ImGui::GetIO();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(1)) cancelDrag();
	if (mDragArmed && ImGui::IsMouseDragging(0)) mDragging = true;
	if (mDragging) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	auto count = catalogue ? catalogue->definitions().size() : 0;
	int capacity = count > static_cast<size_t>(paletteColumnCount()) ? paletteColumnCount() - 1 : paletteColumnCount();
	if (!count)
	{
		auto min = paletteFurnitureSlotMin(trayTopLeft, 0);
		commands.AddText(min, IM_COL32(180, 180, 190, 255),
			catalogue ? "No Furniture definitions" : "No Furniture catalogue");
	}
	size_t index = 0;
	if (catalogue) for (auto const& [key, definition] : catalogue->definitions())
	{
		if (index++ < mPage * capacity || index > (mPage + 1) * capacity) continue;
		auto min = paletteFurnitureSlotMin(trayTopLeft, static_cast<int>((index - 1) % capacity));
		ImVec2 max{min.x + PaletteSlotWidth, min.y + PaletteSlotSize};
		bool over = hovered && io.MousePos.x >= min.x && io.MousePos.x < max.x
			&& io.MousePos.y >= min.y && io.MousePos.y < max.y;
		commands.AddRect(min, max, over ? IM_COL32(251, 188, 4, 255) : IM_COL32(180, 180, 190, 180), 3);
		commands.PushClipRect(min, max, true);
		commands.AddText({min.x + 4, min.y + 10}, IM_COL32_WHITE, definition.label.c_str());
		commands.PopClipRect();
		if (over)
		{
			ImGui::SetTooltip("Drag %s into Location plan", definition.label.c_str());
			if (ImGui::IsMouseClicked(0))
			{
				mDragArmed = true; mDragging = false; mDragKey = key;
				mDragCatalogue = catalogue;
			}
		}
	}
	if (count > static_cast<size_t>(capacity))
	{
		auto min = paletteFurnitureSlotMin(trayTopLeft, capacity);
		ImVec2 max{min.x + PaletteSlotWidth, min.y + PaletteSlotSize};
		commands.AddRect(min, max, IM_COL32(180, 180, 190, 180), 3);
		commands.AddText({min.x + 4, min.y + 10}, IM_COL32_WHITE, "Next page");
		if (hovered && io.MousePos.x >= min.x && io.MousePos.x < max.x
			&& io.MousePos.y >= min.y && io.MousePos.y < max.y && ImGui::IsMouseClicked(0))
			mPage = (mPage + 1) % ((count + capacity - 1) / capacity);
	}
	auto row = paletteFurnitureSlotMin(trayTopLeft, 0);
	return mDragArmed || (hovered && io.MousePos.x >= row.x
		&& io.MousePos.x < trayTopLeft.x + paletteTraySize(true).x - PalettePadding
		&& io.MousePos.y >= row.y && io.MousePos.y < row.y + PaletteSlotSize);
}

void LocationPlan::renderSelectionAction(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::Sector> const& selection, uint32_t worldLevel)
{
	if (ownsLocation(world, selection) && ImGui::Button("Location plan"))
		open(world, selection, worldLevel);
}

void LocationPlan::render(std::shared_ptr<core::World> const& world, Presenter const& present,
	DocumentHistory& history, core::Agent const* selectedAgent)
{
	auto location = target(world);
	if (!location) return;
	mWorldLevel = std::clamp(mWorldLevel, location->getCellY(),
		location->getCellY() + location->getLevelsHigh() - 1);
	ImGui::SetNextWindowSize({600, 360}, ImGuiCond_FirstUseEver);
	if (mFocus) { ImGui::SetNextWindowFocus(); mFocus = false; }
	if (ImGui::Begin("Location plan", &mOpen))
	{
		ImGui::Text("Location: %s", location->getName().empty()
			? location->getDescription().c_str() : location->getName().c_str());
		ImGui::Text("Layer: %u", location->getLayerIndex());
		auto levelLabel = std::to_string(mWorldLevel);
		ImGui::SetNextItemWidth(256);
		if (ImGui::BeginCombo("Level", levelLabel.c_str()))
		{
			for (uint32_t offset = 0; offset < location->getLevelsHigh(); ++offset)
			{
				auto level = location->getCellY() + offset;
				auto label = std::to_string(level);
				if (ImGui::Selectable(label.c_str(), level == mWorldLevel)) { cancelDrag(); mWorldLevel = level; }
				if (level == mWorldLevel) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		mDepthRows = std::max(mDepthRows, locationPlanDepthRows(*world, *location, mWorldLevel));
		auto instance = selectedFurnitureInstance(world);
		bool canDeleteSelection = instance && instance->sector == location->getIndex()
			&& location->getCellY() + instance->y == mWorldLevel;
		ImGui::BeginDisabled(!canDeleteSelection);
		if (ImGui::Button("Delete selected Furniture") && canDeleteSelection)
		{
			if (deleteSelectedFurniture(world, instance->id, mDeleteDiagnostic, history))
			{
				selectFurnitureInstance(world, 0);
				cancelDrag();
				mDeleteDiagnostic.clear();
			}
		}
		ImGui::EndDisabled();
		if (!mDeleteDiagnostic.empty()) ImGui::TextWrapped("%s", mDeleteDiagnostic.c_str());
		ImGui::TextUnformatted("World X / Local depth (ordering) - edges cyan / selected route yellow");
		ImGui::TextUnformatted("Vertices: usable gold / external green / other cyan");
		auto position = ImGui::GetCursorScreenPos();
		auto size = ImGui::GetContentRegionAvail();
		if (size.x > 0 && size.y > 0)
		{
			ImGui::InvisibleButton("##LocationPlanViewport", size);
			WorldDrawList commands({position, {position.x + size.x, position.y + size.y}});
			auto selected = selectedFurnitureInstance(world);
			renderLocationPlanGrid(commands, *location, position, size, mDepthRows, world.get(), mWorldLevel,
				selected ? selected->id : 0, selectedAgent);
			auto catalogue = world->furnitureCatalogue();
			auto const& io = ImGui::GetIO();
			float left = position.x + 48, right = position.x + size.x - 12;
			float top = position.y + 8, bottom = position.y + size.y - 28;
			bool over = right > left && bottom > top
				&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
				&& io.MousePos.x >= left && io.MousePos.x < right
				&& io.MousePos.y >= top && io.MousePos.y < bottom;
			float mouseX = right > left ? (io.MousePos.x - left) * location->getCellsWide() / (right - left) : 0;
			int mouseDepth = bottom > top ? static_cast<int>(std::clamp(std::floor(
				double(bottom - io.MousePos.y) * mDepthRows / (bottom - top)), 0.0,
				double(std::numeric_limits<int>::max()))) : 0;
			if (!mDragging && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			{
				if (auto name = locationPlanVertexNameAtPosition(*world, *location, mWorldLevel,
					io.MousePos, position, size, mDepthRows))
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					ImGui::SetTooltip("%s", name->c_str());
				}
				else if (over && catalogue)
					for (auto it = world->furniture().rbegin(); it != world->furniture().rend(); ++it)
					{
						auto definition = catalogue->definition(it->definitionKey);
						if (definition && it->sector == location->getIndex()
							&& location->getCellY() + it->y == mWorldLevel && it->localDepth == mouseDepth
							&& mouseX >= it->x + definition->minX && mouseX < it->x + definition->maxX)
						{
							ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;
						}
					}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(1)) cancelDrag();
			if (over && !mDragArmed && ImGui::IsMouseClicked(0) && catalogue)
			{
				// Reverse draw order makes coincident footprints select the visible instance.
				for (auto it = world->furniture().rbegin(); it != world->furniture().rend(); ++it)
				{
					auto definition = catalogue->definition(it->definitionKey);
					if (!definition || it->sector != location->getIndex()
						|| location->getCellY() + it->y != mWorldLevel || it->localDepth != mouseDepth
						|| mouseX < it->x + definition->minX || mouseX >= it->x + definition->maxX) continue;
					selectFurnitureInstance(world, it->id);
					mMoveId = it->id; mMoveOriginal = *it;
					mMoveOffsetX = mouseX - it->x; mMoveOffsetDepth = mouseDepth - it->localDepth;
					mDragCatalogue = catalogue; break;
				}
			}
			if (mMoveId)
			{
				auto instance = selectedFurnitureInstance(world);
				if (!instance || instance->id != mMoveId || mDragCatalogue.lock() != catalogue
					|| instance->x != mMoveOriginal.x || instance->y != mMoveOriginal.y
					|| instance->localDepth != mMoveOriginal.localDepth || instance->name != mMoveOriginal.name)
					cancelDrag();
				else if (ImGui::IsMouseDragging(0) || mDragging)
				{
					mDragging = true;
					float x = mouseX - mMoveOffsetX;
					if (!io.KeyShift) x = std::round(location->getCellX() + x) - location->getCellX();
					int depth = std::max(0, mouseDepth - mMoveOffsetDepth);
					std::string diagnostic;
					bool valid = world->canEditFurniture(mMoveId, x, instance->y, instance->name, &diagnostic, depth);
					if (over)
					{
						renderLocationPlanPreview(commands, *location, *catalogue->definition(instance->definitionKey),
							x, depth, valid, position, size, mDepthRows);
						if (!valid) ImGui::SetTooltip("%s", diagnostic.c_str());
						if (ImGui::IsMouseReleased(0) && valid
							&& (x != instance->x || depth != instance->localDepth))
						{
							if (editSelectedFurniture(world, mMoveId, x, instance->y, false, instance->name, diagnostic, history, depth))
								selectFurnitureInstance(world, mMoveId);
							mDepthRows = std::max(mDepthRows, locationPlanDepthRows(*world, *location, mWorldLevel));
						}
					}
				}
			}
			if (mDragging && !mMoveId && mDragCatalogue.lock() == world->furnitureCatalogue())
			{
				auto catalogue = world->furnitureCatalogue();
				auto definition = catalogue ? catalogue->definition(mDragKey) : nullptr;
				auto mouse = ImGui::GetIO().MousePos;
				float left = position.x + 48, right = position.x + size.x - 12;
				float top = position.y + 8, bottom = position.y + size.y - 28;
				if (definition && right > left && bottom > top && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
					&& mouse.x >= left && mouse.x < right && mouse.y >= top && mouse.y < bottom)
				{
					// World X is snapped first; translate only after snapping. Shift
					// deliberately has no effect on new-definition drops.
					float x = std::round(location->getCellX()
						+ (mouse.x - left) * location->getCellsWide() / (right - left)) - location->getCellX();
					auto depthValue = std::floor(double(bottom - mouse.y) * mDepthRows / double(bottom - top));
					int depth = static_cast<int>(std::min(depthValue, double(std::numeric_limits<int>::max())));
					float y = static_cast<float>(mWorldLevel - location->getCellY());
					auto name = placementName(*world, *definition);
					std::string diagnostic;
					bool valid = world->canPlaceFurniture(location->getIndex(), mDragKey, x, y, name, &diagnostic, depth);
					renderLocationPlanPreview(commands, *location, *definition, x, depth, valid, position, size, mDepthRows);
					if (!valid) ImGui::SetTooltip("%s", diagnostic.c_str());
					if (ImGui::IsMouseReleased(0) && valid)
						if (placeSelectedFurniture(world, location->getIndex(), mDragKey, x, y, false,
							name, diagnostic, history, depth))
							mDepthRows = std::max(mDepthRows, locationPlanDepthRows(*world, *location, mWorldLevel));
				}
			}
			present(commands, position, size);
		}
	}
	ImGui::End();
	if ((mDragArmed || mMoveId) && (!ImGui::GetIO().MouseDown[0]
		|| mDragCatalogue.lock() != world->furnitureCatalogue())) cancelDrag();
	if (!mOpen) close();
}
