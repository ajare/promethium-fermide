#include "PermissionsPanel.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include "imgui/imgui.h"
#include "imgui/IconsFontAwesome5.h"
#include "core/AccessPermission.h"
#include "core/Log.h"
#include "core/World.h"
#include "core/Sector.h"
#include "DocumentEdit.h"

using namespace std;
namespace
{
	array<char, core::AccessPermission::MaxNameBytes + 1> newName{};
	map<uint64_t, array<char, core::AccessPermission::MaxNameBytes + 1>> editedNames;
	core::AccessPermissionId pendingDelete{};
	array<char, core::PermissionSet::MaxNameBytes + 1> newSetName{};
	map<uint64_t, array<char, core::PermissionSet::MaxNameBytes + 1>> editedSetNames;
	core::PermissionSetId pendingSetDelete{};
	core::PermissionSetId selectedAssignment{};
	core::World const* assignmentWorld{ nullptr };
	core::AgentId assignmentAgent{};

	template<class Mutation> bool commit(shared_ptr<core::World> const& world,
		string& diagnostic, Mutation mutation)
	{
		diagnostic.clear();
		if (!world) { diagnostic = "There is no World to edit"; return false; }
		auto undo = captureDocumentSnapshot(world);
		if (!mutation()) return false;
		commitDocumentEdit(std::move(undo));
		return true;
	}

	set<core::AccessPermissionId> asSet(vector<core::AccessPermissionId> const& values)
	{ return { values.begin(), values.end() }; }
}

