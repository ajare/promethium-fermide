#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/OccupantPacking.h"
#include "core/Shuttle.h"
#include "core/Path.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// Lift scheduling, passenger safe exits, the lift allocation dispatcher and the
	// boarding, riding and disembarking branches of lift allocation moved out of
	// World (ADR 0004 stage 3). The behaviour is unchanged: the coordinator
	// works on World's traversal-resource, traversal-request,
	// interaction-request and agent registries through friendship and calls its
	// own landing queue ticket attach, queue position refresh, grants and
	// denials directly - that queue and admission core joined the coordinator in
	// stage 4. The shuttle passenger-carriage lookup these used to call back for
	// has moved in with the shuttle door assignment family
	// (SimulationCoordinatorShuttles.cpp) and is called directly. No facade
	// callback is left in this seam.
	//
	// These were helpers with no entry points of their own until the dispatcher and
	// the boarding, riding and disembarking branches of lift allocation joined
	// them: every caller reaches the scheduling helpers either from inside the
	// coordinator or through the World facade (design pattern, not the Facade
	// sector type), while allocateLiftTraversal, allocateLiftBoarding,
	// allocateLiftRiding and allocateLiftDisembarking are reached only from
	// inside the coordinator - the dispatcher from the coordinator's own
	// traversal-request allocation, the branches from the dispatcher.

	bool SimulationCoordinator::setLiftBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mLift) return false;
		return setTransportBroken(id, broken);
	}

	bool SimulationCoordinator::setShuttleBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mShuttle) return false;
		return setTransportBroken(id, broken);
	}

	bool SimulationCoordinator::setTransportBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || (!resource->mLift && !resource->mShuttle)) return false;
		auto& condition = resource->mShuttle ? resource->mShuttle->mBroken : resource->mLift->mBroken;
		if (condition == broken) return true;
		mWorld.invalidateSimulationSnapshot();
		condition = broken;
		// Include every supported Shuttle landing, not only the representative
		// Door stored on each Stop. Owned Doors cannot fail independently.
		for (auto const& [landingId, landing] : mWorld.mTraversalResources.entries())
		{
			(void)landingId;
			if (landing->mLiftCoordinator == id && landing->mDoor) landing->mDoor->mBroken = broken;
		}
		if (broken)
		{
			// A moving Shuttle's last spacing goal includes its last translation.
			// Do not let locomotion finish that stale translation after service freezes.
			if (resource->mShuttle && resource->mLiftMoving)
				for (auto occupant : resource->mOccupants)
					if (auto passenger = mWorld.mAgents.find(occupant)) passenger->mTraversalLocalGoal.reset();
			for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
			{
				auto target = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!operation->mHasCommand || !target
					|| (target != resource && target->mLiftCoordinator != id)
					|| (operation->mState != DeviceOperationState::Pending
						&& operation->mState != DeviceOperationState::Running)) continue;
				touchDeviceOperation(operationId, *operation);
				operation->mState = DeviceOperationState::Failed;
			}
			// Retain onboard intent and occupancy, but not a failed selector request.
			// Restoration must make a fresh request rather than exhaust retries.
			for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
			{
				(void)requestId;
				if (request->mResource != id || request->mState != TraversalRequestState::Pending) continue;
				for (auto const& [interactionId, interaction] : mWorld.mInteractionRequests.entries())
					if (interaction->mActor == request->mOwner && interaction->mResult == InteractionResult::Pending)
						cancelInteraction(interactionId);
				request->mPreparationRequested = false;
				request->mPreparationOperation = {};
				request->mPreparationAttempts = 0;
			}
		}
		// Do not notify Agents; only local observation changes their knowledge.
		return true;
	}

	uint32_t SimulationCoordinator::findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const
	{
		uint32_t best = ~0u;
		float distance = 0.0f;
		for (uint32_t i = 0; i < resource.mLiftStops.size(); ++i)
		{
			auto coordinate = resource.mShuttle ? endpoint.x : endpoint.y;
			auto candidate = abs(resource.mLiftStops[i].globalPosition - coordinate);
			if (best == ~0u || candidate < distance)
			{
				best = i;
				distance = candidate;
			}
		}
		return best;
	}

	uint32_t SimulationCoordinator::findAgentLiftDestination(Agent const& agent,
		TraversalResource const& resource) const
	{
		if (!agent.mPath.path) return ~0u;
		uint32_t destination = ~0u;
		bool foundRide = false;
		for (uint32_t i = agent.mPath.targetNode + 1; i < agent.mPath.path->nodes.size(); ++i)
		{
			auto const& node = agent.mPath.path->nodes[i];
			if (!node.edge) continue;
			if ((node.edge->getType() == EdgeType::Lift
				|| node.edge->getType() == EdgeType::Shuttle) && node.targetVertex)
			{
				foundRide = true;
				destination = findLiftStop(resource, node.targetVertex->getPosition());
				continue;
			}
			// A contiguous set of ride edges is one journey. Stop at its
			// disembark edge rather than accidentally inspecting a later lift.
			if (foundRide) break;
		}
		return destination;
	}

	void SimulationCoordinator::orderLiftOccupants(TraversalResource& resource)
	{
		if (!resource.mLift || resource.mOpenPlatformLift || resource.mLiftMoving) return;

		// Lift capacity positions are authored from the doors into the car. Walk the
		// manifest in the opposite direction so the first passenger fills the far
		// end. Keeping this order for equal destinations also keeps boarding order
		// without introducing a second source of manifest identity.
		vector<uint32_t> occupiedPositions;
		vector<AgentId> passengers;
		for (uint32_t i = (uint32_t)resource.mOccupants.size(); i-- > 0;)
			if (resource.mOccupants[i])
			{
				occupiedPositions.push_back(i);
				passengers.push_back(resource.mOccupants[i]);
			}

		auto destinationProgress = [&](AgentId passenger)
		{
			auto destination = resource.mLiftPassengerDestinations.find(passenger);
			uint32_t stop = destination == resource.mLiftPassengerDestinations.end()
				? ~0u : destination->second;
			if (stop >= resource.mLiftStops.size())
			{
				auto intent = resource.mLiftTripIntents.find(passenger);
				if (intent != resource.mLiftTripIntents.end()) stop = intent->second.destinationStop;
			}
			if (stop >= resource.mLiftStops.size()) return numeric_limits<float>::infinity();
			auto const delta = resource.mLiftStops[stop].globalPosition - resource.mLiftPosition;
			return resource.mLiftDirection == TraversalDirection::Descending ? -delta : delta;
		};
		stable_sort(passengers.begin(), passengers.end(), [&](AgentId left, AgentId right)
		{
			return destinationProgress(left) > destinationProgress(right);
		});
		for (size_t rank = 0; rank < passengers.size(); ++rank)
			resource.mOccupants[occupiedPositions[rank]] = passengers[rank];
	}

	void SimulationCoordinator::respaceLiftOccupantsAfterAlighting(TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		resource.mLiftPassengerTargets.clear();
		if (!resource.mLift || resource.mOpenPlatformLift || resource.mLiftMoving
			|| resource.mCapacityPositions.empty()) return;

		vector<AgentId> passengers;
		for (auto occupant : resource.mOccupants)
		{
			if (!occupant) continue;
			auto destination = resource.mLiftPassengerDestinations.find(occupant);
			auto const alightingHere = destination != resource.mLiftPassengerDestinations.end()
				&& destination->second == resource.mLiftCurrentStop;
			if (!alightingHere && !resource.mLiftExitAtSafeStop.contains(occupant))
				passengers.push_back(occupant);
		}
		if (passengers.empty()) return;

		// Capacity positions describe the complete body-safe car extent. Packing the
		// smaller remaining group in that same extent changes targets without changing
		// capacity ownership, leaving room for the passenger walking to the Doors.
		auto const targets = packOccupants(passengers.size(), 0,
			{ resource.mCapacityPositions.front().x, resource.mCapacityPositions.back().x },
			CORE_AGENT_MAX_WIDTH, mWorld.mTraversalGeometryPolicy.occupantClearance,
			OccupantPackingOrder::Forward, OccupantPackingLayout::Compact);
		for (size_t rank = 0; rank < passengers.size(); ++rank)
			resource.mLiftPassengerTargets[passengers[rank]] = {
				targets[rank], resource.mCapacityPositions.front().y };
	}

	bool SimulationCoordinator::liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const
	{
		for (auto occupant : resource.mOccupants)
		{
			if (!occupant) continue;
			auto destination = resource.mLiftPassengerDestinations.find(occupant);
			if (destination != resource.mLiftPassengerDestinations.end() && destination->second == stop)
				return true;
		}
		return false;
	}

	void SimulationCoordinator::addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!owner || stop >= resource.mLiftStopRequestOwners.size()) return;
		if (resource.mLiftStopRequestOwners[stop].insert(owner).second)
			resource.mLiftStopRequestTicks[stop][owner] = mWorld.mSimulationTick;
	}

	void SimulationCoordinator::removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!owner || stop >= resource.mLiftStopRequestOwners.size()) return;
		resource.mLiftStopRequestOwners[stop].erase(owner);
		resource.mLiftStopRequestTicks[stop].erase(owner);
	}

	uint32_t SimulationCoordinator::chooseNextLiftStop(TraversalResource& resource) const
	{
		auto requested = [&](uint32_t stop)
		{
			return stop < resource.mLiftStopRequestOwners.size()
				&& !resource.mLiftStopRequestOwners[stop].empty();
		};
		auto position = resource.mLiftPosition;
		auto nearestInDirection = [&](TraversalDirection direction)
		{
			uint32_t selected = ~0u;
			float selectedDistance = 0.0f;
			for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
			{
				if (!requested(stop) || stop == resource.mLiftCurrentStop) continue;
				auto hasCompatibleOwner = any_of(resource.mLiftStopRequestOwners[stop].begin(),
					resource.mLiftStopRequestOwners[stop].end(), [&](AgentId owner)
					{
						if (resource.mLiftPassengerDestinations.contains(owner)) return true;
						auto intent = resource.mLiftTripIntents.find(owner);
						if (intent == resource.mLiftTripIntents.end()
							|| intent->second.destinationStop >= resource.mLiftStops.size()) return true;
						auto desired = resource.mLiftStops[intent->second.destinationStop].globalPosition
							> resource.mLiftStops[intent->second.originStop].globalPosition
							? TraversalDirection::Ascending : TraversalDirection::Descending;
						return desired == direction;
					});
				if (!hasCompatibleOwner) continue;
				auto delta = resource.mLiftStops[stop].globalPosition - position;
				if ((direction == TraversalDirection::Ascending && delta <= 0.0f)
					|| (direction == TraversalDirection::Descending && delta >= 0.0f)) continue;
				auto distance = abs(delta);
				if (selected == ~0u || distance < selectedDistance
					|| (distance == selectedDistance && stop < selected))
				{
					selected = stop;
					selectedDistance = distance;
				}
			}
			return selected;
		};

		if (resource.mLiftDirection != TraversalDirection::None)
		{
			auto selected = nearestInDirection(resource.mLiftDirection);
			if (selected != ~0u) return selected;
			auto reverse = resource.mLiftDirection == TraversalDirection::Ascending
				? TraversalDirection::Descending : TraversalDirection::Ascending;
			selected = nearestInDirection(reverse);
			if (selected != ~0u)
			{
				resource.mLiftDirection = reverse;
				return selected;
			}
		}

		// Idle dispatch is based on the oldest individual interest. Actual distance
		// and stable stop ID resolve simultaneous calls deterministically.
		uint32_t selected = ~0u;
		uint64_t selectedTick = 0;
		float selectedDistance = 0.0f;
		for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
		{
			if (!requested(stop)) continue;
			auto oldest = min_element(resource.mLiftStopRequestTicks[stop].begin(),
				resource.mLiftStopRequestTicks[stop].end(), [](auto const& left, auto const& right)
				{ return left.second != right.second ? left.second < right.second : left.first < right.first; });
			if (oldest == resource.mLiftStopRequestTicks[stop].end()) continue;
			auto distance = abs(resource.mLiftStops[stop].globalPosition - position);
			if (selected == ~0u || oldest->second < selectedTick
				|| (oldest->second == selectedTick && (distance < selectedDistance
					|| (distance == selectedDistance && stop < selected))))
			{
				selected = stop;
				selectedTick = oldest->second;
				selectedDistance = distance;
			}
		}
		if (selected != ~0u)
		{
			auto delta = resource.mLiftStops[selected].globalPosition - position;
			resource.mLiftDirection = delta > 0.0f ? TraversalDirection::Ascending
				: delta < 0.0f ? TraversalDirection::Descending : TraversalDirection::None;
		}
		return selected;
	}

	bool SimulationCoordinator::isLiftBoardingDirectionCompatible(TraversalResource& resource,
		uint32_t originStop, uint32_t destinationStop)
	{
		if (originStop >= resource.mLiftStops.size() || destinationStop >= resource.mLiftStops.size()
			|| originStop == destinationStop) return false;
		auto desired = resource.mLiftStops[destinationStop].globalPosition
			> resource.mLiftStops[originStop].globalPosition
			? TraversalDirection::Ascending : TraversalDirection::Descending;
		if (resource.mLiftDirection == TraversalDirection::None
			|| resource.mLiftDirection == desired)
		{
			resource.mLiftDirection = desired;
			return true;
		}
		// An admitted boarder commits this run's direction before its destination
		// becomes an onboard stop request. Reversing now strands its reservation.
		if (any_of(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(),
			[](auto reservation) { return (bool)reservation; })) return false;
		// Reverse at this stop only after LOOK has exhausted demand ahead.
		for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
		{
			if (stop == originStop || resource.mLiftStopRequestOwners[stop].empty()) continue;
			auto delta = resource.mLiftStops[stop].globalPosition - resource.mLiftPosition;
			if ((resource.mLiftDirection == TraversalDirection::Ascending && delta <= 0.0f)
				|| (resource.mLiftDirection == TraversalDirection::Descending && delta >= 0.0f)) continue;
			for (auto owner : resource.mLiftStopRequestOwners[stop])
			{
				if (resource.mLiftPassengerDestinations.contains(owner)) return false;
				auto intent = resource.mLiftTripIntents.find(owner);
				if (intent == resource.mLiftTripIntents.end()) return false;
				auto ownerDirection = resource.mLiftStops[intent->second.destinationStop].globalPosition
					> resource.mLiftStops[intent->second.originStop].globalPosition
					? TraversalDirection::Ascending : TraversalDirection::Descending;
				if (ownerDirection == resource.mLiftDirection) return false;
			}
		}
		resource.mLiftDirection = desired;
		return true;
	}

	void SimulationCoordinator::releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		if (auto request = mWorld.mTraversalRequests.find(requestId);
			request && (request->mSourceSector != resource.mLiftSector || resource.mOpenPlatformLift))
		{
			auto intent = resource.mLiftTripIntents.find(request->mOwner);
			if (intent != resource.mLiftTripIntents.end())
			{
				removeLiftStopRequest(resource, intent->second.originStop, request->mOwner);
				resource.mLiftTripIntents.erase(intent);
			}
		}
		auto const oldQueueSize = resource.mAdmissionQueue.size();
		resource.mAdmissionQueue.erase(remove(resource.mAdmissionQueue.begin(),
			resource.mAdmissionQueue.end(), requestId), resource.mAdmissionQueue.end());
		if (resource.mAdmissionQueue.size() != oldQueueSize) ++resource.mRouteQueueEpoch;
		resource.mLiftConfirmationQueue.erase(remove(resource.mLiftConfirmationQueue.begin(),
			resource.mLiftConfirmationQueue.end(), requestId), resource.mLiftConfirmationQueue.end());
		if (resource.mLiftActiveConfirmation == requestId)
			resource.mLiftActiveConfirmation = resource.mLiftConfirmationQueue.empty()
				? TraversalRequestId{} : resource.mLiftConfirmationQueue.front();
		for (auto& reservation : resource.mAdmissionReservations)
			if (reservation == requestId) reservation = {};
		if (resource.mLiftAdmissionReservation == requestId) resource.mLiftAdmissionReservation = {};
		if (resource.mOpenPlatformLift)
		{
			for (auto& boundaryOwner : resource.mVirtualBoundaryOwners)
				if (boundaryOwner == requestId) boundaryOwner = {};
			resource.mOpenPlatformMissedBoarding.erase(requestId);
			resource.mOpenPlatformMissedPositions.erase(requestId);
			for (auto& lane : resource.mQueueLanes)
				lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
			if (auto request = mWorld.mTraversalRequests.find(requestId))
			{
				request->mQueuePosition = ~0u;
				request->mQueueApproach = ~0u;
				if (auto agent = mWorld.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();
			}
			refreshQueuePositions(resource);
		}
		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			request->mCapacityPosition = ~0u;
			request->mShuttleAlightingDoor = {};
		}
	}

	void SimulationCoordinator::requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [resourceId, resourcePtr] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if ((!resource.mLift && !resource.mShuttle)
				|| find(resource.mOccupants.begin(), resource.mOccupants.end(), passenger)
				== resource.mOccupants.end()) continue;
			for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
				removeLiftStopRequest(resource, stop, passenger);
			resource.mLiftPassengerDestinations.erase(passenger);
			resource.mLiftExitAtSafeStop.insert(passenger);
			auto failure = resource.mLiftExitFailures.find(passenger);
			if (failure == resource.mLiftExitFailures.end()
				|| failure->second == TraversalFailureReason::None)
				resource.mLiftExitFailures[passenger] = reason;
			auto safeStop = resource.mLiftMoving && resource.mLiftTargetStop < resource.mLiftStops.size()
				? resource.mLiftTargetStop : resource.mLiftCurrentStop;
			addLiftStopRequest(resource, safeStop, passenger);
			return;
		}
	}

	void SimulationCoordinator::assignLiftSafeExitPaths(TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		if (resource.mLiftMoving || resource.mLiftCurrentStop >= resource.mLiftStops.size()
			|| (!(resource.mShuttle && resource.mShuttle->isBroken())
				&& resource.mLiftStopPhase != LiftStopPhase::Opening
				&& resource.mLiftStopPhase != LiftStopPhase::Disembarking
				&& resource.mLiftStopPhase != LiftStopPhase::Boarding)) return;
		vector<AgentId> assigned;
		vector<AgentId> gone;
		for (auto passenger : resource.mLiftExitAtSafeStop)
		{
			if (resource.mOpenPlatformLift)
			{
				for (auto& occupant : resource.mOccupants) if (occupant == passenger) occupant = {};
				for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
					removeLiftStopRequest(resource, stop, passenger);
				resource.mLiftPassengerDestinations.erase(passenger);
				resource.mLiftExitFailures.erase(passenger);
				if (auto agent = mWorld.mAgents.find(passenger))
				{
					auto location = mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get();
					auto global = agent->getGlobalPosition();
					global.y = resource.mLiftPosition;
					agent->setPosition({ location, global - location->getPosition() }, false);
					if (agent->getState() != Agent::State::Idle) agent->clearRuntimePath();
				}
				assigned.push_back(passenger);
				continue;
			}
			auto agent = mWorld.mAgents.find(passenger);
			if (!agent || agent->getSector() != mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get())
			{
				// The passenger is no longer here. Forgetting it from the pending-exit
				// set alone would leave the manifest slot standing forever (#57).
				gone.push_back(passenger);
				continue;
			}
			auto landingId = resource.mLiftStops[resource.mLiftCurrentStop].landingResource;
			if (resource.mShuttle)
			{
				auto carriage = findShuttlePassengerCarriage(resource, passenger);
				auto selectedPosition = numeric_limits<float>::quiet_NaN();
				if (carriage < resource.mShuttleCarriages.size())
					if (auto selected = resource.mShuttleCarriages[carriage].alightingDoors.find(passenger);
						selected != resource.mShuttleCarriages[carriage].alightingDoors.end())
						if (auto original = find_if(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
							[&](auto const& value) { return value.landingResource == selected->second; });
							original != resource.mShuttleDoors.end())
							selectedPosition = original->carriagePosition;
				auto door = find_if(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
					[&](auto const& value) { return value.stopIndex == resource.mLiftCurrentStop
						&& value.carriageIndex == carriage
						&& (isnan(selectedPosition)
							|| abs(value.carriagePosition - selectedPosition) < 0.001f); });
				if (door != resource.mShuttleDoors.end())
				{
					landingId = door->landingResource;
					resource.mShuttleCarriages[carriage].alightingDoors[passenger] = landingId;
				}
			}
			if (resource.mShuttle && resource.mShuttle->isBroken())
			{
				auto landing = mWorld.mTraversalResources.find(landingId);
				if (!landing || !landing->mDoor || !landing->mDoor->isOpen()) continue;
			}
			shared_ptr<const Edge> landingEdge;
			shared_ptr<const Vertex> source;
			shared_ptr<const Vertex> destination;
			for (auto const& edge : mWorld.mGraph->getEdges())
			{
				if (edge->getTraversalResourceId() != landingId) continue;
				auto first = edge->getVertex(0);
				auto second = edge->getVertex(1);
				if (SectorId{ (uint64_t)first->getSector()->getIndex() + 1 } == resource.mLiftSector)
				{ source = first; destination = second; }
				else if (SectorId{ (uint64_t)second->getSector()->getIndex() + 1 } == resource.mLiftSector)
				{ source = second; destination = first; }
				if (source) { landingEdge = edge; break; }
			}
			if (!landingEdge) continue;
			auto path = make_shared<Path>();
			path->nodes.push_back({ nullptr, source, 0.0f, std::nullopt, std::nullopt });
			// This safety Path is a runtime coordination command, not a route-choice
			// result; crossing remains governed by the landing permit and physical timing.
			path->nodes.push_back({ landingEdge, destination, 0.0f, std::nullopt, std::nullopt });
			agent->assignPath(std::move(path), true, false);
			assigned.push_back(passenger);
		}
		for (auto passenger : assigned) resource.mLiftExitAtSafeStop.erase(passenger);
		// Released outside the loop: releaseAgentFromResource() also prunes the
		// pending-exit set, which this loop is still iterating.
		for (auto passenger : gone) releaseAgentFromResource(resource, passenger);
	}

	bool SimulationCoordinator::replaceOnboardLiftDestination(Agent& agent, shared_ptr<Path> const& path,
		uint32_t& sourceNode)
	{
		mWorld.invalidateSimulationSnapshot();
		auto owner = getAgentId(&agent);
		if (!owner || !path) return false;
		for (auto const& [resourceId, resourcePtr] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if ((!resource.mLift && !resource.mShuttle) || !resource.mEnabled
				|| find(resource.mOccupants.begin(), resource.mOccupants.end(), owner) == resource.mOccupants.end()) continue;
			for (uint32_t i = 0; i + 1 < path->nodes.size(); ++i)
			{
				auto const& node = path->nodes[i + 1];
				if (!node.edge || (node.edge->getType() != EdgeType::Lift
					&& node.edge->getType() != EdgeType::Shuttle) || !node.targetVertex) continue;
				auto destinationStop = findLiftStop(resource, node.targetVertex->getPosition());
				if (destinationStop >= resource.mLiftStops.size()) return false;
				for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
					removeLiftStopRequest(resource, stop, owner);
				resource.mLiftPassengerDestinations.erase(owner);
				// Allocation below will either share an already-active destination or
				// serialize a fresh selection at the interior control.
				resource.mLiftExitAtSafeStop.erase(owner);
				resource.mLiftExitFailures.erase(owner);
				vector<InteractionRequestId> obsoleteSelections;
				for (auto const& [interactionId, interaction] : mWorld.mInteractionRequests.entries())
					if (interaction->mActor == owner && interaction->mResult == InteractionResult::Pending)
						obsoleteSelections.push_back(interactionId);
				for (auto interactionId : obsoleteSelections) cancelInteraction(interactionId);
				if (agent.mTraversalTask)
				{
					if (auto request = mWorld.mTraversalRequests.find(agent.mTraversalTask->request))
					{
						request->mSourceEndpoint = path->nodes[i].targetVertex->getPosition();
						request->mDestinationEndpoint = node.targetVertex->getPosition();
						request->mPreparationRequested = false;
						request->mPreparationOperation = {};
						request->mPreparationAttempts = 0;
						request->mNextPreparationTick = mWorld.mSimulationTick;
					}
				}
				sourceNode = i;
				return true;
			}
		}
		return false;
	}

	// The lift allocation dispatcher. An open platform lift allocates against its
	// own resource, so it is dispatched out first. Otherwise the journey resource
	// is the request's own resource when the request was made on the lift or
	// shuttle itself, and the landing's coordinator link otherwise; the stop is
	// resolved the same way - nearest to the endpoint for the journey, the
	// landing's fixed stop index for a landing. A journey which is not enabled may
	// still be left, so only disembarking survives the disabled check. The
	// request's sectors against the journey sector give the boarding / riding /
	// disembarking classification, and the request goes to the branch which owns
	// it; anything which is none of the three is denied.
	void SimulationCoordinator::allocateLiftTraversal(TraversalRequestId requestId,
		TraversalResource& edgeResource)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (edgeResource.mOpenPlatformLift)
		{
			allocateOpenPlatformLiftTraversal(requestId, edgeResource);
			return;
		}
		auto coordinatorId = (edgeResource.mLift || edgeResource.mShuttle)
			? request->mResource : edgeResource.mLiftCoordinator;
		auto coordinator = mWorld.mTraversalResources.find(coordinatorId);
		if (!coordinator || (!coordinator->mLift && !coordinator->mShuttle))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		auto stop = (edgeResource.mLift || edgeResource.mShuttle)
			? findLiftStop(*coordinator, request->mDestinationEndpoint) : edgeResource.mLiftStopIndex;
		if (stop >= coordinator->mLiftStops.size())
		{
			denyTraversalRequest(requestId);
			return;
		}
		auto boarding = request->mSourceSector != coordinator->mLiftSector
			&& request->mDestinationSector == coordinator->mLiftSector;
		auto disembarking = request->mSourceSector == coordinator->mLiftSector
			&& request->mDestinationSector != coordinator->mLiftSector;
		auto riding = request->mSourceSector == coordinator->mLiftSector
			&& request->mDestinationSector == coordinator->mLiftSector;
		if (!coordinator->mEnabled && !disembarking)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}

		if ((coordinator->mLift && coordinator->mLift->isBroken())
			|| (coordinator->mShuttle && coordinator->mShuttle->isBroken()))
		{
			if (boarding)
			{
				denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
				return;
			}
			// An already-boarded passenger may finish the ride edge only at its
			// retained destination with both Doors already open. Otherwise wait.
			if (riding)
			{
				auto destination = coordinator->mLiftPassengerDestinations.find(request->mOwner);
				if (destination == coordinator->mLiftPassengerDestinations.end()
					|| coordinator->mLiftMoving || destination->second != coordinator->mLiftCurrentStop) return;
				auto landingId = coordinator->mLiftStops[destination->second].landingResource;
				if (coordinator->mShuttle)
				{
					auto carriage = findShuttlePassengerCarriage(*coordinator, request->mOwner);
					if (carriage >= coordinator->mShuttleCarriages.size()) return;
					auto selected = coordinator->mShuttleCarriages[carriage].alightingDoors.find(request->mOwner);
					if (selected == coordinator->mShuttleCarriages[carriage].alightingDoors.end()) return;
					landingId = selected->second;
				}
				else if (!coordinator->mLiftCarDoorOpen) return;
				auto landing = mWorld.mTraversalResources.find(landingId);
				if (!landing || !landing->mDoor || !landing->mDoor->isOpen()) return;
			}
		}

		if (boarding)
		{
			allocateLiftBoarding(requestId, edgeResource, *coordinator, stop);
			return;
		}

		if (riding)
		{
			allocateLiftRiding(requestId, *coordinator);
			return;
		}

		if (disembarking)
		{
			allocateLiftDisembarking(requestId, edgeResource, *coordinator, stop);
			return;
		}
		denyTraversalRequest(requestId);
	}

	// Boarding allocation. The Agent stands outside the car and asks to enter it at
	// the stop the car is standing at. It takes a queue ticket on the landing it
	// crossed through and registers its trip intent on the journey resource, then
	// prepares the landing call through that landing's control; while the call is
	// outstanding the passenger's physical queue position is suspended, since one
	// Agent cannot both operate the button and walk to a reserved position. Once the
	// call has succeeded and the car is stopped at the stop, in its Boarding phase,
	// free of disembark demand, and compatible with the run direction, the passenger
	// is admitted only when it is the earliest of the requests eligible at this stop
	// and direction in the admission queue. A shuttle passenger is assigned its
	// boarding door and a free capacity slot first; once entry commits, carriage
	// boarding order redistributes every passenger's walking target across the
	// buffered usable width. A Lift passenger reserves the free capacity position
	// farthest from the Doors, while committed occupants are ordered by Stop. The
	// passenger first reaches and releases its queue position, then approaches
	// the landing threshold before taking the door lease and crossing grant.
	// Lift passengers finish boarding in admission order.
	void SimulationCoordinator::allocateLiftBoarding(TraversalRequestId requestId,
		TraversalResource& edgeResource, TraversalResource& coordinator, uint32_t stop)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		// Pending admission is still a future boarding choice. Granted crossings
		// never re-enter this allocator; riding and alighting use separate paths.
		if (!mWorld.agentAdheresToTransportLandingPermission(request->mResource,
			request->mSourceSector, request->mSourceEndpoint, request->mOwner))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			mWorld.replanAgentAfterAuthorizationRefusal(request->mOwner);
			return;
		}
		// Shuttle boarding cannot overlap disembarkation at the aligned stop,
		// even if a request reaches allocation during a phase transition.
		if (coordinator.mShuttle
			&& (liftHasDisembarkDemand(coordinator, stop)
				|| !coordinator.mLiftExitAtSafeStop.empty())) return;
		auto boardingLanding = &edgeResource;
		auto actor = mWorld.mAgents.find(request->mOwner);
		auto desiredStop = actor ? findAgentLiftDestination(*actor, coordinator) : ~0u;
		if (desiredStop >= coordinator.mLiftStops.size() || desiredStop == stop)
		{
			denyTraversalRequest(requestId);
			return;
		}
		// Recheck destination willingness immediately before joining the boarding
		// queue. This deliberately uses the journey resource (not this origin
		// landing resource), so every car and boarding origin observes the one
		// shared destination requirement. A stale Path cannot bypass a
		// grant, requirement, or effective-adherence change.
		if (!request->mQueueTicket)
		{
			auto journeyResource = edgeResource.mLiftCoordinator
				? edgeResource.mLiftCoordinator : request->mResource;
			if (!mWorld.agentAdheresToLiftDestinationPermission(journeyResource,
				desiredStop, request->mOwner))
			{
				denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
				mWorld.replanAgentAfterAuthorizationRefusal(request->mOwner);
				return;
			}
			// Lift and shuttle passengers use the same landing-door queue. Shuttle
			// assignments may later move the ticket to another Door in the same
			// access zone without changing its logical priority.
			attachQueueTicket(requestId, edgeResource);
			if (!request->mQueueTicket) return;
			coordinator.mAdmissionQueue.push_back(requestId);
			++coordinator.mRouteQueueEpoch;
			coordinator.mLiftTripIntents[request->mOwner] = { stop, desiredStop, mWorld.mSimulationTick };
		}
		bool const opportunisticBoarding = mWorld.isTransportLocallyBoardable(
			request->mResource, request->mSourceEndpoint)
			&& !mWorld.canAgentOperateTransportLandingControl(request->mResource,
				request->mSourceSector, request->mSourceEndpoint, request->mOwner);
		if (!request->mPreparationRequested && !opportunisticBoarding)
		{
			if (edgeResource.mControls.empty())
			{
				denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
				return;
			}
			auto interactionId = requestInteractionForTraversal(edgeResource.mControls.front(), request->mOwner);
			if (!interactionId) return;
			auto interaction = mWorld.mInteractionRequests.find(interactionId);
			request->mPreparationRequested = true;
			if (interaction && !interaction->mOperations.empty())
				request->mPreparationOperation = interaction->mOperations.front().first;
			// A passenger physically operating the landing call cannot also walk
			// toward a reserved queue position. Suspend that position until the
			// button has been pressed; logical FIFO admission is retained.
			if (!edgeResource.mPreparationOperator)
			{
				edgeResource.mPreparationOperator = requestId;
				refreshQueuePositions(edgeResource);
			}
			return;
		}
		auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation);
		if (edgeResource.mPreparationOperator == requestId && operation
			&& (operation->mActivated
				|| (operation->mState != DeviceOperationState::Pending
					&& operation->mState != DeviceOperationState::Running)))
		{
			edgeResource.mPreparationOperator = {};
			refreshQueuePositions(edgeResource);
		}
		if (request->mPreparationRequested && (!operation
			|| operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running)) return;
		if (request->mPreparationRequested && operation->mState != DeviceOperationState::Succeeded)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		if (coordinator.mLiftMoving || coordinator.mLiftCurrentStop != stop
			|| coordinator.mLiftStopPhase != LiftStopPhase::Boarding
			|| liftHasDisembarkDemand(coordinator, stop)) return;
		if (!isLiftBoardingDirectionCompatible(coordinator, stop, desiredStop)) return;
		if (request->mCapacityPosition == ~0u)
		{
			if (mWorld.mSimulationTick > coordinator.mLiftBoardingCutoffTick) return;
			// Preserve FIFO among passengers eligible at this stop and in this run;
			// requests at other stops or for the return direction do not block them.
			auto selected = find_if(coordinator.mAdmissionQueue.begin(), coordinator.mAdmissionQueue.end(),
				[&](TraversalRequestId candidateId)
				{
					auto candidate = mWorld.mTraversalRequests.find(candidateId);
					if (!candidate || (coordinator.mShuttle
						&& candidate->mSourceSector != request->mSourceSector)) return false;
					auto landing = mWorld.mTraversalResources.find(candidate->mResource);
					if (!landing || landing->mLiftStopIndex != stop) return false;
					auto intent = coordinator.mLiftTripIntents.find(candidate->mOwner);
					if (intent == coordinator.mLiftTripIntents.end()) return false;
					auto desired = coordinator.mLiftStops[intent->second.destinationStop].globalPosition
						> coordinator.mLiftStops[stop].globalPosition
						? TraversalDirection::Ascending : TraversalDirection::Descending;
					return desired == coordinator.mLiftDirection;
				});
			if (selected == coordinator.mAdmissionQueue.end() || *selected != requestId) return;
			if (coordinator.mShuttle && !assignShuttleBoardingDoor(requestId, coordinator, stop)) return;
			boardingLanding = mWorld.mTraversalResources.find(request->mResource);
			if (!boardingLanding || request->mQueuePosition == ~0u) return;

			uint32_t first = 0, count = coordinator.mCapacity;
			if (coordinator.mShuttle)
			{
				if (request->mShuttleCarriage >= coordinator.mShuttleCarriages.size()) return;
				auto const& carriage = coordinator.mShuttleCarriages[request->mShuttleCarriage];
				first = carriage.firstCapacityPosition;
				count = carriage.capacity;
			}
			uint32_t position = ~0u;
			if (coordinator.mShuttle)
			{
				// Reserve deterministic slots from the leading end. Slot identity owns
				// capacity only; after the crossing commits, the carriage's boarding
				// order determines each passenger's buffered walking target.
				float direction = coordinator.mLiftDirection == TraversalDirection::Descending
					? -1.0f : 1.0f;
				float bestProgress = -numeric_limits<float>::infinity();
				for (uint32_t i = first; i < first + count; ++i)
				{
					if (coordinator.mOccupants[i] || coordinator.mAdmissionReservations[i]) continue;
					auto globalX = coordinator.mLiftPosition
						+ coordinator.mCapacityPositions[i].x;
					auto progress = direction * (globalX - actor->getGlobalPosition().x);
					if (position == ~0u || progress > bestProgress)
					{
						position = i;
						bestProgress = progress;
					}
				}
			}
			else for (uint32_t i = first + count; i-- > first;)
				if (!coordinator.mOccupants[i] && !coordinator.mAdmissionReservations[i])
				{ position = i; break; }
			if (position == ~0u) return;
			request->mCapacityPosition = position;
			if (coordinator.mShuttle
				&& !assignShuttleAlightingDoor(requestId, coordinator))
			{
				request->mCapacityPosition = ~0u;
				return;
			}
			coordinator.mAdmissionReservations[position] = requestId;
			coordinator.mAdmissionQueue.erase(selected);
			++coordinator.mRouteQueueEpoch;
			if (coordinator.mShuttle) refreshShuttlePassengerTargets(coordinator);
		}
		// Door assignment can differ from the edge that originally queued this
		// request. Always use the assigned landing, including on subsequent ticks.
		boardingLanding = mWorld.mTraversalResources.find(request->mResource);
		if (!boardingLanding) return;
		if (coordinator.mLift)
		{
			// Capacity admission precedes the approach walk. Do not let a later
			// admitted passenger overtake an earlier one approaching or crossing
			// the Door. The reservation is released when entry commits.
			for (auto reservation : coordinator.mAdmissionReservations)
			{
				auto earlier = mWorld.mTraversalRequests.find(reservation);
				if (earlier && (earlier->mState == TraversalRequestState::Pending
					|| earlier->mState == TraversalRequestState::Granted)
					&& earlier->mQueueTicket.value < request->mQueueTicket.value) return;
			}
		}
		if (request->mQueuePosition != ~0u)
		{
			boardingLanding = mWorld.mTraversalResources.find(request->mResource);
			if (!boardingLanding || request->mQueueApproach >= boardingLanding->mQueueLanes.size()) return;
			auto const& queueLane = boardingLanding->mQueueLanes[request->mQueueApproach];
			if (request->mQueuePosition >= queueLane.positions.size()
				|| !actor || actor->getGlobalPosition().distanceTo(
					queueLane.positions[request->mQueuePosition]) > 0.001f) return;
			auto& laneQueue = boardingLanding->mQueueLanes[request->mQueueApproach].queue;
			laneQueue.erase(remove(laneQueue.begin(), laneQueue.end(), requestId), laneQueue.end());
			request->mQueuePosition = ~0u;
			if (actor) actor->mTraversalLocalGoal.reset();
			refreshQueuePositions(*boardingLanding);
		}
		if (coordinator.mLift || coordinator.mShuttle)
		{
			// Reaching a waiting position does not mean reaching the Door. After
			// releasing that position, walk to the assigned threshold before allowing
			// the in-place layer crossing; never board from a queue spot or Button.
			if (!actor) return;
			// Enclosed Lifts retain their centre-based landing alignment (ADR 0005).
			auto const crossingWidth = coordinator.mLift ? 0.0f
				: CORE_DOOR_CROSSING_HALF_WIDTH(boardingLanding->mDoor->getCellsWide());
			if (!isWithinDoorCrossingBand(actor->getGlobalPosition(),
				request->mSourceEndpoint, crossingWidth))
			{
				actor->mTraversalLocalGoal = request->mSourceEndpoint;
				return;
			}
			actor->mTraversalLocalGoal.reset();
		}
		if (!request->mPreparationLease)
			request->mPreparationLease = acquireDoorOpenLease(*boardingLanding,
				DoorOpenLeaseKind::Preparation, requestId);
		if (!boardingLanding->mDoor->isOpen())
		{
			if (!boardingLanding->mDoor->isOpening()) boardingLanding->mDoor->requestOpen();
			return;
		}
		if (coordinator.mShuttle)
		{
			// Reservations project the carriage's post-boarding layout before a
			// crossing is granted. Existing passengers walk to those targets first,
			// leaving the newcomer a buffered route in from the threshold.
			refreshShuttlePassengerTargets(coordinator);
			if (request->mShuttleCarriage >= coordinator.mShuttleCarriages.size()) return;
			auto const& carriage = coordinator.mShuttleCarriages[request->mShuttleCarriage];
			auto transit = mWorld.mSectors[(size_t)coordinator.mLiftSector.value - 1].get();
			for (auto passengerId : carriage.passengerOrder)
			{
				auto passenger = mWorld.mAgents.find(passengerId);
				auto target = carriage.passengerTargets.find(passengerId);
				if (!passenger || target == carriage.passengerTargets.end()) return;
				auto globalTarget = transit->getPosition() + target->second;
				globalTarget.x += coordinator.mLiftPosition - transit->getPosition().x;
				if (passenger->getGlobalPosition().distanceTo(globalTarget) > 0.001f) return;
			}
		}
		auto lane = find(boardingLanding->mCrossingOwners.begin(), boardingLanding->mCrossingOwners.end(), TraversalRequestId{});
		if (lane == boardingLanding->mCrossingOwners.end()) return;
		request->mCrossingLane = (uint32_t)distance(boardingLanding->mCrossingOwners.begin(), lane);
		*lane = requestId;
		coordinator.mLiftAdmissionReservation = requestId;
		coordinator.mLiftCarDoorOpen = true;
		grantTraversalRequest(requestId);
	}

	// Riding allocation. The Agent is already an occupant of the car and asks to
	// travel to another stop inside it. A destination already scheduled for the
	// passenger is granted straight away. A Shuttle's contiguous ride commits as
	// one journey at the passenger's current buffered carriage position; the nearest
	// destination Door is selected by the following disembark request. Otherwise the
	// journey stop is read from the ride edge ahead on the path: a stop some other
	// passenger has already
	// requested is shared without further ceremony, and a fresh stop is confirmed
	// one passenger at a time through the confirmation queue at the interior
	// selector control. A failed confirmation is retried on the waiting policy's
	// delay until the retries run out, at which point the passenger is asked to
	// leave at the next safe stop and the request is denied.
	void SimulationCoordinator::allocateLiftRiding(TraversalRequestId requestId, TraversalResource& coordinator)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (find(coordinator.mOccupants.begin(), coordinator.mOccupants.end(), request->mOwner)
			== coordinator.mOccupants.end()) return;
		if (auto scheduled = coordinator.mLiftPassengerDestinations.find(request->mOwner);
			scheduled != coordinator.mLiftPassengerDestinations.end())
		{
			if (!coordinator.mLiftMoving && coordinator.mLiftCurrentStop == scheduled->second)
			{
				if (auto actor = mWorld.mAgents.find(request->mOwner))
				{
					if (coordinator.mShuttle)
					{
						// Multiple Door cells create a contiguous chain of Shuttle
						// edges. Intermediate nodes may still belong to the origin
						// stop, so commit through the final node in this journey while
						// preserving the passenger's buffered carriage position.
						auto destinationVertex = actor->mTraversalTask
							? actor->mTraversalTask->destinationVertex : shared_ptr<const Vertex>{};
						auto destinationNode = actor->mPath.targetNode + 1;
						if (actor->mPath.path)
							for (uint32_t i = actor->mPath.targetNode + 1;
								i < actor->mPath.path->nodes.size(); ++i)
							{
								auto const& node = actor->mPath.path->nodes[i];
								if (!node.edge || node.edge->getType() != EdgeType::Shuttle
									|| node.edge->getTraversalResourceId()
										!= coordinator.mShuttle->getTraversalResourceId()) break;
								if (node.targetVertex)
								{
									destinationVertex = node.targetVertex;
									destinationNode = i;
								}
							}
						if (!destinationVertex) return;

						auto destinationEndpoint = destinationVertex->getPosition();
						actor->mTraversalLocalGoal.reset();

						// Commit the contiguous ride as one journey so Agent does not
						// subsequently traverse stale intermediate Shuttle nodes.
						request->mDestinationEndpoint = destinationEndpoint;
						request->mDestinationSector = SectorId{
							(uint64_t)destinationVertex->getSector()->getIndex() + 1 };
						if (actor->mTraversalTask)
							actor->mTraversalTask->destinationVertex = destinationVertex;
						if (destinationNode > actor->mPath.targetNode)
							actor->mPath.targetNode = destinationNode - 1;
					}
				}
				grantTraversalRequest(requestId);
			}
			return;
		}
		auto actor = mWorld.mAgents.find(request->mOwner);
		auto journeyStop = actor ? findAgentLiftDestination(*actor, coordinator) : ~0u;
		if (journeyStop >= coordinator.mLiftStops.size()) { denyTraversalRequest(requestId); return; }
		if (!coordinator.mLiftStopRequestOwners[journeyStop].empty())
		{
			addLiftStopRequest(coordinator, journeyStop, request->mOwner);
			coordinator.mLiftPassengerDestinations[request->mOwner] = journeyStop;
			orderLiftOccupants(coordinator);
			// This passenger may have queued for serialized destination
			// confirmation before another passenger activated the same stop.
			// Sharing that destination makes the queued confirmation obsolete.
			coordinator.mLiftConfirmationQueue.erase(remove(
				coordinator.mLiftConfirmationQueue.begin(),
				coordinator.mLiftConfirmationQueue.end(), requestId),
				coordinator.mLiftConfirmationQueue.end());
			if (coordinator.mLiftActiveConfirmation == requestId)
				coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.empty()
					? TraversalRequestId{} : coordinator.mLiftConfirmationQueue.front();
			return;
		}
		if (find(coordinator.mLiftConfirmationQueue.begin(), coordinator.mLiftConfirmationQueue.end(), requestId)
			== coordinator.mLiftConfirmationQueue.end())
			coordinator.mLiftConfirmationQueue.push_back(requestId);
		if (!coordinator.mLiftActiveConfirmation)
			coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.front();
		if (coordinator.mLiftActiveConfirmation != requestId) return;
		if (!request->mPreparationRequested)
		{
			if (mWorld.mSimulationTick < request->mNextPreparationTick) return;
			if (journeyStop >= coordinator.mControls.size()) { denyTraversalRequest(requestId); return; }
			coordinator.mLiftSelector = coordinator.mControls[journeyStop];
			auto selector = mWorld.mInteractionPoints.find(coordinator.mLiftSelector);
			auto actor = mWorld.mAgents.find(request->mOwner);
			if (selector && actor) selector->mPosition = actor->getGlobalPosition();
			auto interactionId = requestInteractionForTraversal(coordinator.mLiftSelector, request->mOwner);
			if (!interactionId) return;
			auto interaction = mWorld.mInteractionRequests.find(interactionId);
			if (interaction && interaction->mResult == InteractionResult::Rejected)
			{
				requestLiftPassengerSafeExit(request->mOwner, TraversalFailureReason::PreparationFailed);
				denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
				return;
			}
			request->mPreparationRequested = true;
			if (interaction && !interaction->mOperations.empty())
				request->mPreparationOperation = interaction->mOperations.front().first;
			return;
		}
		auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation);
		if (!operation || operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running) return;
		if (operation->mState != DeviceOperationState::Succeeded)
		{
			if (request->mPreparationAttempts < mWorld.mTraversalWaitingPolicy.maximumDestinationRetries)
			{
				++request->mPreparationAttempts;
				for (auto const& [interactionId, interaction] : mWorld.mInteractionRequests.entries())
					if (interaction->mActor == request->mOwner
						&& interaction->mResult == InteractionResult::Pending)
						cancelInteraction(interactionId);
				request->mPreparationRequested = false;
				request->mPreparationOperation = {};
				request->mNextPreparationTick = mWorld.mSimulationTick
					+ mWorld.mTraversalWaitingPolicy.destinationRetryDelayTicks;
				return;
			}
			requestLiftPassengerSafeExit(request->mOwner, TraversalFailureReason::PreparationFailed);
			denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		addLiftStopRequest(coordinator, journeyStop, request->mOwner);
		coordinator.mLiftPassengerDestinations[request->mOwner] = journeyStop;
		orderLiftOccupants(coordinator);
		coordinator.mLiftDestinationStop = journeyStop;
		coordinator.mLiftConfirmationQueue.erase(coordinator.mLiftConfirmationQueue.begin());
		coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.empty()
			? TraversalRequestId{} : coordinator.mLiftConfirmationQueue.front();
		return;
	}

	// Disembarking allocation. The Agent occupies the car and asks to leave it at
	// the stop the car is standing at. A shuttle passenger uses the alighting Door
	// chosen when it boarded, then walks within the
	// carriage to the shuttle-side node the remaining path selected before the
	// Door crossing is granted. A lift passenger preserves its standing position
	// when the ride completes, reserves a crossing lane, and walks at ordinary
	// speed to the car-side Door endpoint. The stop enters its Disembarking phase,
	// a door open lease holds the landing door open for the crossing, and the grant
	// uses the reserved crossing lane on the landing resource.
	void SimulationCoordinator::allocateLiftDisembarking(TraversalRequestId requestId,
		TraversalResource& edgeResource, TraversalResource& coordinator, uint32_t stop)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (find(coordinator.mOccupants.begin(), coordinator.mOccupants.end(), request->mOwner)
			== coordinator.mOccupants.end()
			|| coordinator.mLiftMoving || coordinator.mLiftCurrentStop != stop) return;
		auto disembarkLanding = &edgeResource;
		if (coordinator.mLift && coordinator.mLift->isBroken()
			&& (!coordinator.mLiftCarDoorOpen || !disembarkLanding->mDoor->isOpen())) return;
		if (coordinator.mShuttle)
		{
			if (!assignShuttleDisembarkDoor(requestId, coordinator, stop)) return;
			disembarkLanding = mWorld.mTraversalResources.find(request->mResource);
			if (!disembarkLanding) return;
			if (coordinator.mShuttle->isBroken() && !disembarkLanding->mDoor->isOpen()) return;

			// Once the Shuttle has stopped, walk within the carriage into the
			// selected Door's crossing band before granting the crossing. Staying at
			// the nearest valid point avoids pulling a passenger off a buffered
			// carriage-side position merely to touch the Door's centre vertex.
			auto actor = mWorld.mAgents.find(request->mOwner);
			if (!actor) return;
			auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(
				disembarkLanding->mDoor->getCellsWide());
			auto alignmentTarget = actor->getGlobalPosition();
			alignmentTarget.x = clamp(alignmentTarget.x,
				request->mSourceEndpoint.x - crossingWidth,
				request->mSourceEndpoint.x + crossingWidth);
			if (abs(actor->getGlobalPosition().x - alignmentTarget.x) > 0.001f)
			{
				actor->mTraversalLocalGoal = alignmentTarget;
				return;
			}
			actor->mTraversalLocalGoal.reset();
		}
		coordinator.mLiftStopPhase = LiftStopPhase::Disembarking;
		if (coordinator.mLift)
			respaceLiftOccupantsAfterAlighting(coordinator);
		if (!request->mPreparationLease)
			request->mPreparationLease = acquireDoorOpenLease(*disembarkLanding,
				DoorOpenLeaseKind::Preparation, requestId);
		if (!disembarkLanding->mDoor->isOpen())
		{
			if (!disembarkLanding->mDoor->isOpening()) disembarkLanding->mDoor->requestOpen();
			return;
		}
		if (request->mCrossingLane == ~0u)
		{
			auto lane = find(disembarkLanding->mCrossingOwners.begin(),
				disembarkLanding->mCrossingOwners.end(), TraversalRequestId{});
			if (lane == disembarkLanding->mCrossingOwners.end()) return;
			request->mCrossingLane = (uint32_t)distance(
				disembarkLanding->mCrossingOwners.begin(), lane);
			*lane = requestId;
		}

		if (coordinator.mLift)
		{
			// Reserve the crossing lane before approaching it. The Agent remains in
			// WaitingForTraversal and therefore walks to the car-side Door endpoint at
			// ordinary speed instead of being snapped there when the ride completes.
			auto actor = mWorld.mAgents.find(request->mOwner);
			if (!actor) return;
			auto alignmentTarget = actor->getGlobalPosition();
			alignmentTarget.x = request->mSourceEndpoint.x;
			if (abs(actor->getGlobalPosition().x - alignmentTarget.x) > 0.001f)
			{
				actor->mTraversalLocalGoal = alignmentTarget;
				return;
			}
			actor->mTraversalLocalGoal.reset();
		}
		coordinator.mLiftCarDoorOpen = true;
		grantTraversalRequest(requestId);
	}

} // core
