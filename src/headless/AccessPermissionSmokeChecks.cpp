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
#include "imgui/imgui.h"
#include "imgui_internal.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"

void runAccessPermissionSmokeChecks();
void runPermissionAdherenceSmokeChecks();
void runTransportLandingAdherenceSmokeChecks();

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

	std::shared_ptr<core::World> load(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("target", 1, 1);
		core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(world->deserialize(*reader, work), "permission World did not load");
		return world;
	}

	void authorizationAndPersistence()
	{
		core::World world("permissions", 3, 2);
		auto corridor = world.addCorridor(0, 0, 3);
		auto control = world.addSectorLightSwitch(corridor, 1);
		world.finishBuild(); world.pauseSimulation();
		auto agent = world.createAgent("operator", corridor, 0, 1.5f);
		auto red = world.addAccessPermission("  Red key  ");
		auto blue = world.addAccessPermission("Blue key");
		std::string diagnostic;
		require(world.setInteractionPointPermissionRequirement(control.interactionPoint,
			{ red, blue }, &diagnostic), diagnostic);

		world.resumeSimulation();
		auto denied = world.requestInteraction(control.interactionPoint, agent);
		require(static_cast<bool>(denied), "unauthorized request had no observable outcome");
		auto rejected = world.lookupInteractionRequest(denied);
		require(rejected && rejected.entity->getResult() == core::InteractionResult::Rejected,
			"unauthorized request was not rejected");
		require(rejected.entity->getMissingPermissions().size() == 2,
			"rejection did not identify every missing permission");
		require(rejected.entity->getOperations().empty(), "rejection created a Device operation");
		require(world.getSimulationSnapshot().deviceOperations.empty(), "rejection changed device coordination");

		world.pauseSimulation();
		require(world.grantAgentAccessPermission(agent, red, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(agent, blue, &diagnostic), diagnostic);
		require(world.resumeSimulation(), "authorized fixture did not resume");
		auto authorized = world.requestInteraction(control.interactionPoint, agent);
		require(static_cast<bool>(authorized), "authorized request was refused");
		auto admitted = world.lookupInteractionRequest(authorized);
		require(admitted && admitted.entity->getResult() == core::InteractionResult::Pending
			&& !admitted.entity->getOperations().empty(), "authorized request did not operate normally");
		world.advanceTicks(900);
		require(!world.getSector(corridor)->areLightsOn(), "authorized Agent did not operate the protected point");
		world.pauseSimulation();
		auto yaml = save(world);
		auto restored = load(yaml);
		require(restored->getAccessPermissionCount() == 2, "definitions did not persist");
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 2, "grants did not persist");
		require(restored->getInteractionPointPermissionRequirement(control.interactionPoint).size() == 2,
			"requirement did not persist");

		restored->pauseSimulation();
		auto usage = restored->getAccessPermissionUsage(red);
		require(usage.directAgentGrants == 1 && usage.interactionPointRequirements == 1,
			"authoritative usage counts are wrong");
		require(restored->deleteAccessPermission(red, &diagnostic), diagnostic);
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 1
			&& restored->getInteractionPointPermissionRequirement(control.interactionPoint).size() == 1,
			"deletion did not atomically clear references");
		auto reused = restored->addAccessPermission("Green key");
		require(reused == red, "cleared fixed-capacity slot was not reusable");
		require(restored->getAgentEffectiveAccessGrants(agent).size() == 1,
			"reused identity inherited an old grant");
	}

	void permissionSets()
	{
		core::World world("Permission sets", 3, 1);
		auto corridor = world.addCorridor(0, 0, 3);
		auto control = world.addSectorLightSwitch(corridor, 1);
		world.finishBuild(); world.pauseSimulation();
		auto agent = world.createAgent("operator", corridor, 0, 1.5f);
		auto red = world.addAccessPermission("Red key");
		auto blue = world.addAccessPermission("Blue key");
		auto redSet = world.addPermissionSet("  Staff  ");
		auto bothSet = world.addPermissionSet("Operations");
		std::string diagnostic;
		require(world.getPermissionSetName(redSet) == "Staff", "Permission set name was not trimmed");
		require(world.setPermissionSetAccessPermission(redSet, red, true, &diagnostic), diagnostic);
		require(world.setPermissionSetAccessPermission(bothSet, red, true, &diagnostic), diagnostic);
		require(world.setPermissionSetAccessPermission(bothSet, blue, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(agent, redSet, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(agent, bothSet, true, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(agent, red, &diagnostic), diagnostic);
		require(world.getAgentEffectiveAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ red, blue },
			"effective grants were not the union of direct and set sources");
		auto sources = world.getAgentAccessGrantSources(agent, red);
		require(sources.direct && sources.permissionSets
			== std::vector<core::PermissionSetId>{ redSet, bothSet },
			"effective grant sources were not reported in stable order");

		// Removing either overlapping source must preserve the effective grant.
		require(world.revokeAgentAccessPermission(agent, red, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(agent, redSet, false, &diagnostic), diagnostic);
		require(world.getAgentEffectiveAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ red, blue },
			"removing overlapping sources removed an effective grant");
		require(world.setInteractionPointPermissionRequirement(control.interactionPoint,
			{ red, blue }, &diagnostic), diagnostic);
		require(world.resumeSimulation(), "Permission set authorization fixture did not resume");
		auto request = world.requestInteraction(control.interactionPoint, agent);
		auto admitted = world.lookupInteractionRequest(request);
		require(admitted && admitted.entity->getResult() == core::InteractionResult::Pending,
			"Permission set grants did not authorize a real operation");
		world.pauseSimulation();

		auto stableId = bothSet;
		require(world.renamePermissionSet(bothSet, "Operations team", &diagnostic), diagnostic);
		require(world.lookupPermissionSet(stableId)
			&& world.getPermissionSetName(stableId) == "Operations team",
			"Permission set rename did not preserve identity");
		auto usage = world.getAccessPermissionUsage(red);
		require(usage.permissionSetMemberships == 2,
			"Access permission usage omitted Permission set memberships");

		auto restored = load(save(world));
		require(restored->getPermissionSetCount() == 2
			&& restored->getPermissionSetPermissions(stableId)
				== std::vector<core::AccessPermissionId>{ red, blue }
			&& restored->getAgentPermissionSetAssignments(agent)
				== std::vector<core::PermissionSetId>{ stableId },
			"Permission sets or assignments did not survive save/load");
		restored->pauseSimulation();
		require(restored->deleteAccessPermission(red, &diagnostic), diagnostic);
		require(restored->getPermissionSetPermissions(redSet).empty()
			&& restored->getPermissionSetPermissions(stableId)
				== std::vector<core::AccessPermissionId>{ blue },
			"Access permission deletion did not cascade through Permission sets");
		require(restored->deletePermissionSet(stableId, &diagnostic), diagnostic);
		require(restored->getAgentPermissionSetAssignments(agent).empty()
			&& restored->lookupAccessPermission(blue),
			"Permission set deletion did not remove assignments while preserving permissions");

		// Access permission and Permission set names intentionally use separate namespaces.
		auto sameName = restored->addPermissionSet("Blue key");
		require(static_cast<bool>(sameName), "Permission set namespace collided with Access permissions");
		bool duplicateRefused = false;
		try { (void)restored->addPermissionSet("Blue key"); }
		catch (std::invalid_argument const&) { duplicateRefused = true; }
		require(duplicateRefused, "duplicate Permission set name was accepted");
		require(static_cast<bool>(restored->addPermissionSet("blue key")),
			"Permission set uniqueness was not case-sensitive");

		core::World unbounded("many sets", 1, 1);
		unbounded.addCorridor(0, 0, 1); unbounded.finishBuild(); unbounded.pauseSimulation();
		for (int index = 0; index < 257; ++index)
			(void)unbounded.addPermissionSet("Set " + std::to_string(index));
		require(unbounded.getPermissionSetCount() == 257,
			"Permission sets retained a feature-specific fixed count limit");
	}

	void manualDoorAuthorization()
	{
		core::World world("manual Door permissions", 12, 1);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto protectedDoor = world.addSectorDoor(0, 0, 3, {});
		auto openDoor = world.addSectorDoor(0, 0, 9, {});
		uint32_t frontMarker, backMarker;
		world.addSectorMarker(front, 0, 2.5f, &frontMarker);
		world.addSectorMarker(back, 0, 2.5f, &backMarker);
		world.finishBuild(); world.pauseSimulation();
		auto agentId = world.createAgent("walker", front, 0, 2.5f);
		auto agent = world.lookupAgent(agentId).entity;
		auto key = world.addAccessPermission("Door key");
		auto unrelated = world.addAccessPermission("Unrelated");
		std::string diagnostic;
		require(world.setManualDoorPermissionRequirement(protectedDoor.traversalResource,
			{ key }, &diagnostic), diagnostic);
		require(world.isManualDoorPermissionEligible(protectedDoor.traversalResource),
			"buttonless manual Door was not permission-eligible");

		auto graph = world.getGraph();
		auto frontVertex = graph->getVertexByIdentifier(frontMarker);
		auto backVertex = graph->getVertexByIdentifier(backMarker);
		auto chosenDoor = [](std::shared_ptr<core::Path> const& path)
		{
			if (!path) return core::TraversalResourceId{};
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getType() == core::EdgeType::Door)
					return node.edge->getTraversalResourceId();
			return core::TraversalResourceId{};
		};
		require(chosenDoor(graph->calculatePath(agent, frontVertex, backVertex))
			== openDoor.traversalResource,
			"unauthorized shorter Door was assigned a finite route cost");
		require(world.grantAgentAccessPermission(agentId, key, &diagnostic), diagnostic);
		require(chosenDoor(graph->calculatePath(agent, frontVertex, backVertex))
			== protectedDoor.traversalResource,
			"authorized Agent did not select the protected Door");
		require(chosenDoor(graph->calculatePath(agent, backVertex, frontVertex))
			== protectedDoor.traversalResource,
			"Door requirement was not bidirectional");

		// Relevant losses hard-replan, gains use ordinary persistence, and a grant
		// which appears in no Door requirement leaves the Path object untouched.
		agent->setPath(graph->calculatePath(agent, frontVertex, backVertex), true);
		require(world.revokeAgentAccessPermission(agentId, key, &diagnostic), diagnostic);
		require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
			"Authorization loss did not enter Route planning");
		require(world.resumeSimulation(), "Authorization replan could not resume");
		world.advanceTicks(agent->getRoutePlanningRemainingTicks());
		require(chosenDoor(agent->getPath()) == openDoor.traversalResource,
			"losing a relevant grant did not invalidate the current Path");
		require(world.setAgentRuntimeAccessPermissionGrant(agentId, key, true), "Runtime grant failed");
		require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
			"Authorization gain calculated instead of planning");
		world.advanceTicks(agent->getRoutePlanningRemainingTicks());
		require(chosenDoor(agent->getPath()) == protectedDoor.traversalResource,
			"gaining a relevant grant did not reconsider the current Path");
		auto unchanged = agent->getPath();
		require(world.setAgentRuntimeAccessPermissionGrant(agentId, unrelated, true), "Unrelated runtime grant failed");
		require(agent->getPath() == unchanged, "an unrelated grant disturbed the current Path");

		// Adding a requirement which the Agent does not satisfy immediately
		// invalidates the affected Path, then replans after its interval.
		world.pauseSimulation();
		require(world.setManualDoorPermissionRequirement(protectedDoor.traversalResource,
			{}, &diagnostic), diagnostic);
		require(world.setAgentRuntimeAccessPermissionGrant(agentId, key, false), "Runtime revoke failed");
		agent->setPath(graph->calculatePath(agent, frontVertex, backVertex), true);
		require(world.setManualDoorPermissionRequirement(protectedDoor.traversalResource,
			{ key }, &diagnostic), diagnostic);
		require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
			"Manual Door requirement did not defer replanning");
		require(world.resumeSimulation(), "Requirement replan could not resume");
		world.advanceTicks(agent->getRoutePlanningRemainingTicks());
		require(chosenDoor(agent->getPath()) == openDoor.traversalResource,
			"adding a manual Door requirement did not replan the affected Path at expiry");
		auto door = std::static_pointer_cast<const core::DoorSectorObject>(
			protectedDoor.door.sector->getObject(protectedDoor.door.index))->getDoor();

		auto localAgentId = world.createAgent("local observer", front, 0, 2.5f);
		auto localAgent = world.lookupAgent(localAgentId).entity;
		door->requestOpen(); door->update(10.0f);
		require(localAgent->getEffectivePermissionAdherence().value,
			"Permission adherence was not default true");
		require(!world.agentAdheresToDoorPermission(protectedDoor.traversalResource,
			core::SectorId{ static_cast<uint64_t>(front) + 1 }, localAgentId),
			"Open manual Door adherence did not detect the unsatisfied requirement");
		require(chosenDoor(graph->calculatePath(localAgent, frontVertex, backVertex))
			== openDoor.traversalResource,
			"default Permission adherence admitted an unauthorized open manual Door");
		world.pauseSimulation();
		require(world.setAgentIndividualPermissionAdherence(localAgentId, false, &diagnostic), diagnostic);
		require(chosenDoor(graph->calculatePath(localAgent, frontVertex, backVertex))
			== protectedDoor.traversalResource,
			"disabled Permission adherence did not preserve opportunistic open-Door passage");
		core::RouteDecisionContext remote{ localAgent, {}, {}, nullptr, localAgent->getWalkSpeed(), &world };
		for (auto const& edge : graph->getEdges())
			if (edge->getTraversalResourceId() == protectedDoor.traversalResource)
				require(!edge->getDirectedTraversalFacts(edge->getVertex(1), remote).feasible,
					"route search consulted remote live Door state");

		world.pauseSimulation();
		auto yaml = save(world);
		auto restored = load(yaml);
		require(restored->getManualDoorPermissionRequirement(protectedDoor.traversalResource)
			== std::vector<core::AccessPermissionId>{ key },
			"manual Door requirement did not persist");
		auto usage = restored->getAccessPermissionUsage(key);
		require(usage.manualDoorRequirements == 1, "manual Door usage count is wrong");
		restored->pauseSimulation();
		require(restored->deleteAccessPermission(key, &diagnostic), diagnostic);
		require(restored->getManualDoorPermissionRequirement(protectedDoor.traversalResource).empty(),
			"Access permission deletion did not clear the manual Door requirement");
	}

	void unavailableAuthorizationChangeClearsAffectedPath()
	{
		core::World world("unavailable authorization route", 3, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 3, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 3, 1);
		auto door = world.addSectorDoor(0, 0, 1, {});
		uint32_t frontMarker, backMarker;
		world.addSectorMarker(front, 0, 0.5f, &frontMarker);
		world.addSectorMarker(back, 0, 0.5f, &backMarker);
		world.finishBuild(); world.pauseSimulation();
		auto blockedId = world.createAgent("blocked by requirement", front, 0, 0.5f);
		auto revokedId = world.createAgent("blocked by grant loss", front, 0, 0.5f);
		auto blocked = world.lookupAgent(blockedId).entity;
		auto revoked = world.lookupAgent(revokedId).entity;
		auto key = world.addAccessPermission("Only route key");
		std::string diagnostic;
		require(world.grantAgentAccessPermission(revokedId, key, &diagnostic), diagnostic);
		auto graph = world.getGraph();
		auto start = graph->getVertexByIdentifier(frontMarker);
		auto destination = graph->getVertexByIdentifier(backMarker);
		blocked->setPath(graph->calculatePath(blocked, start, destination), true);
		revoked->setPath(graph->calculatePath(revoked, start, destination), true);
		require(blocked->getPath() && revoked->getPath(),
			"authorization fixtures did not start with a Path");

		require(world.setManualDoorPermissionRequirement(door.traversalResource,
			{ key }, &diagnostic), diagnostic);
		require(!blocked->getPath(),
			"adding an unsatisfied requirement did not clear a Path with no alternative");
		require(static_cast<bool>(revoked->getPath()),
			"adding a satisfied requirement cleared an authorized Path");
		require(world.revokeAgentAccessPermission(revokedId, key, &diagnostic), diagnostic);
		require(!revoked->getPath(),
			"losing a required grant did not clear a Path with no alternative");
		require(blocked->getState() == core::Agent::State::RoutePlanning
			&& revoked->getState() == core::Agent::State::RoutePlanning,
			"Authorization failure was resolved before the planning interval");
		world.consumeSimulationEvents();
		require(world.resumeSimulation(), "Failed authorization fixture could not resume");
		auto const firstExpiry = std::min(blocked->getRoutePlanningRemainingTicks(),
			revoked->getRoutePlanningRemainingTicks());
		auto const lastExpiry = std::max(blocked->getRoutePlanningRemainingTicks(),
			revoked->getRoutePlanningRemainingTicks());
		world.advanceTicks(firstExpiry - 1);
		for (auto const& event : world.consumeSimulationEvents())
			require(event.type != core::SimulationEventType::RouteLost,
				"Authorization Route loss was published early");
		world.advanceTicks(lastExpiry - firstExpiry + 1);
		unsigned lost = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost)
			{
				++lost;
				require(event.routeLossReason == core::RouteLossReason::Unreachable,
					"Authorization failure reported the wrong Route loss reason");
			}
		auto snapshot = world.getSimulationSnapshot();
		require(lost == 2 && snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
			"Authorization failure omitted outcomes or retained traversal ownership");
		for (auto const& agent : snapshot.agents)
			require(agent.state == core::AgentPathState::Idle && !agent.intendedDestination
				&& !agent.routePlanningTotalTicks && !agent.routePlanningRemainingTicks,
				"Authorization failure retained movement intent");
		world.advanceTicks(20);
		for (auto const& event : world.consumeSimulationEvents())
			require(event.type != core::SimulationEventType::RouteLost,
				"Authorization failure published duplicate Route loss");
	}

	void interactionRequirementDelaysReplanningAffectedPath()
	{
		core::World world("Interaction requirement replanning", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto protectedDoor = world.addSectorDoor(0, 0, 3, {});
		auto alternateDoor = world.addSectorDoor(0, 0, 9, {});
		uint32_t frontMarker, backMarker;
		world.addSectorMarker(front, 0, 2.5f, &frontMarker);
		world.addSectorMarker(back, 0, 2.5f, &backMarker);
		world.finishBuild(); world.pauseSimulation();
		world.addSectorDoorButton(protectedDoor.door.sector->getIndex(),
			protectedDoor.door.index);
		world.finishBuild(); world.pauseSimulation();
		auto agentId = world.createAgent("blocked by requirement", front, 0, 2.5f);
		auto revokedId = world.createAgent("blocked by grant loss", front, 0, 2.5f);
		auto agent = world.lookupAgent(agentId).entity;
		auto revoked = world.lookupAgent(revokedId).entity;
		auto graph = world.getGraph();
		auto chosenDoor = [](std::shared_ptr<core::Path> const& path)
		{
			if (!path) return core::TraversalResourceId{};
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getType() == core::EdgeType::Door)
					return node.edge->getTraversalResourceId();
			return core::TraversalResourceId{};
		};
		auto start = graph->getVertexByIdentifier(frontMarker);
		auto destination = graph->getVertexByIdentifier(backMarker);
		agent->setPath(graph->calculatePath(agent, start, destination), true);
		revoked->setPath(graph->calculatePath(revoked, start, destination), true);
		require(chosenDoor(agent->getPath()) == protectedDoor.traversalResource
			&& chosenDoor(revoked->getPath()) == protectedDoor.traversalResource,
			"controlled Door was not on the initial Paths");
		auto key = world.addAccessPermission("Control key");
		std::string diagnostic;
		require(world.grantAgentAccessPermission(revokedId, key, &diagnostic), diagnostic);
		auto resource = world.lookupTraversalResource(protectedDoor.traversalResource);
		require(resource && resource.entity->getControls().size() == 2,
			"controlled Door did not expose both Interaction points");
		for (auto point : resource.entity->getControls())
			require(world.setInteractionPointPermissionRequirement(point,
				{ key }, &diagnostic), diagnostic);
		require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
			"Interaction point requirement did not defer replanning");
		require(chosenDoor(revoked->getPath()) == protectedDoor.traversalResource,
			"adding a satisfied Interaction point requirement disturbed an authorized Path");
		require(world.revokeAgentAccessPermission(revokedId, key, &diagnostic), diagnostic);
		require(revoked->getState() == core::Agent::State::RoutePlanning && !revoked->getPath(),
			"Interaction point grant loss did not defer replanning");
		require(world.resumeSimulation(), "Interaction requirement replan could not resume");
		world.advanceTicks(std::max(agent->getRoutePlanningRemainingTicks(), revoked->getRoutePlanningRemainingTicks()));
		require(chosenDoor(agent->getPath()) == alternateDoor.traversalResource
			&& chosenDoor(revoked->getPath()) == alternateDoor.traversalResource,
			"Interaction point authorization did not replan at expiry");
	}

	void controlledDoorAuthorization()
	{
		core::World world("controlled Door permissions", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		world.addRoom("Back", 1, 0, 0, 12, 1);
		auto door = world.addSectorDoor(0, 0, 3, {});
		world.finishBuild(); world.pauseSimulation();
		auto red = world.addAccessPermission("Red control");
		auto blue = world.addAccessPermission("Blue control");
		std::string diagnostic;
		require(world.setManualDoorPermissionRequirement(door.traversalResource,
			{ red }, &diagnostic), diagnostic);
		world.addSectorDoorButton(door.door.sector->getIndex(), door.door.index);
		world.finishBuild(); world.pauseSimulation();
		auto resource = world.lookupTraversalResource(door.traversalResource);
		require(resource && resource.entity->getControls().size() == 2,
			"protected manual Door did not gain two controls");
		for (auto point : resource.entity->getControls())
			require(world.getInteractionPointPermissionRequirement(point)
				== std::vector<core::AccessPermissionId>{ red },
				"manual Door requirement was not copied to each control");
		require(world.getManualDoorPermissionRequirement(door.traversalResource).empty(),
			"obsolete direct Door requirement survived control conversion");

		// Routing evaluates the control on the approach side without consulting a
		// remote live state. A locally observed open threshold remains permission-
		// based passage for an adhering Agent, while disabling adherence restores
		// opportunistic use.
		auto agentId = world.createAgent("unauthorized", front, 0, 1.5f);
		auto agent = world.lookupAgent(agentId).entity;
		auto graph = world.getGraph();
		std::shared_ptr<const core::Edge> doorEdge;
		for (auto const& edge : graph->getEdges())
			if (edge->getTraversalResourceId() == door.traversalResource) doorEdge = edge;
		require(doorEdge != nullptr, "controlled Door edge was unavailable");
		auto target = doorEdge->getVertex(0)->getSector()->getIndex() == front
			? doorEdge->getVertex(1) : doorEdge->getVertex(0);
		core::RouteDecisionContext remote{ agent, {}, {}, nullptr, agent->getWalkSpeed(), &world };
		require(!doorEdge->getDirectedTraversalFacts(target, remote).feasible,
			"routing admitted an unauthorized remote Door control");
		auto liveDoor = std::static_pointer_cast<const core::DoorSectorObject>(
			door.door.sector->getObject(door.door.index))->getDoor();
		liveDoor->requestOpen(); liveDoor->update(10.0f);
		core::RouteDecisionContext local{ agent, {}, {}, world.getSector(front).get(),
			agent->getWalkSpeed(), &world };
		require(!doorEdge->getDirectedTraversalFacts(target, local).feasible,
			"default Permission adherence admitted an unauthorized open controlled Door");
		require(world.setAgentIndividualPermissionAdherence(agentId, false, &diagnostic), diagnostic);
		require(doorEdge->getDirectedTraversalFacts(target, local).feasible,
			"disabled Permission adherence did not permit opportunistic open-Door passage");

		auto controls = resource.entity->getControls();
		require(world.resumeSimulation(), "controlled Door runtime fixture did not resume");
		auto rejectedId = world.requestInteraction(controls.front(), agentId);
		auto rejected = world.lookupInteractionRequest(rejectedId);
		require(rejected && rejected.entity->getResult() == core::InteractionResult::Rejected,
			"Door control bypassed the shared Interaction point authorization gate");
		world.pauseSimulation();
		require(world.setInteractionPointPermissionRequirement(controls[1],
			{ blue }, &diagnostic), diagnostic);
		bool refused = false;
		try { world.removeSectorDoorButton(door.door.sector->getIndex(), door.door.index); }
		catch (core::Exception const&) { refused = true; }
		require(refused && world.lookupTraversalResource(door.traversalResource).entity->getControls().size() == 2,
			"conflicting control conversion changed the Door without an explicit result");
		auto rebuilt = world.removeSectorDoorButton(door.door.sector->getIndex(),
			door.door.index, std::vector<core::AccessPermissionId>{ blue });
		require(rebuilt && world.getManualDoorPermissionRequirement(
			rebuilt->getDoor()->getTraversalResourceId())
			== std::vector<core::AccessPermissionId>{ blue },
			"explicit conversion result did not become the direct manual requirement");
		auto restored = load(save(world));
		require(restored->getManualDoorPermissionRequirement(
			rebuilt->getDoor()->getTraversalResourceId())
			== std::vector<core::AccessPermissionId>{ blue },
			"converted Door requirement did not survive save/load");

		core::World bulk("Bulkhead control permissions", 8, 1);
		bulk.addRoom("Left", 0, 0, 0, 4, 1);
		bulk.addRoom("Right", 0, 0, 4, 4, 1);
		auto bulkhead = bulk.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT, {});
		bulk.finishBuild(); bulk.pauseSimulation();
		auto left = bulk.addAccessPermission("Left key");
		auto right = bulk.addAccessPermission("Right key");
		require(bulk.setInteractionPointPermissionRequirement(
			bulkhead.controls[0].interactionPoint, { left }, &diagnostic), diagnostic);
		require(bulk.setInteractionPointPermissionRequirement(
			bulkhead.controls[1].interactionPoint, { right }, &diagnostic), diagnostic);
		auto bulkRestored = load(save(bulk));
		require(bulkRestored->getInteractionPointPermissionRequirement(
			bulkhead.controls[0].interactionPoint) == std::vector<core::AccessPermissionId>{ left }
			&& bulkRestored->getInteractionPointPermissionRequirement(
				bulkhead.controls[1].interactionPoint) == std::vector<core::AccessPermissionId>{ right },
			"side-specific Bulkhead Door requirements did not survive save/load");

		core::World::CreateBulkheadDoorOptions options;
		require(bulk.getSectorBulkheadDoorOptions(bulkhead.door.sector->getIndex(),
			bulkhead.door.index, options), "Bulkhead Door options were unavailable");
		options.controls[0] = false;
		auto bulkObject = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(
			bulk.applySectorBulkheadDoorOptions(bulkhead.door.sector->getIndex(),
				bulkhead.door.index, options));
		auto surviving = bulk.lookupTraversalResource(
			bulkObject->getDoor()->getTraversalResourceId()).entity->getControls();
		require(surviving.size() == 1
			&& bulk.getInteractionPointPermissionRequirement(surviving.front())
				== std::vector<core::AccessPermissionId>{ right },
			"deleting one Bulkhead Door control moved or deleted the other side's requirement");
		uint32_t bulkObjectIndex = ~0u;
		for (uint32_t index = 0; index < bulkObject->getSector()->getNumObjects(); ++index)
			if (bulkObject->getSector()->getObject(index) == bulkObject) bulkObjectIndex = index;
		require(bulk.getSectorBulkheadDoorOptions(bulkObject->getSector()->getIndex(),
			bulkObjectIndex, options), "rebuilt Bulkhead Door options were unavailable");
		options.activationMode = core::DoorActivationMode::Automatic;
		options.controls[0] = options.controls[1] = false;
		bulkObject = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(
			bulk.applySectorBulkheadDoorOptions(bulkObject->getSector()->getIndex(),
				bulkObjectIndex, options));
		require(bulk.lookupTraversalResource(bulkObject->getDoor()->getTraversalResourceId())
			.entity->getControls().empty(),
			"automatic Bulkhead Door retained a permission-eligible Agent control");
	}

	void malformedAuthorizationIsTransactional()
	{
		core::World authored("malformed source", 2, 1);
		auto corridor = authored.addCorridor(0, 0, 2);
		authored.finishBuild(); authored.pauseSimulation();
		auto agent = authored.createAgent("assigned", corridor, 0, 0.5f);
		auto permission = authored.addAccessPermission("Original");
		auto permissionSet = authored.addPermissionSet("Original set");
		std::string diagnostic;
		require(authored.setPermissionSetAccessPermission(
			permissionSet, permission, true, &diagnostic), diagnostic);
		require(authored.setAgentPermissionSetAssignment(
			agent, permissionSet, true, &diagnostic), diagnostic);
		auto validYaml = save(authored);
		auto yaml = validYaml;
		auto section = yaml.find("accessPermissions:");
		auto id = yaml.find("id: 1", section);
		require(section != std::string::npos && id != std::string::npos, "permission fixture did not serialize definitions");
		yaml.replace(id, std::string("id: 1").size(), "id: 257");

		core::World target("untouched", 5, 3);
		target.addLayer(); target.addCorridor(0, 0, 5); target.finishBuild();
		auto before = save(target);
		bool refused = false;
		try
		{
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
			target.deserialize(*reader, work);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "out-of-range serialized authorization was accepted");
		require(save(target) == before, "refused authorization partially changed the target World");

		yaml = validYaml;
		section = yaml.find("agentPermissionSetAssignments:");
		auto sets = yaml.find("sets:", section);
		auto assignment = yaml.find("- 1", sets);
		require(section != std::string::npos && sets != std::string::npos
			&& assignment != std::string::npos,
			"Permission set assignment fixture did not serialize references");
		yaml.replace(assignment, std::string("- 1").size(), "- 999");
		refused = false;
		try
		{
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
			target.deserialize(*reader, work);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "dangling serialized Permission set assignment was accepted");
		require(save(target) == before,
			"dangling Permission set assignment partially changed the target World");
	}

	void extensibleControlRequirementsPersistIndependently()
	{
		core::World world("extensible permissions", 10, 3);
		auto bridgeRoom = world.addRoom("Bridge", 0, 0, 0, 6, 2);
		world.addSectorWalkway(bridgeRoom, 1, 0);
		world.addSectorWalkway(bridgeRoom, 1, 3);
		auto bridge = world.addSectorForceBridge(bridgeRoom, 1, 1,
			{ 2, CORE_SIDE_LEFT, true, false, 2 });
		auto ladderRoom = world.addRoom("Ladder", 0, 0, 7, 3, 2);
		world.addSectorWalkway(ladderRoom, 1, 1);
		auto ladder = world.addRoomLadder(ladderRoom, 0, 1, { 0, true, false });
		world.finishBuild(); world.pauseSimulation();
		auto left = world.addAccessPermission("Bridge left");
		auto right = world.addAccessPermission("Bridge right");
		auto low = world.addAccessPermission("Ladder low");
		auto high = world.addAccessPermission("Ladder high");
		std::string diagnostic;
		require(world.setInteractionPointPermissionRequirement(
			bridge.controls[0].interactionPoint, { left }, &diagnostic), diagnostic);
		require(world.setInteractionPointPermissionRequirement(
			bridge.controls[1].interactionPoint, { right }, &diagnostic), diagnostic);
		require(world.setInteractionPointPermissionRequirement(
			ladder.controls[0].interactionPoint, { low }, &diagnostic), diagnostic);
		require(world.setInteractionPointPermissionRequirement(
			ladder.controls[1].interactionPoint, { high }, &diagnostic), diagnostic);

		auto restored = load(save(world));
		require(restored->getInteractionPointPermissionRequirement(
			bridge.controls[0].interactionPoint) == std::vector<core::AccessPermissionId>{ left }
			&& restored->getInteractionPointPermissionRequirement(
				bridge.controls[1].interactionPoint) == std::vector<core::AccessPermissionId>{ right }
			&& restored->getInteractionPointPermissionRequirement(
				ladder.controls[0].interactionPoint) == std::vector<core::AccessPermissionId>{ low }
			&& restored->getInteractionPointPermissionRequirement(
				ladder.controls[1].interactionPoint) == std::vector<core::AccessPermissionId>{ high },
			"extensible control requirements did not persist independently");
	}

	void transportLandingRequirementsPersist()
	{
		core::World world("protected lift", 10, 2);
		world.addCorridor(0, 0, 10);
		world.addCorridor(1, 0, 10);
		world.finishBuild(); world.pauseSimulation();
		auto key = world.addAccessPermission("Lift key");
		core::World::CreateLiftOptions options;
		options.cellsWide = 2; options.stopOffsets = { 0, 1 };
		options.landingControlPermissionRequirements = { { key }, {} };
		auto created = world.addLift(1, 0, 8, options);
		world.finishBuild(); world.pauseSimulation();
		require(world.getInteractionPointPermissionRequirement(
			created.doors[0].controls[0].interactionPoint) == std::vector<core::AccessPermissionId>{ key },
			"Lift landing requirement was not applied");
		require(world.getInteractionPointPermissionRequirement(
			created.doors[1].controls[0].interactionPoint).empty(),
			"independent Lift landing inherited a sibling requirement");

		auto restored = load(save(world));
		require(restored->getInteractionPointPermissionRequirement(
			created.doors[0].controls[0].interactionPoint) == std::vector<core::AccessPermissionId>{ key },
			"Lift landing requirement did not survive save/load");
		restored->pauseSimulation();
		std::string diagnostic;
		require(restored->deleteAccessPermission(key, &diagnostic), diagnostic);
		require(restored->getInteractionPointPermissionRequirement(
			created.doors[0].controls[0].interactionPoint).empty(),
			"Access permission deletion left a Lift landing requirement");
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
		auto rider = world.lookupAgent(riderId).entity;
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
		world.consumeSimulationEvents();
		rider->setPath(shared, true);
		if (authorizationChange)
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
				if (authorizationChange == 2)
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
		for (auto const& event : world.consumeSimulationEvents())
		{
			require(!shuttle || event.type != core::SimulationEventType::RouteLost,
				"Accepted shared Shuttle journey produced Route loss");
			if (event.type == core::SimulationEventType::DeviceOperationAdded)
				require(event.deviceOperation.requester != riderId
					|| event.deviceOperation.command.type != select.type,
					"Piggyback rider attempted a protected destination selection");
		}
		require(rider->getSector()->getIndex() == middle && !rider->getPath()
			&& std::abs(rider->getGlobalPosition().y - (shuttle ? 0.0f : 1.0f)) < 0.01f,
			"Unauthorized rider did not complete shared journey and disembark: " + std::to_string(authorizationChange)
			+ " platform " + std::to_string(platform) + " shuttle " + std::to_string(shuttle)
			+ " sector " + std::to_string(rider->getSector()->getIndex()) + " x " + std::to_string(rider->getGlobalPosition().x)
			+ " y " + std::to_string(rider->getGlobalPosition().y)
			+ " state " + std::to_string(static_cast<int>(rider->getState())));
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

	void liftDestinationAuthoring(bool platform = false, bool shuttle = false)
	{
		auto world = std::make_shared<core::World>("destination authoring", shuttle ? 64 : 10, 3);
		auto unrelated = world->addRoom("Unrelated", 1, 0, 0, 2, 1);
		uint32_t room = 0;
		uint32_t shuttleMiddle = ~0u;
		if (platform)
		{
			room = world->addRoom("Platform room", 0, 0, 0, 10, 3);
			for (uint32_t level = 1; level < 3; ++level)
				for (uint32_t x = 0; x < 10; ++x) world->addSectorWalkway(room, level, x);
		}
		else if (shuttle)
		{
			world->addCorridor(0, 0, 16);
			shuttleMiddle = world->addCorridor(0, 24, 16);
			world->addCorridor(0, 48, 16);
		}
		else for (uint32_t level = 0; level < 3; ++level) world->addCorridor(level, 0, 10);
		core::World::CreateLiftOptions options;
		options.cellsWide = platform ? 1 : 2; options.stopOffsets = { 0, 1, 2 };
		core::World::CreateLiftResult lift;
		if (shuttle)
		{
			auto created = world->addShuttle(1, 0, 4, 60, { 2, 4, { 0, 24, 48 }, 0 });
			lift = { created.shuttle, created.doors, created.traversalResource, created.interiorSelector };
		}
		else lift = permissionLift(*world, platform, room, 8, options);
		auto sector = lift.lift.sector->getIndex();
		auto object = platform ? lift.lift.index : ~0u;
		world->finishBuild(); world->pauseSimulation();
		auto red = world->addAccessPermission("Red key");
		auto blue = world->addAccessPermission("Blue key");
		require(world->getLiftDestinationLevels(sector, object) == (shuttle ? std::vector<uint32_t>{ 4, 28, 52 } : std::vector<uint32_t>{ 0, 1, 2 }), "Destination Levels missing");
		for (uint32_t stop = 0; stop < 3; ++stop)
			require(world->getLiftDestinationPermissionRequirement(sector, stop, object).empty(), "New Stop is restricted");
		std::string diagnostic;
		auto unrestricted = save(*world);
		require(world->setLiftDestinationPermissionRequirement(sector, 0, {}, &diagnostic, object)
			&& save(*world) == unrestricted, "Empty no-op changed authored data");
		gWorldDocumentHistory.clear();
		require(commitLiftDestinationPermissionRequirement(world, sector, 2, red, true, diagnostic, object), diagnostic);
		require(commitLiftDestinationPermissionRequirement(world, sector, 2, blue, true, diagnostic, object), diagnostic);
		require(gWorldDocumentHistory.undoCount() == 2, "Destination edits bypass document history");
		auto restore = [&](DocumentSnapshot const& snapshot) { world = load(snapshot.yaml); world->pauseSimulation(); return true; };
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination undo failed");
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == std::vector<core::AccessPermissionId>{ red }, "Undo lost destination requirement");
		require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination redo failed");
		auto expected = std::vector<core::AccessPermissionId>{ red, blue };
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Redo lost destination requirement");
		require(commitClearLiftDestinationPermissionRequirement(world, sector, 2, diagnostic, object), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object).empty(), "Clear retained destination permissions");
		require(gWorldDocumentHistory.undoCount() == 3, "Destination clear bypassed document history");
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination clear undo failed");
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination clear undo lost requirements");
		require(world->renameAccessPermission(red, "Renamed key", &diagnostic), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Rename changed identity");
		require(world->getAccessPermissionUsage(red).liftDestinationRequirements == 1, "Usage omitted destination");
		auto before = save(*world);
		require(!world->setLiftDestinationPermissionRequirement(sector, 2, { red, red }, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(sector, 2, { core::AccessPermissionId{ 256 } }, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(sector, 3, {}, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(0, 0, {}, &diagnostic)
			&& save(*world) == before, "Invalid destination edit was not transactional");
		require(world->resumeSimulation(), "Destination fixture did not resume");
		require(!world->setLiftDestinationPermissionRequirement(sector, 2, {}, &diagnostic, object), "Running destination edit accepted");
		world->pauseSimulation();

		ImGui::CreateContext();
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1000, 700);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::SetNextWindowSize(ImVec2(950, 650)); ImGui::Begin("Lift Selection");
		ImGui::LogToBuffer();
		renderLiftDestinationPermissions(world, sector, object);
		std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
		require(text.find("Destination permissions") != std::string::npos
			&& text.find("Accepted shared journeys") != std::string::npos
			&& text.find("Intermediate feature") == std::string::npos
			&& text.find("Dynamic authorization is not yet complete") == std::string::npos
			&& text.find("NOT YET ENFORCED") == std::string::npos
			&& text.find("None") != std::string::npos
			&& text.find("Add / remove permissions") != std::string::npos
			&& text.find("Clear") != std::string::npos
			&& text.find("Renamed key") != std::string::npos
			&& text.find("Blue key") != std::string::npos
			&& (!shuttle || (text.find("Stop 0: x 4") != std::string::npos
				&& text.find("Stop 1: x 28") != std::string::npos
				&& text.find("Stop 2: x 52") != std::string::npos)), "Selection omitted requirements or destination positions");
		ImGui::LogFinish(); ImGui::End(); ImGui::Render(); ImGui::DestroyContext();

		auto yaml = save(*world);
		auto restored = load(yaml);
		require(restored->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination YAML round trip failed");
		core::SerializationWorkData binaryWork;
		auto binaryWriter = core::BinarySerializer::toString();
		world->serialize(*binaryWriter, binaryWork); binaryWriter->serialize();
		auto binaryReader = core::BinarySerializer::fromString(binaryWriter->getSerializedString());
		binaryReader->deserialize();
		core::World binaryWorld("binary target", 1, 1);
		require(binaryWorld.deserialize(*binaryReader, binaryWork)
			&& binaryWorld.getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination binary round trip failed");
		// Old Worlds have no destination field and remain unrestricted.
		auto legacy = YAML::Load(yaml);
		legacy["version"] = 29;
		for (auto record : legacy["construction"]) record.remove("destinationPermissionRequirements");
		require(load(YAML::Dump(legacy))->getLiftDestinationPermissionRequirement(sector, 2, object).empty(), "Older World is restricted");

		for (auto replacement : {
			"[{permissions: []}, {permissions: []}, {permissions: [999]}]",
			"[{permissions: []}, {permissions: []}, {permissions: [0]}]",
			"[{permissions: []}, {permissions: []}, {permissions: [1, 1]}]",
			"[{permissions: []}, {permissions: []}, {permissions: broken}]",
			"[{permissions: [1]}]", "broken", "{}" })
		{
			auto malformed = YAML::Load(yaml);
			for (auto record : malformed["construction"])
				if (record["destinationPermissionRequirements"])
					record["destinationPermissionRequirements"] = YAML::Load(replacement);
			auto unchanged = save(*restored);
			bool refused = false;
			try
			{
				core::SerializationWorkData work;
				auto reader = core::YamlSerializer::fromString(YAML::Dump(malformed)); reader->deserialize();
				restored->deserialize(*reader, work);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused && save(*restored) == unchanged, "Malformed destination mutated target World");
		}

		if (shuttle)
		{
			auto apply = [&](core::World::ShuttleEditPlan const& plan)
			{
				require(plan.valid, plan.diagnostic);
				sector = world->applyShuttleEdit(plan);
			};
			auto locationPlan = world->planRemoveLocation(unrelated);
			require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan); --sector;
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle reindex lost requirement");
			apply(world->planEditShuttleVehicle(sector, 2, 4, 1u << 2));
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle Door edit lost requirement");
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
			apply(world->planRemoveShuttleStop(sector, 1));
			require(world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Shuttle retained Stop lost requirement");
			apply(world->planAddShuttleStop(sector, 24));
			require(world->getLiftDestinationPermissionRequirement(sector, 1).empty()
				&& world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle recreated Stop inherited requirement");
			require(load(save(*world))->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle edit round trip failed");
			// Removing the supporting Location also removes its Stop, without
			// shifting another destination's permissions onto a different Stop.
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
			auto removePlatform = world->planRemoveLocation(shuttleMiddle - 1);
			require(removePlatform.valid, removePlatform.diagnostic);
			world->applyLocationEdit(removePlatform);
			for (uint32_t i = 0; i < world->getNumSectors(); ++i)
				if (world->getSector(i)->getType() == core::SectorType::Shuttle) sector = i;
			require(world->getLiftDestinationLevels(sector).size() == 2, "Supporting Location removal did not retain two Shuttle Stops");
			require(world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Platform removal lost retained Shuttle destination");
			world->addCorridor(0, 24, 16);
			apply(world->planAddShuttleStop(sector, 24));
			require(world->getLiftDestinationLevels(sector).size() == 3, "Recreated platform did not restore three Shuttle Stops");
			require(world->getLiftDestinationPermissionRequirement(sector, 1).empty(), "Recreated Shuttle platform inherited requirement");
			require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == std::vector<core::AccessPermissionId>{ blue }, "Shuttle permission cleanup failed");
			apply(world->planRemoveShuttle(sector));
			require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Removed Shuttle counted in usage");
			auto recreated = world->addShuttle(1, 0, 4, 60, { 2, 4, { 0, 24, 48 }, 0 });
			require(world->getLiftDestinationPermissionRequirement(recreated.shuttle.sector->getIndex(), 2).empty(), "Recreated Shuttle inherited requirement");
			gWorldDocumentHistory.clear();
			return;
		}

		if (platform)
		{
			auto locatePlatform = [&]
			{
				for (uint32_t i = 0; i < world->getSector(sector)->getNumObjects(); ++i)
					if (std::dynamic_pointer_cast<const core::LiftSectorObject>(world->getSector(sector)->getObject(i))) return i;
				throw std::runtime_error("Platform object missing");
			};
			// Ground is a destination too, independently of the landing call.
			require(world->setLiftDestinationPermissionRequirement(sector, 0, { blue }, &diagnostic, object), diagnostic);
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic, object), diagnostic);
			auto locationPlan = world->planRemoveLocation(unrelated);
			require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan); --sector;
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Platform sector reindex lost requirement");
			core::World::CreateLiftOptions settings;
			require(world->getPlatformLiftOptions(sector, object, settings), "Platform options missing");
			settings.capacity = 1; settings.platformStopDurationSeconds = 4.0f;
			auto edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Platform settings lost requirement");
			settings.stopOffsets = { 0, 2 };
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object) == expected, "Platform retained Stop lost requirement");
			settings.stopOffsets = { 0, 1, 2 };
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object).empty(), "Recreated Platform Stop inherited requirement");
			// Removing a supporting Walkway removes only that optional Stop.
			uint32_t walkway = ~0u;
			for (uint32_t i = 0; i < world->getSector(sector)->getNumObjects(); ++i)
			{
				auto candidate = world->getSector(sector)->getObject(i);
				if (std::dynamic_pointer_cast<const core::WalkwaySectorObject>(candidate)
					&& candidate->getCellX() == 8 && candidate->getCellY() == 1) walkway = i;
			}
			require(walkway != ~0u, "Supporting Walkway missing");
			require(world->removeSectorWalkway(sector, walkway), "Walkway removal failed");
			require(world->getLiftDestinationLevels(sector, object) == std::vector<uint32_t>{ 0, 2 }
				&& world->getLiftDestinationPermissionRequirement(sector, 1, object) == expected, "Walkway deletion lost retained destination");
			world->addSectorWalkway(sector, 1, 8);
			object = locatePlatform();
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object).empty()
				&& world->getLiftDestinationPermissionRequirement(sector, 0, object) == std::vector<core::AccessPermissionId>{ blue },
				"Walkway recreation inherited requirement or lost mandatory ground requirement");
			require(load(save(*world))->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Structural edit persistence failed");
			require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == std::vector<core::AccessPermissionId>{ blue }, "Platform deletion cleanup failed");
			edit = world->planRemovePlatformLift(sector, object);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Deleted Platform retained usage");
			auto recreated = world->addSectorPlatformLift(sector, 0, 8, options);
			world->finishBuild(); world->pauseSimulation();
			for (uint32_t stop = 0; stop < 3; ++stop)
				require(world->getLiftDestinationPermissionRequirement(sector, stop, recreated.lift.index).empty(), "Recreated Platform is restricted");
			gWorldDocumentHistory.clear();
			return;
		}

		// Deleting a landing Location also removes its Stop and remaps retained requirements.
		restored->pauseSimulation();
		require(restored->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
		auto landingPlan = restored->planRemoveLocation(2);
		require(landingPlan.valid, landingPlan.diagnostic); restored->applyLocationEdit(landingPlan);
		require(restored->getLiftDestinationLevels(sector - 1) == std::vector<uint32_t>{ 0, 2 }
			&& restored->getLiftDestinationPermissionRequirement(sector - 1, 1) == expected
			&& restored->getAccessPermissionUsage(red).liftDestinationRequirements == 1,
			"Landing deletion lost retained requirements or retained a deleted Stop requirement");
		require(load(save(*restored))->getLiftDestinationPermissionRequirement(sector - 1, 1) == expected,
			"Landing deletion produced malformed destination data");

		// Unrelated deletion reindexes Sectors without changing destination identity.
		auto locationPlan = world->planRemoveLocation(unrelated);
		require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan);
		--sector;
		require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Sector reindexing lost requirements");
		// Removing a restricted earlier Stop reindexes the retained destination; recreation is unrestricted.
		require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
		auto plan = world->planRemoveLiftStop(sector, 1);
		require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getLiftDestinationLevels(sector) == std::vector<uint32_t>{ 0, 2 }
			&& world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Retained Stop lost requirement after reindexing");
		plan = world->planResizeLift(sector, 8, 0, 2, 3);
		require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getLiftDestinationLevels(sector) == std::vector<uint32_t>{ 0, 1, 2 }
			&& world->getLiftDestinationPermissionRequirement(sector, 1).empty()
			&& world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Recreated Stop inherited a requirement");
		require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2) == std::vector<core::AccessPermissionId>{ blue }, "Deletion left destination reference");
		plan = world->planRemoveLift(sector); require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Deleted transport retained usage");
		lift = world->addLift(1, 0, 8, options); world->finishBuild(); world->pauseSimulation();
		require(world->getLiftDestinationPermissionRequirement(lift.lift.sector->getIndex(), 2).empty(), "Recreated Lift inherited requirements");
		gWorldDocumentHistory.clear();
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
		auto authored = save(world);
		require(world.setAgentRuntimeAccessPermissionGrant(id, red, false), "Runtime revoke failed");
		require(world.setAgentRuntimePermissionSetAssignment(id, set, false), "Runtime set revoke failed");
		require(world.missingLiftDestinationPermissions(select, id) == std::vector<core::AccessPermissionId>{ red, blue }
			&& save(world) == authored, "Runtime grants leaked into authored Platform authorization");
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

	void runtimePropertiesPanelChangesCurrentAuthorizationOnly()
	{
		auto world = std::make_shared<core::World>("runtime properties panel", 2, 1);
		auto corridor = world->addCorridor(0, 0, 2);
		world->finishBuild(); world->pauseSimulation();
		auto agent = world->createAgent("agent", corridor, 0, 0.5f);
		auto permission = world->addAccessPermission("Runtime key");
		auto permissionSet = world->addPermissionSet("Runtime card");
		std::string diagnostic;
		require(world->setPermissionSetAccessPermission(permissionSet, permission,
			true, &diagnostic), diagnostic);
		world->markSaved();
		require(world->resumeSimulation(), "runtime properties fixture did not resume");

		require(world->setAgentRuntimeAccessPermissionGrant(agent, permission, true),
			"runtime direct grant was refused while simulation was running");
		require(world->setAgentRuntimePermissionSetAssignment(agent, permissionSet, true),
			"runtime Permission set assignment was refused while simulation was running");
		require(world->getAgentCurrentDirectAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ permission },
			"runtime direct grant was not visible as current Agent state");
		require(world->getAgentCurrentPermissionSetAssignments(agent)
			== std::vector<core::PermissionSetId>{ permissionSet },
			"runtime Permission set assignment was not visible as current Agent state");
		require(world->getAgentDirectAccessGrants(agent).empty()
			&& world->getAgentPermissionSetAssignments(agent).empty(),
			"runtime authorization rewrote the authored Agent configuration");
		require(!world->isModified(), "runtime authorization dirtied the World document");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		io.IniFilename = nullptr;
		ImGui::NewFrame(); ImGui::Begin("Runtime properties test");
		ImGui::SetNextItemOpen(true);
		renderAgentRuntimeProperties(world, agent);
		ImGui::End(); ImGui::Render(); ImGui::DestroyContext();

		world->resetSimulation();
		require(world->getAgentCurrentDirectAccessGrants(agent).empty()
			&& world->getAgentCurrentPermissionSetAssignments(agent).empty(),
			"Reset did not restore authored Agent authorization");
		world->pauseSimulation();
		require(world->setAgentRuntimeAccessPermissionGrant(agent, permission, true)
			&& world->getAgentCurrentDirectAccessGrants(agent)
				== std::vector<core::AccessPermissionId>{ permission },
			"runtime direct grant was unavailable while simulation was paused");
		world->resetSimulation();
		require(world->getAgentCurrentDirectAccessGrants(agent).empty(),
			"Reset did not clear a paused runtime authorization change");
	}

	void panelCommitParticipatesInHistory()
	{
		auto world = std::make_shared<core::World>("panel", 2, 1);
		auto corridor = world->addRoom("Front", 0, 0, 0, 2, 1);
		world->addRoom("Back", 1, 0, 0, 2, 1);
		auto door = world->addSectorDoor(0, 0, 0, {});
		auto control = world->addSectorLightSwitch(corridor, 1);
		world->finishBuild(); world->pauseSimulation();
		auto agent = world->createAgent("agent", corridor, 0, 0.5f);
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		std::string diagnostic;
		auto id = commitAccessPermissionAdd(world, "Staff", diagnostic);
		require(id && gWorldDocumentHistory.canUndo() && world->isModified(),
			"Permissions panel commit did not create one document edit");
		auto permissionSet = commitPermissionSetAdd(world, "Operators", diagnostic);
		require(static_cast<bool>(permissionSet), "Permissions panel did not add a Permission set");
		require(commitPermissionSetMembership(world, permissionSet, id, true, diagnostic), diagnostic);
		require(commitAgentPermissionSetAssignment(world, agent, permissionSet, true, diagnostic), diagnostic);
		require(commitPermissionSetRename(world, permissionSet, "Operators renamed", diagnostic), diagnostic);
		auto sources = world->getAgentAccessGrantSources(agent, id);
		require(sources.permissionSets == std::vector<core::PermissionSetId>{ permissionSet },
			"selected Agent panel mutations did not expose the effective set source");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		io.IniFilename = nullptr;
		ImGui::NewFrame(); ImGui::Begin("Permissions test");
		renderPermissionsPanel(world);
		renderAgentAccessPermissions(world, agent);
		renderInteractionPermissionRequirements(world, control.interactionPoint);
		renderManualDoorPermissionRequirements(world, door.traversalResource);
		ImGui::End(); ImGui::Render(); ImGui::DestroyContext();
	}
}

