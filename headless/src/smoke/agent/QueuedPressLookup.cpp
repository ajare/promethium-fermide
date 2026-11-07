// Derived queued-press lookup by Agent, for ticket #626.
//
// The Movement phase used to scan every InteractionPoint and its whole
// queue for each remote Agent to decide whether it must stay stationary
// while pressing. It now asks a derived per-Agent index directly, which
// is maintained by the request/queue lifecycle transitions. These checks
// pin the contract by comparing positions every tick:
//
//   * a remote Agent pauses for exactly the existing queued/active press
//     interval and resumes the tick after the press completes
//   * pending device travel after the press does not keep an Agent
//     stationary: the Agent walks while its request is still Pending and
//     the device is still moving
//   * a shared activated operation releases the queued Agent without a
//     second press
//   * coalesced, cancelled, rejected, removed and retired work never
//     leave a stale press-state entry in the derived index
//   * the direct query visits only the queried Agent's own index entries;
//     it never scales with the point or queue registries
//     (operation-count evidence)
//
// Registration: agent smoke module.

#include "Checks.h"
#include "../simulation/InteractionResults.h"

#include "core/World.h"
#include "core/Agent.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/Graph.h"

#include <cmath>
#include <string>
#include <vector>

namespace core
{
	// Declared a friend of World: exercises the private derived index, the
	// query-operation counters and the coordinator's direct query.
	struct QueuedPressLookupTestAccess
	{
		static std::size_t indexedAgents(core::World const& world)
		{
			return world.mQueuedPressesByAgent.size();
		}
		static std::size_t queuedRequests(core::World const& world, core::AgentId actor)
		{
			auto const found = world.mQueuedPressesByAgent.find(actor);
			return found == world.mQueuedPressesByAgent.end() ? 0 : found->second.size();
		}
		static bool hasQueuedPressWork(core::World const& world, core::AgentId actor)
		{
			return world.mSimulationCoordinator.hasQueuedPressWork(actor);
		}
		static uint64_t queryVisits(core::World const& world)
		{
			return world.mQueuedPressQueryVisits;
		}
		static void resetQueryVisits(World& world)
		{
			world.mQueuedPressQueryVisits = 0;
		}
	};
}

namespace
{
	using smoke::require;
	using Access = core::QueuedPressLookupTestAccess;

