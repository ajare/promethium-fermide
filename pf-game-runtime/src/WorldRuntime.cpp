#include "game/WorldRuntime.h"

#include <stdexcept>
#include <utility>

#include "core/World.h"
#include "game/MovedFrom.h"

namespace game
{
	WorldRuntime::WorldRuntime(std::unique_ptr<core::World> world)
		: world_(std::move(world))
	{
		if (!world_)
			throw std::invalid_argument("WorldRuntime requires a World to own");
	}

	WorldRuntime::WorldRuntime(std::string const& name, std::uint32_t cellsWide,
		std::uint32_t levelsHigh)
		: world_(std::make_unique<core::World>(name, cellsWide, levelsHigh))
	{
	}

	WorldRuntime::~WorldRuntime() = default;

	// The move constructor and move assignment are deleted in the header (#977),
	// so there is no moved-from WorldRuntime in existence to reason about: no
	// shell, no null `world_`, and no authoring handle left pointing at one.
	// What is left to police is the dereference itself, so that the day a null
	// `world_` becomes reachable again the failure is named rather than silent
	// (#978). Every read of `world_` in this file goes through `requireLive`.

	core::World& WorldRuntime::requireLive(char const* accessor)
	{
		if (!world_) refuseMovedFrom(accessor);
		return *world_;
	}

	core::World const& WorldRuntime::requireLive(char const* accessor) const
	{
		if (!world_) refuseMovedFrom(accessor);
		return *world_;
	}

	core::World const& WorldRuntime::world() const
	{
		return requireLive("WorldRuntime::world");
	}

	core::World& WorldRuntime::authoringWorld()
	{
		// Reachable only from WorldAuthoring, which is declared a friend: the
		// single widening of the const view in the whole boundary (#967).
		return requireLive("WorldRuntime::authoringWorld");
	}

	void WorldRuntime::update(float elapsedSeconds)
	{
		requireLive("WorldRuntime::update").update(elapsedSeconds);
	}

	bool WorldRuntime::advanceTick()
	{
		return requireLive("WorldRuntime::advanceTick").advanceTick();
	}

	bool WorldRuntime::advanceTicks(std::uint64_t count)
	{
		return requireLive("WorldRuntime::advanceTicks").advanceTicks(count);
	}

	void WorldRuntime::pauseSimulation()
	{
		requireLive("WorldRuntime::pauseSimulation").pauseSimulation();
	}

	bool WorldRuntime::resumeSimulation()
	{
		return requireLive("WorldRuntime::resumeSimulation").resumeSimulation();
	}

	void WorldRuntime::resetSimulation()
	{
		requireLive("WorldRuntime::resetSimulation").resetSimulation();
	}

	bool WorldRuntime::isSimulationPaused() const
	{
		return requireLive("WorldRuntime::isSimulationPaused").isSimulationPaused();
	}

	void WorldRuntime::wakeAllAgents()
	{
		requireLive("WorldRuntime::wakeAllAgents").wakeAllAgents();
	}

	void WorldRuntime::setSimulationTimeScale(double scale)
	{
		requireLive("WorldRuntime::setSimulationTimeScale").setSimulationTimeScale(scale);
	}

	double WorldRuntime::getSimulationTimeScale() const
	{
		return requireLive("WorldRuntime::getSimulationTimeScale").getSimulationTimeScale();
	}

	void WorldRuntime::finishBuild()
	{
		requireLive("WorldRuntime::finishBuild").finishBuild();
	}

	bool WorldRuntime::rebuildTraversalTopology()
	{
		return requireLive("WorldRuntime::rebuildTraversalTopology").rebuildTraversalTopology();
	}

	core::AgentId WorldRuntime::getPlayerControlledAgent() const
	{
		return requireLive("WorldRuntime::getPlayerControlledAgent").getPlayerControlledAgent();
	}

	void WorldRuntime::setPlayerMovementDirection(int direction)
	{
		requireLive("WorldRuntime::setPlayerMovementDirection").setPlayerMovementDirection(direction);
	}

	void WorldRuntime::setPlayerStairSelection(bool up, bool down)
	{
		requireLive("WorldRuntime::setPlayerStairSelection").setPlayerStairSelection(up, down);
	}

	void WorldRuntime::setPlayerCrouch(bool held)
	{
		requireLive("WorldRuntime::setPlayerCrouch").setPlayerCrouch(held);
	}

	void WorldRuntime::requestPlayerDoorAction()
	{
		requireLive("WorldRuntime::requestPlayerDoorAction").requestPlayerDoorAction();
	}

	std::optional<core::World::PlayerTransportSelector> WorldRuntime::getPlayerTransportSelector() const
	{
		return requireLive("WorldRuntime::getPlayerTransportSelector").getPlayerTransportSelector();
	}

	void WorldRuntime::browsePlayerTransportStop(int direction, bool horizontal)
	{
		requireLive("WorldRuntime::browsePlayerTransportStop").browsePlayerTransportStop(direction, horizontal);
	}

	void WorldRuntime::confirmPlayerTransportStop()
	{
		requireLive("WorldRuntime::confirmPlayerTransportStop").confirmPlayerTransportStop();
	}

	void WorldRuntime::clearPlayerInput()
	{
		requireLive("WorldRuntime::clearPlayerInput").clearPlayerInput();
	}

	std::uint64_t WorldRuntime::getSimulationTick() const
	{
		return requireLive("WorldRuntime::getSimulationTick").getSimulationTick();
	}

	core::SimulationPhase WorldRuntime::getCurrentSimulationPhase() const
	{
		return requireLive("WorldRuntime::getCurrentSimulationPhase").getCurrentSimulationPhase();
	}

	core::SimulationSnapshot WorldRuntime::getSimulationSnapshot() const
	{
		return requireLive("WorldRuntime::getSimulationSnapshot").getSimulationSnapshot();
	}

