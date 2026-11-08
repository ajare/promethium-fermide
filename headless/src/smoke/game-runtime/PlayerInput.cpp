#include "Checks.h"
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/AgentBehaviour.h"
#include "core/DoorSectorObject.h"
#include "core/Simulation.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::findAgent;

	// The nine player-input methods are the whole surface a game plays through
	// (#960 groups them exactly this way). Their shape is a compile-time
	// contract: a rename, a dropped method or a changed parameter fails the
	// build here instead of turning into a missing capability at runtime.
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::getPlayerControlledAgent),
		core::AgentId(game::WorldRuntime::*)() const>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::setPlayerMovementDirection),
		void (game::WorldRuntime::*)(int)>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::setPlayerStairSelection),
		void (game::WorldRuntime::*)(bool, bool)>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::setPlayerCrouch),
		void (game::WorldRuntime::*)(bool)>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::requestPlayerDoorAction),
		void (game::WorldRuntime::*)()>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::getPlayerTransportSelector),
		std::optional<core::World::PlayerTransportSelector>(game::WorldRuntime::*)() const>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::browsePlayerTransportStop),
		void (game::WorldRuntime::*)(int, bool)>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::confirmPlayerTransportStop),
		void (game::WorldRuntime::*)()>);
	static_assert(std::is_same_v<decltype(&game::WorldRuntime::clearPlayerInput),
		void (game::WorldRuntime::*)()>);

	// Every fixture is authored before the World is handed over, because a
	// WorldRuntime cannot set anything up afterwards. What the checks then read
	// is only what a player would see: where the Agent got to, what posture it
	// holds, whether the Door moved, which Stop the vehicle is serving.
	core::AgentId makePlayer(core::World& world, core::AgentId id)
	{
		std::string diagnostic;
		require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour,
			core::PlayerControlledBehaviourRevision, {}, &diagnostic),
			"the fixture Agent cannot be made player-controlled: " + diagnostic);
		return id;
	}

	// The snapshot carries a Sector identity rather than an index, and zero is
	// the null handle, so the World's Sector index is one less than the value.
	uint32_t sectorIndexOf(core::SectorId const& id)
	{
		require(id.value > 0, "the snapshot reports no Sector at all");
		return static_cast<uint32_t>(id.value - 1);
	}

	core::AgentSnapshot const& playerSnapshot(game::WorldRuntime const& runtime, core::AgentId id)
	{
		auto const* found = findAgent(runtime.getSimulationSnapshot(), id);
		require(found != nullptr, "the Player is missing from the snapshot");
		return *found;
	}

	float playerX(game::WorldRuntime const& runtime, core::AgentId id)
	{
		return playerSnapshot(runtime, id).globalPosition.x;
	}

	core::Pose playerPose(game::WorldRuntime const& runtime, core::AgentId id)
	{
		return playerSnapshot(runtime, id).pose;
	}

	uint32_t playerSector(game::WorldRuntime const& runtime, core::AgentId id)
	{
		return sectorIndexOf(playerSnapshot(runtime, id).sectorId);
	}

	// Everything a failing check needs in its message, so a surprise reads as
	// a place rather than as a bare number.
	std::string describe(game::WorldRuntime const& runtime, core::AgentId id)
	{
		auto const& agent = playerSnapshot(runtime, id);
		auto const index = sectorIndexOf(agent.sectorId);
		std::string name = index < runtime.world().getNumSectors()
			? runtime.world().getSector(index)->getName() : "<none>";
		return "sector=" + name + " at " + std::to_string(agent.globalPosition.x) + ","
			+ std::to_string(agent.globalPosition.y) + " pose=" + std::to_string(int(agent.pose));
	}

	void near(float actual, float expected, char const* message)
	{
		require(std::abs(actual - expected) < 0.0001f,
			std::string(message) + ": actual=" + std::to_string(actual)
				+ ", expected=" + std::to_string(expected));
	}

	void resume(game::WorldRuntime& runtime)
	{
		if (runtime.world().isTraversalTopologyDirty())
			require(runtime.rebuildTraversalTopology(), runtime.world().getTopologyDiagnostic());
		require(runtime.resumeSimulation(), "the fixture run cannot resume");
	}

	struct PlayerWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId player{};
	};

	// A Room wide enough to walk the length of and tall enough to stand in.
	PlayerWorld makeRoom(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->finishBuild();
		world->pauseSimulation();
		auto const player = makePlayer(*world, world->createAgent("Player", room, 0, 1.f));
		return { std::move(world), player };
	}

	// Two Rooms stacked in depth with a manual Door between them, and the
	// Player standing at the Door so the contextual action means something.
	// The runtime is held pinned behind a pointer because the fixture hands
	// this struct back by value: a `WorldRuntime` cannot move (#977), so the
	// only way a fixture that returns one can be written is to move the
	// pointer and leave the runtime where it was made.
	struct DoorWorld
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		core::AgentId player{};
		std::shared_ptr<core::Door> door;
		uint32_t front{ 0 };
		uint32_t back{ 0 };
	};

	DoorWorld makeDoorWorld(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 8, 2);
		auto const front = world->addRoom("Front", 0, 0, 0, 8, 1);
		auto const back = world->addRoom("Back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options;
		options.holdOpenSeconds = 30.f;
		auto const created = world->addSectorDoor(0, 0, 2, options);
		auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(
			created.door.sector->getObject(created.door.index))->getDoor();
		require(bool(door), "the Door fixture has no Door object");
		world->finishBuild();
		world->pauseSimulation();
		auto const player = makePlayer(*world, world->createAgent("Player", front, 0, 2.5f));
		DoorWorld built{ std::make_unique<game::WorldRuntime>(std::move(world)), player, door, front, back };
		resume(*built.runtime);
		require(!built.door->isOpen() && !built.door->isOpening(),
			"the fixture Door must start closed");
		require(playerSector(*built.runtime, built.player) == front,
			"the Player must start in the front Room, " + describe(*built.runtime, built.player));
		return built;
	}

	core::TraversalResourceSnapshot vehicle(game::WorldRuntime const& runtime,
		core::TraversalResourceId id)
	{
		auto const snapshot = runtime.getSimulationSnapshot();
		auto const found = std::find_if(snapshot.traversalResources.begin(),
			snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == id; });
		require(found != snapshot.traversalResources.end(), "the Lift is missing from the snapshot");
		return *found;
	}

	uint32_t stopRequests(core::TraversalResourceSnapshot const& resource, uint32_t stop)
	{
		require(stop < resource.liftStopRequestOwnerCounts.size(),
			"the snapshot reports no request count for that Stop");
		return resource.liftStopRequestOwnerCounts[stop];
	}

	// Walks the Player to a Lift's ground-floor Door, waits for it to open and
	// boards it. Standing in the shaft Transit is the only state in which the
	// Stop selector exists at all. The runtime is pinned for the same reason
	// as in `DoorWorld` above (#977).
	struct BoardedLift
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		core::AgentId player{};
		core::TraversalResourceId resource{};
		uint32_t transitSector{ 0 };
	};

	void boardLift(game::WorldRuntime& runtime, core::AgentId player, core::Door& door,
		uint32_t transitSector)
	{
		auto const centre = door.getPosition().x + door.getSize().x * .5f;
		int const direction = playerX(runtime, player) < centre ? 1 : -1;
		// The Player starts beside the Door's control, so one press calls the
		// Lift. Without that call the car never arrives and there is nothing to
		// board.
		runtime.requestPlayerDoorAction();
		runtime.advanceTicks(60);
		require(!runtime.getPlayerTransportSelector().has_value(),
			"a Player outside the vehicle must not be shown a Stop selector");
		runtime.setPlayerMovementDirection(direction);
		for (int tick = 0; tick < 120 && direction * (centre - playerX(runtime, player)) > .05f;
			++tick)
			require(runtime.advanceTick(), "the run stopped while walking to the Lift Door");
		runtime.setPlayerMovementDirection(0);
		for (int tick = 0; tick < 180 && !door.isOpen(); ++tick)
			require(runtime.advanceTick(), "the Lift Door never opened");
		runtime.setPlayerCrouch(true);
		runtime.setPlayerMovementDirection(direction);
		require(runtime.advanceTick(), "the run stopped while crouched at the Lift Door");
		require(playerPose(runtime, player) == core::Pose::Crawling,
			"the fixture must reach the Lift Door crawling, " + describe(runtime, player));
		runtime.requestPlayerDoorAction();
		runtime.setPlayerMovementDirection(0);
		for (int tick = 0; tick < 180 && playerSector(runtime, player) != transitSector; ++tick)
			require(runtime.advanceTick(), "the run stopped while boarding the Lift");
		require(playerSector(runtime, player) == transitSector,
			"the Player must be aboard the Lift to be shown its Stops: doorOpen="
				+ std::to_string(door.isOpen()) + ", " + describe(runtime, player));
	}

	BoardedLift makeBoardedLift()
	{
		auto world = std::make_unique<core::World>("Player transport input", 20, 6);
		auto const lower = world->addRoom("Origin", 0, 0, 0, 6, 1);
		world->addRoom("Middle", 0, 2, 0, 6, 1);
		world->addRoom("Top", 0, 4, 0, 6, 1);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.stopOffsets = { 0, 2, 4 };
		options.minimumDwellSeconds = 5.f;
		options.maximumBoardingSeconds = 20.f;
		auto const created = world->addLift(1, 0, 2, options);
		auto const& firstDoor = created.doors.front();
		auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(
			firstDoor.door.sector->getObject(firstDoor.door.index))->getDoor();
		require(bool(door), "the Lift fixture has no ground-floor Door");
		auto const point = world->lookupInteractionPoint(firstDoor.controls[0].interactionPoint).entity;
		require(point != nullptr, "the Lift Door has no control to stand beside");
		world->finishBuild();
		world->pauseSimulation();
		auto const player = makePlayer(*world, world->createAgent("Player", lower, 0,
			point->getPosition().x - world->getSector(lower)->getPosition().x));
		BoardedLift boarded{ std::make_unique<game::WorldRuntime>(std::move(world)), player,
			created.traversalResource, created.lift.sector->getIndex() };
		resume(*boarded.runtime);
		boardLift(*boarded.runtime, boarded.player, *door, boarded.transitSector);
		return boarded;
	}

	// Builds a Staircase World and drives the Player into it with one stair
	// selection. The two runs differ only in that selection, so any
	// difference in where the Player ends up belongs to it.
	struct StairRun
	{
		float y{ 0.f };
		uint32_t sector{ 0 };
		uint32_t staircase{ 0 };
		std::string where;
	};

	StairRun driveAtStaircase(bool up, bool down)
	{
		auto world = std::make_unique<core::World>("Player Staircase input", 12, 2);
		auto const bottom = world->addRoom("Bottom", 0, 0, 0, 12, 1);
		world->addCorridor(1, 0, 12);
		auto const stairs = world->addStaircase(1, 0, 4, { 3, CORE_SIDE_RIGHT, 0.f });
		world->finishBuild();
		world->pauseSimulation();
		// Just short of the lower endpoint, walking into it.
		auto const player = makePlayer(*world, world->createAgent("Player", bottom, 0, 3.85f));
		game::WorldRuntime runtime(std::move(world));
		resume(runtime);
		runtime.setPlayerStairSelection(up, down);
		runtime.setPlayerMovementDirection(1);
		runtime.advanceTicks(36);
		runtime.setPlayerMovementDirection(0);
		runtime.advanceTicks(6);
		return { playerSnapshot(runtime, player).globalPosition.y, playerSector(runtime, player),
			stairs, describe(runtime, player) };
	}

	// Two Agents in one Room, either of which may be the player-controlled one.
	struct CandidateWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId first{};
		core::AgentId second{};
	};

	CandidateWorld makeCandidates(bool firstIsPlayer, bool secondIsPlayer)
	{
		auto world = std::make_unique<core::World>("Candidate Players", 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->finishBuild();
		world->pauseSimulation();
		auto const first = world->createAgent("First", room, 0, 1.f);
		auto const second = world->createAgent("Second", room, 0, 3.f);
		if (firstIsPlayer) makePlayer(*world, first);
		if (secondIsPlayer) makePlayer(*world, second);
		return { std::move(world), first, second };
	}
}

