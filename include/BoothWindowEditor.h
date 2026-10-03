#pragma once

#include "core/World.h"
#include <yaml-cpp/yaml.h>

struct BoothWindowClipboard
{
	core::Window::State initialState{core::Window::State::Closed};
};

YAML::Node makeBoothWindowClipboardObject(core::World const& world, core::WindowSectorObject const& object);
BoothWindowClipboard readBoothWindowClipboardObject(YAML::Node const& object);
std::shared_ptr<const core::SectorObject> pasteBoothWindow(std::shared_ptr<core::World> const& world,
	uint32_t layer, uint32_t y, uint32_t x, BoothWindowClipboard const& payload);
// Returns true when the authored state changed and the caller must refresh selection.
bool renderBoothWindowPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object);
