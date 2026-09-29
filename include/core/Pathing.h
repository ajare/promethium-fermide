#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/Path.h"
#include "core/Vector2.h"

namespace core
{
	class Agent;
	class Edge;
	class Graph;
	class Vertex;

	// Reusable, Graph-owned scratch for one path search. Slots are dense indices
	// into Graph::getVertices(); generation stamps make starting a search O(1).
	class PathfindingWorkspace
	{
	public:
		struct FrontierNode
		{
			double priority;
			uint32_t slot;
		};

		struct DirectedArc
		{
			// Borrowed from the Graph-owned adjacency lists, never Agent-owned.
			std::shared_ptr<const Edge> const* edge;
			uint32_t targetSlot;
			float length;
			float universalLowerBound;
			uint64_t perceptionIdentity;
		};
		std::vector<DirectedArc> directedArcs;
		void invalidateTopology();
		[[nodiscard]] uint64_t getDirectedFactsBuildCount() const { return mDirectedFactsBuildCount; }
		[[nodiscard]] size_t getScratchBytes() const;
		// Four target tables, LRU-bounded and invalidated with directed topology.
		std::vector<double> const& prepareTargetLowerBounds(uint32_t targetSlot);
		[[nodiscard]] double routePriority(uint32_t slot, float score) const;
		[[nodiscard]] uint64_t getLowerBoundBuildCount() const { return mLowerBoundBuildCount; }
		[[nodiscard]] uint64_t getLowerBoundHitCount() const { return mLowerBoundHitCount; }

		std::vector<float> scores;
		std::vector<std::optional<float>> durations;
		std::vector<size_t> routeOffsets;
		std::vector<std::optional<EvaluatedRouteCost>> routeCosts;
		std::vector<std::optional<size_t>> selectedRouteCosts;
		void captureRouteCosts(Graph const& graph, RouteDecisionContext const& context);
		std::vector<uint32_t> cameFrom;
		std::vector<uint32_t> visitGenerations;
		std::vector<std::shared_ptr<const Edge>> edges;

		void beginSearch(size_t vertexCount);
		[[nodiscard]] uint32_t getGeneration() const;
		[[nodiscard]] bool frontierEmpty() const;
		void put(uint32_t slot, double priority);
		uint32_t get();
		[[nodiscard]] uint64_t getScratchAllocationCount() const;

	private:
		static constexpr uint32_t NoPosition = ~uint32_t{ 0 };

		std::vector<FrontierNode> mFrontier;
		std::vector<uint32_t> mFrontierPositions;
		std::vector<uint32_t> mFrontierGenerations;
		uint32_t mGeneration{ 0 };
		uint64_t mScratchAllocationCount{ 0 };
		uint64_t mDirectedFactsBuildCount{ 0 };
		bool mTopologyCaptured = false;
		void captureTopology(Graph const& graph);
		struct TargetLowerBounds
		{
			uint32_t target = NoPosition;
			uint64_t lastUse = 0;
			std::vector<double> distances;
		};
		std::array<TargetLowerBounds, 4> mTargetLowerBounds;
		size_t mSelectedLowerBounds = 0;
		uint64_t mLowerBoundClock = 0;
		uint64_t mLowerBoundBuildCount = 0;
		uint64_t mLowerBoundHitCount = 0;

		[[nodiscard]] static bool precedes(FrontierNode const& left, FrontierNode const& right);
		void swapFrontierNodes(uint32_t left, uint32_t right);
		void siftUp(uint32_t position);
		void siftDown(uint32_t position);
	};

	namespace pathing
	{
		std::shared_ptr<Path> findPath(Agent const* agent, Graph const* graph,
			std::shared_ptr<const Vertex> source, std::shared_ptr<const Vertex> target);

		// Scores both suffixes from one immutable Route decision context so a
		// waiting reconsideration never compares stale or differently observed costs.
		std::optional<std::pair<float, float>> comparePathSuffixCosts(
			Agent const& agent, Graph const& graph,
			Path const& current, uint32_t currentFromNode,
			Path const& alternative, uint32_t alternativeFromNode);

		// Aggregation is intentionally on demand. A Path without a captured
		// decision context (notably a restored Path) has no historical diagnostic.
		std::optional<PathRouteDiagnostics> getRouteDiagnostics(Path const& path);
		bool routeDiagnosticContextIsStale(Agent const& agent,
			Graph const& graph, RouteDiagnosticContext const& context);
		bool routeDiagnosticContextIsStale(Agent const& agent,
			Graph const& graph, Path const& path);

		// Runs a bounded reverse search only when requested. It does not attach a
		// search tree or alternative set to the Agent or Path.
		std::optional<PathRouteExplanation> explainRoute(Agent const& agent,
			Graph const& graph, Path const& path,
			uint32_t maximumExpandedVertices = 4096,
			uint32_t maximumEvaluatedTraversals = 32768);
		char const* routeExclusionReasonText(RouteExclusionReason reason);

		std::shared_ptr<const Vertex> findNextVertexForVertexInPath(
			std::shared_ptr<Path> path, Vertex const* vertex, uint32_t index = 0);
	}
}
