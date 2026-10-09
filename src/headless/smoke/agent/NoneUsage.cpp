#include "Checks.h"
#include "MobilityLifecycle.h"
#include "core/AgentType.h"
#include "core/AccessPanel.h"
#include "core/AirlockTransit.h"
#include "core/DoorSectorObject.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentTagRegistry.h"
#include <fstream>
#include <cmath>
#include <limits>

namespace
{
	using smoke::require;
	std::string source(std::string const& distance = "")
	{
		auto text = core::bundledHumanAgentType().source;
		agent_smoke::replaceSource(text, "type_id = \"Human\"", "type_id = 'NoneFixture'");
		agent_smoke::replaceSource(text, "object_usage = \"arms\"", "object_usage = 'none'");
		agent_smoke::replaceSource(text, "object_usage_distance = 0.25,", distance);
		return text;
	}
	void attach(core::World& world)
	{
		std::string diagnostic;
		require(world.attachAgentType("none.agent.lua", source(), &diagnostic), diagnostic);
	}
	void quickPlanning(core::World& world, core::AgentId id)
	{
		world.pauseSimulation();
		require(world.setAgentIndividualMinimumRoutePlanningTime(id, .1f)
			&& world.setAgentIndividualMaximumRoutePlanningTime(id, .1f), "Planning bounds refused");
		world.resumeSimulation();
	}
	bool lost(core::World& world, core::AgentId id)
	{
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost && event.agent.id == id
				&& event.routeLossReason == core::RouteLossReason::Unreachable) return true;
		return false;
	}
	void clean(core::World const& world)
	{
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty()
			&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "None leaked coordination work");
	}
	void individualOverrides(smoke::Context const&)
	{
		core::World world("Individual usage", 10, 2);
		auto room = world.addCorridor(0, 0, 10); world.finishBuild(); attach(world);
		auto id = world.createAgent("Operator", room, 0, 2.f);
		auto none = world.createAgent("NoneFixture", "None", room, 0, 4.f);
		world.pauseSimulation();
		std::string diagnostic;
		require(world.setAgentIndividualObjectUsageDistance(id, .125f, &diagnostic), diagnostic);
		require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Mode override refused");
		require(world.lookupAgent(id).entity->getObjectUsageDistance() == .125f, "Mode changed distance");
		require(world.setAgentIndividualObjectUsageDistance(id, 0.f), "None did not ignore distance");
		world.markSaved(); world.consumeSimulationEvents();
		require(!world.setAgentIndividualObjectUsage(id, core::ObjectUsage::Arms, &diagnostic)
			&& !world.isModified() && world.consumeSimulationEvents().empty(), "Invalid combination mutated state");
		require(!world.setAgentIndividualObjectUsage(id, std::nullopt), "Removing mode ignored invalid Arms distance");
		require(world.setAgentIndividualObjectUsageDistance(id, std::nullopt), "Distance removal refused");
		require(world.lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::None
			&& world.lookupAgent(id).entity->getObjectUsageDistance() == .25f, "Distance removal removed mode or default");
		require(world.setAgentIndividualObjectUsage(id, std::nullopt), "Mode removal refused");
		for (float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
		{
			world.markSaved(); world.consumeSimulationEvents();
			require(!world.setAgentIndividualObjectUsageDistance(id, invalid)
				&& !world.isModified() && world.consumeSimulationEvents().empty()
				&& !world.lookupAgent(id).entity->getIndividualObjectUsageDistance(), "Invalid Arms distance was not atomic");
			require(world.setAgentIndividualObjectUsageDistance(none, invalid), "None rejected ignored distance");
			require(!world.setAgentIndividualObjectUsage(none, core::ObjectUsage::Arms), "Ignored invalid distance enabled Arms");
		}
		require(world.setAgentIndividualObjectUsageDistance(none, std::nullopt), "None removal refused");
		require(!world.setAgentIndividualObjectUsage(none, core::ObjectUsage::Arms), "None's zero default admitted Arms");
		require(world.setAgentIndividualObjectUsageDistance(none, .5f)
			&& world.setAgentIndividualObjectUsage(none, core::ObjectUsage::Arms), "Independent distance did not enable Arms");
		require(!world.setAgentIndividualObjectUsageDistance(none, std::nullopt), "Removing required distance was accepted");
		core::DeviceCommand command; command.type = core::DeviceCommandType::SetSectorLights;
		command.target = core::SectorId{room + 1}; command.desiredState = false;
		auto point = world.createInteractionPoint("Reach control", core::SectorId{room + 1}, {2.2f, 0.f}, .5f, 1.f,
			{{command, core::InteractionBindingRequirement::Required}});
		world.resumeSimulation();
		require(!world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Running edit accepted");
		auto request = world.requestInteraction(point, id); require(bool(request), "Arms request refused");
		world.pauseSimulation();
		require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Pending mode change refused");
		auto interaction = world.lookupInteractionRequest(request);
		require(interaction && interaction.entity->getResult() == core::InteractionResult::Cancelled,
			"None override did not cancel pending operation immediately");
		world.resumeSimulation(); world.advanceTicks(150);
		require(world.getSector(room)->areLightsOn(), "Cancelled operation activated");
		world.pauseSimulation();
		require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::Arms)
			&& world.setAgentIndividualObjectUsageDistance(id, .01f), "Arms restoration refused");
		world.resumeSimulation();
		// Physical controls still approach rather than activating remotely.
		auto origin = world.lookupAgent(id).entity->getGlobalPosition();
		require(bool(world.requestInteraction(point, id)), "Short-armed approach request refused");
		world.advanceTicks(1);
		require(world.lookupAgent(id).entity->getGlobalPosition().x > origin.x
			&& world.getSector(room)->areLightsOn(), "Short distance did not require approach");
		world.advanceTicks(150);
		require(!world.getSector(room)->areLightsOn(), "Restored Arms did not operate control");
	}

	void inheritedUsage(smoke::Context const&)
	{
		core::World world("Inherited usage", 10, 2), other("Shared usage", 10, 2);
		auto room = world.addCorridor(0, 0, 10), otherRoom = other.addCorridor(0, 0, 10);
		world.finishBuild(); other.finishBuild(); attach(world);
		world.pauseSimulation(); other.pauseSimulation();
		auto registry = core::AgentTagRegistry::create();
		world.attachAgentTagRegistry("usage.tags.yaml", registry); other.attachAgentTagRegistry("usage.tags.yaml", registry);
		auto mode = registry->addAgentTag("mode"), distance = registry->addAgentTag("distance");
		auto id = world.createAgent("Operator", room, 0, 2.f);
		auto peer = other.createAgent("Peer", otherRoom, 0, 2.f);
		auto none = world.createAgent("NoneFixture", "ScriptNone", room, 0, 4.f);
		require(registry->addAgentTagObjectUsage(mode) && registry->addAgentTagObjectUsageDistance(distance), "Tag property addition failed");
		require(world.assignAgentTag(id, mode) && world.assignAgentTag(id, distance)
			&& other.assignAgentTag(peer, mode) && other.assignAgentTag(peer, distance), "Independent tag assignment failed");
		require(world.assignAgentTag(none, distance) && world.assignAgentTag(none, mode), "Inherited distance did not enable frozen None Agent's Arms");
		auto agent = world.lookupAgent(id).entity;
		require(agent->getEffectiveObjectUsage().sourceTag == mode
			&& agent->getEffectiveObjectUsageDistance().sourceTag == distance
			&& world.lookupAgent(none).entity->getPhysicalBaseline().objectUsage == core::ObjectUsage::None,
			"Sources or frozen defaults were lost");
		require(registry->setAgentTagObjectUsageDistance(distance, .6f), "Shared distance edit failed");
		require(world.setAgentIndividualObjectUsageDistance(id, .1f), "Individual distance failed");
		require(registry->setAgentTagObjectUsage(mode, core::ObjectUsage::None), "Shared mode edit failed");
		require(agent->getObjectUsage() == core::ObjectUsage::None && agent->getObjectUsageDistance() == .1f
			&& agent->getEffectiveObjectUsageDistance().individual, "Properties were resolved as an atomic profile");
		require(world.setAgentIndividualObjectUsageDistance(id, std::nullopt)
			&& agent->getObjectUsageDistance() == .6f && agent->getEffectiveObjectUsageDistance().sourceTag == distance,
			"Removing individual distance did not reveal tag");
		require(registry->setAgentTagObjectUsageDistance(distance, 0.f), "None rejected ignored tag distance");
		world.markSaved(); other.markSaved(); registry->markUnmodified();
		auto revision = registry->getNextPropertyRevision();
		for (float invalid : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
		{
			require(registry->setAgentTagObjectUsageDistance(distance, invalid), "None refused ignored inherited distance");
			auto currentRevision = registry->getNextPropertyRevision();
			require(!registry->setAgentTagObjectUsage(mode, core::ObjectUsage::Arms)
				&& registry->getNextPropertyRevision() == currentRevision, "Invalid inherited usable distance admitted Arms");
		}
		require(registry->setAgentTagObjectUsageDistance(distance, 0.f), "Restore ignored zero failed");
		world.markSaved(); other.markSaved(); registry->markUnmodified(); revision = registry->getNextPropertyRevision();
		require(!registry->setAgentTagObjectUsage(mode, static_cast<core::ObjectUsage>(99))
			&& !registry->setAgentTagObjectUsage(mode, core::ObjectUsage::Arms)
			&& !registry->removeAgentTagObjectUsage(mode) && !registry->deleteAgentTag(mode)
			&& !world.removeAgentTag(id, mode) && !world.setAgentIndividualObjectUsage(id, core::ObjectUsage::Arms)
			&& !world.isModified() && !other.isModified() && !registry->isModified()
			&& registry->getNextPropertyRevision() == revision, "Invalid shared combination was not atomic");
		require(world.setAgentIndividualObjectUsageDistance(id, 0.f), "None ignored individual distance failed");
		auto replacement = core::AgentTagRegistry::create();
		bool refused = false;
		try { world.attachAgentTagRegistryAndClearAssignments("replacement.tags.yaml", replacement); }
		catch (std::exception const&) { refused = true; }
		require(refused && world.getAgentTagRegistry() == registry && !replacement->hasLoadedWorlds()
			&& world.lookupAgent(id).entity->getAgentTagIds().contains(mode), "Destructive switch exposed invalid fallback or registered a refused World");
		require(world.setAgentIndividualObjectUsageDistance(id, .5f), "Masked valid distance failed");
		require(!registry->setAgentTagObjectUsage(mode, core::ObjectUsage::Arms), "One masked Agent hid another World's invalid configuration");
		require(other.setAgentIndividualObjectUsageDistance(peer, .5f)
			&& world.setAgentIndividualObjectUsageDistance(none, .5f)
			&& registry->setAgentTagObjectUsage(mode, core::ObjectUsage::Arms), "Valid masked combinations refused");
		require(!world.setAgentIndividualObjectUsageDistance(id, std::nullopt), "Removal revealed invalid inherited distance");
		auto conflict = registry->addAgentTag("conflict");
		require(world.assignAgentTag(id, conflict), "Empty tag assignment failed");
		revision = registry->getNextPropertyRevision();
		require(!registry->addAgentTagObjectUsageDistance(conflict) && !registry->addAgentTagObjectUsage(conflict)
			&& registry->getNextPropertyRevision() == revision, "Individual override masked tag-source conflict");
		require(registry->setAgentTagObjectUsageDistance(distance, .25f)
			&& world.setAgentIndividualObjectUsageDistance(id, std::nullopt), "Restore inherited distance failed");
		core::DeviceCommand command; command.type = core::DeviceCommandType::SetSectorLights;
		command.target = core::SectorId{room + 1}; command.desiredState = false;
		auto point = world.createInteractionPoint("Tag control", core::SectorId{room + 1}, {2.2f, 0.f}, .5f, 1.f,
			{{command, core::InteractionBindingRequirement::Required}});
		world.resumeSimulation();
		require(!registry->setAgentTagObjectUsage(mode, core::ObjectUsage::None), "Shared edit accepted while a World ran");
		auto request = world.requestInteraction(point, id); require(bool(request), "Inherited Arms did not request control");
		world.pauseSimulation();
		require(registry->setAgentTagObjectUsage(mode, core::ObjectUsage::None), "Paused None edit failed");
		bool cancelled = false;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::InteractionRequestChanged && event.interactionRequest.id == request
				&& event.interactionRequest.result == core::InteractionResult::Cancelled) cancelled = true;
		require(cancelled, "Shared edit did not publish pending-operation cancellation");
		world.resumeSimulation(); world.advanceTicks(150);
		require(world.getSector(room)->areLightsOn(), "Cancelled inherited operation executed");
		world.pauseSimulation();
		require(registry->setAgentTagObjectUsage(mode, core::ObjectUsage::Arms), "Restored Arms failed");
		world.resumeSimulation(); require(bool(world.requestInteraction(point, id)), "Restored inherited operation refused");
		world.advanceTicks(150); require(!world.getSector(room)->areLightsOn(), "Restored inherited Arms did not operate");
		world.pauseSimulation();
		require(registry->removeAgentTagObjectUsage(mode), "Mode removal failed");
		require(agent->getObjectUsage() == core::ObjectUsage::Arms && !agent->getEffectiveObjectUsage().sourceTag
			&& agent->getObjectUsageDistance() == .25f && world.lookupAgent(none).entity->getObjectUsage() == core::ObjectUsage::None,
			"Mode removal destroyed distance or frozen fallback");
		require(registry->removeAgentTagObjectUsageDistance(distance), "Distance removal failed");
		require(agent->getObjectUsageDistance() == .25f && !agent->getEffectiveObjectUsageDistance().sourceTag
			&& agent->getAgentTagIds().contains(distance) && world.lookupAgent(none).entity->getIndividualObjectUsageDistance() == .5f,
			"Distance removal changed assignments, other overrides or frozen defaults");
	}

	void inheritedRoutes(smoke::Context const&)
	{
		core::World world("Inherited routes", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1), back = world.addRoom("Back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
		world.addSectorDoor(0, 0, 3, options); world.addSectorMarker(back, 0, 3.5f, "Goal"); world.finishBuild();
		world.pauseSimulation(); auto registry = core::AgentTagRegistry::create();
		world.attachAgentTagRegistry("routes.tags.yaml", registry);
		auto tag = registry->addAgentTag("operator"); require(registry->addAgentTagObjectUsage(tag), "Tag addition failed");
		auto id = world.createAgent("Walker", front, 0, 1.f); require(world.assignAgentTag(id, tag), "Assignment failed");
		quickPlanning(world, id); require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Route intent failed");
		world.pauseSimulation(); require(registry->setAgentTagObjectUsage(tag, core::ObjectUsage::None), "None tag edit failed");
		world.resumeSimulation(); world.advanceTicks(180);
		require(lost(world, id) && world.lookupAgent(id).entity->getSector()->getIndex() == front,
			"Tag None did not reconsider self-operation route");
		world.pauseSimulation(); require(registry->setAgentTagObjectUsage(tag, core::ObjectUsage::Arms), "Arms tag edit failed");
		world.resumeSimulation(); require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Restored route intent failed");
		bool admitted = false;
		for (unsigned tick = 0; tick < 900 && !admitted; ++tick)
		{
			world.advanceTick();
			for (auto const& permit : world.getSimulationSnapshot().traversalPermits) admitted = admitted || permit.owner == id;
		}
		require(admitted, "Inherited Arms did not admit manual crossing");
		world.pauseSimulation(); require(registry->setAgentTagObjectUsage(tag, core::ObjectUsage::None), "Committed None edit failed");
		world.resumeSimulation(); world.advanceTicks(300);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == back, "Shared edit revoked admitted crossing");
	}

	void overrideCommittedCrossing(smoke::Context const&)
	{
		core::World world("Usage committed crossing", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
		world.addSectorDoor(0, 0, 3, options); world.addSectorMarker(back, 0, 3.5f, "Goal"); world.finishBuild();
		auto id = world.createAgent("Walker", front, 0, 3.5f); quickPlanning(world, id);
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Crossing intent refused");
		bool admitted = false;
		for (unsigned tick = 0; tick < 300 && !admitted; ++tick)
		{
			world.advanceTick();
			for (auto const& permit : world.getSimulationSnapshot().traversalPermits) admitted = admitted || permit.owner == id;
		}
		require(admitted, "Manual crossing was not admitted");
		world.pauseSimulation();
		require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Committed mode edit refused");
		world.resumeSimulation(); world.advanceTicks(600);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == back,
			"None override revoked admitted crossing");
		clean(world);
		core::World routing("Usage Path reconsideration", 10, 2);
		auto origin = routing.addRoom("Front", 0, 0, 0, 10, 1);
		auto target = routing.addRoom("Back", 1, 0, 0, 10, 1);
		routing.addSectorDoor(0, 0, 7, options); routing.addSectorMarker(target, 0, 7.5f, "Goal"); routing.finishBuild();
		auto walker = routing.createAgent("Walker", origin, 0, 1.f); quickPlanning(routing, walker);
		require(routing.moveAgentToNamedMarker(walker, "Goal").accepted(), "Reconsideration intent refused");
		routing.advanceTicks(15);
		require(bool(routing.lookupAgent(walker).entity->getPath()), "Initial Arms Path missing");
		routing.pauseSimulation();
		require(routing.setAgentIndividualObjectUsage(walker, core::ObjectUsage::None), "Path mode edit refused");
		routing.resumeSimulation(); routing.advanceTicks(600);
		require(routing.lookupAgent(walker).entity->getSector()->getIndex() == origin,
			"None edit traversed operation-dependent Path");
		bool routeLost = false;
		for (auto const& event : routing.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::RouteLost && event.agent.id == walker)
				routeLost = event.routeLossReason == core::RouteLossReason::TopologyChanged;
		require(routeLost && !routing.lookupAgent(walker).entity->getPath(),
			"None edit did not publish the existing paused-Path restoration failure");
		clean(routing);
	}

	void declarations(smoke::Context const&)
	{
		for (auto const* distance : {"", "object_usage_distance = 0,", "object_usage_distance = -1,",
			"object_usage_distance = 0/0,", "object_usage_distance = math.huge,", "object_usage_distance = false,", "reach = .25,"})
		{
			core::World world("None declaration", 8, 2);
			auto room = world.addCorridor(0, 0, 8); world.finishBuild();
			require(world.attachAgentType("none.agent.lua", source(distance)), "None type attach failed");
			auto id = world.createAgent("NoneFixture", "Non-operator", room, 0, 2.f);
			auto agent = world.lookupAgent(id).entity;
			require(agent->getObjectUsage() == core::ObjectUsage::None && agent->getObjectUsageDistance() == 0,
				"None did not ignore distance with a canonical observation");
			auto snapshot = world.getSimulationSnapshot().agents.front();
			require(snapshot.objectUsage == core::ObjectUsage::None && snapshot.objectUsageDistance == 0,
				"None snapshot differs from frozen defaults");
			world.addLevel();
			require(world.lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::None, "Replay lost None");
		}
	}
	void invalidNoneDeclarations(smoke::Context const&)
	{
		for (bool aliases : {false, true})
		{
			core::World world("Invalid None", 8, 2);
			auto room = world.addCorridor(0, 0, 8); world.finishBuild();
			auto text = source(aliases ? "reach = .25, object_usage_distance = 0," : "");
			if (!aliases) agent_smoke::replaceSource(text, "width = 0.4", "width = 0");
			require(world.attachAgentType("none.agent.lua", text), "Invalid instance type object attach failed");
			world.markSaved(); world.consumeSimulationEvents();
			bool refused = false;
			try { world.createAgent("NoneFixture", "Refused", room, 0, 2.f); }
			catch (std::exception const& error) { refused = std::string(error.what()).find(aliases ? "mutually exclusive" : "width") != std::string::npos; }
			require(refused && !world.isModified() && world.getSimulationSnapshot().agents.empty()
				&& world.consumeSimulationEvents().empty(), "Invalid None skipped validation or partially published an Agent");
		}
	}

	void directRefusals(smoke::Context const&)
	{
		core::World world("None requests", 10, 2);
		world.addRoom("Front", 0, 0, 0, 10, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 10, 1);
		auto booth = std::static_pointer_cast<const core::BoothWindow>(world.addBoothWindow(0, 0, 3).object);
		auto placed = world.addAccessPanel(back, 0, 5);
		auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(placed.sector->getObject(placed.index))->getPanel();
		world.finishBuild(); attach(world);
		auto id = world.createAgent("NoneFixture", "Non-operator", back, 0, 3.5f);
		world.consumeSimulationEvents();
		auto original = world.lookupAgent(id).entity->getGlobalPosition();
		require(!world.requestInteraction(booth->getPanel(), id)
			&& !world.canRequestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, id)
			&& !world.requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, id), "None operated an owned control");
		require(world.consumeSimulationEvents().empty(), "Refused owned request published events");
		for (auto type : {core::DeviceCommandType::SetSectorLights})
		{
			core::DeviceCommand command; command.type = type; command.target = core::SectorId{back + 1};
			auto point = world.createInteractionPoint("Physical control", core::SectorId{back + 1}, original, 1.f, 0.f,
				{{command, core::InteractionBindingRequirement::Required}});
			world.consumeSimulationEvents();
			require(!world.requestInteraction(point, id), "None admitted a typed Button operation");
			require(world.consumeSimulationEvents().empty(), "Refused request published an operation/event");
		}
		world.advanceTicks(100);
		require(booth->getState() == core::Window::State::Closed && panel->getState() == core::AccessPanel::State::Closed
			&& world.getSector(back)->areLightsOn() && world.lookupAgent(id).entity->getGlobalPosition() == original,
			"None moved to or operated a refused control");
		clean(world);
	}
	void doors(smoke::Context const&)
	{
		for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::RemoteControlled, core::DoorActivationMode::Automatic})
		for (bool alternate : {false, true})
		for (bool scripted : {false, true})
		{
			core::World world("None routes", 10, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 10, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 10, 1);
			core::World::CreateDoorOptions options; options.activationMode = mode;
			world.addSectorDoor(0, 0, 2, options);
			if (alternate) { options.activationMode = core::DoorActivationMode::Automatic; world.addSectorDoor(0, 0, 7, options); }
			world.addSectorMarker(back, 0, 2.5f, "Goal"); world.finishBuild(); attach(world);
			auto id = world.createAgent(scripted ? "NoneFixture" : "Human", "Walker", front, 0, 1.f);
			if (!scripted)
			{
				world.pauseSimulation();
				require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "None override refused");
			}
			quickPlanning(world, id);
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "None movement intent refused");
			world.advanceTicks(2000);
			auto agent = world.lookupAgent(id).entity;
			bool const feasible = alternate || mode == core::DoorActivationMode::Automatic;
			require((agent->getSector()->getIndex() == back) == feasible && !agent->getPath(), "None route did not exclude self-operation or use automatic alternative");
			if (!feasible) require(lost(world, id), "Impossible None route did not publish Unreachable Route loss");
			else for (auto const& event : world.consumeSimulationEvents())
				require(event.type != core::SimulationEventType::InteractionRequestAdded || event.interactionRequest.actor != id,
					"None pressed a passing/traversal Button");
			clean(world);
		}
		// Locally usable manual Doors retain authorization/adherence and Mobility.
		for (unsigned variant = 0; variant < 4; ++variant)
		{
			core::World world("None open Door", 8, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
			core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual; options.holdOpenSeconds = 20;
			auto made = world.addSectorDoor(0, 0, 3, options);
			world.addSectorMarker(back, 0, 3.5f, "Goal"); world.finishBuild(); attach(world);
			auto id = world.createAgent("NoneFixture", "Walker", front, 0, 3.5f); quickPlanning(world, id);
			world.pauseSimulation();
			auto key = world.addAccessPermission("Door key");
			require(world.setManualDoorPermissionRequirement(made.traversalResource, {key}), "Door requirement refused");
			if (variant == 1) require(world.setAgentIndividualPermissionAdherence(id, false), "Adherence refused");
			if (variant >= 2) require(world.grantAgentAccessPermission(id, key), "Grant refused");
			if (variant == 3) { core::MobilityProfile mobility; mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse); require(world.setAgentIndividualMobilityProfile(id, mobility), "Mobility refused"); }
			world.resumeSimulation();
			core::DeviceCommand open; open.type = core::DeviceCommandType::OpenDoor; open.traversalResource = made.traversalResource;
			open.desiredState = true;
			auto operatorId = world.createAgent("Door operator", front, 0, 3.5f);
			world.pauseSimulation(); require(world.grantAgentAccessPermission(operatorId, key), "Operator grant refused"); world.resumeSimulation();
			auto control = world.createInteractionPoint("Door opening control", core::SectorId{front + 1}, {3.5f, 0}, .25f, 0,
				{{open, core::InteractionBindingRequirement::Required}});
			require(bool(world.requestInteraction(control, operatorId)), "Physical opening refused");
			world.advanceTicks(150);
			require(!world.canAgentOpenManualDoor(made.traversalResource, id), "None gained operation through a permission grant");
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Open Door intent refused");
			if (variant == 2)
			{
				bool admitted = false;
				for (unsigned tick = 0; tick < 150 && !admitted; ++tick)
				{
					world.advanceTick();
					for (auto const& permit : world.getSimulationSnapshot().traversalPermits) admitted = admitted || permit.owner == id;
				}
				require(admitted, "None was not admitted to an open Door");
				require(world.setAgentRuntimeAccessPermissionGrant(id, key, false), "Committed revoke failed");
			}
			world.advanceTicks(300);
			require((world.lookupAgent(id).entity->getSector()->getIndex() == back) == (variant == 1 || variant == 2), "None changed open-Door authorization, adherence or Mobility, variant " + std::to_string(variant));
			clean(world);
		}
	}
	void otherResources(smoke::Context const&)
	{
		for (bool extended : {false, true})
		{
			core::World world("None Force Bridge", 8, 2);
			auto room = world.addRoom("Bridge", 0, 0, 0, 8, 2);
			world.addSectorWalkway(room, 1, 0); world.addSectorWalkway(room, 1, 3);
			world.addSectorForceBridge(room, 1, 1, {2, CORE_SIDE_LEFT, true, extended, 2});
			world.addSectorMarker(room, 1, 3.5f, "Goal"); world.finishBuild(); attach(world);
			auto id = world.createAgent("NoneFixture", "Walker", room, 1, .5f); quickPlanning(world, id);
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Bridge intent refused"); world.advanceTicks(800);
			require((world.lookupAgent(id).entity->getGlobalPosition().x > 3.f) == extended, "None bridge route confused operation with passage");
			if (!extended) require(lost(world, id), "Retracted None bridge did not yield Route loss");
			clean(world);
		}
		for (unsigned kind = 0; kind < 3; ++kind)
		{
			core::World world("None automatic devices", 10, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
			auto right = world.addRoom("Right", 0, 0, kind == 0 ? 4 : 6, kind == 0 ? 6 : 4, 1);
			if (kind == 0) { core::World::CreateBulkheadDoorOptions options; options.activationMode = core::DoorActivationMode::Automatic; options.controls[0] = options.controls[1] = false; world.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT, options); }
			if (kind == 1) world.addChamber(0, 0, 4, 2);
			if (kind == 2) world.addAirlock(0, 0, 4, 2, 1);
			world.addSectorMarker(right, 0, 1.f, "Goal"); world.finishBuild(); attach(world);
			auto id = world.createAgent("NoneFixture", "Walker", left, 0, 3.5f); quickPlanning(world, id);
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Device intent refused"); world.advanceTicks(3000);
			std::string status;
			for (auto const& entry : world.getSimulationSnapshot().traversalRequests) status += " / " + entry.diagnostic;
			for (auto const& entry : world.getSimulationSnapshot().securityScanners) status += " phase=" + entry.phase;
			require((world.lookupAgent(id).entity->getSector()->getIndex() == right) == (kind != 2), "None disabled automatic devices or operated an Airlock Button, kind " + std::to_string(kind)
				+ " sector=" + std::to_string(world.lookupAgent(id).entity->getSector()->getIndex())
				+ " x=" + std::to_string(world.lookupAgent(id).entity->getGlobalPosition().x) + status);
			if (kind == 2) require(lost(world, id), "None Airlock self-operation did not yield Route loss");
			clean(world);
		}
	}

	void sharedAirlock(smoke::Context const&)
	{
		core::World world("None shared Airlock", 10, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 6, 4, 1);
		auto chamber = world.addAirlock(0, 0, 4, 2, 1);
		world.addSectorMarker(right, 0, 1.f, "Goal"); world.finishBuild(); attach(world);
		auto id = world.createAgent("NoneFixture", "Passenger", left, 0, 3.5f); quickPlanning(world, id);
		auto operatorId = world.createAgent("Operator", left, 0, 2.5f);
		auto resource = std::static_pointer_cast<const core::AirlockTransit>(world.getSector(chamber))->getTraversalResourceId();
		require(world.moveAgentToNamedMarker(operatorId, "Goal").accepted(), "Airlock operator intent refused");
		bool usable = false;
		for (unsigned tick = 0; tick < 1400 && !usable; ++tick)
		{
			world.advanceTick(); usable = world.canAgentEnterAirlock(resource, core::SectorId{left + 1}, id, true);
		}
		require(usable, "None could not observe another Agent's usable Airlock entrance");
		world.consumeSimulationEvents();
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "None shared Airlock intent refused"); world.advanceTicks(4500);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == right
			&& !world.lookupAgent(id).entity->getPath(), "None did not finish accepted Airlock journey");
		for (auto const& event : world.consumeSimulationEvents())
			require(event.type != core::SimulationEventType::InteractionRequestAdded || event.interactionRequest.actor != id, "None operated the Airlock outside control");
		clean(world);
	}

	void lifetimes(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "none-lifetime.agent.lua";
		auto write = [&](std::string const& text) { std::ofstream out(path); out << text; require(bool(out), "Cannot write None lifetime resource"); };
		write(source());
		auto resource = core::externalAgentTypeResourceName(path);
		core::World world("None lifetimes", 10, 2);
		auto room = world.addCorridor(0, 0, 8); world.finishBuild(); world.pauseSimulation();
		auto def = core::resolveAgentTypeResource(resource);
		require(def && world.attachAgentType(resource, def->source), "None resource attach failed");
		auto id = world.createAgent("NoneFixture", "Survivor", room, 0, 2.f);
		world.saveTo((context.temporaryRoot() / "none.world.yaml").string());
		world.saveTo((context.temporaryRoot() / "none.world").string());
		for (auto const* filename : {"none.world.yaml", "none.world"})
			require(core::loadWorldDocument(context.temporaryRoot() / filename)->lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::None, "None did not round-trip");
		auto revised = source("object_usage_distance = .25,");
		agent_smoke::replaceSource(revised, "object_usage = 'none'", "object_usage = 'arms'"); write(revised);
		world.addLevel();
		require(world.lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::None, "Structural replay hot-reloaded None");
		for (auto const* filename : {"none.world.yaml", "none.world"})
			require(core::loadWorldDocument(context.temporaryRoot() / filename)->lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::Arms, "Reopen did not construct fresh usage");
		world.resetSimulation();
		require(world.lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::Arms, "Reset retained None instance");
		auto invalid = source(); agent_smoke::replaceSource(invalid, "object_usage = 'none'", "object_usage = 'arms'"); write(invalid);
		world.consumeSimulationEvents();
		bool refused = false; try { world.resetSimulation(); } catch (std::exception const&) { refused = true; }
		require(refused && world.lookupAgent(id).entity->getObjectUsage() == core::ObjectUsage::Arms
			&& world.consumeSimulationEvents().empty(), "Invalid fresh usable mode partially replaced None lifetime");
	}
	void sharedJourneys(smoke::Context const&)
	{
		for (unsigned kind = 0; kind < 3; ++kind)
		for (bool protectedJourney : {false, true})
		{
			bool const platform = kind == 1, shuttle = kind == 2;
			core::World world("None shared journey", shuttle ? 40 : 16, 2);
			auto ground = platform ? world.addRoom("Platform Room", 0, 0, 0, 16, 2) : world.addCorridor(0, 0, 16);
			auto upper = platform ? ground : world.addCorridor(shuttle ? 0 : 1, shuttle ? 24 : 0, 16);
			if (platform) for (unsigned x = 0; x < 16; ++x) world.addSectorWalkway(ground, 1, x);
			world.finishBuild(); world.pauseSimulation();
			auto key = world.addAccessPermission("Journey key");
			core::TraversalResourceId vehicle;
			core::InteractionPointId landing;
			uint32_t vehicleSector, object = ~0u;
			if (shuttle)
			{
				core::World::CreateShuttleOptions options{1, 4, {0, 24}, 0};
				options.capacity = 2; options.minimumDwellSeconds = 20; options.maximumBoardingSeconds = 30;
				if (protectedJourney) options.landingControlPermissionRequirements = {{key}, {}};
				auto made = world.addShuttle(1, 0, 4, 36, options);
				vehicle = made.traversalResource; vehicleSector = made.shuttle.sector->getIndex(); landing = made.doors.front().controls[0].interactionPoint;
			}
			else
			{
				core::World::CreateLiftOptions options; options.cellsWide = platform ? 1 : 2; options.capacity = 2; options.stopOffsets = {0, 1};
				options.minimumDwellSeconds = 20; options.maximumBoardingSeconds = 30; options.platformStopDurationSeconds = 20;
				if (protectedJourney) options.landingControlPermissionRequirements = {{key}, {}};
				if (platform) { auto made = world.addSectorPlatformLift(ground, 0, 8, options); vehicle = made.traversalResource; vehicleSector = ground; object = made.lift.index; landing = made.buttons.front().interactionPoint; }
				else { auto made = world.addLift(1, 0, 8, options); vehicle = made.traversalResource; vehicleSector = made.lift.sector->getIndex(); landing = made.doors.front().controls[0].interactionPoint; }
			}
			world.addSectorMarker(upper, platform ? 1 : 0, 7.f, "Goal"); world.finishBuild(); attach(world); world.pauseSimulation();
			if (protectedJourney) require(world.setLiftDestinationPermissionRequirement(vehicleSector, 1, {key}, nullptr, object), "Destination requirement refused");
			auto id = world.createAgent("Human", "Passenger", ground, 0, shuttle ? 5.5f : 8.5f);
			require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Passenger None override refused");
			auto operatorId = world.createAgent("Operator", ground, 0, 7.f);
			require(world.grantAgentAccessPermission(operatorId, key), "Operator grant refused");
			quickPlanning(world, id);
			auto origin = world.lookupAgent(id).entity->getGlobalPosition();
			auto destination = origin; if (shuttle) destination.x += 24; else destination.y = 1;
			require(!world.canAgentUseLiftJourney(vehicle, origin, destination, id), "None assumed a future self-operated journey");
			require(!world.requestInteraction(landing, id), "None called the transport");
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "None transport intent refused"); world.advanceTicks(30);
			require(lost(world, id), "None self-operated journey did not yield Route loss"); clean(world);
			core::DeviceCommand call, select;
			call.type = shuttle ? core::DeviceCommandType::CallShuttle : core::DeviceCommandType::CallLift; call.traversalResource = vehicle; call.stopIndex = 0;
			select.type = shuttle ? core::DeviceCommandType::SelectShuttleDestination : core::DeviceCommandType::SelectLiftDestination; select.traversalResource = vehicle; select.stopIndex = 1;
			auto control = world.createInteractionPoint("Independent transport control", core::SectorId{ground + 1}, world.lookupAgent(operatorId).entity->getGlobalPosition(), .25f, 0,
				{{call, core::InteractionBindingRequirement::Required}, {select, core::InteractionBindingRequirement::Required}});
			require(!world.requestInteraction(control, id), "None selected a destination");
			require(bool(world.requestInteraction(control, operatorId)), "Operator did not establish shared journey");
			require(world.moveAgentToNamedMarker(operatorId, "Goal").accepted(), "Operator journey intent refused");
			world.advanceTicks(5);
			if (protectedJourney)
			{
				require(!world.canAgentUseLiftJourney(vehicle, origin, destination, id), "None bypassed Permission adherence");
				world.pauseSimulation(); require(world.setAgentIndividualPermissionAdherence(id, false), "Opportunism refused"); world.resumeSimulation();
			}
			bool usable = false;
			for (unsigned tick = 0; tick < 1000 && !usable; ++tick) { world.advanceTick(); usable = world.canAgentUseLiftJourney(vehicle, origin, destination, id); }
			std::string diagnostic;
			for (auto const& entry : world.getSimulationSnapshot().traversalResources)
				if (entry.id == vehicle) diagnostic = " stop=" + std::to_string(entry.liftCurrentStop) + " phase=" + std::to_string((int)entry.liftStopPhase) + " moving=" + std::to_string(entry.liftMoving);
			require(usable, "None cannot observe an accepted boardable journey, kind " + std::to_string(kind) + diagnostic);
			world.consumeSimulationEvents(); require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Shared intent refused");
			bool onboard = false, arrived = false;
			for (unsigned tick = 0; tick < 7000; ++tick)
			{
				world.advanceTick();
				if (!onboard && world.isAgentTransportOccupant(vehicle, id))
				{
					onboard = true;
					world.pauseSimulation();
					require(world.setAgentIndividualPermissionAdherence(id, true), "Onboard adherence change refused");
					require(world.setAgentIndividualObjectUsage(id, std::nullopt)
						&& world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None)
						&& world.setAgentIndividualObjectUsageDistance(id, 0.f), "Onboard override edits refused");
					world.resumeSimulation();
				}
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector()->getIndex() == upper && std::abs(agent->getGlobalPosition().y - (shuttle ? 0.f : 1.f)) < .001f && agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(onboard && arrived, "None failed to finish accepted shared journey, kind " + std::to_string(kind));
			for (auto const& event : world.consumeSimulationEvents())
				require(event.type != core::SimulationEventType::InteractionRequestAdded || event.interactionRequest.actor != id, "None performed transport operation instead of sharing it");
			world.advanceTicks(120); clean(world);
		}
	}
}
void agent_smoke::registerNoneUsage(std::vector<smoke::Check>& checks)
{
	checks.push_back({"agentTypesInheritedObjectUsage", inheritedUsage});
	checks.push_back({"agentTypesInheritedObjectUsageRoutes", inheritedRoutes});
	checks.push_back({"agentTypesObjectUsageOverrides", individualOverrides});
	checks.push_back({"agentTypesObjectUsageCommittedCrossing", overrideCommittedCrossing});
	checks.push_back({"agentTypesNoneDeclarations", declarations});
	checks.push_back({"agentTypesNoneInvalidDeclarations", invalidNoneDeclarations});
	checks.push_back({"agentTypesNoneDirectRefusals", directRefusals});
	checks.push_back({"agentTypesNoneDoors", doors});
	checks.push_back({"agentTypesNoneOtherResources", otherResources});
	checks.push_back({"agentTypesNoneSharedAirlock", sharedAirlock});
	checks.push_back({"agentTypesNoneLifetimes", lifetimes});
	checks.push_back({"agentTypesNoneSharedJourneys", sharedJourneys});
}
