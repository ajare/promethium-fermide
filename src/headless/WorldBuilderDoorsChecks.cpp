#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/SectorObjectType.h"
#include "core/World.h"
#include "core/WorldBuilder.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("WorldBuilder Doors check failed: " + what);
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
		throw std::runtime_error("WorldBuilder Doors check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The builder is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern for the Door authoring
		// family. World keeps the state it edits (ADR 0001) - the
		// Sectors, Layers, cells, traversal resources, and construction
		// records; the builder holds only a reference.
		World world("wb-doors-checks", 12, 3);
		WorldBuilder builder(world);
		World const& view = world;
		auto cell = [&view](uint32_t layer, uint32_t x, uint32_t y) -> CellDefinition const&
		{
			return view.getLayer(layer)->getCellDefinition(x, y);
		};

		std::string diagnostic;

		// Two Location pairs stacked front/back: the ordinary Door pair,
		// and an adjacent pair on the front Layer for the Bulkhead Door.
		auto const roomA = builder.addRoom("Room A", 0, 0, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		auto const roomB = builder.addRoom("Room B", 0, 0, 3, 3, 1, CORE_ROOM_MAX_HEIGHT);
		auto const roomC = builder.addRoom("Room C", 1, 0, 0, 3, 1, CORE_ROOM_MAX_HEIGHT);
		auto const roomD = builder.addRoom("Room D", 1, 0, 3, 3, 1, CORE_ROOM_MAX_HEIGHT);
		(void)roomB; (void)roomC; (void)roomD;

		// ---------------------------------------------------------------
		// canAddCorridorDoor: judged before anything is written.
		// ---------------------------------------------------------------
		require(builder.canAddCorridorDoor(0, 0, 1, &diagnostic),
			"valid ordinary Door was refused: " + diagnostic);
		{
			// Facade agreement: the World facade and the builder must
			// return the same verdict.
			std::string worldDiagnostic;
			auto const builderVerdict = builder.canAddCorridorDoor(0, 0, 1, &diagnostic);
			auto const worldVerdict = world.canAddCorridorDoor(0, 0, 1, &worldDiagnostic);
			require(builderVerdict == worldVerdict,
				"facade disagrees with the builder on a Door verdict");
		}
		require(!builder.canAddCorridorDoor(1, 0, 1, &diagnostic)
			&& diagnostic.find("Layer directly behind") != std::string::npos,
			"Door on the back-most Layer was accepted: " + diagnostic);
		require(!builder.canAddCorridorDoor(0, 0, 9, &diagnostic),
			"Door over an unoccupied cell was accepted: " + diagnostic);
		{
			World::CreateDoorOptions manualWithControls;
			manualWithControls.activationMode = DoorActivationMode::Manual;
			manualWithControls.controls[0] = true;
			require(!builder.canAddCorridorDoor(0, 0, 1, manualWithControls, &diagnostic)
				&& diagnostic.find("remote-controlled") != std::string::npos,
				"Manual Door with physical controls was accepted: " + diagnostic);
		}

		// A refusal leaves nothing behind.
		auto const sectorsBeforeAdd = world.getNumSectors();
		requireThrows([&]{
			World::CreateDoorOptions bad;
			bad.width = 0;
			builder.addSectorDoor(0, 0, 1, bad);
		}, "zero-width Door add was accepted");
		require(world.getNumSectors() == sectorsBeforeAdd,
			"a refused Door add left a Sector behind");
		require(cell(0, 1, 0).sectorObjectType == SectorObjectType::None,
			"a refused Door add stamped a cell");

		// ---------------------------------------------------------------
		// addSectorDoor: the plain manual Door.
		// ---------------------------------------------------------------
		auto const plain = builder.addSectorDoor(0, 0, 1);
		require(plain.door.type == SectorObjectType::Door,
			"Door add did not produce a Door object");
		require(cell(0, 1, 0).sectorObjectType == SectorObjectType::Door,
			"Door was not stamped on the authored Layer's cell");
		require(plain.traversalResource != TraversalResourceId{},
			"Door add did not create a traversal resource");
		auto doorObject = std::dynamic_pointer_cast<DoorSectorObject>(
			plain.door.sector->getObject(plain.door.index));
		require(doorObject && doorObject->getDoor(), "Door object is not a DoorSectorObject");
		auto door = doorObject->getDoor();
		require(door->getTraversalResourceId() == plain.traversalResource,
			"Door and result disagree on the traversal resource");
		require(door->getActivationMode() == DoorActivationMode::Manual,
			"plain Door did not keep Manual activation");
		// The back Sector registers the shared Door but its Layer keeps its
		// own object occupancy: the back Layer's cell stays object-less.
		require(cell(1, 1, 0).sectorObjectType == SectorObjectType::None,
			"the back Layer's cell was claimed by the shared Door");

		// A second Door cannot reuse the threshold cell.
		requireThrows([&]{ builder.addSectorDoor(0, 0, 1); },
			"a second Door on an occupied threshold was accepted");

		// ---------------------------------------------------------------
		// addSectorDoorButton: both sides gain Buttons, mode flips to
		// RemoteControlled, and the pre-Button mode is recorded.
		// ---------------------------------------------------------------
		require(builder.canAddSectorDoorButton(roomA, plain.door.index),
			"canAddSectorDoorButton refused a Button-less Door");
		builder.addSectorDoorButton(roomA, plain.door.index);
		require(door->getActivationMode() == DoorActivationMode::RemoteControlled,
			"adding Buttons did not switch the Door to RemoteControlled");
		require(!builder.canAddSectorDoorButton(roomA, plain.door.index),
			"canAddSectorDoorButton accepted a Door with Buttons on both sides");
		require(builder.canRemoveSectorDoorButton(roomA, plain.door.index),
			"canRemoveSectorDoorButton refused a Buttoned Door");
		// The back Sector sees the same Door: find it in the back
		// Sector's own object list.
		{
			auto const backSector = world.getSector(roomC);
			uint32_t backIndex = ~0u;
			for (uint32_t i = 0; i < backSector->getNumObjects(); ++i)
				if (std::dynamic_pointer_cast<const DoorSectorObject>(backSector->getObject(i)))
				{ backIndex = i; break; }
			require(backIndex != ~0u && builder.canRemoveSectorDoorButton(roomC, backIndex),
				"canRemoveSectorDoorButton refused through the back Sector view");
		}

		// Transport-owned guards and selection errors refuse before mutation.
		requireThrows([&]{ builder.addSectorDoorButton(roomA, 0); },
			"Button add on a non-Door object was accepted");
		requireThrows([&]{ builder.addSectorDoorButton(999, 0); },
			"Button add on an unknown Sector was accepted");

		// ---------------------------------------------------------------
		// removeSectorDoorButton: refuses unless paused, then rebuilds.
		// ---------------------------------------------------------------
		requireThrows([&]{ builder.removeSectorDoorButton(roomA, plain.door.index); },
			"Button removal was accepted with the simulation running");
		world.pauseSimulation();
		auto const rebuilt = builder.removeSectorDoorButton(roomA, plain.door.index);
		require(rebuilt && rebuilt->getDoor(),
			"Button removal did not return the rebuilt Door");
		require(rebuilt->getDoor()->getActivationMode() == DoorActivationMode::Manual,
			"Button removal did not restore the pre-Button activation mode");
		// The rebuild replaced the object; re-read its index from the front cell.
		auto const rebuiltIndex = cell(0, 1, 0).sectorObjectIndex;
		require(!builder.canRemoveSectorDoorButton(roomA, rebuiltIndex),
			"canRemoveSectorDoorButton accepted a Door with no Buttons left");
		requireThrows([&]{ builder.removeSectorDoorButton(roomA, rebuiltIndex); },
			"removing Buttons from a Button-less Door was accepted");
		world.resumeSimulation();

		// ---------------------------------------------------------------
		// Bulkhead Doors. The build has finished by now, so structural
		// edits require the simulation to be paused.
		// ---------------------------------------------------------------
		world.pauseSimulation();
		World::CreateBulkheadDoorOptions bhOptions;
		require(builder.canAddSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, bhOptions, &diagnostic),
			"valid Bulkhead Door was refused: " + diagnostic);
		require(!builder.canAddSectorBulkheadDoor(0, 0, 1, CORE_SIDE_LEFT, bhOptions, &diagnostic)
			&& diagnostic.find("distinct") != std::string::npos,
			"Bulkhead Door inside one Location was accepted: " + diagnostic);
		require(!builder.canAddSectorBulkheadDoor(0, 0, 3, CORE_SIDE_MIDDLE, bhOptions, &diagnostic),
			"Bulkhead Door with an invalid side was accepted");
		{
			World::CreateBulkheadDoorOptions twoLanes;
			twoLanes.crossingLanes = 2;
			require(!builder.canAddSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, twoLanes, &diagnostic)
				&& diagnostic.find("exactly one crossing lane") != std::string::npos,
				"a two-lane Bulkhead Door was accepted: " + diagnostic);
		}
		{
			World::CreateBulkheadDoorOptions nanHold;
			nanHold.holdOpenSeconds = std::numeric_limits<float>::quiet_NaN();
			require(!builder.canAddSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, nanHold, &diagnostic),
				"a Bulkhead Door with NaN hold-open time was accepted");
		}

		auto const bulkhead = builder.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT);
		require(bulkhead.door.type == SectorObjectType::BulkheadDoor,
			"Bulkhead add did not produce a BulkheadDoor object");
		require(bulkhead.traversalResource != TraversalResourceId{},
			"Bulkhead add did not create a traversal resource");
		auto bhObject = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(
			bulkhead.door.sector->getObject(bulkhead.door.index));
		require(bhObject && bhObject->getDoor(),
			"Bulkhead object is not a BulkheadDoorSectorObject");
		require(bhObject->getDoor()->getActivationMode() == DoorActivationMode::RemoteControlled,
			"Bulkhead Door did not keep its default RemoteControlled activation");
		// The boundary cells now carry the Bulkhead indices.
		require(cell(0, 2, 0).bulkheadIndices[CORE_SIDE_RIGHT] == bulkhead.door.index
			&& cell(0, 3, 0).bulkheadIndices[CORE_SIDE_LEFT] == bulkhead.door.index,
			"Bulkhead boundary cells were not registered");
		// The same boundary cannot take a second Bulkhead Door.
		requireThrows([&]{ builder.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT); },
			"a second Bulkhead Door on the same boundary was accepted");

		// A Facade has no wall ends (ADR 0003).
		auto const facade = builder.addFacade(0, 0, 6, 2, 1, CORE_ROOM_MAX_HEIGHT,
			Facade::defaultColour());
		(void)facade;
		require(!builder.canAddSectorBulkheadDoor(0, 0, 6, CORE_SIDE_LEFT, bhOptions, &diagnostic)
			&& diagnostic.find("Facade") != std::string::npos,
			"Bulkhead Door against a Facade was accepted: " + diagnostic);

		// ---------------------------------------------------------------
		// createDoor / createBulkheadDoor: the raw seams refuse on their
		// own cell checks but perform no add-level judgement.
		// ---------------------------------------------------------------
		requireThrows([&]{ builder.createDoor(0, 9, 0, 1); },
			"createDoor over an unoccupied cell was accepted");
		requireThrows([&]{ builder.createBulkheadDoor(0, 9, 0, CORE_SIDE_LEFT); },
			"createBulkheadDoor over an unoccupied cell was accepted");
		world.resumeSimulation();

		std::cout << "WorldBuilder Doors checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "WorldBuilder Doors checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
