#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>
#include "core/LiftTransit.h"
#include "core/DoorSectorObject.h"

namespace
{

	bool markerPlacementEnforcesPaletteCoreRules()
	{
		core::World world("Marker placement rules", 7, 3);
		auto room = world.addRoom("Marker room", 0, 0, 0, 6, 2);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canAddSectorMarker(room, 0, 2.5f, &diagnostic)
			|| world.canAddSectorMarker(room, 1, 2.5f, &diagnostic)) return false;

		bool runningRejected = false;
		try { world.addSectorMarker(room, 0, 2.5f); }
		catch (std::exception const&) { runningRejected = true; }
		if (!runningRejected || world.isTraversalTopologyDirty()) return false;

		world.pauseSimulation();
		auto created = world.addSectorMarker(room, 0, 2.5f);
		if (created.type != core::SectorObjectType::Marker || created.index == ~0u
			|| world.canAddSectorMarker(room, 0, 2.52f, &diagnostic)
			|| diagnostic.find("already exists") == std::string::npos) return false;

		bool duplicateRejected = false;
		try { world.addSectorMarker(room, 0, 2.52f); }
		catch (std::exception const&) { duplicateRejected = true; }
		return duplicateRejected && world.rebuildTraversalTopology()
			&& world.resumeSimulation() && !world.isSimulationPaused();
	}

	bool corridorDoorPlacementEnforcesPaletteRules()
	{
		// Every Location kind may host either side of a Door. Exercise all nine
		// front/back combinations so Room, Corridor, and Facade stay symmetric.
		for (int frontKind = 0; frontKind < 3; ++frontKind)
			for (int backKind = 0; backKind < 3; ++backKind)
			{
				core::World world("Location Door placement rules", 10, 4);
				auto addLocation = [&](int kind, uint32_t layer)
				{
					if (kind == 0) return world.addRoom("Room", layer, 0, 0, 6, 1);
					if (kind == 1) return world.addCorridor(layer, 0, 0, 6, 1);
					return world.addFacade(layer, 0, 0, 6, 1);
				};
				addLocation(frontKind, 0);
				addLocation(backKind, 1);

				std::string diagnostic;
				if (!world.canAddCorridorDoor(0, 0, 2, &diagnostic)) return false;
				auto door = world.addSectorDoor(0, 0, 2);
				world.finishBuild();
				if (door.door.type != core::SectorObjectType::Door
					|| !world.isTraversalTopologyValid()) return false;
			}

		core::World world("Door obstruction rules", 10, 4);
		auto frontRoom = world.addRoom("Front room", 0, 0, 0, 6, 2);
		auto backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		std::string diagnostic;
		world.addSectorMarker(frontRoom, 0, 4.5f);
		world.addSectorMarker(backRoom, 0, 0.5f);
		if (world.canAddCorridorDoor(0, 0, 4, &diagnostic)
			|| diagnostic.find("blocks") == std::string::npos
			|| world.canAddCorridorDoor(0, 1, 1, &diagnostic)
			|| diagnostic.find("behind") == std::string::npos) return false;
		return true;
	}

	bool objectMoveValidatesAndRebuildsOnceCommitted()
	{
		core::World world("Object movement", 10, 3);
		auto corridor = world.addCorridor(0, 0, 8);
		world.addRoom("Back room", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		auto created = world.addSectorDoor(0, 0, 1, doorOptions);
		world.addSectorMarker(corridor, 0, 5.5f);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", corridor, 0, 0.5f);

		auto outsideBothSectors = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 1, 1);
		auto blocked = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 5, 0);
		auto valid = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 3, 0);
		if (outsideBothSectors.valid || blocked.valid || !valid.valid) return false;

		auto moved = world.applyObjectMove(valid);
		if (!moved || moved->getObjectType() != core::SectorObjectType::Door
			|| moved->getCellX() != 3 || moved->getCellY() != 0
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid()) return false;
		auto doorOwner = moved->getSector();
		uint32_t movedDoorIndex = ~0u;
		for (uint32_t i = 0; i < doorOwner->getNumObjects(); ++i)
			if (doorOwner->getObject(i) == moved) { movedDoorIndex = i; break; }
		if (movedDoorIndex == ~0u
			|| !world.removeSectorDoor(doorOwner->getIndex(), movedDoorIndex)
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.getSimulationSnapshot().traversalResources.empty()) return false;
		for (auto const& sector : world.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Door) return false;

		core::World windowWorld("Window editing", 10, 3);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		windowWorld.addRoom("Back", 1, 0, 0, 8, 1);
		auto createdWindow = windowWorld.addSectorWindow(0, 0, 1, 1, 1, {});
		windowWorld.finishBuild();
		windowWorld.pauseSimulation();
		auto windowAgent = windowWorld.createAgent("Stationary", fore, 0, 0.5f);
		auto windowMove = windowWorld.planMoveSectorObject(
			createdWindow.window.sector->getIndex(), createdWindow.window.index, 3, 0);
		if (!windowMove.valid) return false;
		auto movedWindow = windowWorld.applyObjectMove(windowMove);
		if (!movedWindow || movedWindow->getObjectType() != core::SectorObjectType::Window
			|| movedWindow->getCellX() != 3) return false;
		auto owner = movedWindow->getSector();
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == movedWindow) { movedIndex = i; break; }
		if (movedIndex == ~0u || !windowWorld.removeSectorWindow(owner->getIndex(), movedIndex))
			return false;
		for (auto const& sector : windowWorld.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Window) return false;
		if (windowWorld.lookupAgent(windowAgent).entity == nullptr
			|| !windowWorld.isSimulationPaused() || !windowWorld.isTraversalTopologyValid()) return false;

		core::World pasteMoveWorld("Paste-style movement", 10, 2);
		pasteMoveWorld.addCorridor(0, 0, 4);
		auto right = pasteMoveWorld.addCorridor(0, 6, 4);
		pasteMoveWorld.addRoom("Left back", 1, 0, 0, 4, 1);
		pasteMoveWorld.addRoom("Right back", 1, 0, 6, 4, 1);
		auto crossSectorDoor = pasteMoveWorld.addSectorDoor(0, 0, 1);
		pasteMoveWorld.finishBuild();
		pasteMoveWorld.pauseSimulation();
		auto doorPlan = pasteMoveWorld.planMoveSectorObject(
			crossSectorDoor.door.sector->getIndex(), crossSectorDoor.door.index, 7, 0);
		if (!doorPlan.valid) return false;
		auto movedAcrossSectors = pasteMoveWorld.applyObjectMove(doorPlan);
		if (!movedAcrossSectors || movedAcrossSectors->getSector()->getIndex() != right) return false;

		core::World markerMoveWorld("Marker movement", 10, 1);
		auto markerLeft = markerMoveWorld.addCorridor(0, 0, 4);
		auto markerRight = markerMoveWorld.addCorridor(0, 6, 4);
		auto marker = markerMoveWorld.addSectorMarker(markerLeft, 0, 2.5f);
		markerMoveWorld.finishBuild();
		markerMoveWorld.pauseSimulation();
		auto markerPlan = markerMoveWorld.planMoveSectorObject(markerLeft, marker.index, 8, 0);
		if (!markerPlan.valid) return false;
		auto movedMarker = markerMoveWorld.applyObjectMove(markerPlan);
		return movedMarker && movedMarker->getSector()->getIndex() == markerRight
			&& movedMarker->getCellX() == 8;
	}

	bool windowResizeUsesWindowPlacementRules()
	{
		core::World world("Window resizing", 12, 3);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 3);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 3);
		world.addRoom("Behind", 1, 0, 0, 12, 3);
		core::World::CreateWindowOptions options;
		options.traversable = true;
		options.initialState = core::Window::State::Tinted;
		options.style = core::Window::Style::Tinted;
		// Palette placement creates a one-cell aperture; resizing does the rest.
		auto created = world.addSectorWindow(0, 0, 3, 1, 1, options);
		if (created.window.sector->getObject(created.window.index)->getSize()
			!= core::Vector2{ 1.0f, 1.0f }) return false;
		world.finishBuild();
		world.pauseSimulation();

		auto findWindowIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			auto owner = object->getSector();
			auto index = findWindowIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorWindow(
				owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.window.sector->getObject(created.window.index), 1, 0, 3, 1);
		if (!resized || resized->getCellX() != 1
			|| resized->getSize() != core::Vector2{ 3.0f, 1.0f }) return false;
		resized = resize(resized, 1, 0, 6, 1);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 0, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 0).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 8, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 12, 1).valid)
			return false;
		world.addSectorMarker(front, 0, 7.5f);
		if (world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 7, 1).valid)
			return false;

		resized = resize(resized, 1, 0, 6, 3);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 3.0f }) return false;
		std::string diagnostic;
		if (world.canAddSectorWindow(0, 2, 2, 1, 1, &diagnostic)) return false;
		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 4).valid)
			return false;
		resized = resize(resized, 1, 1, 6, 2);
		if (!resized || resized->getCellY() != 1
			|| resized->getSize() != core::Vector2{ 6.0f, 2.0f }) return false;

		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		world.addSectorMarker(front, 0, 2.5f);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 3).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateWindowOptions retained;
		return world.getSectorWindowOptions(0, 1, 1, 6, 2, retained)
			&& retained.traversable && retained.initialState == core::Window::State::Tinted
			&& retained.style == core::Window::Style::Tinted
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
	}

	bool doorResizeRespectsDoorPlacementRules()
	{
		core::World world("Door resizing", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 1);
		world.addRoom("Behind", 1, 0, 0, 12, 1);
		core::World::CreateDoorOptions options;
		options.controls[0] = true;
		options.controls[1] = true;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.holdOpenSeconds = 4.5f;
		// Palette placement creates a one-cell Door; resizing does the rest.
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", front, 0, 0.5f);

		auto findDoorIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height = 1)
		{
			auto owner = object->getSector();
			auto index = findDoorIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorDoor(owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.door.sector->getObject(created.door.index), 2, 0, 2);
		if (!resized || resized->getCellX() != 2
			|| resized->getSize() != core::Vector2{ 2.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findDoorIndex(owner, resized);
		if (index == ~0u) return false;
		// A Door is one or two cells wide, never zero and never three.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 0, 1).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 3, 1).valid)
			return false;
		// Door resizing remains horizontal; its grid footprint is always one level.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 0).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;
		// The span may not cross the front Sector boundary into the neighbour.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 7, 0, 2, 1).valid)
			return false;
		// Nor may it grow into a cell another object occupies.
		world.addSectorMarker(front, 0, 4.5f);
		if (world.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 2, 1).valid)
			return false;
		// These rooms are one level tall, so the Door cannot grow upward here.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateDoorOptions retained;
		if (!world.getSectorDoorOptions(0, 0, 2, 2, retained)
			|| retained.activationMode != core::DoorActivationMode::RemoteControlled
			|| retained.holdOpenSeconds != 4.5f
			|| !retained.controls[0] || !retained.controls[1]
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid())
			return false;

		// Authored crossing lanes outrank a narrower Door: shrinking below them
		// would silently drop capacity, so the plan refuses.
		core::World laneWorld("Lane door resizing", 8, 2);
		laneWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		laneWorld.addRoom("Aft", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions lanes;
		lanes.width = 2;
		lanes.crossingLanes = 2;
		auto laneDoor = laneWorld.addSectorDoor(0, 0, 3, lanes);
		laneWorld.finishBuild();
		laneWorld.pauseSimulation();
		owner = laneDoor.door.sector;
		index = findDoorIndex(owner, laneDoor.door.sector->getObject(laneDoor.door.index));
		if (index == ~0u
			|| laneWorld.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 1, 1).valid)
			return false;

		// Lift landing doors belong to the transport and refuse to resize.
		core::World liftWorld("Lift door resizing", 10, 8);
		liftWorld.addCorridor(1, 0, 8);
		liftWorld.addCorridor(4, 0, 8);
		auto lift = liftWorld.addLift(1, 0, 2, 2, 6);
		liftWorld.finishBuild();
		liftWorld.pauseSimulation();
		if (lift.doors.empty()) return false;
		auto landingDoor = lift.doors[0].door.sector->getObject(lift.doors[0].door.index);
		if (!liftWorld.isLiftOwnedDoor(landingDoor)) return false;
		owner = landingDoor->getSector();
		index = findDoorIndex(owner, landingDoor);
		if (index == ~0u
			|| liftWorld.planResizeSectorDoor(owner->getIndex(), index,
				landingDoor->getCellX(), landingDoor->getCellY(), 1, 1).valid)
			return false;
		return true;
	}
}

void registerObjectEditing(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "markerPlacementEnforcesPaletteCoreRules", [](smoke::Context const&)
		{
			smoke::require(markerPlacementEnforcesPaletteCoreRules(), "Marker placement did not enforce core viability rules");
		} });
	checks.push_back({ "corridorDoorPlacementEnforcesPaletteRules", [](smoke::Context const&)
		{
			smoke::require(corridorDoorPlacementEnforcesPaletteRules(), "Door placement did not enforce Location or obstruction rules");
		} });
	checks.push_back({ "objectMoveValidatesAndRebuildsOnceCommitted", [](smoke::Context const&)
		{
			smoke::require(objectMoveValidatesAndRebuildsOnceCommitted(), "Object movement did not validate and rebuild atomically");
		} });
	checks.push_back({ "windowResizeUsesWindowPlacementRules", [](smoke::Context const&)
		{
			smoke::require(windowResizeUsesWindowPlacementRules(), "Window resizing did not preserve options or placement rules");
		} });
	checks.push_back({ "doorResizeRespectsDoorPlacementRules", [](smoke::Context const&)
		{
			smoke::require(doorResizeRespectsDoorPlacementRules(), "Door resizing did not preserve options or placement rules");
		} });
}