void runAccessPermissionSmokeChecks()
{
	authorizationAndPersistence();
	permissionSets();
	manualDoorAuthorization();
	unavailableAuthorizationChangeClearsAffectedPath();
	interactionRequirementDelaysReplanningAffectedPath();
	controlledDoorAuthorization();
	malformedAuthorizationIsTransactional();
	extensibleControlRequirementsPersistIndependently();
	transportLandingRequirementsPersist();
	liftDestinationAuthoring();
	liftDestinationAuthoring(true);
	liftDestinationAuthoring(false, true);
	platformDestinationResetAndIntermediateJourney();
	liftDestinationEnforcement();
	liftDestinationEnforcement(1);
	liftDestinationEnforcement(2);
	liftDestinationEnforcement(3);
	for (unsigned change = 0; change < 4; ++change) liftDestinationEnforcement(change, true);
	for (unsigned change = 0; change < 4; ++change) liftDestinationEnforcement(change, false, true);
	changingLiftDestinationAuthorization(true);
	changingLiftDestinationAuthorization(false, true);
	liftDestinationAlternative();
	changingLiftDestinationAuthorization();
	runtimePropertiesPanelChangesCurrentAuthorizationOnly();
	panelCommitParticipatesInHistory();
	runPermissionAdherenceSmokeChecks();
	runTransportLandingAdherenceSmokeChecks();
}
