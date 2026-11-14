#include "game/WorldAuthoring.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Log.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"
#include "game/MovedFrom.h"

namespace
{
	// A handle that has been moved from names no runtime and owns no history.
	// Every accessor that needs one funnels through `game::requireLive`, so
	// the moved-from case fails with the accessor named instead of
	// dereferencing a null pointer and reporting nothing about which handle or
	// which call went wrong (#978). The helper is shared with `WorldRuntime`
	// so the two halves of the boundary raise the same exception with the same
	// shape of message, and one grep finds both. The string is built when the
	// throw happens, not on every call.
	using game::requireLive;

	// The work data every restore of this document needs, read off the World it
	// is being restored into. The Furniture catalogue and the accepted Action
	// package are not part of the snapshot - history holds the document, not
	// the packages it depends on - so a restore resolves them from the live
	// World, the same way the editor's undo does (#968).
	core::SerializationWorkData makeRestoreWorkData(core::World const& world)
	{
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		workData.furnitureCatalogue = world.furnitureCatalogue();
		workData.actionRegistry = world.actionRegistry();
		return workData;
}

	std::string restoreFailure(std::exception const& error)
	{
		return "Could not restore the authored document: " + std::string(error.what());
	}
}

namespace game
{
	WorldAuthoring::WorldAuthoring(WorldRuntime& runtime) noexcept
		: runtime_(&runtime)
		, history_(std::make_unique<DocumentHistory>())
	{
	}

	WorldAuthoring::~WorldAuthoring() = default;

	WorldAuthoring::WorldAuthoring(WorldAuthoring&& other) noexcept
		: runtime_(other.runtime_)
		, history_(std::move(other.history_))
	{
		other.runtime_ = nullptr;
	}

	WorldAuthoring& WorldAuthoring::operator=(WorldAuthoring&& other) noexcept
	{
		if (this != &other)
		{
			runtime_ = other.runtime_;
			history_ = std::move(other.history_);
			other.runtime_ = nullptr;
		}
		return *this;
	}

	core::World& WorldAuthoring::world()
	{
		// The one place the mutable World is taken. `WorldRuntime::authoringWorld`
		// is private and granted to this class alone, so the privilege does not
		// come from a cast that any caller could repeat (#967).
		return requireLive(runtime_, "WorldAuthoring::world").authoringWorld();
	}

	WorldRuntime& WorldAuthoring::runtime()
	{
		return requireLive(runtime_, "WorldAuthoring::runtime");
	}

	WorldRuntime const& WorldAuthoring::runtime() const
	{
		return requireLive(runtime_, "WorldAuthoring::runtime");
	}

	core::World const& WorldAuthoring::document() const
	{
		// The read view, taken from the runtime's own public const accessor
		// rather than from the privileged one. Preflight and capture are reads,
		// so they do not spend the mutable-view privilege (#968).
		return requireLive(runtime_, "WorldAuthoring::document").world();
	}

// --- Levels and Layers ---
uint32_t WorldAuthoring::addLayer()
{
	return world().addLayer();
}
void WorldAuthoring::addLevel()
{
	world().addLevel();
}
void WorldAuthoring::setLayerName(uint32_t layerIndex, std::string name)
{
	world().setLayerName(layerIndex, std::move(name));
}
void WorldAuthoring::setLevelName(uint32_t level, std::string name)
{
	world().setLevelName(level, std::move(name));
}
core::World::LevelDeletePlan WorldAuthoring::planDeleteLevel(uint32_t level) const
{
	return document().planDeleteLevel(level);
}
bool WorldAuthoring::applyDeleteLevel(core::World::LevelDeletePlan const& plan)
{
	return world().applyDeleteLevel(plan);
}
core::World::LayerDeletePlan WorldAuthoring::planDeleteLayer(uint32_t layerIndex) const
{
	return document().planDeleteLayer(layerIndex);
}
bool WorldAuthoring::applyDeleteLayer(core::World::LayerDeletePlan const& plan)
{
	return world().applyDeleteLayer(plan);
}

// --- Locations ---
uint32_t WorldAuthoring::addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
{
	// The World carries a (y, x, cellsWide, levelsHigh) form beside the
	// (layerIndex, y, x, cellsWide, levelsHigh) one, and with every parameter a
	// uint32_t a four-argument call names both. The cast picks the form this
	// overload mirrors rather than leaving the choice to overload resolution.
	auto const form = static_cast<std::uint32_t (core::World::*)(
		std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>(&core::World::addCorridor);
	return (world().*form)(y, x, cellsWide, levelsHigh);
}
uint32_t WorldAuthoring::addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
{
	return world().addCorridor(layerIndex, y, x, cellsWide, levelsHigh);
}
uint32_t WorldAuthoring::addRoom(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight)
{
	return world().addRoom(name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight);
}
bool WorldAuthoring::canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, std::string* diagnostic) const
{
	return document().canAddBackground(layerIndex, y, x, cellsWide, levelsHigh, diagnostic);
}
uint32_t WorldAuthoring::addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, core::BackgroundColour const& colour)
{
	return world().addBackground(layerIndex, y, x, cellsWide, levelsHigh, colour);
}
bool WorldAuthoring::canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, std::string* diagnostic) const
{
	return document().canAddFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, diagnostic);
}
uint32_t WorldAuthoring::addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, core::BackgroundColour const& colour)
{
	return world().addFacade(name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, colour);
}
uint32_t WorldAuthoring::addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, core::BackgroundColour const& colour)
{
	return world().addFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, colour);
}
core::World::LocationEditPlan WorldAuthoring::planResizeLocation(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeLocation(sectorIndex, x, y, cellsWide, levelsHigh);
}
core::World::LocationEditPlan WorldAuthoring::planRemoveLocation(uint32_t sectorIndex) const
{
	return document().planRemoveLocation(sectorIndex);
}
core::World::LocationEditPlan WorldAuthoring::planResizeFacade(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeFacade(sectorIndex, x, y, cellsWide, levelsHigh);
}
core::World::LocationEditPlan WorldAuthoring::planRemoveFacade(uint32_t sectorIndex) const
{
	return document().planRemoveFacade(sectorIndex);
}
uint32_t WorldAuthoring::applyLocationEdit(core::World::LocationEditPlan const& plan)
{
	return world().applyLocationEdit(plan);
}
core::World::LocationEditPlan WorldAuthoring::planResizeBackground(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeBackground(sectorIndex, x, y, cellsWide, levelsHigh);
}
core::World::LocationEditPlan WorldAuthoring::planRemoveBackground(uint32_t sectorIndex) const
{
	return document().planRemoveBackground(sectorIndex);
}
uint32_t WorldAuthoring::applyBackgroundEdit(core::World::LocationEditPlan const& plan)
{
	return world().applyBackgroundEdit(plan);
}

