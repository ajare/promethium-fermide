#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/Simulation.h"
// Pulled in whole rather than forward declared: the transport Stop selector is a
// nested type of `core::World`, and #960 decision 6 keeps the read-model types
// as boundary types instead of mirroring them, so the runtime returns the core
// struct itself rather than a second copy that could drift.
#include "core/World.h"
#include "game/Platform.h"

namespace game
{
	// The game-facing façade (design pattern) over a World: it owns one World
	// exclusively and is the only way to drive it (#964, spec #960).
	//
	// Exclusive ownership is the whole point of the type. The World is held as a
	// `std::unique_ptr`, the runtime is neither copyable nor movable, and the
	// only public World accessor hands out a `core::World const&`. A render
	// path therefore cannot mutate what it renders, and no second holder of the
	// same World can exist. Authoring lives over the runtime rather than beside
	// it: the privileged mutable view arrives with WorldAuthoring (#967), never
	// here. The runtime does not move because that view names it by address -
	// see the deleted move operations below (#977).
	//
	// Everything below is a thin forward onto the same simulation the editor
	// already drives, so behaviour is unchanged; only the shape of the surface
	// through which a game reaches it is new.
	class PF_GAME_RUNTIME_API WorldRuntime
	{
	public:
		// Takes exclusive ownership of `world`. Throws std::invalid_argument
		// when `world` is null: a runtime with nothing to drive is not a state
		// a caller may construct.
		explicit WorldRuntime(std::unique_ptr<core::World> world);

		// Convenience construction of the owned World with the same accepted
		// dimensions as the core constructor.
		WorldRuntime(std::string const& name, std::uint32_t cellsWide, std::uint32_t levelsHigh);

		~WorldRuntime();

		// Neither moved nor copied (#977). The moves used to be defaulted, and
		// that was the bug: a `WorldAuthoring` handle names the runtime it wraps
		// by address, so moving a runtime that a handle was wrapped over left
		// the handle naming the moved-from shell - whose `world_` is null - with
		// nothing to re-point it and nothing to detect that it had gone stale.
		// The hazard was legal at every call site and invisible at all of them.
		//
		// Exclusive ownership still transfers; what is gone is transferring it
		// by moving the runtime object. Every consumer already holds the runtime
		// behind a pointer - `std::unique_ptr<WorldRuntime>`, or
		// `std::shared_ptr<WorldRuntime>` where several holders share one World
		// - and moving a pointer moves the address the handle cached instead of
		// the address of the thing it cached, which is exactly why the existing
		// holders were never bitten. Where a caller wants the runtime and its
		// authoring handle as one value it takes `game::AuthoringPair` from
		// pf-game-authoring (#977), which pins the runtime on the heap for the
		// pair's whole life: the transfer cannot be spelled in a form that
		// leaves a handle behind.
		WorldRuntime(WorldRuntime&&) = delete;
		WorldRuntime& operator=(WorldRuntime&&) = delete;
		WorldRuntime(WorldRuntime const&) = delete;
		WorldRuntime& operator=(WorldRuntime const&) = delete;

		// The read view handed to rendering. Valid for as long as this runtime
		// lives and remains the same World throughout; every read through it is
		// const, so rendering cannot mutate what it renders.
		//
		// Throws `std::logic_error` naming this accessor if the runtime holds no
		// World. It cannot happen today - both ctors refuse an empty one and the
		// moves are deleted above, so there is no moved-from `WorldRuntime` to
		// read from (#977) - but every read below goes through the same guard so
		// that if that ever stops being true it reports itself instead of
		// dereferencing null (#978). That is also why this is not `noexcept`: a
		// guard that cannot throw answers a moved-from value with
		// `std::terminate` rather than with anything a caller could read.
		core::World const& world() const;

		// --- Driving -------------------------------------------------------
		// Rendering supplies elapsed wall time here. It is accumulated and only
		// whole fixed simulation ticks are executed, exactly as on World.
		void update(float elapsedSeconds);

		// Headless deterministic seam. Never uses render timing; reports false
		// when a behaviour failure stops the run before the next tick.
		bool advanceTick();

		bool advanceTicks(std::uint64_t count);

		// Pause, resume and reset behave exactly as they do on World today:
		// pausing cancels live traversal while retaining each Agent's route
		// intent, resuming over dirty topology is refused, and reset restores
		// the authored state.
		void pauseSimulation();

		bool resumeSimulation();

		void resetSimulation();

		bool isSimulationPaused() const;

