#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <string>
#include <vector>

namespace
{
	using smoke::twoNodePath;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	bool pausedTopologyRebuildIsAtomicAndCleansOwnership()
	{
		core::World world("Paused topology rebuild", 8, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto door = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto generation = world.getTopologyGeneration();
		auto oldGraph = world.getGraph();
		auto edge = *std::find_if(oldGraph->getEdges().begin(), oldGraph->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rebuild traveller", fore, 0,
			source->getPosition().x - world.getSector(fore)->getPosition().x);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(3);
		auto active = world.getSimulationSnapshot();
		if (active.traversalRequests.empty()) return false;

		// An active simulation cannot be structurally changed.
		bool rejected = false;
		try { world.addSectorMarker(fore, 0, 0.5f); }
		catch (std::exception const&) { rejected = true; }
		if (!rejected || world.isSimulationPaused()) return false;

		world.pauseSimulation();
		auto pausedTick = world.getSimulationTick();
		world.advanceTicks(10);
		auto paused = world.getSimulationSnapshot();
		if (!paused.paused || world.getSimulationTick() != pausedTick
			|| !paused.traversalRequests.empty() || !paused.traversalPermits.empty()) return false;
		for (auto const& resource : paused.traversalResources)
		{
			if (resource.id != door.traversalResource) continue;
			if (resource.openLeaseCount || resource.crossingOwner
				|| std::any_of(resource.queueLanes.begin(), resource.queueLanes.end(),
					[](auto const& lane) { return std::any_of(lane.positions.begin(), lane.positions.end(),
						[](auto const& position) { return (bool)position.owner; }); })) return false;
		}

		uint32_t marker;
		world.addSectorMarker(fore, 0, 0.5f, &marker);
		if (!world.isTraversalTopologyDirty() || !world.rebuildTraversalTopology()
			|| world.getGraph() == oldGraph || world.getTopologyGeneration() != generation + 1
			|| !world.getGraph()->getVertexByIdentifier(marker)
			|| !world.resumeSimulation()) return false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& agent->getState() != core::Agent::State::Idle; ++i) world.advanceTick();
		if (agent->getSector() != world.getSector(back).get()) return false;

		// Candidate failure leaves the previous graph installed, the simulation
		// paused, and removed handles permanently invalid.
		core::World invalid("Invalid paused rebuild", 6, 2);
		invalid.addRoom("Fore", 0, 0, 0, 5, 1);
		invalid.addRoom("Back", 1, 0, 0, 5, 1);
		auto invalidDoor = invalid.addSectorDoor(0, 0, 2);
		invalid.finishBuild();
		auto previousGraph = invalid.getGraph();
		invalid.pauseSimulation();
		if (!invalid.removeTraversalResource(invalidDoor.traversalResource)
			|| invalid.lookupTraversalResource(invalidDoor.traversalResource)) return false;
		auto replacement = invalid.createTraversalResource("Replacement handle proof");
		if (replacement.value <= invalidDoor.traversalResource.value
			|| invalid.rebuildTraversalTopology() || invalid.resumeSimulation()
			|| !invalid.isSimulationPaused() || invalid.getGraph() != previousGraph
			|| invalid.getTopologyDiagnostic().empty()) return false;
		return true;
	}

	bool graphBuildWarnsForLocationWithoutVertices()
	{
		core::World world("Graph diagnostics", 8, 1);
		world.addRoom("Empty Room", 0, 0, 0, 3, 1);
		auto const connected = world.addRoom("Connected Room", 0, 0, 3, 3, 1);
		world.addSectorMarker(connected, 0, 1.5f, "Connected Marker");
		world.finishBuild();

		return std::count_if(world.getBuildLog().begin(), world.getBuildLog().end(),
			[](core::LogMessage const& entry)
			{
				return entry.level == core::LogLevel::Warning
					&& entry.msg == "Sector 'Empty Room' on level 0, layer 0 has no vertices.";
			}) == 1;
	}

	bool traversalGeometryPolicyIsWorldOwned()
	{
		core::World configured("Configured traversal geometry", 2, 1);
		core::World untouched("Default traversal geometry", 2, 1);

		auto const& defaults = untouched.getTraversalGeometryPolicy();
		if (defaults.minimumQueueSeparation != CORE_RESOURCE_QUEUE_SLOT_PITCH
			|| defaults.advanceStepThreshold != CORE_RESOURCE_QUEUE_ADVANCE_THRESHOLD
			|| defaults.overflowTailSeparation != CORE_RESOURCE_QUEUE_SLOT_PITCH
			|| defaults.occupantClearance != CORE_SHUTTLE_OCCUPANT_CLEARANCE)
		{
			return false;
		}

		auto policy = configured.getTraversalGeometryPolicy();
		policy.minimumQueueSeparation = 0.75f;
		policy.advanceStepThreshold = 0.2f;
		policy.overflowTailSeparation = 0.8f;
		policy.occupantClearance = 0.15f;
		configured.setTraversalGeometryPolicy(policy);

		auto const& roundTripped = configured.getTraversalGeometryPolicy();
		auto const& stillDefault = untouched.getTraversalGeometryPolicy();
		return roundTripped.minimumQueueSeparation == 0.75f
			&& roundTripped.advanceStepThreshold == 0.2f
			&& roundTripped.overflowTailSeparation == 0.8f
			&& roundTripped.occupantClearance == 0.15f
			&& stillDefault.minimumQueueSeparation == CORE_RESOURCE_QUEUE_SLOT_PITCH
			&& stillDefault.advanceStepThreshold == CORE_RESOURCE_QUEUE_ADVANCE_THRESHOLD
			&& stillDefault.overflowTailSeparation == CORE_RESOURCE_QUEUE_SLOT_PITCH
			&& stillDefault.occupantClearance == CORE_SHUTTLE_OCCUPANT_CLEARANCE;
	}
}

void registerTopology(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "pausedTopologyRebuildIsAtomicAndCleansOwnership", [](smoke::Context const&)
		{
			smoke::require(pausedTopologyRebuildIsAtomicAndCleansOwnership(), "paused topology rebuild was not safe and atomic");
		} });
	checks.push_back({ "graphBuildWarnsForLocationWithoutVertices", [](smoke::Context const&)
		{
			smoke::require(graphBuildWarnsForLocationWithoutVertices(),
				"an empty Location did not report its missing Graph vertices");
		} });
	checks.push_back({ "traversalGeometryPolicyIsWorldOwned", [](smoke::Context const&)
		{
			smoke::require(traversalGeometryPolicyIsWorldOwned(), "traversal geometry policy defaults, round trip, or World ownership failed");
		} });
}