// --- Location walls ---
bool WorldAuthoring::canAddLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side, std::string* diagnostic) const
{
	return document().canAddLocationWall(sectorIndex, levelIndex, side, diagnostic);
}
void WorldAuthoring::addLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side)
{
	world().addLocationWall(sectorIndex, levelIndex, side);
}
bool WorldAuthoring::canRemoveLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side, std::string* diagnostic) const
{
	return document().canRemoveLocationWall(sectorIndex, levelIndex, side, diagnostic);
}
void WorldAuthoring::removeLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side)
{
	world().removeLocationWall(sectorIndex, levelIndex, side);
}

// --- Ladders ---
bool WorldAuthoring::canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, std::string* diagnostic) const
{
	return document().canAddLadder(layerIndex, y, x, levelsHigh, diagnostic);
}
core::World::CreateLadderResult WorldAuthoring::addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateLadderOptions const& options)
{
	return world().addLadder(layerIndex, y, x, options);
}
core::World::CreateLadderResult WorldAuthoring::addSectorLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLadderOptions const& options)
{
	return world().addSectorLadder(sectorIndex, levelIndex, xOffset, options);
}
bool WorldAuthoring::canAddRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, uint32_t* levelsHigh, std::string* diagnostic) const
{
	return document().canAddRoomLadder(sectorIndex, levelIndex, xOffset, levelsHigh, diagnostic);
}
core::World::CreateLadderResult WorldAuthoring::addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset)
{
	return world().addRoomLadder(sectorIndex, levelIndex, xOffset);
}
core::World::CreateLadderResult WorldAuthoring::addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLadderOptions options)
{
	return world().addRoomLadder(sectorIndex, levelIndex, xOffset, options);
}
std::shared_ptr<const core::SectorObject> WorldAuthoring::applyRoomLadderOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateLadderOptions const& options)
{
	return world().applyRoomLadderOptions(sectorIndex, objectIndex, options);
}
bool WorldAuthoring::removeRoomLadder(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeRoomLadder(sectorIndex, objectIndex);
}
core::World::LadderEditPlan WorldAuthoring::planResizeLadder(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateLadderOptions const& options) const
{
	return document().planResizeLadder(sectorIndex, x, y, options);
}
core::World::LadderEditPlan WorldAuthoring::planRemoveLadder(uint32_t sectorIndex) const
{
	return document().planRemoveLadder(sectorIndex);
}
uint32_t WorldAuthoring::applyLadderEdit(core::World::LadderEditPlan const& plan)
{
	return world().applyLadderEdit(plan);
}

// --- Stairwells and staircases ---
bool WorldAuthoring::canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, std::string* diagnostic) const
{
	return document().canAddStairwell(layerIndex, y, x, levelsHigh, diagnostic);
}
uint32_t WorldAuthoring::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, int mountSide)
{
	return world().addStairwell(layerIndex, y, x, levelsHigh, mountSide);
}
core::World::CreateStairwellResult WorldAuthoring::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateStairwellOptions const& options)
{
	return world().addStairwell(layerIndex, y, x, options);
}
bool WorldAuthoring::canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide, std::string* diagnostic) const
{
	return document().canAddStaircase(layerIndex, y, x, cellsWide, riseSide, diagnostic);
}
uint32_t WorldAuthoring::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide, float speed)
{
	return world().addStaircase(layerIndex, y, x, cellsWide, riseSide, speed);
}
uint32_t WorldAuthoring::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateStaircaseOptions const& options)
{
	return world().addStaircase(layerIndex, y, x, options);
}
core::World::StairwellEditPlan WorldAuthoring::planResizeStairwell(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateStairwellOptions const& options) const
{
	return document().planResizeStairwell(sectorIndex, x, y, options);
}
core::World::StairwellEditPlan WorldAuthoring::planRemoveStairwell(uint32_t sectorIndex) const
{
	return document().planRemoveStairwell(sectorIndex);
}
uint32_t WorldAuthoring::applyStairwellEdit(core::World::StairwellEditPlan const& plan)
{
	return world().applyStairwellEdit(plan);
}
core::World::StaircaseEditPlan WorldAuthoring::planResizeStaircase(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateStaircaseOptions const& options) const
{
	return document().planResizeStaircase(sectorIndex, x, y, options);
}
core::World::StaircaseEditPlan WorldAuthoring::planRemoveStaircase(uint32_t sectorIndex) const
{
	return document().planRemoveStaircase(sectorIndex);
}
uint32_t WorldAuthoring::applyStaircaseEdit(core::World::StaircaseEditPlan const& plan)
{
	return world().applyStaircaseEdit(plan);
}

