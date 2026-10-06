#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "AgentClipboard.h"
#include "AgentBehaviourAssignmentPanel.h"
#include "PermissionsPanel.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Planning", 12, 2);
		core::AgentId id;
		core::MarkerId destination;
		Fixture(uint64_t seed = 0, bool extraAgent = false)
		{
			auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
			world->addSectorMarker(room, 0, 8.5f, "Destination");
			world->finishBuild();
			if (extraAgent) world->createAgent("Other", room, 0, 1.0f);
			id = world->createAgent("Planner", room, 0, 2.375f);
			destination = world->getMarkerIds().front();
			if (seed)
			{
				world->pauseSimulation();
				require(world->setRandomSeed(seed), "Seed refused");
				require(world->resumeSimulation(), "Seed fixture resume refused");
			}
		}
		core::Agent* agent() { return world->lookupAgent(id).entity; }
		void fixedDuration()
		{
			world->pauseSimulation();
			require(world->setAgentIndividualMinimumRoutePlanningTime(id, 0.101f), "Minimum refused");
			require(world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Maximum refused");
			require(world->resumeSimulation(), "Resume failed");
		}
	};

	std::string document(core::World& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string clipboard(core::World& world, core::AgentId id)
	{
		return makeAgentClipboardText(makeAgentClipboardPayload(world, id, "Copy"), false);
	}

	void boundariesAndPresentation()
	{
		Fixture f;
		f.fixedDuration();
		auto const position = f.agent()->getGlobalPosition();
		auto const decisions = f.world->getGraph()->getRouteWorkCounts().decisions;
		require(f.world->moveAgentToMarker(f.id, f.destination).accepted(), "Command refused");
		auto const duration = core::secondsToTicks(0.101f, f.world->getFixedTimestep());
		require(duration == 7 && f.agent()->getRoutePlanningTotalTicks() == duration,
			"Planning did not round normalized endpoints upward");
		for (uint64_t tick = 0; tick < duration; ++tick)
		{
			auto const snapshot = f.world->getSimulationSnapshot().agents.front();
			require(snapshot.state == core::AgentPathState::RoutePlanning && !snapshot.hasPath
				&& !snapshot.hasLocomotionTask && snapshot.intendedDestination == f.destination
				&& snapshot.routePlanningTotalTicks == duration
				&& snapshot.routePlanningRemainingTicks == duration - tick
				&& snapshot.globalPosition == position, "Planning snapshot or stationary timer incorrect");
			require(f.world->getGraph()->getRouteWorkCounts().decisions == decisions,
				"Path calculated before expiry");
			if (tick == 2)
			{
				f.world->pauseSimulation();
				require(!f.world->advanceTick(), "Pause advanced planning");
				require(f.world->setAgentActive(f.id, false), "Deactivation refused");
				require(f.world->resumeSimulation(), "Resume refused");
				f.world->advanceTicks(10);
				require(f.agent()->getRoutePlanningRemainingTicks() == duration - tick
					&& f.agent()->getGlobalPosition() == position, "Deactivation changed planning episode");
				f.world->pauseSimulation();
				require(f.world->setAgentActive(f.id, true), "Reactivation refused");
				require(f.world->resumeSimulation(), "Resume refused");
			}
			if (tick == 3)
			{
				headless::ScopedImGuiContext context;
				auto& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.DisplaySize = { 800, 600 };
				io.Fonts->AddFontDefault(); io.Fonts->Build();
				ImGui::NewFrame(); ImGui::Begin("Planning panel");
				bool choosingAction = true;
				require(!renderAgentMovementActionPopup({}, {}, true, choosingAction) && !choosingAction,
					"Missing destination opened an Action picker");
				ImGui::LogToBuffer();
				renderAgentRuntimeProperties(f.world, f.id);
				std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				require(text.find("Route planning") != std::string::npos
					&& text.find("Destination: Destination") != std::string::npos
					&& text.find("seconds total") != std::string::npos
					&& text.find("seconds remaining") != std::string::npos,
					"Selection panel omitted planning intent or timing");
				ImGui::LogFinish(); ImGui::End(); ImGui::Render();
			}
			f.world->advanceTick();
		}
		require(f.agent()->getGlobalPosition() == position && f.agent()->getPath()
			&& f.agent()->getState() == core::Agent::State::MovingToVertex
			&& f.world->getGraph()->getRouteWorkCounts().decisions == decisions + 1,
			"Expiry must calculate once, change state, and not move");
		f.world->advanceTick();
		require(f.agent()->getGlobalPosition() != position, "Movement did not start on following tick");
	}

	void inclusiveEndpointsAndPersistence()
	{
		Fixture f;
		f.world->pauseSimulation();
		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 0.1f)
			&& f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 0.11f), "Interval refused");
		f.world->resumeSimulation();
		auto serialize = [&]()
		{
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			f.world->serialize(*writer, work); writer->serialize();
			return writer->getSerializedString();
		};
		auto const baseline = serialize();
		auto const baselineClipboard = clipboard(*f.world, f.id);
		std::set<uint64_t> sampled;
		for (unsigned i = 0; i < 64; ++i)
		{
			f.world->moveAgentToMarker(f.id, f.destination);
			sampled.insert(f.agent()->getRoutePlanningTotalTicks());
			f.world->advanceTick();
			require(serialize() == baseline, "Planning intent, timer or stream entered persistence");
			require(clipboard(*f.world, f.id) == baselineClipboard,
				"Planning intent, timer or stream entered clipboard");
			f.world->cancelAgentMovement(f.id); f.world->advanceTick();
			f.world->consumeSimulationEvents();
		}
		require(sampled == std::set<uint64_t>{ 6, 7 }, "Sampling excluded an inclusive endpoint");
	}

	void voluntaryAuthorizationPlanning(float persistence, bool invalidate, bool fail,
		bool withdrawShortcut = false)
	{
		core::World world("Voluntary authorization", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto shortcut = world.addSectorDoor(front, 0, 3, {});
		auto original = world.addSectorDoor(front, 0, 9, {});
		world.addSectorMarker(back, 0, 2.5f, "Destination");
		world.finishBuild();
		world.pauseSimulation();
		auto id = world.createAgent("Planner", front, 0, 2.5f);
		auto agent = world.lookupAgent(id).entity;
		auto key = world.addAccessPermission("Shortcut");
		auto oldKey = world.addAccessPermission("Original");
		require(world.setManualDoorPermissionRequirement(shortcut.traversalResource, { key }), "Requirement refused");
		require(world.setManualDoorPermissionRequirement(original.traversalResource, { oldKey }), "Requirement refused");
		require(world.grantAgentAccessPermission(id, oldKey), "Initial grant refused");
		require(world.setAgentIndividualRoutePersistence(id, persistence), "Persistence refused");
		require(world.setAgentIndividualMinimumRoutePlanningTime(id, 0.1f), "Minimum refused");
		require(world.setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Maximum refused");
		require(world.resumeSimulation(), "Resume failed");
		world.moveAgentToMarker(id, world.getMarkerIds().front());
		world.advanceTicks(agent->getRoutePlanningRemainingTicks());
		auto retained = agent->getPath();
		require(bool(retained), "Initial Path missing");
		auto position = agent->getGlobalPosition();
		auto decisions = world.getGraph()->getRouteWorkCounts().decisions;
		// Runtime Path retention must not change the authored document or clipboard.
		auto const authored = document(world);
		auto const copied = clipboard(world, id);
		require(world.setAgentRuntimeAccessPermissionGrant(id, key, true), "Gain refused");
		require(!agent->getPath() && agent->getState() == core::Agent::State::RoutePlanning
			&& world.getGraph()->getRouteWorkCounts().decisions == decisions,
			"Voluntary planning exposed or calculated a Path at entry");
		auto total = agent->getRoutePlanningRemainingTicks();
		world.advanceTick();
		require(document(world) == authored && clipboard(world, id) == copied,
			"Private candidate or planning runtime state entered document/clipboard");
		if (invalidate)
			require(world.setAgentRuntimeAccessPermissionGrant(id, oldKey, false), "Candidate invalidation refused");
		// Repeated same-destination gains must neither resample nor postpone expiry.
		require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Revoke refused");
		require(world.setAgentRuntimeAccessPermissionGrant(id, key, true), "Repeated gain refused");
		if (fail || withdrawShortcut)
			require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Shortcut withdrawal refused");
		require(agent->getRoutePlanningRemainingTicks() == total - 1
			&& agent->getRoutePlanningTotalTicks() == total, "Environmental trigger restarted planning");
		world.consumeSimulationEvents();
		world.advanceTicks(total - 2);
		require(!agent->getPath() && agent->getGlobalPosition() == position, "Thinking exposed a Path or moved");
		world.advanceTick();
		require(agent->getGlobalPosition() == position, "Expiry moved Agent");
		unsigned lost = 0;
		for (auto const& event : world.consumeSimulationEvents())
		{
			if (event.type == core::SimulationEventType::RouteLost) ++lost;
			require(event.type != core::SimulationEventType::MovementCancelled, "Voluntary planning cancelled destination");
		}
		if (fail)
		{
			require(lost == 1 && !agent->getPath() && agent->getState() == core::Agent::State::Idle,
				"Failed upgrade did not publish Route loss and enter Idle");
			world.advanceTicks(10);
			for (auto const& event : world.consumeSimulationEvents())
				require(event.type != core::SimulationEventType::RouteLost, "Repeated Route loss");
		}
		else
		{
			require(!lost && agent->getPath(), "Valid voluntary decision lost route");
			require((agent->getPath() == retained) == ((persistence == 1.0f || withdrawShortcut) && !invalidate),
				"Persistence or mandatory upgrade chose wrong Path");
			world.advanceTick();
			require(agent->getGlobalPosition() != position, "Chosen Path did not move next tick");
		}
	}

}

namespace routing_smoke
{
	void registerPlanningEditor(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "routing/voluntaryAuthorizationUpgrade", [](smoke::Context const&)
		{
			EditorState state;
			voluntaryAuthorizationPlanning(0.0f, false, false);
		} });
		checks.push_back({ "routing/voluntaryAuthorizationPersistence", [](smoke::Context const&)
		{
			EditorState state;
			voluntaryAuthorizationPlanning(1.0f, false, false);
		} });
		checks.push_back({ "routing/voluntaryAuthorizationInvalidation", [](smoke::Context const&)
		{
			EditorState state;
			voluntaryAuthorizationPlanning(1.0f, true, false);
		} });
		checks.push_back({ "routing/voluntaryAuthorizationRouteLoss", [](smoke::Context const&)
		{
			EditorState state;
			voluntaryAuthorizationPlanning(1.0f, true, true);
		} });
		checks.push_back({ "routing/voluntaryAuthorizationWithdrawnShortcut", [](smoke::Context const&)
		{
			EditorState state;
			voluntaryAuthorizationPlanning(0.0f, false, false, true);
		} });
		checks.push_back({ "routing/boundariesAndPresentation", [](smoke::Context const&)
		{
			EditorState state;
			boundariesAndPresentation();
		} });
		checks.push_back({ "routing/inclusiveEndpointsAndPersistence", [](smoke::Context const&)
		{
			EditorState state;
			inclusiveEndpointsAndPersistence();
		} });
	}
}
