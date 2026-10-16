#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Edge.h"
#include "core/RouteCost.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	std::string serialize(core::Serializable const& document)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		document.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	void deserialize(core::Serializable& document, std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(document.deserialize(*reader, work), "Permission adherence fixture failed to load");
	}

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{ std::make_shared<core::World>("Adherence", 8, 2) };
		core::AgentTagId tag{ registry->addAgentTag("staff") };
		uint32_t corridor{};
		core::AgentId id{};
		std::string diagnostic;

		Fixture()
		{
			world->attachAgentTagRegistry("adherence.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			world->pauseSimulation();
			id = world->createAgent("Walker", corridor, 0, 1.5f);
			require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		}

		core::Agent* agent() const { return world->lookupAgent(id).entity; }
	};

	void defaultsOverridesAndCompatibility()
	{
		Fixture f;
		require(core::agentPropertyMetadata(core::AgentPropertyType::PermissionAdherence)
			.propertyNamespace == "Pathing", "Permission adherence is not a Pathing property");
		auto effective = f.agent()->getEffectivePermissionAdherence();
		require(effective.value && !effective.individual && !effective.sourceTag,
			"Permission adherence did not default to true");
		require(f.registry->addAgentTagPermissionAdherence(f.tag, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagPermissionAdherence(f.tag, false, &f.diagnostic), f.diagnostic);
		effective = f.agent()->getEffectivePermissionAdherence();
		require(!effective.value && effective.sourceTag == f.tag && !effective.individual
			&& effective.propertyRevision == f.registry->getAgentTagPermissionAdherence(f.tag)->revision,
			"Tag Permission adherence lost value or source");
		require(f.world->setAgentIndividualPermissionAdherence(f.id, true, &f.diagnostic), f.diagnostic);
		effective = f.agent()->getEffectivePermissionAdherence();
		require(effective.value && effective.individual && !effective.sourceTag,
			"Individual Permission adherence did not override the tag");
		require(f.world->setAgentIndividualPermissionAdherence(f.id, std::nullopt, &f.diagnostic), f.diagnostic);
		require(!f.agent()->getEffectivePermissionAdherence().value,
			"Removing the individual override did not reveal the tag value");

		auto other = f.registry->addAgentTag("visitors");
		require(f.world->assignAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(!f.registry->addAgentTagPermissionAdherence(other, &f.diagnostic),
			"Registry accepted conflicting inherited Permission adherence");
		require(f.world->removeAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(f.registry->addAgentTagPermissionAdherence(other, &f.diagnostic), f.diagnostic);
		require(!f.world->assignAgentTag(f.id, other, &f.diagnostic),
			"Assignment accepted conflicting inherited Permission adherence");
		require(f.world->resumeSimulation(), "Permission adherence fixture could not resume");
		require(!f.world->setAgentIndividualPermissionAdherence(f.id, true, &f.diagnostic)
			&& !f.registry->setAgentTagPermissionAdherence(f.tag, true, &f.diagnostic),
			"Permission adherence editing was allowed while simulation was running");
	}

	void persistenceCopyAndLegacyDefaults()
	{
		Fixture f;
		auto legacyRegistry = YAML::Load(serialize(*f.registry));
		legacyRegistry["agentTagRegistry"]["version"] = 13;
		auto legacyWorld = YAML::Load(serialize(*f.world));
		legacyWorld["world"]["version"] = 30;

		require(f.registry->addAgentTagPermissionAdherence(f.tag, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagPermissionAdherence(f.tag, false, &f.diagnostic), f.diagnostic);
		require(f.world->setAgentIndividualPermissionAdherence(f.id, false, &f.diagnostic), f.diagnostic);

		auto registryCopy = core::AgentTagRegistry::create();
		deserialize(*registryCopy, serialize(*f.registry));
		auto loaded = std::make_shared<core::World>("Loaded", 1, 1);
		deserialize(*loaded, serialize(*f.world));
		loaded->resolveAgentTagRegistry(registryCopy);
		auto loadedAgent = loaded->lookupAgent(f.id).entity;
		require(registryCopy->getAgentTagPermissionAdherence(f.tag)
			&& !registryCopy->getAgentTagPermissionAdherence(f.tag)->value
			&& loadedAgent->getIndividualPermissionAdherence() == false
			&& !loadedAgent->getEffectivePermissionAdherence().value,
			"Permission adherence did not survive World and registry round trips");

		auto legacyRegistryCopy = core::AgentTagRegistry::create();
		deserialize(*legacyRegistryCopy, YAML::Dump(legacyRegistry));
		deserialize(*loaded, YAML::Dump(legacyWorld));
		loaded->resolveAgentTagRegistry(legacyRegistryCopy);
		require(loaded->lookupAgent(f.id).entity->getEffectivePermissionAdherence().value,
			"Legacy documents did not receive default-true Permission adherence");
	}

	void extensibleResourceApproachesAndGrants()
	{
		core::World world("Extensible adherence", 12, 3);
		auto bridgeRoom = world.addRoom("Bridge", 0, 0, 0, 6, 2);
		world.addSectorWalkway(bridgeRoom, 1, 0);
		world.addSectorWalkway(bridgeRoom, 1, 3);
		auto bridge = world.addSectorForceBridge(bridgeRoom, 1, 1,
			{ 2, CORE_SIDE_LEFT, true, true, 2 });
		auto ladderRoom = world.addRoom("Ladder", 0, 0, 7, 3, 2);
		world.addSectorWalkway(ladderRoom, 1, 1);
		auto ladder = world.addRoomLadder(ladderRoom, 0, 1, { 0, true, true });
		world.finishBuild(); world.pauseSimulation();

		auto left = world.addAccessPermission("Bridge left");
		auto low = world.addAccessPermission("Ladder low");
		std::string diagnostic;
		require(world.setInteractionPointPermissionRequirement(
			bridge.controls[0].interactionPoint, { left }, &diagnostic), diagnostic);
		require(world.setInteractionPointPermissionRequirement(
			ladder.controls[0].interactionPoint, { low }, &diagnostic), diagnostic);
		auto leftAgent = world.createAgent("Bridge walker", bridgeRoom, 1, 0.5f);
		auto ladderAgent = world.createAgent("Ladder climber", ladderRoom, 0, 1.5f);
		auto bridgeSector = core::SectorId{ static_cast<uint64_t>(bridgeRoom) + 1 };
		auto ladderSector = core::SectorId{ static_cast<uint64_t>(ladderRoom) + 1 };

		require(!world.agentAdheresToExtensiblePermission(bridge.traversalResource,
			bridgeSector, { 0.5f, 1.0f }, leftAgent),
			"Default adherence admitted a protected extended Force Bridge");
		auto* bridgeWalker = world.lookupAgent(leftAgent).entity;
		auto const policy = world.getRouteChoicePolicy();
		core::RouteDecisionContext bridgeContext{ bridgeWalker, policy.baselineProfile, policy,
			bridgeWalker->getSector(), bridgeWalker->getWalkSpeed(), &world,
			bridgeWalker->getClimbSpeed() };
		std::shared_ptr<const core::Edge> bridgeEdge;
		std::shared_ptr<const core::Vertex> bridgeRight;
		for (auto const& edge : world.getGraph()->getEdges())
			if (edge->getTraversalResourceId() == bridge.traversalResource)
			{
				bridgeEdge = edge;
				bridgeRight = edge->getVertex(0)->getPosition().x
					> edge->getVertex(1)->getPosition().x ? edge->getVertex(0) : edge->getVertex(1);
				break;
			}
		require(bridgeEdge && !bridgeEdge->getDirectedTraversalFacts(
			bridgeRight, bridgeContext).feasible,
			"Route planning admitted a protected extended Force Bridge");
		require(world.agentAdheresToExtensiblePermission(bridge.traversalResource,
			bridgeSector, { 3.5f, 1.0f }, leftAgent),
			"A far-side Force Bridge restriction contaminated the unrestricted approach");
		require(!world.canAgentOperateExtensibleControl(bridge.traversalResource,
			bridgeSector, { 0.5f, 1.0f }, leftAgent),
			"An unauthorized Agent could operate a Force Bridge control");
		require(world.setAgentIndividualPermissionAdherence(leftAgent, false, &diagnostic), diagnostic);
		require(world.agentAdheresToExtensiblePermission(bridge.traversalResource,
			bridgeSector, { 0.5f, 1.0f }, leftAgent)
			&& !world.canAgentOperateExtensibleControl(bridge.traversalResource,
				bridgeSector, { 0.5f, 1.0f }, leftAgent),
			"Disabled adherence authorized a protected Force Bridge operation");
		require(bridgeEdge->getDirectedTraversalFacts(bridgeRight, bridgeContext).feasible,
			"Route planning did not preserve opportunistic Force Bridge use");
		require(world.setAgentAccessPermissionGrant(leftAgent, left, true, &diagnostic), diagnostic);
		require(world.canAgentOperateExtensibleControl(bridge.traversalResource,
			bridgeSector, { 0.5f, 1.0f }, leftAgent),
			"A direct grant did not authorize the applicable Force Bridge control");

		require(!world.agentAdheresToExtensiblePermission(ladder.traversalResource,
			ladderSector, { 8.5f, 0.0f }, ladderAgent),
			"Default adherence admitted a protected extended Ladder");
		require(world.agentAdheresToExtensiblePermission(ladder.traversalResource,
			ladderSector, { 8.5f, 1.0f }, ladderAgent),
			"A low Ladder restriction contaminated the unrestricted high approach");
		auto set = world.addPermissionSet("Ladder users");
		require(world.setPermissionSetAccessPermission(set, low, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(ladderAgent, set, true, &diagnostic), diagnostic);
		require(world.agentAdheresToExtensiblePermission(ladder.traversalResource,
			ladderSector, { 8.5f, 0.0f }, ladderAgent),
			"A Permission set grant did not satisfy an extensible Ladder requirement");

		// Protect both ends independently, even though each pair shares a Room.
		// Exercise the public operation/willingness seams and directed routing facts.
		auto right = world.addAccessPermission("Bridge right");
		auto high = world.addAccessPermission("Ladder high");
		require(world.setInteractionPointPermissionRequirement(
			bridge.controls[1].interactionPoint, { right }, &diagnostic), diagnostic);
		require(world.setInteractionPointPermissionRequirement(
			ladder.controls[1].interactionPoint, { high }, &diagnostic), diagnostic);
		require(world.setAgentIndividualPermissionAdherence(leftAgent, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(ladderAgent, set, false, &diagnostic), diagnostic);
		auto checkEnds = [&](core::TraversalResourceId resource, core::SectorId sector,
			core::AgentId actor, core::AccessPermissionId first, core::AccessPermissionId second,
			bool vertical, float middle, core::Vector2 start, core::Vector2 end)
		{
			auto* agent = world.lookupAgent(actor).entity;
			auto const position = agent->getGlobalPosition();
			for (unsigned grants = 0; grants != 4; ++grants)
			{
				auto setGrant = [&](core::AccessPermissionId permission, bool granted)
				{
					auto current = world.getAgentDirectAccessGrants(actor);
					if ((std::find(current.begin(), current.end(), permission) != current.end()) != granted)
						require(world.setAgentAccessPermissionGrant(actor, permission, granted, &diagnostic), diagnostic);
				};
				setGrant(first, grants & 1);
				setGrant(second, grants & 2);
				auto check = [&](core::SectorId approach, core::Vector2 point, bool allowed, bool applies = true)
				{
					require(world.canAgentOperateExtensibleControl(resource, approach, point, actor) == (applies && allowed),
						"Extensible operation classified the wrong approach endpoint");
					require(world.agentAdheresToExtensiblePermission(resource, approach, point, actor) == (!applies || allowed),
						"Extensible Permission adherence classified the wrong approach endpoint");
				};
				check(sector, start, grants & 1);
				check(sector, end, grants & 2);
				for (float coordinate : { std::nextafter(middle, -std::numeric_limits<float>::infinity()),
					middle, std::nextafter(middle, std::numeric_limits<float>::infinity()) })
				{
					auto point = start;
					if (vertical) point.y = coordinate; else point.x = coordinate;
					check(sector, point, grants & (coordinate > middle ? 2 : 1));
				}
				check({}, start, false, false);
				core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
					agent->getSector(), agent->getWalkSpeed(), &world, agent->getClimbSpeed() };
				bool found = false;
				for (auto const& edge : world.getGraph()->getEdges())
					if (edge->getTraversalResourceId() == resource
						&& edge->getType() == (vertical ? core::EdgeType::Ladder : core::EdgeType::ForceBridge))
					{
						found = true;
						for (unsigned target = 0; target != 2; ++target)
						{
							auto source = edge->getVertex(1 - target)->getPosition();
							auto allowed = grants & ((vertical ? source.y : source.x) > middle ? 2 : 1);
							require(edge->getDirectedTraversalFacts(edge->getVertex(target), context).feasible == bool(allowed),
								"Directed routing did not preserve independent endpoint requirements");
						}
					}
				require(found, "Extensible routing edge missing");
				require(agent->getGlobalPosition() == position, "Eligibility queries moved the Agent");
			}
		};
		checkEnds(bridge.traversalResource, bridgeSector, leftAgent, left, right,
			false, 2.f, { 0.5f, 1.f }, { 3.5f, 1.f });
		// Logical landings are y=0 and y=1. Their midpoint is 0.5, not the
		// rendered Ladder centre above it (which includes the handholds).
		checkEnds(ladder.traversalResource, ladderSector, ladderAgent, low, high,
			true, 0.5f, { 8.5f, 0.f }, { 8.5f, 1.f });
	}
}

void permission_smoke::registerAdherence(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "defaultsOverridesAndCompatibility",
		[](smoke::Context const&)
		{
			defaultsOverridesAndCompatibility();
		} });
	checks.push_back({ "persistenceCopyAndLegacyDefaults",
		[](smoke::Context const&)
		{
			persistenceCopyAndLegacyDefaults();
		} });
	checks.push_back({ "extensibleResourceApproachesAndGrants",
		[](smoke::Context const&)
		{
			extensibleResourceApproachesAndGrants();
		} });
}