void registerGameRuntimePlayerInput(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "everyPlayerInputMethodIsCallableThroughTheSharedLibrary",
		[](smoke::Context const&)
		{
			auto fixture = makeRoom("Nine player input methods");
			game::WorldRuntime runtime(std::move(fixture.world));
			resume(runtime);
			require(runtime.getPlayerControlledAgent() == fixture.player,
				"the controlled Agent does not resolve through the runtime");
			runtime.setPlayerMovementDirection(0);
			runtime.setPlayerStairSelection(false, false);
			runtime.setPlayerCrouch(false);
			runtime.requestPlayerDoorAction();
			require(!runtime.getPlayerTransportSelector().has_value(),
				"a Player standing in a Room must not be shown a transport Stop selector");
			runtime.browsePlayerTransportStop(1, false);
			runtime.confirmPlayerTransportStop();
			runtime.clearPlayerInput();
			require(runtime.advanceTicks(60), "the run stopped while exercising the input surface");
			near(playerX(runtime, fixture.player), 1.f,
				"input that was never held must not have moved the Player");
		} });

	checks.push_back({ "steeringMovesTheControlledAgentAndNeutralSteeringStopsIt",
		[](smoke::Context const&)
		{
			auto fixture = makeRoom("Steering");
			game::WorldRuntime runtime(std::move(fixture.world));
			resume(runtime);
			auto const* actor = runtime.world().lookupAgent(fixture.player).entity;
			require(actor != nullptr, "the fixture Player cannot be resolved");
			auto const speed = actor->getWalkSpeed();
			runtime.setPlayerMovementDirection(1);
			runtime.advanceTicks(60);
			near(playerX(runtime, fixture.player), 1.f + speed,
				"held steering must walk the Player at its own speed");
			runtime.setPlayerMovementDirection(0);
			runtime.advanceTicks(60);
			near(playerX(runtime, fixture.player), 1.f + speed,
				"releasing the steering key must stop the Player where it stood");
			require(playerPose(runtime, fixture.player) == core::Pose::Standing,
				"ordinary steering must leave the Player Standing");
		} });

	checks.push_back({ "heldCrouchLowersThePlayerAndReleasingItStandsThemUp",
		[](smoke::Context const&)
		{
			auto fixture = makeRoom("Crouch");
			game::WorldRuntime runtime(std::move(fixture.world));
			resume(runtime);
			require(playerPose(runtime, fixture.player) == core::Pose::Standing,
				"the Player must start Standing");
			runtime.setPlayerCrouch(true);
			runtime.advanceTick();
			require(playerPose(runtime, fixture.player) == core::Pose::Crouching,
				"held crouch must lower the Player to Crouching, "
					+ describe(runtime, fixture.player));
			runtime.setPlayerCrouch(false);
			runtime.advanceTick();
			require(playerPose(runtime, fixture.player) == core::Pose::Standing,
				"releasing crouch must stand the Player back up");
		} });

	checks.push_back({ "stairSelectionDecidesWhetherThePlayerMountsTheStaircase",
		[](smoke::Context const&)
		{
			auto const unselected = driveAtStaircase(false, false);
			require(unselected.y == 0.f,
				"a Player with no stair selection must stay on the floor, " + unselected.where);
			require(unselected.sector != unselected.staircase,
				"without a selection the Player must not be on the Staircase, " + unselected.where);
			auto const ascending = driveAtStaircase(true, false);
			require(ascending.sector == ascending.staircase,
				"the Up selection must mount the Staircase, " + ascending.where);
			require(ascending.y > 0.f,
				"the Up selection must climb above the lower landing, " + ascending.where);
		} });

	checks.push_back({ "theContextualActionOpensAManualDoorAndThenCrossesIt",
		[](smoke::Context const&)
		{
			auto fixture = makeDoorWorld("Player Door input");
			fixture.runtime->requestPlayerDoorAction();
			require(fixture.runtime->advanceTick(), "the run stopped while opening the Door");
			require(fixture.door->isOpening() || fixture.door->isOpen(),
				"the contextual action must open the manual Door");
			require(fixture.runtime->advanceTicks(180), "the run stopped while the Door opened");
			require(fixture.door->isOpen(), "the Door never reached Open");
			require(playerSector(*fixture.runtime, fixture.player) == fixture.front,
				"opening a Door must not move the Player onto it, "
					+ describe(*fixture.runtime, fixture.player));

			fixture.runtime->requestPlayerDoorAction();
			require(fixture.runtime->advanceTick(), "the run stopped while crossing the Door");
			require(fixture.runtime->advanceTicks(60), "the run stopped after crossing the Door");
			require(playerSector(*fixture.runtime, fixture.player) == fixture.back,
				"a second contextual action must cross the open Door into the Room behind it, "
					+ describe(*fixture.runtime, fixture.player));
		} });

	checks.push_back({ "transportStopBrowseMovesTheHighlightAndConfirmRequestsIt",
		[](smoke::Context const&)
		{
			auto boarded = makeBoardedLift();
			auto selector = boarded.runtime->getPlayerTransportSelector();
			require(selector.has_value(), "a boarded Player must be shown the Stop selector");
			require(selector->stops.size() == 3, "the Lift must offer three Stops, got "
				+ std::to_string(selector->stops.size()));
			require(selector->focus == 1, "the highlight must start on the Stop being boarded, got "
				+ std::to_string(selector->focus));
			require(stopRequests(vehicle(*boarded.runtime, boarded.resource), 2) == 0,
				"nothing may be requested before a confirm");

			boarded.runtime->browsePlayerTransportStop(1, false);
			auto const browsed = boarded.runtime->getPlayerTransportSelector();
			require(browsed.has_value() && browsed->focus == 2,
				"browsing forward must move the highlight to the next Stop, got "
					+ std::to_string(browsed ? int(browsed->focus) : -1));
			require(stopRequests(vehicle(*boarded.runtime, boarded.resource), 2) == 0,
				"browsing alone must not request a Stop");

			boarded.runtime->confirmPlayerTransportStop();
			require(stopRequests(vehicle(*boarded.runtime, boarded.resource), 2) == 1,
				"confirming must request the highlighted Stop");
			boarded.runtime->confirmPlayerTransportStop();
			require(stopRequests(vehicle(*boarded.runtime, boarded.resource), 2) == 1,
				"confirming the same Stop twice must not double-book it");
		} });

	checks.push_back({ "theControlledAgentIsTheLowestIdEligibleAgent",
		[](smoke::Context const&)
		{
			{
				auto none = makeCandidates(false, false);
				game::WorldRuntime runtime(std::move(none.world));
				require(!runtime.getPlayerControlledAgent(),
					"with no player-controlled Agent nothing may be controlled");
			}
			{
				auto only = makeCandidates(false, true);
				game::WorldRuntime runtime(std::move(only.world));
				require(runtime.getPlayerControlledAgent() == only.second,
					"the only player-controlled Agent must be the controlled one");
			}
			{
				auto both = makeCandidates(true, true);
				game::WorldRuntime runtime(std::move(both.world));
				require(runtime.getPlayerControlledAgent() == both.first,
					"control must go to the lowest stable ID, not to the most recent assignment");
			}
		} });

	checks.push_back({ "clearPlayerInputStopsTheAgentOnTheNextTick",
		[](smoke::Context const&)
		{
			auto fixture = makeRoom("Clear input");
			game::WorldRuntime runtime(std::move(fixture.world));
			resume(runtime);
			runtime.setPlayerMovementDirection(1);
			runtime.setPlayerCrouch(true);
			runtime.advanceTicks(30);
			require(playerX(runtime, fixture.player) > 1.01f,
				"the fixture must be walking before the clear, "
					+ describe(runtime, fixture.player));
			require(playerPose(runtime, fixture.player) != core::Pose::Standing,
				"the fixture must be lowered before the clear, "
					+ describe(runtime, fixture.player));

			auto const stopped = playerSnapshot(runtime, fixture.player).globalPosition;
			runtime.clearPlayerInput();
			require(runtime.advanceTicks(60), "the run stopped after clearing input");
			auto const after = playerSnapshot(runtime, fixture.player).globalPosition;
			require(std::abs(after.x - stopped.x) < 0.0001f
				&& std::abs(after.y - stopped.y) < 0.0001f,
				"cleared input must leave the Player where it stopped, dx="
					+ std::to_string(after.x - stopped.x));
			require(playerPose(runtime, fixture.player) == core::Pose::Standing,
				"clearing input must stand the Player back up");
		} });

	checks.push_back({ "inputHeldWhilePausedIsNotDeliveredOnResume",
		[](smoke::Context const&)
		{
			auto fixture = makeRoom("Paused input");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.pauseSimulation();
			runtime.setPlayerMovementDirection(1);
			runtime.setPlayerCrouch(true);
			require(runtime.resumeSimulation(), "the paused fixture cannot resume");
			require(runtime.advanceTicks(60), "the run stopped after resuming");
			near(playerX(runtime, fixture.player), 1.f,
				"input held while paused must not drive the Player after a resume");
			require(playerPose(runtime, fixture.player) == core::Pose::Standing,
				"crouch held while paused must not lower the Player after a resume");
		} });

	checks.push_back({ "clearingInputDiscardsAPressedActionThatWouldOtherwiseFire",
		[](smoke::Context const&)
		{
			// The Player is standing at the Door, so an undisposed press opens
			// it. Clearing first must throw the press away rather than deliver
			// it on some later Tick.
			auto cleared = makeDoorWorld("Discarded Door press");
			cleared.runtime->requestPlayerDoorAction();
			cleared.runtime->clearPlayerInput();
			require(cleared.runtime->advanceTicks(60), "the run stopped after discarding the press");
			require(!cleared.door->isOpening() && !cleared.door->isOpen(),
				"a cleared press must never reach the Door, opening="
					+ std::to_string(cleared.door->isOpening()) + " open="
					+ std::to_string(cleared.door->isOpen()));

			auto kept = makeDoorWorld("Delivered Door press");
			kept.runtime->requestPlayerDoorAction();
			require(kept.runtime->advanceTicks(2), "the run stopped delivering the press");
			require(kept.door->isOpening() || kept.door->isOpen(),
				"the same press, not cleared, must reach the Door");
		} });
}
