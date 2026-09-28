#include <algorithm>
#include <cmath>
#include <limits>

#include "core/Defines.h"
#include "core/Pathing.h"
#include "core/Graph.h"
#include "core/Marker.h"
#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Exceptions.h"
#include "core/Vertex.h"

namespace core
{
	void PathfindingWorkspace::captureRouteCosts(Graph const& graph, RouteDecisionContext const& context)
	{
		auto const oldOffsetsCapacity = routeOffsets.capacity();
		auto const oldCostsCapacity = routeCosts.capacity();
		routeOffsets.resize(graph.getVertices().size());
		routeCosts.clear();
		routeCosts.reserve(graph.getEdges().size() * 2);
		for (auto const& vertex : graph.getVertices())
		{
			routeOffsets[vertex->getSearchIndex()] = routeCosts.size();
			for (auto const& edge : vertex->getEdges())
				routeCosts.push_back(context.policy.evaluate(
					edge->getDirectedTraversalFacts(edge->getOtherVertex(vertex), context), context.profile));
		}
		if (routeOffsets.capacity() != oldOffsetsCapacity) ++mScratchAllocationCount;
		if (routeCosts.capacity() != oldCostsCapacity) ++mScratchAllocationCount;
	}

	bool PathfindingWorkspace::precedes(FrontierNode const& left, FrontierNode const& right)
	{
		if (left.priority < right.priority) return true;
		if (right.priority < left.priority) return false;
		return left.slot < right.slot;
	}

	void PathfindingWorkspace::swapFrontierNodes(uint32_t left, uint32_t right)
	{
		std::swap(mFrontier[left], mFrontier[right]);
		mFrontierPositions[mFrontier[left].slot] = left;
		mFrontierPositions[mFrontier[right].slot] = right;
	}

	void PathfindingWorkspace::siftUp(uint32_t position)
	{
		while (position != 0)
		{
			auto const parent = (position - 1) / 2;
			if (!precedes(mFrontier[position], mFrontier[parent])) break;
			swapFrontierNodes(position, parent);
			position = parent;
		}
	}

	void PathfindingWorkspace::siftDown(uint32_t position)
	{
		auto const count = static_cast<uint32_t>(mFrontier.size());
		for (;;)
		{
			auto const left = position * 2 + 1;
			if (left >= count) break;
			auto const right = left + 1;
			auto best = left;
			if (right < count && precedes(mFrontier[right], mFrontier[left])) best = right;
			if (!precedes(mFrontier[best], mFrontier[position])) break;
			swapFrontierNodes(position, best);
			position = best;
		}
	}

	void PathfindingWorkspace::beginSearch(size_t vertexCount)
	{
		auto resizeTracked = [this, vertexCount](auto& storage)
		{
			auto const oldCapacity = storage.capacity();
			storage.resize(vertexCount);
			if (storage.capacity() != oldCapacity) ++mScratchAllocationCount;
		};

		resizeTracked(scores);
		resizeTracked(durations);
		resizeTracked(cameFrom);
		resizeTracked(visitGenerations);
		resizeTracked(edges);
		resizeTracked(mFrontierPositions);
		resizeTracked(mFrontierGenerations);

		auto const oldFrontierCapacity = mFrontier.capacity();
		mFrontier.reserve(vertexCount);
		if (mFrontier.capacity() != oldFrontierCapacity) ++mScratchAllocationCount;
		mFrontier.clear();

		++mGeneration;
		if (mGeneration == 0)
		{
			// Generation zero means "never visited". This once-per-2^32-searches
			// reset prevents old stamps becoming live after wraparound.
			std::fill(visitGenerations.begin(), visitGenerations.end(), 0);
			std::fill(mFrontierGenerations.begin(), mFrontierGenerations.end(), 0);
			mGeneration = 1;
		}
	}

	uint32_t PathfindingWorkspace::getGeneration() const
	{
		return mGeneration;
	}

	bool PathfindingWorkspace::frontierEmpty() const
	{
		return mFrontier.empty();
	}

