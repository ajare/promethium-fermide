#include "Checks.h"
#include "SimulationTrace.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>
#include "core/Transit.h"

namespace
{
	using smoke::ScenarioResult;
	using smoke::canonicalResult;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	// Ticket #15: pulling a middle Layer out of a four-Layer World while Agents
	// are still using it.  The plan the editor would confirm has to name every
	// casualty: the Locations on the deleted Layer, the Transit on that Layer, the
	// Lift one Layer behind which loses its landings, the Door crossing it, and
	// the Agents standing in each doomed Sector.  Applying the plan must then
	// leave a compacted World whose surviving Layers are still valid and
	// walkable, with the deeper Ladder and its landing pair moved forward intact.
	struct MiddleLayerDeletionResult
	{
		bool planValid{ false };
		bool planCountsValid{ false };
		bool casualtiesRemoved{ false };
		bool layersCompacted{ false };
		bool survivorsTraversable{ false };
		std::string diagnostic;
		ScenarioResult run;
	};

	// Authored Sector handles, recorded before a deletion renumbers them.
	struct MiddleLayerLayout
	{
		uint32_t entry{ 0 };
		uint32_t lobby{ 0 };
		uint32_t lowerLobby{ 0 };
		uint32_t middleLevel{ 0 };
		uint32_t middleStore{ 0 };
		uint32_t doomedLadder{ 0 };
		uint32_t deepStore{ 0 };
		uint32_t deepCorridor{ 0 };
		uint32_t deepYard{ 0 };
		uint32_t doomedLift{ 0 };
		uint32_t annexe{ 0 };
		uint32_t keptLadder{ 0 };
	};

