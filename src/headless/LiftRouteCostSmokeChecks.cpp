#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/RouteCost.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	std::filesystem::path testWorld(char const* name)
	{
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
			/ "resources" / "test-worlds" / name;
	}

	void enclosedLiftSeparatesAccessFromRide()
	{
		auto world = core::loadWorldDocument(testWorld("lift-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext remote{ agent, policy.baselineProfile, policy,
			nullptr, agent->getWalkSpeed() };

		bool bodyChecked = false;
		float capacityOneWait = -1.0f;
		float capacityTwoWait = -1.0f;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() == core::EdgeType::Lift)
			{
				auto facts = edge->getDirectedTraversalFacts(edge->getVertex(1), remote);
				require(facts.feasible
					&& std::abs(facts.components.motionSeconds
						- edge->getLength() / CORE_LIFT_SPEED) < 0.0001f,
					"Lift ride duration ignored stop distance or configured speed");
				require(facts.components.interactionUnits == 0.0f,
					"Lift body repeated boarding or alighting interaction");
				bodyChecked = true;
				continue;
			}
			if (edge->getType() != core::EdgeType::Door) continue;
			for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex)
			{
				auto target = edge->getVertex(targetIndex);
				auto source = edge->getOtherVertex(target);
				if (!source || !core::isLocationLike(source->getSector()->getType())) continue;
				auto access = agent->observeLiftAccess(edge->getTraversalResourceId(),
					source->getPosition(), false);
				if (!access) continue;
				auto facts = edge->getDirectedTraversalFacts(target, remote);
				require(facts.components.interactionUnits
					>= policy.liftCallBoardingInteraction,
					"Lift admission omitted call and boarding interaction");
				require(facts.components.expectedWaitSeconds >= policy.liftExpectedWaitSeconds,
					"Unobserved Lift admission inspected live scheduler state");
				if (access->capacity == 1) capacityOneWait = facts.components.expectedWaitSeconds;
				if (access->capacity == 2) capacityTwoWait = facts.components.expectedWaitSeconds;
			}
		}
		require(bodyChecked, "Lift fixture has no ride edge");
		require(capacityOneWait > capacityTwoWait && capacityTwoWait >= 0.0f,
			"Lift capacity did not increase expected admission waiting");
	}

	void transportQueueSnapshotsAreEventDriven()
	{
		core::World world("Routing queue epochs", 10, 2);
		auto lower = world.addCorridor(0, 0, 10);
		auto upper = world.addCorridor(1, 0, 10);
		world.addLift(1, 0, 8, 2, 2);
		world.finishBuild();
		auto graph = world.getGraph();
		core::TraversalResourceId resourceId;
		for (auto const& edge : graph->getEdges())
			if (edge->getType() == core::EdgeType::Lift) resourceId = edge->getTraversalResourceId();
		auto resource = world.lookupTraversalResource(resourceId).entity;
		require(resource != nullptr, "Queue epoch fixture has no Lift resource");
		auto target = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 1.0f });
		for (uint32_t index = 0; index < 8; ++index)
		{
			auto agent = world.lookupAgent(world.createAgent("Queue observer", lower, 0, 8.5f)).entity;
			auto path = graph->calculatePath(agent, target);
			require(path != nullptr, "Queue epoch fixture has no Path");
			agent->setPath(path, true);
		}
		require(world.resumeSimulation(), "Could not resume queue epoch fixture");
		bool sawQueue = false;
		for (uint32_t tick = 0; tick < 240; ++tick)
		{
			auto before = world.observeLiftAccess(resourceId, { 8.5f, 0 }, true);
			auto const builds = resource->getRouteQueueSnapshotBuildCount();
			auto const epoch = resource->getRouteQueueEpoch();
			for (int observer = 0; observer < 20; ++observer)
			{
				auto repeated = world.observeLiftAccess(resourceId, { 8.5f, 0 }, true);
				require(repeated && before && repeated->queuedAgents == before->queuedAgents,
					"Shared queue observation changed without a mutation");
			}
			require(resource->getRouteQueueSnapshotBuildCount() == builds,
				"Multiple observers traversed the same unchanged queue");
			world.advanceTick();
			auto const afterTickBuilds = resource->getRouteQueueSnapshotBuildCount();
			(void)world.observeLiftAccess(resourceId, { 8.5f, 0 }, false);
			require(resource->getRouteQueueSnapshotBuildCount() == afterTickBuilds,
				"Remote observation captured a local queue");
			auto after = world.observeLiftAccess(resourceId, { 8.5f, 0 }, true);
			if (resource->getRouteQueueEpoch() == epoch)
				require(resource->getRouteQueueSnapshotBuildCount() == builds,
					"A simulation tick invalidated an unchanged queue snapshot");
			sawQueue = sawQueue || (after && after->queuedAgents > 0);
		}
		require(sawQueue, "Queue epoch fixture never exercised waiting Agents");
		world.pauseSimulation();
		auto cancelled = world.observeLiftAccess(resourceId, { 8.5f, 0 }, true);
		require(cancelled && cancelled->queuedAgents == 0,
			"Queue snapshot retained cancelled requests after pause");
	}

	void waitingAversionReversesWaitingChoiceWithoutChangingTiming()
	{
		core::RouteChoicePolicy policy;
		core::DirectedTraversalFacts lift;
		lift.feasible = true;
		lift.components.motionSeconds = 2.0f;
		lift.components.knownWaitSeconds = 1.0f;
		lift.components.expectedWaitSeconds = 2.0f;
		lift.objectiveDurationSeconds = 5.0f;
		core::DirectedTraversalFacts walk;
		walk.feasible = true;
		walk.components.motionSeconds = 8.0f;
		walk.objectiveDurationSeconds = 8.0f;

		core::EffectiveRoutingProfile low;
		low.waitingAversion = core::AgentWaitingAversionMinimum;
		auto high = low;
		high.waitingAversion = core::AgentWaitingAversionMaximum;
		auto const lowLift = policy.evaluate(lift, low);
		auto const highLift = policy.evaluate(lift, high);
		auto const lowWalk = policy.evaluate(walk, low);
		auto const highWalk = policy.evaluate(walk, high);
		require(lowLift && highLift && lowWalk && highWalk
			&& lowLift->perceivedCost < lowWalk->perceivedCost
			&& highLift->perceivedCost > highWalk->perceivedCost,
			"Waiting aversion did not reverse a waiting-versus-walking choice");
		require(lowLift->objectiveDurationSeconds == highLift->objectiveDurationSeconds
			&& lowLift->objectiveDurationSeconds == lift.objectiveDurationSeconds,
			"Waiting aversion changed objective device or queue timing");
	}

	void crowdAversionReversesWaitingChoiceWithoutChangingTiming()
	{
		core::RouteChoicePolicy policy;
		core::DirectedTraversalFacts lift;
		lift.feasible = true;
		lift.components.motionSeconds = 2.0f;
		lift.components.knownWaitSeconds = 1.0f;
		lift.components.expectedWaitSeconds = 2.0f;
		lift.components.crowdingUnits = 3.0f;
		lift.objectiveDurationSeconds = 5.0f;
		core::DirectedTraversalFacts walk;
		walk.feasible = true;
		walk.components.motionSeconds = 8.0f;
		walk.objectiveDurationSeconds = 8.0f;

		core::EffectiveRoutingProfile low;
		low.crowdAversion = core::AgentCrowdAversionMinimum;
		auto high = low;
		high.crowdAversion = core::AgentCrowdAversionMaximum;
		auto const lowLift = policy.evaluate(lift, low);
		auto const highLift = policy.evaluate(lift, high);
		auto const lowWalk = policy.evaluate(walk, low);
		auto const highWalk = policy.evaluate(walk, high);
		require(lowLift && highLift && lowWalk && highWalk
			&& lowLift->perceivedCost < lowWalk->perceivedCost
			&& highLift->perceivedCost > highWalk->perceivedCost,
			"Crowd aversion did not reverse a waiting-versus-walking choice");
		require(lowLift->objectiveDurationSeconds == highLift->objectiveDurationSeconds
			&& lowLift->objectiveDurationSeconds == lift.objectiveDurationSeconds,
			"Crowd aversion changed objective device or queue timing");
	}

	void platformLiftUsesSlowerFiniteService()
	{
		auto world = core::loadWorldDocument(testWorld("platformlift-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 19 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
			nullptr, agent->getWalkSpeed() };
		bool bodyChecked = false;
		bool admissionChecked = false;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() == core::EdgeType::Lift)
			{
				auto access = agent->observeLiftAccess(edge->getTraversalResourceId(),
					edge->getVertex(0)->getPosition(), false);
				if (!access) continue;
				auto facts = edge->getDirectedTraversalFacts(edge->getVertex(1), context);
				require(facts.feasible && std::isfinite(facts.components.motionSeconds)
					&& std::abs(facts.components.motionSeconds
						- edge->getLength() / CORE_PLATFORM_LIFT_SPEED) < 0.0001f,
					"Platform lift route estimate did not use its slower configured speed");
				bodyChecked = true;
			}
			else if (edge->getType() == core::EdgeType::LiftMount)
			{
				auto target = edge->getVertex(1);
				if (target->getObject()) target = edge->getVertex(0);
				auto facts = edge->getDirectedTraversalFacts(target, context);
				if (facts.components.interactionUnits >= policy.platformLiftInconvenience)
					admissionChecked = true;
			}
		}
		require(bodyChecked, "Platform lift fixture has no finite ride edge");
		require(admissionChecked,
			"Platform lift admission omitted its ordinary baseline inconvenience");
	}
}

void runLiftRouteCostSmokeChecks()
{
	enclosedLiftSeparatesAccessFromRide();
	transportQueueSnapshotsAreEventDriven();
	waitingAversionReversesWaitingChoiceWithoutChangingTiming();
	crowdAversionReversesWaitingChoiceWithoutChangingTiming();
	platformLiftUsesSlowerFiniteService();
}