	core::SimulationSnapshot const& WorldRuntime::getSimulationSnapshotView() const
	{
		return requireLive("WorldRuntime::getSimulationSnapshotView").getSimulationSnapshotView();
	}

	std::uint64_t WorldRuntime::getSimulationSnapshotBuildCount() const
	{
		return requireLive("WorldRuntime::getSimulationSnapshotBuildCount").getSimulationSnapshotBuildCount();
	}

	std::vector<core::SimulationEvent> WorldRuntime::consumeSimulationEvents()
	{
		return requireLive("WorldRuntime::consumeSimulationEvents").consumeSimulationEvents();
	}

	void WorldRuntime::setSimulationObserver(core::SimulationObserver* observer)
	{
		requireLive("WorldRuntime::setSimulationObserver").setSimulationObserver(observer);
	}

	// The two pacing constants are deliberately not defined here. They are
	// `constexpr`, so their bodies have to be visible in every translation
	// unit that folds them, which puts them in the header (#983).

	// --- Resource names -------------------------------------------------
	// Reads through the same const view rendering gets. There is no widening
	// here: the names are read off `world_` exactly as a `core::World const&`
	// would read them, and the binding acts that would change them are not on
	// this type at all (#969).

	namespace
	{
		// The one empty value the unbound reads answer with (#980). A single
		// object shared by the whole family, so the reference handed back
		// outlives every caller and two unbound reads of the same package read
		// the same value rather than two equal-but-distinct ones.
		std::string const& noResource()
		{
			static const std::string kNoResource;
			return kNoResource;
		}

		// The guard the resource-name reads and the expected-UUID reads share
		// (#980). `core::World`'s own getters throw when nothing is
		// referenced, because the editor uses that as a programming-error
		// signal. The runtime is a read surface, not an authoring one, and a
		// document bound to no package is a normal state rather than a fault:
		// the absence is answered here instead of faulting a render or game
		// path - and instead of arriving as a `std::runtime_error` thrown in
		// another module and crossing the shared-library boundary into a game
		// loop that has no way to author the reference it is missing.
		//
		// `read` is taken only when the reference exists, which is what keeps
		// the throwing World getter unreachable from this type. It must return
		// `std::string const&` explicitly: a lambda that lets its return type
		// deduce returns a copy, and the guard would then hand back a
		// reference to a temporary that dies at the end of the call.
		template <typename Read>
		std::string const& readResourceString(bool hasReference, Read read)
		{
			return hasReference ? read() : noResource();
		}
	}

	std::string const& WorldRuntime::agentTagRegistryResourceName() const
	{
		auto const& world = requireLive("WorldRuntime::agentTagRegistryResourceName");
		return readResourceString(world.hasAgentTagRegistryReference(),
			[&world]() -> std::string const& { return world.getAgentTagRegistryResourceName(); });
	}

	std::string const& WorldRuntime::agentBehaviourRegistryResourceName() const
	{
		auto const& world = requireLive("WorldRuntime::agentBehaviourRegistryResourceName");
		return readResourceString(world.hasAgentBehaviourRegistryReference(),
			[&world]() -> std::string const& { return world.getAgentBehaviourRegistryResourceName(); });
	}

	std::string const& WorldRuntime::furnitureCatalogueResourceName() const
	{
		return requireLive("WorldRuntime::furnitureCatalogueResourceName").furnitureCatalogueResourceName();
	}

	std::string const& WorldRuntime::sectorImageSetResourceName() const
	{
		return requireLive("WorldRuntime::sectorImageSetResourceName").sectorImageSetResourceName();
	}

	std::string const& WorldRuntime::objectImageSetResourceName() const
	{
		return requireLive("WorldRuntime::objectImageSetResourceName").objectImageSetResourceName();
	}

	std::string const& WorldRuntime::agentImageSetResourceName() const
	{
		return requireLive("WorldRuntime::agentImageSetResourceName").agentImageSetResourceName();
	}

	std::string const& WorldRuntime::furnitureImageSetResourceName() const
	{
		return requireLive("WorldRuntime::furnitureImageSetResourceName").furnitureImageSetResourceName();
	}

