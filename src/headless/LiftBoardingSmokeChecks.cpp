#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"

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

// Replay authored paths, including opposing landing queues. A capacity-reserved
// boarder must reach its boarding position and retain the admitted run direction.
void runLiftBoardingRepro(char const* filename)
{
	checkLiftBoarding(filename, false);
	checkLiftBoarding(filename, true);
	std::cout << "PASS: Lift demand drained in full and reduced boarding regressions\n";
}
