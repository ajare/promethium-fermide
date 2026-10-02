#include "Checks.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/BinarySerializer.h"
#include "core/DoorSectorObject.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/SerializationException.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool value, std::string const& message)
	{ if (!value) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> load(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("restored", 1, 1);
		core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(world->deserialize(*reader, work), "Location World did not load");
		world->pauseSimulation();
		return world;
	}

	uint32_t location(core::World& world, bool corridor, uint32_t layer, uint32_t x, uint32_t width)
	{
		return corridor ? world.addCorridor(layer, 0, x, width, 1)
			: world.addRoom("Room", layer, 0, x, width, 1);
	}

	void authoring(bool corridor)
	{
		core::World world("Location authoring", 12, 1);
		auto sector = location(world, corridor, 0, 0, 4);
		auto facade = world.addFacade("Facade", 0, 0, 4, 4, 1);
		auto background = world.addBackground(0, 0, 8, 4, 1);
		world.finishBuild(); world.pauseSimulation();
		require(world.isLocationPermissionEligible(sector)
			&& !world.isLocationPermissionEligible(facade)
			&& !world.isLocationPermissionEligible(background)
			&& !world.isLocationPermissionEligible(999), "Wrong eligible Sector kinds");
		require(world.getLocationPermissionRequirement(sector).empty(), "New Location is restricted");
		auto red = world.addAccessPermission("Red");
		auto blue = world.addAccessPermission("Blue");
		std::string diagnostic;
		world.markSaved();
		auto before = save(world);
		require(world.setLocationPermissionRequirement(sector, {}, &diagnostic)
			&& !world.isModified() && save(world) == before, "Empty requirement no-op changed World");
		for (auto invalid : { std::vector<core::AccessPermissionId>{ {} },
			std::vector<core::AccessPermissionId>{ core::AccessPermissionId{ 257 } },
			std::vector<core::AccessPermissionId>{ core::AccessPermissionId{ 256 } },
			std::vector<core::AccessPermissionId>{ red, red } })
			require(!world.setLocationPermissionRequirement(sector, invalid, &diagnostic)
				&& !diagnostic.empty() && save(world) == before && !world.isModified(), "Invalid permission edit was not atomic");
		for (auto invalidSector : { facade, background, 999u })
		{
			require(!world.setLocationPermissionRequirement(invalidSector, { red }, &diagnostic)
				&& !diagnostic.empty() && save(world) == before, "Invalid Location edit changed World");
			bool refused = false;
			try { (void)world.getLocationPermissionRequirement(invalidSector); }
			catch (std::invalid_argument const& error) { refused = !std::string(error.what()).empty(); }
			require(refused, "Invalid Location query had no diagnostic");
		}
		require(world.setLocationPermissionRequirement(sector, { blue, red }, &diagnostic), diagnostic);
		auto expected = std::vector<core::AccessPermissionId>{ red, blue };
		require(world.getLocationPermissionRequirement(sector) == expected && world.isModified(), "Requirement is not canonical");
		world.markSaved(); before = save(world);
		require(world.setLocationPermissionRequirement(sector, expected, &diagnostic)
			&& !world.isModified() && save(world) == before, "Same requirement dirtied World");
		require(world.renameAccessPermission(red, "Renamed", &diagnostic), diagnostic);
		require(world.getLocationPermissionRequirement(sector) == expected, "Rename changed permission identity");
		require(world.getAccessPermissionUsage(red).locationRequirements == 1, "Usage omitted Location");
		require(world.resumeSimulation(), "Authoring fixture did not resume");
		before = save(world);
		require(!world.setLocationPermissionRequirement(sector, {}, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos && save(world) == before, "Running edit accepted");
		world.pauseSimulation();
		auto yaml = save(world);
		auto restored = load(yaml);
		require(restored->getLocationPermissionRequirement(sector) == expected, "YAML round trip lost requirement");
		core::SerializationWorkData work;
		auto binary = core::BinarySerializer::toString(); world.serialize(*binary, work); binary->serialize();
		auto reader = core::BinarySerializer::fromString(binary->getSerializedString()); reader->deserialize();
		core::World binaryWorld("binary", 1, 1);
		require(binaryWorld.deserialize(*reader, work)
			&& binaryWorld.getLocationPermissionRequirement(sector) == expected, "Binary round trip lost requirement");
		auto legacy = YAML::Load(yaml); legacy["version"] = 31;
		for (auto record : legacy["construction"]) record.remove("locationPermissionRequirement");
		require(load(YAML::Dump(legacy))->getLocationPermissionRequirement(sector).empty(), "Older World became restricted");
		auto absent = YAML::Load(yaml);
		for (auto record : absent["construction"]) record.remove("locationPermissionRequirement");
		require(load(YAML::Dump(absent))->getLocationPermissionRequirement(sector).empty(), "Missing requirement did not default to empty");
		for (auto replacement : { "[999]", "[0]", "[1, 1]", "broken", "{}" })
		{
			auto malformed = YAML::Load(yaml);
			malformed["construction"][0]["locationPermissionRequirement"] = YAML::Load(replacement);
			before = save(*restored);
			bool refused = false;
			try
			{
				auto bad = core::YamlSerializer::fromString(YAML::Dump(malformed)); bad->deserialize();
				restored->deserialize(*bad, work);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused && save(*restored) == before, "Malformed Location requirement mutated target");
		}
		for (auto invalidRecord : { 1u, 2u })
		{
			auto malformed = YAML::Load(yaml);
			malformed["construction"][invalidRecord]["locationPermissionRequirement"] = YAML::Load("[1]");
			bool refused = false;
			try { (void)load(YAML::Dump(malformed)); }
			catch (core::SerializationException const&) { refused = true; }
			require(refused, "Unsupported serialized Sector kind acquired requirement");
		}
	}

	bool visits(core::Path const& path, uint32_t sector)
	{
		for (auto const& node : path.nodes)
			if (node.targetVertex->getSector()->getIndex() == sector) return true;
		return false;
	}

	void routing(bool corridor, bool alternative)
	{
		core::World world("Location routing", 12, 1);
		auto origin = location(world, corridor, 0, 0, 4);
		auto protectedLocation = location(world, corridor, 0, 4, 4);
		auto destination = location(world, corridor, 0, 8, 4);
		world.removeLocationWall(origin, 0, 1);
		world.removeLocationWall(protectedLocation, 0, 1);
		uint32_t startId, targetId, protectedId;
		world.addSectorMarker(origin, 0, 0.5f, &startId);
		world.addSectorMarker(destination, 0, 3.5f, &targetId);
		world.addSectorMarker(protectedLocation, 0, 2.5f, &protectedId);
		auto control = world.addSectorLightSwitch(protectedLocation, 1);
		if (alternative)
		{
			location(world, corridor, 1, 0, 12);
			world.addSectorDoor(0, 0, 1, {});
			world.addSectorDoor(0, 0, 10, {});
		}
		world.finishBuild(); world.pauseSimulation();
		auto red = world.addAccessPermission("Red");
		auto blue = world.addAccessPermission("Blue");
		auto operation = world.addAccessPermission("Control only");
		auto card = world.addPermissionSet("Blue card");
		std::string diagnostic;
		require(world.setPermissionSetAccessPermission(card, blue, true, &diagnostic), diagnostic);
		auto deniedId = world.createAgent("denied", origin, 0, 0.5f);
		auto partialId = world.createAgent("partial", origin, 0, 0.5f);
		auto authorizedId = world.createAgent("authorized", origin, 0, 0.5f);
		auto denied = world.lookupAgent(deniedId).entity;
		auto authorized = world.lookupAgent(authorizedId).entity;
		auto graph = world.getGraph();
		auto start = graph->getVertexByIdentifier(startId);
		auto target = graph->getVertexByIdentifier(targetId);
		auto empty = graph->calculatePath(denied, start, target);
		require(empty && visits(*empty, protectedLocation), "Empty Location requirement changed route choice");
		require(world.setLocationPermissionRequirement(protectedLocation, { red, blue }, &diagnostic), diagnostic);
		require(world.setAgentIndividualPermissionAdherence(deniedId, false, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(partialId, red, &diagnostic), diagnostic);
		require(world.grantAgentAccessPermission(authorizedId, red, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(authorizedId, card, true, &diagnostic), diagnostic);
		for (auto id : { deniedId, partialId })
		{
			auto actor = world.lookupAgent(id).entity;
			auto route = graph->calculatePath(actor, start, target);
			require(alternative ? route && !visits(*route, protectedLocation) : !route, "Unauthorized Location was an intermediate waypoint");
			require(!graph->calculatePath(actor, graph->getVertexByIdentifier(protectedId)), "Protected Marker remained a destination");
			for (auto const& vertex : graph->getVertices())
				if (vertex->getSector()->getIndex() == protectedLocation)
					require(!graph->calculatePath(actor, start, vertex), "A Location-owned/hosted-object vertex bypassed authorization");
		}
		auto permitted = graph->calculatePath(authorized, target);
		require(permitted && visits(*permitted, protectedLocation), "Direct plus Permission set grants did not choose shortcut");
		for (auto const& vertex : graph->getVertices())
			if (vertex->getSector()->getIndex() == protectedLocation)
				require(static_cast<bool>(graph->calculatePath(authorized, start, vertex)), "Authorized hosted vertex is unreachable");
		// Current runtime overlays are read at initial route choice too; this is
		// not a test of mid-journey authorization changes (follow-up scope).
		auto currentId = world.createAgent("current grants", origin, 0, 0.5f);
		require(world.setAgentRuntimeAccessPermissionGrant(currentId, red, true)
			&& world.setAgentRuntimePermissionSetAssignment(currentId, card, true), "Current grants refused");
		auto currentRoute = graph->calculatePath(world.lookupAgent(currentId).entity, target);
		require(currentRoute && visits(*currentRoute, protectedLocation), "Route choice ignored current effective grants");
		// Direct-only and set-only all-of matching, using the same public Graph seam.
		auto directId = world.createAgent("direct", origin, 0, 0.5f);
		require(world.grantAgentAccessPermission(directId, red) && world.grantAgentAccessPermission(directId, blue), "Direct grants refused");
		auto allCard = world.addPermissionSet("All keys");
		require(world.setPermissionSetAccessPermission(allCard, red, true)
			&& world.setPermissionSetAccessPermission(allCard, blue, true), "Set membership refused");
		auto setId = world.createAgent("set only", origin, 0, 0.5f);
		require(world.setAgentPermissionSetAssignment(setId, allCard, true), "Set assignment refused");
		for (auto id : { directId, setId })
		{
			auto route = graph->calculatePath(world.lookupAgent(id).entity, target);
			require(route && visits(*route, protectedLocation), "All-of direct/set grants did not authorize passage");
		}
		// Location grants do not authorize a protected Button operation.
		auto operatorId = world.createAgent("Location-authorized operator", protectedLocation, 0, 1.5f);
		require(world.grantAgentAccessPermission(operatorId, red) && world.grantAgentAccessPermission(operatorId, blue), "Operator grants refused");
		require(world.setInteractionPointPermissionRequirement(control.interactionPoint, { operation }), "Control requirement refused");
		if (alternative)
		{
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
			require(world.setAgentIndividualMobilityProfile(partialId, mobility), "Mobility edit refused");
			require(!graph->calculatePath(world.lookupAgent(partialId).entity, target), "Location constraint was bypassed by Mobility fallback");
		}
		auto restored = load(save(world));
		std::shared_ptr<const core::Vertex> restoredTarget;
		for (auto const& vertex : restored->getGraph()->getVertices())
			if (vertex->getSector()->getIndex() == destination
				&& vertex->getPosition().distanceTo(target->getPosition()) < 0.001f) restoredTarget = vertex;
		require(static_cast<bool>(restoredTarget), "Restored target is missing");
		auto restoredPath = restored->getGraph()->calculatePath(restored->lookupAgent(authorizedId).entity, restoredTarget);
		require(restoredPath && visits(*restoredPath, protectedLocation), "Round trip lost routing authorization");
		require(world.resumeSimulation(), "Routing fixture did not resume");
		auto request = world.requestInteraction(control.interactionPoint, operatorId);
		require(request && world.lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Rejected,
			"Location requirement substituted for operation authorization");
		require(world.moveAgentToMarker(deniedId, world.getMarkerIds()[1]).accepted()
			&& world.moveAgentToMarker(authorizedId, world.getMarkerIds()[1]).accepted(), "Movement intent refused");
		world.advanceTicks(3000);
		bool routeLost = false;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost && event.agent.id == deniedId)
				routeLost = event.routeLossReason == core::RouteLossReason::Unreachable;
		if (alternative)
			require(!routeLost && denied->getSector()->getIndex() == destination
				&& denied->getGlobalPosition().distanceTo(target->getPosition()) < 0.05f,
				"Unauthorized simulation did not take the unrestricted alternative");
		else require(routeLost && denied->getSector()->getIndex() == origin, "No-route simulation did not report unreachable");
		require(authorized->getSector()->getIndex() == destination
			&& authorized->getGlobalPosition().distanceTo(target->getPosition()) < 0.05f, "Authorized simulation did not reach destination");
	}

	void boundary(bool corridor)
	{
		core::World world("Location boundary", 4, 1);
		auto front = location(world, corridor, 0, 0, 4);
		auto back = location(world, corridor, 1, 0, 4);
		auto door = world.addSectorDoor(0, 0, 1, {});
		uint32_t frontMarker, backMarker;
		world.addSectorMarker(front, 0, 1.5f, &frontMarker);
		world.addSectorMarker(back, 0, 1.5f, &backMarker);
		world.finishBuild(); world.pauseSimulation();
		auto key = world.addAccessPermission("Location key");
		auto controlKey = world.addAccessPermission("Door key");
		auto insideId = world.createAgent("inside", front, 0, 1.5f);
		auto outsideId = world.createAgent("outside", back, 0, 1.5f);
		auto inside = world.lookupAgent(insideId).entity;
		auto outside = world.lookupAgent(outsideId).entity;
		auto graph = world.getGraph();
		auto a = graph->getVertexByIdentifier(frontMarker);
		auto b = graph->getVertexByIdentifier(backMarker);
		require(world.setLocationPermissionRequirement(front, { key }), "Location requirement refused");
		require(graph->calculatePath(inside, a, b) && graph->calculatePath(inside, b),
			"Source Location permission was required merely to leave");
		require(!graph->calculatePath(inside, a, a), "Unauthorized origin was admitted as a destination");
		require(!graph->calculatePath(outside, b, a), "Boundary checked the source rather than entered Location");
		// A control grant is not a Location grant, and a Location grant is not a control grant.
		require(world.grantAgentAccessPermission(outsideId, controlKey), "Control grant refused");
		require(!graph->calculatePath(outside, a), "Door grant substituted for Location grant");
		require(world.grantAgentAccessPermission(outsideId, key), "Location grant refused");
		require(world.setManualDoorPermissionRequirement(door.traversalResource, { controlKey }), "Door requirement refused");
		auto locationOnlyId = world.createAgent("Location only", back, 0, 1.5f);
		require(world.grantAgentAccessPermission(locationOnlyId, key), "Location grant refused");
		require(!graph->calculatePath(world.lookupAgent(locationOnlyId).entity, a), "Location grant substituted for Door grant");
		require(static_cast<bool>(graph->calculatePath(outside, a)), "Independent requirements did not compose");
		core::MobilityProfile mobility;
		mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualMobilityProfile(outsideId, mobility), "Mobility edit refused");
		require(!graph->calculatePath(outside, a), "Permissions substituted for Mobility feasibility");
		// An already-open Door and non-adherence still cannot override the hard
		// entered-Location constraint, although they permit ordinary piggybacking.
		auto piggybackId = world.createAgent("piggyback", back, 0, 1.5f);
		require(world.setAgentIndividualPermissionAdherence(piggybackId, false), "Non-adherence refused");
		auto liveDoor = std::static_pointer_cast<const core::DoorSectorObject>(
			door.door.sector->getObject(door.door.index))->getDoor();
		liveDoor->requestOpen(); liveDoor->update(10.0f);
		require(!graph->calculatePath(world.lookupAgent(piggybackId).entity, a), "Open Door/non-adherence bypassed Location requirement");
		require(world.grantAgentAccessPermission(piggybackId, key), "Location grant refused");
		require(static_cast<bool>(graph->calculatePath(world.lookupAgent(piggybackId).entity, a)),
			"Location grant changed independent locally usable Door semantics");
	}
}

void permission_smoke::registerLocations(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "locationAuthoringRoom", [](smoke::Context const&) { authoring(false); } });
	checks.push_back({ "locationAuthoringCorridor", [](smoke::Context const&) { authoring(true); } });
	checks.push_back({ "locationRoutingRoom", [](smoke::Context const&) { routing(false, false); } });
	checks.push_back({ "locationRoutingCorridor", [](smoke::Context const&) { routing(true, false); } });
	checks.push_back({ "locationAlternativeRoom", [](smoke::Context const&) { routing(false, true); } });
	checks.push_back({ "locationAlternativeCorridor", [](smoke::Context const&) { routing(true, true); } });
	checks.push_back({ "locationBoundaryRoom", [](smoke::Context const&) { boundary(false); } });
	checks.push_back({ "locationBoundaryCorridor", [](smoke::Context const&) { boundary(true); } });
}
