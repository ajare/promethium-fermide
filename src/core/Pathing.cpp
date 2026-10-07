#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>

#include "core/Defines.h"
#include "core/Pathing.h"
#include "core/Graph.h"
#include "core/Marker.h"
#include "core/Location.h"
#include "core/MobilityProfile.h"
#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Exceptions.h"
#include "core/Vertex.h"

namespace core
{
	void PathfindingWorkspace::invalidateTopology()
	{
		mTopologyCaptured = false;
		directedArcs.clear();
		routeOffsets.clear();
		mDecision.reset();
		for (auto& table : mTargetLowerBounds) table.target = NoPosition;
	}

	void PathfindingWorkspace::captureTopology(Graph const& graph)
	{
		if (mTopologyCaptured) return;
		auto const oldOffsetsCapacity = routeOffsets.capacity();
		auto const oldArcsCapacity = directedArcs.capacity();
		routeOffsets.resize(graph.getVertices().size() + 1);
		directedArcs.clear();
		mHasLocalDepthRoutes = false;
		auto const oldInputCapacity = mInputArcs.capacity();
		mInputArcs.clear();
		mInputArcs.reserve(graph.getEdges().size() * 2);
		directedArcs.reserve(graph.getEdges().size() * 2);
		for (auto const& vertex : graph.getVertices())
		{
			routeOffsets[vertex->getSearchIndex()] = directedArcs.size();
			for (auto const& edge : vertex->getEdges())
			{
				mHasLocalDepthRoutes |= edge->getLocalDepth() != 0;
				auto const target = edge->getOtherVertex(vertex);
				auto const length = edge->getLength();
				// Relax every non-walking traversal to zero, including all fast
				// transports, forbidden directions, and coincident topology arcs.
				// The remaining walking costs use the maximum valid physical speed.
				auto const lowerBound = edge->getType() != EdgeType::Location ? 0.0f
					: length == 0 ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
					: length / (CORE_AGENT_BASE_WALK_SPEED * AgentWalkSpeedModifierMaximum);
				auto const inputIndex = edge->getType() == EdgeType::Location
					? std::numeric_limits<size_t>::max() : mInputArcs.size();
				if (edge->getType() != EdgeType::Location) mInputArcs.push_back(directedArcs.size());
				directedArcs.push_back({ &edge, target->getSearchIndex(), length, lowerBound,
					(uint64_t{ edge->getRoutingIndex() } << 32) ^ target->getSearchIndex(), inputIndex });
			}
		}
		routeOffsets.back() = directedArcs.size();
		if (routeOffsets.capacity() != oldOffsetsCapacity) ++mScratchAllocationCount;
		if (directedArcs.capacity() != oldArcsCapacity) ++mScratchAllocationCount;
		if (mInputArcs.capacity() != oldInputCapacity) ++mScratchAllocationCount;
		// Reserve every LRU slot on warm-up, so novel destinations and evictions
		// cannot grow scratch during later searches on this Graph.
		for (auto& table : mTargetLowerBounds)
		{
			auto const oldCapacity = table.distances.capacity();
			table.distances.resize(graph.getVertices().size());
			if (table.distances.capacity() != oldCapacity) ++mScratchAllocationCount;
		}
		++mDirectedFactsBuildCount;
		mTopologyCaptured = true;
	}

	size_t PathfindingWorkspace::getScratchBytes() const
	{
		size_t lowerBoundBytes = 0;
		for (auto const& table : mTargetLowerBounds)
			lowerBoundBytes += table.distances.capacity() * sizeof(double);
		return lowerBoundBytes + directedArcs.capacity() * sizeof(DirectedArc)
			+ routeOffsets.capacity() * sizeof(size_t)
			+ routeCosts.capacity() * sizeof(decltype(routeCosts)::value_type)
			+ mCostGenerations.capacity() * sizeof(uint32_t)
			+ mExclusions.capacity() * sizeof(RouteExclusionReason)
			+ mInputArcs.capacity() * sizeof(size_t)
			+ mInputs.capacity() * sizeof(RouteTraversalInputs)
			+ selectedRouteCosts.capacity() * sizeof(decltype(selectedRouteCosts)::value_type)
			+ scores.capacity() * sizeof(float)
			+ durations.capacity() * sizeof(decltype(durations)::value_type)
			+ (cameFrom.capacity() + visitGenerations.capacity()
				+ mFrontierPositions.capacity() + mFrontierGenerations.capacity()) * sizeof(uint32_t)
			+ edges.capacity() * sizeof(decltype(edges)::value_type)
			+ mFrontier.capacity() * sizeof(FrontierNode);
	}

	void PathfindingWorkspace::nextCostGeneration()
	{
		if (++mCostGeneration == 0)
		{
			std::fill(mCostGenerations.begin(), mCostGenerations.end(), 0);
			mCostGeneration = 1;
		}
	}

