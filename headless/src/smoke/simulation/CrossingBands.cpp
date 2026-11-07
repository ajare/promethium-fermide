#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"

namespace
{
	using smoke::twoNodePath;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	// Ticket #97: the band predicate itself - within crossingWidth in x of the
	// threshold, on the threshold row in y.
	bool doorCrossingBandPredicateShape()
	{
		auto const width = CORE_DOOR_CROSSING_HALF_WIDTH(3);
		if (std::abs(width - 1.2f) > 0.0001f) return false;
		if (std::abs(CORE_DOOR_CROSSING_HALF_WIDTH(1) - 0.2f) > 0.0001f) return false;

		auto const threshold = core::Vector2{ 4.5f, 0.0f };
		if (!core::isWithinDoorCrossingBand({ 4.5f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 3.3f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 5.7f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 3.2f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 5.8f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, 0.1f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, -0.01f }, threshold, width)) return false;
		return true;
	}

	// Ticket #97: Door vertices carry the crossing width derived from the
	// physical doorway (cell width minus the x insets) minus the agent width.
	bool doorVertexCarriesCrossingWidth()
	{
		core::World world("Crossing width vertices", 8, 2);
		world.addRoom("Width fore", 0, 0, 0, 7, 1);
		world.addRoom("Width back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions wide;
		wide.width = 3;
		world.addSectorDoor(0, 0, 1);
		world.addSectorDoor(0, 0, 3, wide);
		world.finishBuild();

		bool foundNarrow = false;
		bool foundWide = false;
		for (auto const& vertex : world.getGraph()->getVertices())
		{
			auto doorVertex = std::dynamic_pointer_cast<const core::DoorVertex>(vertex);
			if (!doorVertex || !doorVertex->getDoor()) continue;
			if (doorVertex->getDoor()->getCellsWide() == 1)
			{
				foundNarrow = std::abs(doorVertex->getCrossingWidth() - 0.2f) <= 0.0001f;
				if (!foundNarrow) return false;
			}
			else if (doorVertex->getDoor()->getCellsWide() == 3)
			{
				foundWide = std::abs(doorVertex->getCrossingWidth() - 1.2f) <= 0.0001f;
				if (!foundWide) return false;
			}
		}
		return foundNarrow && foundWide;
	}

	// Runs a contended manual door with a blocker and a waiter that enters the
	// flow at the band, and records the waiter's request and grant moments
	// relative to the band.
	struct CrossingBandTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool grantedBeforeCentre{ false };
		bool grantedWithinBand{ false };
		bool crossedOver{ false };
	};

