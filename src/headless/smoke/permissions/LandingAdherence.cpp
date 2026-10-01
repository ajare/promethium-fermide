#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	void landingJourney(unsigned kind, unsigned change)
	{
		// 0: inherited opportunism; 1: stale Path; 2/3: revoke riding/boarding;
		// 4/5: revoke/tighten before boarding; 6/7: tighten/enable adherence riding.
		auto const opportunistic = change < 2 || change == 7;
		auto world = std::make_shared<core::World>("Landing adherence", kind == 2 ? 40 : 16, 2);
		auto registry = core::AgentTagRegistry::create();
		world->attachAgentTagRegistry("landing.tags.yaml", registry);
		auto ground = kind == 1 ? world->addRoom("Platform room", 0, 0, 0, 16, 2)
			: world->addCorridor(0, 0, 16);
		auto upper = kind == 1 ? ground : world->addCorridor(kind == 2 ? 0 : 1, kind == 2 ? 24 : 0, 16);
		if (kind == 1) for (unsigned x = 0; x < 16; ++x) world->addSectorWalkway(ground, 1, x);
		world->addSectorMarker(ground, 0, 7.0f);
		world->addSectorMarker(upper, kind == 1 ? 1 : 0, 7.0f);
		world->finishBuild(); world->pauseSimulation();
		auto red = world->addAccessPermission("Landing red");
		auto blue = world->addAccessPermission("Landing blue");
		core::TraversalResourceId vehicle;
		std::vector<core::InteractionPointId> controls;
		if (kind == 2)
		{
			core::World::CreateShuttleOptions options{ 1, 4, { 0, 24 }, 0 };
			options.capacity = 2;
			options.minimumDwellSeconds = 20; options.maximumBoardingSeconds = 30;
			options.landingControlPermissionRequirements = { { red, blue }, {} };
			auto created = world->addShuttle(1, 0, 4, 36, options);
			vehicle = created.traversalResource;
			for (auto const& door : created.doors) controls.push_back(door.controls[0].interactionPoint);
		}
		else
		{
			core::World::CreateLiftOptions options;
			options.capacity = 2;
			options.cellsWide = kind == 1 ? 1 : 2; options.stopOffsets = { 0, 1 };
			options.minimumDwellSeconds = 20; options.maximumBoardingSeconds = 30;
			options.platformStopDurationSeconds = 20;
			options.landingControlPermissionRequirements = { { red, blue }, {} };
			if (kind == 1)
			{
				auto created = world->addSectorPlatformLift(ground, 0, 8, options);
				vehicle = created.traversalResource;
				for (auto const& button : created.buttons) controls.push_back(button.interactionPoint);
			}
			else
			{
				auto created = world->addLift(1, 0, 8, options);
				vehicle = created.traversalResource;
				for (auto const& door : created.doors) controls.push_back(door.controls[0].interactionPoint);
			}
		}
		world->finishBuild(); world->pauseSimulation();
		std::string diagnostic;
		auto id = world->createAgent("Boarder", ground, 0, kind == 2 ? 5.5f : 8.5f);
		auto agent = world->lookupAgent(id).entity;
		auto remoteId = world->createAgent("Remote observer", upper, kind == 1 ? 1 : 0, 7.0f);
		auto tag = registry->addAgentTag("opportunist");
		require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		require(registry->addAgentTagPermissionAdherence(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagPermissionAdherence(tag, false, &diagnostic), diagnostic);
		require(world->setAgentIndividualPermissionAdherence(remoteId, false, &diagnostic), diagnostic);
		auto operatorId = world->createAgent("Landing operator", ground, 0, 7.0f);
		require(world->grantAgentAccessPermission(operatorId, red, &diagnostic), diagnostic);
		require(world->grantAgentAccessPermission(operatorId, blue, &diagnostic), diagnostic);
		auto graph = world->getGraph();
		auto start = graph->getVertexAtPosition(0, 7, 0, 0.01f);
		auto target = graph->getVertexAtPosition(0, kind == 2 ? 31.0f : 7.0f, kind == 2 ? 0.0f : 1.0f, 0.01f);
		// A non-adhering Agent still cannot presume future assistance.
		require(!graph->calculatePath(agent, start, target), "Closed protected transport admitted opportunistic route");
		require(world->resumeSimulation(), "Landing fixture did not resume");
		auto operatorAgent = world->lookupAgent(operatorId).entity;
		operatorAgent->setPath(graph->calculatePath(operatorAgent, target), true);
		std::shared_ptr<core::Path> path;
		for (unsigned tick = 0; tick < 600 && !path; ++tick)
		{
			world->advanceTick();
			path = graph->calculatePath(agent, start, target);
		}
		std::string state;
		for (auto const& resource : world->getSimulationSnapshot().traversalResources)
			if (resource.id == vehicle) state += " phase " + std::to_string(static_cast<int>(resource.liftStopPhase))
				+ " stop " + std::to_string(resource.liftCurrentStop);
		for (auto const& request : world->getSimulationSnapshot().interactionRequests)
			state += " interaction " + std::to_string(request.point.value) + " result " + std::to_string(static_cast<int>(request.result));
		require(static_cast<bool>(path), "Non-adhering Agent could not observe boardable transport " + std::to_string(kind)
			+ state + " effective " + std::to_string(agent->getEffectivePermissionAdherence().value));
		require(!graph->calculatePath(world->lookupAgent(remoteId).entity, start, target),
			"Remote Agent used another landing's live boarding opportunity");
		require(static_cast<bool>(graph->calculatePath(world->lookupAgent(remoteId).entity, target, start)),
			"Unrestricted second boarding origin inherited the first landing's requirement");
		world->pauseSimulation();
		require(world->setAgentIndividualPermissionAdherence(id, true, &diagnostic), diagnostic);
		require(agent->getEffectivePermissionAdherence().individual && !graph->calculatePath(agent, start, target),
			"Individual adherence did not override inherited boarding choice");
		require(world->grantAgentAccessPermission(id, red, &diagnostic), diagnostic);
		require(!graph->calculatePath(agent, start, target), "Partial grants bypassed all-required landing semantics");
		auto set = world->addPermissionSet("Landing operators");
		require(world->setPermissionSetAccessPermission(set, blue, true, &diagnostic), diagnostic);
		require(world->setAgentPermissionSetAssignment(id, set, true, &diagnostic), diagnostic);
		require(static_cast<bool>(graph->calculatePath(agent, start, target)), "Direct/set union did not permit boarding");
		if (opportunistic)
		{
			require(world->revokeAgentAccessPermission(id, red, &diagnostic), diagnostic);
			require(world->setAgentIndividualPermissionAdherence(id, std::nullopt, &diagnostic), diagnostic);
		}
		// Persisted values use the same effective profile as the live boarding gates.
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world->serialize(*writer, work); writer->serialize();
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
		auto restored = std::make_shared<core::World>("Restored", 1, 1);
		require(restored->deserialize(*reader, work), "Landing fixture did not deserialize");
		restored->resolveAgentTagRegistry(registry);
		require(restored->lookupAgent(id).entity->getEffectivePermissionAdherence().value == !opportunistic,
			"Landing adherence did not persist");
		path = graph->calculatePath(agent, target);
		require(static_cast<bool>(path), "Boarding path missing before runtime test");
		if (change == 1) require(world->setAgentIndividualPermissionAdherence(id, true, &diagnostic), diagnostic);
		require(world->resumeSimulation(), "Landing runtime fixture did not resume");
		if (opportunistic)
		{
			auto rejected = world->requestInteraction(controls[0], id);
			require(rejected && world->lookupInteractionRequest(rejected).entity->getResult() == core::InteractionResult::Rejected,
				"Adherence value authorized a protected landing operation");
		}
		// Deliberately reinstall an old Path: admission must independently enforce willingness.
		agent->setPath(path, true);
		if (change == 4)
		{
			require(world->setAgentRuntimePermissionSetAssignment(id, set, false), "Pending set revoke failed");
			require(agent->getState() == core::Agent::State::RoutePlanning, "Pending grant loss did not reconsider boarding");
		}
		if (change == 5)
		{
			world->pauseSimulation();
			auto extra = world->addAccessPermission("Tightened landing");
			require(world->grantAgentAccessPermission(operatorId, extra, &diagnostic), diagnostic);
			require(world->setInteractionPointPermissionRequirement(controls[0], { red, blue, extra }, &diagnostic), diagnostic);
			require(world->resumeSimulation(), "Tightened landing did not resume");
		}
		bool crossed = false;
		for (unsigned tick = 0; tick < 18000; ++tick)
		{
			world->advanceTick();
			if ((change == 2 || change == 3 || change >= 6) && !crossed)
			{
				auto snapshot = world->getSimulationSnapshot();
				for (auto const& resource : snapshot.traversalResources)
					if (resource.id == vehicle)
					{
						bool onboard = std::any_of(resource.capacityPositions.begin(), resource.capacityPositions.end(),
							[&](auto const& position) { return position.occupant == id; });
						bool boundary = std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
							[&](auto const& request)
							{
								if (request.owner != id) return false;
								if (kind == 1) return std::find(resource.virtualBoundaryOwners.begin(),
									resource.virtualBoundaryOwners.end(), request.id) != resource.virtualBoundaryOwners.end();
								return request.edgeType == core::EdgeType::Door && request.sourceSector == core::SectorId{ ground + 1 }
									&& request.state == core::TraversalRequestState::Granted;
							});
						if ((change == 2 && onboard) || (change >= 6 && onboard && resource.liftStopRequestOwnerCounts.size() > 1
								&& resource.liftStopRequestOwnerCounts[1] >= 2)
							|| (change == 3 && boundary))
						{
							crossed = true;
							if (change < 6) require(world->setAgentRuntimeAccessPermissionGrant(id, red, false), "Committed landing revoke failed");
							else
							{
								world->pauseSimulation();
								if (change == 7) require(world->setAgentIndividualPermissionAdherence(id, true, &diagnostic), diagnostic);
								else
								{
									auto extra = world->addAccessPermission("Onboard tightening");
									require(world->grantAgentAccessPermission(operatorId, extra, &diagnostic), diagnostic);
									require(world->setInteractionPointPermissionRequirement(controls[0], { red, blue, extra }, &diagnostic), diagnostic);
								}
								require(world->resumeSimulation(), "Onboard landing edit did not resume");
							}
						}
					}
			}
			if (agent->getState() == core::Agent::State::Idle && !agent->getPath()
				&& operatorAgent->getState() == core::Agent::State::Idle && !operatorAgent->getPath()) break;
		}
		if (change == 1 || change == 4 || change == 5)
		{
			bool lost = false;
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::RouteLost && event.agent.id == id) lost = true;
			require(lost && agent->getSector()->getIndex() == ground && !agent->getPath(),
				"Stale unauthorized boarding did not replan to Route loss");
		}
		else
		{
			require(change < 2 || crossed, "Authorization change did not exercise established boarding boundary");
			require(!agent->getPath() && agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.01f,
				"Landing adherence trapped a passenger or prevented safe exit: " + std::to_string(kind) + "/" + std::to_string(change)
				+ " x " + std::to_string(agent->getGlobalPosition().x) + " y " + std::to_string(agent->getGlobalPosition().y)
				+ " state " + std::to_string(static_cast<int>(agent->getState())));
		}
		operatorAgent = world->lookupAgent(operatorId).entity;
		require(!operatorAgent->getPath() && operatorAgent->getGlobalPosition().distanceTo(target->getPosition()) < 0.01f,
			"Another passenger's accepted journey was cancelled by landing authorization changes");
		if (change == 0)
		{
			// Replay the loaded profile through a fresh, initially closed transport.
			auto loadedGraph = restored->getGraph();
			auto loadedTarget = loadedGraph->getVertexAtPosition(0, kind == 2 ? 31.0f : 7.0f,
				kind == 2 ? 0.0f : 1.0f, 0.01f);
			auto loadedAgent = restored->lookupAgent(id).entity;
			auto loadedOperator = restored->lookupAgent(operatorId).entity;
			require(restored->resumeSimulation(), "Loaded landing fixture did not resume");
			loadedOperator->setPath(loadedGraph->calculatePath(loadedOperator, loadedTarget), true);
			std::shared_ptr<core::Path> loadedPath;
			for (unsigned tick = 0; tick < 600 && !loadedPath; ++tick)
			{
				restored->advanceTick();
				loadedPath = loadedGraph->calculatePath(loadedAgent, loadedTarget);
			}
			require(static_cast<bool>(loadedPath), "Persisted profile lost its boarding opportunity");
			loadedAgent->setPath(loadedPath, true);
			for (unsigned tick = 0; tick < 18000 && loadedAgent->getState() != core::Agent::State::Idle; ++tick)
				restored->advanceTick();
			require(!loadedAgent->getPath() && loadedAgent->getGlobalPosition().distanceTo(loadedTarget->getPosition()) < 0.01f,
				"Persisted profile did not complete opportunistic transport journey");
		}
	}

	void landingAlternatives(unsigned kind)
	{
		core::World world("Landing alternatives", kind == 2 ? 256 : 20, 2);
		// Isolate authorization-driven replanning from unrelated long-queue triggers.
		auto policy = world.getTraversalWaitingPolicy();
		policy.minimumReplanWaitTicks = 1000000;
		world.setTraversalWaitingPolicy(policy);
		auto ground = kind == 1 ? world.addRoom("Platform room", 0, 0, 0, 20, 2)
			: world.addCorridor(0, 0, kind == 2 ? 256 : 20);
		auto upper = kind == 0 ? world.addCorridor(1, 0, 20) : ground;
		if (kind == 1) for (unsigned x = 0; x < 20; ++x) world.addSectorWalkway(ground, 1, x);
		uint32_t destination;
		world.addSectorMarker(upper, kind == 1 ? 1 : 0, kind == 2 ? 241.0f : 1.0f, &destination);
		world.finishBuild(); world.pauseSimulation();
		auto key = world.addAccessPermission("Protected landing");
		core::TraversalResourceId protectedVehicle;
		if (kind == 2)
		{
			core::World::CreateShuttleOptions options{ 1, 4, { 0, 240 }, 0 };
			options.landingControlPermissionRequirements = { { key }, {} };
			protectedVehicle = world.addShuttle(1, 0, 4, 252, options).traversalResource;
		}
		else
		{
			core::World::CreateLiftOptions options;
			options.cellsWide = kind == 1 ? 1 : 2; options.stopOffsets = { 0, 1 };
			options.landingControlPermissionRequirements = { { key }, {} };
			protectedVehicle = kind == 1 ? world.addSectorPlatformLift(ground, 0, 4, options).traversalResource
				: world.addLift(1, 0, 4, options).traversalResource;
			options.landingControlPermissionRequirements.clear();
			if (kind == 1) world.addSectorPlatformLift(ground, 0, 16, options);
			else world.addLift(1, 0, 16, options);
		}
		world.finishBuild(); world.pauseSimulation();
		std::string diagnostic;
		auto id = world.createAgent("Alternative boarder", ground, 0, 1.0f);
		if (kind == 2) require(world.setAgentIndividualWalkSpeedModifier(id, 0.8f, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(id, key, &diagnostic), diagnostic);
		auto agent = world.lookupAgent(id).entity;
		auto target = world.getGraph()->getVertexByIdentifier(destination);
		auto path = world.getGraph()->calculatePath(agent, target);
		auto usesProtectedVehicle = [&](std::shared_ptr<core::Path> const& route)
		{
			return route && std::any_of(route->nodes.begin(), route->nodes.end(), [&](auto const& node)
				{ return node.edge && node.edge->getTraversalResourceId() == protectedVehicle; });
		};
		require(usesProtectedVehicle(path), "Authorized landing alternative fixture did not choose nearby transport");
		require(world.resumeSimulation(), "Alternative fixture did not resume");
		agent->setPath(path, true);
		require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Alternative fixture grant loss failed");
		require(agent->getState() == core::Agent::State::RoutePlanning, "Grant loss did not reconsider future boarding");
		world.advanceTicks(200);
		require(agent->getPath() && !usesProtectedVehicle(agent->getPath()),
			"Landing authorization loss did not select an available alternative");
		for (unsigned tick = 0; tick < 66000 && agent->getState() != core::Agent::State::Idle; ++tick)
			world.advanceTick();
		std::string detail;
		for (auto const& request : world.getSimulationSnapshot().traversalRequests) detail += request.diagnostic;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost) detail += " lost: " + event.diagnostic;
		require(!agent->getPath() && agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.01f,
			"Landing authorization alternative did not complete safely: " + std::to_string(kind)
			+ " x " + std::to_string(agent->getGlobalPosition().x) + " y " + std::to_string(agent->getGlobalPosition().y)
			+ " state " + std::to_string(static_cast<int>(agent->getState())) + detail);
	}
}

