#include "Checks.h"
// Interaction points and the effective Mobility profile, for ticket #193.
//
// Buttons is a capability rather than a traversal kind (ADR 0011): an Agent
// whose effective profile forbids it cannot operate an Interaction point,
// whether that profile is authored on the Agent or supplied by an Agent tag.
// Individual profiles override tag profiles (ADR 0012), so the interaction
// gate has to read the effective profile rather than a single source.
//
// Everything here drives the public World API and a real tick pipeline. What
// gets pinned down:
//
//   an Agent inheriting a Buttons restriction from its tag is refused a
//   direct interaction request, with no movement, no press, and no
//   coordination records left behind
//   an individual profile without Buttons overrides the inherited
//   restriction and is admitted
//   removing the individual override reveals the inherited restriction
//   again, and clearing the tag restriction lets the same interaction
//   succeed
//   a request already queued when a paused profile edit forbids Buttons is
//   cancelled rather than left holding the point, so the queue behind it
//   drains

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/MobilityProfile.h"
#include "core/World.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	core::MobilityProfile cannotUse(core::TraversalKind kind)
	{
		core::MobilityProfile profile;
		profile.set(kind, core::MobilityUse::CannotUse);
		return profile;
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

	// The inherited restriction refuses the request; an individual profile
	// without Buttons admits it; removing that override reveals the inherited
	// restriction again; clearing the tag restriction lets it succeed.
	void interactionRequestsHonourEffectiveButtonsRestriction()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("restricted");
		std::string diagnostic;
		require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagMobilityProfile(tag,
			cannotUse(core::TraversalKind::Buttons), &diagnostic), diagnostic);

		core::World world("Interaction mobility", 8, 1);
		world.attachAgentTagRegistry("interaction-mobility.tags.yaml", registry);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		auto const agentId = world.createAgent("Restricted", corridor, 0, 0.5f);
		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const point = world.createInteractionPoint("Switch", sectorId,
			{ 4.0f, 0.0f }, 0.1f, core::World::getFixedTimestep(), { binding });

		world.pauseSimulation();
		require(world.assignAgentTag(agentId, tag, &diagnostic), diagnostic);
		require(core::agentForbidsButtons(world.lookupAgent(agentId).entity),
			"The fixture did not inherit its Buttons restriction");
		require(world.resumeSimulation(), "The restriction fixture did not resume");

		// Inherited restriction: the request is refused before it can claim a
		// place in the point's queue.
		require(!world.requestInteraction(point, agentId),
			"A tag-forbidden Agent's interaction request was accepted");
		auto const* agent = world.lookupAgent(agentId).entity;
		world.advanceTicks(600);
		require(agent->getGlobalPosition().x == 0.5f,
			"A tag-forbidden Agent walked toward the control");
		require(world.getSector(corridor)->areLightsOn(),
			"A tag-forbidden Agent pressed the control");
		require(world.getSimulationSnapshot().interactionRequests.empty()
			&& world.getSimulationSnapshot().deviceOperations.empty(),
			"A refused interaction leaked coordination records");

		// Individual profile without Buttons overrides the inherited one.
		world.pauseSimulation();
		require(world.setAgentIndividualMobilityProfile(agentId,
			cannotUse(core::TraversalKind::Lift), &diagnostic), diagnostic);
		require(!core::agentForbidsButtons(agent),
			"The individual profile did not override the inherited Buttons restriction");
		require(world.resumeSimulation(), "The override fixture did not resume");
		auto const overridden = world.requestInteraction(point, agentId);
		require(static_cast<bool>(overridden),
			"The individual override did not admit the interaction request");
		require(world.cancelInteraction(overridden),
			"The admitted override request could not be cancelled");

		// Removing the override reveals the inherited restriction again.
		world.pauseSimulation();
		require(world.setAgentIndividualMobilityProfile(agentId, std::nullopt, &diagnostic), diagnostic);
		require(core::agentForbidsButtons(agent),
			"Removing the override did not reveal the inherited Buttons restriction");
		require(world.resumeSimulation(), "The revealed fixture did not resume");
		require(!world.requestInteraction(point, agentId),
			"The revealed inherited restriction did not refuse the request");
		require(world.getSector(corridor)->areLightsOn(),
			"A refused request changed the device");

		// Clearing the tag restriction lets the same interaction succeed.
		world.pauseSimulation();
		require(registry->setAgentTagMobilityProfile(tag, {}, &diagnostic), diagnostic);
		require(!core::agentForbidsButtons(agent), "Clearing the tag profile left Buttons forbidden");
		require(world.resumeSimulation(), "The cleared fixture did not resume");
		auto const allowed = world.requestInteraction(point, agentId);
		require(static_cast<bool>(allowed), "The cleared restriction did not admit the request");
		world.advanceTicks(900);
		require(!world.getSector(corridor)->areLightsOn(),
			"The Agent with no effective restriction never pressed the control");
		require(observedInteractionResults(world, { allowed }).at(allowed)
			== core::InteractionResult::Succeeded,
			"The cleared restriction's interaction did not succeed");
	}

	// A request already queued when a paused edit forbids Buttons is cancelled,
	// so a stopped Agent does not hold the point or block the queue.
	void queuedInteractionIsCancelledByPausedProfileEdit()
	{
		core::World world("Queued profile edit", 12, 1);
		auto const corridor = world.addCorridor(0, 0, 12);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		auto const first = world.createAgent("First", corridor, 0, 0.5f);
		auto const second = world.createAgent("Second", corridor, 0, 1.5f);
		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const point = world.createInteractionPoint("Switch", sectorId,
			{ 6.0f, 0.0f }, 0.1f, core::World::getFixedTimestep(), { binding });

		std::string diagnostic;
		world.pauseSimulation();
		auto const firstRequest = world.requestInteraction(point, first);
		auto const secondRequest = world.requestInteraction(point, second);
		require(firstRequest && secondRequest, "The queued fixture could not queue its requests");
		require(world.setAgentIndividualMobilityProfile(second,
			cannotUse(core::TraversalKind::Buttons), &diagnostic), diagnostic);
		auto const parkedAt = world.lookupAgent(second).entity->getGlobalPosition();
		require(world.resumeSimulation(), "The queued fixture did not resume");

		world.advanceTicks(1400);

		auto const results = observedInteractionResults(world, { firstRequest, secondRequest });
		require(world.lookupAgent(second).entity->getGlobalPosition() == parkedAt,
			"A profile-forbidden queued Agent walked toward the control");
		require(results.at(firstRequest) == core::InteractionResult::Succeeded,
			"The unrestricted Agent's press never succeeded");
		require(results.at(secondRequest) == core::InteractionResult::Cancelled,
			"A profile-forbidden queued request was not cancelled");
		require(!world.getSector(corridor)->areLightsOn(), "The point was never pressed");
	}
}

void permission_smoke::registerInteractionMobility(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "interactionRequestsHonourEffectiveButtonsRestriction",
		[](smoke::Context const&)
		{
			interactionRequestsHonourEffectiveButtonsRestriction();
		} });
	checks.push_back({ "queuedInteractionIsCancelledByPausedProfileEdit",
		[](smoke::Context const&)
		{
			queuedInteractionIsCancelledByPausedProfileEdit();
		} });
}
