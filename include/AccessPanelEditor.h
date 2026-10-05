#pragma once
#include "core/World.h"

// Production document actions used by palette and Selection.
std::shared_ptr<const core::SectorObject> placeAccessPanel(std::shared_ptr<core::World> const& world,
	uint32_t sector, uint32_t levelOffset, uint32_t cellX, core::AccessPanelGeometry geometry,
	std::string& diagnostic);
bool editAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object, core::AccessPanelGeometry geometry,
	std::string& diagnostic);
bool deleteAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object);
struct AccessPanelAgentAction
{
	core::AccessPanelId panel;
	core::AccessPanel::Action action;
	std::string label;
	bool enabled;
};
std::vector<AccessPanelAgentAction> accessPanelAgentActions(core::World const& world, core::AgentId actor);
void renderAccessPanelAgentActions(std::shared_ptr<core::World> const& world, core::AgentId actor);

bool renderAccessPanelPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object);
