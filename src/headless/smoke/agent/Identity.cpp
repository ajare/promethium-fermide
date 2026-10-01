#include "Checks.h"
#include "core/World.h"
#include "core/Simulation.h"
#include <type_traits>

static_assert(!std::is_convertible_v<core::AgentId, core::InteractionPointId>);

namespace
{
	bool worldOwnsTypedEntitiesAndInvalidatesHandles()
	{
		core::World world("Ownership check", 3, 2);
		auto corridor = world.addCorridor(0, 0, 2);
		world.finishBuild();

		auto agentId = world.createAgent("Owned idle agent", corridor, 0, 0.5f);
		auto pointId = world.createInteractionPoint("Light switch");
		auto operationId = world.createDeviceOperation("Turn lights on", agentId);
		auto resourceId = world.createTraversalResource("Ordinary passage");

		auto snapshot = world.getSimulationSnapshot();
		if (snapshot.agents.size() != 1 || snapshot.agents.front().id != agentId
			|| snapshot.interactionPoints.size() != 1 || snapshot.interactionPoints.front().id != pointId
			|| snapshot.deviceOperations.size() != 1 || snapshot.deviceOperations.front().id != operationId
			|| snapshot.deviceOperations.front().requester != agentId
			|| snapshot.traversalResources.size() != 1 || snapshot.traversalResources.front().id != resourceId)
		{
			return false;
		}

		if (!world.removeAgent(agentId)
			|| world.lookupAgent(agentId)
			|| world.lookupAgent(agentId).diagnostic.empty()
			|| world.lookupDeviceOperation(operationId)
			|| world.lookupDeviceOperation(operationId).diagnostic.empty())
		{
			return false;
		}
		if (!world.removeInteractionPoint(pointId) || !world.removeTraversalResource(resourceId))
		{
			return false;
		}

		world.advanceTick();
		auto afterRemoval = world.getSimulationSnapshot();
		return afterRemoval.agents.empty()
			&& afterRemoval.interactionPoints.empty()
			&& afterRemoval.deviceOperations.empty()
			&& afterRemoval.traversalResources.empty();
	}

}

void agent_smoke::registerIdentity(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "identity", [](smoke::Context const&)
	{
		smoke::require(worldOwnsTypedEntitiesAndInvalidatesHandles(),
			"Typed World ownership or handle invalidation failed");
	} });
}
