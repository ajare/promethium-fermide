#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/SectorObjectType.h"
#include "core/World.h"
#include "core/WorldBuilder.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("WorldBuilder VerticalTransit check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("WorldBuilder VerticalTransit check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The builder is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern for the VerticalTransit
		// authoring family. World keeps the state it edits (ADR 0001) -
		// the Sectors, Layers, cells, traversal resources, interaction
		// points, and construction records; the builder holds only a
		// reference.
		//
		// Layer 0 is the front-most Layer and carries the landing
		// Locations; the Transits are authored on Layer 1 and land on
		// Layer 0 directly in front.
		World world("wb-vt-checks", 24, 6);
		WorldBuilder builder(world);
		World const& view = world;
		auto cell = [&view](uint32_t layer, uint32_t x, uint32_t y) -> CellDefinition const&
		{
			return view.getLayer(layer)->getCellDefinition(x, y);
		};

		std::string diagnostic;

		// Landing Locations on the front Layer. Landing Levels are single-
		// level Rooms stacked vertically: the upper Level of a multi-level
		// Room has no floor to land on.
		builder.addRoom("LadderLow", 0, 0, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("LadderHigh", 0, 1, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("StairwellLandingLow", 0, 2, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("StairwellLandingHigh", 0, 3, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("StairwellLanding2Low", 0, 2, 4, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("StairwellLanding2High", 0, 3, 4, 3, 1, CORE_ROOM_MAX_HEIGHT);
		auto const stairRoom = builder.addRoom("StairRoom", 0, 4, 0, 4, 2, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("LiftStop0", 0, 0, 8, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("LiftStop1", 0, 1, 8, 3, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("ShuttleStopA", 0, 2, 12, 4, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("ShuttleStopB", 0, 2, 17, 4, 1, CORE_ROOM_MAX_HEIGHT);
		builder.addRoom("TallSingle", 0, 2, 21, 2, 2, CORE_ROOM_MAX_HEIGHT);
		// The Staircase upper endpoint must meet an open Room wall into an
		// adjacent traversable Location, so open the StairRoom's upper-right
		// wall into a Corridor.
		builder.addCorridor(0, 5, 4, 2, 1);
		world.removeLocationWall(stairRoom, 1, CORE_SIDE_RIGHT);

		// ---------------------------------------------------------------
		// Ladder: canAddLadder judged before anything is written.
		// ---------------------------------------------------------------
		require(builder.canAddLadder(1, 0, 0, 2, &diagnostic),
			"valid Ladder was refused: " + diagnostic);
		{
			// Facade agreement: the World facade and the builder must
			// return the same verdict.
			std::string worldDiagnostic;
			auto const builderVerdict = builder.canAddLadder(1, 0, 0, 2, &diagnostic);
			auto const worldVerdict = world.canAddLadder(1, 0, 0, 2, &worldDiagnostic);
			require(builderVerdict == worldVerdict,
				"facade disagrees with the builder on a Ladder verdict");
		}
		require(!builder.canAddLadder(0, 0, 0, 2, &diagnostic)
			&& diagnostic.find("Layer in front") != std::string::npos,
			"Ladder on the front-most Layer was accepted: " + diagnostic);
		require(!builder.canAddLadder(1, 0, 0, 1, &diagnostic)
			&& diagnostic.find("at least two levels") != std::string::npos,
			"one-level Ladder was accepted: " + diagnostic);
		require(!builder.canAddLadder(1, 0, 0, 9, &diagnostic)
			&& diagnostic.find("outside the World bounds") != std::string::npos,
			"out-of-bounds Ladder was accepted: " + diagnostic);
		require(!builder.canAddLadder(1, 2, 21, 2, &diagnostic)
			&& diagnostic.find("two different landing Locations") != std::string::npos,
			"Ladder over one Location only was accepted: " + diagnostic);

		// A refusal leaves nothing behind.
		auto const sectorsBeforeLadder = world.getNumSectors();
		requireThrows([&]{
			World::CreateLadderOptions bad{ 1, false, false };
			builder.addLadder(1, 0, 0, bad);
		}, "one-level Ladder add was accepted");
		require(world.getNumSectors() == sectorsBeforeLadder,
			"a refused Ladder add left a Sector behind");
		require(cell(1, 0, 0).sectorIndex == ~0u,
			"a refused Ladder add stamped a cell");

		// An extensible Ladder: span, traversal resource, and one
		// extension control per endpoint.
		World::CreateLadderOptions extensible{ 2, true, false };
		auto const ladder = builder.addLadder(1, 0, 0, extensible);
		require(ladder.ladder.type == SectorObjectType::Ladder,
			"Ladder add did not produce a Ladder object");
		require(ladder.traversalResource != TraversalResourceId{},
			"Ladder add did not create a traversal resource");
		require(ladder.controls[0].sector && ladder.controls[1].sector,
			"extensible Ladder add did not create both endpoint controls");
		// The span cells on the authored Layer carry the Ladder's Sector.
		require(cell(1, 0, 0).sectorIndex == ladder.ladder.sector->getIndex()
			&& cell(1, 0, 1).sectorIndex == ladder.ladder.sector->getIndex(),
			"Ladder span cells were not stamped on the transit Layer");
		// The same span cannot take a second Ladder.
		require(!builder.canAddLadder(1, 0, 0, 2, &diagnostic)
			&& diagnostic.find("blocks the Ladder") != std::string::npos,
			"a second Ladder on the same span was accepted: " + diagnostic);

		// ---------------------------------------------------------------
		// Stairwell.
		// ---------------------------------------------------------------
		require(builder.canAddStairwell(1, 2, 0, 2, &diagnostic),
			"valid Stairwell was refused: " + diagnostic);
		require(!builder.canAddStairwell(1, 2, 2, 2, &diagnostic)
			&& diagnostic.find("landing Locations") != std::string::npos,
			"Stairwell spanning outside its landing was accepted: " + diagnostic);
		require(!builder.canAddStairwell(1, 2, 0, 1, &diagnostic),
			"one-level Stairwell was accepted");

		// A directional capacity without a positive batch limit refuses
		// before anything is written.
		auto const sectorsBeforeStairwell = world.getNumSectors();
		requireThrows([&]{
			World::CreateStairwellOptions noBatch{ 2, CORE_SIDE_LEFT, 1, 0 };
			builder.addStairwell(1, 2, 4, noBatch);
		}, "directional Stairwell with a zero batch limit was accepted");
		require(world.getNumSectors() == sectorsBeforeStairwell,
			"a refused Stairwell add left a Sector behind");

		// The plain overload builds the ordinary unconstrained Stairwell.
		auto const stairwellIndex = builder.addStairwell(1, 2, 0, 2, CORE_SIDE_LEFT);
		require(cell(1, 0, 2).sectorIndex == stairwellIndex
			&& cell(1, 1, 3).sectorIndex == stairwellIndex,
			"Stairwell span cells were not stamped on the transit Layer");
		// The options overload with a directional capacity creates the
		// narrow-stairwell traversal resource.
		World::CreateStairwellOptions narrow{ 2, CORE_SIDE_LEFT, 1, 2 };
		auto const narrowStairwell = builder.addStairwell(1, 2, 4, narrow);
		require(narrowStairwell.traversalResource != TraversalResourceId{},
			"directional Stairwell add did not create a traversal resource");

		// ---------------------------------------------------------------
		// Staircase.
		// ---------------------------------------------------------------
		require(builder.canAddStaircase(1, 4, 1, 3, CORE_SIDE_RIGHT, &diagnostic),
			"valid Staircase was refused: " + diagnostic);
		require(!builder.canAddStaircase(1, 4, 1, 3, 7, &diagnostic)
			&& diagnostic.find("rise direction is invalid") != std::string::npos,
			"Staircase with an invalid rise side was accepted: " + diagnostic);
		require(!builder.canAddStaircase(1, 4, 1, 1, CORE_SIDE_RIGHT, &diagnostic)
			&& diagnostic.find("at least two cells wide") != std::string::npos,
			"one-cell Staircase was accepted: " + diagnostic);

		requireThrows([&]{
			World::CreateStaircaseOptions stationaryBroken{ 2, CORE_SIDE_RIGHT, 0.0f, true };
			builder.addStaircase(1, 4, 1, stationaryBroken);
		}, "a stationary Broken Staircase was accepted");
		requireThrows([&]{
			World::CreateStaircaseOptions nanSpeed{ 2, CORE_SIDE_RIGHT,
				std::numeric_limits<float>::quiet_NaN(), false };
			builder.addStaircase(1, 4, 1, nanSpeed);
		}, "a Staircase with a NaN speed was accepted");
		require(world.getNumSectors() == sectorsBeforeStairwell + 2,
			"refused Staircase adds left Sectors behind");

		auto const staircaseIndex = builder.addStaircase(1, 4, 1, 3, CORE_SIDE_RIGHT, 0.5f);
		require(cell(1, 1, 4).sectorIndex == staircaseIndex
			&& cell(1, 3, 5).sectorIndex == staircaseIndex,
			"Staircase span cells were not stamped on the transit Layer");

		// ---------------------------------------------------------------
		// Enclosed Lift.
		// ---------------------------------------------------------------
		auto const rows = builder.getLiftLandingRows(1, 0, 8, 2, 2);
		require(rows.size() == 2, "Lift shaft did not read two landing rows");
		require(rows[0].usableForStop() && rows[1].usableForStop(),
			"both Lift landing rows over open Locations were not usable");
		require(builder.getLiftLandingRows(0, 0, 8, 2, 2).empty(),
			"the front-most Layer reported Lift landings");

		auto const sectorsBeforeLift = world.getNumSectors();
		requireThrows([&]{ builder.addLift(1, 0, 8, 3, 2); },
			"a three-cell-wide editor Lift was accepted");
		requireThrows([&]{ builder.addLift(1, 0, 8, 0, 2); },
			"a zero-width editor Lift was accepted");
		require(world.getNumSectors() == sectorsBeforeLift,
			"a refused Lift add left Sectors behind");

		auto const lift = builder.addLift(1, 0, 8, 2, 2);
		require(lift.lift.type == SectorObjectType::Lift,
			"Lift add did not produce a Lift object");
		require(lift.doors.size() == 2,
			"Lift add did not derive one landing Door per usable row");
		require(lift.traversalResource != TraversalResourceId{},
			"Lift add did not create a coordinator resource");
		require(lift.interiorSelector != InteractionPointId{},
			"Lift add did not create the interior destination selector");
		require(cell(1, 8, 0).sectorIndex == lift.lift.sector->getIndex()
			&& cell(1, 9, 1).sectorIndex == lift.lift.sector->getIndex(),
			"Lift shaft cells were not stamped on the transit Layer");

		// ---------------------------------------------------------------
		// Shuttle.
		// ---------------------------------------------------------------
		World::CreateShuttleOptions shuttleOptions{ 1, 4, { 0, 6 }, 0 };
		shuttleOptions.doorMask = 0b0001;
		auto const sectorsBeforeShuttle = world.getNumSectors();
		requireThrows([&]{ builder.addShuttle(1, 2, 12, 3, shuttleOptions); },
			"a Shuttle wider than its track was accepted");
		requireThrows([&]{
			World::CreateShuttleOptions tight{ 1, 4, { 0, 3 }, 0 };
			tight.doorMask = 0b0001;
			builder.addShuttle(1, 2, 12, 10, tight);
		}, "Shuttle stops closer than the carriage width were accepted");
		require(world.getNumSectors() == sectorsBeforeShuttle,
			"a refused Shuttle add left Sectors behind");

		auto const shuttle = builder.addShuttle(1, 2, 12, 10, shuttleOptions);
		require(shuttle.shuttle.type == SectorObjectType::Shuttle,
			"Shuttle add did not produce a Shuttle object");
		require(shuttle.doors.size() == 2,
			"Shuttle add did not produce one Door per stop");
		require(shuttle.traversalResource != TraversalResourceId{},
			"Shuttle add did not create a coordinator resource");
		require(cell(1, 12, 2).sectorIndex == shuttle.shuttle.sector->getIndex()
			&& cell(1, 21, 2).sectorIndex == shuttle.shuttle.sector->getIndex(),
			"Shuttle track cells were not stamped on the transit Layer");

		std::cout << "WorldBuilder VerticalTransit checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "WorldBuilder VerticalTransit checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
