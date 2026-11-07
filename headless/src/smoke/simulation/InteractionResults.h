#pragma once
#include "core/World.h"
#include <map>
#include <vector>

namespace simulation_smoke
{
	// Terminal interaction requests are retired once no live owner names them
	// (#183), so a scenario which wants to assert an outcome reads the published
	// event stream instead of looking the hot-registry record up after the fact.
	// One drain collects every requested outcome together.
	inline std::map<core::InteractionRequestId, core::InteractionResult> observedInteractionResults(
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

	inline core::InteractionResult observedInteractionResult(core::World& world,
		core::InteractionRequestId id)
	{
		return observedInteractionResults(world, { id }).at(id);
	}
}
