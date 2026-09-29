#include <bit>
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
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/Vertex.h"
#include "core/World.h"

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
			result.emplace_back(node.targetVertex->getId(), std::bit_cast<uint32_t>(node.edgeWeight));
		return result;
	}

	void costContract()
	{
		core::RouteChoicePolicy policy;
		core::DirectedTraversalFacts facts;
		require(!policy.evaluate(facts, {}), "Excluded traversal acquired a finite cost");
		facts.feasible = true;
		facts.components.motionSeconds = 2;
		facts.components.physicalEffortUnits = 2000000;
		facts.objectiveDurationSeconds = 2;
		auto cost = policy.evaluate(facts, {});
		require(cost && cost->perceivedCost == 2000002 && cost->objectiveDurationSeconds == 2,
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

	void bundledRoutesMatchReference()
	{
		auto const root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
		for (auto const* filename : { "lift-test-1.world.yaml", "shuttle-test-1.world.yaml",
			"stairwell-test-1.world.yaml", "staircase-test-1.world.yaml" })
		{
			auto world = core::loadWorldDocument(root / "resources" / "test-worlds" / filename);
			auto graph = world->getGraph();
			core::Agent agent("Reference walker");
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
					if (path) require(std::abs(path->nodes.back().edgeWeight - expected) < 0.001f,
						"Bundled Path does not minimise perceived directed costs (#191)");
				}
			}
		}
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
		for (size_t i = 1; i < first->nodes.size(); ++i)
		{
			auto const& node = first->nodes[i];
			require(node.edge != nullptr, "A non-source Path node has no Edge");
			cumulativeCost += node.edge->getWeight(node.targetVertex, agent, true);
			require(std::abs(cumulativeCost - node.edgeWeight) < 0.0001f,
				"Path cumulative cost differs from its Edge weights");
		}
		require(first->nodes.back().objectiveDurationSeconds
			&& std::abs(*first->nodes.back().objectiveDurationSeconds - cumulativeCost) < 0.0001f,
			"Walking Path did not report physical objective duration");
		auto const diagnostic = core::pathing::getRouteDiagnostics(*first);
		require(diagnostic
			&& std::abs(diagnostic->components.total() - diagnostic->perceivedCost) < 0.0001f
			&& diagnostic->objectiveEstimatedDurationSeconds
			&& std::abs(*diagnostic->objectiveEstimatedDurationSeconds - cumulativeCost) < 0.0001f,
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
			&& std::abs(stationary->nodes.front().edgeWeight) < 0.0001f,
			"Source-equals-target Path behaviour changed");
	}
}

void runPathfindingWorkspaceSmokeChecks()
{
	costContract();
	bundledRoutesMatchReference();
	reusedWorkspaceIsStableAndDoesNotGrow();
}
