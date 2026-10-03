#include "Checks.h"
#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/MarkerSectorObject.h"
#include "core/World.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <yaml-cpp/yaml.h>

namespace
{
	struct VertexSpec { char const* key; float x; };
	struct EdgeSpec { char const* from; char const* to; int depth; };
	using Score = std::pair<float, uint64_t>;

	Score reference(std::shared_ptr<const core::Vertex> source, std::shared_ptr<const core::Vertex> target, int depth, float speed)
	{
		// Deliberately enumerate complete simple Paths on the tiny authored graph,
		// independently of the production search, frontier and label representation.
		Score best{ std::numeric_limits<float>::infinity(), 0 };
		std::set<core::Vertex const*> visited;
		std::function<void(std::shared_ptr<const core::Vertex>, int, Score)> visit = [&](auto vertex, int incoming, Score score)
		{
			if (vertex == target) { best = std::min(best, score); return; }
			if (vertex != source)
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
					marker && marker->hasProperty(core::MarkerProperty::BlocksPathing)) return;
			visited.insert(vertex.get());
			for (auto const& edge : vertex->getEdges())
			{
				auto next = edge->getOtherVertex(vertex);
				if (visited.contains(next.get())) continue;
				auto cost = edge->getLength() == 0 ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : edge->getLength() / speed;
				visit(next, edge->getLocalDepth(), { score.first + cost,
					score.second + static_cast<uint64_t>(std::abs(int64_t{ incoming } - edge->getLocalDepth())) });
			}
			visited.erase(vertex.get());
		};
		visit(source, depth, { 0, 0 });
		return best;
	}

	void scenario(smoke::Context const& context, char const* name,
		std::vector<VertexSpec> vertices, std::vector<EdgeSpec> edges,
		int incoming, int expectedFirstDepth)
	{
		using smoke::require;
		auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/desk.furniture.yaml").string());
		auto definition = yaml["furnitureCatalogue"]["definitions"][0];
		definition["usablePoints"] = YAML::Load("[{key: history, label: History, x: 0}, {key: start, label: Start, x: 0.25}, {key: goal, label: Goal, x: 1.75}]");
		definition["vertices"] = YAML::Load("[{key: history, x: 0, external: true, usablePoint: history}, {key: start, x: 0.25, usablePoint: start}, {key: goal, x: 1.75, usablePoint: goal}]");
		for (auto const& vertex : vertices)
		{
			YAML::Node node; node["key"] = vertex.key; node["x"] = vertex.x;
			definition["vertices"].push_back(node);
		}
		definition["edges"] = YAML::Node(YAML::NodeType::Sequence);
		edges.insert(edges.begin(), { "history", "start", incoming });
		for (auto const& edge : edges)
		{
			YAML::Node node; node["from"] = edge.from; node["to"] = edge.to; node["depthOffset"] = edge.depth;
			definition["edges"].push_back(node);
		}
		auto path = context.temporaryRoot() / (std::string(name) + ".furniture.yaml");
		{ std::ofstream file(path); file << yaml; }
		core::World world(name, 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::load(path));
		world.placeFurniture(room, "desk", 2, 0, "Desk");
		world.finishBuild();
		auto graph = world.getGraph();
		auto point = [&](char const* key)
		{
			for (auto const& destination : world.furniture().front().destinations)
				if (destination.key == key)
					for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
						if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
							object && object->getMarker()->getId() == destination.marker)
							return graph->getVertexForObject(object);
			throw std::runtime_error("Missing authored point");
		};
		auto history = point("history"), start = point("start"), goal = point("goal");
		auto agent = world.lookupAgent(world.createAgent("Walker", room, 0, incoming == 0 ? 2.25f : 2.0f)).entity;
		require(agent->getLocalDepth() == 0, "No-history departure depth changed");
		if (incoming != 0)
		{
			agent->setPath(graph->calculatePath(agent, history, start), true);
			world.advanceTicks(120);
			require(agent->getState() == core::Agent::State::Idle && agent->getLocalDepth() == incoming,
				"History preparation did not retain arrival depth");
			world.advanceTicks(10);
		}
		auto expected = reference(start, goal, incoming, agent->getWalkSpeed());
		std::vector<uint32_t> original;
		std::shared_ptr<core::Path> selected;
		for (int repeat = 0; repeat < 20; ++repeat)
		{
			selected = graph->calculatePath(agent, start, goal);
			require(selected && selected->nodes.size() > 1, "Depth route missing");
			Score actual{ selected->nodes.back().cumulativePerceivedCost, 0 };
			int depth = incoming;
			std::vector<uint32_t> digest;
			std::set<core::Vertex const*> visited;
			for (auto const& node : selected->nodes)
			{
				require(visited.insert(node.targetVertex.get()).second, "Selected route contains a cycle");
				digest.push_back(node.targetVertex->getId());
				if (node.edge)
				{
					actual.second += static_cast<uint64_t>(std::abs(int64_t{ depth } - node.edge->getLocalDepth()));
					depth = node.edge->getLocalDepth();
				}
			}
			require(actual == expected, std::string(name) + ": selected Path differs from complete-Path reference");
			require(expectedFirstDepth < 0 || selected->nodes[1].edge->getLocalDepth() == expectedFirstDepth,
				std::string(name) + ": unexpected branch");
			if (repeat == 0) original = digest;
			else require(digest == original, "Repeated equal-cost alternatives are not deterministic");
		}
		auto inferred = graph->calculatePath(agent, goal);
		require(inferred && inferred->nodes.back().cumulativePerceivedCost == expected.first
			&& (expectedFirstDepth < 0 || inferred->nodes[1].edge->getLocalDepth() == expectedFirstDepth),
			std::string(name) + ": stationary inferred source bypassed authored routes");
		// Force the simulation to select its own replacement after topology replay,
		// using retained stationary context, rather than merely execute our query.
		auto id = world.getAgentId(agent);
		agent->setPath(selected, true);
		world.pauseSimulation();
		require(world.rebuildTraversalTopology() && world.resumeSimulation(), "Topology replay failed");
		agent = world.lookupAgent(id).entity;
		world.advanceTicks(agent->getRoutePlanningRemainingTicks());
		require(agent->getPath() && agent->getPath()->nodes.back().cumulativePerceivedCost == expected.first
			&& (expectedFirstDepth < 0 || agent->getPath()->nodes[1].edge->getLocalDepth() == expectedFirstDepth),
			std::string(name) + ": simulation selected a different depth route");
		auto const departureDepth = agent->getPath()->nodes[1].edge->getLocalDepth();
		world.advanceTicks(5);
		require(agent->getLocalDepth() == departureDepth, std::string(name) + ": simulation departed on the wrong depth route");
		world.advanceTicks(600);
		require(agent->getState() == core::Agent::State::Idle && agent->getGlobalPosition() == goal->getPosition(),
			"Simulation failed to finish depth-continuous route");
	}

	void depthContinuity(smoke::Context const& context)
	{
		// A tiny primary improvement still wins; depth is never an epsilon penalty.
		scenario(context, "cheaper", {{ "a", 1 }, { "b", 0.2499f }},
			{{ "start", "a", 100 }, { "a", "goal", 100 }, { "start", "b", 3 }, { "b", "goal", 3 }}, 3, 100);
		scenario(context, "numericalGap", {{ "a", 1 }, { "b", 1 }},
			{{ "start", "a", 0 }, { "a", "goal", 0 }, { "start", "b", 5 }, { "b", "goal", 5 }}, 3, 5);
		scenario(context, "greedyTrap", {{ "a", 1 }, { "b", 1 }},
			{{ "start", "a", 3 }, { "a", "goal", 0 }, { "start", "b", 5 }, { "b", "goal", 5 }}, 3, 5);
		scenario(context, "sharedArrival", {{ "a", 0.5f }, { "b", 0.5f }, { "shared", 1 }},
			{{ "start", "a", 2 }, { "a", "shared", 2 }, { "start", "b", 5 }, { "b", "shared", 5 },
			 { "shared", "goal", 5 }}, 3, 5);
		scenario(context, "noHistory", {{ "a", 1 }, { "b", 1 }},
			{{ "start", "a", 5 }, { "a", "goal", 5 }, { "start", "b", 0 }, { "b", "goal", 0 }}, 0, 0);
		scenario(context, "connectors", {{ "a", 0.25f }, { "b", 0.25f }, { "c", 0.25f }},
			{{ "start", "a", 0 }, { "a", "goal", 5 }, { "start", "b", 5 }, { "b", "goal", 5 },
			 { "a", "c", 2 }, { "c", "b", 5 }}, 3, 5);
		scenario(context, "equalAlternatives", {{ "a", 1 }, { "b", 1 }},
			{{ "start", "a", 5 }, { "a", "goal", 5 }, { "start", "b", 1 }, { "b", "goal", 1 }}, 3, -1);
		scenario(context, "wideGap", {{ "a", 1 }, { "b", 1 }},
			{{ "start", "a", std::numeric_limits<int>::max() }, { "a", "goal", 0 },
			 { "start", "b", 5 }, { "b", "goal", 5 }}, 3, 5);
	}
}

namespace routing_smoke
{
	void registerDepthContinuity(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "furniture/depthContinuity", depthContinuity });
	}
}
