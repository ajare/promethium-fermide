#include "Checks.h"
#include "MobilityLifecycle.h"
#include "PoseJourneys.h"
#include "core/DoorEdge.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/AccessPanel.h"
#include "core/MobilityProfile.h"
#include "core/AgentTypeRuntime.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/WorldDocument.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "../simulation/InteractionResults.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
	using smoke::require;

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input) throw std::runtime_error("Cannot read fixture: " + path.string());
		return std::string{ std::istreambuf_iterator<char>(input), {} };
	}

	std::string serializeWorld(core::World const& world, bool binary)
	{
		std::unique_ptr<core::Serializer> writer = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return binary
			? static_cast<core::BinarySerializer*>(writer.get())->getSerializedString()
			: static_cast<core::YamlSerializer*>(writer.get())->getSerializedString();
	}

	bool loadWorld(std::string const& bytes, bool binary, core::World& target)
	{
		std::unique_ptr<core::Serializer> reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		reader->deserialize();
		core::SerializationWorkData work;
		return target.deserialize(*reader, work);
	}

	std::string typeSource(std::string const& typeId, std::string const& displayName,
		std::string const& newBody)
	{
		return "return {\n"
			"    api_version = 2,\n"
			"    type_id = \"" + typeId + "\",\n"
			"    display_name = \"" + displayName + "\",\n"
			"    new = function()\n" + newBody + "\n"
			"    end,\n"
			"}\n";
	}

	std::string validBaseline(std::string const& overrides = {})
	{
		return "        return {\n"
			"            width = 0.5,\n"
			"            standing_height = 0.6,\n"
			"            reach = 0.3,\n"
			"            walk_speed = 0.4,\n"
			"            climb_speed = 0.2,\n"
			"            stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35,\n"
			"            poses = { standing = {image_tile='agent'}, sitting = {image_tile='agent',height_ratio=0.5}, lying = {image_tile='agent',height_ratio=5/6,width_ratio=1.2}, crouching = {image_tile='agent',height_ratio=0.6}, crawling = {image_tile='agent',height_ratio=0.4} },\n"
			"            automatic_poses = { room_movement = {{pose='standing',speed_ratio=1},{pose='crouching',speed_ratio=1},{pose='crawling',speed_ratio=1}}, door_crossing = {{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=0.5}} },\n"
			"            mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' },\n" + overrides
			+ "        }\n";
	}

	std::string armsBaseline(std::string const& distance)
	{
		auto body = validBaseline();
		body.replace(body.find("reach = 0.3"), std::string("reach = 0.3").size(),
			"object_usage = 'arms', object_usage_distance = " + distance);
		return body;
	}

	void armsDeclarations(smoke::Context const&)
	{
		for (auto const& body : {validBaseline(), armsBaseline("0.125")})
		{
			core::World world("Arms declarations", 6, 2);
			auto room = world.addCorridor(0, 0, 6); world.finishBuild();
			require(world.attachAgentType("arms.agent.lua", typeSource("Arms", "Arms", body)), "Arms attach failed");
			auto id = world.createAgent("Arms", "Operator", room, 0, 1.f);
			auto* agent = world.lookupAgent(id).entity;
			auto expected = body.find("object_usage_distance") == std::string::npos ? .3f : .125f;
			require(agent->getObjectUsage() == core::ObjectUsage::Arms && agent->getObjectUsageDistance() == expected,
				"Legacy/new Arms defaults disagree");
			auto snapshot = world.getSimulationSnapshot().agents.front();
			require(snapshot.objectUsage == core::ObjectUsage::Arms && snapshot.objectUsageDistance == expected,
				"Arms and arm length are not independently observed");
		}
	}

	void armsPhysicalOperations(smoke::Context const&)
	{
		// Ordinary physical points approach when outside arm length; they do not
		// acquire remote request geometry. Geometry still narrows even long arms.
		for (auto const& [distance, geometry] : {std::pair{.125f, 1.f}, std::pair{.75f, 1.f}, std::pair{.75f, .125f}, std::pair{.75f, 0.f}})
		{
			core::World world("Physical Arms", 8, 2);
			auto room = world.addCorridor(0, 0, 8); world.finishBuild();
			require(world.attachAgentType("arms.agent.lua", typeSource("Arms", "Arms", armsBaseline(std::to_string(distance)))), "Arms attach failed");
			auto id = world.createAgent("Arms", "Operator", room, 0, 2.f);
			core::InteractionBinding binding;
			binding.command = {core::DeviceCommandType::SetSectorLights, core::SectorId{room + 1}, false};
			auto point = world.createInteractionPoint("Physical button", core::SectorId{room + 1}, {2.5f, 0.f}, geometry, 0.f, {binding});
			auto request = world.requestInteraction(point, id);
			require(bool(request), "Out-of-range ordinary physical request must approach, not be refused");
			world.advanceTicks(4);
			bool const fits = distance >= .5f && geometry >= .5f;
			require(world.getSector(room)->areLightsOn() != fits, "Physical activation ignored arm length/geometry: " + std::to_string(distance) + "/" + std::to_string(geometry));
			auto position = world.lookupAgent(id).entity->getGlobalPosition();
			require(fits ? position.x == 2.f : position.x > 2.f, "Arms range changed approach semantics");
			world.advanceTicks(300);
			require(!world.getSector(room)->areLightsOn()
				&& simulation_smoke::observedInteractionResult(world, request) == core::InteractionResult::Succeeded,
				"Physical Button did not finish through real ticks");
			position = world.lookupAgent(id).entity->getGlobalPosition();
			require(position.distanceTo({2.5f, 0.f}) <= std::min(distance, geometry), "Button pressed outside physical eligibility");
		}
		for (float distance : {.0625f, .25f, .75f})
		{
			core::World world("Arms owned controls", 8, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
			auto booth = std::static_pointer_cast<const core::BoothWindow>(world.addBoothWindow(0, 0, 3).object);
			auto placed = world.addAccessPanel(back, 0, 5);
			auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(placed.sector->getObject(placed.index))->getPanel();
			world.finishBuild();
			require(world.attachAgentType("arms.agent.lua", typeSource("Arms", "Arms", armsBaseline(std::to_string(distance)))), "Arms attach failed");
			auto point = world.lookupInteractionPoint(booth->getPanel()).entity;
			auto id = world.createAgent("Arms", "Operator", back, 0, point->getPosition().x - .125f);
			auto request = world.requestInteraction(booth->getPanel(), id);
			require(bool(request) == (distance >= .125f), "Shutter request ignored frozen arm length");
			world.advanceTicks(100);
			require(booth->getState() == (distance >= .125f ? core::Window::State::Open : core::Window::State::Closed), "Shutter physical outcome incorrect");
			auto panelRequest = world.requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, id);
			require(bool(panelRequest), "Access panel should allow physical approach");
			world.advanceTicks(600);
			require(panel->getState() == core::AccessPanel::State::Open
				&& simulation_smoke::observedInteractionResult(world, panelRequest) == core::InteractionResult::Succeeded,
				"Short-armed Access panel operator did not approach and finish");
			// Long arms never permit operating through a Layer/Location boundary.
			auto wrong = world.createAgent("Arms", "Wrong side", front, 0, 3.5f);
			require(!world.requestInteraction(booth->getPanel(), wrong), "Arm length relaxed Location eligibility");
		}
	}

	void armsManualDoor(smoke::Context const&)
	{
		for (float distance : {.0625f, .25f, .75f})
		{
			core::World world("Manual Arms Door", 8, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
			core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
			auto made = world.addSectorDoor(0, 0, 3, options);
			auto door = std::static_pointer_cast<const core::DoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
			world.addSectorMarker(back, 0, 6.f, "Goal"); world.finishBuild();
			require(world.attachAgentType("arms.agent.lua", typeSource("Arms", "Arms", armsBaseline(std::to_string(distance)))), "Arms attach failed");
			auto id = world.createAgent("Arms", "Operator", front, 0, 1.f);
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Manual Door route refused");
			bool opened = false, arrived = false;
			for (unsigned tick = 0; tick < 2400; ++tick)
			{
				auto before = world.lookupAgent(id).entity->getGlobalPosition();
				world.advanceTick();
				if (!opened && door->getState() != core::Door::State::Closed)
				{
					opened = true;
					require(std::abs(before.x - 3.5f) <= distance + .01f, "Manual Door opened beyond arm length");
				}
				auto* agent = world.lookupAgent(id).entity;
				if (agent->getSector() == world.getSector(back).get() && agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(opened && arrived, "Short-armed manual operator stalled in Door queue");
		}
	}

	void armsOnboardSelector(smoke::Context const&)
	{
		for (float distance : {.0625f, .75f})
		{
			core::World world("Arms onboard selector", 8, 4);
			auto lower = world.addCorridor(0, 0, 7);
			auto upper = world.addCorridor(2, 0, 7);
			core::World::CreateLiftOptions options; options.cellsWide = 1; options.stopOffsets = {0, 2};
			world.addLift(1, 0, 3, options);
			world.addSectorMarker(upper, 0, 5.f, "Goal"); world.finishBuild();
			require(world.attachAgentType("arms.agent.lua", typeSource("Arms", "Arms", armsBaseline(std::to_string(distance)))), "Arms attach failed");
			auto id = world.createAgent("Arms", "Passenger", lower, 0, 1.f);
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Lift intent refused");
			bool selected = false, arrived = false;
			for (unsigned tick = 0; tick < 4000; ++tick)
			{
				world.advanceTick();
				for (auto const& operation : world.getSimulationSnapshot().deviceOperations)
					if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
						&& operation.state == core::DeviceOperationState::Succeeded) selected = true;
				auto* agent = world.lookupAgent(id).entity;
				if (agent->getSector() == world.getSector(upper).get() && agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(selected && arrived, "Frozen arm length broke passenger-local selector operation");
		}
	}

	void supportedPoseRooms(smoke::Context const&)
	{
		for (bool crawlFirst : {false, true})
		{
			core::World world("Context Room journeys", 8, 2);
			auto high = world.addCorridor(0, 0, 0, 2, 1);
			auto low = world.addRoom("Low", 0, 0, 2, 4, 1);
			world.addSectorMarker(high, 0, .5f, "High goal");
			world.addSectorMarker(low, 0, 3.f, "Low goal");
			world.addSectorMarker(low, 0, 1.f, "Low waypoint");
			world.finishBuild(); world.pauseSimulation();
			require(world.setRoomHeightScale(low, .4f), "Room scale refused");
			world.removeLocationWall(low, 0, CORE_SIDE_LEFT); world.finishBuild();
			pose_journeys::attachRobot(world);
			bool rejected = false;
			try { (void)world.createAgent("Android", "Impossible", low, 0, .5f); }
			catch (std::invalid_argument const&) { rejected = true; }
			require(rejected && world.getSimulationSnapshot().agents.empty(), "Impossible placement published an Agent");
			auto robot = world.createAgent("Android", "Robot", high, 0, .5f);
			auto* robotAgent = world.lookupAgent(robot).entity;
			auto before = robotAgent->getGlobalPosition();
			rejected = false;
			try { const_cast<core::Sector*>(world.getSector(low).get())->enterAgent(robotAgent, 0, .5f); }
			catch (std::invalid_argument const&) { rejected = true; }
			require(rejected && robotAgent->getGlobalPosition() == before, "Impossible relocation mutated position");
			std::string diagnostic;
			auto roomChoices = crawlFirst
				? "{{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=0.25},{pose='crouching',speed_ratio=0.5}}"
				: "{{pose='standing',speed_ratio=1},{pose='crouching',speed_ratio=0.5},{pose='crawling',speed_ratio=0.25}}";
			require(world.attachAgentType("ordered.agent.lua", typeSource("Ordered", "Ordered", validBaseline(
				"standing_height=.45, automatic_poses={room_movement=" + std::string(roomChoices)
				+ ",door_crossing={{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=.5}}},")), &diagnostic), diagnostic);
			auto id = world.createAgent("Ordered", "Ordered", low, 0, .5f);
			auto* agent = world.lookupAgent(id).entity;
			auto expected = core::Pose::Crouching;
			require(agent->getPose() == expected, "Room did not select the tallest fitting pose");
			bool estimated = false;
			for (auto const& edge : world.getGraph()->getEdges())
				if (edge->getType() == core::EdgeType::Location
					&& edge->getVertex(0)->getSector()->getIndex() == low
					&& edge->getVertex(1)->getSector()->getIndex() == low && edge->getLength() > 0)
				{
					core::RouteDecisionContext context{agent, {}, world.getGraph()->getRouteChoicePolicy(), agent->getSector(), agent->getWalkSpeed(), &world};
					auto direct = edge->getDirectedTraversalFacts(edge->getVertex(1), context);
					auto captured = core::RouteTraversalInputs::capture(*edge, edge->getVertex(1), context).evaluate(context);
					auto seconds = edge->getLength() / (agent->getWalkSpeed() * .5f);
					require(direct.feasible && captured.feasible && std::abs(direct.components.motionSeconds - seconds) < .00001f
						&& direct.components.motionSeconds == captured.components.motionSeconds, "Room direct/captured motion ignored context ratio");
					estimated = true;
				}
			require(estimated, "Room estimate fixture lacked motion components");
			world.resumeSimulation();
			pose_journeys::refused(world, robot, "Low goal");
			require(world.moveAgentToNamedMarker(id, "Low goal").accepted(), "Room journey refused");
			bool moving = false; float moved = 0; unsigned motionTicks = 0;
			for (unsigned tick = 0; tick < 3000; ++tick)
			{
				auto x = agent->getGlobalPosition().x; world.advanceTick();
				auto delta = std::abs(agent->getGlobalPosition().x - x);
				if (delta > 0) { moving = true; moved += delta; ++motionTicks; }
				require(agent->getPose() == expected, "Room movement changed supported order");
				if (moving && agent->getState() == core::Agent::State::Idle) break;
			}
			auto ratio = .5f;
			require(moving && agent->getState() == core::Agent::State::Idle
				&& std::abs(motionTicks * world.getFixedTimestep() - moved / (agent->getWalkSpeed() * ratio)) < .04f,
				"Room context ratio disagreed with actual duration");
			require(world.moveAgentToNamedMarker(id, "High goal").accepted(), "Return journey refused");
			bool stoodAtBoundary = false;
			for (unsigned tick = 0; tick < 4000; ++tick)
			{
				world.advanceTick();
				if (agent->getGlobalPosition().x < 2.f)
				{
					require(agent->getPose() == core::Pose::Standing, "Physical Room boundary lagged logical membership");
					stoodAtBoundary = true;
				}
				if (stoodAtBoundary && agent->getState() == core::Agent::State::Idle) break;
			}
			require(stoodAtBoundary, "Return never crossed physical Room boundary");
		}
	}

	void supportedPoseBridge(smoke::Context const&)
	{
		core::World world("Room pose on Force Bridge", 8, 3);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 6, 2);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options{2, CORE_SIDE_LEFT, false, true, 0};
		world.addSectorForceBridge(room, 1, 1, options);
		world.addSectorMarker(room, 1, 3.5f, "Goal");
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("bridge-croucher.agent.lua", typeSource("BridgeCroucher", "Bridge Croucher", validBaseline(
			"standing_height=1.4, automatic_poses={room_movement={{pose='standing',speed_ratio=1},{pose='crouching',speed_ratio=.5}},door_crossing={{pose='standing',speed_ratio=1}}},")), &diagnostic), diagnostic);
		auto id = world.createAgent("BridgeCroucher", "Croucher", room, 1, .5f);
		auto* agent = world.lookupAgent(id).entity;
		require(agent->getPose() == core::Pose::Crouching, "Walkway clearance ignored Room pose");
		bool estimated = false;
		for (auto const& edge : world.getGraph()->getEdges())
			if (edge->getType() == core::EdgeType::ForceBridge)
			{
				core::RouteDecisionContext context{agent, {}, world.getRouteChoicePolicy(), agent->getSector(), agent->getWalkSpeed(), &world};
				for (auto target : {edge->getVertex(0), edge->getVertex(1)})
				{
					auto direct = edge->getDirectedTraversalFacts(target, context);
					auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
					require(direct.feasible && captured.feasible
						&& std::abs(direct.components.motionSeconds - edge->getLength() / (agent->getWalkSpeed() * .5f)) < .00001f
						&& direct.components.motionSeconds == captured.components.motionSeconds,
						"Force Bridge estimate ignored Room context speed ratio");
				}
				estimated = true;
			}
		require(estimated, "Force Bridge fixture lacks traversal");
		require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Bridge Marker intent refused");
		float moved = 0; unsigned motionTicks = 0;
		for (unsigned tick = 0; tick < 3000; ++tick)
		{
			auto x = agent->getGlobalPosition().x; world.advanceTick();
			auto delta = std::abs(agent->getGlobalPosition().x - x);
			if (delta > 0) { moved += delta; ++motionTicks; }
			require(agent->getPose() == core::Pose::Crouching, "Bridge lost Room posture");
			if (moved > 0 && agent->getState() == core::Agent::State::Idle) break;
		}
		// Each physical waypoint rounds its final movement step to a tick.
		require(moved > 0 && agent->getState() == core::Agent::State::Idle
			&& std::abs(motionTicks * world.getFixedTimestep() - moved / (agent->getWalkSpeed() * .5f)) < 4 * world.getFixedTimestep(),
			"Bridge Room pose speed disagreed with actual journey duration: moved=" + std::to_string(moved)
				+ " ticks=" + std::to_string(motionTicks) + " state=" + std::to_string(static_cast<int>(agent->getState())));
	}

	void supportedPoseDoors(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::Automatic,
			core::DoorActivationMode::RemoteControlled})
		for (bool alternate : {false, true})
		{
			core::World world("Robot Door journeys", 12, 2);
			auto a = world.addRoom("A", 0, 0, 0, 12, 1);
			auto b = world.addRoom("B", 1, 0, 0, 12, 1);
			core::World::CreateDoorOptions options;
			options.heightScale = .6f; options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			auto low = world.addSectorDoor(0, 0, 2, options);
			if (alternate) world.addSectorDoor(0, 0, 9, {});
			world.addSectorMarker(reverse ? a : b, 0, 3.5f, "Goal");
			world.finishBuild(); pose_journeys::attachRobot(world);
			auto id = world.createAgent("Android", "Robot", reverse ? b : a, 0, 1.5f);
			if (!alternate) { pose_journeys::refused(world, id, "Goal"); continue; }
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Alternative intent refused");
			bool arrived = false;
			for (unsigned tick = 0; tick < 5000; ++tick)
			{
				world.advanceTick(); auto* agent = world.lookupAgent(id).entity;
				require(agent->getPose() == core::Pose::Standing, "Alternative invented lowered robot pose");
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					require(request.resource != low.traversalResource, "Alternative admitted impossible Door");
				if (agent->getSector() == world.getSector(reverse ? a : b).get()
					&& agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(arrived, "Feasible alternative did not arrive");
		}
	}

	void supportedPoseApertures(smoke::Context const&)
	{
		for (bool bulkhead : {false, true})
		for (bool reverse : {false, true})
		for (bool broken : {false, true})
		for (float gap : {0.f, -.000005f, -.00002f})
		for (auto style : {core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenApart,
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight})
		for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::Automatic,
			core::DoorActivationMode::RemoteControlled})
		{
			bool const widthConstrained = style != core::Door::OpenStyle::OpenUp;
			core::World world("Supported aperture boundary", 8, 2);
			auto a = world.addRoom("A", 0, 0, 0, bulkhead ? 4 : 8, 1);
			auto b = world.addRoom("B", bulkhead ? 0 : 1, 0, bulkhead ? 4 : 0, bulkhead ? 4 : 8, 1);
			core::TraversalResourceId resource;
			if (bulkhead)
			{
				core::World::CreateBulkheadDoorOptions options;
				options.activationMode = mode;
				options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
				resource = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options).traversalResource;
			}
			else
			{
				core::World::CreateDoorOptions options;
				options.activationMode = mode;
				options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
				options.openStyle = style;
				options.heightScale = widthConstrained ? 1.f : (.45f * .9f + gap) / CORE_DOOR_HEIGHT;
				resource = world.addSectorDoor(0, 0, 2, options).traversalResource;
			}
			world.addSectorMarker(reverse ? a : b, 0, 2.5f, "Goal"); world.finishBuild();
			std::shared_ptr<core::Door> door;
			for (auto const& edge : world.getGraph()->getEdges())
				if (edge->getTraversalResourceId() == resource)
				{
					if (auto ordinary = std::dynamic_pointer_cast<const core::DoorEdge>(edge)) door = ordinary->getDoor();
					if (auto sameLayer = std::dynamic_pointer_cast<const core::BulkheadDoorEdge>(edge)) door = sameLayer->getDoor();
				}
			require(bool(door), "Boundary threshold missing");
			world.pauseSimulation();
			if (bulkhead) static_cast<core::Shape&>(*door) = core::Shape(door->getPosition(), {door->getSize().x, .45f * .9f + gap});
			if (broken)
			{
				require(world.setDoorBroken(resource, true), "Boundary Broken mode refused");
				auto fraction = widthConstrained && !bulkhead ? (.4f + gap) / door->getSize().x : 1.f;
				require(world.setDoorBrokenOpenPercentage(resource, fraction), "Boundary aperture refused");
			}
			pose_journeys::attachRobot(world);
			auto id = world.createAgent("Android", "Robot", reverse ? b : a, 0, bulkhead ? (reverse ? .5f : 3.5f) : 2.5f);
			require(world.setAgentIndividualHeightModifier(id, .9f), "Effective Height modifier refused");
			world.resumeSimulation();
			bool fits = gap >= -.00001f || (!bulkhead && widthConstrained && !broken);
			if (!fits) { pose_journeys::refused(world, id, "Goal"); continue; }
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Exact-fit intent refused");
			bool arrived = false;
			for (unsigned tick = 0; tick < 2500; ++tick)
			{
				world.advanceTick(); auto* agent = world.lookupAgent(id).entity;
				require(agent->getPose() == core::Pose::Standing, "Exact-fit robot lowered posture");
				if (agent->getSector() == world.getSector(reverse ? a : b).get() && agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(arrived, "Exact/tolerance fit was planned but not admissible");
		}
	}

	void supportedCrouchingDoor(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		for (bool broken : {false, true})
		for (bool lowered : {false, true})
		{
			core::World world("Crouching Door context", 8, 2);
			auto a = world.addRoom("A", 0, 0, 0, 8, 1), b = world.addRoom("B", 1, 0, 0, 8, 1);
			core::World::CreateDoorOptions options;
			options.heightScale = broken || !lowered ? 1.f : .6f;
			auto made = world.addSectorDoor(0, 0, 2, options);
			world.addSectorMarker(reverse ? a : b, 0, 2.5f, "Goal"); world.finishBuild();
			auto door = std::static_pointer_cast<const core::DoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
			world.pauseSimulation();
			if (broken) { require(world.setDoorBroken(made.traversalResource, true), "Broken mode refused"); require(world.setDoorBrokenOpenPercentage(made.traversalResource, lowered ? .6f : 1.f), "Broken aperture refused"); }
			std::string diagnostic;
			require(world.attachAgentType("crouching.agent.lua", typeSource("Croucher", "Croucher", validBaseline(
				"standing_height=.45, automatic_poses={room_movement={{pose='standing',speed_ratio=1}},door_crossing={{pose='standing',speed_ratio=.8},{pose='crawling',speed_ratio=.5},{pose='crouching',speed_ratio=.25}}},")), &diagnostic), diagnostic);
			auto id = world.createAgent("Croucher", "Croucher", reverse ? b : a, 0, 2.5f);
			auto* agent = world.lookupAgent(id).entity;
			for (auto const& edge : world.getGraph()->getEdges())
				if (edge->getTraversalResourceId() == made.traversalResource)
				{
					auto target = edge->getVertex(0)->getSector()->getIndex() == (reverse ? a : b) ? edge->getVertex(0) : edge->getVertex(1);
					core::RouteDecisionContext context{agent, {}, world.getGraph()->getRouteChoicePolicy(), agent->getSector(), agent->getWalkSpeed(), &world};
					auto direct = edge->getDirectedTraversalFacts(target, context);
					auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
					require(direct.feasible && captured.feasible && std::abs(direct.components.motionSeconds - (lowered ? .4f : 8.f / 60.f)) < .00001f
						&& direct.components.motionSeconds == captured.components.motionSeconds, "Crouching captured/direct duration disagreed: direct=" + std::to_string(direct.components.motionSeconds)
						+ " captured=" + std::to_string(captured.components.motionSeconds) + " broken=" + std::to_string(broken));
				}
			world.resumeSimulation(); require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Crouching intent refused");
			unsigned crossingTicks = 0; bool arrived = false;
			for (unsigned tick = 0; tick < 3000; ++tick)
			{
				world.advanceTick();
				if (agent->getState() == core::Agent::State::TraversingEdge
					&& !world.getSimulationSnapshot().traversalPermits.empty())
				{
					require(agent->getPose() == (lowered ? core::Pose::Crouching : core::Pose::Standing), "Door selected wrong supported pose");
					++crossingTicks;
				}
				else require(agent->getPose() == core::Pose::Standing, "Waiting or recovery retained crossing pose");
				if (agent->getSector() == world.getSector(reverse ? a : b).get() && agent->getState() == core::Agent::State::Idle) { arrived = true; break; }
			}
			require(arrived && crossingTicks == (lowered ? 24u : 8u), "Crossing did not use context pose/tick-quantized duration");
		}
	}

	void scriptedHumanIdentity(smoke::Context const&)
	{
		core::World world("Scripted Human", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		require(world.hasAgentType("Human"), "The bundled Human type was not registered");
		require(world.agentTypeDisplayName("Human") == "Human",
			"The bundled Human type lost its display name");
		auto const id = world.createAgent("Created human", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && std::string(agent->getTypeName()) == "Human"
			&& agent->getTypeId() == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"Scripted Human identity did not resolve");
		auto const& physical = agent->getPhysicalBaseline();
		require(physical.width == 0.4f && physical.standingHeight == 0.45f
			&& physical.objectUsageDistance == 0.25f && physical.walkSpeed == 0.5f
			&& physical.climbSpeed == 0.25f && physical.stairAscentSpeed == 0.35f
			&& physical.stairDescentSpeed == 0.45f && physical.poses.at(core::Pose::Sitting).heightRatio == 0.6f
			&& physical.poses.at(core::Pose::Crouching).heightRatio == 0.6f && physical.poses.at(core::Pose::Crawling).heightRatio == 0.3f
			&& physical.automaticSpeedRatio(core::AutomaticPoseContext::DoorCrossing, core::Pose::Crawling).value() == 0.5f,
			"Scripted Human baseline did not match the bundled definition");
	}

	void bundledDefinitionMatchesResource(smoke::Context const& context)
	{
		auto const resource = readFile(context.fixture("resources/test-worlds/human.agent.lua"));
		require(resource == core::bundledHumanAgentType().source,
			"The bundled Human resource drifted from the embedded definition");
		auto const definition = core::bundledHumanAgentType();
		auto preflight = core::AgentTypeRuntimeAdapter::preflightType(
			definition.resourceName, definition.source);
		require(preflight.loaded && preflight.typeId == "Human"
			&& preflight.displayName == "Human",
			"The bundled Human definition did not preflight");
	}

	void genericScriptBackedType(smoke::Context const&)
	{
		core::World world("Generic type", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()), &diagnostic),
			diagnostic.c_str());
		require(world.hasAgentType("Tall")
			&& world.agentTypeDisplayName("Tall") == "Tall Person",
			"The attached type identity did not resolve");
		auto const id = world.createAgent("Tall", "Tall one", corridor, 0, 2.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Tall"
			&& std::string(agent->getTypeName()) == "Tall Person"
			&& agent->getTypeResourceName() == "tall.agent.lua",
			"The generic script-backed Agent identity did not round-trip");
		auto const& physical = agent->getPhysicalBaseline();
		require(physical.width == 0.5f && physical.standingHeight == 0.6f
			&& physical.objectUsageDistance == 0.3f && physical.walkSpeed == 0.4f,
			"The generic type's frozen baseline was not applied");
		// The former Human-only query factory now accepts any resolved resource,
		// retains an isolated instance, and does not publish to the World.
		core::AgentTypeDefinition definition{ "Tall", "Tall Person", "tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()) };
		auto query = core::Agent::create(definition, "Query");
		require(query->getTypeId() == "Tall" && query->getWidth() == agent->getWidth()
			&& query->getWalkSpeed() == agent->getWalkSpeed()
			&& world.getSimulationSnapshot().agents.size() == 1,
			"Resource-backed query factory refused an arbitrary type or published an Agent");
		require(world.attachAgentType("unit.agent.lua", typeSource("Unit", "Unit",
			validBaseline("poses={standing={image_tile='agent'},sitting={image_tile='agent',height_ratio=1},crouching={image_tile='agent',height_ratio=.8},crawling={image_tile='agent',height_ratio=.4}},\n"
				"automatic_poses={room_movement={{pose='standing',speed_ratio=1}},door_crossing={{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=1}}},\n")), &diagnostic), diagnostic);
		auto unit = world.lookupAgent(world.createAgent("Unit", "Unit", corridor, 0, 4.f)).entity;
		require(unit->getPhysicalBaseline().automaticSpeedRatio(core::AutomaticPoseContext::DoorCrossing, core::Pose::Crawling).value() == 1.f
			&& unit->getPhysicalBaseline().poses.at(core::Pose::Sitting).heightRatio == 1.f
			&& unit->getTraversalCrawlingDoorClearanceExtent(true) == unit->getStandingHeight() * .4f,
			"The inclusive Sitting height/speed ratio upper boundary was refused or changed");
	}

	void reservedBehaviourMember(smoke::Context const&)
	{
		for (auto const* value : { "function() end", "false", "0", "'private'", "{}" })
		{
			auto const source = typeSource("Reserved", "Reserved",
				validBaseline(std::string("behaviour = ") + value + ",\n"));
			auto const preflight = core::AgentTypeRuntimeAdapter::preflightType(
				"reserved.agent.lua", source);
			require(!preflight.loaded
				&& preflight.failure == core::ScriptExecutionFailure::ConversionError
				&& preflight.diagnostic.find("'behaviour'") != std::string::npos
				&& preflight.diagnostic.find("reserved") != std::string::npos
				&& preflight.diagnostic.find("runtime-owned") != std::string::npos,
				"Reserved instance member passed preflight or lacked a clear diagnostic");
			core::AgentTypeRuntimeAdapter runtime;
			auto const constructed = runtime.construct("Reserved", source, "Reserved");
			require(!constructed.succeeded && !constructed.instance
				&& constructed.failure == preflight.failure
				&& constructed.diagnostic == preflight.diagnostic,
				"Direct construction bypassed the reserved-member contract");
			core::World world("Reserved member", 4, 2);
			std::string diagnostic;
			require(!world.attachAgentType("reserved.agent.lua", source, &diagnostic)
				&& !world.hasAgentType("Reserved")
				&& world.getSimulationSnapshot().agents.empty()
				&& diagnostic.find("behaviour") != std::string::npos,
				"Reserved-member attachment partially published an Agent type");
			// A failed probe/construction must not poison subsequent valid input.
			auto const valid = typeSource("Valid", "Valid", validBaseline(
				"behaviour = nil, private_behaviour = function() end,\n"));
			require(core::AgentTypeRuntimeAdapter::preflightType("valid.agent.lua", valid).loaded
				&& runtime.construct("Valid", valid, "Valid").succeeded,
				"API v2 private data or an absent behaviour member was refused");
		}
	}

	void invalidBaselinesAreRejected(smoke::Context const&)
	{
		// Sources refused at preflight (attach) time: the type object itself is
		// malformed before any instance is constructed.
		struct AttachCase { std::string name; std::string source; };
		std::vector<AttachCase> attachCases;
		attachCases.push_back({ "malformed Lua", "this is not valid lua" });
		attachCases.push_back({ "missing constructor",
			"return { api_version = 2, type_id = \"NoCtor\", display_name = \"No Ctor\" }\n" });
		attachCases.push_back({ "invalid constructor",
			"return { api_version = 2, type_id = 'NoCtor', display_name = 'No Ctor', new = 42 }" });
		attachCases.push_back({ "module exception", "error('module refused')" });
		attachCases.push_back({ "invalid type id",
			"return { api_version = 2, type_id = \"bad id!\", display_name = \"Bad\", new = function() return {} end }\n" });
		for (auto const& test : attachCases)
		{
			core::World world("Reject", 4, 2);
			std::string diagnostic;
			require(!world.attachAgentType(test.name + ".agent.lua", test.source, &diagnostic)
				&& !diagnostic.empty(),
				("Invalid Agent type was accepted or undiagnosed at attach: " + test.name).c_str());
		}

		// Sources with a valid type object whose new() returns an invalid
		// baseline: attachment succeeds, but creation must refuse atomically.
		struct ConstructCase { std::string name; std::string body; std::string expected; };
		std::vector<ConstructCase> constructCases;
		// Every field is tested, and every failure names the offending field.
		for (auto const* field : { "width", "standing_height", "reach", "walk_speed",
			"climb_speed", "stair_ascent_speed", "stair_descent_speed" })
		{
			for (auto const* value : { "nil", "0", "-1", "0/0", "math.huge",
				"-math.huge", "false", "'0.5'", "{}", "1e-300" })
				constructCases.push_back({ std::string(field) + " = " + value,
					validBaseline(std::string(field) + " = " + value + ",\n"),
					std::string(field) == "reach" && std::string(value) == "nil" ? "object_usage_distance" : field });
			constructCases.push_back({ std::string(field) + " overflow",
				validBaseline(std::string(field) + " = 1e300,\n"), field });
			if (std::string_view(field).ends_with("ratio"))
				constructCases.push_back({ std::string(field) + " above one",
					validBaseline(std::string(field) + " = 1.01,\n"), field });
		}
		for (auto const* value : { "nil", "0", "-1", "0/0", "math.huge", "-math.huge", "false", "'0.5'", "{}", "1e-300", "1e300" })
			constructCases.push_back({std::string("Arms distance ") + value, armsBaseline(value), "object_usage_distance"});
		for (auto const* value : {"'remote_control'", "'None'", "'Arms'", "'arms\\0suffix'", "''", "false", "42", "{}"})
		{
			auto body = armsBaseline(".25");
			body.replace(body.find("object_usage = 'arms'"), std::string("object_usage = 'arms'").size(), std::string("object_usage = ") + value);
			constructCases.push_back({std::string("Invalid usage ") + value, body, "object_usage"});
		}
		for (auto const* value : {".25", ".75", "false"})
		{
			auto body = armsBaseline(".25");
			body.insert(body.find("width ="), std::string("reach = ") + value + ", ");
			constructCases.push_back({"Competing distance authorities", body, "mutually exclusive"});
		}
		for (auto const* value : { "42", "nil", "false", "function() end", "'instance'" })
			constructCases.push_back({ std::string("non-table ") + value,
				std::string("return ") + value, "instance table" });
		constructCases.push_back({ "constructor exception", "error('constructor refused')",
			"constructor refused" });
		for (auto const* capability : { "io.open('unsafe')", "os.execute('unsafe')",
			"debug.getregistry()", "package.loadlib('unsafe', 'unsafe')",
			"require('unsafe')", "dofile('unsafe')", "loadfile('unsafe')",
			"load('unsafe')", "setmetatable({}, {})", "collectgarbage()" })
			constructCases.push_back({ capability,
				std::string("local forbidden = ") + capability + "\n" + validBaseline(), "" });
		for (auto const& test : constructCases)
		{
			core::World world("Reject", 6, 2);
			auto const corridor = world.addCorridor(0, 0, 6);
			world.finishBuild();
			std::string diagnostic;
			require(world.attachAgentType("invalid.agent.lua",
				typeSource("Invalid", "Invalid", test.body), &diagnostic),
				("Valid type object was refused at attach: " + test.name + " (" + diagnostic + ")").c_str());
			auto const authoredBefore = serializeWorld(world, false);
			world.consumeSimulationEvents();
			bool threw = false;
			try
			{
				(void)world.createAgent("Invalid", "Broken", corridor, 0, 1.0f);
			}
			catch (std::exception const& error)
			{
				diagnostic = error.what();
				threw = diagnostic.find("invalid.agent.lua") != std::string::npos
					&& diagnostic.find(test.expected) != std::string::npos;
			}
			require(threw, ("Invalid construction was not diagnosed: " + test.name + ": " + diagnostic).c_str());
			require(world.getSimulationSnapshot().agents.empty()
				&& world.getSector(corridor)->getAgents().empty(),
				("Invalid baseline left a partial Agent: " + test.name).c_str());
			require(world.consumeSimulationEvents().empty() && serializeWorld(world, false) == authoredBefore,
				"Invalid baseline published events or mutated the document");
		}
	}

	void poseDeclarations(smoke::Context const& context)
	{
		auto const robot = readFile(context.fixture("resources/test-worlds/android.agent.lua"));
		core::World world("Pose declarations", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("android.agent.lua", robot, &diagnostic), diagnostic);
		auto const id = world.createAgent("Android", "Robot", corridor, 0, 1.f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->supportsPose(core::Pose::Standing)
			&& agent->getPoseEnvelope(core::Pose::Standing)->y == 0.45f,
			"Standing-only resource did not construct its canonical envelope");
		for (auto pose : { core::Pose::Sitting, core::Pose::Lying, core::Pose::Crouching, core::Pose::Crawling })
			require(!agent->supportsPose(pose) && !agent->getPoseEnvelope(pose),
				"A Standing-only type acquired an unsupported envelope");
		world.pauseSimulation();
		require(world.setAgentIndividualHeightModifier(id, 0.8f), "Could not modify Robot Height");
		require(agent->getPoseEnvelope(core::Pose::Standing)->y == 0.45f * 0.8f
			&& !agent->getPoseEnvelope(core::Pose::Crawling), "Height changed capabilities");
		for (bool binary : { false, true })
		{
			core::World restored("Robot roundtrip", 2, 2);
			require(loadWorld(serializeWorld(world, binary), binary, restored), "Robot document failed to load");
			auto const* loaded = restored.lookupAgent(id).entity;
			require(loaded && loaded->getTypeId() == "Android"
				&& loaded->getTypeResourceName() == "android.agent.lua"
				&& loaded->getPoseImageTile() == "robot-standing"
				&& loaded->getPhysicalBaseline().poses == agent->getPhysicalBaseline().poses
				&& !loaded->supportsPose(core::Pose::Crawling), "Robot capability/identity roundtrip changed");
		}
		auto const yaml = serializeWorld(world, false);
		require(yaml.find("automatic_poses") == std::string::npos && yaml.find("poses:") == std::string::npos
			&& yaml.find("image_tile") == std::string::npos,
			"Document persisted frozen pose definitions");

		// Supporting a pose does not require automatic selection in either list.
		auto source = typeSource("Explicit", "Explicit", validBaseline(
			"automatic_poses={room_movement={{pose='standing',speed_ratio=0.8}},door_crossing={{pose='standing',speed_ratio=0.9}}},\n"));
		require(world.attachAgentType("explicit.agent.lua", source, &diagnostic), diagnostic);
		auto const* explicitAgent = world.lookupAgent(world.createAgent("Explicit", "Explicit", corridor, 0, 3.f)).entity;
		require(explicitAgent->supportsPose(core::Pose::Crawling)
			&& !explicitAgent->getPhysicalBaseline().automaticSpeedRatio(core::AutomaticPoseContext::DoorCrossing, core::Pose::Crawling)
			&& explicitAgent->getPoseEnvelope(core::Pose::Crawling)->y == 0.6f * 0.4f
			&& explicitAgent->getPoseEnvelope(core::Pose::Lying)->y == 0.5f,
			"Supported-but-not-automatic poses lost their canonical envelopes");
		auto const& human = core::bundledHumanBaseline();
		for (auto const& [pose, definition] : human.poses)
		{
			require(definition.imageTile == std::string("human-") + core::poseName(pose),
				"Bundled Human pose lacks dedicated artwork");
		}
		require(human.poses.at(core::Pose::Lying).heightRatio == static_cast<float>(26.0 / 72.0)
			&& human.poses.at(core::Pose::Lying).widthRatio == static_cast<float>(72.0 / 26.0)
			&& human.poses.at(core::Pose::Standing).widthRatio == 1.f,
			"Human Lying ratios disagree with its baked artwork");
		require(human.roomMovement == std::vector<core::AutomaticPoseChoice>{
			{core::Pose::Standing, 1.f}, {core::Pose::Crouching, 1.f}, {core::Pose::Crawling, 1.f}}
			&& human.doorCrossing == std::vector<core::AutomaticPoseChoice>{
			{core::Pose::Standing, 1.f}, {core::Pose::Crouching, 1.f}, {core::Pose::Crawling, 0.5f}}, "Human context choices/speeds changed");

		// Script ordering never overrides tallest-fit selection, even if Crawling
		// is faster. Both contexts share fit but keep their own speed ratios.
		auto query = core::Agent::create(core::AgentTypeDefinition{"Tallest", "Tallest", "tallest.agent.lua",
			typeSource("Tallest", "Tallest", validBaseline(
				"automatic_poses={room_movement={{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=.75},{pose='crouching',speed_ratio=.25}},door_crossing={{pose='standing',speed_ratio=.9},{pose='crawling',speed_ratio=.5},{pose='crouching',speed_ratio=.3}}},"))}, "Query");
		require(query->poseFits(core::Pose::Lying, .51f, 0.f, .61f)
			&& !query->poseFits(core::Pose::Lying, .49f, 0.f, .61f)
			&& !query->poseFits(core::Pose::Lying, .51f, 0.f, .59f),
			"Lying fit ignored declared height or width ratios");
		for (auto context : {core::AutomaticPoseContext::RoomMovement, core::AutomaticPoseContext::DoorCrossing})
		{
			for (auto const& [clearance, pose] : std::vector<std::pair<float, core::Pose>>{
				{.6f, core::Pose::Standing}, {.4f, core::Pose::Crouching}, {.3f, core::Pose::Crawling}})
			{
				auto selected = query->selectAutomaticPose(context, clearance);
				require(selected && selected->pose == pose
					&& selected->speedRatio == query->getPhysicalBaseline().automaticSpeedRatio(context, pose),
					"Movement context did not select the tallest fitting allowed pose and its speed");
			}
			require(!query->selectAutomaticPose(context, .2f)
				&& !query->selectAutomaticPose(context, .6f, 0.f, .4f)
				&& !explicitAgent->selectAutomaticPose(context, .3f),
				"No-fit or context-excluded pose was admitted");
			auto supported = query->selectAutomaticPose(context, .4f, .1f);
			require(supported && supported->pose == core::Pose::Crawling, "Tallest selection ignored support elevation");
		}
	}

	void invalidPoseDeclarations(smoke::Context const&)
	{
		std::vector<std::pair<std::string, std::string>> cases{
			{"poses=nil,", "poses"}, {"poses=42,", "poses"},
			{"poses={},", "poses.standing"}, {"poses={standing=false},", "poses.standing"},
			{"poses={standing={image_tile='agent'},sitting=42},", "poses.sitting"},
			{"poses={standing={image_tile='agent'},flying={}},", "poses.flying"},
			{"poses={standing={image_tile='agent'},[1]={}},", "poses"},
			{"poses={standing={image_tile='agent',height_ratio=1}},", "poses.standing.height_ratio"},
			{"poses={standing={image_tile='agent'},lying={image_tile='agent',height_ratio=1.01}},", "poses.lying.height_ratio"},
			{"poses={standing={image_tile='agent'},lying={image_tile='agent'}},", "poses.lying.height_ratio"},
			{"poses={standing={image_tile='agent'},sitting={image_tile='agent',ratio=0.5}},", "poses.sitting.ratio"},
			{"poses={standing={}},", "poses.standing.image_tile"},
			{"poses={standing={image_tile='agent'},lying={height_ratio=.5}},", "poses.lying.image_tile"},
			{"poses={standing={image_tile='agent'},crouching={height_ratio=.6}},", "poses.crouching.image_tile"},
			{"automatic_poses=nil,", "automatic_poses"},
			{"automatic_poses={room_movement={},door_crossing={}},", "room_movement"},
			{"automatic_poses={unknown={}},", "automatic_poses.unknown"},
			{"sitting_height_ratio=0.5,", "sitting_height_ratio"},
			{"poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=1}},", "poses.crouching.height_ratio"},
			{"poses={standing={image_tile='agent'},crawling={image_tile='agent',height_ratio=1}},", "poses.crawling.height_ratio"},
			{"poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=.6},crawling={image_tile='agent',height_ratio=.6}},", "poses.crawling.height_ratio"},
			{"poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=.6},crawling={image_tile='agent',height_ratio=.7}},", "poses.crawling.height_ratio"},
			{"poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=.6},crawling={image_tile='agent',height_ratio=.600000001}},", "poses.crawling.height_ratio"},
			{"poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=.999999999}},", "poses.crouching.height_ratio"},
			{"poses={standing={image_tile='agent'},crawling={image_tile='agent',height_ratio=.999999999}},", "poses.crawling.height_ratio"},
		};
		for (auto pose : { "sitting", "lying", "crouching", "crawling" })
			for (auto value : { "nil", "'0.5'", "0/0", "math.huge", "-math.huge", "0", "-1", "1.01", "1e-300", "1e300", "false", "{}" })
				cases.emplace_back(std::string("poses={standing={image_tile='agent'},") + pose + "={image_tile='agent',height_ratio=" + value + "}},", std::string("poses.") + pose + ".height_ratio");
		for (auto value : {"false", "'1.2'", "{}", "0/0", "math.huge", "-math.huge", "0", "-1", "1e-300", "1e300"})
			cases.emplace_back(std::string("poses={standing={image_tile='agent'},lying={image_tile='agent',height_ratio=.5,width_ratio=")
				+ value + "}},", "poses.lying.width_ratio");
		cases.emplace_back("width=1e30,poses={standing={image_tile='agent'},lying={image_tile='agent',height_ratio=.5,width_ratio=1e30}},",
			"poses.lying.width_ratio");
		cases.emplace_back("width=1e-30,poses={standing={image_tile='agent'},lying={image_tile='agent',height_ratio=.5,width_ratio=1e-30}},",
			"poses.lying.width_ratio");
		for (auto value : {"nil", "false", "42", "{}", "''", "string.rep('x',129)", "'bad\\0tile'", "'bad\\ntile'"})
			cases.emplace_back(std::string("poses={standing={image_tile=") + value + "}},", "poses.standing.image_tile");
		for (auto key : { "room_movement", "door_crossing" })
		{
			auto contract = [&](std::string const& list) {
				return std::string("automatic_poses={room_movement={{pose='standing',speed_ratio=1}},door_crossing={{pose='standing',speed_ratio=1}},") + key + "=" + list + "},";
			};
			for (auto list : { "nil", "false", "{}", "{[2]={pose='standing',speed_ratio=1}}",
				"{[1]={pose='standing',speed_ratio=1},[3]={pose='crawling',speed_ratio=1}}",
				"{foo={pose='standing',speed_ratio=1}}", "{{pose='standing',speed_ratio=1},{pose='standing',speed_ratio=1}}",
				"{{pose='crawling',speed_ratio=1}}", "{{pose='standing',speed_ratio=1},{pose='sitting',speed_ratio=1}}",
				"{{pose='standing',speed_ratio=1},{pose='flying',speed_ratio=1}}",
				"{{pose='standing',speed_ratio=1},{pose='lying',speed_ratio=1}}",
				"{{pose='standing',speed_ratio=1},false}",
				"{{speed_ratio=1}}", "{{pose=7,speed_ratio=1}}", "{{pose='standing',speed_ratio=1,extra=1}}" })
				cases.emplace_back(contract(list), key);
			for (auto value : { "nil", "'0.5'", "0/0", "math.huge", "-math.huge", "0", "-1", "1.01", "1e-300", "1e300", "false", "{}" })
				cases.emplace_back(contract(std::string("{{pose='standing',speed_ratio=") + value + "}}"), std::string(key) + "[1].speed_ratio");
			cases.emplace_back("poses={standing={image_tile='agent'}}," + contract("{{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=1}}"), key);
		}
		for (auto const& [overrides, expected] : cases)
		{
			core::World world("Invalid pose", 4, 2);
			auto const corridor = world.addCorridor(0, 0, 4);
			world.finishBuild();
			auto const source = typeSource("Invalid", "Invalid", validBaseline(overrides));
			std::string diagnostic;
			require(world.attachAgentType("invalid-pose.agent.lua", source, &diagnostic), diagnostic);
			// Production preview and placement use independent constructors with
			// the same refusal contract, without partially publishing an Agent.
			core::AgentTypeDefinition definition{"Invalid", "Invalid", "invalid-pose.agent.lua", source};
			{
				struct LoaderReset { ~LoaderReset() { core::setAgentTypeResourceLoader({}); } } reset;
				core::setAgentTypeResourceLoader([definition](std::string const& name) -> std::optional<core::AgentTypeDefinition> {
					if (name == definition.resourceName) return definition;
					return std::nullopt;
				});
				require(!core::agentTypeResourcePreview(definition.resourceName, diagnostic)
					&& diagnostic.find(definition.resourceName) != std::string::npos
					&& diagnostic.find(expected) != std::string::npos,
					"Malformed pose preview was accepted or undiagnosed: " + overrides + " " + diagnostic);
			}
			auto const before = serializeWorld(world, false);
			bool refused = false;
			try { world.createAgent("Invalid", "Invalid", corridor, 0, 1.f); }
			catch (std::exception const& error) { diagnostic = error.what(); refused = true; }
			require(refused && diagnostic.find("invalid-pose.agent.lua") != std::string::npos
				&& diagnostic.find(expected) != std::string::npos,
				"Pose declaration was accepted or poorly diagnosed: " + overrides + " " + diagnostic);
			require(world.getSimulationSnapshot().agents.empty() && world.getSector(corridor)->getAgents().empty()
				&& serializeWorld(world, false) == before, "Malformed poses partially mutated World");
		}
		for (auto version : { "1", "3", "nil", "'2'", "2.5" })
		{
			auto source = typeSource("Version", "Version", validBaseline());
			agent_smoke::replaceSource(source, "api_version = 2", std::string("api_version = ") + version);
			core::World world("Version refusal", 4, 2);
			std::string diagnostic;
			require(!world.attachAgentType("version.agent.lua", source, &diagnostic)
				&& !world.hasAgentType("Version") && diagnostic.find("api_version = 2") != std::string::npos
				&& diagnostic.find("migrate") != std::string::npos && diagnostic.find("automatic_poses") != std::string::npos,
				"Version refusal lacked explicit v1 migration guidance");
			core::AgentTypeRuntimeAdapter runtime;
			require(!runtime.construct("Version", source, "Version").succeeded,
				"Direct construction bypassed API version validation");
		}
	}

	void scriptedMobilityProfiles(smoke::Context const&)
	{
		core::World world("Script defaults", 6, 2);
		auto const corridor = world.addCorridor(0, 0, 6);
		world.addCorridor(1, 0, 0, 6, 1);
		auto const door = world.addSectorDoor(0, 0, 2);
		world.finishBuild();
		std::string diagnostic;
		std::string scriptProfile = "{ staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'cannot_use', buttons = 'only_if_no_other_option' }";
		require(world.attachAgentType("limited.agent.lua", typeSource("Limited", "Limited",
			validBaseline("mobility_profile = " + scriptProfile + ",\n")), &diagnostic), diagnostic);
		core::AgentId id;
		try { id = world.createAgent("Limited", "Limited", corridor, 0, 1.f); }
		catch (std::exception const& error) { require(false, error.what()); }
		auto* agent = world.lookupAgent(id).entity;
		require(agent && agent->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door)
			== core::MobilityUse::CannotUse && agent->getEffectiveMobilityProfile().value
			== agent->getScriptDefaultMobilityProfile(),
			"A script Mobility default was not frozen or used without a tag registry");
		agent_smoke::requireDoorRoute(world, id, 1, false);
		world.pauseSimulation();
		core::MobilityProfile individual;
		individual.set(core::TraversalKind::Lift, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualMobilityProfile(id, individual, &diagnostic)
			&& agent->getEffectiveMobilityProfile().value == individual, diagnostic);
		agent_smoke::requireDoorRoute(world, id, 1, true);
		require(world.setAgentIndividualMobilityProfile(id, std::nullopt, &diagnostic)
			&& agent->getEffectiveMobilityProfile().value == agent->getScriptDefaultMobilityProfile(), diagnostic);
		agent_smoke::requireDoorRoute(world, id, 1, false);

		// Use the same authored passage with a real remote control to prove
		// script-default Buttons composition and the last-resort second pass.
		world.addSectorDoorButton(door.door.sector->getIndex(), door.door.index);
		auto fallbackProfile = scriptProfile;
		agent_smoke::replaceSource(fallbackProfile, "door = 'cannot_use'", "door = 'can_use'");
		require(world.attachAgentType("fallback.agent.lua", typeSource("Fallback", "Fallback",
			validBaseline("mobility_profile = " + fallbackProfile + ",\n")), &diagnostic), diagnostic);
		auto fallback = world.createAgent("Fallback", "Fallback", corridor, 0, 3.f);
		agent_smoke::requireDoorRoute(world, fallback, 1, true);
		auto forbiddenProfile = fallbackProfile;
		agent_smoke::replaceSource(forbiddenProfile, "buttons = 'only_if_no_other_option'", "buttons = 'cannot_use'");
		require(world.attachAgentType("no-buttons.agent.lua", typeSource("NoButtons", "No Buttons",
			validBaseline("mobility_profile = " + forbiddenProfile + ",\n")), &diagnostic), diagnostic);
		auto noButtons = world.createAgent("NoButtons", "No Buttons", corridor, 0, 4.f);
		agent_smoke::requireDoorRoute(world, noButtons, 1, false);

		uint32_t invalidIndex = 0;
		for (auto const& invalid : std::vector<std::tuple<std::string, std::string, std::string>>{
			{ "missing", "nil", "mobility_profile" }, { "malformed", "42", "mobility_profile" },
			{ "omitted", "{ staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', buttons = 'can_use' }", "door" },
			{ "unknown", "{ staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use', jetpack = 'can_use' }", "jetpack" },
			{ "invalid-use", "{ staircase = 'bad', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' }", "staircase" } })
		{
			auto const& [name, profile, field] = invalid;
			auto const typeId = "Bad" + std::to_string(++invalidIndex);
			core::World rejected("Reject profile", 4, 2);
			auto const location = rejected.addCorridor(0, 0, 4);
			rejected.finishBuild();
			require(rejected.attachAgentType(name + ".agent.lua", typeSource(typeId,
				"Bad", validBaseline("mobility_profile = " + profile + ",\n")), &diagnostic), diagnostic);
			bool threw = false;
			try { (void)rejected.createAgent(typeId, "Bad", location, 0, 1.f); }
			catch (std::exception const& error)
			{
				auto const message = std::string(error.what());
				threw = message.find(name + ".agent.lua") != std::string::npos
					&& message.find(field) != std::string::npos;
			}
			require(threw && rejected.getSimulationSnapshot().agents.empty(),
				("Invalid script Mobility profile was published or lacked resource/field diagnostics: " + name).c_str());
		}
	}

	void constructorFailureLeavesNoPartialAgent(smoke::Context const&)
	{
		core::World world("Constructor failure", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("boom.agent.lua",
			typeSource("Boom", "Boom", "        error(\"constructor failed\")\n"),
			&diagnostic), diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Boom", "Broken", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "A throwing constructor published an Agent");
		require(world.getSimulationSnapshot().agents.empty()
			&& world.getSector(corridor)->getAgents().empty(),
			"A throwing constructor left a partial Agent");
	}

	void executionBudgetIsEnforced(smoke::Context const&)
	{
		core::World world("Budget", 4, 2, {},
			core::AgentTypeRuntimeLimits{ 64u * 1024u * 1024u, 2000u });
		auto const corridor = world.addCorridor(0, 0, 4);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("loop.agent.lua",
			typeSource("Loop", "Loop", "        while true do end\n"), &diagnostic),
			diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Loop", "Spinner", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "An unbounded constructor was not stopped by the instruction budget");
		require(world.getSimulationSnapshot().agents.empty(),
			"A budgeted failure left a partial Agent");
		for (auto const* guard : { "pcall", "xpcall" })
		{
			auto const body = std::string(guard) + "(function() while true do end end"
				+ (std::string_view(guard) == "xpcall" ? ", function(e) return e end" : "")
				+ ")\n" + validBaseline();
			require(world.attachAgentType(std::string(guard) + ".agent.lua",
				typeSource(guard, guard, body), &diagnostic), diagnostic);
			bool refused = false;
			try { world.createAgent(guard, "Caught budget", corridor, 0, 1.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.empty(),
				"A protected call swallowed terminal instruction-budget exhaustion");
		}
		require(!!world.createAgent("Recovery", corridor, 0, 1.f),
			"Instruction-budget failure poisoned subsequent creation");
	}

	void allocationBudgetIsEnforced(smoke::Context const&)
	{
		core::World world("Memory budget", 4, 2, {},
			core::AgentTypeRuntimeLimits{ 512u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 4);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("hungry.agent.lua",
			typeSource("Hungry", "Hungry",
				"        local blob = string.rep(\"x\", 16 * 1024 * 1024)\n"
				+ validBaseline()), &diagnostic), diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Hungry", "Hungry", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "A huge allocation was not stopped by the memory budget");
		require(world.getSimulationSnapshot().agents.empty(),
			"A memory-budget failure left a partial Agent");
		for (auto const* guard : { "pcall", "xpcall" })
		{
			auto const body = std::string(guard)
				+ "(function() local blob = string.rep('x', 16 * 1024 * 1024) end"
				+ (std::string_view(guard) == "xpcall" ? ", function(e) return e end" : "")
				+ ")\n" + validBaseline();
			require(world.attachAgentType(std::string(guard) + ".agent.lua",
				typeSource(guard, guard, body), &diagnostic), diagnostic);
			bool refused = false;
			try { world.createAgent(guard, "Caught budget", corridor, 0, 1.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.empty(),
				"A protected call swallowed terminal allocation-budget exhaustion");
		}
		require(!!world.createAgent("Recovery", corridor, 0, 1.f),
			"Allocation-budget failure poisoned subsequent creation");
	}

	void instancesAreIsolated(smoke::Context const&)
	{
		core::World world("Isolation", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		// A module-level counter proves isolation: each new() must start from a
		// fresh environment, so every instance observes width == 1.0 rather than
		// a shared, incrementing counter.
		std::string const counter = "count = count + 1\n" + validBaseline("width = count,\n");
		std::string diagnostic;
		require(world.attachAgentType("counter.agent.lua",
			"local count = 0\n" + typeSource("Counter", "Counter", counter), &diagnostic), diagnostic.c_str());
		auto const first = world.createAgent("Counter", "First", corridor, 0, 1.0f);
		auto const second = world.createAgent("Counter", "Second", corridor, 0, 2.0f);
		require(world.lookupAgent(first).entity->getPhysicalBaseline().width == 1.0f
			&& world.lookupAgent(second).entity->getPhysicalBaseline().width == 1.0f,
			"Mutable module state leaked between Agent instances");
	}

	void legacyAndScriptedHumanLoading(smoke::Context const&)
	{
		core::World original("Document identity", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		auto const id = original.createAgent("Saved human", sector, 0, 1.0f);
		(void)id;
		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Scripted Human World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getTypeId() == "Human"
				&& std::string(agent->getTypeName()) == "Human",
				"Scripted Human identity was lost on load");
		}
		// An explicit missing resource is refused, never silently Human.
		auto yaml = serializeWorld(original, false);
		auto const resourceAt = yaml.find("resource: human.agent.lua");
		require(resourceAt != std::string::npos, "Saved Agent omitted its resource reference");
		yaml.replace(resourceAt, std::string("resource: human.agent.lua").size(),
			"resource: missing.agent.lua");
		core::World refused("refused", 2, 2);
		bool rejected = false;
		try
		{
			(void)loadWorld(yaml, false, refused);
		}
		catch (core::SerializationException const& error)
		{
			rejected = std::string(error.what()).find("missing.agent.lua") != std::string::npos;
		}
		require(rejected, "A missing Agent type resource was not refused clearly");
	}

	void duplicateTypeIdIsRejected(smoke::Context const&)
	{
		core::World world("Duplicate", 4, 2);
		std::string diagnostic;
		require(!world.attachAgentType("other-human.agent.lua",
			typeSource("Human", "Other Human", validBaseline()), &diagnostic),
			"A duplicate Human type ID was accepted");
		require(world.agentTypeDisplayName("Human") == "Human",
			"A refused duplicate replaced the bundled Human");
	}

	void resetReconstructsScriptedInstances(smoke::Context const&)
	{
		core::World world("Reset", 6, 2);
		auto const sector = world.addCorridor(0, 0, 6);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()), &diagnostic),
			diagnostic.c_str());
		auto const id = world.createAgent("Tall", "Tall one", sector, 0, 1.0f);
		auto const before = world.lookupAgent(id).entity->getPhysicalBaseline().width;
		world.resetSimulation();
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Tall"
			&& agent->getPhysicalBaseline().width == before,
			"Reset lost or changed the scripted Agent's frozen baseline");
	}

	void resetRevisionAndAuthoredData(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const path = root / "revision.agent.lua";
		auto write = [&](float width) {
			std::ofstream out(path);
			// Both module upvalues and instance-private data are fresh each time.
			out << "local count = 0\n" << typeSource("Revision",
				width > 0.5f ? "Revised display" : "Original display",
				"count = count + 1\n" + validBaseline(
					"width = " + std::to_string(width) + " * count,\n"
					"reach = nil, object_usage = 'arms', object_usage_distance = " + std::to_string(width) + ",\n"
					"private_state = { count = count },\n"
					+ std::string(width > 0.5f
						? "poses={standing={image_tile='agent'},crouching={image_tile='agent',height_ratio=0.8},crawling={image_tile='agent',height_ratio=0.4}},automatic_poses={room_movement={{pose='standing',speed_ratio=0.9}},door_crossing={{pose='standing',speed_ratio=0.7},{pose='crawling',speed_ratio=0.6}}},\n"
						: "") +
					"mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = '"
					+ std::string(width > 0.5f ? "can_use" : "cannot_use") + "', buttons = 'only_if_no_other_option' },\n"));
			require(bool(out), "Could not write revision fixture");
		};
		write(0.5f);
		auto const resource = core::externalAgentTypeResourceName(path);
		core::World world("Revision", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		constexpr uint32_t back = 1; // Layer identity survives structural reindexing.
		world.addCorridor(back, 0, 0, 8, 1);
		world.addSectorDoor(0, 0, 2);
		world.finishBuild();
		world.pauseSimulation();
		auto definition = core::resolveAgentTypeResource(resource);
		std::string diagnostic;
		require(definition && world.attachAgentType(resource, definition->source, &diagnostic), diagnostic);
		auto first = world.createAgent("Revision", "First", corridor, 0, 1.f);
		auto second = world.createAgent("Revision", "Second", corridor, 0, 3.f);
		auto third = world.createAgent("Revision", "Tagged", corridor, 0, 5.f);
		auto registry = core::AgentTagRegistry::create();
		auto tag = registry->addAgentTag("physical");
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(tag, { 0.7f, 0.9f }, &diagnostic), diagnostic);
		core::MobilityProfile tagProfile;
		tagProfile.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
		tagProfile.set(core::TraversalKind::Ladder, core::MobilityUse::OnlyIfNoOtherOption);
		require(registry->addAgentTagMobilityProfile(tag, &diagnostic)
			&& registry->setAgentTagMobilityProfile(tag, tagProfile, &diagnostic), diagnostic);
		auto const registryPath = root / "revision.tags.yaml";
		registry->saveTo(registryPath.string());
		world.attachAgentTagRegistry(registryPath.filename().string(), registry);
		require(world.assignAgentTag(first, tag, &diagnostic)
			&& world.assignAgentTag(third, tag, &diagnostic), diagnostic);
		core::MobilityProfile individual;
		individual.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
		individual.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualMobilityProfile(first, individual, &diagnostic), diagnostic);
		auto verifyMobility = [&](core::World const& candidate, bool revised) {
			auto a = candidate.lookupAgent(first).entity;
			auto b = candidate.lookupAgent(second).entity;
			auto c = candidate.lookupAgent(third).entity;
			require(a && b && c && a->getIndividualMobilityProfile() == individual
				&& a->getEffectiveMobilityProfile().value == individual
				&& !b->getIndividualMobilityProfile() && b->getAgentTagIds().empty()
				&& !c->getIndividualMobilityProfile() && c->getAgentTagIds().contains(tag)
				&& c->getEffectiveMobilityProfile().value == tagProfile,
				"Lifetime reconstruction lost Mobility overrides/tags or materialised defaults");
			for (auto id : { first, second, third })
			{
				auto const* agent = candidate.lookupAgent(id).entity;
				require(agent->getTypeId() == "Revision" && agent->getTypeResourceName() == resource
					&& agent->getObjectUsage() == core::ObjectUsage::Arms
					&& agent->getObjectUsageDistance() == (revised ? .75f : .5f)
					&& agent->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door)
						== (revised ? core::MobilityUse::CanUse : core::MobilityUse::CannotUse),
					"Lifetime did not resolve the expected frozen script Mobility revision");
				require(agent->supportsPose(core::Pose::Lying) == !revised
					&& agent->supportsPose(core::Pose::Crawling)
					&& agent->getPoseEnvelope(core::Pose::Crouching)->y == agent->getStandingHeight() * (revised ? 0.8f : 0.6f)
					&& agent->getPhysicalBaseline().roomMovement.front().speedRatio == (revised ? 0.9f : 1.f),
					"Lifetime changed or failed to reconstruct frozen pose capabilities/orders");
			}
			agent_smoke::requireDoorRoute(candidate, first, back, true); // last-resort second pass
			agent_smoke::requireDoorRoute(candidate, second, back, revised);
			agent_smoke::requireDoorRoute(candidate, third, back, false);
		};
		verifyMobility(world, false);
		require(world.setAgentIndividualWalkSpeedModifier(first, 1.2f), "Could not author speed");
		require(world.setAgentIndividualHeightModifier(first, 0.95f), "Could not author height");
		auto const sample = world.lookupAgent(first).entity->getHeightModifierSample();
		world.saveTo((root / "revision.world.yaml").string());
		world.saveTo((root / "revision.world").string());
		auto const authored = serializeWorld(world, false);
		write(0.75f);
		require(world.lookupAgent(first).entity->getPhysicalBaseline().width == 0.5f
			&& world.lookupAgent(second).entity->getPhysicalBaseline().width == 0.5f,
			"On-disk edits changed a live frozen baseline");
		verifyMobility(world, false);
		for (auto const* filename : { "revision.world.yaml", "revision.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			verifyMobility(*loaded, true);
			require(loaded->lookupAgent(first).entity->getPhysicalBaseline().width == 0.75f
				&& loaded->lookupAgent(second).entity->getPhysicalBaseline().width == 0.75f
				&& std::string(loaded->lookupAgent(first).entity->getTypeName()) == "Revised display",
				"Load reused a baseline snapshot, refused revised display, or shared mutable module state");
		}
		world.resetSimulation();
		verifyMobility(world, true);
		for (auto id : { first, second, third })
		{
			auto const* agent = world.lookupAgent(id).entity;
			require(agent && agent->getTypeId() == "Revision"
				&& agent->getTypeResourceName() == resource
				&& agent->getPhysicalBaseline().width == 0.75f,
				"Reset did not reconstruct isolated instances from the current revision");
		}
		auto const* agent = world.lookupAgent(first).entity;
		require(agent->getHeightModifierSample() == sample
			&& agent->getIndividualHeightModifier() == 0.95f
			&& agent->getIndividualWalkSpeedModifier() == 1.2f
			&& agent->getAgentTagIds().contains(tag), "Reset changed authored properties or tag samples");
		require(world.isSimulationPaused() && !world.isModified()
			&& serializeWorld(world, false) == authored,
			"Reset changed authored document data, pause or dirty state");
		require(authored.find("private_state") == std::string::npos
			&& authored.find("object_usage") == std::string::npos
			&& authored.find("standing_height") == std::string::npos
			&& authored.find("mobility_profile") == std::string::npos
			&& authored.find("automatic_poses") == std::string::npos && authored.find("poses:") == std::string::npos,
			"Document serialized private state or script defaults");
		require(world.setAgentIndividualMobilityProfile(first, std::nullopt, &diagnostic), diagnostic);
		agent_smoke::requireDoorRoute(world, first, back, false); // tag replaces the complete default
		require(world.removeAgentTag(first, tag, &diagnostic), diagnostic);
		agent_smoke::requireDoorRoute(world, first, back, true); // revised script exposed
	}

	void resetFailureIsAtomic(smoke::Context const& context)
	{
		auto const path = context.temporaryRoot() / "failure.agent.lua";
		auto const valid = typeSource("Failure", "Failure", validBaseline(
			"mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'cannot_use', buttons = 'can_use' },\n"));
		auto write = [&](std::string const& source) {
			std::ofstream out(path); out << source;
			require(bool(out), "Could not write failure fixture");
		};
		write(valid);
		auto const resource = core::externalAgentTypeResourceName(path);
		core::World world("Atomic reset", 8, 2, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 8);
		world.addCorridor(1, 0, 0, 8, 1);
		world.addSectorDoor(0, 0, 2);
		world.finishBuild();
		// Human preconstruction succeeds before the second type is refused.
		auto const first = world.createAgent("First", corridor, 0, 1.f);
		std::string diagnostic;
		require(world.attachAgentType(resource, valid, &diagnostic), diagnostic);
		auto const second = world.createAgent("Failure", "Second", corridor, 0, 3.f);
		auto const third = world.createAgent("Failure", "Third", corridor, 0, 5.f);
		world.update(0.1);
		auto const* originalFirst = world.lookupAgent(first).entity;
		auto const* originalSecond = world.lookupAgent(second).entity;
		auto const* originalThird = world.lookupAgent(third).entity;
		auto const authored = serializeWorld(world, false);
		auto const tick = world.getSimulationSnapshot().tick;
		auto const paused = world.isSimulationPaused();
		auto const modified = world.isModified();
		for (auto const& failure : {
			std::pair<std::string, std::string>{ std::string("invalid Lua"), {} },
			{ typeSource("Failure", "Failure", "error('Reset constructor refused')"), {} },
			{ typeSource("Failure", "Failure", validBaseline("width = 0,\n")), {} },
			{ typeSource("Failure", "Failure", validBaseline("poses={standing={image_tile='agent'},crawling={image_tile='agent',height_ratio='0.3'}},\n")), {} },
			{ typeSource("Failure", "Failure", validBaseline("automatic_poses={},\n")), {} },
			{ typeSource("Failure", "Failure", validBaseline("mobility_profile = nil,\n")), {} },
			// A complete profile with one invalid use genuinely exercises the
			// invalid-use path rather than failing early on an omitted entry.
			{ typeSource("Failure", "Failure", validBaseline("mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'bad', buttons = 'can_use' },\n")), "door" },
			{ typeSource("WrongIdentity", "Wrong", validBaseline()), {} },
			{ typeSource("Failure", "Failure", "while true do end"), {} },
			{ typeSource("Failure", "Failure", validBaseline("private_blob = string.rep('x', 128 * 1024 * 1024),\n")), {} },
			// One revised constructor fits; the next instance exceeds the total
			// runtime budget. Failure must discard the whole candidate set.
			{ typeSource("Failure", "Failure", validBaseline("private_blob = string.rep('x', 800 * 1024),\n")), {} },
			{ std::string{}, {} } })
		{
			auto const& [source, field] = failure;
			write(source);
			if (source.empty()) std::filesystem::remove(path);
			bool refused = false;
			try { world.resetSimulation(); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find(resource) != std::string::npos
					&& (field.empty() || message.find(field) != std::string::npos);
			}
			require(refused, "Reset failure did not diagnose its resource and offending field");
			require(world.lookupAgent(first).entity == originalFirst
				&& world.lookupAgent(second).entity == originalSecond
				&& world.lookupAgent(third).entity == originalThird
				&& world.getSimulationSnapshot().tick == tick
				&& world.isSimulationPaused() == paused && world.isModified() == modified
				&& serializeWorld(world, false) == authored,
				"A refused Reset partially reconstructed or rewound the World");
			agent_smoke::requireDoorRoute(world, first, 1, true);
			agent_smoke::requireDoorRoute(world, second, 1, false);
			agent_smoke::requireDoorRoute(world, third, 1, false);
		}
		// Fresh load validates declarations too, and must leave an existing
		// target World intact when the currently resolved definition is invalid.
		write(valid);
		for (auto const* filename : { "invalid-poses.world.yaml", "invalid-poses.world" })
			world.saveTo((context.temporaryRoot() / filename).string());
		write(typeSource("Failure", "Failure", validBaseline("automatic_poses={},\n")));
		for (auto const* filename : { "invalid-poses.world.yaml", "invalid-poses.world" })
		{
			bool refused = false;
			try { (void)core::loadWorldDocument(context.temporaryRoot() / filename); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find(resource) != std::string::npos && message.find("automatic_poses") != std::string::npos;
			}
			require(refused && world.lookupAgent(first).entity == originalFirst
				&& world.lookupAgent(second).entity == originalSecond && serializeWorld(world, false) == authored,
				"Malformed pose document load changed the live World or lacked diagnostics");
		}
		write(valid);
		world.resetSimulation();
		require(world.lookupAgent(second).entity->getPhysicalBaseline().width == 0.5f,
			"A failed Reset poisoned subsequent reconstruction");
	}

	void resetReleasesReplacedInstances(smoke::Context const&)
	{
		core::World world("Reset lifetime", 8, 2, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("private.agent.lua", typeSource("Private", "Private",
			validBaseline("private_blob = string.rep('x', 800 * 1024),\n")), &diagnostic), diagnostic);
		auto const id = world.createAgent("Private", "Instance", corridor, 0, 1.f);
		for (int cycle = 0; cycle < 4; ++cycle)
		{
			world.resetSimulation(); // Must not charge both old and fresh instances to one runtime.
			require(world.lookupAgent(id).entity->getPhysicalBaseline().width == 0.5f,
				"Reset lost its private instance's baseline");
			bool refused = false;
			try { world.createAgent("Private", "Duplicate", corridor, 0, 3.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.size() == 1,
				"Reset discarded the private instance or failed to preserve its runtime budget");
		}
	}

	void resolvedResourceIdentity(smoke::Context const&)
	{
		auto const definition = core::resolveAgentTypeResource("human.agent.lua");
		require(definition.has_value(),
			"The bundled Human Agent type resource did not resolve");
		require(definition->typeId == "Human" && definition->displayName == "Human"
			&& definition->resourceName == "human.agent.lua",
			"The resolved bundled Human resource lost its identity");
		require(definition->source == core::bundledHumanAgentType().source,
			"The resolved bundled Human resource drifted from the embedded definition");
	}

	void resolvedResourceUnavailable(smoke::Context const&)
	{
		require(!core::resolveAgentTypeResource("no-such.agent.lua").has_value(),
			"An unknown Agent type resource resolved");
	}

	void previewMatchesPlacement(smoke::Context const&)
	{
		core::World world("Preview agreement", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const id = world.createAgent("Human", "Placed", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent != nullptr, "The placed Human was not found");
		auto const& baseline = agent->getPhysicalBaseline();
		auto const preview = core::Agent::placementDimensions(core::bundledHumanBaseline());
		require(preview.x == baseline.width && preview.y == baseline.standingHeight,
			"The preview dimensions did not match the scripted placement baseline");
		auto const modified = core::Agent::placementDimensions(core::bundledHumanBaseline(), 1.5f);
		require(modified.x == baseline.width
			&& modified.y == baseline.standingHeight * 1.5f,
			"The preview did not apply the height modifier to the scripted baseline");
	}

	void scriptedAuthorizationPlacement(smoke::Context const&)
	{
		core::World world("Authorization", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const id = world.createAgent("Human", "Granted", corridor, 0, 1.0f,
			std::set<core::AccessPermissionId>{}, std::set<core::PermissionSetId>{});
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"The scripted authorization placement lost Human identity");
		require(agent->getPhysicalBaseline().walkSpeed
				== core::bundledHumanBaseline().walkSpeed,
			"The scripted authorization placement did not use the scripted baseline");
	}

	std::string removeLineContaining(std::string text, std::string const& needle)
	{
		auto const pos = text.find(needle);
		if (pos == std::string::npos) return text;
		auto lineStart = text.rfind('\n', pos);
		lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
		auto lineEnd = text.find('\n', pos);
		lineEnd = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
		text.erase(lineStart, lineEnd - lineStart);
		return text;
	}

	// Loads every valid manifest-registered Agent type resource and returns the
	// Android fixture definition the same way the editor does at startup.
	std::optional<core::AgentTypeDefinition> androidDefinition(
		smoke::Context const& context)
	{
		require(!core::resolveAgentTypeResource("scout.agent.lua")
			&& !core::resolveAgentTypeResource("standing-robot.agent.lua")
			&& !core::resolveAgentTypeResource("robot.agent.lua"),
			"A removed bundled Agent type is still registered");
		auto const source = readFile(context.fixture("resources/test-worlds/android.agent.lua"));
		auto const resolved = core::resolveAgentTypeResource("android.agent.lua");
		if (!resolved || resolved->typeId != "Android" || resolved->displayName != "Android"
			|| resolved->resourceName != "android.agent.lua" || resolved->source != source)
			return std::nullopt;
		return resolved;
	}

	void cleaningBot(smoke::Context const&)
	{
		auto const definition = core::resolveAgentTypeResource("cleaning-bot.agent.lua");
		require(definition && definition->typeId == "CleaningBot"
			&& definition->displayName == "Cleaning Bot", "CleaningBot resource did not resolve");
		auto attach = [&](core::World& world) {
			std::string diagnostic;
			require(world.attachAgentType(definition->resourceName, definition->source, &diagnostic), diagnostic);
		};
		auto verify = [](core::Agent const* bot) {
			require(bot && bot->getTypeId() == "CleaningBot"
				&& bot->getTypeResourceName() == "cleaning-bot.agent.lua"
				&& bot->getPoseImageTile() == "cleaning-bot-standing", "CleaningBot identity/artwork changed");
			auto const& physical = bot->getPhysicalBaseline();
			require(physical.width == .4f && physical.standingHeight == .15f
				&& physical.walkSpeed == .3f && physical.poses.size() == 1
				&& bot->supportsPose(core::Pose::Standing), "CleaningBot dimensions or poses changed");
			for (auto context : {core::AutomaticPoseContext::RoomMovement, core::AutomaticPoseContext::DoorCrossing})
				require(physical.automaticSpeedRatio(context, core::Pose::Standing) == 1.f
					&& !physical.automaticSpeedRatio(context, core::Pose::Crawling), "CleaningBot automatic pose changed");
			for (auto kind : {core::TraversalKind::Staircase, core::TraversalKind::Escalator,
				core::TraversalKind::Stairwell, core::TraversalKind::Ladder, core::TraversalKind::Lift,
				core::TraversalKind::PlatformLift, core::TraversalKind::Shuttle, core::TraversalKind::Buttons})
				require(core::agentForbidsTraversal(bot, kind), "CleaningBot acquired forbidden Mobility");
			require(!core::agentForbidsTraversal(bot, core::TraversalKind::Door)
				&& bot->getEffectiveMobilityProfile().value == bot->getScriptDefaultMobilityProfile(),
				"CleaningBot did not use its script-default Mobility");
		};
		core::World world("CleaningBot lifetime", 8, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
		auto made = world.addAccessPanel(room, 0, 4);
		auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(made.sector->getObject(made.index))->getPanel();
		world.finishBuild(); attach(world);
		auto id = world.createAgent("CleaningBot", "Cleaner", room, 0, 4.5f);
		verify(world.lookupAgent(id).entity);
		using Action = core::AccessPanel::Action;
		require(!world.canRequestAccessPanel(panel->getId(), Action::Open, id)
			&& !world.requestAccessPanel(panel->getId(), Action::Open, id)
			&& !world.requestInteraction(panel->getControl(Action::Open), id),
			"CleaningBot operated an Access panel through a public request seam");
		for (bool binary : {false, true})
		{
			core::World restored("Restored cleaner", 2, 2);
			require(loadWorld(serializeWorld(world, binary), binary, restored), "CleaningBot document failed to load");
			verify(restored.lookupAgent(id).entity);
		}
		world.resetSimulation(); verify(world.lookupAgent(id).entity);

		// Regular manual/automatic Doors work; every button-operated threshold
		// is excluded in route search and refuses a journey, including bulkheads.
		for (bool bulkhead : {false, true})
		for (bool reverse : {false, true})
		for (auto mode : {core::DoorActivationMode::Manual, core::DoorActivationMode::Automatic,
			core::DoorActivationMode::RemoteControlled})
		{
			if (bulkhead && mode != core::DoorActivationMode::RemoteControlled) continue;
			core::World journey("CleaningBot doors", 8, 2);
			auto a = journey.addRoom("A", 0, 0, 0, bulkhead ? 4 : 8, 1);
			auto b = journey.addRoom("B", bulkhead ? 0 : 1, 0, bulkhead ? 4 : 0, bulkhead ? 4 : 8, 1);
			if (bulkhead)
			{
				core::World::CreateBulkheadDoorOptions options;
				options.activationMode = mode; options.controls[0] = options.controls[1] = true;
				journey.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
			}
			else
			{
				core::World::CreateDoorOptions options;
				options.activationMode = mode;
				options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
				journey.addSectorDoor(0, 0, 2, options);
			}
			auto target = reverse ? a : b;
			journey.addSectorMarker(target, 0, bulkhead ? 1.5f : 2.5f, "Goal");
			journey.finishBuild(); attach(journey);
			auto botId = journey.createAgent("CleaningBot", "Cleaner", reverse ? b : a, 0,
				bulkhead ? (reverse ? 1.5f : 2.5f) : 1.5f);
			if (mode == core::DoorActivationMode::RemoteControlled)
			{
				pose_journeys::refused(journey, botId, "Goal");
				continue;
			}
			require(journey.moveAgentToNamedMarker(botId, "Goal").accepted(), "CleaningBot regular Door intent refused");
			bool arrived = false;
			for (unsigned tick = 0; tick < 1800 && !arrived; ++tick)
			{
				require(journey.advanceTick(), "CleaningBot journey tick refused");
				auto* bot = journey.lookupAgent(botId).entity;
				require(bot->getPose() == core::Pose::Standing, "CleaningBot invented a lowered pose");
				arrived = bot->getSector() == journey.getSector(target).get() && bot->getState() == core::Agent::State::Idle;
			}
			require(arrived, "CleaningBot did not cross a regular non-button Door");
		}
	}

	void fixtureTypePhysicalOutcomes(smoke::Context const& context)
	{
		auto const resolved = androidDefinition(context);
		require(resolved.has_value(), "The Android Agent type resource did not resolve");

		core::World world("Android physical", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("android.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		require(world.hasAgentType("Android")
			&& world.agentTypeDisplayName("Android") == "Android",
			"The Android type identity did not resolve");
		auto const id = world.createAgent("Android", "Runner", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Android"
			&& std::string(agent->getTypeName()) == "Android"
			&& agent->getTypeResourceName() == "android.agent.lua",
			"The placed Android identity did not resolve");

		auto const& physical = agent->getPhysicalBaseline();
		require(physical.width == 0.4f && physical.standingHeight == 0.45f
			&& physical.objectUsageDistance == 0.25f && physical.walkSpeed == 0.5f
			&& physical.climbSpeed == 0.25f && physical.stairAscentSpeed == 0.35f
			&& physical.stairDescentSpeed == 0.45f && physical.poses.size() == 1,
			"Android's frozen baseline did not match the resource");

		// The highest available public consumers read the frozen baseline.
		require(agent->getWalkSpeed() == physical.walkSpeed
			&& agent->getClimbSpeed() == physical.climbSpeed,
			"Android movement did not use its frozen baseline");
		auto const bounds = agent->getBounds().getSize();
		auto near = [](float left, float right)
		{
			return std::fabs(left - right) < 1e-5f;
		};
		require(near(bounds.x, physical.width) && near(bounds.y, physical.standingHeight),
			"Android bounds did not use its frozen baseline");
		require(!agent->supportsPose(core::Pose::Crawling),
			"Android acquired unsupported Crawling");
	}

	void fixturePersistenceRoundTrip(smoke::Context const& context)
	{
		auto const resolved = androidDefinition(context);
		require(resolved.has_value(), "The Android Agent type resource did not resolve");

		core::World original("Android document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("android.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		auto const id = original.createAgent("Android", "Saved android", sector, 0, 1.0f);
		(void)id;

		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Android World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getTypeId() == "Android"
				&& agent->getTypeResourceName() == "android.agent.lua"
				&& std::string(agent->getTypeName()) == "Android",
				"Android type/resource identity was not preserved");
			auto const& physical = agent->getPhysicalBaseline();
			require(physical.width == 0.4f && physical.walkSpeed == 0.5f
				&& physical.objectUsageDistance == 0.25f,
				"Android's frozen baseline was not reconstructed on load");
		}

		auto const yaml = serializeWorld(original, false);
		require(yaml.find("typeId: Android") != std::string::npos
			&& yaml.find("resource: android.agent.lua") != std::string::npos,
			"The saved document omitted the Android type ID or resource reference");
		require(yaml.find("walk_speed") == std::string::npos
			&& yaml.find("standing_height") == std::string::npos
			&& yaml.find("crawling_height_ratio") == std::string::npos,
			"The saved document embedded a physical-baseline snapshot");
	}

	void loadingRefusesMismatchedOrMissingResource(smoke::Context const& context)
	{
		auto const resolved = androidDefinition(context);
		require(resolved.has_value(), "The Android Agent type resource did not resolve");

		core::World original("Refusal document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("android.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Android", "Android", sector, 0, 1.0f);

		auto const yaml = serializeWorld(original, false);

		// A mismatched type ID is refused and publishes no partial Agent.
		auto mismatched = yaml;
		auto const typeIdAt = mismatched.find("typeId: Android");
		require(typeIdAt != std::string::npos, "Saved Android omitted its type ID");
		mismatched.replace(typeIdAt, std::string("typeId: Android").size(), "typeId: NotAndroid");
		core::World refusedType("refused type", 2, 2);
		bool typeRejected = false;
		try
		{
			(void)loadWorld(mismatched, false, refusedType);
		}
		catch (core::SerializationException const& error)
		{
			typeRejected = std::string(error.what()).find("NotAndroid") != std::string::npos;
		}
		require(typeRejected, "A mismatched Agent type ID was not refused clearly");
		require(refusedType.getSimulationSnapshot().agents.empty(),
			"A mismatched type ID left a partial Agent");

		// A missing resource reference is refused and publishes no partial Agent.
		auto missing = yaml;
		auto const resourceAt = missing.find("resource: android.agent.lua");
		require(resourceAt != std::string::npos, "Saved Android omitted its resource reference");
		missing.replace(resourceAt, std::string("resource: android.agent.lua").size(),
			"resource: missing.agent.lua");
		core::World refusedResource("refused resource", 2, 2);
		bool resourceRejected = false;
		try
		{
			(void)loadWorld(missing, false, refusedResource);
		}
		catch (core::SerializationException const& error)
		{
			resourceRejected = std::string(error.what()).find("missing.agent.lua") != std::string::npos;
		}
		require(resourceRejected, "A missing Agent type resource was not refused clearly");
		require(refusedResource.getSimulationSnapshot().agents.empty(),
			"A missing resource left a partial Agent");
	}

	void competingTypeIdsRejectedOnLoad(smoke::Context const& context)
	{
		auto const android = androidDefinition(context);
		require(android.has_value(), "The Android Agent type resource did not resolve");

		core::World original("Competing", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("android.agent.lua", android->source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Android", "First", sector, 0, 1.0f);
		(void)original.createAgent("Android", "Second", sector, 0, 2.0f);

		auto yaml = serializeWorld(original, false);
		auto const needle = std::string("resource: android.agent.lua");
		auto const pos = yaml.rfind(needle);
		require(pos != std::string::npos, "The second Android omitted its resource reference");
		yaml.replace(pos, needle.size(), "resource: rival.agent.lua");

		// A rival resource declaring the same type ID must be rejected when it
		// would compete with the already-resolved resource inside one World.
		core::AgentTypeDefinition rival = *android;
		rival.resourceName = "rival.agent.lua";
		rival.displayName = "Android";
		rival.source = android->source;
		struct LoaderScope
		{
			explicit LoaderScope(core::AgentTypeDefinition androidDef,
				core::AgentTypeDefinition rivalDef)
			{
				core::setAgentTypeResourceLoader(
					[androidDef = std::move(androidDef), rivalDef = std::move(rivalDef)](
						std::string const& name) -> std::optional<core::AgentTypeDefinition>
					{
						if (name == "android.agent.lua") return androidDef;
						if (name == "rival.agent.lua") return rivalDef;
						return std::nullopt;
					});
			}
			~LoaderScope() { core::setAgentTypeResourceLoader({}); }
		};

		core::World restored("restored", 2, 2);
		bool rejected = false;
		{
			LoaderScope scope{ *android, rival };
			try
			{
				(void)loadWorld(yaml, false, restored);
			}
			catch (core::SerializationException const& error)
			{
				rejected = std::string(error.what()).find("competing") != std::string::npos;
			}
		}
		require(rejected,
			"Competing resources with the same type ID were not rejected on load");
	}

	void legacyHumanWithoutResourceResolves(smoke::Context const&)
	{
		core::World original("Legacy identity", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		constexpr uint32_t back = 1;
		original.addCorridor(back, 0, 0, 6, 1);
		original.addSectorDoor(0, 0, 2);
		original.finishBuild();
		original.pauseSimulation();
		auto const id = original.createAgent("Saved human", sector, 0, 1.0f);
		require(original.setAgentIndividualWalkSpeedModifier(id, 1.2f),
			"Could not author an individual Walk speed modifier");

		auto yaml = removeLineContaining(serializeWorld(original, false),
			"resource: human.agent.lua");
		yaml = removeLineContaining(yaml, "typeId: Human");
		yaml = removeLineContaining(yaml, "type: Human");
		require(yaml.find("resource: human.agent.lua") == std::string::npos,
			"The resource reference was not stripped");

		core::World restored("restored", 2, 2);
		require(loadWorld(yaml, false, restored), "The legacy Human World did not load");
		auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
		require(agent && agent->getTypeId() == "Human"
			&& std::string(agent->getTypeName()) == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"The legacy Human did not resolve to the bundled Human");
		require(agent->getPhysicalBaseline().walkSpeed == core::bundledHumanBaseline().walkSpeed,
			"The legacy Human did not resolve to the bundled baseline");
		require(agent->getIndividualWalkSpeedModifier()
			&& *agent->getIndividualWalkSpeedModifier() == 1.2f,
			"The legacy Human lost an authored individual property");
		require(!agent->getIndividualMobilityProfile()
			&& agent->getEffectiveMobilityProfile().value == core::MobilityProfile{},
			"Legacy Human did not retain the migrated all–Can use default");
		agent_smoke::requireDoorRoute(restored, core::AgentId{ 1 }, back, true);
	}

	void topologyReplayPreservesLiveInstances(smoke::Context const&)
	{
		// The public allocation budget makes a second live construction fail.
		// Replay must carry the existing private object, not duplicate it (nor
		// drop it before construction). No VM inspection or method API is needed.
		core::World world("Retained instance", 12, 3, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		world.pauseSimulation();
		std::string diagnostic;
		require(world.attachAgentType("retained.agent.lua",
			typeSource("Retained", "Distinct display name", validBaseline(
				"            private_blob = string.rep('x', 800 * 1024),\n")),
			&diagnostic), diagnostic.c_str());
		core::AgentId id;
		try { id = world.createAgent("Retained", "Survivor", corridor, 0, 2.0f); }
		catch (std::exception const& error) { throw std::runtime_error(std::string("Initial construction: ") + error.what()); }
		require(world.setAgentIndividualWalkSpeedModifier(id, 1.2f),
			"Could not author a survivor property");
		require(world.setAgentActive(id, false), "Could not deactivate survivor");
		auto refuseDuplicate = [&] {
			bool refused = false;
			try { (void)world.createAgent("Retained", "Duplicate", corridor, 0, 4.0f); }
			catch (std::exception const&) { refused = true; }
			require(refused, "The retained private object no longer consumes its instance budget");
		};
		refuseDuplicate();
		auto verify = [&] {
			auto const* agent = world.lookupAgent(id).entity;
			require(agent && agent->getTypeId() == "Retained"
				&& std::string(agent->getTypeName()) == "Distinct display name"
				&& agent->getTypeResourceName() == "retained.agent.lua"
				&& agent->getPhysicalBaseline().width == 0.5f
				&& agent->getIndividualWalkSpeedModifier() == 1.2f && !agent->isActive(),
				"Replay lost survivor identity, baseline, or authored/runtime state");
		};
		world.addLevel();
		verify();
		auto resize = world.planResizeLocation(corridor, 0, 0, 9, 1);
		require(resize.valid, "Survivor Location resize did not validate");
		corridor = world.applyLocationEdit(resize);
		verify();
		// Refused topology changes are true no-ops, including private lifetime.
		auto invalid = world.planResizeLocation(corridor, 11, 0, 9, 1);
		require(!invalid.valid, "An out-of-bounds Location resize was accepted");
		verify();
		refuseDuplicate(); // carrying only a baseline would have discarded the blob
		auto remove = world.planRemoveLocation(corridor);
		require(remove.valid, "Location removal did not validate");
		world.applyLocationEdit(remove);
		require(!world.lookupAgent(id).entity, "A deleted Agent survived Location removal");
		auto replacementSector = world.addCorridor(0, 0, 9);
		world.finishBuild();
		// A new lightweight construction and teardown remain safe after releasing
		// the large instance (its unreferenced Lua storage is collected lazily).
		auto const replacement = world.createAgent("Fresh", replacementSector, 0, 2.0f);
		require(!!replacement && replacement != id,
			"Deletion did not permit fresh construction with a new Agent identity");
	}

	void loadingConstructsFreshInstances(smoke::Context const&)
	{
		// A module-level counter is mutable state that must never survive into
		// a document or across load: loading reconstructs from source, so the
		// reloaded World's instance observes width == 1 rather than a persisted
		// count.
		std::string const counter = "count = count + 1\n" + validBaseline("width = count,\n");
		core::AgentTypeDefinition counterType;
		counterType.typeId = "Counter";
		counterType.displayName = "Counter";
		counterType.resourceName = "counter.agent.lua";
		counterType.source = "local count = 0\n" + typeSource("Counter", "Counter", counter);
		core::setAgentTypeResourceLoader(
			[counterType](std::string const& name) -> std::optional<core::AgentTypeDefinition>
			{
				if (name == counterType.resourceName) return counterType;
				return std::nullopt;
			});

		core::World original("Counter document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("counter.agent.lua", counterType.source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Counter", "Counter", sector, 0, 1.0f);

		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Counter World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getPhysicalBaseline().width == 1.0f,
				"Loading preserved mutable Lua state instead of constructing fresh instances");
		}
		core::setAgentTypeResourceLoader({});
	}
}

void agent_smoke::registerAgentTypes(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentTypesArmsDeclarations", armsDeclarations });
	checks.push_back({ "agentTypesArmsPhysicalOperations", armsPhysicalOperations });
	checks.push_back({ "agentTypesArmsManualDoor", armsManualDoor });
	checks.push_back({ "agentTypesArmsOnboardSelector", armsOnboardSelector });
	checks.push_back({ "agentPosesSupportedRooms", supportedPoseRooms });
	checks.push_back({ "agentPosesSupportedBridge", supportedPoseBridge });
	checks.push_back({ "agentPosesSupportedDoors", supportedPoseDoors });
	checks.push_back({ "agentPosesCrouchingDoor", supportedCrouchingDoor });
	checks.push_back({ "agentPosesApertureBoundaries", supportedPoseApertures });
	checks.push_back({ "agentTypesPoseDeclarations", poseDeclarations });
	checks.push_back({ "agentTypesInvalidPoseDeclarations", invalidPoseDeclarations });
	checks.push_back({ "agentTypesScriptedHumanIdentity", scriptedHumanIdentity });
	checks.push_back({ "agentTypesBundledDefinitionMatchesResource", bundledDefinitionMatchesResource });
	checks.push_back({ "agentTypesGenericScriptBackedType", genericScriptBackedType });
	checks.push_back({ "agentTypesReservedBehaviourMember", reservedBehaviourMember });
	checks.push_back({ "agentTypesInvalidBaselinesRejected", invalidBaselinesAreRejected });
	checks.push_back({ "agentTypesScriptedMobilityProfiles", scriptedMobilityProfiles });
	checks.push_back({ "agentTypesConstructorFailureLeavesNoPartialAgent", constructorFailureLeavesNoPartialAgent });
	checks.push_back({ "agentTypesExecutionBudgetEnforced", executionBudgetIsEnforced });
	checks.push_back({ "agentTypesAllocationBudgetEnforced", allocationBudgetIsEnforced });
	checks.push_back({ "agentTypesInstancesAreIsolated", instancesAreIsolated });
	checks.push_back({ "agentTypesLegacyAndScriptedLoading", legacyAndScriptedHumanLoading });
	checks.push_back({ "agentTypesDuplicateTypeIdRejected", duplicateTypeIdIsRejected });
	checks.push_back({ "agentTypesResetReconstructsScriptedInstances", resetReconstructsScriptedInstances });
	checks.push_back({ "agentTypesResetRevisionAndAuthoredData", resetRevisionAndAuthoredData });
	checks.push_back({ "agentTypesResetFailureIsAtomic", resetFailureIsAtomic });
	checks.push_back({ "agentTypesResetReleasesReplacedInstances", resetReleasesReplacedInstances });
	checks.push_back({ "agentTypesResolvedResourceIdentity", resolvedResourceIdentity });
	checks.push_back({ "agentTypesResolvedResourceUnavailable", resolvedResourceUnavailable });
	checks.push_back({ "agentTypesPreviewMatchesPlacement", previewMatchesPlacement });
	checks.push_back({ "agentTypesScriptedAuthorizationPlacement", scriptedAuthorizationPlacement });
	checks.push_back({ "agentTypesCleaningBot", cleaningBot });
	checks.push_back({ "agentTypesFixturePhysicalOutcomes", fixtureTypePhysicalOutcomes });
	checks.push_back({ "agentTypesFixturePersistenceRoundTrip", fixturePersistenceRoundTrip });
	checks.push_back({ "agentTypesLoadingRefusesMismatchedOrMissingResource", loadingRefusesMismatchedOrMissingResource });
	checks.push_back({ "agentTypesCompetingTypeIdsRejectedOnLoad", competingTypeIdsRejectedOnLoad });
	checks.push_back({ "agentTypesLegacyHumanWithoutResource", legacyHumanWithoutResourceResolves });
	checks.push_back({ "agentTypesLoadingConstructsFreshInstances", loadingConstructsFreshInstances });
	checks.push_back({ "agentTypesTopologyReplayPreservesLiveInstances", topologyReplayPreservesLiveInstances });
}
