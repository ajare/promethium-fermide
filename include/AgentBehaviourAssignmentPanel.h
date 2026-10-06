#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/AgentBehaviour.h"
#include "core/EntityId.h"

namespace core { class World; class Agent; struct Path; }

// Public editor transaction seams: validation happens in World before the
// captured snapshot is committed, so every successful assignment/configuration
// replacement is one undoable edit and every refusal leaves history untouched.
bool commitAgentBehaviourAssignment(
	std::shared_ptr<core::World> const& world, core::AgentId agent,
	core::AgentBehaviourId behaviour, uint64_t revision,
	core::AgentBehaviourConfiguration const& configuration,
	std::string& diagnostic);
bool commitAgentBehaviourClear(std::shared_ptr<core::World> const& world,
	core::AgentId agent, std::string& diagnostic);

// Destination-specific picker. No request is issued until an Action is chosen;
// active becomes false on selection or dismissal. Idle remains an explicit choice.
std::optional<std::string> renderAgentMovementActionPopup(
	std::shared_ptr<const core::World> const& world, core::MarkerId marker,
	bool openRequested, bool& active);
bool commitAgentMarkerActionRequest(std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::MarkerId marker, std::string_view action, std::string& diagnostic);
// Issues one editor movement request to a Marker through the seam that matches
// the World's state: a paused World authors a reset-persistent document request
// (an undoable edit), while a running World issues a transient runtime request
// that changes no document state. Returns false with a diagnostic when refused.
bool requestAgentMarkerAction(std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::MarkerId marker, std::string_view action, std::string& diagnostic);
// Applies one editor path edit against the live World. When the edited path
// ends at a Marker and starts pathing, the destination is an Action request: a
// paused World authors a reset-persistent document request while a running
// World issues a transient runtime request. Returns false with a diagnostic
// when refused.
bool applyAgentPathEdit(std::shared_ptr<core::World> const& world, core::Agent* agent,
	std::shared_ptr<core::Path> path, bool startPathing, bool replaceCurrentPath,
	std::string_view action, std::string& diagnostic);

// Compact picker for the Agents table and full schema-generated editor for the
// selected-Agent panel. Marker values are always displayed and chosen by name.
void renderAgentBehaviourAssignmentCell(
	std::shared_ptr<core::World> const& world, core::AgentId agent);
void renderAgentBehaviourConfigurationPanel(
	std::shared_ptr<core::World> const& world, core::AgentId agent);
