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
				if (action->reference != IdleAction && action->reference != UseFurnitureAction && (!registry || !registry->find(action->reference)))
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
		// Selection is not reload. Use the explicit paused reload transaction
		// to replace an already accepted executable snapshot.
		if (mActionRegistry && path.filename().string() == mActionRegistryFilename)
			return reject("Action registry already selected; use Reload Action registry");
		try
		{
			auto registry = ActionRegistry::load(path);
			for (auto const& [marker, actions] : mMarkerActions)
				if (lookupMarker(marker)) for (auto const& action : actions)
					if (action != UseFurnitureAction && !registry->find(action)) return reject("Registry selection would invalidate a Marker Action: " + action);
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

	bool World::reloadActionRegistry(std::filesystem::path const& path, std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = "Action reload: " + message; return false; };
		if (!isSimulationPaused()) return reject("Pause the simulation before reloading");
		if (!mActionRegistry || path.filename().string() != mActionRegistryFilename)
			return reject("Select this registry before reloading it");
		try
		{
			auto replacement = ActionRegistry::load(path);
			if (replacement->uuid() != mActionRegistry->uuid()) return reject("Registry UUID does not match the selected package");
			for (auto const& [marker, actions] : mMarkerActions)
				for (auto const& action : actions)
					if (action != UseFurnitureAction && !replacement->find(action))
						return reject("Marker " + std::to_string(marker.value) + " references missing Action " + action);
			std::string missing;
			for (auto const& [id, agent] : mAgents.entries())
				if (auto const& assignment = agent->getBehaviourAssignment())
					for (auto const& [field, value] : assignment->configuration)
						if (!configurationActionsAvailable(value, replacement.get(), missing))
							return reject("Agent " + agent->getName() + " field " + field + " references missing Action " + missing);
			mActionRegistry = std::move(replacement);
			for (auto& [id, goal] : mMovementGoals)
				if (goal.marker && !actionAvailable(goal.marker, goal.selectedAction))
				{ goal.actionInvalidated = true; goal.cancelling = true; }
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& error) { return reject(error.what()); }
	}

	bool World::reloadFurnitureCatalogue(std::filesystem::path const& path, std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = "Furniture reload: " + message; return false; };
		if (!FurnitureCatalogue::filenameIsValid(path.filename().string()))
			return reject("Furniture catalogue must end with .furniture.lua; YAML Furniture requires conversion to Lua");
		if (!isSimulationPaused()) return reject("Pause the simulation before reloading");
		if (!mFurnitureCatalogue || path.filename().string() != mFurnitureCatalogueFilename)
			return reject("Select this catalogue before reloading it");
		try
		{
			auto replacement = FurnitureCatalogue::load(path);
			if (replacement->uuid() != mFurnitureCatalogue->uuid()) return reject("Catalogue UUID does not match the selected package");
			auto candidate = makeCandidateWorld();
			candidate->mFurnitureCatalogue = replacement;
			candidate->mDeserializingConstruction = true;
			for (auto const& record : mConstructionRecords) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
			for (auto const& [marker, actions] : mMarkerActions)
				for (auto const& action : actions)
					if (!candidate->actionAvailable(marker, action))
						return reject("Marker " + std::to_string(marker.value) + " references unavailable Action " + action);
			// Every lifecycle in this catalogue is affected, even if geometry is unchanged.
			// Finish all users in stable Agent order, retaining the old package on error.
			bool finishingFailed = false;
			for (auto const& [id, agent] : mAgents.entries())
				if (agent->mFurnitureUse && agent->mFurnitureUse->catalogue == mFurnitureCatalogue)
				{
					auto outcomes = mPendingMovementOutcomes.size();
					finishFurnitureUse(id);
					finishingFailed |= mPendingMovementOutcomes.size() != outcomes;
				}
			if (finishingFailed) return reject("Old finish_use failed; users were safely cleaned up and the old catalogue remains installed (see Action diagnostics)");
			bool layoutChanged = false;
			for (auto const& instance : mFurniture)
				layoutChanged |= *mFurnitureCatalogue->definition(instance.definitionKey) != *replacement->definition(instance.definitionKey);
			auto old = mFurnitureCatalogue;
			auto goals = mMovementGoals;
			auto outcomes = mPendingMovementOutcomes;
			mFurnitureCatalogue = std::move(replacement);
			if (layoutChanged)
			{
				try { rebuildFromConstructionRecords(mConstructionRecords); }
				catch (...) { mFurnitureCatalogue = std::move(old); throw; }
				// Structural replay carries Agents, not Paths. Retain request identities
				// and replan against the new graph, never replacing their Action with Idle.
				mMovementGoals = std::move(goals);
				mPendingMovementOutcomes = std::move(outcomes);
				for (auto& [id, goal] : mMovementGoals)
				{
					goal.retainedPath.reset();
					if (auto agent = mAgents.find(id)) mSimulationCoordinator.beginRoutePlanning(*agent);
				}
			}
			invalidateSimulationSnapshot();
			for (auto& [id, goal] : mMovementGoals)
				if (goal.marker && !actionAvailable(goal.marker, goal.selectedAction))
				{ goal.actionInvalidated = true; goal.cancelling = true; }
			if (diagnostic) diagnostic->clear();
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
			if (agent->mResetAction != IdleAction && agent->mResetAction != UseFurnitureAction)
			{
				agent->mResetPath.reset(); agent->mResetDestinationMarker = {}; agent->mResetPathActive = false;
			}
		mActionRegistry.reset();
		mActionRegistryFilename.clear();
		for (auto& [id, goal] : mMovementGoals)
			if (goal.selectedAction != IdleAction && goal.selectedAction != UseFurnitureAction) { goal.actionInvalidated = true; goal.cancelling = true; }
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
			if (action == UseFurnitureAction)
			{
				if (!actionAvailable(marker, action)) return reject("Furniture definition has no use functions");
			}
			else if (!mActionRegistry || !mActionRegistry->find(action)) return reject("Unavailable Marker Action identity");
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
		if (action == UseFurnitureAction)
		{
			auto instance = furnitureForMarker(marker);
			auto definition = instance && mFurnitureCatalogue ? mFurnitureCatalogue->definition(instance->definitionKey) : nullptr;
			return definition && definition->hasUse;
		}
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
		auto agent = mAgents.find(agentId);
		if (auto occupant = usablePointOccupant(markerId); occupant && occupant != agentId)
		{
			event.type = SimulationEventType::ActionFailed;
			event.diagnostic = "Usable point is occupied by another Agent";
			return;
		}
		if (agent && agent->mFurnitureUse)
		{
			if (action == UseFurnitureAction && agent->mFurnitureUse->marker == markerId) return;
			finishFurnitureUse(agentId);
			event.agent = mSimulationCoordinator.makeAgentSnapshot(agent);
		}
		if (action == IdleAction) return;
		auto marker = lookupMarker(markerId);
		if (!agent || !marker || !actionAvailable(markerId, action))
		{
			event.type = SimulationEventType::MovementCancelled;
			event.movementCancellationReason = MovementCancellationReason::ActionUnavailable;
			event.diagnostic = "Selected Action is no longer available";
			return;
		}
		auto views = actionViews(agentId, markerId);
		auto instance = furnitureForMarker(markerId);
		auto result = action == UseFurnitureAction
			? mFurnitureCatalogue->executeUse(instance->definitionKey, false, views)
			: mActionRegistry->execute(action, views);
		applyActionResult(agentId, markerId, std::move(result), event);
		if (action == UseFurnitureAction && event.type == SimulationEventType::DestinationReached)
			agent->mFurnitureUse = Agent::FurnitureUse{markerId, instance->id, instance->definitionKey, mFurnitureCatalogue};
	}

	FurnitureInstance const* World::furnitureForMarker(MarkerId marker) const
	{
		for (auto const& instance : mFurniture)
			for (auto const& point : instance.destinations)
				if (point.marker == marker) return &instance;
		return nullptr;
	}

	ActionViews World::actionViews(AgentId agentId, MarkerId markerId) const
	{
		auto agent = mAgents.find(agentId);
		auto marker = lookupMarker(markerId);
		auto position = agent->getGlobalPosition();
		ActionViews views{agent->getName(), getName(), marker ? marker->getName() : "", agentId.value, markerId.value,
			getSimulationTick(), position.x, position.y, {}, {}, {},
			agent->mPose == Pose::Sitting ? "sitting" : agent->mPose == Pose::Lying ? "lying" : "standing", 0};
		if (auto instance = furnitureForMarker(markerId))
		{
			views.furnitureId = instance->id;
			views.furnitureName = instance->name;
			views.definitionKey = instance->definitionKey;
			for (auto const& point : instance->destinations)
				if (point.marker == markerId) views.usablePointKey = point.key;
		}
		return views;
	}

	void World::finishFurnitureUse(AgentId agentId)
	{
		auto agent = mAgents.find(agentId);
		if (!agent || !agent->mFurnitureUse) return;
		auto use = std::move(*agent->mFurnitureUse);
		agent->mFurnitureUse.reset();
		// Departure has already put the Agent into a navigation state, but the
		// finishing contract runs before the physical position commits, so mark the
		// batch while its callback and staged effects are validated.
		struct FinishingScope
		{
			AgentId& slot;
			AgentId restore;
			~FinishingScope() { slot = restore; }
		} finishingScope{ mFinishingFurnitureUseAgent, mFinishingFurnitureUseAgent };
		mFinishingFurnitureUseAgent = agentId;
		SimulationEvent event;
		event.agent = mSimulationCoordinator.makeAgentSnapshot(agent);
		event.destinationMarker = use.marker;
		event.selectedAction = UseFurnitureAction;
		event.type = SimulationEventType::DestinationReached;
		auto result = use.catalogue->executeUse(use.definition, true, actionViews(agentId, use.marker));
		applyActionResult(agentId, use.marker, std::move(result), event, true);
		// Safety is host-owned, independent of callback success and staged effects.
		agent->mPose = Pose::Standing;
		agent->mOccupiedUsablePoint = {};
		invalidateSimulationSnapshot();
		if (event.type == SimulationEventType::ActionFailed)
		{
			// An ordinary refusal stays ordinary. The host has already restored
			// Standing and released occupancy, and the incomplete-cleanup and Lua
			// failure paths own the pause/headless-failure policy themselves; only
			// those may escalate a failing finish_use into a script error.
			event.agent = mSimulationCoordinator.makeAgentSnapshot(agent);
			mPendingMovementOutcomes.push_back(std::move(event));
		}
	}

	void World::applyActionResult(AgentId agentId, MarkerId markerId, ActionExecutionResult result,
		SimulationEvent& event, bool finishing)
	{
		auto agent = mAgents.find(agentId);
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
		if (finishing && (pose != Pose::Standing || claim))
		{
			reject("Furniture finish_use omitted Standing/occupancy cleanup");
			event.scriptFailure = ScriptExecutionFailure::ConversionError;
			addLogMessage("Marker Action", 0, LogLevel::Error, event.diagnostic);
			mActionExecutionFailed = true;
			return; // incomplete finishing must not publish staged devices or logs
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
