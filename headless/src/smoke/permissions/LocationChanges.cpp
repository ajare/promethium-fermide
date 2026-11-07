#include "Checks.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Simulation.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool value, std::string const& message)
	{ if (!value) throw std::runtime_error(message); }

	bool visits(std::shared_ptr<core::Path> const& path, uint32_t sector)
	{
		if (path) for (auto const& node : path->nodes)
			if (node.targetVertex->getSector()->getIndex() == sector) return true;
		return false;
	}

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	struct Fixture
	{
		core::World world{ "Changing Location authorization", 24, 1 };
		uint32_t origin, protectedLocation, destination, other = ~0u;
		uint32_t startId, targetId;
		core::AccessPermissionId red, blue, unrelated;
		core::PermissionSetId card;
		core::AgentId id;

		Fixture(bool corridor, bool alternative)
		{
			auto location = [&](uint32_t layer, uint32_t x, uint32_t width)
			{
				return corridor ? world.addCorridor(layer, 0, x, width, 1)
					: world.addRoom("Room", layer, 0, x, width, 1);
			};
			origin = location(0, 0, 8);
			protectedLocation = location(0, 8, 8);
			destination = location(0, 16, 8);
			world.removeLocationWall(origin, 0, 1);
			world.removeLocationWall(protectedLocation, 0, 1);
			world.addSectorMarker(origin, 0, 0.5f, &startId);
			world.addSectorMarker(origin, 0, 3.5f);
			world.addSectorMarker(destination, 0, 7.5f, &targetId);
			if (alternative)
			{
				other = location(1, 0, 24);
				world.addSectorDoor(0, 0, 1, {});
				world.addSectorDoor(0, 0, 22, {});
			}
			world.finishBuild(); world.pauseSimulation();
			red = world.addAccessPermission("Red");
			blue = world.addAccessPermission("Blue");
			unrelated = world.addAccessPermission("Unrelated");
			card = world.addPermissionSet("Blue card");
			require(world.setPermissionSetAccessPermission(card, blue, true), "Card membership refused");
			id = world.createAgent("Traveller", origin, 0, 0.5f);
			require(world.setAgentIndividualMinimumRoutePlanningTime(id, 0.2f)
				&& world.setAgentIndividualMaximumRoutePlanningTime(id, 0.2f), "Planning timing refused");
			require(world.setAgentIndividualRoutePersistence(id, 1.0f), "Persistence refused");
		}

		core::Agent* agent() { return world.lookupAgent(id).entity; }
		std::shared_ptr<core::Path> select()
		{
			auto path = world.getGraph()->calculatePath(agent(), world.getGraph()->getVertexByIdentifier(targetId));
			require(static_cast<bool>(path), "Initial Path missing");
			agent()->setPath(path, true);
			return path;
		}
		void authorize()
		{
			require(world.grantAgentAccessPermission(id, red)
				&& world.setAgentPermissionSetAssignment(id, card, true), "Authored grants refused");
		}
		void restrict()
		{
			require(world.setLocationPermissionRequirement(protectedLocation, { red, blue }), "Requirement refused");
		}
		void finishPlanning()
		{
			require(agent()->getState() == core::Agent::State::RoutePlanning && !agent()->getPath(), "Not planning");
			if (world.isSimulationPaused()) require(world.resumeSimulation(), "Resume failed");
			auto position = agent()->getGlobalPosition();
			auto remaining = agent()->getRoutePlanningRemainingTicks();
			require(remaining > 0, "Planning interval missing");
			world.advanceTicks(remaining - 1);
			require(agent()->getState() == core::Agent::State::RoutePlanning
				&& agent()->getGlobalPosition() == position, "Planning moved or expired early");
			world.advanceTick();
			require(agent()->getState() != core::Agent::State::RoutePlanning
				&& agent()->getGlobalPosition() == position, "Expiry moved or did not finish");
		}
		void arrive()
		{
			bool enteredProtected = false;
			for (unsigned tick = 0; tick < 6000
				&& (agent()->getPath() || agent()->getState() == core::Agent::State::RoutePlanning); ++tick)
			{
				world.advanceTick();
				enteredProtected |= agent()->getSector()->getIndex() == protectedLocation;
			}
			require(!enteredProtected && !agent()->getPath()
				&& agent()->getSector()->getIndex() == destination
				&& agent()->getGlobalPosition().distanceTo({ 23.5f, 0.0f }) < 0.01f,
				"Authorized alternative did not complete without protected entry");
		}
	};

	unsigned routeLosses(core::World& world, core::AgentId id)
	{
		unsigned count = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost && event.agent.id == id)
			{
				require(event.routeLossReason == core::RouteLossReason::Unreachable, "Unexpected Route loss reason");
				++count;
			}
		return count;
	}

	void losses(bool corridor)
	{
		for (bool alternative : { false, true })
		for (unsigned source = 0; source < 7; ++source)
		{
			Fixture f(corridor, alternative);
			f.authorize();
			if (source < 5) f.restrict();
			else if (source == 6)
				require(f.world.setLocationPermissionRequirement(f.protectedLocation, { f.red }), "Initial requirement refused");
			auto path = f.select();
			require(visits(path, f.protectedLocation), "Initial authorized shortcut not selected");
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, true), "Unrelated gain refused");
			require(f.agent()->getPath() == path, "Unrelated gain disturbed Path");
			if (source == 1 || source == 3)
			{
				require(f.world.resumeSimulation(), "Loss fixture resume failed");
				f.world.advanceTicks(8);
				require(f.agent()->getGlobalPosition().x > 0.5f
					&& f.agent()->getSector()->getIndex() == f.origin, "Loss fixture did not approach future entry");
			}
			auto position = f.agent()->getGlobalPosition();
			switch (source)
			{
			case 0: require(f.world.revokeAgentAccessPermission(f.id, f.red), "Direct loss refused"); break;
			case 1: require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, false), "Runtime direct loss refused"); break;
			case 2: require(f.world.setAgentPermissionSetAssignment(f.id, f.card, false), "Set loss refused"); break;
			case 3: require(f.world.setAgentRuntimePermissionSetAssignment(f.id, f.card, false), "Runtime set loss refused"); break;
			case 4: require(f.world.setPermissionSetAccessPermission(f.card, f.blue, false), "Set membership loss refused"); break;
			case 5: require(f.world.setLocationPermissionRequirement(f.protectedLocation, { f.world.addAccessPermission("Missing") }), "New requirement refused"); break;
			case 6: require(f.world.setLocationPermissionRequirement(f.protectedLocation, { f.red, f.blue, f.world.addAccessPermission("Missing") }), "Tightening refused"); break;
			}
			require(f.agent()->getState() == core::Agent::State::RoutePlanning && !f.agent()->getPath(),
				"Future Location loss did not hard-invalidate: " + std::to_string(source));
			require(f.agent()->getGlobalPosition() == position, "Loss snapped the moving Agent");
			auto remaining = f.agent()->getRoutePlanningRemainingTicks();
			require(!f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, true), "Repeated gain accepted");
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, false), "Unrelated loss refused");
			require(f.agent()->getRoutePlanningRemainingTicks() == remaining, "Unrelated loss restarted interval");
			require(routeLosses(f.world, f.id) == 0, "Loss reported before replan expiry");
			f.finishPlanning();
			if (alternative)
			{
				require(f.agent()->getPath() && !visits(f.agent()->getPath(), f.protectedLocation)
					&& visits(f.agent()->getPath(), f.other), "Hard loss did not bypass persistence for alternative");
				f.arrive();
				require(routeLosses(f.world, f.id) == 0, "Alternative produced Route loss");
			}
			else
			{
				require(!f.agent()->getPath() && f.agent()->getSector()->getIndex() == f.origin
					&& routeLosses(f.world, f.id) == 1, "No-route replan did not report exactly one loss");
				f.world.advanceTicks(600);
				require(routeLosses(f.world, f.id) == 0, "Route loss repeated");
			}
		}
	}

	void gains(bool corridor)
	{
		for (float persistence : { 0.0f, 1.0f })
		for (unsigned source = 0; source < 5; ++source)
		{
			Fixture f(corridor, true);
			f.restrict();
			if (persistence != 1.0f)
				require(f.world.setAgentIndividualRoutePersistence(f.id, persistence), "Persistence refused");
			auto path = f.select();
			require(!visits(path, f.protectedLocation), "Unauthorized shortcut selected");
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, true), "Partial gain refused");
			require(f.agent()->getPath() == path, "Partial all-of gain disturbed valid Path");
			switch (source)
			{
			case 0: require(f.world.grantAgentAccessPermission(f.id, f.blue), "Authored gain refused"); break;
			case 1: require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.blue, true), "Runtime gain refused"); break;
			case 2: require(f.world.setAgentPermissionSetAssignment(f.id, f.card, true), "Authored set gain refused"); break;
			case 3: require(f.world.setAgentRuntimePermissionSetAssignment(f.id, f.card, true), "Runtime set gain refused"); break;
			case 4:
				require(f.world.setPermissionSetAccessPermission(f.card, f.blue, false)
					&& f.world.setAgentPermissionSetAssignment(f.id, f.card, true), "Empty card refused");
				require(f.agent()->getPath() == path, "Empty card disturbed Path");
				require(f.world.setPermissionSetAccessPermission(f.card, f.blue, true), "Membership gain refused"); break;
			}
			require(f.agent()->getState() == core::Agent::State::RoutePlanning && !f.agent()->getPath(), "Relevant gain did not plan voluntarily");
			require(f.world.resumeSimulation(), "Gain fixture resume failed");
			f.world.advanceTicks(3);
			auto remaining = f.agent()->getRoutePlanningRemainingTicks();
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, true), "Unrelated gain refused");
			require(!f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, true), "Duplicate gain accepted");
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, false)
				&& f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, true), "Repeated relevant changes refused");
			require(f.agent()->getRoutePlanningRemainingTicks() == remaining, "Gains restarted planning");
			f.finishPlanning();
			require(persistence == 1.0f ? f.agent()->getPath() == path
				: f.agent()->getPath() != path && visits(f.agent()->getPath(), f.protectedLocation), "Gain ignored Route persistence");
			require(routeLosses(f.world, f.id) == 0, "Successful voluntary planning reported loss");
		}
	}

	void editsAndRetainedPaths(bool corridor)
	{
		for (bool noRoute : { false, true })
		for (bool grantLoss : { false, true })
		{
			Fixture f(corridor, true);
			f.authorize(); f.restrict();
			auto path = f.select();
			require(f.world.setLocationPermissionRequirement(f.destination, { f.blue })
				&& f.world.setLocationPermissionRequirement(f.protectedLocation, { f.red })
				&& f.world.setLocationPermissionRequirement(f.other, { f.unrelated }), "Coherent edit refused");
			require(f.agent()->getPath() == path, "Satisfied/loosening/unrelated edit disturbed Path");
			// A relevant gain begins voluntary planning with a valid shortcut retained.
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, true), "Gain refused");
			require(f.world.resumeSimulation(), "Resume failed");
			f.world.advanceTicks(3);
			auto remaining = f.agent()->getRoutePlanningRemainingTicks();
			f.world.pauseSimulation();
			if (grantLoss)
				require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, false), "Retained grant loss refused");
			else
				require(f.world.setLocationPermissionRequirement(f.protectedLocation, { f.red, f.world.addAccessPermission("Missing") }), "Retained tightening refused");
			if (noRoute) require(f.world.setLocationPermissionRequirement(f.destination, { f.blue, f.world.addAccessPermission("No destination") }), "Destination edit refused");
			require(f.agent()->getRoutePlanningRemainingTicks() == remaining, "Hard upgrade resampled interval");
			f.finishPlanning();
			if (noRoute)
			{
				require(!f.agent()->getPath() && routeLosses(f.world, f.id) == 1, "Invalid retained Path was resumed");
				f.world.advanceTicks(100);
				require(routeLosses(f.world, f.id) == 0, "Retained failure repeated Route loss");
			}
			else
			{
				require(f.agent()->getPath() && !visits(f.agent()->getPath(), f.protectedLocation), "Retained invalidation kept shortcut");
				f.arrive();
			}
		}
	}

	void staleEntry(bool corridor)
	{
		{
			Fixture f(corridor, true);
			f.authorize(); f.restrict();
			auto stale = f.select();
			require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, false), "Stale shortcut loss refused");
			f.agent()->setPath(stale, true);
			require(f.world.resumeSimulation(), "Stale shortcut resume failed");
			// The Agent may approach the boundary on the stale Path, but cannot
			// enter the protected intermediate Location; the later replan succeeds.
			f.arrive();
			require(routeLosses(f.world, f.id) == 0, "Stale shortcut lost an available alternative");
		}
		for (bool doorBoundary : { false, true })
		for (bool adherence : { false, true })
		{
			core::World world("Stale Location entry", 8, 1);
			auto location = [&](uint32_t layer, uint32_t x)
			{
				return corridor ? world.addCorridor(layer, 0, x, 4, 1)
					: world.addRoom("Room", layer, 0, x, 4, 1);
			};
			auto origin = location(0, 0);
			auto destination = location(doorBoundary ? 1 : 0, doorBoundary ? 0 : 4);
			core::World::CreateDoorResult door;
			if (doorBoundary) door = world.addSectorDoor(0, 0, 1, {});
			else world.removeLocationWall(origin, 0, 1);
			uint32_t target;
			world.addSectorMarker(destination, 0, doorBoundary ? 1.5f : 3.5f, &target);
			world.finishBuild(); world.pauseSimulation();
			auto key = world.addAccessPermission("Entry key");
			auto id = world.createAgent("Stale traveller", origin, 0, doorBoundary ? 1.5f : 3.5f);
			require(world.grantAgentAccessPermission(id, key)
				&& world.setAgentIndividualPermissionAdherence(id, adherence), "Initial authorization refused");
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
			require(world.setAgentIndividualMobilityProfile(id, mobility), "Mobility refused");
			require(world.setLocationPermissionRequirement(destination, { key }), "Entry requirement refused");
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexByIdentifier(target));
			require(static_cast<bool>(path), "Authorized stale fixture Path missing");
			require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Entry loss refused");
			if (doorBoundary)
			{
				auto liveDoor = std::static_pointer_cast<const core::DoorSectorObject>(door.door.sector->getObject(door.door.index))->getDoor();
				liveDoor->requestOpen(); liveDoor->update(10.0f);
			}
			// Bypass change-time invalidation through the public Path assignment seam.
			agent->setPath(path, true);
			require(world.resumeSimulation(), "Stale fixture resume failed");
			for (unsigned tick = 0; tick < 600; ++tick)
			{
				world.advanceTick();
				require(agent->getSector()->getIndex() == origin, "Stale Path entered unauthorized Location");
			}
			require(!agent->getPath() && routeLosses(world, id) == 1, "Stale entry refusal did not report one Route loss");
			world.advanceTicks(100);
			require(routeLosses(world, id) == 0, "Stale refusal repeated Route loss");
		}
	}

	void resetAndUnrelated(bool corridor)
	{
		Fixture f(corridor, true);
		f.authorize(); f.restrict();
		auto path = f.select();
		// Removing a redundant source changes neither effective grants nor Path.
		require(f.world.grantAgentAccessPermission(f.id, f.blue), "Redundant direct grant refused");
		require(f.world.setAgentRuntimePermissionSetAssignment(f.id, f.card, false), "Redundant set removal refused");
		require(f.agent()->getPath() == path, "Redundant source removal disturbed Path");
		require(f.world.setAgentRuntimePermissionSetAssignment(f.id, f.card, true), "Set restoration refused");
		f.world.markSaved();
		auto authored = save(f.world);
		require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, true)
			&& f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.unrelated, false), "Unrelated overlay refused");
		require(f.agent()->getPath() == path, "Unrelated changes disturbed valid Path");
		require(f.world.setAgentRuntimeAccessPermissionGrant(f.id, f.red, false)
			&& f.world.setAgentRuntimePermissionSetAssignment(f.id, f.card, false), "Runtime revocation refused");
		require(save(f.world) == authored && !f.world.isModified(), "Runtime authorization rewrote authored World");
		f.finishPlanning();
		require(!visits(f.agent()->getPath(), f.protectedLocation), "Runtime loss did not use alternative");
		f.world.resetSimulation();
		require(f.world.getAgentCurrentDirectAccessGrants(f.id) == f.world.getAgentDirectAccessGrants(f.id)
			&& f.world.getAgentCurrentPermissionSetAssignments(f.id) == f.world.getAgentPermissionSetAssignments(f.id), "Reset did not restore authored grants");
		require(visits(f.agent()->getPath(), f.protectedLocation), "Reset did not restore authorized shortcut");
		require(f.world.resumeSimulation(), "Reset resume failed");
		f.world.advanceTicks(6000);
		require(!f.agent()->getPath() && f.agent()->getSector()->getIndex() == f.destination
			&& routeLosses(f.world, f.id) == 0, "Reset authorization did not restore movement");
	}
}

void permission_smoke::registerLocationChanges(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "locationLossesRoom", [](smoke::Context const&) { losses(false); } });
	checks.push_back({ "locationGainsRoom", [](smoke::Context const&) { gains(false); } });
	checks.push_back({ "locationRequirementChangesRoom", [](smoke::Context const&) { editsAndRetainedPaths(false); } });
	checks.push_back({ "locationStaleEntryRoom", [](smoke::Context const&) { staleEntry(false); } });
	checks.push_back({ "locationResetAndUnrelatedRoom", [](smoke::Context const&) { resetAndUnrelated(false); } });
	checks.push_back({ "locationLossesCorridor", [](smoke::Context const&) { losses(true); } });
	checks.push_back({ "locationGainsCorridor", [](smoke::Context const&) { gains(true); } });
	checks.push_back({ "locationRequirementChangesCorridor", [](smoke::Context const&) { editsAndRetainedPaths(true); } });
	checks.push_back({ "locationStaleEntryCorridor", [](smoke::Context const&) { staleEntry(true); } });
	checks.push_back({ "locationResetAndUnrelatedCorridor", [](smoke::Context const&) { resetAndUnrelated(true); } });
}
