#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/World.h"
#include "game/AuthoringPlatform.h"
#include "game/DocumentHistory.h"
#include "game/WorldRuntime.h"

namespace game
{
	// The authoring half of the game-facing boundary (design pattern) over a
	// World (#967, spec #960). Where `WorldRuntime` owns a World and is the
	// only way to *drive* it, `WorldAuthoring` is the only way to *change* it.
	//
	// It wraps a `WorldRuntime` rather than owning a World beside it. That is
	// not a contrivance: it is what the editor already does, live-editing
	// against a running simulation. Because authoring sits over the runtime,
	// the editor's preview is literally the game's runtime path with authoring
	// powers added, rather than a second implementation of it that can drift.
	//
	// The privilege is expressed in the types rather than left to discipline.
	// `WorldRuntime` hands out `core::World const&` and nothing else; the
	// mutable view it holds is private and granted to this class alone, so
	// `WorldAuthoring` really is the only thing in a process that can turn a
	// running World into a mutable one. A program that links only
	// `pf-game-runtime` cannot even name this type, which is why the game's
	// link line is the review-visible statement of what it is allowed to do.
	//
	// This is the external, consumer-facing authoring surface. It is not
	// `core::WorldBuilder`, the internal collaborator `World` forwards to
	// (ADR 0022); authoring reaches `WorldBuilder` only through `World`,
	// exactly as every other caller does.
	//
	// The families below are thin forwards with the signatures they forward
	// mirrored one for one (#968). That mirroring is deliberate: a call site
	// that today says `world->addRoom(...)` says `authoring.addRoom(...)` and
	// nothing else changes, so the editor can converge on this handle without
	// a translation layer that could drift from the thing it wraps. Nothing
	// here holds World state. Every plan is built by the World, every edit is
	// applied by the World, and the construction records that replay is still
	// the World's alone - behaviour moves, state does not (ADR 0022). The one
	// thing the handle does own outright is the document's undo history, at the
	// end of this class; that is not World state in the sense the sentence
	// means, because the World can be driven, saved and loaded without ever
	// consulting it. It is the author's state, which is why it sits here and
	// not beside the runtime (#975).
	//
	// The two views are used differently and that split is visible in the
	// bodies. A write goes through `world()`, the mutable view the runtime
	// grants this class alone. A preflight guard is `const` because the World
	// says it is, so it reads through `document()`, the same `core::World
	// const&` a render path gets from the runtime. Asking whether an edit is
	// legal therefore requires no more privilege than drawing the World has
	// always needed.
	class PF_GAME_AUTHORING_API WorldAuthoring
	{
	public:
		// Wraps `runtime`. Nothing is owned and nothing is copied: the World
		// stays the runtime's, and the runtime must outlive this handle. Taking
		// a reference rather than a pointer makes "authoring needs a running
		// World to author against" a compile-time fact - there is no null case
		// to refuse at runtime.
		explicit WorldAuthoring(WorldRuntime& runtime) noexcept;

		~WorldAuthoring();

		// The handle is movable but never copied: copying would suggest two
		// independently held privileges where there is only one view of one
		// World.
		//
		// Moving a handle cannot strand anything, because the runtime it names
		// cannot move - the moves were deleted in #977 for exactly that reason.
		// What a move does leave behind is an empty handle, and every accessor
		// below fails loudly on one instead of dereferencing the null `runtime_`
		// it now holds (#978).
		WorldAuthoring(WorldAuthoring&& other) noexcept;
		WorldAuthoring& operator=(WorldAuthoring&& other) noexcept;
		WorldAuthoring(WorldAuthoring const&) = delete;
		WorldAuthoring& operator=(WorldAuthoring const&) = delete;

		// The mutable World, for authoring. This is the only mutable `World&`
		// reachable through the game-facing boundary, and it is valid only for
		// as long as the wrapped runtime lives. Structural edits follow the
		// protocol the World already enforces: pause at the edit/simulation
		// boundary, edit, rebuild, resume.
		//
		// Throws `std::logic_error` when this handle has been moved from. There
		// is no World behind it any more, and a null dereference is not an
		// answer to a question about the document (#978).
		core::World& world();

		// The wrapped runtime, for preview. The editor renders and drives
		// through this, so what it previews is the same runtime path the game
		// runs - the guarantee this arrangement exists to provide. Throws
		// `std::logic_error` on a moved-from handle, for the same reason.
		WorldRuntime& runtime();

		WorldRuntime const& runtime() const;

		// The read-only view of the document being authored. It is the same
		// `core::World const&` the runtime hands to rendering, and every
		// `can...` guard below is answered from it. Preflighting an edit is a
		// read, so it is answered without spending the mutable-view privilege;
		// only the edit itself needs that. Throws `std::logic_error` on a
		// moved-from handle, for the same reason.
		core::World const& document() const;