	void PathfindingWorkspace::beginRouteDecision(Graph const& graph, RouteDecisionContext const& input)
	{
		auto const started = std::chrono::steady_clock::now();
		captureTopology(graph);
		mGraph = &graph;
		mDecision.emplace(RouteDecisionContext{ input.agent, input.profile, input.policy,
			input.observationSector, input.walkSpeed, input.world, input.climbSpeed,
			input.allowFallbackMobility, input.perceptionKey, input.observationEpoch,
			input.mobilityProfile ? input.mobilityProfile : std::optional<MobilityProfile>{
				input.agent ? input.agent->getEffectiveMobilityProfile().value : MobilityProfile{} }, input.localDepth });
		auto resize = [this](auto& storage, size_t size)
		{
			auto const capacity = storage.capacity();
			storage.resize(size);
			if (storage.capacity() != capacity) ++mScratchAllocationCount;
		};
		resize(routeCosts, directedArcs.size());
		resize(mCostGenerations, directedArcs.size());
		resize(mExclusions, directedArcs.size());
		resize(mInputs, mInputArcs.size());
		for (size_t i = 0; i < mInputArcs.size(); ++i)
		{
			auto const& arc = directedArcs[mInputArcs[i]];
			mInputs[i] = RouteTraversalInputs::capture(**arc.edge, graph.getVertices()[arc.targetSlot], *mDecision);
			++work.preparedArcs;
		}
		nextCostGeneration();
		++work.decisions;
		work.preparationSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	}

	void PathfindingWorkspace::allowFallbackMobility()
	{
		auto const c = *mDecision;
		mDecision.emplace(RouteDecisionContext{ c.agent, c.profile, c.policy, c.observationSector,
			c.walkSpeed, c.world, c.climbSpeed, true, c.perceptionKey, c.observationEpoch, c.mobilityProfile, c.localDepth });
		// Feasibility is part of the cache key. Keep exactly the same captured
		// observations, but never reuse first-pass exclusions in the fallback pass.
		nextCostGeneration();
	}

	DirectedTraversalFacts PathfindingWorkspace::arcFacts(size_t index) const
	{
		auto const& arc = directedArcs.at(index);
		auto const& context = *mDecision;
		DirectedTraversalFacts facts;
		// Location passage is a hard, destination-owned constraint, including
		// ordinary floor arcs (which otherwise bypass traversal-input capture).
		// It never depends on Permission adherence or a device's usable state.
		if (context.agent && context.world)
		{
			auto sector = mGraph->getVertices()[arc.targetSlot]->getSector();
			if (sector && !context.world->canAgentAccessLocation(*sector, *context.agent))
			{
				// Walking to an exit may require source-owned waypoints. Admit
				// those only from within the occupied source Location; an arc
				// from outside can never re-enter it. Protected destinations are
				// rejected separately, even when they share the origin Location.
				auto source = (*arc.edge)->getOtherVertex(mGraph->getVertices()[arc.targetSlot]);
				if (sector.get() != context.observationSector || source->getSector() != sector)
				{
					facts.exclusionReason = RouteExclusionReason::Permission;
					return facts;
				}
			}
		}
		if (arc.inputIndex == std::numeric_limits<size_t>::max())
		{
			facts.feasible = true;
			facts.components.motionSeconds = arc.length == 0.0f
				? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : arc.length / context.walkSpeed;
			facts.objectiveDurationSeconds = facts.components.motionSeconds;
			facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		}
		else facts = mInputs[arc.inputIndex].evaluate(context);
		if (facts.feasible && facts.components.uncertaintyUnits > 0)
		{
			auto mix = [](uint64_t value)
			{
				value += 0x9e3779b97f4a7c15ULL;
				value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
				value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
				return value ^ (value >> 31);
			};
			auto const hash = mix(context.perceptionKey ^ arc.perceptionIdentity ^ mix(context.observationEpoch));
			auto const unit = static_cast<float>(hash >> 40) / 16777215.0f;
			auto const amplitude = (1.0f - context.profile.routeFamiliarity) * facts.components.uncertaintyUnits;
			facts.components.perceptionVariationUnits = (unit * 2.0f - 1.0f) * amplitude;
		}
		return facts;
	}

	std::optional<EvaluatedRouteCost> const& PathfindingWorkspace::evaluateArc(size_t index)
	{
		if (mCostGenerations.at(index) != mCostGeneration)
		{
			auto const facts = arcFacts(index);
			routeCosts[index] = mDecision->policy.evaluate(facts, mDecision->profile);
			mExclusions[index] = facts.exclusionReason;
			mCostGenerations[index] = mCostGeneration;
			++work.evaluatedArcs;
		}
		else ++work.cacheHits;
		return routeCosts[index];
	}

	void PathfindingWorkspace::captureRouteCosts(Graph const& graph, RouteDecisionContext const& input)
	{
		beginRouteDecision(graph, input);
		for (size_t i = 0; i < directedArcs.size(); ++i) evaluateArc(i);
	}

