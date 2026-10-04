#pragma once

#include "core/World.h"
#include <yaml-cpp/yaml.h>

struct BoothWindowClipboard
{
	core::Window::State initialState{core::Window::State::Closed};
	std::string authorizationWorldIdentity;
	std::vector<core::AccessPermissionId> permissions;
	std::vector<std::string> permissionNames;
};

struct DumbwaiterClipboard
{
	core::World::CreateDumbwaiterOptions options;
	std::array<BoothWindowClipboard, 2> landingPermissions;
};
YAML::Node makeDumbwaiterClipboardObject(core::World const& world, core::Dumbwaiter const& unit);
std::string makeDumbwaiterClipboardText(core::World const& world, core::Dumbwaiter const& unit, bool cut = false);
DumbwaiterClipboard readDumbwaiterClipboardObject(YAML::Node const& object);
core::World::CreateDumbwaiterOptions resolveDumbwaiterClipboardPermissions(core::World const& world,
	DumbwaiterClipboard const& payload);
core::DumbwaiterId pasteDumbwaiter(std::shared_ptr<core::World> const& world,
	uint32_t layer, uint32_t y, uint32_t x, DumbwaiterClipboard const& payload);

YAML::Node makeBoothWindowClipboardObject(core::World const& world, core::WindowSectorObject const& object);
BoothWindowClipboard readBoothWindowClipboardObject(YAML::Node const& object);
std::shared_ptr<const core::SectorObject> pasteBoothWindow(std::shared_ptr<core::World> const& world,
	uint32_t layer, uint32_t y, uint32_t x, BoothWindowClipboard const& payload);
std::vector<core::AccessPermissionId> resolveBoothWindowClipboardPermissions(core::World const& world,
	BoothWindowClipboard const& payload);
// Returns true when the authored state changed and the caller must refresh selection.
core::DeviceOperationId operateBoothWindowShutter(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object);

void renderDumbwaiterAgentActions(std::shared_ptr<core::World> const& world, core::AgentId actor);

bool renderDumbwaiterPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::Dumbwaiter> const& unit);

bool renderBoothWindowPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object);
