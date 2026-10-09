#include "Checks.h"
#include "MobilityLifecycle.h"
#include "../simulation/InteractionResults.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AccessPanel.h"
#include "core/Button.h"
#include "core/DoorSectorObject.h"
#include "core/Lift.h"
#include "core/LiftTransit.h"
#include "core/Window.h"
#include <yaml-cpp/yaml.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace
{
	using smoke::require;

	void clean(core::World& world)
	{
		world.advanceTicks(600);
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty()
			&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Mixed journey leaked coordination work");
	}

	void runLaboratory(core::World& world)
	{
		std::shared_ptr<const core::Door> door;
		std::vector<core::TraversalResourceId> vehicles;
		core::TraversalResourceId platform;
		for (unsigned s = 0; s < world.getNumSectors(); ++s)
		{
			auto sector = world.getSector(s);
			if (auto transit = std::dynamic_pointer_cast<const core::LiftTransit>(sector))
				vehicles.push_back(transit->getLift()->getTraversalResourceId());
			for (unsigned o = 0; o < sector->getNumObjects(); ++o)
			{
				auto object = sector->getObject(o)->_getObject();
				if (s == 0) if (auto candidate = std::dynamic_pointer_cast<const core::Door>(object)) door = candidate;
				if (auto lift = std::dynamic_pointer_cast<const core::Lift>(object))
				{
					platform = lift->getTraversalResourceId();
					vehicles.push_back(platform);
				}
			}
		}
		require(door && vehicles.size() == 2, "Laboratory lost manual Door or both lift kinds");
		auto actor = world.lookupAgent(core::AgentId{1}).entity;
		require(actor->getObjectUsage() == core::ObjectUsage::RemoteControl && actor->getObjectUsageDistance() == 3.f
			&& !actor->getIndividualObjectUsage(), "Example script defaults materialized or lost");
		auto tagged = world.lookupAgent(core::AgentId{2}).entity;
		require(tagged->getEffectiveObjectUsage().sourceTag && tagged->getEffectiveObjectUsageDistance().sourceTag
			&& !tagged->getIndividualObjectUsage(), "Example tag provenance lost");
		require(world.lookupAgent(core::AgentId{3}).entity->getEffectiveObjectUsage().individual
			&& world.lookupAgent(core::AgentId{4}).entity->getObjectUsage() == core::ObjectUsage::Arms
			&& world.lookupAgent(core::AgentId{5}).entity->getObjectUsage() == core::ObjectUsage::None, "Example modes lost");
		std::set<uint64_t> arrived;
		std::set<core::TraversalResourceId> boarded, selected;
		bool early = false, physicalCrossing = false, diagnostics = false, changed = false;
		world.resumeSimulation();
		for (unsigned tick = 0; tick < 20000 && arrived.size() != 5; ++tick)
		{
			world.advanceTick();
			actor = world.lookupAgent(core::AgentId{1}).entity;
			auto position = actor->getGlobalPosition();
			if (door->isOpening() && actor->getSector()->getIndex() == 0 && position.x < 8.f) early = true;
			if (!physicalCrossing && actor->getSector()->getIndex() == 2)
			{
				require(std::abs(position.x - 8.5f) <= .201f && std::abs(position.y) < .001f,
					"Remote activation replaced physical threshold alignment");
				physicalCrossing = true;
			}
			if (auto path = actor->getPath(); path && !diagnostics)
			{
				auto const& last = path->nodes.back();
				require(last.objectiveDurationSeconds && std::isfinite(*last.objectiveDurationSeconds)
					&& *last.objectiveDurationSeconds > 0 && std::isfinite(last.cumulativePerceivedCost)
					&& last.cumulativePerceivedCost > 0, "Mixed Path lost duration/perceived-cost diagnostics");
				diagnostics = true;
			}
			for (auto vehicle : vehicles)
			{
				unsigned occupants = 0;
				for (uint64_t id = 1; id <= 5; ++id)
					if (world.isAgentTransportOccupant(vehicle, core::AgentId{id}))
					{
						++occupants;
						if (id == 1) boarded.insert(vehicle);
					}
				require(occupants <= 1, "Remote agents bypassed capacity-one queue");
			}
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::InteractionRequestAdded)
					require(event.interactionRequest.actor != core::AgentId{5}, "None observer operated a device");
				if (event.type == core::SimulationEventType::DeviceOperationChanged
					&& event.deviceOperation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& event.deviceOperation.requester == core::AgentId{1}
					&& event.deviceOperation.state == core::DeviceOperationState::Succeeded)
				{
					selected.insert(event.deviceOperation.command.traversalResource);
					if (event.deviceOperation.command.traversalResource == platform && !changed)
					{
						changed = true;
						world.pauseSimulation();
						require(world.setAgentIndividualObjectUsage(core::AgentId{1}, core::ObjectUsage::None), "Committed capability edit failed");
						world.resumeSimulation();
					}
				}
				if (event.type == core::SimulationEventType::RouteLost && event.agent.id == core::AgentId{1})
					throw smoke::Failure("Primary mixed route lost reason=" + std::to_string(int(event.routeLossReason))
						+ " position=" + std::to_string(position.x) + "," + std::to_string(position.y));
				if (event.type == core::SimulationEventType::DestinationReached) arrived.insert(event.agent.id.value);
			}
			require(world.getSector(2)->areLightsOn(), "Mixed Path activated unrelated light switch");
		}
		std::string transportDiagnostic;
		for (auto const& resource : world.getSimulationSnapshot().traversalResources)
			if (resource.isLift) transportDiagnostic += " lift=" + std::to_string(resource.id.value)
				+ " phase=" + std::to_string(int(resource.liftStopPhase)) + " stop=" + std::to_string(resource.liftCurrentStop)
				+ " occupants=" + std::to_string(resource.occupantCount) + " admissions=" + std::to_string(resource.admissionReservationCount);
		for (auto const& request : world.getSimulationSnapshot().traversalRequests)
			if (request.owner == core::AgentId{1}) transportDiagnostic += " edge=" + std::to_string(int(request.edgeType))
				+ " target=" + std::to_string(request.destinationEndpoint.x) + "," + std::to_string(request.destinationEndpoint.y)
				+ " diagnostic=" + request.diagnostic;
		for (auto const& request : world.getSimulationSnapshot().interactionRequests)
			transportDiagnostic += " interaction=" + std::to_string(request.point.value) + " outcome=" + std::to_string(int(request.result));
		require(arrived.size() == 5 && early && physicalCrossing && diagnostics && boarded.size() == 2 && selected.size() == 2 && changed,
			"Mixed Door/Lift/Platform journey incomplete: arrivals=" + std::to_string(arrived.size())
			+ " boarded=" + std::to_string(boarded.size()) + " selected=" + std::to_string(selected.size())
			+ " primary=" + std::to_string(actor->getGlobalPosition().x) + "," + std::to_string(actor->getGlobalPosition().y)
			+ " state=" + std::to_string(int(actor->getState())) + " door=" + std::to_string(door->getOpenPercentage())
			+ " early=" + std::to_string(early) + " crossed=" + std::to_string(physicalCrossing) + transportDiagnostic);
		require(actor->getSector()->getIndex() == 3 && std::abs(actor->getGlobalPosition().y - 2.f) < .001f
			&& std::abs(actor->getGlobalPosition().x - 17.f) < .001f, "Passenger did not physically disembark at goal");
		clean(world);
	}

	void remoteMixedWorld(smoke::Context const& context)
	{
		auto fixture = context.fixture("resources/test-worlds/remote-activation.world.yaml");
		auto world = core::loadWorldDocument(fixture);
		require(bool(world), "Laboratory did not load");
		// Both wire formats preserve manifest resource references, not live Lua snapshots.
		for (auto name : {"laboratory.world.yaml", "laboratory.world"})
		{
			auto path = context.temporaryRoot() / name;
			world->saveTo(path.string());
			auto restored = core::loadWorldDocument(path);
			require(bool(restored), "Mixed World round trip failed");
			runLaboratory(*restored);
		}
		runLaboratory(*world);
		world->pauseSimulation();
		require(world->setAgentIndividualObjectUsage(core::AgentId{1}, std::nullopt), "Reset override removal failed");
		world->resetSimulation();
		runLaboratory(*world);
	}

	void remoteMixedCosts(smoke::Context const& context)
	{
		struct Evidence { float duration = 0, perceived = 0; unsigned ticks = 0; };
		auto measure = [&](bool arms)
		{
			auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/remote-activation.world.yaml"));
			world->pauseSimulation();
			for (uint64_t id = 2; id <= 5; ++id) require(world->setAgentActive(core::AgentId{id}, false), "Cost isolation failed");
			if (arms) require(world->setAgentIndividualObjectUsage(core::AgentId{1}, core::ObjectUsage::Arms)
				&& world->setAgentIndividualObjectUsageDistance(core::AgentId{1}, .25f), "Arms comparison authoring failed");
			Evidence evidence;
			world->resumeSimulation();
			bool arrived = false;
			for (unsigned tick = 0; tick < 12000 && !arrived; ++tick)
			{
				world->advanceTick();
				auto actor = world->lookupAgent(core::AgentId{1}).entity;
				if (auto path = actor->getPath(); path && evidence.duration == 0)
				{
					auto const& last = path->nodes.back();
					require(last.objectiveDurationSeconds.has_value(), "Cost comparison lost objective estimate");
					evidence.duration = *last.objectiveDurationSeconds;
					evidence.perceived = last.cumulativePerceivedCost;
				}
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached && event.agent.id == core::AgentId{1})
					{ arrived = true; evidence.ticks = tick + 1; }
			}
			require(arrived, "Isolated mixed cost comparison did not arrive");
			clean(*world);
			return evidence;
		};
		auto remote = measure(false), arms = measure(true);
		require(remote.duration > 0 && remote.perceived > 0 && remote.duration <= arms.duration
			&& remote.perceived <= arms.perceived && remote.ticks < arms.ticks,
			"Remote approach savings disagree with duration, perceived cost or elapsed journey ticks");
	}

	void remoteMixedControls(smoke::Context const& context)
	{
		// One Sector, two Levels, three physical centres, deliberately different
		// from their approach points. A persisted Local depth must not become range.
		core::World scene("Mixed remote controls", 10, 3);
		auto front = scene.addRoom("Front", 0, 0, 0, 10, 2);
		auto back = scene.addRoom("Back", 1, 0, 0, 10, 2);
		for (unsigned x = 0; x < 10; ++x) scene.addSectorWalkway(back, 1, x);
		auto booth = std::static_pointer_cast<const core::BoothWindow>(scene.addBoothWindow(0, 0, 4).object);
		auto made = scene.addAccessPanel(back, 0, 2);
		auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(made.sector->getObject(made.index))->getPanel();
		auto buttonMade = scene.addSectorLightSwitch(back, 7);
		auto button = std::dynamic_pointer_cast<const core::Button>(buttonMade.sector->getObject(buttonMade.index)->_getObject());
		scene.finishBuild(); scene.pauseSimulation();
		auto id = scene.createAgent("Cross-Level remote", back, 1, 4.5f);
		require(scene.setAgentIndividualObjectUsage(id, core::ObjectUsage::RemoteControl)
			&& scene.setAgentIndividualObjectUsageDistance(id, 10.f), "Mixed control authoring failed");
		auto file = context.temporaryRoot() / "controls.world.yaml"; scene.saveTo(file.string());
		auto yaml = YAML::LoadFile(file.string()); yaml["agents"][0]["localDepth"] = 7;
		{ std::ofstream out(file); out << yaml; }
		auto world = core::loadWorldDocument(file);
		require(bool(world), "Mixed controls depth document refused");
		panel = world->lookupAccessPanel(panel->getId()); booth = world->lookupBoothWindow(booth->getDeviceId());
		auto actor = world->lookupAgent(id).entity;
		require(actor->getLocalDepth() == 7, "Authored Local depth lost");
		auto position = actor->getGlobalPosition();
		for (unsigned category = 0; category < 3; ++category)
		{
			auto centre = category == 0 ? panel->getPosition() + panel->getSize() * .5f
				: category == 1 ? booth->getPosition() + booth->getSize() * .5f : button->getPosition() + button->getSize() * .5f;
			auto measured = std::hypot(double(position.x) - centre.x, double(position.y) - centre.y);
			auto range = float(measured);
			if (double(range) < measured) range = std::nextafter(range, std::numeric_limits<float>::infinity());
			auto request = [&] { return category == 0 ? world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, id)
				: world->requestInteraction(category == 1 ? booth->getPanel() : buttonMade.interactionPoint, id); };
			world->pauseSimulation();
			require(world->setAgentIndividualObjectUsageDistance(id, range - .0001f), "Outside range edit failed");
			require(!request(), "Mixed control accepted outside physical centre range");
			require(world->setAgentIndividualObjectUsageDistance(id, range), "Inclusive range edit failed");
			auto accepted = request(); require(bool(accepted), "Same-Sector cross-Level inclusive range refused category=" + std::to_string(category));
			// Loss of mode before execution publishes cancellation for every category.
			require(world->setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Paused mode edit refused");
			require(simulation_smoke::observedInteractionResult(*world, accepted) == core::InteractionResult::Cancelled,
				"Mixed pending request not cancelled");
			require(!request(), "None acquired an Arms fallback");
			require(world->setAgentIndividualObjectUsage(id, core::ObjectUsage::RemoteControl), "Remote restoration failed");
			accepted = request(); require(bool(accepted), "Remote request after cancellation refused");
			world->resumeSimulation(); world->advanceTicks(180);
			require(simulation_smoke::observedInteractionResult(*world, accepted) == core::InteractionResult::Succeeded,
				"Mixed control did not publish success");
			require(actor->getGlobalPosition() == position && actor->getLocalDepth() == 7, "Remote control approached or changed depth");
		}
		require(panel->getState() == core::AccessPanel::State::Open && booth->getState() == core::Window::State::Open
			&& !world->getSector(back)->areLightsOn(), "Mixed device state mismatch");
		world->pauseSimulation();
		auto outsider = world->createAgent("Other Sector", front, 0, 4.5f);
		require(world->setAgentIndividualObjectUsage(outsider, core::ObjectUsage::RemoteControl)
			&& world->setAgentIndividualObjectUsageDistance(outsider, 100.f), "Outsider authoring failed");
		require(!world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Close, outsider)
			&& !world->requestInteraction(booth->getPanel(), outsider)
			&& !world->requestInteraction(buttonMade.interactionPoint, outsider), "Mixed controls crossed Sector boundary");
		require(world->setAgentIndividualObjectUsageDistance(id, 10.f)
			&& world->setAgentIndividualRemoteAccessPanels(id, false), "Panel category edit failed");
		require(!world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Close, id), "Disabled remote panel fell back to Arms");
		require(world->setAgentIndividualRemoteAccessPanels(id, true)
			&& world->setAgentIndividualRemoteBoothWindowShutters(id, false), "Independent shutter category edit failed");
		require(!world->requestInteraction(booth->getPanel(), id), "Disabled remote shutter fell back to Arms");
		auto closePanel = world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Close, id);
		require(bool(closePanel), "Shutter flag disabled unrelated panel category");
		world->resumeSimulation(); world->advanceTicks(180);
		require(simulation_smoke::observedInteractionResult(*world, closePanel) == core::InteractionResult::Succeeded,
			"Mixed panel Close did not succeed");
		world->pauseSimulation();
		require(world->setAgentIndividualRemoteBoothWindowShutters(id, true), "Shutter restoration failed");
		auto closeShutter = world->requestInteraction(booth->getPanel(), id);
		require(bool(closeShutter), "Mixed shutter Close request failed");
		world->resumeSimulation(); world->advanceTicks(180);
		require(simulation_smoke::observedInteractionResult(*world, closeShutter) == core::InteractionResult::Succeeded,
			"Mixed shutter Close did not succeed");
		auto lights = world->requestInteraction(buttonMade.interactionPoint, id);
		require(bool(lights), "Mixed lights request failed");
		world->advanceTicks(180);
		require(simulation_smoke::observedInteractionResult(*world, lights) == core::InteractionResult::Succeeded
			&& panel->getState() == core::AccessPanel::State::Closed && booth->getState() == core::Window::State::Closed
			&& !world->getSector(back)->areLightsOn() && actor->getGlobalPosition() == position,
			"Mixed closing outcomes or stationary operation changed");
		clean(*world);
	}
}

void agent_smoke::registerRemoteIntegration(std::vector<smoke::Check>& checks)
{
	checks.push_back({"remoteMixedWorld", remoteMixedWorld});
	checks.push_back({"remoteMixedControls", remoteMixedControls});
	checks.push_back({"remoteMixedCosts", remoteMixedCosts});
}
