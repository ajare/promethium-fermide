#pragma once
#include "core/World.h"
#include "yaml-cpp/yaml.h"

// Authored-only payload: no identity, runtime state, graph or interaction handles.
YAML::Node makeAccessPanelClipboardObject(core::World const& world,
	std::shared_ptr<const core::SectorObject> const& object);
core::AccessPanelGeometry readAccessPanelClipboardObject(YAML::Node const& object);
std::shared_ptr<const core::SectorObject> pasteAccessPanel(std::shared_ptr<core::World> const& world,
	uint32_t sector, uint32_t levelOffset, uint32_t cellX, YAML::Node const& object, std::string& diagnostic);
std::shared_ptr<const core::SectorObject> moveAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object, uint32_t cellX, uint32_t cellY, std::string& diagnostic);

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
