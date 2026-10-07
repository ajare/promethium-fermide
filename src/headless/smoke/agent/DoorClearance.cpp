#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/DoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/AgentTagRegistry.h"
#include "PathFixture.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

// This driver supplies only the missing retained-Pose locomotion segment. It
// dispatches a validated authored Action while real Agent locomotion is waiting
// at the threshold, and leaves pose, occupancy, queues and permits to their
// production owners. It does not write private fields or move Furniture.
namespace core
{
	struct DoorClearanceDiagnostic
	{
		static void poseAtThreshold(World& world, AgentId id, MarkerId marker, std::string const& action)
		{
			SimulationEvent event;
			event.type = SimulationEventType::DestinationReached;
			world.executeMarkerAction(id, marker, action, event);
			smoke::require(event.type != SimulationEventType::ActionFailed, "Threshold diagnostic Action failed");
		}
	};
}

namespace
{
	using smoke::require;
	constexpr char const* uuid = "bd264603-2c7d-4e89-896b-ae7dcc1c33ab";

	void write(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream file(path);
		file << source;
		require(static_cast<bool>(file), "Cannot write Door clearance diagnostic fixture");
	}

	std::filesystem::path catalogue(smoke::Context const& context, float support, core::Pose pose,
		std::string const& supportLiteral = {})
	{
		auto path = context.temporaryRoot() / "clearance.furniture.lua";
		write(path, "return {api_version=1,uuid='" + std::string(uuid) + "',definitions={{"
			"key='support',label='Support',tiles={{x=0,y=0,imageSet='ObjectAtlas',image='chair'}},"
			"usablePoints={{key='body',label='Body',x=0.5,blocksPathing=false,supportElevation="
			+ (supportLiteral.empty() ? std::to_string(support) : supportLiteral) + "}},use=function(a,w,m) w.set_pose('"
			+ std::string(core::poseName(pose)) + "'); w.claim() end,"
			"finish_use=function(a,w,m) w.set_pose('standing'); w.release() end}}}");
		return path;
	}

	void arrive(core::World& world, core::AgentId id, core::MarkerId marker, std::string const& action)
	{
		require(world.moveAgentToMarker(id, marker, action).accepted(), "Pose Action request refused");
		for (unsigned tick = 0; tick < 600; ++tick)
		{
			require(world.advanceTick(), "Pose Action simulation failed");
			for (auto const& event : world.consumeSimulationEvents())
				if (event.agent.id == id && event.type == core::SimulationEventType::DestinationReached) return;
		}
		throw std::runtime_error("Pose Action did not arrive");
	}

	std::shared_ptr<const core::Edge> doorEdge(core::World const& world)
	{
		for (auto const& edge : world.getGraph()->getEdges())
			if (edge->getType() == core::EdgeType::Door) return edge;
		throw std::runtime_error("Missing diagnostic Door");
	}
}