// --- Lifts ---
core::World::CreateLiftResult WorldAuthoring::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateLiftOptions const& options)
{
	return world().addLift(layerIndex, y, x, options);
}
core::World::CreateLiftResult WorldAuthoring::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
{
	return world().addLift(layerIndex, y, x, cellsWide, levelsHigh);
}
bool WorldAuthoring::canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset, core::World::CreateLiftOptions const& options, std::string* diagnostic) const
{
	return document().canAddPlatformLift(sectorIndex, xOffset, options, diagnostic);
}
core::World::CreatePlatformLiftResult WorldAuthoring::addSectorPlatformLift(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLiftOptions const& options)
{
	return world().addSectorPlatformLift(sectorIndex, levelIndex, xOffset, options);
}
core::World::PlatformLiftEditPlan WorldAuthoring::planPlatformLiftEdit(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateLiftOptions const& options) const
{
	return document().planPlatformLiftEdit(sectorIndex, objectIndex, options);
}
core::World::PlatformLiftEditPlan WorldAuthoring::planRemovePlatformLift(uint32_t sectorIndex, uint32_t objectIndex) const
{
	return document().planRemovePlatformLift(sectorIndex, objectIndex);
}
std::shared_ptr<const core::SectorObject> WorldAuthoring::applyPlatformLiftEdit(core::World::PlatformLiftEditPlan const& plan)
{
	return world().applyPlatformLiftEdit(plan);
}
core::World::LiftEditPlan WorldAuthoring::planResizeLift(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeLift(sectorIndex, x, y, cellsWide, levelsHigh);
}
core::World::LiftEditPlan WorldAuthoring::planRemoveLift(uint32_t sectorIndex) const
{
	return document().planRemoveLift(sectorIndex);
}
core::World::LiftEditPlan WorldAuthoring::planRemoveLiftStop(uint32_t sectorIndex, uint32_t stopIndex) const
{
	return document().planRemoveLiftStop(sectorIndex, stopIndex);
}
uint32_t WorldAuthoring::applyLiftEdit(core::World::LiftEditPlan const& plan)
{
	return world().applyLiftEdit(plan);
}

// --- Shuttles ---
core::World::CreateShuttleResult WorldAuthoring::addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, core::World::CreateShuttleOptions const& options)
{
	return world().addShuttle(layerIndex, y, x, cellsWide, options);
}
core::World::ShuttleEditPlan WorldAuthoring::planResizeShuttle(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide) const
{
	return document().planResizeShuttle(sectorIndex, x, y, cellsWide);
}
core::World::ShuttleEditPlan WorldAuthoring::planEditShuttleVehicle(uint32_t sectorIndex, uint32_t numCars, uint32_t carWidth, uint32_t doorMask) const
{
	return document().planEditShuttleVehicle(sectorIndex, numCars, carWidth, doorMask);
}
core::World::ShuttleEditPlan WorldAuthoring::planRemoveShuttle(uint32_t sectorIndex) const
{
	return document().planRemoveShuttle(sectorIndex);
}
core::World::ShuttleEditPlan WorldAuthoring::planRemoveShuttleStop(uint32_t sectorIndex, uint32_t stopIndex) const
{
	return document().planRemoveShuttleStop(sectorIndex, stopIndex);
}
core::World::ShuttleEditPlan WorldAuthoring::planAddShuttleStop(uint32_t sectorIndex, uint32_t stopOffset) const
{
	return document().planAddShuttleStop(sectorIndex, stopOffset);
}
uint32_t WorldAuthoring::applyShuttleEdit(core::World::ShuttleEditPlan const& plan)
{
	return world().applyShuttleEdit(plan);
}

// --- Airlocks ---
bool WorldAuthoring::canAddAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float cycleSeconds, std::string* diagnostic) const
{
	return document().canAddAirlock(layer, y, x, width, cycleSeconds, diagnostic);
}
uint32_t WorldAuthoring::addAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float cycleSeconds)
{
	return world().addAirlock(layer, y, x, width, cycleSeconds);
}
core::World::AirlockEditPlan WorldAuthoring::planResizeAirlock(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t width) const
{
	return document().planResizeAirlock(sectorIndex, x, y, width);
}
core::World::AirlockEditPlan WorldAuthoring::planRemoveAirlock(uint32_t sectorIndex) const
{
	return document().planRemoveAirlock(sectorIndex);
}
uint32_t WorldAuthoring::applyAirlockEdit(core::World::AirlockEditPlan const& plan)
{
	return world().applyAirlockEdit(plan);
}

// --- Chambers ---
bool WorldAuthoring::canAddChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, std::string* diagnostic, core::ChamberSubtype subtype) const
{
	return document().canAddChamber(layer, y, x, width, diagnostic, subtype);
}
uint32_t WorldAuthoring::addChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, bool leftToRight, core::ChamberSubtype subtype)
{
	return world().addChamber(layer, y, x, width, leftToRight, subtype);
}
core::World::ChamberEditPlan WorldAuthoring::planResizeChamber(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t width, bool leftToRight) const
{
	return document().planResizeChamber(sectorIndex, x, y, width, leftToRight);
}
core::World::ChamberEditPlan WorldAuthoring::planRemoveChamber(uint32_t sectorIndex) const
{
	return document().planRemoveChamber(sectorIndex);
}
core::World::ChamberEditPlan WorldAuthoring::planSetChamberSubtype(uint32_t sectorIndex, core::ChamberSubtype subtype) const
{
	return document().planSetChamberSubtype(sectorIndex, subtype);
}
uint32_t WorldAuthoring::applyChamberEdit(core::World::ChamberEditPlan const& plan)
{
	return world().applyChamberEdit(plan);
}

// --- Access panels ---
bool WorldAuthoring::canAddAccessPanel(uint32_t sectorIndex, uint32_t levelOffset,
	uint32_t cellX, core::AccessPanelGeometry geometry,
	std::string* diagnostic) const
{
	return document().canAddAccessPanel(sectorIndex, levelOffset, cellX, geometry,
		diagnostic);
}
core::World::CreateObjectResult WorldAuthoring::addAccessPanel(uint32_t sectorIndex,
	uint32_t levelOffset, uint32_t cellX, core::AccessPanelGeometry geometry,
	std::optional<float> speed)
{
	return world().addAccessPanel(sectorIndex, levelOffset, cellX, geometry, speed);
}
bool WorldAuthoring::removeAccessPanel(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeAccessPanel(sectorIndex, objectIndex);
}

// --- Doors ---
bool WorldAuthoring::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, std::string* diagnostic) const
{
	return document().canAddCorridorDoor(layerIndex, y, x, diagnostic);
}
bool WorldAuthoring::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateDoorOptions const& options, std::string* diagnostic) const
{
	return document().canAddCorridorDoor(layerIndex, y, x, options, diagnostic);
}
core::World::CreateDoorResult WorldAuthoring::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x)
{
	return world().addSectorDoor(layerIndex, y, x);
}
core::World::CreateDoorResult WorldAuthoring::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateDoorOptions const& options)
{
	return world().addSectorDoor(layerIndex, y, x, options);
}
bool WorldAuthoring::canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
{
	return document().canAddSectorDoorButton(sectorIndex, objectIndex);
}
void WorldAuthoring::addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex)
{
	world().addSectorDoorButton(sectorIndex, objectIndex);
}
bool WorldAuthoring::canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
{
	return document().canRemoveSectorDoorButton(sectorIndex, objectIndex);
}
std::shared_ptr<const core::DoorSectorObject> WorldAuthoring::removeSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex, std::optional<std::vector<core::AccessPermissionId>> resultingRequirement)
{
	return world().removeSectorDoorButton(sectorIndex, objectIndex, resultingRequirement);
}
bool WorldAuthoring::removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeSectorDoor(sectorIndex, objectIndex);
}