	core::AgentId makeRemote(core::World& world, std::string const& name, uint32_t sector, float x)
	{
		auto const id = world.createAgent(name, sector, 0, x);
		require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::RemoteControl)
			&& world.setAgentIndividualObjectUsageDistance(id, 50.f),
			"Remote authoring failed for " + name);
		return id;
	}

	void walkToMarker(core::World& world, core::AgentId agent, uint32_t destination)
	{
		auto const entity = world.lookupAgent(agent).entity;
		auto const target = world.getGraph()->getVertexByIdentifier(destination);
		require(target != nullptr, "Walk destination vertex missing");
		auto const path = world.getGraph()->calculatePath(entity, target);
		require(path != nullptr, "Walk path missing");
		entity->setPath(path, true);
	}

	// Fore Room (Layer 0) and Back Room (Layer 1) joined by a
	// RemoteControlled Door with buttons on both sides. The destination
	// marker keeps the walk inside the Fore Room: a route through the Door
	// would add its own anticipatory remote preparation, which is a
	// separate contract from the plain queued-press window under test here.
	// The Fore Room Light switch and the Fore-side Door button are
	// physical controls a remote Agent may operate; the Door keeps
	// travelling long after the press.
	struct PressFixture
	{
		core::World world{ "Queued press lookup", 12, 2 };
		uint32_t fore{};
		uint32_t back{};
		uint32_t destination{};
		std::shared_ptr<const core::Door> doorObject{};
		core::InteractionPointId lightPoint{};
		core::InteractionPointId doorPoint{};
		core::AgentId actor{};
		unsigned durationTicks{ 1 };

		PressFixture()
		{
			fore = world.addRoom("Fore", 0, 0, 0, 12, 1);
			back = world.addRoom("Back", 1, 0, 0, 12, 1);
			core::World::CreateDoorOptions options{};
			options.activationMode = core::DoorActivationMode::RemoteControlled;
			options.controls[0] = true;
			options.controls[1] = true;
			auto const made = world.addSectorDoor(0, 0, 5, options);
			doorObject = std::dynamic_pointer_cast<const core::Door>(
				made.door.sector->getObject(made.door.index)->_getObject());
			require(doorObject != nullptr, "Door fixture object missing");
			world.addSectorMarker(fore, 0, 10.5f, &destination);
			auto const light = world.addSectorLightSwitch(fore, 3);
			lightPoint = light.interactionPoint;
			doorPoint = made.controls[0].interactionPoint;
			world.finishBuild();
			world.pauseSimulation();
			// Pick the Door button that lives in the Fore Room.
			auto const front = world.lookupInteractionPoint(made.controls[0].interactionPoint).entity;
			require(front != nullptr, "Door front control missing");
			if (front->getSector() != core::SectorId{ static_cast<uint64_t>(fore) + 1 })
			{
				auto const other = world.lookupInteractionPoint(made.controls[1].interactionPoint).entity;
				require(other && other->getSector() == core::SectorId{ static_cast<uint64_t>(fore) + 1 },
					"Door Fore-side control missing");
				doorPoint = made.controls[1].interactionPoint;
			}
			durationTicks = static_cast<unsigned>(
				world.lookupInteractionPoint(lightPoint).entity->getDurationTicks());
			require(durationTicks >= 1, "Light switch press duration degenerate");
			actor = makeRemote(world, "Remote presser", fore, 1.f);
		}

		core::AgentId makeRemoteAgent(std::string const& name, uint32_t sector, float x)
		{
			return makeRemote(world, name, sector, x);
		}

		void startWalking(core::AgentId agent)
		{
			walkToMarker(world, agent, destination);
		}
	};

	// The held window is exactly the queued/active press interval (#626):
	// frozen from the allocation tick through the press tick, moving again
	// the tick after, with the index following the queue every step.
	void queuedPressHoldsForExactlyThePressInterval(smoke::Context const&)
	{
		PressFixture f;
		f.startWalking(f.actor);
		f.world.resumeSimulation();
		f.world.advanceTicks(10);
		auto agent = f.world.lookupAgent(f.actor).entity;
		require(agent->getGlobalPosition().x > 1.05f, "Agent did not get under way");

		f.world.resumeSimulation();
		auto const frozenAt = agent->getGlobalPosition();
		// Request while running: a pause/resume cycle re-plans retained
		// paths and cancels pending work, which is a different contract.
		auto const request = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(request), "Queued press request refused");
		require(Access::queuedRequests(f.world, f.actor) == 1, "Enqueued press not indexed");
		require(Access::hasQueuedPressWork(f.world, f.actor), "Direct query missed the queued press");
		for (unsigned tick = 1; tick <= f.durationTicks; ++tick)
		{
			f.world.advanceTick();
			require(agent->getGlobalPosition() == frozenAt,
				"Agent moved during press tick " + std::to_string(tick));
			if (tick < f.durationTicks)
				require(Access::queuedRequests(f.world, f.actor) == 1,
					"Queued press index entry vanished during tick " + std::to_string(tick));
		}
		// The press completed inside the window: the request left the queue
		// and the derived index with it.
		require(Access::queuedRequests(f.world, f.actor) == 0,
			"Completed press left a queued index entry");
		require(!Access::hasQueuedPressWork(f.world, f.actor),
			"Stale press-state after completion");
		f.world.advanceTick();
		require(agent->getGlobalPosition() != frozenAt,
			"Agent stayed frozen after the press interval");
		// The shared operation completes a tick or two after the press.
		f.world.advanceTicks(5);
		auto const results = simulation_smoke::observedInteractionResults(f.world, { request });
		require(results.at(request) == core::InteractionResult::Succeeded,
			"Queued press did not publish success");
		require(Access::indexedAgents(f.world) == 0,
			"Retired work left an index key behind");
	}

	// Pending device travel alone does not keep an Agent stationary (#626):
	// once the press leaves the queue the Agent walks on while the Door
	// keeps travelling and the request is still Pending.
	void deviceTravelAfterPressDoesNotHold(smoke::Context const&)
	{
		PressFixture f;
		f.startWalking(f.actor);
		f.world.resumeSimulation();
		f.world.advanceTicks(10);
		auto agent = f.world.lookupAgent(f.actor).entity;
		require(agent->getGlobalPosition().x > 1.05f, "Operator did not get under way");

		auto const request = f.world.requestInteraction(f.doorPoint, f.actor);
		require(bool(request), "Door press refused");
		require(Access::queuedRequests(f.world, f.actor) == 1, "Door press not indexed");
		// Run until the press leaves the queue.
		unsigned ticks = 0;
		while (Access::queuedRequests(f.world, f.actor) == 1 && ticks < 30)
		{
			++ticks;
			f.world.advanceTick();
		}
		require(Access::queuedRequests(f.world, f.actor) == 0, "Door press never left the queue");
		// The Door is still travelling and the request is still Pending.
		require(!f.doorObject->isOpen(), "Door travel finished before the release check");
		auto const releaseResult = simulation_smoke::observedInteractionResult(f.world, request);
		require(releaseResult == core::InteractionResult::Pending,
			"Request resolved during device travel check result=" + std::to_string(
				static_cast<int>(releaseResult)));
		auto const atRelease = agent->getGlobalPosition();
		f.world.advanceTick();
		require(agent->getGlobalPosition() != atRelease,
			"Agent stayed stationary while the device still travelled");
		require(!Access::hasQueuedPressWork(f.world, f.actor),
			"Device travel re-created press-state");
		f.world.advanceTicks(180);
		require(f.doorObject->isOpen()
			&& simulation_smoke::observedInteractionResult(f.world, request)
				== core::InteractionResult::Succeeded,
			"Door travel or request completion lost");
	}

	// A queued request whose shared operation has already been activated is
	// released without a second press (#626): the queued Agent resumes the
	// tick after the pop while its request is still Pending.
	void sharedActivatedOperationReleasesQueuedAgent(smoke::Context const&)
	{
		PressFixture f;
		auto const second = f.makeRemoteAgent("Second operator", f.fore, 1.5f);
		f.startWalking(f.actor);
		f.startWalking(second);
		f.world.resumeSimulation();
		f.world.advanceTicks(10);
		auto secondAgent = f.world.lookupAgent(second).entity;

		auto const firstRequest = f.world.requestInteraction(f.doorPoint, f.actor);
		auto const sharedRequest = f.world.requestInteraction(f.doorPoint, second);
		require(bool(firstRequest) && bool(sharedRequest) && firstRequest != sharedRequest,
			"Two-operator shared press not accepted");
		require(Access::queuedRequests(f.world, f.actor) == 1
			&& Access::queuedRequests(f.world, second) == 1,
			"Shared press requests not indexed while queued");
		// The first operator presses after its press duration; the second
		// is then dequeued as reused work on the next allocation.
		unsigned ticks = 0;
		while (Access::queuedRequests(f.world, second) == 1 && ticks < 30)
		{
			++ticks;
			f.world.advanceTick();
		}
		require(Access::queuedRequests(f.world, second) == 0,
			"Queued shared request never released");
		require(!f.doorObject->isOpen(),
			"Shared release check ran after Door travel ended");
		require(simulation_smoke::observedInteractionResult(f.world, sharedRequest)
			== core::InteractionResult::Pending,
			"Shared request resolved before the release check");
		auto const atRelease = secondAgent->getGlobalPosition();
		f.world.advanceTick();
		require(secondAgent->getGlobalPosition() != atRelease,
			"Queued operator stayed frozen after the shared release");
		require(!Access::hasQueuedPressWork(f.world, second),
			"Released shared request kept press-state");
	}

	// Cancellation clears the index immediately and the Agent never pauses (#626).
	void cancelledPressLeavesNoPressState(smoke::Context const&)
	{
		PressFixture f;
		f.startWalking(f.actor);

		f.world.resumeSimulation();
		auto const request = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(request), "Cancel-fixture request refused");
		require(Access::queuedRequests(f.world, f.actor) == 1, "Cancel-fixture request not indexed");
		require(f.world.cancelInteraction(request), "Cancellation refused");
		require(Access::queuedRequests(f.world, f.actor) == 0,
			"Cancelled request left an index entry");
		require(!Access::hasQueuedPressWork(f.world, f.actor),
			"Cancelled request still answers the direct query");

		f.world.resumeSimulation();
		f.world.advanceTicks(3 * f.durationTicks + 5);
		require(simulation_smoke::observedInteractionResult(f.world, request)
			== core::InteractionResult::Cancelled, "Cancelled request result lost");
		require(Access::indexedAgents(f.world) == 0, "Cancelled work left an index key");
	}

	// A request cancelled while queued behind an active press releases that
	// Agent immediately; the pressing Agent keeps its held window (#626).
	void cancelBehindActivePressReleasesOnlyTheQueuedAgent(smoke::Context const&)
	{
		PressFixture f;
		auto const queued = f.makeRemoteAgent("Queued behind", f.fore, 1.5f);
		f.startWalking(f.actor);
		f.startWalking(queued);
		f.world.resumeSimulation();
		f.world.advanceTicks(10);
		auto presser = f.world.lookupAgent(f.actor).entity;
		auto waiter = f.world.lookupAgent(queued).entity;

		auto const pressing = f.world.requestInteraction(f.lightPoint, f.actor);
		auto const waiting = f.world.requestInteraction(f.lightPoint, queued);
		require(bool(pressing) && bool(waiting) && pressing != waiting,
			"Queue-behind requests not accepted");
		// Hold the presser's window: run through its press ticks while the
		// waiter is still queued behind it.
		for (unsigned tick = 1; tick <= f.durationTicks; ++tick)
		{
			f.world.advanceTick();
			require(Access::queuedRequests(f.world, queued) == 1,
				"Queued-behind request left the index during the press window");
		}
		require(f.world.cancelInteraction(waiting), "Queued-behind cancellation refused");
		require(Access::queuedRequests(f.world, queued) == 0,
			"Cancelled queued-behind request left an index entry");
		require(Access::queuedRequests(f.world, f.actor) == 0,
			"Pressing Agent should have completed its press by now");

		auto const waiterAt = waiter->getGlobalPosition();
		f.world.advanceTick();
		require(waiter->getGlobalPosition() != waiterAt,
			"Cancelled queued-behind Agent stayed frozen");
	}

	// Coalesced and refused requests add nothing to the index (#626): the
	// same-point repeat returns the same record, the different-point repeat
	// is an empty-handle refusal, and neither grows the entry set.
	void coalescedAndRefusedRequestsAddNoEntries(smoke::Context const&)
	{
		PressFixture f;
		f.world.pauseSimulation();
		auto const request = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(request), "Coalesce-fixture request refused");
		require(Access::queuedRequests(f.world, f.actor) == 1, "Coalesce base not indexed");

		auto const repeat = f.world.requestInteraction(f.lightPoint, f.actor);
		require(repeat == request, "Same-point repeat did not coalesce to the same record");
		require(Access::queuedRequests(f.world, f.actor) == 1,
			"Coalesced repeat added an index entry");

		auto const refused = f.world.requestInteraction(f.doorPoint, f.actor);
		require(!refused, "Different-point request while pending was not an empty-handle refusal");
		require(Access::queuedRequests(f.world, f.actor) == 1,
			"Refused second-point request added an index entry");
		require(Access::indexedAgents(f.world) == 1, "Refusal grew the indexed Agent set");
		f.world.cancelInteraction(request);
		require(Access::indexedAgents(f.world) == 0, "Cleanup cancellation left entries");
	}

	// A recorded permission rejection is not queued work (#626): the Agent
	// keeps walking and the index never sees the record.
	void rejectedPressIsRecordedButNotQueued(smoke::Context const&)
	{
		PressFixture f;
		auto const gate = f.world.addAccessPermission("Press gate");
		require(bool(gate), "Permission fixture failed");
		require(f.world.setInteractionPointPermissionRequirement(f.lightPoint, { gate }),
			"Point permission requirement failed");
		f.startWalking(f.actor);

		f.world.resumeSimulation();
		auto const rejected = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(rejected), "Permission rejection did not record a request");
		require(f.world.lookupInteractionRequest(rejected).entity->getResult()
			== core::InteractionResult::Rejected, "Rejection result not recorded");
		require(Access::queuedRequests(f.world, f.actor) == 0,
			"Rejected request entered the derived index");
		require(!Access::hasQueuedPressWork(f.world, f.actor),
			"Rejected request answers the direct query");

		f.world.resumeSimulation();
		auto const before = f.world.lookupAgent(f.actor).entity->getGlobalPosition();
		f.world.advanceTicks(5);
		require(f.world.lookupAgent(f.actor).entity->getGlobalPosition() != before,
			"Rejected request held the Agent stationary");
	}

	// Every removal path clears the derived index (#626): the point going
	// away, the explicit request removal, and the Agent removal.
	void removalPathsClearTheIndex(smoke::Context const&)
	{
		PressFixture f;
		f.world.pauseSimulation();

		auto const queued = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(queued) && Access::queuedRequests(f.world, f.actor) == 1,
			"Removal-fixture request not indexed");
		require(f.world.removeInteractionPoint(f.lightPoint).removed,
			"Interaction point removal refused");
		require(Access::indexedAgents(f.world) == 0,
			"Point removal left press-state behind");

		// Explicit request removal: the stale handle may stay in the point
		// queue until dequeued, but the index must not keep it.
		auto const removed = f.world.requestInteraction(f.doorPoint, f.actor);
		require(bool(removed) && Access::queuedRequests(f.world, f.actor) == 1,
			"Door request not indexed");
		require(f.world.removeInteractionRequest(removed).removed,
			"Explicit request removal refused");
		require(Access::queuedRequests(f.world, f.actor) == 0,
			"Explicit request removal left an index entry");
		require(!Access::hasQueuedPressWork(f.world, f.actor),
			"Removed request answers the direct query");

		// Agent removal: an idle Agent with a queued request leaves no key.
		auto const idle = f.makeRemoteAgent("Idle remover", f.fore, 4.f);
		auto const idleRequest = f.world.requestInteraction(f.doorPoint, idle);
		require(bool(idleRequest) && Access::queuedRequests(f.world, idle) == 1,
			"Idle request not indexed");
		require(f.world.removeAgent(idle).removed, "Idle Agent removal refused");
		require(Access::queuedRequests(f.world, idle) == 0,
			"Agent removal left its index key");
		require(Access::indexedAgents(f.world) == 0,
			"Removal paths left some press-state behind");
	}

	// Operation-count evidence (#626): the direct query visits only the
	// queried Agent's own entries. With 64 queued presses spread over the
	// World's controls, a query for the watched Agent visits exactly one
	// record per call - the eliminated scan visited every point and every
	// queue entry instead.
	void queryScalesWithOwnEntriesNotTheRegistries(smoke::Context const&)
	{
		PressFixture f;
		f.world.pauseSimulation();
		constexpr unsigned Crowd = 64;
		for (unsigned i = 0; i < Crowd; ++i)
		{
			// Alternate between the two Fore-side controls so the crowd
			// spans several queues and points.
			auto const point = (i % 2 == 0) ? f.lightPoint : f.doorPoint;
			auto const agent = f.makeRemoteAgent("Crowd " + std::to_string(i), f.fore,
				2.f + static_cast<float>(i) * 0.05f);
			auto const request = f.world.requestInteraction(point, agent);
			require(bool(request), "Crowd request refused");
		}
		// The watched Agent has exactly one queued press of its own.
		auto const watched = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(watched) && Access::queuedRequests(f.world, f.actor) == 1,
			"Watched press not indexed");
		require(Access::indexedAgents(f.world) == Crowd + 1,
			"Crowd index does not hold every queued Agent");

		Access::resetQueryVisits(f.world);
		for (unsigned round = 0; round < 100; ++round)
			require(Access::hasQueuedPressWork(f.world, f.actor),
				"Watched press lost during scaling rounds");
		require(Access::queryVisits(f.world) == 100,
			"Direct query visited more than the queried Agent's own entries: visits="
			+ std::to_string(Access::queryVisits(f.world)));

		// An Agent with no queued work costs nothing at all.
		auto const idle = f.makeRemoteAgent("Idle observer", f.fore, 1.f);
		for (unsigned round = 0; round < 100; ++round)
			require(!Access::hasQueuedPressWork(f.world, idle),
				"Idle Agent wrongly reports press-state");
		require(Access::queryVisits(f.world) == 100,
			"Idle Agent query visited registry entries");

		// The eliminated per-Agent scan walked every point and every queue
		// entry per query: at least Crowd + 1 queued requests across the
		// point registry, i.e. thousands of visits for the same 100
		// queries, against the 100 recorded here.
		require(Access::queryVisits(f.world) == 100, "Scaling evidence changed shape");
	}

	// Terminal records retire on the tick boundary; the index stays clean (#626).
	void retirementLeavesNoPressState(smoke::Context const&)
	{
		PressFixture f;
		f.startWalking(f.actor);
		f.world.resumeSimulation();
		auto const request = f.world.requestInteraction(f.lightPoint, f.actor);
		require(bool(request), "Retirement-fixture request refused");
		f.world.advanceTicks(f.durationTicks + 10);
		require(Access::indexedAgents(f.world) == 0,
			"Retirement left an index key");
		for (auto const& record : f.world.getSimulationSnapshot().interactionRequests)
			require(record.id != request,
				"Retirement-fixture request never retired from the snapshot");
	}
}

void agent_smoke::registerQueuedPressLookup(std::vector<smoke::Check>& checks)
{
	checks.push_back({"queuedPressHoldsForExactlyThePressInterval",
		queuedPressHoldsForExactlyThePressInterval });
	checks.push_back({"queuedPressDeviceTravelDoesNotHold", deviceTravelAfterPressDoesNotHold });
	checks.push_back({"queuedPressSharedOperationRelease", sharedActivatedOperationReleasesQueuedAgent });
	checks.push_back({"queuedPressCancellationClears", cancelledPressLeavesNoPressState });
	checks.push_back({"queuedPressCancelBehindActive", cancelBehindActivePressReleasesOnlyTheQueuedAgent });
	checks.push_back({"queuedPressCoalesceAndRefusal", coalescedAndRefusedRequestsAddNoEntries });
	checks.push_back({"queuedPressRejectedNotQueued", rejectedPressIsRecordedButNotQueued });
	checks.push_back({"queuedPressRemovalPathsClear", removalPathsClearTheIndex });
	checks.push_back({"queuedPressQueryScalingEvidence", queryScalesWithOwnEntriesNotTheRegistries });
	checks.push_back({"queuedPressRetirementClean", retirementLeavesNoPressState });
}
