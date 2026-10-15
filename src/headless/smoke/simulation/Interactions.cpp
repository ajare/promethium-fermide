#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/SimulationCoordinator.h"
#include <algorithm>
#include <vector>
#include "InteractionResults.h"

namespace
{
	constexpr uint64_t MaximumSimulationTicks = 1000;
	using namespace simulation_smoke;

	// Characterize public requests and the coordinator's internal passing seam (#620).
	void requestEntryPointsPreserveTrace()
	{
		using namespace core;
		using smoke::require;
		World world("Request entry points", 6, 2);
		SimulationCoordinator coordinator(world);
		auto corridor = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sector = SectorId{ corridor + 1u };
		auto actor = world.createAgent("Operator", corridor, 0, 0.5f);
		InteractionBinding binding{ { DeviceCommandType::SetSectorLights, sector, false },
			InteractionBindingRequirement::BestEffort };
		auto point = world.createInteractionPoint("Timed control", sector,
			{ 0.5f, 0.0f }, 1.0f, World::getFixedTimestep() * 5.0f, { binding });
		auto other = world.createInteractionPoint("Other control", sector,
			{ 0.5f, 0.0f }, 1.0f, 0.0f, { binding });
		world.consumeSimulationEvents();
		auto queued = world.requestInteraction(point, actor);
		require(bool(queued), "Normal request was refused");
		auto operation = world.lookupInteractionRequest(queued).entity->getOperations().front();
		require(operation.second == InteractionBindingRequirement::BestEffort, "Binding requirement changed");
		require(world.requestInteraction(point, actor) == queued, "Same-point pending request identity changed");
		require(!world.requestInteraction(other, actor)
			&& !coordinator.requestInteractionWhilePassing(point, actor)
			&& !coordinator.requestInteractionWhilePassing(other, actor), "Pending physical work was not refused");
		auto events = world.consumeSimulationEvents();
		require(events.size() == 2 && events[0].type == SimulationEventType::DeviceOperationAdded
			&& events[1].type == SimulationEventType::InteractionRequestAdded
			&& events[0].deviceOperation.id == operation.first
			&& events[1].interactionRequest.id == queued
			&& events[0].tick == 0 && events[1].tick == 0
			&& events[1].sequence == events[0].sequence + 1
			&& events[0].phase == events[1].phase, "Request construction trace changed");
		world.advanceTick();
		require(world.getSector(corridor)->areLightsOn()
			&& world.lookupDeviceOperation(operation.first).entity->getState() == DeviceOperationState::Pending,
			"Queued request bypassed its press duration");
		require(world.cancelInteraction(queued), "Queued cancellation failed");
		world.consumeSimulationEvents();
		auto passing = coordinator.requestInteractionWhilePassing(point, actor);
		require(bool(passing) && passing != queued, "Passing request did not receive a fresh identity");
		auto passingOperation = world.lookupInteractionRequest(passing).entity->getOperations().front();
		require(passingOperation.first != operation.first && passingOperation.second == operation.second,
			"Cancelled operation was reused or binding requirement changed");
		events = world.consumeSimulationEvents();
		require(events.size() == 2 && events[0].type == SimulationEventType::DeviceOperationAdded
			&& events[1].type == SimulationEventType::InteractionRequestAdded
			&& events[1].interactionRequest.id == passing && events[0].tick == 1 && events[1].tick == 1
			&& events[1].sequence == events[0].sequence + 1
			&& events[0].phase == events[1].phase, "Passing construction trace changed");
		world.advanceTick();
		require(world.lookupDeviceOperation(passingOperation.first).entity->getState() == DeviceOperationState::Running,
			"Passing activation waited for the queued press duration");
		world.advanceTick();
		require(!world.getSector(corridor)->areLightsOn(), "Passing operation did not complete on the next tick");
		require(world.lookupAgent(actor).entity->getGlobalPosition().distanceTo({ 0.5f, 0.0f }) < 0.001f,
			"Request construction moved the Agent");

		world.pauseSimulation();
		auto permission = world.addAccessPermission("Control key");
		require(world.setInteractionPointPermissionRequirement(other, { permission }), "Control requirement refused");
		require(world.resumeSimulation(), "Request fixture did not resume");
		world.consumeSimulationEvents();
		auto rejected = world.requestInteraction(other, actor);
		auto rejectedPassing = coordinator.requestInteractionWhilePassing(other, actor);
		require(rejected && rejectedPassing && rejected != rejectedPassing,
			"Permission refusal must record distinct rejected requests");
		events = world.consumeSimulationEvents();
		require(events.size() == 2 && events[1].sequence == events[0].sequence + 1,
			"Refusal published operations or changed event order");
		for (size_t i = 0; i < events.size(); ++i)
		{
			auto id = i == 0 ? rejected : rejectedPassing;
			auto request = world.lookupInteractionRequest(id);
			require(request && request.entity->getResult() == InteractionResult::Rejected
				&& request.entity->getMissingPermissions() == std::vector<AccessPermissionId>{ permission }
				&& request.entity->getOperations().empty(), "Refusal diagnostics or operations changed");
			require(events[i].type == SimulationEventType::InteractionRequestAdded
				&& events[i].tick == 3 && events[i].phase == events[0].phase
				&& events[i].interactionRequest.id == id && events[i].interactionRequest.point == other
				&& events[i].interactionRequest.actor == actor
				&& events[i].interactionRequest.result == InteractionResult::Rejected
				&& events[i].interactionRequest.missingPermissions == request.entity->getMissingPermissions()
				&& events[i].interactionRequest.operations.empty(), "Refusal event trace changed");
		}
	}