// --- Windows ---
bool WorldAuthoring::canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, std::string* diagnostic) const
{
	return document().canAddSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, diagnostic);
}
uint32_t WorldAuthoring::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
{
	return world().addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh);
}
core::World::CreateWindowResult WorldAuthoring::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, core::World::CreateWindowOptions const& options)
{
	return world().addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, options);
}
bool WorldAuthoring::removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeSectorWindow(sectorIndex, objectIndex);
}
bool WorldAuthoring::canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, uint32_t height, std::string* diagnostic) const
{
	return document().canAddBoothWindow(layer, y, x, width, height, diagnostic);
}
core::World::CreateWindowResult WorldAuthoring::addBoothWindow(uint32_t layer, uint32_t y, uint32_t x, core::Window::State initialState)
{
	return world().addBoothWindow(layer, y, x, initialState);
}

// --- Bulkhead doors ---
bool WorldAuthoring::canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side, core::World::CreateBulkheadDoorOptions const& options, std::string* diagnostic) const
{
	return document().canAddSectorBulkheadDoor(layerIndex, y, x, side, options, diagnostic);
}
core::World::CreateBulkheadDoorResult WorldAuthoring::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side)
{
	return world().addSectorBulkheadDoor(layerIndex, y, x, side);
}
core::World::CreateBulkheadDoorResult WorldAuthoring::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side, core::World::CreateBulkheadDoorOptions const& options)
{
	return world().addSectorBulkheadDoor(layerIndex, y, x, side, options);
}
std::shared_ptr<const core::SectorObject> WorldAuthoring::applySectorBulkheadDoorOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateBulkheadDoorOptions const& options)
{
	return world().applySectorBulkheadDoorOptions(sectorIndex, objectIndex, options);
}
bool WorldAuthoring::removeSectorBulkheadDoor(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeSectorBulkheadDoor(sectorIndex, objectIndex);
}

// --- Switches and force bridges ---
core::World::CreateObjectResult WorldAuthoring::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
{
	return world().addSectorLightSwitch(sectorIndex, xOffset);
}
bool WorldAuthoring::canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateForceBridgeOptions const& options, std::string* diagnostic) const
{
	return document().canAddSectorForceBridge(sectorIndex, levelIndex, xOffset, options, diagnostic);
}
core::World::CreateForceBridgeResult WorldAuthoring::addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset)
{
	return world().addSectorForceBridge(sectorIndex, levelIndex, xOffset);
}
core::World::CreateForceBridgeResult WorldAuthoring::addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateForceBridgeOptions const& options)
{
	return world().addSectorForceBridge(sectorIndex, levelIndex, xOffset, options);
}
std::shared_ptr<const core::SectorObject> WorldAuthoring::applySectorForceBridgeOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateForceBridgeOptions const& options)
{
	return world().applySectorForceBridgeOptions(sectorIndex, objectIndex, options);
}
bool WorldAuthoring::removeSectorForceBridge(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeSectorForceBridge(sectorIndex, objectIndex);
}

// --- Walkways ---
bool WorldAuthoring::canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, std::string* diagnostic) const
{
	return document().canAddSectorWalkway(sectorIndex, levelIndex, xOffset, diagnostic);
}
core::World::CreateObjectResult WorldAuthoring::addSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset)
{
	return world().addSectorWalkway(sectorIndex, levelIndex, xOffset);
}
core::World::WalkwayEditPlan WorldAuthoring::planRemoveSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex) const
{
	return document().planRemoveSectorWalkway(sectorIndex, objectIndex);
}
bool WorldAuthoring::applyWalkwayEdit(core::World::WalkwayEditPlan const& plan)
{
	return world().applyWalkwayEdit(plan);
}
bool WorldAuthoring::removeSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex)
{
	return world().removeSectorWalkway(sectorIndex, objectIndex);
}

// --- Markers ---
bool WorldAuthoring::canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, std::string* diagnostic) const
{
	return document().canAddSectorMarker(sectorIndex, levelIndex, xOffset, diagnostic);
}
core::World::CreateObjectResult WorldAuthoring::addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier)
{
	return world().addSectorMarker(sectorIndex, levelIndex, xOffset, vertexIdentifier);
}
core::World::CreateObjectResult WorldAuthoring::addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, std::string const& name, uint32_t* vertexIdentifier)
{
	return world().addSectorMarker(sectorIndex, levelIndex, xOffset, name, vertexIdentifier);
}
bool WorldAuthoring::canRenameMarker(core::MarkerId id, std::string const& name, std::string* diagnostic) const
{
	return document().canRenameMarker(id, name, diagnostic);
}
bool WorldAuthoring::renameMarker(core::MarkerId id, std::string const& name, std::string* diagnostic)
{
	return world().renameMarker(id, name, diagnostic);
}
bool WorldAuthoring::canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex, std::string* diagnostic) const
{
	return document().canRemoveSectorMarker(sectorIndex, objectIndex, diagnostic);
}
bool WorldAuthoring::removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex, std::string* diagnostic)
{
	return world().removeSectorMarker(sectorIndex, objectIndex, diagnostic);
}

// --- Moving authored objects ---
core::World::ObjectMovePlan WorldAuthoring::planMoveSectorObject(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y) const
{
	return document().planMoveSectorObject(sectorIndex, objectIndex, x, y);
}
core::World::ObjectMovePlan WorldAuthoring::planResizeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeSectorWindow(sectorIndex, objectIndex, x, y, cellsWide, levelsHigh);
}
core::World::ObjectMovePlan WorldAuthoring::planResizeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
{
	return document().planResizeSectorDoor(sectorIndex, objectIndex, x, y, cellsWide, levelsHigh);
}
std::shared_ptr<const core::SectorObject> WorldAuthoring::applyObjectMove(core::World::ObjectMovePlan const& plan)
{
	return world().applyObjectMove(plan);
}