	std::string const& WorldRuntime::actionRegistryFilename() const
	{
		return requireLive("WorldRuntime::actionRegistryFilename").actionRegistryFilename();
	}

// --- The read surface ---------------------------------------------
// Every method below is a `const` forward onto the same-named `const`
// World method (#966, spec #960). There is no widening here: `world_` is
// read through the very same const view `world()` hands to rendering, so
// a read through the runtime and a read through the render view are the
// same call on the same object. The grouping mirrors the header's, and
// each family answers for a different part of the document.
//
// The two `static` forwards below are the exception to "same const view":
// they read no World state at all, so there is no view to go through and
// no `requireLive` guard to run. They are on the read surface because the
// audit that defines it counts a stateless public `World` static as a read
// (#981), not despite that.

// --- Identity, host state and diagnostics ---
std::string const& WorldRuntime::getName() const
{
	return world().getName();
}
std::string const& WorldRuntime::getClipboardIdentity() const
{
	return world().getClipboardIdentity();
}
std::weak_ptr<void const> WorldRuntime::getLifetimeToken() const
{
	return world().getLifetimeToken();
}
std::uint64_t WorldRuntime::getRandomSeed() const
{
	return world().getRandomSeed();
}
std::filesystem::path const& WorldRuntime::documentDirectory() const
{
	return world().documentDirectory();
}
std::vector<std::string> const& WorldRuntime::getLoadWarnings() const
{
	return world().getLoadWarnings();
}
std::string WorldRuntime::atlasFamilyDiagnostic() const
{
	return world().atlasFamilyDiagnostic();
}
core::Log const& WorldRuntime::getBuildLog() const
{
	return world().getBuildLog();
}

// --- World dimensions, Layers and Levels ---
std::uint32_t WorldRuntime::getCellsWide() const
{
	return world().getCellsWide();
}
std::uint32_t WorldRuntime::getLevelsHigh() const
{
	return world().getLevelsHigh();
}
std::uint32_t WorldRuntime::getLayerCount() const
{
	return world().getLayerCount();
}
std::string const& WorldRuntime::getLayerName(std::uint32_t layerIndex) const
{
	return world().getLayerName(layerIndex);
}
std::string const& WorldRuntime::getLevelName(std::uint32_t level) const
{
	return world().getLevelName(level);
}
std::uint32_t WorldRuntime::getNumSectors() const
{
	return world().getNumSectors();
}
std::shared_ptr<const core::Layer> WorldRuntime::getLayer(std::uint32_t layerIndex) const
{
	return world().getLayer(layerIndex);
}
std::shared_ptr<const core::Graph> WorldRuntime::getGraph() const
{
	return world().getGraph();
}
bool WorldRuntime::dimensionsAreSupported(std::uint32_t cellsWide,
	std::uint32_t levelsHigh, std::uint32_t layerCount, std::string* diagnostic)
{
	return core::World::dimensionsAreSupported(cellsWide, levelsHigh, layerCount,
		diagnostic);
}

// --- Sectors and spatial lookup ---
std::shared_ptr<const core::Sector> WorldRuntime::getSector(std::uint32_t index) const
{
	return world().getSector(index);
}
std::vector<std::shared_ptr<const core::Sector>> WorldRuntime::getSectorsInBounds(std::uint32_t layerIndex, float x, float y, float width, float height) const
{
	return world().getSectorsInBounds(layerIndex, x, y, width, height);
}
std::vector<std::shared_ptr<const core::Sector>> WorldRuntime::getSectors(std::uint32_t layerIndex) const
{
	return world().getSectors(layerIndex);
}
std::shared_ptr<const core::Sector> WorldRuntime::getSectorAtPosition(std::uint32_t layerIndex, float x, float y) const
{
	return world().getSectorAtPosition(layerIndex, x, y);
}
core::Agent const* WorldRuntime::getAgentAtPosition(std::uint32_t layerIndex, float x, float y) const
{
	// Through `world()`, so the `const` overload is the only one this forward
	// can select (#974).
	return world().getAgentAtPosition(layerIndex, x, y);
}
std::shared_ptr<const core::Object> WorldRuntime::getObjectAtPosition(std::uint32_t layerIndex, float x, float y, std::shared_ptr<const core::SectorObject>* sectorObject) const
{
	return world().getObjectAtPosition(layerIndex, x, y, sectorObject);
}

// --- Traversal topology state ---
bool WorldRuntime::getPausedPathIntent(core::Agent const& agent, core::World::TopologyPathIntent& intent) const
{
	return world().getPausedPathIntent(agent, intent);
}
bool WorldRuntime::isTraversalTopologyDirty() const
{
	return world().isTraversalTopologyDirty();
}
bool WorldRuntime::isTraversalTopologyValid() const
{
	return world().isTraversalTopologyValid();
}
std::uint64_t WorldRuntime::getTopologyGeneration() const
{
	return world().getTopologyGeneration();
}
std::string const& WorldRuntime::getTopologyDiagnostic() const
{
	return world().getTopologyDiagnostic();
}

// --- SectorObject ownership classification ---
bool WorldRuntime::hasDumbwaiters() const
{
	return world().hasDumbwaiters();
}
std::shared_ptr<const core::Dumbwaiter> WorldRuntime::lookupDumbwaiter(core::DumbwaiterId id) const
{
	return world().lookupDumbwaiter(id);
}
bool WorldRuntime::isDumbwaiterOwnedControl(std::shared_ptr<const core::SectorObject> const& object) const
{
	return world().isDumbwaiterOwnedControl(object);
}
bool WorldRuntime::isAirlockOwnedObject(std::shared_ptr<const core::SectorObject> const& object) const
{
	return world().isAirlockOwnedObject(object);
}
bool WorldRuntime::isChamberOwnedObject(std::shared_ptr<const core::SectorObject> const& object) const
{
	return world().isChamberOwnedObject(object);
}
bool WorldRuntime::isLiftOwnedDoor(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* liftSectorIndex, std::uint32_t* stopIndex) const
{
	return world().isLiftOwnedDoor(object, liftSectorIndex, stopIndex);
}
bool WorldRuntime::isLiftOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* liftSectorIndex, std::uint32_t* stopIndex) const
{
	return world().isLiftOwnedControl(object, liftSectorIndex, stopIndex);
}
bool WorldRuntime::isShuttleOwnedDoor(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* shuttleSectorIndex, std::uint32_t* stopIndex, std::uint32_t* carriageIndex, std::uint32_t* doorIndex) const
{
	return world().isShuttleOwnedDoor(object, shuttleSectorIndex, stopIndex, carriageIndex, doorIndex);
}
bool WorldRuntime::isShuttleOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* shuttleSectorIndex, std::uint32_t* stopIndex) const
{
	return world().isShuttleOwnedControl(object, shuttleSectorIndex, stopIndex);
}
bool WorldRuntime::isBulkheadDoorOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* doorSectorIndex, std::uint32_t* doorObjectIndex) const
{
	return world().isBulkheadDoorOwnedControl(object, doorSectorIndex, doorObjectIndex);
}
bool WorldRuntime::isForceBridgeOwnedControl(std::shared_ptr<const core::SectorObject> const& object, std::uint32_t* forceBridgeSectorIndex, std::uint32_t* forceBridgeObjectIndex) const
{
	return world().isForceBridgeOwnedControl(object, forceBridgeSectorIndex, forceBridgeObjectIndex);
}
bool WorldRuntime::isControlTargetBroken(core::InteractionPointId point) const
{
	return world().isControlTargetBroken(point);
}

