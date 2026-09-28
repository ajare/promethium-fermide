#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/ShuttleTransit.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool usesShuttle(std::shared_ptr<core::Path> const& path)
	{
		return path && std::any_of(path->nodes.begin(), path->nodes.end(), [](auto const& node)
		{ return node.edge && node.edge->getType() == core::EdgeType::Shuttle; });
	}
}

void runShuttleRouteCostSmokeChecks()
{
	core::World world("Shuttle route costs", 70, 2);
	auto platform = world.addRoom("Parallel walking", 0, 0, 0, 70, 1);
	core::World::CreateShuttleOptions options{ 1, 4, { 0, 10, 60 }, 2 };
	options.capacity = 3;
	options.doorMask = 0b0001;
	options.minimumDwellSeconds = 1.0f;
	auto created = world.addShuttle(1, 0, 0, 65, options);
	world.finishBuild();
	auto id = world.createAgent("Slow walker", platform, 0, 0.5f);
	auto agent = world.lookupAgent(id).entity;
	world.pauseSimulation();
	require(world.setAgentIndividualWalkSpeedModifier(id, 0.8f), "Cannot configure walking speed");
	auto policy = world.getRouteChoicePolicy();
	policy.shuttleHeadwaySeconds = 12.0f;
	world.setRouteChoicePolicy(policy);
	auto graph = world.getGraph();
	auto source = graph->getClosestVertexInSector(world.getSector(platform).get(), { 0.5f, 0 });
	auto near = graph->getClosestVertexInSector(world.getSector(platform).get(), { 10.5f, 0 });
	auto far = graph->getClosestVertexInSector(world.getSector(platform).get(), { 60.5f, 0 });
	require(source && near && far, "Missing Shuttle platform vertices");
	auto shortPath = graph->calculatePath(agent, source, near);
	auto longPath = graph->calculatePath(agent, source, far);
	require(shortPath && !usesShuttle(shortPath) && usesShuttle(longPath),
		"One Shuttle headway must favour walking short trips and riding long trips");

	core::RouteDecisionContext remote{ agent, policy.baselineProfile, policy,
		nullptr, agent->getWalkSpeed(), &world };
	float totalRide = 0;
	float totalDwell = 0;
	uint32_t boardings = 0;
	for (auto const& node : longPath->nodes)
	{
		if (!node.edge) continue;
		auto facts = node.edge->getDirectedTraversalFacts(node.targetVertex, remote);
		require(facts.feasible && facts.objectiveDurationSeconds.has_value(),
			"Shuttle journey must expose objective duration");
		if (node.edge->getType() == core::EdgeType::Shuttle)
		{
			totalRide += facts.components.motionSeconds;
			totalDwell += facts.components.expectedWaitSeconds;
			require(facts.components.interactionUnits == 0 && facts.components.crowdingUnits == 0,
				"Shuttle ride repeated admission costs");
		}
		else if (facts.components.interactionUnits >= policy.shuttleBoardingInteraction) ++boardings;
	}
	auto shuttle = std::dynamic_pointer_cast<core::ShuttleTransit>(created.shuttle.sector)->getShuttle();
	require(std::abs(totalRide - 60.0f / shuttle->getSpeed()) < 0.001f
		&& std::abs(totalDwell - 2.0f) < 0.001f && boardings == 1,
		"Shuttle journey must charge actual ride distance, intermediate dwell, and one boarding");

	for (auto const& edge : graph->getEdges())
	{
		if (edge->getType() != core::EdgeType::Door) continue;
		for (uint32_t i = 0; i < 2; ++i)
		{
			auto target = edge->getVertex(i);
			auto origin = edge->getOtherVertex(target);
			if (origin->getSector().get() != world.getSector(platform).get()) continue;
			auto facts = edge->getDirectedTraversalFacts(target, remote);
			require(facts.components.expectedWaitSeconds >= policy.shuttleHeadwaySeconds
				* (0.5f + policy.shuttleExpectedQueuePassengers / options.capacity),
				"Shuttle capacity must contribute expected missed-service waiting");
			auto highWait = remote.profile;
			highWait.waitingAversion = 3;
			auto highInteraction = remote.profile;
			highInteraction.interactionAversion = 3;
			auto highCrowd = remote.profile;
			highCrowd.crowdAversion = 3;
			auto baseline = policy.evaluate(facts, remote.profile);
			for (auto profile : { highWait, highInteraction, highCrowd })
			{
				auto result = policy.evaluate(facts, profile);
				require(result->perceivedCost > baseline->perceivedCost
					&& result->objectiveDurationSeconds == baseline->objectiveDurationSeconds,
					"Shuttle aversions must change preference, not objective timing");
			}
		}
	}

	// Observe a real local boarding queue while the vehicle is still far away.
	auto boardingNode = std::find_if(longPath->nodes.begin(), longPath->nodes.end(),
		[&](auto const& node) { return node.edge && node.edge->getType() == core::EdgeType::Door
			&& node.edge->getOtherVertex(node.targetVertex)->getSector().get() == world.getSector(platform).get(); });
	require(boardingNode != longPath->nodes.end(), "Missing Shuttle boarding threshold");
	auto remoteBefore = boardingNode->edge->getDirectedTraversalFacts(boardingNode->targetVertex, remote);
	auto crowdAverseId = world.createAgent("Crowd averse", platform, 0, 0.5f);
	auto crowdAverse = world.lookupAgent(crowdAverseId).entity;
	require(world.setAgentIndividualWalkSpeedModifier(crowdAverseId, 0.8f)
		&& world.setAgentIndividualCrowdAversion(crowdAverseId, 3.0f)
		&& world.setAgentIndividualCrowdAversion(id, 0.0f), "Cannot configure crowd preferences");
	require(world.resumeSimulation(), "Cannot resume Shuttle observation fixture");
	for (uint32_t i = 0; i < 40; ++i)
	{
		auto waiterId = world.createAgent("Local waiter", platform, 0, 0.5f);
		world.lookupAgent(waiterId).entity->setPath(longPath, true);
	}
	world.advanceTicks(3);
	auto remoteAfter = boardingNode->edge->getDirectedTraversalFacts(boardingNode->targetVertex, remote);
	require(remoteBefore.components.expectedWaitSeconds == remoteAfter.components.expectedWaitSeconds
		&& remoteBefore.components.crowdingUnits == remoteAfter.components.crowdingUnits,
		"Remote Shuttle queue or vehicle state leaked into routing");
	core::RouteDecisionContext local{ agent, remote.profile, policy,
		world.getSector(platform).get(), agent->getWalkSpeed(), &world };
	auto crowded = boardingNode->edge->getDirectedTraversalFacts(boardingNode->targetVertex, local);
	require(crowded.components.crowdingUnits > 1.0f
		&& crowded.components.expectedWaitSeconds > remoteAfter.components.expectedWaitSeconds,
		"Local Shuttle queue did not refine crowding and missed-service estimates");

	// Hold the same service state fixed; only Crowd aversion changes this choice.
	policy.shuttleHeadwaySeconds = 0;
	policy.shuttleBoardingSeconds = 0.0f;
	policy.shuttleAlightingSeconds = 0.0f;
	world.setRouteChoicePolicy(policy);
	auto uncrowdedPreference = graph->calculatePath(agent, source, near);
	auto crowdedPreference = graph->calculatePath(crowdAverse, source, near);
	require(usesShuttle(uncrowdedPreference) && crowdedPreference && !usesShuttle(crowdedPreference),
		"Local Shuttle crowding must let Crowd aversion select walking");

	// Static previews use the same authored Shuttle facts without an Agent's World link.
	auto preview = graph->calculatePath(nullptr, source, far);
	require(preview && preview->nodes.back().objectiveDurationSeconds.has_value(),
		"Shuttle preview lost objective duration");
}
