#include "Checks.h"
#include "BrokenExtensibleFixture.h"
#include "InteractionResults.h"
#include "core/RouteTraversalInputs.h"

namespace
{
	using namespace broken_extensible;
	using smoke::require;
	constexpr Kind kinds[] = { Kind::RoomLadder, Kind::TransitLadder, Kind::Bridge };

	void frozenMotionAndAdmission()
	{
		for (auto kind : kinds)
			for (bool retracting : { false, true })
				for (float fraction : { 0.0f, 0.5f, 1.0f })
				{
					Scene scene(kind, retracting);
					if (retracting) scene.device->retract(); else scene.device->extend();
					scene.device->update(scene.device->getExtendRetractTime() * fraction);
					auto position = scene.device->getExtendedPercentage();
					require(scene.world->setExtensibleBroken(scene.resource, true), "Live break failed");
					scene.device->update(100);
					require(scene.device->getExtendedPercentage() == position && !scene.device->extend()
						&& !scene.device->retract() && !scene.device->toggle(), "Broken extension moved/accepted operation");
					require(bool(scene.path()) == (position >= 1), "Broken passage was not based on physical extension");
					if (position >= 1)
					{
						scene.agent->setPath(scene.path(), true); scene.world->advanceTicks(1800);
						require(scene.agent->getState() == core::Agent::State::Idle
							&& scene.agent->getGlobalPosition().distanceTo(scene.target->getPosition()) < 0.001f,
							"Fully extended Broken traversal did not complete");
					}
					require(scene.world->setExtensibleBroken(scene.resource, false)
						&& scene.device->getExtendedPercentage() == position, "Restoration reset physical extension");
					scene.device->update(100);
					require(retracting ? scene.device->isRetracted() : scene.device->isExtended(), "Restore did not resume frozen motion");
					require(scene.device->extend(), "Restored device refused fresh requests");
				}
	}

	void waitingOperationsAndSafety()
	{
		for (auto kind : kinds)
		{
			Scene waiting(kind, false);
			waiting.agent->setPath(waiting.path(), true);
			for (uint32_t tick = 0; tick < 1000 && !waiting.device->isExtending(); ++tick) waiting.world->advanceTick();
			require(waiting.device->isExtending(), "Extension preparation never began");
			waiting.world->setExtensibleBroken(waiting.resource, true);
			auto position = waiting.device->getExtendedPercentage();
			auto snapshot = waiting.world->getSimulationSnapshot();
			require(std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& value)
				{ return value.state == core::DeviceOperationState::Failed; }), "Pending extension operation did not fail");
			waiting.world->advanceTick();
			require(waiting.agent->getState() == core::Agent::State::RoutePlanning && !waiting.agent->getPath(), "Partial discovery did not mandate planning");
			waiting.world->advanceTicks(120);
			auto resource = waiting.snapshot();
			require(waiting.device->getExtendedPercentage() == position && resource.admissionReservationCount == 0
				&& resource.extensionRequestLeaseCount == 0 && resource.preparationOperator == core::TraversalRequestId{}
				&& std::all_of(resource.queueLanes.begin(), resource.queueLanes.end(), [](auto const& lane) { return lane.queue.empty(); }), "Broken wait left obsolete ownership");
			for (bool desired : { false, true })
			{
				core::InteractionBinding binding;
				binding.command = { core::DeviceCommandType::SetExtendedState, {}, desired, waiting.resource };
				auto point = waiting.world->createInteractionPoint("Broken command", core::SectorId{ waiting.room + 1u },
					waiting.agent->getGlobalPosition(), 1, 0, { binding });
				auto request = waiting.world->requestInteraction(point, waiting.id);
				require(bool(request), "Broken operation was not represented");
				auto op = waiting.world->lookupInteractionRequest(request).entity->getOperations().front().first;
				require(waiting.world->lookupDeviceOperation(op).entity->getState() == core::DeviceOperationState::Failed, "Fresh Broken operation accepted");
				waiting.world->advanceTicks(5);
				require(simulation_smoke::observedInteractionResult(*waiting.world, request) == core::InteractionResult::Failed, "Interaction did not report failed command");
			}

