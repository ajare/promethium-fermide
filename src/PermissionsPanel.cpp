#include "PermissionsPanel.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include "imgui/imgui.h"
#include "core/AccessPermission.h"
#include "core/World.h"
#include "DocumentEdit.h"

using namespace std;
namespace
{
	array<char, core::AccessPermission::MaxNameBytes + 1> newName{};
	map<uint64_t, array<char, core::AccessPermission::MaxNameBytes + 1>> editedNames;
	core::AccessPermissionId pendingDelete{};

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

void resetPermissionsPanelState() { newName.fill(0); editedNames.clear(); pendingDelete = {}; }

void renderPermissionsPanel(shared_ptr<core::World> const& world)
{
	if (!world) return;
	ImGui::TextDisabled("Access permissions (%u/256)", world->getAccessPermissionCount());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::BeginTable("AccessPermissions", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Agents");
		ImGui::TableSetupColumn("Controls / Doors"); ImGui::TableSetupColumn("Delete"); ImGui::TableHeadersRow();
		for (auto id : world->getAccessPermissionIds())
		{
			ImGui::PushID((int)id.value); ImGui::TableNextRow(); ImGui::TableNextColumn();
			auto [entry, inserted] = editedNames.try_emplace(id.value);
			auto& name = entry->second;
			if (inserted) strncpy(name.data(), world->getAccessPermissionName(id).c_str(), name.size() - 1);
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
			ImGui::TableNextColumn(); ImGui::Text("%u / %u", usage.interactionPointRequirements,
				usage.manualDoorRequirements);
			ImGui::TableNextColumn(); if (ImGui::Button("Delete")) pendingDelete = id;
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
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
			ImGui::Text("This clears %u direct Agent grants, %u Interaction point requirements, and %u manual Door requirements.",
				usage.directAgentGrants, usage.interactionPointRequirements,
				usage.manualDoorRequirements);
			if (ImGui::Button("Delete")) { string diagnostic; auto deleted = pendingDelete; commitAccessPermissionDelete(world, pendingDelete, diagnostic); editedNames.erase(deleted.value); pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) { pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}
}

void renderAgentAccessPermissions(shared_ptr<core::World> const& world, core::AgentId agent)
{
	if (!world || world->getAccessPermissionCount() == 0) return;
	if (!ImGui::TreeNode("Access permissions")) return;
	auto direct = asSet(world->getAgentDirectAccessGrants(agent));
	auto effective = asSet(world->getAgentEffectiveAccessGrants(agent));
	ImGui::BeginDisabled(!world->isSimulationPaused());
	for (auto id : world->getAccessPermissionIds())
	{
		bool selected = direct.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
		{ string diagnostic; commitAgentAccessPermissionGrant(world, agent, id, selected, diagnostic); }
		if (effective.contains(id)) { ImGui::SameLine(); ImGui::TextDisabled("effective"); }
	}
	ImGui::EndDisabled(); ImGui::TreePop();
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