	std::vector<double> const& PathfindingWorkspace::prepareTargetLowerBounds(uint32_t targetSlot)
	{
		if (!mTopologyCaptured || static_cast<size_t>(targetSlot) + 1 >= routeOffsets.size())
			throw std::invalid_argument("Lower-bound target is outside captured topology");
		++mLowerBoundClock;
		for (size_t index = 0; index < mTargetLowerBounds.size(); ++index)
		{
			auto& table = mTargetLowerBounds[index];
			if (table.target != targetSlot) continue;
			table.lastUse = mLowerBoundClock;
			mSelectedLowerBounds = index;
			++mLowerBoundHitCount;
			return table.distances;
		}
		auto selected = std::min_element(mTargetLowerBounds.begin(), mTargetLowerBounds.end(),
			[](auto const& a, auto const& b) { return a.lastUse < b.lastUse; });
		mSelectedLowerBounds = static_cast<size_t>(selected - mTargetLowerBounds.begin());
		auto& table = *selected;
		table.target = targetSlot;
		table.lastUse = mLowerBoundClock;
		std::fill(table.distances.begin(), table.distances.end(), std::numeric_limits<double>::infinity());
		// Universal bounds are symmetric, so ordinary adjacency is also reverse
		// adjacency. Ignore blocking Markers and feasibility: this is a relaxation.
		beginSearch(table.distances.size());
		table.distances[targetSlot] = 0;
		put(targetSlot, 0);
		while (!frontierEmpty())
		{
			auto const current = get();
			for (auto index = routeOffsets[current]; index < routeOffsets[current + 1]; ++index)
			{
				auto const& arc = directedArcs[index];
				auto const candidate = table.distances[current] + arc.universalLowerBound;
				if (candidate >= table.distances[arc.targetSlot]) continue;
				table.distances[arc.targetSlot] = candidate;
				put(arc.targetSlot, candidate);
			}
		}
		auto const count = static_cast<double>(table.distances.size());
		auto const factor = std::max(0.0, 1.0 - 2.0 * count * std::numeric_limits<float>::epsilon());
		for (auto& distance : table.distances)
			if (std::isfinite(distance)) distance = std::max(0.0,
				distance * factor - count * std::numeric_limits<float>::denorm_min());
		++mLowerBoundBuildCount;
		return table.distances;
	}

