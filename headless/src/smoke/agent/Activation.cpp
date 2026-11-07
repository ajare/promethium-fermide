// Migrated from AgentActivationSmokeChecks.cpp (#285); core dependency tier.
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"
#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeWorld(core::World& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// A whole-document load, the way the editor opens a file.
	std::shared_ptr<core::World> loadWorld(std::string const& yaml)
	{
		auto loaded = std::make_shared<core::World>("Loaded World", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised World could not be read back");
		require(loaded->deserialize(*reader, workData), "The World did not reload");
		return loaded;
	}

	core::Agent const* findAgentByName(core::World const& world, std::string const& name)
	{
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
		{
			for (auto const& sector : world.getSectors(layer))
			{
				if (!sector) continue;
				for (auto* agent : sector->getAgents())
					if (agent && agent->getName() == name) return agent;
			}
		}
		return nullptr;
	}

	core::AgentSnapshot snapshotOf(core::World const& world, core::AgentId id)
	{
		for (auto const& entry : world.getSimulationSnapshot().agents)
			if (entry.id == id) return entry;
		throw std::runtime_error("The snapshot does not name the Agent");
	}

	// Terminal interaction requests are retired once no live owner names them
	// (#183), so an outcome is read from the published event stream rather than
	// looking the hot-registry record up after the fact.
	std::map<core::InteractionRequestId, core::InteractionResult> observedInteractionResults(
		core::World& world, std::vector<core::InteractionRequestId> const& ids)
	{
		std::map<core::InteractionRequestId, core::InteractionResult> results;
		for (auto id : ids) results.emplace(id, core::InteractionResult::Pending);
		for (auto const& event : world.consumeSimulationEvents())
		{
			if (event.type != core::SimulationEventType::InteractionRequestChanged
				&& event.type != core::SimulationEventType::InteractionRequestAdded)
			{
				continue;
			}
			if (event.interactionRequest.result == core::InteractionResult::Pending) continue;
			auto found = results.find(event.interactionRequest.id);
			if (found != results.end()) found->second = event.interactionRequest.result;
		}
		return results;
	}

	// One corridor, one marker at its far end, and two Agents at the near
	// end, each already walking toward the marker. The walk is what tells a
	// simulated Agent from a parked one.
	struct WalkFixture
	{
		core::World world;
		uint32_t corridor;
		uint32_t destinationIdentifier;
		core::AgentId first;
		core::AgentId second;

		WalkFixture()
			: world("Activation walk", 12, 3)
			, destinationIdentifier{ 0x41475231u }
		{
			corridor = world.addCorridor(0, 0, 8);
			world.addSectorMarker(corridor, 0, 7.5f, &destinationIdentifier);
			world.finishBuild();

			auto const destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
			require(destination != nullptr, "The walk destination vertex is missing");

			first = world.createAgent("Walker", corridor, 0, 0.5f);
			second = world.createAgent("Parker", corridor, 0, 1.5f);
			for (auto id : { first, second })
			{
				auto* agent = world.lookupAgent(id).entity;
				auto path = world.getGraph()->calculatePath(agent, destination);
				require(path && !path->nodes.empty(), "The walk route could not be calculated");
				agent->setPath(std::move(path), true);
			}
		}
	};

	void everyAgentStartsActivated()
	{
		core::World world("Fresh World", 4, 1);
		auto const corridor = world.addCorridor(0, 0, 4);
		world.finishBuild();
		auto const id = world.createAgent("Alice", corridor);
		auto* agent = world.lookupAgent(id).entity;
		require(agent->isActive(), "A newly created Agent has to start activated");
		require(snapshotOf(world, id).active, "A newly created Agent's snapshot has to say activated");
	}

	void activationIsRefusedWhileTheSimulationRuns()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// A fresh World simulates from the first tick, so this run is live.
		require(!fixture.world.isSimulationPaused(), "The fixture has to start running");
		require(!fixture.world.canSetAgentActive(fixture.second, false, &diagnostic),
			"canSetAgentActive agreed to a deactivation while the simulation was running");
		require(!diagnostic.empty(), "A refused deactivation gave no reason");
		require(!fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"setAgentActive deactivated an Agent while the simulation was running");
		require(fixture.world.lookupAgent(fixture.second).entity->isActive(),
			"A refused deactivation changed the Agent anyway");
		require(snapshotOf(fixture.world, fixture.second).active,
			"A refused deactivation reached the snapshot anyway");

		require(!fixture.world.canSetAgentActive(core::AgentId{}, false, &diagnostic),
			"canSetAgentActive accepted an Agent the World does not own");
		require(!diagnostic.empty(), "An unknown Agent gave no reason for the refusal");
	}

	void deactivatedAgentsAreNotSimulated()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// Let both Agents get underway, then park one. The parked position is
		// read after the pause: pausing snaps a mid-traversal Agent back to
		// the vertex its route began from, and it is the post-pause spot the
		// deactivated Agent has to keep.
		fixture.world.advanceTicks(20);
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		auto const* parker = fixture.world.lookupAgent(fixture.second).entity;
		auto const parkedAt = parker->getGlobalPosition();
		require(fixture.world.resumeSimulation(), "The fixture could not resume");

		// The walk is ~7 units at 0.5 units a second of fixed 1/60 ticks, so
		// well over a thousand ticks are needed to actually finish it.
		for (uint32_t tick = 0; tick < 1500; ++tick) fixture.world.advanceTick();

		auto* walker = fixture.world.lookupAgent(fixture.first).entity;
		require(walker->getGlobalPosition().x > 6.0f,
			"The activated Agent did not finish its walk, so the comparison proved nothing");
		require(parker->getGlobalPosition() == parkedAt,
			"A deactivated Agent moved while the simulation ran");
		require(parker->getState() == core::Agent::State::Idle,
			"A deactivated Agent left the Idle state without being simulated");
		require(!parker->getPath(), "A deactivated Agent's torn-down route came back on its own");
		require(!snapshotOf(fixture.world, fixture.second).active,
			"The snapshot did not report the deactivation");
		require(snapshotOf(fixture.world, fixture.first).active,
			"The snapshot reported the walking Agent as deactivated");

		// Reactivating after the resume cannot resurrect the route the pause
		// tore down: the Agent stands where it was parked until the author
		// gives it a new one.
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		require(fixture.world.resumeSimulation(), "The fixture could not resume again");
		for (uint32_t tick = 0; tick < 300; ++tick) fixture.world.advanceTick();
		require(parker->getGlobalPosition() == parkedAt,
			"Reactivation resurrected a route the pause had dropped");
	}

	void reactivationWhilePausedPutsTheAgentBackUnderTheSimulation()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		// Changed its mind before the resume: the pause's retained route must
		// replay like any other activated Agent's.
		require(fixture.world.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		require(fixture.world.resumeSimulation(), "The fixture could not resume");

		for (uint32_t tick = 0; tick < 1500; ++tick) fixture.world.advanceTick();
		require(fixture.world.lookupAgent(fixture.second).entity->getGlobalPosition().x > 6.0f,
			"An Agent reactivated while paused was not simulated on resume");
	}

	void wakingSkipsDeactivatedAgents()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		// Both Agents are Idle with their routes torn down. Hand each a route
		// without starting it, then wake the world: only the activated Agent
		// may answer.
		auto const destination = fixture.world.getGraph()->getVertexByIdentifier(
			fixture.destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		for (auto id : { fixture.first, fixture.second })
		{
			auto* agent = fixture.world.lookupAgent(id).entity;
			auto path = fixture.world.getGraph()->calculatePath(agent, destination);
			require(path && !path->nodes.empty(), "The walk route could not be calculated");
			agent->setPath(std::move(path), false);
		}

		fixture.world.wakeAllAgents();
		require(fixture.world.lookupAgent(fixture.first).entity->getState()
			== core::Agent::State::MovingToVertex, "Waking did not start the activated Agent");
		require(fixture.world.lookupAgent(fixture.second).entity->getState()
			== core::Agent::State::Idle, "Waking started a deactivated Agent");

		require(fixture.world.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		fixture.world.wakeAllAgents();
		require(fixture.world.lookupAgent(fixture.second).entity->getState()
			== core::Agent::State::MovingToVertex, "Waking did not start the reactivated Agent");
	}

	void groupsToggleTheirCurrentMembersEnMasse()
	{
		core::World world("Group activation", 6, 1);
		auto const corridor = world.addCorridor(0, 0, 6);
		world.finishBuild();

		auto const crew = world.addAgentGroup("Crew");
		auto const visitors = world.addAgentGroup("Visitors");
		auto const empty = world.addAgentGroup("Empty");
		auto const alice = world.createAgent("Alice", corridor, 0, 0.5f);
		auto const bob = world.createAgent("Bob", corridor, 0, 1.5f);
		auto const visitor = world.createAgent("Visitor", corridor, 0, 2.5f);
		auto const ungrouped = world.createAgent("Ungrouped", corridor, 0, 3.5f);
		std::string diagnostic;
		require(world.setAgentGroup(alice, crew, &diagnostic)
			&& world.setAgentGroup(bob, crew, &diagnostic)
			&& world.setAgentGroup(visitor, visitors, &diagnostic),
			"The activation fixture could not assign its Agent groups: " + diagnostic);

		// A group is an aggregate view of its members' own flags. Empty groups
		// have no active member, while every newly-created occupied group does.
		require(world.isAgentGroupActive(crew),
			"A group of activated Agents was not reported active");
		require(!world.isAgentGroupActive(empty),
			"An empty Agent group was reported active");

		// The bulk operation has the same pause gate as one Agent and validates
		// before touching anybody.
		require(!world.setAgentGroupActive(crew, false, &diagnostic),
			"A running simulation allowed an Agent group to be deactivated");
		require(!diagnostic.empty(), "A refused group deactivation gave no reason");
		require(world.lookupAgent(alice).entity->isActive()
			&& world.lookupAgent(bob).entity->isActive(),
			"A refused group deactivation changed some members");

		world.pauseSimulation();
		require(world.setAgentGroupActive(crew, false, &diagnostic),
			"The paused group deactivation was refused: " + diagnostic);
		require(!world.lookupAgent(alice).entity->isActive()
			&& !world.lookupAgent(bob).entity->isActive(),
			"Group deactivation did not deactivate every current member");
		require(world.lookupAgent(visitor).entity->isActive()
			&& world.lookupAgent(ungrouped).entity->isActive(),
			"Group deactivation changed an Agent outside the group");
		require(!world.isAgentGroupActive(crew),
			"A wholly deactivated group was still reported active");

		// The operation writes each Agent's ordinary flag rather than creating
		// inheritance: one member can override it, producing a mixed group. The
		// aggregate remains active while any member is active, so another group
		// deactivation catches that remaining override in one press.
		require(world.setAgentActive(alice, true, &diagnostic),
			"An Agent could not override its group deactivation: " + diagnostic);
		require(world.isAgentGroupActive(crew),
			"A mixed group with one active member was not reported active");
		require(world.setAgentGroupActive(crew, false, &diagnostic),
			"The mixed group could not be deactivated again: " + diagnostic);
		require(!world.lookupAgent(alice).entity->isActive()
			&& !world.lookupAgent(bob).entity->isActive(),
			"Deactivating a mixed group did not deactivate all its members");

		require(world.setAgentGroupActive(crew, true, &diagnostic),
			"The Agent group could not be reactivated: " + diagnostic);
		require(world.lookupAgent(alice).entity->isActive()
			&& world.lookupAgent(bob).entity->isActive(),
			"Group activation did not activate every current member");

		require(!world.canSetAgentGroupActive(core::AgentGroupId{ 999 }, false, &diagnostic),
			"An unknown Agent group was accepted for bulk activation");
		require(!diagnostic.empty(), "An unknown Agent group gave no refusal reason");
	}

	void activationSurvivesSerializationAndReset()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		auto const yaml = serializeWorld(fixture.world);
		require(yaml.find("active") != std::string::npos,
			"A deactivated Agent did not persist its activation at all");

		auto const loaded = loadWorld(yaml);
		auto const* loadedParker = findAgentByName(*loaded, "Parker");
		require(loadedParker != nullptr, "The reloaded World lost the parked Agent");
		require(!loadedParker->isActive(), "A whole-document load lost the deactivation");
		require(findAgentByName(*loaded, "Walker")->isActive(),
			"A whole-document load lost an activation");

		// Reset replays the authored document against the same World: the
		// flag is authored state, so it must come through untouched.
		fixture.world.resetSimulation();
		require(!fixture.world.lookupAgent(fixture.second).entity->isActive(),
			"resetSimulation reactivated a deactivated Agent");
		require(fixture.world.lookupAgent(fixture.first).entity->isActive(),
			"resetSimulation deactivated an activated Agent");

		// A document written before activation existed has no `active` key on
		// its Agents; it has to load with every Agent activated. Only the
		// `active: false` lines come out - a path map carries its own `active`
		// field, which is older than activation and stays.
		auto legacy = yaml;
		for (;;)
		{
			auto const field = legacy.find("active: false");
			if (field == std::string::npos) break;
			auto const lineStart = legacy.rfind('\n', field);
			auto const lineEnd = legacy.find('\n', field);
			legacy.erase(lineStart, lineEnd - lineStart);
		}
		require(legacy.find("active: false") == std::string::npos,
			"The legacy fixture still mentions deactivation");
		auto const loadedLegacy = loadWorld(legacy);
		require(findAgentByName(*loadedLegacy, "Parker")->isActive(),
			"A document without the activation field loaded a deactivated Agent");
		require(findAgentByName(*loadedLegacy, "Walker")->isActive(),
			"A document without the activation field lost an activation");
	}

	void activationSurvivesTopologyEdits()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// Move the destination marker one cell to the left: an atomic replay
		// of the authored records, carrying every Agent back by hand.
		auto const plan = fixture.world.planMoveSectorObject(fixture.corridor, 0, 6, 0);
		require(plan.valid, "Moving the marker was refused: " + plan.diagnostic);
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		(void)fixture.world.applyObjectMove(plan);

		require(!fixture.world.lookupAgent(fixture.second).entity->isActive(),
			"An object-move replay reactivated a deactivated Agent");
		require(fixture.world.lookupAgent(fixture.first).entity->isActive(),
			"An object-move replay deactivated an activated Agent");
	}

	// Ticket #192: a deactivated Agent is not simulated, so a direct interaction
	// request must not admit it in the first place and a request that slipped in
	// before deactivation must not move or press it.
	void inactiveAgentsAreRefusedInteractionRequests()
	{
		core::World world("Inactive interaction", 10, 1);
		auto const corridor = world.addCorridor(0, 0, 10);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		auto const operatorId = world.createAgent("Operator", corridor, 0, 1.0f);
		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const point = world.createInteractionPoint("Switch", sectorId,
			{ 5.0f, 0.0f }, 0.1f, core::World::getFixedTimestep(), { binding });

		std::string diagnostic;
		world.pauseSimulation();
		require(world.setAgentActive(operatorId, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		require(world.resumeSimulation(), "The fixture could not resume");

		// Admission: the direct request is refused outright, and the refusal
		// leaves no request or device operation behind to block the point.
		require(!world.requestInteraction(point, operatorId),
			"A deactivated Agent's direct interaction request was accepted");

		auto const* agent = world.lookupAgent(operatorId).entity;
		auto const parkedAt = agent->getGlobalPosition();
		world.advanceTicks(300);
		require(agent->getGlobalPosition() == parkedAt,
			"A deactivated Agent walked toward an Interaction point");
		require(world.getSector(corridor)->areLightsOn(),
			"A deactivated Agent pressed the control");
		require(world.getSimulationSnapshot().interactionRequests.empty()
			&& world.getSimulationSnapshot().deviceOperations.empty(),
			"A refused interaction left coordination records behind");

		// Reactivation restores ordinary interaction: the Agent walks to the
		// point and the press reaches the control.
		world.pauseSimulation();
		require(world.setAgentActive(operatorId, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		require(world.resumeSimulation(), "The fixture could not resume again");
		auto const request = world.requestInteraction(point, operatorId);
		require(static_cast<bool>(request), "The reactivated Agent's interaction was refused");
		world.advanceTicks(900);
		require(!world.getSector(corridor)->areLightsOn(),
			"The reactivated Agent's press never reached the control");
		require(observedInteractionResults(world, { request }).at(request)
			== core::InteractionResult::Succeeded,
			"The reactivated Agent's interaction did not succeed");
	}

	// Ticket #192: an interaction already queued when its Agent is deactivated
	// is cancelled, not left holding the point for a stopped Agent. The queue
	// behind it drains, and the deactivated Agent never moves.
	void queuedInteractionsAreCancelledByDeactivation()
	{
		core::World world("Queued interaction", 12, 1);
		auto const corridor = world.addCorridor(0, 0, 12);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		auto const first = world.createAgent("First", corridor, 0, 0.5f);
		auto const second = world.createAgent("Second", corridor, 0, 1.5f);
		auto const third = world.createAgent("Third", corridor, 0, 2.5f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const point = world.createInteractionPoint("Switch", sectorId,
			{ 6.0f, 0.0f }, 0.1f, core::World::getFixedTimestep(), { binding });

		// Queue all three behind one point before any tick allocates the first.
		std::string diagnostic;
		world.pauseSimulation();
		auto const firstRequest = world.requestInteraction(point, first);
		auto const secondRequest = world.requestInteraction(point, second);
		auto const thirdRequest = world.requestInteraction(point, third);
		require(firstRequest && secondRequest && thirdRequest,
			"The queued-interaction fixture could not queue its requests");
		require(world.setAgentActive(second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		auto const parkedAt = world.lookupAgent(second).entity->getGlobalPosition();
		require(world.resumeSimulation(), "The fixture could not resume");

		world.advanceTicks(1400);

		auto const results = observedInteractionResults(world,
			{ firstRequest, secondRequest, thirdRequest });
		require(world.lookupAgent(second).entity->getGlobalPosition() == parkedAt,
			"A deactivated queued Agent walked toward the Interaction point");
		require(results.at(firstRequest) == core::InteractionResult::Succeeded,
			"The first Agent's press never succeeded");
		require(results.at(secondRequest) == core::InteractionResult::Cancelled,
			"A deactivated queued request was not cancelled");
		require(results.at(thirdRequest) == core::InteractionResult::Succeeded,
			"The queue did not advance past the deactivated Agent");
		require(!world.getSector(corridor)->areLightsOn(), "The point was never pressed");
	}
}

void agent_smoke::registerActivation(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "everyAgentStartsActivated", [](smoke::Context const&) { everyAgentStartsActivated(); } });
	checks.push_back({ "activationIsRefusedWhileTheSimulationRuns", [](smoke::Context const&) { activationIsRefusedWhileTheSimulationRuns(); } });
	checks.push_back({ "deactivatedAgentsAreNotSimulated", [](smoke::Context const&) { deactivatedAgentsAreNotSimulated(); } });
	checks.push_back({ "reactivationWhilePausedPutsTheAgentBackUnderTheSimulation", [](smoke::Context const&) { reactivationWhilePausedPutsTheAgentBackUnderTheSimulation(); } });
	checks.push_back({ "wakingSkipsDeactivatedAgents", [](smoke::Context const&) { wakingSkipsDeactivatedAgents(); } });
	checks.push_back({ "groupsToggleTheirCurrentMembersEnMasse", [](smoke::Context const&) { groupsToggleTheirCurrentMembersEnMasse(); } });
	checks.push_back({ "activationSurvivesSerializationAndReset", [](smoke::Context const&) { activationSurvivesSerializationAndReset(); } });
	checks.push_back({ "activationSurvivesTopologyEdits", [](smoke::Context const&) { activationSurvivesTopologyEdits(); } });
	checks.push_back({ "inactiveAgentsAreRefusedInteractionRequests", [](smoke::Context const&) { inactiveAgentsAreRefusedInteractionRequests(); } });
	checks.push_back({ "queuedInteractionsAreCancelledByDeactivation", [](smoke::Context const&) { queuedInteractionsAreCancelledByDeactivation(); } });
}
