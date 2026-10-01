#include "Checks.h"
#include "SimulationTrace.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>
#include "core/LiftTransit.h"
#include "core/Transit.h"

namespace
{
	using smoke::ScenarioResult;
	using smoke::canonicalResult;

	// Ticket #14: a three-Layer World whose back-most Layer carries a Transit
	// landing on the Layer directly in front of it.  The Agent starts on the
	// front-most Layer, crosses a Door authored on the 0<->1 pair into Layer 1,
	// boards the Layer 2 Lift through its landing Doors, rides it, and disembarks
	// into the upper level of the Layer 1 Corridor.
	struct ThreeLayerJourneyResult
	{
		bool pathShapeValid{ false };
		bool sawTransitOccupant{ false };
		bool reachedDestination{ false };
		ScenarioResult run;
	};

	ThreeLayerJourneyResult runThreeLayerTransitJourney()
	{
		ThreeLayerJourneyResult result;

		core::World world("Three-layer transit traversal", 8, 4);
		while (world.getLayerCount() < 3) world.addLayer();
		if (world.getLayerCount() != 3) return result;

		// Layer 0 is the Agent's entry Layer, Layer 1 the Transit's landing Layer,
		// and Layer 2 the Transit Layer itself.
		auto entry = world.addCorridor(0, 0, 0, 6, 1);
		auto lower = world.addCorridor(1, 0, 0, 6, 1);
		auto upper = world.addCorridor(1, 2, 0, 6, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		auto lift = world.addLift(2, 0, 4, liftOptions);

		// A Door is authored on the front Layer of the pair it crosses.
		auto door = world.addSectorDoor(0, 0, 1);
		world.finishBuild();

		if (!world.isTraversalTopologyValid() || lift.doors.size() != 2
			|| door.door.type != core::SectorObjectType::Door) return result;

		// The Transit sits on Layer 2 and every landing it owns is on Layer 1.
		auto transit = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(lift.lift.sector->getIndex()));
		if (!transit || transit->getLayerIndex() != 2 || transit->getNumStops() != 2) return result;
		if (transit->getStop(0).sector->getIndex() != lower
			|| transit->getStop(1).sector->getIndex() != upper)
			return result;
		for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			if (transit->getStop(stop).sector->getLayerIndex() != 1) return result;

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 5.5f, 2.0f });
		if (!target) return result;

		auto agentId = world.createAgent("Deep traveller", entry, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		if (!agent) return result;
		auto path = world.getGraph()->calculatePath(agent, target);
		if (!path) return result;

		// The route must cross into Layer 1 exactly once, board and leave the Layer 2
		// Transit through its landing Doors, and ride it exactly once.
		uint32_t entryDoors{ 0 }, landingDoors{ 0 }, rides{ 0 };
		for (auto const& node : path->nodes)
		{
			if (!node.edge) continue;
			auto const a = node.edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = node.edge->getVertex(1)->getSector()->getLayerIndex();
			auto const type = node.edge->getType();
			if (type == core::EdgeType::Door)
			{
				if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++entryDoors;
				else if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++landingDoors;
			}
			else if (type == core::EdgeType::Lift) ++rides;
		}
		if (entryDoors != 1 || landingDoors != 2 || rides != 1) return result;
		result.pathShapeValid = true;

		agent->setPath(path, true);
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
			if (agent->getSector() == transit.get()) result.sawTransitOccupant = true;
		}

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		auto const finalPosition = agent->getGlobalPosition();
		result.reachedDestination = result.sawTransitOccupant
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(upper).get()
			&& agent->getSector()->getLayerIndex() == 1
			&& finalPosition.distanceTo(target->getPosition()) < 0.001f;
		return result;
	}
}

void registerLayerJourney(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runThreeLayerTransitJourney", [](smoke::Context const&)
		{
			auto const deepJourney = runThreeLayerTransitJourney();
			smoke::require(deepJourney.pathShapeValid, "three-layer route did not cross into Layer 1 and ride the Layer 2 Transit");
			smoke::require(deepJourney.reachedDestination, "Agent did not traverse the three-layer world through its back-layer Transit");
			auto const deepRepeat = runThreeLayerTransitJourney();
			smoke::require(canonicalResult(deepJourney.run) == canonicalResult(deepRepeat.run),
				"three-layer Transit traversal was not deterministic");
		} });
}