core::AccessPermissionId commitAccessPermissionAdd(shared_ptr<core::World> const& world,
	string const& name, string& diagnostic)
{
	core::AccessPermissionId result;
	commit(world, diagnostic, [&]
	{
		try { result = world->addAccessPermission(name); return true; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
	});
	return result;
}

bool commitAccessPermissionRename(shared_ptr<core::World> const& world,
	core::AccessPermissionId id, string const& name, string& diagnostic)
{
	diagnostic.clear();
	if (!world) { diagnostic = "There is no World to edit"; return false; }
	auto found = world->lookupAccessPermission(id);
	if (!found) { diagnostic = found.diagnostic; return false; }
	if (core::AccessPermission::trimName(name) == found.entity->getName()) return true;
	return commit(world, diagnostic,
		[&] { return world->renameAccessPermission(id, name, &diagnostic); });
}

bool commitAccessPermissionDelete(shared_ptr<core::World> const& world,
	core::AccessPermissionId id, string& diagnostic)
{ return commit(world, diagnostic, [&] { return world->deleteAccessPermission(id, &diagnostic); }); }

bool commitAgentAccessPermissionGrant(shared_ptr<core::World> const& world,
	core::AgentId agent, core::AccessPermissionId permission, bool granted, string& diagnostic)
{ return commit(world, diagnostic, [&] { return world->setAgentAccessPermissionGrant(agent, permission, granted, &diagnostic); }); }

core::PermissionSetId commitPermissionSetAdd(shared_ptr<core::World> const& world,
	string const& name, string& diagnostic)
{
	core::PermissionSetId result;
	commit(world, diagnostic, [&] { try { result = world->addPermissionSet(name); return true; }
		catch (exception const& error) { diagnostic = error.what(); return false; } });
	return result;
}

bool commitPermissionSetRename(shared_ptr<core::World> const& world,
	core::PermissionSetId id, string const& name, string& diagnostic)
{
	diagnostic.clear();
	if (!world) { diagnostic = "There is no World to edit"; return false; }
	auto found = world->lookupPermissionSet(id);
	if (!found) { diagnostic = found.diagnostic; return false; }
	if (core::PermissionSet::trimName(name) == found.entity->getName()) return true;
	return commit(world, diagnostic,
		[&] { return world->renamePermissionSet(id, name, &diagnostic); });
}
bool commitPermissionSetDelete(shared_ptr<core::World> const& world,
	core::PermissionSetId id, string& diagnostic)
{ return commit(world, diagnostic, [&] { return world->deletePermissionSet(id, &diagnostic); }); }
bool commitPermissionSetMembership(shared_ptr<core::World> const& world,
	core::PermissionSetId set, core::AccessPermissionId permission, bool included, string& diagnostic)
{ return commit(world, diagnostic, [&] { return world->setPermissionSetAccessPermission(set, permission, included, &diagnostic); }); }
bool commitAgentPermissionSetAssignment(shared_ptr<core::World> const& world,
	core::AgentId agent, core::PermissionSetId set, bool assigned, string& diagnostic)
{ return commit(world, diagnostic, [&] { return world->setAgentPermissionSetAssignment(agent, set, assigned, &diagnostic); }); }

bool commitInteractionPermissionRequirement(shared_ptr<core::World> const& world,
	core::InteractionPointId point, core::AccessPermissionId permission, bool required,
	string& diagnostic)
{
	return commit(world, diagnostic, [&]
	{
		auto values = world->getInteractionPointPermissionRequirement(point);
		auto found = find(values.begin(), values.end(), permission);
		if (required && found == values.end()) values.push_back(permission);
		else if (!required && found != values.end()) values.erase(found);
		else { diagnostic = required ? "The permission is already required" : "The permission is not required"; return false; }
		return world->setInteractionPointPermissionRequirement(point, values, &diagnostic);
	});
}

bool commitManualDoorPermissionRequirement(shared_ptr<core::World> const& world,
	core::TraversalResourceId door, core::AccessPermissionId permission, bool required,
	string& diagnostic)
{
	return commit(world, diagnostic, [&]
	{
		auto values = world->getManualDoorPermissionRequirement(door);
		auto found = find(values.begin(), values.end(), permission);
		if (required && found == values.end()) values.push_back(permission);
		else if (!required && found != values.end()) values.erase(found);
		else { diagnostic = required ? "The permission is already required" : "The permission is not required"; return false; }
		return world->setManualDoorPermissionRequirement(door, values, &diagnostic);
	});
}

bool commitLocationPermissionRequirement(shared_ptr<core::World> const& world,
	uint32_t sectorIndex, core::AccessPermissionId permission, bool required, string& diagnostic)
{
	return commit(world, diagnostic, [&]
	{
		if (!world->isLocationPermissionEligible(sectorIndex))
			return world->setLocationPermissionRequirement(sectorIndex, {}, &diagnostic);
		auto values = world->getLocationPermissionRequirement(sectorIndex);
		auto found = find(values.begin(), values.end(), permission);
		if (required && found == values.end()) values.push_back(permission);
		else if (!required && found != values.end()) values.erase(found);
		else return false;
		return world->setLocationPermissionRequirement(sectorIndex, values, &diagnostic);
	});
}

bool commitClearLocationPermissionRequirement(shared_ptr<core::World> const& world,
	uint32_t sectorIndex, string& diagnostic)
{
	return commit(world, diagnostic, [&]
	{
		if (!world->isLocationPermissionEligible(sectorIndex))
			return world->setLocationPermissionRequirement(sectorIndex, {}, &diagnostic);
		if (world->getLocationPermissionRequirement(sectorIndex).empty()) return false;
		return world->setLocationPermissionRequirement(sectorIndex, {}, &diagnostic);
	});
}

void renderLocationPermissionRequirements(shared_ptr<core::World> const& world, uint32_t sectorIndex)
{
	if (!world || !world->isLocationPermissionEligible(sectorIndex)) return;
	auto required = asSet(world->getLocationPermissionRequirement(sectorIndex));
	string summary;
	for (auto id : required)
	{
		if (!summary.empty()) summary += ", ";
		summary += world->getAccessPermissionName(id);
	}
	ImGui::TextUnformatted("Location permissions");
	ImGui::TextWrapped("Required (all): %s", summary.empty() ? "None" : summary.c_str());
	if (!ImGui::TreeNode("Required Access permissions")) return;
	ImGui::TextWrapped("Every listed permission is required to enter or route through this Location, from direct or Permission set grants. Control operation requirements remain independent.");
	ImGui::PushID("location-permissions");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	for (auto id : world->getAccessPermissionIds())
	{
		ImGui::PushID(static_cast<int>(id.value));
		bool selected = required.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
		{
			string diagnostic;
			if (!commitLocationPermissionRequirement(world, sectorIndex, id, selected, diagnostic)
				&& !diagnostic.empty()) core::addLogMessage("Location permissions", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::PopID();
	}
	ImGui::BeginDisabled(required.empty());
	if (ImGui::Button("Clear"))
	{
		string diagnostic;
		commitClearLocationPermissionRequirement(world, sectorIndex, diagnostic);
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (!world->getAccessPermissionCount()) ImGui::TextDisabled("No Access permissions defined");
	ImGui::PopID();
	ImGui::TreePop();
}

bool commitLiftDestinationPermissionRequirement(shared_ptr<core::World> const& world,
	uint32_t sectorIndex, uint32_t stopIndex, core::AccessPermissionId permission, bool required,
	string& diagnostic, uint32_t objectIndex)
{
	return commit(world, diagnostic, [&]
	{
		auto values = world->getLiftDestinationPermissionRequirement(sectorIndex, stopIndex, objectIndex);
		auto found = find(values.begin(), values.end(), permission);
		if (required && found == values.end()) values.push_back(permission);
		else if (!required && found != values.end()) values.erase(found);
		else return false;
		return world->setLiftDestinationPermissionRequirement(sectorIndex, stopIndex, values, &diagnostic, objectIndex);
	});
}

bool commitClearLiftDestinationPermissionRequirement(shared_ptr<core::World> const& world,
	uint32_t sectorIndex, uint32_t stopIndex, string& diagnostic, uint32_t objectIndex)
{
	return commit(world, diagnostic, [&]
	{
		if (world->getLiftDestinationPermissionRequirement(sectorIndex, stopIndex, objectIndex).empty())
		{
			diagnostic = "The destination permission requirement is already empty";
			return false;
		}
		return world->setLiftDestinationPermissionRequirement(sectorIndex, stopIndex, {}, &diagnostic, objectIndex);
	});
}

void renderLiftDestinationPermissions(shared_ptr<core::World> const& world, uint32_t sectorIndex, uint32_t objectIndex)
{
	if (!world) return;
	ImGui::TextUnformatted("Destination permissions");
	ImGui::TextWrapped("All listed Access permissions are required to select this destination, from direct or Permission set grants. Accepted shared journeys and disembarking remain available after permission loss.");
	auto levels = world->getLiftDestinationLevels(sectorIndex, objectIndex);
	auto shuttle = world->getSector(sectorIndex)->getType() == core::SectorType::Shuttle;
	if (!ImGui::BeginTable("Destination permissions", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("Stop / position"); ImGui::TableSetupColumn("Add / remove permissions");
	ImGui::TableSetupColumn("Required (all)"); ImGui::TableSetupColumn("Clear");
	ImGui::TableHeadersRow();
	for (uint32_t stop = 0; stop < levels.size(); ++stop)
	{
		ImGui::PushID(static_cast<int>(stop));
		auto required = asSet(world->getLiftDestinationPermissionRequirement(sectorIndex, stop, objectIndex));
		string summary;
		for (auto id : required)
		{
			if (!summary.empty()) summary += ", ";
			summary += world->getAccessPermissionName(id);
		}
		if (summary.empty()) summary = "None";
		ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Stop %u: %s %u", stop, shuttle ? "x" : "Level", levels[stop]);
		ImGui::TableNextColumn();
		ImGui::BeginDisabled(!world->isSimulationPaused());
		if (ImGui::BeginCombo("##permissions", summary.c_str()))
		{
			for (auto id : world->getAccessPermissionIds())
				if (ImGui::Selectable(world->getAccessPermissionName(id).c_str(), required.contains(id), ImGuiSelectableFlags_DontClosePopups))
				{
					string diagnostic;
					commitLiftDestinationPermissionRequirement(world, sectorIndex, stop, id, !required.contains(id), diagnostic, objectIndex);
				}
			if (!world->getAccessPermissionCount()) ImGui::TextDisabled("No Access permissions defined");
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", summary.c_str());
		ImGui::TableNextColumn();
		ImGui::BeginDisabled(!world->isSimulationPaused() || required.empty());
		if (ImGui::Button("Clear"))
		{
			string diagnostic;
			commitClearLiftDestinationPermissionRequirement(world, sectorIndex, stop, diagnostic, objectIndex);
		}
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void resetPermissionsPanelState()
{
	newName.fill(0); editedNames.clear(); pendingDelete = {};
	newSetName.fill(0); editedSetNames.clear(); pendingSetDelete = {};
	selectedAssignment = {}; assignmentWorld = nullptr; assignmentAgent = {};
}

void renderPermissionsPanel(shared_ptr<core::World> const& world)
{
	if (!world) return;
	ImGui::TextDisabled("Access permissions (%u/256)", world->getAccessPermissionCount());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::BeginTable("AccessPermissions", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Direct grants", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Permission sets", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Controls / Doors / Destinations / Locations", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthStretch, 1.0f); ImGui::TableHeadersRow();
		for (auto id : world->getAccessPermissionIds())
		{
			ImGui::PushID((int)id.value); ImGui::TableNextRow(); ImGui::TableNextColumn();
			auto [entry, inserted] = editedNames.try_emplace(id.value);
			auto& name = entry->second;
			if (inserted) strncpy(name.data(), world->getAccessPermissionName(id).c_str(), name.size() - 1);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputText("##name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				string diagnostic;
				if (!commitAccessPermissionRename(world, id, name.data(), diagnostic))
				{
					name.fill(0);
					strncpy(name.data(), world->getAccessPermissionName(id).c_str(), name.size() - 1);
				}
			}
			auto usage = world->getAccessPermissionUsage(id);
			ImGui::TableNextColumn(); ImGui::Text("%u", usage.directAgentGrants);
			ImGui::TableNextColumn(); ImGui::Text("%u", usage.permissionSetMemberships);
			ImGui::TableNextColumn(); ImGui::Text("%u / %u / %u / %u", usage.interactionPointRequirements,
				usage.manualDoorRequirements, usage.liftDestinationRequirements, usage.locationRequirements);
			ImGui::TableNextColumn(); if (ImGui::Button("Delete")) pendingDelete = id;
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::SetNextItemWidth(256.0f);
	ImGui::InputText("New permission", newName.data(), newName.size()); ImGui::SameLine();
	if (ImGui::Button("Add permission"))
	{ string diagnostic; if (commitAccessPermissionAdd(world, newName.data(), diagnostic)) newName.fill(0); }
	ImGui::EndDisabled();
	if (pendingDelete)
	{
		ImGui::OpenPopup("Delete Access permission?");
		if (ImGui::BeginPopupModal("Delete Access permission?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			auto usage = world->getAccessPermissionUsage(pendingDelete);
			ImGui::Text("Delete '%s'?", world->getAccessPermissionName(pendingDelete).c_str());
			ImGui::Text("This clears %u direct Agent grants, membership in %u Permission sets, %u Interaction point requirements, %u manual Door requirements, %u transport destination requirements, and %u Location requirements.",
				usage.directAgentGrants, usage.permissionSetMemberships,
				usage.interactionPointRequirements, usage.manualDoorRequirements, usage.liftDestinationRequirements, usage.locationRequirements);
			if (ImGui::Button("Delete")) { string diagnostic; auto deleted = pendingDelete; commitAccessPermissionDelete(world, pendingDelete, diagnostic); editedNames.erase(deleted.value); pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) { pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	ImGui::Separator(); ImGui::TextDisabled("Permission sets (%u)", world->getPermissionSetCount());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::BeginTable("PermissionSets", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Agents");
		ImGui::TableSetupColumn("Access permissions"); ImGui::TableSetupColumn("Delete");
		ImGui::TableHeadersRow();
		for (auto setId : world->getPermissionSetIds())
		{
			ImGui::PushID(static_cast<int>(setId.value));
			ImGui::TableNextRow(); ImGui::TableNextColumn();
			auto [entry, inserted] = editedSetNames.try_emplace(setId.value); auto& name = entry->second;
			if (inserted) strncpy(name.data(), world->getPermissionSetName(setId).c_str(), name.size() - 1);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputText("##set-name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				string diagnostic;
				if (!commitPermissionSetRename(world, setId, name.data(), diagnostic))
				{ name.fill(0); strncpy(name.data(), world->getPermissionSetName(setId).c_str(), name.size() - 1); }
			}
			ImGui::TableNextColumn(); ImGui::Text("%u", world->getPermissionSetUsageCount(setId));
			ImGui::TableNextColumn();
			auto members = asSet(world->getPermissionSetPermissions(setId));
			if (ImGui::TreeNode("Access permissions"))
			{
				for (auto permission : world->getAccessPermissionIds())
				{
					ImGui::PushID(static_cast<int>(permission.value));
					bool included = members.contains(permission);
					if (ImGui::Checkbox(world->getAccessPermissionName(permission).c_str(), &included))
					{ string diagnostic; commitPermissionSetMembership(world, setId, permission, included, diagnostic); }
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::TableNextColumn(); if (ImGui::Button("Delete set")) pendingSetDelete = setId;
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::SetNextItemWidth(256.0f);
	ImGui::InputText("New Permission set", newSetName.data(), newSetName.size()); ImGui::SameLine();
	if (ImGui::Button("Add Permission set"))
	{ string diagnostic; if (commitPermissionSetAdd(world, newSetName.data(), diagnostic)) newSetName.fill(0); }
	ImGui::EndDisabled();
	if (pendingSetDelete)
	{
		ImGui::OpenPopup("Delete Permission set?");
		if (ImGui::BeginPopupModal("Delete Permission set?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Delete '%s'?", world->getPermissionSetName(pendingSetDelete).c_str());
			ImGui::Text("This removes assignments from %u Agent(s). Access permissions are preserved.",
				world->getPermissionSetUsageCount(pendingSetDelete));
			if (ImGui::Button("Delete")) { string diagnostic; auto deleted = pendingSetDelete;
				commitPermissionSetDelete(world, deleted, diagnostic); editedSetNames.erase(deleted.value);
				pendingSetDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) { pendingSetDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}
}

void renderAgentAccessPermissions(shared_ptr<core::World> const& world, core::AgentId agent)
{
	if (!world || (world->getAccessPermissionCount() == 0 && world->getPermissionSetCount() == 0)) return;
	if (!ImGui::TreeNode("Access permissions")) return;
	if (assignmentWorld != world.get() || assignmentAgent != agent)
	{
		assignmentWorld = world.get(); assignmentAgent = agent; selectedAssignment = {};
	}
	auto assignments = world->getAgentPermissionSetAssignments(agent);
	set<core::PermissionSetId> assigned(assignments.begin(), assignments.end());
	if (!assigned.contains(selectedAssignment)) selectedAssignment = {};
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::PushID("assigned-permission-sets");
	ImGui::TextDisabled("Permission sets");
	bool firstChip = true;
	bool selectedChipFocused = false;
	for (auto setId : assignments)
	{
		auto label = "~" + world->getPermissionSetName(setId);
		auto width = ImGui::CalcTextSize(label.c_str()).x
			+ 2.0f * ImGui::GetStyle().FramePadding.x;
		if (!firstChip && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x
			+ width <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
			ImGui::SameLine();
		firstChip = false;
		ImGui::PushID(static_cast<int>(setId.value));
		bool selected = selectedAssignment == setId;
		if (selected)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Text));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
		}
		if (ImGui::SmallButton(label.c_str()))
			selectedAssignment = selected ? core::PermissionSetId{} : setId;
		if (selectedAssignment == setId && ImGui::IsItemFocused()) selectedChipFocused = true;
		if (selected) { ImGui::PopStyleVar(); ImGui::PopStyleColor(); }
		ImGui::PopID();
	}
	if (firstChip) ImGui::TextDisabled("No Permission sets assigned.");
	else ImGui::TextDisabled("Select a Permission set and press Delete to remove it.");
	if (world->isSimulationPaused() && selectedChipFocused
		&& ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		&& ImGui::IsKeyPressed(ImGuiKey_Delete))
	{
		string diagnostic;
		commitAgentPermissionSetAssignment(world, agent, selectedAssignment, false, diagnostic);
		selectedAssignment = {};
	}
	ImGui::SetNextItemWidth(256.0f);
	if (ImGui::BeginCombo("##add", "Add Permission set..."))
	{
		bool anyAddable = false;
		for (auto setId : world->getPermissionSetIds())
		{
			if (assigned.contains(setId)) continue;
			anyAddable = true;
			ImGui::PushID(static_cast<int>(setId.value));
			if (ImGui::Selectable(("~" + world->getPermissionSetName(setId)).c_str()))
			{ string diagnostic; commitAgentPermissionSetAssignment(world, agent, setId, true, diagnostic); }
			ImGui::PopID();
		}
		if (!anyAddable) ImGui::TextDisabled("No Permission sets available to add.");
		ImGui::EndCombo();
	}
	ImGui::PopID();

	ImGui::PushID("individual-permissions");
	ImGui::TextDisabled("Individual permissions");
	auto direct = asSet(world->getAgentDirectAccessGrants(agent));
	ImGui::SetNextItemWidth(256.0f);
	if (ImGui::BeginCombo("##add", "Permissions..."))
	{
		for (auto id : world->getAccessPermissionIds())
		{
			ImGui::PushID(static_cast<int>(id.value));
			bool granted = direct.contains(id);
			if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &granted))
			{ string diagnostic; commitAgentAccessPermissionGrant(world, agent, id, granted, diagnostic); }
			ImGui::PopID();
		}
		if (!world->getAccessPermissionCount()) ImGui::TextDisabled("No Access permissions defined.");
		ImGui::EndCombo();
	}
	for (auto id : world->getAgentDirectAccessGrants(agent))
	{
		ImGui::PushID(static_cast<int>(id.value));
		ImGui::TextUnformatted(world->getAccessPermissionName(id).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##remove"))
		{ string diagnostic; commitAgentAccessPermissionGrant(world, agent, id, false, diagnostic); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove individual permission");
		ImGui::PopID();
	}
	ImGui::PopID();
	ImGui::EndDisabled();

	ImGui::TextDisabled("Effective permissions");
	auto effective = world->getAgentEffectiveAccessGrants(agent);
	if (effective.empty()) ImGui::TextDisabled("No effective permissions.");
	for (auto id : effective)
	{
		auto sources = world->getAgentAccessGrantSources(agent, id);
		string text = sources.direct ? "individual" : "";
		for (auto setId : sources.permissionSets)
		{
			if (!text.empty()) text += ", ";
			text += "~" + world->getPermissionSetName(setId);
		}
		ImGui::TextUnformatted(world->getAccessPermissionName(id).c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", text.c_str());
	}
	ImGui::TreePop();
}

void renderAgentRuntimeProperties(shared_ptr<core::World> const& world, core::AgentId agent)
{
	if (!world) return;
	for (auto const& snapshot : world->getSimulationSnapshot().agents)
		if (snapshot.id == agent && snapshot.state == core::AgentPathState::RoutePlanning)
		{
			auto marker = world->lookupMarker(snapshot.intendedDestination);
			ImGui::TextUnformatted("State: Route planning");
			ImGui::Text("Destination: %s", marker ? marker->getName().c_str() : "<removed>");
			ImGui::Text("Planning time: %.2f seconds total, %.2f seconds remaining",
				snapshot.routePlanningTotalTicks * world->getFixedTimestep(),
				snapshot.routePlanningRemainingTicks * world->getFixedTimestep());
			break;
		}
	if (!ImGui::TreeNode("Runtime properties")) return;
	ImGui::TextWrapped("Changes affect the current simulation only and are restored from authored values by Reset simulation.");
	if (world->getAccessPermissionCount() == 0 && world->getPermissionSetCount() == 0)
	{
		ImGui::TextDisabled("No runtime properties are available.");
		ImGui::TreePop();
		return;
	}

	auto currentAssignments = world->getAgentCurrentPermissionSetAssignments(agent);
	set<core::PermissionSetId> assigned(currentAssignments.begin(), currentAssignments.end());
	if (world->getPermissionSetCount()) ImGui::TextDisabled("Current Permission sets");
	ImGui::PushID("runtime-permission-sets");
	for (auto setId : world->getPermissionSetIds())
	{
		ImGui::PushID(static_cast<int>(setId.value));
		bool selected = assigned.contains(setId);
		if (ImGui::Checkbox(world->getPermissionSetName(setId).c_str(), &selected))
			world->setAgentRuntimePermissionSetAssignment(agent, setId, selected);
		ImGui::PopID();
	}
	ImGui::PopID();

	auto currentDirect = asSet(world->getAgentCurrentDirectAccessGrants(agent));
	if (world->getAccessPermissionCount()) ImGui::TextDisabled("Current direct grants and effective sources");
	ImGui::PushID("runtime-direct-grants");
	for (auto id : world->getAccessPermissionIds())
	{
		ImGui::PushID(static_cast<int>(id.value));
		bool selected = currentDirect.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
			world->setAgentRuntimeAccessPermissionGrant(agent, id, selected);
		auto sources = world->getAgentAccessGrantSources(agent, id);
		if (sources.direct || !sources.permissionSets.empty())
		{
			string text = sources.direct ? "direct" : "";
			for (auto setId : sources.permissionSets)
			{
				if (!text.empty()) text += ", ";
				text += world->getPermissionSetName(setId);
			}
			ImGui::SameLine(); ImGui::TextDisabled("effective: %s", text.c_str());
		}
		ImGui::PopID();
	}
	ImGui::PopID();
	ImGui::TreePop();
}

void renderInteractionPermissionRequirements(shared_ptr<core::World> const& world,
	core::InteractionPointId point)
{
	if (!world || !world->isInteractionPointPermissionEligible(point)) return;
	if (!ImGui::TreeNode("Required Access permissions")) return;
	auto required = asSet(world->getInteractionPointPermissionRequirement(point));
	ImGui::BeginDisabled(!world->isSimulationPaused());
	for (auto id : world->getAccessPermissionIds())
	{
		bool selected = required.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
		{ string diagnostic; commitInteractionPermissionRequirement(world, point, id, selected, diagnostic); }
	}
	ImGui::EndDisabled();
	if (world->getAccessPermissionCount() == 0) ImGui::TextDisabled("No Access permissions defined");
	ImGui::TreePop();
}

void renderManualDoorPermissionRequirements(shared_ptr<core::World> const& world,
	core::TraversalResourceId door)
{
	if (!world || !world->isManualDoorPermissionEligible(door)) return;
	if (!ImGui::TreeNode("Required Access permissions")) return;
	auto required = asSet(world->getManualDoorPermissionRequirement(door));
	ImGui::BeginDisabled(!world->isSimulationPaused());
	for (auto id : world->getAccessPermissionIds())
	{
		bool selected = required.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
		{ string diagnostic; commitManualDoorPermissionRequirement(world, door, id, selected, diagnostic); }
	}
	ImGui::EndDisabled();
	if (world->getAccessPermissionCount() == 0) ImGui::TextDisabled("No Access permissions defined");
	ImGui::TreePop();
}
