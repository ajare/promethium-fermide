#include "Checks.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Edge.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Graph.h"
#include "core/LadderMountEdge.h"
#include "core/Path.h"
#include "core/RouteCost.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}


	void ladderCostsPhysicalClimbingEffortMountingAndRisk(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/ladder-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
			agent->getSector(), agent->getWalkSpeed(), world.get(), agent->getClimbSpeed() };
		bool checked = false;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() != core::EdgeType::Ladder || edge->requiresButton()) continue;
			auto lower = edge->getVertex(0);
			auto upper = edge->getVertex(1);
			if (lower->getPosition().y > upper->getPosition().y) std::swap(lower, upper);
			auto const up = edge->getDirectedTraversalFacts(upper, context);
			auto const down = edge->getDirectedTraversalFacts(lower, context);
			require(up.feasible && down.feasible
				&& std::abs(up.components.motionSeconds
					- edge->getLength() / agent->getClimbSpeed()) < 0.0001f
				&& std::abs(down.components.motionSeconds
					- edge->getLength() / agent->getClimbSpeed()) < 0.0001f,
				"Ladder duration ignored its physical length or effective climb speed");
			require(up.components.physicalEffortUnits > down.components.physicalEffortUnits
				&& down.components.physicalEffortUnits > 0.0f,
				"Ladder effort was not finite and direction-sensitive");
			require(up.components.interactionUnits > 0.0f && up.components.riskUnits > 0.0f,
				"Ladder mounting and ordinary risk were not distinct finite components");

			world->pauseSimulation();
			std::string diagnostic;
			require(world->setAgentIndividualLadderSpeedModifier(
				core::AgentId{ 1 }, 0.5f, &diagnostic),
				"Could not set slow Ladder speed for traversal check");
			auto const slowTraversalSpeed = agent->getClimbSpeed();
			core::RouteDecisionContext slowContext{ agent, policy.baselineProfile, policy,
				agent->getSector(), agent->getWalkSpeed(), world.get(), agent->getClimbSpeed() };
			auto const slowDuration = edge->getDirectedTraversalFacts(upper, slowContext)
				.components.motionSeconds;
			require(world->setAgentIndividualLadderSpeedModifier(
				core::AgentId{ 1 }, 1.5f, &diagnostic),
				"Could not set fast Ladder speed for traversal check");
			auto const fastTraversalSpeed = agent->getClimbSpeed();
			core::RouteDecisionContext fastContext{ agent, policy.baselineProfile, policy,
				agent->getSector(), agent->getWalkSpeed(), world.get(), agent->getClimbSpeed() };
			auto const fastDuration = edge->getDirectedTraversalFacts(upper, fastContext)
				.components.motionSeconds;
			require(std::abs(fastTraversalSpeed / slowTraversalSpeed - 3.0f) < 0.0001f
				&& std::abs(slowDuration / fastDuration - 3.0f) < 0.0001f,
				"Measured and estimated Ladder speeds did not scale consistently");
			checked = true;
			break;
		}
		require(checked, "Ladder fixture has no permanent Ladder body edge");
	}

	void extensibleLadderUsesLocalStateAndRemoteExpectation(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/ladder-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() != core::EdgeType::Ladder || !edge->requiresButton()) continue;
			auto const target = edge->getVertex(1);
			auto const source = edge->getOtherVertex(target);
			std::shared_ptr<core::Ladder> ladder;
			core::Sector const* approach = nullptr;
			for (auto const& adjacent : source->getEdges())
				if (auto mount = std::dynamic_pointer_cast<const core::LadderMountEdge>(adjacent))
				{
					ladder = mount->getLadder();
					approach = adjacent->getOtherVertex(source)->getSector().get();
					break;
				}
			require(ladder && approach, "Extensible Ladder has no approach mount");
			core::RouteDecisionContext local{ agent, policy.baselineProfile, policy,
				approach, agent->getWalkSpeed(), world.get(), agent->getClimbSpeed() };
			core::RouteDecisionContext remote{ agent, policy.baselineProfile, policy,
				nullptr, agent->getWalkSpeed(), world.get(), agent->getClimbSpeed() };
			auto const retracted = edge->getDirectedTraversalFacts(target, local);
			auto const unknownBefore = edge->getDirectedTraversalFacts(target, remote);
			require(retracted.feasible && retracted.components.knownWaitSeconds > 0.0f
				&& retracted.components.expectedWaitSeconds == 0.0f
				&& retracted.components.interactionUnits
					> policy.ladderMountDismountInteraction,
				"A locally retracted Ladder did not add known preparation and interaction");
			require(unknownBefore.components.knownWaitSeconds == 0.0f
				&& unknownBefore.components.expectedWaitSeconds > 0.0f,
				"A remote Ladder used live state instead of a baseline expectation");
			require(ladder->extend(), "Could not extend Ladder for observation check");
			ladder->update(ladder->getExtendRetractTime());
			auto const extended = edge->getDirectedTraversalFacts(target, local);
			auto const unknownAfter = edge->getDirectedTraversalFacts(target, remote);
			require(extended.components.knownWaitSeconds == 0.0f
				&& extended.components.interactionUnits
					== policy.ladderMountDismountInteraction,
				"A locally extended Ladder retained preparation cost");
			require(std::abs(unknownAfter.components.expectedWaitSeconds
				- unknownBefore.components.expectedWaitSeconds) < 0.0001f,
				"Remote Ladder cost changed with unobservable live deployment state");
			return;
		}
		require(false, "Ladder fixture has no extensible Ladder body edge");
	}

	void defaultAgentAvoidsACompetitiveLadderShortcut()
	{
		core::World world("Ladder route choice", 10, 2);
		auto const lower = world.addCorridor(0, 0, 10);
		auto const upper = world.addCorridor(1, 0, 10);
		auto const ladder = world.addLadder(1, 0, 4, { 2, false, true });
		world.addStaircase(1, 0, 5, { 2, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		auto const id = world.createAgent("Route chooser", lower, 0, 4.5f);
		auto agent = world.lookupAgent(id).entity;
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 5.5f, 1.0f });
		auto path = world.getGraph()->calculatePath(agent, target);
		require(path != nullptr, "Competitive Ladder fixture produced no route");
		bool usedLadder = false, usedStairs = false;
		for (auto const& node : path->nodes) if (node.edge)
		{
			usedLadder = usedLadder
				|| node.edge->getTraversalResourceId() == ladder.traversalResource;
			usedStairs = usedStairs || node.edge->getType() == core::EdgeType::Staircase;
		}
		require(!usedLadder && usedStairs,
			"Default Agent preferred a modest Ladder shortcut to conventional stairs");

		std::string diagnostic;
		world.pauseSimulation();
		auto const unchangedWalkSpeed = agent->getWalkSpeed();
		auto const unchangedStairSpeed = agent->getStationaryStairSpeed(true);
		require(world.setAgentIndividualLadderSpeedModifier(id, 1.5f, &diagnostic),
			"Could not set fast individual Ladder speed modifier");
		auto fastPath = world.getGraph()->calculatePath(agent, target);
		require(fastPath != nullptr, "Fast Ladder climber produced no route");
		bool fastUsedLadder = false;
		for (auto const& node : fastPath->nodes) if (node.edge)
			fastUsedLadder = fastUsedLadder
				|| node.edge->getTraversalResourceId() == ladder.traversalResource;
		require(fastUsedLadder,
			"Changing only Ladder speed did not reverse the Ladder-versus-stairs decision");
		require(agent->getWalkSpeed() == unchangedWalkSpeed
			&& agent->getStationaryStairSpeed(true) == unchangedStairSpeed,
			"Ladder speed modifier changed walking or stationary-stair speed");

		auto const fastClimbSpeed = agent->getClimbSpeed();
		require(world.setAgentIndividualLadderSpeedModifier(id, 0.5f, &diagnostic),
			"Could not set slow individual Ladder speed modifier");
		require(std::abs(fastClimbSpeed / agent->getClimbSpeed() - 3.0f) < 0.0001f,
			"Measured Ladder movement speed did not scale with the effective modifier");
	}

	void riskAversionChangesPreferenceWithoutChangingFeasibility()
	{
		core::World world("Risk preference", 10, 2);
		auto const lower = world.addCorridor(0, 0, 10);
		auto const upper = world.addCorridor(1, 0, 10);
		auto const ladder = world.addLadder(1, 0, 4, { 2, false, true });
		world.addStaircase(1, 0, 5, { 2, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		auto const id = world.createAgent("Risk chooser", lower, 0, 4.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto const target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 5.5f, 1.0f });
		auto usesLadder = [&](std::shared_ptr<core::Path> const& path)
		{
			if (!path) return false;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getTraversalResourceId() == ladder.traversalResource)
					return true;
			return false;
		};

		world.pauseSimulation();
		std::string diagnostic;
		auto const walkSpeed = agent->getWalkSpeed();
		auto const climbSpeed = agent->getClimbSpeed();
		require(world.setAgentIndividualRiskAversion(id, 0.0f, &diagnostic),
			"Could not set low Risk aversion");
		require(usesLadder(world.getGraph()->calculatePath(agent, target)),
			"A low-risk-aversion Agent did not use the competitive Ladder");
		require(world.setAgentIndividualRiskAversion(id, 3.0f, &diagnostic),
			"Could not set high Risk aversion");
		require(!usesLadder(world.getGraph()->calculatePath(agent, target)),
			"A high-risk-aversion Agent did not avoid the competitive Ladder");
		require(agent->getWalkSpeed() == walkSpeed && agent->getClimbSpeed() == climbSpeed,
			"Risk aversion changed physical movement speed");

		// Editor previews have no Agent. Their route choice comes from the explicit
		// baseline profile rather than a manufactured default Agent (#221).
		auto const source = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 4.5f, 0.0f });
		auto policy = world.getRouteChoicePolicy();
		policy.baselineProfile.riskAversion = 0.0f;
		world.setRouteChoicePolicy(policy);
		require(usesLadder(world.getGraph()->calculatePath(nullptr, source, target)),
			"Low-risk baseline preview did not use the competitive Ladder");
		policy.baselineProfile.riskAversion = 3.0f;
		world.setRouteChoicePolicy(policy);
		require(!usesLadder(world.getGraph()->calculatePath(nullptr, source, target)),
			"High-risk baseline preview did not avoid the competitive Ladder");

		core::MobilityProfile onlyLadder;
		onlyLadder.set(core::TraversalKind::Staircase, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualMobilityProfile(id, onlyLadder, &diagnostic),
			"Could not make the Ladder the only permitted Path");
		require(usesLadder(world.getGraph()->calculatePath(agent, target)),
			"Risk aversion made the only permitted risk-bearing Path unavailable");
	}

	void forceBridgeUsesWalkingExposureAndApproachControls()
	{
		core::World world("Force Bridge route costs", 8, 3);
		auto const room = world.addRoom("Bridge room", 0, 0, 0, 6, 2);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, false, 1 };
		auto const created = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		auto agentId = world.createAgent("Bridge observer", room, 1, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		auto const policy = world.getRouteChoicePolicy();
		core::RouteDecisionContext local{ agent, policy.baselineProfile, policy,
			agent->getSector(), agent->getWalkSpeed(), &world, agent->getClimbSpeed() };
		core::RouteDecisionContext remote{ agent, policy.baselineProfile, policy,
			nullptr, agent->getWalkSpeed(), &world, agent->getClimbSpeed() };
		for (auto const& edge : world.getGraph()->getEdges())
		{
			if (edge->getType() != core::EdgeType::ForceBridge) continue;
			auto left = edge->getVertex(0);
			auto right = edge->getVertex(1);
			if (left->getPosition().x > right->getPosition().x) std::swap(left, right);
			auto const fromLeft = edge->getDirectedTraversalFacts(right, local);
			auto const fromRight = edge->getDirectedTraversalFacts(left, local);
			require(fromLeft.feasible && !fromRight.feasible,
				"Force Bridge remained routable from an approach with no reachable control");
			require(std::abs(fromLeft.components.motionSeconds
				- edge->getLength() / agent->getWalkSpeed()) < 0.0001f
				&& fromLeft.components.riskUnits > 0.0f
				&& fromLeft.components.knownWaitSeconds > 0.0f
				&& fromLeft.components.interactionUnits > 0.0f,
				"Retracted Force Bridge did not separate walking, exposure, preparation, and interaction");
			auto const unknownBefore = edge->getDirectedTraversalFacts(right, remote);
			require(unknownBefore.feasible && unknownBefore.components.knownWaitSeconds == 0.0f
				&& unknownBefore.components.expectedWaitSeconds > 0.0f,
				"Remote Force Bridge state did not use its baseline expectation");
			auto object = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(
				created.forceBridge.sector->getObject(created.forceBridge.index));
			require(object && object->getForceBridge()->extend(), "Could not extend Force Bridge");
			object->getForceBridge()->update(object->getForceBridge()->getExtendRetractTime());
			auto const extended = edge->getDirectedTraversalFacts(right, local);
			auto const unknownAfter = edge->getDirectedTraversalFacts(right, remote);
			require(extended.components.knownWaitSeconds == 0.0f
				&& extended.components.interactionUnits == 0.0f,
				"Locally extended Force Bridge retained preparation cost");
			require(std::abs(unknownAfter.components.expectedWaitSeconds
				- unknownBefore.components.expectedWaitSeconds) < 0.0001f,
				"Remote Force Bridge cost changed with unobservable live state");
			return;
		}
		require(false, "Force Bridge fixture has no bridge edge");
	}
}


namespace routing_smoke
{
	void registerLadderForceBridgeRouteCost(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "ladderCostsPhysicalClimbingEffortMountingAndRisk", [](smoke::Context const& smokeContext) { ladderCostsPhysicalClimbingEffortMountingAndRisk(smokeContext); } });
		checks.push_back({ "extensibleLadderUsesLocalStateAndRemoteExpectation", [](smoke::Context const& smokeContext) { extensibleLadderUsesLocalStateAndRemoteExpectation(smokeContext); } });
		checks.push_back({ "defaultAgentAvoidsACompetitiveLadderShortcut", [](smoke::Context const&) { defaultAgentAvoidsACompetitiveLadderShortcut(); } });
		checks.push_back({ "riskAversionChangesPreferenceWithoutChangingFeasibility", [](smoke::Context const&) { riskAversionChangesPreferenceWithoutChangingFeasibility(); } });
		checks.push_back({ "forceBridgeUsesWalkingExposureAndApproachControls", [](smoke::Context const&) { forceBridgeUsesWalkingExposureAndApproachControls(); } });
	}
}
