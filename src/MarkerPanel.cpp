#include "MarkerPanel.h"

#include <array>
#include <cstring>
#include <utility>

#include "DocumentEdit.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "imgui/imgui.h"

bool commitActionRegistrySelection(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& path, std::string& diagnostic)
{
	if (!world) { diagnostic = "No World selected"; return false; }
	// Mirror the Furniture catalogue workflow: a registry must sit beside the
	// saved World, because the document persists only its basename and resolves
	// it there on reopen. Relative references are read beside the World.
	auto const& directory = world->documentDirectory();
	if (directory.empty())
	{
		diagnostic = "Save the World before loading an Action registry";
		return false;
	}
	std::filesystem::path source = path;
	if (!source.is_absolute()) source = directory / source;
	std::error_code error;
	auto const canonicalDirectory = std::filesystem::weakly_canonical(directory, error);
	auto const canonicalSource = std::filesystem::weakly_canonical(source, error);
	if (error || canonicalSource.parent_path() != canonicalDirectory)
	{
		diagnostic = "Select a .actions.lua registry beside the World; the World references it by basename";
		return false;
	}
	auto undo = captureDocumentSnapshot(world);
	if (!undo) { diagnostic = "Cannot capture World history"; return false; }
	if (!world->selectActionRegistry(source, &diagnostic)) return false;
	commitDocumentEdit(std::move(undo));
	return true;
}

bool reloadSelectedActionRegistry(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& path, std::string& diagnostic)
{
	if (!world) { diagnostic = "No World selected"; return false; }
	return world->reloadActionRegistry(path, &diagnostic);
}

bool commitMarkerActionAssignment(std::shared_ptr<core::World> const& world,
	core::MarkerId marker, std::vector<std::string> actions, std::string& diagnostic)
{
	if (!world) { diagnostic = "No World selected"; return false; }
	auto undo = captureDocumentSnapshot(world);
	if (!undo) { diagnostic = "Cannot capture World history"; return false; }
	if (!world->setMarkerActions(marker, std::move(actions), &diagnostic)) return false;
	commitDocumentEdit(std::move(undo));
	return true;
}

void renderMarkerEditorPanel(
	std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object,
	MarkerPanelErrorReporter const& reportError)
{
	if (!world || !object
		|| object->getObjectType() != core::SectorObjectType::Marker)
	{
		ImGui::TextDisabled("No Marker selected.");
		return;
	}

	auto marker = std::static_pointer_cast<const core::MarkerSectorObject>(
		object)->getMarker();
	static core::MarkerId editingId{};
	static std::array<char, core::Marker::MaxNameBytes + 1> name{};
	if (editingId != marker->getId())
	{
		editingId = marker->getId();
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}

	ImGui::TextUnformatted("Marker");
	ImGui::SetNextItemWidth(-1.0f);
	bool const submitted = ImGui::InputText("Name", name.data(), name.size(),
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (submitted || ImGui::IsItemDeactivatedAfterEdit())
	{
		auto const trimmed = core::Marker::trimName(name.data());
		if (trimmed != marker->getName())
		{
			auto undo = captureDocumentSnapshot(world);
			std::string diagnostic;
			if (world->renameMarker(marker->getId(), name.data(), &diagnostic))
				commitDocumentEdit(std::move(undo));
			else if (reportError)
				reportError(diagnostic);
			else
				core::addLogMessage("Marker editor", 0,
					core::LogLevel::Warning, diagnostic);
		}
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Properties");
	bool blocksPathing = marker->hasProperty(core::MarkerProperty::BlocksPathing);
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::Checkbox("Blocks pathing", &blocksPathing))
	{
		auto undo = captureDocumentSnapshot(world);
		auto properties = marker->getProperties();
		auto const bit = core::markerPropertyBit(core::MarkerProperty::BlocksPathing);
		properties = blocksPathing ? properties | bit : properties & ~bit;
		std::string diagnostic;
		if (world->setMarkerProperties(marker->getId(), properties, &diagnostic))
			commitDocumentEdit(std::move(undo));
		else if (reportError)
			reportError(diagnostic);
		else
			core::addLogMessage("Marker editor", 0,
				core::LogLevel::Warning, diagnostic);
	}
	ImGui::EndDisabled();
	if (!world->isSimulationPaused()
		&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Pause the simulation to edit pathing properties");

	ImGui::SeparatorText("Agent Actions");
	ImGui::TextUnformatted("Idle (always available)");
	static std::array<char, 1024> registryPath{};
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::InputText("Registry (.actions.lua)", registryPath.data(), registryPath.size());
	auto report = [&](std::string const& diagnostic)
	{
		if (reportError) reportError(diagnostic);
		else core::addLogMessage("Marker editor", 0, core::LogLevel::Warning, diagnostic);
	};
	if (ImGui::Button("Load Action registry"))
	{
		std::string diagnostic;
		if (!commitActionRegistrySelection(world, registryPath.data(), diagnostic)) report(diagnostic);
	}
	if (world->actionRegistry())
	{
		ImGui::Text("Selected: %s", world->actionRegistryFilename().c_str());
		if (ImGui::Button("Reload Action registry"))
		{
			std::string diagnostic;
			if (!reloadSelectedActionRegistry(world, world->actionRegistry()->sourcePath(), diagnostic)) report(diagnostic);
		}
		if (ImGui::Button("Remove registry and assignments"))
		{
			auto undo = captureDocumentSnapshot(world);
			std::string diagnostic;
			if (undo && world->clearActionRegistry(&diagnostic)) commitDocumentEdit(std::move(undo));
			else report(diagnostic);
		}
		auto assigned = world->markerActions(marker->getId());
		for (size_t i = 0; i < assigned.size(); ++i)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::TextUnformatted(world->agentActionDisplayName(assigned[i]).c_str());
			ImGui::SameLine();
			bool changed = false;
			if (ImGui::SmallButton("Up") && i > 0) { std::swap(assigned[i], assigned[i - 1]); changed = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Down") && i + 1 < assigned.size()) { std::swap(assigned[i], assigned[i + 1]); changed = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove")) { assigned.erase(assigned.begin() + static_cast<ptrdiff_t>(i)); changed = true; }
			ImGui::PopID();
			if (changed)
			{
				std::string diagnostic;
				if (!commitMarkerActionAssignment(world, marker->getId(), assigned, diagnostic)) report(diagnostic);
				break;
			}
		}
		if (ImGui::BeginCombo("Add Action", "Select Action"))
		{
			for (auto const& definition : world->actionRegistry()->actions())
				if (ImGui::Selectable(definition.name.c_str()))
				{
					assigned.push_back(world->actionRegistry()->identity(definition));
					std::string diagnostic;
					if (!commitMarkerActionAssignment(world, marker->getId(), assigned, diagnostic)) report(diagnostic);
				}
			ImGui::EndCombo();
		}
	}
	ImGui::EndDisabled();

	auto position = marker->getPosition();
	position.x += marker->getOffset();
	ImGui::Text("Position: %.2f, %.2f",
		static_cast<double>(position.x), static_cast<double>(position.y));
}
