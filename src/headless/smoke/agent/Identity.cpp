#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Simulation.h"
#include <type_traits>
#include <cmath>

static_assert(!std::is_convertible_v<core::AgentId, core::InteractionPointId>);

namespace
{
	bool worldOwnsTypedEntitiesAndInvalidatesHandles()
	{
		core::World world("Ownership check", 3, 2);
		auto corridor = world.addCorridor(0, 0, 2);
		world.finishBuild();

		auto agentId = world.createAgent("Owned idle agent", corridor, 0, 0.5f);
		auto const* human = world.lookupAgent(agentId).entity;
		smoke::require(human && std::string(human->getTypeName()) == "Human",
			"Ordinary World creation did not create a Human");
		auto const& physical = human->getPhysicalBaseline();
		auto const preview = core::Agent::placementDimensions(human->getPhysicalBaseline());
		smoke::require(preview.x == human->getWidth() && preview.y == human->getStandingHeight()
			&& std::abs(human->getBounds().getSize().x - preview.x) < 0.000001f
			&& std::abs(human->getBounds().getSize().y - preview.y) < 0.000001f,
			"Uncommitted Human dimensions disagreed with World bounds");
		smoke::require(physical.width == 0.4f
			&& physical.standingHeight == 0.45f
			&& physical.reach == 0.25f
			&& physical.walkSpeed == 0.5f
			&& physical.climbSpeed == 0.25f
			&& physical.stairAscentSpeed == 0.35f && physical.stairDescentSpeed == 0.45f
			&& physical.poses.at(core::Pose::Sitting) == 0.6f && physical.poses.at(core::Pose::Crouching) == 0.6f
			&& physical.poses.at(core::Pose::Crawling) == 0.3f && physical.automaticSpeedRatio(core::AutomaticPoseContext::DoorCrossing, core::Pose::Crawling).value() == 0.5f,
			"Human physical baselines changed");
		world.pauseSimulation();
		std::string diagnostic;
		smoke::require(world.setAgentIndividualHeightModifier(agentId, 0.8f, &diagnostic)
			&& world.setAgentIndividualWalkSpeedModifier(agentId, 1.2f, &diagnostic),
			"Human physical modifier authoring failed");
		smoke::require(human->getStandingHeight() == physical.standingHeight * 0.8f
			&& human->getWalkSpeed() == physical.walkSpeed * 1.2f,
			"Human modifiers did not apply to physical baselines");
		auto const modifiedPreview = core::Agent::placementDimensions(human->getPhysicalBaseline(), 0.8f);
		smoke::require(std::abs(human->getBounds().getSize().x - modifiedPreview.x) < 0.000001f
			&& std::abs(human->getBounds().getSize().y - modifiedPreview.y) < 0.000001f,
			"Modified Human bounds disagreed with placement dimensions");
		smoke::require(world.setAgentIndividualHeightModifier(agentId, {}, &diagnostic)
			&& human->getStandingHeight() == preview.y,
			"Clearing Height without a tag did not reveal the default dimensions");
		world.resetSimulation();
		smoke::require(std::string(world.lookupAgent(agentId).entity->getTypeName()) == "Human",
			"Reset lost Human identity");

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