			waiting.world->setExtensibleBroken(waiting.resource, false);
			auto restoredPath = waiting.path(); require(bool(restoredPath), "Restored route unavailable kind=" + std::to_string(static_cast<int>(kind))
				+ " sector=" + std::to_string(waiting.agent->getSector()->getIndex())
				+ " y=" + std::to_string(waiting.agent->getGlobalPosition().y)
				+ " x=" + std::to_string(waiting.agent->getGlobalPosition().x));
			waiting.agent->setPath(restoredPath, true); waiting.world->advanceTicks(1800);
			require(waiting.agent->getState() == core::Agent::State::Idle
				&& waiting.agent->getGlobalPosition().distanceTo(waiting.target->getPosition()) < 0.001f,
				"Fresh restored traversal did not complete: kind=" + std::to_string(static_cast<int>(kind))
				+ " state=" + std::to_string(static_cast<int>(waiting.agent->getState()))
				+ " extension=" + std::to_string(waiting.device->getExtendedPercentage())
				+ " enabled=" + std::to_string(waiting.snapshot().enabled)
				+ " sector=" + std::to_string(waiting.agent->getSector()->getIndex())
				+ " y=" + std::to_string(waiting.agent->getGlobalPosition().y)
				+ " x=" + std::to_string(waiting.agent->getGlobalPosition().x));