		// The authoring topology and preflight families follow, grouped by
		// what is being authored. Each `can...` guard is declared next to the
		// operation it guards rather than in a separate preflight section, so
		// the question and the act that answers for it are read together and
		// cannot drift apart across a review.
		//
		// Every method forwards to the World unchanged, including its failure
		// contract: a guard returns false and fills `diagnostic`, an operation
		// either succeeds or throws `core::Exception`, and a `plan...` /
		// `apply...` pair still validates in the plan and commits in the
		// apply. Nothing here adds a policy of its own.

	// --- Levels and Layers ---
	uint32_t addLayer();
	void addLevel();
	void setLayerName(uint32_t layerIndex, std::string name);
	void setLevelName(uint32_t level, std::string name);
	core::World::LevelDeletePlan planDeleteLevel(uint32_t level) const;
	bool applyDeleteLevel(core::World::LevelDeletePlan const& plan);
	core::World::LayerDeletePlan planDeleteLayer(uint32_t layerIndex) const;
	bool applyDeleteLayer(core::World::LayerDeletePlan const& plan);

	// --- Locations ---
	uint32_t addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh = 1);
	uint32_t addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh = 1);
	uint32_t addRoom(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT);
	bool canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, std::string* diagnostic = nullptr) const;
	uint32_t addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, core::BackgroundColour const& colour = {});
	bool canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, std::string* diagnostic = nullptr) const;
	uint32_t addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT, core::BackgroundColour const& colour = core::Facade::defaultColour());
	uint32_t addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT, core::BackgroundColour const& colour = core::Facade::defaultColour());
	core::World::LocationEditPlan planResizeLocation(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	core::World::LocationEditPlan planRemoveLocation(uint32_t sectorIndex) const;
	core::World::LocationEditPlan planResizeFacade(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	core::World::LocationEditPlan planRemoveFacade(uint32_t sectorIndex) const;
	uint32_t applyLocationEdit(core::World::LocationEditPlan const& plan);
	core::World::LocationEditPlan planResizeBackground(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	core::World::LocationEditPlan planRemoveBackground(uint32_t sectorIndex) const;
	uint32_t applyBackgroundEdit(core::World::LocationEditPlan const& plan);

	// --- Location walls ---
	bool canAddLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side, std::string* diagnostic = nullptr) const;
	void addLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side);
	bool canRemoveLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side, std::string* diagnostic = nullptr) const;
	void removeLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side);

	// --- Ladders ---
	bool canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, std::string* diagnostic = nullptr) const;
	core::World::CreateLadderResult addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateLadderOptions const& options);
	core::World::CreateLadderResult addSectorLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLadderOptions const& options);
	bool canAddRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, uint32_t* levelsHigh = nullptr, std::string* diagnostic = nullptr) const;
	core::World::CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset);
	core::World::CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLadderOptions options);
	std::shared_ptr<const core::SectorObject> applyRoomLadderOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateLadderOptions const& options);
	bool removeRoomLadder(uint32_t sectorIndex, uint32_t objectIndex);
	core::World::LadderEditPlan planResizeLadder(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateLadderOptions const& options) const;
	core::World::LadderEditPlan planRemoveLadder(uint32_t sectorIndex) const;
	uint32_t applyLadderEdit(core::World::LadderEditPlan const& plan);

	// --- Stairwells and staircases ---
	bool canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, std::string* diagnostic = nullptr) const;
	uint32_t addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, int mountSide);
	core::World::CreateStairwellResult addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateStairwellOptions const& options);
	bool canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide, std::string* diagnostic = nullptr) const;
	uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide, float speed = 0.0f);
	uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateStaircaseOptions const& options);
	core::World::StairwellEditPlan planResizeStairwell(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateStairwellOptions const& options) const;
	core::World::StairwellEditPlan planRemoveStairwell(uint32_t sectorIndex) const;
	uint32_t applyStairwellEdit(core::World::StairwellEditPlan const& plan);
	core::World::StaircaseEditPlan planResizeStaircase(uint32_t sectorIndex, uint32_t x, uint32_t y, core::World::CreateStaircaseOptions const& options) const;
	core::World::StaircaseEditPlan planRemoveStaircase(uint32_t sectorIndex) const;
	uint32_t applyStaircaseEdit(core::World::StaircaseEditPlan const& plan);

	// --- Lifts ---
	core::World::CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateLiftOptions const& options);
	core::World::CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh);
	bool canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset, core::World::CreateLiftOptions const& options, std::string* diagnostic = nullptr) const;
	core::World::CreatePlatformLiftResult addSectorPlatformLift(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateLiftOptions const& options);
	core::World::PlatformLiftEditPlan planPlatformLiftEdit(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateLiftOptions const& options) const;
	core::World::PlatformLiftEditPlan planRemovePlatformLift(uint32_t sectorIndex, uint32_t objectIndex) const;
	std::shared_ptr<const core::SectorObject> applyPlatformLiftEdit( core::World::PlatformLiftEditPlan const& plan);
	core::World::LiftEditPlan planResizeLift(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	core::World::LiftEditPlan planRemoveLift(uint32_t sectorIndex) const;
	core::World::LiftEditPlan planRemoveLiftStop(uint32_t sectorIndex, uint32_t stopIndex) const;
	uint32_t applyLiftEdit(core::World::LiftEditPlan const& plan);

	// --- Shuttles ---
	core::World::CreateShuttleResult addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, core::World::CreateShuttleOptions const& options);
	core::World::ShuttleEditPlan planResizeShuttle(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t cellsWide) const;
	core::World::ShuttleEditPlan planEditShuttleVehicle(uint32_t sectorIndex, uint32_t numCars, uint32_t carWidth, uint32_t doorMask) const;
	core::World::ShuttleEditPlan planRemoveShuttle(uint32_t sectorIndex) const;
	core::World::ShuttleEditPlan planRemoveShuttleStop(uint32_t sectorIndex, uint32_t stopIndex) const;
	core::World::ShuttleEditPlan planAddShuttleStop(uint32_t sectorIndex, uint32_t stopOffset) const;
	uint32_t applyShuttleEdit(core::World::ShuttleEditPlan const& plan);

	// --- Airlocks ---
	bool canAddAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float cycleSeconds = 3.0f, std::string* diagnostic = nullptr) const;
	uint32_t addAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float cycleSeconds = 3.0f);
	core::World::AirlockEditPlan planResizeAirlock(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t width) const;
	core::World::AirlockEditPlan planRemoveAirlock(uint32_t sectorIndex) const;
	uint32_t applyAirlockEdit(core::World::AirlockEditPlan const& plan);

	// --- Chambers ---
	bool canAddChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, std::string* diagnostic = nullptr, core::ChamberSubtype subtype = core::ChamberSubtype::SecurityScanner) const;
	uint32_t addChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, bool leftToRight = true, core::ChamberSubtype subtype = core::ChamberSubtype::SecurityScanner);
	core::World::ChamberEditPlan planResizeChamber(uint32_t sectorIndex, uint32_t x, uint32_t y, uint32_t width, bool leftToRight) const;
	core::World::ChamberEditPlan planRemoveChamber(uint32_t sectorIndex) const;
	core::World::ChamberEditPlan planSetChamberSubtype(uint32_t sectorIndex, core::ChamberSubtype subtype) const;
	uint32_t applyChamberEdit(core::World::ChamberEditPlan const& plan);

	// --- Access panels ---
	bool canAddAccessPanel(uint32_t sectorIndex, uint32_t levelOffset, uint32_t cellX,
		core::AccessPanelGeometry geometry = {}, std::string* diagnostic = nullptr) const;
	core::World::CreateObjectResult addAccessPanel(uint32_t sectorIndex,
		uint32_t levelOffset, uint32_t cellX,
		core::AccessPanelGeometry geometry = {}, std::optional<float> speed = {});
	bool removeAccessPanel(uint32_t sectorIndex, uint32_t objectIndex);

	// --- Doors ---
	bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, std::string* diagnostic = nullptr) const;
	bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateDoorOptions const& options, std::string* diagnostic = nullptr) const;
	core::World::CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x);
	core::World::CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, core::World::CreateDoorOptions const& options);
	bool canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;
	void addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex);
	bool canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;
	std::shared_ptr<const core::DoorSectorObject> removeSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex, std::optional<std::vector<core::AccessPermissionId>> resultingRequirement = std::nullopt);
	bool removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex);

	// --- Windows ---
	bool canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide = 1, uint32_t levelsHigh = 1, std::string* diagnostic = nullptr) const;
	uint32_t addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh);
	core::World::CreateWindowResult addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, core::World::CreateWindowOptions const& options);
	bool removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex);
	bool canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x, uint32_t width = 1, uint32_t height = 1, std::string* diagnostic = nullptr) const;
	core::World::CreateWindowResult addBoothWindow(uint32_t layer, uint32_t y, uint32_t x, core::Window::State initialState = core::Window::State::Closed);

	// --- Bulkhead doors ---
	bool canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side, core::World::CreateBulkheadDoorOptions const& options, std::string* diagnostic = nullptr) const;
	core::World::CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side);
	core::World::CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side, core::World::CreateBulkheadDoorOptions const& options);
	std::shared_ptr<const core::SectorObject> applySectorBulkheadDoorOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateBulkheadDoorOptions const& options);
	bool removeSectorBulkheadDoor(uint32_t sectorIndex, uint32_t objectIndex);

	// --- Switches and force bridges ---
	core::World::CreateObjectResult addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset);
	bool canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateForceBridgeOptions const& options, std::string* diagnostic = nullptr) const;
	core::World::CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset);
	core::World::CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, core::World::CreateForceBridgeOptions const& options);
	std::shared_ptr<const core::SectorObject> applySectorForceBridgeOptions(uint32_t sectorIndex, uint32_t objectIndex, core::World::CreateForceBridgeOptions const& options);
	bool removeSectorForceBridge(uint32_t sectorIndex, uint32_t objectIndex);

	// --- Walkways ---
	bool canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, std::string* diagnostic = nullptr) const;
	core::World::CreateObjectResult addSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset);
	core::World::WalkwayEditPlan planRemoveSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex) const;
	bool applyWalkwayEdit(core::World::WalkwayEditPlan const& plan);
	bool removeSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex);

	// --- Markers ---
	bool canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, std::string* diagnostic = nullptr) const;
	core::World::CreateObjectResult addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier = nullptr);
	core::World::CreateObjectResult addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset, std::string const& name, uint32_t* vertexIdentifier = nullptr);
	bool canRenameMarker(core::MarkerId id, std::string const& name, std::string* diagnostic = nullptr) const;
	bool renameMarker(core::MarkerId id, std::string const& name, std::string* diagnostic = nullptr);
	bool canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex, std::string* diagnostic = nullptr) const;
	bool removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex, std::string* diagnostic = nullptr);

	// --- Moving authored objects ---
	core::World::ObjectMovePlan planMoveSectorObject(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y) const;
	core::World::ObjectMovePlan planResizeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	core::World::ObjectMovePlan planResizeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;
	std::shared_ptr<const core::SectorObject> applyObjectMove(core::World::ObjectMovePlan const& plan);

	// --- Dumbwaiters ---
	bool canAddDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x, core::World::CreateDumbwaiterOptions const& options, std::string* diagnostic = nullptr) const;
	core::DumbwaiterId addDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x, core::World::CreateDumbwaiterOptions const& options);
	core::World::DumbwaiterMovePlan planMoveDumbwaiter(core::DumbwaiterId id, uint32_t shaftLayer, uint32_t y, uint32_t x) const;
	bool applyDumbwaiterMove(core::World::DumbwaiterMovePlan const& plan);
	bool removeDumbwaiter(core::DumbwaiterId id);

	// --- Sector surface variants ---
	//
	// The authoring half of "this cell wears this variant" (#1042). The
	// preflight is a read and the edit is not, which is the split the whole
	// class is built on: the hover affordance that colours the cell under the
	// cursor asks `canSet...` and needs no more privilege than drawing the
	// World already has.
	//
	// The edit answers with both halves of what happened (#1051) - accepted
	// or refused, and among the accepted whether the cell came out holding
	// anything different - because the caller owns the undo step and has to
	// be able to tell a press worth recording from one that is not.
	bool canSetSectorSurfaceVariant(uint32_t sectorIndex, uint32_t column, uint32_t level, std::optional<uint32_t> variant, std::string* diagnostic = nullptr) const;
	core::SurfaceVariantWrite setSectorSurfaceVariant(uint32_t sectorIndex, uint32_t column, uint32_t level, std::optional<uint32_t> variant, std::string* diagnostic = nullptr);

	// --- Agents ---
	bool canPlaceAgentInLocation(uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets, std::string* diagnostic = nullptr) const;
	core::AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset);
	core::AgentId createAgent(std::string const& name, uint32_t sectorId);
	core::AgentId createAgent(std::string typeId, std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset);
	core::AgentId createAgent(std::string typeId, std::string const& name, uint32_t sectorId);
	core::AgentId createAgent(std::string typeId, std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets, std::optional<core::ObjectUsage> objectUsage = std::nullopt, std::optional<float> objectUsageDistance = std::nullopt, std::set<core::AgentTagId> const& tags = {});
	core::AgentId createAgent(std::string typeId, std::string const& name, uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets);
	core::AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets);
	core::AgentId createAgent(std::string const& name, uint32_t sectorId, std::set<core::AccessPermissionId> const& grants, std::set<core::PermissionSetId> const& sets);
	core::EntityRemovalResult removeAgent(core::AgentId id);

	// --- Resource binding: Agent tag registry ---
	// The attached Agent tag registry, mutable. #974 closed the hole where a
	// `const` read on `WorldRuntime` handed out a writable registry; the
	// writable handle did not disappear, it moved here, because writing a
	// registry is an authoring act (#969). A game holding only the runtime
	// reads the same registry as `shared_ptr<const AgentTagRegistry>` and
	// cannot reach this.
	//
	// The registry object is handed out, not the World's slot: which package
	// answers is the attach / detach family below, and no accessor reseats
	// it. Null when nothing is attached, exactly as on the World.
	std::shared_ptr<core::AgentTagRegistry> getAgentTagRegistry();

	// A World references zero or one Agent tag registry Resource by name and
	// expected UUID, and the attach / detach family is how that reference is
	// moved. It is grouped here rather than scattered because the whole family
	// is one kind of act: changing which external package the document's tag
	// IDs resolve against. None of it is something a running game does - it
	// loads Worlds through the resource system and never rebinds them - which
	// is why the family sits on the authoring handle and not the runtime
	// (#969, spec #960 decision 5).
	//
	// The names travel with the packages. The read half of that pair - what
	// name the document currently holds - stays on `WorldRuntime`, so a game
	// can bind visuals without ever being able to rebind them.
	//
	// Attaching and detaching are paused-only, and the World is what enforces
	// that; these forwards add no guard of their own. The `...AndClearAssignments`
	// variants are the destructive transactions the editor reaches for only
	// after confirmation, kept explicit rather than folded into the plain
	// attach so a caller cannot clear a document's tag state by accident.
	void attachAgentTagRegistry(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry);
	void attachAgentTagRegistryAndClearAssignments(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry);
	void detachAgentTagRegistry();
	void detachAgentTagRegistryAndClearAssignments();
	void resolveAgentTagRegistry(std::shared_ptr<core::AgentTagRegistry> registry);

	// Save As moves an intact World into an equivalent, independent tag
	// namespace. Forwarded because Save As is an authoring act through and
	// through: assignments and samples are preserved, any definition or
	// allocator difference is refused.
	void replaceAgentTagRegistryWithIndependentCopy(std::string resourceName, std::shared_ptr<core::AgentTagRegistry> registry);

	// --- Resource binding: Agent behaviour registry ---
	// The mutable package handle, for the same reason and with the same shape
	// as `getAgentTagRegistry()` above (#974).
	std::shared_ptr<core::AgentBehaviourRegistry> getAgentBehaviourRegistry();
	void attachAgentBehaviourRegistry(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry);
	void attachAgentBehaviourRegistryAndClearAssignments(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry);
	void detachAgentBehaviourRegistry();
	void detachAgentBehaviourRegistryAndClearAssignments();
	void resolveAgentBehaviourRegistry(std::shared_ptr<core::AgentBehaviourRegistry> registry);

	// A package load failure is dependency state, not malformed World data:
	// recording it leaves the persisted reference and the unresolved
	// assignments available for repair rather than dirtying the document.
	void markAgentBehaviourRegistryUnavailable(std::string diagnostic);
	void replaceAgentBehaviourRegistryWithIndependentCopy(std::string resourceName, std::shared_ptr<core::AgentBehaviourRegistry> registry);

	// --- Resource binding: Agent types ---
	// Registers a `.agent.lua` type definition under `resourceName`. The
	// source is validated before it is accepted, and a refusal mutates nothing.
	bool attachAgentType(std::string resourceName, std::string source, std::string* diagnostic = nullptr);

	// Rolls back an unused placement registration. Paired with the attach
	// above so a placement that was never committed can be taken back through
	// the same handle that made it.
	bool detachUnusedAgentType(std::string_view typeId);

	// --- Resource binding: catalogues and Action packages ---
	void attachFurnitureCatalogue(std::string resourceName, std::shared_ptr<const core::FurnitureCatalogue> catalogue);
	bool reloadFurnitureCatalogue(std::filesystem::path const& path, std::string* diagnostic = nullptr);
	bool selectActionRegistry(std::filesystem::path const& path, std::string* diagnostic = nullptr);
	bool reloadActionRegistry(std::filesystem::path const& path, std::string* diagnostic = nullptr);
	bool clearActionRegistry(std::string* diagnostic = nullptr);

	// --- Resource binding: image-set names ---
	// Which image set each visual class draws from. Setting one is a binding
	// act, so it is here; reading one is a fact about the document, so it is
	// on `WorldRuntime`. The pair is the boundary drawn at the finest grain
	// this family allows.
	void setSectorImageSetResourceName(std::string name);
	void setObjectImageSetResourceName(std::string name);
	void setAgentImageSetResourceName(std::string name);
	void setFurnitureImageSetResourceName(std::string name);

	// --- In-place property edits -------------------------------------
	// These change one authored property of something that already exists, so
	// there is no plan to make and no cascade to run: the World patches the
	// live object and its construction record together. Each one still
	// refuses when the document cannot accept it, and the refusal is the
	// World's, not a new policy added here.
	bool setRoomHeightScale(uint32_t sectorIndex, std::optional<float> scale,
		std::string* diagnostic = nullptr);
	bool setBackgroundColour(uint32_t sectorIndex,
		core::BackgroundColour const& colour, std::string* diagnostic = nullptr);
	bool setFacadeColour(uint32_t sectorIndex,
		core::BackgroundColour const& colour, std::string* diagnostic = nullptr);
	bool setMarkerProperties(core::MarkerId id, core::MarkerProperties properties,
		std::string* diagnostic = nullptr);
	bool setAirlockCycleSeconds(uint32_t sectorIndex, float seconds);

	// --- Agent live state --------------------------------------------
	// The editor changes whether an Agent runs and drops a Path it no longer
	// wants; the World already labels these as editor operations. They are
	// here rather than on the runtime because the game never asks for them -
	// the player steers an Agent through the input surface, and never
	// activates one or clears its route (#972, spec #960 decision 4).
	bool setAgentActive(core::AgentId agent, bool active,
		std::string* diagnostic = nullptr);
	bool clearAgentPath(core::AgentId agent);

	// Press an Interaction Point on behalf of some Agent the editor has
	// picked, which is how the canvas tests a Button or an Access panel.
	// This is not player input - the runtime's input surface is about the
	// player - so it is an authoring-side act on the document.
	core::InteractionRequestId requestInteraction(core::InteractionPointId point,
		core::AgentId actor);

	// --- Build -------------------------------------------------------
	// The rebuild that closes a structural edit. `WorldRuntime` also exposes
	// this, because a game has to build the World it loaded; the editor's
	// call is not the same act. It follows an edit that changed topology and
	// is part of the edit transaction the World forces - pause, edit, build,
	// resume - so it belongs with the handle that made the edit (#972).
	void finishBuild();

	// --- Persistence -------------------------------------------------
	// Writing the document and settling the saved marker are two calls, not
	// one, and that is deliberate rather than unfinished.
	//
	// `saveTo` writes this document. The World's own contract is that the
	// clean-state transition happens only after the file has been opened,
	// written, flushed, closed and replaced without error; anything thrown
	// leaves the document exactly as dirty as it was, so a failed save never
	// loses work by pretending it succeeded.
	//
	// The handle's saved marker is not moved by `saveTo`, because this handle
	// cannot see the rest of the transaction. A World document travels with
	// an Agent tag registry document and an Agent behaviour package, and only
	// the caller orchestrating the save knows when all of them have reached
	// disk. Marking the history saved here would clear the author's
	// "unsaved" signal while a companion document was still pending. So the
	// caller marks it - `markSaved()` - once the whole set is down, which is
	// exactly the order the editor's save path already uses.
	//
	// Two modified flags exist and they mean different things. The document's
	// own flag is read through `document().isModified()` and is cleared by
	// `saveTo`. The handle's `isModified()` is the history's bookkeeping
	// about committed edits and is cleared by `markSaved()`. Neither is a
	// proxy for the other, and a document is stale when either is set.
	void saveTo(std::string const& filepath);

	// --- Agent groups ---
	bool canAddAgentGroup(std::string const& name, std::string* diagnostic = nullptr) const;
	core::AgentGroupId addAgentGroup(std::string const& name);
	bool canRenameAgentGroup(core::AgentGroupId id, std::string const& name, std::string* diagnostic = nullptr) const;
	bool renameAgentGroup(core::AgentGroupId id, std::string const& name, std::string* diagnostic = nullptr);
	bool canDeleteAgentGroup(core::AgentGroupId id, std::string* diagnostic = nullptr) const;
	bool deleteAgentGroup(core::AgentGroupId id, std::string* diagnostic = nullptr);

	// --- Agent tags ---
	bool canRemoveAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic = nullptr) const;
	bool removeAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic = nullptr);

	// --- Access control ---
	bool canAddAccessPermission(std::string const& name, std::string* diagnostic = nullptr) const;
	core::AccessPermissionId addAccessPermission(std::string const& name);
	bool renameAccessPermission(core::AccessPermissionId id, std::string const& name, std::string* diagnostic = nullptr);
	core::PermissionSetId addPermissionSet(std::string const& name);
	bool renamePermissionSet(core::PermissionSetId id, std::string const& name, std::string* diagnostic = nullptr);

	// --- Interaction points and traversal resources ---
	core::EntityRemovalResult removeInteractionPoint(core::InteractionPointId id);
	core::EntityRemovalResult removeInteractionRequest(core::InteractionRequestId id);
	core::EntityRemovalResult removeDeviceOperation(core::DeviceOperationId id);
	bool addTraversalControl(core::TraversalResourceId resource, core::InteractionPointId control);
	core::EntityRemovalResult removeTraversalResource(core::TraversalResourceId id);

	// --- Furniture ---
	bool canPlaceFurniture(uint32_t sector, std::string const& definition, float x, float y, std::string const& name, std::string* diagnostic = nullptr, int localDepth = 0) const;
	uint64_t placeFurniture(uint32_t sector, std::string const& definition, float x, float y, std::string const& name, int localDepth = 0);
	bool canEditFurniture(uint64_t id, float x, float y, std::string const& name, std::string* diagnostic = nullptr, std::optional<int> localDepth = std::nullopt) const;
	bool editFurniture(uint64_t id, float x, float y, std::string const& name, std::string* diagnostic = nullptr, std::optional<int> localDepth = std::nullopt);
	bool canRemoveFurniture(uint64_t id, std::string* diagnostic = nullptr) const;
	bool removeFurniture(uint64_t id, std::string* diagnostic = nullptr);

	// The convenience overload the World carries inline, mirrored so a caller
	// that wants a Dumbwaiter with all defaults does not have to name the
	// options struct.
	core::DumbwaiterId addDumbwaiter(std::uint32_t shaftLayer, std::uint32_t y, std::uint32_t x)
	{
		return addDumbwaiter(shaftLayer, y, x, core::World::CreateDumbwaiterOptions{});
	}

	// --- History: undo and redo -----------------------------------------
	// The authoring handle owns the document history, because the history is
	// the authoring half of the arrangement: the runtime drives, the author
	// changes, and a change that cannot be taken back is not an edit.
	//
	// That claim is the kind that gets trusted without being checked, so it is
	// worth saying what makes it true rather than leaving it asserted (#975).
	// The editor's undo, redo, unsaved work and saved marker all read this
	// history and no other: the process-wide `DocumentHistory` the editor used
	// to drive is gone, so `isModified()` below and the editor's
	// unsaved-changes prompt are one question asked of one object. The
	// editor-preview smoke module pins that identity, because the way an
	// authoring façade (design pattern) like this one fails is not by answering
	// wrongly but by answering truly about something nobody is looking at.
	//
	// The transaction shape is the one the editor already uses, because it is
	// the one the World's edit protocol forces. Snapshot the document before
	// the edit, make the edit, commit the snapshot. `captureSnapshot()` is
	// taken before the mutation and `commitEdit()` after it succeeds, so a
	// thrown edit leaves the history untouched rather than recording a change
	// that never happened.
	//
	// Undo and redo restore the wrapped World *in place*. The World object the
	// runtime owns is never replaced - it is the same object before and after,
	// which is what lets a running runtime keep driving while its document is
	// rolled back underneath it. The restored World comes back built: the
	// construction-record replay that `World::deserialize` performs ends in
	// `finishBuild()`, so no separate rebuild step is needed.
	//
	// What in-place restoration does not do is preserve C++ object identity.
	// Agents come back as new instances under the same stable `AgentId`, so a
	// caller that held an `Agent*` across an undo is holding a dangling
	// pointer. Ids are the currency across this boundary, exactly as they are
	// across a save and load.
	//
	// The run state is left as it was found. A paused World stays paused and a
	// running one keeps running; undo changes the document, not whether the
	// simulation is live.

	// Serialize the document as it currently stands, ready to be committed
	// once an edit has succeeded. Returns nullopt when the document cannot be
	// captured, which leaves the history untouched if committed anyway.
	std::optional<DocumentSnapshot> captureSnapshot() const;

	// Record a captured snapshot as an edit. Passing nullopt is a no-op, so
	// a failed edit needs no special case at the call site.
	void commitEdit(std::optional<DocumentSnapshot> snapshot);

	// Roll the document back one edit, in place. False means there was
	// nothing to undo, or the snapshot could not be restored; either way the
	// World is left exactly as it was and the history is unchanged.
	bool undo();

	// Re-apply the last undone edit, in place. Same contract as `undo()`.
	bool redo();

	// Undo or redo one step of this handle's history with the restore step
	// supplied by the caller rather than with `restoreInPlace()`.
	//
	// The no-argument pair above restores the wrapped World in place, which
	// is what a caller holding one World and one runtime wants. A caller
	// whose document travels with companion documents does not: the editor's
	// snapshot names an Agent tag registry and a behaviour package it does
	// not carry, and has to resolve both before the restored state is
	// accepted, so it brings its own restore. What is not brought, and cannot
	// be, is the stack: that stays here on the handle, so an editor undo and
	// the `isModified()` the editor's prompt reads are the same history
	// either way (#975).
	//
	// `restore` runs before the stacks move, exactly as it does on
	// `DocumentHistory`: returning false, or throwing, leaves the history
	// untouched.
	bool undo(std::optional<DocumentSnapshot> current,
		DocumentHistory::RestoreDocument const& restore);

	bool redo(std::optional<DocumentSnapshot> current,
		DocumentHistory::RestoreDocument const& restore);

	// The remaining history family reads and writes this handle's own history,
	// and none of it is `noexcept`: a handle that has been moved from owns no
	// history, and every one of these throws `std::logic_error` on one rather
	// than dereferencing the null pointer it now holds (#978).
	bool canUndo() const;

	bool canRedo() const;

	std::size_t undoDepth() const;

	std::size_t redoDepth() const;

	// True when the document differs from the last saved state. This is the
	// history's own bookkeeping, not the World's modified flag.
	bool isModified() const;

	// Mark the current history position as saved, clearing the modified flag.
	void markSaved();

	// Drop both stacks and the saved marker. Used when a document is closed or
	// replaced wholesale, where replaying an old edit would be wrong.
	void clearHistory();

	// The history this handle owns, as the object rather than as this family's
	// mirror of it.
	//
	// The families above forward to the World; this hands out the handle's own
	// state, because some seams are written against `DocumentHistory` itself
	// rather than against an authoring handle - the save target carries a
	// saved-state marker, and the shared editor panels commit into whatever
	// history they were passed. Handing over the object keeps one history of
	// one type instead of re-declaring its surface twice, and it is the very
	// same object `isModified()`, `markSaved()` and `clearHistory()` act on,
	// which is what makes the editor's unsaved-changes prompt and this
	// authoring façade (design pattern) unable to drift apart (#975).
	DocumentHistory& history();

	DocumentHistory const& history() const;

	// Take over the history `source` holds, leaving `source` with a fresh
	// empty one of its own.
	//
	// The history belongs to the document, not to the World instance inside it
	// or to the runtime that happens to be driving that World. The editor's
	// undo re-materialises its document over a restored World - a new runtime,
	// and so a new handle, because a runtime owns the World it was built with
	// for its whole life (#971) - and the author's undo stack has to cross
	// that seam with the document instead of dying with the handle it happened
	// to be sitting on. Without this the history would reset on the first
	// undo, which is exactly the kind of quiet loss this family exists to
	// prevent (#975).
	// Both handles have to be live. Adopting from a moved-from handle, or into
	// one, would leave a handle with no history to answer from - the same
	// quiet loss arriving by a different route - so it throws
	// `std::logic_error` rather than performing the swap (#978).
	void adoptHistoryFrom(WorldAuthoring& source);

	private:
		// Replace the wrapped World's contents with `snapshot`, in place. Used
		// by `undo()` and `redo()` as the history's restore callback; returns
		// false without touching the history when the snapshot cannot be read.
		bool restoreInPlace(DocumentSnapshot const& snapshot);

		// Not owned. The runtime owns the World; this only names it.
		WorldRuntime* runtime_;

		// Owned. The history belongs to the authoring handle rather than to a
		// process-wide global, so two documents in one process keep separate
		// undo stacks and neither can roll back the other's work.
		std::unique_ptr<DocumentHistory> history_;
	};

	// A `WorldRuntime` and the `WorldAuthoring` wrapped over it, held as one
	// value that can be stored, returned and moved (#977).
	//
	// This is the transfer the boundary needs and that the runtime alone cannot
	// offer. `WorldRuntime` refuses to move because a handle names it by
	// address, so the pair cannot hold the runtime by value either: it pins
	// the runtime on the heap and wires the handle to that pinned address once,
	// at construction, for the pair's whole life. Moving the pair moves two
	// pointers and leaves the runtime exactly where it was, so there is no
	// move left in the type system that a handle can be left behind by.
	//
	// That is the whole guarantee, and it is structural rather than policed:
	// a caller that wants to hand a World over with its authoring powers
	// attached does it through this type, or through a pointer to a runtime it
	// has already pinned, or not at all. The bug this exists to prevent was
	// not a wrong value flowing through an API but a legal one, and the fix is
	// to make the illegal shape unwriteable rather than to check for it.
	class PF_GAME_AUTHORING_API AuthoringPair
	{
	public:
		// Takes the World, builds the runtime that owns it, and wraps this
		// pair's own authoring handle over that runtime. A null `world` is
		// refused by the runtime itself with `std::invalid_argument`, and the
		// refusal is passed through unchanged: a pair with nothing to drive is
		// no more constructible than a runtime with nothing to drive.
		explicit AuthoringPair(std::unique_ptr<core::World> world);

		// Wraps a fresh authoring handle over a runtime the caller already
		// owns. The runtime keeps the address it already has, which is what
		// makes the handle safe to hold: from here the pair keeps that object
		// pinned. A null `runtime` throws `std::invalid_argument`, because a
		// pair whose runtime is null has no address to pin and every accessor
		// on it would be a question with no thing behind it.
		explicit AuthoringPair(std::unique_ptr<WorldRuntime> runtime);

		~AuthoringPair();

		// Movable as a whole, never copied. The move hands over two pointers;
		// the runtime and the World inside it do not move, so the handle is
		// still wired to the thing it was wired to. A moved-from pair is empty
		// and its accessors fail loudly rather than dereferencing null (#978).
		AuthoringPair(AuthoringPair&& other) noexcept;
		AuthoringPair& operator=(AuthoringPair&& other) noexcept;
		AuthoringPair(AuthoringPair const&) = delete;
		AuthoringPair& operator=(AuthoringPair const&) = delete;

		// The pinned runtime. `authoring().runtime()` and this name the same
		// object, and that identity is the property the transfer exists to
		// preserve: driving through here drives what the handle authors.
		// Throws `std::logic_error` on a moved-from pair.
		WorldRuntime& runtime();

		WorldRuntime const& runtime() const;

		// The authoring handle wrapped over the runtime above. Throws
		// `std::logic_error` on a moved-from pair.
		WorldAuthoring& authoring();

		WorldAuthoring const& authoring() const;

	private:
		// Held by pointer on purpose. The handle below caches this object's
		// address, so the one thing that must never happen to it is moving.
		std::unique_ptr<WorldRuntime> runtime_;
		std::unique_ptr<WorldAuthoring> authoring_;
	};
}
