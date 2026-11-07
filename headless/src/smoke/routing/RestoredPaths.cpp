#include "core/AgentType.h"
#include "Checks.h"
// Saved-Path restoration under the effective Mobility profile, for ticket #194.
//
// A World records each Agent's route intent as an authored Path, restored from
// the document on load. A tag-supplied Mobility profile, however, only becomes
// available once the World's Agent tag registry is resolved (ADR 0011), and
// resolution runs after the Agents and their Paths are restored. A restored
// Path can therefore hold an edge the effective profile forbids: the runtime
// traversal gate refuses that edge, but the saved Path is still wrong and the
// Agent is stranded instead of being routed the long way round.
//
// Everything here drives the public World and document APIs. What gets pinned
// down:
//
//   a tag-supplied Ladder restriction makes the authored route take the
//   Staircase, and every edge of the Path restored by loadWorldDocument is
//   traversable for the loaded Agent, with the forbidden Ladder gone
//   the restored Path keeps its intended destination and active flag, so an
//   active route still starts and the Agent reaches the upper Marker by the
//   Staircase, while an inactive route stays parked
//   resetSimulation rebuilds the same permitted route
//   an equivalent individual Mobility profile (available during Agent
//   deserialization, ADR 0012) gives the same permitted restored route

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/MobilityProfile.h"
#include "core/Path.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}


	core::MobilityProfile ladderRestriction()
	{
		core::MobilityProfile profile;
		profile.set(core::TraversalKind::Ladder, core::MobilityUse::CannotUse);
		return profile;
	}

	unsigned untraversableEdgeCount(core::Agent const* agent, core::Path const& path)
	{
		auto profile = core::RouteChoicePolicy{}.baselineProfile;
		core::RouteDecisionContext const context{ agent, profile, {},
			agent ? agent->getSector() : nullptr,
			agent ? agent->getWalkSpeed() : core::bundledHumanBaseline().walkSpeed,
			nullptr, agent ? agent->getClimbSpeed() : core::bundledHumanBaseline().climbSpeed,
			true, 0, 0, agent ? std::optional{ agent->getEffectiveMobilityProfile().value }
				: std::optional<core::MobilityProfile>{} };
		unsigned count = 0;
		for (auto const& node : path.nodes)
		{
			if (node.edge && !node.edge->getDirectedTraversalFacts(
				node.targetVertex, context).feasible) ++count;
		}
		return count;
	}

	unsigned edgeKindCount(core::Path const& path, core::EdgeType type)
	{
		unsigned count = 0;
		for (auto const& node : path.nodes)
			if (node.edge && node.edge->getType() == type) ++count;
		return count;
	}

	// Two Levels joined at the front Layer by both a Ladder and a much longer
	// Staircase, so a Ladder restriction has a real fallback to route through.
	// The Marker on the upper Level is the single authored destination.
	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::World world;
		core::AgentTagId tag;
		core::AgentId id;
		uint32_t lower;
		uint32_t upper;
		uint32_t markerVertex{ 0 };

		explicit Fixture(bool tagSuppliedRestriction)
			: registry(core::AgentTagRegistry::create())
			, world("Restored path Mobility", 20, 2)
		{
			std::string diagnostic;
			if (tagSuppliedRestriction)
			{
				tag = registry->addAgentTag("no-ladders");
				require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
				require(registry->setAgentTagMobilityProfile(tag, ladderRestriction(), &diagnostic),
					diagnostic);
				world.attachAgentTagRegistry("restored.tags.yaml", registry);
			}
			lower = world.addCorridor(0, 0, 20);
			upper = world.addCorridor(1, 0, 20);
			world.addLadder(1, 0, 1, { 2, false, true });
			world.addStaircase(1, 0, 12, { 4, CORE_SIDE_RIGHT, 0.0f });
			world.addSectorMarker(upper, 0, 1.5f, "Upper", &markerVertex);
			world.finishBuild();
			id = world.createAgent("Walker", lower, 0, 1.5f);
			world.pauseSimulation();
			if (tagSuppliedRestriction)
				require(world.assignAgentTag(id, tag, &diagnostic), diagnostic);
			else
				require(world.setAgentIndividualMobilityProfile(id, ladderRestriction(), &diagnostic),
					diagnostic);
		}

		core::Agent* agent() { return world.lookupAgent(id).entity; }

		// Author the route to the upper Marker through the ordinary authored-Path
		// seam, then keep it as the World's reset baseline exactly as the editor does.
		std::shared_ptr<core::Path> authorRoute(bool active)
		{
			auto graph = world.getGraph();
			auto path = graph->calculatePath(agent(), graph->getVertexByIdentifier(markerVertex));
			require(path && path->nodes.size() > 1, "The fixture destination is unreachable");
			agent()->setPath(path, active);
			return path;
		}

		void save(std::filesystem::path const& directory)
		{
			if (world.hasAttachedAgentTagRegistry())
				registry->saveTo((directory / "restored.tags.yaml").string());
			world.saveTo((directory / "restored.world.yaml").string());
		}
	};

	// A referenced registry is resolved after the World YAML. The saved route
	// intent must remain unevaluated in that interval rather than briefly becoming
	// a permissive or stale Path (#221).
	void savedIntentWaitsForRegistryResolution(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(true);
		fixture.authorRoute(true);
		fixture.save(directory);
		auto const worldPath = directory / "restored.world.yaml";

		auto restored = std::make_shared<core::World>("Loading", 1, 1);
		auto serializer = core::YamlSerializer::fromFile(worldPath.string());
		serializer->deserialize();
		core::SerializationWorkData workData;
		require(restored->deserialize(*serializer, workData),
			"The World YAML did not deserialize");
		auto* agent = restored->lookupAgent(fixture.id).entity;
		require(agent && !agent->getPath(),
			"Saved route intent was evaluated before tag-registry resolution");

		core::loadAndAttachAgentTagRegistry(*restored, worldPath);
		auto path = agent->getPath();
		require(path && edgeKindCount(*path, core::EdgeType::Ladder) == 0
			&& edgeKindCount(*path, core::EdgeType::Staircase) > 0,
			"Registry resolution did not evaluate saved intent with the effective profile");
	}

	// The core regression: after load, the restored Path must be the one the
	// effective (here tag-supplied) profile permits.
	void tagSuppliedProfileIsHonouredByTheRestoredPath(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(true);
		auto authored = fixture.authorRoute(true);
		require(edgeKindCount(*authored, core::EdgeType::Ladder) == 0
			&& edgeKindCount(*authored, core::EdgeType::Staircase) > 0
			&& untraversableEdgeCount(fixture.agent(), *authored) == 0,
			"The authored route did not avoid its forbidden Ladder");
		fixture.save(directory);

		auto reopened = core::loadWorldDocument(directory / "restored.world.yaml");
		auto* restored = reopened->lookupAgent(fixture.id).entity;
		require(restored, "The reopened World lost the fixture Agent");
		auto path = restored->getPath();
		require(path && path->nodes.size() > 1, "The reopened Agent lost its Path");
		require(edgeKindCount(*path, core::EdgeType::Ladder) == 0,
			"A restored Path kept the tag-forbidden Ladder");
		require(edgeKindCount(*path, core::EdgeType::Staircase) > 0,
			"The restored Path did not fall back to the Staircase");
		require(untraversableEdgeCount(restored, *path) == 0,
			"A restored Path holds an edge the loaded Agent cannot traverse");
		require(reopened->getLoadWarnings().empty(),
			"A reachable restored destination produced a load warning");
	}

	// An unreachable saved destination is valid route-loss state, not malformed
	// World data. The World opens, leaves the Agent idle, and reports the loss to
	// the editor without pretending initial restoration was a replan.
	void unreachableSavedDestinationLoadsIdleWithWarning(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		core::World world("Unreachable restored path", 8, 1);
		auto const sourceSector = world.addCorridor(0, 0, 8);
		auto const targetSector = world.addCorridor(1, 0, 0, 8, 1);
		uint32_t sourceIdentifier = 0;
		uint32_t targetIdentifier = 0;
		world.addSectorMarker(sourceSector, 0, 1.5f, &sourceIdentifier);
		world.addSectorMarker(targetSector, 0, 6.5f, &targetIdentifier);
		world.finishBuild();
		auto const id = world.createAgent("Stranded", sourceSector, 0, 1.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr,
			world.getGraph()->getVertexByIdentifier(sourceIdentifier), 0.0f, std::nullopt, std::nullopt });
		path->nodes.push_back({ nullptr,
			world.getGraph()->getVertexByIdentifier(targetIdentifier), 0.0f, std::nullopt, std::nullopt });
		agent->setPath(path, true);
		auto const document = directory / "unreachable.world.yaml";
		world.saveTo(document.string());

		auto reopened = core::loadWorldDocument(document);
		auto* restored = reopened->lookupAgent(id).entity;
		require(restored && !restored->getPath()
			&& restored->getState() == core::Agent::State::Idle,
			"An unreachable restored destination did not leave its Agent idle");
		auto const& warnings = reopened->getLoadWarnings();
		require(warnings.size() == 1
			&& warnings.front().find("Stranded") != std::string::npos
			&& warnings.front().find("unreachable") != std::string::npos,
			"The unreachable restored destination did not produce an Agent-specific warning");

	}

	// The restored Path is the Agent's intent, not merely a route: an active one
	// still starts and carries the Agent to the upper Marker by the Staircase.
	void restoredActivePathReachesTheMarkerByTheStaircase(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(true);
		fixture.authorRoute(true);
		fixture.save(directory);

		auto reopened = core::loadWorldDocument(directory / "restored.world.yaml");
		auto* agent = reopened->lookupAgent(fixture.id).entity;
		require(agent && agent->getState() == core::Agent::State::MovingToVertex,
			"The restored active Path did not start");
		require(reopened->resumeSimulation(), "The reopened World did not resume");
		for (unsigned tick = 0; tick < 20000 && agent->getPath(); ++tick)
			reopened->advanceTick();
		require(!agent->getPath(), "The restored Path never completed");
		auto const arrived = agent->getGlobalPosition();
		require(std::abs(arrived.y - 1.0f) < 0.001f && std::abs(arrived.x - 1.5f) < 0.2f,
			"The Agent did not arrive at the upper Marker");
		require(agent->getSector() && agent->getSector()->getIndex() == fixture.upper,
			"The Agent did not arrive on the upper Level");
	}

	// The active flag is part of the intent too: an inactive restored Path keeps
	// its destination without starting itself.
	void restoredInactivePathKeepsItsDestinationWithoutStarting(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(true);
		fixture.authorRoute(false);
		fixture.save(directory);

		auto reopened = core::loadWorldDocument(directory / "restored.world.yaml");
		auto* restored = reopened->lookupAgent(fixture.id).entity;
		auto path = restored->getPath();
		require(path && path->nodes.size() > 1, "The inactive restored Path was dropped");
		require(edgeKindCount(*path, core::EdgeType::Ladder) == 0
			&& untraversableEdgeCount(restored, *path) == 0,
			"The inactive restored Path was not rebuilt");
		require(restored->getState() == core::Agent::State::Idle,
			"An inactive restored Path started itself");
	}

	// Reset replays through the same deserialize/resolve ordering, so it must
	// rebuild the same permitted route rather than re-expose the forbidden one.
	void resetSimulationRebuildsThePermittedRoute(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(true);
		fixture.authorRoute(true);
		fixture.save(directory);

		auto reopened = core::loadWorldDocument(directory / "restored.world.yaml");
		reopened->resetSimulation();
		auto* restored = reopened->lookupAgent(fixture.id).entity;
		auto path = restored->getPath();
		require(path && path->nodes.size() > 1, "Reset dropped the restored Path");
		require(edgeKindCount(*path, core::EdgeType::Ladder) == 0,
			"Reset re-exposed the tag-forbidden Ladder");
		require(edgeKindCount(*path, core::EdgeType::Staircase) > 0,
			"Reset did not keep the Staircase fallback");
		require(untraversableEdgeCount(restored, *path) == 0,
			"A Path rebuilt by reset holds an untraversable edge");
	}

	// An individual profile is available during Agent deserialization, so it must
	// yield the same permitted route as the tag-supplied one (ADR 0012).
	void individualProfileGivesTheSamePermittedRestoredRoute(smoke::Context const& context)
	{
		auto const directory = context.temporaryRoot() / __func__;
		std::filesystem::create_directories(directory);
		Fixture fixture(false);
		fixture.authorRoute(true);
		fixture.save(directory);

		auto reopened = core::loadWorldDocument(directory / "restored.world.yaml");
		auto* restored = reopened->lookupAgent(fixture.id).entity;
		auto path = restored->getPath();
		require(path && path->nodes.size() > 1, "The individual-profile Agent lost its Path");
		require(edgeKindCount(*path, core::EdgeType::Ladder) == 0
			&& edgeKindCount(*path, core::EdgeType::Staircase) > 0,
			"An individual Ladder restriction did not route the restored Path by the Staircase");
		require(untraversableEdgeCount(restored, *path) == 0,
			"A rebuilt Path holds an untraversable edge for an individual profile");
	}
}

