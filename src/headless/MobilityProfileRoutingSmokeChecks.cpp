#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/MobilityProfile.h"
#include "core/World.h"

void runMobilityProfileRoutingSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class ClassifiedEdge final : public core::Edge
	{
		bool mRequiresButton;
	public:
		explicit ClassifiedEdge(bool requiresButton)
			: Edge(core::EdgeType::Location), mRequiresButton(requiresButton) {}
		std::string getDescription() const override { return "classified test edge"; }
		std::shared_ptr<core::Edge> copyWithoutVertices() override
		{
			return std::make_shared<ClassifiedEdge>(mRequiresButton);
		}
		bool isTraversable(std::shared_ptr<const core::Vertex>,
			std::shared_ptr<const core::Agent>) const override { return true; }
		core::EdgeTraversalRequestResult requestTraversal(
			std::shared_ptr<const core::Vertex>, std::shared_ptr<const core::Agent>) const override
		{
			return core::EdgeTraversalRequestResult::OK;
		}
		core::DirectedTraversalFacts getDirectedTraversalFacts(
			std::shared_ptr<const core::Vertex>, core::RouteDecisionContext const&) const override
		{
			core::DirectedTraversalFacts facts;
			facts.feasible = true;
			facts.components.motionSeconds = CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
			facts.objectiveDurationSeconds = facts.components.motionSeconds;
			facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
			return facts;
		}
		bool requiresButton() const override { return mRequiresButton; }
	};
}

void runMobilityProfileRoutingSmokeChecks()
{
	auto registry = core::AgentTagRegistry::create();
	auto const tag = registry->addAgentTag("restricted");
	std::string diagnostic;
	require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
	core::MobilityProfile profile;
	profile.set(core::TraversalKind::Staircase, core::MobilityUse::CannotUse);
	profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
	profile.set(core::TraversalKind::Ladder, core::MobilityUse::OnlyIfNoOtherOption);
	require(registry->setAgentTagMobilityProfile(tag, profile, &diagnostic), diagnostic);

	auto world = std::make_shared<core::World>("Routing", 4, 2);
	world->attachAgentTagRegistry("routing.tags.yaml", registry);
	auto const corridor = world->addCorridor(0, 0, 3);
	world->finishBuild();
	auto const id = world->createAgent("Restricted", corridor, 0, 1.5f);
	world->pauseSimulation();
	require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
	auto const lookup = world->lookupAgent(id);
	require(static_cast<bool>(lookup), "The Mobility routing Agent was not created");
	auto const* agent = lookup.entity;

	ClassifiedEdge ordinary(false);
	ClassifiedEdge buttonOperated(true);
	require(core::agentForbidsEdge(agent, ordinary, core::TraversalKind::Staircase),
		"An explicitly forbidden traversal kind remained routable");
	require(!core::agentForbidsEdge(agent, ordinary, core::TraversalKind::Ladder),
		"An unrelated traversal kind was forbidden");
	require(core::agentForbidsEdge(agent, buttonOperated, core::TraversalKind::Ladder),
		"Buttons did not forbid an edge requiring an interaction point");
	require(!core::agentForbidsEdge(agent, ordinary, core::TraversalKind::Ladder)
		&& core::agentRejectsEdge(agent, ordinary, core::TraversalKind::Ladder, false)
		&& !core::agentRejectsEdge(agent, ordinary, core::TraversalKind::Ladder, true),
		"A last-resort traversal was not excluded only from the first routing pass");
	require(!core::agentForbidsEdge(nullptr, buttonOperated, core::TraversalKind::Staircase),
		"A route without an Agent unexpectedly acquired Mobility restrictions");
}
