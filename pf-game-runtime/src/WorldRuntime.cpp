#include "game/WorldRuntime.h"

#include <stdexcept>
#include <utility>

#include "core/World.h"

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

	WorldRuntime::WorldRuntime(WorldRuntime&& other) noexcept = default;

	WorldRuntime& WorldRuntime::operator=(WorldRuntime&& other) noexcept = default;

	core::World const& WorldRuntime::world() const noexcept
	{
		return *world_;
	}

	core::World& WorldRuntime::authoringWorld() noexcept
	{
		// Reachable only from WorldAuthoring, which is declared a friend: the
		// single widening of the const view in the whole boundary (#967).
		return *world_;
	}

	void WorldRuntime::update(float elapsedSeconds)
	{
		world_->update(elapsedSeconds);
	}

	bool WorldRuntime::advanceTick()
	{
		return world_->advanceTick();
	}

	bool WorldRuntime::advanceTicks(std::uint64_t count)
	{
		return world_->advanceTicks(count);
	}

	void WorldRuntime::pauseSimulation()
	{
		world_->pauseSimulation();
	}

	bool WorldRuntime::resumeSimulation()
	{
		return world_->resumeSimulation();
	}

	void WorldRuntime::resetSimulation()
	{
		world_->resetSimulation();
	}

	bool WorldRuntime::isSimulationPaused() const noexcept
	{
		return world_->isSimulationPaused();
	}

	void WorldRuntime::wakeAllAgents()
	{
		world_->wakeAllAgents();
	}

	void WorldRuntime::setSimulationTimeScale(double scale)
	{
		world_->setSimulationTimeScale(scale);
	}

	double WorldRuntime::getSimulationTimeScale() const noexcept
	{
		return world_->getSimulationTimeScale();
	}

	void WorldRuntime::finishBuild()
	{
		world_->finishBuild();
	}

	bool WorldRuntime::rebuildTraversalTopology()
	{
		return world_->rebuildTraversalTopology();
	}

	core::AgentId WorldRuntime::getPlayerControlledAgent() const
	{
		return world_->getPlayerControlledAgent();
	}

	void WorldRuntime::setPlayerMovementDirection(int direction)
	{
		world_->setPlayerMovementDirection(direction);
	}

	void WorldRuntime::setPlayerStairSelection(bool up, bool down)
	{
		world_->setPlayerStairSelection(up, down);
	}

	void WorldRuntime::setPlayerCrouch(bool held)
	{
		world_->setPlayerCrouch(held);
	}

	void WorldRuntime::requestPlayerDoorAction()
	{
		world_->requestPlayerDoorAction();
	}

	std::optional<core::World::PlayerTransportSelector> WorldRuntime::getPlayerTransportSelector() const
	{
		return world_->getPlayerTransportSelector();
	}

	void WorldRuntime::browsePlayerTransportStop(int direction, bool horizontal)
	{
		world_->browsePlayerTransportStop(direction, horizontal);
	}

	void WorldRuntime::confirmPlayerTransportStop()
	{
		world_->confirmPlayerTransportStop();
	}

	void WorldRuntime::clearPlayerInput()
	{
		world_->clearPlayerInput();
	}

	std::uint64_t WorldRuntime::getSimulationTick() const noexcept
	{
		return world_->getSimulationTick();
	}

	core::SimulationPhase WorldRuntime::getCurrentSimulationPhase() const noexcept
	{
		return world_->getCurrentSimulationPhase();
	}

	core::SimulationSnapshot WorldRuntime::getSimulationSnapshot() const
	{
		return world_->getSimulationSnapshot();
	}

	core::SimulationSnapshot const& WorldRuntime::getSimulationSnapshotView() const
	{
		return world_->getSimulationSnapshotView();
	}

	std::uint64_t WorldRuntime::getSimulationSnapshotBuildCount() const noexcept
	{
		return world_->getSimulationSnapshotBuildCount();
	}

	std::vector<core::SimulationEvent> WorldRuntime::consumeSimulationEvents()
	{
		return world_->consumeSimulationEvents();
	}

	void WorldRuntime::setSimulationObserver(core::SimulationObserver* observer) noexcept
	{
		world_->setSimulationObserver(observer);
	}

	float WorldRuntime::getFixedTimestep()
	{
		return core::World::getFixedTimestep();
	}

	std::uint32_t WorldRuntime::getMaxTicksPerUpdate()
	{
		return core::World::getMaxTicksPerUpdate();
	}

	// --- Resource names -------------------------------------------------
	// Reads through the same const view rendering gets. There is no widening
	// here: the names are read off `world_` exactly as a `core::World const&`
	// would read them, and the binding acts that would change them are not on
	// this type at all (#969).

	// The World's own registry name getters throw when nothing is referenced,
	// because the editor uses that as a programming-error signal. The runtime
	// is a read surface, not an authoring one: an unbound Resource reads as
	// "no name" here rather than faulting a render or game path.
	std::string const& WorldRuntime::agentTagRegistryResourceName() const
	{
		static const std::string kNoName;
		if (!world_->hasAgentTagRegistryReference()) return kNoName;
		return world_->getAgentTagRegistryResourceName();
	}

	std::string const& WorldRuntime::agentBehaviourRegistryResourceName() const
	{
		static const std::string kNoName;
		if (!world_->hasAgentBehaviourRegistryReference()) return kNoName;
		return world_->getAgentBehaviourRegistryResourceName();
	}

	std::string const& WorldRuntime::furnitureCatalogueResourceName() const
	{
		return world_->furnitureCatalogueResourceName();
	}

	std::string const& WorldRuntime::sectorImageSetResourceName() const
	{
		return world_->sectorImageSetResourceName();
	}

	std::string const& WorldRuntime::agentImageSetResourceName() const
	{
		return world_->agentImageSetResourceName();
	}

	std::string const& WorldRuntime::furnitureImageSetResourceName() const
	{
		return world_->furnitureImageSetResourceName();
	}

	std::string const& WorldRuntime::actionRegistryFilename() const
	{
		return world_->actionRegistryFilename();
	}
}
