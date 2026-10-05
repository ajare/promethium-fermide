#include "core/World.h"
#include "core/Agent.h"
#include "core/Marker.h"
#include "core/Log.h"
#include <algorithm>
#include <set>

namespace core
{
	bool World::selectActionRegistry(std::filesystem::path const& path, std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!isSimulationPaused()) return reject("Pause the simulation to select an Action registry");
		// Selection is not reload. Same reference/source remains immutable until
		// the dedicated transactional reload slice supplies its lifecycle policy.
		if (mActionRegistry && path.filename().string() == mActionRegistryFilename)
			return reject("Action registry already selected; live reload is not supported");
		try
		{
			auto registry = ActionRegistry::load(path);
			for (auto const& [marker, actions] : mMarkerActions)
				if (lookupMarker(marker)) for (auto const& action : actions)
					if (!registry->find(action)) return reject("Registry selection would invalidate a Marker Action: " + action);
			mActionRegistry = std::move(registry);
			mActionRegistryFilename = path.filename().string();
			markModified();
			return true;
		}
		catch (std::exception const& error) { return reject(error.what()); }
	}

	bool World::clearActionRegistry(std::string* diagnostic)
	{
		if (!isSimulationPaused())
		{
			if (diagnostic) *diagnostic = "Pause the simulation to remove the Action registry";
			return false;
		}
		if (!mActionRegistry) return true;
		mMarkerActions.clear();
		for (auto const& [id, agent] : mAgents.entries())
			if (agent->mResetAction != IdleAction)
			{
				agent->mResetPath.reset(); agent->mResetDestinationMarker = {}; agent->mResetPathActive = false;
			}
		mActionRegistry.reset();
		mActionRegistryFilename.clear();
		for (auto& [id, goal] : mMovementGoals)
			if (goal.selectedAction != IdleAction) { goal.actionInvalidated = true; goal.cancelling = true; }
		markModified();
		invalidateSimulationSnapshot();
		return true;
	}

	std::vector<std::string> World::markerActions(MarkerId marker) const
	{
		if (!lookupMarker(marker)) return {};
		auto found = mMarkerActions.find(marker);
		return found == mMarkerActions.end() ? std::vector<std::string>{} : found->second;
	}

	bool World::setMarkerActions(MarkerId marker, std::vector<std::string> actions, std::string* diagnostic)
	{
		auto reject = [&](char const* message) { if (diagnostic) *diagnostic = message; return false; };
		if (!isSimulationPaused()) return reject("Pause the simulation to assign Marker Actions");
		if (!lookupMarker(marker)) return reject("Unknown Marker");
		std::vector<std::string> unique;
		std::set<std::string> seen;
		for (auto& action : actions)
		{
			if (action == IdleAction) continue; // universally derived, never redundant
			if (!mActionRegistry || !mActionRegistry->find(action)) return reject("Unavailable Marker Action identity");
			if (seen.insert(action).second) unique.push_back(std::move(action));
		}
		if (unique == markerActions(marker)) return true;
		if (unique.empty()) mMarkerActions.erase(marker);
		else mMarkerActions[marker] = std::move(unique);
		for (auto& [id, goal] : mMovementGoals)
			if (goal.marker == marker && !actionAvailable(marker, goal.selectedAction))
			{ goal.actionInvalidated = true; goal.cancelling = true; }
		for (auto const& [id, agent] : mAgents.entries())
			if (agent->mResetDestinationMarker == marker && !actionAvailable(marker, agent->mResetAction))
			{
				agent->mResetPath.reset(); agent->mResetDestinationMarker = {}; agent->mResetPathActive = false;
			}
		markModified();
		invalidateSimulationSnapshot();
		return true;
	}

	bool World::actionAvailable(MarkerId marker, std::string_view action) const
	{
		if (!lookupMarker(marker)) return false;
		if (action == IdleAction) return true;
		if (!mActionRegistry || !mActionRegistry->find(action)) return false;
		auto found = mMarkerActions.find(marker);
		return found != mMarkerActions.end()
			&& std::find(found->second.begin(), found->second.end(), action) != found->second.end();
	}

	std::string World::agentActionDisplayName(std::string_view action) const
	{
		if (action == IdleAction) return "Idle";
		if (action == UseFurnitureAction) return "Use furniture";
		if (mActionRegistry) if (auto definition = mActionRegistry->find(action)) return definition->name;
		return "Unavailable Action";
	}

	bool World::authorAgentMarkerRequest(AgentId id, MarkerId marker, std::string_view action, std::string* diagnostic)
	{
		if (!isSimulationPaused())
		{
			if (diagnostic) *diagnostic = "Pause the simulation to author a movement request";
			return false;
		}
		auto result = moveAgentToMarker(id, marker, action);
		if (!result.accepted())
		{
			if (diagnostic) *diagnostic = "Marker movement request refused (Action unavailable or Agent not ready)";
			return false;
		}
		auto agent = mAgents.find(id);
		agent->mResetPosition = agent->mPosition;
		agent->mResetLocalDepth = agent->mLocalDepth;
		agent->mResetDestinationMarker = marker;
		agent->mResetAction = action;
		agent->mResetPathActive = true;
		markModified();
		return true;
	}

	void World::executeMarkerAction(AgentId agentId, MarkerId markerId, std::string_view action, SimulationEvent& event)
	{
		if (action == IdleAction) return;
		auto agent = mAgents.find(agentId);
		auto marker = lookupMarker(markerId);
		if (!agent || !marker || !actionAvailable(markerId, action))
		{
			event.type = SimulationEventType::MovementCancelled;
			event.movementCancellationReason = MovementCancellationReason::ActionUnavailable;
			event.diagnostic = "Selected Action is no longer available";
			return;
		}
		auto position = agent->getGlobalPosition();
		ActionViews views{agent->getName(), getName(), marker->getName(), agentId.value, markerId.value,
			getSimulationTick(), position.x, position.y};
		auto result = mActionRegistry->execute(action, views);
		if (!result.succeeded)
		{
			event.type = SimulationEventType::ActionFailed;
			event.scriptFailure = result.failure;
			event.diagnostic = std::move(result.diagnostic);
			addLogMessage("Marker Action", 0, LogLevel::Error, event.diagnostic);
			mActionExecutionFailed = true;
		}
	}
}
