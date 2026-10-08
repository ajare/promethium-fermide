#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/BulkheadDoorEdge.h"
#include "core/DoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include <cmath>

namespace
{
	using smoke::require;

	void clean(core::World const& world)
	{
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Bulkhead traversal leaked");
		for (auto const& resource : snapshot.traversalResources)
		{
			require(resource.openLeaseCount == 0, "Bulkhead lease leaked");
			for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Bulkhead queue leaked");
			for (auto const& lane : resource.crossingLanes) require(!lane.owner, "Bulkhead lane leaked");
		}
	}

	std::vector<std::shared_ptr<const core::BulkheadDoorEdge>> thresholds(core::World const& world, float height)
	{
		std::vector<std::shared_ptr<const core::BulkheadDoorEdge>> result;
		for (auto const& edge : world.getGraph()->getEdges())
			if (auto bulkhead = std::dynamic_pointer_cast<const core::BulkheadDoorEdge>(edge))
			{
				auto door = bulkhead->getDoor();
				require(!door->setHeightScale(.5f), "Bulkhead gained Height authoring");
				// Public Shape value assignment supplies a reduced physical fixture,
				// without a new authoring command or test-only mutation hook.
				static_cast<core::Shape&>(*door) = core::Shape(door->getPosition(), {door->getSize().x, height});
				result.push_back(bulkhead);
			}
		require(!result.empty(), "Missing Bulkhead threshold");
		return result;
	}
}

