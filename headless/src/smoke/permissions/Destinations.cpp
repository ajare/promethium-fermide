#include "Checks.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/SerializationException.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Edge.h"
#include "core/Path.h"
#include "core/Vertex.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	core::World::CreateLiftResult permissionLift(core::World& world, bool platform,
		uint32_t room, uint32_t x, core::World::CreateLiftOptions options)
	{
		if (!platform) return world.addLift(1, 0, x, options);
		options.cellsWide = 1;
		options.platformStopDurationSeconds = 20.0f;
		auto created = world.addSectorPlatformLift(room, 0, x, options);
		return { created.lift, {}, created.traversalResource, created.interiorSelector };
	}

	void liftDestinationEnforcement(unsigned authorizationChange = 0, bool platform = false, bool shuttle = false)
	{
		core::World world("destination enforcement", shuttle ? 64 : 16, 3);
		auto ground = platform ? world.addRoom("Platform room", 0, 0, 0, 16, 3) : world.addCorridor(0, 0, shuttle ? 9 : 16);
		auto middle = platform ? ground : world.addCorridor(shuttle ? 0 : 1, shuttle ? 24 : 0, 16);
		auto upper = platform ? ground : world.addCorridor(shuttle ? 0 : 2, shuttle ? 48 : 0, 16);
		auto riderOrigin = shuttle ? world.addRoom("Second Carriage landing", 0, 0, 9, 7, 1) : ground;
		if (platform) for (uint32_t y = 1; y < 3; ++y)
			for (uint32_t x = 0; x < 16; ++x) world.addSectorWalkway(ground, y, x);
		uint32_t startMarker, middleMarker, upperMarker;
		world.addSectorMarker(ground, 0, 7.0f, &startMarker);
		world.addSectorMarker(middle, platform ? 1 : 0, 7.0f, &middleMarker);
		world.addSectorMarker(upper, platform ? 2 : 0, 7.0f, &upperMarker);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2; options.stopOffsets = { 0, 1, 2 };
		options.minimumDwellSeconds = 20.0f;
		options.maximumBoardingSeconds = 30.0f;
		core::World::CreateLiftResult lift;
		if (shuttle)
		{
			core::World::CreateShuttleOptions shuttleOptions{ 2, 4, { 0, 24, 48 }, 0 };
			shuttleOptions.capacity = 2;
			shuttleOptions.minimumDwellSeconds = 20.0f;
			shuttleOptions.maximumBoardingSeconds = 30.0f;
			auto created = world.addShuttle(1, 0, 4, 60, shuttleOptions);
			lift = { created.shuttle, created.doors, created.traversalResource, created.interiorSelector };
		}
		else lift = permissionLift(world, platform, ground, 8, options);
		auto object = platform ? lift.lift.index : ~0u;
		world.finishBuild(); world.pauseSimulation();
		auto red = world.addAccessPermission("Destination red");
		auto blue = world.addAccessPermission("Destination blue");
		std::string diagnostic;
		require(world.setLiftDestinationPermissionRequirement(lift.lift.sector->getIndex(), 1,
			{ red, blue }, &diagnostic, object), diagnostic);
		auto remoteId = world.createAgent("remote observer", upper, platform ? 2 : 0, 7.0f);
		auto riderId = world.createAgent("piggyback rider", riderOrigin, 0, shuttle ? 1.5f : 7.0f);
		auto operatorId = world.createAgent("destination operator", ground, 0, 7.0f);
		// Destination piggybacking is intentional only for an explicitly
		// non-adhering passenger; default-true passengers decline it.
		require(world.setAgentIndividualPermissionAdherence(riderId, false, &diagnostic), diagnostic);
		auto rider = world.lookupAgent(riderId).entity;
		require(!rider->getEffectivePermissionAdherence().value,
			"Piggyback fixture did not apply non-adhering profile");
		auto graph = world.getGraph();
		auto start = graph->getVertexByIdentifier(startMarker);
		auto target = graph->getVertexByIdentifier(middleMarker);
		require(!graph->calculatePath(rider, start, target), "Unauthorized destination remained routable");
		require(static_cast<bool>(graph->calculatePath(rider, start, graph->getVertexByIdentifier(upperMarker))),
			"Protected intermediate Stop blocked an unrestricted destination");

		core::DeviceCommand select;
		select.type = shuttle ? core::DeviceCommandType::SelectShuttleDestination : core::DeviceCommandType::SelectLiftDestination;
		select.traversalResource = lift.traversalResource;
		select.stopIndex = 1;
		require(world.missingLiftDestinationPermissions(select, {}).empty(), "Agentless selection did not bypass authorization");
		require(world.missingLiftDestinationPermissions(select, core::AgentId{ 99999 })
			== std::vector<core::AccessPermissionId>{ red, blue }, "Invalid Agent attribution bypassed authorization");
		auto point = world.createInteractionPoint("alternate destination entry point",
			core::SectorId{ ground + 1 }, { 7.0f, 0.0f }, 0.25f, world.getFixedTimestep(),
			{ { select, core::InteractionBindingRequirement::Required } });
		require(world.resumeSimulation(), "Lift fixture did not resume");
		require(world.moveAgentToMarker(riderId, world.getMarkerIds()[1]).accepted(), "Lift movement intent refused");
		world.advanceTicks(300);
		bool routeLost = false;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost)
				routeLost = event.routeLossReason == core::RouteLossReason::Unreachable;
		require(routeLost && !rider->getPath(), "Protected destination did not produce established Route loss");
		auto rejected = world.requestInteraction(point, operatorId);
		require(static_cast<bool>(rejected), "Destination command was not admitted for authorization check");
		require(world.lookupInteractionRequest(rejected).entity->getMissingPermissions()
			== std::vector<core::AccessPermissionId>{ red, blue }, "Command entry point bypassed destination authorization");
		world.pauseSimulation();
		require(world.grantAgentAccessPermission(operatorId, red, &diagnostic), diagnostic);
		world.resumeSimulation();
		rejected = world.requestInteraction(point, operatorId);
		require(static_cast<bool>(rejected), "Partial-grant command was not admitted");
		require(world.lookupInteractionRequest(rejected).entity->getResult() == core::InteractionResult::Rejected
			&& world.lookupInteractionRequest(rejected).entity->getMissingPermissions()
			== std::vector<core::AccessPermissionId>{ blue }, "Partial grants authorized a destination");
		world.pauseSimulation();
		auto set = world.addPermissionSet("Destination operators");
		require(world.setPermissionSetAccessPermission(set, blue, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(operatorId, set, true, &diagnostic), diagnostic);
		world.resumeSimulation();
		core::DeviceCommand call;
		call.type = shuttle ? core::DeviceCommandType::CallShuttle : core::DeviceCommandType::CallLift;
		call.traversalResource = lift.traversalResource; call.stopIndex = 0;
		auto landing = world.createInteractionPoint("Independent landing call", core::SectorId{ riderOrigin + 1 },
			{ shuttle ? 10.5f : 7.0f, 0.0f }, 0.25f, world.getFixedTimestep(), { { call, core::InteractionBindingRequirement::Required } });
		require(static_cast<bool>(world.requestInteraction(landing, riderId)),
			"Independent unrestricted landing call was refused");
		world.advanceTicks(180);
		require(world.setAgentRuntimeAccessPermissionGrant(operatorId, red, false), "Runtime grant removal failed");
		auto currentRejected = world.requestInteraction(point, operatorId);
		require(currentRejected && world.lookupInteractionRequest(currentRejected).entity->getMissingPermissions()
			== std::vector<core::AccessPermissionId>{ red }, "Command used initial rather than current grants");
		require(world.setAgentRuntimeAccessPermissionGrant(operatorId, red, true), "Runtime grant restoration failed");
		auto accepted = world.requestInteraction(point, operatorId);
		require(static_cast<bool>(accepted), "Authorized command was not admitted");
		require(world.lookupInteractionRequest(accepted).entity->getResult() == core::InteractionResult::Pending,
			"Direct and Permission set union did not authorize destination");
		auto riderPoint = shuttle ? world.createInteractionPoint("Other Carriage destination entry point",
			core::SectorId{ riderOrigin + 1 }, { 10.5f, 0.0f }, 0.25f, world.getFixedTimestep(),
			{ { select, core::InteractionBindingRequirement::Required } }) : point;
		auto coalesced = world.requestInteraction(riderPoint, riderId);
		require(coalesced && world.lookupInteractionRequest(coalesced).entity->getResult() == core::InteractionResult::Rejected,
			"Unauthorized command joined an already accepted destination operation");
		require(world.lookupInteractionRequest(accepted).entity->getResult() == core::InteractionResult::Pending,
			"Refused selection cancelled another Agent's accepted operation");
		world.advanceTicks(60);
		auto operatorAgent = world.lookupAgent(operatorId).entity;
		operatorAgent->setPath(graph->calculatePath(operatorAgent, target), true);
		std::shared_ptr<core::Path> shared;
		for (unsigned tick = 0; tick < 3000 && !shared; ++tick)
		{
			world.advanceTicks(1);
			shared = graph->calculatePath(rider, target);
		}
		for (auto const& resource : world.getSimulationSnapshot().traversalResources)
			if (resource.id == lift.traversalResource)
				require(static_cast<bool>(shared), "Locally observed accepted Stop request was not routable: phase "
					+ std::to_string(static_cast<int>(resource.liftStopPhase)) + " stop "
					+ std::to_string(resource.liftCurrentStop) + " position " + std::to_string(rider->getGlobalPosition().y));
		require(!graph->calculatePath(world.lookupAgent(remoteId).entity, target),
			"Remote Agent relied on another landing's live shared Stop request");
		// The same local accepted journey is declined by an adhering unauthorized
		// passenger, while effective direct and Permission set grants admit it.
		world.pauseSimulation();
		auto adheringId = world.createAgent("adhering destination observer", riderOrigin, 0, 6.0f);
		auto adhering = world.lookupAgent(adheringId).entity;
		require(adhering->getEffectivePermissionAdherence().value
			&& !graph->calculatePath(adhering, target),
			"Adhering Agent accepted a protected destination piggyback journey");
		require(world.grantAgentAccessPermission(adheringId, red, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(adheringId, set, true, &diagnostic), diagnostic);
		require(static_cast<bool>(graph->calculatePath(adhering, target)),
			"Effective direct and Permission set grants did not admit the protected destination");
		require(world.removeAgent(adheringId).removed, "Destination observer removal failed");
		require(world.resumeSimulation(), "Destination adherence fixture did not resume");
		world.consumeSimulationEvents();
		if (authorizationChange == 5)
		{
			world.pauseSimulation();
			require(world.setAgentIndividualPermissionAdherence(riderId, true, &diagnostic), diagnostic);
			require(world.resumeSimulation(), "Pre-boarding adherence change did not resume");
		}
		rider->setPath(shared, true);
		if (authorizationChange && authorizationChange != 5)
		{
			// Both passengers must have boarded before changing authorization;
			// the operator's accepted selection is now a shared Stop request.
			auto bothOnboard = [&]
			{
				for (auto const& resource : world.getSimulationSnapshot().traversalResources)
					if (resource.id == lift.traversalResource)
						return std::any_of(resource.capacityPositions.begin(), resource.capacityPositions.end(),
							[&](auto const& position) { return position.occupant == operatorId; })
							&& std::any_of(resource.capacityPositions.begin(), resource.capacityPositions.end(),
								[&](auto const& position) { return position.occupant == riderId; });
				return false;
			};
			for (unsigned tick = 0; tick < 1200 && !bothOnboard(); ++tick) world.advanceTicks(1);
			require(bothOnboard(),
				"Shared passengers did not board before authorization change");
			if (shuttle)
				for (auto const& resource : world.getSimulationSnapshot().traversalResources)
					if (resource.id == lift.traversalResource)
						require(resource.shuttleCarriages.size() == 2
							&& resource.shuttleCarriages[0].occupantCount == 1
							&& resource.shuttleCarriages[1].occupantCount == 1,
							"Shared destination fixture did not occupy both coupled Carriages");
			if (authorizationChange == 1)
			{
				require(world.setAgentRuntimeAccessPermissionGrant(operatorId, red, false), "Onboard runtime revoke failed");
				require(world.setAgentRuntimePermissionSetAssignment(operatorId, set, false), "Onboard set revoke failed");
			}
			else
			{
				world.pauseSimulation();
				if (authorizationChange == 4)
					require(world.setAgentIndividualPermissionAdherence(riderId, true, &diagnostic), diagnostic);
				else if (authorizationChange == 2)
				{
					auto extra = world.addAccessPermission("Tightened destination");
					require(world.setLiftDestinationPermissionRequirement(lift.lift.sector->getIndex(), 1,
						{ red, blue, extra }, &diagnostic, object), diagnostic);
				}
				else
				{
					require(world.revokeAgentAccessPermission(operatorId, red, &diagnostic), diagnostic);
					require(world.setPermissionSetAccessPermission(set, blue, false, &diagnostic), diagnostic);
				}
				require(world.resumeSimulation(), "Tightened onboard requirement did not resume");
			}
		}
		world.advanceTicks(shuttle ? 18000 : 6000);
		std::string riderDiagnostic;
		for (auto const& event : world.consumeSimulationEvents())
		{
			if (event.type == core::SimulationEventType::RouteLost && event.agent.id == riderId)
				riderDiagnostic += " route lost " + event.diagnostic;
			require(!shuttle || event.type != core::SimulationEventType::RouteLost
				|| (authorizationChange == 5 && event.agent.id == riderId),
				"Accepted shared Shuttle journey produced Route loss");
			if (event.type == core::SimulationEventType::DeviceOperationAdded)
				require(event.deviceOperation.requester != riderId
					|| event.deviceOperation.command.type != select.type,
					"Piggyback rider attempted a protected destination selection");
		}
		for (auto const& request : world.getSimulationSnapshot().traversalRequests)
			if (request.owner == riderId) riderDiagnostic += " / " + request.diagnostic
				+ " state " + std::to_string(static_cast<int>(request.state));
		if (authorizationChange == 5)
			require(rider->getSector()->getIndex() == riderOrigin && !rider->getPath()
				&& riderDiagnostic.find("route lost") != std::string::npos,
				"Pre-boarding adherence change bypassed destination admission" + riderDiagnostic);
		else require(rider->getSector()->getIndex() == middle && !rider->getPath()
			&& std::abs(rider->getGlobalPosition().y - (shuttle ? 0.0f : 1.0f)) < 0.01f,
			"Unauthorized rider did not complete shared journey and disembark: " + std::to_string(authorizationChange)
			+ " platform " + std::to_string(platform) + " shuttle " + std::to_string(shuttle)
			+ " sector " + std::to_string(rider->getSector()->getIndex()) + " x " + std::to_string(rider->getGlobalPosition().x)
			+ " y " + std::to_string(rider->getGlobalPosition().y)
			+ " state " + std::to_string(static_cast<int>(rider->getState())) + riderDiagnostic);
		std::string journeyDiagnostic;
		for (auto const& resource : world.getSimulationSnapshot().traversalResources)
			if (resource.id == lift.traversalResource)
				journeyDiagnostic += " stop " + std::to_string(resource.liftCurrentStop)
					+ " phase " + std::to_string(static_cast<int>(resource.liftStopPhase));
		for (auto const& interaction : world.getSimulationSnapshot().interactionRequests)
			if (interaction.actor == operatorId && interaction.result == core::InteractionResult::Pending)
				journeyDiagnostic += " interaction " + std::to_string(interaction.point.value);
		for (auto const& request : world.getSimulationSnapshot().traversalRequests)
			journeyDiagnostic += " / " + request.diagnostic + " edge " + std::to_string(static_cast<int>(request.edgeType))
				+ " from " + std::to_string(request.sourceEndpoint.x) + " to " + std::to_string(request.destinationEndpoint.x);
		require(operatorAgent->getSector()->getIndex() == middle && !operatorAgent->getPath()
			&& std::abs(operatorAgent->getGlobalPosition().y - (shuttle ? 0.0f : 1.0f)) < 0.01f,
			"Operator did not disembark after accepted selection: change " + std::to_string(authorizationChange)
			+ " shuttle " + std::to_string(shuttle) + " sector " + std::to_string(operatorAgent->getSector()->getIndex())
			+ " x " + std::to_string(operatorAgent->getGlobalPosition().x)
			+ " state " + std::to_string(static_cast<int>(operatorAgent->getState())) + journeyDiagnostic);
		if (shuttle && authorizationChange == 0)
		{
			world.pauseSimulation();
			require(world.grantAgentAccessPermission(remoteId, red, &diagnostic), diagnostic);
			require(world.setAgentPermissionSetAssignment(remoteId, set, true, &diagnostic), diagnostic);
			auto authored = save(world);
			require(world.setAgentRuntimeAccessPermissionGrant(remoteId, red, false), "Shuttle runtime revoke failed");
			require(world.setAgentRuntimePermissionSetAssignment(remoteId, set, false), "Shuttle runtime set revoke failed");
			require(world.missingLiftDestinationPermissions(select, remoteId) == std::vector<core::AccessPermissionId>{ red, blue }
				&& save(world) == authored, "Shuttle runtime grants changed authored data");
			world.resetSimulation();
			require(world.missingLiftDestinationPermissions(select, remoteId).empty(), "Shuttle Reset lost authored grants");
			world.resumeSimulation();
			require(world.moveAgentToMarker(remoteId, world.getMarkerIds()[1]).accepted(), "Second origin movement refused");
			require(world.moveAgentToMarker(riderId, world.getMarkerIds()[2]).accepted(), "Unrestricted far Stop refused");
			world.advanceTicks(36000);
			rider = world.lookupAgent(riderId).entity;
			require(rider->getSector()->getIndex() == upper && !rider->getPath(),
				"Protected intermediate Shuttle Stop blocked completed journey");
			auto remote = world.lookupAgent(remoteId).entity;
			require(remote->getSector()->getIndex() == middle && !remote->getPath()
				&& std::abs(remote->getGlobalPosition().x - 31.0f) < 0.01f,
				"Authorized journey from second Shuttle origin did not complete");
		}
	}

	void liftDestinationAlternative()
	{
		core::World world("alternative Lift", 16, 2);
		auto ground = world.addCorridor(0, 0, 16);
		auto upper = world.addCorridor(1, 0, 16);
		uint32_t from, to;
		world.addSectorMarker(ground, 0, 1.0f, &from);
		world.addSectorMarker(upper, 0, 1.0f, &to);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2; options.stopOffsets = { 0, 1 };
		auto protectedLift = world.addLift(1, 0, 4, options);
		auto alternative = world.addLift(1, 0, 12, options);
		world.finishBuild(); world.pauseSimulation();
		auto key = world.addAccessPermission("Protected destination");
		std::string diagnostic;
		require(world.setLiftDestinationPermissionRequirement(protectedLift.lift.sector->getIndex(), 1,
			{ key }, &diagnostic), diagnostic);
		auto id = world.createAgent("alternative rider", ground, 0, 1.0f);
		auto agent = world.lookupAgent(id).entity;
		auto graph = world.getGraph();
		auto path = graph->calculatePath(agent, graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to));
		require(static_cast<bool>(path), "Valid alternative Lift route was lost");
		bool usesAlternative = false;
		for (auto const& node : path->nodes)
			if (node.edge)
			{
				require(node.edge->getTraversalResourceId() != protectedLift.traversalResource,
					"Route selected a Lift requiring unauthorized destination selection");
				usesAlternative |= node.edge->getTraversalResourceId() == alternative.traversalResource;
			}
		require(usesAlternative, "Route did not choose the available alternative");
		agent->setPath(path, true);
		world.resumeSimulation(); world.advanceTicks(6000);
		require(agent->getSector()->getIndex() == upper && !agent->getPath(),
			"Alternative unrestricted destination journey did not complete");
	}

	void changingLiftDestinationAuthorization(bool platform = false, bool shuttle = false)
	{
		// Exercise each grant source and authored requirement edits against both
		// a replacement Path and Route loss, before any selection is accepted.
		for (bool alternative : { false, true })
		for (unsigned change = 0; change < 6; ++change)
		{
			core::World world("changing destination", shuttle ? 256 : 20, 2);
			auto ground = platform ? world.addRoom("Platform room", 0, 0, 0, 20, 2)
				: world.addCorridor(0, 0, shuttle ? (alternative ? 256 : 16) : 20);
			auto upper = platform || (shuttle && alternative) ? ground
				: world.addCorridor(shuttle ? 0 : 1, shuttle ? 240 : 0, shuttle ? 16 : 20);
			if (platform) for (uint32_t x = 0; x < 20; ++x) world.addSectorWalkway(ground, 1, x);
			uint32_t from, to;
			world.addSectorMarker(ground, 0, 1.0f, &from);
			world.addSectorMarker(upper, platform ? 1 : 0, shuttle && alternative ? 241.0f : 1.0f, &to);
			core::World::CreateLiftOptions options;
			options.cellsWide = 2; options.stopOffsets = { 0, 1 };
			core::World::CreateLiftResult lift;
			if (shuttle)
			{
				auto created = world.addShuttle(1, 0, 4, 252, { 2, 4, { 0, 240 }, 0 });
				lift = { created.shuttle, created.doors, created.traversalResource, created.interiorSelector };
			}
			else lift = permissionLift(world, platform, ground, 4, options);
			auto object = platform ? lift.lift.index : ~0u;
			core::TraversalResourceId other;
			if (alternative && !shuttle) other = permissionLift(world, platform, ground, 16, options).traversalResource;
			world.finishBuild(); world.pauseSimulation();
			auto key = world.addAccessPermission("Destination key");
			auto unrelated = world.addAccessPermission("Unrelated key");
			auto set = world.addPermissionSet("Operators");
			auto id = world.createAgent("rider", ground, 0, 1.0f);
			auto agent = world.lookupAgent(id).entity;
			std::string diagnostic;
			require(world.setAgentIndividualRoutePersistence(id, 1.0f, &diagnostic), diagnostic);
			if (shuttle) require(world.setAgentIndividualWalkSpeedModifier(id, 0.8f, &diagnostic), diagnostic);
			require(world.setPermissionSetAccessPermission(set, key, true, &diagnostic), diagnostic);
			if (change < 2) require(world.grantAgentAccessPermission(id, key, &diagnostic), diagnostic);
			else if (change < 5) require(world.setAgentPermissionSetAssignment(id, set, true, &diagnostic), diagnostic);
			if (change != 5) require(world.setLiftDestinationPermissionRequirement(
				lift.lift.sector->getIndex(), 1, { key }, &diagnostic, object), diagnostic);
			auto graph = world.getGraph();
			auto path = graph->calculatePath(agent, graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to));
			require(static_cast<bool>(path), "Initially authorized Lift Path missing");
			agent->setPath(path, true);
			require(world.setAgentRuntimeAccessPermissionGrant(id, unrelated, true), "Unrelated grant failed");
			require(agent->getPath() == path, "Unrelated grant disturbed Lift Path");
			switch (change)
			{
			case 0: require(world.revokeAgentAccessPermission(id, key, &diagnostic), diagnostic); break;
			case 1: require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Runtime revoke failed"); break;
			case 2: require(world.setAgentPermissionSetAssignment(id, set, false, &diagnostic), diagnostic); break;
			case 3: require(world.setAgentRuntimePermissionSetAssignment(id, set, false), "Runtime set removal failed"); break;
			case 4: require(world.setPermissionSetAccessPermission(set, key, false, &diagnostic), diagnostic); break;
			case 5: require(world.setLiftDestinationPermissionRequirement(lift.lift.sector->getIndex(), 1, { key }, &diagnostic, object), diagnostic); break;
			}
			require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
				"Destination authorization loss did not invalidate Path: " + std::to_string(change));
			require(world.resumeSimulation(), "Changing authorization fixture did not resume");
			world.advanceTicks(agent->getRoutePlanningRemainingTicks());
			if (!alternative)
			{
				bool lost = false;
				for (auto const& event : world.consumeSimulationEvents())
					lost |= event.type == core::SimulationEventType::RouteLost
						&& event.routeLossReason == core::RouteLossReason::Unreachable;
				require(lost && !agent->getPath(), "Destination loss without alternative did not produce Route loss");
				continue;
			}
			auto uses = [](std::shared_ptr<core::Path> const& value, core::TraversalResourceId resource)
			{
				if (value) for (auto const& node : value->nodes)
					if (node.edge && node.edge->getTraversalResourceId() == resource) return true;
				return false;
			};
			require(shuttle ? agent->getPath() && !uses(agent->getPath(), lift.traversalResource)
				: uses(agent->getPath(), other), "Authorization loss did not select alternative");
			require(world.setAgentRuntimeAccessPermissionGrant(id, key, true), "Runtime gain failed");
			require(agent->getState() == core::Agent::State::RoutePlanning, "Destination gain did not plan voluntarily");
			world.advanceTicks(agent->getRoutePlanningRemainingTicks());
			require(shuttle ? agent->getPath() && !uses(agent->getPath(), lift.traversalResource)
				: uses(agent->getPath(), other), "Destination gain ignored Route persistence");
			world.advanceTicks(shuttle ? 48000 : 6000);
			require(agent->getSector()->getIndex() == upper && !agent->getPath()
				&& std::abs(agent->getGlobalPosition().y - (shuttle ? 0.0f : 1.0f)) < 0.01f, "Replacement journey did not complete");
		}
	}

	void platformDestinationResetAndIntermediateJourney()
	{
		core::World world("Platform Reset", 10, 3);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 3);
		for (uint32_t y = 1; y < 3; ++y)
			for (uint32_t x = 0; x < 10; ++x) world.addSectorWalkway(room, y, x);
		uint32_t top;
		world.addSectorMarker(room, 2, 1.0f, &top);
		core::World::CreateLiftOptions options; options.stopOffsets = { 0, 1, 2 };
		auto lift = world.addSectorPlatformLift(room, 0, 8, options);
		world.finishBuild(); world.pauseSimulation();
		auto red = world.addAccessPermission("Red");
		auto blue = world.addAccessPermission("Blue");
		auto set = world.addPermissionSet("Card");
		auto id = world.createAgent("Rider", room, 0, 1.0f);
		std::string diagnostic;
		require(world.setLiftDestinationPermissionRequirement(room, 0, { red, blue }, &diagnostic, lift.lift.index), diagnostic);
		require(world.setLiftDestinationPermissionRequirement(room, 1, { red, blue }, &diagnostic, lift.lift.index), diagnostic);
		require(world.grantAgentAccessPermission(id, red, &diagnostic), diagnostic);
		require(world.setPermissionSetAccessPermission(set, blue, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(id, set, true, &diagnostic), diagnostic);
		core::DeviceCommand select;
		select.type = core::DeviceCommandType::SelectLiftDestination;
		select.traversalResource = lift.traversalResource; select.stopIndex = 0;
		require(world.missingLiftDestinationPermissions(select, id).empty(), "Authored grants did not authorize ground");
		require(world.agentAdheresToLiftDestinationPermission(lift.traversalResource, 0, id),
			"Direct and Permission set grants did not satisfy mandatory-ground adherence");
		auto authored = save(world);
		require(world.setAgentRuntimeAccessPermissionGrant(id, red, false), "Runtime revoke failed");
		require(world.setAgentRuntimePermissionSetAssignment(id, set, false), "Runtime set revoke failed");
		require(world.missingLiftDestinationPermissions(select, id) == std::vector<core::AccessPermissionId>{ red, blue }
			&& save(world) == authored, "Runtime grants leaked into authored Platform authorization");
		require(!world.agentAdheresToLiftDestinationPermission(lift.traversalResource, 0, id),
			"Adhering unauthorized Agent accepted the protected mandatory ground destination");
		require(world.setAgentIndividualPermissionAdherence(id, false, &diagnostic), diagnostic);
		require(world.agentAdheresToLiftDestinationPermission(lift.traversalResource, 0, id),
			"Non-adhering Agent declined the protected mandatory ground destination");
		require(world.setAgentIndividualPermissionAdherence(id, true, &diagnostic), diagnostic);
		world.resetSimulation();
		require(world.missingLiftDestinationPermissions(select, id).empty(), "Reset failed to restore Platform authorization");
		require(world.setAgentRuntimeAccessPermissionGrant(id, red, false), "Runtime revoke failed");
		require(world.setAgentRuntimePermissionSetAssignment(id, set, false), "Runtime set revoke failed");
		auto rider = world.lookupAgent(id).entity;
		world.resumeSimulation();
		require(world.moveAgentToMarker(id, world.getMarkerIds().front()).accepted(), "Upper Platform destination refused");
		world.advanceTicks(6000);
		require(!rider->getPath() && std::abs(rider->getGlobalPosition().y - 2.0f) < 0.01f,
			"Unauthorized passenger did not travel past protected intermediate Platform Stop");
	}
}

