#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/Sector.h"
#include "core/Defines.h"

namespace
{
	void checkLiftBoarding(char const* filename, bool reduced)
	{
		auto world = core::loadWorldDocument(filename);
		if (!world) throw std::runtime_error("Lift boarding regression could not load World");
		if (reduced)
		{
			// Minimized opposing-direction reproduction from lift-test-1. Removing
			// any of these five Agents prevents the original level-1 stall.
			constexpr std::array<uint64_t, 5> retained{ 3, 8, 9, 13, 21 };
			auto const initial = world->getSimulationSnapshot();
			for (auto const& agent : initial.agents)
				if (std::find(retained.begin(), retained.end(), agent.id.value) == retained.end())
				{
					world->lookupAgent(agent.id).entity->clearPath();
					if (!world->removeAgent(agent.id))
						throw std::runtime_error("Could not reduce Lift boarding reproduction");
				}
		}
		if (!world->resumeSimulation() || !world->advanceTicks(36000))
			throw std::runtime_error("Lift boarding regression could not run World");
		auto const snapshot = world->getSimulationSnapshot();
		bool sawLift = false;
		for (auto const& resource : snapshot.traversalResources)
		{
			if (!resource.isLift) continue;
			sawLift = true;
			if (resource.admissionReservationCount || resource.occupantCount
				|| std::any_of(resource.liftStopRequestOwnerCounts.begin(),
					resource.liftStopRequestOwnerCounts.end(), [](auto count) { return count != 0; }))
				throw std::runtime_error("Lift boarding stalled with outstanding demand: " + resource.name);
		}
		if (!sawLift || snapshot.agents.empty())
			throw std::runtime_error("Lift boarding regression has no Lift or Agents");
		// The complete document also exercises unrelated routes (including a
		// denied Staircase traversal). Require full completion in the reduced
		// case, and drained Lift demand in both cases.
		if (reduced)
			for (auto const& agent : snapshot.agents)
				if (agent.state != core::AgentPathState::Idle)
					throw std::runtime_error("Lift boarding journey did not finish: " + agent.name);
	}
}

static void checkLiftCrossings(char const* filename, bool reduced)
{
	auto world = core::loadWorldDocument(filename);
	if (!world) throw std::runtime_error("Lift crossing regression could not load World");
	if (reduced)
	{
		// Two waiters at the same landing suffice: one occupies the threshold
		// queue position, and the other used to cross from the adjacent position.
		auto const initial = world->getSimulationSnapshot();
		for (auto const& agent : initial.agents)
			if (agent.id.value != 20 && agent.id.value != 21)
			{
				world->lookupAgent(agent.id).entity->clearPath();
				if (!world->removeAgent(agent.id))
					throw std::runtime_error("Could not reduce Lift crossing reproduction");
			}
	}
	if (!world->resumeSimulation()) throw std::runtime_error("Could not resume Lift crossing World");
	std::set<uint64_t> checked;
	for (unsigned tick = 0; tick < 18000; ++tick)
	{
		if (!world->advanceTick()) throw std::runtime_error("Could not advance Lift crossing World");
		auto const& snapshot = world->getSimulationSnapshotView();
		for (auto const& request : snapshot.traversalRequests)
		{
			if (request.state != core::TraversalRequestState::Granted
				|| request.sourceSector == request.destinationSector) continue;
			auto sector = world->getSector((uint32_t)request.destinationSector.value - 1);
			if (!sector || sector->getType() != core::SectorType::Lift
				|| !checked.insert(request.id.value).second) continue;
			auto agent = world->lookupAgent(request.owner).entity;
			auto const position = agent->getGlobalPosition();
			// Both fixture Lifts have full-width landing Doors (one and two cells).
			auto const width = CORE_DOOR_CROSSING_HALF_WIDTH(sector->getCellsWide());
			if (!core::isWithinDoorCrossingBand(position, request.sourceEndpoint, width))
			{
				std::cerr << "Boarding at tick " << tick + 1 << ": Agent " << request.owner.value
					<< " x=" << position.x << " threshold=" << request.sourceEndpoint.x
					<< " half-width=" << width << '\n';
				throw std::runtime_error("Lift boarding started outside the crossing band");
			}
		}
	}
	if (checked.empty()) throw std::runtime_error("No Lift crossings checked");
	if (reduced)
		for (auto const& agent : world->getSimulationSnapshotView().agents)
			if (agent.state != core::AgentPathState::Idle)
				throw std::runtime_error("Lift crossing regression stranded a boarder");
}

void runLiftCrossingRepro(char const* filename)
{
	checkLiftCrossings(filename, false);
	checkLiftCrossings(filename, true);
	std::cout << "PASS: Lift boarding stays within the landing doorway in full and two-Agent runs\n";
}

// Replay authored paths, including opposing landing queues. A capacity-reserved
// boarder must reach its boarding position and retain the admitted run direction.
void runLiftBoardingRepro(char const* filename)
{
	checkLiftBoarding(filename, false);
	checkLiftBoarding(filename, true);
	std::cout << "PASS: Lift demand drained in full and reduced boarding regressions\n";
}
