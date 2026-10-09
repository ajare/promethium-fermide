#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"
#include "core/Shuttle.h"
#include "core/Window.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/Coordination.h"
#include "core/ExtensibleObject.h"
#include "core/MobilityProfile.h"
#include "core/Sector.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	// Interaction and device-operation orchestration moved out of World
	// (ADR 0004 stage 2). The behaviour is unchanged: the coordinator works on
	// World's interaction and device-operation registries through friendship,
	// and calls back through the World facade for the machinery which has not
	// moved out of World yet - the structural-edit guard. The snapshots it
	// publishes are built by the coordinator's own snapshot seam, which joined
	// it in stage 5. The lift stop request it makes for an accepted lift call
	// goes to the coordinator's own lift scheduling helper.

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name)
	{
		mWorld.invalidateSimulationSnapshot();
		auto id = mWorld.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(name)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = makeInteractionPointSnapshot(id, *mWorld.mInteractionPoints.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!sector || sector.value > mWorld.mSectors.size())
		{
			throw invalid_argument("An interaction point requires a valid sector");
		}
		if (!std::isfinite(position.x) || !std::isfinite(position.y)
			|| !std::isfinite(reach) || reach < 0.0f
			|| !std::isfinite(durationSeconds) || durationSeconds < 0.0f
			|| bindings.empty())
		{
			throw invalid_argument("An interaction point requires a finite position, non-negative finite reach and duration, and at least one binding");
		}
		for (auto const& binding : bindings)
		{
			bool validTarget = false;
			if (binding.command.type == DeviceCommandType::SetBoothWindowState
				|| binding.command.type == DeviceCommandType::ToggleBoothWindow)
				{
				auto booth = mWorld.lookupBoothWindow(binding.command.boothWindow);
				validTarget = booth && !booth->getDumbwaiterOwner();
			}
			else if (binding.command.type == DeviceCommandType::SetAccessPanelState)
				validTarget = bool(mWorld.lookupAccessPanel(binding.command.accessPanel));
			else if (binding.command.type == DeviceCommandType::PressDumbwaiterLanding)
				validTarget = mWorld.lookupDumbwaiter(binding.command.dumbwaiter) && binding.command.stopIndex < 2;
			else if (binding.command.type == DeviceCommandType::SetSectorLights)
				validTarget = binding.command.target && binding.command.target.value <= mWorld.mSectors.size();
			else if (binding.command.type == DeviceCommandType::RequestAirlock)
				validTarget = binding.command.target && binding.command.target.value <= mWorld.mSectors.size()
					&& mWorld.mSectors[binding.command.target.value - 1]->getType() == SectorType::Airlock
					&& binding.command.stopIndex < 2;
			else if (auto resource = mWorld.mTraversalResources.find(binding.command.traversalResource))
				validTarget = binding.command.type == DeviceCommandType::OpenDoor ? resource->mDoor != nullptr
					: binding.command.type == DeviceCommandType::SetExtendedState ? resource->mExtensible != nullptr
					: (binding.command.type == DeviceCommandType::CallLift
						|| binding.command.type == DeviceCommandType::SelectLiftDestination
						|| binding.command.type == DeviceCommandType::CallShuttle
						|| binding.command.type == DeviceCommandType::SelectShuttleDestination)
						&& (resource->mLift || resource->mShuttle)
						&& binding.command.stopIndex < resource->mLiftStops.size();
			if (!validTarget)
			{
				throw invalid_argument("An interaction binding requires a valid command target");
			}
		}
		// A float-to-integer conversion outside the representable range is
		// undefined behavior, so a finite but enormous duration saturates rather
		// than overflowing the tick budget.
		double const rawTicks = ceil(static_cast<double>(durationSeconds)
			/ static_cast<double>(World::getFixedTimestep()));
		constexpr auto maxTicks = numeric_limits<uint64_t>::max();
		auto const durationTicks = rawTicks >= static_cast<double>(maxTicks)
			? maxTicks
			: max<uint64_t>(1, static_cast<uint64_t>(rawTicks));
		auto id = mWorld.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(
			name, sector, position, reach, durationTicks, std::move(bindings))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = makeInteractionPointSnapshot(id, *mWorld.mInteractionPoints.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionPoint> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id)
	{
		auto entity = mWorld.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint>{ entity, {} }
			: EntityLookup<InteractionPoint>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<InteractionPoint const> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id) const
	{
		auto entity = mWorld.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint const>{ entity, {} }
			: EntityLookup<InteractionPoint const>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult SimulationCoordinator::removeInteractionPoint(InteractionPointId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupInteractionPoint(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		if (found.entity->mBoothWindowOwner || found.entity->mDumbwaiterOwner || found.entity->mAccessPanelOwner)
			return { false, "Device controls are owned and cannot be removed independently" };
		if (any_of(found.entity->mBindings.begin(), found.entity->mBindings.end(), [](auto const& binding)
			{ return binding.command.type == DeviceCommandType::RequestAirlock; }))
			return { false, "Airlock controls are fixed and cannot be removed independently" };
		bool structural = any_of(mWorld.mTraversalResources.entries().begin(), mWorld.mTraversalResources.entries().end(),
			[id](auto const& entry)
			{
				auto const& resource = *entry.second;
				if (find(resource.mControls.begin(), resource.mControls.end(), id) != resource.mControls.end()) return true;
				if (resource.mLiftSelector == id) return true;
				return any_of(resource.mLiftStops.begin(), resource.mLiftStops.end(),
					[id](auto const& stop) { return stop.callControl == id; });
			});
		if (structural) mWorld.beginStructuralEdit("removeInteractionPoint");
		vector<InteractionRequestId> requests;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->getPoint() == id && request->getResult() == InteractionResult::Pending)
			{
				requests.push_back(requestId);
			}
		}
		for (auto requestId : requests)
		{
			cancelInteraction(requestId);
		}
		auto snapshot = makeInteractionPointSnapshot(id, *found.entity);
		mWorld.mInteractionPoints.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointRemoved;
		event.interactionPoint = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	DeviceOperationId SimulationCoordinator::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		auto missing = mWorld.missingLiftDestinationPermissions(command, requester);
		auto resource = mWorld.mTraversalResources.find(command.traversalResource);
		bool const broken = resource && ((command.type == DeviceCommandType::OpenDoor
			&& resource->mDoor && resource->mDoor->isBroken())
			|| (command.type == DeviceCommandType::SetExtendedState
				&& resource->mExtensible && resource->mExtensible->isBroken())
			|| ((command.type == DeviceCommandType::CallLift
				|| command.type == DeviceCommandType::SelectLiftDestination)
				&& resource->mLift && resource->mLift->isBroken())
			|| ((command.type == DeviceCommandType::CallShuttle
				|| command.type == DeviceCommandType::SelectShuttleDestination)
				&& resource->mShuttle && resource->mShuttle->isBroken()));
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			if (command.type != DeviceCommandType::SetAccessPanelState
				&& command.type != DeviceCommandType::ToggleBoothWindow
				&& command.type != DeviceCommandType::PressDumbwaiterLanding
				&& !broken && missing.empty() && operation->mHasCommand && operation->mCommand == command
				&& (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running))
			{
				operation->mRequesters.insert(requester);
				return id;
			}
		}
		auto name = command.type == DeviceCommandType::SetSectorLights
			? string("Set sector lights ") + (command.desiredState ? "on" : "off")
			: command.type == DeviceCommandType::OpenDoor ? "Open door"
			: command.type == DeviceCommandType::SetExtendedState
				? string(command.desiredState ? "Extend resource" : "Retract resource")
			: command.type == DeviceCommandType::CallLift ? "Call lift"
			: command.type == DeviceCommandType::SelectLiftDestination ? "Select lift destination"
			: command.type == DeviceCommandType::CallShuttle ? "Call shuttle"
			: command.type == DeviceCommandType::SelectShuttleDestination ? "Select shuttle destination"
				: command.type == DeviceCommandType::RequestAirlock ? "Request Airlock"
				: command.type == DeviceCommandType::ToggleBoothWindow ? "Toggle BoothWindow shutter"
				: command.type == DeviceCommandType::SetBoothWindowState ? "Set BoothWindow shutter target"
				: command.type == DeviceCommandType::PressDumbwaiterLanding ? "Press Dumbwaiter landing"
				: command.type == DeviceCommandType::SetAccessPanelState
					? (command.desiredState ? "Open Access panel" : "Close Access panel") : "Device command";
		if (!missing.empty())
		{
			name += ": missing Access permissions";
			for (auto permission : missing) name += format(" {}", permission.value);
		}
		auto id = mWorld.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester, command)));
		if (broken) mWorld.mDeviceOperations.find(id)->mState = DeviceOperationState::Failed;
		if (command.type == DeviceCommandType::PressDumbwaiterLanding)
		{
			auto unit = mWorld.lookupDumbwaiter(command.dumbwaiter);
			if (!unit || command.stopIndex > 1 || unit->isBusy())
				mWorld.mDeviceOperations.find(id)->mState = DeviceOperationState::Rejected;
		}
		if (!missing.empty())
		{
			auto operation = mWorld.mDeviceOperations.find(id);
			operation->mMissingPermissions = std::move(missing);
			operation->mState = DeviceOperationState::Rejected;
		}
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.phase = mWorld.mCurrentPhase;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mWorld.mDeviceOperations.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	bool World::agentCanOperateObjects(Agent const& actor) const
	{
		return actor.getObjectUsage() != ObjectUsage::None;
	}

	bool World::agentCanPhysicallyOperate(Agent const& actor, float distance, float geometryReach) const
	{
		return actor.getObjectUsage() == ObjectUsage::Arms
			&& std::isfinite(distance) && distance >= 0.f
			&& distance <= std::min(actor.getObjectUsageDistance(), geometryReach);
	}

	optional<Vector2> World::physicalButtonCentre(InteractionPointId pointId) const
	{
		auto placement = physicalControlPlacement(pointId);
		if (!placement) return nullopt;
		auto button = dynamic_pointer_cast<Button>(mSectors[placement->sectorIndex]->getObject(placement->objectIndex)->_getObject());
		if (!button) return nullopt;
		return button->getPosition() + button->getSize() * .5f;
	}

	optional<float> World::remoteButtonApproachDistance(InteractionPointId pointId, SectorId approach,
		Vector2 const& position, AgentId agentId) const
	{
		auto actor = mAgents.find(agentId);
		auto point = mInteractionPoints.find(pointId);
		auto centre = physicalButtonCentre(pointId);
		if (!actor || actor->getObjectUsage() != ObjectUsage::RemoteControl || !point || !centre
			|| point->mSector != approach) return nullopt;
		double range = actor->getObjectUsageDistance();
		double vertical = double(centre->y) - position.y;
		if (std::abs(vertical) > range) return nullopt;
		auto horizontal = std::sqrt(std::max(0., range * range - vertical * vertical));
		return float(std::max(0., std::abs(double(centre->x) - position.x) - horizontal));
	}

	bool World::agentCanOperateInteraction(InteractionPointId pointId, Agent const& actor, bool requireReach) const
	{
		auto point = mInteractionPoints.find(pointId);
		if (!point || !point->mSector || !actor.isActive() || agentForbidsButtons(&actor)
			|| actor.getSector() != mSectors[point->mSector.value - 1].get()) return false;
		if (actor.getObjectUsage() == ObjectUsage::RemoteControl)
		{
			auto centre = physicalButtonCentre(pointId);
			auto position = actor.getGlobalPosition();
			return centre && std::hypot(double(position.x) - centre->x, double(position.y) - centre->y) <= actor.getObjectUsageDistance();
		}
		return actor.getObjectUsage() == ObjectUsage::Arms
			&& (!(requireReach || point->requiresReachAtRequest())
				|| agentCanPhysicallyOperate(actor, actor.getGlobalPosition().distanceTo(point->mPosition), point->mReach));
	}

	bool World::interactionRequestEligible(InteractionPointId pointId, AgentId actorId, bool requireReach) const
	{
		auto point = mInteractionPoints.find(pointId);
		auto actor = mAgents.find(actorId);
		if (point && point->mAccessPanelOwner
			&& !canRequestAccessPanel(point->mAccessPanelOwner,
				point->mBindings.front().command.desiredState ? AccessPanel::Action::Open : AccessPanel::Action::Close, actorId)) return false;
		// A finishing callback runs before physical movement commits, so the Agent
		// is still standing at the usable point even though departure already set
		// its movement state. Judge that one staged device request from the
		// physical position rather than rejecting it merely because departure began.
		bool const finishing = actorId == mFinishingFurnitureUseAgent;
		return point && actor && agentCanOperateInteraction(pointId, *actor, requireReach)
			&& (actor->getObjectUsage() == ObjectUsage::RemoteControl || finishing
				|| actor->getState() == Agent::State::Idle || actor->getState() == Agent::State::WaitingForTraversal);
	}

	InteractionRequestId SimulationCoordinator::requestInteraction(InteractionPointId pointId, AgentId actorId)
	{
		mWorld.invalidateSimulationSnapshot();
		auto point = mWorld.mInteractionPoints.find(pointId);
		auto actor = mWorld.mAgents.find(actorId);
		// A deactivated Agent is not simulated (#118), so it cannot take on new
		// physical interaction work: the request is refused outright rather than
		// parked, so it can never claim a place in the point's queue (#192).
		// The effective Mobility profile's Buttons bit is a capability rather
		// than a route preference (ADR 0011), so a forbidden Agent is refused
		// here too (#193); agentForbidsButtons keeps individual-over-tag
		// precedence (ADR 0012) and never consults ordinary Door restrictions.
		if (!mWorld.interactionRequestEligible(pointId, actorId))
		{
			return {};
		}
		auto const missing = mWorld.missingInteractionPermissions(*point, *actor);
		if (!missing.empty())
		{
			auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
			auto request = mWorld.mInteractionRequests.find(id);
			request->mResult = InteractionResult::Rejected;
			request->mMissingPermissions = missing;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::InteractionRequestAdded;
			event.phase = mWorld.mCurrentPhase;
			event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
			mWorld.mEvents.push_back(std::move(event));
			return id;
		}
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
			{
				return request->mPoint == pointId ? id : InteractionRequestId{};
			}
		}
		auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
		auto request = mWorld.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			request->mOperations.emplace_back(findOrCreateDeviceOperation(binding.command, actorId), binding.requirement);
		}
		point->mQueue.push_back(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mWorld.mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionRequestId SimulationCoordinator::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = mWorld.mAgents.find(actor);
		// A route may need a Button beyond the current range. Approach only as
		// far as the inclusive range boundary, never the physical control point.
		if (agent && agent->getObjectUsage() == ObjectUsage::RemoteControl
			&& !mWorld.agentCanOperateInteraction(point, *agent, true))
		{
			auto control = mWorld.mInteractionPoints.find(point);
			auto centre = mWorld.physicalButtonCentre(point);
			if (!control || !centre || !control->mSector
				|| agent->getSector() != mWorld.mSectors[control->mSector.value - 1].get()
				|| agentForbidsButtons(agent) || !mWorld.missingInteractionPermissions(*control, *agent).empty()) return {};
			auto position = agent->getGlobalPosition();
			auto range = agent->getObjectUsageDistance();
			auto vertical = centre->y - position.y;
			if (std::abs(vertical) > range) return {};
			auto horizontal = std::sqrt(std::max(0., double(range) * range - double(vertical) * vertical));
			// Aim just inside to avoid float round-off repeatedly refusing equality.
			auto x = float(centre->x + (position.x < centre->x ? -1.f : 1.f) * horizontal * .999999f);
			agent->mRemoteButtonApproachTarget = Vector2{x, position.y};
			return {};
		}
		if (agent) agent->mRemoteButtonApproachTarget.reset();
		if (agent && agent->mEarlyDoorPressInteraction)
		{
			auto const earlyId = agent->mEarlyDoorPressInteraction;
			agent->mEarlyDoorPressInteraction = {};
			auto early = mWorld.mInteractionRequests.find(earlyId);
			if (early && early->mActor == actor && early->mPoint == point) return earlyId;
		}
		return requestInteraction(point, actor);
	}

	InteractionRequestId SimulationCoordinator::requestInteractionWhilePassing(
		InteractionPointId pointId, AgentId actorId)
	{
		mWorld.invalidateSimulationSnapshot();
		auto point = mWorld.mInteractionPoints.find(pointId);
		auto remoteActor = mWorld.mAgents.find(actorId);
		if (remoteActor && remoteActor->getObjectUsage() == ObjectUsage::RemoteControl)
			return requestInteraction(pointId, actorId);
		if (point && point->requiresReachAtRequest()) return requestInteraction(pointId, actorId);
		auto actor = mWorld.mAgents.find(actorId);
		// Even the press a moving Agent makes in passing is physical work, so a
		// deactivated Agent may not start one (#118, #192), and a
		// Buttons-forbidden Agent may not operate a control at all (#193).
		if (!point || !actor || !actor->isActive() || !mWorld.agentCanOperateObjects(*actor) || agentForbidsButtons(actor)
			|| !point->mSector
			|| actor->getSector() != mWorld.mSectors[(size_t)point->mSector.value - 1].get())
		{
			return {};
		}
		auto const missing = mWorld.missingInteractionPermissions(*point, *actor);
		if (!missing.empty())
		{
			auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
			auto request = mWorld.mInteractionRequests.find(id);
			request->mResult = InteractionResult::Rejected;
			request->mMissingPermissions = missing;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::InteractionRequestAdded;
			event.phase = mWorld.mCurrentPhase;
			event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
			mWorld.mEvents.push_back(std::move(event));
			return id;
		}
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			(void)id;
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
				return {};
		}

		auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(
			new InteractionRequest(pointId, actorId)));
		auto request = mWorld.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			auto operationId = findOrCreateDeviceOperation(binding.command, actorId);
			request->mOperations.emplace_back(operationId, binding.requirement);
			if (auto operation = mWorld.mDeviceOperations.find(operationId)) operation->mActivated = true;
		}
		pressPhysicalControl(pointId);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mWorld.mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionRequest const> SimulationCoordinator::lookupInteractionRequest(InteractionRequestId id) const
	{
		auto entity = mWorld.mInteractionRequests.find(id);
		return entity ? EntityLookup<InteractionRequest const>{ entity, {} }
			: EntityLookup<InteractionRequest const>{ nullptr, format("InteractionRequest handle {} is invalid", id.value) };
	}

	void SimulationCoordinator::detachInteractionRequester(InteractionRequest& request)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [operationId, requirement] : request.mOperations)
		{
			(void)requirement;
			if (auto operation = mWorld.mDeviceOperations.find(operationId))
			{
				operation->mRequesters.erase(request.mActor);
				if (!(operation->mActivated && operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
					&& operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
				{
					touchDeviceOperation(operationId, *operation);
					operation->mState = DeviceOperationState::Cancelled;
				}
			}
		}
	}

	void SimulationCoordinator::agentObjectUsageChanged(AgentId id)
	{
		auto actor = mWorld.mAgents.find(id);
		if (!actor) return;
		actor->mRemoteButtonApproachTarget.reset();
		std::vector<InteractionRequestId> cancelled;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor != id || request->mResult != InteractionResult::Pending) continue;
			auto point = mWorld.mInteractionPoints.find(request->mPoint);
			if (!point || !mWorld.agentCanOperateInteraction(request->mPoint, *actor, false))
				cancelled.push_back(requestId);
		}
		for (auto request : cancelled) cancelInteraction(request);
	}

	bool SimulationCoordinator::cancelInteraction(InteractionRequestId id)
	{
		auto request = mWorld.mInteractionRequests.find(id);
		if (!request || request->mResult != InteractionResult::Pending)
		{
			return false;
		}
		request->mResult = InteractionResult::Cancelled;
		detachInteractionRequester(*request);
		if (auto point = mWorld.mInteractionPoints.find(request->mPoint))
		{
			if (point->mActiveRequest == id)
			{
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), id), point->mQueue.end());
		}
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestChanged;
		event.phase = mWorld.mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
		if (auto point = mWorld.mInteractionPoints.find(request->mPoint))
			event.interactionName = point->getName();
		mWorld.mAgentBehaviourRuntime->observeOutcome(event);
		mWorld.mEvents.push_back(std::move(event));
		return true;
	}

	EntityRemovalResult SimulationCoordinator::removeInteractionRequest(InteractionRequestId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupInteractionRequest(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeInteractionRequestSnapshot(id, *found.entity);
		mWorld.mInteractionRequests.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestRemoved;
		event.interactionRequest = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	DeviceOperationId SimulationCoordinator::submitDeviceCommand(DeviceCommand const& command)
	{
		if (command.type == DeviceCommandType::PressDumbwaiterLanding)
		{
			auto unit = std::const_pointer_cast<Dumbwaiter>(mWorld.lookupDumbwaiter(command.dumbwaiter));
			if (!unit || command.stopIndex > 1) return {};
			mWorld.invalidateSimulationSnapshot();
			auto id = findOrCreateDeviceOperation(command, {});
			auto operation = mWorld.mDeviceOperations.find(id);
			admitDumbwaiterPress(id, *operation);
			// Publish the admission outcome immediately, including busy refusal.
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::DeviceOperationChanged;
			event.deviceOperation = makeDeviceOperationSnapshot(id, *operation);
			mWorld.mEvents.push_back(std::move(event));
			return id;
		}
		// Editor activation uses exactly the same operations as Interaction bindings.
		// No Agent, admission resource, or document/history mutation is involved.
		if ((command.type != DeviceCommandType::SetBoothWindowState
			&& command.type != DeviceCommandType::ToggleBoothWindow)
			|| !mWorld.lookupBoothWindow(command.boothWindow)) return {};
		if (mWorld.lookupBoothWindow(command.boothWindow)->getDumbwaiterOwner()) return {};
		mWorld.invalidateSimulationSnapshot();
		auto id = findOrCreateDeviceOperation(command, {});
		mWorld.mDeviceOperations.find(id)->mActivated = true;
		return id;
	}

	DeviceOperationId SimulationCoordinator::createDeviceOperation(string const& name, AgentId requester)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = lookupAgent(requester);
		if (!agent)
		{
			throw invalid_argument(format("Cannot create DeviceOperation: {}", agent.diagnostic));
		}

		auto id = mWorld.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mWorld.mDeviceOperations.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<DeviceOperation> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id)
	{
		auto entity = mWorld.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation>{ entity, {} }
			: EntityLookup<DeviceOperation>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<DeviceOperation const> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id) const
	{
		auto entity = mWorld.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation const>{ entity, {} }
			: EntityLookup<DeviceOperation const>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	bool SimulationCoordinator::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		auto operation = mWorld.mDeviceOperations.find(id);
		if (!operation || !operation->mRequesters.erase(requester))
		{
			return false;
		}
		vector<InteractionRequestId> affectedRequests;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor != requester || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			if (find_if(request->mOperations.begin(), request->mOperations.end(), [id](auto const& binding)
				{ return binding.first == id; }) != request->mOperations.end())
			{
				affectedRequests.push_back(requestId);
			}
		}
		for (auto requestId : affectedRequests)
		{
			cancelInteraction(requestId);
		}
		if (!(operation->mActivated && operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
			&& operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running))
		{
			touchDeviceOperation(id, *operation);
			operation->mState = DeviceOperationState::Cancelled;
		}
		return true;
	}

	EntityRemovalResult SimulationCoordinator::removeDeviceOperation(DeviceOperationId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupDeviceOperation(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeDeviceOperationSnapshot(id, *found.entity);
		mWorld.mDeviceOperations.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationRemoved;
		event.deviceOperation = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	void SimulationCoordinator::advanceDeviceOperations()
	{
		mWorld.invalidateSimulationSnapshot();
		// The physical safe-retract intent outlives its queryable operation, which
		// fails on breakage. Restoration may resume it only after leases drain.
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (!resource->mRetractionPending || !resource->mExtensible
				|| resource->mExtensible->isBroken()) continue;
			if (resource->mExtensionRequestLeases.empty() && resource->mExtensionOccupantLeases.empty())
			{
				resource->mExtensible->retract();
				if (resource->mExtensible->isRetracted()) resource->mRetractionPending = false;
			}
		}
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			(void)id;
			if (!operation->mHasCommand || !operation->mActivated)
			{
				continue;
			}
			touchDeviceOperation(id, *operation);
			if (operation->mState == DeviceOperationState::Pending)
			{
				operation->mState = DeviceOperationState::Running;
				if (operation->mCommand.type == DeviceCommandType::SetBoothWindowState
					|| operation->mCommand.type == DeviceCommandType::ToggleBoothWindow)
				{
					auto found = mWorld.mBoothWindows.find(operation->mCommand.boothWindow);
					auto booth = found == mWorld.mBoothWindows.end() ? nullptr : found->second.lock();
					if (!booth) operation->mState = DeviceOperationState::Rejected;
					else
					{
						// A new target supersedes prior travel, not its physical position.
						for (auto const& [otherId, other] : mWorld.mDeviceOperations.entries())
							if (otherId != id && other->mState == DeviceOperationState::Running
								&& (other->mCommand.type == DeviceCommandType::SetBoothWindowState
									|| other->mCommand.type == DeviceCommandType::ToggleBoothWindow)
								&& other->mCommand.boothWindow == operation->mCommand.boothWindow)
							{
								touchDeviceOperation(otherId, *other);
								other->mState = DeviceOperationState::Cancelled;
							}
						if (operation->mCommand.type == DeviceCommandType::ToggleBoothWindow)
							operation->mCommand.desiredState = !booth->mTargetOpen;
						booth->mTargetOpen = operation->mCommand.desiredState;
						booth->refreshState();
					}
				}
				else if (operation->mCommand.type == DeviceCommandType::OpenDoor)
				{
					auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
					if (!resource || !resource->mDoor || !resource->mEnabled
						|| !(operation->mCommand.desiredState
							? resource->mDoor->requestOpen() : resource->mDoor->requestClose()))
					{
						operation->mState = DeviceOperationState::Rejected;
					}
				}
				else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
				{
					auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
					if (!resource || !resource->mExtensible || !resource->mExtensible->isExtensible())
					{
						operation->mState = DeviceOperationState::Rejected;
					}
					else if (resource->mExtensible->isBroken())
					{
						operation->mState = DeviceOperationState::Failed;
					}
					else if (operation->mCommand.desiredState)
					{
						resource->mRetractionPending = false;
						resource->mEnabled = true;
						if (!resource->mExtensible->extend())
							operation->mState = DeviceOperationState::Rejected;
					}
					else
					{
						// Accepting a safe retract closes admission immediately. Physical
						// retraction starts only after every independently-owned lease drains.
						resource->mRetractionPending = true;
						resource->mEnabled = false;
					}
				}
				continue;
			}
			if (operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding) continue;
			if (operation->mState != DeviceOperationState::Running)
			{
				continue;
			}
			if (operation->mCommand.type == DeviceCommandType::SetBoothWindowState
				|| operation->mCommand.type == DeviceCommandType::ToggleBoothWindow)
			{
				if (!mWorld.lookupBoothWindow(operation->mCommand.boothWindow))
					operation->mState = DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetAccessPanelState)
			{
				if (!mWorld.lookupAccessPanel(operation->mCommand.accessPanel))
					operation->mState = DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetSectorLights
				&& operation->mCommand.target
				&& operation->mCommand.target.value <= mWorld.mSectors.size())
			{
				auto sector = mWorld.mSectors[(size_t)operation->mCommand.target.value - 1];
				bool succeeded = operation->mCommand.desiredState ? sector->lightsOn() : sector->lightsOff();
				operation->mState = succeeded ? DeviceOperationState::Succeeded : DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::RequestAirlock)
			{
				operation->mState = acceptAirlockCommand(operation->mCommand)
					? DeviceOperationState::Succeeded : DeviceOperationState::Rejected;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
			{
				auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mExtensible || resource->mExtensible->isBroken())
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (operation->mCommand.desiredState && resource->mExtensible->isExtended())
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
				else if (!operation->mCommand.desiredState)
				{
					if (resource->mExtensionRequestLeases.empty()
						&& resource->mExtensionOccupantLeases.empty())
					{
						if (!resource->mExtensible->isRetracted() && !resource->mExtensible->isRetracting())
							resource->mExtensible->retract();
						if (resource->mExtensible->isRetracted())
						{
							resource->mRetractionPending = false;
							operation->mState = DeviceOperationState::Succeeded;
						}
					}
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::CallLift
				|| operation->mCommand.type == DeviceCommandType::SelectLiftDestination
				|| operation->mCommand.type == DeviceCommandType::CallShuttle
				|| operation->mCommand.type == DeviceCommandType::SelectShuttleDestination)
			{
				auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || (!resource->mLift && !resource->mShuttle) || !resource->mEnabled
					|| (resource->mLift && resource->mLift->isBroken())
					|| (resource->mShuttle && resource->mShuttle->isBroken())
					|| operation->mCommand.stopIndex >= resource->mLiftStops.size())
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else
				{
					if (operation->mCommand.type == DeviceCommandType::CallLift
						|| operation->mCommand.type == DeviceCommandType::CallShuttle)
					{
						// The operation may be shared by several waiting passengers. Each
						// keeps independent ownership even though the physical call coalesces.
						for (auto requester : operation->mRequesters)
							addLiftStopRequest(*resource, operation->mCommand.stopIndex, requester);
						if (resource->mLiftStopPhase == LiftStopPhase::Idle)
							resource->mLiftStopPhase = LiftStopPhase::Closing;
					}
					else resource->mLiftDestinationStop = operation->mCommand.stopIndex;
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::OpenDoor)
			{
				auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mDoor || resource->mDoor->isBroken())
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (!resource->mEnabled)
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else if ((operation->mCommand.desiredState && resource->mDoor->isOpen())
					|| (!operation->mCommand.desiredState && resource->mDoor->isClosed()))
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else
			{
				operation->mState = DeviceOperationState::Failed;
			}
		}
		// Travel is owned by the device, not by the lifetime of an operation or
		// a traversal lease. This phase runs only on active simulation ticks.
		for (auto const& [deviceId, weak] : mWorld.mBoothWindows)
		{
			(void)deviceId;
			if (auto booth = weak.lock())
			{
				auto const step = World::getFixedTimestep() / BoothWindow::TravelSeconds;
				booth->mProgress = clamp(booth->mProgress + (booth->mTargetOpen ? step : -step), 0.0f, 1.0f);
				// Absorb floating point accumulation error at the final fixed tick.
				if (booth->mProgress < 1e-6f) booth->mProgress = 0.0f;
				if (booth->mProgress > 1.0f - 1e-6f) booth->mProgress = 1.0f;
				booth->refreshState();
			}
		}
		for (auto const& [panelId, weak] : mWorld.mAccessPanels)
		{
			(void)panelId;
			if (auto panel = weak.lock())
			{
				auto height = panel->getGeometry().height;
				double step = height > 0 ? double(panel->getSpeed()) * World::getFixedTimestep() / height : 1;
				panel->mProgress = float(clamp(double(panel->mProgress) + (panel->mOpen ? step : -step), 0.0, 1.0));
				if (!panel->mOpen && panel->mProgress < 1e-6f) panel->mProgress = 0;
				if (panel->mOpen && panel->mProgress > 1 - 1e-6f) panel->mProgress = 1;
			}
		}
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mState == DeviceOperationState::Running
				&& operation->mCommand.type == DeviceCommandType::SetAccessPanelState)
				if (auto panel = mWorld.lookupAccessPanel(operation->mCommand.accessPanel);
					panel && panel->getProgress() == (operation->mCommand.desiredState ? 1 : 0))
				{
					touchDeviceOperation(id, *operation);
					operation->mState = DeviceOperationState::Succeeded;
				}
		advanceDumbwaiters();
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mState == DeviceOperationState::Running
				&& (operation->mCommand.type == DeviceCommandType::SetBoothWindowState
					|| operation->mCommand.type == DeviceCommandType::ToggleBoothWindow))
				if (auto booth = mWorld.lookupBoothWindow(operation->mCommand.boothWindow);
					booth && (operation->mCommand.desiredState ? booth->getProgress() == 1.0f : booth->getProgress() == 0.0f))
				{
					touchDeviceOperation(id, *operation);
					operation->mState = DeviceOperationState::Succeeded;
				}
	}

	void SimulationCoordinator::admitDumbwaiterPress(DeviceOperationId id, DeviceOperation& operation)
	{
		touchDeviceOperation(id, operation);
		auto unit = std::const_pointer_cast<Dumbwaiter>(mWorld.lookupDumbwaiter(operation.mCommand.dumbwaiter));
		if (operation.mState != DeviceOperationState::Pending) return;
		if (!unit || operation.mCommand.stopIndex > 1 || unit->isBusy())
		{
			operation.mState = DeviceOperationState::Rejected;
			return;
		}
		// Shared Agent/user gate reserves the cycle at activation. Retire all
		// competing pending presses now, even if their point queue has not run.
		unit->mOperation = id;
		unit->mDestination = 1 - unit->mCurrentStop;
		unit->mTravelTicks = 0;
		unit->mPhase = DumbwaiterPhase::Closing;
		operation.mActivated = true;
		operation.mState = DeviceOperationState::Running;
		auto departure = std::const_pointer_cast<BoothWindow>(unit->mApertures[unit->mCurrentStop]);
		departure->mTargetOpen = false;
		departure->refreshState();
		for (auto const& [otherId, other] : mWorld.mDeviceOperations.entries())
			if (otherId != id && other->mState == DeviceOperationState::Pending
				&& other->mCommand.type == DeviceCommandType::PressDumbwaiterLanding
				&& other->mCommand.dumbwaiter == unit->getId())
			{
				touchDeviceOperation(otherId, *other);
				other->mState = DeviceOperationState::Rejected;
			}
	}

	void SimulationCoordinator::resetDumbwaiter(Dumbwaiter& unit)
	{
		mWorld.invalidateSimulationSnapshot();
		if (auto operation = mWorld.mDeviceOperations.find(unit.mOperation);
			operation && (operation->mState == DeviceOperationState::Running || operation->mState == DeviceOperationState::Pending))
		{
			operation->mState = DeviceOperationState::Cancelled;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::DeviceOperationChanged;
			event.deviceOperation = makeDeviceOperationSnapshot(unit.mOperation, *operation);
			mWorld.mEvents.push_back(std::move(event));
		}
		std::vector<InteractionRequestId> requests;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
			if (request->mResult == InteractionResult::Pending
				&& (request->mPoint == unit.getLandingButton(0) || request->mPoint == unit.getLandingButton(1)))
				requests.push_back(id);
		for (auto id : requests) cancelInteraction(id);
		unit.mOperation = {};
		unit.mPhase = DumbwaiterPhase::Idle;
		unit.mCurrentStop = unit.mInitialStop;
		unit.mCarOffset = float(unit.mInitialStop);
		unit.mTravelTicks = 0;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto booth = std::const_pointer_cast<BoothWindow>(unit.mApertures[stop]);
			booth->mTargetOpen = stop == unit.mInitialStop;
			booth->mProgress = booth->mTargetOpen ? 1.0f : 0.0f;
			booth->refreshState();
		}
	}

	void SimulationCoordinator::advanceDumbwaiters()
	{
		for (auto const& sector : mWorld.mSectors)
		{
			auto unit = std::dynamic_pointer_cast<Dumbwaiter>(sector);
			if (!unit || !unit->isBusy()) continue;
			switch (unit->mPhase)
			{
			case DumbwaiterPhase::Closing:
				if (unit->mApertures[unit->mCurrentStop]->getProgress() == 0)
					unit->mPhase = DumbwaiterPhase::Travelling;
				break;
			case DumbwaiterPhase::Travelling:
			{
				++unit->mTravelTicks;
				auto amount = std::min(1.0f, float(unit->mTravelTicks) * World::getFixedTimestep() / unit->mTravelSeconds);
				if (amount > 1.0f - 1e-6f) amount = 1;
				unit->mCarOffset = float(unit->mCurrentStop) + (float(unit->mDestination) - float(unit->mCurrentStop)) * amount;
				if (amount == 1)
				{
					unit->mCurrentStop = unit->mDestination;
					unit->mPhase = DumbwaiterPhase::Opening;
					auto arrival = std::const_pointer_cast<BoothWindow>(unit->mApertures[unit->mCurrentStop]);
					arrival->mTargetOpen = true;
					arrival->refreshState();
				}
				break;
			}
			case DumbwaiterPhase::Opening:
				if (unit->mApertures[unit->mCurrentStop]->getProgress() == 1)
				{
					if (auto operation = mWorld.mDeviceOperations.find(unit->mOperation))
					{
						touchDeviceOperation(unit->mOperation, *operation);
						operation->mState = DeviceOperationState::Succeeded;
					}
					unit->mOperation = {};
					unit->mPhase = DumbwaiterPhase::Idle;
				}
				break;
			default: break;
			}
		}
	}

	void SimulationCoordinator::pressPhysicalControl(InteractionPointId pointId)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& sector : mWorld.mSectors)
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				auto button = object
					? dynamic_pointer_cast<Button>(object->_getObject()) : nullptr;
				if (button && button->getInteractionPointId() == pointId)
				{
					button->disable();
					return;
				}
			}
		}
	}

	vector<InteractionPointId> SimulationCoordinator::upcomingRemoteButtons(Agent const& agent, TraversalResourceId& upcoming) const
	{
		// Authored Path intent, not proximity, identifies the operation. Stop at
		// the first action-bearing edge; never inspect another Sector's controls.
		auto const& path = agent.mPath.path;
		if (!path || !agent.getSector()) return {};
		shared_ptr<const Vertex> source;
		for (uint32_t i = agent.mPath.targetNode + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			source = path->nodes[i - 1].targetVertex;
			if (!node.edge || !node.targetVertex || !source || source->getSector().get() != agent.getSector()) return {};
			if (node.edge->getType() != EdgeType::Location)
			{
				upcoming = node.edge->getTraversalResourceId();
				break;
			}
			if (node.targetVertex->getSector().get() != agent.getSector()) return {};
		}
		auto resource = mWorld.mTraversalResources.find(upcoming);
		if (!source || !resource || !resource->mEnabled || agentForbidsButtons(&agent)) return {};
		auto controls = resource->mControls;
		if (resource->mOpenPlatformLift)
		{
			auto stop = findLiftStop(*resource, source->getPosition());
			if (stop < resource->mLiftStops.size()) controls.push_back(resource->mLiftStops[stop].callControl);
		}
		vector<InteractionPointId> result;
		auto sector = SectorId{uint64_t(agent.getSector()->getIndex()) + 1};
		for (auto pointId : controls)
		{
			auto point = mWorld.mInteractionPoints.find(pointId);
			if (!point || point->mSector != sector || !mWorld.physicalButtonCentre(pointId)
				|| !mWorld.missingInteractionPermissions(*point, agent).empty()) continue;
			bool needed = any_of(point->mBindings.begin(), point->mBindings.end(), [&](auto const& binding)
			{
				auto const& command = binding.command;
				if (command.type == DeviceCommandType::OpenDoor)
					return command.desiredState && resource->mDoor && !resource->mDoor->isOpen()
						&& command.traversalResource == upcoming;
				if (command.type == DeviceCommandType::SetExtendedState)
				{
					// Endpoint controls remain independent even within one Room.
					bool nearest = none_of(controls.begin(), controls.end(), [&](auto otherId)
					{
						auto other = mWorld.mInteractionPoints.find(otherId);
						return other && other->mSector == sector
							&& other->mPosition.distanceTo(source->getPosition()) + .001f < point->mPosition.distanceTo(source->getPosition());
					});
					return nearest && command.desiredState && resource->mExtensible && !resource->mExtensible->admitsNewTraversals()
						&& command.traversalResource == upcoming;
				}
				if (command.type == DeviceCommandType::RequestAirlock)
					return resource->mAirlock && command.traversalResource == upcoming;
				if (command.type == DeviceCommandType::CallLift || command.type == DeviceCommandType::CallShuttle)
					return bool(resource->mLiftCoordinator) || resource->mOpenPlatformLift;
				return false;
			});
			if (needed) result.push_back(pointId);
		}
		return result;
	}

	Vector2 SimulationCoordinator::limitRemoteButtonMovement(Agent const& agent, Vector2 const& start, Vector2 const& end) const
	{
		if (agent.getObjectUsage() != ObjectUsage::RemoteControl || agent.mEarlyDoorPressAttempted) return end;
		TraversalResourceId upcoming;
		auto controls = upcomingRemoteButtons(agent, upcoming);
		auto delta = end - start;
		double a = double(delta.x) * delta.x + double(delta.y) * delta.y;
		if (a == 0) return end;
		double limit = 1.;
		for (auto point : controls)
		{
			auto centre = *mWorld.physicalButtonCentre(point);
			double x = double(start.x) - centre.x, y = double(start.y) - centre.y;
			double range = agent.getObjectUsageDistance();
			double c = x * x + y * y - range * range;
			if (c <= 0) continue;
			double b = x * delta.x + y * delta.y;
			double discriminant = b * b - a * c;
			if (discriminant < 0) continue;
			auto enter = (-b - std::sqrt(discriminant)) / a;
			if (enter >= 0 && enter < limit)
			{
				// A horizontal tangent at exact vertical range has one eligible
				// point, not an interval into which an epsilon may be added.
				if (discriminant == 0 && delta.y == 0) return {centre.x, start.y};
				limit = std::min(1., enter + .00001);
			}
		}
		return start + delta * float(limit);
	}

	void SimulationCoordinator::tryPressUpcomingDoorButton(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		mWorld.invalidateSimulationSnapshot();
		auto const& path = agent.mPath.path;
		auto const target = agent.mPath.targetNode;
		if (!path || target + 1 >= path->nodes.size())
		{
			agent.mEarlyDoorPressResource = {};
			agent.mEarlyDoorPressInteraction = {};
			agent.mEarlyDoorPressAttempted = false;
			return;
		}

		if (agent.getObjectUsage() == ObjectUsage::RemoteControl)
		{
			TraversalResourceId upcoming;
			auto controls = upcomingRemoteButtons(agent, upcoming);
			if (agent.mEarlyDoorPressResource != upcoming)
			{
				agent.mEarlyDoorPressResource = upcoming;
				agent.mEarlyDoorPressInteraction = {};
				agent.mEarlyDoorPressAttempted = false;
			}
			if (agent.mEarlyDoorPressAttempted) return;
			auto actor = getAgentId(&agent);
			for (auto pointId : controls)
			{
				if (!mWorld.agentCanOperateInteraction(pointId, agent, true)) continue;
				if (auto request = requestInteraction(pointId, actor))
				{
					agent.mEarlyDoorPressInteraction = request;
					agent.mEarlyDoorPressAttempted = true;
					break;
				}
			}
			return;
		}

		// A Door Button contributes an Interactable vertex to the in-sector route.
		// Follow that route through its in-sector edges to find the next Door
		// threshold without looking beyond the current Sector.
		TraversalResourceId resourceId;
		auto currentSector = agent.getSector();
		for (uint32_t i = target + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			if (!node.edge || !node.targetVertex) break;
			if (node.edge->getType() == EdgeType::Door)
			{
				auto sourceVertex = path->nodes[i - 1].targetVertex;
				if (sourceVertex && sourceVertex->getSector().get() == currentSector)
					resourceId = node.edge->getTraversalResourceId();
				break;
			}
			if (node.targetVertex->getSector().get() != currentSector) break;
		}
		auto resource = mWorld.mTraversalResources.find(resourceId);
		// Enclosed Lift and Shuttle landing Doors deliberately use the Unavailable
		// Door activation mode: their transport coordinator, reached through the
		// landing call control, owns opening and scheduling instead.
		auto coordinator = resource
			? mWorld.mTraversalResources.find(resource->mLiftCoordinator) : nullptr;
		auto const liftDoor = coordinator && bool(coordinator->mLift);
		auto const shuttleDoor = coordinator && bool(coordinator->mShuttle);
		auto const regularDoor = !liftDoor && !shuttleDoor;
		if (!resource || !resource->mDoor
			|| (regularDoor
				&& resource->mDoorActivationMode != DoorActivationMode::RemoteControlled))
		{
			agent.mEarlyDoorPressResource = {};
			agent.mEarlyDoorPressInteraction = {};
			agent.mEarlyDoorPressAttempted = false;
			return;
		}
		if (agent.mEarlyDoorPressResource != resourceId)
		{
			agent.mEarlyDoorPressResource = resourceId;
			agent.mEarlyDoorPressInteraction = {};
			agent.mEarlyDoorPressAttempted = false;
		}
		if (agent.mEarlyDoorPressAttempted || !resource->mEnabled
			|| (regularDoor && (resource->mDoor->isOpen() || resource->mDoor->isOpening())))
		{
			return;
		}

		auto actorId = getAgentId(&agent);
		for (auto const& [id, interaction] : mWorld.mInteractionRequests.entries())
		{
			(void)id;
			if (interaction->mActor == actorId
				&& interaction->mResult == InteractionResult::Pending) return;
		}

		auto distanceToSegment = [&](Vector2 const& point)
		{
			auto delta = movementEnd - movementStart;
			auto lengthSquared = delta.x * delta.x + delta.y * delta.y;
			float amount = 0.0f;
			if (lengthSquared > 0.0f)
			{
				auto fromStart = point - movementStart;
				amount = clamp((fromStart.x * delta.x + fromStart.y * delta.y)
					/ lengthSquared, 0.0f, 1.0f);
			}
			return point.distanceTo(movementStart + delta * amount);
		};

		InteractionPointId selected;
		float selectedDistance = numeric_limits<float>::max();
		auto sourceSector = SectorId{ (uint64_t)agent.getSector()->getIndex() + 1 };
		for (auto pointId : resource->mControls)
		{
			auto point = mWorld.mInteractionPoints.find(pointId);
			if (!point || point->mSector != sourceSector) continue;
			bool preparesThreshold = any_of(point->mBindings.begin(), point->mBindings.end(),
				[&](InteractionBinding const& binding)
				{
					if (liftDoor) return binding.command.type == DeviceCommandType::CallLift;
					if (shuttleDoor) return binding.command.type == DeviceCommandType::CallShuttle;
					return binding.command.type == DeviceCommandType::OpenDoor
						&& binding.command.desiredState
						&& binding.command.traversalResource == resourceId;
				});
			if (!preparesThreshold) continue;
			auto distance = distanceToSegment(point->mPosition);
			if (!mWorld.agentCanPhysicallyOperate(agent, distance, point->mReach)) continue;
			if (!selected || distance < selectedDistance - 0.001f
				|| (abs(distance - selectedDistance) <= 0.001f && pointId < selected))
			{
				selected = pointId;
				selectedDistance = distance;
			}
		}
		if (!selected) return;

		if (auto interaction = requestInteractionWhilePassing(selected, actorId))
		{
			agent.mEarlyDoorPressInteraction = interaction;
			agent.mEarlyDoorPressAttempted = true;
		}
	}

	void SimulationCoordinator::allocateInteractions()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest)
			{
				auto active = mWorld.mInteractionRequests.find(point->mActiveRequest);
				if (active && active->mResult == InteractionResult::Pending)
				{
					continue;
				}
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			while (!point->mQueue.empty())
			{
				auto requestId = point->mQueue.front();
				auto request = mWorld.mInteractionRequests.find(requestId);
				if (!request || request->mResult != InteractionResult::Pending)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				// A request whose Agent was deactivated, or whose effective Mobility
				// profile now forbids Buttons, is cancelled here before allocation, so
				// it neither walks to the point nor holds the queue for the Agents
				// behind it (#118, #192, #193). A profile edited while paused must
				// not be overridden by earlier work, and cancellation must not resume
				// that work later.
				auto actor = mWorld.mAgents.find(request->mActor);
				if (!actor || !mWorld.agentCanOperateInteraction(pointId, *actor, false))
				{
					cancelInteraction(requestId);
					continue;
				}
				bool reusedActiveWork = false;
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mWorld.mDeviceOperations.find(operationId); operation && operation->mActivated)
					{
						reusedActiveWork = true;
						break;
					}
				}
				if (reusedActiveWork)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				point->mActiveRequest = requestId;
				point->mInteractionTicksRemaining = point->mDurationTicks;
				break;
			}
		}
	}

	void SimulationCoordinator::moveInteractions(float frameTime)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			auto request = mWorld.mInteractionRequests.find(point->mActiveRequest);
			if (!request || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			auto actor = mWorld.mAgents.find(request->mActor);
			if (actor)
			{
				auto missing = mWorld.missingInteractionPermissions(*point, *actor);
				if (!missing.empty())
				{
					auto requestId = point->mActiveRequest;
					request->mResult = InteractionResult::Rejected;
					request->mMissingPermissions = std::move(missing);
					detachInteractionRequester(*request);
					point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), requestId), point->mQueue.end());
					point->mActiveRequest = {};
					point->mInteractionTicksRemaining = 0;
					SimulationEvent event;
					event.sequence = mWorld.mNextEventSequence++;
					event.tick = mWorld.mSimulationTick;
					event.type = SimulationEventType::InteractionRequestChanged;
					event.phase = mWorld.mCurrentPhase;
					event.interactionRequest = makeInteractionRequestSnapshot(requestId, *request);
					event.interactionName = point->getName();
					mWorld.mAgentBehaviourRuntime->observeOutcome(event);
					mWorld.mEvents.push_back(std::move(event));
					mWorld.replanAgentAfterAuthorizationRefusal(request->mActor);
					continue;
				}
			}
			if (point->mAccessPanelOwner && !mWorld.canRequestAccessPanel(point->mAccessPanelOwner,
				point->mBindings.front().command.desiredState ? AccessPanel::Action::Open : AccessPanel::Action::Close,
				request->mActor))
			{
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			if (!actor || !mWorld.agentCanOperateInteraction(pointId, *actor, false))
			{
				// A deactivated Agent must not walk to the point or press it, and a
				// Buttons-forbidden Agent must not press it either; the point must
				// not stay reserved for either: cancel instead of suspending
				// (#118, #192, #193).
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			// Initial Route planning is stationary even if an earlier independent
			// interaction request is still waiting to be performed.
			if (actor->getState() == Agent::State::RoutePlanning) continue;
			if (actor->getObjectUsage() != ObjectUsage::RemoteControl
				&& !mWorld.agentCanPhysicallyOperate(*actor, actor->getGlobalPosition().distanceTo(point->mPosition), point->mReach))
			{
				actor->moveToPosition(point->mPosition, frameTime);
				continue;
			}
			if (point->mInteractionTicksRemaining > 0)
			{
				--point->mInteractionTicksRemaining;
			}
			if (point->mInteractionTicksRemaining == 0)
			{
				// Reflect the physical press in the rendered control. Auto-reenabling
				// Buttons return to their normal colour after the configured delay.
				pressPhysicalControl(pointId);
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mWorld.mDeviceOperations.find(operationId);
						operation && operation->mState == DeviceOperationState::Pending)
					{
						if (operation->mCommand.type == DeviceCommandType::SetAccessPanelState)
						{
							auto found = mWorld.mAccessPanels.find(operation->mCommand.accessPanel);
							auto panel = found == mWorld.mAccessPanels.end() ? nullptr : found->second.lock();
							touchDeviceOperation(operationId, *operation);
							operation->mActivated = true;
							auto action = operation->mCommand.desiredState ? AccessPanel::Action::Open : AccessPanel::Action::Close;
							auto control = panel ? mWorld.mInteractionPoints.find(panel->getControl(action)) : nullptr;
							bool eligible = panel && control && mWorld.canRequestAccessPanel(panel->getId(), action, request->mActor)
								&& mWorld.agentCanPhysicallyOperate(*actor, actor->getGlobalPosition().distanceTo(control->getPosition()), control->getReach());
							operation->mState = eligible ? DeviceOperationState::Running : DeviceOperationState::Rejected;
							if (eligible)
							{
								// Reversal keeps the physical leaf position, cancelling superseded travel.
								for (auto const& [otherId, other] : mWorld.mDeviceOperations.entries())
									if (otherId != operationId && other->mState == DeviceOperationState::Running
										&& other->mCommand.type == DeviceCommandType::SetAccessPanelState
										&& other->mCommand.accessPanel == panel->getId())
									{
										touchDeviceOperation(otherId, *other);
										other->mState = DeviceOperationState::Cancelled;
									}
								panel->mOpen = operation->mCommand.desiredState;
							}
						}
						else if (operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
							admitDumbwaiterPress(operationId, *operation);
						else operation->mActivated = true;
					}
				}
				point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), point->mActiveRequest), point->mQueue.end());
				point->mActiveRequest = {};
			}
		}
	}

	void SimulationCoordinator::updateInteractionResults()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			bool waiting = false;
			bool requiredFailure = false;
			bool requiredRejection = false;
			bool bestEffortFailure = false;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				auto operation = mWorld.mDeviceOperations.find(operationId);
				if (operation && operation->mState == DeviceOperationState::Rejected)
				{
					if (requirement == InteractionBindingRequirement::Required)
					{
						requiredRejection = true;
					}
					else
					{
						bestEffortFailure = true;
					}
				}
				else if (!operation || operation->mState == DeviceOperationState::Failed
					|| operation->mState == DeviceOperationState::Cancelled)
				{
					(requirement == InteractionBindingRequirement::Required ? requiredFailure : bestEffortFailure) = true;
				}
				else if (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running)
				{
					waiting = true;
				}
			}
			if (requiredRejection)
			{
				request->mResult = InteractionResult::Rejected;
			}
			else if (requiredFailure)
			{
				request->mResult = InteractionResult::Failed;
			}
			else if (!waiting)
			{
				request->mResult = bestEffortFailure ? InteractionResult::SucceededWithBestEffortFailure : InteractionResult::Succeeded;
			}
			if (request->mResult != InteractionResult::Pending)
			{
				SimulationEvent event;
				event.sequence = mWorld.mNextEventSequence++;
				event.tick = mWorld.mSimulationTick;
				event.type = SimulationEventType::InteractionRequestChanged;
				event.phase = mWorld.mCurrentPhase;
				event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
				if (auto point = mWorld.mInteractionPoints.find(request->mPoint))
					event.interactionName = point->getName();
				mWorld.mAgentBehaviourRuntime->observeOutcome(event);
				mWorld.mEvents.push_back(std::move(event));
			}
		}
	}

	void SimulationCoordinator::retireConsumedCoordination()
	{
		mWorld.invalidateSimulationSnapshot();

		// A terminal record is still somebody's unread outcome: the point that
		// queued the request, the traversal resource waiting on a shared
		// preparation, or the Agent that pressed a control while passing. Only
		// when no live owner names it may the record go.
		std::set<InteractionRequestId> referencedRequests;
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest) referencedRequests.insert(point->mActiveRequest);
			for (auto requestId : point->mQueue)
			{
				if (requestId) referencedRequests.insert(requestId);
			}
		}
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mActivePreparation) referencedRequests.insert(resource->mActivePreparation);
		}
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (agent->mEarlyDoorPressInteraction) referencedRequests.insert(agent->mEarlyDoorPressInteraction);
		}

		std::vector<InteractionRequestId> retiredRequests;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending && !referencedRequests.contains(id))
			{
				retiredRequests.push_back(id);
			}
		}
		std::set<InteractionRequestId> retiring(retiredRequests.begin(), retiredRequests.end());

		// Device operations outlive their request while another live owner - a
		// request which is staying, or a traversal request still preparing -
		// names them.
		std::set<DeviceOperationId> referencedOperations;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (retiring.contains(id)) continue;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				(void)requirement;
				referencedOperations.insert(operationId);
			}
		}
		for (auto const& [id, request] : mWorld.mTraversalRequests.entries())
		{
			(void)id;
			if (request->mPreparationOperation) referencedOperations.insert(request->mPreparationOperation);
		}

		std::vector<DeviceOperationId> retiredOperations;
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			if (operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) continue;
			if (!referencedOperations.contains(id)) retiredOperations.push_back(id);
		}

		// Requests first: dropping them releases the operations only they named.
		for (auto requestId : retiredRequests) removeInteractionRequest(requestId);
		for (auto operationId : retiredOperations) removeDeviceOperation(operationId);
	}

} // core