void permission_smoke::registerDestinations(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "platformDestinationResetAndIntermediateJourney",
		[](smoke::Context const&)
		{
			platformDestinationResetAndIntermediateJourney();
		} });
	checks.push_back({ "destinationEnforcementLift0",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(0, false, false);
		} });
	checks.push_back({ "destinationEnforcementLift1",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(1, false, false);
		} });
	checks.push_back({ "destinationEnforcementLift2",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(2, false, false);
		} });
	checks.push_back({ "destinationEnforcementLift3",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(3, false, false);
		} });
	checks.push_back({ "destinationEnforcementLift4",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(4, false, false);
		} });
	checks.push_back({ "destinationEnforcementLift5",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(5, false, false);
		} });
	checks.push_back({ "changingDestinationAuthorizationLift",
		[](smoke::Context const&)
		{
			changingLiftDestinationAuthorization(false, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform0",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(0, true, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform1",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(1, true, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform2",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(2, true, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform3",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(3, true, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform4",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(4, true, false);
		} });
	checks.push_back({ "destinationEnforcementPlatform5",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(5, true, false);
		} });
	checks.push_back({ "changingDestinationAuthorizationPlatform",
		[](smoke::Context const&)
		{
			changingLiftDestinationAuthorization(true, false);
		} });
	checks.push_back({ "destinationEnforcementShuttle0",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(0, false, true);
		} });
	checks.push_back({ "destinationEnforcementShuttle1",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(1, false, true);
		} });
	checks.push_back({ "destinationEnforcementShuttle2",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(2, false, true);
		} });
	checks.push_back({ "destinationEnforcementShuttle3",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(3, false, true);
		} });
	checks.push_back({ "destinationEnforcementShuttle4",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(4, false, true);
		} });
	checks.push_back({ "destinationEnforcementShuttle5",
		[](smoke::Context const&)
		{
			liftDestinationEnforcement(5, false, true);
		} });
	checks.push_back({ "changingDestinationAuthorizationShuttle",
		[](smoke::Context const&)
		{
			changingLiftDestinationAuthorization(false, true);
		} });
	checks.push_back({ "liftDestinationAlternative",
		[](smoke::Context const&)
		{
			liftDestinationAlternative();
		} });
}
