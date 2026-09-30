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
	array<char, core::PermissionSet::MaxNameBytes + 1> newSetName{};
	map<uint64_t, array<char, core::PermissionSet::MaxNameBytes + 1>> editedSetNames;
	core::PermissionSetId pendingSetDelete{};

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

void resetPermissionsPanelState()
{
	newName.fill(0); editedNames.clear(); pendingDelete = {};
	newSetName.fill(0); editedSetNames.clear(); pendingSetDelete = {};
}

void renderPermissionsPanel(shared_ptr<core::World> const& world)
{
	if (!world) return;
	ImGui::TextDisabled("Access permissions (%u/256)", world->getAccessPermissionCount());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::BeginTable("AccessPermissions", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Direct grants");
		ImGui::TableSetupColumn("Permission sets"); ImGui::TableSetupColumn("Controls / Doors");
		ImGui::TableSetupColumn("Delete"); ImGui::TableHeadersRow();
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
			ImGui::TableNextColumn(); ImGui::Text("%u", usage.permissionSetMemberships);
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
			ImGui::Text("This clears %u direct Agent grants, membership in %u Permission sets, %u Interaction point requirements, and %u manual Door requirements.",
				usage.directAgentGrants, usage.permissionSetMemberships,
				usage.interactionPointRequirements, usage.manualDoorRequirements);
			if (ImGui::Button("Delete")) { string diagnostic; auto deleted = pendingDelete; commitAccessPermissionDelete(world, pendingDelete, diagnostic); editedNames.erase(deleted.value); pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) { pendingDelete = {}; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	ImGui::Separator(); ImGui::TextDisabled("Permission sets (%u)", world->getPermissionSetCount());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	for (auto setId : world->getPermissionSetIds())
	{
		ImGui::PushID(static_cast<int>(setId.value));
		auto [entry, inserted] = editedSetNames.try_emplace(setId.value); auto& name = entry->second;
		if (inserted) strncpy(name.data(), world->getPermissionSetName(setId).c_str(), name.size() - 1);
		if (ImGui::InputText("##set-name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			string diagnostic;
			if (!commitPermissionSetRename(world, setId, name.data(), diagnostic))
			{ name.fill(0); strncpy(name.data(), world->getPermissionSetName(setId).c_str(), name.size() - 1); }
		}
		ImGui::SameLine(); ImGui::TextDisabled("%u Agent(s)", world->getPermissionSetUsageCount(setId));
		ImGui::SameLine(); if (ImGui::Button("Delete set")) pendingSetDelete = setId;
		auto members = asSet(world->getPermissionSetPermissions(setId));
		if (ImGui::TreeNode("Access permissions"))
		{
			for (auto permission : world->getAccessPermissionIds())
			{
				bool included = members.contains(permission);
				if (ImGui::Checkbox(world->getAccessPermissionName(permission).c_str(), &included))
				{ string diagnostic; commitPermissionSetMembership(world, setId, permission, included, diagnostic); }
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
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
	auto direct = asSet(world->getAgentDirectAccessGrants(agent));
	auto assignments = world->getAgentPermissionSetAssignments(agent);
	set<core::PermissionSetId> assigned(assignments.begin(), assignments.end());
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (world->getPermissionSetCount()) ImGui::TextDisabled("Permission sets");
	for (auto setId : world->getPermissionSetIds())
	{
		bool selected = assigned.contains(setId);
		if (ImGui::Checkbox(world->getPermissionSetName(setId).c_str(), &selected))
		{ string diagnostic; commitAgentPermissionSetAssignment(world, agent, setId, selected, diagnostic); }
	}
	if (world->getAccessPermissionCount()) ImGui::TextDisabled("Direct grants and effective sources");
	for (auto id : world->getAccessPermissionIds())
	{
		ImGui::PushID(static_cast<int>(id.value));
		bool selected = direct.contains(id);
		if (ImGui::Checkbox(world->getAccessPermissionName(id).c_str(), &selected))
		{ string diagnostic; commitAgentAccessPermissionGrant(world, agent, id, selected, diagnostic); }
		auto sources = world->getAgentAccessGrantSources(agent, id);
		if (sources.direct || !sources.permissionSets.empty())
		{
			string text = sources.direct ? "direct" : "";
			for (auto setId : sources.permissionSets)
			{ if (!text.empty()) text += ", "; text += world->getPermissionSetName(setId); }
			ImGui::SameLine(); ImGui::TextDisabled("effective: %s", text.c_str());
		}
		ImGui::PopID();
	}
	ImGui::EndDisabled(); ImGui::TreePop();
}

void renderAgentRuntimeProperties(shared_ptr<core::World> const& world, core::AgentId agent)
{
	if (!world || !ImGui::TreeNode("Runtime properties")) return;
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