void runPoseDoorClearanceDiagnostics(smoke::Context const& context)
{
	struct Case { char const* label; core::Pose pose; float support, opening; bool fits; float modifier = 1.f; };
	// .45 Standing, .27 Sitting/Crouching, .135 Crawling, .40 rotated Lying.
	// Support is physical; getPoseRenderYOffset() (.25 for claimed Lying) is
	// decorative. Crawling clearance is its 30% height, not Lying's body width.
	for (auto const& test : {
		Case{"sitting-fit", core::Pose::Sitting, 0.f, .28f, true},
		Case{"sitting-raised-refusal", core::Pose::Sitting, .03f, .28f, false},
		Case{"sitting-exact-fit", core::Pose::Sitting, .01f, .28f, true},
		Case{"sitting-within-tolerance", core::Pose::Sitting, .010005f, .28f, true},
		Case{"sitting-over-tolerance", core::Pose::Sitting, .01002f, .28f, false},
		Case{"crouching-fit", core::Pose::Crouching, 0.f, .28f, true},
		Case{"crouching-raised-refusal", core::Pose::Crouching, .03f, .28f, false},
		Case{"crouching-exact-fit", core::Pose::Crouching, .01f, .28f, true},
		Case{"crouching-over-tolerance", core::Pose::Crouching, .01002f, .28f, false},
		Case{"crawling-width-not-used", core::Pose::Crawling, 0.f, .20f, true},
		Case{"crawling-too-low-refusal", core::Pose::Crawling, 0.f, .13f, false},
		Case{"crawling-raised-refusal", core::Pose::Crawling, .03f, .16f, false},
		Case{"crawling-exact-fit", core::Pose::Crawling, .01f, .145f, true},
		Case{"crawling-over-tolerance", core::Pose::Crawling, .01002f, .145f, false},
		Case{"lying-rotated-fit", core::Pose::Lying, 0.f, .41f, true},
		Case{"lying-raised-refusal", core::Pose::Lying, .03f, .41f, false},
		Case{"lying-exact-fit", core::Pose::Lying, .01f, .41f, true},
		Case{"lying-over-tolerance", core::Pose::Lying, .01002f, .41f, false},
		Case{"lying-decorative-refusal", core::Pose::Lying, 0.f, .39f, false},
		Case{"sitting-modifier-exact-fit", core::Pose::Sitting, .01f, .199f, true, .7f},
		Case{"sitting-modifier-refusal", core::Pose::Sitting, .01002f, .199f, false, .7f},
		Case{"crouching-modifier-exact-fit", core::Pose::Crouching, .01f, .199f, true, .7f},
		Case{"crawling-modifier-height-scaled", core::Pose::Crawling, 0.f, .10f, true, .7f},
		Case{"lying-modifier-width-unchanged", core::Pose::Lying, 0.f, .39f, false, .7f},
		Case{"tall-sitting-fit", core::Pose::Sitting, .62f, .9f, true},
		Case{"tall-sitting-refusal", core::Pose::Sitting, .64f, .9f, false},
		Case{"tall-crouching-fit", core::Pose::Crouching, .62f, .9f, true},
		Case{"tall-crouching-refusal", core::Pose::Crouching, .64f, .9f, false},
		Case{"tall-crawling-fit", core::Pose::Crawling, .75f, .9f, true},
		Case{"tall-crawling-refusal", core::Pose::Crawling, .78f, .9f, false},
		Case{"tall-lying-fit", core::Pose::Lying, .49f, .9f, true},
		Case{"tall-lying-refusal", core::Pose::Lying, .51f, .9f, false}})
	for (bool reverse : {false, true})
	for (bool decorated : {false, true})
	{
		// Unclaimed poses cannot have support: paired decoration checks therefore
		// apply to zero-support cases only. The claimed case adds render offsets,
		// not physical elevation, so both must have identical crossing outcomes.
		if (!decorated && test.support != 0.f) continue;
		core::World world(test.label, 6, 3);
		auto front = world.addRoom("Front", 0, 1, 0, 6, 1);
		auto back = world.addRoom("Back", 1, 1, 0, 6, 1);
		auto sector = reverse ? back : front;
		core::World::CreateDoorOptions options;
		if (test.opening == .9f) options.height = core::Door::Height::Tall;
		else options.heightScale = test.opening / CORE_DOOR_HEIGHT;
		world.addSectorDoor(0, 1, 2, options);
		auto path = catalogue(context, test.support, test.pose);
		world.attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
		require(world.placeFurniture(sector, "support", 2, 0, "Support") != 0, "Support placement refused");
		world.finishBuild();
		auto marker = world.furniture().front().destinations.front().marker;
		auto id = world.createAgent("Body", sector, 0, 2.5f);
		auto agent = world.lookupAgent(id).entity;
		world.pauseSimulation();
		require(world.setAgentIndividualHeightModifier(id, test.modifier), "Diagnostic Height refused");
		// A one-shot Action supplies a pose/claim without a Furniture-use
		// departure callback. The transaction driver retains it at the threshold.
		auto actions = context.temporaryRoot() / "clearance.actions.lua";
		write(actions, "return {api_version=1,uuid='" + std::string(uuid) + "',actions={{"
			"key='pose',name='Pose',run=function(a,w,m) w.set_pose('"
			+ std::string(core::poseName(test.pose)) + "'); "
			+ (decorated ? "w.claim(); " : "") + "end}}}");
		auto action = std::string(uuid) + ":pose";
		require(world.selectActionRegistry(actions) && world.setMarkerActions(marker, {action}), "Pose fixture registry refused");
		require(world.resumeSimulation(), "Diagnostic resume refused");
		arrive(world, id, marker, action);
		auto edge = doorEdge(world);
		auto source = edge->getVertex(0)->getSector()->getIndex() == sector ? edge->getVertex(0) : edge->getVertex(1);
		auto target = edge->getOtherVertex(source);
		// Start the real Agent locomotion while it is already at the threshold.
		// Path start clears generic poses, so the diagnostic dispatches the
		// authored Action at the waiting boundary rather than editing fields.
		agent->setPath(smoke::twoNodePath(source, target, edge), true);
		core::DoorClearanceDiagnostic::poseAtThreshold(world, id, marker, action);
		auto policy = world.getRouteChoicePolicy();
		core::RouteDecisionContext retained{agent, policy.baselineProfile, policy, agent->getSector(),
			agent->getWalkSpeed(), &world, agent->getClimbSpeed(), false, 0, 0, {}, 0, false};
		auto direct = edge->getDirectedTraversalFacts(target, retained);
		auto captured = core::RouteTraversalInputs::capture(*edge, target, retained).evaluate(retained);
		require(direct.feasible == test.fits && captured.feasible == test.fits, std::string(test.label) + ": planning disagrees");
		if (!test.fits) require(direct.exclusionReason == core::RouteExclusionReason::Clearance, "Wrong retained exclusion");
		auto envelope = agent->getDoorClearanceExtent();
		auto decorativeOffset = agent->getPoseRenderYOffset();
		bool lost = false, replanned = false, observedCrossing = false;
		for (unsigned tick = 0; tick < 600; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				lost |= event.type == core::SimulationEventType::RouteLost;
			if (agent->getState() == core::Agent::State::TraversingEdge)
			{
				observedCrossing = true;
				require(agent->getPose() == test.pose,
					"Diagnostic Pose was cleared before actual crossing");
			}
			if (agent->getState() == core::Agent::State::RoutePlanning)
			{
				replanned = true;
				// A new ordinary journey stands up, so stop the retained-envelope
				// diagnostic at its admission refusal. Movement-reset tests below
				// separately exercise the retry's real routing and lifecycle.
				agent->clearPath();
				break;
			}
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		bool crossed = agent->getSector() == target->getSector().get();
		require(observedCrossing == test.fits && (test.fits || replanned || lost) && crossed == test.fits
			&& (agent->getSector() == target->getSector().get()) == test.fits,
			std::string(test.label) + ": World admission/arrival disagrees with planning: crossed="
			+ std::to_string(crossed) + " lost=" + std::to_string(lost)
			+ " pose=" + std::to_string(static_cast<int>(agent->getPose()))
			+ " state=" + std::to_string(static_cast<int>(agent->getState())));
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalPermits.empty() && snapshot.traversalRequests.empty(), "Diagnostic ownership leaked");
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[clearance] " << test.label << " direction=" << (reverse ? "back-front" : "front-back")
			<< " support=" << test.support << " decorative-y=" << decorativeOffset
			<< " height-modifier=" << test.modifier << " top-above-floor=" << envelope << " opening=" << test.opening
			<< " planning=" << (direct.feasible ? "fit" : "refused")
			<< " outcome=" << (crossed ? "crossed/arrived" : "refused/route-planning") << '\n';
	}
	for (auto literal : {"-0.01", "0/0", "1/0", "'not-a-height'"})
	{
		bool refused = false;
		try { (void)core::FurnitureCatalogue::readFile(catalogue(context, 0.f, core::Pose::Standing, literal)); }
		catch (std::exception const&) { refused = true; }
		require(refused, std::string("Invalid support elevation accepted: ") + literal);
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[clearance] invalid-support=" << literal << " outcome=refused\n";
	}
}

