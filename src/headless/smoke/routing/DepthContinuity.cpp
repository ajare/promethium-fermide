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

	Score sectorReference(core::Graph const& graph, core::Agent const& agent,
		std::shared_ptr<const core::Vertex> source, std::shared_ptr<const core::Vertex> target)
	{
		// Exhaustive complete-Path oracle, with production primary-cost inputs but
		// independent enumeration and Sector-local secondary scoring.
		core::PathfindingWorkspace workspace;
		core::RouteDecisionContext decision{ &agent, graph.getRouteChoicePolicy().baselineProfile,
			graph.getRouteChoicePolicy(), agent.getSector(), agent.getWalkSpeed(), graph.getWorld() };
		workspace.captureRouteCosts(graph, decision);
		Score best{ std::numeric_limits<float>::infinity(), 0 };
		std::set<core::Vertex const*> visited;
		std::function<void(std::shared_ptr<const core::Vertex>, int, Score)> visit = [&](auto vertex, int incoming, Score score)
		{
			if (score.first > best.first) return;
			if (vertex == target) { best = std::min(best, score); return; }
			if (vertex != source)
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
					marker && marker->hasProperty(core::MarkerProperty::BlocksPathing)) return;
			visited.insert(vertex.get());
			auto slot = vertex->getSearchIndex();
			for (auto index = workspace.routeOffsets[slot]; index < workspace.routeOffsets[slot + 1]; ++index)
			{
				auto const& arc = workspace.directedArcs[index];
				auto next = graph.getVertices()[arc.targetSlot];
				auto const& cost = workspace.routeCosts[index];
				if (!cost || visited.contains(next.get())) continue;
				auto crossing = vertex->getSector() != next->getSector();
				auto depth = crossing ? 0 : (*arc.edge)->getLocalDepth();
				auto change = crossing ? uint64_t{ 0 } : static_cast<uint64_t>(std::abs(int64_t{ incoming } - depth));
				visit(next, depth, { score.first + cost->perceivedCost, score.second + change });
			}
			visited.erase(vertex.get());
		};
		visit(source, agent.getLocalDepth(), { 0, 0 });
		return best;
	}

	void sectorDepthContinuity(smoke::Context const& context)
	{
		using smoke::require;
		// Real authored topology: a retained-depth seat in the origin, and two
		// equal-length branches in each destination. No synthetic graph mutation.
		for (bool layers : { false, true })
		for (bool cheaper : { false, true })
		{
			auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/desk.furniture.yaml").string());
			auto definition = yaml["furnitureCatalogue"]["definitions"][0];
			definition["usablePoints"] = YAML::Load("[{key: seat, label: Seat, x: 1.75}]");
			definition["vertices"] = YAML::Load("[{key: left, x: 0, external: true}, {key: a, x: 1}, {key: b, x: 1}, {key: seat, x: 1.75, usablePoint: seat}, {key: right, x: 2, external: true}]");
			definition["edges"] = YAML::Load("[{from: left, to: a, depthOffset: 0}, {from: a, to: seat, depthOffset: 0}, {from: left, to: b, depthOffset: 9}, {from: b, to: seat, depthOffset: 9}, {from: left, to: right, depthOffset: 0}]");
			if (cheaper) definition["vertices"][1]["x"] = 1.7501f;
			auto cataloguePath = context.temporaryRoot() / (std::string("sector-") + (layers ? "layers" : "locations") + (cheaper ? "-cheaper" : "") + ".furniture.yaml");
			{ std::ofstream file(cataloguePath); file << yaml; }
			core::World world("Sector reset", 18, 1);
			world.addLayer();
			auto origin = world.addRoom("Origin", 0, 0, 0, 6, 1);
			auto corridor = world.addCorridor(layers ? 1 : 0, 0, layers ? 0 : 6, 6, 1);
			auto facade = world.addFacade("Facade", layers ? 2 : 0, 0, layers ? 0 : 12, 6, 1);
			core::TraversalResourceId remoteDoor;
			if (layers)
			{
				remoteDoor = world.addSectorDoor(0, 0, 1, {}).traversalResource;
				world.addSectorDoor(1, 0, 1, {});
			}
			else
			{
				world.removeLocationWall(origin, 0, 1);
				world.removeLocationWall(corridor, 0, 1);
			}
			world.attachFurnitureCatalogue(cataloguePath.filename().string(), core::FurnitureCatalogue::load(cataloguePath));
			for (auto sector : { origin, corridor, facade }) world.placeFurniture(sector, "desk", 2, 0, "Desk" + std::to_string(sector));
			world.finishBuild();
			auto graph = world.getGraph();
			auto seat = [&](uint32_t sector)
			{
				for (uint32_t i = 0; i < world.getSector(sector)->getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(sector)->getObject(i)))
						return graph->getVertexForObject(object);
				throw std::runtime_error("Missing seat");
			};
			auto agentId = world.createAgent("Walker", origin, 0, 3.0f);
			auto agent = world.lookupAgent(agentId).entity;
			// Select the depth-9 branch explicitly to establish source history.
			for (auto const& edge : seat(origin)->getEdges())
				if (edge->getLocalDepth() == 9)
				{
					auto from = edge->getOtherVertex(seat(origin));
					agent->setPath(graph->calculatePath(agent, from, seat(origin)), true);
					break;
				}
			world.advanceTicks(120);
			require(agent->getState() == core::Agent::State::Idle && agent->getLocalDepth() == 9, "Source history was not prepared");
			for (auto destination : { facade, origin, corridor, facade })
			{
				auto expected = sectorReference(*graph, *agent, seat(agent->getSector()->getIndex()), seat(destination));
				std::vector<uint32_t> digest;
				std::shared_ptr<core::Path> path;
				for (int repeat = 0; repeat < 10; ++repeat)
				{
					path = graph->calculatePath(agent, seat(agent->getSector()->getIndex()), seat(destination));
					require(path != nullptr, "Multi-Sector Path missing: " + cataloguePath.filename().string() + " to " + std::to_string(destination));
					Score actual{ path->nodes.back().cumulativePerceivedCost, 0 };
					int depth = agent->getLocalDepth();
					for (size_t i = 1; i < path->nodes.size(); ++i)
					{
						auto const& node = path->nodes[i];
						if (node.targetVertex->getSector() != path->nodes[i - 1].targetVertex->getSector()) depth = 0;
						else
						{
							actual.second += static_cast<uint64_t>(std::abs(int64_t{ depth } - node.edge->getLocalDepth()));
							depth = node.edge->getLocalDepth();
						}
					}
					require(actual == expected, "Multi-Sector route differs from exhaustive reset-baseline oracle");
					std::vector<uint32_t> next;
					for (auto const& node : path->nodes) next.push_back(node.targetVertex->getId());
					if (repeat == 0) digest = next;
					else require(next == digest, "Multi-Sector selection is nondeterministic");
					require(path->nodes.back().edge->getLocalDepth() == (cheaper ? 9 : 0), "Sector entry did not prefer baseline zero or primary cost");
				}
				auto inferred = graph->calculatePath(agent, seat(destination));
				require(inferred && inferred->nodes.back().cumulativePerceivedCost == expected.first
					&& inferred->nodes.back().edge->getLocalDepth() == (cheaper ? 9 : 0), "Inferred departure disagrees with complete-Path selection");
				agent->setPath(path, true);
				auto previous = agent->getSector();
				unsigned crossings = 0;
				unsigned thresholdTicks = 0;
				for (unsigned tick = 0; tick < 3000 && agent->getState() != core::Agent::State::Idle; ++tick)
				{
					auto request = world.lookupTraversalRequest(agent->getTraversalRequestId());
					if (request && request.entity->getEdgeType() == core::EdgeType::Door && agent->getState() == core::Agent::State::TraversingEdge) ++thresholdTicks;
					world.advanceTick();
					if (agent->getSector() != previous)
					{
						++crossings;
						require(agent->getLocalDepth() == 0, "Runtime Sector entry retained foreign depth");
						require(!layers || agent->getSector()->getLayerIndex() == previous->getLayerIndex() + 1 || previous->getLayerIndex() == agent->getSector()->getLayerIndex() + 1, "Threshold skipped a Layer");
						previous = agent->getSector();
					}
				}
				require(crossings > 0 && (!layers || thresholdTicks >= 6 * crossings), "Boundary timing changed");
				require(agent->getState() == core::Agent::State::Idle && agent->getGlobalPosition() == seat(destination)->getPosition()
					&& agent->getLocalDepth() == (cheaper ? 9 : 0), "Stationary destination context disagrees with selection");
			}
			world.pauseSimulation();
			auto permission = world.addAccessPermission("Restricted");
			require(world.setLocationPermissionRequirement(corridor, { permission }), "Could not protect Corridor");
			require(!graph->calculatePath(agent, seat(origin)), "Depth preference bypassed Corridor access refusal");
			require(!world.setLocationPermissionRequirement(facade, { permission }), "Facade acquired Location requirement");
			require(world.grantAgentAccessPermission(agentId, permission), "Could not grant access");
			require(world.setLocationPermissionRequirement(origin, { permission }), "Could not protect Room");
			require(graph->calculatePath(agent, seat(origin)) != nullptr, "Authorized Room route refused");
			require(world.revokeAgentAccessPermission(agentId, permission), "Could not revoke access");
			require(!graph->calculatePath(agent, seat(origin)), "Depth route entered unauthorized Room");
			require(world.grantAgentAccessPermission(agentId, permission), "Could not restore access");
			if (layers)
			{
				auto before = graph->calculatePath(agent, seat(origin));
				require(world.setDoorBroken(remoteDoor, true), "Could not break remote Door");
				auto after = graph->calculatePath(agent, seat(origin));
				require(before && after && before->nodes.back().cumulativePerceivedCost == after->nodes.back().cumulativePerceivedCost,
					"Depth selection revealed remote live Door condition");
				require(world.setDoorBroken(remoteDoor, false), "Could not restore remote Door");
				auto controlPermission = world.addAccessPermission("Control");
				require(world.setManualDoorPermissionRequirement(remoteDoor, { controlPermission }), "Could not protect Door control");
				require(!graph->calculatePath(agent, seat(origin)), "Depth route bypassed control-operation permission");
				require(world.grantAgentAccessPermission(agentId, controlPermission), "Could not grant control permission");
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
				require(world.setAgentIndividualMobilityProfile(agentId, mobility), "Could not set Mobility");
				require(!graph->calculatePath(agent, seat(origin)), "Depth route used forbidden Door");
				mobility.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
				require(world.setAgentIndividualMobilityProfile(agentId, mobility), "Could not set Mobility");
				auto fallback = graph->calculatePath(agent, seat(origin));
				require(fallback && fallback->diagnosticContext->allowFallbackMobility, "Last-resort pass lost its meaning");
			}
			if (!cheaper)
			{
				// A valid equal-primary Path with a worse secondary branch must not
				// be voluntarily replaced just to improve depth continuity.
				auto retained = graph->calculatePath(agent, seat(origin));
				for (auto const& edge : seat(origin)->getEdges())
					if (edge->getLocalDepth() == 9)
					{
						auto branch = edge->getOtherVertex(seat(origin));
						auto& approach = retained->nodes[retained->nodes.size() - 2];
						auto left = approach.edge->getOtherVertex(approach.targetVertex);
						approach.targetVertex = branch;
						for (auto const& incoming : branch->getEdges())
							if (incoming->getOtherVertex(branch) == left) approach.edge = incoming;
						retained->nodes.back().edge = edge;
					}
				agent->setPath(retained, true);
				require(world.resumeSimulation(), "Could not resume persistence fixture");
				world.beginVoluntaryRoutePlanning(agentId);
				world.advanceTicks(agent->getRoutePlanningRemainingTicks());
				require(agent->getPath() == retained && retained->nodes.back().edge->getLocalDepth() == 9,
					"Depth preference overrode Route persistence for an equal-primary valid Path");
			}
		}
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
		checks.push_back({ "furniture/sectorDepthContinuity", sectorDepthContinuity });
	}
}