	void PathfindingWorkspace::put(uint32_t slot, float priority)
	{
		if (mFrontierGenerations[slot] == mGeneration
			&& mFrontierPositions[slot] != NoPosition)
		{
			auto const position = mFrontierPositions[slot];
			if (priority >= mFrontier[position].priority) return;
			mFrontier[position].priority = priority;
			siftUp(position);
			return;
		}

		mFrontierGenerations[slot] = mGeneration;
		mFrontierPositions[slot] = static_cast<uint32_t>(mFrontier.size());
		mFrontier.push_back({ priority, slot });
		siftUp(static_cast<uint32_t>(mFrontier.size() - 1));
	}

	uint32_t PathfindingWorkspace::get()
	{
		auto const result = mFrontier.front().slot;
		mFrontierPositions[result] = NoPosition;
		if (mFrontier.size() == 1)
		{
			mFrontier.pop_back();
			return result;
		}

		mFrontier.front() = mFrontier.back();
		mFrontier.pop_back();
		mFrontierPositions[mFrontier.front().slot] = 0;
		siftDown(0);
		return result;
	}

	uint64_t PathfindingWorkspace::getScratchAllocationCount() const
	{
		return mScratchAllocationCount;
	}

	namespace pathing
	{
		using node_type = std::shared_ptr<const Vertex>;

		namespace
		{
			bool blocksPathing(node_type const& vertex)
			{
				auto marker = std::dynamic_pointer_cast<Marker>(vertex->getObject());
				return marker && marker->hasProperty(MarkerProperty::BlocksPathing);
			}

			bool graphSlot(Graph const* graph, node_type const& vertex, uint32_t& slot)
			{
				if (!vertex) return false;
				slot = vertex->getSearchIndex();
				auto const& vertices = graph->getVertices();
				return slot < vertices.size() && vertices[slot].get() == vertex.get();
			}

			std::shared_ptr<Path> reconstructPath(Graph const* graph, uint32_t sourceSlot,
				uint32_t targetSlot, PathfindingWorkspace const& workspace)
			{
				auto const generation = workspace.getGeneration();
				if (workspace.visitGenerations[targetSlot] != generation) return nullptr;

				std::vector<PathNode> nodes;
				auto const& vertices = graph->getVertices();
				auto slot = targetSlot;
				while (slot != sourceSlot)
				{
					nodes.push_back({ workspace.edges[slot], vertices[slot], workspace.scores[slot], workspace.durations[slot] });
					slot = workspace.cameFrom[slot];
				}
				nodes.push_back({ nullptr, vertices[sourceSlot], 0.0f, 0.0f });
				std::reverse(nodes.begin(), nodes.end());
				return std::make_shared<Path>(std::move(nodes));
			}
		}