	void terminalCleanupPreservesOwnershipAndTrace()
	{
		using namespace core;
		using smoke::require;
		for (bool refuse : { false, true })
		for (bool queued : { false, true })
		{
			World world("Terminal cleanup", 6, 2);
			auto corridor = world.addCorridor(0, 0, 5);
			world.finishBuild();
			auto sector = SectorId{ corridor + 1u };
			auto victim = world.createAgent("Cancelled or refused", corridor, 0, 0.5f);
			auto survivor = world.createAgent("Surviving requester", corridor, 0, 0.5f);
			auto blocker = world.createAgent("Queue owner", corridor, 0, 0.5f);
			world.pauseSimulation();
			auto permission = world.addAccessPermission("Control key");
			require(world.grantAgentAccessPermission(survivor, permission), "Survivor grant failed");
			require(world.resumeSimulation(), "Terminal setup resume failed");
			InteractionBinding binding{ { DeviceCommandType::SetSectorLights, sector, false },
				InteractionBindingRequirement::Required };
			auto point = world.createInteractionPoint("Shared timed control", sector,
				{ 0.5f, 0.0f }, 1.0f, World::getFixedTimestep() * 5.0f, { binding });
			auto blockerRequest = queued ? world.requestInteraction(point, blocker) : InteractionRequestId{};
			auto request = world.requestInteraction(point, victim);
			auto survivingRequest = world.requestInteraction(point, survivor);
			require(request && survivingRequest, "Terminal fixture requests refused");
			auto operation = world.lookupInteractionRequest(request).entity->getOperations().front().first;
			require(world.lookupInteractionRequest(survivingRequest).entity->getOperations().front().first == operation,
				"Terminal fixture did not coalesce");
			world.advanceTick();
			auto duration = world.getSimulationSnapshot().interactionPoints.front().durationTicks;
			require(world.getSimulationSnapshot().interactionPoints.front().activeRequest
				== (queued ? blockerRequest : request), "Unexpected initial active owner");
			world.consumeSimulationEvents();
			uint64_t terminalTick = 1;
			if (refuse)
			{
				world.pauseSimulation();
				require(world.setInteractionPointPermissionRequirement(point, { permission }), "Requirement edit failed");
				require(world.resumeSimulation(), "Terminal fixture resume failed");
				if (queued)
				{
					// The queued request is checked only when it becomes active. Do not
					// introduce an earlier authorization boundary during the refactor.
					require(world.lookupInteractionRequest(request).entity->getResult() == InteractionResult::Pending,
						"Queued authorization was checked early");
					require(world.cancelInteraction(blockerRequest), "Blocker cancellation failed");
				}
				world.advanceTick();
				terminalTick = 2;
			}
			else require(world.cancelInteraction(request), "Terminal cancellation failed");
			auto expected = refuse ? InteractionResult::Rejected : InteractionResult::Cancelled;
			auto terminal = world.lookupInteractionRequest(request);
			require(terminal && terminal.entity->getResult() == expected
				&& terminal.entity->getMissingPermissions() == (refuse
					? std::vector<AccessPermissionId>{ permission } : std::vector<AccessPermissionId>{}),
				"Terminal record was deleted or lost diagnostics");
			require(!world.cancelInteraction(request), "Terminal request was cancelled twice");
			auto op = world.lookupDeviceOperation(operation);
			require(op && op.entity->getState() == DeviceOperationState::Pending
				&& op.entity->getRequesters().contains(survivor)
				&& !op.entity->getRequesters().contains(victim), "Detachment cancelled shared work");
			auto active = world.getSimulationSnapshot().interactionPoints.front().activeRequest;
			require(active == (!refuse && queued ? blockerRequest : InteractionRequestId{}),
				"Cleanup released another request's active slot");
			if (!refuse && queued)
			{
				// Cancelling a queued requester must not reset the active owner's
				// remaining duration. Its first press tick has already elapsed.
				world.advanceTicks(duration - 1);
				require(!world.getSimulationSnapshot().interactionPoints.front().activeRequest,
					"Queued cancellation reset the active press timer");
				world.advanceTick();
				require(world.lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Running,
					"Completed press did not activate shared work");
			}
			world.advanceTicks(duration + 4);
			auto events = world.consumeSimulationEvents();
			unsigned terminalCount = 0, successCount = 0, removalCount = 0;
			uint64_t terminalSequence = 0;
			for (auto const& event : events)
			{
				if (event.type == SimulationEventType::InteractionRequestChanged && event.interactionRequest.id == request)
				{
					++terminalCount;
					terminalSequence = event.sequence;
					require(event.tick == terminalTick && event.interactionRequest.result == expected
						&& event.interactionName == "Shared timed control", "Terminal publication trace changed");
				}
				if (event.type == SimulationEventType::InteractionRequestChanged && event.interactionRequest.id == survivingRequest)
				{
					++successCount;
					require(event.interactionRequest.result == InteractionResult::Succeeded, "Shared survivor failed");
				}
				if (event.type == SimulationEventType::InteractionRequestRemoved && event.interactionRequest.id == request)
				{
					++removalCount;
					require(terminalSequence && event.sequence > terminalSequence && event.tick > terminalTick,
						"Terminal request retired before its observation boundary");
				}
			}
			require(terminalCount == 1 && successCount == 1 && removalCount == 1,
				"Terminal outcome or retirement not published exactly once");
			require(!world.getSector(corridor)->areLightsOn()
				&& world.getSimulationSnapshot().interactionRequests.empty()
				&& world.getSimulationSnapshot().deviceOperations.empty(), "Terminal cleanup leaked work");
			for (auto actor : { victim, survivor, blocker })
				require(world.lookupAgent(actor).entity->getGlobalPosition().distanceTo({ 0.5f, 0.0f }) < 0.001f,
					"Terminal cleanup changed Agent position");
		}
	}

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
			requestEntryPointsPreserveTrace();
			terminalCleanupPreservesOwnershipAndTrace();
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