	CrossingBandTrace runCrossingWidthGrantScenario(uint32_t cellsWide)
	{
		CrossingBandTrace trace;
		core::World world("Crossing width grant", 10, 2);
		auto fore = world.addRoom("Band fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Band back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		// Pin the narrow-band scenario to a fully open threshold so request
		// creation is not instead triggered by the occupied waiting queue.
		if (cellsWide == 1) {
			world.acquireDoorOpenLease(created.traversalResource);
			auto door = std::static_pointer_cast<const core::DoorSectorObject>(created.door.sector->getObject(created.door.index))->getDoor();
			door->requestOpen(); world.advanceTicks(150);
		}

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto blockerId = world.createAgent("Band blocker", fore, 0, 7.0f);
		auto waiterId = world.createAgent("Band waiter", fore, 0, 8.0f);
		world.lookupAgent(blockerId).entity->setPath(twoNodePath(source, destination, edge), true);
		world.lookupAgent(waiterId).entity->setPath(twoNodePath(source, destination, edge), true);

		bool firstObservation = true;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto waiter = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiterId; });
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiterId; });
			if (waiter == snapshot.agents.end() || request == snapshot.traversalRequests.end())
			{
				continue;
			}
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(waiter->globalPosition.x)) + ':'
				+ std::to_string(snapshot.traversalPermits.size()) + ';';
			auto const hasPermit = std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; });
			auto const dx = std::abs(waiter->globalPosition.x - centre.x);
			// Ticket #98: the request-creation gate is the band itself, so the
			// waiter's first observed request sits inside the band.
			if (firstObservation)
			{
				firstObservation = false;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
			}
			if (!hasPermit)
			{
				continue;
			}
			trace.grantedWithinBand = dx <= crossingWidth + 0.001f
				&& std::abs(waiter->globalPosition.y - centre.y) <= 0.001f;
			// The waiter approaches from the right; "before centre" means it is
			// still short of the door centre by a clear margin.
			trace.grantedBeforeCentre = waiter->globalPosition.x > centre.x + 0.25f;
			break;
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.crossedOver = world.lookupAgent(waiterId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	// Ticket #97/#98: at a wide door the head of queue enters the flow inside
	// the crossing band and is granted from there without reaching its
	// assigned centre position, and repeated runs produce identical traces.
	bool crossingWidthGrantsHeadOfQueueBeforeCentre()
	{
		auto const first = runCrossingWidthGrantScenario(3);
		auto const second = runCrossingWidthGrantScenario(3);
		return first.createdWithinBand && first.grantedWithinBand && first.grantedBeforeCentre
			&& first.crossedOver && !first.text.empty() && first.text == second.text;
	}

	// Ticket #97/#98: at a 1-cell door the band is only +/-0.2. With the
	// request-creation gate on the band the waiter enters the flow at the
	// band edge and is granted within the +/-0.2 tolerance of the centre -
	// never before it - and repeated runs are identical.
	bool narrowDoorBandArrivalGrantsAtCentreTolerance()
	{
		auto const first = runCrossingWidthGrantScenario(1);
		auto const second = runCrossingWidthGrantScenario(1);
		return first.createdWithinBand && first.grantedWithinBand
			&& !first.grantedBeforeCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: a lone Agent approaching an open wide Door enters the
	// traversal flow - request created, queue ticket taken - as soon as it is
	// within the crossing width at the threshold row, and crosses from where
	// it stands without ever converging on the door centre.
	struct BandEntryTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool createdOnThresholdRow{ false };
		bool createdOffCentre{ false };
		bool grantedOffCentre{ false };
		bool neverNearedCentre{ false };
		bool crossedOver{ false };
	};

	BandEntryTrace runBandEntryScenario(uint32_t cellsWide)
	{
		BandEntryTrace trace;
		core::World world("Band entry crossing", 10, 2);
		auto fore = world.addRoom("Entry fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Entry back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		// Hold the door open so the grant lands as soon as the request exists.
		if (!world.acquireDoorOpenLease(created.traversalResource)) return trace;
		// This scenario measures arrival at an already open Door, not leaf motion.
		auto door = std::static_pointer_cast<const core::DoorSectorObject>(created.door.sector->getObject(created.door.index))->getDoor();
		door->requestOpen(); world.advanceTicks(150);

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto agentId = world.createAgent("Band arriver", fore, 0, centre.x + 2.5f);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(source, destination, edge), true);

		auto minDx = 1000.0f;
		bool requestObserved = false;
		bool granted = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			if (world.lookupAgent(agentId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
			auto snapshot = world.getSimulationSnapshot();
			auto agent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == agentId; });
			if (agent == snapshot.agents.end()) return trace;
			auto const dx = std::abs(agent->globalPosition.x - centre.x);
			minDx = std::min(minDx, dx);
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == agentId; });
			if (request == snapshot.traversalRequests.end()) continue;
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(agent->globalPosition.x)) + ';';
			if (!requestObserved)
			{
				requestObserved = true;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
				trace.createdOnThresholdRow
					= std::abs(agent->globalPosition.y - centre.y) <= 0.001f;
				trace.createdOffCentre = dx > 0.5f;
			}
			if (!granted && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; }))
			{
				granted = true;
				trace.grantedOffCentre = dx > 0.5f;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.neverNearedCentre = minDx > 0.5f;
		trace.crossedOver = world.lookupAgent(agentId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	bool bandArrivalCrossesWideDoorFromStandingPosition()
	{
		auto const first = runBandEntryScenario(3);
		auto const second = runBandEntryScenario(3);
		return first.createdWithinBand && first.createdOnThresholdRow && first.createdOffCentre
			&& first.grantedOffCentre && first.neverNearedCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: band arrival composes with the queue and the existing early
	// stop at a contended wide door. A second Agent joins while the first is
	// still waiting inside the band: both requests share the queue, the grant
	// follows ticket order on the single crossing lane, and neither Agent is
	// stranded between the gates.
	bool bandArrivalComposesWithEarlyStopForContendedDoor()
	{
		core::World world("Band contention", 10, 2);
		auto fore = world.addRoom("Contended fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Contended back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.crossingLanes = 1;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(3);

		auto firstId = world.createAgent("Contended first", fore, 0, 7.0f);
		world.lookupAgent(firstId).entity->setPath(twoNodePath(source, destination, edge), true);

		core::AgentId secondId{};
		bool overlappedPending{ false };
		bool secondCreatedOffCentre{ false };
		bool secondHadQueuePosition{ false };
		bool firstGrantedBeforeSecond{ false };
		int firstGrantTick = -1;
		int secondGrantTick = -1;
		std::string text;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto firstRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == firstId; });
			// Spawn the second Agent just outside the band once the first is
			// queued inside it, so the two requests contend for one lane.
			if (!secondId && firstRequest != snapshot.traversalRequests.end()
				&& firstRequest->state == core::TraversalRequestState::Pending
				&& firstRequest->hasQueuePosition)
			{
				secondId = world.createAgent("Contended second", fore, 0,
					centre.x + crossingWidth + 0.25f);
				world.lookupAgent(secondId).entity->setPath(
					twoNodePath(source, destination, edge), true);
			}
			if (!secondId) continue;
			auto secondRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == secondId; });
			auto secondAgent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == secondId; });
			if (secondRequest == snapshot.traversalRequests.end()
				|| secondAgent == snapshot.agents.end())
			{
				continue;
			}
			// The first request may already be retired while the second Agent
			// is still crossing. Keep observing the second without dereferencing end().
			auto const hasFirstRequest = firstRequest != snapshot.traversalRequests.end();
			text += std::to_string(tick) + ':'
				+ std::to_string(hasFirstRequest ? (int)firstRequest->state : -1) + ':'
				+ std::to_string((int)secondRequest->state) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(secondAgent->globalPosition.x)) + ';';
			if (!secondCreatedOffCentre)
			{
				secondCreatedOffCentre
					= std::abs(secondAgent->globalPosition.x - centre.x) > 0.25f;
			}
			secondHadQueuePosition = secondHadQueuePosition || secondRequest->hasQueuePosition;
			overlappedPending = overlappedPending
				|| (hasFirstRequest && firstRequest->state == core::TraversalRequestState::Pending
					&& secondRequest->state == core::TraversalRequestState::Pending);
			if (hasFirstRequest && firstGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == firstRequest->id; }))
			{
				firstGrantTick = (int)tick;
			}
			if (secondGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == secondRequest->id; }))
			{
				secondGrantTick = (int)tick;
				firstGrantedBeforeSecond = firstGrantTick >= 0 && firstGrantTick < secondGrantTick;
			}
			if (firstGrantedBeforeSecond && secondGrantTick >= 0
				&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
				&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return overlappedPending && secondCreatedOffCentre && secondHadQueuePosition
			&& firstGrantedBeforeSecond
			&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get()
			&& !text.empty();
	}

	// Ticket #98: the band only arms an Agent whose next edge crosses the
	// Door. An Agent walking through the band's x range at the threshold row
	// with no intent to cross never creates a traversal request.
	bool bandArrivalLeavesNonCrossingAgentsUnaffected()
	{
		core::World world("Band passer by", 10, 2);
		auto fore = world.addRoom("Passer fore", 0, 0, 0, 9, 1);
		world.addRoom("Passer back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t pastDoorId;
		world.addSectorMarker(fore, 0, 8.0f, &pastDoorId);
		world.finishBuild();

		auto walker = world.createAgent("Passer by", fore, 0, 1.0f);
		auto walkerEntity = world.lookupAgent(walker).entity;
		auto target = world.getGraph()->getVertexByIdentifier(pastDoorId);
		if (!target) return false;
		auto path = world.getGraph()->calculatePath(walkerEntity, target);
		if (!path) return false;
		walkerEntity->setPath(std::move(path), true);

		bool crossedBandRow = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			// Only a crossing intent arms the Door gate; the walker's ordinary
			// Location-edge requests must never target the door resource.
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == walker && request.resource == created.traversalResource)
					return false;
			auto const x = walkerEntity->getGlobalPosition().x;
			crossedBandRow = crossedBandRow || (x > 3.3f && x < 5.7f);
			if (walkerEntity->getState() == core::Agent::State::Idle) break;
		}
		return crossedBandRow && walkerEntity->getGlobalPosition().x > 7.0f;
	}
}

