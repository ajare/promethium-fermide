#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "core/TraversalResourceRegistry.h"

#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/Exceptions.h"
#include "core/ExtensibleObject.h"
#include "core/ForceBridge.h"
#include "core/Graph.h"
#include "core/Ladder.h"
#include "core/Lift.h"
#include "core/Object.h"
#include "core/OccupantPacking.h"
#include "core/Shuttle.h"
#include "core/Simulation.h"
#include "core/SimulationCoordinator.h"
#include "core/Stairwell.h"
#include "core/Window.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	TraversalResourceRegistry::TraversalResourceRegistry(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Creation
	//
	// Every rejecting check runs before beginStructuralEdit() and before any
	// registry write, so a refused resource is a true no-op: no traversal
	// resource, event, or dirty document (#198).
	// ----------------------------------------------------------------------

	TraversalResourceId TraversalResourceRegistry::createTraversalResource(string const& name)
	{
		mWorld.invalidateSimulationSnapshot();
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createDoorTraversalResource(string const& name,
		shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds)
	{
		mWorld.invalidateSimulationSnapshot();
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// resource is a true no-op: no traversal resource, event, or dirty
		// document (#198).
		if (!door || !isFiniteTiming(holdOpenSeconds))
		{
			throw invalid_argument("A door traversal resource requires a Door and a finite, non-negative hold time");
		}
		mWorld.beginStructuralEdit("createDoorTraversalResource");
		auto holdTicks = secondsToTicks(holdOpenSeconds, mWorld.getFixedTimestep());
		auto laneCount = max(1u, door->getCellsWide());
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(door), mode, holdTicks)));
		mWorld.mTraversalResources.find(id)->mCrossingOwners.resize(laneCount);
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createWindowTraversalResource(string const& name,
		shared_ptr<Window> window)
	{
		if (!window || window->isBoothWindow())
			throw invalid_argument("A Window crossing resource requires an ordinary Window, not a BoothWindow");
		mWorld.invalidateSimulationSnapshot();
		mWorld.beginStructuralEdit("createWindowTraversalResource");
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(window))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createLadderTraversalResource(string const& name,
		shared_ptr<Ladder> ladder, SectorId ladderSector, uint32_t directionalBatchLimit)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.beginStructuralEdit("createLadderTraversalResource");
		if (!ladder || !ladderSector || ladderSector.value > mWorld.mSectors.size()
			|| directionalBatchLimit == 0)
		{
			throw invalid_argument("A ladder traversal resource requires a Ladder, sector, and positive batch limit");
		}
		// Capacity is a physical property of the usable vertical span: each slot keeps
		// neighbouring Agents at least CORE_LADDER_SLOT_SPACING apart in render space.
		// Even a short valid Ladder admits one Agent.
		auto crossedLevels = (float)(ladder->getLevelsHigh() - 1);
		auto agentSpacing = CORE_LADDER_SLOT_SPACING / CORE_CELL_YX_RENDER_RATIO;
		auto capacity = max(1u, (uint32_t)floor(crossedLevels / agentSpacing));
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = ladder->getPosition();
		auto x = origin.x + ladder->getSize().x * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ x, origin.y + (agentSpacing >= crossedLevels
				? crossedLevels * 0.5f : agentSpacing * ((float)i + 0.5f)) });
		}

		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, ladder, ladder->isExtensible() ? static_pointer_cast<ExtensibleObject>(ladder) : nullptr,
			ladderSector, agentSpacing, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId liftSector, vector<LiftStop> stops, uint32_t capacity,
		float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!lift || !liftSector || liftSector.value > mWorld.mSectors.size() || stops.size() < 2
			|| capacity == 0)
		{
			throw invalid_argument("A lift traversal resource requires a lift sector, at least two stops, and positive capacity");
		}
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(minimumDwellSeconds) || !isFiniteTiming(maximumBoardingSeconds)
			|| maximumBoardingSeconds < minimumDwellSeconds)
		{
			throw invalid_argument("A lift traversal resource requires finite dwell timing with 0 <= minimum dwell <= maximum boarding time");
		}
		mWorld.beginStructuralEdit("createLiftTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (!stops[i].locationSector || stops[i].locationSector.value > mWorld.mSectors.size())
				throw invalid_argument(format("Lift stop {} has an invalid location sector mapping", i));
			auto landing = mWorld.mTraversalResources.find(stops[i].landingResource);
			if (!landing || !landing->mDoor)
				throw invalid_argument(format("Lift stop {} has no valid landing-door traversal mapping", i));
			if (!isfinite(stops[i].globalPosition))
				throw invalid_argument(format("Lift stop {} has a non-finite position", i));
			if (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition)
				throw invalid_argument(format("Lift stop {} is not strictly above the previous stop", i));
		}
		auto usableWidth = lift->getSize().x;
		if (capacity > (uint32_t)floor(usableWidth / CORE_RESOURCE_SLOT_WIDTH))
			throw invalid_argument("Declared lift capacity cannot be represented by separated interior positions");
		vector<Vector2> positions(capacity);
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto const occupantTargets = packOccupants(capacity, 0,
			{ halfAgentWidth, usableWidth - halfAgentWidth }, CORE_RESOURCE_SLOT_WIDTH,
			mWorld.mTraversalGeometryPolicy.occupantClearance, OccupantPackingOrder::Forward,
			OccupantPackingLayout::Compact);
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { occupantTargets[i], 0.0f };
		auto minimumDwellTicks = secondsToTicks(minimumDwellSeconds, mWorld.getFixedTimestep());
		auto maximumBoardingTicks = secondsToTicks(maximumBoardingSeconds, mWorld.getFixedTimestep());
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(lift), liftSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createOpenPlatformLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId locationSector, vector<LiftStop> stops, uint32_t capacity,
		float stopDurationSeconds)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!lift || !locationSector || locationSector.value > mWorld.mSectors.size() || stops.size() < 2
			|| capacity == 0)
			throw invalid_argument("An open platform lift requires a location, at least two stops, and positive capacity");
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(stopDurationSeconds))
			throw invalid_argument("An open platform lift requires a finite, non-negative stop duration");
		mWorld.beginStructuralEdit("createOpenPlatformLiftTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (stops[i].locationSector != locationSector || stops[i].landingResource
				|| !isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Platform lift stop {} has invalid virtual-boundary geometry", i));
		}
		if (capacity > (uint32_t)floor(lift->getSize().x / CORE_RESOURCE_SLOT_WIDTH))
			throw invalid_argument("Declared platform lift capacity cannot be represented by separated standing positions");
		vector<Vector2> positions(capacity);
		auto start = (lift->getSize().x - capacity * CORE_RESOURCE_SLOT_WIDTH) * 0.5f
			+ CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { start + i * CORE_RESOURCE_SLOT_WIDTH, 0.0f };
		auto stopDurationTicks = secondsToTicks(stopDurationSeconds, mWorld.getFixedTimestep());
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name,
			std::move(lift), locationSector, std::move(stops), capacity,
			stopDurationTicks, stopDurationTicks, std::move(positions))));
		auto resource = mWorld.mTraversalResources.find(id);
		resource->mOpenPlatformLift = true;
		resource->mVirtualBoundaryOwners.resize(1);
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *resource);
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createShuttleTraversalResource(string const& name,
		shared_ptr<Shuttle> shuttle, SectorId shuttleSector, vector<LiftStop> stops,
		uint32_t capacity, float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!shuttle || shuttle->getNumCars() == 0 || !shuttleSector
			|| shuttleSector.value > mWorld.mSectors.size() || stops.size() < 2 || capacity == 0)
			throw invalid_argument("A coupled shuttle requires valid stops, carriages, and positive per-carriage capacity");
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(minimumDwellSeconds) || !isFiniteTiming(maximumBoardingSeconds)
			|| maximumBoardingSeconds < minimumDwellSeconds)
		{
			throw invalid_argument("A coupled shuttle requires finite dwell timing with 0 <= minimum dwell <= maximum boarding time");
		}
		mWorld.beginStructuralEdit("createShuttleTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			auto landing = mWorld.mTraversalResources.find(stops[i].landingResource);
			if (!stops[i].locationSector || stops[i].locationSector.value > mWorld.mSectors.size()
				|| !landing || !landing->mDoor)
				throw invalid_argument(format("Shuttle stop {} has an invalid location or landing-door mapping", i));
			if (!isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Shuttle stop {} has invalid linear geometry", i));
		}
		auto usableWidth = (float)shuttle->getCarWidth();
		auto const representablePositions = maximumShuttleCarriageCapacity(
			shuttle->getCarWidth());
		if (capacity > representablePositions)
			throw invalid_argument("Declared shuttle capacity cannot be represented by buffered carriage standing positions");
		vector<Vector2> positions;
		positions.reserve(capacity * shuttle->getNumCars());
		auto const first = CORE_RESOURCE_SLOT_WIDTH * 0.5f + CORE_SHUTTLE_OCCUPANT_CLEARANCE;
		auto const last = usableWidth - CORE_RESOURCE_SLOT_WIDTH * 0.5f
			- CORE_SHUTTLE_OCCUPANT_CLEARANCE;
		for (uint32_t carriage = 0; carriage < shuttle->getNumCars(); ++carriage)
			for (uint32_t i = 0; i < capacity; ++i)
			{
				auto const progress = capacity == 1 ? 0.5f : (float)i / (float)(capacity - 1);
				positions.push_back({ carriage * (shuttle->getCarWidth() + 1.0f)
					+ first + (last - first) * progress, 0.0f });
			}
		auto minimumDwellTicks = secondsToTicks(minimumDwellSeconds, mWorld.getFixedTimestep());
		auto maximumBoardingTicks = secondsToTicks(maximumBoardingSeconds, mWorld.getFixedTimestep());
		auto shuttlePtr = shuttle;
		auto stopCount = (uint32_t)stops.size();
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(shuttle), shuttleSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		auto resource = mWorld.mTraversalResources.find(id);
		resource->mShuttleCarriages.reserve(shuttlePtr->getNumCars());
		for (uint32_t carriage = 0; carriage < shuttlePtr->getNumCars(); ++carriage)
			resource->mShuttleCarriages.push_back({ carriage, carriage * capacity, capacity,
				std::vector<std::vector<TraversalResourceId>>(stopCount), {}, {}, {},
				TraversalDirection::None });
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createStairwellTraversalResource(string const& name,
		shared_ptr<Stairwell> stairwell, SectorId stairwellSector, uint32_t capacity,
		uint32_t directionalBatchLimit)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.beginStructuralEdit("createStairwellTraversalResource");
		if (!stairwell || !stairwellSector || stairwellSector.value > mWorld.mSectors.size()
			|| capacity == 0 || directionalBatchLimit == 0)
		{
			throw invalid_argument("A narrow stairwell resource requires a Stairwell, sector, capacity, and batch limit");
		}
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = stairwell->getPosition();
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ origin.x + ((float)i + 0.5f) * stairwell->getSize().x / capacity,
				origin.y + stairwell->getSize().y * 0.5f });
		}
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, stairwell, stairwellSector, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId TraversalResourceRegistry::createForceBridgeTraversalResource(string const& name,
		shared_ptr<ForceBridge> forceBridge)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.beginStructuralEdit("createForceBridgeTraversalResource");
		if (!forceBridge) throw invalid_argument("A force bridge traversal resource requires a ForceBridge");
		auto id = mWorld.mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, forceBridge, forceBridge->isExtensible()
				? static_pointer_cast<ExtensibleObject>(forceBridge) : nullptr)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mWorld.mTraversalResources.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	// ----------------------------------------------------------------------
	// Identity and lookup
	// ----------------------------------------------------------------------

	EntityLookup<TraversalResource> TraversalResourceRegistry::lookupTraversalResource(TraversalResourceId id)
	{
		auto entity = mWorld.mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource>{ entity, {} }
			: EntityLookup<TraversalResource>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<TraversalResource const> TraversalResourceRegistry::lookupTraversalResource(TraversalResourceId id) const
	{
		auto entity = mWorld.mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource const>{ entity, {} }
			: EntityLookup<TraversalResource const>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	TraversalResourceId TraversalResourceRegistry::getTraversalResourceId(Object const* object) const
	{
		if (!object) return {};
		for (auto const& [id, resource] : mWorld.mTraversalResources.entries())
		{
			bool matches = resource->mDoor.get() == object || resource->mWindow.get() == object
				|| resource->mLadder.get() == object || resource->mForceBridge.get() == object
				|| resource->mLift.get() == object || resource->mShuttle.get() == object
				|| resource->mStairwell.get() == object;
			if (!matches) continue;
			return resource->mLiftCoordinator ? resource->mLiftCoordinator : id;
		}
		return {};
	}

	// ----------------------------------------------------------------------
	// Removal
	// ----------------------------------------------------------------------

	EntityRemovalResult TraversalResourceRegistry::removeTraversalResource(TraversalResourceId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupTraversalResource(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		bool structural = found.entity->mDoor || found.entity->mWindow || found.entity->mLadder
			|| found.entity->mForceBridge || found.entity->mLift || found.entity->mShuttle
			|| found.entity->mStairwell;
		if (!structural && mWorld.mGraph)
			structural = any_of(mWorld.mGraph->getEdges().begin(), mWorld.mGraph->getEdges().end(),
				[id](auto const& edge) { return edge->getTraversalResourceId() == id; });
		if (structural) mWorld.beginStructuralEdit("removeTraversalResource");
		auto hasOwner = [](auto const& values)
			{ return any_of(values.begin(), values.end(), [](auto value) { return (bool)value; }); };
		bool owned = hasOwner(found.entity->mOccupants)
			|| hasOwner(found.entity->mAdmissionReservations)
			|| hasOwner(found.entity->mCrossingOwners)
			|| hasOwner(found.entity->mVirtualBoundaryOwners)
			|| !found.entity->mAdmissionQueue.empty() || !found.entity->mOpenLeases.empty()
			|| !found.entity->mExtensionRequestLeases.empty()
			|| !found.entity->mExtensionOccupantLeases.empty()
			|| !found.entity->mPlayerStopRequests.empty()
			|| any_of(found.entity->mQueueLanes.begin(), found.entity->mQueueLanes.end(),
				[](auto const& lane) { return !lane.queue.empty(); })
			|| any_of(found.entity->mLiftStopRequestOwners.begin(), found.entity->mLiftStopRequestOwners.end(),
				[](auto const& owners) { return !owners.empty(); });
		if (owned)
			return { false, format("TraversalResource handle {} still has active ownership and cannot be replaced safely", id.value) };
		auto snapshot = makeTraversalResourceSnapshot(id, *found.entity);
		mWorld.mTraversalResources.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalResourceRemoved;
		event.traversalResource = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	// ----------------------------------------------------------------------
	// Ownership and snapshots
	//
	// The snapshot projection and the ownership release paths are executed
	// by SimulationCoordinator, where the traversal scheduling they ride on
	// lives (ADR 0004): releasing ownership cancels requests and releases
	// permits through the ordinary transaction paths, and the lift stop-
	// request upkeep belongs to the lift dispatch code. The registry is the
	// facade seam (ADR 0022), not a second implementation.
	// ----------------------------------------------------------------------

	TraversalResourceSnapshot TraversalResourceRegistry::makeTraversalResourceSnapshot(
		TraversalResourceId id, TraversalResource const& resource) const
	{
		return mWorld.mSimulationCoordinator.makeTraversalResourceSnapshot(id, resource);
	}

	bool TraversalResourceRegistry::holdsTraversalOwnership(AgentId id) const
	{
		return mWorld.mSimulationCoordinator.holdsTraversalOwnership(id);
	}

	void TraversalResourceRegistry::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.mSimulationCoordinator.releaseAgentFromResource(resource, id);
	}

	void TraversalResourceRegistry::releaseTraversalOwnership(AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		mWorld.mSimulationCoordinator.releaseTraversalOwnership(id);
	}

	// ----------------------------------------------------------------------
	// Policies
	// ----------------------------------------------------------------------

	TraversalWaitingPolicy const& TraversalResourceRegistry::getTraversalWaitingPolicy() const
	{
		return mWorld.mTraversalWaitingPolicy;
	}

	void TraversalResourceRegistry::setTraversalWaitingPolicy(TraversalWaitingPolicy policy)
	{
		mWorld.invalidateSimulationSnapshot();
		if (policy.localGoalTimeoutTicks == 0 || policy.permitProgressTimeoutTicks == 0
			|| policy.replanIntervalTicks == 0 || policy.queueDelayPerAgentSeconds < 0.0f
			|| policy.replanEtaMarginSeconds < 0.0f)
		{
			throw invalid_argument("Traversal waiting policy durations must be positive and costs non-negative");
		}
		mWorld.mTraversalWaitingPolicy = policy;
	}

	TraversalGeometryPolicy const& TraversalResourceRegistry::getTraversalGeometryPolicy() const
	{
		return mWorld.mTraversalGeometryPolicy;
	}

	void TraversalResourceRegistry::setTraversalGeometryPolicy(TraversalGeometryPolicy policy)
	{
		if (!isfinite(policy.minimumQueueSeparation)
			|| !isfinite(policy.advanceStepThreshold)
			|| !isfinite(policy.overflowTailSeparation)
			|| !isfinite(policy.occupantClearance)
			|| policy.minimumQueueSeparation < CORE_RESOURCE_QUEUE_SLOT_PITCH
			|| policy.advanceStepThreshold < 0.0f
			|| policy.overflowTailSeparation < 0.0f
			|| policy.occupantClearance < 0.0f)
		{
			throw invalid_argument("Traversal geometry policy values must be finite and non-negative, and queue separation cannot be below the authored lane pitch");
		}
		mWorld.invalidateSimulationSnapshot();
		mWorld.mTraversalGeometryPolicy = policy;

		// Lift slots are authored when the traversal resource is built, while the
		// geometry policy remains configurable afterwards. Keep existing enclosed
		// cars and their car-side boarding lanes in sync with the World policy.
		for (auto const& entry : mWorld.mTraversalResources.entries())
		{
			auto& resource = *entry.second;
			if (!resource.mLift || resource.mOpenPlatformLift || resource.mShuttle
				|| resource.mCapacityPositions.empty()) continue;
			auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
			auto const usableWidth = resource.mLift->getSize().x;
			auto const targets = packOccupants(resource.mCapacityPositions.size(), 0,
				{ halfAgentWidth, usableWidth - halfAgentWidth }, CORE_RESOURCE_SLOT_WIDTH,
				policy.occupantClearance, OccupantPackingOrder::Forward,
				OccupantPackingLayout::Compact);
			for (size_t i = 0; i < targets.size(); ++i)
				resource.mCapacityPositions[i].x = targets[i];

			for (auto const& stop : resource.mLiftStops)
			{
				auto landing = mWorld.mTraversalResources.find(stop.landingResource);
				if (!landing) continue;
				for (auto& lane : landing->mQueueLanes)
				{
					if (lane.sector != resource.mLiftSector) continue;
					lane.positions.clear();
					for (auto const& position : resource.mCapacityPositions)
						lane.positions.push_back({ resource.mLift->getPosition().x + position.x,
							stop.globalPosition + position.y });
					lane.positionOwners.resize(lane.positions.size());
					if (!lane.positions.empty())
					{
						lane.origin = lane.positions.front();
						lane.direction = Vector2::UNIT_X;
						lane.extent = lane.positions.back().x - lane.positions.front().x;
					}
				}
			}
		}
	}
}