// --- Dumbwaiters ---
bool WorldAuthoring::canAddDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x, core::World::CreateDumbwaiterOptions const& options, std::string* diagnostic) const
{
	return document().canAddDumbwaiter(shaftLayer, y, x, options, diagnostic);
}
core::DumbwaiterId WorldAuthoring::addDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x, core::World::CreateDumbwaiterOptions const& options)
{
	return world().addDumbwaiter(shaftLayer, y, x, options);
}
core::World::DumbwaiterMovePlan WorldAuthoring::planMoveDumbwaiter(core::DumbwaiterId id, uint32_t shaftLayer, uint32_t y, uint32_t x) const
{
	return document().planMoveDumbwaiter(id, shaftLayer, y, x);
}
bool WorldAuthoring::applyDumbwaiterMove(core::World::DumbwaiterMovePlan const& plan)
{
	return world().applyDumbwaiterMove(plan);
}
bool WorldAuthoring::removeDumbwaiter(core::DumbwaiterId id)
{
	return world().removeDumbwaiter(id);
}

bool WorldAuthoring::canSetSectorSurfaceVariant(uint32_t sectorIndex, uint32_t column,
	uint32_t level, std::optional<uint32_t> variant, std::string* diagnostic) const
{
	return document().canSetSectorSurfaceVariant(sectorIndex, column, level, variant,
		diagnostic);
}

core::SurfaceVariantWrite WorldAuthoring::setSectorSurfaceVariant(
	uint32_t sectorIndex, uint32_t column, uint32_t level,
	std::optional<uint32_t> variant, std::string* diagnostic)
{
	return world().setSectorSurfaceVariant(sectorIndex, column, level, variant,
		diagnostic);
}

bool WorldAuthoring::canSetSectorFloorDepth(uint32_t sectorIndex, int32_t depth,
	std::string* diagnostic) const
{
	return document().canSetSectorFloorDepth(sectorIndex, depth, diagnostic);
}

core::World::FloorDepthWrite WorldAuthoring::setSectorFloorDepth(
	uint32_t sectorIndex, int32_t depth, std::string* diagnostic)
{
	return world().setSectorFloorDepth(sectorIndex, depth, diagnostic);
}

core::World::FloorDepthWrite WorldAuthoring::clearSectorFloorDepth(
	uint32_t sectorIndex, std::string* diagnostic)
{
	return world().clearSectorFloorDepth(sectorIndex, diagnostic);
}

int32_t WorldAuthoring::effectiveFloorDepth(uint32_t sectorIndex) const
{
	return document().effectiveFloorDepth(sectorIndex);
}

int32_t WorldAuthoring::layerFloorOffsetPx(uint32_t layerIndex) const
{
	return document().layerFloorOffsetPx(layerIndex);
}

std::vector<int32_t> WorldAuthoring::layerFloorOffsets() const
{
	return document().layerFloorOffsets();
}

bool WorldAuthoring::canSetSectorTypeFloorDepth(core::SectorType type,
	int32_t depth, std::string* diagnostic) const
{
	return document().canSetSectorTypeFloorDepth(type, depth, diagnostic);
}

core::World::FloorDepthWrite WorldAuthoring::setSectorTypeFloorDepth(
	core::SectorType type, int32_t depth, std::string* diagnostic)
{
	return world().setSectorTypeFloorDepth(type, depth, diagnostic);
}

core::World::FloorDepthWrite WorldAuthoring::clearSectorTypeFloorDepth(
	core::SectorType type, std::string* diagnostic)
{
	return world().clearSectorTypeFloorDepth(type, diagnostic);
}

int32_t WorldAuthoring::sectorTypeFloorDepth(core::SectorType type) const
{
	return document().sectorTypeFloorDepth(type);
}

bool WorldAuthoring::hasSectorTypeFloorDepth(core::SectorType type) const
{
	return document().hasSectorTypeFloorDepth(type);
}

bool WorldAuthoring::hasFloorDepthOverride(uint32_t sectorIndex) const
{
	return document().hasFloorDepthOverride(sectorIndex);
}

// --- Agents ---
bool WorldAuthoring::canPlaceAgentInLocation(uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets, std::string* diagnostic) const
{
	return document().canPlaceAgentInLocation(sectorId, grants, sets, diagnostic);
}
core::AgentId WorldAuthoring::createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset)
{
	return world().createAgent(name, sectorId, levelOffset, xOffset);
}
core::AgentId WorldAuthoring::createAgent(std::string const& name, uint32_t sectorId)
{
	return world().createAgent(name, sectorId);
}
core::AgentId WorldAuthoring::createAgent(std::string typeId, std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset)
{
	return world().createAgent(typeId, name, sectorId, levelOffset, xOffset);
}
core::AgentId WorldAuthoring::createAgent(std::string typeId, std::string const& name, uint32_t sectorId)
{
	return world().createAgent(typeId, name, sectorId);
}
core::AgentId WorldAuthoring::createAgent(std::string typeId, std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets, std::optional<core::ObjectUsage> objectUsage, std::optional<float> objectUsageDistance, std::set<core::AgentTagId> const& tags)
{
	return world().createAgent(typeId, name, sectorId, levelOffset, xOffset, grants, sets, objectUsage, objectUsageDistance, tags);
}
core::AgentId WorldAuthoring::createAgent(std::string typeId, std::string const& name, uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets)
{
	return world().createAgent(typeId, name, sectorId, grants, sets);
}
core::AgentId WorldAuthoring::createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets)
{
	return world().createAgent(name, sectorId, levelOffset, xOffset, grants, sets);
}
core::AgentId WorldAuthoring::createAgent(std::string const& name, uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets)
{
	return world().createAgent(name, sectorId, grants, sets);
}
core::EntityRemovalResult WorldAuthoring::removeAgent(core::AgentId id)
{
	return world().removeAgent(id);
}