	// Layer 0 is the entry Layer, Layer 1 the Layer under test, and Layers 2 and 3
	// the deeper Layers which must survive.  Every Location is one level high so a
	// Ladder can land on two stacked Locations, and the two Doors are authored at
	// different cells so the deletion counts the Door it really crosses rather
	// than one which merely shares a cell.
	MiddleLayerLayout authorMiddleLayerDeletionWorld(core::World& world)
	{
		while (world.getLayerCount() < 4) world.addLayer();
		world.setLayerName(1, "Middle");
		world.setLayerName(2, "Deep");
		world.setLayerName(3, "Attic");

		MiddleLayerLayout layout;
		layout.entry = world.addCorridor(0, 0, 0, 12, 1);
		layout.lobby = world.addRoom("Lobby", 0, 1, 0, 12, 1);
		layout.lowerLobby = world.addRoom("Lower Lobby", 0, 2, 0, 12, 1);
		layout.middleLevel = world.addRoom("Middle Level", 1, 0, 0, 12, 1);
		layout.middleStore = world.addRoom("Middle Store", 1, 2, 0, 11, 1);
		layout.doomedLadder = world.addLadder(1, 1, 11, { 2, false, true })
			.ladder.sector->getIndex();
		layout.deepStore = world.addRoom("Deep Store", 2, 0, 0, 10, 1);
		layout.deepCorridor = world.addRoom("Deep Corridor", 2, 1, 0, 10, 1);
		layout.deepYard = world.addRoom("Deep Yard", 2, 2, 0, 10, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		layout.doomedLift = world.addLift(2, 0, 10, liftOptions).lift.sector->getIndex();

		layout.annexe = world.addRoom("Annexe", 3, 0, 0, 10, 1);
		layout.keptLadder = world.addLadder(3, 1, 9, { 2, false, true })
			.ladder.sector->getIndex();

		// A Door is authored on the front Layer of the pair it crosses.
		world.addSectorDoor(0, 0, 4);
		world.addSectorDoor(2, 0, 6);
		world.addSectorMarker(layout.deepStore, 0, 1.5f, nullptr);
		world.addSectorMarker(layout.annexe, 0, 8.5f, nullptr);
		world.finishBuild();
		return layout;
	}

	std::shared_ptr<const core::Sector> sectorByName(core::World const& world, std::string const& name)
	{
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
			if (world.getSector(i)->getName() == name) return world.getSector(i);
		return nullptr;
	}

	MiddleLayerDeletionResult runMiddleLayerDeletion()
	{
		MiddleLayerDeletionResult result;
		core::World world("Layer deletion smoke world", 12, 4);
		auto const layout = authorMiddleLayerDeletionWorld(world);
		if (!world.isTraversalTopologyValid())
		{
			result.diagnostic = "authored World is invalid: " + world.getTopologyDiagnostic();
			return result;
		}

		// The doomed Lift lands on the Layer under test, and the doomed Ladder sits
		// on it, so both are casualties of the deletion even though only one of
		// them is actually on it.
		auto const doomedLift = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLift));
		auto const doomedLadder = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLadder));
		if (!doomedLift || doomedLift->getLayerIndex() != 2 || doomedLift->getNumStops() != 2)
		{
			result.diagnostic = "the authored Lift does not sit on Layer 2 with two stops";
			return result;
		}
		if (!doomedLadder || doomedLadder->getLayerIndex() != 1 || doomedLadder->getNumStops() != 2)
		{
			result.diagnostic = "the authored Ladder does not sit on Layer 1 with two stops";
			return result;
		}
		for (uint32_t stop = 0; stop < doomedLift->getNumStops(); ++stop)
			if (doomedLift->getStop(stop).sector->getLayerIndex() != 1)
			{
				result.diagnostic = "the doomed Lift does not land on Layer 1";
				return result;
			}

		// One Agent stands in every kind of Sector the deletion touches, and two of
		// them are mid-journey when the Layer is pulled out from under them.
		auto const entryAgentId = world.createAgent("Entry walker", layout.entry, 0, 0.5f);
		auto const sitterAgentId = world.createAgent("Middle sitter", layout.middleLevel, 0, 0.5f);
		auto const climberAgentId = world.createAgent("Doomed climber", layout.doomedLadder, 0, 0.5f);
		auto const riderAgentId = world.createAgent("Doomed rider", layout.doomedLift, 0, 0.5f);
		auto const deepAgentId = world.createAgent("Deep traveller", layout.deepStore, 0, 0.5f);
		auto const yardAgentId = world.createAgent("Yard keeper", layout.deepYard, 0, 0.5f);

		auto const annexeBefore = world.getSector(layout.annexe).get();
		auto const annexeTarget = world.getGraph()->getClosestVertexInSector(annexeBefore, { 8.5f, 0.0f });
		if (!annexeTarget
			|| annexeTarget->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f)
		{
			result.diagnostic = "the Annexe destination Marker is not where the scenario authors it";
			return result;
		}
		auto const traveller = world.lookupAgent(deepAgentId).entity;
		if (!traveller)
		{
			result.diagnostic = "the traveller Agent was not created";
			return result;
		}
		auto const outbound = world.getGraph()->calculatePath(traveller, annexeTarget);
		if (!outbound)
		{
			result.diagnostic = "the authored World has no route from the Deep Store to the Annexe";
			return result;
		}
		traveller->setPath(outbound, true);
		for (uint64_t tick = 0; tick < 60; ++tick) world.advanceTick();
		if (traveller->getState() == core::Agent::State::Idle)
		{
			result.diagnostic = "the traveller finished before the deletion could catch it mid-journey";
			return result;
		}

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(1);
		result.planValid = plan.valid;
		if (!plan.valid)
		{
			result.diagnostic = plan.diagnostic;
			return result;
		}

		result.planCountsValid = plan.layerCountBefore == 4 && plan.layerCountAfter == 3
			&& plan.locationsRemoved == 2 && plan.transitsRemoved == 2
			&& plan.doorsRemoved == 1 && plan.agentsRemoved == 3
			&& plan.requiresConfirmation();
		if (!result.planCountsValid)
		{
			std::ostringstream detail;
			detail << "layers " << plan.layerCountBefore << "->" << plan.layerCountAfter
				<< ", locations=" << plan.locationsRemoved
				<< ", transits=" << plan.transitsRemoved
				<< ", doors=" << plan.doorsRemoved
				<< ", agents=" << plan.agentsRemoved;
			result.diagnostic = detail.str();
			return result;
		}

		world.applyDeleteLayer(plan);

		// Every casualty is gone: the two Locations and the Ladder on the deleted
		// Layer, the Lift which lost its landings, and the three Agents which were
		// standing in them.  The deeper Ladder survives because its landing pair
		// was never touched.
		uint32_t lifts{ 0 };
		std::shared_ptr<const core::Sector> survivingLadder;
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
		{
			auto const sector = world.getSector(i);
			if (!sector) continue;
			if (sector->getName() == "Middle Level" || sector->getName() == "Middle Store")
			{
				result.diagnostic = "a Location survived on the deleted Layer";
				return result;
			}
			if (sector->getType() == core::SectorType::Lift) ++lifts;
			if (sector->getType() == core::SectorType::Ladder) survivingLadder = sector;
		}
		result.casualtiesRemoved = world.getNumSectors() == 8 && lifts == 0 && survivingLadder != nullptr
			&& world.lookupAgent(sitterAgentId).entity == nullptr
			&& world.lookupAgent(climberAgentId).entity == nullptr
			&& world.lookupAgent(riderAgentId).entity == nullptr;
		result.casualtiesRemoved = result.casualtiesRemoved
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
		if (!result.casualtiesRemoved)
		{
			std::ostringstream detail;
			detail << "sectors=" << world.getNumSectors() << ", lifts=" << lifts
				<< ", topology=" << (world.isTraversalTopologyValid() ? "valid" : world.getTopologyDiagnostic());
			result.diagnostic = detail.str();
			return result;
		}

		// The Layers behind the deletion moved forward one, names and all, and the
		// Sectors on them moved with their Layers.
		result.layersCompacted = world.getLayerCount() == 3
			&& world.getLayerName(1) == "Deep" && world.getLayerName(2) == "Attic";
		for (auto const* name : { "Deep Store", "Deep Corridor", "Deep Yard" })
		{
			auto const sector = sectorByName(world, name);
			if (!sector || sector->getLayerIndex() != 1) result.layersCompacted = false;
		}
		auto const annexe = sectorByName(world, "Annexe");
		auto const survivingTransit = std::dynamic_pointer_cast<const core::Transit>(survivingLadder);
		if (!annexe || annexe->getLayerIndex() != 2) result.layersCompacted = false;
		if (!survivingTransit || survivingTransit->getLayerIndex() != 2
			|| survivingTransit->getNumStops() != 2)
			result.layersCompacted = false;
		else
			for (uint32_t stop = 0; stop < survivingTransit->getNumStops(); ++stop)
				if (survivingTransit->getStop(stop).sector->getLayerIndex() != 1)
					result.layersCompacted = false;

		// The Door which crossed the deleted Layer is gone; the one behind it now
		// crosses the compacted pair.
		uint32_t deletedCrossing{ 0 }, compactedCrossing{ 0 };
		for (auto const& edge : world.getGraph()->getEdges())
		{
			if (!edge || edge->getType() != core::EdgeType::Door) continue;
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++deletedCrossing;
			if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++compactedCrossing;
		}
		result.layersCompacted = result.layersCompacted && deletedCrossing == 0 && compactedCrossing == 1;
		if (!result.layersCompacted)
		{
			std::ostringstream detail;
			detail << "layers=" << world.getLayerCount() << ", door crossings into the deleted pair="
				<< deletedCrossing << ", door crossings over the compacted pair=" << compactedCrossing;
			result.diagnostic = detail.str();
			return result;
		}

		// The Agents which were not in a doomed Sector are still there, resting on
		// the Layer their Sector compacted to.
		if (world.lookupAgent(entryAgentId).entity == nullptr
			|| world.lookupAgent(deepAgentId).entity == nullptr
			|| world.lookupAgent(yardAgentId).entity == nullptr)
		{
			result.diagnostic = "a surviving Agent was removed by the deletion";
			return result;
		}
		auto const entry = world.lookupAgent(entryAgentId).entity;
		auto const deep = world.lookupAgent(deepAgentId).entity;
		auto const yard = world.lookupAgent(yardAgentId).entity;
		if (entry->getSector()->getLayerIndex() != 0
			|| deep->getSector()->getName() != "Deep Store"
			|| deep->getSector()->getLayerIndex() != 1
			|| yard->getSector()->getName() != "Deep Yard"
			|| yard->getSector()->getLayerIndex() != 1)
		{
			result.diagnostic = "a surviving Agent was not re-placed on its compacted Layer";
			return result;
		}

		// And the compacted World still works: one Agent crosses the surviving
		// Door into the compacted back Layer, and the other rides the compacted
		// Ladder between the two Locations which were never in danger.
		world.resumeSimulation();
		auto const annexeVertex = world.getGraph()->getClosestVertexInSector(annexe.get(), { 8.5f, 0.0f });
		auto const corridorVertex = world.getGraph()->getClosestVertexInSector(
			sectorByName(world, "Deep Corridor").get(), { 1.5f, 1.5f });
		// The destination Marker has to have followed its Sector through the record
		// rewrite rather than landing on a renumbered neighbour.
		if (!annexeVertex || annexeVertex->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f
			|| !corridorVertex)
		{
			result.diagnostic = "a Marker did not follow its Sector through the compaction";
			return result;
		}
		auto const deepPath = world.getGraph()->calculatePath(deep, annexeVertex);
		auto const yardPath = world.getGraph()->calculatePath(yard, corridorVertex);
		if (!deepPath || !yardPath)
		{
			result.diagnostic = "the compacted World has no route for a surviving Agent";
			return result;
		}

		bool rodeLadder{ false };
		for (auto const& node : yardPath->nodes)
			if (node.edge && node.edge->getType() == core::EdgeType::Ladder) rodeLadder = true;
		if (!rodeLadder)
		{
			result.diagnostic = "the compacted Ladder is no longer part of the Yard keeper's route";
			return result;
		}

		deep->setPath(deepPath, true);
		yard->setPath(yardPath, true);
		while ((deep->getState() != core::Agent::State::Idle || yard->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
		}

		result.survivorsTraversable = deep->getState() == core::Agent::State::Idle
			&& deep->getSector() == annexe.get()
			&& deep->getGlobalPosition().distanceTo(annexeVertex->getPosition()) < 0.001f
			&& yard->getState() == core::Agent::State::Idle
			&& yard->getSector() == sectorByName(world, "Deep Corridor").get()
			&& yard->getGlobalPosition().distanceTo(corridorVertex->getPosition()) < 0.001f;

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		return result;
	}
}

void registerLayerDeletion(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runMiddleLayerDeletion", [](smoke::Context const&)
		{
			auto const deletion = runMiddleLayerDeletion();
			smoke::require(deletion.planValid, "middle Layer deletion was rejected: " + deletion.diagnostic);
			smoke::require(deletion.planCountsValid, "middle Layer deletion did not report its casualties: "
					+ deletion.diagnostic);
			smoke::require(deletion.casualtiesRemoved, "middle Layer deletion left Sectors, Transits, or Agents behind: "
					+ deletion.diagnostic);
			smoke::require(deletion.layersCompacted, "Layers behind the deleted Layer did not compact correctly: "
					+ deletion.diagnostic);
			smoke::require(deletion.survivorsTraversable, "surviving Agents could not travel the compacted World");
			auto const deletionRepeat = runMiddleLayerDeletion();
			smoke::require(canonicalResult(deletion.run) == canonicalResult(deletionRepeat.run),
				"Layer deletion and compaction was not deterministic");
		} });
}
