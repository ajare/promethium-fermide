#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "core/Background.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"
#include "core/WorldBuilder.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("WorldBuilder Sectors check failed: " + what);
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
		throw std::runtime_error("WorldBuilder Sectors check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The builder is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern for the Sector authoring
		// family. World keeps the state it edits (ADR 0001) - the
		// Sectors, Layers, cells, and construction records; the builder
		// holds only a reference.
		World world("wb-sectors-checks", 10, 3);
		WorldBuilder builder(world);
		// World's non-const getLayer is private; read cells through the
		// public const view.
		World const& view = world;
		auto cell = [&view](uint32_t layer, uint32_t x, uint32_t y) -> CellDefinition const&
		{
			return view.getLayer(layer)->getCellDefinition(x, y);
		};

		std::string diagnostic;
		auto const nan = std::numeric_limits<float>::quiet_NaN();

		// ---------------------------------------------------------------
		// Room: judged before anything is written; a refusal is a no-op.
		// ---------------------------------------------------------------
		auto const roomA = builder.addRoom("Room A", 0, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT);
		require(roomA == 0 && world.getNumSectors() == 1,
			"Room add did not append exactly one Sector");
		auto room = world.getSector(roomA);
		require(room->getType() == SectorType::Location && room->isRoom(),
			"Room add did not produce a non-corridor Location");
		require(room->getName() == "Room A", "Room add lost its name");
		auto const& groundCell = cell(0, 0, 0);
		require(groundCell.sectorIndex == roomA && groundCell.floorType == CellFloorType::Ground,
			"Room ground level was not stamped Ground");
		auto const& upperCell = cell(0, 0, 1);
		require(upperCell.sectorIndex == roomA && upperCell.floorType == CellFloorType::None,
			"Room upper level was not stamped None");

		auto const sectorsAfterRoom = world.getNumSectors();
		requireThrows([&]{ builder.addRoom("NaN", 0, 0, 6, 1, 1, nan); },
			"Room with a NaN topLevelHeight was accepted");
		requireThrows([&]{ builder.addRoom("Tall", 0, 0, 6, 1, 1, CORE_ROOM_MAX_HEIGHT + 0.1f); },
			"Room above the maximum height was accepted");
		requireThrows([&]{ builder.addRoom("Short", 0, 0, 6, 1, 1, CORE_ROOM_MIN_HEIGHT - 0.1f); },
			"Room below the minimum height was accepted");
		requireThrows([&]{ builder.addRoom("Flat", 0, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT); },
			"zero-width Room was accepted");
		requireThrows([&]{ builder.addRoom("Thin", 0, 0, 6, 1, 0, CORE_ROOM_MAX_HEIGHT); },
			"zero-height Room was accepted");
		requireThrows([&]{ builder.addRoom("Nowhere", 99, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT); },
			"Room on an unknown Layer was accepted");
		requireThrows([&]{ builder.addRoom("Overlap", 0, 0, 2, 1, 1, CORE_ROOM_MAX_HEIGHT); },
			"Room overlapping an occupied cell was accepted");
		require(world.getNumSectors() == sectorsAfterRoom,
			"a refused Room add left a Sector behind");
		require(cell(0, 6, 0).sectorIndex == ~0u,
			"a refused Room add stamped a cell");

		// ---------------------------------------------------------------
		// Corridor: a Location that may sit on any Layer.
		// ---------------------------------------------------------------
		auto const corridor = builder.addCorridor(0, 5, 2, 1);
		require(world.getSector(corridor)->getType() == SectorType::Location,
			"Corridor add did not produce a Location");
		auto corridorLoc = std::dynamic_pointer_cast<const Location>(world.getSector(corridor));
		require(corridorLoc && corridorLoc->isCorridor(),
			"Corridor add did not set the corridor flag");
		require(world.getSector(corridor)->getName() == "Corridor",
			"Corridor add lost its name");
		requireThrows([&]{ builder.addCorridor(0, 0, 0, 1); },
			"zero-width Corridor was accepted");
		requireThrows([&]{ builder.addCorridor(0, 0, 1, 0); },
			"zero-height Corridor was accepted");
		requireThrows([&]{ builder.addCorridor(0, 0, 1, 1); },
			"Corridor overlapping the Room was accepted");
		require(world.getNumSectors() == corridor + 1,
			"a refused Corridor add left a Sector behind");
		// The Layer-less form keeps the front-most Layer as its default.
		auto const corridorL1 = builder.addCorridor(1, 0, 0, 2, 1);
		require(world.getSector(corridorL1)->getLayerIndex() == 1,
			"layered Corridor add landed on the wrong Layer");

		// ---------------------------------------------------------------
		// Background: any Layer is legal, front-most and back-most
		// included; occupied cells are not (ADR 0002).
		// ---------------------------------------------------------------
		require(!builder.canAddBackground(0, 0, 0, 0, 1, &diagnostic)
			&& diagnostic.find("at least one cell wide") != std::string::npos,
			"zero-width Background was accepted");
		require(!builder.canAddBackground(0, 0, 0, 1, 0, &diagnostic)
			&& diagnostic.find("at least one level high") != std::string::npos,
			"zero-height Background was accepted");
		require(!builder.canAddBackground(99, 0, 0, 1, 1, &diagnostic),
			"Background on an unknown Layer was accepted");
		require(!builder.canAddBackground(0, 0, 0, 1, 1, &diagnostic)
			&& diagnostic.find("occupied") != std::string::npos,
			"Background over an occupied cell was accepted: " + diagnostic);
		require(builder.canAddBackground(1, 0, 3, 3, 2, &diagnostic),
			"Background on the back-most Layer was refused: " + diagnostic);
		{
			// Evaluate each verdict into a variable first: argument evaluation
			// order is unspecified and the shared diagnostic must not race.
			std::string facadeDiagnostic;
			auto const builderVerdict = builder.canAddBackground(0, 0, 5, 1, 1, &diagnostic);
			auto const facadeVerdict = world.canAddBackground(0, 0, 5, 1, 1, &facadeDiagnostic);
			require(builderVerdict == facadeVerdict,
				"facade disagrees with the builder on a Background verdict");
		}

		BackgroundColour const colour{ 10, 20, 30 };
		auto const bg = builder.addBackground(1, 0, 3, 3, 2, colour);
		require(world.getSector(bg)->getType() == SectorType::Background,
			"Background add did not produce a Background");
		auto background = std::dynamic_pointer_cast<const Background>(world.getSector(bg));
		require(background && background->getColour() == colour,
			"Background add lost its colour");
		auto const& bgCell = cell(1, 4, 1);
		require(bgCell.sectorIndex == bg
			&& bgCell.floorType == Background::cellFloorType()
			&& bgCell.sectorObjectType == SectorObjectType::None,
			"Background cells were not stamped floor-less and object-less");
		auto const sectorsAfterBg = world.getNumSectors();
		requireThrows([&]{ builder.addBackground(1, 1, 4, 1, 1, colour); },
			"Background over an existing Background was accepted");
		require(world.getNumSectors() == sectorsAfterBg,
			"a refused Background add left a Sector behind");

		// ---------------------------------------------------------------
		// Facade: placed exactly as a Room is (ADR 0003).
		// ---------------------------------------------------------------
		require(!builder.canAddFacade(0, 0, 6, 1, 1, nan, &diagnostic)
			&& diagnostic.find("out of range") != std::string::npos,
			"Facade with a NaN topLevelHeight was accepted");
		require(!builder.canAddFacade(0, 0, 6, 0, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic)
			&& diagnostic.find("at least one cell wide") != std::string::npos,
			"zero-width Facade was accepted");
		require(!builder.canAddFacade(0, 0, 6, 1, 0, CORE_ROOM_MAX_HEIGHT, &diagnostic)
			&& diagnostic.find("at least one level high") != std::string::npos,
			"zero-height Facade was accepted");
		require(!builder.canAddFacade(0, 0, 5, 1, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"Facade over the Corridor's cell was accepted");
		require(builder.canAddFacade(0, 0, 7, 2, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"valid Facade was refused: " + diagnostic);
		{
			std::string facadeDiagnostic;
			auto const builderVerdict = builder.canAddFacade(0, 0, 7, 2, 1,
				CORE_ROOM_MAX_HEIGHT, &diagnostic);
			auto const worldVerdict = world.canAddFacade(0, 0, 7, 2, 1,
				CORE_ROOM_MAX_HEIGHT, &facadeDiagnostic);
			require(builderVerdict == worldVerdict,
				"facade disagrees with the builder on a Facade verdict");
		}

		auto const facade = builder.addFacade(0, 0, 7, 2, 1, CORE_ROOM_MAX_HEIGHT,
			Facade::defaultColour());
		require(world.getSector(facade)->getType() == SectorType::Facade,
			"Facade add did not produce a Facade");
		require(world.getSector(facade)->getName() == Facade::defaultName(),
			"unnamed Facade add did not use the default name");
		auto const& facadeCell = cell(0, 7, 0);
		require(facadeCell.sectorIndex == facade && facadeCell.floorType == CellFloorType::Ground,
			"Facade bottom level was not stamped Ground");

		BackgroundColour const namedColour{ 200, 100, 50 };
		auto const namedFacade = builder.addFacade("Frontage", 1, 2, 0, 2, 1,
			CORE_ROOM_MAX_HEIGHT, namedColour);
		require(world.getSector(namedFacade)->getName() == "Frontage",
			"named Facade add lost its name");
		auto named = std::dynamic_pointer_cast<const Facade>(world.getSector(namedFacade));
		require(named && named->getColour() == namedColour,
			"named Facade add lost its colour");

		// ---------------------------------------------------------------
		// createLocation: the raw creation seam appends the Sector but
		// stamps no cells - that is addLocation's job.
		// ---------------------------------------------------------------
		auto const raw = builder.createLocation("Raw", SectorType::Location, 1, 9, 2,
			1, 1, CORE_ROOM_MAX_HEIGHT, false);
		require(raw == world.getNumSectors() - 1,
			"createLocation did not append the Sector last");
		require(cell(1, 9, 2).sectorIndex == ~0u,
			"createLocation stamped cells it should leave alone");
		builder.addLocation("Added", SectorType::Location, 1, 9, 2, 1, 1,
			CORE_ROOM_MAX_HEIGHT, false);
		require(cell(1, 9, 2).sectorIndex
			== world.getNumSectors() - 1,
			"addLocation did not stamp its cell with the new Sector");
		require(cell(1, 9, 2).floorType == CellFloorType::Ground,
			"addLocation did not give the bottom level a Ground floor");

		std::cout << "WorldBuilder Sectors checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "WorldBuilder Sectors checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