// --- Agent groups ---
bool WorldAuthoring::canAddAgentGroup(std::string const& name, std::string* diagnostic) const
{
	return document().canAddAgentGroup(name, diagnostic);
}
core::AgentGroupId WorldAuthoring::addAgentGroup(std::string const& name)
{
	return world().addAgentGroup(name);
}
bool WorldAuthoring::canRenameAgentGroup(core::AgentGroupId id, std::string const& name, std::string* diagnostic) const
{
	return document().canRenameAgentGroup(id, name, diagnostic);
}
bool WorldAuthoring::renameAgentGroup(core::AgentGroupId id, std::string const& name, std::string* diagnostic)
{
	return world().renameAgentGroup(id, name, diagnostic);
}
bool WorldAuthoring::canDeleteAgentGroup(core::AgentGroupId id, std::string* diagnostic) const
{
	return document().canDeleteAgentGroup(id, diagnostic);
}
bool WorldAuthoring::deleteAgentGroup(core::AgentGroupId id, std::string* diagnostic)
{
	return world().deleteAgentGroup(id, diagnostic);
}

// --- Agent tags ---
bool WorldAuthoring::canRemoveAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic) const
{
	return document().canRemoveAgentTag(agent, tag, diagnostic);
}
bool WorldAuthoring::removeAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic)
{
	return world().removeAgentTag(agent, tag, diagnostic);
}

// --- Access control ---
bool WorldAuthoring::canAddAccessPermission(std::string const& name, std::string* diagnostic) const
{
	return document().canAddAccessPermission(name, diagnostic);
}
core::AccessPermissionId WorldAuthoring::addAccessPermission(std::string const& name)
{
	return world().addAccessPermission(name);
}
bool WorldAuthoring::renameAccessPermission(core::AccessPermissionId id, std::string const& name, std::string* diagnostic)
{
	return world().renameAccessPermission(id, name, diagnostic);
}
core::PermissionSetId WorldAuthoring::addPermissionSet(std::string const& name)
{
	return world().addPermissionSet(name);
}
bool WorldAuthoring::renamePermissionSet(core::PermissionSetId id, std::string const& name, std::string* diagnostic)
{
	return world().renamePermissionSet(id, name, diagnostic);
}

// --- Interaction points and traversal resources ---
core::EntityRemovalResult WorldAuthoring::removeInteractionPoint(core::InteractionPointId id)
{
	return world().removeInteractionPoint(id);
}
core::EntityRemovalResult WorldAuthoring::removeInteractionRequest(core::InteractionRequestId id)
{
	return world().removeInteractionRequest(id);
}
core::EntityRemovalResult WorldAuthoring::removeDeviceOperation(core::DeviceOperationId id)
{
	return world().removeDeviceOperation(id);
}
bool WorldAuthoring::addTraversalControl(core::TraversalResourceId resource, core::InteractionPointId control)
{
	return world().addTraversalControl(resource, control);
}
core::EntityRemovalResult WorldAuthoring::removeTraversalResource(core::TraversalResourceId id)
{
	return world().removeTraversalResource(id);
}

// --- Furniture ---
bool WorldAuthoring::canPlaceFurniture(uint32_t sector, std::string const& definition, float x, float y, std::string const& name, std::string* diagnostic, int localDepth) const
{
	return document().canPlaceFurniture(sector, definition, x, y, name, diagnostic, localDepth);
}
uint64_t WorldAuthoring::placeFurniture(uint32_t sector, std::string const& definition, float x, float y, std::string const& name, int localDepth)
{
	return world().placeFurniture(sector, definition, x, y, name, localDepth);
}
bool WorldAuthoring::canEditFurniture(uint64_t id, float x, float y, std::string const& name, std::string* diagnostic, std::optional<int> localDepth) const
{
	return document().canEditFurniture(id, x, y, name, diagnostic, localDepth);
}
bool WorldAuthoring::editFurniture(uint64_t id, float x, float y, std::string const& name, std::string* diagnostic, std::optional<int> localDepth)
{
	return world().editFurniture(id, x, y, name, diagnostic, localDepth);
}
bool WorldAuthoring::canRemoveFurniture(uint64_t id, std::string* diagnostic) const
{
	return document().canRemoveFurniture(id, diagnostic);
}
bool WorldAuthoring::removeFurniture(uint64_t id, std::string* diagnostic)
{
	return world().removeFurniture(id, diagnostic);
}

// --- Resource binding: Agent tag registry ---
// The mutable registry handle the runtime no longer gives out (#974).
// Taken from the mutable view, so the mutable overload of the World's
// accessor is the one this resolves; the returned handle writes the
// registry but cannot reseat the World's reference to it.
std::shared_ptr<core::AgentTagRegistry> WorldAuthoring::getAgentTagRegistry()
{
	return world().getAgentTagRegistry();
}

// Every attach below is a write to the document's registry reference, so it
// goes through `world()`. None of them is answered from `document()`: a
// binding change is not a read that could be preflighted here, and the World
// owns the paused-only rule that governs it (#969).
void WorldAuthoring::attachAgentTagRegistry(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry)
{
	world().attachAgentTagRegistry(std::move(resourceName), std::move(registry));
}
void WorldAuthoring::attachAgentTagRegistryAndClearAssignments(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry)
{
	world().attachAgentTagRegistryAndClearAssignments(std::move(resourceName), std::move(registry));
}
void WorldAuthoring::detachAgentTagRegistry()
{
	world().detachAgentTagRegistry();
}
void WorldAuthoring::detachAgentTagRegistryAndClearAssignments()
{
	world().detachAgentTagRegistryAndClearAssignments();
}
void WorldAuthoring::resolveAgentTagRegistry(std::shared_ptr<core::AgentTagRegistry> registry)
{
	world().resolveAgentTagRegistry(std::move(registry));
}
void WorldAuthoring::replaceAgentTagRegistryWithIndependentCopy(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry)
{
	world().replaceAgentTagRegistryWithIndependentCopy(std::move(resourceName), std::move(registry));
}