// --- SectorObject authored options ---
bool WorldRuntime::getLadderOptions(std::uint32_t sectorIndex, core::World::CreateLadderOptions& options) const
{
	return world().getLadderOptions(sectorIndex, options);
}
bool WorldRuntime::getStairwellOptions(std::uint32_t sectorIndex, core::World::CreateStairwellOptions& options) const
{
	return world().getStairwellOptions(sectorIndex, options);
}
bool WorldRuntime::getStaircaseOptions(std::uint32_t sectorIndex, core::World::CreateStaircaseOptions& options) const
{
	return world().getStaircaseOptions(sectorIndex, options);
}
std::vector<core::World::LiftLandingRow> WorldRuntime::getLiftLandingRows(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t levelsHigh) const
{
	return world().getLiftLandingRows(layerIndex, y, x, cellsWide, levelsHigh);
}
bool WorldRuntime::getLiftLandingGeometry(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t& landingX, std::uint32_t& landingWidth) const
{
	return world().getLiftLandingGeometry(layerIndex, y, x, landingX, landingWidth);
}
std::vector<std::uint32_t> WorldRuntime::getValidShuttleStopOffsets(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t numCars, std::uint32_t carWidth, bool allowPartialLandings, std::uint32_t doorMask) const
{
	return world().getValidShuttleStopOffsets(layerIndex, y, x, cellsWide, numCars, carWidth, allowPartialLandings, doorMask);
}
bool WorldRuntime::getShuttleOptions(core::Shuttle const* shuttle, core::World::CreateShuttleOptions& options) const
{
	return world().getShuttleOptions(shuttle, options);
}
std::vector<core::World::ShuttleStopCandidate> WorldRuntime::getShuttleStopCandidatesForDoor(std::uint32_t shuttleLayer, std::uint32_t y, std::uint32_t doorX) const
{
	return world().getShuttleStopCandidatesForDoor(shuttleLayer, y, doorX);
}
bool WorldRuntime::getSectorDoorOptions(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t width, core::World::CreateDoorOptions& options) const
{
	return world().getSectorDoorOptions(layerIndex, y, x, width, options);
}
bool WorldRuntime::getSectorWindowOptions(std::uint32_t layerIndex, std::uint32_t y, std::uint32_t x, std::uint32_t cellsWide, std::uint32_t levelsHigh, core::World::CreateWindowOptions& options) const
{
	return world().getSectorWindowOptions(layerIndex, y, x, cellsWide, levelsHigh, options);
}
bool WorldRuntime::getSectorBulkheadDoorOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateBulkheadDoorOptions& options) const
{
	return world().getSectorBulkheadDoorOptions(sectorIndex, objectIndex, options);
}
bool WorldRuntime::calculateSectorForceBridgeWidthToRight(std::uint32_t sectorIndex, std::uint32_t levelIndex, std::uint32_t xOffset, std::uint32_t& width, std::string* diagnostic) const
{
	return world().calculateSectorForceBridgeWidthToRight(sectorIndex, levelIndex, xOffset, width, diagnostic);
}
bool WorldRuntime::getSectorForceBridgeOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateForceBridgeOptions& options) const
{
	return world().getSectorForceBridgeOptions(sectorIndex, objectIndex, options);
}
bool WorldRuntime::getRoomLadderOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateLadderOptions& options) const
{
	return world().getRoomLadderOptions(sectorIndex, objectIndex, options);
}
std::vector<core::World::PlatformLiftStopCandidate> WorldRuntime::getPlatformLiftStopCandidates(std::uint32_t sectorIndex, std::uint32_t xOffset) const
{
	return world().getPlatformLiftStopCandidates(sectorIndex, xOffset);
}
bool WorldRuntime::getPlatformLiftOptions(std::uint32_t sectorIndex, std::uint32_t objectIndex, core::World::CreateLiftOptions& options) const
{
	return world().getPlatformLiftOptions(sectorIndex, objectIndex, options);
}

// --- Markers and Agent actions ---
std::vector<std::string> WorldRuntime::availableAgentActions(core::MarkerId marker) const
{
	return world().availableAgentActions(marker);
}
std::vector<std::string> WorldRuntime::markerActions(core::MarkerId marker) const
{
	return world().markerActions(marker);
}
std::string WorldRuntime::agentActionDisplayName(std::string_view identity) const
{
	return world().agentActionDisplayName(identity);
}
std::vector<core::MarkerId> WorldRuntime::getMarkerIds() const
{
	return world().getMarkerIds();
}
std::shared_ptr<const core::Marker> WorldRuntime::lookupMarker(core::MarkerId id) const
{
	return world().lookupMarker(id);
}
bool WorldRuntime::furnitureUseEligible(core::AgentId agent, core::MarkerId marker, std::string* diagnostic) const
{
	return world().furnitureUseEligible(agent, marker, diagnostic);
}
bool WorldRuntime::agentPoseStateFits(core::Agent const& agent, float clearance, float heightFactor) const
{
	return world().agentPoseStateFits(agent, clearance, heightFactor);
}
bool WorldRuntime::agentHeightStateFits(core::Agent const& agent, float next, std::string* diagnostic) const
{
	return world().agentHeightStateFits(agent, next, diagnostic);
}
void WorldRuntime::validateReplayPoseState(core::World const& candidate, std::uint32_t movedSectorIndex, int deltaX, int deltaY) const
{
	return world().validateReplayPoseState(candidate, movedSectorIndex, deltaX, deltaY);
}
bool WorldRuntime::furniturePoseFits(core::AgentId agent, core::MarkerId marker, core::Pose pose, bool usingSupport, std::string* diagnostic) const
{
	return world().furniturePoseFits(agent, marker, pose, usingSupport, diagnostic);
}
[[nodiscard]] float WorldRuntime::maximumInstalledStandingWidth() const
{
	return world().maximumInstalledStandingWidth();
}

