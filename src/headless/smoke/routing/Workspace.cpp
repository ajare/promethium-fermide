#include "core/Human.h"
#include "Checks.h"
#include "RoutingPopulation.h"
#include <bit>
#include "core/Defines.h"
#include <filesystem>
#include <limits>
#include "core/AgentTagRegistryDocument.h"
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Location.h"
#include "core/Marker.h"
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/Vertex.h"
#include "core/World.h"


namespace core
{
	// Test-only access to original Sector enumeration and indexed query output.
	// The oracle below reads edges/cells, never the index's runs or intervals.
	struct GraphSourceIndexTestAccess
	{
		static void checkOverlappingIntervals(Graph const& graph)
		{
			// Balanced subtree maxima for nested, crossing and coincident intervals.
			std::vector<Graph::FloorInterval> intervals{
				{ 0, 0, 0, 20, 20, 0 }, { 0, 0, 1, 2, 20, 1 },
				{ 0, 0, 2, 12, 12, 2 }, { 0, 0, 3, 4, 20, 3 },
				{ 0, 0, 4, 15, 15, 4 }, { 0, 0, 4, 15, 15, 5 },
				{ 0, 0, 6, 9, 9, 6 } };
			for (float x : { -1.0f, 0.0f, 2.0f, 4.0f, 5.5f, 6.0f, 9.0f, 15.0f, 20.0f })
			{
				graph.mContainingIntervals.clear();
				graph.collectFloorIntervals(intervals, 0, intervals.size(), x);
				std::vector<Graph::FloorInterval const*> expected;
				for (auto const& interval : intervals)
					if (interval.left < x && x < interval.right) expected.push_back(&interval);
				if (graph.mContainingIntervals != expected)
					throw std::runtime_error("Interval tree omitted nested/coincident overlap or included boundary");
			}
			graph.mContainingIntervals.clear();
		}
		static auto const& candidates(Graph const& graph, Sector const* sector)
		{ return graph.mSectorVertexLookup.at(sector); }
		static void checkIntervals(Graph const& graph, Sector const* sector, Vector2 position)
		{
			std::vector<std::pair<uint32_t, uint32_t>> expected, actual;
			for (auto const& vertex : graph.getVertices())
				for (auto const& edge : vertex->getEdges())
				{
					auto target = edge->getOtherVertex(vertex);
					if (vertex->getSector().get() != sector || target->getSector().get() != sector
						|| edge->getType() != EdgeType::Location || vertex->getSearchIndex() >= target->getSearchIndex()) continue;
					auto a = vertex->getPosition(), b = target->getPosition();
					if (std::abs(a.y - position.y) <= 0.001f && std::abs(b.y - position.y) <= 0.001f
						&& position.x > std::min(a.x, b.x) && position.x < std::max(a.x, b.x))
						expected.emplace_back(vertex->getSearchIndex(), target->getSearchIndex());
				}
			auto const capacity = graph.mContainingIntervals.capacity();
			graph.findContainingFloorIntervals(sector, position);
			if (graph.mContainingIntervals.capacity() != capacity)
				throw std::runtime_error("Containing-floor query grew scratch");
			for (auto interval : graph.mContainingIntervals)
			{
				if (std::abs(graph.getVertices()[interval->targetSlot]->getPosition().y - position.y) <= 0.001f)
					actual.emplace_back(interval->sourceSlot, interval->targetSlot);
			}
			if (actual != expected) throw std::runtime_error("Indexed floor seeds differ from brute-force ordered endpoints");
		}
	};
}

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	using PathDigest = std::vector<std::pair<uint32_t, uint32_t>>;

	PathDigest digest(std::shared_ptr<core::Path> const& path)
	{
		PathDigest result;
		for (auto const& node : path->nodes)
			result.emplace_back(node.targetVertex->getId(), std::bit_cast<uint32_t>(node.cumulativePerceivedCost));
		return result;
	}

	void costContract()
	{
		core::RouteChoicePolicy policy;
		core::DirectedTraversalFacts facts;
		require(!policy.evaluate(facts, {}), "Excluded traversal acquired a finite cost");
		facts.feasible = true;
		facts.components.motionSeconds = 2.0f;
		facts.components.physicalEffortUnits = 2000000.0f;
		facts.objectiveDurationSeconds = 2.0f;
		auto cost = policy.evaluate(facts, {});
		require(cost && cost->perceivedCost == 2000002.0f && cost->objectiveDurationSeconds == 2.0f,
			"Finite dislike became exclusion or changed objective duration");
		core::PathfindingWorkspace workspace;
		workspace.beginSearch(4);
		workspace.put(3, 1);
		workspace.put(1, 1);
		workspace.put(2, 1);
		require(workspace.get() == 1 && workspace.get() == 2 && workspace.get() == 3,
			"Equal-cost frontier ties are not ordered by graph slot");
		for (float invalid : { -1.0f, std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN() })
		{
			facts.components.riskUnits = invalid;
			bool rejected = false;
			try { (void)policy.evaluate(facts, {}); }
			catch (std::invalid_argument const&) { rejected = true; }
			require(rejected, "Invalid component entered routing");
		}
	}

	void bundledRoutesMatchReference(smoke::Context const& smokeContext)
	{
		for (auto const* filename : { "lift-test-1.world.yaml", "shuttle-test-1.world.yaml",
			"stairwell-test-1.world.yaml", "staircase-test-1.world.yaml" })
		{
			auto world = core::loadWorldDocument(smokeContext.fixture(std::filesystem::path("resources/test-worlds") / filename));
			auto graph = world->getGraph();
			core::Human agent("Reference walker");
			core::RouteDecisionContext const context{ &agent, {}, {}, nullptr,
				agent.getWalkSpeed(), world.get() };
			auto const& vertices = graph->getVertices();
			for (auto const& source : vertices)
			{
				// Independent O(V^2) Dijkstra, no production heap or workspace.
				std::vector<float> distances(vertices.size(), std::numeric_limits<float>::infinity());
				std::vector<bool> settled(vertices.size(), false);
				distances[source->getSearchIndex()] = 0;
				for (size_t step = 0; step < vertices.size(); ++step)
				{
					size_t best = vertices.size();
					for (size_t i = 0; i < vertices.size(); ++i)
						if (!settled[i] && (best == vertices.size() || distances[i] < distances[best])) best = i;
					if (best == vertices.size() || !std::isfinite(distances[best])) break;
					settled[best] = true;
					for (auto const& edge : vertices[best]->getEdges())
					{
						auto next = edge->getOtherVertex(vertices[best]);
						auto cost = context.policy.evaluate(edge->getDirectedTraversalFacts(next, context), context.profile);
						if (cost) distances[next->getSearchIndex()] = std::min(distances[next->getSearchIndex()],
							distances[best] + cost->perceivedCost);
					}
				}
				for (auto const& target : vertices)
				{
					auto path = graph->calculatePath(&agent, source, target);
					auto expected = distances[target->getSearchIndex()];
					require(bool(path) == std::isfinite(expected), "Reference reachability mismatch");
					if (path) require(std::abs(path->nodes.back().cumulativePerceivedCost - expected) < 0.001f,
						"Bundled Path does not minimise perceived directed costs (#191)");
				}
			}
		}
	}

	void uncertainRoutesSurviveWorldReset()
	{
		core::World world("Uncertain reset", 8, 3);
		auto lower = world.addCorridor(0, 0, 8);
		world.addCorridor(1, 0, 8);
		auto upper = world.addCorridor(2, 0, 8);
		world.addLadder(1, 0, 4, { 3, true, true });
		uint32_t destination, origin;
		world.addSectorMarker(lower, 0, 1.5f, &destination);
		world.addSectorMarker(upper, 0, 6.5f, &origin);
		world.finishBuild();
		world.pauseSimulation();
		auto id = world.createAgent("Uncertain walker", lower, 0, 1.5f);
		require(world.setAgentIndividualRouteFamiliarity(id, 0.0f), "Could not set uncertain profile");
		auto target = world.getGraph()->getVertexByIdentifier(destination);
		auto markerId = std::dynamic_pointer_cast<core::Marker>(target->getObject())->getMarkerId();
		auto source = world.getGraph()->getVertexByIdentifier(origin);
		auto sourceMarkerId = std::dynamic_pointer_cast<core::Marker>(source->getObject())->getMarkerId();
		// Evaluate a hypothetical return from the upper Stop. The Agent remains
		// below and cannot observe the Ladder from that remote approach.
		auto before = world.getGraph()->calculatePath(world.lookupAgent(id).entity, source, target);
		require(before != nullptr, "Uncertain reset fixture has no Path");
		bool uncertain = false;
		PathDigest expected;
		for (auto const& node : before->nodes)
		{
			expected.emplace_back(node.targetVertex->getSearchIndex(), std::bit_cast<uint32_t>(node.cumulativePerceivedCost));
			uncertain = uncertain || (node.diagnosticCost && node.diagnosticCost->components.uncertainty > 0
				&& node.diagnosticCost->components.stableVariation != 0);
		}
		require(uncertain, "Reset fixture did not exercise uncertain perceived route costs");
		world.resetSimulation();
		target.reset();
		source.reset();
		for (auto const& vertex : world.getGraph()->getVertices())
		{
			auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
			if (marker && marker->getMarkerId() == markerId) target = vertex;
			if (marker && marker->getMarkerId() == sourceMarkerId) source = vertex;
		}
		require(target && source, "Reset lost an uncertain route Marker");
		auto after = world.getGraph()->calculatePath(world.lookupAgent(id).entity, source, target);
		require(after != nullptr, "Reset lost the uncertain Path");
		PathDigest actual;
		for (auto const& node : after->nodes)
			actual.emplace_back(node.targetVertex->getSearchIndex(), std::bit_cast<uint32_t>(node.cumulativePerceivedCost));
		require(actual == expected, "Process-global Edge IDs changed perceived costs after a World reset");
	}

	void sourceInferenceMatchesOpenIntervalReference(smoke::Context const& smokeContext)
	{
		for (auto name : { "forcebridge-test-1.world.yaml", "staircase-test-1.world.yaml", "platformlift-test-1.world.yaml" })
		{
			auto world = core::loadWorldDocument(smokeContext.fixture(std::filesystem::path("resources/test-worlds") / name));
			auto graph = world->getGraph();
			for (auto const& origin : graph->getVertices())
			{
				auto sector = origin->getSector();
				if (!dynamic_cast<core::Location const*>(sector.get())) continue;
				for (float offset : { -1.0f, -0.5f, -0.001f, 0.0f, 0.001f, 0.25f, 0.5f, 1.0f })
				for (float heightOffset : { -0.0011f, -0.001f, 0.0f, 0.001f, 0.0011f })
				{
					auto position = origin->getPosition();
					position.x += offset;
					position.y += heightOffset;
					if (position.x < 0 || position.x > world->getCellsWide()
						|| position.y < 0 || position.y >= world->getLevelsHigh()) continue;
					auto actual = graph->getPathSourceVertex(sector.get(), position);
					core::GraphSourceIndexTestAccess::checkIntervals(*graph, sector.get(), position);
					std::shared_ptr<const core::Vertex> expected;
					auto best = std::numeric_limits<float>::infinity();
					for (auto const& candidate : core::GraphSourceIndexTestAccess::candidates(*graph, sector.get()))
					{
						if (candidate->getSector() != sector) continue;
						auto target = candidate->getPosition();
						if (std::abs(target.y - position.y) > 0.001f) continue;
						auto distance = position.distanceToSq(target);
						if (distance >= best) continue;
						bool reachable = true;
						for (int x = static_cast<int>(std::floor(std::min(position.x, target.x)));
							x < static_cast<int>(std::ceil(std::max(position.x, target.x))); ++x)
						{
							if (x < 0 || x >= static_cast<int>(world->getCellsWide())) { reachable = false; break; }
							auto const& cell = std::as_const(*world).getLayer(sector->getLayerIndex())
								->getCellDefinition(x, static_cast<uint32_t>(std::round(position.y)));
							if (cell.sectorIndex != sector->getIndex() || cell.floorType == core::CellFloorType::None
								|| cell.floorType == core::CellFloorType::ForceBridge) { reachable = false; break; }
						}
						if (reachable)
						{
							expected = candidate;
							best = distance;
						}
					}
					// Exact identity matters at coincident thresholds, not just distance.
					if (actual != expected)
						throw std::runtime_error(std::string("Floor source mismatch in ") + name
							+ " at " + std::to_string(position.x) + "," + std::to_string(position.y)
							+ ": expected " + (expected ? expected->getDescription() : "none")
							+ ", actual " + (actual ? actual->getDescription() : "none"));
				}
			}
		}
	}

	void sourceIndexesFollowWalkwayEdits()
	{
		core::World world("Source topology edits", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 2);
		uint32_t removed = 0;
		for (uint32_t x = 0; x < 8; ++x)
		{
			auto walkway = world.addSectorWalkway(room, 1, x);
			if (x == 3) removed = walkway.index;
		}
		world.addSectorMarker(room, 1, 2.5f);
		world.addSectorMarker(room, 1, 5.5f);
		world.finishBuild();
		world.pauseSimulation();
		auto sector = world.getSector(room);
		core::Vector2 const position{ 3.5f, 1.0f };
		require(world.getGraph()->getPathSourceVertex(sector.get(), position) != nullptr,
			"Supported Walkway has no source");
		require(world.removeSectorWalkway(room, removed) && world.rebuildTraversalTopology(),
			"Could not rebuild edited Walkway topology");
		sector = world.getSector(room);
		require(!world.getGraph()->getPathSourceVertex(sector.get(), position),
			"Source index retained removed Floor support");
		core::GraphSourceIndexTestAccess::checkIntervals(*world.getGraph(), sector.get(), position);
		world.addSectorWalkway(room, 1, 3);
		require(world.rebuildTraversalTopology(), "Could not restore Walkway topology");
		sector = world.getSector(room);
		require(world.getGraph()->getPathSourceVertex(sector.get(), position) != nullptr,
			"Source index missed restored Floor support");
		core::GraphSourceIndexTestAccess::checkIntervals(*world.getGraph(), sector.get(), position);
	}

	void sourceIndexWorkIsLocal()
	{
		for (uint32_t count : { 32u, 128u, 512u })
		{
			core::World world("Source index scale", count + 8, 2);
			auto sector = world.addRoom("Source", 0, 0, 0, count + 8, 2);
			auto back = world.addRoom("Other Layer", 1, 0, 0, count + 8, 2);
			uint32_t nearId, farId;
			world.addSectorMarker(sector, 0, 1.5f, &nearId);
			world.addSectorMarker(sector, 0, 4.5f, &farId);
			for (uint32_t x = 0; x < count; ++x)
			{
				world.addSectorMarker(sector, 0, x + 6.5f, "Front " + std::to_string(x));
				world.addSectorMarker(back, 0, x + 0.5f, "Back " + std::to_string(x));
			}
			world.finishBuild();
			world.pauseSimulation();
			auto id = world.createAgent("Source walker", sector, 0, 2.5f);
			auto agent = world.lookupAgent(id).entity;
			auto graph = world.getGraph();
			auto target = graph->getVertexByIdentifier(farId);
			auto first = graph->calculatePath(agent, target);
			require(first && first->nodes.front().targetVertex == target,
				"Virtual source forced nearest endpoint instead of optimal endpoint");
			auto allocations = graph->getScratchAllocationCount();
			auto backSource = graph->getPathSourceVertex(world.getSector(back).get(), { 2.5f, 0.0f });
			require(backSource && backSource->getSector()->getLayerIndex() == 1,
				"Coincident positions on different Layers shared a source");
			auto before = graph->getSourceIndexStatistics();
			for (int repeat = 0; repeat < 10; ++repeat)
			{
				require(graph->getPathSourceVertex(agent->getSector(), { 3.0f, 0.0f })
					== graph->getVertexByIdentifier(nearId), "Source tie changed original candidate order");
				require(digest(graph->calculatePath(agent, target)) == digest(first), "Indexed repeated Path changed");
			}
			auto after = graph->getSourceIndexStatistics();
			require(after.candidatesExamined - before.candidatesExamined <= 40,
				"Unrelated Markers increased source candidate work");
			require(after.intervalsExamined - before.intervalsExamined
				<= 40u * static_cast<uint64_t>(std::bit_width(count + 4)),
				"Unrelated segments caused linear interval work");
			require(after.containingIntervals - before.containingIntervals == 10,
				"Source seeding omitted or duplicated containing intervals");
			require(after.builds == before.builds && graph->getScratchAllocationCount() == allocations,
				"Warm queries rebuilt source topology or grew scratch");
			core::GraphSourceIndexTestAccess::checkIntervals(*graph, agent->getSector(), agent->getGlobalPosition());
			core::GraphSourceIndexTestAccess::checkOverlappingIntervals(*graph);
		}
	}

	// Independent eager facts/policy oracle. Includes stable variation, but never
	// calls RouteTraversalInputs or reads the demand cache.
	std::vector<std::optional<core::EvaluatedRouteCost>> eagerCosts(core::Graph const& graph,
		core::RouteDecisionContext const& context)
	{
		std::vector<std::optional<core::EvaluatedRouteCost>> result;
		auto mix = [](uint64_t value)
		{
			value += 0x9e3779b97f4a7c15ULL;
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31);
		};
		for (auto const& vertex : graph.getVertices()) for (auto const& edge : vertex->getEdges())
		{
			auto target = edge->getOtherVertex(vertex);
			auto facts = edge->getDirectedTraversalFacts(target, context);
			if (facts.feasible && facts.components.uncertaintyUnits > 0)
			{
				auto const hash = mix(context.perceptionKey ^ (uint64_t{edge->getRoutingIndex()} << 32)
					^ target->getSearchIndex() ^ mix(context.observationEpoch));
				auto const unit = static_cast<float>(hash >> 40) / 16777215.0f;
				facts.components.perceptionVariationUnits = (unit * 2.0f - 1.0f)
					* ((1.0f - context.profile.routeFamiliarity) * facts.components.uncertaintyUnits);
			}
			result.push_back(context.policy.evaluate(facts, context.profile));
		}
		return result;
	}

	void checkDemandCosts(core::PathfindingWorkspace& workspace,
		std::vector<std::optional<core::EvaluatedRouteCost>> const& expected)
	{
		auto const allocations = workspace.getScratchAllocationCount();
		auto const evaluated = workspace.work.evaluatedArcs;
		for (size_t i = 0; i < expected.size(); ++i)
		{
			auto const& actual = workspace.evaluateArc(i);
			require(bool(actual) == bool(expected[i]), "Demand scorer changed hard feasibility");
			if (!actual) continue;
			require(actual->perceivedCost == expected[i]->perceivedCost
				&& actual->objectiveDurationSeconds == expected[i]->objectiveDurationSeconds,
				"Demand scorer differs from independent eager cost/duration");
			auto const& a = actual->components;
			auto const& b = expected[i]->components;
			require(a.movement == b.movement && a.knownWait == b.knownWait && a.expectedWait == b.expectedWait
				&& a.effort == b.effort && a.interaction == b.interaction && a.crowding == b.crowding
				&& a.risk == b.risk && a.uncertainty == b.uncertainty && a.stableVariation == b.stableVariation,
				"Demand diagnostics differ from independent eager components");
		}
		require(workspace.work.evaluatedArcs - evaluated == expected.size(), "Demand cache did not start a fresh decision/pass");
		for (size_t i = 0; i < expected.size(); ++i) workspace.evaluateArc(i);
		require(workspace.work.evaluatedArcs - evaluated == expected.size()
			&& workspace.getScratchAllocationCount() == allocations, "Demand cache repeated work or grew scratch");
	}

	void capturedInputsMatchEagerProviders(smoke::Context const& smokeContext)
	{
		for (auto const* filename : { "lift-test-1.world.yaml", "shuttle-test-1.world.yaml",
			"stairwell-test-1.world.yaml", "staircase-test-1.world.yaml", "ladder-test-1.world.yaml",
			"forcebridge-test-1.world.yaml", "platformlift-test-1.world.yaml", "bulkhead-test-1.world.yaml",
			"door-test-1.world.yaml" })
		{
			auto world = core::loadWorldDocument(smokeContext.fixture(std::filesystem::path("resources/test-worlds") / filename));
			auto graph = world->getGraph();
			auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
			if (agent && !agent->getSector()) agent = nullptr;
			core::PathfindingWorkspace workspace;
			for (float extreme : { 0.0f, 1.0f }) for (bool fallback : { false, true })
			for (uint32_t observedSector = 0; observedSector <= world->getNumSectors(); ++observedSector)
			{
				core::EffectiveRoutingProfile profile;
				profile.walkSpeedModifier = extreme ? core::AgentWalkSpeedModifierMaximum : core::AgentWalkSpeedModifierMinimum;
				profile.stairSpeedModifier = profile.ladderSpeedModifier = extreme ? 1.5f : 0.5f;
				profile.escalatorWalkingChance = profile.routeFamiliarity = extreme;
				profile.effortAversion = profile.interactionAversion = profile.riskAversion = profile.crowdAversion = extreme * 3;
				profile.waitingAversion = extreme ? 3 : 0.5f;
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
				auto const& physical = core::Agent::physicalBaselineForType("Human");
				core::RouteDecisionContext context{ agent, profile, {},
					observedSector < world->getNumSectors() ? world->getSector(observedSector).get() : nullptr,
					physical.walkSpeed * profile.walkSpeedModifier, world.get(),
					physical.climbSpeed * profile.ladderSpeedModifier, fallback, 12345, 7, mobility };
				workspace.beginRouteDecision(*graph, context);
				checkDemandCosts(workspace, eagerCosts(*graph, context));
				if (!fallback)
				{
					workspace.allowFallbackMobility();
					checkDemandCosts(workspace, eagerCosts(*graph, workspace.decisionContext()));
				}
			}
		}
	}

	void localDemandWorkIsBounded()
	{
		core::World world("Demand-local", 1024, 1);
		auto sector = world.addCorridor(0, 0, 1024);
		std::vector<uint32_t> markers(1000);
		for (uint32_t i = 0; i < markers.size(); ++i) world.addSectorMarker(sector, 0, i + 0.5f, &markers[i]);
		world.finishBuild();
		auto graph = world.getGraph();
		auto agent = world.lookupAgent(world.createAgent("Local", sector, 0, 500.5f)).entity;
		auto source = graph->getVertexByIdentifier(markers[500]);
		auto target = graph->getVertexByIdentifier(markers[501]);
		auto before = graph->getRouteWorkCounts();
		auto path = graph->calculatePath(agent, source, target);
		auto after = graph->getRouteWorkCounts();
		require(path && path->nodes.back().cumulativePerceivedCost == 1.0f / agent->getWalkSpeed(),
			"Local demand route is not minimum physical cost");
		require(after.evaluatedArcs - before.evaluatedArcs < graph->getEdges().size() * 2
			&& after.evaluatedArcs - before.evaluatedArcs < 10 && after.preparedArcs == 0,
			"Local routing hid whole-Graph work in cost or snapshot preparation");
		before = after;
		auto same = graph->calculatePath(agent, source, source);
		require(same && graph->getRouteWorkCounts().evaluatedArcs == before.evaluatedArcs,
			"Source-equals-target evaluated unused arcs");
		before = graph->getRouteWorkCounts();
		auto suffixes = core::pathing::comparePathSuffixCosts(*agent, *graph, *path, 0, *path, 0);
		after = graph->getRouteWorkCounts();
		require(suffixes && suffixes->first == suffixes->second && after.evaluatedArcs - before.evaluatedArcs == 1
			&& after.cacheHits > before.cacheHits, "Suffix comparison did not share one demand context/cache");
		before = after;
		auto explanation = core::pathing::explainRoute(*agent, *graph, *path, 1, 1);
		require(explanation && explanation->analysisTruncated
			&& graph->getRouteWorkCounts().evaluatedArcs - before.evaluatedArcs <= 1,
			"Explanation exceeded its cost-evaluation budget");

		auto const agentId = world.getAgentId(agent);
		for (auto mode : { 0, 1, 2 })
		{
			world.resetSimulation();
			graph = world.getGraph();
			agent = world.lookupAgent(agentId).entity;
			source = graph->getClosestVertexInSector(agent->getSector(), { 500.5f, 0 });
			std::vector<std::shared_ptr<const core::Vertex>> destinations;
			for (uint32_t i = 0; i < 1000; ++i)
				destinations.push_back(graph->getClosestVertexInSector(agent->getSector(), { i + 0.5f, 0 }));
			for (int pass = 0; pass < 2; ++pass)
			{
				before = graph->getRouteWorkCounts();
				auto const allocations = graph->getScratchAllocationCount();
				for (uint32_t i = 0; i < 1000; ++i)
				{
					auto destination = destinations[mode == 0 ? 501 : mode == 1 ? 999 : i];
					require(graph->calculatePath(agent, source, destination) != nullptr, "Demand workload lost route");
				}
				require(!pass || graph->getScratchAllocationCount() == allocations, "Demand workloads grew warmed scratch");
			}
		}
	}

	void doorObservationEpochsIgnoreTicks()
	{
		core::World world("Door route epochs", 12, 1);
		auto fore = world.addRoom("Fore", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto door = world.addSectorDoor(0, 0, 5, {});
		uint32_t destination;
		world.addSectorMarker(back, 0, 8.5f, &destination);
		world.finishBuild();
		auto resource = world.lookupTraversalResource(door.traversalResource).entity;
		auto sector = core::SectorId{ static_cast<uint64_t>(fore) + 1 };
		require(world.observeAccessZoneDensity(door.traversalResource, sector) == 0,
			"Empty Door observation reported crowding");
		auto const emptyEpoch = resource->getDoorRouteObservationEpoch();
		auto observer = world.lookupAgent(world.createAgent("Observer", fore, 0, 0.5f)).entity;
		core::RouteDecisionContext frozen{ observer, {}, {}, observer->getSector(), observer->getWalkSpeed(), &world };
		core::PathfindingWorkspace demand;
		demand.beginRouteDecision(*world.getGraph(), frozen);
		auto emptyCosts = eagerCosts(*world.getGraph(), frozen);
		auto repeatedObservedPaths = [&]()
		{
			auto const graph = world.getGraph();
			for (int i = 0; i < 1000; ++i)
				require(graph->calculatePath(observer, graph->getVertexByIdentifier(destination)) != nullptr,
					"Observed demand route disappeared");
		};
		repeatedObservedPaths();
		repeatedObservedPaths();
		world.advanceTicks(3);
		for (int observer = 0; observer < 20; ++observer)
		{
			(void)world.observeAccessZoneDensity(door.traversalResource, sector);
			(void)world.estimateTraversalDelay(door.traversalResource, sector);
		}
		require(resource->getDoorRouteObservationEpoch() == emptyEpoch,
			"Door observation epoch advanced with ticks or observers");
		for (int index = 0; index < 4; ++index)
		{
			auto agent = world.lookupAgent(world.createAgent("Waiter", fore, 0, 5.5f)).entity;
			agent->setPath(world.getGraph()->calculatePath(agent,
				world.getGraph()->getVertexByIdentifier(destination)), true);
		}
		world.advanceTicks(10);
		require(world.observeAccessZoneDensity(door.traversalResource, sector) > 0
			&& world.estimateTraversalDelay(door.traversalResource, sector) > 0,
			"Changed Door queue did not refresh the compact observation");
		// Evaluate only after live queues changed: the old decision must still
		// reproduce its old evidence, including costs not previously requested.
		checkDemandCosts(demand, emptyCosts);
		demand.beginRouteDecision(*world.getGraph(), frozen);
		auto queuedCosts = eagerCosts(*world.getGraph(), frozen);
		checkDemandCosts(demand, queuedCosts);
		bool changed = false;
		for (size_t i = 0; i < emptyCosts.size(); ++i)
			if (emptyCosts[i] && queuedCosts[i] && emptyCosts[i]->perceivedCost != queuedCosts[i]->perceivedCost) changed = true;
		require(changed, "New decision reused stale local observations");
		repeatedObservedPaths();
		auto const queuedEpoch = resource->getDoorRouteObservationEpoch();
		require(queuedEpoch > emptyEpoch, "Door observation did not publish its changed queue");
		(void)world.observeAccessZoneDensity(door.traversalResource, sector);
		require(resource->getDoorRouteObservationEpoch() == queuedEpoch,
			"Unchanged Door facts rebuilt the shared observation");
		world.pauseSimulation();
		require(world.observeAccessZoneDensity(door.traversalResource, sector) == 0
			&& resource->getDoorRouteObservationEpoch() > queuedEpoch,
			"Door observation retained cancelled queue membership");
	}

	void lowerBoundsAreUniversalAndBounded()
	{
		core::World world("Routing lower bounds", 20, 2);
		auto lower = world.addCorridor(0, 0, 20);
		auto upper = world.addCorridor(1, 0, 20);
		world.addSectorMarker(lower, 0, 1.5f);
		world.addSectorMarker(lower, 0, 18.5f);
		world.addSectorMarker(upper, 0, 1.5f);
		world.addSectorMarker(upper, 0, 18.5f);
		world.addStaircase(1, 0, 8, { 2, CORE_SIDE_RIGHT, 0.0f });
		world.addLadder(1, 0, 2, { 2, false, true });
		world.addLift(1, 0, 14, 2, 2);
		world.finishBuild();
		auto graph = world.getGraph();
		auto const count = graph->getVertices().size();
		auto id = world.createAgent("Bound reference", lower, 0, 1.5f);
		auto agent = world.lookupAgent(id).entity;
		world.pauseSimulation();
		require(world.setAgentIndividualStairSpeedModifier(id, 1.5f)
			&& world.setAgentIndividualLadderSpeedModifier(id, 1.5f)
			&& world.setAgentIndividualInteractionAversion(id, 0)
			&& world.setAgentIndividualEffortAversion(id, 0)
			&& world.setAgentIndividualCrowdAversion(id, 0)
			&& world.setAgentIndividualRiskAversion(id, 0)
			&& world.setAgentIndividualWaitingAversion(id, 0.5f)
			&& world.setAgentIndividualRouteFamiliarity(id, 1), "Could not set extreme routing profile");
		core::PathfindingWorkspace workspace;
		for (float speed : { core::AgentWalkSpeedModifierMinimum, 1.0f,
			core::AgentWalkSpeedModifierMaximum })
		{
			core::EffectiveRoutingProfile profile;
			profile.walkSpeedModifier = speed;
			profile.stairSpeedModifier = 1.5f;
			profile.ladderSpeedModifier = 1.5f;
			profile.interactionAversion = profile.effortAversion = profile.crowdAversion = profile.riskAversion = 0;
			profile.waitingAversion = 0.5f;
			profile.routeFamiliarity = 1;
			require(world.setAgentIndividualWalkSpeedModifier(id, speed), "Could not set reference Walk speed");
			auto const& physical = agent->getPhysicalBaseline();
			core::RouteDecisionContext context{ agent, profile, {}, agent->getSector(),
				physical.walkSpeed * speed, &world, physical.climbSpeed * 1.5f };
			workspace.captureRouteCosts(*graph, context);
			for (uint32_t source = 0; source < count; ++source)
			{
				// Independent float Dijkstra: deliberately retain the production
				// accumulation precision, including zero-cost topology connectors.
				std::vector<float> distances(count, std::numeric_limits<float>::infinity());
				std::vector<bool> settled(count, false);
				distances[source] = 0;
				for (size_t step = 0; step < count; ++step)
				{
					size_t best = count;
					for (size_t i = 0; i < count; ++i)
						if (!settled[i] && (best == count || distances[i] < distances[best])) best = i;
					if (best == count || !std::isfinite(distances[best])) break;
					settled[best] = true;
					for (auto arc = workspace.routeOffsets[best]; arc < workspace.routeOffsets[best + 1]; ++arc)
					{
						auto const& directed = workspace.directedArcs[arc];
						auto target = directed.targetSlot;
						// Independent eager provider, not the demand scorer/cache.
						auto const cost = context.policy.evaluate((*directed.edge)->getDirectedTraversalFacts(
							graph->getVertices()[target], context), profile);
						if (!cost) continue;
						distances[target] = std::min(distances[target], distances[best] + cost->perceivedCost);
					}
				}
				for (uint32_t target = 0; target < count; ++target)
				{
					auto const& bounds = workspace.prepareTargetLowerBounds(target);
					require(bounds[source] <= distances[target], "Shared lower bound overestimated a valid profile");
					auto path = graph->calculatePath(agent, graph->getVertices()[source], graph->getVertices()[target]);
					require(bool(path) == std::isfinite(distances[target])
						&& (!path || path->nodes.back().cumulativePerceivedCost == distances[target]),
						"Bounded search disagreed with exact float reference Dijkstra at a profile extreme");
				}
			}
		}
		auto const builds = workspace.getLowerBoundBuildCount();
		auto const allocations = workspace.getScratchAllocationCount();
		workspace.prepareTargetLowerBounds(static_cast<uint32_t>(count - 1));
		require(workspace.getLowerBoundBuildCount() == builds && workspace.getLowerBoundHitCount() > 0,
			"Repeated target did not reuse its lower bounds");
		for (uint32_t target = 0; target < 5; ++target) workspace.prepareTargetLowerBounds(target);
		auto const evicted = workspace.getLowerBoundBuildCount();
		workspace.prepareTargetLowerBounds(0);
		require(workspace.getLowerBoundBuildCount() == evicted + 1, "Target LRU did not evict its oldest table");
		workspace.invalidateTopology();
		workspace.captureRouteCosts(*graph, { nullptr, {}, {} });
		workspace.prepareTargetLowerBounds(0);
		require(workspace.getLowerBoundBuildCount() == evicted + 2
			&& workspace.getScratchAllocationCount() == allocations,
			"Topology invalidation retained stale bounds or evictions grew warmed scratch");
	}


	void reusedWorkspaceIsStableAndDoesNotGrow()
	{
		core::World world("Path workspace", 40, 1);
		auto const connected = world.addCorridor(0, 0, 15);
		world.addRoom("Separator", 0, 0, 15, 2, 1);
		auto const isolated = world.addCorridor(0, 17, 12);
		uint32_t sourceId = 0;
		uint32_t destinationId = 0;
		uint32_t unreachableId = 0;
		world.addSectorMarker(connected, 0, 1.5f, &sourceId);
		world.addSectorMarker(connected, 0, 13.5f, &destinationId);
		world.addSectorMarker(isolated, 0, 4.5f, &unreachableId);
		world.finishBuild();

		auto const graph = world.getGraph();
		auto const source = graph->getVertexByIdentifier(sourceId);
		auto const destination = graph->getVertexByIdentifier(destinationId);
		auto const unreachable = graph->getVertexByIdentifier(unreachableId);
		auto const agent = world.lookupAgent(
			world.createAgent("Workspace walker", connected, 0, 1.5f)).entity;

		for (size_t slot = 0; slot < graph->getVertices().size(); ++slot)
		{
			auto const& vertex = graph->getVertices()[slot];
			require(vertex->getSearchIndex() == static_cast<uint32_t>(slot),
				"Graph Vertex does not have its deterministic dense search slot");
		}
		for (auto const& edge : graph->getEdges())
			for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
			{
				auto const vertex = edge->getVertex(endpoint);
				require(vertex->getSearchIndex() < static_cast<uint32_t>(graph->getVertices().size())
					&& graph->getVertices()[vertex->getSearchIndex()].get() == vertex.get(),
					"An Edge endpoint is absent from the Graph's dense Vertex order");
			}

		auto const first = graph->calculatePath(agent, source, destination);
		require(first && first->nodes.size() >= 2, "The workspace fixture has no connected Path");
		float cumulativeCost = 0.0f;
		float cumulativeDuration = 0.0f;
		for (size_t i = 1; i < first->nodes.size(); ++i)
		{
			auto const& node = first->nodes[i];
			require(node.edge != nullptr, "A non-source Path node has no Edge");
			require(node.diagnosticCost && node.diagnosticCost->objectiveDurationSeconds,
				"Walking Path node omitted captured route facts");
			cumulativeCost += node.diagnosticCost->perceivedCost;
			cumulativeDuration += *node.diagnosticCost->objectiveDurationSeconds;
			require(std::abs(cumulativeCost - node.cumulativePerceivedCost) < 0.0001f,
				"Path cumulative perceived cost differs from captured route costs");
		}
		require(first->nodes.back().objectiveDurationSeconds
			&& std::abs(*first->nodes.back().objectiveDurationSeconds - cumulativeDuration) < 0.0001f,
			"Walking Path did not report physical objective duration");
		auto const diagnostic = core::pathing::getRouteDiagnostics(*first);
		require(diagnostic
			&& std::abs(diagnostic->components.total() - diagnostic->perceivedCost) < 0.0001f
			&& diagnostic->objectiveEstimatedDurationSeconds
			&& std::abs(*diagnostic->objectiveEstimatedDurationSeconds - cumulativeDuration) < 0.0001f,
			"On-demand Path diagnostics did not preserve cost components and objective duration");
		require(diagnostic->context.provenance.effortAversion.source
				== core::RoutingPropertySource::Default
			&& !core::pathing::routeDiagnosticContextIsStale(*agent, *graph, *first),
			"Fresh Path diagnostics did not identify default property provenance");
		auto const explanation = core::pathing::explainRoute(*agent, *graph, *first);
		auto const repeatedExplanation = core::pathing::explainRoute(*agent, *graph, *first);
		require(explanation && repeatedExplanation
			&& explanation->vertices.size() == first->nodes.size()
			&& explanation->vertices.back().target
			&& !explanation->capturedContextStale
			&& explanation->comparisonEvidence == core::RouteExplanationEvidence::CurrentContext,
			"On-demand Route explanation omitted Path Vertices or current-context evidence");
		require(explanation->vertices.front().continuations.size()
			== repeatedExplanation->vertices.front().continuations.size()
			&& explanation->vertices.front().continuations.front().edge->getId()
				== repeatedExplanation->vertices.front().continuations.front().edge->getId(),
			"The same decision context produced non-deterministic Route explanation ordering");
		auto const bounded = core::pathing::explainRoute(*agent, *graph, *first, 1, 1);
		require(bounded && bounded->analysisTruncated,
			"Route alternative analysis ignored its explicit work bounds");
		agent->setPath(first, true);
		world.pauseSimulation();
		core::World::TopologyPathIntent pausedIntent;
		require(world.getPausedPathIntent(*agent, pausedIntent)
			&& pausedIntent.routeDiagnostics
			&& std::abs(pausedIntent.routeDiagnostics->perceivedCost
				- diagnostic->perceivedCost) < 0.0001f,
			"Pausing discarded the selected Path's captured diagnostics");
		require(world.setAgentIndividualEffortAversion(world.getAgentId(agent), 2.0f),
			"Could not change a diagnostic routing property");
		require(core::pathing::routeDiagnosticContextIsStale(*agent, *graph, *first),
			"A property change did not mark captured Path diagnostics stale");
		auto const currentExplanation = core::pathing::explainRoute(*agent, *graph, *first);
		require(currentExplanation && currentExplanation->capturedContextStale
			&& currentExplanation->comparisonEvidence == core::RouteExplanationEvidence::CurrentContext,
			"Changed evidence was presented as the historical Route decision context");
		auto const individualPath = graph->calculatePath(agent, source, destination);
		auto const individualDiagnostic = individualPath
			? core::pathing::getRouteDiagnostics(*individualPath) : std::nullopt;
		require(individualDiagnostic
			&& individualDiagnostic->context.provenance.effortAversion.source
				== core::RoutingPropertySource::Individual,
			"Path diagnostics did not identify individual property provenance");
		core::Path restoredLikePath;
		require(!core::pathing::getRouteDiagnostics(restoredLikePath),
			"A Path without captured historical context fabricated diagnostics");
		auto const preview = graph->calculatePath(nullptr, source, destination);
		require(preview && digest(preview) == digest(first), "Null-Agent baseline preview changed routing");
		require(!graph->calculatePath(nullptr, destination), "Null-Agent inferred source was accepted");
		auto const expectedDigest = digest(first);
		auto const allocationsAfterWarmup = graph->getScratchAllocationCount();
		require(allocationsAfterWarmup != 0, "The first Path search did not initialise workspace scratch");

		for (uint32_t search = 0; search < 300; ++search)
		{
			auto const repeated = graph->calculatePath(agent, source, destination);
			require(repeated && digest(repeated) == expectedDigest,
				"Repeated Path searches produced different Vertex or cost sequences");
		}
		require(graph->getScratchAllocationCount() == allocationsAfterWarmup,
			"A warmed Path workspace grew while searching the same Graph");
		require(graph->getDirectedFactsBuildCount() == 1,
			"Property changes or repeated searches rebuilt immutable directed geometry");

		core::PathfindingWorkspace topologyWorkspace;
		core::RouteDecisionContext baseline{ nullptr, {}, {} };
		topologyWorkspace.captureRouteCosts(*graph, baseline);
		auto const beforeRebuild = topologyWorkspace.getScratchAllocationCount();
		topologyWorkspace.invalidateTopology();
		topologyWorkspace.captureRouteCosts(*graph, baseline);
		require(topologyWorkspace.getDirectedFactsBuildCount() == 2
			&& topologyWorkspace.getScratchAllocationCount() == beforeRebuild,
			"Explicit topology invalidation failed to rebuild using retained storage");

		require(!graph->calculatePath(agent, source, unreachable),
			"A Path crossed between disconnected Sectors");
		auto const afterFailure = graph->calculatePath(agent, source, destination);
		require(afterFailure && digest(afterFailure) == expectedDigest,
			"A failed search leaked stale workspace state into the next search");
		require(graph->getScratchAllocationCount() == allocationsAfterWarmup,
			"Failed/successful searches grew a warmed Path workspace");

		auto const stationary = graph->calculatePath(agent, source, source);
		require(stationary && stationary->nodes.size() == 1
			&& stationary->nodes.front().targetVertex == source
			&& !stationary->nodes.front().edge
			&& std::abs(stationary->nodes.front().cumulativePerceivedCost) < 0.0001f,
			"Source-equals-target Path behaviour changed");
	}
}