namespace routing_smoke
{
	void registerRestoredPaths(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "savedIntentWaitsForRegistryResolution", [](smoke::Context const& context)
		{
			savedIntentWaitsForRegistryResolution(context);
		} });
		checks.push_back({ "tagSuppliedProfileIsHonouredByTheRestoredPath", [](smoke::Context const& context)
		{
			tagSuppliedProfileIsHonouredByTheRestoredPath(context);
		} });
		checks.push_back({ "unreachableSavedDestinationLoadsIdleWithWarning", [](smoke::Context const& context)
		{
			unreachableSavedDestinationLoadsIdleWithWarning(context);
		} });
		checks.push_back({ "restoredActivePathReachesTheMarkerByTheStaircase", [](smoke::Context const& context)
		{
			restoredActivePathReachesTheMarkerByTheStaircase(context);
		} });
		checks.push_back({ "restoredInactivePathKeepsItsDestinationWithoutStarting", [](smoke::Context const& context)
		{
			restoredInactivePathKeepsItsDestinationWithoutStarting(context);
		} });
		checks.push_back({ "resetSimulationRebuildsThePermittedRoute", [](smoke::Context const& context)
		{
			resetSimulationRebuildsThePermittedRoute(context);
		} });
		checks.push_back({ "individualProfileGivesTheSamePermittedRestoredRoute", [](smoke::Context const& context)
		{
			individualProfileGivesTheSamePermittedRestoredRoute(context);
		} });
	}
}
