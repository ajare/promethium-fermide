#pragma once

#include <memory>
#include <string>
#include "core/EntityId.h"

namespace core { class World; }

core::AccessPermissionId commitAccessPermissionAdd(std::shared_ptr<core::World> const& world,
	std::string const& name, std::string& diagnostic);
bool commitAccessPermissionRename(std::shared_ptr<core::World> const& world,
	core::AccessPermissionId id, std::string const& name, std::string& diagnostic);
bool commitAccessPermissionDelete(std::shared_ptr<core::World> const& world,
	core::AccessPermissionId id, std::string& diagnostic);
bool commitAgentAccessPermissionGrant(std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::AccessPermissionId permission, bool granted, std::string& diagnostic);
core::PermissionSetId commitPermissionSetAdd(std::shared_ptr<core::World> const& world,
	std::string const& name, std::string& diagnostic);
bool commitPermissionSetRename(std::shared_ptr<core::World> const& world,
	core::PermissionSetId id, std::string const& name, std::string& diagnostic);
bool commitPermissionSetDelete(std::shared_ptr<core::World> const& world,
	core::PermissionSetId id, std::string& diagnostic);
bool commitPermissionSetMembership(std::shared_ptr<core::World> const& world,
	core::PermissionSetId set, core::AccessPermissionId permission, bool included,
	std::string& diagnostic);
bool commitAgentPermissionSetAssignment(std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::PermissionSetId set, bool assigned, std::string& diagnostic);
bool commitInteractionPermissionRequirement(std::shared_ptr<core::World> const& world,
	core::InteractionPointId point, core::AccessPermissionId permission, bool required,
	std::string& diagnostic);
bool commitManualDoorPermissionRequirement(std::shared_ptr<core::World> const& world,
	core::TraversalResourceId door, core::AccessPermissionId permission, bool required,
	std::string& diagnostic);

void renderPermissionsPanel(std::shared_ptr<core::World> const& world);
void renderAgentAccessPermissions(std::shared_ptr<core::World> const& world, core::AgentId agent);
void renderAgentRuntimeProperties(std::shared_ptr<core::World> const& world, core::AgentId agent);
void renderInteractionPermissionRequirements(std::shared_ptr<core::World> const& world,
	core::InteractionPointId point);
void renderManualDoorPermissionRequirements(std::shared_ptr<core::World> const& world,
	core::TraversalResourceId door);
bool commitLiftDestinationPermissionRequirement(std::shared_ptr<core::World> const& world,
	uint32_t sectorIndex, uint32_t stopIndex, core::AccessPermissionId permission, bool required,
	std::string& diagnostic);
void renderLiftDestinationPermissions(std::shared_ptr<core::World> const& world, uint32_t sectorIndex);
void resetPermissionsPanelState();