// --- Agent types ---
bool WorldRuntime::hasAgentType(std::string_view typeId) const
{
	return world().hasAgentType(typeId);
}
std::string WorldRuntime::agentTypeDisplayName(std::string_view typeId) const
{
	return world().agentTypeDisplayName(typeId);
}
std::string WorldRuntime::agentTypeResourceName(std::string_view typeId) const
{
	return world().agentTypeResourceName(typeId);
}
std::optional<core::AgentTypeDefinition> WorldRuntime::registeredAgentTypeDefinition(std::string_view typeId, std::string_view resourceName) const
{
	return world().registeredAgentTypeDefinition(typeId, resourceName);
}
std::optional<core::AgentPhysicalBaseline> WorldRuntime::registeredAgentTypeBaseline(std::string_view typeId, std::string_view resourceName) const
{
	return world().registeredAgentTypeBaseline(typeId, resourceName);
}

// --- Agents ---
void WorldRuntime::validateAgentLocationPlacement(core::Sector const& sector, core::Agent const& agent, std::optional<float> floorY) const
{
	return world().validateAgentLocationPlacement(sector, agent, floorY);
}
core::EntityLookup<core::Agent const> WorldRuntime::lookupAgent(core::AgentId id) const
{
	return world().lookupAgent(id);
}
bool WorldRuntime::canSetAgentActive(core::AgentId agent, bool active, std::string* diagnostic) const
{
	return world().canSetAgentActive(agent, active, diagnostic);
}
core::AgentId WorldRuntime::getAgentId(core::Agent const* agent) const
{
	return world().getAgentId(agent);
}

// --- Agent behaviour assignment and runtime ---
bool WorldRuntime::hasAgentBehaviourRegistryReference() const
{
	return world().hasAgentBehaviourRegistryReference();
}
bool WorldRuntime::hasAttachedAgentBehaviourRegistry() const
{
	return world().hasAttachedAgentBehaviourRegistry();
}
std::string const& WorldRuntime::getExpectedAgentBehaviourRegistryUuid() const
{
	// Same contract as the resource-name family above (#980): the document
	// expecting no package reads as empty here. The throwing form stays on
	// `core::World`, reached through `world()`, for the editor's save path.
	// `requireLive` is spelled out rather than reached through `world()` so
	// that this reads as one shape with the resource-name getters above.
	auto const& world = requireLive("WorldRuntime::getExpectedAgentBehaviourRegistryUuid");
	return readResourceString(world.hasAgentBehaviourRegistryReference(),
		[&world]() -> std::string const& { return world.getExpectedAgentBehaviourRegistryUuid(); });
}
std::shared_ptr<const core::AgentBehaviourRegistry> WorldRuntime::getAgentBehaviourRegistry() const
{
	// A `const` read answers with a `const` handle (#974). The runtime has no
	// mutable registry to hand out: binding and mutating the package is
	// `WorldAuthoring`'s act, not the game's (#969).
	return world().getAgentBehaviourRegistry();
}
bool WorldRuntime::agentBehaviourConfigurationsAreValid() const
{
	return world().agentBehaviourConfigurationsAreValid();
}
std::string const& WorldRuntime::getAgentBehaviourDependencyDiagnostic() const
{
	return world().getAgentBehaviourDependencyDiagnostic();
}
bool WorldRuntime::validateAgentBehaviourAssignment(core::AgentBehaviourId behaviour, std::uint64_t revision, core::AgentBehaviourConfiguration const& configuration, core::AgentBehaviourConfiguration* normalized, std::string* diagnostic) const
{
	return world().validateAgentBehaviourAssignment(behaviour, revision, configuration, normalized, diagnostic);
}
std::optional<core::AgentBehaviourAssignment> const& WorldRuntime::getAgentBehaviourAssignment(core::AgentId agent) const
{
	return world().getAgentBehaviourAssignment(agent);
}
std::uint32_t WorldRuntime::getAgentBehaviourAssignmentCount() const
{
	return world().getAgentBehaviourAssignmentCount();
}
bool WorldRuntime::agentBehaviourOwnsMovement(core::AgentId agent) const
{
	return world().agentBehaviourOwnsMovement(agent);
}
std::vector<core::AgentBehaviourRuntimeDiagnostic> WorldRuntime::getAgentBehaviourRuntimeDiagnostics() const
{
	return world().getAgentBehaviourRuntimeDiagnostics();
}
core::AgentBehaviourRuntimeLimits WorldRuntime::getAgentBehaviourRuntimeLimits() const
{
	return world().getAgentBehaviourRuntimeLimits();
}
std::shared_ptr<const core::ActionRegistry> const& WorldRuntime::actionRegistry() const
{
	return world().actionRegistry();
}