		// Release every Agent the World owns without reaching into Agent
		// internals.
		void wakeAllAgents();

		void setSimulationTimeScale(double scale);

		double getSimulationTimeScale() const;

		// Structural build steps the runtime lifecycle owns: finishBuild()
		// generates the traversal graph for a freshly created World, and
		// rebuildTraversalTopology() revalidates it after a paused edit.
		void finishBuild();

		bool rebuildTraversalTopology();

		// --- Player input --------------------------------------------------
		// The whole surface a player drives a World through (#965, spec #960).
		// Every one of these is held by the World as transient input state: it is
		// never authored, never persisted, and is read once per fixed Tick. Each
		// method forwards to the same handling the editor's play mode already
		// uses, so behaviour is identical; only the route to it is new.
		//
		// Input is dropped while the simulation is paused, exactly as on World:
		// a paused World cannot be steered, so a held key does not fire on the
		// first Tick after a resume.

		// The Agent the input belongs to: the lowest stable-ID Agent whose
		// behaviour assignment makes it player-controlled. A default-constructed
		// AgentId means nothing is controlled, and every other method here then
		// has nothing to act on.
		core::AgentId getPlayerControlledAgent() const;

		// Held steering: -1 left, 0 idle, +1 right, clamped on the way in.
		void setPlayerMovementDirection(int direction);

		// Held Staircase/Stairwell choice. Both set at once is read as no
		// selection rather than as a conflict, and neither key alone selects a
		// flight the Player is not walking into.
		void setPlayerStairSelection(bool up, bool down);

		// Held crouch: the Player lowers to Crouching standing still and to
		// Crawling while moving, and only where the space around them takes it.
		void setPlayerCrouch(bool held);

		// One press of the contextual action key, consumed at the next
		// allocation phase. Its meaning is read from where the Player stands:
		// a threshold crossing first, then opening a manual Door, then the
		// nearest physical control.
		void requestPlayerDoorAction();

		// The Stop list a Player riding a Lift or Shuttle is shown, or
		// std::nullopt when they are not in a position to choose one.
		std::optional<core::World::PlayerTransportSelector> getPlayerTransportSelector() const;

		// Moves the highlighted Stop in the selector; `horizontal` browses a
		// Shuttle's side-by-side Stops rather than a Lift's stacked ones. It
		// changes the highlight only, nothing is requested until confirm.
		void browsePlayerTransportStop(int direction, bool horizontal);

		// Requests the highlighted Stop for the riding Player. Repeating it on
		// the same Stop is idempotent, and the request outlives the Player
		// stepping off.
		void confirmPlayerTransportStop();

		// Drops every held and pending input at once. The Agent stops on the
		// next Tick rather than coasting to a halt, and a pressed-but-unconsumed
		// action is discarded rather than delivered later.
		void clearPlayerInput();

		// --- Observation ---------------------------------------------------
		std::uint64_t getSimulationTick() const;

		core::SimulationPhase getCurrentSimulationPhase() const;

		core::SimulationSnapshot getSimulationSnapshot() const;

		// Valid until the next tick or mutation; callers retaining data must
		// copy it.
		core::SimulationSnapshot const& getSimulationSnapshotView() const;

		std::uint64_t getSimulationSnapshotBuildCount() const;

		// Drains the event queue: each event is returned exactly once.
		std::vector<core::SimulationEvent> consumeSimulationEvents();

		// Non-owning; register/unregister on the simulation thread. The
		// observer must outlive its registration and may only read the World
		// during callbacks.
		void setSimulationObserver(core::SimulationObserver* observer);

		// The fixed simulation timestep and the per-update Tick bound, mirrored
		// from core so the runtime is the single place a host asks about
		// pacing (#983). Not to keep `core/World.h` out of the include set:
		// this header includes that file whole near the top, so no consumer of
		// the runtime avoids it and that was never what these bought.
		//
		// Both are `constexpr`, matching the core originals, so a host that
		// wrote `constexpr float dt = core::World::getFixedTimestep();`
		// before this boundary existed keeps writing it. The value stays a
		// compile-time constant - usable in array bounds, template arguments,
		// `static_assert`s and compile-time arithmetic - rather than decaying
		// into a runtime call the moment the read is routed through here. The
		// bodies forward to core rather than restating the literals, so the
		// mirror still cannot drift from what the simulation runs on.
		//
		// `constexpr` implies `inline`, so both are defined here rather than
		// in `WorldRuntime.cpp`, and they are not exported across the
		// shared-library boundary: that is the rule `game/Platform.h` already
		// states for `template` and `inline` members. A host pacing itself
		// therefore makes no cross-DLL call for either constant.
		static constexpr float getFixedTimestep()
		{
			return core::World::getFixedTimestep();
		}