void runBulkheadCrawlingJourneys(smoke::Context const&)
{
	for (float opening : {.7f, .45f, .25f, .135f, .134995f, .13498f, .1f})
	for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::Automatic,
		core::DoorActivationMode::RemoteControlled})
	for (bool reverse : {false, true})
	for (bool alternate : {false, true})
	{
		core::World world("Bulkhead crawling journey", 12, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto middle = world.addRoom("Middle", 0, 0, 4, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 8, 4, 1);
		core::World::CreateBulkheadDoorOptions options;
		options.activationMode = mode;
		options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
		world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
		world.addSectorBulkheadDoor(0, 0, 7, CORE_SIDE_RIGHT, options);
		if (alternate)
		{
			world.addRoom("Bypass", 1, 0, 0, 12, 1);
			world.addSectorDoor(0, 0, 1, {});
			world.addSectorDoor(0, 0, 10, {});
		}
		auto origin = reverse ? right : left, destination = reverse ? left : right;
		world.addSectorMarker(destination, 0, reverse ? 1.5f : 2.5f, "Goal");
		world.finishBuild();
		auto edges = thresholds(world, opening);
		auto id = world.createAgent("Traveller", origin, 0, reverse ? 2.5f : 1.5f);
		auto agent = world.lookupAgent(id).entity;
		auto policy = world.getRouteChoicePolicy();
		core::RouteDecisionContext context{agent, policy.baselineProfile, policy, agent->getSector(),
			agent->getWalkSpeed(), &world, agent->getClimbSpeed(), false, 0, 0, {}, 0, false};
		bool const fits = opening >= .13499f, crawls = fits && opening < .45f;
		for (auto const& edge : edges)
		for (unsigned direction = 0; direction != 2; ++direction)
		{
			auto target = edge->getVertex(direction);
			auto direct = edge->getDirectedTraversalFacts(target, context);
			auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
			require(direct.feasible == fits && captured.feasible == fits, "Bulkhead clearance disagrees across route seams");
			if (fits)
			{
				auto motion = edge->getLength() / agent->getWalkSpeed()
					/ (crawls ? agent->getPhysicalBaseline().crawlingSpeedRatio : 1.0f);
				require(std::abs(direct.components.motionSeconds - motion) < .00001f
					&& std::abs(captured.components.motionSeconds - motion) < .00001f,
					"Bulkhead crawling cost is not type-ratio-slowed motion");
			}
			else require(direct.exclusionReason == core::RouteExclusionReason::Clearance
				&& captured.exclusionReason == core::RouteExclusionReason::Clearance, "Bulkhead impossible fit lost Clearance exclusion");
		}
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Bulkhead intent refused");
		unsigned losses = 0, arrivals = 0, episodes = 0, motionTicks = 0;
		bool wasCrawling = false, waited = false;
		float lastX = agent->getGlobalPosition().x;
		for (unsigned tick = 0; tick != 4000; ++tick)
		{
			world.advanceTick();
			auto x = agent->getGlobalPosition().x;
			bool crawling = agent->getPose() == core::Pose::Crawling;
			if (crawls && !alternate && agent->getState() == core::Agent::State::TraversingEdge)
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					if (request.owner == id && request.edgeType == core::EdgeType::BulkheadDoor)
						require(crawling, "Bulkhead admitted Standing: physical height=" + std::to_string(edges.front()->getDoor()->getSize().y)
							+ " mode=" + std::to_string(static_cast<int>(edges.front()->getDoor()->classifyAgentCrossing(*agent, agent->getGlobalPosition().y)))
							+ " y=" + std::to_string(agent->getGlobalPosition().y));
			require(agent->getPose() != core::Pose::Crouching, "Bulkhead automatically crouched");
			waited |= agent->getState() == core::Agent::State::WaitingForTraversal;
			if (crawling)
			{
				require(agent->getState() == core::Agent::State::TraversingEdge, "Bulkhead lowered while waiting");
				if (!wasCrawling) ++episodes;
				if (wasCrawling && std::abs(x - lastX) > .00001f)
				{
					++motionTicks;
					require(std::abs(x - lastX) <= agent->getWalkSpeed() / 120.f + .00001f,
						"Crawling Bulkhead movement exceeded half speed");
				}
			}
			if (wasCrawling && !crawling)
			{
				// Both endpoints are .3 from the wall: body width .4 plus
				// the .2 Door thickness. Standing is safe only beyond this band.
				bool beyond = reverse ? (x <= 3.70001f || (x <= 7.70001f && x > 4.f))
					: (x >= 8.29999f || (x >= 4.29999f && x < 8.f));
				require(beyond && agent->getPose() == core::Pose::Standing, "Bulkhead stood before full physical exit");
			}
			wasCrawling = crawling;
			lastX = x;
			for (auto const& event : world.consumeSimulationEvents())
			{
				losses += event.type == core::SimulationEventType::RouteLost;
				arrivals += event.type == core::SimulationEventType::DestinationReached;
			}
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing,
			"Bulkhead journey stranded or leaked Pose");
		require((agent->getSector() == world.getSector(destination).get()) == (fits || alternate)
			&& losses == (!fits && !alternate ? 1u : 0u) && arrivals == (fits || alternate ? 1u : 0u),
			"Bulkhead journey did not arrive/choose alternative/lose route");
		if (fits) require(waited && (alternate ? (episodes == 0 || episodes == 2) : episodes == (crawls ? 2u : 0u))
			&& (episodes == 0 || motionTicks > 20),
			"Repeated Bulkhead crossings missed Standing waits or physical Crawling: opening=" + std::to_string(opening)
			+ " mode=" + std::to_string(static_cast<int>(mode)) + " reverse=" + std::to_string(reverse)
			+ " alternate=" + std::to_string(alternate) + " waited=" + std::to_string(waited)
			+ " episodes=" + std::to_string(episodes) + " motionTicks=" + std::to_string(motionTicks));
		(void)middle;
		clean(world);
	}

	// Competing real routes: unobstructed direct same-Layer walking wins when
	// Standing fits; the added Crawling motion makes the adjacent-Layer bypass
	// preferable. Opening waits and interaction premiums are neutral here.
	for (float opening : {.7f, .25f})
	{
		core::World world("Bulkhead crawling route cost", 8, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
		world.addRoom("Bypass", 1, 0, 0, 8, 1);
		core::World::CreateBulkheadDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.controls[0] = options.controls[1] = false;
		auto bulkhead = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
		core::World::CreateDoorOptions ordinary;
		ordinary.activationMode = core::DoorActivationMode::Automatic;
		auto a = world.addSectorDoor(0, 0, 3, ordinary);
		auto b = world.addSectorDoor(0, 0, 4, ordinary);
		world.addSectorMarker(right, 0, 1.5f, "Goal");
		world.finishBuild();
		auto bulkheadEdges = thresholds(world, opening);
		for (auto const& edge : bulkheadEdges) require(edge->getDoor()->requestOpen(), "Cost Bulkhead opening refused");
		for (auto const& edge : world.getGraph()->getEdges())
			if (auto door = std::dynamic_pointer_cast<const core::DoorEdge>(edge))
				require(door->getDoor()->requestOpen(), "Cost bypass opening refused");
		auto policy = world.getRouteChoicePolicy();
		policy.unobservedDoorClosedProbability = 0.f;
		policy.thresholdInteraction = 0.f;
		world.setRouteChoicePolicy(policy);
		std::vector<std::pair<core::TraversalResourceId, core::DoorOpenLeaseId>> leases;
		for (auto resource : {bulkhead.traversalResource, a.traversalResource, b.traversalResource})
			leases.push_back({resource, world.acquireDoorOpenLease(resource)});
		world.advanceTicks(300);
		auto id = world.createAgent("Traveller", left, 0, 2.5f);
		auto agent = world.lookupAgent(id).entity;
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Competing Bulkhead intent refused");
		bool usedBulkhead = false, usedDoor = false;
		for (unsigned tick = 0; tick != 2000; ++tick)
		{
			world.advanceTick();
			for (auto const& request : world.getSimulationSnapshot().traversalRequests)
				if (request.owner == id && request.state == core::TraversalRequestState::Granted)
				{
					usedBulkhead |= request.edgeType == core::EdgeType::BulkheadDoor;
					usedDoor |= request.edgeType == core::EdgeType::Door;
				}
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		require(agent->getSector() == world.getSector(right).get() && usedBulkhead == (opening == .7f)
			&& usedDoor == (opening == .25f), "Slower Bulkhead Crawling did not change competing route choice: opening=" + std::to_string(opening)
			+ " bulkhead=" + std::to_string(usedBulkhead) + " bypass=" + std::to_string(usedDoor));
		for (auto const& [resource, lease] : leases) require(world.releaseDoorOpenLease(resource, lease), "Cost fixture lease release refused");
		clean(world);
	}
}

void runBulkheadCrawlingLifecycle(smoke::Context const&)
{
	for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::Automatic,
		core::DoorActivationMode::RemoteControlled})
	for (bool reverse : {false, true})
	for (int change = 0; change != 11; ++change)
	{
		core::World world("Bulkhead crawling lifecycle", 8, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
		core::World::CreateBulkheadDoorOptions options;
		options.activationMode = mode;
		options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
		options.speedOverride = .1f;
		auto created = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
		auto origin = reverse ? right : left, destination = reverse ? left : right;
		world.addSectorMarker(destination, 0, reverse ? 1.5f : 2.5f, "Goal");
		world.addSectorMarker(destination, 0, reverse ? .5f : 3.5f, "Replacement");
		world.finishBuild();
		auto door = thresholds(world, .12f).front()->getDoor();
		auto id = world.createAgent("Traveller", origin, 0, reverse ? 2.5f : 1.5f);
		auto agent = world.lookupAgent(id).entity;
		world.pauseSimulation();
		require(world.setAgentIndividualHeightModifier(id, .8f) && world.resumeSimulation(), "Bulkhead initial Height refused");
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Bulkhead lifecycle intent refused");
		bool changed = false, crossed = false, brokenOpen = false;
		unsigned arrivals = 0, losses = 0, cancellations = 0;
		for (unsigned tick = 0; tick != 4000; ++tick)
		{
			world.advanceTick();
			bool crawling = agent->getPose() == core::Pose::Crawling;
			crossed |= crawling;
			if (!crawling) require(agent->getPose() == core::Pose::Standing, "Bulkhead lifecycle selected unintended Pose");
			bool const before = change >= 5 && change <= 8;
			if (!changed && (before ? agent->getState() == core::Agent::State::WaitingForTraversal : crawling))
			{
				changed = true;
				if (change == 0)
				{
					auto position = agent->getGlobalPosition();
					world.pauseSimulation();
					auto frozen = world.getSimulationTick();
					world.advanceTicks(20);
					require(world.getSimulationTick() == frozen && agent->getGlobalPosition() == position
						&& agent->getPose() == core::Pose::Crawling, "Pause changed Bulkhead crossing");
					require(world.setAgentActive(id, false) && world.resumeSimulation(), "Bulkhead deactivation refused");
					world.advanceTicks(20);
					require(agent->getGlobalPosition() == position && agent->getPose() == core::Pose::Crawling,
						"Deactivation changed Bulkhead crossing");
					world.pauseSimulation();
					require(world.setAgentActive(id, true) && world.resumeSimulation(), "Bulkhead reactivation refused");
				}
				else if (change == 1 || change == 6) require(world.cancelAgentMovement(id).accepted(), "Bulkhead cancellation refused");
				else if (change == 2 || change == 7) require(world.moveAgentToNamedMarker(id, "Replacement").accepted(), "Bulkhead replacement refused");
				else if (change == 3 || change == 5)
				{
					world.pauseSimulation();
					require(world.setAgentIndividualHeightModifier(id, 1.f) && world.resumeSimulation(), "Bulkhead enlargement refused");
				}
				else if (change == 4)
				{
					world.resetSimulation();
					require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing, "Bulkhead Reset retained Pose");
					break;
				}
				else if (change == 8 || change == 9)
				{
					brokenOpen = door->isOpen();
					require(world.setDoorBroken(created.traversalResource, true), "Bulkhead Broken edit refused");
				}
				else if (change == 10)
					static_cast<core::Shape&>(*door) = core::Shape(door->getPosition(), {door->getSize().x, .1f});
			}
			for (auto const& event : world.consumeSimulationEvents())
			{
				arrivals += event.type == core::SimulationEventType::DestinationReached;
				losses += event.type == core::SimulationEventType::RouteLost;
				cancellations += event.type == core::SimulationEventType::MovementCancelled;
			}
			if (changed && agent->getState() == core::Agent::State::Idle) break;
		}
		require(changed, "Bulkhead lifecycle transition not exercised");
		if (change != 4)
		{
			bool const refused = change == 5 || (change == 8 && !brokenOpen);
			bool finishes = !refused && change != 6;
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& (agent->getSector() == world.getSector(destination).get()) == finishes,
				"Bulkhead lifecycle violated physical commitment: change=" + std::to_string(change));
			require(arrivals == (change == 1 || change == 6 || refused ? 0u : 1u)
				&& cancellations == (change == 1 || change == 2 || change == 6 || change == 7 ? 1u : 0u)
				&& losses == (refused ? 1u : 0u), "Bulkhead lifecycle outcome missing/duplicated: change=" + std::to_string(change)
				+ " arrivals=" + std::to_string(arrivals) + " losses=" + std::to_string(losses)
				+ " cancellations=" + std::to_string(cancellations));
			require(!finishes || crossed, "Bulkhead committed exit never crawled");
		}
		clean(world);
	}
}

