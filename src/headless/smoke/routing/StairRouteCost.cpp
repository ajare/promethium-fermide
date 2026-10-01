#include "Checks.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Marker.h"
#include "core/Path.h"
#include "core/RouteCost.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}


	void stationaryStaircaseIsDirectionalAndPhysical(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/staircase-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto configured = world->getRouteChoicePolicy();
		configured.stairAscentSpeed = 0.25f;
		configured.stairDescentSpeed = 0.5f;
		world->setRouteChoicePolicy(configured);
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
			agent->getSector(), agent->getWalkSpeed() };
		auto const ladderSpeed = agent->getClimbSpeed();
		bool checked = false;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() == core::EdgeType::StaircaseMount)
			{
				auto facts = edge->getDirectedTraversalFacts(edge->getVertex(1), context);
				require(facts.feasible && facts.components.interactionUnits == 0.0f
					&& facts.objectiveDurationSeconds == 0.0f,
					"A topology-only Staircase mount acquired a per-Vertex charge");
				continue;
			}
			if (edge->getType() != core::EdgeType::Staircase
				|| edge->getTraversalSpeed(nullptr) > 0.0f) continue;
			auto lower = edge->getVertex(0);
			auto upper = edge->getVertex(1);
			if (lower->getPosition().y > upper->getPosition().y) std::swap(lower, upper);
			auto up = edge->getDirectedTraversalFacts(upper, context);
			auto down = edge->getDirectedTraversalFacts(lower, context);
			auto const length = edge->getLength();
			require(up.feasible && down.feasible
				&& std::abs(up.components.motionSeconds - length / policy.stairAscentSpeed) < 0.0001f
				&& std::abs(down.components.motionSeconds - length / policy.stairDescentSpeed) < 0.0001f,
				"Stationary Staircase duration ignored physical length or direction");
			require(up.components.physicalEffortUnits > down.components.physicalEffortUnits,
				"Staircase ascent did not carry greater baseline effort");
			require(std::abs(up.components.interactionUnits - policy.stairInteractionPerFlight) < 0.0001f
				&& std::abs(down.components.interactionUnits - policy.stairInteractionPerFlight) < 0.0001f,
				"Staircase interaction was not charged once per flight");
			require(std::abs(edge->getTraversalSpeed(agent, upper) - policy.stairAscentSpeed) < 0.0001f
				&& std::abs(edge->getTraversalSpeed(agent, lower) - policy.stairDescentSpeed) < 0.0001f,
				"Estimated and runtime stationary Staircase speeds disagree");
			world->pauseSimulation();
			std::string diagnostic;
			require(world->setAgentIndividualStairSpeedModifier(core::AgentId{ 1 }, 0.5f,
				&diagnostic), diagnostic.c_str());
			auto slowProfile = policy.baselineProfile;
			slowProfile.stairSpeedModifier = agent->getEffectiveStairSpeedModifier().value;
			core::RouteDecisionContext slowContext{ agent, slowProfile, policy,
				agent->getSector(), agent->getWalkSpeed() };
			auto slowUp = edge->getDirectedTraversalFacts(upper, slowContext);
			require(std::abs(slowUp.components.motionSeconds
				- up.components.motionSeconds / 0.5f) < 0.0001f
				&& std::abs(edge->getTraversalSpeed(agent, upper)
					- policy.stairAscentSpeed * 0.5f) < 0.0001f,
				"Stair speed modifier changed estimated and runtime speed differently");
			checked = true;
			break;
		}
		require(checked, "Staircase fixture has no stationary flight");
		require(agent->getClimbSpeed() == ladderSpeed,
			"Stair speed modifier changed Ladder climb speed");
		bool escalatorChecked = false;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() != core::EdgeType::Staircase
				|| edge->getTraversalSpeed(nullptr) <= 0.0f) continue;
			require(std::abs(edge->getTraversalSpeed(agent) - edge->getTraversalSpeed(nullptr)) < 0.0001f,
				"Stair speed modifier changed moving Escalator belt speed");
			escalatorChecked = true;
			break;
		}
		require(escalatorChecked, "Staircase fixture has no moving Escalator");
	}

	std::shared_ptr<const core::Vertex> markerVertex(
		std::shared_ptr<const core::Graph> const& graph, uint64_t markerId)
	{
		for (auto const& vertex : graph->getVertices())
		{
			auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
			if (marker && marker->getId().value == markerId) return vertex;
		}
		return {};
	}

	void stairSpeedCanReverseRouteChoice(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/staircase-test-1.world.yaml"));
		world->pauseSimulation();
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		std::vector<std::shared_ptr<const core::Vertex>> markers;
		for (auto const& vertex : graph->getVertices())
			if (std::dynamic_pointer_cast<core::Marker>(vertex->getObject())) markers.push_back(vertex);
		std::string diagnostic;
		require(world->setAgentIndividualStairSpeedModifier(core::AgentId{ 1 }, 0.5f,
			&diagnostic), diagnostic.c_str());
		std::vector<std::shared_ptr<core::Path>> slow;
		for (auto const& source : markers)
			for (auto const& target : markers)
				slow.push_back(graph->calculatePath(agent, source, target));
		require(world->setAgentIndividualStairSpeedModifier(core::AgentId{ 1 }, 1.5f,
			&diagnostic), diagnostic.c_str());
		bool reversed = false;
		size_t index = 0;
		for (auto const& source : markers)
			for (auto const& target : markers)
			{
				auto fast = graph->calculatePath(agent, source, target);
				auto const& before = slow[index++];
				if (!before || !fast || before->nodes.size() != fast->nodes.size())
				{
					if (bool(before) != bool(fast) || (before && fast)) reversed = true;
					continue;
				}
				for (size_t node = 1; node < fast->nodes.size(); ++node)
					if (before->nodes[node].edge->getId() != fast->nodes[node].edge->getId())
					{ reversed = true; break; }
			}
		require(reversed, "Stair speed modifier did not reverse any competing route choice");
	}

	void effortAversionCanReverseRouteChoiceWithoutChangingSpeed(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/staircase-test-1.world.yaml"));
		world->pauseSimulation();
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		std::vector<std::shared_ptr<const core::Vertex>> markers;
		for (auto const& vertex : graph->getVertices())
			if (std::dynamic_pointer_cast<core::Marker>(vertex->getObject())) markers.push_back(vertex);

		float stairSpeed = 0.0f;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() != core::EdgeType::Staircase
				|| edge->getTraversalSpeed(nullptr) > 0.0f) continue;
			stairSpeed = edge->getTraversalSpeed(agent, edge->getVertex(1));
			break;
		}
		require(stairSpeed > 0.0f, "Effort-aversion fixture has no stationary Staircase");

		std::string diagnostic;
		require(world->setAgentIndividualEffortAversion(core::AgentId{ 1 }, 0.0f,
			&diagnostic), diagnostic.c_str());
		std::vector<std::shared_ptr<core::Path>> lowAversion;
		for (auto const& source : markers)
			for (auto const& target : markers)
				lowAversion.push_back(graph->calculatePath(agent, source, target));
		require(world->setAgentIndividualEffortAversion(core::AgentId{ 1 }, 3.0f,
			&diagnostic), diagnostic.c_str());

		bool reversed = false;
		size_t index = 0;
		for (auto const& source : markers)
			for (auto const& target : markers)
			{
				auto highAversion = graph->calculatePath(agent, source, target);
				auto const& low = lowAversion[index++];
				if (!low || !highAversion) continue;
				if (low->nodes.size() != highAversion->nodes.size()) { reversed = true; continue; }
				for (size_t node = 1; node < low->nodes.size(); ++node)
					if (low->nodes[node].edge->getId() != highAversion->nodes[node].edge->getId())
					{ reversed = true; break; }
			}
		require(reversed, "Effort aversion did not reverse any stairs-versus-alternative route choice");

		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() != core::EdgeType::Staircase
				|| edge->getTraversalSpeed(nullptr) > 0.0f) continue;
			require(std::abs(edge->getTraversalSpeed(agent, edge->getVertex(1)) - stairSpeed) < 0.0001f,
				"Effort aversion changed physical stair speed");
			break;
		}
	}

	void multiFlightStairwellAccumulatesEveryFlight(smoke::Context const& smokeContext)
	{
		auto world = core::loadWorldDocument(smokeContext.fixture("resources/test-worlds/stairwell-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
			agent->getSector(), agent->getWalkSpeed() };
		auto const level0 = markerVertex(graph, 1);
		auto const level2 = markerVertex(graph, 5);
		require(level0 && level2, "Stairwell fixture is missing endpoint Markers");
		auto upPath = graph->calculatePath(agent, level0, level2);
		auto downPath = graph->calculatePath(agent, level2, level0);
		require(upPath && downPath, "Two-level Stairwell route is unreachable");

		auto totals = [&](std::shared_ptr<core::Path> const& path)
		{
			core::RouteCostComponents total;
			float length = 0.0f;
			for (auto const& node : path->nodes)
			{
				if (!node.edge || node.edge->getType() != core::EdgeType::Stairwell) continue;
				auto facts = node.edge->getDirectedTraversalFacts(node.targetVertex, context);
				require(facts.feasible, "Stairwell body edge became infeasible");
				total.motionSeconds += facts.components.motionSeconds;
				total.physicalEffortUnits += facts.components.physicalEffortUnits;
				total.interactionUnits += facts.components.interactionUnits;
				length += node.edge->getLength();
			}
			return std::pair{ total, length };
		};
		auto const [up, upLength] = totals(upPath);
		auto const [down, downLength] = totals(downPath);
		require(upLength > 0.0f && std::abs(upLength - downLength) < 0.0001f,
			"Stairwell route did not traverse the same physical run in reverse");
		require(std::abs(up.motionSeconds - upLength / policy.stairAscentSpeed) < 0.0001f
			&& std::abs(down.motionSeconds - downLength / policy.stairDescentSpeed) < 0.0001f,
			"Multi-flight Stairwell collapsed to one graph minimum cost");
		require(std::abs(up.interactionUnits - 2.0f * policy.stairInteractionPerFlight) < 0.0001f
			&& std::abs(down.interactionUnits - 2.0f * policy.stairInteractionPerFlight) < 0.0001f,
			"Stairwell interaction followed topology Vertices instead of flights");
		require(up.physicalEffortUnits > down.physicalEffortUnits,
			"Stairwell ascent did not carry greater baseline effort");

		world->pauseSimulation();
		core::MobilityProfile mobility;
		mobility.set(core::TraversalKind::Stairwell, core::MobilityUse::OnlyIfNoOtherOption);
		require(world->setAgentIndividualMobilityProfile(core::AgentId{ 1 }, mobility),
			"Could not make Stairwell traversal a last resort");
		require(static_cast<bool>(graph->calculatePath(agent, level0, level2)),
			"A last-resort Stairwell was not used when no other Path existed");
		mobility.set(core::TraversalKind::Stairwell, core::MobilityUse::CannotUse);
		require(world->setAgentIndividualMobilityProfile(core::AgentId{ 1 }, mobility),
			"Could not forbid Stairwell traversal");
		require(!graph->calculatePath(agent, level0, level2),
			"A forbidden Stairwell remained a finite fallback alternative");
	}
}


namespace routing_smoke
{
	void registerStairRouteCost(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "stationaryStaircaseIsDirectionalAndPhysical", [](smoke::Context const& smokeContext) { stationaryStaircaseIsDirectionalAndPhysical(smokeContext); } });
		checks.push_back({ "stairSpeedCanReverseRouteChoice", [](smoke::Context const& smokeContext) { stairSpeedCanReverseRouteChoice(smokeContext); } });
		checks.push_back({ "effortAversionCanReverseRouteChoiceWithoutChangingSpeed", [](smoke::Context const& smokeContext) { effortAversionCanReverseRouteChoiceWithoutChangingSpeed(smokeContext); } });
		checks.push_back({ "multiFlightStairwellAccumulatesEveryFlight", [](smoke::Context const& smokeContext) { multiFlightStairwellAccumulatesEveryFlight(smokeContext); } });
	}
}
