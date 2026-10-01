#include "Checks.h"
// Door option and placement preflight, for ticket #196.
//
// validateSectorDoorOptions() checked a Door's height and hold-open time but
// not its width or explicit crossing-lane count. Two bad outcomes followed:
//
//   * width == 0 passed every check. The placement loop in _addSectorDoor()
//     ran zero times, leaving the Door's two Sector pointers unset for the
//     queue configuration to dereference - a crash reached both from the
//     public API and from a malformed replayed construction record.
//   * crossingLanes > width was not refused until configureDoorCrossingLanes(),
//     after the Door's SectorObjects and traversal resource already existed, so
//     a rejected call left partial, unrecorded state and dirty topology.
//
// The full option and placement preflight now runs before
// beginStructuralEdit(), so a refused Door add is a true no-op. These checks
// compare Sector object counts, traversal-resource snapshots, construction
// records/serialization, modified state, and topology state before and after
// each refusal, and confirm one-cell, wide, and explicit-lane Doors still
// build and cross.

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Runs an action expected to fail and returns the diagnostic it produced.
	// An empty string means the action was wrongly accepted.
	std::string refusalMessage(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (core::Exception const& error)
		{
			return error.getMessage();
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	uint64_t sectorObjectCount(core::World const& world)
	{
		uint64_t count = 0;
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
			for (auto const& sector : world.getSectors(layer))
				if (sector) count += sector->getNumObjects();
		return count;
	}

	// Everything a refused Door add must leave untouched.
	struct WorldState
	{
		uint32_t sectors;
		uint64_t sectorObjects;
		std::string serialized;
		size_t traversalResources;
		bool modified;
		bool topologyDirty;
		bool topologyValid;
	};

	WorldState captureState(core::World const& world)
	{
		return WorldState{
			world.getNumSectors(),
			sectorObjectCount(world),
			serializeWorld(world),
			world.getSimulationSnapshot().traversalResources.size(),
			world.isModified(),
			world.isTraversalTopologyDirty(),
			world.isTraversalTopologyValid()
		};
	}

	void requireIdentical(core::World const& world, WorldState const& before, std::string const& what)
	{
		require(world.getNumSectors() == before.sectors,
			what + ": a refused Door add changed the Sector count");
		require(sectorObjectCount(world) == before.sectorObjects,
			what + ": a refused Door add created or removed a SectorObject");
		require(serializeWorld(world) == before.serialized,
			what + ": a refused Door add changed the serialized construction records");
		require(world.getSimulationSnapshot().traversalResources.size() == before.traversalResources,
			what + ": a refused Door add created or removed a traversal resource");
		require(!world.isModified() && !before.modified,
			what + ": a refused Door add marked the saved World modified");
		require(world.isTraversalTopologyDirty() == before.topologyDirty
			&& world.isTraversalTopologyValid() == before.topologyValid
			&& world.isTraversalTopologyValid(),
			what + ": a refused Door add changed the traversal topology state");
	}

	// Runs the attempt against a saved, finished, paused World whose Door pair
	// is valid, so the only possible refusal is the one under test and any other
	// change the call makes is visible.
	void inCleanWorld(std::function<void(core::World&)> attempt, std::string const& what)
	{
		core::World world("Door preflight", 8, 2);
		world.addCorridor(0u, 0u, 0u, 6u, 1u);
		world.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
		world.finishBuild();
		world.markSaved();
		world.pauseSimulation();

		require(!world.isModified() && world.isTraversalTopologyValid(),
			what + ": the clean-World setup did not start clean");
		world.consumeSimulationEvents();

		auto const before = captureState(world);
		auto const diagnostic = refusalMessage([&] { attempt(world); });
		require(!diagnostic.empty(), what + " was accepted");
		require(diagnostic.find("at least one cell") != std::string::npos
			|| diagnostic.find("crossing lane count") != std::string::npos,
			what + " carried no Door-geometry diagnostic: " + diagnostic);
		require(world.consumeSimulationEvents().empty(), what + " published an event");

		requireIdentical(world, before, what);
	}

	// The public API refuses a zero-width Door before any mutation.
	void zeroWidthDoorIsRefused()
	{
		inCleanWorld([](core::World& world)
		{
			core::World::CreateDoorOptions options;
			options.width = 0;
			world.addSectorDoor(0u, 0u, 1u, options);
		}, "zero-width Door");
	}

	// The public API refuses crossing lanes beyond the threshold width before
	// any SectorObject, traversal resource, event, or construction record exists.
	void overWideCrossingLanesAreRefused()
	{
		inCleanWorld([](core::World& world)
		{
			core::World::CreateDoorOptions options;
			options.width = 1;
			options.crossingLanes = 2;
			world.addSectorDoor(0u, 0u, 1u, options);
		}, "over-wide Door crossing lanes");
	}

	std::string loadFailure(std::string const& yaml)
	{
		core::World target("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		target.deserialize(*reader, workData);
		return {};
	}

	// A valid door world, serialized, so a replay fixture cannot drift from the
	// format the World actually writes.
	std::string doorWorldYaml()
	{
		core::World source("Door replay", 8, 2);
		source.addCorridor(0u, 0u, 0u, 6u, 1u);
		source.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
		source.addSectorDoor(0u, 0u, 1u, core::World::CreateDoorOptions{});
		source.finishBuild();
		return serializeWorld(source);
	}

	void replaceOnce(std::string& text, std::string const& from, std::string const& to,
		std::string const& what)
	{
		auto const at = text.find(from);
		require(at != std::string::npos, "The replay fixture has no " + what + " field");
		require(text.find(from, at + 1) == std::string::npos,
			"The replay fixture's " + what + " field is not unique");
		text.replace(at, from.size(), to);
	}

	// A malformed replay record with width 0 is refused with a diagnostic rather
	// than terminating the process.
	void replayedZeroWidthDoorIsRefused()
	{
		auto yaml = doorWorldYaml();
		replaceOnce(yaml, "width: 1", "width: 0", "Door width");
		auto const diagnostic = refusalMessage([&] { loadFailure(yaml); });
		require(!diagnostic.empty(), "A replayed zero-width Door was accepted");
		require(diagnostic.find("at least one cell") != std::string::npos,
			"A replayed zero-width Door gave no width diagnostic: " + diagnostic);
	}

	// A malformed replay record with crossingLanes beyond the width is refused
	// before the Door is built.
	void replayedOverWideCrossingLanesAreRefused()
	{
		auto yaml = doorWorldYaml();
		replaceOnce(yaml, "crossingLanes: 0", "crossingLanes: 2", "crossingLanes");
		auto const diagnostic = refusalMessage([&] { loadFailure(yaml); });
		require(!diagnostic.empty(), "A replayed over-wide Door crossing was accepted");
		require(diagnostic.find("crossing lane count") != std::string::npos,
			"A replayed over-wide Door crossing gave no lane diagnostic: " + diagnostic);
	}

	// The same document with honest values still loads.
	void honestDoorRecordStillLoads()
	{
		auto const diagnostic = refusalMessage([&] { loadFailure(doorWorldYaml()); });
		require(diagnostic.empty(), "A valid Door document was refused: " + diagnostic);
	}

	// Derived lanes follow the threshold width, explicit lanes are honoured, and
	// the explicit count is what the traversal resource carries.
	void validDoorGeometryCarriesTheRightLaneCount()
	{
		core::World world("Derived and explicit lanes", 10, 2);
		world.addRoom("Front", 0u, 0u, 0u, 10u, 1u);
		world.addRoom("Back", 1u, 0u, 0u, 10u, 1u);
		auto const oneCell = world.addSectorDoor(0u, 0u, 1u, core::World::CreateDoorOptions{});
		core::World::CreateDoorOptions wide;
		wide.width = 3;
		auto const derived = world.addSectorDoor(0u, 0u, 4u, wide);
		core::World::CreateDoorOptions explicitLanes;
		explicitLanes.width = 3;
		explicitLanes.crossingLanes = 2;
		auto const explicitDoor = world.addSectorDoor(0u, 0u, 7u, explicitLanes);
		world.finishBuild();
		require(world.isTraversalTopologyValid(),
			"Valid Doors left the traversal topology invalid");

		auto const& resources = world.getSimulationSnapshot().traversalResources;
		auto laneCount = [&](core::TraversalResourceId id)
		{
			for (auto const& resource : resources)
				if (resource.id == id) return resource.crossingLanes.size();
			return size_t{ 0 };
		};
		require(laneCount(oneCell.traversalResource) == 1,
			"A one-cell Door did not derive its single crossing lane");
		require(laneCount(derived.traversalResource) == 3,
			"A wide Door did not derive one crossing lane per threshold cell");
		require(laneCount(explicitDoor.traversalResource) == 2,
			"An explicit crossing-lane count was not carried by the traversal resource");
	}

	// A wide Door still lets an Agent cross into the Layer behind.
	void wideDoorStillCrosses()
	{
		core::World world("Wide Door crossing", 8, 2);
		auto const front = world.addRoom("Front", 0u, 0u, 0u, 8u, 1u);
		auto const back = world.addRoom("Back", 1u, 0u, 0u, 8u, 1u);
		core::World::CreateDoorOptions wide;
		wide.width = 3;
		world.addSectorDoor(0u, 0u, 2u, wide);
		world.addSectorMarker(back, 0, 7.5f, "End");
		world.finishBuild();

		auto const id = world.createAgent("Walker", front, 0, 0.5f);
		auto const marker = world.getMarkerIds()[0];
		require(world.moveAgentToMarker(id, marker).accepted(), "Wide Door route refused");
		world.advanceTicks(2000);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getSector()->getIndex() == back,
			"The Agent did not cross the wide Door");
	}
}

void permission_smoke::registerPreflight(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "zeroWidthDoorIsRefused",
		[](smoke::Context const&)
		{
			zeroWidthDoorIsRefused();
		} });
	checks.push_back({ "overWideCrossingLanesAreRefused",
		[](smoke::Context const&)
		{
			overWideCrossingLanesAreRefused();
		} });
	checks.push_back({ "replayedZeroWidthDoorIsRefused",
		[](smoke::Context const&)
		{
			replayedZeroWidthDoorIsRefused();
		} });
	checks.push_back({ "replayedOverWideCrossingLanesAreRefused",
		[](smoke::Context const&)
		{
			replayedOverWideCrossingLanesAreRefused();
		} });
	checks.push_back({ "honestDoorRecordStillLoads",
		[](smoke::Context const&)
		{
			honestDoorRecordStillLoads();
		} });
	checks.push_back({ "validDoorGeometryCarriesTheRightLaneCount",
		[](smoke::Context const&)
		{
			validDoorGeometryCarriesTheRightLaneCount();
		} });
	checks.push_back({ "wideDoorStillCrosses",
		[](smoke::Context const&)
		{
			wideDoorStillCrosses();
		} });
}
