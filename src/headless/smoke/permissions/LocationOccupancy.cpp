#include "Checks.h"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/BinarySerializer.h"
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

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	unsigned outcomes(core::World& world, core::AgentId id, bool lost)
	{
		unsigned count = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.agent.id == id && (event.type == core::SimulationEventType::RouteLost
				|| event.type == core::SimulationEventType::DestinationReached))
			{
				require(event.type == (lost ? core::SimulationEventType::RouteLost : core::SimulationEventType::DestinationReached), "Wrong movement outcome");
				if (lost) require(event.routeLossReason == core::RouteLossReason::Unreachable, "Wrong Route loss reason");
				++count;
			}
		return count;
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Occupancy", 12, 1);
		uint32_t inside, outside, internalVertex, externalVertex;
		core::MarkerId internalMarker, externalMarker;
		core::AccessPermissionId red, blue;
		core::PermissionSetId card;
		core::AgentId id;

		Fixture(bool corridor, bool exit = true)
		{
			inside = corridor ? world->addCorridor(0, 0, 0, 6, 1) : world->addRoom("Protected", 0, 0, 0, 6, 1);
			outside = corridor ? world->addCorridor(0, 0, 6, 6, 1) : world->addRoom("Outside", 0, 0, 6, 6, 1);
			if (exit) world->removeLocationWall(inside, 0, 1);
			world->addSectorMarker(inside, 0, 3.5f, &internalVertex);
			world->addSectorMarker(inside, 0, 4.5f);
			world->addSectorMarker(outside, 0, 5.5f, &externalVertex);
			internalMarker = world->getMarkerIds()[0]; externalMarker = world->getMarkerIds()[2];
			world->finishBuild(); world->pauseSimulation();
			red = world->addAccessPermission("Red"); blue = world->addAccessPermission("Blue");
			card = world->addPermissionSet("Card");
			require(world->setPermissionSetAccessPermission(card, blue, true), "Card refused");
			id = world->createAgent("Occupant", inside, 0, 0.5f);
			require(world->setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
				&& world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Timing refused");
		}
		core::Agent* agent() { return world->lookupAgent(id).entity; }
		void restrict() { require(world->setLocationPermissionRequirement(inside, { red, blue }), "Requirement refused"); }
		std::shared_ptr<const core::Vertex> target(bool internal)
		{
			return world->getGraph()->getClosestVertexInSector(world->getSector(internal ? inside : outside).get(),
				internal ? core::Vector2{ 3.5f, 0 } : core::Vector2{ 11.5f, 0 });
		}
		void restore(unsigned format)
		{
			if (format == 1)
			{
				auto reader = core::YamlSerializer::fromString(save(*world)); reader->deserialize();
				world = std::make_shared<core::World>("Restored", 1, 1);
				core::SerializationWorkData work;
				world->deserialize(*reader, work);
			}
			else if (format == 2)
			{
				core::SerializationWorkData work;
				auto writer = core::BinarySerializer::toString();
				world->serialize(*writer, work); writer->serialize();
				auto reader = core::BinarySerializer::fromString(writer->getSerializedString()); reader->deserialize();
				world = std::make_shared<core::World>("Restored", 1, 1);
				world->deserialize(*reader, work);
			}
		}
	};

	void occupancy(bool corridor)
	{
		for (unsigned format : { 0u, 1u, 2u })
		for (unsigned grants = 0; grants < 5; ++grants)
		for (bool exit : { false, true })
		for (bool internal : { false, true })
		{
			Fixture f(corridor, exit);
			if (grants == 1 || grants == 3 || grants == 4) require(f.world->grantAgentAccessPermission(f.id, f.red), "Red refused");
			if (grants == 2 || grants == 4) require(f.world->setAgentPermissionSetAssignment(f.id, f.card, true), "Card refused");
			if (grants == 3) require(f.world->grantAgentAccessPermission(f.id, f.blue), "Blue refused");
			f.restrict(); f.restore(format);
			auto const authorized = grants >= 3;
			auto const lost = internal ? !authorized : !exit;
			auto const position = f.agent()->getGlobalPosition();
			auto target = f.target(internal);
			auto path = f.world->getGraph()->calculatePath(f.agent(), target);
			require(static_cast<bool>(path) != lost, "Wrong source-only route feasibility");
			if (path && !authorized)
			{
				bool left = false;
				for (auto const& node : path->nodes)
				{
					auto const inSource = node.targetVertex->getSector()->getIndex() == f.inside;
					require(!left || !inSource, "Path re-entered source");
					left |= !inSource;
				}
				require(left, "Escape stayed inside");
			}
			require(f.world->moveAgentToMarker(f.id, internal ? f.internalMarker : f.externalMarker).accepted(), "Intent refused");
			require(f.world->resumeSimulation(), "Resume refused");
			f.world->advanceTicks(1800);
			require(f.agent()->getState() == core::Agent::State::Idle && !f.agent()->getPath(), "Movement did not settle");
			require(lost ? f.agent()->getGlobalPosition() == position
				: f.agent()->getGlobalPosition().distanceTo(target->getPosition()) < 0.01f, "Wrong final position / invented evacuation");
			require(outcomes(*f.world, f.id, lost) == 1, "Missing or repeated outcome");
			if (!lost && !internal && !authorized)
			{
				require(!f.world->getGraph()->calculatePath(f.agent(), f.target(true)), "Re-entry allowed");
				auto outsidePosition = f.agent()->getGlobalPosition();
				require(f.world->moveAgentToMarker(f.id, f.internalMarker).accepted(), "Re-entry intent refused");
				f.world->advanceTicks(120);
				require(f.agent()->getGlobalPosition() == outsidePosition && !f.agent()->getPath()
					&& outcomes(*f.world, f.id, true) == 1, "Left occupant re-entered");
			}
			f.world->advanceTicks(60);
			require(outcomes(*f.world, f.id, lost) == 0, "Outcome repeated");
		}
		// Restore destination intent as well as occupancy; the rebuilt escape
		// Path is valid, but a persisted internal destination is not a load failure.
		for (unsigned format : { 1u, 2u })
		for (bool internal : { false, true })
		{
			Fixture restored(corridor);
			auto path = restored.world->getGraph()->calculatePath(restored.agent(), restored.target(internal));
			restored.agent()->setPath(path, true);
			restored.restrict(); restored.restore(format);
			require(restored.agent()->getSector()->getIndex() == restored.inside
				&& static_cast<bool>(restored.agent()->getPath()) != internal, "Restored occupancy/intent was refused or bypassed source-only rule");
			if (!internal)
			{
				require(restored.world->resumeSimulation(), "Restored escape resume refused");
				restored.world->advanceTicks(1800);
				require(restored.agent()->getSector()->getIndex() == restored.outside
					&& restored.agent()->getGlobalPosition() == core::Vector2{ 11.5f, 0 }, "Restored escape did not complete");
			}
		}
		// An existing internal Path becomes invalid when the occupant loses access.
		Fixture f(corridor);
		auto stale = f.world->getGraph()->calculatePath(f.agent(), f.world->getGraph()->getVertexByIdentifier(f.internalVertex));
		f.agent()->setPath(stale, true);
		f.restrict();
		require(f.agent()->getState() == core::Agent::State::RoutePlanning && !f.agent()->getPath(), "Internal occupancy loss did not plan");
		f.agent()->setPath(stale, true);
		require(f.agent()->getState() == core::Agent::State::RoutePlanning, "Stale internal destination bypassed planning");
		require(f.world->resumeSimulation(), "Resume refused");
		f.world->advanceTicks(120);
		require(f.agent()->getGlobalPosition() == core::Vector2{ 0.5f, 0 } && outcomes(*f.world, f.id, true) == 1, "Internal loss moved occupant");
	}

	void committedEntry(bool corridor)
	{
		for (bool committed : { false, true })
		for (bool escape : { false, true })
		for (bool coincident : { false, true })
		for (bool setLoss : { false, true })
		{
			core::World world("Committed entry", 8, 1);
			auto location = [&](uint32_t layer, uint32_t x)
			{ return corridor ? world.addCorridor(layer, 0, x, 4, 1) : world.addRoom("Room", layer, 0, x, 4, 1); };
			auto origin = location(0, 0), protectedLocation = location(1, 0), outside = location(1, 4);
			world.addSectorDoor(0, 0, 1, {});
			world.removeLocationWall(protectedLocation, 0, 1);
			uint32_t targetId;
			world.addSectorMarker(escape ? outside : protectedLocation, 0, coincident && !escape ? 1.5f : 3.5f, &targetId);
			world.finishBuild(); world.pauseSimulation();
			auto key = world.addAccessPermission("Entry");
			auto card = world.addPermissionSet("Card");
			require(world.setPermissionSetAccessPermission(card, key, true), "Card refused");
			auto id = world.createAgent("Traveller", origin, 0, 1.5f);
			require(setLoss ? world.setAgentPermissionSetAssignment(id, card, true) : world.grantAgentAccessPermission(id, key), "Grant refused");
			require(world.setLocationPermissionRequirement(protectedLocation, { key }), "Requirement refused");
			require(world.setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
				&& world.setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Timing refused");
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexByIdentifier(targetId));
			require(static_cast<bool>(path), "Initial Path missing");
			agent->setPath(path, true);
			require(world.resumeSimulation(), "Resume refused");
			if (committed)
			{
				for (unsigned tick = 0; tick < 600 && agent->getState() != core::Agent::State::TraversingEdge; ++tick) world.advanceTick();
				require(agent->getState() == core::Agent::State::TraversingEdge && agent->getSector()->getIndex() == origin, "Crossing never began");
			}
			auto const position = agent->getGlobalPosition();
			require(setLoss ? world.setAgentRuntimePermissionSetAssignment(id, card, false)
				: world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Loss refused");
			require(agent->getGlobalPosition() == position, "Loss snapped crossing");
			if (committed)
			{
				require(agent->getState() == core::Agent::State::TraversingEdge && agent->getPath() == path, "Committed crossing interrupted");
				for (unsigned tick = 0; tick < 30 && agent->getSector()->getIndex() == origin; ++tick) world.advanceTick();
				require(agent->getSector()->getIndex() == protectedLocation && agent->getGlobalPosition() == position
					&& agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(), "Completed entry did not immediately plan safely");
			}
			else require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(), "Pre-entry loss did not plan");
			auto remaining = agent->getRoutePlanningRemainingTicks();
			require(remaining > 0, "Planning interval missing");
			world.advanceTicks(remaining - 1);
			require(agent->getState() == core::Agent::State::RoutePlanning && agent->getGlobalPosition() == position, "Planning moved or expired early");
			world.advanceTick();
			require(agent->getGlobalPosition() == position, "Planning expiry moved");
			world.advanceTicks(1200);
			auto const lost = !committed || !escape;
			require(outcomes(world, id, lost) == 1 && agent->getState() == core::Agent::State::Idle && !agent->getPath(), "Wrong post-commit outcome");
			require(agent->getSector()->getIndex() == (committed ? (escape ? outside : protectedLocation) : origin), "Wrong entry/escape sector");
		}
	}

	void placements(bool corridor)
	{
		Fixture f(corridor); f.restrict();
		f.world->markSaved(); f.world->consumeSimulationEvents();
		auto document = save(*f.world);
		auto original = f.agent()->getGlobalPosition();
		auto outsideId = f.world->createAgent("Outside", f.outside, 0, 1.5f);
		auto mover = f.world->lookupAgent(outsideId).entity;
		f.world->markSaved(); f.world->consumeSimulationEvents(); document = save(*f.world);
		auto reject = [&](std::function<void()> operation, bool missingRed, bool missingBlue)
		{
			bool refused = false;
			try { operation(); }
			catch (std::invalid_argument const& error)
			{
				auto text = std::string(error.what()); refused = true;
				require((text.find("('Red')") != std::string::npos) == missingRed
					&& (text.find("('Blue')") != std::string::npos) == missingBlue, "Incomplete/effective-grant diagnostics: " + text);
			}
			require(refused && save(*f.world) == document && !f.world->isModified(), "Rejected placement changed document");
			require(f.world->consumeSimulationEvents().empty(), "Rejected placement changed events");
			require(f.agent()->getGlobalPosition() == original && mover->getSector()->getIndex() == f.outside
				&& f.world->getSector(f.inside)->getAgents().size() == 1, "Rejected placement changed ownership/positions");
		};
		for (bool running : { false, true })
		{
			if (running) require(f.world->resumeSimulation(), "Spawn resume refused");
			f.world->consumeSimulationEvents();
			reject([&] { f.world->createAgent("No grants", f.inside); }, true, true);
			reject([&] { f.world->createAgent("No grants", f.inside, 0, 1.5f); }, true, true);
			reject([&] { f.world->createAgent("Partial direct", f.inside, 0, 1.5f, { f.red }, {}); }, false, true);
			reject([&] { f.world->createAgent("Partial set", f.inside, {}, { f.card }); }, true, false);
			reject([&] { f.world->validateAgentLocationPlacement(*f.world->getSector(f.inside), *mover); }, true, true);
			reject([&] { const_cast<core::Sector*>(f.world->getSector(f.inside).get())->enterAgent(mover,
				f.world->getGraph()->getVertexByIdentifier(f.internalVertex)); }, true, true);
			reject([&] { const_cast<core::Sector*>(f.world->getSector(f.inside).get())->enterAgent(mover, 0, 1.5f); }, true, true);
			reject([&] { const_cast<core::Sector*>(f.world->getSector(f.inside).get())->enterAgent(mover); }, true, true);
			f.world->pauseSimulation();
		}
		f.world->consumeSimulationEvents();
		// A current runtime union, not authored grants alone, governs relocation.
		require(f.world->setAgentRuntimeAccessPermissionGrant(outsideId, f.red, true)
			&& f.world->setAgentRuntimePermissionSetAssignment(outsideId, f.card, true), "Runtime authorization refused");
		f.world->validateAgentLocationPlacement(*f.world->getSector(f.inside), *mover);
		require(f.world->setAgentRuntimeAccessPermissionGrant(outsideId, f.red, false), "Runtime revocation refused");
		reject([&] { const_cast<core::Sector*>(f.world->getSector(f.inside).get())->enterAgent(mover, 0, 1.5f); }, true, false);
		auto authorized = f.world->createAgent("Authorized", f.inside, 0, 1.5f, { f.red }, { f.card });
		require(authorized.value == 3 && f.world->lookupAgent(authorized).entity->getSector()->getIndex() == f.inside,
			"Authorized placement refused / rejected placement consumed IDs");
		auto allCard = f.world->addPermissionSet("All");
		require(f.world->setPermissionSetAccessPermission(allCard, f.red, true)
			&& f.world->setPermissionSetAccessPermission(allCard, f.blue, true), "All card refused");
		require(static_cast<bool>(f.world->createAgent("Set only", f.inside, {}, { allCard })), "Set-only placement refused");
		require(static_cast<bool>(f.world->createAgent("Direct only", f.inside, { f.red, f.blue }, {})), "Direct-only placement refused");
		require(f.world->renameAccessPermission(f.red, "Renamed red"), "Permission rename refused");
		std::string diagnostic;
		require(!f.world->canPlaceAgentInLocation(f.inside, {}, { f.card }, &diagnostic)
			&& diagnostic.find("1 ('Renamed red')") != std::string::npos
			&& diagnostic.find("('Blue')") == std::string::npos, "Placement diagnostic did not use current identity/name/set grants");
	}
}

void permission_smoke::registerLocationOccupancy(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "locationOccupancyRoom", [](smoke::Context const&) { occupancy(false); } });
	checks.push_back({ "locationOccupancyCorridor", [](smoke::Context const&) { occupancy(true); } });
	checks.push_back({ "locationCommittedEntryRoom", [](smoke::Context const&) { committedEntry(false); } });
	checks.push_back({ "locationCommittedEntryCorridor", [](smoke::Context const&) { committedEntry(true); } });
	checks.push_back({ "locationPlacementRoom", [](smoke::Context const&) { placements(false); } });
	checks.push_back({ "locationPlacementCorridor", [](smoke::Context const&) { placements(true); } });
}