void permission_smoke::registerLandingAdherence(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "landingJourneyLift0",
		[](smoke::Context const&)
		{
			landingJourney(0, 0);
		} });
	checks.push_back({ "landingJourneyLift1",
		[](smoke::Context const&)
		{
			landingJourney(0, 1);
		} });
	checks.push_back({ "landingJourneyLift2",
		[](smoke::Context const&)
		{
			landingJourney(0, 2);
		} });
	checks.push_back({ "landingJourneyLift3",
		[](smoke::Context const&)
		{
			landingJourney(0, 3);
		} });
	checks.push_back({ "landingJourneyLift4",
		[](smoke::Context const&)
		{
			landingJourney(0, 4);
		} });
	checks.push_back({ "landingJourneyLift5",
		[](smoke::Context const&)
		{
			landingJourney(0, 5);
		} });
	checks.push_back({ "landingJourneyLift6",
		[](smoke::Context const&)
		{
			landingJourney(0, 6);
		} });
	checks.push_back({ "landingJourneyLift7",
		[](smoke::Context const&)
		{
			landingJourney(0, 7);
		} });
	checks.push_back({ "landingAlternativesLift",
		[](smoke::Context const&)
		{
			landingAlternatives(0);
		} });
	checks.push_back({ "landingJourneyPlatform0",
		[](smoke::Context const&)
		{
			landingJourney(1, 0);
		} });
	checks.push_back({ "landingJourneyPlatform1",
		[](smoke::Context const&)
		{
			landingJourney(1, 1);
		} });
	checks.push_back({ "landingJourneyPlatform2",
		[](smoke::Context const&)
		{
			landingJourney(1, 2);
		} });
	checks.push_back({ "landingJourneyPlatform3",
		[](smoke::Context const&)
		{
			landingJourney(1, 3);
		} });
	checks.push_back({ "landingJourneyPlatform4",
		[](smoke::Context const&)
		{
			landingJourney(1, 4);
		} });
	checks.push_back({ "landingJourneyPlatform5",
		[](smoke::Context const&)
		{
			landingJourney(1, 5);
		} });
	checks.push_back({ "landingJourneyPlatform6",
		[](smoke::Context const&)
		{
			landingJourney(1, 6);
		} });
	checks.push_back({ "landingJourneyPlatform7",
		[](smoke::Context const&)
		{
			landingJourney(1, 7);
		} });
	checks.push_back({ "landingAlternativesPlatform",
		[](smoke::Context const&)
		{
			landingAlternatives(1);
		} });
	checks.push_back({ "landingJourneyShuttle0",
		[](smoke::Context const&)
		{
			landingJourney(2, 0);
		} });
	checks.push_back({ "landingJourneyShuttle1",
		[](smoke::Context const&)
		{
			landingJourney(2, 1);
		} });
	checks.push_back({ "landingJourneyShuttle2",
		[](smoke::Context const&)
		{
			landingJourney(2, 2);
		} });
	checks.push_back({ "landingJourneyShuttle3",
		[](smoke::Context const&)
		{
			landingJourney(2, 3);
		} });
	checks.push_back({ "landingJourneyShuttle4",
		[](smoke::Context const&)
		{
			landingJourney(2, 4);
		} });
	checks.push_back({ "landingJourneyShuttle5",
		[](smoke::Context const&)
		{
			landingJourney(2, 5);
		} });
	checks.push_back({ "landingJourneyShuttle6",
		[](smoke::Context const&)
		{
			landingJourney(2, 6);
		} });
	checks.push_back({ "landingJourneyShuttle7",
		[](smoke::Context const&)
		{
			landingJourney(2, 7);
		} });
	checks.push_back({ "landingAlternativesShuttle",
		[](smoke::Context const&)
		{
			landingAlternatives(2);
		} });
}