// --- Agent tags ---
bool WorldRuntime::hasAgentTagRegistryReference() const
{
	return world().hasAgentTagRegistryReference();
}
bool WorldRuntime::hasAttachedAgentTagRegistry() const
{
	return world().hasAttachedAgentTagRegistry();
}
std::string const& WorldRuntime::getExpectedAgentTagRegistryUuid() const
{
	// Same contract as the resource-name family above (#980): the document
	// expecting no package reads as empty here. The throwing form stays on
	// `core::World`, reached through `world()`, for the editor's save path.
	// `requireLive` is spelled out rather than reached through `world()` so
	// that this reads as one shape with the resource-name getters above.
	auto const& world = requireLive("WorldRuntime::getExpectedAgentTagRegistryUuid");
	return readResourceString(world.hasAgentTagRegistryReference(),
		[&world]() -> std::string const& { return world.getExpectedAgentTagRegistryUuid(); });
}
std::shared_ptr<const core::AgentTagRegistry> WorldRuntime::getAgentTagRegistry() const
{
	// A `const` read answers with a `const` handle (#974), matching
	// `furnitureCatalogue()` and `lookupDumbwaiter()`.
	return world().getAgentTagRegistry();
}
std::uint64_t WorldRuntime::getAgentTagAssignmentCount() const
{
	return world().getAgentTagAssignmentCount();
}
std::uint32_t WorldRuntime::getAgentTagAssignedAgentCount() const
{
	return world().getAgentTagAssignedAgentCount();
}
std::uint64_t WorldRuntime::getAgentTagSampleCount() const
{
	return world().getAgentTagSampleCount();
}
bool WorldRuntime::canAssignAgentTag(core::AgentId agent, core::AgentTagId tag, std::string* diagnostic) const
{
	return world().canAssignAgentTag(agent, tag, diagnostic);
}
bool WorldRuntime::validateAgentTagAssignments(std::set<core::AgentTagId> const& tags, std::optional<core::AgentPropertySample> const& walkSpeedSample, std::optional<core::AgentPropertySample> const& heightSample, std::optional<core::AgentPropertySample> const& stairSpeedSample, std::optional<core::AgentPropertySample> const& ladderSpeedSample, std::optional<core::AgentPropertySample> const& interactionAversionSample, std::optional<core::AgentPropertySample> const& effortAversionSample, std::optional<core::AgentPropertySample> const& waitingAversionSample, std::optional<core::AgentPropertySample> const& crowdAversionSample, std::optional<core::AgentPropertySample> const& riskAversionSample, std::optional<core::AgentPropertySample> const& routeFamiliaritySample, std::optional<core::AgentPropertySample> const& routePersistenceSample, std::optional<core::AgentPropertySample> const& minimumRoutePlanningTimeSample, std::optional<core::AgentPropertySample> const& maximumRoutePlanningTimeSample, std::string* diagnostic) const
{
	return world().validateAgentTagAssignments(tags, walkSpeedSample, heightSample, stairSpeedSample, ladderSpeedSample, interactionAversionSample, effortAversionSample, waitingAversionSample, crowdAversionSample, riskAversionSample, routeFamiliaritySample, routePersistenceSample, minimumRoutePlanningTimeSample, maximumRoutePlanningTimeSample, diagnostic);
}
std::set<core::AgentTagId> const& WorldRuntime::getAgentTags(core::AgentId agent) const
{
	return world().getAgentTags(agent);
}

// --- Agent groups ---
std::uint32_t WorldRuntime::getAgentGroupCount() const
{
	return world().getAgentGroupCount();
}
std::vector<core::AgentGroupId> WorldRuntime::getAgentGroupIds() const
{
	return world().getAgentGroupIds();
}
core::EntityLookup<core::AgentGroup const> WorldRuntime::lookupAgentGroup(core::AgentGroupId id) const
{
	return world().lookupAgentGroup(id);
}
std::string const& WorldRuntime::getAgentGroupName(core::AgentGroupId id) const
{
	return world().getAgentGroupName(id);
}
bool WorldRuntime::canSetAgentGroup(core::AgentId agent, core::AgentGroupId group, std::string* diagnostic) const
{
	return world().canSetAgentGroup(agent, group, diagnostic);
}
core::AgentGroupId WorldRuntime::getAgentGroup(core::AgentId agent) const
{
	return world().getAgentGroup(agent);
}
bool WorldRuntime::isAgentGroupActive(core::AgentGroupId group) const
{
	return world().isAgentGroupActive(group);
}
bool WorldRuntime::canSetAgentGroupActive(core::AgentGroupId group, bool active, std::string* diagnostic) const
{
	return world().canSetAgentGroupActive(group, active, diagnostic);
}
std::uint32_t WorldRuntime::getAgentGroupMemberCount(core::AgentGroupId id) const
{
	return world().getAgentGroupMemberCount(id);
}

// --- Access permissions and permission sets ---
std::uint32_t WorldRuntime::getAccessPermissionCount() const
{
	return world().getAccessPermissionCount();
}
std::vector<core::AccessPermissionId> WorldRuntime::getAccessPermissionIds() const
{
	return world().getAccessPermissionIds();
}
core::EntityLookup<core::AccessPermission const> WorldRuntime::lookupAccessPermission(core::AccessPermissionId id) const
{
	return world().lookupAccessPermission(id);
}
std::string const& WorldRuntime::getAccessPermissionName(core::AccessPermissionId id) const
{
	return world().getAccessPermissionName(id);
}
core::World::AccessPermissionUsage WorldRuntime::getAccessPermissionUsage(core::AccessPermissionId id) const
{
	return world().getAccessPermissionUsage(id);
}
std::vector<core::AccessPermissionId> WorldRuntime::getAgentDirectAccessGrants(core::AgentId agent) const
{
	return world().getAgentDirectAccessGrants(agent);
}
std::vector<core::AccessPermissionId> WorldRuntime::getAgentCurrentDirectAccessGrants(core::AgentId agent) const
{
	return world().getAgentCurrentDirectAccessGrants(agent);
}
std::vector<core::PermissionSetId> WorldRuntime::getAgentCurrentPermissionSetAssignments(core::AgentId agent) const
{
	return world().getAgentCurrentPermissionSetAssignments(agent);
}
std::vector<core::AccessPermissionId> WorldRuntime::getAgentEffectiveAccessGrants(core::AgentId agent) const
{
	return world().getAgentEffectiveAccessGrants(agent);
}
core::World::EffectiveAccessGrantSources WorldRuntime::getAgentAccessGrantSources(core::AgentId agent, core::AccessPermissionId permission) const
{
	return world().getAgentAccessGrantSources(agent, permission);
}
std::uint32_t WorldRuntime::getPermissionSetCount() const
{
	return world().getPermissionSetCount();
}
std::vector<core::PermissionSetId> WorldRuntime::getPermissionSetIds() const
{
	return world().getPermissionSetIds();
}
core::EntityLookup<core::PermissionSet const> WorldRuntime::lookupPermissionSet(core::PermissionSetId id) const
{
	return world().lookupPermissionSet(id);
}
std::string const& WorldRuntime::getPermissionSetName(core::PermissionSetId id) const
{
	return world().getPermissionSetName(id);
}
std::uint32_t WorldRuntime::getPermissionSetUsageCount(core::PermissionSetId id) const
{
	return world().getPermissionSetUsageCount(id);
}
std::vector<core::AccessPermissionId> WorldRuntime::getPermissionSetPermissions(core::PermissionSetId id) const
{
	return world().getPermissionSetPermissions(id);
}
std::vector<core::PermissionSetId> WorldRuntime::getAgentPermissionSetAssignments(core::AgentId agent) const
{
	return world().getAgentPermissionSetAssignments(agent);
}
bool WorldRuntime::isLocationPermissionEligible(std::uint32_t sectorIndex) const
{
	return world().isLocationPermissionEligible(sectorIndex);
}
std::vector<core::AccessPermissionId> WorldRuntime::getLocationPermissionRequirement(std::uint32_t sectorIndex) const
{
	return world().getLocationPermissionRequirement(sectorIndex);
}
bool WorldRuntime::canAgentAccessLocation(core::Sector const& sector, core::Agent const& agent) const
{
	return world().canAgentAccessLocation(sector, agent);
}
bool WorldRuntime::isInteractionPointPermissionEligible(core::InteractionPointId point) const
{
	return world().isInteractionPointPermissionEligible(point);
}
std::vector<core::AccessPermissionId> WorldRuntime::getInteractionPointPermissionRequirement(core::InteractionPointId point) const
{
	return world().getInteractionPointPermissionRequirement(point);
}
bool WorldRuntime::isManualDoorPermissionEligible(core::TraversalResourceId door) const
{
	return world().isManualDoorPermissionEligible(door);
}
std::vector<core::AccessPermissionId> WorldRuntime::getManualDoorPermissionRequirement(core::TraversalResourceId door) const
{
	return world().getManualDoorPermissionRequirement(door);
}

