#include "core/World.h"
#include "core/Agent.h"
#include "core/Marker.h"
#include "core/Log.h"
#include <algorithm>
#include <set>

namespace core
{
	namespace
	{
		bool configurationActionsAvailable(AgentBehaviourConfigurationValue const& value,
			ActionRegistry const* registry, std::string& missing)
		{
			if (auto action = agentBehaviourConfigurationGetIf<AgentBehaviourAction>(&value))
			{
				if (action->reference != IdleAction && (!registry || !registry->find(action->reference)))
				{
					missing = action->reference;
					return false;
				}
			}
			else if (auto list = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationList>(&value))
			{
				for (auto const& item : *list)
					if (!configurationActionsAvailable(item, registry, missing)) return false;
			}
			else if (auto record = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationRecord>(&value))
			{
				for (auto const& [name, item] : *record)
					if (!configurationActionsAvailable(item, registry, missing)) return false;
			}
			return true;
		}
	}

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
			std::string missing;
			for (auto const& [id, agent] : mAgents.entries())
				if (auto const& assignment = agent->getBehaviourAssignment())
					for (auto const& [field, value] : assignment->configuration)
						if (!configurationActionsAvailable(value, registry.get(), missing))
							return reject("Registry selection would invalidate a behaviour Action: " + missing);
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
		std::string missing;
		for (auto const& [id, agent] : mAgents.entries())
			if (auto const& assignment = agent->getBehaviourAssignment())
				for (auto const& [field, value] : assignment->configuration)
					if (!configurationActionsAvailable(value, nullptr, missing))
					{
						if (diagnostic) *diagnostic = "Action registry is referenced by behaviour configuration: " + missing;
						return false;
					}
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
			return;
		}
		// Shadow only host-owned values. No effects (including logging and
		// operation events) are published until the entire batch is accepted.
		auto pose = agent->mPose;
		auto claim = agent->mOccupiedUsablePoint;
		InteractionPointId device;
		auto reject = [&](char const* diagnostic)
		{
			event.type = SimulationEventType::ActionFailed;
			event.diagnostic = diagnostic; // ordinary request refusal, not a Lua failure
		};
		for (auto const& effect : result.effects)
		{
			switch (effect.type)
			{
			case ActionEffectType::Pose: pose = static_cast<Pose>(effect.value); break;
			case ActionEffectType::Claim:
				if (!isFurnitureMarker(markerId) || (claim && claim != markerId))
				{ reject("Selected Marker is not an eligible usable point"); return; }
				if (auto owner = usablePointOccupant(markerId); owner && owner != agentId)
				{ reject("Usable point is occupied by another Agent"); return; }
				claim = markerId;
				break;
			case ActionEffectType::Release:
				if (!isFurnitureMarker(markerId) || claim != markerId)
				{ reject("Only the owning Agent may release this usable point"); return; }
				claim = {};
				break;
			case ActionEffectType::Device:
			{
				InteractionPointId pointId{effect.point};
				auto point = mInteractionPoints.find(pointId);
				if ((device && device != pointId) || !interactionRequestEligible(pointId, agentId, true)
					|| !missingInteractionPermissions(*point, *agent).empty() || point->mBindings.empty())
				{ reject("Device Interaction point is not eligible or authorized"); return; }
				for (auto const& binding : point->mBindings)
					if (static_cast<int>(binding.command.type) != effect.value
						|| !missingLiftDestinationPermissions(binding.command, agentId).empty())
					{ reject("Typed device operation is not offered or authorized"); return; }
				for (auto const& [id, request] : mInteractionRequests.entries())
					if (request->mActor == agentId && request->mResult == InteractionResult::Pending
						&& request->mPoint != pointId)
					{ reject("Agent already has another device request"); return; }
				device = pointId;
				break;
			}
			}
		}
		// Committed requests retain the ordinary asynchronous device/traversal
		// authorities; scripts never submit unvalidated raw device commands.
		if (device) requestInteraction(device, agentId);
		if (claim != agent->mOccupiedUsablePoint)
		{
			if (claim) claimUsablePoint(agentId, claim);
			else agent->mOccupiedUsablePoint = {};
		}
		agent->mPose = pose;
		event.agent.pose = pose;
		invalidateSimulationSnapshot();
		for (auto const& log : result.logs) addLogMessage("Marker Action", 0, LogLevel::Info, log.message);
		if (result.logsSuppressed) addLogMessage("Marker Action", 0, LogLevel::Warning, "Action logging budget exhausted");
	}
}