namespace
{
	void populationRouting()
	{
		auto const first = routing_support::populationRoutingRun();
		require(first == routing_support::populationRoutingRun({}, false), "Fresh population Path digest changed");
	}
}

namespace routing_smoke
{
	void registerWorkspace(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "costContract", [](smoke::Context const&) { costContract(); } });
		checks.push_back({ "capturedInputsMatchEagerProviders", [](smoke::Context const& smokeContext) { capturedInputsMatchEagerProviders(smokeContext); } });
		checks.push_back({ "localDemandWorkIsBounded", [](smoke::Context const&) { localDemandWorkIsBounded(); } });
		checks.push_back({ "bundledRoutesMatchReference", [](smoke::Context const& smokeContext) { bundledRoutesMatchReference(smokeContext); } });
		checks.push_back({ "reusedWorkspaceIsStableAndDoesNotGrow", [](smoke::Context const&) { reusedWorkspaceIsStableAndDoesNotGrow(); } });
		checks.push_back({ "lowerBoundsAreUniversalAndBounded", [](smoke::Context const&) { lowerBoundsAreUniversalAndBounded(); } });
		checks.push_back({ "doorObservationEpochsIgnoreTicks", [](smoke::Context const&) { doorObservationEpochsIgnoreTicks(); } });
		checks.push_back({ "sourceInferenceMatchesOpenIntervalReference", [](smoke::Context const& smokeContext) { sourceInferenceMatchesOpenIntervalReference(smokeContext); } });
		checks.push_back({ "sourceIndexWorkIsLocal", [](smoke::Context const&) { sourceIndexWorkIsLocal(); } });
		checks.push_back({ "sourceIndexesFollowWalkwayEdits", [](smoke::Context const&) { sourceIndexesFollowWalkwayEdits(); } });
		checks.push_back({ "uncertainRoutesSurviveWorldReset", [](smoke::Context const&) { uncertainRoutesSurviveWorldReset(); } });
		checks.push_back({ "populationRouting", [](smoke::Context const&) { populationRouting(); } });
	}
}
