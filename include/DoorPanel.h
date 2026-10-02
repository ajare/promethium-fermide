#pragma once

// The Selection panel's Door branch (ticket #99). Lives in its own translation
// unit - rather than inside UI.cpp - so the headless smoke checks can compile
// the real panel and render it inside a CPU-side ImGui context to pin its
// disabled-scope balance down; UI.cpp's spdlog/nfd dependencies never link
// headlessly.

#include <memory>

namespace core
{
	class World;
	class Door;
	class SectorObject;
}

// Shared condition/status controls used by ordinary and Bulkhead Door Selection.
void renderDoorConditionPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<core::Door> const& door);

void renderDoorPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> object);
