#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/RouteCost.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	std::filesystem::path testWorld(char const* name)
	{
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
			/ "resources" / "test-worlds" / name;
	}

	void enclosedLiftSeparatesAccessFromRide()
	{
		auto world = core::loadWorldDocument(testWorld("lift-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext remote{ agent, policy.baselineProfile, policy,
			nullptr, agent->getWalkSpeed() };

		bool bodyChecked = false;
		float capacityOneWait = -1.0f;
		float capacityTwoWait = -1.0f;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() == core::EdgeType::Lift)
			{
				auto facts = edge->getDirectedTraversalFacts(edge->getVertex(1), remote);
				require(facts.feasible
					&& std::abs(facts.components.motionSeconds
						- edge->getLength() / CORE_LIFT_SPEED) < 0.0001f,
					"Lift ride duration ignored stop distance or configured speed");
				require(facts.components.interactionUnits == 0.0f,
					"Lift body repeated boarding or alighting interaction");
				bodyChecked = true;
				continue;
			}
			if (edge->getType() != core::EdgeType::Door) continue;
			for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex)
			{
				auto target = edge->getVertex(targetIndex);
				auto source = edge->getOtherVertex(target);
				if (!source || !core::isLocationLike(source->getSector()->getType())) continue;
				auto access = agent->observeLiftAccess(edge->getTraversalResourceId(),
					source->getPosition(), false);
				if (!access) continue;
				auto facts = edge->getDirectedTraversalFacts(target, remote);
				require(facts.components.interactionUnits
					>= policy.liftCallBoardingInteraction,
					"Lift admission omitted call and boarding interaction");
				require(facts.components.expectedWaitSeconds >= policy.liftExpectedWaitSeconds,
					"Unobserved Lift admission inspected live scheduler state");
				if (access->capacity == 1) capacityOneWait = facts.components.expectedWaitSeconds;
				if (access->capacity == 2) capacityTwoWait = facts.components.expectedWaitSeconds;
			}
		}
		require(bodyChecked, "Lift fixture has no ride edge");
		require(capacityOneWait > capacityTwoWait && capacityTwoWait >= 0.0f,
			"Lift capacity did not increase expected admission waiting");
	}

	void platformLiftUsesSlowerFiniteService()
	{
		auto world = core::loadWorldDocument(testWorld("platformlift-test-1.world.yaml"));
		auto graph = world->getGraph();
		auto agent = world->lookupAgent(core::AgentId{ 19 }).entity;
		auto const policy = world->getRouteChoicePolicy();
		core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
			nullptr, agent->getWalkSpeed() };
		bool bodyChecked = false;
		bool admissionChecked = false;
		for (auto const& edge : graph->getEdges())
		{
			if (edge->getType() == core::EdgeType::Lift)
			{
				auto access = agent->observeLiftAccess(edge->getTraversalResourceId(),
					edge->getVertex(0)->getPosition(), false);
				if (!access) continue;
				auto facts = edge->getDirectedTraversalFacts(edge->getVertex(1), context);
				require(facts.feasible && std::isfinite(facts.components.motionSeconds)
					&& std::abs(facts.components.motionSeconds
						- edge->getLength() / CORE_PLATFORM_LIFT_SPEED) < 0.0001f,
					"Platform lift route estimate did not use its slower configured speed");
				bodyChecked = true;
			}
			else if (edge->getType() == core::EdgeType::LiftMount)
			{
				auto target = edge->getVertex(1);
				if (target->getObject()) target = edge->getVertex(0);
				auto facts = edge->getDirectedTraversalFacts(target, context);
				if (facts.components.interactionUnits >= policy.platformLiftInconvenience)
					admissionChecked = true;
			}
		}
		require(bodyChecked, "Platform lift fixture has no finite ride edge");
		require(admissionChecked,
			"Platform lift admission omitted its ordinary baseline inconvenience");
	}
}

void runLiftRouteCostSmokeChecks()
{
	enclosedLiftSeparatesAccessFromRide();
	platformLiftUsesSlowerFiniteService();
}
