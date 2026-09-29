#include <algorithm>
#include <cmath>
#include <limits>

#include "core/Defines.h"
#include "core/Pathing.h"
#include "core/Graph.h"
#include "core/Marker.h"
#include "core/Location.h"
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
		auto mix = [](uint64_t value)
		{
			value += 0x9e3779b97f4a7c15ULL;
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31);
		};
		for (auto const& vertex : graph.getVertices())
		{
			routeOffsets[vertex->getSearchIndex()] = routeCosts.size();
			for (auto const& edge : vertex->getEdges())
			{
				auto const target = edge->getOtherVertex(vertex);
				auto facts = edge->getDirectedTraversalFacts(target, context);
				if (facts.feasible && facts.components.uncertaintyUnits > 0)
				{
					auto const hash = mix(context.perceptionKey
						^ (uint64_t{ edge->getId() } << 32) ^ target->getId()
						^ mix(context.observationEpoch));
					auto const unit = static_cast<float>(hash >> 40) / 16777215.0f;
					auto const amplitude = (1.0f - context.profile.routeFamiliarity)
						* facts.components.uncertaintyUnits;
					facts.components.perceptionVariationUnits = (unit * 2.0f - 1.0f) * amplitude;
				}
				routeCosts.push_back(context.policy.evaluate(facts, context.profile));
			}
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

			std::shared_ptr<Path> reconstructPath(Graph const* graph,
				uint32_t targetSlot, PathfindingWorkspace const& workspace)
			{
				auto const generation = workspace.getGeneration();
				if (workspace.visitGenerations[targetSlot] != generation) return nullptr;

				std::vector<PathNode> nodes;
				auto const& vertices = graph->getVertices();
				auto slot = targetSlot;
				while (workspace.cameFrom[slot] != slot)
				{
					nodes.push_back({ workspace.edges[slot], vertices[slot], workspace.scores[slot], workspace.durations[slot] });
					slot = workspace.cameFrom[slot];
				}
				nodes.push_back({ nullptr, vertices[slot], workspace.scores[slot], workspace.durations[slot] });
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
			auto profile = graph->getRouteChoicePolicy().baselineProfile;
			// Resolve Agent-authored preferences once for this immutable search
			// context; directed-edge capture then reuses the concrete value.
			profile.stairSpeedModifier = routingAgent->getEffectiveStairSpeedModifier().value;
			profile.escalatorWalkingChance = routingAgent->getEffectiveEscalatorWalkingChance().value;
			profile.interactionAversion = routingAgent->getEffectiveInteractionAversion().value;
			profile.effortAversion = routingAgent->getEffectiveEffortAversion().value;
			profile.waitingAversion = routingAgent->getEffectiveWaitingAversion().value;
			profile.crowdAversion = routingAgent->getEffectiveCrowdAversion().value;
			profile.riskAversion = routingAgent->getEffectiveRiskAversion().value;
			profile.routeFamiliarity = routingAgent->getEffectiveRouteFamiliarity().value;
			auto const worldSeed = graph->getWorld() ? graph->getWorld()->getRandomSeed() : uint64_t{ 0 };
			auto const agentId = agent && graph->getWorld()
				? graph->getWorld()->getAgentId(agent).value : uint64_t{ 0 };
			auto const journeyIdentity = routingAgent->getRouteJourneyIdentity(target.get());
			auto const perceptionKey = worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
				^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL);
			RouteDecisionContext const context{ routingAgent, profile,
				graph->getRouteChoicePolicy(), agent ? agent->getSector() : nullptr,
				routingAgent->getWalkSpeed(), graph->getWorld(), routingAgent->getClimbSpeed(),
				false, perceptionKey, 0 };
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

			auto runSearch = [&](RouteDecisionContext const& searchContext)
			{
				auto& workspace = graph->getPathfindingWorkspace();
				auto const& vertices = graph->getVertices();
				workspace.beginSearch(vertices.size());
				workspace.captureRouteCosts(*graph, searchContext);
				auto const generation = workspace.getGeneration();
				auto seed = [&](node_type const& vertex, float approachSeconds)
				{
					auto slot = vertex->getSearchIndex();
					if (!std::isfinite(approachSeconds) || approachSeconds < 0)
						throw std::invalid_argument("Invalid route approach duration");
					if (workspace.visitGenerations[slot] == generation
						&& workspace.scores[slot] <= approachSeconds) return;
					workspace.visitGenerations[slot] = generation;
					workspace.cameFrom[slot] = slot;
					workspace.scores[slot] = approachSeconds;
					workspace.durations[slot] = approachSeconds;
					workspace.edges[slot].reset();
					workspace.put(slot, approachSeconds);
				};
				auto const floorSource = inferredSource && dynamic_cast<Location const*>(agent->getSector());
				if (floorSource)
				{
					auto const position = agent->getGlobalPosition();
					seed(source, position.distanceTo(source->getPosition()) / searchContext.walkSpeed);
					// A virtual source splits the ordinary floor edge beneath the Agent.
					// Seed both endpoints with actual approach time, not the full edge
					// length from an arbitrarily chosen nearest vertex. Never split a
					// Gap, Force Bridge, threshold, or transit edge to bypass admission.
					for (auto const& edge : graph->getEdges())
					{
						if (edge->getType() != EdgeType::Location) continue;
						auto a = edge->getVertex(0);
						auto b = edge->getVertex(1);
						if (a->getSector().get() != agent->getSector()
							|| b->getSector().get() != agent->getSector()) continue;
						auto const pa = a->getPosition();
						auto const pb = b->getPosition();
						if (std::abs(pa.y - position.y) > 0.001f || std::abs(pb.y - position.y) > 0.001f
							|| position.x <= std::min(pa.x, pb.x) || position.x >= std::max(pa.x, pb.x)) continue;
						seed(a, position.distanceTo(pa) / searchContext.walkSpeed);
						seed(b, position.distanceTo(pb) / searchContext.walkSpeed);
					}
				}
				else seed(source, 0.0f);

				while (!workspace.frontierEmpty())
				{
					auto const currentSlot = workspace.get();
					if (currentSlot == targetSlot) break;
					auto const& current = vertices[currentSlot];
					// A blocking Marker can still be a Path endpoint. It cannot be expanded
					// as an intermediate waypoint; a Path which starts there may leave it.
					if (blocksPathing(current))
					{
						auto const isOrigin = floorSource
							? current->getPosition().distanceTo(agent->getGlobalPosition()) < 0.001f
							: currentSlot == sourceSlot;
						if (!isOrigin) continue;
					}

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

				return reconstructPath(graph, targetSlot, workspace);
			};

			if (auto path = runSearch(context)) return path;
			RouteDecisionContext const fallbackContext{ routingAgent, profile,
				graph->getRouteChoicePolicy(), agent ? agent->getSector() : nullptr,
				routingAgent->getWalkSpeed(), graph->getWorld(), routingAgent->getClimbSpeed(),
				true, perceptionKey, 0 };
			return runSearch(fallbackContext);
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