	double PathfindingWorkspace::routePriority(uint32_t slot, float score) const
	{
		auto const& table = mTargetLowerBounds[mSelectedLowerBounds];
		if (table.target == NoPosition || slot == table.target) return score;
		auto const lowerBound = table.distances[slot];
		if (!std::isfinite(lowerBound)) return score; // zero-heuristic fallback
		// Costs are accumulated in float for compatibility. Reverse sums have a
		// different order: ordinary g+h could slightly overestimate the final
		// float score. Any optimal continuation is simple (at most V arcs). Budget
		// two float epsilons per vertex for both summations and arc rounding, plus
		// absolute underflow error. Discount g as well as h: g participates in
		// every remaining addition. A target retains its exact score, so stopping
		// when it wins the frontier remains sound even at rounding boundaries.
		auto const count = static_cast<double>(table.distances.size());
		auto const factor = 1.0 - 2.0 * count * std::numeric_limits<float>::epsilon();
		if (factor <= 0) return score; // zero heuristic for very large Graphs
		// Adding non-negative costs cannot reduce g, even with float rounding.
		return std::max(static_cast<double>(score), (static_cast<double>(score) + lowerBound) * factor
			- count * std::numeric_limits<float>::denorm_min());
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
		resizeTracked(selectedRouteCosts);
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

	void PathfindingWorkspace::put(uint32_t slot, double priority)
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
					std::optional<EvaluatedRouteCost> diagnostic;
					if (workspace.selectedRouteCosts[slot])
						diagnostic = workspace.routeCosts[*workspace.selectedRouteCosts[slot]];
					nodes.push_back({ workspace.edges[slot], vertices[slot], workspace.scores[slot],
						workspace.durations[slot], diagnostic });
					slot = workspace.cameFrom[slot];
				}
				// The inferred-source approach is part of both perceived movement and
				// objective estimated duration, even though it has no Graph Edge.
				EvaluatedRouteCost sourceCost{ workspace.scores[slot], workspace.durations[slot], {} };
				sourceCost.components.movement = workspace.scores[slot];
				nodes.push_back({ nullptr, vertices[slot], workspace.scores[slot],
					workspace.durations[slot], sourceCost });
				std::reverse(nodes.begin(), nodes.end());
				return std::make_shared<Path>(Path{ std::move(nodes), std::nullopt });
			}

			// Local depth is arrival context, not a vertex property. Each (vertex,
			// incoming depth) needs its own label: a worse prefix can have a better
			// continuation. Strict lexicographic improvements prevent equal-cost
			// connectors from creating predecessor cycles. Ordinary depth-0 graphs
			// retain the dense reusable search below.
			std::shared_ptr<Path> findDepthContinuousPath(Graph const* graph,
				uint32_t targetSlot, PathfindingWorkspace& workspace,
				RouteDecisionContext const& context, uint32_t sourceSlot, bool floorSource)
			{
				struct Label
				{
					uint32_t vertex;
					int depth;
					float cost;
					uint64_t change;
					std::optional<float> duration;
					size_t parent;
					std::optional<size_t> arc;
				};
				struct Pending
				{
					double priority;
					uint64_t change;
					size_t label;
					bool operator<(Pending const& other) const
					{
						if (priority != other.priority) return priority > other.priority;
						if (change != other.change) return change > other.change;
						return label > other.label;
					}
				};
				auto const& vertices = graph->getVertices();
				std::vector<Label> labels;
				std::vector<std::vector<size_t>> arrivals(vertices.size());
				std::priority_queue<Pending> pending;
				auto offer = [&](Label candidate)
				{
					auto& atVertex = arrivals[candidate.vertex];
					auto found = std::find_if(atVertex.begin(), atVertex.end(), [&](size_t i)
						{ return labels[i].depth == candidate.depth; });
					if (found != atVertex.end())
					{
						auto const& previous = labels[*found];
						if (candidate.cost > previous.cost || (candidate.cost == previous.cost
							&& candidate.change >= previous.change)) return;
					}
					// Keep immutable predecessor labels, even when an arrival improves.
					auto const index = labels.size();
					labels.push_back(candidate);
					if (found == atVertex.end()) atVertex.push_back(index);
					else *found = index;
					pending.push({ candidate.cost, candidate.change, index });
				};
				for (uint32_t slot = 0; slot < vertices.size(); ++slot)
					if (workspace.visitGenerations[slot] == workspace.getGeneration())
						offer({ slot, context.localDepth, workspace.scores[slot], 0,
							workspace.durations[slot], labels.size(), std::nullopt });
				std::optional<size_t> best;
				while (!pending.empty())
				{
					auto const entry = pending.top();
					pending.pop();
					auto const current = labels[entry.label];
					if (best && entry.priority > labels[*best].cost) break;
					auto const& atVertex = arrivals[current.vertex];
					if (std::find(atVertex.begin(), atVertex.end(), entry.label) == atVertex.end()) continue;
					if (current.vertex == targetSlot)
					{
						if (!best || current.cost < labels[*best].cost
							|| (current.cost == labels[*best].cost && current.change < labels[*best].change))
							best = entry.label;
						continue;
					}
					if (blocksPathing(vertices[current.vertex]))
					{
						auto const isOrigin = floorSource
							? vertices[current.vertex]->getPosition().distanceTo(context.agent->getGlobalPosition()) < 0.001f
							: current.vertex == sourceSlot;
						if (!isOrigin) continue;
					}
					++workspace.work.expandedVertices;
					for (auto arcIndex = workspace.routeOffsets[current.vertex];
						arcIndex < workspace.routeOffsets[current.vertex + 1]; ++arcIndex)
					{
						auto const& arc = workspace.directedArcs[arcIndex];
						auto const& cost = workspace.evaluateArc(arcIndex);
						if (!cost) continue;
						auto const total = current.cost + cost->perceivedCost;
						if (!std::isfinite(total)) throw std::invalid_argument("Cumulative route cost is not finite");
						std::optional<float> duration;
						if (current.duration && cost->objectiveDurationSeconds)
						{
							duration = *current.duration + *cost->objectiveDurationSeconds;
							if (!std::isfinite(*duration)) throw std::invalid_argument("Route duration is not finite");
						}
						// Sector-local axes are unrelated. Crossing establishes a fresh
						// baseline, not a depth movement from the source's last route.
						auto const crossing = vertices[current.vertex]->getSector() != vertices[arc.targetSlot]->getSector();
						auto const depth = crossing ? 0 : (*arc.edge)->getLocalDepth();
						auto const gap = crossing ? int64_t{ 0 } : std::abs(int64_t{ depth } - int64_t{ current.depth });
						offer({ arc.targetSlot, depth, total, current.change + static_cast<uint64_t>(gap),
							duration, entry.label, arcIndex });
					}
				}
				if (!best) return nullptr;
				std::vector<PathNode> nodes;
				auto index = *best;
				while (labels[index].arc)
				{
					auto const& label = labels[index];
					auto const arc = *label.arc;
					nodes.push_back({ *workspace.directedArcs[arc].edge, vertices[label.vertex],
						label.cost, label.duration, workspace.routeCosts[arc] });
					index = label.parent;
				}
				auto const& origin = labels[index];
				EvaluatedRouteCost sourceCost{ origin.cost, origin.duration, {} };
				sourceCost.components.movement = origin.cost;
				nodes.push_back({ nullptr, vertices[origin.vertex], origin.cost, origin.duration, sourceCost });
				std::reverse(nodes.begin(), nodes.end());
				return std::make_shared<Path>(Path{ std::move(nodes), std::nullopt });
			}

			template<typename EffectiveProperty>
			RoutingPropertyProvenance provenance(EffectiveProperty const& property)
			{
				if (property.individual)
					return { RoutingPropertySource::Individual, {} };
				if (property.sourceTag)
					return { RoutingPropertySource::AgentTagSample, property.sourceTag };
				return {};
			}

			EffectiveRoutingProfile effectiveProfile(Agent const& agent,
				RouteChoicePolicy const& policy, RoutingProfileProvenance* sources = nullptr)
			{
				auto result = policy.baselineProfile;
				auto resolve = [&](auto const& property, auto member)
				{
					if (sources) sources->*member = provenance(property);
					return property.value;
				};
				result.walkSpeedModifier = resolve(agent.getEffectiveWalkSpeedModifier(), &RoutingProfileProvenance::walkSpeedModifier);
				result.stairSpeedModifier = resolve(agent.getEffectiveStairSpeedModifier(), &RoutingProfileProvenance::stairSpeedModifier);
				result.ladderSpeedModifier = resolve(agent.getEffectiveLadderSpeedModifier(), &RoutingProfileProvenance::ladderSpeedModifier);
				result.escalatorWalkingChance = resolve(agent.getEffectiveEscalatorWalkingChance(), &RoutingProfileProvenance::escalatorWalkingChance);
				result.interactionAversion = resolve(agent.getEffectiveInteractionAversion(), &RoutingProfileProvenance::interactionAversion);
				result.effortAversion = resolve(agent.getEffectiveEffortAversion(), &RoutingProfileProvenance::effortAversion);
				result.waitingAversion = resolve(agent.getEffectiveWaitingAversion(), &RoutingProfileProvenance::waitingAversion);
				result.crowdAversion = resolve(agent.getEffectiveCrowdAversion(), &RoutingProfileProvenance::crowdAversion);
				result.riskAversion = resolve(agent.getEffectiveRiskAversion(), &RoutingProfileProvenance::riskAversion);
				result.routeFamiliarity = resolve(agent.getEffectiveRouteFamiliarity(), &RoutingProfileProvenance::routeFamiliarity);
				result.routePersistence = resolve(agent.getEffectiveRoutePersistence(), &RoutingProfileProvenance::routePersistence);
				return result;
			}

			RouteDecisionContext currentDecisionContext(Agent const& agent,
				Graph const& graph, Vertex const* target, bool allowFallback)
			{
				auto const profile = effectiveProfile(agent, graph.getRouteChoicePolicy());
				auto const* world = graph.getWorld();
				auto const worldSeed = world ? world->getRandomSeed() : uint64_t{ 0 };
				auto const agentId = world ? world->getAgentId(&agent).value : uint64_t{ 0 };
				auto const journeyIdentity = agent.getRouteJourneyIdentity(target);
				return { &agent, profile, graph.getRouteChoicePolicy(), agent.getSector(),
					CORE_AGENT_BASE_WALK_SPEED * profile.walkSpeedModifier, world,
					CORE_AGENT_BASE_CLIMB_SPEED * profile.ladderSpeedModifier, allowFallback,
					worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
						^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL), 0 };
			}
		}

		std::shared_ptr<Path> findPath(Agent const* agent, Graph const* graph,
			node_type source, node_type target)
		{
			if (!graph || (!agent && !source)) return nullptr;
			if (graph->getWorld() && target)
				if (auto marker = std::dynamic_pointer_cast<Marker>(target->getObject()))
					if (auto occupant = graph->getWorld()->usablePointOccupant(marker->getId());
						occupant && (!agent || occupant != graph->getWorld()->getAgentId(agent)))
						return nullptr;
			if (agent && graph->getWorld() && target)
			{
				auto sector = target->getSector();
				if (sector && !graph->getWorld()->canAgentAccessLocation(*sector, *agent)) return nullptr;
			}
			auto profile = graph->getRouteChoicePolicy().baselineProfile;
			// Resolve Agent-authored preferences once for this immutable search
			// context. A null-Agent editor preview deliberately keeps the explicit
			// policy baseline instead of manufacturing and dereferencing an Agent.
			RoutingProfileProvenance profileSources;
			if (agent) profile = effectiveProfile(*agent, graph->getRouteChoicePolicy(), &profileSources);
			auto const mobility = agent ? agent->getEffectiveMobilityProfile() : EffectiveAgentMobilityProfile{};
			auto const worldSeed = graph->getWorld() ? graph->getWorld()->getRandomSeed() : uint64_t{ 0 };
			auto const agentId = agent && graph->getWorld()
				? graph->getWorld()->getAgentId(agent).value : uint64_t{ 0 };
			auto const journeyIdentity = agent
				? agent->getRouteJourneyIdentity(target.get()) : uint64_t{ 0 };
			auto const perceptionKey = worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
				^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL);
			RouteDecisionContext const context{ agent, profile,
				graph->getRouteChoicePolicy(), agent ? agent->getSector() : nullptr,
				CORE_AGENT_BASE_WALK_SPEED * (agent ? profile.walkSpeedModifier : 1.0f),
				graph->getWorld(),
				CORE_AGENT_BASE_CLIMB_SPEED * (agent ? profile.ladderSpeedModifier : 1.0f),
				false, perceptionKey, 0,
				mobility.value };
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
			workspace.beginRouteDecision(*graph, context);
			auto runSearch = [&](RouteDecisionContext const& searchContext)
			{
				auto const& vertices = graph->getVertices();
				workspace.prepareTargetLowerBounds(targetSlot);
				workspace.beginSearch(vertices.size());
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
					workspace.selectedRouteCosts[slot].reset();
					workspace.put(slot, workspace.routePriority(slot, approachSeconds));
				};
				auto const floorSource = inferredSource && dynamic_cast<Location const*>(agent->getSector());
				if (floorSource)
				{
					auto const started = std::chrono::steady_clock::now();
					auto const position = agent->getGlobalPosition();
					seed(source, position.distanceTo(source->getPosition()) / searchContext.walkSpeed);
					// A virtual source splits the ordinary floor edge beneath the Agent.
					// Seed both endpoints with actual approach time, not the full edge
					// length from an arbitrarily chosen nearest vertex. Never split a
					// Gap, Force Bridge, threshold, or transit edge to bypass admission.
					graph->findContainingFloorIntervals(agent->getSector(), position);
					for (auto const* interval : graph->mContainingIntervals)
					{
						auto const& arc = *interval;
						auto const pa = vertices[arc.sourceSlot]->getPosition();
						auto const pb = vertices[arc.targetSlot]->getPosition();
						if (std::abs(pa.y - position.y) > 0.001f || std::abs(pb.y - position.y) > 0.001f
							|| position.x <= std::min(pa.x, pb.x) || position.x >= std::max(pa.x, pb.x)) continue;
						seed(vertices[arc.sourceSlot], position.distanceTo(pa) / searchContext.walkSpeed);
						seed(vertices[arc.targetSlot], position.distanceTo(pb) / searchContext.walkSpeed);
					}
					graph->mSourceIndexStatistics.seedingSeconds += std::chrono::duration<double>(
						std::chrono::steady_clock::now() - started).count();
				}
				else seed(source, 0.0f);

				std::shared_ptr<Path> depthPath;
				if (workspace.hasLocalDepthRoutes())
					depthPath = findDepthContinuousPath(graph, targetSlot, workspace,
						searchContext, sourceSlot, floorSource);
				while (!workspace.hasLocalDepthRoutes() && !workspace.frontierEmpty())
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

					++workspace.work.expandedVertices;
					auto const scoringStarted = std::chrono::steady_clock::now();
					for (auto routeCostIndex = workspace.routeOffsets[currentSlot];
						routeCostIndex < workspace.routeOffsets[currentSlot + 1]; ++routeCostIndex)
					{
						auto const& arc = workspace.directedArcs[routeCostIndex];
						auto const& edge = *arc.edge;
						auto const nextSlot = arc.targetSlot;
						auto const& cost = workspace.evaluateArc(routeCostIndex);
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
							workspace.selectedRouteCosts[nextSlot] = routeCostIndex;
							workspace.put(nextSlot, workspace.routePriority(nextSlot, newCost));
						}
					}
					graph->mSourceIndexStatistics.arcScoringSeconds += std::chrono::duration<double>(
						std::chrono::steady_clock::now() - scoringStarted).count();
				}

				auto path = workspace.hasLocalDepthRoutes() ? depthPath : reconstructPath(graph, targetSlot, workspace);
				if (path)
				{
					path->diagnosticContext = RouteDiagnosticContext{
						searchContext.profile, profileSources, mobility.value, provenance(mobility),
						graph->getWorld() ? graph->getWorld()->getTopologyGeneration() : 0,
						searchContext.allowFallbackMobility };
				}
				return path;
			};

			if (auto path = runSearch(context)) return path;
			RouteDecisionContext const fallbackContext{ agent, profile,
				context.policy, context.observationSector, context.walkSpeed,
				context.world, context.climbSpeed,
				true, perceptionKey, 0, context.mobilityProfile, context.localDepth };
			workspace.allowFallbackMobility();
			return runSearch(fallbackContext);
		}

		std::optional<std::pair<float, float>> comparePathSuffixCosts(
			Agent const& agent, Graph const& graph,
			Path const& current, uint32_t currentFromNode,
			Path const& alternative, uint32_t alternativeFromNode)
		{
			if (current.nodes.empty() || alternative.nodes.empty()
				|| currentFromNode >= current.nodes.size()
				|| alternativeFromNode >= alternative.nodes.size()) return std::nullopt;

			auto const profile = effectiveProfile(agent, graph.getRouteChoicePolicy());
			auto const* world = graph.getWorld();
			auto const worldSeed = world ? world->getRandomSeed() : uint64_t{ 0 };
			auto const agentId = world ? world->getAgentId(&agent).value : uint64_t{ 0 };
			auto const target = alternative.nodes.back().targetVertex.get();
			auto const journeyIdentity = agent.getRouteJourneyIdentity(target);
			auto const perceptionKey = worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
				^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL);
			RouteDecisionContext const context{ &agent, profile, graph.getRouteChoicePolicy(),
				agent.getSector(), CORE_AGENT_BASE_WALK_SPEED * profile.walkSpeedModifier,
				world, CORE_AGENT_BASE_CLIMB_SPEED * profile.ladderSpeedModifier,
				true, perceptionKey, 0 };

			auto& workspace = graph.getPathfindingWorkspace();
			workspace.beginRouteDecision(graph, context);
			auto score = [&](Path const& path, uint32_t fromNode) -> std::optional<float>
			{
				float total = 0.0f;
				for (uint32_t i = fromNode + 1; i < path.nodes.size(); ++i)
				{
					auto const& source = path.nodes[i - 1].targetVertex;
					auto const& edge = path.nodes[i].edge;
					if (i > fromNode + 1 && blocksPathing(source)) return std::nullopt;
					uint32_t slot;
					if (!edge || !graphSlot(&graph, source, slot)) return std::nullopt;
					auto const& edges = source->getEdges();
					auto found = std::find(edges.begin(), edges.end(), edge);
					if (found == edges.end()) return std::nullopt;
					auto const index = workspace.routeOffsets[source->getSearchIndex()]
						+ static_cast<size_t>(found - edges.begin());
					if (index >= workspace.routeCosts.size()) return std::nullopt;
					auto const& cost = workspace.evaluateArc(index);
					if (!cost) return std::nullopt;
					total += cost->perceivedCost;
					if (!std::isfinite(total)) return std::nullopt;
				}
				return total;
			};
			auto const currentCost = score(current, currentFromNode);
			auto const alternativeCost = score(alternative, alternativeFromNode);
			if (!currentCost || !alternativeCost) return std::nullopt;
			return std::pair{ *currentCost, *alternativeCost };
		}

		std::optional<PathRouteDiagnostics> getRouteDiagnostics(Path const& path)
		{
			if (!path.diagnosticContext || path.nodes.empty()) return std::nullopt;
			PathRouteDiagnostics result;
			result.context = *path.diagnosticContext;
			for (auto const& node : path.nodes)
			{
				if (!node.diagnosticCost) return std::nullopt;
				auto const& c = node.diagnosticCost->components;
				result.components.movement += c.movement;
				result.components.knownWait += c.knownWait;
				result.components.expectedWait += c.expectedWait;
				result.components.effort += c.effort;
				result.components.interaction += c.interaction;
				result.components.crowding += c.crowding;
				result.components.risk += c.risk;
				result.components.uncertainty += c.uncertainty;
				result.components.stableVariation += c.stableVariation;
			}
			result.perceivedCost = path.nodes.back().getCumulativePerceivedCost();
			result.objectiveEstimatedDurationSeconds
				= path.nodes.back().objectiveDurationSeconds;
			return result;
		}

		bool routeDiagnosticContextIsStale(Agent const& agent,
			Graph const& graph, RouteDiagnosticContext const& captured)
		{
			if (graph.getWorld()
				&& captured.topologyGeneration != graph.getWorld()->getTopologyGeneration()) return true;
			RoutingProfileProvenance sources;
			auto const current = effectiveProfile(agent, graph.getRouteChoicePolicy(), &sources);
			auto const sameProfile = current.walkSpeedModifier == captured.profile.walkSpeedModifier
				&& current.stairSpeedModifier == captured.profile.stairSpeedModifier
				&& current.ladderSpeedModifier == captured.profile.ladderSpeedModifier
				&& current.escalatorWalkingChance == captured.profile.escalatorWalkingChance
				&& current.waitingAversion == captured.profile.waitingAversion
				&& current.effortAversion == captured.profile.effortAversion
				&& current.interactionAversion == captured.profile.interactionAversion
				&& current.crowdAversion == captured.profile.crowdAversion
				&& current.riskAversion == captured.profile.riskAversion
				&& current.routeFamiliarity == captured.profile.routeFamiliarity
				&& current.routePersistence == captured.profile.routePersistence;
			auto const mobility = agent.getEffectiveMobilityProfile();
			return !sameProfile || sources != captured.provenance
				|| mobility.value != captured.mobilityProfile
				|| provenance(mobility) != captured.mobilityProvenance;
		}

		bool routeDiagnosticContextIsStale(Agent const& agent,
			Graph const& graph, Path const& path)
		{
			return !path.diagnosticContext
				|| routeDiagnosticContextIsStale(agent, graph, *path.diagnosticContext);
		}

		char const* routeExclusionReasonText(RouteExclusionReason reason)
		{
			switch (reason)
			{
			case RouteExclusionReason::Mobility: return "excluded by the Agent's Mobility profile";
			case RouteExclusionReason::Clearance: return "excluded by Standing Agent doorway clearance";
			case RouteExclusionReason::Direction: return "excluded by traversal direction";
			case RouteExclusionReason::Control: return "excluded because no usable control can open or prepare it";
			case RouteExclusionReason::PreparationSide: return "excluded because the resource cannot be prepared from this side";
			case RouteExclusionReason::Permission: return "excluded because traversal permission is unavailable";
			case RouteExclusionReason::NoContinuation: return "excluded because it has no feasible continuation to the target";
			case RouteExclusionReason::AnalysisLimit: return "not compared because the bounded analysis limit was reached";
			case RouteExclusionReason::Unknown: return "excluded by traversal feasibility";
			default: return "";
			}
		}

		std::optional<PathRouteExplanation> explainRoute(Agent const& agent,
			Graph const& graph, Path const& path, uint32_t maximumExpandedVertices,
			uint32_t maximumEvaluatedTraversals)
		{
			if (path.nodes.empty() || !path.nodes.back().targetVertex
				|| maximumExpandedVertices == 0 || maximumEvaluatedTraversals == 0)
				return std::nullopt;

			auto const target = path.nodes.back().targetVertex;
			auto const allowFallback = path.diagnosticContext
				&& path.diagnosticContext->allowFallbackMobility;
			auto const context = currentDecisionContext(agent, graph, target.get(), allowFallback);
			uint32_t targetSlot;
			if (!graphSlot(&graph, target, targetSlot)) return std::nullopt;
			auto& workspace = graph.getPathfindingWorkspace();
			workspace.beginRouteDecision(graph, context);
			auto const vertexCount = graph.getVertices().size();
			std::vector<float> costs(vertexCount, std::numeric_limits<float>::infinity());
			std::vector<PerceivedRouteCostComponents> components(vertexCount);
			using QueueItem = std::pair<float, uint32_t>;
			std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
			costs[target->getSearchIndex()] = 0.0f;
			queue.push({ 0.0f, target->getSearchIndex() });
			uint32_t expanded = 0;
			uint32_t evaluated = 0;
			bool truncated = false;

			auto add = [](PerceivedRouteCostComponents left,
				PerceivedRouteCostComponents const& right)
			{
				left.movement += right.movement; left.knownWait += right.knownWait;
				left.expectedWait += right.expectedWait; left.effort += right.effort;
				left.interaction += right.interaction; left.crowding += right.crowding;
				left.risk += right.risk; left.uncertainty += right.uncertainty;
				left.stableVariation += right.stableVariation;
				return left;
			};
			auto evaluate = [&](std::shared_ptr<const Edge> const& edge,
				std::shared_ptr<const Vertex> const& to)
			{
				std::pair<RouteExclusionReason, std::optional<EvaluatedRouteCost>> result;
				if (evaluated >= maximumEvaluatedTraversals)
				{
					truncated = true;
					result.first = RouteExclusionReason::AnalysisLimit;
					return result;
				}
				++evaluated;
				auto const from = edge->getOtherVertex(to);
				uint32_t slot;
				if (!graphSlot(&graph, from, slot))
				{
					result.first = RouteExclusionReason::NoContinuation;
					return result;
				}
				auto const& adjacency = from->getEdges();
				auto const found = std::find(adjacency.begin(), adjacency.end(), edge);
				if (found == adjacency.end()) return result;
				auto const index = workspace.routeOffsets[slot] + static_cast<size_t>(found - adjacency.begin());
				result.second = workspace.evaluateArc(index);
				result.first = workspace.arcExclusion(index);
				return result;
			};

			while (!queue.empty())
			{
				auto const [cost, slot] = queue.top(); queue.pop();
				if (cost != costs[slot]) continue;
				if (++expanded > maximumExpandedVertices) { truncated = true; break; }
				auto const& current = graph.getVertices()[slot];
				if (current.get() != target.get() && blocksPathing(current)) continue;
				++workspace.work.expandedVertices;
				for (auto const& edge : current->getEdges())
				{
					if (evaluated >= maximumEvaluatedTraversals) { truncated = true; break; }
					auto const source = edge->getOtherVertex(current);
					auto const [facts, arc] = evaluate(edge, current);
					if (!arc) continue;
					auto const candidate = cost + arc->perceivedCost;
					if (!std::isfinite(candidate)) throw std::invalid_argument("Route explanation cost is not finite");
					auto const sourceSlot = source->getSearchIndex();
					if (candidate < costs[sourceSlot])
					{
						costs[sourceSlot] = candidate;
						components[sourceSlot] = add(arc->components, components[slot]);
						queue.push({ candidate, sourceSlot });
					}
				}
				if (truncated) break;
			}

			PathRouteExplanation result;
			result.capturedContextStale = routeDiagnosticContextIsStale(agent, graph, path);
			// Alternative evidence is intentionally recomputed now. Only the selected
			// suffix has retained decision-time evidence.
			result.comparisonEvidence = RouteExplanationEvidence::CurrentContext;
			result.analysisTruncated = truncated;
			result.vertices.reserve(path.nodes.size());
			for (uint32_t i = 0; i < path.nodes.size(); ++i)
			{
				auto const& node = path.nodes[i];
				PathVertexExplanation vertex{ i, node.targetVertex, i + 1 == path.nodes.size(), false, {} };
				if (vertex.target || !node.targetVertex)
				{
					result.vertices.push_back(std::move(vertex));
					continue;
				}
				auto const selectedEdge = path.nodes[i + 1].edge;
				auto const incomingEdge = i ? node.edge : nullptr;
				for (auto const& edge : node.targetVertex->getEdges())
				{
					if (edge == incomingEdge && edge != selectedEdge) continue;
					auto next = edge->getOtherVertex(node.targetVertex);
					auto const [facts, arc] = evaluate(edge, next);
					RouteContinuationExplanation continuation;
					continuation.edge = edge;
					continuation.nextVertex = next;
					continuation.selected = edge == selectedEdge;
					continuation.exclusionReason = facts;
					if (arc && std::isfinite(costs[next->getSearchIndex()]))
					{
						continuation.feasible = true;
						continuation.perceivedContinuationCost = arc->perceivedCost
							+ costs[next->getSearchIndex()];
						continuation.components = add(arc->components,
							components[next->getSearchIndex()]);
					}
					else if (arc)
						continuation.exclusionReason = truncated
							? RouteExclusionReason::AnalysisLimit : RouteExclusionReason::NoContinuation;
					vertex.continuations.push_back(std::move(continuation));
					if (evaluated >= maximumEvaluatedTraversals) { truncated = true; break; }
				}
				// The selected suffix is retained decision-time evidence. Alternatives are
				// recomputed only on demand; when the context is stale the UI labels that
				// comparison as current rather than rewriting this history.
				for (auto& continuation : vertex.continuations)
				{
					if (!continuation.selected) continue;
					PerceivedRouteCostComponents suffix;
					bool available = true;
					for (uint32_t n = i + 1; n < path.nodes.size(); ++n)
					{
						if (!path.nodes[n].diagnosticCost) { available = false; break; }
						suffix = add(suffix, path.nodes[n].diagnosticCost->components);
					}
					if (available)
					{
						continuation.capturedComponents = suffix;
						continuation.capturedPerceivedContinuationCost = suffix.total();
					}
				}
				std::sort(vertex.continuations.begin(), vertex.continuations.end(),
					[](auto const& left, auto const& right)
					{
						if (left.selected != right.selected) return left.selected;
						if (left.feasible != right.feasible) return left.feasible;
						if (left.feasible && left.perceivedContinuationCost != right.perceivedContinuationCost)
							return left.perceivedContinuationCost < right.perceivedContinuationCost;
						if (left.nextVertex->getId() != right.nextVertex->getId())
							return left.nextVertex->getId() < right.nextVertex->getId();
						return left.edge->getId() < right.edge->getId();
					});
				vertex.meaningfulDecision = std::any_of(vertex.continuations.begin(),
					vertex.continuations.end(), [](auto const& item)
					{ return !item.selected && item.feasible; });
				result.vertices.push_back(std::move(vertex));
			}
			result.analysisTruncated = truncated;
			return result;
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