void runBulkheadCrawlingGates(smoke::Context const&)
{
	unsigned standingTicks[2]{};
	for (float opening : {.7f, .25f})
	for (bool reverse : {false, true})
	for (int restriction = 0; restriction != 7; ++restriction)
	{
		core::World world("Protected Bulkhead crawling", 8, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
		core::World::CreateBulkheadDoorOptions options;
		options.activationMode = restriction == 5 ? core::DoorActivationMode::Unavailable : core::DoorActivationMode::RemoteControlled;
		options.controls[0] = options.controls[1] = restriction != 5;
		auto created = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
		auto origin = reverse ? right : left, destination = reverse ? left : right;
		world.addSectorMarker(destination, 0, 2.f, "Goal");
		world.finishBuild();
		auto door = thresholds(world, opening).front()->getDoor();
		auto id = world.createAgent("Traveller", origin, 0, 2.f);
		auto agent = world.lookupAgent(id).entity;
		if (restriction == 6)
		{
			auto lease = world.acquireDoorOpenLease(created.traversalResource);
			require(door->requestOpen(), "Bulkhead open request refused");
			world.advanceTicks(300);
			require(door->isOpen() && world.setDoorBroken(created.traversalResource, true), "Broken-open Bulkhead fixture failed");
			require(world.releaseDoorOpenLease(created.traversalResource, lease), "Bulkhead external lease release refused");
		}
		world.pauseSimulation();
		if (restriction <= 1 || restriction == 6)
		{
			auto key = world.addAccessPermission("Key");
			for (auto const& control : created.controls)
				require(world.setInteractionPointPermissionRequirement(control.interactionPoint, {key}), "Bulkhead control protection refused");
			if (restriction == 0) require(world.setAgentAccessPermissionGrant(id, key, true), "Bulkhead grant refused");
		}
		if (restriction == 2 || restriction == 3)
		{
			core::MobilityProfile profile;
			profile.set(restriction == 2 ? core::TraversalKind::Buttons : core::TraversalKind::Door, core::MobilityUse::CannotUse);
			require(world.setAgentIndividualMobilityProfile(id, profile), "Bulkhead Mobility refused");
		}
		if (restriction == 4) require(world.setDoorBroken(created.traversalResource, true), "Broken-closed Bulkhead edit refused");
		require(world.resumeSimulation() && world.moveAgentToNamedMarker(id, "Goal").accepted(), "Protected Bulkhead intent refused");
		unsigned arrivals = 0, losses = 0, crawled = 0, crossingTicks = 0;
		for (unsigned tick = 0; tick != 4000; ++tick)
		{
			world.advanceTick();
			crawled += agent->getPose() == core::Pose::Crawling;
			if (agent->getState() == core::Agent::State::TraversingEdge)
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					crossingTicks += request.owner == id && request.resource == created.traversalResource
						&& request.state == core::TraversalRequestState::Granted;
			for (auto const& event : world.consumeSimulationEvents())
			{
				arrivals += event.type == core::SimulationEventType::DestinationReached;
				losses += event.type == core::SimulationEventType::RouteLost;
			}
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		bool allowed = restriction == 0 || restriction == 6;
		require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
			&& (agent->getSector() == world.getSector(destination).get()) == allowed
			&& (crawled > 0) == (allowed && opening < .45f) && arrivals == (allowed ? 1u : 0u) && losses == (allowed ? 0u : 1u),
			"Bulkhead Crawling bypassed feasibility/operation rule: restriction=" + std::to_string(restriction));
		if (restriction == 6)
		{
			if (opening == .7f) standingTicks[reverse] = crossingTicks;
			else require(crossingTicks >= standingTicks[reverse] * 2 - 1
				&& crossingTicks <= standingTicks[reverse] * 2 + 1,
				"Bulkhead physical crossing duration did not double: standing=" + std::to_string(standingTicks[reverse])
				+ " crawling=" + std::to_string(crossingTicks));
		}
		clean(world);
	}
}