// --- Resource binding: Agent behaviour registry ---
std::shared_ptr<core::AgentBehaviourRegistry> WorldAuthoring::getAgentBehaviourRegistry()
{
	// Mutable package handle, same shape and same reason as the tag registry
	// above (#974).
	return world().getAgentBehaviourRegistry();
}
void WorldAuthoring::attachAgentBehaviourRegistry(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry)
{
	world().attachAgentBehaviourRegistry(std::move(resourceName), std::move(registry));
}
void WorldAuthoring::attachAgentBehaviourRegistryAndClearAssignments(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry)
{
	world().attachAgentBehaviourRegistryAndClearAssignments(std::move(resourceName), std::move(registry));
}
void WorldAuthoring::detachAgentBehaviourRegistry()
{
	world().detachAgentBehaviourRegistry();
}
void WorldAuthoring::detachAgentBehaviourRegistryAndClearAssignments()
{
	world().detachAgentBehaviourRegistryAndClearAssignments();
}
void WorldAuthoring::resolveAgentBehaviourRegistry(std::shared_ptr<core::AgentBehaviourRegistry> registry)
{
	world().resolveAgentBehaviourRegistry(std::move(registry));
}
void WorldAuthoring::markAgentBehaviourRegistryUnavailable(std::string diagnostic)
{
	world().markAgentBehaviourRegistryUnavailable(std::move(diagnostic));
}
void WorldAuthoring::replaceAgentBehaviourRegistryWithIndependentCopy(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry)
{
	world().replaceAgentBehaviourRegistryWithIndependentCopy(std::move(resourceName), std::move(registry));
}

// --- Resource binding: Agent types ---
bool WorldAuthoring::attachAgentType(std::string resourceName, std::string source, std::string* diagnostic)
{
	return world().attachAgentType(std::move(resourceName), std::move(source), diagnostic);
}
bool WorldAuthoring::detachUnusedAgentType(std::string_view typeId)
{
	return world().detachUnusedAgentType(typeId);
}

// --- Resource binding: catalogues and Action packages ---
void WorldAuthoring::attachFurnitureCatalogue(std::string resourceName, std::shared_ptr<const core::FurnitureCatalogue> catalogue)
{
	world().attachFurnitureCatalogue(std::move(resourceName), std::move(catalogue));
}
bool WorldAuthoring::reloadFurnitureCatalogue(std::filesystem::path const& path, std::string* diagnostic)
{
	return world().reloadFurnitureCatalogue(path, diagnostic);
}
bool WorldAuthoring::selectActionRegistry(std::filesystem::path const& path, std::string* diagnostic)
{
	return world().selectActionRegistry(path, diagnostic);
}
bool WorldAuthoring::reloadActionRegistry(std::filesystem::path const& path, std::string* diagnostic)
{
	return world().reloadActionRegistry(path, diagnostic);
}
bool WorldAuthoring::clearActionRegistry(std::string* diagnostic)
{
	return world().clearActionRegistry(diagnostic);
}

// --- Resource binding: image-set names ---
void WorldAuthoring::setSectorImageSetResourceName(std::string name)
{
	world().setSectorImageSetResourceName(std::move(name));
}
void WorldAuthoring::setObjectImageSetResourceName(std::string name)
{
	world().setObjectImageSetResourceName(std::move(name));
}
void WorldAuthoring::setAgentImageSetResourceName(std::string name)
{
	world().setAgentImageSetResourceName(std::move(name));
}
void WorldAuthoring::setFurnitureImageSetResourceName(std::string name)
{
	world().setFurnitureImageSetResourceName(std::move(name));
}
void WorldAuthoring::setLayerSectorImageSet(std::uint32_t layerIndex,
	std::optional<std::string> sectorImageSet)
{
	world().setLayerSectorImageSet(layerIndex, std::move(sectorImageSet));
}

// --- In-place property edits ---
bool WorldAuthoring::setRoomHeightScale(uint32_t sectorIndex,
	std::optional<float> scale, std::string* diagnostic)
{
	return world().setRoomHeightScale(sectorIndex, scale, diagnostic);
}
bool WorldAuthoring::setBackgroundColour(uint32_t sectorIndex,
	core::BackgroundColour const& colour, std::string* diagnostic)
{
	return world().setBackgroundColour(sectorIndex, colour, diagnostic);
}
bool WorldAuthoring::setFacadeColour(uint32_t sectorIndex,
	core::BackgroundColour const& colour, std::string* diagnostic)
{
	return world().setFacadeColour(sectorIndex, colour, diagnostic);
}
bool WorldAuthoring::setMarkerProperties(core::MarkerId id,
	core::MarkerProperties properties, std::string* diagnostic)
{
	return world().setMarkerProperties(id, properties, diagnostic);
}
bool WorldAuthoring::setAirlockCycleSeconds(uint32_t sectorIndex, float seconds)
{
	return world().setAirlockCycleSeconds(sectorIndex, seconds);
}

// --- Agent live state ---
bool WorldAuthoring::setAgentActive(core::AgentId agent, bool active,
	std::string* diagnostic)
{
	return world().setAgentActive(agent, active, diagnostic);
}
bool WorldAuthoring::clearAgentPath(core::AgentId agent)
{
	return world().clearAgentPath(agent);
}
core::InteractionRequestId WorldAuthoring::requestInteraction(
	core::InteractionPointId point, core::AgentId actor)
{
	return world().requestInteraction(point, actor);
}

// --- Build ---
void WorldAuthoring::finishBuild()
{
	world().finishBuild();
}

// --- Persistence ---
void WorldAuthoring::saveTo(std::string const& filepath)
{
	// The whole save contract is the World's and is deliberately not
	// restated or softened here: the document becomes clean only once the
	// write has fully succeeded, and a throw leaves it dirty. The handle's
	// history is left alone on purpose - see the header note on why the
	// saved marker is the caller's to move (#969).
	world().saveTo(filepath);
}

// --- History: undo and redo ---

std::optional<DocumentSnapshot> WorldAuthoring::captureSnapshot() const
{
	// Guarded before the try rather than inside it: a moved-from handle is a
	// programming error, not a document that could not be captured, and it has
	// to reach the caller instead of arriving as a nullopt (#978).
	requireLive(history_.get(), "WorldAuthoring::captureSnapshot");
	try
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		// Capturing is not saving. The document keeps its modified flag so an
		// undo back to the saved state still reads as modified, which is what
		// the editor's own snapshot path does.
		workData.markSerializedUnmodified = false;
		document().serialize(*serializer, workData);
		serializer->serialize();
		return history_->capture(serializer->getSerializedString());
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Authoring", 0, core::LogLevel::Error,
			"Could not capture the authored document: " + std::string(error.what()));
		return std::nullopt;
	}
}

