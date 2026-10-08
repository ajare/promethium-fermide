#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "imgui/imgui.h"

class WorldDrawList;

// Creates the process-wide MPP and Willpower resource stack against the
// already-current OpenGL context, then loads Resources.yaml and the mandatory
// viewport ImageSets.
void createWorldRenderSystem(std::filesystem::path const& resourceDirectory,
	std::size_t drawableWidth, std::size_t drawableHeight);
void resizeWorldRenderSystem(std::size_t drawableWidth, std::size_t drawableHeight);
void destroyWorldRenderSystem();

// Renders one CPU command stream into the MPP-owned offscreen target and
// returns its OpenGL texture name for composition by ImGui.
enum class WorldCanvas { World, LocationPlan };
std::uint32_t renderWorldCommands(WorldDrawList const& commands,
	ImVec2 canvasPosition, ImVec2 canvasSize, WorldCanvas canvas = WorldCanvas::World);

// Application Resource inventory used by the World panel's catalogue
// selectors. Returns the declared Resource names of one type, alphabetically.
// `type` is "FurnitureCatalogue", "AgentTagRegistry", or
// "AgentBehaviourRegistry".
std::vector<std::string> applicationResourceNames(std::string const& type);

// One valid, loaded Agent type offered by the editor's creation selector.
struct ApplicationAgentType
{
	std::string resourceName;
	std::string typeId;
	std::string displayName;
};

// Every valid manifest-registered Agent type loaded at startup, ordered by
// display name (then resource name). Invalid resources are excluded; the
// bundled Human is always present.
std::vector<ApplicationAgentType> applicationAgentTypes();

// Returns the manifest source (file or package directory) for a named Resource
// of one type, or nullopt when the Resource is unknown.
std::optional<std::filesystem::path> applicationResourceSource(
	std::string const& type, std::string const& name);
