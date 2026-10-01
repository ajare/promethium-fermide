#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/SerializationException.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Edge.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "imgui/imgui.h"
#include "imgui_internal.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	void runtimePropertiesPanelChangesCurrentAuthorizationOnly()
	{
		auto world = std::make_shared<core::World>("runtime properties panel", 2, 1);
		auto corridor = world->addCorridor(0, 0, 2);
		world->finishBuild(); world->pauseSimulation();
		auto agent = world->createAgent("agent", corridor, 0, 0.5f);
		auto permission = world->addAccessPermission("Runtime key");
		auto permissionSet = world->addPermissionSet("Runtime card");
		std::string diagnostic;
		require(world->setPermissionSetAccessPermission(permissionSet, permission,
			true, &diagnostic), diagnostic);
		world->markSaved();
		require(world->resumeSimulation(), "runtime properties fixture did not resume");

		require(world->setAgentRuntimeAccessPermissionGrant(agent, permission, true),
			"runtime direct grant was refused while simulation was running");
		require(world->setAgentRuntimePermissionSetAssignment(agent, permissionSet, true),
			"runtime Permission set assignment was refused while simulation was running");
		require(world->getAgentCurrentDirectAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ permission },
			"runtime direct grant was not visible as current Agent state");
		require(world->getAgentCurrentPermissionSetAssignments(agent)
			== std::vector<core::PermissionSetId>{ permissionSet },
			"runtime Permission set assignment was not visible as current Agent state");
		require(world->getAgentDirectAccessGrants(agent).empty()
			&& world->getAgentPermissionSetAssignments(agent).empty(),
			"runtime authorization rewrote the authored Agent configuration");
		require(!world->isModified(), "runtime authorization dirtied the World document");

		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		io.IniFilename = nullptr;
		ImGui::NewFrame(); ImGui::Begin("Runtime properties test");
		ImGui::SetNextItemOpen(true);
		renderAgentRuntimeProperties(world, agent);
		ImGui::End(); ImGui::Render();

		world->resetSimulation();
		require(world->getAgentCurrentDirectAccessGrants(agent).empty()
			&& world->getAgentCurrentPermissionSetAssignments(agent).empty(),
			"Reset did not restore authored Agent authorization");
		world->pauseSimulation();
		require(world->setAgentRuntimeAccessPermissionGrant(agent, permission, true)
			&& world->getAgentCurrentDirectAccessGrants(agent)
				== std::vector<core::AccessPermissionId>{ permission },
			"runtime direct grant was unavailable while simulation was paused");
		world->resetSimulation();
		require(world->getAgentCurrentDirectAccessGrants(agent).empty(),
			"Reset did not clear a paused runtime authorization change");
	}

	void panelCommitParticipatesInHistory()
	{
		auto world = std::make_shared<core::World>("panel", 2, 1);
		auto corridor = world->addRoom("Front", 0, 0, 0, 2, 1);
		world->addRoom("Back", 1, 0, 0, 2, 1);
		auto door = world->addSectorDoor(0, 0, 0, {});
		auto control = world->addSectorLightSwitch(corridor, 1);
		world->finishBuild(); world->pauseSimulation();
		auto agent = world->createAgent("agent", corridor, 0, 0.5f);
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		std::string diagnostic;
		auto id = commitAccessPermissionAdd(world, "Staff", diagnostic);
		require(id && gWorldDocumentHistory.canUndo() && world->isModified(),
			"Permissions panel commit did not create one document edit");
		auto permissionSet = commitPermissionSetAdd(world, "Operators", diagnostic);
		require(static_cast<bool>(permissionSet), "Permissions panel did not add a Permission set");
		require(commitPermissionSetMembership(world, permissionSet, id, true, diagnostic), diagnostic);
		require(commitAgentPermissionSetAssignment(world, agent, permissionSet, true, diagnostic), diagnostic);
		require(commitPermissionSetRename(world, permissionSet, "Operators renamed", diagnostic), diagnostic);
		auto sources = world->getAgentAccessGrantSources(agent, id);
		require(sources.permissionSets == std::vector<core::PermissionSetId>{ permissionSet },
			"selected Agent panel mutations did not expose the effective set source");

		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		io.IniFilename = nullptr;
		ImGui::NewFrame(); ImGui::Begin("Permissions test");
		renderPermissionsPanel(world);
		renderAgentAccessPermissions(world, agent);
		renderInteractionPermissionRequirements(world, control.interactionPoint);
		renderManualDoorPermissionRequirements(world, door.traversalResource);
		ImGui::End(); ImGui::Render();
	}
}

void permission_smoke::registerAccessEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runtimePropertiesPanelChangesCurrentAuthorizationOnly",
		[](smoke::Context const&)
		{
			EditorState state;
			runtimePropertiesPanelChangesCurrentAuthorizationOnly();
		} });
	checks.push_back({ "panelCommitParticipatesInHistory",
		[](smoke::Context const&)
		{
			EditorState state;
			panelCommitParticipatesInHistory();
		} });
}
