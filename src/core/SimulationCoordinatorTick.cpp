#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/BulkheadDoor.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Lift.h"
#include "core/MarkerSectorObject.h"
#include "core/Sector.h"
#include "core/Shuttle.h"
#include "core/Simulation.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// The tick pipeline moved out of World (ADR 0004 stage 5): the fixed
	// timestep accumulator, the six simulation phases and the per-phase
	// advancement of lift, shuttle and door resources, tick event publication,
	// the simulation clock queries and event consumption.
	//
	// The behaviour is unchanged. The clock, the phase marker, the event queue
	// and every registry stay in World (ADR 0001: World owns simulation
	// entities, and no state moves in ADR 0004); the coordinator drives them
	// through the World it was given and calls its own interaction, queue,
	// admission, lease, lift and snapshot seams directly instead of calling
	// back through the World facade (design pattern, not the Facade sector
	// type).
	//
	// The last four methods serve the edit/simulation boundary. Pausing and
	// resuming around a topology rebuild is deliberately not a coordinator
	// entry point of its own: the protocol belongs to World's structural-edit
	// contract, which refuses an edit unless the simulation is paused and
	// refuses to resume over dirty topology. What the coordinator supplies is
	// the simulation-side work that protocol performs - taking every live
	// traversal apart, remembering the route each Agent was working to, putting
	// those routes back on the new graph, and publishing the boundary events.

	void SimulationCoordinator::advanceLiftResources()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [resourceId, resourcePtr] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if ((!resource.mLift && !resource.mShuttle) || resource.mLiftStops.empty()) continue;
			for (auto requestId : resource.mOpenPlatformMissedBoarding)
				if (auto request = mWorld.mTraversalRequests.find(requestId); request)
					if (auto actor = mWorld.mAgents.find(request->mOwner))
					{
						actor->mTraversalLocalGoal.reset();
						auto held = resource.mOpenPlatformMissedPositions.find(requestId);
						if (held != resource.mOpenPlatformMissedPositions.end() && actor->getSector())
							actor->setPosition({ const_cast<Sector*>(actor->getSector()),
								held->second - actor->getSector()->getPosition() }, false);
					}
			auto previousVehiclePosition = resource.mLiftPosition;
			auto forEachLanding = [&](auto&& callback)
			{
				if (resource.mShuttle)
				{
					for (auto const& door : resource.mShuttleDoors)
						callback(door.stopIndex, mWorld.mTraversalResources.find(door.landingResource));
				}
				else for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
					callback(stop, mWorld.mTraversalResources.find(resource.mLiftStops[stop].landingResource));
			};
			auto hasWaitingAdmissionAtStop = [&](uint32_t stop)
			{
				return any_of(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
					[&](TraversalRequestId id)
					{
						auto request = mWorld.mTraversalRequests.find(id);
						if (!request) return false;
						if (resource.mOpenPlatformLift)
						{
							auto intent = resource.mLiftTripIntents.find(request->mOwner);
							return intent != resource.mLiftTripIntents.end()
								&& intent->second.originStop == stop;
						}
						auto landing = mWorld.mTraversalResources.find(request->mResource);
						return landing && landing->mLiftStopIndex == stop;
					});
			};
			auto beginBoardingWindow = [&]
			{
				resource.mLiftStopPhase = LiftStopPhase::Boarding;
				if (resource.mOpenPlatformLift)
				{
					for (auto requestId : resource.mAdmissionQueue)
						if (auto request = mWorld.mTraversalRequests.find(requestId); request)
						{
							auto intent = resource.mLiftTripIntents.find(request->mOwner);
							if (intent != resource.mLiftTripIntents.end()
								&& intent->second.originStop == resource.mLiftCurrentStop)
							{
								resource.mOpenPlatformMissedBoarding.erase(requestId);
								resource.mOpenPlatformMissedPositions.erase(requestId);
							}
						}
					refreshQueuePositions(resource);
				}
				// A nonzero cutoff belongs to an existing boarding window, such as
				// one temporarily reopened by an obstruction during closure.
				if (resource.mLiftBoardingCutoffTick) return;
				resource.mLiftServiceStartedTick = mWorld.mSimulationTick;
				resource.mLiftBoardingCutoffTick = mWorld.mSimulationTick + resource.mLiftMaximumBoardingTicks;
			};

			// A safety hold or obstruction at the aligned landing overrides closure.
			// The original cutoff is retained, so this cannot admit a late caller.
			if (!resource.mLiftMoving && resource.mLiftCurrentStop < resource.mLiftStops.size()
				&& resource.mLiftStopPhase == LiftStopPhase::Closing)
			{
				bool heldOrObstructed = false;
				forEachLanding([&](uint32_t stop, TraversalResource* landing)
				{
					if (stop != resource.mLiftCurrentStop || !landing) return;
					auto obstruction = any_of(landing->mSensorObservations.begin(),
						landing->mSensorObservations.end(), [](auto const& value)
						{ return value.second == DoorSensorObservation::Obstruction; });
					heldOrObstructed = heldOrObstructed || !landing->mOpenLeases.empty() || obstruction;
				});
				if (heldOrObstructed) resource.mLiftStopPhase = LiftStopPhase::Opening;
			}

			if ((resource.mLiftStopPhase == LiftStopPhase::Idle
				|| resource.mLiftStopPhase == LiftStopPhase::Closing) && !resource.mLiftMoving)
			{
				// An idle car is already available to callers at its aligned stop, even
				// while their physical call interaction is still in progress. Service
				// them before dispatching the empty car to an earlier completed call.
				resource.mLiftTargetStop = resource.mLiftStopPhase == LiftStopPhase::Idle
					&& hasWaitingAdmissionAtStop(resource.mLiftCurrentStop)
					? resource.mLiftCurrentStop : chooseNextLiftStop(resource);
			}

			if (resource.mLiftTargetStop < resource.mLiftStops.size()
				&& resource.mLiftTargetStop != resource.mLiftCurrentStop)
			{
				auto target = resource.mLiftStops[resource.mLiftTargetStop].globalPosition;
				bool interlocked = any_of(resource.mVirtualBoundaryOwners.begin(),
					resource.mVirtualBoundaryOwners.end(), [](auto owner) { return (bool)owner; });
				forEachLanding([&](uint32_t, TraversalResource* landing)
				{
					if (!landing || !landing->mDoor) return;
					if (!landing->mOpenLeases.empty()) interlocked = true;
					if (!landing->mDoor->isClosed())
					{
						interlocked = true;
						if (landing->mOpenLeases.empty() && !landing->mDoor->isClosing())
							landing->mDoor->requestClose();
					}
				});
				if (!interlocked && resource.mLiftStopPhase == LiftStopPhase::Closing)
				{
					resource.mLiftStopPhase = LiftStopPhase::Moving;
					resource.mLiftMoving = true;
				}
				if (resource.mLiftStopPhase == LiftStopPhase::Moving)
				{
					auto const speed = resource.mShuttle ? resource.mShuttle->getSpeed()
						: resource.mLift->getSpeed();
					auto amount = speed * World::getFixedTimestep();
					if (target > resource.mLiftPosition)
						resource.mLiftPosition = min(target, resource.mLiftPosition + amount);
					else resource.mLiftPosition = max(target, resource.mLiftPosition - amount);
					if (abs(resource.mLiftPosition - target) < 0.001f)
					{
						resource.mLiftPosition = target;
						resource.mLiftCurrentStop = resource.mLiftTargetStop;
						resource.mLiftMoving = false;
						resource.mLiftStopPhase = LiftStopPhase::Opening;
						resource.mLiftServiceStartedTick = mWorld.mSimulationTick;
						resource.mLiftBoardingCutoffTick = resource.mShuttle ? 0
							: mWorld.mSimulationTick + resource.mLiftMaximumBoardingTicks;
					}
				}
			}
			else if (resource.mLiftTargetStop == resource.mLiftCurrentStop
				&& (resource.mLiftStopPhase == LiftStopPhase::Idle
					|| resource.mLiftStopPhase == LiftStopPhase::Moving))
			{
				resource.mLiftMoving = false;
				resource.mLiftStopPhase = LiftStopPhase::Opening;
				resource.mLiftServiceStartedTick = mWorld.mSimulationTick;
				resource.mLiftBoardingCutoffTick = resource.mShuttle ? 0
					: mWorld.mSimulationTick + resource.mLiftMaximumBoardingTicks;
			}
			else if (resource.mLiftTargetStop == resource.mLiftCurrentStop
				&& resource.mLiftStopPhase == LiftStopPhase::Closing)
			{
				// Calls accepted after cutoff cannot reverse a close already in progress.
				// Once fully closed they may begin a distinct service visit.
				bool allClosed = true;
				forEachLanding([&](uint32_t, TraversalResource* landing)
				{
					allClosed = allClosed && (!landing || !landing->mDoor || landing->mDoor->isClosed());
				});
				if (allClosed) resource.mLiftStopPhase = LiftStopPhase::Idle;
			}

			// Every Shuttle Door authored at the aligned stop opens together. Door
			// leases still protect active crossings; the stop phase controls closure.
			if (resource.mShuttle && !resource.mLiftMoving
				&& resource.mLiftCurrentStop < resource.mLiftStops.size()
				&& (resource.mLiftStopPhase == LiftStopPhase::Opening
					|| resource.mLiftStopPhase == LiftStopPhase::Disembarking
					|| resource.mLiftStopPhase == LiftStopPhase::Boarding))
				forEachLanding([&](uint32_t stop, TraversalResource* landing)
				{
					if (stop == resource.mLiftCurrentStop && landing && landing->mDoor
						&& !landing->mDoor->isOpen() && !landing->mDoor->isOpening())
						landing->mDoor->requestOpen();
				});

			if (!resource.mLiftMoving && resource.mLiftStopPhase == LiftStopPhase::Opening
				&& mWorld.mSimulationTick > resource.mLiftServiceStartedTick)
			{
				if (liftHasDisembarkDemand(resource, resource.mLiftCurrentStop))
					resource.mLiftStopPhase = LiftStopPhase::Disembarking;
				else beginBoardingWindow();
			}
			if (resource.mLiftStopPhase == LiftStopPhase::Disembarking
				&& !liftHasDisembarkDemand(resource, resource.mLiftCurrentStop)
				&& resource.mLiftExitAtSafeStop.empty())
				beginBoardingWindow();

			assignLiftSafeExitPaths(resource);

			bool crossing = any_of(resource.mVirtualBoundaryOwners.begin(),
				resource.mVirtualBoundaryOwners.end(), [](auto owner) { return (bool)owner; });
			forEachLanding([&](uint32_t, TraversalResource* landing)
			{
				if (landing) crossing = crossing || any_of(landing->mCrossingOwners.begin(),
					landing->mCrossingOwners.end(), [](auto owner) { return (bool)owner; });
			});
			auto reserved = any_of(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(),
				[](auto id) { return (bool)id; });
			auto occupied = (uint32_t)count_if(resource.mOccupants.begin(), resource.mOccupants.end(),
				[](auto id) { return (bool)id; });
			auto unresolvedDestination = any_of(resource.mOccupants.begin(), resource.mOccupants.end(), [&](auto owner)
				{ return owner && !resource.mLiftPassengerDestinations.contains(owner); });
			bool occupantsPositioned = true;
			if (resource.mLift && !resource.mOpenPlatformLift)
			{
				auto transit = mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get();
				for (uint32_t i = 0; i < resource.mOccupants.size(); ++i)
					if (auto passenger = mWorld.mAgents.find(resource.mOccupants[i]))
					{
						auto local = resource.mCapacityPositions[i];
						if (auto target = resource.mLiftPassengerTargets.find(resource.mOccupants[i]);
							target != resource.mLiftPassengerTargets.end()) local = target->second;
						local.y += resource.mLiftPosition - transit->getPosition().y;
						if (passenger->getGlobalPosition().distanceTo(
							transit->getPosition() + local) > 0.001f)
							occupantsPositioned = false;
					}
			}
			bool waitingHere = hasWaitingAdmissionAtStop(resource.mLiftCurrentStop);

			bool closeStop = false;
			if (resource.mLiftStopPhase == LiftStopPhase::Boarding)
			{
				if (resource.mOpenPlatformLift)
				{
					// A Platform Lift has one exact admission cutoff. Waiting callers do not
					// extend it, but a passenger admitted before it must finish walking through
					// the crossing lane and selecting a destination before the car closes.
					closeStop = mWorld.mSimulationTick >= resource.mLiftBoardingCutoffTick
						&& !crossing && !reserved && !unresolvedDestination
						&& !resource.mLiftActiveConfirmation;
				}
				else closeStop = mWorld.mSimulationTick
						>= resource.mLiftServiceStartedTick + resource.mLiftMinimumDwellTicks
					&& !crossing && !reserved && !unresolvedDestination && occupantsPositioned
					&& !resource.mLiftActiveConfirmation
					&& (occupied == resource.mCapacity
						|| mWorld.mSimulationTick > resource.mLiftBoardingCutoffTick || !waitingHere);
			}
			if (closeStop)
			{
				if (resource.mOpenPlatformLift)
					for (auto requestId : resource.mAdmissionQueue)
						if (auto request = mWorld.mTraversalRequests.find(requestId); request)
						{
							auto intent = resource.mLiftTripIntents.find(request->mOwner);
							if (intent != resource.mLiftTripIntents.end()
								&& intent->second.originStop == resource.mLiftCurrentStop)
							{
								resource.mOpenPlatformMissedBoarding.insert(requestId);
								addLiftStopRequest(resource, intent->second.originStop, request->mOwner);
								if (auto actor = mWorld.mAgents.find(request->mOwner))
								{
									actor->mTraversalLocalGoal.reset();
									resource.mOpenPlatformMissedPositions[requestId]
										= actor->getGlobalPosition();
								}
							}
						}
				resource.mLiftStopPhase = LiftStopPhase::Closing;
				resource.mLiftTargetStop = ~0u;
			}

			if (resource.mLiftDraining && occupied == 0 && !crossing && !reserved)
			{
				for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
				{
					resource.mLiftStopRequestOwners[stop].clear();
					resource.mLiftStopRequestTicks[stop].clear();
				}
				resource.mLiftDraining = false;
				resource.mLiftTargetStop = ~0u;
				resource.mLiftDirection = TraversalDirection::None;
				resource.mLiftStopPhase = LiftStopPhase::Idle;
			}

			resource.mLiftCarDoorOpen = !resource.mOpenPlatformLift
				&& (resource.mLiftStopPhase == LiftStopPhase::Opening
					|| resource.mLiftStopPhase == LiftStopPhase::Disembarking
					|| resource.mLiftStopPhase == LiftStopPhase::Boarding);
			if (resource.mLift) resource.mLift->setCoordinatedPosition(resource.mLiftPosition);
			else resource.mShuttle->setCoordinatedPosition(resource.mLiftPosition);
			if (resource.mShuttle) refreshShuttlePassengerTargets(resource);
			for (uint32_t i = 0; i < resource.mOccupants.size(); ++i)
				if (auto passenger = mWorld.mAgents.find(resource.mOccupants[i]))
				{
					auto transit = mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get();
					auto local = resource.mCapacityPositions[i];
					if (auto target = resource.mLiftPassengerTargets.find(resource.mOccupants[i]);
						target != resource.mLiftPassengerTargets.end()) local = target->second;
					if (resource.mShuttle)
					{
						// Carry the passenger by the vehicle's translation without changing
						// their position within the carriage, then let Agent locomotion close
						// the remaining distance to the carriage's current spacing target.
						auto vehicleDelta = resource.mLiftPosition - previousVehiclePosition;
						if (abs(vehicleDelta) > 0.0f)
							passenger->setPosition({ transit,
								passenger->getLocalPosition() + Vector2{ vehicleDelta, 0.0f } }, false);
						auto const carriageIndex = i / resource.mShuttleCapacityPerCarriage;
						if (carriageIndex < resource.mShuttleCarriages.size())
						{
							auto const& targets = resource.mShuttleCarriages[carriageIndex].passengerTargets;
							if (auto target = targets.find(resource.mOccupants[i]); target != targets.end())
								local = target->second;
						}
						auto target = transit->getPosition() + local;
						target.x += resource.mLiftPosition - transit->getPosition().x;
						passenger->mTraversalLocalGoal = target;
					}
					else if (resource.mOpenPlatformLift)
					{
						// Platform Lift occupants stand at the same separated capacity positions
						// as enclosed Lift passengers. Translate those positions with the car.
						auto vehicleDelta = resource.mLiftPosition - previousVehiclePosition;
						if (abs(vehicleDelta) > 0.0f)
							passenger->setPosition({ transit,
								passenger->getLocalPosition() + Vector2{ 0.0f, vehicleDelta } }, false);
						auto target = transit->getPosition() + local;
						target.y = resource.mLiftPosition;
						passenger->mTraversalLocalGoal = target;
					}
					else
					{
						local.y += resource.mLiftPosition - transit->getPosition().y;
						auto target = transit->getPosition() + local;
						if (resource.mLiftMoving)
						{
							passenger->setPosition({ transit, local }, false);
							passenger->mTraversalLocalGoal.reset();
						}
						else if (passenger->getGlobalPosition().distanceTo(target) > 0.001f)
							passenger->mTraversalLocalGoal = target;
						else passenger->mTraversalLocalGoal.reset();
					}
				}
		}
	}

	void SimulationCoordinator::advanceDoorResources()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [resourceId, resourcePtr] : mWorld.mTraversalResources.entries())
		{
			auto& resource = *resourcePtr;
			if (!resource.mDoor) continue;
			bool presence = false;
			bool obstruction = false;
			resource.mAutomaticPresenceObserved = false;
			if (resource.mDoorActivationMode == DoorActivationMode::Automatic)
			{
				auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(resource.mDoor);
				if (bulkhead)
				{
					auto const doorCenterX = bulkhead->getPosition().x
						+ bulkhead->getSize().x * 0.5f;
					auto const doorHalfWidth = bulkhead->getSize().x * 0.5f;
					for (int side = 0; side < CORE_NUM_SIDES
						&& !resource.mAutomaticPresenceObserved; ++side)
					{
						auto sector = bulkhead->getSideSector(side);
						if (!sector) continue;
						for (auto agent : sector->getAgents())
						{
							auto const position = agent->getGlobalPosition();
							if (abs(position.y - bulkhead->getPosition().y) > 0.001f) continue;
							auto const distance = max(0.0f, abs(position.x - doorCenterX)
								- doorHalfWidth - agent->getWidth() * 0.5f);
							if (distance <= bulkhead->getAutomaticSensorDistance() + 0.001f)
							{
								resource.mAutomaticPresenceObserved = true;
								break;
							}
						}
					}
					presence = resource.mAutomaticPresenceObserved;
				}
			}
			for (auto const& [sensor, observation] : resource.mSensorObservations)
			{
				(void)sensor;
				presence = presence || observation == DoorSensorObservation::Presence;
				obstruction = obstruction || observation == DoorSensorObservation::Obstruction;
			}
			resource.mDoor->setObstructed(obstruction);
			if ((obstruction || (presence && resource.mDoorActivationMode == DoorActivationMode::Automatic))
				&& resource.mEnabled && !resource.mDoor->isOpen() && !resource.mDoor->isOpening())
			{
				resource.mDoor->requestOpen();
			}

			bool activeCrossing = any_of(resource.mCrossingOwners.begin(), resource.mCrossingOwners.end(),
				[](TraversalRequestId owner) { return (bool)owner; });
			if (!resource.mEnabled && !activeCrossing)
			{
				vector<TraversalRequestId> pending;
				for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
				{
					if (request->mResource == resourceId && request->mState == TraversalRequestState::Pending)
						pending.push_back(requestId);
				}
				for (auto requestId : pending) denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			}
		}
	}

	void SimulationCoordinator::runSimulationPhase(SimulationPhase phase)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.mCurrentPhase = phase;
		auto const timestep = World::getFixedTimestep();

		switch (phase)
		{
		case SimulationPhase::ResourceAdvancement:
			for (auto const& sector : mWorld.mSectors)
			{
				sector->advanceResources(timestep);
			}
			advanceLiftResources();
			advanceDoorResources();
			advanceDeviceOperations();
			break;

		case SimulationPhase::IntentCollection:
			for (auto const& [id, agent] : mWorld.mAgents.entries())
			{
				(void)id;
				if (!agent->isActive()) continue;
				agent->collectTraversalIntent();
			}
			break;

		case SimulationPhase::Allocation:
			for (auto const& [id, agent] : mWorld.mAgents.entries())
			{
				(void)id;
				if (!agent->isActive()) continue;
				agent->allocateTraversal();
			}
			allocateInteractions();
			break;

		case SimulationPhase::Movement:
			// Sample every predecessor before any Agent moves this tick.
			updateQueueStandingTargets();
			for (auto const& [id, agent] : mWorld.mAgents.entries())
			{
				(void)id;
				// A deactivated Agent is not simulated (#118): no phase of the
				// tick moves it, commits for it, or cleans up traversal on its
				// behalf, and its queued door presses are not pressed.
				if (!agent->isActive()) continue;
				auto const movementStart = agent->getGlobalPosition();
				auto const approachingDoor = agent->getState() == Agent::State::MovingToVertex
					|| (agent->getState() == Agent::State::TraversingEdge && agent->mTraversalTask
						&& agent->mTraversalTask->edge
						&& agent->mTraversalTask->edge->getType() != EdgeType::Door);
				agent->update(timestep);
				if (approachingDoor)
					tryPressUpcomingDoorButton(*agent, movementStart, agent->getGlobalPosition());
			}
			moveInteractions(timestep);
			break;

		case SimulationPhase::Commit:
			for (auto const& [id, agent] : mWorld.mAgents.entries())
			{
				(void)id;
				if (!agent->isActive()) continue;
				agent->commitTraversal();
			}
			break;

		case SimulationPhase::CleanupAndEventPublication:
			updateTraversalProgressAndTimeouts();
			for (auto const& [id, agent] : mWorld.mAgents.entries())
			{
				(void)id;
				if (!agent->isActive()) continue;
				agent->cleanupTraversal();
			}
			updateInteractionResults();
			// Expiry follows a complete stationary tick; movement starts next tick.
			advanceRoutePlanning();
			updateMovementGoals();
			break;

		case SimulationPhase::None:
			break;
		}
	}

	void SimulationCoordinator::touchDeviceOperation(DeviceOperationId id, DeviceOperation const& operation)
	{
		if (mWorld.mRecordingTickChanges
			&& (!mWorld.mTickOperationLimit || id.value < mWorld.mTickOperationLimit))
			mWorld.mTickOperations.try_emplace(id, operation.getState());
	}

	void SimulationCoordinator::publishTickEvents()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto phase : { SimulationPhase::ResourceAdvancement, SimulationPhase::IntentCollection,
			SimulationPhase::Allocation, SimulationPhase::Movement, SimulationPhase::Commit,
			SimulationPhase::CleanupAndEventPublication })
		{
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::PhaseCompleted;
			event.phase = phase;
			mWorld.mEvents.push_back(std::move(event));
		}

		for (auto const& [id, previous] : mWorld.mTickAgents)
		{
			auto agent = mWorld.mAgents.find(id);
			if (!agent) continue;
			auto current = makeAgentSnapshot(agent);
			auto changed = current.sectorId != previous.sectorId
				|| current.localPosition != previous.localPosition
				|| current.globalPosition != previous.globalPosition
				|| current.state != previous.state
				|| current.active != previous.active
				|| current.routePlanningRemainingTicks != previous.routePlanningRemainingTicks
				|| current.intendedDestination != previous.intendedDestination
				|| current.hasPath != previous.hasPath
				|| current.targetPathNode != previous.targetPathNode
				|| current.pathNodeCount != previous.pathNodeCount
				|| current.hasLocomotionTask != previous.hasLocomotionTask
				|| current.traversalRequest != previous.traversalRequest
				|| current.traversalPermit != previous.traversalPermit
				|| current.interactionRequest != previous.interactionRequest;

			if (changed)
			{
				SimulationEvent event;
				event.sequence = mWorld.mNextEventSequence++;
				event.tick = mWorld.mSimulationTick;
				event.type = SimulationEventType::AgentChanged;
				event.phase = SimulationPhase::CleanupAndEventPublication;
				event.hasPreviousAgent = true;
				event.previousAgent = previous;
				event.agent = current;
				mWorld.mEvents.push_back(std::move(event));
			}
		}

		for (auto const& [id, previous] : mWorld.mTickOperations)
		{
			auto operation = mWorld.mDeviceOperations.find(id);
			if (operation && operation->getState() != previous)
			{
				SimulationEvent event;
				event.sequence = mWorld.mNextEventSequence++;
				event.tick = mWorld.mSimulationTick;
				event.type = SimulationEventType::DeviceOperationChanged;
				event.phase = SimulationPhase::CleanupAndEventPublication;
				event.deviceOperation = makeDeviceOperationSnapshot(id, *operation);
				mWorld.mEvents.push_back(std::move(event));
			}
		}
	}

	bool SimulationCoordinator::advanceTick()
	{
		if (mWorld.mSimulationPaused) return false;
		auto const stepStartedAt = SimulationStepTiming::Clock::now();
		auto const eventStart = mWorld.mEvents.size();
		mWorld.invalidateSimulationSnapshot();
		// The boundary runs with no active phase. Instances are synchronized before
		// deterministic startup/outcome callbacks enqueue commands; those commands
		// are applied here before this tick can collect traversal intent.
		if (!mWorld.mAgentBehaviourRuntime->runBoundary(mWorld)) return false;
		mWorld.invalidateSimulationSnapshot();
		mWorld.mTickAgents.clear();
		// Every active Agent participates in the phases, including stationary ones.
		// Capture after behaviour commands, matching the historical tick boundary.
		for (auto const& [id, agent] : mWorld.mAgents.entries())
			if (agent->isActive()) mWorld.mTickAgents.emplace(id, makeAgentSnapshot(agent.get()));
		mWorld.mTickOperations.clear();
		mWorld.mTickOperationLimit = mWorld.mDeviceOperations.nextId();
		mWorld.mRecordingTickChanges = true;
		++mWorld.mSimulationTick;
		// A tick boundary is the only safe place to retire terminal records: the
		// previous tick has already published its transitions and offered every
		// waiting traversal its chance to observe, and this tick's owners have
		// not yet started. Records a live owner still names are never retired.
		retireConsumedCoordination();
		updateMovementGoals();

		runSimulationPhase(SimulationPhase::ResourceAdvancement);
		runSimulationPhase(SimulationPhase::IntentCollection);
		runSimulationPhase(SimulationPhase::Allocation);
		runSimulationPhase(SimulationPhase::Movement);
		runSimulationPhase(SimulationPhase::Commit);
		runSimulationPhase(SimulationPhase::CleanupAndEventPublication);
		mWorld.invalidateSimulationSnapshot();
		publishTickEvents();
		if (mWorld.mSimulationObserver)
		{
			// Copy only this tick's publication; never drain the consumer queue.
			try
			{
				std::vector<SimulationEvent> events(mWorld.mEvents.begin() + eventStart, mWorld.mEvents.end());
				mWorld.mSimulationObserver->onTick(mWorld.mSimulationTick, events);
			}
			catch (std::bad_alloc const&) { /* Observation must fail soft. */ }
		}
		mWorld.mRecordingTickChanges = false;
		mWorld.mCurrentPhase = SimulationPhase::None;
		auto const stepCompletedAt = SimulationStepTiming::Clock::now();
		mWorld.mSimulationStepTiming.record(stepCompletedAt - stepStartedAt, stepCompletedAt);
		return true;
	}

	bool SimulationCoordinator::advanceTicks(uint64_t count)
	{
		for (uint64_t i = 0; i < count; ++i)
			if (!advanceTick()) return false;
		return true;
	}

	void SimulationCoordinator::update(float elapsedSeconds)
	{
		if (mWorld.mSimulationPaused) return;
		if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f)
		{
			return;
		}

		mWorld.mAccumulatedTime += static_cast<double>(elapsedSeconds) * mWorld.mTimeScale;
		auto const timestep = (double)World::getFixedTimestep();
		uint32_t executed = 0;
		// The fixed timestep is a float. Allow its accumulated representation
		// error across the bounded batch (e.g. 60 ticks for exactly one second),
		// then retain the existing correction of a tiny negative remainder.
		while (executed < World::getMaxTicksPerUpdate()
			&& mWorld.mAccumulatedTime + timestep * 1e-4 >= timestep)
		{
			if (!advanceTick()) break;
			mWorld.mAccumulatedTime -= timestep;
			++executed;
		}

		if (mWorld.mAccumulatedTime < 0.0)
		{
			mWorld.mAccumulatedTime = 0.0;
		}
	}

	uint64_t SimulationCoordinator::getSimulationTick() const
	{
		return mWorld.mSimulationTick;
	}

	SimulationPhase SimulationCoordinator::getCurrentSimulationPhase() const
	{
		return mWorld.mCurrentPhase;
	}

	vector<SimulationEvent> SimulationCoordinator::consumeSimulationEvents()
	{
		auto result = std::move(mWorld.mEvents);
		mWorld.mEvents.clear();
		return result;
	}

	void SimulationCoordinator::publishTopologyEvent(SimulationEventType type, string diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = type;
		event.phase = SimulationPhase::None;
		event.diagnostic = std::move(diagnostic);
		mWorld.mEvents.push_back(std::move(event));
	}

	void SimulationCoordinator::cancelTraversalForTopologyRebuild(Agent& agent)
	{
		if (agent.mState == Agent::State::RoutePlanning) return;
		if (agent.mTraversalTask)
		{
			// A threshold crossing which has not committed still belongs to its source
			// sector. Put it back on that safe boundary before releasing its permit.
			// A Location edge within one sector is ordinary walking, not a crossing:
			// its current position is already safe and must survive pause unchanged.
			auto const& task = *agent.mTraversalTask;
			auto const localWalk = task.edge && task.edge->getType() == EdgeType::Location
				&& task.sourceVertex && task.destinationVertex
				&& task.sourceVertex->getSector() == task.destinationVertex->getSector();
			// Stairs are continuous walking surfaces. Their in-flight position is a safe
			// pause position, just like same-sector walking.
			auto const continuousStair = task.edge
				&& (task.edge->getType() == EdgeType::Staircase
					|| task.edge->getType() == EdgeType::Stairwell);
			if (!localWalk && !continuousStair && (agent.mState == Agent::State::TraversingEdge
				|| agent.mState == Agent::State::AwaitingTraversalCommit)
				&& task.sourceVertex && agent.getSector())
			{
				auto source = agent.mTraversalTask->sourceVertex->getPosition();
				agent.setPosition({ const_cast<Sector*>(agent.getSector()),
					source - agent.getSector()->getPosition() }, false);
			}
			cancelTraversal(agent.mTraversalTask->request, agent.mTraversalTask->permit, false);
			releaseTraversal(agent.mTraversalTask->request, agent.mTraversalTask->permit);
			agent.mTraversalTask.reset();
			agent.mTraversalLocalGoal.reset();
		}
		if (agent.mQueuedTraversalTask)
		{
			cancelTraversal(agent.mQueuedTraversalTask->request,
				agent.mQueuedTraversalTask->permit, false);
			releaseTraversal(agent.mQueuedTraversalTask->request,
				agent.mQueuedTraversalTask->permit);
			agent.mQueuedTraversalTask.reset();
		}
		agent.mPath.path.reset();
		agent.mPath.targetNode = 0;
		agent.mState = Agent::State::Idle;
	}

	void SimulationCoordinator::restorePausedPathIntents()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [id, intent] : mWorld.mPausedPathIntents)
		{
			auto agent = mWorld.mAgents.find(id);
			// A deactivated Agent is not simulated (#118): its retained route must
			// not be replayed onto the graph. The intent still drops with the map,
			// so reactivation later does not resurrect a route the pause had
			// already torn down.
			if (!agent || !agent->isActive() || !agent->getSector()) continue;
			auto goal = mWorld.mMovementGoals.find(id);
			try
			{
				shared_ptr<const Vertex> destination;
				if (goal != mWorld.mMovementGoals.end())
				{
					// Marker identity survives structural replay. Re-resolve its new graph
					// vertex rather than restoring a stale Sector/position pair.
					for (auto const& sector : mWorld.mSectors)
						for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
							if (auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
								object && object->getMarker()->getId() == goal->second.marker)
								destination = mWorld.mGraph->getVertexForObject(object);
					if (!destination)
					{
						goal->second.routeLossReason = RouteLossReason::DestinationRemoved;
						continue;
					}
					goal->second.position = destination->getPosition();
					goal->second.sector = SectorId{
						(uint64_t)destination->getSector()->getIndex() + 1 };
				}
				else
				{
					if (!intent.destinationSector
						|| intent.destinationSector.value > mWorld.mSectors.size())
					{
						continue;
					}
					auto destinationSector = mWorld.mSectors[
						(size_t)intent.destinationSector.value - 1];
					destination = mWorld.mGraph->getClosestVertexInSector(
						destinationSector.get(), intent.destinationPosition);
				}
				auto path = mWorld.mGraph->calculatePath(agent, destination);
				if (path && !path->nodes.empty())
				{
					agent->assignPath(std::move(path), intent.wasPathing, false);
					if (intent.wasPathing && intent.resumeContinuousTraversal)
					{
						for (uint32_t node = 1; node < agent->mPath.path->nodes.size(); ++node)
						{
							auto const& edge = agent->mPath.path->nodes[node].edge;
							auto const& target = agent->mPath.path->nodes[node].targetVertex;
							if (!edge || !target || edge->getType() != intent.traversalEdgeType
								|| target->getPosition().distanceTo(
									intent.traversalDestinationPosition) > 0.001f) continue;
							auto const source = edge->getOtherVertex(target);
							if (!source || source->getPosition().distanceTo(
								intent.traversalSourcePosition) > 0.001f) continue;
							agent->mPath.targetNode = node - 1;
							agent->mState = Agent::State::WaitingForTraversal;
							break;
						}
					}
					if (goal != mWorld.mMovementGoals.end())
						goal->second.routeLossReason = RouteLossReason::None;
				}
				else if (goal != mWorld.mMovementGoals.end())
					goal->second.routeLossReason = RouteLossReason::TopologyChanged;
			}
			catch (Exception const&)
			{
				// The destination was structurally removed or disconnected. The Agent
				// remains safely idle; this does not invalidate otherwise usable topology.
				if (goal != mWorld.mMovementGoals.end())
					goal->second.routeLossReason = RouteLossReason::TopologyChanged;
			}
		}
		mWorld.mPausedPathIntents.clear();
	}

	void SimulationCoordinator::cancelAllTraversalForTopologyRebuild()
	{
		mWorld.mPausedPathIntents.clear();
		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			if (agent->mPath.path && !agent->mPath.path->nodes.empty())
			{
				auto destination = agent->mPath.path->nodes.back().targetVertex;
				if (destination && destination->getSector())
				{
					auto& intent = mWorld.mPausedPathIntents[id];
					intent.destinationSector = SectorId{
						(uint64_t)destination->getSector()->getIndex() + 1 };
					intent.destinationPosition = destination->getPosition();
					intent.wasPathing = agent->mState != Agent::State::Idle;
					intent.routeDiagnostics = pathing::getRouteDiagnostics(*agent->mPath.path);
					if (agent->mState == Agent::State::TraversingEdge && agent->mTraversalTask
						&& agent->mTraversalTask->edge && agent->mTraversalTask->sourceVertex
						&& agent->mTraversalTask->destinationVertex)
					{
						auto const type = agent->mTraversalTask->edge->getType();
						intent.resumeContinuousTraversal = type == EdgeType::Staircase
							|| type == EdgeType::Stairwell;
						intent.traversalEdgeType = type;
						intent.traversalSourcePosition =
							agent->mTraversalTask->sourceVertex->getPosition();
						intent.traversalDestinationPosition =
							agent->mTraversalTask->destinationVertex->getPosition();
					}
				}
			}
			cancelTraversalForTopologyRebuild(*agent);
		}

		// Defensive cleanup also handles requests whose owning Agent was removed or
		// whose task was already detached. Typed IDs are never recycled.
		vector<TraversalRequestId> orphaned;
		for (auto const& [id, request] : mWorld.mTraversalRequests.entries())
		{
			(void)request;
			orphaned.push_back(id);
		}
		for (auto id : orphaned)
		{
			auto request = mWorld.mTraversalRequests.find(id);
			auto permit = request ? request->mPermit : TraversalPermitId{};
			cancelTraversal(id, permit, false);
			releaseTraversal(id, permit);
		}
	}

} // core
