#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"

#include "core/Graph.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
#include "core/YamlSerializer.h"

#include "WorldChecks.h"

namespace persistence
{
	using smoke::require;

	void worldLayerNamesRoundTrip(smoke::Context const&)
	{
		core::World original("Named layers", 4, 2);
		original.setLayerName(0, "Front");
		original.setLayerName(1, "Rear");
		original.addCorridor(0, 0, 4);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("layerNames:") != std::string::npos
			&& yaml.find("- Front") != std::string::npos
			&& yaml.find("- Rear") != std::string::npos,
			"Custom layer names were not serialized");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Named layer World did not deserialize");
		require(loaded.getLayerCount() == 2
			&& loaded.getLayerName(0) == "Front"
			&& loaded.getLayerName(1) == "Rear",
			"Custom layer names did not round-trip");
	}

	void addedLayersAppendToTheBackAndRoundTrip(smoke::Context const&)
	{
		core::World original("Growing", 4, 2);
		original.addCorridor(0, 0, 4);
		require(original.getLayerCount() == 2, "A new World does not start with two layers");

		auto const appended = original.addLayer();
		require(appended == 2 && original.getLayerCount() == 3,
			"addLayer() did not append a third layer");
		require(original.getLayerName(2) == "Layer 2",
			"addLayer() did not name the new layer by its position");
		require(original.getLayerName(0) == "Layer 0" && original.getLayerName(1) == "Layer 1",
			"addLayer() disturbed the names of existing layers");

		original.setLayerName(2, "Sub-basement");
		original.addRoom("Store", 2, 0, 0, 3, 1);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("layers: 3") != std::string::npos
			&& yaml.find("- Sub-basement") != std::string::npos,
			"The added layer was not serialized");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "The three-layer World did not deserialize");
		require(loaded.getLayerCount() == 3
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1"
			&& loaded.getLayerName(2) == "Sub-basement",
			"The added layer did not round-trip");
		core::World const& loadedRef = loaded;
		require(loadedRef.getLayer(2) && loadedRef.getLayer(2)->getZ() == 2,
			"The added layer was not created at the expected depth");
	}

	void layerCountIsCappedAtCoreMaxLayers(smoke::Context const&)
	{
		core::World world("Capped", 1, 1);
		while (world.getLayerCount() < CORE_MAX_LAYERS) world.addLayer();
		require(world.getLayerCount() == CORE_MAX_LAYERS,
			"addLayer() stopped short of CORE_MAX_LAYERS");

		bool rejected = false;
		try
		{
			world.addLayer();
		}
		catch (std::exception const&)
		{
			rejected = true;
		}
		require(rejected, "addLayer() did not reject going beyond CORE_MAX_LAYERS");
		require(world.getLayerCount() == CORE_MAX_LAYERS,
			"A rejected addLayer() still changed the layer count");
	}

	void oversizedWorldDimensionsAreRefusedBeforeCellAccess(smoke::Context const&)
	{
		// #184: cellsWide * levelsHigh used to overflow uint32_t to zero, so a
		// Layer allocated no cells while bounds validation still accepted (0, 0).
		bool constructorRefused = false;
		std::string constructorDiagnostic;
		try
		{
			core::World oversized("Overflow", 65536, 65536);
		}
		catch (core::WorldException const& exception)
		{
			constructorRefused = true;
			constructorDiagnostic = exception.what();
		}
		require(constructorRefused,
			"An overflowing World size was accepted by the constructor");
		require(constructorDiagnostic.find("limit") != std::string::npos,
			"An overflowing World size did not report the cell limit");

		std::string diagnostic;
		require(core::World::dimensionsAreSupported(4, 2, 2, &diagnostic),
			"A small World size was refused");
		require(diagnostic.empty(), "An accepted World size left a diagnostic");
		require(!core::World::dimensionsAreSupported(0, 4, 2, &diagnostic),
			"A zero-width World size was accepted");
		require(!core::World::dimensionsAreSupported(4, 0, 2, &diagnostic),
			"A zero-level World size was accepted");
		// The budget is total cells across Layers, so a per-Layer product that
		// only crosses the limit once both Layers are counted is still refused.
		require(!core::World::dimensionsAreSupported(
				CORE_MAX_WORLD_CELLS / 2 + 1, 1, 2, &diagnostic),
			"A World just over the total cell limit was accepted");
		require(!core::World::dimensionsAreSupported(1, 1,
				CORE_MAX_LAYERS + 1, &diagnostic),
			"A World with too many Layers was accepted");

		// A normal World still allocates every cell and indexes within bounds.
		core::World const normal("Normal", 4, 3);
		require(!normal.getLayer(0)->getCellDefinition(3, 2).occupied(),
			"A normal World did not allocate its full cell grid");

		// The load path applies the same rule and refuses the document before
		// mutating the World being loaded into.
		auto const yaml = R"yaml(version: 1
name: Overflow
cellsWide: 65536
levelsHigh: 65536
construction: []
agents: []
)yaml";
		core::World loaded("placeholder", 2, 2);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		bool loadRefused = false;
		try
		{
			(void)loaded.deserialize(*reader, workData);
		}
		catch (core::SerializationException const&)
		{
			loadRefused = true;
		}
		require(loadRefused, "An overflowing World document was accepted");
		require(loaded.getName() == "placeholder" && loaded.getCellsWide() == 2
			&& loaded.getLevelsHigh() == 2,
			"A refused World document still mutated the loading World");
	}

	void levelsHaveNamesLimitsAndCascadingDeletion(smoke::Context const&)
	{
		require(core::World::dimensionsAreSupported(2, 128, 2), "128 Levels refused");
		require(!core::World::dimensionsAreSupported(2, 129, 2), "129 Levels accepted");
		core::World world("Levels", 8, 4);
		require(world.getLevelName(0) == "Level 0", "Default Level name incorrect");
		world.setLevelName(3, "Roof");
		world.addRoom("Spanning", 0, 0, 0, 8, 2);
		world.addRoom("Survivor", 0, 3, 0, 8, 1);
		world.finishBuild();
		auto lost = world.createAgent("Lost", 0, 0, 1.0f);
		auto kept = world.createAgent("Kept", 1, 0, 1.0f);
		world.addLevel();
		require(world.getLevelsHigh() == 5 && world.getLevelName(4) == "Level 4",
			"Adding a Level failed");
		auto plan = world.planDeleteLevel(1);
		require(plan.valid, plan.diagnostic.c_str());
		require(std::any_of(plan.consequences.begin(), plan.consequences.end(),
			[](auto const& text) { return text == "Delete Agent Lost"; }), "Missing Agent cascade");
		world.applyDeleteLevel(plan);
		require(world.getLevelsHigh() == 4 && world.getLevelName(2) == "Roof",
			"Level names did not compact");
		require(!world.lookupAgent(lost) && world.lookupAgent(kept), "Wrong Agents survived deletion");
		require(world.lookupAgent(kept).entity->getGlobalPosition().y == 2.0f, "Agent did not move down");
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData data;
		world.serialize(*writer, data);
		writer->serialize();
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		core::World loaded("Loaded", 2, 1);
		loaded.deserialize(*reader, data);
		require(loaded.getLevelName(2) == "Roof", "Level names did not round-trip");
		auto legacy = core::YamlSerializer::fromString(
			"version: 1\nname: Legacy\ncellsWide: 2\nlevelsHigh: 2\nconstruction: []\nagents: []\n");
		legacy->deserialize();
		loaded.deserialize(*legacy, data);
		require(loaded.getLevelName(1) == "Level 1", "Legacy Level names not defaulted");

		core::World transitWorld("Cascade", 8, 4);
		transitWorld.addRoom("Landing", 0, 0, 0, 8, 3);
		transitWorld.addRoom("Back", 1, 0, 0, 8, 3);
		transitWorld.addSectorDoor(0, 0, 2);
		transitWorld.addSectorWindow(0, 1, 4, 1, 1);
		transitWorld.finishBuild();
		auto cascade = transitWorld.planDeleteLevel(1);
		require(cascade.valid, cascade.diagnostic.c_str());
		require(cascade.consequences.size() >= 5, "Missing threshold cascades");
		transitWorld.applyDeleteLevel(cascade);
		require(transitWorld.getNumSectors() == 0, "Spanning Sectors survived Level deletion");

		core::World maximum("Maximum", 2, 128);
		bool refused = false;
		try { maximum.addLevel(); } catch (core::WorldException const&) { refused = true; }
		require(refused && maximum.getLevelsHigh() == 128, "Adding Level 129 was not refused");
		core::World minimum("Minimum", 2, 1);
		require(!minimum.planDeleteLevel(0).valid, "Last Level deletion accepted");
	}

	void deletingAMiddleLayerCompactsTheLayersAboveIt(smoke::Context const&)
	{
		core::World world("Compacting", 8, 3);
		world.addLayer();
		world.addCorridor(0, 0, 8);
		world.addRoom("Basement", 1, 0, 0, 8, 1);
		world.addRoom("Cellar", 2, 0, 0, 8, 1);
		world.setLayerName(2, "Deep Cellar");
		world.addSectorDoor(0, 0, 2);
		// Authored on the front Layer of the 1<->2 pair, so it crosses the Layer the
		// test deletes.
		world.addSectorWindow(1, 0, 5, 1, 1);
		world.finishBuild();
		auto const survivor = world.createAgent("Walker", 0, 0, 1.0f);
		auto const buried = world.createAgent("Buried", 1, 0, 1.0f);

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(1);
		require(plan.valid, ("Middle layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.layerCountBefore == 3 && plan.layerCountAfter == 2,
			"Layer deletion did not report its compaction");
		require(plan.locationsRemoved == 1 && plan.doorsRemoved == 1
			&& plan.windowsRemoved == 1 && plan.agentsRemoved == 1,
			"Layer deletion did not report its destructive consequences");
		require(plan.requiresConfirmation(),
			"A destructive layer deletion did not require confirmation");
		require(!plan.consequences.empty(), "Layer deletion produced no consequence list");
		require(world.applyDeleteLayer(plan), "Layer deletion was not applied");

		require(world.getLayerCount() == 2, "Layers were not compacted");
		require(world.getLayerName(1) == "Deep Cellar",
			"Layer names did not travel with the compacted layer");
		require(world.getNumSectors() == 2, "Sectors were not removed with their layer");
		require(world.getSector(0)->getLayerIndex() == 0
			&& world.getSector(1)->getName() == "Cellar"
			&& world.getSector(1)->getLayerIndex() == 1,
			"Higher layers were not compacted down by one");
		require(world.lookupAgent(survivor).entity != nullptr,
			"Agent outside the deleted layer was removed");
		require(world.lookupAgent(buried).entity == nullptr,
			"Agent in the deleted layer was retained");
		require(world.isSimulationPaused(), "Layer deletion resumed the simulation");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		core::World reloaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData),
			"A compacted World did not round-trip");
		require(reloaded.getLayerCount() == 2
			&& reloaded.getLayerName(1) == "Deep Cellar"
			&& reloaded.getNumSectors() == 2
			&& reloaded.getSector(1)->getName() == "Cellar",
			"Layer compaction did not survive serialization");
	}

	void deletingTheFrontLayerRemovesTransitsOneLayerBehind(smoke::Context const&)
	{
		core::World world("Front deletion", 8, 3);
		world.addLayer();
		world.addCorridor(0, 0, 8);
		world.addCorridor(2, 0, 8);
		world.addRoom("Deep", 2, 0, 0, 8, 3);
		world.addLadder(1, 0, 3, { 3, false, true });
		world.finishBuild();
		auto const climber = world.createAgent("Climber", 3, 0, 0.5f);

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(0);
		require(plan.valid, ("Front layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.locationsRemoved == 2, "Front layer Sectors were not counted");
		require(plan.transitsRemoved == 1,
			"Transits one layer behind the deletion were not counted");
		require(plan.agentsRemoved == 1, "Agents in the deleted Transit were not counted");
		require(world.applyDeleteLayer(plan), "Front layer deletion was not applied");
		require(world.getLayerCount() == 2, "Layers were not compacted");
		require(world.getNumSectors() == 1
			&& world.getSector(0)->getName() == "Deep"
			&& world.getSector(0)->getLayerIndex() == 1,
			"The surviving Room did not compact to the layer behind the deletion");
		require(world.lookupAgent(climber).entity == nullptr,
			"Agent in a removed Transit was retained");
	}

	void layerDeletionPreservesAuthoredRecordDependencies(smoke::Context const&)
	{
		core::World world("Dependencies", 16, 3);
		world.addLayer();
		world.addCorridor(0, 0, 7);
		world.addCorridor(1, 3, 7);
		world.addStaircase(1, 0, 4, { 4, CORE_SIDE_RIGHT, 0.4f });
		world.addRoom("Room 1", 0, 1, 10, 4, 2);
		world.addCorridor(2, 14, 2);
		world.removeLocationWall(3, 1, CORE_SIDE_RIGHT);
		world.addStaircase(1, 1, 11, { 3, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		world.pauseSimulation();

		// Deleting the Transit Layer drops two Sectors ahead of the Room, so the
		// wall removal must follow the Room rather than land on a renumbered Sector.
		auto const middle = world.planDeleteLayer(1);
		require(middle.valid,
			("Dependency-aware layer deletion was rejected: " + middle.diagnostic).c_str());
		require(middle.transitsRemoved == 2, "Dependency test dropped the wrong Transits");
		world.applyDeleteLayer(middle);
		require(world.getNumSectors() == 4, "Dependency test compacted to the wrong Sector count");
		require(world.getSector(2)->getName() == "Room 1",
			"The wall removal did not follow its Room through the Sector compaction");
		require(world.getSector(2)->getEndType(1, CORE_SIDE_RIGHT) != core::SectorEndType::Wall,
			"The removed wall came back after the layer deletion");
		require(world.isTraversalTopologyValid(),
			("Layer deletion left an invalid topology: " + world.getTopologyDiagnostic()).c_str());

		// Deleting the back-most Layer drops nothing, so this exercises the record
		// ordering alone.
		core::World untouched("Dependencies", 16, 3);
		untouched.addLayer();
		untouched.addCorridor(0, 0, 7);
		untouched.addCorridor(1, 3, 7);
		untouched.addStaircase(1, 0, 4, { 4, CORE_SIDE_RIGHT, 0.4f });
		untouched.addRoom("Room 1", 0, 1, 10, 4, 2);
		untouched.addCorridor(2, 14, 2);
		untouched.removeLocationWall(3, 1, CORE_SIDE_RIGHT);
		untouched.addStaircase(1, 1, 11, { 3, CORE_SIDE_RIGHT, 0.0f });
		untouched.finishBuild();
		untouched.pauseSimulation();
		auto const back = untouched.planDeleteLayer(2);
		require(back.valid,
			("Deleting the back-most Layer broke an authored dependency: " + back.diagnostic).c_str());
		untouched.applyDeleteLayer(back);
		require(untouched.getNumSectors() == 6 && untouched.getLayerCount() == 2,
			"Deleting the back-most Layer changed more than the Layer count");
		require(untouched.isTraversalTopologyValid(),
			("Back-most layer deletion left an invalid topology: "
				+ untouched.getTopologyDiagnostic()).c_str());
	}

	void layerDeletionKeepsAtLeastTwoLayers(smoke::Context const&)
	{
		core::World world("Two layers", 4, 2);
		world.addCorridor(0, 0, 4);
		world.finishBuild();

		auto const last = world.planDeleteLayer(1);
		require(!last.valid && !last.diagnostic.empty(),
			"Deleting down to a single layer was not rejected");

		bool threw = false;
		try
		{
			world.applyDeleteLayer(last);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "applyDeleteLayer did not reject an invalid plan");
		require(world.getLayerCount() == 2, "A rejected layer deletion changed the layer count");

		auto const missing = world.planDeleteLayer(7);
		require(!missing.valid && !missing.diagnostic.empty(),
			"Deleting a nonexistent layer was not rejected");

		core::World emptyBack("Empty back", 4, 2);
		emptyBack.addCorridor(0, 0, 4);
		emptyBack.addLayer();
		emptyBack.finishBuild();
		emptyBack.pauseSimulation();
		auto const back = emptyBack.planDeleteLayer(2);
		require(back.valid, ("Deleting the empty back-most layer was rejected: " + back.diagnostic).c_str());
		require(back.requiresConfirmation() && !back.consequences.empty(),
			"An empty layer deletion offered nothing to confirm");
		require(emptyBack.applyDeleteLayer(back) && emptyBack.getLayerCount() == 2,
			"The empty back-most layer was not removed");
		require(emptyBack.getNumSectors() == 1,
			"Deleting an empty layer changed the Sector count");
	}
}