			Scene admitted(kind);
			admitted.agent->setPath(admitted.path(), true);
			for (uint32_t tick = 0; tick < 1000; ++tick)
			{
				admitted.world->advanceTick();
				if (admitted.device->getExtensionLeaseCount() > 0
					&& admitted.agent->getState() == core::Agent::State::TraversingEdge) break;
			}
			require(admitted.device->getExtensionLeaseCount() > 0, "No admitted traversal");
			admitted.world->setExtensibleBroken(admitted.resource, true);
			admitted.world->advanceTicks(1800);
			require(admitted.agent->getState() == core::Agent::State::Idle
				&& admitted.agent->getGlobalPosition().distanceTo(admitted.target->getPosition()) < 0.001f
				&& admitted.device->getExtensionLeaseCount() == 0
				&& admitted.snapshot().occupantCount == 0 && admitted.snapshot().admissionReservationCount == 0,
				"Break interrupted admitted traversal or leaked ownership");
		}
	}

	void individualLocalMemory()
	{
		for (auto kind : kinds)
		{
			Scene scene(kind, false);
			scene.world->removeAgent(scene.id);
			scene.id = scene.world->createAgent("Remote observer", scene.remote, kind == Kind::Bridge ? 1 : 0, 0.5f);
			scene.agent = scene.world->lookupAgent(scene.id).entity;
			scene.world->setExtensibleBroken(scene.resource, true);
			require(scene.path() && !scene.agent->rememberedDeviceCondition(scene.resource), "Path search revealed unknown remote breakage");
			scene.place(scene.room); scene.world->advanceTick();
			auto memory = scene.agent->rememberedDeviceCondition(scene.resource);
			require(memory && memory->broken && memory->position < 1, "Idle/passing observer missed local condition");
			scene.place(scene.remote); scene.world->setExtensibleBroken(scene.resource, false); scene.world->advanceTicks(120);
			require(!scene.path() && scene.agent->rememberedDeviceCondition(scene.resource) == memory, "Remote repair updated stale memory");
			auto stranger = scene.world->createAgent("Uninformed", scene.remote, kind == Kind::Bridge ? 1 : 0, 0.5f);
			require(bool(scene.world->getGraph()->calculatePath(scene.world->lookupAgent(stranger).entity, scene.target)), "Knowledge leaked between Agents");
			scene.place(scene.room); scene.world->advanceTick();
			require(!scene.agent->rememberedDeviceCondition(scene.resource)->broken && scene.path(), "Local repair failed to replace knowledge");
			scene.device->extend(); scene.device->update(100); scene.world->setExtensibleBroken(scene.resource, true); scene.world->advanceTick();
			memory = scene.agent->rememberedDeviceCondition(scene.resource);
			require(memory && memory->position == 1 && scene.path(), "Broken extended condition not remembered as usable");
			scene.place(scene.remote); scene.world->setExtensibleBroken(scene.resource, false);
			scene.device->retract(); scene.device->update(100); scene.world->setExtensibleBroken(scene.resource, true); scene.world->advanceTicks(120);
			require(scene.path() && scene.agent->rememberedDeviceCondition(scene.resource) == memory, "Remembered extended passage consulted remote physical state");
			for (auto const& edge : scene.world->getGraph()->getEdges())
				if (edge->getTraversalResourceId() == scene.resource)
					for (uint32_t side = 0; side < 2; ++side)
					{
						core::RouteDecisionContext context{ scene.agent, {}, scene.world->getRouteChoicePolicy(), scene.agent->getSector(), scene.agent->getWalkSpeed(), scene.world.get() };
						auto eager = edge->getDirectedTraversalFacts(edge->getVertex(side), context);
						auto captured = core::RouteTraversalInputs::capture(*edge, edge->getVertex(side), context).evaluate(context);
						require(eager.feasible && captured.feasible && eager.components.expectedWaitSeconds == 0
							&& captured.components.expectedWaitSeconds == 0, "Memory capture/oracle disagreed on frozen extension");
					}
			scene.place(scene.room); scene.world->advanceTick();
			require(!scene.path(), "Fresh partial observation did not replace usable stale memory");
			scene.world->setExtensibleBroken(scene.resource, false); scene.world->advanceTick();
			scene.place(scene.remote); memory = scene.agent->rememberedDeviceCondition(scene.resource);
			scene.world->setExtensibleBroken(scene.resource, true); scene.world->advanceTick();
			require(scene.path() && scene.agent->rememberedDeviceCondition(scene.resource) == memory && !memory->broken, "Remote failure notified healthy memory");
		}
	}

	void planningAndPersistence()
	{
		for (auto kind : kinds)
			for (bool alternatives : { false, true })
			{
				if (kind == Kind::Bridge && alternatives) continue;
				Scene scene(kind, true, true, false, alternatives);
				require(uses(scene.path(), scene.resource), "Fixture did not choose direct device");
				scene.agent->setPath(scene.path(), true);
				scene.device->retract(); scene.device->update(scene.device->getExtendRetractTime() * 0.5f);
				scene.world->setExtensibleBroken(scene.resource, true); scene.world->advanceTick();
				require(scene.agent->getState() == core::Agent::State::RoutePlanning && !scene.agent->getPath(), "Unusable Path did not invalidate");
				scene.world->advanceTicks(5);
				if (alternatives)
				{
					require(uses(scene.agent->getPath(), scene.alternative), "Mandatory planning failed to find alternative");
					scene.world->setExtensibleBroken(scene.resource, false); scene.world->advanceTick();
					require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Restoration did not trigger voluntary planning");
					scene.world->advanceTicks(6);
					require(uses(scene.agent->getPath(), scene.alternative), "Voluntary repair bypassed Route persistence");
				}
				else
				{
					auto events = scene.world->consumeSimulationEvents();
					require(std::count_if(events.begin(), events.end(), [](auto const& event)
						{ return event.type == core::SimulationEventType::RouteLost; }) == 1, "No alternative did not produce one Route loss");
				}
			}
	}
	void safeRetractionAndPermission()
	{
		for (auto kind : kinds)
		{
			Scene scene(kind);
			scene.agent->setPath(scene.path(), true);
			for (uint32_t tick = 0; tick < 1000 && scene.device->getExtensionLeaseCount() == 0; ++tick) scene.world->advanceTick();
			require(scene.device->getExtensionLeaseCount() > 0, "Safe-retract fixture has no admission");
			auto operatorId = scene.world->createAgent("Operator", scene.room, kind == Kind::Bridge ? 1 : 0, 0.5f);
			core::InteractionBinding binding;
			binding.command = { core::DeviceCommandType::SetExtendedState, {}, false, scene.resource };
			auto point = scene.world->createInteractionPoint("Safe retract", core::SectorId{ scene.room + 1u },
				scene.world->lookupAgent(operatorId).entity->getGlobalPosition(), 1, 0, { binding });
			auto interaction = scene.world->requestInteraction(point, operatorId);
			auto operation = scene.world->lookupInteractionRequest(interaction).entity->getOperations().front().first;
			for (uint32_t tick = 0; tick < 10 && !scene.snapshot().retractionPending; ++tick) scene.world->advanceTick();
			require(scene.snapshot().retractionPending && scene.device->getExtensionLeaseCount() > 0
				&& scene.device->getExtendedPercentage() == 1, "Retraction failed to wait for admitted traversal");
			scene.world->setExtensibleBroken(scene.resource, true);
			require(scene.world->lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Failed, "Pending safe retract did not fail on breakage");
			scene.world->advanceTicks(1800);
			require(scene.agent->getGlobalPosition().distanceTo(scene.target->getPosition()) < 0.001f
				&& scene.device->getExtensionLeaseCount() == 0 && scene.device->getExtendedPercentage() == 1,
				"Frozen safe retract disrupted traversal or retracted while Broken");
			scene.world->setExtensibleBroken(scene.resource, false); scene.world->advanceTicks(1800);
			require(scene.device->isRetracted() && !scene.snapshot().retractionPending, "Restore did not resume lease-safe pending retraction: kind=" + std::to_string(static_cast<int>(kind))
				+ " position=" + std::to_string(scene.device->getExtendedPercentage())
				+ " pending=" + std::to_string(scene.snapshot().retractionPending)
				+ " leases=" + std::to_string(scene.device->getExtensionLeaseCount()));

			Scene protectedScene(kind);
			protectedScene.world->pauseSimulation();
			auto permission = protectedScene.world->addAccessPermission("Operate");
			for (auto control : protectedScene.snapshot().controls)
				require(protectedScene.world->setInteractionPointPermissionRequirement(control, { permission }), "Control requirement edit failed");
			protectedScene.world->setExtensibleBroken(protectedScene.resource, true);
			require(!protectedScene.path(), "Broken extended device bypassed Permission adherence");
			protectedScene.world->setAgentIndividualPermissionAdherence(protectedScene.id, false);
			require(bool(protectedScene.path()), "Non-adhering Agent could not use fully extended Broken device");
			protectedScene.world->resumeSimulation(); protectedScene.world->advanceTick();
			protectedScene.place(protectedScene.remote);
			protectedScene.world->setExtensibleBroken(protectedScene.resource, false);
			protectedScene.device->retract(); protectedScene.device->update(100);
			protectedScene.world->setExtensibleBroken(protectedScene.resource, true);
			require(bool(protectedScene.path()), "Remote partial position invalidated extended memory");
			protectedScene.world->pauseSimulation();
			require(protectedScene.world->setAgentIndividualPermissionAdherence(protectedScene.id, true), "Adherence edit failed");
			require(!protectedScene.path(), "Remote physical state bypassed remembered passage requirements");
		}

		Scene bridge(Kind::Bridge);
		auto opposite = bridge.world->createAgent("Opposite", bridge.room, 1, 4.5f);
		auto other = bridge.world->lookupAgent(opposite).entity;
		other->setPath(bridge.world->getGraph()->calculatePath(other, bridge.placements.at(bridge.room)), true);
		bridge.agent->setPath(bridge.path(), true);
		for (uint32_t tick = 0; tick < 600 && bridge.device->getExtensionLeaseCount() < 2; ++tick) bridge.world->advanceTick();
		require(bridge.device->getExtensionLeaseCount() >= 2, "Bridge did not admit concurrent two-way crossing");
		bridge.world->setExtensibleBroken(bridge.resource, true); bridge.world->advanceTicks(1800);
		require(bridge.agent->getState() == core::Agent::State::Idle && other->getState() == core::Agent::State::Idle
			&& bridge.device->getExtensionLeaseCount() == 0, "Broken bridge interrupted concurrent accepted crossings");
	}
}

void registerBrokenExtensibles(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "brokenExtensibles/frozenMotionAndAdmission", [](smoke::Context const&) { frozenMotionAndAdmission(); } });
	checks.push_back({ "brokenExtensibles/waitingOperationsAndSafety", [](smoke::Context const&) { waitingOperationsAndSafety(); } });
	checks.push_back({ "brokenExtensibles/individualLocalMemory", [](smoke::Context const&) { individualLocalMemory(); } });
	checks.push_back({ "brokenExtensibles/planningAndPersistence", [](smoke::Context const&) { planningAndPersistence(); } });
	checks.push_back({ "brokenExtensibles/safeRetractionAndPermission", [](smoke::Context const&) { safeRetractionAndPermission(); } });
}
