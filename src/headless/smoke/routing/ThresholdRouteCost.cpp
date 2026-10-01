#include "Checks.h"
#include <cmath>
#include <filesystem>
#include "core/AgentTagRegistryDocument.h"
#include <stdexcept>
#include "core/World.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/DoorSectorObject.h"
#include "core/Edge.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void actualPositionChoosesManualAlternative(smoke::Context const& smokeContext)
	{
		auto const fixture = smokeContext.fixture("src/headless/fixtures/threshold-route-cost.world.yaml");
		auto world = core::loadWorldDocument(fixture);
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		require(agent != nullptr, "Realistic-pathing fixture has no Agent");
		auto path = agent->getPath();
		require(path && !path->nodes.empty(), "Restored realistic Path missing");
		bool crossed = false;
		for (auto const& node : path->nodes)
		{
			if (!node.edge || node.edge->getType() != core::EdgeType::Door) continue;
			require(!node.edge->requiresButton(), "Actual-position route preferred the button Door");
			crossed = true;
		}
		require(crossed, "Realistic Path did not exercise Door alternatives");
		auto const destination = path->nodes.back().targetVertex;
		auto graph = world->getGraph();
		auto warm = graph->calculatePath(agent, destination);
		auto const allocations = graph->getScratchAllocationCount();
		for (int i = 0; i < 20; ++i)
		{
			auto repeated = graph->calculatePath(agent, destination);
			require(repeated && repeated->nodes.front().targetVertex == warm->nodes.front().targetVertex
				&& repeated->nodes.back().cumulativePerceivedCost == warm->nodes.back().cumulativePerceivedCost,
				"Actual-position routing is not deterministic");
		}
		require(graph->getScratchAllocationCount() == allocations,
			"Actual-position routing grew warmed search scratch");
		auto const approach = agent->getGlobalPosition().distanceTo(path->nodes.front().targetVertex->getPosition())
			/ agent->getWalkSpeed();
		require(std::abs(path->nodes.front().cumulativePerceivedCost - approach) < 0.0001f
			&& path->nodes.front().objectiveDurationSeconds == approach,
			"Actual approach cost was lost in reconstruction");
		require(world->resumeSimulation() && world->advanceTicks(3600), "Realistic journey could not advance");
		require(agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& agent->getSector() == destination->getSector().get(),
			"Actual-position Path failed to reach its destination");
	}

	void walkingAndBulkhead()
	{
		core::World world("Walking and Bulkhead costs", 12, 1);
		auto left = world.addRoom("Left", 0, 0, 0, 6, 1);
		world.addRoom("Right", 0, 0, 6, 6, 1);
		world.addSectorBulkheadDoor(0, 0, 6, CORE_SIDE_LEFT, {});
		uint32_t a, b;
		world.addSectorMarker(left, 0, 1.0f, &a);
		world.addSectorMarker(left, 0, 1.5f, &b);
		world.finishBuild();
		for (auto const& edge : world.getGraph()->getEdges())
		{
			core::RouteDecisionContext slow{ nullptr, {}, {}, edge->getVertex(0)->getSector().get(), 0.25f };
			core::RouteDecisionContext fast{ nullptr, {}, {}, edge->getVertex(0)->getSector().get(), 0.75f };
			auto s = edge->getDirectedTraversalFacts(edge->getVertex(1), slow);
			auto f = edge->getDirectedTraversalFacts(edge->getVertex(1), fast);
			if (edge->getLength() > 0 && (edge->getType() == core::EdgeType::Location
				|| edge->getType() == core::EdgeType::BulkheadDoor))
				require(std::abs(s.components.motionSeconds - edge->getLength() / 0.25f) < 0.0001f
					&& std::abs(f.components.motionSeconds * 3 - s.components.motionSeconds) < 0.0001f,
					"Physical distance or effective Walk speed ignored");
			if (edge->getType() == core::EdgeType::BulkheadDoor)
				require(s.components.knownWaitSeconds == CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME,
					"Bulkhead preparation missing");
		}
	}

	void observedQueue()
	{
		core::World world("Observed threshold queue", 12, 1);
		auto fore = world.addRoom("Fore", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto door = world.addSectorDoor(0, 0, 5, {});
		uint32_t destination;
		world.addSectorMarker(back, 0, 8.5f, &destination);
		world.finishBuild();
		auto graph = world.getGraph();
		auto observer = world.lookupAgent(world.createAgent("Observer", fore, 0, 1)).entity;
		std::shared_ptr<const core::Edge> threshold;
		for (auto const& edge : graph->getEdges())
			if (edge->getTraversalResourceId() == door.traversalResource) threshold = edge;
		require(bool(threshold), "Queue fixture missing Door");
		auto source = threshold->getVertex(0);
		if (source->getSector().get() != observer->getSector()) source = threshold->getVertex(1);
		auto target = threshold->getOtherVertex(source);
		core::RouteDecisionContext local{ observer, {}, {}, observer->getSector(), observer->getWalkSpeed() };
		core::RouteDecisionContext remote{ observer, {}, {}, target->getSector().get(), observer->getWalkSpeed() };
		auto before = threshold->getDirectedTraversalFacts(target, remote);
		for (int i = 0; i < 4; ++i)
		{
			auto waiter = world.lookupAgent(world.createAgent("Waiter", fore, 0, 5.5f)).entity;
			waiter->setPath(graph->calculatePath(waiter, graph->getVertexByIdentifier(destination)), true);
		}
		world.advanceTicks(10);
		auto delay = observer->estimateTraversalDelay(door.traversalResource,
			core::SectorId{ (uint64_t)source->getSector()->getIndex() + 1 });
		require(delay > 0, "Queue fixture did not produce waiting Agents");
		auto seen = threshold->getDirectedTraversalFacts(target, local);
		auto unseen = threshold->getDirectedTraversalFacts(target, remote);
		require(std::abs(seen.components.knownWaitSeconds - (2 + delay)) < 0.001f,
			"Approach queue not charged exactly once");
		require(before.components.expectedWaitSeconds == unseen.components.expectedWaitSeconds,
			"Remote queue changed immediate estimate");
		auto reverse = threshold->getDirectedTraversalFacts(source, local);
		require(reverse.components.knownWaitSeconds == 0, "Queue observed from wrong approach side");
		for (auto const& edge : source->getEdges())
			if (edge->getType() == core::EdgeType::Location)
				require(edge->getDirectedTraversalFacts(edge->getOtherVertex(source), local).components.knownWaitSeconds == 0,
					"Queue charged again on approach walking");
	}

	void thresholdChoices()
	{
		core::World world("Threshold route costs", 16, 1);
		auto fore = world.addRoom("Fore", 0, 0, 0, 16, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 16, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = options.controls[1] = true;
		auto remote = world.addSectorDoor(0, 0, 4, options);
		options.controls[0] = options.controls[1] = false;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto automatic = world.addSectorDoor(0, 0, 5, options);
		uint32_t a, b;
		world.addSectorMarker(fore, 0, 4.6f, &a);
		world.addSectorMarker(back, 0, 4.6f, &b);
		world.finishBuild();
		auto agent = world.lookupAgent(world.createAgent("Observer", fore, 0, 4.5f)).entity;
		auto graph = world.getGraph();
		auto getDoor = [&](auto const& result)
		{
			return std::static_pointer_cast<const core::DoorSectorObject>(
				result.door.sector->getObject(result.door.index))->getDoor();
		};
		auto remoteDoor = getDoor(remote);
		auto autoDoor = getDoor(automatic);
		auto choose = [&]()
		{
			auto path = graph->calculatePath(agent, graph->getVertexByIdentifier(a), graph->getVertexByIdentifier(b));
			require(bool(path), "Threshold alternatives became unreachable");
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getType() == core::EdgeType::Door)
					return node.edge->getTraversalResourceId();
			return core::TraversalResourceId{};
		};
		autoDoor->requestOpen(); autoDoor->update(10);
		require(choose() == automatic.traversalResource, "Open automatic alternative did not beat closed remote Door");
		remoteDoor->requestOpen(); remoteDoor->update(10);
		require(choose() == remote.traversalResource, "New journey did not prefer shorter open route");

		for (auto const& edge : graph->getEdges())
		{
			if (edge->getTraversalResourceId() != remote.traversalResource) continue;
			auto target = edge->getVertex(1);
			auto source = edge->getVertex(0);
			core::RouteDecisionContext local{ agent, {}, {}, source->getSector().get(), agent->getWalkSpeed() };
			core::RouteDecisionContext unseen{ agent, {}, {}, nullptr, agent->getWalkSpeed() };
			auto unknownOpen = edge->getDirectedTraversalFacts(target, unseen);
			remoteDoor->requestClose(); remoteDoor->update(10);
			auto closed = edge->getDirectedTraversalFacts(target, local);
			auto unknownClosed = edge->getDirectedTraversalFacts(target, unseen);
			require(unknownOpen.components.expectedWaitSeconds == unknownClosed.components.expectedWaitSeconds
				&& unknownOpen.components.interactionUnits == unknownClosed.components.interactionUnits,
				"Unobserved Door state leaked into route costs");
			require(closed.components.motionSeconds == 0.1f && closed.components.knownWaitSeconds == 2,
				"Door crossing or opening duration missing");
			remoteDoor->setHeight(core::Door::Height::Tall);
			for (auto style : { core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
				core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart })
			{
				remoteDoor->setOpenStyle(style);
				auto facts = edge->getDirectedTraversalFacts(target, local);
				require(facts.feasible && facts.components.knownWaitSeconds == closed.components.knownWaitSeconds,
					"Opening style affected routing");
				require(choose() == automatic.traversalResource, "Opening style changed selected Path");
			}
			remoteDoor->configureTraversal(core::DoorActivationMode::Manual, remote.traversalResource, 5);
			auto manual = edge->getDirectedTraversalFacts(target, local);
			remoteDoor->configureTraversal(core::DoorActivationMode::Automatic, remote.traversalResource, 5);
			auto automaticFacts = edge->getDirectedTraversalFacts(target, local);
			require(closed.components.interactionUnits > manual.components.interactionUnits
				&& manual.components.interactionUnits > automaticFacts.components.interactionUnits,
				"Activation inconvenience was not distinguished");
		}
	}
}


namespace routing_smoke
{
	void registerThresholdRouteCost(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "actualPositionChoosesManualAlternative", [](smoke::Context const& smokeContext) { actualPositionChoosesManualAlternative(smokeContext); } });
		checks.push_back({ "walkingAndBulkhead", [](smoke::Context const&) { walkingAndBulkhead(); } });
		checks.push_back({ "observedQueue", [](smoke::Context const&) { observedQueue(); } });
		checks.push_back({ "thresholdChoices", [](smoke::Context const&) { thresholdChoices(); } });
	}
}