		static constexpr std::uint32_t getMaxTicksPerUpdate()
		{
			return core::World::getMaxTicksPerUpdate();
		}

		// --- Resource names ----------------------------------------------
		// The names a World's external packages are referenced by (#969, spec
		// #960 decision 5). Binding them - attaching, detaching, reloading -
		// is an authoring act and lives on `WorldAuthoring`; the game never
		// does it, because it loads Worlds through the Willpower resource
		// system. What the game does need is to know which names the document
		// was authored against, so it can resolve the visuals it renders.
		//
		// These are the read half of the binding family, and the split is the
		// whole point: a name is a fact about the document, the act of
		// changing which package answers to it is not. Each returns the name
		// as the document holds it, empty when the World references nothing.
		//
		// That non-throwing contract is the contract for the whole family, and
		// it covers the expected-UUID reads too - `getExpectedAgentTagRegistryUuid()`
		// and `getExpectedAgentBehaviourRegistryUuid()`, declared with their
		// registry families below (#980). A document bound to no package is a
		// normal authored state, not a programming error, so those read empty
		// on the same guard rather than faulting a game or render path - and
		// rather than throwing a `std::runtime_error` raised in another module
		// across the shared-library boundary into a loop that has no way to
		// author the reference it is missing.
		//
		// The throwing forms stay where they are wanted, on `core::World`:
		// `getAgentTagRegistryResourceName()` / `getExpectedAgentTagRegistryUuid()`
		// and the behaviour pair still raise when the World references
		// nothing, because the editor's save path uses an absent reference as
		// a programming-error signal. The editor holds the World directly -
		// through `world()` when it starts from a runtime - so the signal is
		// reachable exactly where it is meaningful, and nothing on this type
		// forwards it.

		// The Agent tag registry Resource this document references.
		std::string const& agentTagRegistryResourceName() const;

		// The Agent behaviour registry package this document references.
		std::string const& agentBehaviourRegistryResourceName() const;

		// The Furniture catalogue Resource this document references.
		std::string const& furnitureCatalogueResourceName() const;

		// The image-set Resources the document was authored against.
		std::string const& sectorImageSetResourceName() const;

		// The fourth of the four viewport atlases the document selects (#1022).
		std::string const& objectImageSetResourceName() const;

		std::string const& agentImageSetResourceName() const;

		std::string const& furnitureImageSetResourceName() const;

		// The Action registry package basename, empty when none is selected.
		std::string const& actionRegistryFilename() const;

		// --- The read surface ---------------------------------------------
		// Every read the World answers through a `const` member is answered here
		// (#966, spec #960). The rule is the one #963 established: `const` is the
		// compiler-checked definition of "read" in this codebase, so the read
		// surface is the const method set rather than a hand-picked list that can
		// fall behind the World. A game therefore never needs a second path to
		// read state - and never needs to know which of these the renderer wants.
		//
		// The audited set is that const set *plus* the statics (#981). A `static`
		// read has no const-qualification problem to fix, and that is precisely
		// how `isValidTransportStopShortName` was missed: it was named on #960's
		// list of six read-shaped methods, but nothing about it failed to compile,
		// so the const work in #963/#974 walked past it and it never reached this
		// type. The completeness audit therefore counts a public `World` static
		// that reads no state as a read as well, so the next one cannot go missing
		// behind the same blind spot.
		//
		// Three exclusions, each a deliberate non-read rather than an omission:
		//
		//   * the names already on this type - the drive surface, the player-input
		//     surface and the resource-name reads (#964, #965, #969) - are not
		//     forwarded twice;
		//   * `World::invalidateSimulationSnapshot()`, which is `const` only
		//     because its cache is `mutable`: it writes, so it is not a read and
		//     has no place on a read surface;
		//   * the authoring families - `can...`/`plan...` guards, `add`/`remove`/
		//     `rename`/`edit`, undo and redo - which are reached through
		//     `WorldAuthoring` (#967), never from here.
		//
		// Nothing below mutates, binds a resource or touches persistence: the
		// whole section is `const`, forwards to the `const` overload of the same
		// World method, and returns the same value the World returns. What a
		// caller does with the returned handle is part of the contract too, so
		// every entity this section answers with is `const`-qualified (#974):
		// `Agent const*` rather than `Agent*`, and `shared_ptr<const Registry>`
		// rather than a `shared_ptr<Registry>` a caller could write through.
		// Mutable registries are bound and mutated by `WorldAuthoring` (#969),
		// never reached from here.

