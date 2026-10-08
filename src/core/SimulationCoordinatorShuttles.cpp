#include <algorithm>
#include <vector>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Edge.h"
#include "core/Shuttle.h"
#include "core/OccupantPacking.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// Shuttle door assignment moved out of World (ADR 0004 stage 3). The
	// coordinator works on World's traversal-resource, traversal-request, Agent
	// and graph registries through friendship and calls its own Shuttle helpers.
	//
	// Boarding picks the nearest Door that can still take the passenger: the Door
	// must serve the boarding sector, the carriage must have capacity, and - when
	// the journey leaves the Shuttle into a specific access sector - the same
	// carriage must own a Door at the destination stop as well. Once capacity is
	// reserved, the alighting Door nearest that slot is retained for the journey;
	// disembarking applies that choice rather than selecting a Door after the ride.
	//
	// Each carriage retains passenger boarding order. Occupants and pending
	// reservations define projected counts per alighting Door. Each group spans its
	// Door's buffered sub-range of the carriage. Existing passengers reach those
	// targets before another Door crossing is granted; no spacing update sets an
	// Agent position directly.
	//
	// The static shuttleDoorOffsets() helper which expands a carriage's door mask
	// into the cells its doors occupy travels with this family: it is the origin of
	// the carriage/door indexing the assignment above works on. World's shuttle
	// authoring path calls it through the coordinator class.

	vector<uint32_t> SimulationCoordinator::shuttleDoorOffsets(uint32_t carriageWidth, uint32_t doorMask)
	{
		vector<uint32_t> result;
		if (!carriageWidth || !doorMask || (doorMask >> carriageWidth) != 0) return result;
		for (uint32_t cell = 0; cell < carriageWidth; ++cell)
			if ((doorMask & (1u << cell)) != 0) result.push_back(cell);
		return result;
	}

	uint32_t SimulationCoordinator::findShuttlePassengerCarriage(TraversalResource const& resource,
		AgentId passenger) const
	{
		if (!resource.mShuttle || !resource.mShuttleCapacityPerCarriage) return ~0u;
		for (uint32_t position = 0; position < resource.mOccupants.size(); ++position)
			if (resource.mOccupants[position] == passenger)
				return position / resource.mShuttleCapacityPerCarriage;
		return ~0u;
	}

	void SimulationCoordinator::refreshShuttlePassengerTargets(TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!resource.mShuttle) return;
		auto const carriageWidth = (float)resource.mShuttle->getCarWidth();
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;

		for (auto& carriage : resource.mShuttleCarriages)
		{
			map<AgentId, uint32_t> activeSlots;
			for (uint32_t i = 0; i < carriage.capacity; ++i)
			{
				auto const slot = carriage.firstCapacityPosition + i;
				if (slot < resource.mOccupants.size() && resource.mOccupants[slot])
					activeSlots.emplace(resource.mOccupants[slot], slot);
			}

			carriage.passengerOrder.erase(remove_if(carriage.passengerOrder.begin(),
				carriage.passengerOrder.end(), [&](AgentId passenger)
					{ return !activeSlots.contains(passenger); }), carriage.passengerOrder.end());
			carriage.passengerTargets.clear();
			if (activeSlots.empty())
			{
				carriage.passengerOrder.clear();
				carriage.packingDirection = TraversalDirection::None;
				continue;
			}

			vector<pair<uint32_t, AgentId>> newcomers;
			for (auto const& [passenger, slot] : activeSlots)
				if (find(carriage.passengerOrder.begin(), carriage.passengerOrder.end(), passenger)
					== carriage.passengerOrder.end())
					newcomers.push_back({ slot, passenger });

			if (carriage.passengerOrder.empty())
			{
				carriage.packingDirection = resource.mLiftDirection;
				if (carriage.packingDirection == TraversalDirection::None && !newcomers.empty())
				{
					auto intent = resource.mLiftTripIntents.find(newcomers.front().second);
					if (intent != resource.mLiftTripIntents.end()
						&& intent->second.originStop < resource.mLiftStops.size()
						&& intent->second.destinationStop < resource.mLiftStops.size())
						carriage.packingDirection = resource.mLiftStops[intent->second.destinationStop].globalPosition
							> resource.mLiftStops[intent->second.originStop].globalPosition
							? TraversalDirection::Ascending : TraversalDirection::Descending;
				}
				if (carriage.packingDirection == TraversalDirection::None)
					carriage.packingDirection = TraversalDirection::Ascending;
			}

			sort(newcomers.begin(), newcomers.end(), [&](auto const& left, auto const& right)
			{
				if (left.first == right.first) return left.second < right.second;
				return carriage.packingDirection == TraversalDirection::Descending
					? left.first < right.first : left.first > right.first;
			});
			for (auto const& [slot, passenger] : newcomers)
			{
				(void)slot;
				carriage.passengerOrder.push_back(passenger);
			}

			auto const carriageStart = carriage.index * (carriageWidth + 1.0f);
			for (auto door = carriage.alightingDoors.begin(); door != carriage.alightingDoors.end();)
				if (!activeSlots.contains(door->first)) door = carriage.alightingDoors.erase(door);
				else ++door;

			// Pending boarders already have an alighting Door, so their space is
			// projected in that Door's group before they enter. Committed occupants
			// retain the choice made by their boarding request.
			map<TraversalResourceId, uint32_t> groupReservations;
			for (uint32_t i = 0; i < carriage.capacity; ++i)
			{
				auto const slot = carriage.firstCapacityPosition + i;
				if (slot >= resource.mAdmissionReservations.size()) continue;
				auto request = mWorld.mTraversalRequests.find(resource.mAdmissionReservations[slot]);
				if (request && request->mShuttleAlightingDoor)
					++groupReservations[request->mShuttleAlightingDoor];
			}

			map<TraversalResourceId, vector<AgentId>> groups;
			for (auto passenger : carriage.passengerOrder)
				if (auto door = carriage.alightingDoors.find(passenger);
					door != carriage.alightingDoors.end()) groups[door->second].push_back(passenger);

			for (auto const& [doorId, passengers] : groups)
			{
				auto selectedDoor = find_if(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
					[&](auto const& door) { return door.landingResource == doorId; });
				if (selectedDoor == resource.mShuttleDoors.end()) continue;
				vector<float> doorPositions;
				for (auto const& door : resource.mShuttleDoors)
					if (door.carriageIndex == carriage.index
						&& door.stopIndex == selectedDoor->stopIndex)
						doorPositions.push_back(door.carriagePosition);
				sort(doorPositions.begin(), doorPositions.end());
				doorPositions.erase(unique(doorPositions.begin(), doorPositions.end()), doorPositions.end());
				auto physicalDoor = lower_bound(doorPositions.begin(), doorPositions.end(),
					selectedDoor->carriagePosition);
				if (physicalDoor == doorPositions.end()) continue;

				auto first = carriageStart + halfAgentWidth;
				auto last = carriageStart + carriageWidth - halfAgentWidth;
				if (physicalDoor != doorPositions.begin())
					first = (*prev(physicalDoor) + *physicalDoor) * 0.5f + halfAgentWidth;
				if (next(physicalDoor) != doorPositions.end())
					last = (*physicalDoor + *next(physicalDoor)) * 0.5f - halfAgentWidth;
				if (last < first) first = last = selectedDoor->carriagePosition;

				auto const targets = packOccupants(passengers.size(), groupReservations[doorId],
					{ first, last }, CORE_RESOURCE_SLOT_WIDTH, CORE_SHUTTLE_OCCUPANT_CLEARANCE,
					carriage.packingDirection == TraversalDirection::Descending
						? OccupantPackingOrder::Forward : OccupantPackingOrder::Reverse,
					OccupantPackingLayout::Buffered);
				for (size_t rank = 0; rank < passengers.size(); ++rank)
				{
					auto const slot = activeSlots.at(passengers[rank]);
					auto const y = slot < resource.mCapacityPositions.size()
						? resource.mCapacityPositions[slot].y : 0.0f;
					carriage.passengerTargets[passengers[rank]] = { targets[rank], y };
				}
			}
		}
	}

	bool SimulationCoordinator::retargetShuttleDoorTraversal(TraversalRequestId requestId,
		TraversalResource& coordinator, ShuttleDoor const& door)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		auto landing = mWorld.mTraversalResources.find(door.landingResource);
		if (!request || !landing || landing->mLiftCoordinator != coordinator.mShuttle->getTraversalResourceId())
			return false;

		shared_ptr<const Edge> selectedEdge;
		shared_ptr<const Vertex> selectedSource;
		shared_ptr<const Vertex> selectedDestination;
		for (auto const& edge : mWorld.mGraph->getEdges())
		{
			if (edge->getTraversalResourceId() != door.landingResource) continue;
			auto first = edge->getVertex(0);
			auto second = edge->getVertex(1);
			auto firstSector = SectorId{ (uint64_t)first->getSector()->getIndex() + 1 };
			auto secondSector = SectorId{ (uint64_t)second->getSector()->getIndex() + 1 };
			if (firstSector == request->mSourceSector && secondSector == request->mDestinationSector)
			{ selectedSource = first; selectedDestination = second; }
			else if (secondSector == request->mSourceSector && firstSector == request->mDestinationSector)
			{ selectedSource = second; selectedDestination = first; }
			if (selectedSource) { selectedEdge = edge; break; }
		}
		if (!selectedEdge) return false;

		if (request->mShuttleDoor && request->mShuttleDoor != door.landingResource)
			if (auto previous = mWorld.mTraversalResources.find(request->mShuttleDoor))
				releaseDoorQueueOwnership(requestId, *previous);
		request->mResource = door.landingResource;
		request->mSourceEndpoint = selectedSource->getPosition();
		request->mDestinationEndpoint = selectedDestination->getPosition();
		request->mShuttleDoor = door.landingResource;
		request->mShuttleCarriage = door.carriageIndex;
		request->mShuttleAccessZone = door.accessZoneIndex;
		if (auto agent = mWorld.mAgents.find(request->mOwner); agent && agent->mTraversalTask)
		{
			agent->mTraversalTask->edge = selectedEdge;
			agent->mTraversalTask->sourceVertex = selectedSource;
			agent->mTraversalTask->destinationVertex = selectedDestination;
		}
		return true;
	}

	bool SimulationCoordinator::assignShuttleBoardingDoor(TraversalRequestId requestId,
		TraversalResource& coordinator, uint32_t stop)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		auto agent = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
		if (!request || !agent) return false;
		if (request->mShuttleCarriage != ~0u) return true;

		// A disconnected destination platform can only be reached from a carriage
		// that has a door into that access zone. Derive that zone from the journey's
		// disembark edge before ranking otherwise eligible carriages.
		SectorId destinationAccessSector;
		bool passedRide = false;
		if (agent->mPath.path)
			for (uint32_t i = agent->mPath.targetNode + 1; i < agent->mPath.path->nodes.size(); ++i)
			{
				auto const& node = agent->mPath.path->nodes[i];
				if (!node.edge || !node.targetVertex) continue;
				if (node.edge->getType() == EdgeType::Shuttle) { passedRide = true; continue; }
				if (passedRide && node.edge->getType() == EdgeType::Door
					&& SectorId{ (uint64_t)node.targetVertex->getSector()->getIndex() + 1 }
						!= coordinator.mLiftSector)
				{
					destinationAccessSector = SectorId{
						(uint64_t)node.targetVertex->getSector()->getIndex() + 1 };
					break;
				}
			}
		auto intent = coordinator.mLiftTripIntents.find(request->mOwner);
		auto destinationStop = intent == coordinator.mLiftTripIntents.end()
			? ~0u : intent->second.destinationStop;

		ShuttleDoor const* selected = nullptr;
		float selectedDistance = 0.0f;
		uint32_t selectedLoad = 0;
		for (auto const& door : coordinator.mShuttleDoors)
		{
			if (door.stopIndex != stop || door.locationSector != request->mSourceSector
				|| door.carriageIndex >= coordinator.mShuttleCarriages.size()) continue;
			if (destinationAccessSector && none_of(coordinator.mShuttleDoors.begin(),
				coordinator.mShuttleDoors.end(), [&](auto const& destinationDoor)
				{
					return destinationDoor.stopIndex == destinationStop
						&& destinationDoor.carriageIndex == door.carriageIndex
						&& destinationDoor.locationSector == destinationAccessSector;
				})) continue;
			auto const& carriage = coordinator.mShuttleCarriages[door.carriageIndex];
			uint32_t load = 0;
			for (uint32_t i = 0; i < carriage.capacity; ++i)
			{
				auto position = carriage.firstCapacityPosition + i;
				load += coordinator.mOccupants[position] || coordinator.mAdmissionReservations[position];
			}
			if (load >= carriage.capacity) continue;

			Vector2 threshold;
			bool foundThreshold = false;
			for (auto const& edge : mWorld.mGraph->getEdges())
			{
				if (edge->getTraversalResourceId() != door.landingResource) continue;
				for (uint32_t vertex = 0; vertex < 2; ++vertex)
					if (SectorId{ (uint64_t)edge->getVertex(vertex)->getSector()->getIndex() + 1 }
						== request->mSourceSector)
					{ threshold = edge->getVertex(vertex)->getPosition(); foundThreshold = true; break; }
				if (foundThreshold) break;
			}
			if (!foundThreshold) continue;
			// Reassignment within an access zone must respect the selected Door's
			// landing requirement, not only the Door named by the original Path.
			if (!mWorld.agentAdheresToTransportLandingPermission(door.landingResource,
				request->mSourceSector, threshold, request->mOwner)) continue;
			auto distance = agent->getGlobalPosition().distanceTo(threshold);
			if (!selected || distance < selectedDistance - 0.001f
				|| (abs(distance - selectedDistance) <= 0.001f
					&& (load < selectedLoad || (load == selectedLoad
						&& (door.carriageIndex < selected->carriageIndex
							|| (door.carriageIndex == selected->carriageIndex
								&& door.landingResource < selected->landingResource))))))
			{
				selected = &door;
				selectedDistance = distance;
				selectedLoad = load;
			}
		}
		if (!selected || !retargetShuttleDoorTraversal(requestId, coordinator, *selected)) return false;

		// The queue ticket was created at the access-zone boundary and is retained
		// while the physical door/position assignment changes.
		auto landing = mWorld.mTraversalResources.find(selected->landingResource);
		for (uint32_t lane = 0; landing && lane < landing->mQueueLanes.size(); ++lane)
		{
			if (landing->mQueueLanes[lane].sector != request->mSourceSector) continue;
			request->mQueueApproach = lane;
			auto& queue = landing->mQueueLanes[lane].queue;
			if (find(queue.begin(), queue.end(), requestId) == queue.end()) queue.push_back(requestId);
			sort(queue.begin(), queue.end(), [&](auto left, auto right)
			{
				auto lhs = mWorld.mTraversalRequests.find(left);
				auto rhs = mWorld.mTraversalRequests.find(right);
				return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
			});
			refreshQueuePositions(*landing);
			break;
		}
		return true;
	}

	bool SimulationCoordinator::assignShuttleAlightingDoor(TraversalRequestId requestId,
		TraversalResource& coordinator)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		auto agent = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
		if (!request || !agent || request->mShuttleCarriage >= coordinator.mShuttleCarriages.size()
			|| request->mCapacityPosition >= coordinator.mCapacityPositions.size()) return false;
		if (request->mShuttleAlightingDoor) return true;
		auto intent = coordinator.mLiftTripIntents.find(request->mOwner);
		if (intent == coordinator.mLiftTripIntents.end()) return false;

		SectorId destinationSector;
		bool passedRide = false;
		if (agent->mPath.path)
			for (uint32_t i = agent->mPath.targetNode + 1; i < agent->mPath.path->nodes.size(); ++i)
			{
				auto const& node = agent->mPath.path->nodes[i];
				if (!node.edge || !node.targetVertex) continue;
				if (node.edge->getType() == EdgeType::Shuttle) { passedRide = true; continue; }
				if (passedRide && node.edge->getType() == EdgeType::Door)
				{
					destinationSector = SectorId{
						(uint64_t)node.targetVertex->getSector()->getIndex() + 1 };
					break;
				}
			}

		auto const slotX = coordinator.mCapacityPositions[request->mCapacityPosition].x;
		ShuttleDoor const* selected = nullptr;
		float selectedDistance = 0.0f;
		for (auto const& door : coordinator.mShuttleDoors)
		{
			if (door.stopIndex != intent->second.destinationStop
				|| door.carriageIndex != request->mShuttleCarriage
				|| (destinationSector && door.locationSector != destinationSector)) continue;
			auto landing = mWorld.mTraversalResources.find(door.landingResource);
			if (!landing || !landing->mDoor || !landing->mDoor->selectAgentCrossing(*agent,
				agent->getGlobalPosition().y, true, landing->mDoor->isIndependentlyBroken()
					? std::optional<float>{landing->mDoor->getOpenPercentage()} : std::nullopt)) continue;
			auto const distance = abs(slotX - door.carriagePosition);
			if (!selected || distance < selectedDistance - 0.001f
				|| (abs(distance - selectedDistance) <= 0.001f
					&& (coordinator.mLiftDirection == TraversalDirection::Ascending
						? door.carriagePosition > selected->carriagePosition
						: coordinator.mLiftDirection == TraversalDirection::Descending
							? door.carriagePosition < selected->carriagePosition
							: door.landingResource < selected->landingResource)))
			{ selected = &door; selectedDistance = distance; }
		}
		if (!selected) return false;
		auto landing = mWorld.mTraversalResources.find(selected->landingResource);
		agent->mAcceptedExitPoses[landing->mDoor] = *landing->mDoor->selectAgentCrossing(*agent,
			agent->getGlobalPosition().y, true, landing->mDoor->isIndependentlyBroken()
				? std::optional<float>{landing->mDoor->getOpenPercentage()} : std::nullopt);
		request->mShuttleAlightingDoor = selected->landingResource;
		return true;
	}

	bool SimulationCoordinator::assignShuttleDisembarkDoor(TraversalRequestId requestId,
		TraversalResource& coordinator, uint32_t stop)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request) return false;
		auto carriage = findShuttlePassengerCarriage(coordinator, request->mOwner);
		if (carriage == ~0u || carriage >= coordinator.mShuttleCarriages.size()) return false;
		auto selectedId = coordinator.mShuttleCarriages[carriage].alightingDoors.find(request->mOwner);
		if (selectedId == coordinator.mShuttleCarriages[carriage].alightingDoors.end()) return false;
		auto original = find_if(coordinator.mShuttleDoors.begin(), coordinator.mShuttleDoors.end(),
			[&](auto const& door) { return door.landingResource == selectedId->second; });
		if (original == coordinator.mShuttleDoors.end()) return false;
		auto selected = find_if(coordinator.mShuttleDoors.begin(), coordinator.mShuttleDoors.end(),
			[&](auto const& door)
			{
				return door.stopIndex == stop && door.carriageIndex == carriage
					&& door.locationSector == request->mDestinationSector
					&& abs(door.carriagePosition - original->carriagePosition) < 0.001f;
			});
		if (selected == coordinator.mShuttleDoors.end()) return false;
		selectedId->second = selected->landingResource;
		return retargetShuttleDoorTraversal(requestId, coordinator, *selected);
	}

} // core
