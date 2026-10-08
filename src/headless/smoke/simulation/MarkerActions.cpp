#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/MobilityProfile.h"
#include <cmath>
#include <fstream>

namespace
{
	using smoke::require;
	std::string const uuid = "ad603358-5ebf-45bb-a686-c3f491152c61";
	std::string const first = uuid + ":hello", second = uuid + ":other";

	void write(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream file(path); file << source;
		require(static_cast<bool>(file), "Cannot write Action fixture");
	}
	std::string package(std::string const& body)
	{
		return "return {api_version=1, uuid='" + uuid + "', actions={" + body + "}}";
	}
	std::shared_ptr<core::World> worldFixture()
	{
		auto world = std::make_shared<core::World>("Actions", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 6.5f, "Target");
		world->addSectorMarker(room, 0, 8.5f, "End");
		world->finishBuild();
		world->createAgent("First", room, 0, 1.5f);
		world->createAgent("Second", room, 0, 1.5f);
		world->pauseSimulation();
		for (uint64_t id : {1u,2u})
		{
			require(world->setAgentIndividualMinimumRoutePlanningTime(core::AgentId{id}, 0.1f), "Planning minimum refused");
			require(world->setAgentIndividualMaximumRoutePlanningTime(core::AgentId{id}, 0.1f), "Planning maximum refused");
		}
		return world;
	}

	std::shared_ptr<core::World> useFixture(std::filesystem::path const& path, float startX = 1.5f)
	{
		auto world = std::make_shared<core::World>("Furniture use", 14, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 11, 1);
		auto isolated = world->addRoom("Isolated", 0, 0, 12, 2, 1);
		world->attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
		require(world->placeFurniture(room, "chair", 3, 0, "Chair")
			&& world->placeFurniture(room, "sofa", 6, 0, "Sofa")
			&& world->placeFurniture(room, "desk", 9, 0, "Desk"), "Use fixture placement refused");
		world->addSectorMarker(room, 0, 1.5f, "Origin");
		world->addSectorMarker(room, 0, 10.5f, "End");
		world->addSectorMarker(isolated, 0, 0.5f, "Unreachable");
		world->finishBuild();
		for (auto const& name : {"First", "Second", "Third"}) world->createAgent(name, room, 0, startX);
		world->pauseSimulation();
		for (uint64_t id : {1u, 2u, 3u})
			require(world->setAgentIndividualMinimumRoutePlanningTime(core::AgentId{id}, 0.1f)
				&& world->setAgentIndividualMaximumRoutePlanningTime(core::AgentId{id}, 0.1f), "Use planning refused");
		return world;
	}

	void transactionalReload(smoke::Context const& context)
	{
		auto root = context.temporaryRoot();
		auto actions = root / "reload.actions.lua";
		write(actions, package("{key='hello',name='Old',run=function(a,w,m) w.log('old-action') end}"));
		auto world = worldFixture();
		auto marker = world->getMarkerIds()[0];
		require(world->selectActionRegistry(actions) && world->setMarkerActions(marker, {first}), "Reload setup refused");
		world->saveTo((root / "reload.world.yaml").string());
		auto old = world->actionRegistry();
		std::string diagnostic;
		for (auto const& source : {std::string("return {}"), package(""),
			std::string("return {api_version=1,uuid='00000000-0000-4000-8000-000000000000',actions={}}")})
		{
			write(actions, source);
			require(!world->reloadActionRegistry(actions, &diagnostic) && !diagnostic.empty()
				&& world->actionRegistry() == old && world->markerActions(marker) == std::vector<std::string>{first}
				&& !world->isModified(), "Failed Action preflight changed package/references/modified state");
		}
		write(actions, package("{key='hello',name='New',run=function(a,w,m) w.log('new-action') end}"));
		require(world->moveAgentToMarker(core::AgentId{1}, marker, first).accepted(), "Pending reload request refused");
		require(world->resumeSimulation(), "Reload running setup refused");
		require(!world->reloadActionRegistry(actions, &diagnostic) && world->actionRegistry() == old, "Running reload changed package");
		world->pauseSimulation();
		require(world->reloadActionRegistry(actions, &diagnostic) && diagnostic.empty()
			&& world->agentActionDisplayName(first) == "New" && !world->isModified(), diagnostic);
		core::consumeLogMessages();
		require(world->resumeSimulation() && world->advanceTicks(1800), "Reloaded request failed");
		bool logged = false;
		for (auto const& log : core::consumeLogMessages()) logged |= log.msg == "new-action";
		require(logged, "Stable pending request did not execute replacement Action");

		auto path = root / "reload.furniture.lua";
		std::ifstream input(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
		std::string source{std::istreambuf_iterator<char>(input), {}};
		write(path, source);
		auto furnished = useFixture(path);
		auto seat = furnished->furniture()[0].marker;
		auto sofa = furnished->furniture()[1].marker;
		require(furnished->moveAgentToMarker(core::AgentId{1}, seat, core::UseFurnitureAction).accepted()
			&& furnished->resumeSimulation() && furnished->advanceTicks(1800), "Reload use setup failed");
		furnished->pauseSimulation();
		require(furnished->selectActionRegistry(actions), "Furniture Action registry selection refused");
		core::consumeLogMessages();
		require(furnished->reloadActionRegistry(actions, &diagnostic)
			&& furnished->usablePointOccupant(seat) == core::AgentId{1}
			&& furnished->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Sitting,
			"Action reload changed an unaffected Furniture use");
		for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("finish:"), "Action reload finished unaffected use");
		furnished->saveTo((root / "furnished.world.yaml").string());
		auto catalogue = furnished->furnitureCatalogue();
		auto position = furnished->lookupAgent(core::AgentId{1}).entity->getGlobalPosition();
		auto replace = [](std::string text, std::string const& from, std::string const& to) {
			auto at = text.find(from); require(at != std::string::npos, "Reload fixture substitution missing");
			text.replace(at, from.size(), to); return text;
		};
		for (auto const& invalid : {std::string("return {}"),
			replace(source, "a1a1a1a1-1111-4111-8111-111111111111", "00000000-0000-4000-8000-000000000000"),
			replace(source, "key='seat'", "key='missing'"),
			replace(source, "x=0.5,blocksPathing=false", "x=99,blocksPathing=false"),
			replace(source, "{x=0,y=0,imageSet='ObjectAtlas',image='chair'}", "{x=-4,y=0,imageSet='ObjectAtlas',image='chair'}")})
		{
			write(path, invalid); core::consumeLogMessages();
			require(!furnished->reloadFurnitureCatalogue(path, &diagnostic) && !diagnostic.empty()
				&& furnished->furnitureCatalogue() == catalogue && furnished->usablePointOccupant(seat) == core::AgentId{1}
				&& furnished->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Sitting
				&& furnished->lookupAgent(core::AgentId{1}).entity->getGlobalPosition() == position
				&& !furnished->isModified(), "Failed Furniture preflight changed live state");
			for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("finish:"), "Preflight executed finish");
		}
		// Changed callbacks must not be used to finish an old use.
		auto changed = replace(source, "world.log('finish:'", "world.log('new-finish:'");
		changed = replace(changed, "world.log('use:'", "world.log('new-use:'");
		write(path, changed); core::consumeLogMessages();
		require(furnished->moveAgentToMarker(core::AgentId{2}, sofa, core::UseFurnitureAction).accepted(), "Pending Furniture request refused");
		require(furnished->reloadFurnitureCatalogue(path, &diagnostic), diagnostic);
		bool finished = false;
		for (auto const& log : core::consumeLogMessages())
		{
			finished |= log.msg == "finish:Chair Seat";
			require(!log.msg.starts_with("new-finish:"), "Reload finished with replacement functions");
		}
		require(finished && !furnished->usablePointOccupant(seat)
			&& furnished->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Standing
			&& furnished->lookupMarker(seat)->getName() == "Chair Seat", "Reload lost cleanup or stable Marker identity");
		// Reconciliation also accepts valid geometry edits without changing owned identities.
		write(path, replace(changed, "x=0.5,blocksPathing=false", "x=0.75,blocksPathing=false"));
		require(furnished->reloadFurnitureCatalogue(path, &diagnostic)
			&& furnished->furniture()[0].marker == seat, "Valid layout reload lost Marker identity: " + diagnostic);
		require(furnished->setMarkerActions(seat, {std::string(core::UseFurnitureAction)}), "Explicit use assignment refused");
		auto accepted = furnished->furnitureCatalogue();
		write(path, replace(changed, "use_pose = \"sitting\", finish_use_pose = \"standing\", use = use, finish_use = finish", ""));
		require(!furnished->reloadFurnitureCatalogue(path, &diagnostic) && !diagnostic.empty()
			&& furnished->furnitureCatalogue() == accepted, "Reload invalidated authored Use furniture assignment");
		require(furnished->setMarkerActions(seat, {}), "Explicit use clear refused");
		require(furnished->resumeSimulation() && furnished->advanceTicks(1800), "Post-reload use failed");
		require(furnished->usablePointOccupant(sofa) == core::AgentId{2}, "Reload lost unaffected pending request");
		furnished->pauseSimulation();
		// Remove use without explicit assignment: pending requests cancel, not Idle.
		require(furnished->moveAgentToMarker(core::AgentId{3}, seat, core::UseFurnitureAction).accepted(), "Invalidation request refused");
		write(path, replace(changed, "use_pose = \"sitting\", finish_use_pose = \"standing\", use = use, finish_use = finish", ""));
		require(furnished->reloadFurnitureCatalogue(path, &diagnostic), diagnostic);
		require(furnished->resumeSimulation() && furnished->advanceTicks(60), "Invalidation processing failed");
		bool cancelled = false;
		for (auto const& event : furnished->consumeSimulationEvents())
			cancelled |= event.type == core::SimulationEventType::MovementCancelled
				&& event.movementCancellationReason == core::MovementCancellationReason::ActionUnavailable
				&& event.selectedAction == core::UseFurnitureAction;
		require(cancelled, "Reload did not explicitly cancel invalidated request");