		// --- Identity, host state and diagnostics ---
		std::string const& getName() const;
		std::string const& getClipboardIdentity() const;
		std::weak_ptr<void const> getLifetimeToken() const;
		std::uint64_t getRandomSeed() const;
		std::filesystem::path const& documentDirectory() const;
		std::vector<std::string> const& getLoadWarnings() const;
		core::Log const& getBuildLog() const;

		// --- World dimensions, Layers and Levels ---
		std::uint32_t getCellsWide() const;
		std::uint32_t getLevelsHigh() const;
		std::uint32_t getLayerCount() const;
		std::string const& getLayerName(std::uint32_t layerIndex) const;
		std::string const& getLevelName(std::uint32_t level) const;
		std::uint32_t getNumSectors() const;
		std::shared_ptr<const core::Layer> getLayer(std::uint32_t layerIndex) const;
		std::shared_ptr<const core::Graph> getGraph() const;

		// The size rule the World constructor enforces, answered before a World
		// is built. Static on `World` because it reads no World state - only its
		// arguments - so it is forwarded as a static rather than being made to
		// pretend it needs a live document. The repeat completeness audit run for
		// #981 found it as the one other public static read the const pass had
		// passed by, so it lands with the forward that closed that hole.
		static bool dimensionsAreSupported(std::uint32_t cellsWide,
			std::uint32_t levelsHigh, std::uint32_t layerCount,
			std::string* diagnostic = nullptr);

		// --- Sectors and spatial lookup ---
		std::shared_ptr<const core::Sector> getSector(std::uint32_t index) const;
		std::vector<std::shared_ptr<const core::Sector>> getSectorsInBounds(std::uint32_t layerIndex, float x, float y, float width, float height) const;
		std::vector<std::shared_ptr<const core::Sector>> getSectors(std::uint32_t layerIndex) const;
		std::shared_ptr<const core::Sector> getSectorAtPosition(std::uint32_t layerIndex, float x, float y) const;
		core::Agent const* getAgentAtPosition(std::uint32_t layerIndex, float x, float y) const;
		std::shared_ptr<const core::Object> getObjectAtPosition(std::uint32_t layerIndex, float x, float y, std::shared_ptr<const core::SectorObject>* sectorObject = nullptr) const;

		// --- Traversal topology state ---
		bool getPausedPathIntent(core::Agent const& agent, core::World::TopologyPathIntent& intent) const;
		bool isTraversalTopologyDirty() const;
		bool isTraversalTopologyValid() const;
		std::uint64_t getTopologyGeneration() const;
		std::string const& getTopologyDiagnostic() const;

