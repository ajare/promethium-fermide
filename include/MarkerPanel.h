#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "core/Simulation.h"

namespace core
{
	class World;
	class SectorObject;
}

bool commitActionRegistrySelection(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& path, std::string& diagnostic);
bool commitMarkerActionAssignment(std::shared_ptr<core::World> const& world,
	core::MarkerId marker, std::vector<std::string> actions, std::string& diagnostic);

// Extracted Marker editor used by the graphical editor and CPU-side ImGui
// verification. The optional reporter lets the application retain its normal
// error presentation without coupling this panel to UI.cpp globals.
using MarkerPanelErrorReporter = std::function<void(std::string const&)>;

void renderMarkerEditorPanel(
	std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object,
	MarkerPanelErrorReporter const& reportError = {});
