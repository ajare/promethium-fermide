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

	void layerHelperApiIsConsistentWithLayerCount(smoke::Context const&)
	{
		require(core::isFrontMostLayer(0), "fore layer is not reported as front-most");
		require(!core::isFrontMostLayer(1), "back layer reported as front-most");
		require(core::isBackMostLayer(1, 2), "back layer is not reported as back-most");
		require(!core::isBackMostLayer(0, 2), "fore layer reported as back-most");
		require(core::isBackMostLayer(2, 3) && !core::isBackMostLayer(1, 3),
			"isBackMostLayer does not follow the World's Layer count");
		require(core::layerInFront(1) == 0, "layerInFront(back) did not return fore");
		require(core::layerBehind(0) == 1, "layerBehind(fore) did not return back");
		require(CORE_MAX_LAYERS == 256, "CORE_MAX_LAYERS is not 256");
	}

	void graphConstructionWalksEveryAdjacentLayerPair(smoke::Context const&)
	{
		char const* roomNames[] = { "Corridor", "Basement", "Deep Cellar", "Catacomb", "Oubliette" };
		// One name per Layer the loop can build. Deriving the bound from the array
		// keeps the scan from reading past the end of roomNames, which clang's
		// Release codegen surfaced as a crash where GCC's layout happened to survive.
		uint32_t const maxLayerCount = static_cast<uint32_t>(sizeof(roomNames) / sizeof(roomNames[0]));

		for (uint32_t layerCount = 2; layerCount <= maxLayerCount; ++layerCount)
		{
			core::World world("Adjacent Pairs", 8, 2);
			while (world.getLayerCount() < layerCount) world.addLayer();

			std::vector<uint32_t> rooms;
			for (uint32_t layer = 0; layer < layerCount; ++layer)
			{
				rooms.push_back(world.addRoom(roomNames[layer], layer, 0, 0, 8, 1));
			}

			// One Marker per Layer gives every Layer a Vertex of its own, so the scan
			// coverage of each Layer can be observed directly.
			for (uint32_t layer = 0; layer < layerCount; ++layer)
			{
				world.addSectorMarker(rooms[layer], 0, 3.0f);
			}

			world.addSectorDoor(0, 0, 5);
			world.finishBuild();

			core::Graph graph(&world);
			graph.build();

			std::vector<uint32_t> verticesPerLayer(layerCount, 0);
			for (auto const& vertex : graph.getVertices())
			{
				auto const layer = vertex->getSector()->getLayerIndex();
				require(layer < layerCount, "A Vertex belongs to a Layer the World does not have");
				++verticesPerLayer[layer];
			}

			for (uint32_t layer = 0; layer < layerCount; ++layer)
			{
				require(verticesPerLayer[layer] > 0,
					"A Layer deeper than the front pair contributed no Vertices to the Graph");
			}

			// The Door authored on the front pair joins that pair alone.
			uint32_t doorEdges{ 0 };
			for (auto const& edge : graph.getEdges())
			{
				if (edge->getType() != core::EdgeType::Door) continue;

				++doorEdges;

				auto const front = edge->getVertex(0)->getSector()->getLayerIndex();
				auto const back = edge->getVertex(1)->getSector()->getLayerIndex();

				require((front == 0 && back == 1) || (front == 1 && back == 0),
					"A Door Edge joined Layers outside the front adjacent pair");
			}

			require(doorEdges == 1, "The authored Door did not produce exactly one Edge");
		}
	}

	void thresholdsAndTransitsPairTheirOwnAdjacentLayerPair(smoke::Context const&)
	{
		core::World world("Deep Pairing", 40, 2);
		while (world.getLayerCount() < 4) world.addLayer();

		// Layer 0 - front-most.  Two stacked Corridors give the Layer 1 Ladder two
		// distinct landing Locations.
		world.addCorridor(0, 0, 0, 4, 1);
		world.addCorridor(0, 1, 0, 4, 1);

		// Layer 1 - back of pair 0<->1, landing Layer for the Layer 2 Transits, and
		// front of pair 1<->2.
		world.addRoom("Store", 1, 0, 0, 2, 1);
		world.addCorridor(1, 0, 8, 8, 1);
		world.addCorridor(1, 1, 8, 8, 1);
		world.addCorridor(1, 1, 30, 8, 1);   // Shuttle landing run

		// Layer 2 - back of pair 1<->2, landing Layer for the Layer 3 Transits, and
		// front of pair 2<->3.
		world.addRoom("Deep Store", 2, 0, 8, 2, 1);
		world.addRoom("Annexe", 2, 0, 11, 1, 1);
		world.addCorridor(2, 0, 16, 8, 1);
		world.addCorridor(2, 1, 16, 8, 1);
		world.addCorridor(2, 0, 24, 2, 1);
		world.addCorridor(2, 1, 24, 2, 1);
		world.addRoom("Stair Hall Lower", 2, 0, 28, 2, 1);
		world.addRoom("Bulkhead Left", 2, 0, 30, 2, 1);
		world.addRoom("Bulkhead Right", 2, 0, 32, 2, 1);
		world.addRoom("Stair Hall Upper", 2, 1, 28, 2, 1);

		// Layer 3 - back-most.
		world.addRoom("Deep Room", 3, 0, 16, 2, 1);
		world.addRoom("Deep Annexe", 3, 0, 19, 1, 1);

		// One of every threshold and Transit, each on a different adjacent Layer pair.
		// Pair 0<->1.
		world.addSectorDoor(0, 0, 1);
		world.addLadder(1, 0, 2, { 2, false, false });
		// Pair 1<->2.
		world.addSectorDoor(1, 0, 9);
		world.addSectorWindow(1, 0, 11, 1, 1, { true });
		world.addLadder(2, 0, 10, { 2, false, false });
		world.addLift(2, 0, 12, 1, 2);
		world.addShuttle(2, 1, 30, 8, { 1, 3, { 0, 5 }, 0 });
		// Pair 2<->3.
		world.addSectorDoor(2, 0, 17);
		world.addSectorWindow(2, 0, 19, 1, 1, { true });
		world.addLadder(3, 0, 18, { 2, false, false });
		world.addStairwell(3, 0, 28, 2, CORE_SIDE_LEFT);
		world.addStaircase(3, 0, 24, 2, CORE_SIDE_RIGHT);
		// A Bulkhead Door joins two Locations on its own Layer, so it pairs nothing.
		world.addSectorBulkheadDoor(2, 0, 32, CORE_SIDE_LEFT);

		world.finishBuild();

		core::Graph graph(&world);
		graph.build();

		// Nothing may reach across a Layer it did not pair with.
		for (auto const& edge : graph.getEdges())
		{
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			require(a == b || a + 1 == b || b + 1 == a,
				"An Edge joined Layers that are not adjacent");
		}

		// Every Layer contributes Vertices.
		std::vector<uint32_t> verticesPerLayer(world.getLayerCount(), 0);
		for (auto const& vertex : graph.getVertices())
			++verticesPerLayer[vertex->getSector()->getLayerIndex()];
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
			require(verticesPerLayer[layer] > 0, "A Layer contributed no Vertices to the Graph");

		auto countEdgesAcross = [&](core::EdgeType type, uint32_t front, uint32_t back)
		{
			uint32_t count{ 0 };
			for (auto const& edge : graph.getEdges())
			{
				if (edge->getType() != type) continue;
				auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
				auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
				if ((a == front && b == back) || (a == back && b == front)) ++count;
			}
			return count;
		};

		// Each authored threshold produced exactly one Edge across its own pair, and no
		// threshold paired a pair it was never authored on.
		// Pair 0<->1 holds only the one explicitly authored Door.  Pair 1<->2 holds the
		// explicit Door, the two landing Doors the Lift creates for its stops, and the two
		// Doors the Shuttle creates for its stops.  Pair 2<->3 again holds only the
		// explicit Door.  No threshold reaches any other pair.
		require(countEdgesAcross(core::EdgeType::Door, 0, 1) == 1,
			"Pair 0<->1 should hold exactly the one authored Door");
		require(countEdgesAcross(core::EdgeType::Door, 1, 2) == 5,
			"Pair 1<->2 should hold the authored Door plus the Lift and Shuttle landing Doors");
		require(countEdgesAcross(core::EdgeType::Door, 2, 3) == 1,
			"Pair 2<->3 should hold exactly the one authored Door");
		require(countEdgesAcross(core::EdgeType::Window, 1, 2) == 1,
			"The Window authored on pair 1<->2 did not pair there");
		require(countEdgesAcross(core::EdgeType::Window, 2, 3) == 1,
			"The Window authored on pair 2<->3 did not pair there");
		require(countEdgesAcross(core::EdgeType::Window, 0, 1) == 0,
			"A Window paired a Layer pair it was never authored on");
		require(countEdgesAcross(core::EdgeType::Door, 0, 2) == 0
			&& countEdgesAcross(core::EdgeType::Door, 1, 3) == 0,
			"A Door skipped a Layer");

		uint32_t bulkheadEdges{ 0 };
		for (auto const& edge : graph.getEdges())
		{
			if (edge->getType() != core::EdgeType::BulkheadDoor) continue;
			require(edge->getVertex(0)->getSector()->getLayerIndex()
				== edge->getVertex(1)->getSector()->getLayerIndex(),
				"A Bulkhead Door Edge crossed Layers");
			++bulkheadEdges;
		}
		require(bulkheadEdges == 1, "The Bulkhead Door did not produce one same-Layer Edge");

		// Every Transit sits on the Layer it was authored on, and mounts onto the Layer
		// directly in front of it - never any other.
		struct TransitExpectation
		{
			core::SectorType sectorType;
			core::EdgeType mountType;
			uint32_t layer;
		};

		// Enclosed Lifts and Shuttles reach their landing Layer through the landing
		// Doors authored in front of them rather than through mount edges, so they are
		// covered by the Door counts above instead of by a mount expectation here.
		std::vector<TransitExpectation> const expected{
			{ core::SectorType::Ladder, core::EdgeType::LadderMount, 1 },
			{ core::SectorType::Ladder, core::EdgeType::LadderMount, 2 },
			{ core::SectorType::Ladder, core::EdgeType::LadderMount, 3 },
			{ core::SectorType::Stairwell, core::EdgeType::StairwellMount, 3 },
			{ core::SectorType::Staircase, core::EdgeType::StaircaseMount, 3 },
		};

		for (auto const& want : expected)
		{
			uint32_t mounts{ 0 };
			uint32_t strays{ 0 };
			for (auto const& edge : graph.getEdges())
			{
				if (edge->getType() != want.mountType) continue;
				auto const a = edge->getVertex(0)->getSector();
				auto const b = edge->getVertex(1)->getSector();
				auto const transit = a->getType() == want.sectorType ? a : b;
				auto const other = transit == a ? b : a;
				if (transit->getType() != want.sectorType) continue;
				// Only this expectation's own Transit; other Layers are checked separately.
				if (transit->getLayerIndex() != want.layer) continue;
				if (other->getLayerIndex() + 1 == transit->getLayerIndex()) ++mounts;
				else ++strays;
			}
			require(mounts > 0, "A Transit never mounted onto the Layer directly in front of it");
			require(strays == 0, "A Transit mounted onto a Layer other than the one in front of it");
		}

		// The authored pairings survive a save and reload unchanged.
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		core::World reloaded("placeholder", 1, 1);
		require(reloaded.deserialize(*reader, workData), "A deep World did not round-trip");
		require(reloaded.getLayerCount() == world.getLayerCount(),
			"Round-tripping changed the Layer count");

		core::Graph reloadedGraph(&reloaded);
		reloadedGraph.build();
		uint32_t deepDoors{ 0 };
		for (auto const& edge : reloadedGraph.getEdges())
		{
			if (edge->getType() != core::EdgeType::Door) continue;
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			if ((a == 2 && b == 3) || (a == 3 && b == 2)) ++deepDoors;
		}
		require(deepDoors == 1, "The reloaded World lost its deep Door pairing");
	}

	void doorAndWindowRemovalWorksOnDeepLayerPairs(smoke::Context const&)
	{
		// Regression for ticket #19: removeSectorDoor/Window looped over absolute
		// Layer indices and passed them to Door/Window::getSector(), which expects a
		// pair side (0 or 1). With three Layers, getSector(2) asserted or read past
		// the end of mSectors.
		core::World world("Deep pair removal", 8, 3);
		world.addLayer();
		world.addCorridor(0, 0, 8);
		world.addRoom("Basement", 1, 0, 0, 8, 1);
		world.addRoom("Cellar", 2, 0, 0, 8, 1);

		auto const door = world.addSectorDoor(0, 0, 3);
		auto const window = world.addSectorWindow(1, 0, 5, 1, 1, { true });

		world.pauseSimulation();
		require(world.removeSectorDoor(door.door.sector->getIndex(), door.door.index),
			"Door on a three-layer World could not be removed");
		require(world.removeSectorWindow(window.window.sector->getIndex(), window.window.index),
			"Window on a three-layer World could not be removed");
	}

	void shuttleDoorCandidatesAreFoundOnTheShuttleLayer(smoke::Context const&)
	{
		// Regression for ticket #22: the editor passed the Layer a Door is authored
		// on to getShuttleStopCandidatesForDoor, which matches the Shuttle Transit's
		// own Layer - one behind.  Dropping a Door onto a Shuttle stop column found no
		// candidate, so the Add Shuttle stop popup never appeared.
		auto const probe = [](uint32_t shuttleLayer)
		{
			core::World world("Shuttle door candidate layers", 32, 2);
			while (world.getLayerCount() <= shuttleLayer) world.addLayer();
			for (uint32_t layer = 0; layer < shuttleLayer; ++layer)
				world.addCorridor(layer, 0, 0, 31, 1);
			core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
			options.doorMask = 0b101;
			auto const created = world.addShuttle(shuttleLayer, 0, 0, 27, options);
			world.finishBuild();
			auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				created.shuttle.sector);
			require(shuttle && shuttle->getLayerIndex() == shuttleLayer,
				"The Shuttle was not authored on the probed Layer");

			// A new stop at offset 9 lines carriage 0's first door up with column 9 of
			// the landing Layer directly in front of the Shuttle.
			auto const found = world.getShuttleStopCandidatesForDoor(shuttleLayer, 0, 9);
			require(any_of(found.begin(), found.end(), [&](auto const& candidate)
				{
					return candidate.sectorIndex == shuttle->getIndex() && candidate.stopOffset == 9;
				}),
				"No Shuttle stop candidate was offered for a Door over a Shuttle door");
			// The Layer the Door is authored on holds no Shuttle, so querying it as a
			// Shuttle Layer must find nothing.
			require(world.getShuttleStopCandidatesForDoor(shuttleLayer - 1, 0, 9).empty(),
				"A Door Layer was treated as a Shuttle Layer");
		};
		probe(1);
		probe(2);
	}

	void candidateReplayIncludesAllLayers(smoke::Context const&)
	{
		// Regression for ticket #17: validation candidates were constructed with the
		// default two Layers, so any construction record on Layer >= 2 threw an out-of-
		// bounds error and every rebuild-based edit was reported as invalid.
		core::World world("Deep candidate replay", 8, 3);
		world.addLayer();
		auto const fore = world.addCorridor(0, 0, 8);
		world.addRoom("Deep room", 2, 0, 0, 8, 1);
		world.finishBuild();

		auto resize = world.planResizeLocation(fore, 0, 0, 4, 1);
		require(resize.valid,
			("Layer-0 Location edit was rejected on a three-layer World: " + resize.diagnostic).c_str());

		world.pauseSimulation();
		auto const resized = world.applyLocationEdit(resize);
		require(world.getSector(resized) && world.getSector(resized)->getCellsWide() == 4,
			"Layer-0 Location edit was not applied on a three-layer World");
	}

	void liftEditsUseTheLiftsOwnLayer(smoke::Context const&)
	{
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Lift edit", 12, 3);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			world.addRoom("Landing A", transitLayer - 1, 0, 0, 12, 1);
			world.addRoom("Landing B", transitLayer - 1, 1, 0, 12, 1);
			world.addRoom("Landing C", transitLayer - 1, 2, 0, 12, 1);
			// A neighbour on the Lift's own Layer, to prove the blocker scan still runs
			// against that Layer rather than being skipped.
			world.addRoom("Shaft neighbour", transitLayer, 1, 8, 1, 1);
			auto const created = world.addLift(transitLayer, 0, 2, 1, 3);
			world.finishBuild();
			world.pauseSimulation();
			auto const index = created.lift.sector->getIndex();
			require(created.lift.sector->getLayerIndex() == transitLayer,
				"The Lift was not authored on the probed Layer");

			auto const blocked = world.planResizeLift(index, 8, 0, 1, 3);
			require(!blocked.valid
				&& blocked.diagnostic.find("8,1 blocks the Lift") != std::string::npos,
				("A Lift move into a Sector on its own Layer was not blocked: "
					+ blocked.diagnostic).c_str());

			auto const plan = world.planResizeLift(index, 5, 0, 1, 3);
			require(plan.valid && plan.move,
				("A Lift move on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
				"The Lift stops were not derived from the Layer in front of the Lift");
			auto const moved = world.applyLiftEdit(plan);
			require(moved == index,
				"applyLiftEdit returned a Sector from the wrong Layer");
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(moved));
			require(lift && lift->getCellX() == 5 && lift->getLayerIndex() == transitLayer
				&& lift->getNumStops() == 3,
				"The Lift was not moved on its own Layer");

			auto const stopRemoval = world.planRemoveLiftStop(moved, 1);
			require(stopRemoval.valid,
				("Deleting a Lift stop on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + stopRemoval.diagnostic).c_str());
			auto const trimmed = world.applyLiftEdit(stopRemoval);
			auto const trimmedLift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(trimmed));
			require(trimmedLift && trimmedLift->getNumStops() == 2
				&& trimmedLift->getLayerIndex() == transitLayer,
				"The deep Lift did not lose its deleted stop");

			auto const removal = world.planRemoveLift(trimmed);
			require(removal.valid,
				("A Lift deletion on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + removal.diagnostic).c_str());
			require(world.applyLiftEdit(removal) == ~0u,
				"Deleting a deep Lift did not return the removed-sector sentinel");
		};
		probe(1);
		probe(2);
	}

	void shuttleEditsUseTheShuttlesOwnLayer(smoke::Context const&)
	{
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Shuttle edit", 32, 2);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			world.addCorridor(transitLayer - 1, 0, 0, 31, 1);
			core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
			options.capacity = 2;
			options.doorMask = 0b101;
			// A neighbour on the Shuttle's own Layer keeps the blocker scan honest.
			world.addRoom("Track neighbour", transitLayer, 0, 29, 1, 1);
			auto const created = world.addShuttle(transitLayer, 0, 0, 27, options);
			world.finishBuild();
			world.pauseSimulation();
			auto const index = created.shuttle.sector->getIndex();
			require(created.shuttle.sector->getLayerIndex() == transitLayer,
				"The Shuttle was not authored on the probed Layer");

			auto const plan = world.planResizeShuttle(index, 2, 0, 27);
			require(plan.valid && plan.move,
				("A Shuttle move on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + plan.diagnostic).c_str());
			auto const moved = world.applyShuttleEdit(plan);
			require(moved == index,
				"applyShuttleEdit returned a Sector from the wrong Layer");
			auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(moved));
			require(shuttle && shuttle->getCellX() == 2 && shuttle->getLayerIndex() == transitLayer
				&& shuttle->getNumStops() == 2,
				"The Shuttle was not moved on its own Layer");

			auto const blocked = world.planResizeShuttle(moved, 3, 0, 27);
			require(!blocked.valid
				&& blocked.diagnostic.find("29,0 blocks the Shuttle") != std::string::npos,
				("A Shuttle track grown into a Sector on its own Layer was not blocked: "
					+ blocked.diagnostic).c_str());
			auto const addStop = world.planAddShuttleStop(moved, 9);
			require(addStop.valid,
				("Adding a Shuttle stop on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + addStop.diagnostic).c_str());
			auto const widened = world.applyShuttleEdit(addStop);
			auto const widenedShuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(widened));
			require(widenedShuttle && widenedShuttle->getNumStops() == 3
				&& widenedShuttle->getLayerIndex() == transitLayer,
				"The deep Shuttle did not gain its new stop");
			auto const removeStop = world.planRemoveShuttleStop(widened, 1);
			require(removeStop.valid,
				("Deleting a Shuttle stop on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + removeStop.diagnostic).c_str());
			auto const narrowed = world.applyShuttleEdit(removeStop);
			auto const narrowedShuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(narrowed));
			require(narrowedShuttle && narrowedShuttle->getNumStops() == 2,
				"The deep Shuttle did not lose its deleted stop");

			auto const removal = world.planRemoveShuttle(narrowed);
			require(removal.valid,
				("A Shuttle deletion on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + removal.diagnostic).c_str());
			require(world.applyShuttleEdit(removal) == ~0u,
				"Deleting a deep Shuttle did not return the removed-sector sentinel");
		};
		probe(1);
		probe(2);
	}

	void shuttleDeletionRemovesWindowsOverTheShuttleItself(smoke::Context const&)
	{
		// A Window looks into the Layer directly behind the Layer it is authored on, so
		// a Window resting on a Shuttle depends on that Shuttle's cells on the Shuttle's
		// own Layer.  Deleting the Shuttle must take the dependent Window with it.
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Shuttle Window dependency", 32, 2);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			world.addCorridor(transitLayer - 1, 0, 0, 31, 1);
			core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
			options.doorMask = 0b101;
			auto const created = world.addShuttle(transitLayer, 0, 0, 27, options);
			// Columns 8 and 9 carry no carriage door, so the Window lands on bare Shuttle.
			world.addSectorWindow(transitLayer - 1, 0, 8, 2, 1);
			world.finishBuild();
			world.pauseSimulation();
			auto const shuttleIndex = created.shuttle.sector->getIndex();
			auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(shuttleIndex));
			require(shuttle && shuttle->getLayerIndex() == transitLayer,
				"The Shuttle was not authored on the probed Layer");

			auto const plan = world.planRemoveShuttle(shuttleIndex);
			require(plan.valid,
				("Deleting a Shuttle with a dependent Window on Layer "
					+ std::to_string(transitLayer) + " was rejected: " + plan.diagnostic).c_str());
			require(any_of(plan.consequences.begin(), plan.consequences.end(),
				[](std::string const& consequence)
				{
					return consequence.find("dependent Window") != std::string::npos;
				}),
				"The dependent Window was not reported as a consequence of Shuttle deletion");
			require(world.applyShuttleEdit(plan) == ~0u,
				"Deleting a deep Shuttle with a dependent Window failed");
			require(!static_cast<core::World const&>(world).getLayer(transitLayer)
				->getCellDefinition(8, 0).occupied(),
				"The deleted Shuttle still occupies its own Layer");
		};
		probe(1);
		probe(2);
	}

	void ladderEditsUseTheLaddersOwnLayer(smoke::Context const&)
	{
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Ladder edit", 10, 5);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			for (uint32_t y = 0; y < 5; ++y) world.addCorridor(transitLayer - 1, y, 0, 10, 1);
			auto const created = world.addLadder(transitLayer, 0, 1, { 3, false, true });
			world.finishBuild();
			world.pauseSimulation();
			auto const index = created.ladder.sector->getIndex();
			require(created.ladder.sector->getLayerIndex() == transitLayer,
				"The Ladder was not authored on the probed Layer");

			core::World::CreateLadderOptions edited{ 3, true, false, 3 };
			auto const plan = world.planResizeLadder(index, 4, 1, edited);
			require(plan.valid && plan.move,
				("A Ladder move on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + plan.diagnostic).c_str());
			auto const moved = world.applyLadderEdit(plan);
			require(moved == index,
				"applyLadderEdit returned a Sector from the wrong Layer");
			auto const ladder = std::dynamic_pointer_cast<const core::LadderTransit>(
				world.getSector(moved));
			require(ladder && ladder->getCellX() == 4 && ladder->getLayerIndex() == transitLayer
				&& ladder->getLevelsHigh() == 3,
				"The Ladder was not moved on its own Layer");

			auto const removal = world.planRemoveLadder(moved);
			require(removal.valid,
				("A Ladder deletion on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + removal.diagnostic).c_str());
			require(world.applyLadderEdit(removal) == ~0u,
				"Deleting a deep Ladder did not return the removed-sector sentinel");
		};
		probe(1);
		probe(2);
	}

	void stairwellEditsUseTheStairwellsOwnLayer(smoke::Context const&)
	{
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Stairwell edit", 10, 5);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			for (uint32_t y = 0; y < 5; ++y) world.addCorridor(transitLayer - 1, y, 0, 10, 1);
			auto const created = world.addStairwell(transitLayer, 0, 1,
				core::World::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
			world.finishBuild();
			world.pauseSimulation();
			auto const index = created.sectorIndex;
			require(world.getSector(index)->getLayerIndex() == transitLayer,
				"The Stairwell was not authored on the probed Layer");

			core::World::CreateStairwellOptions edited{ 3, CORE_SIDE_RIGHT, 2, 3 };
			auto const plan = world.planResizeStairwell(index, 4, 1, edited);
			require(plan.valid && plan.move,
				("A Stairwell move on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + plan.diagnostic).c_str());
			auto const moved = world.applyStairwellEdit(plan);
			require(moved == index,
				"applyStairwellEdit returned a Sector from the wrong Layer");
			auto const stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
				world.getSector(moved));
			require(stairwell && stairwell->getCellX() == 4
				&& stairwell->getLayerIndex() == transitLayer && stairwell->getLevelsHigh() == 3,
				"The Stairwell was not moved on its own Layer");

			auto const removal = world.planRemoveStairwell(moved);
			require(removal.valid,
				("A Stairwell deletion on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + removal.diagnostic).c_str());
			require(world.applyStairwellEdit(removal) == ~0u,
				"Deleting a deep Stairwell did not return the removed-sector sentinel");
		};
		probe(1);
		probe(2);
	}

	void staircaseEditsReturnTheStaircaseOwnLayer(smoke::Context const&)
	{
		// planResizeStaircase already derives its Layers from the Staircase itself, but
		// applyStaircaseEdit still read its return value from Layer 1.
		auto const probe = [](uint32_t transitLayer)
		{
			core::World world("Deep Staircase edit", 6, 3);
			while (world.getLayerCount() <= transitLayer) world.addLayer();
			world.addCorridor(transitLayer - 1, 0, 0, 1, 1);
			world.addCorridor(transitLayer - 1, 0, 3, 1, 1);
			world.addCorridor(transitLayer - 1, 1, 0, 1, 1);
			world.addCorridor(transitLayer - 1, 1, 3, 1, 1);
			auto const index = world.addStaircase(transitLayer, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
			world.finishBuild();
			world.pauseSimulation();
			require(world.getSector(index)->getLayerIndex() == transitLayer,
				"The Staircase was not authored on the probed Layer");

			auto const plan = world.planResizeStaircase(index, 0, 0,
				{ 4, CORE_SIDE_LEFT, -0.75f });
			require(plan.valid,
				("A Staircase flip on Layer " + std::to_string(transitLayer)
					+ " was rejected: " + plan.diagnostic).c_str());
			auto const flipped = world.applyStaircaseEdit(plan);
			require(flipped == index,
				"applyStaircaseEdit returned a Sector from the wrong Layer");
			auto const transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(
				world.getSector(flipped));
			require(transit && transit->getLayerIndex() == transitLayer
				&& transit->getRiseSide() == CORE_SIDE_LEFT,
				"The Staircase was not edited on its own Layer");
		};
		probe(1);
		probe(2);
	}
}
