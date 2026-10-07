// Migrated from AgentHeightSmokeChecks.cpp (#286); core dependency tier.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

#include "Checks.h"
#include "PathFixture.h"
#include "core/DoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/Vertex.h"

void runPoseDoorClearanceDiagnostics(smoke::Context const& context);
void runPoseDoorMovementReset(smoke::Context const& context);

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work),
			"The Height registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work),
			"The Height World did not deserialize");
		return world;
	}

	void rangesAreBoundedRevisionedAndPersisted()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("height");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		auto const* added = registry->getAgentTagHeightModifier(tag);
		require(added && added->range == core::DefaultAgentHeightModifierRange
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Height did not default both endpoints to 1.0 at revision 1");

		auto const beforeInvalid = serializeRegistry(*registry);
		for (auto const invalid : {
			core::AgentModifierRange{ 0.69f, 1.0f },
			core::AgentModifierRange{ 0.7f, 1.01f },
			core::AgentModifierRange{ 0.9f, 0.8f },
			core::AgentModifierRange{ std::numeric_limits<float>::infinity(), 1.0f },
			core::AgentModifierRange{ 0.7f, std::numeric_limits<float>::quiet_NaN() } })
		{
			require(!registry->setAgentTagHeightModifier(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serializeRegistry(*registry) == beforeInvalid
				&& registry->getNextPropertyRevision() == 2,
				"An invalid Height range changed definitions or revisions");
		}

		require(registry->setAgentTagHeightModifier(tag, { 0.7f, 1.0f }, &diagnostic),
			diagnostic);
		auto const expected = *registry->getAgentTagHeightModifier(tag);
		require(expected.revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Height range edit did not allocate one revision");
		auto const yaml = serializeRegistry(*registry);
		require(yaml.find("type: heightModifier") != std::string::npos,
			"Height property type was not persisted");
		auto reopened = deserializeRegistry(yaml);
		require(*reopened->getAgentTagHeightModifier(tag) == expected,
			"Height endpoints or revision did not survive persistence");

		auto malformed = YAML::Load(yaml);
		malformed["tags"][0]["properties"][0]["min"] = 0.6f;
		bool refused{ false };
		try { (void)deserializeRegistry(YAML::Dump(malformed)); }
		catch (std::exception const& error)
		{
			refused = std::string(error.what()).find("between 0.7 and 1.0")
				!= std::string::npos;
		}
		require(refused, "A persisted out-of-bounds Height range was accepted");
	}

	void assignmentPersistenceAndConflictsMatchOtherProperties()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const shortTag = registry->addAgentTag("short");
		auto const duplicate = registry->addAgentTag("duplicate");
		auto const pending = registry->addAgentTag("pending");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(shortTag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(shortTag, { 0.7f, 0.7f },
			&diagnostic), diagnostic);
		require(registry->addAgentTagHeightModifier(duplicate, &diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Height assignment", 8, 2);
		world->attachAgentTagRegistry("height.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		auto const agent = world->createAgent("Short", corridor, 0, 1.5f);
		world->pauseSimulation();
		require(world->assignAgentTag(agent, shortTag, &diagnostic), diagnostic);
		auto const sample = world->lookupAgent(agent).entity->getHeightModifierSample();
		require(sample && sample->type == core::SampledAgentPropertyType::HeightModifier
			&& sample->sourceTag == shortTag
			&& sample->propertyRevision
				== registry->getAgentTagHeightModifier(shortTag)->revision
			&& std::abs(sample->value - 0.7f) < 0.000001f,
			"Height assignment did not create the fixed sample with exact provenance");

		auto const beforeConflict = serializeWorld(*world);
		require(!world->assignAgentTag(agent, duplicate, &diagnostic)
			&& diagnostic.find("Height modifier") != std::string::npos
			&& diagnostic.find("#short") != std::string::npos
			&& diagnostic.find("#duplicate") != std::string::npos
			&& serializeWorld(*world) == beforeConflict,
			"A duplicate inherited Height modifier was not refused atomically");

		require(world->assignAgentTag(agent, pending, &diagnostic), diagnostic);
		auto const registryBefore = serializeRegistry(*registry);
		auto const revisionBefore = registry->getNextPropertyRevision();
		require(!registry->addAgentTagHeightModifier(pending, &diagnostic)
			&& diagnostic.find("Height modifier") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& registry->getNextPropertyRevision() == revisionBefore,
			"A conflicting Height property addition changed the registry");

		auto const exactSample = *sample;
		auto reopened = deserializeWorld(serializeWorld(*world));
		reopened->resolveAgentTagRegistry(registry);
		require(reopened->lookupAgent(agent).entity->getHeightModifierSample()
			== std::optional<core::AgentPropertySample>{ exactSample },
			"The exact Height sample did not survive World persistence");
		reopened->resetSimulation();
		require(reopened->lookupAgent(agent).entity->getHeightModifierSample()
			== std::optional<core::AgentPropertySample>{ exactSample },
			"Simulation reset changed the persisted Height sample");
	}

	void standingDoorClearance()
	{
		// Every ordered Location pairing, both directions, and every usable mode.
		for (int frontKind = 0; frontKind != 3; ++frontKind)
		for (int backKind = 0; backKind != 3; ++backKind)
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled })
		{
			core::World world("Standing clearance", 12, 3);
			auto location = [&](int kind, uint32_t layer)
			{
				if (kind == 1) return world.addCorridor(layer, 1, 0, 12, 1);
				if (kind == 2) return world.addFacade(layer, 1, 0, 12, 1);
				return world.addRoom(layer ? "Back" : "Front", layer, 1, 0, 12, 1);
			};
			location(frontKind, 0);
			location(backKind, 1);
			// The observer's feet are deliberately on a different Floor from the Door.
			auto observer = world.addRoom("Observer", 0, 0, 0, 12, 1);
			core::World::CreateDoorOptions options;
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			options.heightScale = 0.7f;
			world.addSectorDoor(0, 1, 2, options);
			world.finishBuild();
			auto id = world.createAgent("Standing", observer, 0, 1.5f);
			auto agent = world.lookupAgent(id).entity;
			world.pauseSimulation();
			auto registry = core::AgentTagRegistry::create();
			auto tag = registry->addAgentTag("short");
			require(registry->addAgentTagHeightModifier(tag), "Height tag failed");
			require(registry->setAgentTagHeightModifier(tag, { 0.7f, 0.7f }), "Height range failed");
			world.attachAgentTagRegistry("clearance.tags.yaml", registry);
			require(world.assignAgentTag(id, tag), "Height assignment failed");
			auto sample = agent->getHeightModifierSample();
			auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
				[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
			require(edgeIt != world.getGraph()->getEdges().end(), "Missing Door");
			auto edge = *edgeIt;
			auto door = static_cast<core::DoorEdge const&>(*edge).getDoor();
			auto const policy = world.getRouteChoicePolicy();
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
			core::RouteDecisionContext context{ agent, policy.baselineProfile, policy,
				agent->getSector(), agent->getWalkSpeed(), &world, agent->getClimbSpeed(), true, 0, 0, mobility };
			for (uint32_t side = 0; side != 2; ++side)
			{
				auto target = edge->getVertex(side);
				require(edge->getDirectedTraversalFacts(target, context).feasible
					&& core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context).feasible,
					"Tag short Agent or nonzero remote Floor rejected");
				require(world.setAgentIndividualHeightModifier(id, 1.0f), "Individual Height failed");
				auto frozen = core::RouteTraversalInputs::capture(*edge, target, context);
				require(!frozen.evaluate(context).feasible
					&& frozen.evaluate(context).exclusionReason == core::RouteExclusionReason::Clearance
					&& !edge->getDirectedTraversalFacts(target, context).feasible,
					"Too tall bypassed hard fallback exclusion");
				require(world.setAgentIndividualHeightModifier(id, {}), "Height reset failed");
				require(!frozen.evaluate(context).feasible && agent->getHeightModifierSample() == sample,
					"Captured inputs mutated or persisted Height resampled");
			}
			// Runtime uses the actual feet, not a remote route endpoint or Door bottom.
			auto runtimeId = world.createAgent("Runtime", edge->getVertex(0)->getSector()->getIndex(), 0, 2.5f);
			require(world.setAgentIndividualHeightModifier(runtimeId, 0.7f), "Runtime Height refused");
			auto agentHandle = std::shared_ptr<const core::Agent>(world.lookupAgent(runtimeId).entity, [](core::Agent const*) {});
			require(door->setHeightScale(0.6f), "Runtime low scale refused");
			require(!edge->isTraversable(edge->getVertex(1), agentHandle)
				&& edge->requestTraversal(edge->getVertex(1), agentHandle) == core::EdgeTraversalRequestResult::Failed
				&& !door->isOpening(), "Runtime clearance allowed passage or started opening");
			require(door->setHeightScale(0.63f), "Runtime exact scale refused");
			if (mode != core::DoorActivationMode::RemoteControlled)
				require(edge->requestTraversal(edge->getVertex(1), agentHandle) == core::EdgeTraversalRequestResult::OK,
					"Runtime exact fit refused");
			for (auto scale : { 0.1f, 1.0f })
			{
				require(door->setHeightScale(scale), "Scale endpoint refused");
				require(door->admitsStandingHeight(agent->getStandingHeight(), 1.0f) == (scale == 1.0f),
					"Scale endpoint clearance incorrect");
			}
			require(door->setHeightScale({}), "Default scale refused");
			for (auto height : { core::Door::Height::Regular, core::Door::Height::Tall })
			{
				door->setHeight(height);
				require(door->admitsStandingHeight(CORE_AGENT_MAX_HEIGHT, 1.0f), "Default standing Agent rejected");
				auto const feet = door->getPosition().y + core::Door::effectiveHeight(height) - agent->getStandingHeight();
				require(door->admitsStandingHeight(agent->getStandingHeight(), feet)
					&& door->admitsStandingHeight(agent->getStandingHeight(), feet + core::Door::StandingClearanceTolerance * 0.5f)
					&& !door->admitsStandingHeight(agent->getStandingHeight(), feet + core::Door::StandingClearanceTolerance * 2.0f),
					"Default/Tall top-relative exact-fit tolerance incorrect");
			}
		}
	}

	void standingDoorWorldJourneys()
	{
		// Minimal stale-admission defense: edit the live aperture while queued,
		// or after crossing starts. This is not a comprehensive live-edit contract.
		for (bool underway : { false, true })
		{
			core::World world("Stale clearance", 6, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 6, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 6, 1);
			core::World::CreateDoorOptions options;
			options.heightScale = 0.72f;
			auto created = world.addSectorDoor(0, 0, 2, options);
			world.finishBuild();
			auto id = world.createAgent("Head", front, 0, 2.5f);
			auto followerId = world.createAgent("Follower", front, 0, 1.5f);
			auto head = world.lookupAgent(id).entity, follower = world.lookupAgent(followerId).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualHeightModifier(id, 0.8f)
				&& world.setAgentIndividualHeightModifier(followerId, 0.7f), "Queue Heights refused");
			require(world.resumeSimulation(), "Queue resume refused");
			std::shared_ptr<core::Door> door;
			for (auto const& edge : world.getGraph()->getEdges())
				if (edge->getTraversalResourceId() == created.traversalResource)
				{
					door = static_cast<core::DoorEdge const&>(*edge).getDoor();
					auto source = edge->getVertex(0)->getSector()->getIndex() == front
						? edge->getVertex(0) : edge->getVertex(1);
					head->setPath(smoke::twoNodePath(source, edge->getOtherVertex(source), edge), true);
					follower->setPath(smoke::twoNodePath(source, edge->getOtherVertex(source), edge), true);
					break;
				}
			bool edited = false;
			for (uint32_t tick = 0; tick != 1200; ++tick)
			{
				world.advanceTick();
				if (!edited && ((underway && head->getState() == core::Agent::State::TraversingEdge)
					|| (!underway && head->getState() == core::Agent::State::WaitingForTraversal
						&& door->getOpenLeaseCount() > 0)))
				{
					require(door->setHeightScale(0.65f), "Stale aperture edit refused");
					edited = true;
				}
				if (edited && head->getState() == core::Agent::State::Idle
					&& follower->getState() == core::Agent::State::Idle) break;
			}
			require(edited && head->getState() == core::Agent::State::Idle
				&& (head->getSector() == world.getSector(back).get()) == underway
				&& follower->getSector() == world.getSector(back).get(),
				"Stale head blocked fitting follower or admitted crossing could not finish");
			auto snapshot = world.getSimulationSnapshot();
			require(snapshot.traversalPermits.empty() && snapshot.traversalRequests.empty()
				&& door->getOpenLeaseCount() == 0, "Stale clearance ownership leaked");
		}

		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled })
		for (int frontKind = 0; frontKind != 3; ++frontKind)
		for (int backKind = 0; backKind != 3; ++backKind)
		for (bool reverse : { false, true })
		for (bool alternate : { false, true })
		for (float modifier : { 0.7f, 0.8f, 0.8001f, 1.0f })
		for (bool external : { false, true })
		{
			core::World world("Clearance journeys", 12, 2);
			auto location = [&](int kind, uint32_t layer)
			{
				if (kind == 1) return world.addCorridor(layer, 1, 0, 12, 1);
				if (kind == 2) return world.addFacade(layer, 1, 0, 12, 1);
				return world.addRoom(layer ? "Back" : "Front", layer, 1, 0, 12, 1);
			};
			auto front = location(frontKind, 0);
			auto back = location(backKind, 1);
			core::World::CreateDoorOptions options;
			options.heightScale = 0.72f; // .36: exact fit for a valid .8 modifier.
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			auto low = world.addSectorDoor(0, 1, 2, options);
			if (alternate) world.addSectorDoor(0, 1, 9, {});
			auto sourceSector = reverse ? back : front, targetSector = reverse ? front : back;
			world.addSectorMarker(targetSector, 0, 3.5f, "Destination");
			world.finishBuild();
			auto id = world.createAgent("Traveller", sourceSector, 0, 1.5f);
			auto agent = world.lookupAgent(id).entity;
			world.pauseSimulation();
			// Route requests exercise inherited samples; supplied Paths exercise
			// individual-over-tag precedence at the live admission gate.
			auto registry = core::AgentTagRegistry::create();
			auto tag = registry->addAgentTag("height");
			require(registry->addAgentTagHeightModifier(tag), "Journey Height tag failed");
			if (!external && modifier != 1.0f)
				require(registry->setAgentTagHeightModifier(tag, { modifier, modifier }), "Journey Height range failed");
			world.attachAgentTagRegistry("journey.tags.yaml", registry);
			require(world.assignAgentTag(id, tag), "Journey Height assignment failed");
			if (external) require(world.setAgentIndividualHeightModifier(id, modifier), "Journey Height failed");
			require(world.setAgentIndividualPermissionAdherence(id, false), "Adherence edit failed");
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
			require(world.setAgentIndividualMobilityProfile(id, mobility), "Fallback edit failed");
			require(world.resumeSimulation(), "Journey resume failed");
			if (external)
			{
				for (auto const& edge : world.getGraph()->getEdges())
					if (edge->getTraversalResourceId() == low.traversalResource)
					{
						auto source = edge->getVertex(0)->getSector()->getIndex() == sourceSector
							? edge->getVertex(0) : edge->getVertex(1);
						agent->setPath(smoke::twoNodePath(source, edge->getOtherVertex(source), edge), true);
						break;
					}
			}
			else require(world.moveAgentToMarker(id, world.getMarkerIds().front()).accepted(), "Journey intent refused");
			bool lost = false, crossedLow = false;
			for (uint32_t tick = 0; tick != 6000; ++tick)
			{
				world.advanceTick();
				for (auto const& event : world.consumeSimulationEvents())
					if (event.type == core::SimulationEventType::RouteLost)
					{
						require(event.routeLossReason == core::RouteLossReason::Unreachable, "Wrong clearance Route loss");
						lost = true;
					}
				for (auto const& permit : world.getSimulationSnapshot().traversalPermits)
					for (auto const& request : world.getSimulationSnapshot().traversalRequests)
						if (permit.request == request.id && request.resource == low.traversalResource) crossedLow = true;
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			bool const fits = modifier <= 0.8f;
			require(agent->getState() == core::Agent::State::Idle
				&& (agent->getSector() == world.getSector(targetSector).get()) == (fits || alternate)
				&& lost == (!fits && !alternate) && crossedLow == fits,
				"World journey failed: front=" + std::to_string(frontKind) + " back=" + std::to_string(backKind)
				+ " reverse=" + std::to_string(reverse) + " alternate=" + std::to_string(alternate)
				+ " modifier=" + std::to_string(modifier) + " external=" + std::to_string(external) + " mode=" + std::to_string(static_cast<int>(mode))
				+ " state=" + std::to_string(static_cast<int>(agent->getState())) + " lost=" + std::to_string(lost)
				+ " crossedLow=" + std::to_string(crossedLow) + " x=" + std::to_string(agent->getGlobalPosition().x) + " y=" + std::to_string(agent->getGlobalPosition().y));
			auto snapshot = world.getSimulationSnapshot();
			require(snapshot.traversalPermits.empty() && snapshot.traversalRequests.empty(), "Traversal ownership leaked");
			for (auto const& resource : snapshot.traversalResources)
			{
				require(resource.openLeaseCount == 0, "Door lease leaked");
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Door queue leaked");
				for (auto const& lane : resource.crossingLanes) require(!lane.owner, "Crossing lane leaked");
			}
		}
	}

}

void agent_smoke::registerHeight(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "poseDoorClearanceDiagnostics", runPoseDoorClearanceDiagnostics });
	checks.push_back({ "poseDoorMovementReset", runPoseDoorMovementReset });
	checks.push_back({ "standingDoorClearance", [](smoke::Context const&) { standingDoorClearance(); } });
	checks.push_back({ "standingDoorWorldJourneys", [](smoke::Context const&) { standingDoorWorldJourneys(); } });
	checks.push_back({ "heightRangesAreBoundedRevisionedAndPersisted", [](smoke::Context const&) { rangesAreBoundedRevisionedAndPersisted(); } });
	checks.push_back({ "heightAssignmentPersistenceAndConflictsMatchOtherProperties", [](smoke::Context const&) { assignmentPersistenceAndConflictsMatchOtherProperties(); } });
}
