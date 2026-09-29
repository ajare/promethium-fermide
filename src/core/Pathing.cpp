#include <algorithm>
#include <cmath>
#include <limits>
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
					std::optional<EvaluatedRouteCost> diagnostic;
					if (workspace.selectedRouteCosts[slot])
						diagnostic = workspace.routeCosts[*workspace.selectedRouteCosts[slot]];
					nodes.push_back({ workspace.edges[slot], vertices[slot], workspace.scores[slot],
						workspace.durations[slot], diagnostic });
					slot = workspace.cameFrom[slot];
				}
				// The inferred-source approach is part of both perceived movement and
				// objective estimated duration, even though it has no Graph Edge.
				EvaluatedRouteCost sourceCost{ workspace.scores[slot], workspace.durations[slot] };
				sourceCost.components.movement = workspace.scores[slot];
				nodes.push_back({ nullptr, vertices[slot], workspace.scores[slot],
					workspace.durations[slot], sourceCost });
				std::reverse(nodes.begin(), nodes.end());
				return std::make_shared<Path>(Path{ std::move(nodes) });
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
				RouteChoicePolicy const& policy)
			{
				auto result = policy.baselineProfile;
				result.walkSpeedModifier = agent.getEffectiveWalkSpeedModifier().value;
				result.stairSpeedModifier = agent.getEffectiveStairSpeedModifier().value;
				result.ladderSpeedModifier = agent.getEffectiveLadderSpeedModifier().value;
				result.escalatorWalkingChance = agent.getEffectiveEscalatorWalkingChance().value;
				result.interactionAversion = agent.getEffectiveInteractionAversion().value;
				result.effortAversion = agent.getEffectiveEffortAversion().value;
				result.waitingAversion = agent.getEffectiveWaitingAversion().value;
				result.crowdAversion = agent.getEffectiveCrowdAversion().value;
				result.riskAversion = agent.getEffectiveRiskAversion().value;
				result.routeFamiliarity = agent.getEffectiveRouteFamiliarity().value;
				result.routePersistence = agent.getEffectiveRoutePersistence().value;
				return result;
			}

			RoutingProfileProvenance effectiveProvenance(Agent const& agent)
			{
				return {
					provenance(agent.getEffectiveWalkSpeedModifier()),
					provenance(agent.getEffectiveStairSpeedModifier()),
					provenance(agent.getEffectiveLadderSpeedModifier()),
					provenance(agent.getEffectiveEscalatorWalkingChance()),
					provenance(agent.getEffectiveWaitingAversion()),
					provenance(agent.getEffectiveEffortAversion()),
					provenance(agent.getEffectiveInteractionAversion()),
					provenance(agent.getEffectiveCrowdAversion()),
					provenance(agent.getEffectiveRiskAversion()),
					provenance(agent.getEffectiveRouteFamiliarity()),
					provenance(agent.getEffectiveRoutePersistence())
				};
			}

			RouteDiagnosticContext diagnosticContext(Agent const& agent, Graph const& graph,
				EffectiveRoutingProfile const& profile, bool allowFallback)
			{
				auto const mobility = agent.getEffectiveMobilityProfile();
				return { profile, effectiveProvenance(agent), mobility.value,
					provenance(mobility), graph.getWorld()
						? graph.getWorld()->getTopologyGeneration() : 0,
					allowFallback };
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
					agent.getWalkSpeed(), world, agent.getClimbSpeed(), allowFallback,
					worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
						^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL), 0 };
			}
		}

		std::shared_ptr<Path> findPath(Agent const* agent, Graph const* graph,
			node_type source, node_type target)
		{
			if (!graph || (!agent && !source)) return nullptr;
			auto profile = graph->getRouteChoicePolicy().baselineProfile;
			// Resolve Agent-authored preferences once for this immutable search
			// context. A null-Agent editor preview deliberately keeps the explicit
			// policy baseline instead of manufacturing and dereferencing an Agent.
			if (agent) profile = effectiveProfile(*agent, graph->getRouteChoicePolicy());
			auto const worldSeed = graph->getWorld() ? graph->getWorld()->getRandomSeed() : uint64_t{ 0 };
			auto const agentId = agent && graph->getWorld()
				? graph->getWorld()->getAgentId(agent).value : uint64_t{ 0 };
			auto const journeyIdentity = agent
				? agent->getRouteJourneyIdentity(target.get()) : uint64_t{ 0 };
			auto const perceptionKey = worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
				^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL);
			RouteDecisionContext const context{ agent, profile,
				graph->getRouteChoicePolicy(), agent ? agent->getSector() : nullptr,
				agent ? agent->getWalkSpeed() : static_cast<float>(CORE_AGENT_BASE_WALK_SPEED),
				graph->getWorld(),
				agent ? agent->getClimbSpeed() : static_cast<float>(CORE_AGENT_BASE_CLIMB_SPEED),
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
					workspace.selectedRouteCosts[slot].reset();
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
						auto const routeCostIndex = arcIndex++;
						auto const& cost = workspace.routeCosts[routeCostIndex];
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
							workspace.put(nextSlot, newCost);
						}
					}
				}

				auto path = reconstructPath(graph, targetSlot, workspace);
				if (path)
				{
					if (agent)
						path->diagnosticContext = diagnosticContext(*agent, *graph,
							searchContext.profile, searchContext.allowFallbackMobility);
					else
						path->diagnosticContext = RouteDiagnosticContext{
							searchContext.profile, {}, {}, {},
							graph->getWorld() ? graph->getWorld()->getTopologyGeneration() : 0,
							searchContext.allowFallbackMobility };
				}
				return path;
			};

			if (auto path = runSearch(context)) return path;
			RouteDecisionContext const fallbackContext{ agent, profile,
				graph->getRouteChoicePolicy(), agent ? agent->getSector() : nullptr,
				agent ? agent->getWalkSpeed() : static_cast<float>(CORE_AGENT_BASE_WALK_SPEED),
				graph->getWorld(),
				agent ? agent->getClimbSpeed() : static_cast<float>(CORE_AGENT_BASE_CLIMB_SPEED),
				true, perceptionKey, 0 };
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

			auto profile = graph.getRouteChoicePolicy().baselineProfile;
			profile.stairSpeedModifier = agent.getEffectiveStairSpeedModifier().value;
			profile.escalatorWalkingChance = agent.getEffectiveEscalatorWalkingChance().value;
			profile.interactionAversion = agent.getEffectiveInteractionAversion().value;
			profile.effortAversion = agent.getEffectiveEffortAversion().value;
			profile.waitingAversion = agent.getEffectiveWaitingAversion().value;
			profile.crowdAversion = agent.getEffectiveCrowdAversion().value;
			profile.riskAversion = agent.getEffectiveRiskAversion().value;
			profile.routeFamiliarity = agent.getEffectiveRouteFamiliarity().value;
			profile.routePersistence = agent.getEffectiveRoutePersistence().value;
			auto const* world = graph.getWorld();
			auto const worldSeed = world ? world->getRandomSeed() : uint64_t{ 0 };
			auto const agentId = world ? world->getAgentId(&agent).value : uint64_t{ 0 };
			auto const target = alternative.nodes.back().targetVertex.get();
			auto const journeyIdentity = agent.getRouteJourneyIdentity(target);
			auto const perceptionKey = worldSeed ^ (agentId * 0x9e3779b97f4a7c15ULL)
				^ (journeyIdentity * 0xbf58476d1ce4e5b9ULL);
			RouteDecisionContext const context{ &agent, profile, graph.getRouteChoicePolicy(),
				agent.getSector(), agent.getWalkSpeed(), world, agent.getClimbSpeed(),
				true, perceptionKey, 0 };

			auto& workspace = graph.getPathfindingWorkspace();
			workspace.captureRouteCosts(graph, context);
			auto score = [&](Path const& path, uint32_t fromNode) -> std::optional<float>
			{
				float total = 0.0f;
				for (uint32_t i = fromNode + 1; i < path.nodes.size(); ++i)
				{
					auto const& source = path.nodes[i - 1].targetVertex;
					auto const& edge = path.nodes[i].edge;
					if (!source || !edge) return std::nullopt;
					auto const& edges = source->getEdges();
					auto found = std::find(edges.begin(), edges.end(), edge);
					if (found == edges.end()) return std::nullopt;
					auto const index = workspace.routeOffsets[source->getSearchIndex()]
						+ static_cast<size_t>(found - edges.begin());
					if (index >= workspace.routeCosts.size() || !workspace.routeCosts[index])
						return std::nullopt;
					total += workspace.routeCosts[index]->perceivedCost;
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
			auto const current = effectiveProfile(agent, graph.getRouteChoicePolicy());
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
			return !sameProfile || effectiveProvenance(agent) != captured.provenance
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
			auto mobilityReason = [&](Edge const& edge)
			{
				auto rejected = [&](TraversalKind kind)
				{
					auto const use = agentEdgeUse(&agent, edge, kind);
					return use == MobilityUse::CannotUse || (!allowFallback
						&& use == MobilityUse::OnlyIfNoOtherOption);
				};
				switch (edge.getType())
				{
				case EdgeType::Door: case EdgeType::BulkheadDoor: return rejected(TraversalKind::Door);
				case EdgeType::Ladder: case EdgeType::LadderMount: return rejected(TraversalKind::Ladder);
				case EdgeType::Lift: case EdgeType::LiftMount:
					return rejected(TraversalKind::Lift) || rejected(TraversalKind::PlatformLift);
				case EdgeType::Shuttle: case EdgeType::ShuttleMount: return rejected(TraversalKind::Shuttle);
				case EdgeType::Stairwell: case EdgeType::StairwellMount: return rejected(TraversalKind::Stairwell);
				case EdgeType::Staircase: case EdgeType::StaircaseMount:
					return rejected(TraversalKind::Staircase) || rejected(TraversalKind::Escalator);
				default: return false;
				}
			};
			auto evaluate = [&](std::shared_ptr<const Edge> const& edge,
				std::shared_ptr<const Vertex> const& to)
			{
				auto facts = edge->getDirectedTraversalFacts(to, context);
				if (facts.feasible && facts.components.uncertaintyUnits > 0)
				{
					auto mix = [](uint64_t value)
					{
						value += 0x9e3779b97f4a7c15ULL;
						value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
						value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
						return value ^ (value >> 31);
					};
					auto const hash = mix(context.perceptionKey
						^ (uint64_t{ edge->getId() } << 32) ^ to->getId()
						^ mix(context.observationEpoch));
					auto const unit = static_cast<float>(hash >> 40) / 16777215.0f;
					auto const amplitude = (1.0f - context.profile.routeFamiliarity)
						* facts.components.uncertaintyUnits;
					facts.components.perceptionVariationUnits = (unit * 2.0f - 1.0f) * amplitude;
				}
				if (!facts.feasible && facts.exclusionReason == RouteExclusionReason::None)
				{
					if (mobilityReason(*edge)) facts.exclusionReason = RouteExclusionReason::Mobility;
					else if (edge->getType() == EdgeType::Gap || edge->getType() == EdgeType::Window)
						facts.exclusionReason = RouteExclusionReason::Permission;
					else facts.exclusionReason = RouteExclusionReason::Unknown;
				}
				return std::pair{ facts, context.policy.evaluate(facts, context.profile) };
			};

			while (!queue.empty())
			{
				auto const [cost, slot] = queue.top(); queue.pop();
				if (cost != costs[slot]) continue;
				if (++expanded > maximumExpandedVertices) { truncated = true; break; }
				auto const& current = graph.getVertices()[slot];
				if (current.get() != target.get() && blocksPathing(current)) continue;
				for (auto const& edge : current->getEdges())
				{
					if (++evaluated > maximumEvaluatedTraversals) { truncated = true; break; }
					auto const source = edge->getOtherVertex(current);
					auto const [facts, arc] = evaluate(edge, current);
					if (!arc) continue;
					auto const candidate = cost + arc->perceivedCost;
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
				PathVertexExplanation vertex{ i, node.targetVertex, i + 1 == path.nodes.size() };
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
					continuation.exclusionReason = facts.exclusionReason;
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