		// --- SectorObject ownership classification ---
		bool hasDumbwaiters() const;
		std::shared_ptr<const core::Dumbwaiter> lookupDumbwaiter(core::DumbwaiterId id) const;
		bool isDumbwaiterOwnedControl(std::shared_ptr<const core::SectorObject> const& object) const;
		bool isAirlockOwnedObject(std::shared_ptr<const core::SectorObject> const& object) const;
		bool isChamberOwnedObject(std::shared_ptr<const core::SectorObject> const& object) const;
		bool isLiftOwnedDoor(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* liftSectorIndex = nullptr, std::uint32_t* stopIndex = nullptr) const;
		bool isLiftOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* liftSectorIndex = nullptr, std::uint32_t* stopIndex = nullptr) const;
		bool isShuttleOwnedDoor(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* shuttleSectorIndex = nullptr, std::uint32_t* stopIndex = nullptr, std::uint32_t* carriageIndex = nullptr, std::uint32_t* doorIndex = nullptr) const;
		bool isShuttleOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* shuttleSectorIndex = nullptr, std::uint32_t* stopIndex = nullptr) const;
		bool isBulkheadDoorOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* doorSectorIndex = nullptr, std::uint32_t* doorObjectIndex = nullptr) const;
		bool isForceBridgeOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* forceBridgeSectorIndex = nullptr, std::uint32_t* forceBridgeObjectIndex = nullptr) const;
		bool isControlTargetBroken(core::InteractionPointId point) const;

		// --- SectorObject authored options ---
		bool getLadderOptions(std::uint32_t sectorIndex, core::World::CreateLadderOptions& options) const;
		bool getStairwellOptions(std::uint32_t sectorIndex, core::World::CreateStairwellOptions& options) const;
		bool getStaircaseOptions(std::uint32_t sectorIndex, core::World::CreateStaircaseOptions& options) const;
		std::vector<core::World::LiftLandingRow> getLiftLandingRows(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t levelsHigh) const;
		bool getLiftLandingGeometry(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t& landingX, std::uint32_t& landingWidth) const;
		std::vector<std::uint32_t> getValidShuttleStopOffsets(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t numCars, std::uint32_t carWidth, bool allowPartialLandings, std::uint32_t doorMask) const;
		bool getShuttleOptions(core::Shuttle const* shuttle, core::World::CreateShuttleOptions& options) const;
		std::vector<core::World::ShuttleStopCandidate> getShuttleStopCandidatesForDoor(std::uint32_t shuttleLayer, std::uint32_t y, std::uint32_t doorX) const;
		bool getSectorDoorOptions(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t width, core::World::CreateDoorOptions& options) const;
		bool getSectorWindowOptions(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t levelsHigh, core::World::CreateWindowOptions& options) const;
		bool getSectorBulkheadDoorOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateBulkheadDoorOptions& options) const;
		bool calculateSectorForceBridgeWidthToRight(std::uint32_t sectorIndex, std::uint32_t levelIndex, std::uint32_t xOffset, std::uint32_t& width, std::string* diagnostic = nullptr) const;
		bool getSectorForceBridgeOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateForceBridgeOptions& options) const;
		bool getRoomLadderOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateLadderOptions& options) const;
		std::vector<core::World::PlatformLiftStopCandidate> getPlatformLiftStopCandidates(std::uint32_t sectorIndex, std::uint32_t xOffset) const;
		bool getPlatformLiftOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateLiftOptions& options) const;

		// --- Markers and Agent actions ---
		std::vector<std::string> availableAgentActions(core::MarkerId marker) const;
		std::vector<std::string> markerActions(core::MarkerId marker) const;
		std::string agentActionDisplayName(std::string_view identity) const;
		std::vector<core::MarkerId> getMarkerIds() const;
		std::shared_ptr<const core::Marker> lookupMarker(core::MarkerId id) const;
		bool furnitureUseEligible(core::AgentId agent, core::MarkerId marker, std::string* diagnostic = nullptr) const;
		bool agentPoseStateFits(core::Agent const& agent, float clearance, float heightFactor = 1.0f) const;
		bool agentHeightStateFits(core::Agent const& agent, float next, std::string* diagnostic) const;
		void validateReplayPoseState(core::World const& candidate, std::uint32_t movedSectorIndex, int deltaX, int deltaY) const;
		bool furniturePoseFits(core::AgentId agent, core::MarkerId marker, core::Pose pose, bool usingSupport, std::string* diagnostic = nullptr) const;
		[[nodiscard]] float maximumInstalledStandingWidth() const;

		// --- Agent types ---
		bool hasAgentType(std::string_view typeId) const;
		std::string agentTypeDisplayName(std::string_view typeId) const;
		std::string agentTypeResourceName(std::string_view typeId) const;
		std::optional<core::AgentTypeDefinition> registeredAgentTypeDefinition(std::string_view typeId, std::string_view resourceName) const;
		std::optional<core::AgentPhysicalBaseline> registeredAgentTypeBaseline(std::string_view typeId, std::string_view resourceName) const;

		// --- Agents ---
		void validateAgentLocationPlacement(core::Sector const& sector, core::Agent const& agent, std::optional<float> floorY = std::nullopt) const;
		core::EntityLookup<core::Agent const> lookupAgent(core::AgentId id) const;
		bool canSetAgentActive(core::AgentId agent, bool active, std::string* diagnostic = nullptr) const;
		core::AgentId getAgentId(core::Agent const* agent) const;

		// --- Agent behaviour assignment and runtime ---
		bool hasAgentBehaviourRegistryReference() const;
		bool hasAttachedAgentBehaviourRegistry() const;
		// Empty rather than throwing when the document references no package:
		// the resource-name contract above, applied to the UUID (#980).
		std::string const& getExpectedAgentBehaviourRegistryUuid() const;
		std::shared_ptr<const core::AgentBehaviourRegistry> getAgentBehaviourRegistry() const;
		bool agentBehaviourConfigurationsAreValid() const;
		std::string const& getAgentBehaviourDependencyDiagnostic() const;
		bool validateAgentBehaviourAssignment(core::AgentBehaviourId behaviour, std::uint64_t revision, core::AgentBehaviourConfiguration const& configuration, core::AgentBehaviourConfiguration* normalized = nullptr, std::string* diagnostic = nullptr) const;
		std::optional<core::AgentBehaviourAssignment> const& getAgentBehaviourAssignment(core::AgentId agent) const;
		std::uint32_t getAgentBehaviourAssignmentCount() const;
		bool agentBehaviourOwnsMovement(core::AgentId agent) const;
		std::vector<core::AgentBehaviourRuntimeDiagnostic> getAgentBehaviourRuntimeDiagnostics() const;
		core::AgentBehaviourRuntimeLimits getAgentBehaviourRuntimeLimits() const;
		std::shared_ptr<const core::ActionRegistry> const& actionRegistry() const;

		// --- Agent tags ---
		bool hasAgentTagRegistryReference() const;
		bool hasAttachedAgentTagRegistry() const;
		// Empty rather than throwing when the document references no package:
		// the resource-name contract above, applied to the UUID (#980).
		std::string const& getExpectedAgentTagRegistryUuid() const;
		std::shared_ptr<const core::AgentTagRegistry> getAgentTagRegistry() const;
		std::uint64_t getAgentTagAssignmentCount() const;
		std::uint32_t getAgentTagAssignedAgentCount() const;
		std::uint64_t getAgentTagSampleCount() const;
		bool canAssignAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic = nullptr) const;
		bool validateAgentTagAssignments(std::set<core::AgentTagId> const& tags, std::optional<core::AgentPropertySample> const& walkSpeedSample, std::optional<core::AgentPropertySample> const& heightSample, std::optional<core::AgentPropertySample> const& stairSpeedSample, std::optional<core::AgentPropertySample> const& ladderSpeedSample, std::optional<core::AgentPropertySample> const& interactionAversionSample, std::optional<core::AgentPropertySample> const& effortAversionSample, std::optional<core::AgentPropertySample> const& waitingAversionSample, std::optional<core::AgentPropertySample> const& crowdAversionSample, std::optional<core::AgentPropertySample> const& riskAversionSample, std::optional<core::AgentPropertySample> const& routeFamiliaritySample, std::optional<core::AgentPropertySample> const& routePersistenceSample, std::optional<core::AgentPropertySample> const& minimumRoutePlanningTimeSample, std::optional<core::AgentPropertySample> const& maximumRoutePlanningTimeSample, std::string* diagnostic = nullptr) const;
		std::set<core::AgentTagId> const& getAgentTags(core::AgentId agent) const;

		// --- Agent groups ---
		std::uint32_t getAgentGroupCount() const;
		std::vector<core::AgentGroupId> getAgentGroupIds() const;
		core::EntityLookup<core::AgentGroup const> lookupAgentGroup(core::AgentGroupId id) const;
		std::string const& getAgentGroupName(core::AgentGroupId id) const;
		bool canSetAgentGroup(core::AgentId agent, core::AgentGroupId group, std::string* diagnostic = nullptr) const;
		core::AgentGroupId getAgentGroup(core::AgentId agent) const;
		bool isAgentGroupActive(core::AgentGroupId group) const;
		bool canSetAgentGroupActive(core::AgentGroupId group, bool active, std::string* diagnostic = nullptr) const;
		std::uint32_t getAgentGroupMemberCount(core::AgentGroupId id) const;

		// --- Access permissions and permission sets ---
		std::uint32_t getAccessPermissionCount() const;
		std::vector<core::AccessPermissionId> getAccessPermissionIds() const;
		core::EntityLookup<core::AccessPermission const> lookupAccessPermission(core::AccessPermissionId id) const;
		std::string const& getAccessPermissionName(core::AccessPermissionId id) const;
		core::World::AccessPermissionUsage getAccessPermissionUsage(core::AccessPermissionId id) const;
		std::vector<core::AccessPermissionId> getAgentDirectAccessGrants(core::AgentId agent) const;
		std::vector<core::AccessPermissionId> getAgentCurrentDirectAccessGrants(core::AgentId agent) const;
		std::vector<core::PermissionSetId> getAgentCurrentPermissionSetAssignments(core::AgentId agent) const;
		std::vector<core::AccessPermissionId> getAgentEffectiveAccessGrants(core::AgentId agent) const;
		core::World::EffectiveAccessGrantSources getAgentAccessGrantSources(core::AgentId agent, core::AccessPermissionId permission) const;
		std::uint32_t getPermissionSetCount() const;
		std::vector<core::PermissionSetId> getPermissionSetIds() const;
		core::EntityLookup<core::PermissionSet const> lookupPermissionSet(core::PermissionSetId id) const;
		std::string const& getPermissionSetName(core::PermissionSetId id) const;
		std::uint32_t getPermissionSetUsageCount(core::PermissionSetId id) const;
		std::vector<core::AccessPermissionId> getPermissionSetPermissions(core::PermissionSetId id) const;
		std::vector<core::PermissionSetId> getAgentPermissionSetAssignments(core::AgentId agent) const;
		bool isLocationPermissionEligible(std::uint32_t sectorIndex) const;
		std::vector<core::AccessPermissionId> getLocationPermissionRequirement(std::uint32_t sectorIndex) const;
		bool canAgentAccessLocation(core::Sector const& sector, core::Agent const& agent) const;
		bool isInteractionPointPermissionEligible(core::InteractionPointId point) const;
		std::vector<core::AccessPermissionId> getInteractionPointPermissionRequirement(core::InteractionPointId point) const;
		bool isManualDoorPermissionEligible(core::TraversalResourceId door) const;
		std::vector<core::AccessPermissionId> getManualDoorPermissionRequirement(core::TraversalResourceId door) const;

		// --- Transport state and authorization ---
		// Whether a Stop label is one the World will accept: 1-3 characters from
		// A-Z or 0-9. The rule is a property of the argument, not of the
		// document, so it is `static` on `World` (#963) and stays `static` here -
		// a plain forward with no World to read and nothing to guard. It belongs
		// on the read surface because a game that offers or validates Stop labels
		// needs the same rule the World enforces on the write, rather than a
		// second path into core to get it (#981).
		static bool isValidTransportStopShortName(std::string_view name);
		std::string getTransportStopShortName(std::uint32_t sectorIndex, std::uint32_t stopIndex, std::uint32_t objectIndex = ~0u) const;
		std::string getTransportStopShortName(core::TraversalResourceId resource, std::uint32_t stopIndex) const;
		std::vector<std::uint32_t> getLiftDestinationLevels(std::uint32_t sectorIndex, std::uint32_t objectIndex = ~0u) const;
		std::vector<core::AccessPermissionId> getLiftDestinationPermissionRequirement(std::uint32_t sectorIndex, std::uint32_t stopIndex, std::uint32_t objectIndex = ~0u) const;
		std::optional<core::DeviceCondition> knownTransportCondition(core::TraversalResourceId resource, core::Agent const* agent, core::Sector const* observationSector) const;
		std::optional<core::DeviceCondition> knownLiftCondition(core::TraversalResourceId resource, core::Agent const* agent, core::Sector const* observationSector) const;
		bool canAgentEnterAirlock(core::TraversalResourceId resource, core::SectorId approach, core::AgentId agent, bool locallyObserved) const;
		bool canAgentOpenManualDoor(core::TraversalResourceId door, core::AgentId agent) const;
		bool agentHasFeasibleManualDoorOperation(core::TraversalResourceId door, core::AgentId agent) const;
		std::optional<float> remoteButtonApproachDistance(core::InteractionPointId point, core::SectorId approach, core::Vector2 const& position, core::AgentId agent) const;
		bool canAgentOperateDoorControl(core::TraversalResourceId door, core::SectorId approach, core::AgentId agent) const;
		bool agentAdheresToDoorPermission(core::TraversalResourceId door, core::SectorId approach, core::AgentId agent) const;
		bool canAgentOperateExtensibleControl(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& approachPosition, core::AgentId agent) const;
		bool agentAdheresToExtensiblePermission(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& approachPosition, core::AgentId agent) const;
		std::vector<core::AccessPermissionId> missingLiftDestinationPermissions(core::DeviceCommand const& command, core::AgentId agent) const;
		bool canAgentUseLiftJourney(core::TraversalResourceId resource, core::Vector2 const& origin, core::Vector2 const& destination, core::AgentId agent) const;
		bool agentHasFeasibleLiftJourney(core::TraversalResourceId resource, core::Vector2 const& origin, core::Vector2 const& destination, core::AgentId agent) const;
		bool agentAdheresToLiftDestinationPermission(core::TraversalResourceId resource, std::uint32_t destinationStop, core::AgentId agent) const;
		bool canAgentOperateTransportLandingControl(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const;
		bool agentHasFeasibleTransportLandingOperation(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const;
		bool agentAdheresToTransportLandingPermission(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const;
		bool isAgentTransportOccupant(core::TraversalResourceId resource, core::AgentId agent) const;
		bool isTransportLocallyBoardable(core::TraversalResourceId resource, core::Vector2 const& endpoint) const;
		bool canAgentTraverseManualDoorNow(core::TraversalResourceId door, core::AgentId agent) const;

		// --- Interaction points, devices and Access panels ---
		core::EntityLookup<core::InteractionPoint const> lookupInteractionPoint(core::InteractionPointId id) const;
		std::shared_ptr<const core::AccessPanel> lookupAccessPanel(core::AccessPanelId id) const;
		bool canRequestAccessPanel(core::AccessPanelId id, core::AccessPanel::Action action, core::AgentId actor) const;
		core::EntityLookup<core::InteractionRequest const> lookupInteractionRequest(core::InteractionRequestId id) const;
		std::shared_ptr<const core::BoothWindow> lookupBoothWindow(core::BoothWindowId id) const;
		core::EntityLookup<core::DeviceOperation const> lookupDeviceOperation(core::DeviceOperationId id) const;
		core::EntityLookup<core::TraversalResource const> lookupTraversalResource(core::TraversalResourceId id) const;
		core::TraversalResourceId getTraversalResourceId(core::Object const* object) const;
		core::EntityLookup<core::TraversalRequest const> lookupTraversalRequest(core::TraversalRequestId id) const;
		bool isAgentInQueue(core::AgentId id) const;

		// --- Routing policy and live observation ---
		core::RouteChoicePolicy const& getRouteChoicePolicy() const;
		core::TraversalWaitingPolicy const& getTraversalWaitingPolicy() const;
		core::TraversalGeometryPolicy const& getTraversalGeometryPolicy() const;
		float estimateTraversalDelay(core::TraversalResourceId resource, core::SectorId sourceSector) const;
		float observeAccessZoneDensity(core::TraversalResourceId resource, core::SectorId sourceSector) const;
		std::optional<core::ShuttleRouteAccessObservation> observeShuttleAccess(core::TraversalResourceId resource, core::Vector2 const& endpoint, bool includeLocalQueue) const;
		std::optional<core::LiftRouteAccessObservation> observeLiftAccess(core::TraversalResourceId resource, core::Vector2 const& sourceEndpoint, bool includeLocalQueue = true) const;
		std::uint32_t countStandingAgentsOnEscalator(core::Agent const* observer, core::Edge const* edge) const;

		// --- Furniture ---
		std::shared_ptr<const core::FurnitureCatalogue> const& furnitureCatalogue() const;
		std::vector<core::FurnitureInstance> const& furniture() const;
		bool isFurnitureMarker(core::MarkerId id) const;
		core::AgentId usablePointOccupant(core::MarkerId id) const;

		// --- Simulation clock reads ---
		double getDeferredSimulationTime() const;
		double getAverageSimulationStepMicroseconds() const;
		std::vector<core::SimulationEvent> const& getSimulationEvents() const;

	private:
		// Exclusive ownership. Nothing else in the process holds this World: the
		// runtime is not copyable, no shared_ptr is ever formed from it, and the
		// only public accessor is the const view above.
		std::unique_ptr<core::World> world_;

		// The dereference every accessor in this class goes through (#978).
		//
		// `world_` is non-null for the whole life of a constructed runtime:
		// both ctors guarantee it and the moves are deleted above, so there is
		// no moved-from `WorldRuntime` in existence to guard against (#977).
		// This is the backstop for the day that stops being true - a default
		// ctor, a release-the-World method, a move put back - and it turns what
		// would otherwise be a null dereference at whichever call site happened
		// to touch it into a `std::logic_error` naming the accessor and the
		// condition. The check lives here rather than at each of the roughly
		// forty dereference sites so an accessor cannot forget it.
		//
		// The const overload answers a read, the non-const one the drive and
		// authoring surfaces; both raise the same failure.
		core::World& requireLive(char const* accessor);

		core::World const& requireLive(char const* accessor) const;

		// The mutable view of the owned World. Private, and granted to
		// WorldAuthoring alone (#967): ownership stays here, while the authoring
		// handle wrapped over the runtime is the only code in the process that
		// can turn this World back into a mutable one. Absent that grant there
		// is no path from a WorldRuntime to a `core::World&` at all, which is
		// what makes "authoring is the only mutable surface" a compile-time
		// fact rather than a convention - and why a game linking only
		// pf-game-runtime cannot reach mutable World state by any route.
		// Throws `std::logic_error` when the runtime holds no World, for the
		// same reason as `world()` above (#978).
		core::World& authoringWorld();

		friend class WorldAuthoring;
	};
}
