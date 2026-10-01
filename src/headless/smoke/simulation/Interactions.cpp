#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>
#include "InteractionResults.h"

namespace
{
	constexpr uint64_t MaximumSimulationTicks = 1000;
	using namespace simulation_smoke;

	bool typedLightingInteractionCoalescesAndCancelsByRequester()
	{
		core::World world("Typed lighting interaction", 6, 2);
		auto corridorIndex = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto firstAgent = world.createAgent("First operator", corridorIndex, 0, 0.5f);
		auto secondAgent = world.createAgent("Dependent operator", corridorIndex, 0, 0.7f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = world.createInteractionPoint("Typed light control", sectorId,
			{ 3.5f, 0.5f }, 0.1f, core::World::getFixedTimestep() * 3.0f, { binding });
		auto firstRequest = world.requestInteraction(point, firstAgent);
		auto secondRequest = world.requestInteraction(point, secondAgent);
		if (!firstRequest || !secondRequest)
		{
			return false;
		}

		auto first = world.lookupInteractionRequest(firstRequest);
		auto second = world.lookupInteractionRequest(secondRequest);
		if (!first || !second || first.entity->getOperations().size() != 1
			|| second.entity->getOperations().size() != 1
			|| first.entity->getOperations().front().first != second.entity->getOperations().front().first)
		{
			return false;
		}
		auto operationId = first.entity->getOperations().front().first;

		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto operation = world.lookupDeviceOperation(operationId);
			if (operation && operation.entity->getState() == core::DeviceOperationState::Running)
			{
				break;
			}
		}
		auto firstPosition = world.lookupAgent(firstAgent).entity->getGlobalPosition();
		if (firstPosition.distanceTo({ 3.5f, 0.5f }) > 0.101f || !world.cancelInteraction(firstRequest))
		{
			return false;
		}
		auto operation = world.lookupDeviceOperation(operationId);
		if (!operation || operation.entity->getState() == core::DeviceOperationState::Cancelled
			|| operation.entity->getRequesters().size() != 1
			|| !operation.entity->getRequesters().contains(secondAgent))
		{
			return false;
		}

		world.advanceTicks(3);
		// Both requests reached a terminal outcome and were retired with their
		// shared operation, so the outcomes come from the published events.
		auto const results = observedInteractionResults(world, { firstRequest, secondRequest });
		auto snapshot = world.getSimulationSnapshot();
		return results.at(firstRequest) == core::InteractionResult::Cancelled
			&& results.at(secondRequest) == core::InteractionResult::Succeeded
			&& !world.getSector(corridorIndex)->areLightsOn()
			&& snapshot.deviceOperations.empty()
			&& snapshot.interactionRequests.empty()
			&& snapshot.interactionPoints.front().activeRequest == core::InteractionRequestId{};
	}

	// Ticket #183: a long-running World must keep coordination records bounded by
	// active work. A single Agent hammering one light switch must not leave a
	// growing trail of terminal interaction requests and device operations behind.
	bool repeatedInteractionsRetireTerminalRecords()
	{
		core::World world("Interaction retirement", 6, 2);
		auto corridorIndex = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto agent = world.createAgent("Repeat operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = world.createInteractionPoint("Repeat light control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { binding });

		size_t maximumRequests = 0;
		size_t maximumOperations = 0;
		for (uint32_t i = 0; i < 200; ++i)
		{
			auto request = world.requestInteraction(point, agent);
			if (!request) return false;
			world.advanceTicks(4);
			// Every request must actually reach a terminal outcome, otherwise the
			// bounded registry would only prove that work never completed.
			if (observedInteractionResult(world, request) == core::InteractionResult::Pending)
			{
				return false;
			}
			auto snapshot = world.getSimulationSnapshot();
			maximumRequests = std::max(maximumRequests, snapshot.interactionRequests.size());
			maximumOperations = std::max(maximumOperations, snapshot.deviceOperations.size());
			// At most the request still being observed plus a terminal record awaiting
			// the next tick boundary may remain; historical traffic may not accumulate.
			if (snapshot.interactionRequests.size() > 1 || snapshot.deviceOperations.size() > 1)
			{
				return false;
			}
		}

		world.advanceTicks(4);
		auto final = world.getSimulationSnapshot();
		return final.interactionRequests.empty() && final.deviceOperations.empty()
			&& maximumRequests <= 1 && maximumOperations <= 1;
	}

	bool interactionBindingAggregationIsMeaningful()
	{
		core::World world("Binding aggregation", 3, 2);
		auto corridorIndex = world.addCorridor(0, 0, 2);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto actor = world.createAgent("Binding operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding required{ { core::DeviceCommandType::SetSectorLights, sectorId, false },
			core::InteractionBindingRequirement::Required };
		core::InteractionBinding bestEffort{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::BestEffort };
		auto point = world.createInteractionPoint("Multi-binding control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { required, bestEffort });
		auto requestId = world.requestInteraction(point, actor);
		auto request = world.lookupInteractionRequest(requestId);
		if (!request || request.entity->getOperations().size() != 2)
		{
			return false;
		}
		auto failedBestEffort = request.entity->getOperations()[1].first;
		world.lookupDeviceOperation(failedBestEffort).entity->setState(core::DeviceOperationState::Failed);
		world.advanceTicks(4);
		if (observedInteractionResult(world, requestId)
			!= core::InteractionResult::SucceededWithBestEffortFailure)
		{
			return false;
		}

		core::InteractionBinding failingRequired{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::Required };
		auto requiredPoint = world.createInteractionPoint("Required control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { failingRequired });
		auto failedRequestId = world.requestInteraction(requiredPoint, actor);
		auto failedRequest = world.lookupInteractionRequest(failedRequestId);
		if (!failedRequest)
		{
			return false;
		}
		world.lookupDeviceOperation(failedRequest.entity->getOperations().front().first).entity->setState(
			core::DeviceOperationState::Failed);
		world.advanceTick();
		return observedInteractionResult(world, failedRequestId) == core::InteractionResult::Failed;
	}
}

void registerInteractions(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "typedLightingInteractionCoalescesAndCancelsByRequester", [](smoke::Context const&)
		{
			smoke::require(typedLightingInteractionCoalescesAndCancelsByRequester(), "typed lighting interaction, coalescing, or requester cancellation failed");
		} });
	checks.push_back({ "repeatedInteractionsRetireTerminalRecords", [](smoke::Context const&)
		{
			smoke::require(repeatedInteractionsRetireTerminalRecords(), "repeated interactions accumulated terminal coordination records");
		} });
	checks.push_back({ "interactionBindingAggregationIsMeaningful", [](smoke::Context const&)
		{
			smoke::require(interactionBindingAggregationIsMeaningful(), "required and best-effort interaction aggregation failed");
		} });
}
