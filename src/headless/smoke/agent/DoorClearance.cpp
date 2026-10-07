#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/DoorEdge.h"
#include "core/RouteTraversalInputs.h"
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

	std::filesystem::path catalogue(smoke::Context const& context, float support, bool lying,
		std::string const& supportLiteral = {})
	{
		auto path = context.temporaryRoot() / "clearance.furniture.lua";
		write(path, "return {api_version=1,uuid='" + std::string(uuid) + "',definitions={{"
			"key='support',label='Support',tiles={{x=0,y=0,imageSet='ObjectAtlas',image='chair'}},"
			"usablePoints={{key='body',label='Body',x=0.5,blocksPathing=false,supportElevation="
			+ (supportLiteral.empty() ? std::to_string(support) : supportLiteral) + "}},use=function(a,w,m) w.set_pose('"
			+ (lying ? "lying" : "sitting") + "'); w.claim() end,"
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
	struct Case { char const* label; bool lying; float support, opening; bool fits; float modifier = 1.f; };
	// .45 Standing, .27 Sitting, .40 rotated Lying. Support is physical;
	// getPoseRenderYOffset() (.25 for claimed Lying) is decorative.
	for (auto const& test : {
		Case{"sitting-fit", false, 0.f, .28f, true},
		Case{"sitting-raised-refusal", false, .03f, .28f, false},
		Case{"sitting-exact-fit", false, .01f, .28f, true},
		Case{"sitting-within-tolerance", false, .010005f, .28f, true},
		Case{"sitting-over-tolerance", false, .01002f, .28f, false},
		Case{"lying-rotated-fit", true, 0.f, .41f, true},
		Case{"lying-raised-refusal", true, .03f, .41f, false},
		Case{"lying-exact-fit", true, .01f, .41f, true},
		Case{"lying-over-tolerance", true, .01002f, .41f, false},
		Case{"lying-decorative-refusal", true, 0.f, .39f, false},
		Case{"sitting-modifier-exact-fit", false, .01f, .199f, true, .7f},
		Case{"sitting-modifier-refusal", false, .01002f, .199f, false, .7f},
		Case{"lying-modifier-width-unchanged", true, 0.f, .39f, false, .7f},
		Case{"tall-sitting-fit", false, .62f, .9f, true},
		Case{"tall-sitting-refusal", false, .64f, .9f, false},
		Case{"tall-lying-fit", true, .49f, .9f, true},
		Case{"tall-lying-refusal", true, .51f, .9f, false}})
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
		auto path = catalogue(context, test.support, test.lying);
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
			+ (test.lying ? "lying" : "sitting") + "'); "
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
		world.advanceTick();
		require(agent->getState() == core::Agent::State::WaitingForTraversal, "Diagnostic actor did not reach threshold");
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
				require(agent->getPose() == (test.lying ? core::Pose::Lying : core::Pose::Sitting),
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
		try { (void)core::FurnitureCatalogue::readFile(catalogue(context, 0.f, false, literal)); }
		catch (std::exception const&) { refused = true; }
		require(refused, std::string("Invalid support elevation accepted: ") + literal);
		if (std::getenv("PF_DOOR_CLEARANCE_TRACE")) std::cout << "[clearance] invalid-support=" << literal << " outcome=refused\n";
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
		auto path = catalogue(context, .01f, lying);
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