		std::shared_ptr<Path> findPath(Agent const* agent, Graph const* graph,
			node_type source, node_type target)
		{
			if (!graph || (!agent && !source)) return nullptr;
			// A real baseline Agent keeps legacy implementations null-safe.
			std::optional<Agent> baselineAgent;
			if (!agent) baselineAgent.emplace("Route preview");
			auto const* routingAgent = agent ? agent : &*baselineAgent;
			RouteDecisionContext const context{ routingAgent,
				graph->getRouteChoicePolicy().baselineProfile, graph->getRouteChoicePolicy() };
			auto const inferredSource = !source;
			if (inferredSource)
			{
				try
				{
					source = graph->getPathSourceVertex(agent->getSector(), agent->getGlobalPosition());
				}
				catch (GraphException const&)
				{
					return nullptr;
				}
			}

			uint32_t sourceSlot = 0;
			uint32_t targetSlot = 0;
			if (!graphSlot(graph, source, sourceSlot) || !graphSlot(graph, target, targetSlot))
				return nullptr;

			auto& workspace = graph->getPathfindingWorkspace();
			auto const& vertices = graph->getVertices();
			workspace.beginSearch(vertices.size());
			workspace.captureRouteCosts(*graph, context);
			auto const generation = workspace.getGeneration();
			workspace.visitGenerations[sourceSlot] = generation;
			workspace.cameFrom[sourceSlot] = sourceSlot;
			workspace.scores[sourceSlot] = 0.0f;
			workspace.durations[sourceSlot] = 0.0f;
			workspace.edges[sourceSlot].reset();
			workspace.put(sourceSlot, 0.0f);

			while (!workspace.frontierEmpty())
			{
				auto const currentSlot = workspace.get();
				if (currentSlot == targetSlot) break;
				auto const& current = vertices[currentSlot];
				// A blocking Marker can still be a Path endpoint. It cannot be expanded
				// as an intermediate waypoint; a Path which starts there may leave it.
				if (currentSlot != sourceSlot && blocksPathing(current)) continue;

				auto arcIndex = workspace.routeOffsets[currentSlot];
				for (auto const& edge : current->getEdges())
				{
					auto const next = edge->getOtherVertex(current);
					auto const nextSlot = next->getSearchIndex();
					auto const& cost = workspace.routeCosts[arcIndex++];
					if (!cost) continue;
					auto const newCost = workspace.scores[currentSlot] + cost->perceivedCost;
					if (!std::isfinite(newCost))
						throw std::invalid_argument("Cumulative route cost is not finite");

					if (workspace.visitGenerations[nextSlot] != generation
						|| newCost < workspace.scores[nextSlot])
					{
						workspace.visitGenerations[nextSlot] = generation;
						workspace.scores[nextSlot] = newCost;
						workspace.durations[nextSlot].reset();
						if (workspace.durations[currentSlot] && cost->objectiveDurationSeconds)
						{
							auto const duration = *workspace.durations[currentSlot] + *cost->objectiveDurationSeconds;
							if (!std::isfinite(duration)) throw std::invalid_argument("Route duration is not finite");
							workspace.durations[nextSlot] = duration;
						}
						workspace.cameFrom[nextSlot] = currentSlot;
						workspace.edges[nextSlot] = edge;
						workspace.put(nextSlot, newCost);
					}
				}
			}

			auto path = reconstructPath(graph, sourceSlot, targetSlot, workspace);

			// Skip a backwards approach only along ordinary horizontal floor.
			if (inferredSource && path && path->nodes.size() >= 2)
			{
				auto const& agentPosition = agent->getGlobalPosition();
				auto const& firstVertex = path->nodes[0].targetVertex;
				auto const& secondVertex = path->nodes[1].targetVertex;
				auto const firstDirection = firstVertex->getPosition() - agentPosition;
				auto const secondDirection = secondVertex->getPosition() - agentPosition;
				auto const directionsAreOpposite =
					firstDirection.x * secondDirection.x + firstDirection.y * secondDirection.y < 0.0f;

				if (firstVertex->getSector() == secondVertex->getSector() && directionsAreOpposite
					&& std::abs(firstDirection.y) < 0.001f && std::abs(secondDirection.y) < 0.001f
					&& firstVertex->getSubType() != VertexSubType::Interactable
					&& path->nodes[1].edge && path->nodes[1].edge->getType() == EdgeType::Location)
				{
					auto const skippedDuration = path->nodes[1].objectiveDurationSeconds;
					auto const skippedCost = path->nodes[1].edgeWeight;
					path->nodes.erase(path->nodes.begin());
					path->nodes[0].edge = nullptr;
					for (auto& node : path->nodes)
					{
						node.edgeWeight -= skippedCost;
						if (node.objectiveDurationSeconds && skippedDuration)
							*node.objectiveDurationSeconds -= *skippedDuration;
					}
					path->nodes[0].objectiveDurationSeconds = 0.0f;
				}
			}

			return path;
		}

		std::shared_ptr<const Vertex> findNextVertexForVertexInPath(
			std::shared_ptr<Path> path, Vertex const* vertex, uint32_t index)
		{
			for (; index < static_cast<uint32_t>(path->nodes.size()); ++index)
			{
				auto const& node = path->nodes[index];
				if (vertex->getId() == node.edge->getOtherVertex(node.targetVertex)->getId())
					return node.targetVertex;
			}
			return nullptr;
		}
	}
}
