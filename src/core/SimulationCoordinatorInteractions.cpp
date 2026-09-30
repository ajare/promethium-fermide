#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"

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
			if (binding.command.type == DeviceCommandType::SetSectorLights)
				validTarget = binding.command.target && binding.command.target.value <= mWorld.mSectors.size();
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
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			if (operation->mHasCommand && operation->mCommand == command
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
				: "Device command";
		auto id = mWorld.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester, command)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.phase = mWorld.mCurrentPhase;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mWorld.mDeviceOperations.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
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
		if (!point || !actor || !actor->isActive() || agentForbidsButtons(actor) || !point->mSector
			|| (actor->getState() != Agent::State::Idle
				&& actor->getState() != Agent::State::WaitingForTraversal)
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
		auto actor = mWorld.mAgents.find(actorId);
		// Even the press a moving Agent makes in passing is physical work, so a
		// deactivated Agent may not start one (#118, #192), and a
		// Buttons-forbidden Agent may not operate a control at all (#193).
		if (!point || !actor || !actor->isActive() || agentForbidsButtons(actor)
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
				if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
				{
					touchDeviceOperation(operationId, *operation);
					operation->mState = DeviceOperationState::Cancelled;
				}
			}
		}
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
		if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
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
				if (operation->mCommand.type == DeviceCommandType::OpenDoor)
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
			if (operation->mState != DeviceOperationState::Running)
			{
				continue;
			}
			if (operation->mCommand.type == DeviceCommandType::SetSectorLights
				&& operation->mCommand.target
				&& operation->mCommand.target.value <= mWorld.mSectors.size())
			{
				auto sector = mWorld.mSectors[(size_t)operation->mCommand.target.value - 1];
				bool succeeded = operation->mCommand.desiredState ? sector->lightsOn() : sector->lightsOff();
				operation->mState = succeeded ? DeviceOperationState::Succeeded : DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
			{
				auto resource = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mExtensible)
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
				if (!resource || !resource->mDoor)
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
			if (distance > point->mReach) continue;
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
				if (!actor || !actor->isActive() || agentForbidsButtons(actor))
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
			if (!actor || !actor->isActive() || agentForbidsButtons(actor)
				|| actor->getSector() != mWorld.mSectors[(size_t)point->mSector.value - 1].get())
			{
				// A deactivated Agent must not walk to the point or press it, and a
				// Buttons-forbidden Agent must not press it either; the point must
				// not stay reserved for either: cancel instead of suspending
				// (#118, #192, #193).
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			if (actor->getGlobalPosition().distanceTo(point->mPosition) > point->mReach)
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
						operation->mActivated = true;
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