		// Old finishing failure keeps the old package installed, but always cleans claims/Pose.
		for (auto const& finish : {std::string("error('old finishing failed')"), std::string("error('incomplete')")})
		{
			auto failing = replace(source, "world.release()", finish);
			write(path, failing);
			auto failureWorld = useFixture(path);
			auto target = failureWorld->furniture()[0].marker;
			require(failureWorld->moveAgentToMarker(core::AgentId{1}, target, core::UseFurnitureAction).accepted()
				&& failureWorld->resumeSimulation() && failureWorld->advanceTicks(1800), "Failure reload setup failed");
			failureWorld->pauseSimulation(); auto before = failureWorld->furnitureCatalogue(); write(path, source);
			require(!failureWorld->reloadFurnitureCatalogue(path, &diagnostic) && !diagnostic.empty()
				&& failureWorld->furnitureCatalogue() == before && !failureWorld->usablePointOccupant(target)
				&& failureWorld->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Standing,
				"Finishing failure installed package or stranded user");
			require(failureWorld->resumeSimulation() && !failureWorld->advanceTick(), "Finishing failure was suppressed");
		}
	}

	core::SimulationEvent runAction(core::World&, core::AgentId, core::MarkerId, std::string const&);

	void furniturePoseRequirements(smoke::Context const& context)
	{
		std::ifstream input(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
		std::string source{std::istreambuf_iterator<char>(input), {}};
		auto replace = [](std::string text, std::string const& from, std::string const& to)
		{
			auto at = text.find(from); require(at != std::string::npos, "Pose fixture substitution missing");
			text.replace(at, from.size(), to); return text;
		};
		auto path = context.temporaryRoot() / "requirements.furniture.lua";
		for (auto const& mutation : {std::pair{"use_pose = \"sitting\", ", ""},
			std::pair{"finish_use_pose = \"standing\", ", ""},
			std::pair{"use_pose = \"sitting\"", "use_pose = 'unknown'"},
			std::pair{"use_pose = \"sitting\"", "use_pose = {}"},
			std::pair{"finish_use = finish", "finish_use = false"}})
		{
			write(path, replace(source, mutation.first, mutation.second));
			bool refused = false;
			try { (void)core::FurnitureCatalogue::readFile(path); }
			catch (std::exception const& error) { refused = std::string(error.what()).find("Furniture") != std::string::npos; }
			require(refused, "Malformed Furniture posture contract accepted");
		}
		write(path, source);
		core::World world("Target pose requirements", 16, 2);
		auto origin = world.addRoom("Origin", 0, 0, 0, 8, 1);
		auto target = world.addRoom("Target", 0, 0, 8, 8, 1);
		world.attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
		require(world.placeFurniture(target, "chair", 2, 0, "Chair"), "Requirement chair placement failed");
		world.finishBuild();
		auto human = world.createAgent("Human", origin, 0, 1.5f);
		std::ifstream robotInput(context.fixture("resources/test-worlds/standing-robot.agent.lua"));
		std::string robotSource{std::istreambuf_iterator<char>(robotInput), {}};
		std::string diagnostic;
		require(world.attachAgentType("standing-robot.agent.lua", robotSource, &diagnostic), diagnostic);
		auto robot = world.createAgent("StandingRobot", "Robot", origin, 0, 2.5f);
		world.pauseSimulation();
		auto seat = world.furniture()[0].marker;
		require(world.furnitureUseEligible(human, seat, &diagnostic), diagnostic);
		require(!world.furnitureUseEligible(robot, seat, &diagnostic) && diagnostic.find("sitting") != std::string::npos,
			"Standing-only Robot offered chair use");
		require(!world.moveAgentToMarker(robot, seat, core::UseFurnitureAction).accepted(), "Incompatible request began travel");
		require(world.moveAgentToMarker(robot, seat).accepted(), "Incompatible use blocked Idle request");
		require(world.setRoomHeightScale(target, .4f), "Target height refused");
		require(!world.furnitureUseEligible(human, seat, &diagnostic) && diagnostic.find("finish") != std::string::npos,
			"Remote eligibility used origin geometry or ignored finish fit");
		require(!world.moveAgentToMarker(human, seat, core::UseFurnitureAction).accepted()
			&& world.moveAgentToMarker(human, seat).accepted(), "Finish fit became an Idle routing obstacle");
		require(world.setRoomHeightScale(target, .5f), "Target height restore failed");
		require(world.furnitureUseEligible(human, seat), "Exact Standing finish fit refused");
		require(world.setRoomHeightScale(target, .499995f)
			&& world.furnitureUseEligible(human, seat), "Within-tolerance finish fit refused");
		require(world.setRoomHeightScale(target, .49998f)
			&& !world.furnitureUseEligible(human, seat), "Over-tolerance finish fit admitted");
		// Physical support participates in use fit but is released for finish fit.
		for (auto const& boundary : {std::pair{.63f, true}, std::pair{.630005f, true}, std::pair{.63002f, false}})
		{
			write(path, replace(source, "x=0.5,blocksPathing=false", "x=0.5,blocksPathing=false,supportElevation=" + std::to_string(boundary.first)));
			auto supportedWorld = useFixture(path);
			require(supportedWorld->furnitureUseEligible(core::AgentId{1}, supportedWorld->furniture()[0].marker) == boundary.second,
				"Supported use fit did not apply shared tolerance");
		}
		// Arrival rechecks geometry/effective dimensions; stale acceptance publishes nothing.
		write(path, source);
		auto stale = useFixture(path);
		auto staleSeat = stale->furniture()[0].marker;
		require(stale->moveAgentToMarker(core::AgentId{1}, staleSeat, core::UseFurnitureAction).accepted(), "Stale setup refused");
		require(stale->setRoomHeightScale(0, .4f), "Stale target height modification refused");
		core::consumeLogMessages();
		require(stale->resumeSimulation() && stale->advanceTicks(1800), "Stale eligibility became a script failure");
		bool staleRefusal = false;
		for (auto const& event : stale->consumeSimulationEvents())
			staleRefusal |= event.type == core::SimulationEventType::ActionFailed && event.diagnostic.find("finish") != std::string::npos;
		require(staleRefusal && !stale->usablePointOccupant(staleSeat), "Stale arrival published Furniture use");
		for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("use:"), "Stale arrival ran callback");
		// Both phases are capability requirements, even when use itself is supported.
		write(path, replace(replace(source, "use_pose = \"sitting\"", "use_pose = 'standing'"),
			"finish_use_pose = \"standing\"", "finish_use_pose = 'sitting'"));
		auto finishWorld = useFixture(path);
		require(finishWorld->attachAgentType("standing-robot.agent.lua", robotSource, &diagnostic), diagnostic);
		auto finishRobot = finishWorld->createAgent("StandingRobot", "Finish Robot", 0, 0, 1.5f);
		require(!finishWorld->furnitureUseEligible(finishRobot, finishWorld->furniture()[0].marker, &diagnostic)
			&& diagnostic.find("sitting") != std::string::npos, "Unsupported finish capability ignored");
		// Finish posture and release are host-owned, not inferred from callback effects.
		write(path, replace(replace(source, "finish_use_pose = \"standing\"", "finish_use_pose = 'crouching'"),
			"world.release()", "world.log('host release')"));
		auto declaredFinish = useFixture(path);
		auto declaredSeat = declaredFinish->furniture()[0].marker;
		require(runAction(*declaredFinish, core::AgentId{1}, declaredSeat, std::string(core::UseFurnitureAction)).type
			== core::SimulationEventType::DestinationReached, "Declared finish setup failed");
		declaredFinish->pauseSimulation(); core::consumeLogMessages();
		require(declaredFinish->canEditFurniture(declaredFinish->furniture()[0].id, 4, 0, "Moved", &diagnostic)
			&& declaredFinish->canRemoveFurniture(declaredFinish->furniture()[0].id, &diagnostic), diagnostic);
		require(declaredFinish->usablePointOccupant(declaredSeat) == core::AgentId{1}
			&& declaredFinish->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Sitting,
			"Edit preflight published declared finish effects");
		for (auto const& log : core::consumeLogMessages()) require(log.msg != "host release", "Edit preflight executed callback");
		require(runAction(*declaredFinish, core::AgentId{1}, declaredSeat, std::string(core::IdleAction)).type
			== core::SimulationEventType::DestinationReached && !declaredFinish->usablePointOccupant(declaredSeat)
			&& declaredFinish->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Crouching,
			"Host substituted Standing or required callback release");
		// Even caught same-pose calls violate the declarative contract and roll back.
		for (auto body : {"world.set_pose('sitting')", "pcall(function() world.set_pose('sitting') end)"})
		{
			write(path, replace(source, "world.claim()", std::string("world.claim(); world.log('pose rollback'); ") + body));
			auto failedWorld = useFixture(path);
			auto marker = failedWorld->furniture()[0].marker;
			core::consumeLogMessages();
			auto event = runAction(*failedWorld, core::AgentId{1}, marker, std::string(core::UseFurnitureAction));
			require(event.type == core::SimulationEventType::ActionFailed && event.diagnostic.find("set_pose") != std::string::npos
				&& !failedWorld->usablePointOccupant(marker)
				&& failedWorld->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Standing,
				"Forbidden same-pose call leaked posture/occupancy");
			for (auto const& log : core::consumeLogMessages()) require(log.msg != "pose rollback", "Forbidden pose call published logs");
		}
	}

	void registryContracts(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "contract.actions.lua";
		std::vector<std::string> invalid{
			"return nil", "return {}", "while true do end", "return io.open('x')",
			"return {api_version=2, uuid='" + uuid + "', actions={}}",
			"return {api_version=1, uuid='bad', actions={}}",
			package("{key='a',name='A'}"), package("{key='a',name='A',run=1}"),
			package("{key='idle',name='Custom',run=function() end}"),
			package("{key='use-furniture',name='Custom',run=function() end}"),
			package("{key='a',name='Idle',run=function() end}"),
			package("{key='a',name='Use furniture',run=function() end}"),
			package("{key='a',name='A',run=function() end},{key='a',name='B',run=function() end}"),
			package("{key='a',name='A',run=function() end},{key='b',name='A',run=function() end}"),
			package("oops={key='a',name='A',run=function() end}"),
			"local count=0; " + package("{key='a',name='A',run=function() count=count+1 end}")
		};
		auto world = worldFixture();
		for (auto const& source : invalid)
		{
			write(path, source);
			std::string diagnostic;
			require(!world->selectActionRegistry(path, &diagnostic) && !diagnostic.empty(), "Malformed Action registry accepted without diagnostic");
			require(!world->actionRegistry(), "Rejected registry mutated World");
		}
		write(path, package("{key='hello',name='Hello',run=function() end},{key='other',name='Other',run=function() end}"));
		require(world->selectActionRegistry(path), "Valid Action registry refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker, {second, first, second, "idle"}), "Assignment refused");
		require(world->availableAgentActions(marker) == std::vector<std::string>{"idle",second,first}, "Assignment order/deduplication changed");
		require(!world->setMarkerActions(marker, {first, "missing"}), "Dangling Action accepted");
		require(world->markerActions(marker) == std::vector<std::string>{second,first}, "Rejected edit mutated assignments");
		require(world->moveAgentToMarker(core::AgentId{1}, world->getMarkerIds()[1], first).status == core::MovementCommandStatus::UnavailableAction, "Unassigned Action accepted");
		require(world->moveAgentToMarker(core::AgentId{1}, marker, core::UseFurnitureAction).status == core::MovementCommandStatus::UnavailableAction, "Deferred Furniture use was activated");
		write(path, "return nil");
		require(!world->selectActionRegistry(path), "Same-reference live reload accepted");
		require(world->agentActionDisplayName(first) == "Hello", "Loaded immutable source replaced");
	}

	void execution(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "execute.actions.lua";
		write(path, package(R"lua({key='hello',name='Hello',run=function(agent, world, marker)
assert(world.api_version == 1 and require('promethium.actions.v1').api_version == 1)
assert(agent.graph == nil and world.graph == nil and marker.vertex == nil)
assert(agent.set_pose == nil and type(world.claim) == 'function')
assert(io == nil and os == nil and debug == nil and coroutine == nil and load == nil)
assert(math.random == nil and package == nil)
assert(not pcall(function() agent.name='changed' end))
assert(not pcall(function() marker.id=0 end))
assert(not pcall(function() world.tick=0 end))
assert(not pcall(function() require('unsafe') end))
count = (count or 0) + 1
assert(count == 1)
world.log(agent.name .. ':' .. marker.name .. ':' .. count)
end},{key='other',name='Other',run=function(a,w,m) w.log('other') end})lua"));
		std::vector<std::string> runs;
		for (int run = 0; run < 2; ++run)
		{
			auto world = worldFixture();
			require(world->selectActionRegistry(path), "Execution registry refused");
			auto marker = world->getMarkerIds()[0];
			require(world->setMarkerActions(marker, {second,first}), "Execution assignment refused");
			core::consumeLogMessages();
			for (uint64_t id : {1u,2u})
			{
				require(world->moveAgentToNamedMarker(core::AgentId{id}, "Target", first).accepted(), "Custom request refused");
				require(world->moveAgentToMarker(core::AgentId{id}, marker, first).status == core::MovementCommandStatus::NoOp, "Duplicate request not idempotent");
			}
			require(core::consumeLogMessages().empty(), "Action ran at acceptance");
			require(world->renameMarker(marker, "Renamed"), "Rename refused");
			require(world->resumeSimulation(), "Resume refused");
			int arrivals = 0;
			std::string trace;
			for (int tick = 0; tick < 1800 && arrivals < 2; ++tick)
			{
				require(world->advanceTick(), "Action execution failed");
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						require(event.selectedAction == first && event.destinationMarker == marker, "Wrong selected Action/target executed");
						require(event.agent.globalPosition.x == 6.5f, "Action ran before physical arrival");
						trace += std::to_string(event.agent.id.value) + ":" + std::to_string(event.tick) + ";";
						++arrivals;
					}
			}
			require(arrivals == 2, "Independent instances did not arrive");
			auto logs = core::consumeLogMessages();
			std::vector<std::string> messages;
			for (auto const& log : logs) if (log.source == "Marker Action") messages.push_back(log.msg);
			require(messages == std::vector<std::string>{"First:Renamed:1","Second:Renamed:1"}, "Action order, isolation or selected callback changed");
			trace += messages[0] + messages[1];
			runs.push_back(trace);
			world->pauseSimulation();
			require(world->moveAgentToMarker(core::AgentId{1}, marker, first).accepted(), "Repeated arrival refused");
			require(world->setMarkerActions(marker, {second}), "Removal refused");
			require(world->setMarkerActions(marker, {second,first}), "Reassignment refused");
			require(world->resumeSimulation(), "Removal resume refused");
			bool cancelled = false;
			for (int tick = 0; tick < 20 && !cancelled; ++tick)
			{
				require(world->advanceTick(), "Removal cancellation failed");
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::MovementCancelled)
					{
						cancelled = true;
						require(event.movementCancellationReason == core::MovementCancellationReason::ActionUnavailable
							&& !event.diagnostic.empty(), "Removal cancellation has no reason");
					}
			}
			require(cancelled, "Removed Action executed or silently became Idle");
			world->pauseSimulation();
			auto end = world->getMarkerIds()[1];
			require(world->setMarkerActions(end, {first}), "Travel assignment refused");
			require(world->moveAgentToMarker(core::AgentId{1}, end, first).accepted(), "Travel request refused");
			require(world->resumeSimulation() && world->advanceTicks(12), "Travel failed before removal");
			world->pauseSimulation();
			require(world->clearActionRegistry(), "Registry removal refused");
			require(world->resumeSimulation(), "Registry removal resume refused");
			cancelled = false;
			for (int tick = 0; tick < 60 && !cancelled; ++tick)
			{
				require(world->advanceTick(), "Travel cancellation failed");
				for (auto const& event : world->consumeSimulationEvents())
					cancelled |= event.type == core::SimulationEventType::MovementCancelled
						&& event.movementCancellationReason == core::MovementCancellationReason::ActionUnavailable;
			}
			require(cancelled, "Registry removal during travel did not cancel request");
		}
		require(runs[0] == runs[1], "Action execution is not deterministic across Worlds");
	}

	core::SimulationEvent runAction(core::World& world, core::AgentId agent, core::MarkerId marker, std::string const& action = first)
	{
		world.pauseSimulation();
		require(world.moveAgentToMarker(agent, marker, action).accepted(), "Effect request refused");
		world.consumeSimulationEvents();
		require(world.resumeSimulation(), "Effects resume refused");
		for (unsigned tick = 0; tick < 1800; ++tick)
		{
			auto advanced = world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				if (event.agent.id == agent && (event.type == core::SimulationEventType::DestinationReached
					|| event.type == core::SimulationEventType::ActionFailed || event.type == core::SimulationEventType::RouteLost))
					return event;
			require(advanced, "Effect execution failed without outcome");
		}
		throw std::runtime_error("Missing Action effect outcome");
	}

	void atomicEffects(smoke::Context const& context)
	{
		std::vector<std::string> bodies{
			"w.set_pose('sitting'); w.claim(); w.log('committed')",
			"w.set_pose('lying'); w.claim(); w.release(); w.set_pose('standing')",
			"w.set_pose('sitting'); w.log('rollback'); w.release()",
			"w.set_pose('lying'); w.claim(); w.log('rollback'); error('broken')",
			"w.set_pose('lying'); w.claim(); w.log('rollback'); while true do end",
			"w.set_pose('lying'); w.claim(); w.log('rollback'); local t={} while true do t[#t+1]=string.rep('x',10000) end"
		};
		for (size_t scenario = 0; scenario < bodies.size(); ++scenario)
		{
			core::World world("Atomic effects", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("sit.furniture.lua", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/sit.furniture.lua")));
			require(world.placeFurniture(room, "chair", 3, 0, "Chair") != 0, "Effects Furniture refused");
			world.finishBuild();
			auto marker = world.furniture()[0].destinations[0].marker;
			auto agent = world.createAgent("Operator", room, 0, 0.5f);
			world.pauseSimulation();
			auto path = context.temporaryRoot() / ("effects" + std::to_string(scenario) + ".actions.lua");
			write(path, package("{key='hello',name='Hello',run=function(a,w,m) " + bodies[scenario] + " end}"));
			require(world.selectActionRegistry(path) && world.setMarkerActions(marker, {first}), "Effects package refused");
			core::consumeLogMessages();
			auto event = runAction(world, agent, marker);
			if (scenario < 2)
			{
				require(event.type == core::SimulationEventType::DestinationReached, "Successful effects rejected");
				require(world.lookupAgent(agent).entity->getPose() == (scenario == 0 ? core::Pose::Sitting : core::Pose::Standing), "Wrong committed Pose");
				require(world.usablePointOccupant(marker) == (scenario == 0 ? agent : core::AgentId{}), "Wrong committed claim");
				if (scenario == 0)
				{
					auto other = world.createAgent("Contender", room, 0, 0.5f);
					auto competition = runAction(world, other, marker);
					require(competition.type == core::SimulationEventType::RouteLost || competition.type == core::SimulationEventType::ActionFailed,
						"Competing request bypassed occupied destination");
					require(competition.scriptFailure == core::ScriptExecutionFailure::None
						&& world.lookupAgent(other).entity->getPose() == core::Pose::Standing, "Competition crashed or partially posed Agent");
					require(world.usablePointOccupant(marker) == agent, "Competition stole claim");
				}
			}
			else
			{
				require(event.type == core::SimulationEventType::ActionFailed, "Failed batch reported success");
				require((event.scriptFailure == core::ScriptExecutionFailure::None) == (scenario == 2), "Ordinary release refusal became script crash");
				require(world.isSimulationPaused() == (scenario >= 3), "Wrong failure pause policy");
				require(world.lookupAgent(agent).entity->getPose() == core::Pose::Standing && !world.usablePointOccupant(marker), "Batch leaked Pose/claim");
				for (auto const& log : core::consumeLogMessages()) require(log.msg != "rollback", "Rejected batch logged");
			}
		}
	}

	void poseVocabulary(smoke::Context const& context)
	{
		auto near = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
		struct PoseCase { char const* name; core::Pose pose; float scale; };
		for (auto const& test : {
			PoseCase{"crouching", core::Pose::Crouching, 0.6f},
			PoseCase{"crawling", core::Pose::Crawling, 0.3f}})
		{
			// A Furniture use commits its declared pose through host staging
			// vocabulary; its finish_use observes a.pose before restoring Standing.
			auto catalogue = context.temporaryRoot() / (std::string("pose-") + test.name + ".furniture.lua");
			write(catalogue, "return {api_version=1,uuid='" + uuid + "',definitions={{"
				"key='pose',label='Pose',tiles={{x=0,y=0,imageSet='ObjectAtlas',image='chair'}},"
				"usablePoints={{key='body',label='Body',x=0.5,blocksPathing=false}},"
				"use_pose='" + std::string(test.name) + "',finish_use_pose='standing',use=function(a,w,m) w.claim(); w.log('use') end,"
				"finish_use=function(a,w,m) assert(a.pose == '" + std::string(test.name) + "'); w.release(); w.log('finish') end}}}");
			core::World world("Pose vocabulary", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue(catalogue.filename().string(), core::FurnitureCatalogue::readFile(catalogue));
			require(world.placeFurniture(room, "pose", 2, 0, "Pose") != 0, "Vocabulary Furniture refused");
			world.addSectorMarker(room, 0, 6.5f, "Away");
			world.finishBuild();
			auto marker = world.furniture().front().destinations.front().marker;
			auto id = world.createAgent("Operator", room, 0, 1.5f);
			auto agent = world.lookupAgent(id).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualHeightModifier(id, 0.8f), "Vocabulary Height refused");
			world.resumeSimulation();
			auto const standingHeight = agent->getStandingHeight();
			require(world.moveAgentToMarker(id, marker, core::UseFurnitureAction).accepted(), "Vocabulary use refused");
			for (unsigned tick = 0; tick < 1800 && agent->getPose() != test.pose; ++tick)
				require(world.advanceTick(), "Vocabulary use simulation failed");
			require(agent->getPose() == test.pose && world.usablePointOccupant(marker) == id,
				"Use did not pose/claim: " + std::string(test.name));
			require(near(agent->getHeight(), standingHeight * test.scale)
				&& near(agent->getWidth(), agent->getPhysicalBaseline().width),
				"Wrong effective posed height for " + std::string(test.name));
			world.consumeSimulationEvents();
			require(world.moveAgentToNamedMarker(id, "Away").accepted(), "Vocabulary departure refused");
			bool finished = false;
			for (unsigned tick = 0; tick < 1800; ++tick)
			{
				require(world.advanceTick(), "Vocabulary departure simulation failed");
				for (auto const& log : core::consumeLogMessages()) finished |= log.msg == "finish";
				if (finished && agent->getPose() == core::Pose::Standing && !world.usablePointOccupant(marker)) break;
			}
			require(finished && agent->getPose() == core::Pose::Standing && !world.usablePointOccupant(marker),
				"Departure did not observe/restore pose or release occupancy: " + std::string(test.name));
		}
		// Unknown pose names fail validation and roll back without publishing effects.
		for (auto const& bad : {"crouch", "crawl", "sitting-on-floor", ""})
		{
			auto path = context.temporaryRoot() / "bad-pose.actions.lua";
			write(path, package("{key='hello',name='BadPose',run=function(a,w,m) w.set_pose('" + std::string(bad) + "'); w.log('rollback') end}"));
			core::World world("Bad pose", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.addSectorMarker(room, 0, 6.5f, "Target");
			world.finishBuild();
			auto marker = world.getMarkerIds().back();
			auto id = world.createAgent("Operator", room, 0, 1.5f);
			world.pauseSimulation();
			require(world.selectActionRegistry(path) && world.setMarkerActions(marker, {first}), "Bad pose registry refused");
			core::consumeLogMessages();
			auto event = runAction(world, id, marker, first);
			require(event.type == core::SimulationEventType::ActionFailed
				&& event.scriptFailure != core::ScriptExecutionFailure::None
				&& world.lookupAgent(id).entity->getPose() == core::Pose::Standing,
				"Invalid pose was not rejected atomically: '" + std::string(bad) + "'");
			for (auto const& log : core::consumeLogMessages()) require(log.msg != "rollback", "Rejected pose published logs");
		}
	}

	void claimCompetition(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "competition.actions.lua";
		write(path, package("{key='hello',name='Claim',run=function(a,w,m) w.set_pose('sitting'); w.claim() end},"
			"{key='other',name='Release',run=function(a,w,m) w.set_pose('standing'); w.release() end}"));
		for (unsigned run = 0; run < 2; ++run)
		{
			core::World world("Competition", 10, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
			world.attachFurnitureCatalogue("sit.furniture.lua", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/sit.furniture.lua")));
			require(world.placeFurniture(room, "chair", 3, 0, "First chair")
				&& world.placeFurniture(room, "chair", 6, 0, "Second chair"), "Competition placement refused");
			world.finishBuild();
			auto seat = world.furniture()[0].destinations[0].marker;
			auto otherSeat = world.furniture()[1].destinations[0].marker;
			auto owner = world.createAgent("Owner", room, 0, 3.5f);
			auto contender = world.createAgent("Contender", room, 0, 3.5f);
			auto independent = world.createAgent("Independent", room, 0, 6.5f);
			world.pauseSimulation();
			for (auto id : {owner, contender, independent})
			{
				require(world.setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
					&& world.setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Competition planning refused");
			}
			require(world.selectActionRegistry(path) && world.setMarkerActions(seat, {first,second})
				&& world.setMarkerActions(otherSeat, {first,second}), "Competition registry refused");
			for (auto id : {owner, contender}) require(world.moveAgentToMarker(id, seat, first).accepted(), "Competing request refused");
			require(world.moveAgentToMarker(independent, otherSeat, first).accepted(), "Independent claim refused");
			require(world.resumeSimulation(), "Competition resume refused");
			bool failed = false;
			for (unsigned tick = 0; tick < 600; ++tick)
			{
				require(world.advanceTick(), "Claim conflict crashed simulation");
				for (auto const& event : world.consumeSimulationEvents())
					if (event.agent.id == contender && (event.type == core::SimulationEventType::ActionFailed
						|| event.type == core::SimulationEventType::RouteLost))
					{
						failed = true;
						require(event.scriptFailure == core::ScriptExecutionFailure::None, "Claim conflict was a script error");
					}
			}
			require(failed && world.usablePointOccupant(seat) == owner && world.usablePointOccupant(otherSeat) == independent,
				"Claim competition was nondeterministic or leaked across Furniture instances: " + std::to_string(failed)
				+ ":" + std::to_string(world.usablePointOccupant(seat).value) + ":" + std::to_string(world.usablePointOccupant(otherSeat).value));
			require(world.lookupAgent(contender).entity->getPose() == core::Pose::Standing, "Pose-before-claim conflict leaked Pose");
			// A non-owner's release request cannot free the occupied target.
			auto refused = runAction(world, contender, seat, second);
			require(refused.type != core::SimulationEventType::DestinationReached && world.usablePointOccupant(seat) == owner,
				"Non-owner release freed another Agent's claim");
			auto released = runAction(world, owner, seat, second);
			require(released.type == core::SimulationEventType::DestinationReached && !world.usablePointOccupant(seat)
				&& world.lookupAgent(owner).entity->getPose() == core::Pose::Standing, "Owner could not release claim");
			require(world.usablePointOccupant(otherSeat) == independent, "Release affected another Furniture instance");
		}
	}

	void furnitureUse(smoke::Context const& context)
	{
		auto world = useFixture(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
		auto chair = world->furniture()[0].marker;
		auto left = world->furniture()[1].destinations[0].marker;
		auto right = world->furniture()[1].destinations[1].marker;
		auto desk = world->furniture()[2].marker;
		auto owner = core::AgentId{1}, other = core::AgentId{2}, third = core::AgentId{3};
		std::string const use(core::UseFurnitureAction);
		auto path = context.temporaryRoot() / "replacement.actions.lua";
		write(path, package("{key='hello',name='Replacement',run=function(a,w,m) w.release() end},"
			"{key='other',name='Observe Standing',run=function(a,w,m) assert(a.pose == 'standing'); w.log('replacement') end}"));
		require(world->selectActionRegistry(path) && world->setMarkerActions(chair, {second, use, second, "idle"}), "Use assignment refused");
		require(world->availableAgentActions(chair) == std::vector<std::string>{"idle", use, second}, "Derived use ordering/deduplication changed");
		require(world->availableAgentActions(desk) == std::vector<std::string>{"idle"}, "Desk invented derived use");
		require(!world->setMarkerActions(desk, {use}) && world->markerActions(desk).empty(), "Desk accepted an unavailable built-in use assignment");
		require(runAction(*world, owner, chair, "idle").type == core::SimulationEventType::DestinationReached
			&& world->lookupAgent(owner).entity->getPose() == core::Pose::Standing && !world->usablePointOccupant(chair), "Idle implicitly used chair");
		core::consumeLogMessages();
		require(runAction(*world, owner, chair, use).type == core::SimulationEventType::DestinationReached, "Explicit use failed");
		require(world->lookupAgent(owner).entity->getPose() == core::Pose::Sitting && world->usablePointOccupant(chair) == owner, "Chair use did not sit/claim");
		core::consumeLogMessages();
		require(runAction(*world, owner, chair, use).type == core::SimulationEventType::DestinationReached, "Repeated use failed");
		for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("use:") && !log.msg.starts_with("finish:"), "Repeated use restarted lifecycle");
		world->pauseSimulation();
		auto pausedTick = world->getSimulationTick();
		require(!world->advanceTicks(5) && world->getSimulationTick() == pausedTick, "Pause advanced simulation");
		world->lookupAgent(owner).entity->setActive(false);
		require(world->resumeSimulation() && world->advanceTicks(20), "Inactive advancement failed");
		require(world->usablePointOccupant(chair) == owner && world->lookupAgent(owner).entity->getPose() == core::Pose::Sitting, "Pause/deactivation vacated seat");
		world->pauseSimulation(); world->lookupAgent(owner).entity->setActive(true);
		// Occupied destinations reject Idle and assigned custom Actions alike.
		require(!world->moveAgentToMarker(other, chair, use).accepted(), "Occupied use was not refused before travel");
		for (auto const& action : {std::string("idle"), second})
		{
			auto refused = runAction(*world, other, chair, action);
			require(refused.type != core::SimulationEventType::DestinationReached
				&& refused.scriptFailure == core::ScriptExecutionFailure::None && world->usablePointOccupant(chair) == owner, "Occupied destination bypassed exclusivity");
		}
		// Circulation through an occupied point remains possible.
		require(world->moveAgentToNamedMarker(other, "End").accepted() && world->advanceTicks(1800), "Occupied point blocked circulation");
		require(world->lookupAgent(other).entity->getGlobalPosition().x == 10.5f && world->usablePointOccupant(chair) == owner, "Pass-through vacated or blocked seat");
		world->consumeSimulationEvents();
		require(world->moveAgentToNamedMarker(owner, "Unreachable").accepted(), "Unreachable replacement refused at acceptance");
		require(world->usablePointOccupant(chair) == owner, "Acceptance vacated seat");
		require(world->advanceTicks(60) && world->usablePointOccupant(chair) == owner
			&& world->lookupAgent(owner).entity->getPose() == core::Pose::Sitting, "Unreachable planning finished use");
		bool routeLost = false;
		for (auto const& event : world->consumeSimulationEvents())
			routeLost |= event.agent.id == owner && event.type == core::SimulationEventType::RouteLost;
		require(routeLost, "Unreachable replacement has no Route-loss outcome");
		core::consumeLogMessages();
		require(runAction(*world, owner, chair, second).type == core::SimulationEventType::DestinationReached, "Same-seat custom replacement failed");
		std::vector<std::string> logs;
		for (auto const& log : core::consumeLogMessages()) if (log.source == "Marker Action") logs.push_back(log.msg);
		require(logs.size() == 2 && logs[0].starts_with("finish:") && logs[1] == "replacement", "Replacement ran before finish");
		require(!world->usablePointOccupant(chair) && world->lookupAgent(owner).entity->getPose() == core::Pose::Standing, "Replacement did not finish use");
		require(runAction(*world, owner, chair, use).type == core::SimulationEventType::DestinationReached, "Second use failed");
		auto idle = runAction(*world, owner, chair, "idle");
		require(idle.type == core::SimulationEventType::DestinationReached && idle.agent.pose == core::Pose::Standing
			&& !world->usablePointOccupant(chair) && world->lookupAgent(owner).entity->getPose() == core::Pose::Standing, "Same-seat Idle did not finish or reported stale Pose");
		require(runAction(*world, owner, chair, use).type == core::SimulationEventType::DestinationReached, "Departure use failed");
		core::consumeLogMessages();
		require(world->moveAgentToNamedMarker(owner, "Origin").accepted(), "Departure request refused");
		auto start = world->lookupAgent(owner).entity->getGlobalPosition();
		bool departed = false;
		for (unsigned tick = 0; tick < 120 && !departed; ++tick)
		{
			require(world->advanceTick(), "Departure tick failed");
			departed = world->lookupAgent(owner).entity->getGlobalPosition() != start;
			require((world->usablePointOccupant(chair) == owner) == !departed, "Finish not at physical departure boundary");
		}
		require(departed && world->lookupAgent(owner).entity->getPose() == core::Pose::Standing, "Departure did not stand");
		// Distinct sofa seats share immutable functions, never instance occupancy.
		require(runAction(*world, owner, left, use).type == core::SimulationEventType::DestinationReached
			&& runAction(*world, third, right, use).type == core::SimulationEventType::DestinationReached, "Independent sofa use failed");
		require(world->usablePointOccupant(left) == owner && world->usablePointOccupant(right) == third, "Sofa claims are not per-point");
		require(runAction(*world, owner, left, "idle").type == core::SimulationEventType::DestinationReached
			&& !world->usablePointOccupant(left) && world->usablePointOccupant(right) == third, "Sofa finish released another seat");
	}

	void furnitureFinishFailures(smoke::Context const& context)
	{
		auto fixture = context.fixture("src/headless/smoke/fixtures/use.furniture.lua");
		std::ifstream input(fixture); std::string source((std::istreambuf_iterator<char>(input)), {});
		for (auto const& body : {"world.log('finish rollback'); world.release(); error('finish broke')",
			"while true do end", "local t={} while true do t[#t+1]=string.rep('x',10000) end", "world.log('finish rollback'); error('broken')"})
		{
			auto custom = source;
			auto begin = custom.find("  world.release()");
			auto end = custom.find("\nend", begin);
			custom.replace(begin, end-begin, body);
			auto path = context.temporaryRoot() / "failure.furniture.lua"; write(path, custom);
			for (unsigned ending : {0u, 1u, 2u, 3u})
			{
				auto world = useFixture(path); auto seat = world->furniture()[0].marker; auto agent = core::AgentId{1};
				require(runAction(*world, agent, seat, std::string(core::UseFurnitureAction)).type == core::SimulationEventType::DestinationReached, "Failure fixture use failed");
				world->consumeSimulationEvents(); core::consumeLogMessages();
				if (ending < 2)
					require((ending ? world->moveAgentToNamedMarker(agent, "Origin") : world->moveAgentToMarker(agent, seat)).accepted(), "Finish request refused");
				else
				{
					world->pauseSimulation();
					auto position = world->lookupAgent(agent).entity->getGlobalPosition();
					auto id = world->furniture()[0].id;
					require(ending == 2 ? world->editFurniture(id, 4, 0, "Moved") : world->removeFurniture(id), "Failure cleanup edit refused");
					require(!world->usablePointOccupant(seat) && world->lookupAgent(agent).entity->getPose() == core::Pose::Standing
						&& world->lookupAgent(agent).entity->getGlobalPosition() == position, "Edit finish failure stranded/teleported Agent");
					require(world->resumeSimulation(), "Edit failure resume refused");
				}
				bool failed = false;
				for (unsigned tick = 0; tick < 120 && !failed; ++tick) failed = !world->advanceTick();
				require(failed && world->isSimulationPaused() && !world->usablePointOccupant(seat)
					&& world->lookupAgent(agent).entity->getPose() == core::Pose::Standing, "Finish failure stranded pose/occupancy or ignored failure policy");
				bool diagnostic = false;
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::ActionFailed)
					{
						diagnostic = !event.diagnostic.empty() && event.scriptFailure != core::ScriptExecutionFailure::None;
						if (std::string_view(body).starts_with("while")) require(event.scriptFailure == core::ScriptExecutionFailure::InstructionBudgetExceeded, "Finish budget lost classification");
						if (std::string_view(body).starts_with("local")) require(event.scriptFailure == core::ScriptExecutionFailure::MemoryBudgetExceeded, "Finish heap budget lost classification");
					}
				require(diagnostic, "Finish failure has no structured diagnostic");
				for (auto const& log : core::consumeLogMessages()) require(log.msg != "finish rollback", "Failed/incomplete finish published staged logs");
			}
		}
	}

	void furnitureFinishDevice(smoke::Context const& context)
	{
		// A finish_use callback may legitimately request a device operation, such as
		// turning a reading light off when the Agent stands up. The finishing
		// contract runs before the physical position commits, so a request staged at
		// departure is judged from the usable point and an ordinary refusal is not
		// escalated into the script-failure pause/headless-failure policy.
		auto build = [&](std::string const& finishBody, float pointX)
		{
			auto world = std::make_shared<core::World>("Finish device", 14, 2);
			auto room = world->addRoom("Room", 0, 0, 0, 11, 1);
			core::InteractionBinding binding;
			binding.command = {core::DeviceCommandType::SetSectorLights, core::SectorId{1}, false};
			auto point = world->createInteractionPoint("Light", core::SectorId{1},
				{pointX, 0.f}, 0.25f, 0.05f, {binding});
			std::ifstream input(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
			std::string source{std::istreambuf_iterator<char>(input), {}};
			auto begin = source.find("  world.release()");
			auto end = source.find("\nend", begin);
			require(begin != std::string::npos && end != std::string::npos, "Finish device fixture substitution missing");
			auto body = finishBody;
			auto placeholder = body.find("__POINT__");
			require(placeholder != std::string::npos, "Finish device point placeholder missing");
			body.replace(placeholder, std::string("__POINT__").size(), std::to_string(point.value));
			source.replace(begin, end - begin, body);
			auto path = context.temporaryRoot() / "finish-device.furniture.lua";
			write(path, source);
			world->attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
			require(world->placeFurniture(room, "chair", 3, 0, "Chair"), "Finish device placement refused");
			world->addSectorMarker(room, 0, 1.5f, "Origin");
			world->finishBuild();
			world->createAgent("Operator", room, 0, 1.5f);
			world->pauseSimulation();
			require(world->setAgentIndividualMinimumRoutePlanningTime(core::AgentId{1}, 0.1f)
				&& world->setAgentIndividualMaximumRoutePlanningTime(core::AgentId{1}, 0.1f), "Finish device planning refused");
			return world;
		};
		std::string const use(core::UseFurnitureAction);
		auto depart = [&](core::World& world, core::AgentId agent, core::MarkerId seat)
		{
			require(runAction(world, agent, seat, use).type == core::SimulationEventType::DestinationReached, "Finish device use failed");
			world.consumeSimulationEvents();
			core::consumeLogMessages();
			require(world.usablePointOccupant(seat) == agent, "Finish device use did not claim seat");
			require(world.moveAgentToNamedMarker(agent, "Origin").accepted(), "Finish device departure refused");
			auto start = world.lookupAgent(agent).entity->getGlobalPosition();
			bool departed = false;
			for (unsigned tick = 0; tick < 120 && !departed; ++tick)
			{
				require(world.advanceTick(), "Departure finishing did not preserve the ordinary failure policy");
				departed = world.lookupAgent(agent).entity->getGlobalPosition() != start;
			}
			require(departed && world.lookupAgent(agent).entity->getPose() == core::Pose::Standing
				&& !world.usablePointOccupant(seat), "Departure finishing left the use active");
		};
		// The reachable device request is admitted at the departure boundary even
		// though departure already set the movement state, and completes without an
		// ordinary refusal or a script-failure pause.
		{
			auto world = build("  world.release()\n"
				"  world.request_device(__POINT__, 'set-sector-lights')\n  world.log('finish:' .. marker.name)", 3.5f);
			depart(*world, core::AgentId{1}, world->furniture()[0].marker);
			require(world->advanceTicks(100), "Finish device operation did not complete");
			bool succeeded = false, refused = false;
			for (auto const& event : world->consumeSimulationEvents())
			{
				succeeded |= event.type == core::SimulationEventType::DeviceOperationChanged
					&& event.deviceOperation.state == core::DeviceOperationState::Succeeded;
				refused |= event.type == core::SimulationEventType::ActionFailed;
			}
			require(succeeded && !world->getSector(0)->areLightsOn(), "Departure finishing refused the staged device request");
			require(!refused, "Departure finishing published a spurious refusal");
		}
		// An out-of-reach device request is an ordinary refusal: it must stay a
		// non-script failure so host cleanup stands and headless advancement continues.
		{
			auto world = build("  world.release()\n"
				"  world.request_device(__POINT__, 'set-sector-lights')\n  world.log('finish:' .. marker.name)", 6.5f);
			depart(*world, core::AgentId{1}, world->furniture()[0].marker);
			bool refused = false;
			for (auto const& event : world->consumeSimulationEvents())
				refused |= event.type == core::SimulationEventType::ActionFailed
					&& event.scriptFailure == core::ScriptExecutionFailure::None && !event.diagnostic.empty();
			require(refused, "Ordinary finish refusal was not published as an ordinary Action failure");
			require(world->getSector(0)->areLightsOn(), "Refused finish device changed the lights");
		}
	}

	void furnitureStructuralEdits(smoke::Context const& context)
	{
		std::ifstream input(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
		std::string unclaimed((std::istreambuf_iterator<char>(input)), {});
		for (auto call : {"  world.claim()\n", "  world.release()\n"}) unclaimed.erase(unclaimed.find(call), std::string_view(call).size());
		auto unclaimedPath = context.temporaryRoot() / "unclaimed.furniture.lua";
		write(unclaimedPath, unclaimed);
		for (bool claim : {false, true})
		for (bool remove : {false, true})
		{
			auto world = useFixture(claim ? context.fixture("src/headless/smoke/fixtures/use.furniture.lua") : unclaimedPath);
			auto chair = world->furniture()[0].id;
			auto seat = world->furniture()[0].marker;
			auto sofaSeat = world->furniture()[1].destinations[0].marker;
			auto owner = core::AgentId{1}, independent = core::AgentId{2}, traveller = core::AgentId{3};
			require(runAction(*world, owner, seat, "use-furniture").type == core::SimulationEventType::DestinationReached
				&& runAction(*world, independent, sofaSeat, "use-furniture").type == core::SimulationEventType::DestinationReached, "Edit use setup failed");
			world->pauseSimulation(); core::consumeLogMessages();
			auto position = world->lookupAgent(owner).entity->getGlobalPosition();
			auto graph = world->getGraph();
			std::string diagnostic;
			require(!world->editFurniture(chair, 6, 0, "Refused", &diagnostic) && !diagnostic.empty()
				&& world->getGraph() == graph && world->usablePointOccupant(seat) == (claim ? owner : core::AgentId{})
				&& world->lookupAgent(owner).entity->getPose() == core::Pose::Sitting, "Refused edit changed graph/use");
			auto registry = core::AgentBehaviourRegistry::create();
			world->attachAgentBehaviourRegistry("references.behaviours", registry);
			auto behaviour = registry->addAgentBehaviour("Visit", "visit.lua", {
				{"destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt}});
			require(world->setAgentBehaviourAssignment(traveller, behaviour, 1, {{"destination", seat}}, &diagnostic), diagnostic);
			require(!world->removeFurniture(chair, &diagnostic) && diagnostic.find("destination") != std::string::npos
				&& world->getGraph() == graph && world->lookupAgent(owner).entity->getPose() == core::Pose::Sitting,
				"Referenced Furniture deletion changed active use");
			require(world->clearAgentBehaviourAssignment(traveller, &diagnostic), diagnostic);
			require(!world->removeFurniture(99999, &diagnostic), "Unknown Furniture deletion accepted");
			for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("finish:"), "Refused deletion invoked lifecycle");
			// Rename-only replay must preserve the lifecycle as well as the claim.
			require(world->editFurniture(chair, 3, 0, "Renamed", &diagnostic)
				&& world->usablePointOccupant(seat) == (claim ? owner : core::AgentId{}), "Rename lost active use");
			for (auto const& log : core::consumeLogMessages())
				require(!log.msg.starts_with("finish:") && !log.msg.starts_with("use:"), "Rename replay invoked lifecycle");
			if (remove)
			{
				// A request accepted before the claim is freed cannot bypass occupancy;
				// instead test a pending request to the independently movable desk.
				auto desk = world->furniture()[2].id;
				auto target = world->furniture()[2].marker;
				auto actions = context.temporaryRoot() / "deleted.actions.lua";
				write(actions, package("{key='hello',name='Deleted callback',run=function(a,w,m) w.log('stale callback') end}"));
				require(world->selectActionRegistry(actions) && world->setMarkerActions(target, {first}), "Pending Action setup refused");
				require(world->moveAgentToMarker(traveller, target, first).accepted() && world->removeFurniture(desk, &diagnostic), diagnostic);
				require(world->resumeSimulation() && world->advanceTicks(30), "Deletion cancellation failed");
				bool cancelled = false;
				for (auto const& event : world->consumeSimulationEvents())
					cancelled |= event.agent.id == traveller && event.type == core::SimulationEventType::MovementCancelled
						&& event.movementCancellationReason == core::MovementCancellationReason::TargetDeleted;
				require(cancelled, "Removed target did not cancel pending request");
				world->pauseSimulation();
				for (auto const& log : core::consumeLogMessages()) require(log.msg != "stale callback", "Deleted target executed stale Action");
			}
			require(remove ? world->removeFurniture(chair, &diagnostic)
				: world->editFurniture(chair, 4, 0, "Moved", &diagnostic), diagnostic);
			require(world->lookupAgent(owner).entity->getGlobalPosition() == position
				&& world->lookupAgent(owner).entity->getPose() == core::Pose::Standing
				&& !world->usablePointOccupant(seat), "Structural edit teleported Agent or stranded use");
			unsigned finishes = 0;
			for (auto const& log : core::consumeLogMessages()) if (log.msg.starts_with("finish:")) ++finishes;
			require(finishes == 1, "Affected use did not finish exactly once with old view");
			require(world->usablePointOccupant(sofaSeat) == (claim ? independent : core::AgentId{})
				&& world->lookupAgent(independent).entity->getPose() == core::Pose::Sitting, "Local edit reset unrelated use");
			require(runAction(*world, independent, sofaSeat, "idle").type == core::SimulationEventType::DestinationReached, "Preserved use replacement failed");
			finishes = 0;
			for (auto const& log : core::consumeLogMessages()) if (log.msg.starts_with("finish:")) ++finishes;
			require(finishes == 1 && !world->usablePointOccupant(sofaSeat), "Replay lost unrelated active lifecycle");
		}
		for (unsigned surrounding : {0u, 1u, 2u})
		{
			auto world = useFixture(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"));
			if (surrounding == 1) world->addLayer();
			auto seat = world->furniture()[0].marker;
			require(runAction(*world, core::AgentId{1}, seat, "use-furniture").type == core::SimulationEventType::DestinationReached, "Surrounding edit setup failed");
			world->pauseSimulation(); core::consumeLogMessages();
			if (surrounding == 2)
			{
				require(!world->planRemoveLocation(0).valid && world->usablePointOccupant(seat) == core::AgentId{1}, "Refused Location removal changed use");
				world->applyLocationEdit(world->planResizeLocation(0, 1, 0, 11, 1));
			}
			else require(surrounding == 1 ? world->applyDeleteLayer(world->planDeleteLayer(0))
				: world->applyDeleteLevel(world->planDeleteLevel(0)), "Surrounding target invalidation refused");
			unsigned finishes = 0;
			for (auto const& log : core::consumeLogMessages()) if (log.msg.starts_with("finish:")) ++finishes;
			require(finishes == 1 && !world->usablePointOccupant(seat), "Surrounding edit invalidated target before finishing");
		}
	}

	void furnitureUseCompetition(smoke::Context const& context)
	{
		for (unsigned run = 0; run < 2; ++run)
		for (auto startX : {1.5f, 3.5f})
		for (auto action : {core::UseFurnitureAction, core::IdleAction})
		{
			auto world = useFixture(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), startX);
			auto seat = world->furniture()[0].marker;
			require(world->moveAgentToMarker(core::AgentId{1}, seat, core::UseFurnitureAction).accepted()
				&& world->moveAgentToMarker(core::AgentId{2}, seat, action).accepted(), "Competing use/Idle refused");
			require(world->resumeSimulation() && world->advanceTicks(1800), "Use competition crashed");
			require(world->usablePointOccupant(seat) == core::AgentId{1}
				&& world->lookupAgent(core::AgentId{2}).entity->getPose() == core::Pose::Standing, "Use competition is not deterministic/atomic");
			bool failed = false;
			for (auto const& event : world->consumeSimulationEvents())
				if (event.agent.id == core::AgentId{2} && (event.type == core::SimulationEventType::ActionFailed || event.type == core::SimulationEventType::RouteLost))
				{ failed = true; require(event.scriptFailure == core::ScriptExecutionFailure::None, "Use conflict became script error"); }
			require(failed, "Use conflict has no request outcome");
		}
	}

	void furnitureUseAtomicity(smoke::Context const& context)
	{
		// Independent usable points must also work when both claims happen together.
		auto fixture = context.fixture("src/headless/smoke/fixtures/use.furniture.lua");
		for (unsigned repeat = 0; repeat < 2; ++repeat)
		{
			auto world = useFixture(fixture);
			auto left = world->furniture()[1].destinations[0].marker;
			auto right = world->furniture()[1].destinations[1].marker;
			auto firstAgent = core::AgentId{1}, secondAgent = core::AgentId{2};
			require(world->moveAgentToMarker(firstAgent, left, core::UseFurnitureAction).accepted()
				&& world->moveAgentToMarker(secondAgent, right, core::UseFurnitureAction).accepted()
				&& world->resumeSimulation() && world->advanceTicks(1800), "Concurrent independent use failed");
			require(world->usablePointOccupant(left) == firstAgent && world->usablePointOccupant(right) == secondAgent
				&& world->lookupAgent(firstAgent).entity->getPose() == core::Pose::Sitting
				&& world->lookupAgent(secondAgent).entity->getPose() == core::Pose::Sitting,
				"Concurrent use merged independent seats");
			require(runAction(*world, firstAgent, left, std::string(core::IdleAction)).type == core::SimulationEventType::DestinationReached
				&& !world->usablePointOccupant(left) && world->usablePointOccupant(right) == secondAgent,
				"Concurrent-seat replacement released another Agent's claim");
		}
		// A failed Furniture use must roll back Pose, claim, logs and active lifecycle,
		// not only the effects of an ordinary custom Action.
		std::ifstream input(fixture);
		std::string original((std::istreambuf_iterator<char>(input)), {});
		for (auto body : {"world.claim(); world.log('use rollback'); error('use broke')",
			"world.claim(); while true do end"})
		{
			auto source = original;
			auto begin = source.find("  world.claim()");
			auto end = source.find("\nend", begin);
			source.replace(begin, end - begin, body);
			auto path = context.temporaryRoot() / "use-atomicity.furniture.lua";
			write(path, source);
			auto world = useFixture(path);
			auto seat = world->furniture()[0].marker;
			auto owner = core::AgentId{1};
			core::consumeLogMessages();
			auto failed = runAction(*world, owner, seat, std::string(core::UseFurnitureAction));
			require(failed.type == core::SimulationEventType::ActionFailed
				&& failed.scriptFailure != core::ScriptExecutionFailure::None
				&& world->isSimulationPaused() && !world->usablePointOccupant(seat)
				&& world->lookupAgent(owner).entity->getPose() == core::Pose::Standing,
				"Failed Furniture use committed partial state or ignored error policy");
			for (auto const& log : core::consumeLogMessages()) require(log.msg != "use rollback", "Failed use published log");
			require(runAction(*world, owner, seat, std::string(core::IdleAction)).type == core::SimulationEventType::DestinationReached,
				"Failed use left an active lifecycle or stranded the seat");
			for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("finish:"), "Failed use installed a finish callback");
			// A different Agent can now arrive Standing: there was no leaked claim.
			require(runAction(*world, core::AgentId{2}, seat, std::string(core::IdleAction)).type == core::SimulationEventType::DestinationReached,
				"Failed use leaked occupancy to another request");
		}
	}

	void deviceEffects(smoke::Context const& context)
	{
		for (unsigned scenario = 0; scenario < 8; ++scenario)
		{
			auto world = worldFixture();
			auto marker = world->getMarkerIds()[0];
			core::InteractionBinding binding;
			binding.command = {core::DeviceCommandType::SetSectorLights, core::SectorId{1}, false};
			auto point = world->createInteractionPoint("Action light control", core::SectorId{1},
				{scenario == 4 ? 8.5f : 6.5f, 0.f}, 0.25f, 0.05f, {binding});
			if (scenario == 5)
			{
				auto permission = world->addAccessPermission("Protected");
				require(world->setInteractionPointPermissionRequirement(point, {permission}), "Device requirement refused");
			}
			if (scenario == 7)
			{
				core::MobilityProfile profile;
				profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
				require(world->setAgentIndividualMobilityProfile(core::AgentId{1}, profile), "Mobility restriction refused");
			}
			auto path = context.temporaryRoot() / ("device" + std::to_string(scenario) + ".actions.lua");
			auto body = "w.set_pose('sitting'); w.request_device(" + std::to_string(point.value)
				+ ", '" + (scenario == 6 ? "open-door" : "set-sector-lights") + "'); w.log('device')";
			if (scenario == 1) body += "; w.claim()"; // ordinary Marker cannot be claimed
			if (scenario == 2) body += "; error('broken')";
			if (scenario == 3) body += "; while true do end";
			write(path, package("{key='hello',name='Hello',run=function(a,w,m) " + body + " end}"));
			require(world->selectActionRegistry(path) && world->setMarkerActions(marker, {first}), "Device package refused");
			core::consumeLogMessages();
			auto event = runAction(*world, core::AgentId{1}, marker);
			if (scenario == 0)
			{
				require(event.type == core::SimulationEventType::DestinationReached, "Device effects rejected: " + event.diagnostic + " at " + std::to_string(event.agent.globalPosition.y));
				require(world->advanceTicks(100), "Device progression failed");
				bool succeeded = false;
				for (auto const& outcome : world->consumeSimulationEvents())
					succeeded |= outcome.type == core::SimulationEventType::DeviceOperationChanged
						&& outcome.deviceOperation.state == core::DeviceOperationState::Succeeded;
				require(succeeded && !world->getSector(0)->areLightsOn(), "Typed device outcome not observable");
			}
			else
			{
				require(event.type == core::SimulationEventType::ActionFailed, "Invalid device batch accepted");
				auto const& snapshot = world->getSimulationSnapshot();
				require(snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty(), "Rejected invocation published device work");
				require(world->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Standing
					&& world->getSector(0)->areLightsOn(), "Device rollback changed simulation");
				for (auto const& log : core::consumeLogMessages()) require(log.msg != "device", "Device rejection published log");
			}
		}
	}

	void failures(smoke::Context const& context)
	{
		std::vector<std::string> callbacks{
			"w.log('rollback'); error('broken')",
			"w.log('rollback'); a.name='mutated'",
			"w.log('rollback'); io.open('forbidden')",
			"w.log('rollback'); while true do pcall(function() while true do end end) end",
			"w.log('rollback'); local t={} while true do t[#t+1]=string.rep('x',10000) end"
		};
		for (size_t i = 0; i < callbacks.size(); ++i)
		{
			auto path = context.temporaryRoot() / ("failure" + std::to_string(i) + ".actions.lua");
			write(path, package("{key='hello',name='Hello',run=function(a,w,m) " + callbacks[i] + " end}"));
			auto world = worldFixture();
			require(world->selectActionRegistry(path), "Failure fixture refused before invocation");
			auto marker = world->getMarkerIds()[0];
			require(world->setMarkerActions(marker, {first}), "Failure assignment refused");
			require(world->moveAgentToMarker(core::AgentId{1}, marker, first).accepted(), "Failure move refused");
			core::consumeLogMessages();
			require(world->resumeSimulation(), "Failure resume refused");
			bool failed = false;
			for (int tick = 0; tick < 1800 && !failed; ++tick) failed = !world->advanceTick();
			require(failed && world->isSimulationPaused(), "Script failure did not fail headless advancement/pause");
			bool diagnostic = false;
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::ActionFailed)
				{
					diagnostic = !event.diagnostic.empty() && event.scriptFailure != core::ScriptExecutionFailure::None;
					if (i == 3) require(event.scriptFailure == core::ScriptExecutionFailure::InstructionBudgetExceeded, "Instruction budget not structured");
					if (i == 4) require(event.scriptFailure == core::ScriptExecutionFailure::MemoryBudgetExceeded, "Heap budget not structured");
				}
			require(diagnostic, "Script error has no structured outcome");
			for (auto const& message : core::consumeLogMessages()) require(message.msg != "rollback", "Failed Action committed staged logging");
			require(world->lookupAgent(core::AgentId{1}).entity->getName() == "First", "Callback mutated domain state");
		}
	}

	std::shared_ptr<core::World> reopen(std::filesystem::path const& document, bool binary)
	{
		std::unique_ptr<core::Serializer> reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromFile(document.string()))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromFile(document.string()));
		reader->deserialize();
		auto world = std::make_shared<core::World>("Loading",1,1);
		core::SerializationWorkData data;
		data.documentDirectory = document.parent_path();
		if (!world->deserialize(*reader, data)) throw std::runtime_error("Action document rejected");
		return world;
	}

	void furnitureUseDocuments(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "use.furniture.lua";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), path,
			std::filesystem::copy_options::overwrite_existing);
		auto world = useFixture(path);
		auto seat = world->furniture()[0].marker; auto agent = core::AgentId{1};
		require(world->setMarkerActions(seat, {"use-furniture", "idle", "use-furniture"})
			&& world->authorAgentMarkerRequest(agent, seat, core::UseFurnitureAction), "Built-in use authoring refused without registry");
		for (bool binary : {false, true})
		{
			auto document = context.temporaryRoot() / (binary ? "use.world.bin" : "use.world.yaml");
			auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toFile(document.string()));
			core::SerializationWorkData work; work.documentDirectory = context.temporaryRoot();
			world->serialize(*writer, work); writer->serialize(); writer.reset();
			auto loaded = reopen(document, binary);
			require(loaded->availableAgentActions(seat) == std::vector<std::string>{"idle", "use-furniture"}
				&& loaded->markerActions(seat) == std::vector<std::string>{"use-furniture"}, "Document lost derived/assigned use identity");
			if (loaded->isSimulationPaused()) require(loaded->resumeSimulation(), "Reopened use resume failed");
			bool arrived = false;
			for (unsigned tick = 0; tick < 1800 && !arrived; ++tick)
			{
				require(loaded->advanceTick(), "Reopened use execution failed");
				for (auto const& event : loaded->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
					{ arrived = true; require(event.selectedAction == core::UseFurnitureAction, "Persisted use became Idle"); }
			}
			require(arrived && loaded->usablePointOccupant(seat) == agent
				&& loaded->lookupAgent(agent).entity->getPose() == core::Pose::Sitting, "Reopened authored use did not sit/claim");
			require(runAction(*loaded, agent, seat, "idle").type == core::SimulationEventType::DestinationReached
				&& !loaded->usablePointOccupant(seat), "Reopened use did not finish");
		}
		// Save an actually active use. Reconstruction itself must not execute
		// either lifecycle callback; an authored request may run only on later ticks.
		require(runAction(*world, agent, seat, "use-furniture").type == core::SimulationEventType::DestinationReached, "Active document setup failed");
		world->pauseSimulation();
		for (bool binary : {false, true})
		{
			auto document = context.temporaryRoot() / (binary ? "active.world.bin" : "active.world.yaml");
			auto output = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toFile(document.string()));
			core::SerializationWorkData data; data.documentDirectory = context.temporaryRoot();
			world->serialize(*output, data); output->serialize(); output.reset();
			core::consumeLogMessages(); auto loaded = reopen(document, binary);
			require(!loaded->usablePointOccupant(seat) && loaded->lookupAgent(agent).entity->getPose() == core::Pose::Standing, "Load restored transient use");
			for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("use:") && !log.msg.starts_with("finish:"), "Load invoked lifecycle");
		}
		core::consumeLogMessages(); world->resetSimulation();
		require(!world->usablePointOccupant(seat) && world->lookupAgent(agent).entity->getPose() == core::Pose::Standing, "Reset restored transient use");
		for (auto const& log : core::consumeLogMessages()) require(!log.msg.starts_with("use:") && !log.msg.starts_with("finish:"), "Reset invoked lifecycle");
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		world->serialize(*writer, work); writer->serialize(); auto yaml = writer->getSerializedString();
		require(yaml.find("function") == std::string::npos && yaml.find("finish_use") == std::string::npos, "Furniture callbacks entered document");
	}

	void boundedLogging(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "logging.actions.lua";
		write(path, package("{key='hello',name='Hello',run=function(a,w,m) for i=1,100 do w.log(string.rep('x',10000)) end end}"));
		auto world = worldFixture();
		require(world->selectActionRegistry(path), "Logging fixture refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker,{first}), "Logging assignment refused");
		require(world->moveAgentToMarker(core::AgentId{1},marker,first).accepted(), "Logging request refused");
		core::consumeLogMessages(); require(world->resumeSimulation(), "Logging resume refused");
		require(world->advanceTicks(1200), "Bounded logging callback failed");
		size_t messages = 0, bytes = 0, suppressed = 0;
		for (auto const& message : core::consumeLogMessages()) if (message.source == "Marker Action")
		{
			if (message.level == core::LogLevel::Warning) ++suppressed;
			else { ++messages; bytes += message.msg.size(); require(message.msg.size() <= 1024,"Per-message logging limit exceeded"); }
		}
		require(messages > 0 && messages <= 32 && bytes <= 8192 && suppressed == 1, "Staged logging is not bounded");
	}

	void documents(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "document.actions.lua";
		auto originalSource = package("{key='hello',name='Hello',run=function(a,w,m) w.log('document arrived') end},{key='other',name='Other',run=function() end}");
		write(path, originalSource);
		auto world = worldFixture();
		require(world->selectActionRegistry(path), "Document registry refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker, {second,first}), "Document assignment refused");
		require(world->authorAgentMarkerRequest(core::AgentId{1}, marker, first), "Authored request refused");
		for (bool binary : {false,true})
		{
			auto document = context.temporaryRoot() / (binary ? "actions.world" : "actions.world.yaml");
			std::unique_ptr<core::Serializer> writer = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::toFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toFile(document.string()));
			core::SerializationWorkData data;
			world->serialize(*writer, data); writer->serialize();
			// Reopen, not live reload: changing a display name retains UUID/key references.
			auto renamed = originalSource;
			renamed.replace(renamed.find("name='Hello'"), 12, "name='Renamed display'");
			write(path, renamed);
			auto loaded = reopen(document, binary);
			require(loaded->actionRegistryFilename() == "document.actions.lua" && loaded->actionRegistry()->uuid() == uuid, "Registry reference not persisted");
			require(loaded->markerActions(marker) == std::vector<std::string>{second,first}, "Ordered assignments not persisted");
			require(loaded->agentActionDisplayName(first) == "Renamed display", "Display name affected identity");
			core::consumeLogMessages();
			if (loaded->isSimulationPaused()) require(loaded->resumeSimulation(), "Loaded resume refused");
			bool arrived = false;
			for (int tick = 0; tick < 1800 && !arrived; ++tick)
			{
				require(loaded->advanceTick(), "Loaded Action failed");
				for (auto const& event : loaded->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						arrived = true; require(event.selectedAction == first, "Saved Action silently became Idle");
					}
			}
			require(arrived, "Saved custom request did not arrive");
			bool logged = false;
			for (auto const& message : core::consumeLogMessages()) logged |= message.msg == "document arrived";
			require(logged, "Reopened Action was not executed");
		}
		// Public serialization remains data-only: metadata and references, never Lua source/closures.
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data;
		world->serialize(*writer,data); writer->serialize(); auto yaml = writer->getSerializedString();
		require(yaml.find("function") == std::string::npos && yaml.find("document arrived") == std::string::npos, "World serialized Lua source/state");
		for (auto const& [from,to] : std::vector<std::pair<std::string,std::string>>{
			{uuid + ":hello", uuid + ":missing"}, {"marker: 1", "marker: 999"}, {"expectedUuid: " + uuid,"expectedUuid: 00000000-0000-4000-8000-000000000000"}})
		{
			auto malformed = yaml; auto offset = malformed.find(from);
			require(offset != std::string::npos,"Reference fixture not found"); malformed.replace(offset,from.size(),to);
			auto reader = core::YamlSerializer::fromString(malformed); reader->deserialize();
			core::World rejectedWorld("Rejected",1,1); core::SerializationWorkData dependencies;
			dependencies.documentDirectory = context.temporaryRoot(); bool rejected = false;
			try { rejected = !rejectedWorld.deserialize(*reader,dependencies); } catch (std::exception const&) { rejected = true; }
			require(rejected,"Invalid persisted Action reference accepted");
		}
		std::filesystem::remove(path);
		bool rejected = false;
		try { reopen(context.temporaryRoot() / "actions.world.yaml", false); }
		catch (std::exception const&) { rejected = true; }
		require(rejected, "Missing Action registry dependency accepted");
	}

	void authoredRequests(smoke::Context const& context)
	{
		auto root = context.temporaryRoot();

		// A still-available authored Action request survives an Action registry
		// reload: the reload swaps the package but must not cancel the request.
		auto actions = root / "authored.actions.lua";
		write(actions, package("{key='hello',name='Hello',run=function(a,w,m) w.log('authored hello') end}"));
		auto world = worldFixture();
		auto marker = world->getMarkerIds()[0];
		std::string diagnostic;
		require(world->selectActionRegistry(actions) && world->setMarkerActions(marker, {first}), "Authored action setup refused");
		require(world->authorAgentMarkerRequest(core::AgentId{1}, marker, first, &diagnostic), "Authored action request refused: " + diagnostic);
		write(actions, package("{key='hello',name='Renamed',run=function(a,w,m) w.log('authored hello') end}"));
		require(world->reloadActionRegistry(actions, &diagnostic) && diagnostic.empty(), "Action reload refused: " + diagnostic);
		auto document = root / "authored.world.yaml";
		world->saveTo(document.string());
		auto loaded = reopen(document, false);
		require(loaded->agentActionDisplayName(first) == "Renamed", "Reloaded display name not persisted");
		core::consumeLogMessages();
		if (loaded->isSimulationPaused()) require(loaded->resumeSimulation(), "Reopened action resume refused");
		bool arrived = false;
		for (int tick = 0; tick < 1800 && !arrived; ++tick)
		{
			require(loaded->advanceTick(), "Reopened authored Action failed");
			for (auto const& event : loaded->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{ arrived = true; require(event.selectedAction == first, "Saved Action silently became Idle"); }
		}
		require(arrived, "Reopened authored request did not arrive");

		// A Furniture catalogue reload that removes the definition's use functions
		// cancels an authored use-furniture request so the World round-trips.
		auto furniture = root / "authored-use.furniture.lua";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), furniture,
			std::filesystem::copy_options::overwrite_existing);
		auto furnished = useFixture(furniture);
		auto seat = furnished->furniture()[0].marker;
		require(furnished->authorAgentMarkerRequest(core::AgentId{1}, seat, core::UseFurnitureAction, &diagnostic),
			"Authored use request refused: " + diagnostic);
		furnished->saveTo((root / "authored-use-baseline.world.yaml").string());
		std::string source;
		{ std::ifstream in(furniture); source.assign(std::istreambuf_iterator<char>(in), {}); }
		auto replace = [](std::string text, std::string const& from, std::string const& to)
		{
			auto at = text.find(from); require(at != std::string::npos, "Furniture substitution missing");
			text.replace(at, from.size(), to); return text;
		};
		std::ofstream(furniture) << replace(source, "use_pose = \"sitting\", finish_use_pose = \"standing\", use = use, finish_use = finish", "");
		require(furnished->reloadFurnitureCatalogue(furniture, &diagnostic), "Furniture reload refused: " + diagnostic);
		require(furnished->isModified(), "Clearing an authored request did not dirty the document");
		auto useDocument = root / "authored-use.world.yaml";
		furnished->saveTo(useDocument.string());
		auto reloaded = reopen(useDocument, false);
		core::consumeLogMessages();
		if (reloaded->isSimulationPaused()) require(reloaded->resumeSimulation(), "Reopened use resume refused");
		require(reloaded->advanceTicks(60), "Reopened use advance failed");
		for (auto const& event : reloaded->consumeSimulationEvents())
			require(event.type != core::SimulationEventType::DestinationReached || event.selectedAction != core::UseFurnitureAction,
				"Cancelled authored use request re-executed after reload");
		require(!reloaded->usablePointOccupant(seat)
			&& reloaded->lookupAgent(core::AgentId{1}).entity->getPose() == core::Pose::Standing,
			"Cancelled authored use request left occupancy or pose");
	}
}