// --- Transport state and authorization ---
bool WorldRuntime::isValidTransportStopShortName(std::string_view name)
{
	return core::World::isValidTransportStopShortName(name);
}
std::string WorldRuntime::getTransportStopShortName(std::uint32_t sectorIndex, std::uint32_t stopIndex, std::uint32_t objectIndex) const
{
	return world().getTransportStopShortName(sectorIndex, stopIndex, objectIndex);
}
std::string WorldRuntime::getTransportStopShortName(core::TraversalResourceId resource, std::uint32_t stopIndex) const
{
	return world().getTransportStopShortName(resource, stopIndex);
}
std::vector<std::uint32_t> WorldRuntime::getLiftDestinationLevels(std::uint32_t sectorIndex, std::uint32_t objectIndex) const
{
	return world().getLiftDestinationLevels(sectorIndex, objectIndex);
}
std::vector<core::AccessPermissionId> WorldRuntime::getLiftDestinationPermissionRequirement(std::uint32_t sectorIndex, std::uint32_t stopIndex, std::uint32_t objectIndex) const
{
	return world().getLiftDestinationPermissionRequirement(sectorIndex, stopIndex, objectIndex);
}
std::optional<core::DeviceCondition> WorldRuntime::knownTransportCondition(core::TraversalResourceId resource, core::Agent const* agent, core::Sector const* observationSector) const
{
	return world().knownTransportCondition(resource, agent, observationSector);
}
std::optional<core::DeviceCondition> WorldRuntime::knownLiftCondition(core::TraversalResourceId resource, core::Agent const* agent, core::Sector const* observationSector) const
{
	return world().knownLiftCondition(resource, agent, observationSector);
}
bool WorldRuntime::canAgentEnterAirlock(core::TraversalResourceId resource, core::SectorId approach, core::AgentId agent, bool locallyObserved) const
{
	return world().canAgentEnterAirlock(resource, approach, agent, locallyObserved);
}
bool WorldRuntime::canAgentOpenManualDoor(core::TraversalResourceId door, core::AgentId agent) const
{
	return world().canAgentOpenManualDoor(door, agent);
}
bool WorldRuntime::agentHasFeasibleManualDoorOperation(core::TraversalResourceId door, core::AgentId agent) const
{
	return world().agentHasFeasibleManualDoorOperation(door, agent);
}
std::optional<float> WorldRuntime::remoteButtonApproachDistance(core::InteractionPointId point, core::SectorId approach, core::Vector2 const& position, core::AgentId agent) const
{
	return world().remoteButtonApproachDistance(point, approach, position, agent);
}
bool WorldRuntime::canAgentOperateDoorControl(core::TraversalResourceId door, core::SectorId approach, core::AgentId agent) const
{
	return world().canAgentOperateDoorControl(door, approach, agent);
}
bool WorldRuntime::agentAdheresToDoorPermission(core::TraversalResourceId door, core::SectorId approach, core::AgentId agent) const
{
	return world().agentAdheresToDoorPermission(door, approach, agent);
}
bool WorldRuntime::canAgentOperateExtensibleControl(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& approachPosition, core::AgentId agent) const
{
	return world().canAgentOperateExtensibleControl(resource, approach, approachPosition, agent);
}
bool WorldRuntime::agentAdheresToExtensiblePermission(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& approachPosition, core::AgentId agent) const
{
	return world().agentAdheresToExtensiblePermission(resource, approach, approachPosition, agent);
}
std::vector<core::AccessPermissionId> WorldRuntime::missingLiftDestinationPermissions(core::DeviceCommand const& command, core::AgentId agent) const
{
	return world().missingLiftDestinationPermissions(command, agent);
}
bool WorldRuntime::canAgentUseLiftJourney(core::TraversalResourceId resource, core::Vector2 const& origin, core::Vector2 const& destination, core::AgentId agent) const
{
	return world().canAgentUseLiftJourney(resource, origin, destination, agent);
}
bool WorldRuntime::agentHasFeasibleLiftJourney(core::TraversalResourceId resource, core::Vector2 const& origin, core::Vector2 const& destination, core::AgentId agent) const
{
	return world().agentHasFeasibleLiftJourney(resource, origin, destination, agent);
}
bool WorldRuntime::agentAdheresToLiftDestinationPermission(core::TraversalResourceId resource, std::uint32_t destinationStop, core::AgentId agent) const
{
	return world().agentAdheresToLiftDestinationPermission(resource, destinationStop, agent);
}
bool WorldRuntime::canAgentOperateTransportLandingControl(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const
{
	return world().canAgentOperateTransportLandingControl(resource, approach, endpoint, agent);
}
bool WorldRuntime::agentHasFeasibleTransportLandingOperation(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const
{
	return world().agentHasFeasibleTransportLandingOperation(resource, approach, endpoint, agent);
}
bool WorldRuntime::agentAdheresToTransportLandingPermission(core::TraversalResourceId resource, core::SectorId approach, core::Vector2 const& endpoint, core::AgentId agent) const
{
	return world().agentAdheresToTransportLandingPermission(resource, approach, endpoint, agent);
}
bool WorldRuntime::isAgentTransportOccupant(core::TraversalResourceId resource, core::AgentId agent) const
{
	return world().isAgentTransportOccupant(resource, agent);
}
bool WorldRuntime::isTransportLocallyBoardable(core::TraversalResourceId resource, core::Vector2 const& endpoint) const
{
	return world().isTransportLocallyBoardable(resource, endpoint);
}
bool WorldRuntime::canAgentTraverseManualDoorNow(core::TraversalResourceId door, core::AgentId agent) const
{
	return world().canAgentTraverseManualDoorNow(door, agent);
}

// --- Interaction points, devices and Access panels ---
core::EntityLookup<core::InteractionPoint const> WorldRuntime::lookupInteractionPoint(core::InteractionPointId id) const
{
	return world().lookupInteractionPoint(id);
}
std::shared_ptr<const core::AccessPanel> WorldRuntime::lookupAccessPanel(core::AccessPanelId id) const
{
	return world().lookupAccessPanel(id);
}
bool WorldRuntime::canRequestAccessPanel(core::AccessPanelId id, core::AccessPanel::Action action, core::AgentId actor) const
{
	return world().canRequestAccessPanel(id, action, actor);
}
core::EntityLookup<core::InteractionRequest const> WorldRuntime::lookupInteractionRequest(core::InteractionRequestId id) const
{
	return world().lookupInteractionRequest(id);
}
std::shared_ptr<const core::BoothWindow> WorldRuntime::lookupBoothWindow(core::BoothWindowId id) const
{
	return world().lookupBoothWindow(id);
}
core::EntityLookup<core::DeviceOperation const> WorldRuntime::lookupDeviceOperation(core::DeviceOperationId id) const
{
	return world().lookupDeviceOperation(id);
}
core::EntityLookup<core::TraversalResource const> WorldRuntime::lookupTraversalResource(core::TraversalResourceId id) const
{
	return world().lookupTraversalResource(id);
}
core::TraversalResourceId WorldRuntime::getTraversalResourceId(core::Object const* object) const
{
	return world().getTraversalResourceId(object);
}
core::EntityLookup<core::TraversalRequest const> WorldRuntime::lookupTraversalRequest(core::TraversalRequestId id) const
{
	return world().lookupTraversalRequest(id);
}
bool WorldRuntime::isAgentInQueue(core::AgentId id) const
{
	return world().isAgentInQueue(id);
}

// --- Routing policy and live observation ---
core::RouteChoicePolicy const& WorldRuntime::getRouteChoicePolicy() const
{
	return world().getRouteChoicePolicy();
}
core::TraversalWaitingPolicy const& WorldRuntime::getTraversalWaitingPolicy() const
{
	return world().getTraversalWaitingPolicy();
}
core::TraversalGeometryPolicy const& WorldRuntime::getTraversalGeometryPolicy() const
{
	return world().getTraversalGeometryPolicy();
}
float WorldRuntime::estimateTraversalDelay(core::TraversalResourceId resource, core::SectorId sourceSector) const
{
	return world().estimateTraversalDelay(resource, sourceSector);
}
float WorldRuntime::observeAccessZoneDensity(core::TraversalResourceId resource, core::SectorId sourceSector) const
{
	return world().observeAccessZoneDensity(resource, sourceSector);
}
std::optional<core::ShuttleRouteAccessObservation> WorldRuntime::observeShuttleAccess(core::TraversalResourceId resource, core::Vector2 const& endpoint, bool includeLocalQueue) const
{
	return world().observeShuttleAccess(resource, endpoint, includeLocalQueue);
}
std::optional<core::LiftRouteAccessObservation> WorldRuntime::observeLiftAccess(core::TraversalResourceId resource, core::Vector2 const& sourceEndpoint, bool includeLocalQueue) const
{
	return world().observeLiftAccess(resource, sourceEndpoint, includeLocalQueue);
}
std::uint32_t WorldRuntime::countStandingAgentsOnEscalator(core::Agent const* observer, core::Edge const* edge) const
{
	return world().countStandingAgentsOnEscalator(observer, edge);
}

// --- Furniture ---
std::shared_ptr<const core::FurnitureCatalogue> const& WorldRuntime::furnitureCatalogue() const
{
	return world().furnitureCatalogue();
}
std::vector<core::FurnitureInstance> const& WorldRuntime::furniture() const
{
	return world().furniture();
}
bool WorldRuntime::isFurnitureMarker(core::MarkerId id) const
{
	return world().isFurnitureMarker(id);
}
core::AgentId WorldRuntime::usablePointOccupant(core::MarkerId id) const
{
	return world().usablePointOccupant(id);
}

// --- Simulation clock reads ---
double WorldRuntime::getDeferredSimulationTime() const
{
	return world().getDeferredSimulationTime();
}
double WorldRuntime::getAverageSimulationStepMicroseconds() const
{
	return world().getAverageSimulationStepMicroseconds();
}
std::vector<core::SimulationEvent> const& WorldRuntime::getSimulationEvents() const
{
	return world().getSimulationEvents();
}
}