void WorldAuthoring::commitEdit(std::optional<DocumentSnapshot> snapshot)
{
	history().commit(std::move(snapshot));
}

bool WorldAuthoring::undo()
{
	return history().undo(captureSnapshot(),
		[this](DocumentSnapshot const& target) { return restoreInPlace(target); });
}

bool WorldAuthoring::redo()
{
	return history().redo(captureSnapshot(),
		[this](DocumentSnapshot const& target) { return restoreInPlace(target); });
}

bool WorldAuthoring::undo(std::optional<DocumentSnapshot> current,
	DocumentHistory::RestoreDocument const& restore)
{
	return history().undo(std::move(current), restore);
}

bool WorldAuthoring::redo(std::optional<DocumentSnapshot> current,
	DocumentHistory::RestoreDocument const& restore)
{
	return history().redo(std::move(current), restore);
}

bool WorldAuthoring::canUndo() const
{
	return history().canUndo();
}

bool WorldAuthoring::canRedo() const
{
	return history().canRedo();
}

std::size_t WorldAuthoring::undoDepth() const
{
	return history().undoCount();
}

std::size_t WorldAuthoring::redoDepth() const
{
	return history().redoCount();
}

bool WorldAuthoring::isModified() const
{
	return history().isModified();
}

void WorldAuthoring::markSaved()
{
	history().markSaved();
}

void WorldAuthoring::clearHistory()
{
	history().clear();
}

DocumentHistory& WorldAuthoring::history()
{
	return requireLive(history_.get(), "WorldAuthoring::history");
}

DocumentHistory const& WorldAuthoring::history() const
{
	return requireLive(history_.get(), "WorldAuthoring::history");
}

void WorldAuthoring::adoptHistoryFrom(WorldAuthoring& source)
{
	if (&source == this) return;
	// Both sides have to be live. Swapping with a moved-from handle would leave
	// one of them holding a null history, which is the quiet loss this family
	// exists to prevent rather than a transfer to perform (#978).
	requireLive(history_.get(), "WorldAuthoring::adoptHistoryFrom");
	requireLive(source.history_.get(), "WorldAuthoring::adoptHistoryFrom");
	// Swap rather than move-and-rebuild: this handle's own history is emptied
	// first and then becomes the fresh one the source is left holding, so
	// nothing allocates and neither handle is ever left without a history
	// to answer from (#975).
	history_->clear();
	std::swap(history_, source.history_);
}

bool WorldAuthoring::restoreInPlace(DocumentSnapshot const& snapshot)
{
	// The probe runs the whole restore against a throwaway World first. A
	// snapshot that cannot be read - a package the live process can no longer
	// resolve, a document that contradicts itself - is rejected there, before
	// the document under the running runtime is touched at all. It is not a
	// proof that the live apply succeeds, because the live World starts from a
	// different state than the probe does; it is what makes the common
	// failure cheap and early (#968).
	try
	{
		auto probe = core::World("Restore probe", 1, 1);
		auto probeSerializer = core::YamlSerializer::fromString(snapshot.yaml);
		probeSerializer->deserialize();
		auto workData = makeRestoreWorkData(document());
		if (!probe.deserialize(*probeSerializer, workData)) return false;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Authoring", 0, core::LogLevel::Error, restoreFailure(error));
		return false;
	}

	try
	{
		auto& target = world();
		// The run state is the runtime's business, not the document's. A
		// restore resets it, so it is put back the way it was found.
		auto const wasPaused = target.isSimulationPaused();
		auto serializer = core::YamlSerializer::fromString(snapshot.yaml);
		serializer->deserialize();
		auto workData = makeRestoreWorkData(target);
		if (!target.deserialize(*serializer, workData)) return false;
		if (wasPaused) target.pauseSimulation();
		return true;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Authoring", 0, core::LogLevel::Error, restoreFailure(error));
		return false;
	}
}

// --- AuthoringPair -------------------------------------------------

AuthoringPair::AuthoringPair(std::unique_ptr<core::World> world)
	// The World goes into a runtime first, so the null refusal is the runtime's
	// own and is not restated here: a pair cannot be more permissive than the
	// thing it pairs.
	: AuthoringPair(std::make_unique<WorldRuntime>(std::move(world)))
{
}

AuthoringPair::AuthoringPair(std::unique_ptr<WorldRuntime> runtime)
	// The handle is wired here and only here, against the address the runtime
	// has had since it was allocated and will keep until the pair is destroyed.
	// Nothing later can reseat it, because nothing later can move the runtime.
	: runtime_(std::move(runtime))
	, authoring_(runtime_ ? std::make_unique<WorldAuthoring>(*runtime_) : nullptr)
{
	if (!authoring_)
		throw std::invalid_argument(
			"game::AuthoringPair requires a WorldRuntime to pair with");
}

AuthoringPair::~AuthoringPair() = default;

AuthoringPair::AuthoringPair(AuthoringPair&& other) noexcept = default;

AuthoringPair& AuthoringPair::operator=(AuthoringPair&& other) noexcept
{
	if (this != &other)
	{
		// The handle goes before the runtime it names, so there is no moment in
		// which a live handle outlives the World it was wired to. The incoming
		// pair then arrives with its own handle and its own pinned runtime, and
		// the two keep pointing at each other exactly as they did before the
		// move - which is the whole property this type exists to hold (#977).
		authoring_.reset();
		runtime_ = std::move(other.runtime_);
		authoring_ = std::move(other.authoring_);
	}
	return *this;
}

WorldRuntime& AuthoringPair::runtime()
{
	return requireLive(runtime_.get(), "AuthoringPair::runtime");
}

WorldRuntime const& AuthoringPair::runtime() const
{
	return requireLive(runtime_.get(), "AuthoringPair::runtime");
}

WorldAuthoring& AuthoringPair::authoring()
{
	return requireLive(authoring_.get(), "AuthoringPair::authoring");
}

WorldAuthoring const& AuthoringPair::authoring() const
{
	return requireLive(authoring_.get(), "AuthoringPair::authoring");
}
}
