#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Edge.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "imgui/imgui.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"

void runAccessPermissionSmokeChecks();

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
		require(chosenDoor(agent->getPath()) == openDoor.traversalResource,
			"losing a relevant grant did not invalidate the current Path");
		require(world.grantAgentAccessPermission(agentId, key, &diagnostic), diagnostic);
		require(chosenDoor(agent->getPath()) == protectedDoor.traversalResource,
			"gaining a relevant grant did not reconsider the current Path");
		auto unrelated = world.addAccessPermission("Unrelated");
		auto unchanged = agent->getPath();
		require(world.grantAgentAccessPermission(agentId, unrelated, &diagnostic), diagnostic);
		require(agent->getPath() == unchanged, "an unrelated grant disturbed the current Path");

		// Adding a requirement which the Agent does not satisfy immediately
		// invalidates the affected Path and replans onto the other Door.
		require(world.setManualDoorPermissionRequirement(protectedDoor.traversalResource,
			{}, &diagnostic), diagnostic);
		require(world.revokeAgentAccessPermission(agentId, key, &diagnostic), diagnostic);
		agent->setPath(graph->calculatePath(agent, frontVertex, backVertex), true);
		require(world.setManualDoorPermissionRequirement(protectedDoor.traversalResource,
			{ key }, &diagnostic), diagnostic);
		require(chosenDoor(agent->getPath()) == openDoor.traversalResource,
			"adding a manual Door requirement did not immediately replan the affected Path");
		auto door = std::static_pointer_cast<const core::DoorSectorObject>(
			protectedDoor.door.sector->getObject(protectedDoor.door.index))->getDoor();

		auto localAgentId = world.createAgent("local observer", front, 0, 2.5f);
		auto localAgent = world.lookupAgent(localAgentId).entity;
		door->requestOpen(); door->update(10.0f);
		require(chosenDoor(graph->calculatePath(localAgent, frontVertex, backVertex))
			== protectedDoor.traversalResource,
			"unauthorized Agent could not use a locally observed open Door");
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
	}

	void interactionRequirementImmediatelyReplansAffectedPath()
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
		require(chosenDoor(agent->getPath()) == alternateDoor.traversalResource,
			"adding an Interaction point requirement did not immediately replan the affected Path");
		require(chosenDoor(revoked->getPath()) == protectedDoor.traversalResource,
			"adding a satisfied Interaction point requirement disturbed an authorized Path");
		require(world.revokeAgentAccessPermission(revokedId, key, &diagnostic), diagnostic);
		require(chosenDoor(revoked->getPath()) == alternateDoor.traversalResource,
			"losing an Interaction point grant did not immediately replan the affected Path");
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
		// remote live state. Once this same threshold is locally observed open, the
		// requirement no longer blocks passage.
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
		require(doorEdge->getDirectedTraversalFacts(target, local).feasible,
			"locally observed open Door did not permit unauthorized passage");

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
		require(!world.isInteractionPointPermissionEligible(created.interiorSelector),
			"onboard Lift destination selector became permission-eligible");

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
	interactionRequirementImmediatelyReplansAffectedPath();
	controlledDoorAuthorization();
	malformedAuthorizationIsTransactional();
	extensibleControlRequirementsPersistIndependently();
	transportLandingRequirementsPersist();
	panelCommitParticipatesInHistory();
}