void registerMarkerActions(std::vector<smoke::Check>& checks)
{
	checks.push_back({"markerActions/furniturePoseRequirements", furniturePoseRequirements});
	checks.push_back({"markerActions/reload", transactionalReload});
	checks.push_back({"markerActions/authoredRequests", authoredRequests});
	checks.push_back({"markerActions/registry", registryContracts});
	checks.push_back({"markerActions/execution", execution});
	checks.push_back({"markerActions/failures", failures});
	checks.push_back({"markerActions/documents", documents});
	checks.push_back({"markerActions/logging", boundedLogging});
	checks.push_back({"markerActions/atomicEffects", atomicEffects});
	checks.push_back({"markerActions/poseVocabulary", poseVocabulary});
	checks.push_back({"markerActions/deviceEffects", deviceEffects});
	checks.push_back({"markerActions/claimCompetition", claimCompetition});
	checks.push_back({"markerActions/furnitureUse", furnitureUse});
	checks.push_back({"markerActions/furnitureFinishDevice", furnitureFinishDevice});
	checks.push_back({"markerActions/furnitureStructuralEdits", furnitureStructuralEdits});
	checks.push_back({"markerActions/furnitureFinishFailures", furnitureFinishFailures});
	checks.push_back({"markerActions/furnitureUseCompetition", furnitureUseCompetition});
	checks.push_back({"markerActions/furnitureUseAtomicity", furnitureUseAtomicity});
	checks.push_back({"markerActions/furnitureUseDocuments", furnitureUseDocuments});
}