void runLiveDoorClearance(smoke::Context const&)
{
	for (int change = 0; change != 3; ++change)
	for (bool queued : {false, true})
	for (bool alternate : {false, true})
	for (bool reverse : {false, true})
	for (auto mode : {core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
		core::DoorActivationMode::RemoteControlled})
	for (auto style : {core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
		core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart})
	{
		core::World world("Live clearance", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		auto origin = reverse ? back : front, destination = reverse ? front : back;
		core::World::CreateDoorOptions options;
		options.heightScale = .72f;
		options.speedOverride = .1f;
		options.activationMode = mode;
		options.openStyle = style;
		options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
		auto low = world.addSectorDoor(0, 0, 2, options);
		if (alternate) world.addSectorDoor(0, 0, 9, {});
		world.addSectorMarker(destination, 0, 3.5f, "Goal");
		world.finishBuild();
		auto id = world.createAgent("Traveller", origin, 0, queued ? 2.5f : .5f);
		auto agent = world.lookupAgent(id).entity;
		world.pauseSimulation();
		auto registry = core::AgentTagRegistry::create();
		auto tag = registry->addAgentTag("height");
		require(registry->addAgentTagHeightModifier(tag)
			&& registry->setAgentTagHeightModifier(tag, {.7f, .7f}), "Live tag fixture failed");
		world.attachAgentTagRegistry("live.tags.yaml", registry);
		require(world.assignAgentTag(id, tag), "Live tag assignment failed");
		require(world.resumeSimulation(), "Live resume failed");
		require(world.moveAgentToMarker(id, world.getMarkerIds().front()).accepted(), "Live goal refused");
		bool selected = false;
		for (unsigned tick = 0; tick < 600; ++tick)
		{
			world.advanceTick();
			if (agent->getPath() && (!queued || agent->getState() == core::Agent::State::WaitingForTraversal))
			{ selected = true; break; }
		}
		require(selected, "Live route was not selected before edit");
		if (change == 0)
		{
			world.pauseSimulation();
			require(world.setSectorDoorHeightScale(0, 0, 2, 1, .6f), "Idle height edit refused");
			require(world.resumeSimulation(), "Height edit resume failed");
		}
		else
		{
			world.pauseSimulation();
			if (change == 1) require(world.setAgentIndividualHeightModifier(id, 1.f), "Live individual edit failed");
			else require(registry->setAgentTagHeightModifier(tag, {1.f, 1.f}), "Live tag edit failed");
			require(world.resumeSimulation(), "Envelope edit resume failed");
		}
		bool lost = false, crossedLow = false, planning = false;
		for (unsigned tick = 0; tick < 6000; ++tick)
		{
			world.advanceTick();
			planning |= agent->getState() == core::Agent::State::RoutePlanning;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::RouteLost)
				{
					lost = true;
					require(event.routeLossReason == core::RouteLossReason::Unreachable
						|| event.routeLossReason == core::RouteLossReason::TopologyChanged, "Wrong live Route loss");
				}
			for (auto const& request : world.getSimulationSnapshot().traversalRequests)
				if (request.resource == low.traversalResource && request.state == core::TraversalRequestState::Granted)
					crossedLow = true;
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		require(planning && !crossedLow && lost == !alternate
			&& (agent->getSector() == world.getSector(destination).get()) == alternate,
			"Live clearance did not replan safely: change=" + std::to_string(change));
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[live-clearance] change=" << change
			<< " queued=" << queued << " reverse=" << reverse << " mode=" << int(mode) << " style=" << int(style)
			<< " outcome=" << (alternate ? "alternate/arrived" : "route-loss") << '\n';
	}
}

void runCommittedDoorEnvelope(smoke::Context const& context)
{
	for (int change = 0; change != 3; ++change)
	for (bool reverse : {false, true})
	{
		core::World world("Committed envelope", 6, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 6, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 6, 1);
		auto origin = reverse ? back : front;
		core::World::CreateDoorOptions options;
		if (change == 2) options.height = core::Door::Height::Tall;
		else options.heightScale = .28f / CORE_DOOR_HEIGHT;
		world.addSectorDoor(0, 0, 2, options);
		auto path = catalogue(context, change == 2 ? .64f : .03f, core::Pose::Sitting);
		world.attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
		require(world.placeFurniture(origin, "support", 2, 0, "Support") != 0
			&& world.placeFurniture(reverse ? front : back, "support", 2, 0, "Other support") != 0, "Committed support fixture failed");
		world.finishBuild();
		auto marker = world.furniture().front().destinations.front().marker;
		auto otherMarker = world.furniture().back().destinations.front().marker;
		auto id = world.createAgent("Body", origin, 0, 2.5f);
		auto agent = world.lookupAgent(id).entity;
		world.pauseSimulation();
		auto actions = context.temporaryRoot() / "committed.actions.lua";
		write(actions, "return {api_version=1,uuid='" + std::string(uuid) + "',actions={{"
			"key='sit',name='Sit',run=function(a,w,m) w.set_pose('sitting') end},{"
			"key='change',name='Change',run=function(a,w,m) w.set_pose('"
			+ (change == 0 ? "lying" : "sitting") + "'); " + (change == 0 ? "" : "w.claim(); ") + "end}}}");
		auto sit = std::string(uuid) + ":sit", changed = std::string(uuid) + ":change";
		require(world.selectActionRegistry(actions) && world.setMarkerActions(marker, {sit, changed})
			&& world.setMarkerActions(otherMarker, {sit, changed}), "Committed Action fixture failed");
		require(world.resumeSimulation(), "Committed resume failed");
		auto edge = doorEdge(world);
		auto source = edge->getVertex(0)->getSector()->getIndex() == origin ? edge->getVertex(0) : edge->getVertex(1);
		auto target = edge->getOtherVertex(source);
		agent->setPath(smoke::twoNodePath(source, target, edge), true);
		core::DoorClearanceDiagnostic::poseAtThreshold(world, id, marker, sit);
		bool admitted = false;
		for (unsigned tick = 0; tick < 600; ++tick)
		{
			world.advanceTick();
			if (agent->getState() == core::Agent::State::TraversingEdge) { admitted = true; break; }
		}
		require(admitted, "Fitting retained envelope was not admitted");
		core::DoorClearanceDiagnostic::poseAtThreshold(world, id, marker, changed);
		auto door = std::static_pointer_cast<core::DoorEdge const>(edge)->getDoor();
		require(!door->admitsVerticalExtent(agent->getTraversalDoorClearanceExtent(), source->getPosition().y),
			"Changed envelope still fits diagnostic aperture");
		for (unsigned tick = 0; tick < 600 && agent->getState() != core::Agent::State::Idle; ++tick) world.advanceTick();
		require(agent->getState() == core::Agent::State::Idle && agent->getSector() == target->getSector().get(),
			"Admitted Pose/support enlargement interrupted crossing");
		// The commitment is over: the same oversized envelope must not be
		// granted a subsequent crossing, even on an externally supplied Path.
		agent->setPath(smoke::twoNodePath(target, source, edge), true);
		core::DoorClearanceDiagnostic::poseAtThreshold(world, id, otherMarker, changed);
		world.advanceTick();
		require(agent->getState() == core::Agent::State::RoutePlanning
			&& world.getSimulationSnapshot().traversalPermits.empty(), "Subsequent oversized crossing was admitted");
		agent->clearPath();
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[live-clearance] admitted-change=" << change
			<< " reverse=" << reverse << " outcome=completed/subsequent-refused\n";
	}
}

void runPoseDoorMovementReset(smoke::Context const& context)
{
	for (bool lying : {false, true})
	for (bool alternate : {false, true})
	{
		core::World world("Furniture departure prediction", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
		core::World::CreateDoorOptions options;
		options.heightScale = .84f; // .42 fits Sitting and Lying, not Standing.
		auto low = world.addSectorDoor(0, 0, 4, options);
		if (alternate) world.addSectorDoor(0, 0, 9, {});
		auto path = catalogue(context, .01f, lying ? core::Pose::Lying : core::Pose::Sitting);
		world.attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
		require(world.placeFurniture(front, "support", 2, 0, "Chair or bed") != 0, "Reset support refused");
		world.addSectorMarker(back, 0, 6.5f, "Destination");
		world.finishBuild();
		auto seat = world.furniture().front().destinations.front().marker;
		auto id = world.createAgent("Departing", front, 0, 2.5f);
		auto agent = world.lookupAgent(id).entity;
		arrive(world, id, seat, std::string(core::UseFurnitureAction));
		require(agent->getPose() == (lying ? core::Pose::Lying : core::Pose::Sitting)
			&& world.usablePointOccupant(seat) == id, "Furniture use did not pose and claim");
		require(agent->getDoorClearanceExtent() < .42f && agent->getTraversalDoorClearanceExtent(true) > .42f,
			"Departure prediction exploited temporary pose/support");
		require(world.moveAgentToNamedMarker(id, "Destination").accepted(), "Departure intent refused");
		bool lost = false, crossedLow = false;
		for (unsigned tick = 0; tick < 3000; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				lost |= event.type == core::SimulationEventType::RouteLost;
			for (auto const& permit : world.getSimulationSnapshot().traversalPermits)
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					if (permit.request == request.id && request.resource == low.traversalResource) crossedLow = true;
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		require(!crossedLow && lost == !alternate && (agent->getSector() == world.getSector(back).get()) == alternate,
			"Movement used temporary pose through low Door: alternate=" + std::to_string(alternate)
			+ " lying=" + std::to_string(lying) + " lost=" + std::to_string(lost)
			+ " low=" + std::to_string(crossedLow) + " state=" + std::to_string(static_cast<int>(agent->getState()))
			+ " x=" + std::to_string(agent->getGlobalPosition().x));
		require(agent->getPose() == (alternate ? core::Pose::Standing : lying ? core::Pose::Lying : core::Pose::Sitting)
			&& (world.usablePointOccupant(seat) == id) == !alternate,
			"Departure lifecycle released too early or failed to release on departure");
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[clearance] movement-reset pose=" << (lying ? "lying" : "sitting")
			<< " outcome=" << (alternate ? "stood/alternate/arrived/released" : "route-loss/use-retained") << '\n';
	}
}