void registerCrossingBands(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorCrossingBandPredicateShape", [](smoke::Context const&)
		{
			smoke::require(doorCrossingBandPredicateShape(), "door crossing width band predicate admitted or refused the wrong positions");
		} });
	checks.push_back({ "doorVertexCarriesCrossingWidth", [](smoke::Context const&)
		{
			smoke::require(doorVertexCarriesCrossingWidth(), "Door vertices did not carry the derived crossing width");
		} });
	checks.push_back({ "crossingWidthGrantsHeadOfQueueBeforeCentre", [](smoke::Context const&)
		{
			smoke::require(crossingWidthGrantsHeadOfQueueBeforeCentre(), "wide door head of queue was not granted from within the crossing band");
		} });
	checks.push_back({ "narrowDoorBandArrivalGrantsAtCentreTolerance", [](smoke::Context const&)
		{
			smoke::require(narrowDoorBandArrivalGrantsAtCentreTolerance(), "1-cell door band arrival was not granted at its centre tolerance");
		} });
	checks.push_back({ "bandArrivalCrossesWideDoorFromStandingPosition", [](smoke::Context const&)
		{
			smoke::require(bandArrivalCrossesWideDoorFromStandingPosition(), "lone agent did not cross a wide door from its band-entry position");
		} });
	checks.push_back({ "bandArrivalComposesWithEarlyStopForContendedDoor", [](smoke::Context const&)
		{
			smoke::require(bandArrivalComposesWithEarlyStopForContendedDoor(), "band arrival did not compose with the queue at a contended door");
		} });
	checks.push_back({ "bandArrivalLeavesNonCrossingAgentsUnaffected", [](smoke::Context const&)
		{
			smoke::require(bandArrivalLeavesNonCrossingAgentsUnaffected(), "non-crossing agent inside the band x range created a request");
		} });
}
