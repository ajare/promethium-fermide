#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Graph.h"
#include "core/MarkerSectorObject.h"
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Exceptions.h"
#include "core/ExtensibleObject.h"
#include "core/Sector.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	// Agent lifecycle moved out of World (ADR 0004 stage 1). The behaviour is
	// unchanged: the coordinator works on World's registries through
	// friendship, and calls back through the World facade for the machinery
	// which has not moved out of World yet - the modified-state marker,
	// sector lookup, interaction cancellation, and device-operation
	// cancellation and removal. The Agent snapshots it publishes are built by
	// the coordinator's own snapshot seam, which joined it in stage 5. The stop
	// requests it drops for a departing passenger go to the coordinator's own
	// lift scheduling helpers, and traversal cancellation and release to the
	// transaction lifecycle which joined it in stage 4.

	AgentId SimulationCoordinator::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId, uint32_t levelOffset, float xOffset)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!agent)
		{
			throw invalid_argument("World cannot own a null Agent");
		}
		if (mWorld.mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this World");
		}

		auto sector = mWorld._getSector(sectorId);
		if (sector->getType() == SectorType::Background)
		{
			throw WorldException(&mWorld,
				"An Agent cannot occupy a Background: it owns no walkable floor and takes no part in traversal");
		}
		mWorld.validateAgentLocationPlacement(*sector, *agent);
		auto rawAgent = agent.get();
		rawAgent->attachToWorld(&mWorld);
		sector->enterAgent(rawAgent, levelOffset, xOffset);
		auto id = mWorld.mAgents.add(std::move(agent));
		mWorld.mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	AgentId SimulationCoordinator::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!agent)
		{
			throw invalid_argument("World cannot own a null Agent");
		}
		if (mWorld.mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this World");
		}

		auto sector = mWorld._getSector(sectorId);
		if (sector->getType() == SectorType::Background)
		{
			throw WorldException(&mWorld,
				"An Agent cannot occupy a Background: it owns no walkable floor and takes no part in traversal");
		}
		mWorld.validateAgentLocationPlacement(*sector, *agent);
		auto rawAgent = agent.get();
		rawAgent->attachToWorld(&mWorld);
		sector->enterAgent(rawAgent);
		auto id = mWorld.mAgents.add(std::move(agent));
		mWorld.mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	AgentId SimulationCoordinator::createAgent(string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset)
	{
		mWorld.invalidateSimulationSnapshot();
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId, levelOffset, xOffset);
	}

	AgentId SimulationCoordinator::createAgent(string const& name, uint32_t sectorId)
	{
		mWorld.invalidateSimulationSnapshot();
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId);
	}

	void SimulationCoordinator::wakeAllAgents()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			(void)id;
			// A deactivated Agent is not simulated, so waking the world must not
			// restart its locomotion (#118).
			if (!agent->isActive()) continue;
			agent->wake();
		}
	}

	bool SimulationCoordinator::canSetAgentActive(AgentId id, bool /* active */, string* diagnostic) const
	{
		auto found = lookupAgent(id);
		if (!found)
		{
			if (diagnostic) *diagnostic = found.diagnostic;
			return false;
		}
		if (!mWorld.mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Agents cannot be activated or deactivated while the simulation is running";
			return false;
		}
		return true;
	}

	bool SimulationCoordinator::setAgentActive(AgentId id, bool active, string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!canSetAgentActive(id, active, diagnostic)) return false;
		auto* agent = mWorld.mAgents.find(id);
		if (agent->isActive() == active) return true;
		agent->setActive(active);
		if (!active)
		{
			for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
			{
				(void)resourceId;
				if (!resource->mAirlock && !resource->mSecurityScanner) continue;
				bool occupant = find(resource->mOccupants.begin(), resource->mOccupants.end(), id) != resource->mOccupants.end();
				if (!occupant && !(resource->mAirlock && hasCommittedMovement(*agent)))
				{
					// Pausing retained selected boarders. Deactivation abandons
					// unadopted admission, but never an occupied slot or an
					// already underway Airlock entry crossing.
					if ((agent->mTraversalTask && agent->mTraversalTask->edge->getTraversalResourceId() == resourceId)
						|| (agent->mQueuedTraversalTask && agent->mQueuedTraversalTask->edge->getTraversalResourceId() == resourceId))
					{
						agent->cancelTraversal();
						agent->mState = Agent::State::WaitingForTraversal;
					}
				}
				if (auto request = mWorld.mTraversalRequests.find(resource->mPreparationOperator);
					request && request->mOwner == id)
				{
					cancelInteraction(resource->mActivePreparation);
					resource->mActivePreparation = {};
					resource->mPreparationOperator = {};
					request->mPreparationRequested = false;
				}
			}
		}

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = active ? SimulationEventType::AgentActivated
			: SimulationEventType::AgentDeactivated;
		event.agent = makeAgentSnapshot(agent);
		if (agent->getBehaviourAssignment())
			mWorld.mAgentBehaviourRuntime->observeActivation(event);
		mWorld.mEvents.push_back(std::move(event));
		return true;
	}

	EntityLookup<Agent> SimulationCoordinator::lookupAgent(AgentId id)
	{
		auto entity = mWorld.mAgents.find(id);
		return entity ? EntityLookup<Agent>{ entity, {} }
			: EntityLookup<Agent>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<Agent const> SimulationCoordinator::lookupAgent(AgentId id) const
	{
		auto entity = mWorld.mAgents.find(id);
		return entity ? EntityLookup<Agent const>{ entity, {} }
			: EntityLookup<Agent const>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	AgentId SimulationCoordinator::getAgentId(Agent const* agent) const
	{
		auto found = mWorld.mAgentIds.find(agent);
		return found == mWorld.mAgentIds.end() ? AgentId{} : found->second;
	}

	bool SimulationCoordinator::holdsTraversalOwnership(AgentId id) const
	{
		if (!id) return false;

		for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
		{
			(void)requestId;
			if (request->mOwner == id) return true;
		}
		for (auto const& [permitId, permit] : mWorld.mTraversalPermits.entries())
		{
			(void)permitId;
			if (permit->mOwner == id) return true;
		}
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (find(resource->mOccupants.begin(), resource->mOccupants.end(), id)
				!= resource->mOccupants.end()) return true;
			if (resource->mExtensionOccupantLeases.contains(id)) return true;
			if (resource->mLiftExitAtSafeStop.contains(id)) return true;
			if (resource->mLiftExitFailures.contains(id)) return true;
			if (resource->mLiftPassengerDestinations.contains(id)) return true;
			if (resource->mLiftTripIntents.contains(id)) return true;
			if (resource->mLiftPassenger == id) return true;
			for (auto const& owners : resource->mLiftStopRequestOwners)
				if (owners.contains(id)) return true;
			for (auto const& ticks : resource->mLiftStopRequestTicks)
				if (ticks.contains(id)) return true;
		}
		return false;
	}

	bool SimulationCoordinator::isAgentInQueue(AgentId id) const
	{
		if (!id) return false;

		for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
		{
			(void)requestId;
			if (request->mOwner == id && request->mQueueTicket) return true;
		}

		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			for (auto requestId : point->mQueue)
			{
				auto request = mWorld.mInteractionRequests.find(requestId);
				if (request && request->mActor == id
					&& request->mResult == InteractionResult::Pending) return true;
			}
		}

		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			for (auto requestId : resource->mLiftConfirmationQueue)
			{
				auto request = mWorld.mTraversalRequests.find(requestId);
				if (request && request->mOwner == id) return true;
			}
		}
		return false;
	}

	void SimulationCoordinator::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!id) return;

		for (auto& occupant : resource.mOccupants)
			if (occupant == id) occupant = {};
		resource.mLiftPassengerTargets.erase(id);
		for (auto& carriage : resource.mShuttleCarriages)
			carriage.alightingDoors.erase(id);

		// An occupant lease keeps an extensible resource extended on the Agent's
		// behalf; surrendering the slot must surrender the lease with it.
		if (resource.mExtensionOccupantLeases.erase(id) && resource.mExtensible)
			resource.mExtensible->releaseExtensionLease();

		for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
			removeLiftStopRequest(resource, stop, id);
		resource.mLiftPassengerDestinations.erase(id);
		resource.mLiftTripIntents.erase(id);
		resource.mLiftExitAtSafeStop.erase(id);
		resource.mLiftExitFailures.erase(id);

		// The compatibility aliases mirror the manifest. Rebuild them from whatever
		// is left rather than leave them naming a handle which can no longer ride.
		if (resource.mLiftPassenger == id)
		{
			resource.mLiftPassenger = {};
			for (auto occupant : resource.mOccupants)
				if (occupant) { resource.mLiftPassenger = occupant; break; }
			resource.mLiftDestinationStop = ~0u;
		}
	}

	void SimulationCoordinator::releaseTraversalOwnership(AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!id) return;

		// Requests and permits go first. Most of a resource's claims on an Agent are
		// keyed by request - queue lanes, admission reservations, door open leases,
		// extension request leases - and cancelling the request surrenders them all
		// through the same paths ordinary cancellation uses. No safe transport exit is
		// requested: the Agent is on its way out of the World entirely.
		std::vector<TraversalRequestId> requests;
		for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
			if (request->mOwner == id) requests.push_back(requestId);
		for (auto requestId : requests)
		{
			auto request = mWorld.mTraversalRequests.find(requestId);
			if (!request) continue;
			auto const permitId = request->mPermit;
			cancelTraversal(requestId, permitId, false);
			releaseTraversal(requestId, permitId);
		}

		// A permit whose request has already gone is still the Agent's handle.
		std::vector<TraversalPermitId> permits;
		for (auto const& [permitId, permit] : mWorld.mTraversalPermits.entries())
			if (permit->mOwner == id) permits.push_back(permitId);
		for (auto permitId : permits)
		{
			auto permit = mWorld.mTraversalPermits.find(permitId);
			if (!permit) continue;
			releaseTraversal(permit->mRequest, permitId);
		}

		// Finally the claims keyed by Agent itself, which survive every request having
		// been released: the manifest slot of a car the Agent boarded, its stop
		// requests, its pending safe exit, and its occupant leases.
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			releaseAgentFromResource(*resource, id);
		}
	}

	MovementCommandResult SimulationCoordinator::inspectMoveAgentToMarker(
		AgentId id, MarkerId marker, bool behaviourCommand, std::string_view action) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) return { MovementCommandStatus::UnknownAgent };
		if (!behaviourCommand && mWorld.agentBehaviourOwnsMovement(id))
			return { MovementCommandStatus::BehaviourOwned };
		if (!agent->isActive()) return { MovementCommandStatus::InactiveAgent };
		if (!agent->getSector() || agent->getSector()->getType() == SectorType::Background)
			return { MovementCommandStatus::NoOccupiableSector };
		if (!mWorld.lookupMarker(marker)) return { MovementCommandStatus::UnknownMarker };
		if (!mWorld.actionAvailable(marker, action)) return { MovementCommandStatus::UnavailableAction };
		if (auto it = mWorld.mMovementGoals.find(id); it != mWorld.mMovementGoals.end()
			&& !it->second.cancelling && it->second.marker == marker && it->second.selectedAction == action)
			return { MovementCommandStatus::NoOp };
		if (!mWorld.mGraph || mWorld.mTopologyDirty || !mWorld.mTopologyValid)
			return { MovementCommandStatus::TopologyUnavailable };
		return { mWorld.mMovementGoals.contains(id)
			? MovementCommandStatus::Superseded : MovementCommandStatus::Accepted };
	}

	MovementCommandResult SimulationCoordinator::moveAgentToMarker(
		AgentId id, MarkerId marker, bool behaviourCommand, std::string_view action)
	{
		mWorld.invalidateSimulationSnapshot();
		auto const inspected = inspectMoveAgentToMarker(id, marker, behaviourCommand, action);
		if (!inspected.accepted() || inspected.status == MovementCommandStatus::NoOp) return inspected;
		auto agent = mWorld.mAgents.find(id);
		if (auto old = mWorld.mMovementGoals.find(id); old != mWorld.mMovementGoals.end())
		{
			SimulationEvent event;
			event.agent = makeAgentSnapshot(agent);
			event.destinationMarker = old->second.marker;
			event.selectedAction = old->second.selectedAction;
			event.type = SimulationEventType::MovementCancelled;
			event.movementCancellationReason = old->second.marker && !mWorld.lookupMarker(old->second.marker)
				? MovementCancellationReason::TargetDeleted
				: old->second.actionInvalidated ? MovementCancellationReason::ActionUnavailable
				: old->second.cancelling ? MovementCancellationReason::Explicit : MovementCancellationReason::Superseded;
			if (old->second.actionInvalidated) event.diagnostic = "Selected Action was removed from the target Marker";
			mWorld.mPendingMovementOutcomes.push_back(std::move(event));
		}
		shared_ptr<const Vertex> target;
		for (auto const& sector : mWorld.mSectors)
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
					object && object->getMarker()->getId() == marker)
					target = mWorld.mGraph->getVertexForObject(object);
		mWorld.mMovementGoals[id] = { marker, target ? target->getPosition() : Vector2::ZERO, false,
			target ? SectorId{ (uint64_t)target->getSector()->getIndex() + 1 } : SectorId{},
			RouteLossReason::None, behaviourCommand, false, true, RouteLossReason::Unreachable,
			std::nullopt, nullptr };
		mWorld.mMovementGoals[id].selectedAction = action;
		if (hasCommittedMovement(*agent))
		{
			mWorld.mMovementGoals[id].planningDeferred = true;
			agent->mRoutePlanningTotalTicks = agent->mRoutePlanningRemainingTicks = 0;
		}
		else beginRoutePlanning(*agent);
		return inspected;
	}

	bool SimulationCoordinator::hasCommittedMovement(Agent const& agent) const
	{
		auto const id = mWorld.getAgentId(&agent);
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			if (find(resource->mOccupants.begin(), resource->mOccupants.end(), id) != resource->mOccupants.end())
				return true;
			// Platform boarding walks through a virtual boundary before a permit
			// is granted. Authorization changes must not interrupt that transfer.
			if (resource->mOpenPlatformLift)
				for (auto requestId : resource->mVirtualBoundaryOwners)
					if (auto request = mWorld.mTraversalRequests.find(requestId);
						request && request->mOwner == id) return true;
		}
		return agent.mTraversalTask && agent.mTraversalTask->destinationVertex
			&& (agent.mTraversalTask->destinationVertex->getSector().get() != agent.getSector()
				|| agent.mTraversalTask->edge->getTraversalResourceId()
				|| agent.mTraversalTask->edge->getType() == EdgeType::Staircase)
			&& (agent.mState == Agent::State::TraversingEdge
				|| agent.mState == Agent::State::AwaitingTraversalCommit);
	}

	bool SimulationCoordinator::claimUsablePoint(AgentId id, MarkerId marker)
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent || !mWorld.isFurnitureMarker(marker)) return false;
		auto occupant = mWorld.usablePointOccupant(marker);
		if (occupant && occupant != id) return false;
		agent->mOccupiedUsablePoint = marker;
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [otherId, other] : mWorld.mAgents.entries())
		{
			if (otherId == id) continue;
			auto goal = mWorld.mMovementGoals.find(otherId);
			if (goal != mWorld.mMovementGoals.end() && goal->second.marker == marker)
				goal->second.retainedPath.reset();
			if (other->mPath.path && !other->mPath.path->nodes.empty())
				if (auto destination = std::dynamic_pointer_cast<Marker>(other->mPath.path->nodes.back().targetVertex->getObject());
					destination && destination->getId() == marker)
					replanAgentAfterAuthorizationRefusal(otherId);
		}
		return true;
	}

	void SimulationCoordinator::replanAgentAfterAuthorizationRefusal(AgentId id, bool retainPath)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = mWorld.mAgents.find(id);
		if (!agent) return;
		if (agent->mState == Agent::State::RoutePlanning)
		{
			if (!retainPath) mWorld.mMovementGoals.at(id).retainedPath.reset();
			return;
		}
		if (!agent->mPath.path || agent->mPath.path->nodes.empty()) return;
		auto& goal = mWorld.mMovementGoals[id];
		if (goal.cancelling || goal.planningDeferred) return;
		if (retainPath)
		{
			if (hasCommittedMovement(*agent)) return;
			goal.retainedPath = agent->mPath.path;
			goal.retainedFromNode = agent->mPath.targetNode;
			goal.voluntaryPlanningStartedTick = mWorld.getSimulationTick();
		}
		else goal.retainedPath.reset();
		auto destination = agent->mPath.path->nodes.back().targetVertex;
		if (!destination || !destination->getSector()) return;
		goal.position = destination->getPosition();
		goal.sector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
		goal.startPathing = agent->mState != Agent::State::Idle;
		goal.planningFailureReason = RouteLossReason::Unreachable;
		if (!goal.marker)
			for (uint32_t i = 0; i < destination->getSector()->getNumObjects(); ++i)
				if (auto object = dynamic_pointer_cast<MarkerSectorObject>(destination->getSector()->getObject(i));
					object && mWorld.mGraph->getVertexForObject(object) == destination)
					goal.marker = object->getMarker()->getId();
		if (!goal.marker)
		{
			goal.fallbackIntent.emplace();
			goal.fallbackIntent->destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
			goal.fallbackIntent->destinationLocalPosition = destination->getPosition() - destination->getSector()->getPosition();
		}
		if (hasCommittedMovement(*agent)) goal.planningDeferred = true;
		else beginRoutePlanning(*agent);
	}

	void SimulationCoordinator::beginRoutePlanning(Agent& valueAgent)
	{
		auto* agent = &valueAgent;
		auto const id = mWorld.getAgentId(agent);
		agent->clearRuntimePath();
		// Pause/resume replans from the passenger's current position. Keep an
		// accepted transport journey alive while doing so: dropping its
		// manifest and shared Stop request would retroactively revoke selection
		// when the author tightens destination requirements while paused.
		bool acceptedLiftJourney = false;
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if ((resource->mLift || resource->mShuttle || resource->mAirlock || resource->mSecurityScanner)
				&& find(resource->mOccupants.begin(), resource->mOccupants.end(), id) != resource->mOccupants.end())
				acceptedLiftJourney = true;
		}
		if (!acceptedLiftJourney)
		{
			releaseTraversalOwnership(id);
			vector<InteractionRequestId> interactions;
			for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
				if (request->getActor() == id && request->getResult() == InteractionResult::Pending)
					interactions.push_back(requestId);
			for (auto requestId : interactions) cancelInteraction(requestId);
			for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
				if (operation->getRequesters().contains(id)) cancelDeviceOperation(operationId, id);
		}
		mWorld.mPausedPathIntents.erase(id);
		auto const minimum = secondsToTicks(agent->getEffectiveMinimumRoutePlanningTime().value, mWorld.getFixedTimestep());
		auto const maximum = secondsToTicks(agent->getEffectiveMaximumRoutePlanningTime().value, mWorld.getFixedTimestep());
		// Episode-local SplitMix64 stream, independent of Lua and Escalators.
		auto random = mWorld.getRandomSeed() ^ (id.value * 0xd1b54a32d192ed03ULL)
			^ (++agent->mRoutePlanningSequence * 0x94d049bb133111ebULL);
		auto draw = [&]()
		{
			auto value = (random += 0x9e3779b97f4a7c15ULL);
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31);
		};
		auto const range = maximum - minimum + 1;
		auto const threshold = (uint64_t{ 0 } - range) % range;
		auto value = draw();
		while (value < threshold) value = draw();
		agent->mRoutePlanningTotalTicks = minimum + value % range;
		agent->mRoutePlanningRemainingTicks = agent->mRoutePlanningTotalTicks;
		agent->mState = Agent::State::RoutePlanning;
	}

	MovementCommandResult SimulationCoordinator::inspectCancelAgentMovement(
		AgentId id, bool behaviourCommand) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) return { MovementCommandStatus::UnknownAgent };
		if (!behaviourCommand && mWorld.agentBehaviourOwnsMovement(id))
			return { MovementCommandStatus::BehaviourOwned };
		if (!agent->isActive()) return { MovementCommandStatus::InactiveAgent };
		auto it = mWorld.mMovementGoals.find(id);
		if (it == mWorld.mMovementGoals.end() && !agent->mPath.path && !holdsTraversalOwnership(id))
			return { MovementCommandStatus::NoOp };
		if (it != mWorld.mMovementGoals.end() && it->second.cancelling)
			return { MovementCommandStatus::NoOp };
		return { MovementCommandStatus::Accepted };
	}

	MovementCommandResult SimulationCoordinator::cancelAgentMovement(
		AgentId id, bool behaviourCommand)
	{
		auto const inspected = inspectCancelAgentMovement(id, behaviourCommand);
		if (inspected.status != MovementCommandStatus::Accepted) return inspected;
		auto& goal = mWorld.mMovementGoals[id];
		goal.cancelling = true;
		return inspected;
	}

	void SimulationCoordinator::clearAgentMovementForBehaviourEdit(AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = mWorld.mAgents.find(id);
		if (!agent) return;
		mWorld.mMovementGoals.erase(id);
		mWorld.mPausedPathIntents.erase(id);
		agent->clearRuntimePath();
		agent->mResetPosition = agent->mPosition;
		agent->mResetLocalDepth = agent->mLocalDepth;
		agent->mResetPath.reset();
		agent->mResetDestinationMarker = {};
		agent->mResetPathActive = false;
		releaseTraversalOwnership(id);

		vector<InteractionRequestId> interactions;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
			if (request->getActor() == id && request->getResult() == InteractionResult::Pending)
				interactions.push_back(requestId);
		for (auto requestId : interactions) cancelInteraction(requestId);
		for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
			if (operation->getRequesters().contains(id)) cancelDeviceOperation(operationId, id);
	}

	void SimulationCoordinator::advanceRoutePlanning()
	{
		for (auto& [id, goal] : mWorld.mMovementGoals)
		{
			auto agent = mWorld.mAgents.find(id);
			if (!agent || !agent->isActive() || goal.cancelling) continue;
			if (goal.planningDeferred)
			{
				if (hasCommittedMovement(*agent)) continue;
				goal.planningDeferred = false;
				agent->clearRuntimePath();
				if (agent->getSector()
					&& SectorId{ (uint64_t)agent->getSector()->getIndex() + 1 } == goal.sector
					&& agent->getGlobalPosition().distanceTo(goal.position) < 0.001f
					&& mWorld.canAgentAccessLocation(*agent->getSector(), *agent)) continue;
				beginRoutePlanning(*agent);
				continue; // The first complete planning tick is the next tick.
			}
			if (agent->mState != Agent::State::RoutePlanning) continue;
			if (goal.voluntaryPlanningStartedTick == mWorld.getSimulationTick()) continue;
			// Ordinary voluntary episodes keep their permanent invalidation rule.
			// A topology-rebound suffix, however, is evaluated at expiry only.
			if (goal.retainedPath && (!goal.fallbackIntent || goal.fallbackIntent->retainedNodes.empty())
				&& !pathing::comparePathSuffixCosts(*agent, *mWorld.mGraph,
					*goal.retainedPath, goal.retainedFromNode, *goal.retainedPath, goal.retainedFromNode))
				goal.retainedPath.reset();
			if (--agent->mRoutePlanningRemainingTicks != 0) continue;
			// Validate retained topology only when the episode expires: rebinding
			// is not an early route decision or a new planning interval.
			if (goal.retainedPath && !pathing::comparePathSuffixCosts(*agent, *mWorld.mGraph,
				*goal.retainedPath, goal.retainedFromNode, *goal.retainedPath, goal.retainedFromNode))
				goal.retainedPath.reset();
			agent->clearRuntimePath();
			shared_ptr<const Vertex> target;
			for (auto const& sector : mWorld.mSectors)
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
					if (auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
						object && object->getMarker()->getId() == goal.marker)
						target = mWorld.mGraph->getVertexForObject(object);
			if (!goal.marker && goal.fallbackIntent)
			{
				auto const& intent = *goal.fallbackIntent;
				if (intent.destinationSector && intent.destinationSector.value <= mWorld.mSectors.size())
				{
					auto sector = mWorld.mSectors[(size_t)intent.destinationSector.value - 1];
					if (sector)
					{
						try
						{
							target = mWorld.mGraph->getClosestVertexInSector(sector.get(),
								sector->getPosition() + intent.destinationLocalPosition);
						}
						catch (GraphException const&)
						{
							// A retained Sector can lose all graph vertices during editing.
							// Resolve that loss only now, at the episode's expiry.
						}
					}
				}
			}
			if (target)
			{
				goal.position = target->getPosition();
				goal.sector = SectorId{ (uint64_t)target->getSector()->getIndex() + 1 };
			}
			auto path = target ? mWorld.mGraph->calculatePath(agent, target) : nullptr;
			if (target && goal.retainedPath)
			{
				auto costs = path ? pathing::comparePathSuffixCosts(*agent, *mWorld.mGraph,
					*goal.retainedPath, goal.retainedFromNode, *path, 0) : std::nullopt;
				if (!path || (costs && !mWorld.getRouteChoicePolicy().shouldReplacePath(
					costs->first, costs->second, agent->getEffectiveRoutePersistence().value)))
				{
					auto retained = std::move(goal.retainedPath);
					agent->assignPath(std::move(retained), goal.startPathing, false);
					agent->mPath.targetNode = goal.retainedFromNode;
					if (goal.startPathing && goal.fallbackIntent
						&& (goal.fallbackIntent->resumeLocalTraversal || goal.fallbackIntent->resumeContinuousTraversal)
						&& agent->mPath.targetNode + 1 < agent->mPath.path->nodes.size())
						agent->mState = Agent::State::WaitingForTraversal;
					continue;
				}
			}
			goal.retainedPath.reset();
			if (path && target && agent->getSector() == target->getSector().get()
				&& agent->getGlobalPosition() == target->getPosition()
				&& mWorld.canAgentAccessLocation(*target->getSector(), *agent)) continue;
			if (path && !path->nodes.empty())
			{
				agent->assignPath(std::move(path), goal.startPathing, false);
				if (goal.startPathing && goal.fallbackIntent && goal.fallbackIntent->resumeContinuousTraversal)
				{
					auto const& intent = *goal.fallbackIntent;
					for (uint32_t node = 1; node < agent->mPath.path->nodes.size(); ++node)
					{
						auto const& edge = agent->mPath.path->nodes[node].edge;
						auto const& targetVertex = agent->mPath.path->nodes[node].targetVertex;
						if (!edge || !targetVertex || edge->getType() != intent.traversalEdgeType
							|| targetVertex->getPosition().distanceTo(intent.traversalDestinationPosition) > 0.001f) continue;
						auto source = edge->getOtherVertex(targetVertex);
						if (!source || source->getPosition().distanceTo(intent.traversalSourcePosition) > 0.001f) continue;
						agent->mPath.targetNode = node - 1;
						agent->mState = Agent::State::WaitingForTraversal;
						break;
					}
				}
			}
			else goal.routeLossReason = goal.planningFailureReason;
		}
	}

	void SimulationCoordinator::updateMovementGoals()
	{
		mWorld.invalidateSimulationSnapshot();
		auto publishPendingOutcomes = [&]()
		{
			for (auto& event : mWorld.mPendingMovementOutcomes)
			{
				event.sequence = mWorld.mNextEventSequence++;
				event.tick = mWorld.mSimulationTick;
				event.phase = mWorld.mCurrentPhase;
				mWorld.mAgentBehaviourRuntime->observeOutcome(event);
				mWorld.mEvents.push_back(std::move(event));
			}
			mWorld.mPendingMovementOutcomes.clear();
		};
		publishPendingOutcomes();
		for (auto it = mWorld.mMovementGoals.begin(); it != mWorld.mMovementGoals.end();)
		{
			auto id = it->first;
			auto& goal = it->second;
			auto agent = mWorld.mAgents.find(id);
			if (!agent) { it = mWorld.mMovementGoals.erase(it); continue; }
			auto const targetDeleted = goal.marker && !mWorld.lookupMarker(goal.marker);
			auto const actionRemoved = goal.actionInvalidated || (!targetDeleted && goal.marker
				&& goal.selectedAction != IdleAction && !mWorld.actionAvailable(goal.marker, goal.selectedAction));
			if (targetDeleted || actionRemoved) goal.cancelling = true;
			if (!agent->isActive() && !targetDeleted && !actionRemoved) { ++it; continue; }
			if (goal.cancelling)
			{
				// Finish an in-flight crossing and any occupied resource journey first.
				// Transport cancellation uses the already scheduled destination stop:
				// do not release a manifest slot or strand a passenger in a Transit.
				if (hasCommittedMovement(*agent))
				{ ++it; continue; }
				agent->clearRuntimePath();
				releaseTraversalOwnership(id);
				vector<InteractionRequestId> interactions;
				for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
					if (request->getActor() == id && request->getResult() == InteractionResult::Pending) interactions.push_back(requestId);
				for (auto requestId : interactions) cancelInteraction(requestId);
				for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
					if (operation->getRequesters().contains(id)) cancelDeviceOperation(operationId, id);
			}
			else if (goal.planningDeferred || agent->mState == Agent::State::RoutePlanning || agent->mPath.path) { ++it; continue; }
			SimulationEvent event;
			event.tick = mWorld.mSimulationTick;
			event.phase = mWorld.mCurrentPhase;
			event.agent = makeAgentSnapshot(agent);
			event.destinationMarker = goal.marker;
			event.selectedAction = goal.selectedAction;
			event.type = goal.cancelling ? SimulationEventType::MovementCancelled
				: goal.routeLossReason == RouteLossReason::None
					&& (!goal.marker || mWorld.lookupMarker(goal.marker)) && agent->getSector()
					&& SectorId{ (uint64_t)agent->getSector()->getIndex() + 1 } == goal.sector
					&& agent->getGlobalPosition().distanceTo(goal.position) < 0.001f
					? SimulationEventType::DestinationReached : SimulationEventType::RouteLost;
			if (event.type == SimulationEventType::RouteLost)
				event.routeLossReason = goal.marker && !mWorld.lookupMarker(goal.marker)
					? RouteLossReason::DestinationRemoved
					: goal.routeLossReason == RouteLossReason::None
						? RouteLossReason::TopologyChanged : goal.routeLossReason;
			else if (event.type == SimulationEventType::MovementCancelled)
				event.movementCancellationReason = targetDeleted
					? MovementCancellationReason::TargetDeleted : actionRemoved
						? MovementCancellationReason::ActionUnavailable : MovementCancellationReason::Explicit;
			if (actionRemoved) event.diagnostic = "Selected Action was removed from the target Marker";
			if (event.type == SimulationEventType::DestinationReached)
				mWorld.executeMarkerAction(id, goal.marker, goal.selectedAction, event);
			it = mWorld.mMovementGoals.erase(it);
			// Same-Marker finishing may fail inside arrival. Publish its diagnostic
			// before replacement completion and before headless failure pauses us.
			publishPendingOutcomes();
			event.sequence = mWorld.mNextEventSequence++;
			// Runtime observation is a separate subscription: it never drains or
			// mutates the public simulation event queue.
			mWorld.mAgentBehaviourRuntime->observeOutcome(event);
			mWorld.mEvents.push_back(std::move(event));
		}
	}

	EntityRemovalResult SimulationCoordinator::removeAgent(AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupAgent(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		if (found.entity->getState() == Agent::State::WaitingForTraversal
			&& !found.entity->getTraversalPermitId())
		{
			// Removing a waiter is cancellation, not an exceptional state. Its
			// queue ticket and physical reservation are released by clearPath().
			found.entity->clearPath();
		}
		if (found.entity->getState() != Agent::State::Idle)
		{
			return { false, format("Agent handle {} is active and cannot be removed safely", id.value) };
		}

		// Idle is not the same as unclaimed. clearPath() asks a Lift or Shuttle to let
		// a rider off when it is next safe rather than ejecting them from a moving
		// car, so the manifest keeps naming the Agent after its route is gone. Every
		// one of those claims is surrendered here; a handle left behind could never
		// disembark, and the capacity would be lost for the life of the World.
		found.entity->cancelTraversal();
		releaseTraversalOwnership(id);
		if (holdsTraversalOwnership(id))
		{
			return { false, format("Agent handle {} still holds a traversal resource and cannot be removed", id.value) };
		}

		vector<InteractionRequestId> ownedRequests;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->getActor() == id && request->getResult() == InteractionResult::Pending)
			{
				ownedRequests.push_back(requestId);
			}
		}
		for (auto requestId : ownedRequests)
		{
			mWorld.cancelInteraction(requestId);
		}

		vector<DeviceOperationId> ownedOperations;
		for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
		{
			if (operation->getRequesters().contains(id))
			{
				ownedOperations.push_back(operationId);
			}
		}
		for (auto operationId : ownedOperations)
		{
			mWorld.cancelDeviceOperation(operationId, id);
			if (auto operation = mWorld.mDeviceOperations.find(operationId); operation && operation->getRequesters().empty())
			{
				(void)mWorld.removeDeviceOperation(operationId);
			}
		}

		auto snapshot = makeAgentSnapshot(found.entity);
		if (auto sector = const_cast<Sector*>(found.entity->getSector()))
		{
			sector->exitAgent(found.entity);
		}
		// Removing an assigned Agent is also the end of its private behaviour
		// lifetime. Best-effort on_stop runs while the final read-only Agent state
		// is still available and cannot veto removal.
		mWorld.mAgentBehaviourRuntime->removeInstance(mWorld, id,
			AgentBehaviourTeardownReason::Unassignment);
		mWorld.mAgentIds.erase(found.entity);
		mWorld.mAgents.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::AgentRemoved;
		event.agent = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		mWorld.markModified();
		return { true, {} };
	}

} // core
