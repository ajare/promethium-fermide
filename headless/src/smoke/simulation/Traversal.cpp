#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>
#include "core/GapEdge.h"
#include "core/SectorEdge.h"

namespace
{
	using smoke::twoNodePath;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	bool ordinaryTraversalCommitsOnlyAtDestination()
	{
		core::World world("Ordinary transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Ordinary traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		bool observedPermit = false;
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (!snapshot.traversalPermits.empty())
			{
				observedPermit = snapshot.traversalPermits.size() == 1
					&& snapshot.traversalRequests.size() == 1
					&& snapshot.traversalRequests.front().diagnostic.starts_with("Active:")
					&& snapshot.agents.front().hasLocomotionTask;
			}

			if (agent->getState() != core::Agent::State::Idle
				&& agent->getSector() != world.getSector(sourceSector).get())
			{
				return false;
			}
		}

		auto snapshot = world.getSimulationSnapshot();
		return observedPermit
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(destinationSector).get()
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool deniedTraversalCannotBeCrossed()
	{
		core::World world("Denied transition", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Blocked traveller", corridor, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::GapEdge>()), true);
		world.advanceTicks(30);

		auto snapshot = world.getSimulationSnapshot();
		if (agent->getGlobalPosition().distanceTo(source->getPosition()) >= 0.001f
			|| agent->getSector() != world.getSector(corridor).get()
			|| agent->getState() != core::Agent::State::WaitingForTraversal
			|| snapshot.traversalRequests.size() != 1
			|| snapshot.traversalRequests.front().state != core::TraversalRequestState::Denied
			|| !snapshot.traversalRequests.front().diagnostic.starts_with("Denied:")
			|| !snapshot.traversalPermits.empty())
		{
			return false;
		}

		agent->clearPath();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
			&& agent->getSector() == world.getSector(corridor).get();
	}

	bool cancellationReleasesPermitWithoutCommitting()
	{
		core::World world("Cancelled transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Cancelling traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		for (uint32_t i = 0; i < 10 && !agent->getTraversalPermitId(); ++i)
		{
			world.advanceTick();
		}
		if (!agent->getTraversalPermitId() || agent->getSector() != world.getSector(sourceSector).get())
		{
			return false;
		}

		agent->clearPath();
		world.advanceTicks(10);
		auto snapshot = world.getSimulationSnapshot();
		return agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(sourceSector).get()
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}
}

void registerTraversal(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "ordinaryTraversalCommitsOnlyAtDestination", [](smoke::Context const&)
		{
			smoke::require(ordinaryTraversalCommitsOnlyAtDestination(), "ordinary traversal did not hold a permit through atomic commit");
		} });
	checks.push_back({ "deniedTraversalCannotBeCrossed", [](smoke::Context const&)
		{
			smoke::require(deniedTraversalCannotBeCrossed(), "denied traversal was crossed or leaked its request");
		} });
	checks.push_back({ "cancellationReleasesPermitWithoutCommitting", [](smoke::Context const&)
		{
			smoke::require(cancellationReleasesPermitWithoutCommitting(), "traversal cancellation leaked or committed membership");
		} });
}
