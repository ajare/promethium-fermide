#include "Checks.h"
#include "core/AirlockTransit.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/Path.h"
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include "core/YamlSerializer.h"
#include "core/RouteTraversalInputs.h"
#include "core/MarkerSectorObject.h"
#include "core/MobilityProfile.h"
#include "core/Exceptions.h"

namespace
{
	using smoke::require;
	void editSafety(smoke::Context const&)
	{
		for (int stage = 0; stage < 6; ++stage)
		{
			core::World world("Airlock edit safety", 14, 2);
			for (uint32_t row = 0; row < 2; ++row)
			{
				world.addRoom("Left", 0, row, 0, 4, 1);
				world.addCorridor(0, row, row == 1 && stage == 4 ? 7 : 6, 4, 1);
			}
			auto index = world.addAirlock(0, 0, 4, 2, 1);
			auto marker = world.addSectorMarker(1, 0, 2.0f);
			world.finishBuild();
			auto id = world.createAgent("Traveller", 0, 0, 2.0f);
			auto agent = world.lookupAgent(id).entity;
			agent->setPath(world.getGraph()->calculatePath(agent,
				world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
			bool reached = false;
			for (unsigned tick = 0; tick < 2200 && !reached; ++tick)
			{
				world.advanceTick();
				auto state = world.getSimulationSnapshot().airlocks.at(0);
				if (stage == 0 || stage >= 4) reached = !state.reservations.empty() && state.crossings.empty() && state.occupants.empty();
				else if (stage == 1) reached = !state.crossings.empty() && state.occupants.empty();
				else if (stage == 2) reached = !state.occupants.empty() && state.crossings.empty();
				else reached = !state.crossings.empty() && !state.occupants.empty();
			}
			require(reached, "Edit safety fixture never reached requested journey boundary");
			if (stage > 0 && stage < 4)
			{
				auto refuse = [&] {
					require(!world.planResizeAirlock(index, 4, 1, 2).valid
						&& !world.planResizeAirlock(index, 4, 0, 2).valid
						&& !world.planRemoveAirlock(index).valid, "Occupied/crossing chamber edit accepted");
					core::World::AirlockEditPlan forged;
					forged.sectorIndex = index; forged.remove = true; forged.valid = true;
					bool rejected = false;
					try { world.applyAirlockEdit(forged); } catch (core::Exception const&) { rejected = true; }
					require(rejected && world.getNumSectors() == 5, "Stale/forged plan bypassed safety");
				};
				refuse(); world.pauseSimulation();
				auto state = world.getSimulationSnapshot().airlocks.at(0);
				refuse(); world.advanceTicks(60);
				auto after = world.getSimulationSnapshot().airlocks.at(0);
				require(after.occupants == state.occupants && after.crossings == state.crossings
					&& after.reservations == state.reservations, "Paused rejected edit changed journey");
				world.resumeSimulation();
			}
			else
			{
				world.pauseSimulation();
				auto plan = stage == 5 ? world.planRemoveAirlock(index)
					: world.planResizeAirlock(index, 4, 1, stage == 4 ? 3 : 2);
				require(plan.valid, "Empty reserved chamber edit refused");
				index = world.applyAirlockEdit(plan);
				auto state = world.getSimulationSnapshot();
				require(state.traversalRequests.empty() && state.traversalPermits.empty(), "Empty edit retained stale requests/permits");
				if (stage == 5)
				{
					require(state.airlocks.empty() && state.interactionPoints.empty()
						&& state.traversalResources.empty(), "Waiting deletion retained stale resources");
					world.resumeSimulation(); world.advanceTicks(600);
					agent = world.lookupAgent(id).entity;
					require(agent && agent->getSector()->getIndex() == 0 && agent->getState() == core::Agent::State::Idle,
						"Deleted route did not recover safely through Route loss");
					continue;
				}
				require(state.airlocks.at(0).reservations.empty() && state.airlocks.at(0).occupants.empty()
					&& state.airlocks.at(0).crossings.empty(), "Empty edit retained stale reservations");
				index = world.applyAirlockEdit(world.planResizeAirlock(index, 4, 0, 2));
				world.resumeSimulation();
				agent = world.lookupAgent(id).entity;
				require(agent, "Structural replay lost waiting Agent identity");
			}
			for (unsigned tick = 0; tick < 2400 && agent->getSector()->getIndex() != 1; ++tick) world.advanceTick();
			require(agent->getSector()->getIndex() == 1, "Agent did not recover its route after edit/refusal stage=" + std::to_string(stage));
		}
	}

	void journeys(smoke::Context const&)
	{
		for (uint32_t width : { 1u, 2u, 5u })
			for (int direction : { 0, 1 })
				for (float seconds : { 1.0f, 3.0f, 10.0f })
				{
					core::World world("Airlock journey", width + 8, 2);
					auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
					auto right = world.addCorridor(0, 0, width + 3, 3, 1);
					auto index = world.addAirlock(0, 0, 3, width, seconds);
					auto start = direction == 0 ? left : right;
					auto end = direction == 0 ? right : left;
					auto marker = world.addSectorMarker(end, 0, 1.5f);
					world.finishBuild();
					auto id = world.createAgent("Traveller", start, 0, 1.5f);
					auto agent = world.lookupAgent(id).entity;
					auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
					require(bool(path), "Airlock route unavailable");
					agent->setPath(path, true);
					bool boarded = false, exited = false, cycled = false;
					uint64_t cycleStart = 0;
					for (uint32_t tick = 0; tick < 3600; ++tick)
					{
						require(world.advanceTick(), "Airlock tick refused");
						auto const state = world.getSimulationSnapshot().airlocks.at(0);
						require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Airlock interlock violated");
						if (!boarded && agent->getSector()->getIndex() != index)
							require(state.cycleComplete, "First entry unexpectedly required an initial cycle: width=" + std::to_string(width) + " direction=" + std::to_string(direction) + " tick=" + std::to_string(tick) + " reservations=" + std::to_string(state.reservations.size()) + " entry=" + std::to_string(state.entrySide));
						if (!state.cycleComplete)
						{
							require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed, "Opening during cycle");
							if (!cycleStart)
							{
								cycleStart = world.getSimulationTick();
								require(std::abs(state.remainingCycleSeconds - seconds) < world.getFixedTimestep(), "Cycle did not begin at the fully-closed boundary");
							}
							cycled = true;
						}
						if (cycleStart && state.cycleComplete)
						{
							require(world.getSimulationTick() - cycleStart >= core::secondsToTicks(seconds, world.getFixedTimestep()), "Cycle completed early");
							if (boarded && !exited)
								require(state.doors[1 - direction] == core::DoorSnapshotState::Opening
									&& world.getSimulationTick() - cycleStart == core::secondsToTicks(seconds, world.getFixedTimestep()),
									"Exit did not open automatically when the closed-door cycle completed");
							cycleStart = 0;
						}
						for (auto const& point : world.getSimulationSnapshot().interactionPoints)
							require(point.sectorId.value != index + 1, "Automatic chamber retained an internal interaction point");
						for (auto const& request : world.getSimulationSnapshot().traversalRequests)
							if (request.resource == std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index))->getTraversalResourceId()
								&& request.state == core::TraversalRequestState::Granted)
							{
								auto side = request.destinationSector.value == index + 1 ? direction : 1 - direction;
								require(state.doors[side] == core::DoorSnapshotState::Open, "Threshold crossed before fully open or closed during crossing");
								if (request.sourceSector.value == index + 1)
									require(state.occupants.size() == 1, "Capacity released during partial exit");
							}
						if (agent->getSector()->getIndex() == index)
						{
							boarded = true;
							require(state.occupants.size() == 1 && state.occupants[0] == id, "Completed board did not own capacity");
						}
						if (boarded && agent->getSector()->getIndex() == end) exited = true;
						if (exited && agent->getState() == core::Agent::State::Idle && cycled && state.cycleComplete
							&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed) break;
					}
					require(boarded && exited && cycled && agent->getState() == core::Agent::State::Idle, "Airlock journey stalled: state=" + std::to_string((int)agent->getState())
						+ " sector=" + std::to_string(agent->getSector()->getIndex()) + " boarded=" + std::to_string(boarded) + " cycled=" + std::to_string(cycled));
				}
	}

	void approachingBatch(smoke::Context const&)
	{
		for (int side : { 0, 1 })
		{
			core::World world("Spaced Airlock arrivals", 16, 6);
			uint32_t ends[] = { world.addCorridor(0, 1, 2, 5, 1), world.addCorridor(0, 1, 10, 4, 1) };
			world.addAirlock(0, 1, 7, 3, 3);
			auto marker = world.addSectorMarker(ends[1 - side], 0, 3.0f);
			auto markerId = std::static_pointer_cast<core::MarkerSectorObject>(marker.sector->getObject(marker.index))->getMarker()->getId();
			world.finishBuild();
			std::vector<core::AgentId> agents;
			for (float x : { 0.3125f, 1.5f, 2.578125f })
			{
				auto id = world.createAgent("Approaching traveller", ends[side], 0, side ? 4.0f - x : x);
				agents.push_back(id);
				require(world.moveAgentToMarker(id, markerId).accepted(), "Spaced arrival Marker command refused");
			}
			size_t peak = 0;
			bool completed = false;
			for (unsigned tick = 0; tick < 7000; ++tick)
			{
				world.advanceTick();
				auto state = world.getSimulationSnapshot().airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Spaced arrival interlock violated");
				require(state.occupants.size() + state.reservations.size() <= 3, "Spaced arrival overbooked chamber");
				peak = std::max(peak, state.occupants.size());
				completed = std::all_of(agents.begin(), agents.end(), [&](auto id) {
					auto agent = world.lookupAgent(id).entity;
					return agent->getSector()->getIndex() == ends[1 - side] && agent->getState() == core::Agent::State::Idle;
				});
				if (completed) break;
			}
			require(peak == 3, "Spaced arrivals never filled capacity: peak=" + std::to_string(peak));
			require(completed, "Spaced arrivals failed to reach Marker");
		}
	}

	void sharedOutsideCall(smoke::Context const&)
	{
		for (int side : { 0, 1 })
		for (bool cancelCaller : { false, true })
		{
			core::World world("Shared outside call", 20, 2);
			uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 8, 1), world.addRoom("Right", 0, 0, 11, 8, 1) };
			world.addAirlock(0, 0, 8, 3, 10);
			auto marker = world.addSectorMarker(ends[1 - side], 0, 4);
			world.finishBuild();
			std::vector<core::AgentId> agents;
			for (unsigned member = 0; member < 6; ++member)
			{
				auto id = world.createAgent("Waiting traveller", ends[side], 0, side ? 0.5f : 7.5f);
				agents.push_back(id);
				auto agent = world.lookupAgent(id).entity;
				agent->setPath(world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
			}
			std::set<core::InteractionRequestId> presses, attempts;
			core::AgentId cancelled;
			unsigned batches = 0;
			bool completed = false;
			int previousEntry = -1;
			for (unsigned tick = 0; tick < 12000; ++tick)
			{
				world.advanceTick();
				auto snapshot = world.getSimulationSnapshot();
				auto state = snapshot.airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Shared call interlock violated");
				require(state.occupants.size() + state.reservations.size() <= 3, "Shared call overbooked chamber");
				if (state.entrySide >= 0 && previousEntry < 0) ++batches;
				previousEntry = state.entrySide;
				for (auto const& request : snapshot.interactionRequests)
					if (request.point == state.controls[side])
					{
						attempts.insert(request.id);
						if (request.result != core::InteractionResult::Succeeded || !presses.insert(request.id).second) continue;
						// The second call waits through the post-exit cycle. Its
						// acceptance must survive the physical caller leaving the queue.
						if (cancelCaller && presses.size() == 2)
						{
							require(state.entrySide < 0 && !state.cycleComplete, "Call did not wait through cycling");
							cancelled = request.actor;
							require(world.cancelAgentMovement(cancelled).accepted(), "Accepted caller cancellation refused");
						}
					}
				completed = std::all_of(agents.begin(), agents.end(), [&](auto id) {
					return id == cancelled || world.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side];
				});
				if (completed) break;
			}
			require(completed && batches == 2, "Shared call fixture failed to complete two batches");
			require(presses.size() == batches && attempts.size() == batches, "Outside button was pressed again despite an accepted call: presses="
				+ std::to_string(presses.size()) + " attempts=" + std::to_string(attempts.size()) + " batches=" + std::to_string(batches));
			if (cancelCaller) require(cancelled && world.lookupAgent(cancelled).entity->getSector()->getIndex() == ends[side],
				"Cancelled caller did not remain outside");
		}
	}

	void boardingDeadline(smoke::Context const&)
	{
		for (int side : { 0, 1 })
		{
			core::World world("Bounded boarding", 20, 2);
			uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 8, 1), world.addRoom("Right", 0, 0, 11, 8, 1) };
			world.addAirlock(0, 0, 8, 3, 1);
			auto markers = std::array{ world.addSectorMarker(ends[0], 0, 4), world.addSectorMarker(ends[1], 0, 4) };
			world.finishBuild();
			auto add = [&](int origin) {
				auto id = world.createAgent("Deadline traveller", ends[origin], 0, origin ? 0.5f : 7.5f);
				auto agent = world.lookupAgent(id).entity;
				auto marker = markers[1 - origin];
				agent->setPath(world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				return id;
			};
			auto first = add(side);
			core::AgentId opposite, late;
			uint64_t openedAt = 0;
			auto const window = core::secondsToTicks(CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME + CORE_BULKHEAD_DOOR_STAY_OPEN_TIME, world.getFixedTimestep());
			bool closed = false, completed = false;
			for (unsigned tick = 0; tick < 9000; ++tick)
			{
				world.advanceTick();
				auto state = world.getSimulationSnapshot().airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Deadline interlock violated");
				if (!openedAt && state.entrySide == side)
				{
					openedAt = world.getSimulationTick();
					opposite = add(1 - side);
				}
				if (openedAt && !late && world.getSimulationTick() >= openedAt + window)
					late = add(side);
				if (openedAt && !closed)
				{
					for (auto occupant : state.occupants) require(occupant == first, "Post-deadline/opposing arrival joined batch");
					if (state.doors[side] == core::DoorSnapshotState::Closing)
					{
						require(world.getSimulationTick() >= openedAt + window, "Underfilled batch closed before deadline");
						require(world.getSimulationTick() <= openedAt + window + 2, "Underfilled batch extended deadline");
						closed = true;
					}
				}
				if (late && world.lookupAgent(late).entity->getSector()->getIndex() == ends[1 - side])
				{
					require(world.lookupAgent(opposite).entity->getSector()->getIndex() == ends[side], "Late arrival overtook opposing queue");
					completed = true; break;
				}
			}
			// The late Agent may only enter a subsequent batch.
			require(closed && completed, "Underfilled/deferred batch stalled");
		}
	}

	void batches(smoke::Context const&)
	{
		for (uint32_t width : { 1u, 3u })
			for (int firstSide : { 0, 1 })
			{
				core::World world("Airlock batches", width + 20, 2);
				uint32_t ends[2] = { world.addRoom("Left", 0, 0, 0, 8, 1),
					world.addCorridor(0, 0, 8 + width, 8, 1) };
				auto index = world.addAirlock(0, 0, 8, width, 1);
				auto markers = std::array{ world.addSectorMarker(ends[0], 0, 4.0f),
					world.addSectorMarker(ends[1], 0, 4.0f) };
				world.finishBuild();
				std::map<core::AgentId, int> origins;
				auto add = [&](int side)
				{
					auto id = world.createAgent("Batch traveller", ends[side], 0, side ? 0.5f : 7.5f);
					auto agent = world.lookupAgent(id).entity;
					auto marker = markers[1 - side];
					auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
					require(bool(path), "Batch route unavailable"); agent->setPath(path, true);
					origins[id] = side; return id;
				};
				for (uint32_t member = 0; member < width + 1; ++member) add(firstSide);
				for (uint32_t member = 0; member < width + 1; ++member) add(1 - firstSide);
				core::AgentId late;
				std::set<core::AgentId> admitted, exited, members;
				std::vector<core::AgentId> boardingOrder;
				std::vector<core::AgentId> expectedOrder;
				uint32_t batchCount = 0;
				bool positioned = false, sawExitContention = false, completed = false;
				std::string savedBatch;
				int previousEntry = -1;
				for (uint32_t tick = 0; tick < 16000; ++tick)
				{
					require(world.advanceTick(), "Batch tick refused");
					auto const snapshot = world.getSimulationSnapshot();
					auto state = snapshot.airlocks.at(0);
					require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Batch interlock violated");
					require(state.occupants.size() + state.reservations.size() <= width, "Reservations overbooked chamber");
					if (!state.cycleComplete) require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed, "Batch bypassed cycle");
					auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
					auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(), [&](auto const& r) { return r.id == chamber->getTraversalResourceId(); });
					require(resource != snapshot.traversalResources.end(), "Missing batch packing observation");
					if (state.entrySide >= 0 && previousEntry < 0 && !state.reservations.empty())
					{
						++batchCount; members.clear(); boardingOrder.clear(); expectedOrder.clear();
						std::vector<core::TraversalRequestSnapshot> waiting;
						for (auto const& request : snapshot.traversalRequests)
							if (request.resource == resource->id && request.destinationSector.value == index + 1
								&& request.state == core::TraversalRequestState::Pending) waiting.push_back(request);
						std::sort(waiting.begin(), waiting.end(), [](auto const& a, auto const& b) { return a.queueTicket < b.queueTicket; });
						require(!waiting.empty() && origins.at(waiting.front().owner) == state.entrySide, "Opposing service ignored oldest ticket");
						for (auto const& request : waiting)
							if (origins.at(request.owner) == state.entrySide && expectedOrder.size() < width)
							{
								auto slot = state.entrySide == 0 ? width - 1 - (uint32_t)expectedOrder.size() : (uint32_t)expectedOrder.size();
								require(resource->capacityPositions.at(slot).admissionReservation == request.id, "Batch cutoff/order or farthest-first slot incorrect");
								expectedOrder.push_back(request.owner); members.insert(request.owner);
							}
						if (batchCount == 1)
						{
							require(state.reservations.size() == width, "Initial same-direction batch not filled: width=" + std::to_string(width) + " side=" + std::to_string(firstSide) + " entry=" + std::to_string(state.entrySide) + " reservations=" + std::to_string(state.reservations.size()) + " waiting=" + std::to_string(waiting.size()));
							// Arrives after the fixed reservation cutoff, while entry is opening.
							late = add(state.entrySide);
						}
					}
					previousEntry = state.entrySide;
					// Later underfilled batches may now gain members during their
					// boarding window, not only on the tick that entry opens.
					std::vector<core::TraversalRequestSnapshot> additions;
					for (auto const& request : snapshot.traversalRequests)
						if (!members.contains(request.owner) && std::find(state.reservations.begin(), state.reservations.end(), request.id) != state.reservations.end())
							additions.push_back(request);
					std::sort(additions.begin(), additions.end(), [](auto const& a, auto const& b) { return a.queueTicket < b.queueTicket; });
					for (auto const& request : additions)
					{
						require(batchCount > 1 && expectedOrder.size() < width && origins.at(request.owner) == state.entrySide,
							"Full/opposing batch accepted a later member");
						expectedOrder.push_back(request.owner); members.insert(request.owner);
					}
					for (auto id : state.occupants)
					{
						require(members.contains(id) && origins.at(id) == state.entrySide, "Late/opposing waiter joined fixed batch");
						if (admitted.insert(id).second) boardingOrder.push_back(id);
					}
					if (boardingOrder.size() == expectedOrder.size() && !boardingOrder.empty())
						require(boardingOrder == expectedOrder, "Within-side boarding ticket order changed");
					if (state.occupants.size() == width && state.doors[1 - state.entrySide] == core::DoorSnapshotState::Closed)
					{
						bool packed = true;
						for (auto const& position : resource->capacityPositions)
							if (position.occupant)
							{
								auto agent = world.lookupAgent(position.occupant).entity;
								packed = packed && agent->getLocalPosition().distanceTo(position.position) < 0.001f;
							}
						positioned = positioned || packed;
					}
					if (batchCount == 1 && state.occupants.size() == width && savedBatch.empty())
					{
						auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
						work.markSerializedUnmodified = false; world.serialize(*writer, work); writer->serialize();
						savedBatch = writer->getSerializedString();
					}
					for (auto const& request : snapshot.traversalRequests)
						if (request.resource == resource->id && request.state == core::TraversalRequestState::Granted)
						{
							if (request.sourceSector.value == index + 1)
							{
								require(std::find(state.occupants.begin(), state.occupants.end(), request.owner) != state.occupants.end(), "Slot released before completed exit");
								if (exited.size() < origins.size() - width) sawExitContention = true;
							}
							else require(members.contains(request.owner) && origins.at(request.owner) == state.entrySide, "Waiter entered open exit");
						}
					for (auto const& [id, side] : origins)
						if (world.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side]) exited.insert(id);
					if (exited.size() == origins.size() && state.entrySide < 0 && state.cycleComplete)
					{ completed = true; break; }
				}
				require(completed && admitted.contains(late), "Contending/late batch journeys stalled");
				require(positioned && sawExitContention, "Batch standing positions or exit contention not exercised: width=" + std::to_string(width) + " positioned=" + std::to_string(positioned) + " contention=" + std::to_string(sawExitContention));
				require(world.getSimulationSnapshot().interactionPoints.size() == 2, "Batch required an internal control");
				// Authored reset discards all batch state and repeats all journeys.
				world.resetSimulation(); world.wakeAllAgents();
				auto reset = world.getSimulationSnapshot().airlocks.at(0);
				require(reset.entrySide < 0 && reset.reservations.empty() && reset.occupants.empty() && reset.cycleComplete, "Reset retained multi-Agent batch");
				for (uint32_t tick = 0; tick < 16000; ++tick)
				{
					world.advanceTick(); bool done = true;
					for (auto const& [id, side] : origins) done = done && world.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side];
					if (done) break;
				}
				for (auto const& [id, side] : origins) require(world.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side], "Reset batch did not repeat journey");
				auto reader = core::YamlSerializer::fromString(savedBatch); reader->deserialize();
				core::World loaded("Loaded batch", 1, 1); core::SerializationWorkData work;
				require(loaded.deserialize(*reader, work), "Occupied batch save refused");
				auto initial = loaded.getSimulationSnapshot().airlocks.at(0);
				require(initial.capacity == width && initial.entrySide < 0 && initial.occupants.empty()
					&& initial.reservations.empty() && initial.cycleComplete, "Load retained transient batch or lost capacity");
				loaded.wakeAllAgents();
				for (uint32_t tick = 0; tick < 16000; ++tick)
				{
					loaded.advanceTick(); bool done = true;
					for (auto const& [id, side] : origins) done = done && loaded.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side];
					if (done) break;
				}
				for (auto const& [id, side] : origins) require(loaded.lookupAgent(id).entity->getSector()->getIndex() == ends[1 - side], "Loaded batch did not complete journey");
			}
	}

	void interruptedJourneys(smoke::Context const&)
	{
		for (int side : { 0, 1 })
			for (bool allInactive : { false, true })
			{
				core::World world("Interrupted batch", 20, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 8, 1), world.addRoom("Right", 0, 0, 10, 8, 1) };
				world.addAirlock(0, 0, 8, 2, 1);
				auto markers = std::array{ world.addSectorMarker(ends[0], 0, 4), world.addSectorMarker(ends[1], 0, 4) };
				world.finishBuild();
				auto add = [&](int origin)
				{
					auto id = world.createAgent("Passenger", ends[origin], 0, origin ? 0.5f : 7.5f);
					auto agent = world.lookupAgent(id).entity;
					auto marker = markers[1 - origin];
					agent->setPath(world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
					return id;
				};
				auto first = add(side), second = add(side);
				bool interrupted = false, partial = false, resumed = false, complete = false;
				core::AgentId operatorId = first, waiter;
				core::Vector2 frozen;
				for (uint32_t tick = 0; tick < 10000; ++tick)
				{
					require(world.advanceTick(), "Interrupted tick refused");
					auto snapshot = world.getSimulationSnapshot(); auto state = snapshot.airlocks.at(0);
					require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Interrupted interlock violated");
					require(state.occupants.size() + state.reservations.size() <= 2, "Interrupted batch overbooked");
					if (!interrupted && state.occupants.size() == 2 && !state.cycleComplete)
					{
						world.pauseSimulation();
						require(!world.clearAgentPath(operatorId), "Paused path clear discarded committed occupant");
						frozen = world.lookupAgent(operatorId).entity->getGlobalPosition();
						require(world.setAgentActive(operatorId, false), "Operator deactivation refused");
						if (allInactive) require(world.setAgentActive(operatorId == first ? second : first, false), "Co-passenger deactivation refused");
						// Replacement intent points back to entry, but only after committed exit.
						auto active = operatorId == first ? second : first;
						if (!allInactive) require(world.moveAgentToMarker(active, std::static_pointer_cast<core::MarkerSectorObject>(markers[side].sector->getObject(markers[side].index))->getMarker()->getId()).accepted(), "Inside destination change refused");
						waiter = add(1 - side);
						require(world.resumeSimulation(), "Interrupted resume refused"); interrupted = true;
					}
					if (interrupted && !resumed)
					{
						require(world.lookupAgent(operatorId).entity->getGlobalPosition().distanceTo(frozen) < 0.001f, "Inactive occupant moved");
						require(!world.lookupAgent(operatorId).entity->isActive(), "Occupant auto-reactivated");
						require(std::find(state.occupants.begin(), state.occupants.end(), operatorId) != state.occupants.end(), "Inactive occupant lost capacity");
						require(state.entrySide == side && state.reservations.empty(), "New batch mixed with inactive occupant");
						if ((allInactive && state.doors[1 - side] == core::DoorSnapshotState::Open)
							|| (!allInactive && state.occupants.size() == 1))
						{
							partial = !allInactive;
							if (partial) require(world.lookupAgent(operatorId == first ? second : first).entity->getSector()->getIndex() == ends[1 - side], "Changed destination reversed occupant");
							world.pauseSimulation(); require(world.setAgentActive(operatorId, true), "Reactivation refused");
							if (allInactive)
							{
								require(world.setAgentActive(operatorId == first ? second : first, true), "Co-passenger reactivation refused");
								require(world.cancelAgentMovement(operatorId).accepted(), "Occupied cancellation refused");
							}
							require(world.resumeSimulation(), "Reactivation resume refused"); resumed = true;
						}
					}
					if (resumed && world.lookupAgent(operatorId).entity->getSector()->getIndex() == ends[1 - side]
						&& world.lookupAgent(waiter).entity->getSector()->getIndex() == ends[side]
						&& (allInactive || world.lookupAgent(operatorId == first ? second : first).entity->getSector()->getIndex() == ends[side]))
					{ complete = true; break; }
				}
				require(interrupted && resumed && complete && (allInactive || partial), "Interrupted batch failed to recover");
				world.resetSimulation();
				auto state = world.getSimulationSnapshot().airlocks.at(0);
				require(state.capacity == 2 && std::abs(state.cycleSeconds - 1) < 0.001f && state.entrySide == -1 && state.occupants.empty()
					&& state.reservations.empty() && state.crossings.empty() && state.cycleComplete, "Interrupted reset corrupted configuration or retained coordination");
			}
	}

	void ordinaryOpenPassage(smoke::Context const&)
	{
		core::World world("Ordinary open Bulkhead", 12, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 5, 1);
		auto right = world.addRoom("Right", 0, 0, 5, 6, 1);
		auto made = world.addSectorBulkheadDoor(0, 0, 5, CORE_SIDE_LEFT);
		auto door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
		auto leftMarker = world.addSectorMarker(left, 0, 1.0f);
		auto rightMarker = world.addSectorMarker(right, 0, 4.0f); world.finishBuild();
		door->requestOpen(); door->update(door->getOpenCloseTime());
		require(door->isOpen(), "Ordinary Bulkhead fixture did not open");
		auto leftAgent = world.lookupAgent(world.createAgent("Left crossing", left, 0, 4.5f)).entity;
		auto rightAgent = world.lookupAgent(world.createAgent("Right crossing", right, 0, 0.5f)).entity;
		leftAgent->setPath(world.getGraph()->calculatePath(leftAgent, world.getGraph()->getVertexForObject(rightMarker.sector->getObject(rightMarker.index))), true);
		rightAgent->setPath(world.getGraph()->calculatePath(rightAgent, world.getGraph()->getVertexForObject(leftMarker.sector->getObject(leftMarker.index))), true);
		bool concurrent = false;
		for (uint32_t tick = 0; tick < 1200; ++tick)
		{
			world.advanceTick(); concurrent = concurrent || world.getSimulationSnapshot().traversalPermits.size() >= 2;
		}
		require(concurrent && leftAgent->getSector()->getIndex() == right && rightAgent->getSector()->getIndex() == left,
			"Ordinary fully open Bulkhead lost unrestricted bidirectional passage");
	}

	void lostReservation(smoke::Context const&)
	{
		core::World world("Lost batch reservation", 18, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 8, 1);
		auto right = world.addRoom("Right", 0, 0, 10, 8, 1);
		auto index = world.addAirlock(0, 0, 8, 2, 1);
		auto marker = world.addSectorMarker(right, 0, 4.0f); world.finishBuild();
		for (uint32_t member = 0; member < 3; ++member)
		{
			auto id = world.createAgent("Reserved traveller", left, 0, 7.5f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
			require(bool(path), "Lost reservation route unavailable"); agent->setPath(path, true);
		}
		core::AgentId cancelled, retained, deferred;
		bool closed = false, deferredArrived = false;
		for (uint32_t tick = 0; tick < 9000; ++tick)
		{
			world.advanceTick(); auto snapshot = world.getSimulationSnapshot(); auto state = snapshot.airlocks.at(0);
			require(state.occupants.size() + state.reservations.size() <= 2, "Lost reservation overbooked capacity");
			if (!cancelled && state.reservations.size() == 2)
			{
				std::vector<core::TraversalRequestSnapshot> entrants;
				for (auto const& request : snapshot.traversalRequests)
					if (request.destinationSector.value == index + 1) entrants.push_back(request);
				std::sort(entrants.begin(), entrants.end(), [](auto const& a, auto const& b) { return a.queueTicket < b.queueTicket; });
				require(entrants.size() == 3, "Lost reservation fixture did not queue three entrants");
				cancelled = entrants[0].owner; retained = entrants[1].owner; deferred = entrants[2].owner;
				require(world.cancelAgentMovement(cancelled).status == core::MovementCommandStatus::Accepted, "Pre-entry cancellation refused");
			}
			if (cancelled && !closed)
			{
				for (auto occupant : state.occupants) require(occupant == retained, "Cancelled slot refilled from waiting queue");
				if (!state.cycleComplete && !state.occupants.empty()) closed = true;
			}
			if (deferred && world.lookupAgent(deferred).entity->getSector()->getIndex() == right)
			{ deferredArrived = true; break; }
		}
		require(cancelled && closed && deferredArrived && world.lookupAgent(retained).entity->getSector()->getIndex() == right,
			"Lost reservation prevented entry closure or later batch");
		require(world.lookupAgent(cancelled).entity->getSector()->getIndex() == left, "Cancelled entrant consumed occupancy");
	}

	void abandonedBoarding(smoke::Context const&)
	{
		for (int mode : { 0, 1, 2, 3 }) // one/all inactive, permit expiry, all cancelled
		{
			core::World world("Abandoned boarding", 20, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 8, 1);
			auto right = world.addRoom("Right", 0, 0, 10, 8, 1);
			auto index = world.addAirlock(0, 0, 8, 2, 1);
			auto marker = world.addSectorMarker(right, 0, 4); world.finishBuild();
			std::vector<core::AgentId> ids;
			for (int i = 0; i < 3; ++i)
			{
				auto id = world.createAgent("Boarder", left, 0, 7.5f); ids.push_back(id);
				auto agent = world.lookupAgent(id).entity;
				agent->setPath(world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
			}
			bool selected = false, lost = false, cycled = false, completed = false;
			core::AgentId abandoned;
			for (uint32_t tick = 0; tick < 9000; ++tick)
			{
				// Existing Agent update seam: undo one walking step to reproduce a
				// selected threshold boarder that makes no physical progress.
				if (mode == 2 && selected && !lost)
					for (auto id : ids)
					{
						auto agent = world.lookupAgent(id).entity;
						if (agent->getState() == core::Agent::State::TraversingEdge && agent->getSector()->getIndex() == left)
							agent->update(-world.getFixedTimestep());
					}
				world.advanceTick(); auto snapshot = world.getSimulationSnapshot(); auto state = snapshot.airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Abandoned boarding interlock violated");
				if (!selected && state.reservations.size() == 2)
				{
					selected = true;
					for (auto const& request : snapshot.traversalRequests)
						if (request.id == state.reservations.front()) abandoned = request.owner;
					if (mode < 2)
					{
						world.pauseSimulation();
						require(world.setAgentActive(abandoned, false), "Selected boarder deactivation refused");
						if (mode == 1)
							for (auto const& request : snapshot.traversalRequests)
								if (std::find(state.reservations.begin(), state.reservations.end(), request.id) != state.reservations.end())
									require(world.setAgentActive(request.owner, false), "Empty batch deactivation refused");
						require(world.resumeSimulation(), "Abandoned boarding resume refused");
						lost = true;
					}
					else if (mode == 3)
					{
						for (auto const& request : snapshot.traversalRequests)
							if (std::find(state.reservations.begin(), state.reservations.end(), request.id) != state.reservations.end())
								require(world.cancelAgentMovement(request.owner).accepted(), "Selected boarder cancellation refused");
						lost = true;
					}
					else
					{
						auto policy = world.getTraversalWaitingPolicy(); policy.permitProgressTimeoutTicks = 1;
						world.setTraversalWaitingPolicy(policy);
					}
				}
				if (selected && mode == 2 && !lost)
					for (auto const& event : world.consumeSimulationEvents())
						if (auto const& request = event.traversalRequest; request.destinationSector.value == index + 1 && request.failureReason == core::TraversalFailureReason::PermitExpired)
						{
							lost = true;
							require(std::find(state.reservations.begin(), state.reservations.end(), request.id) == state.reservations.end(), "Expired admission retained reservation");
							auto policy = world.getTraversalWaitingPolicy(); policy.permitProgressTimeoutTicks = 120;
							world.setTraversalWaitingPolicy(policy);
						}
				if (lost && !cycled)
				{
					for (auto occupant : state.occupants) require(occupant != ids.back(), "Abandoned slot refilled from later waiter");
					if (!state.cycleComplete)
					{
						cycled = true;
						if (mode == 1 || mode == 3) require(state.occupants.empty(), "Abandoned empty batch admitted occupant");
					}
				}
				if (cycled && world.lookupAgent(ids.back()).entity->getSector()->getIndex() == right) { completed = true; break; }
			}
			require(selected && lost && cycled && completed, "Abandoned or expired batch stalled: mode=" + std::to_string(mode) + " selected=" + std::to_string(selected) + " lost=" + std::to_string(lost) + " cycled=" + std::to_string(cycled));
			if (mode < 2) require(world.lookupAgent(abandoned).entity->getSector()->getIndex() == left, "Inactive pre-entry member boarded");
		}
	}

	struct Scene
	{
		core::World world{ "Airlock lifecycle", 10, 2 };
		uint32_t left = world.addRoom("Left", 0, 0, 0, 3, 1);
		uint32_t right = world.addRoom("Right", 0, 0, 5, 3, 1);
		uint32_t index = world.addAirlock(0, 0, 3, 2);
		core::World::CreateObjectResult marker = world.addSectorMarker(right, 0, 1.5f);
		core::AgentId id;
		core::Agent* agent;
		Scene(bool corridor = false)
			: right(corridor ? world.addCorridor(0, 0, 5, 3, 1) : world.addRoom("Right", 0, 0, 5, 3, 1))
		{
			world.finishBuild(); id = world.createAgent("Traveller", left, 0, 1.5f);
			agent = world.lookupAgent(id).entity;
		}
		std::shared_ptr<const core::AirlockTransit> chamber() const
		{ return std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index)); }
		void route()
		{
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
			require(bool(path), "Lifecycle route unavailable"); agent->setPath(path, true);
		}
		core::AirlockSnapshot step()
		{
			require(world.advanceTick(), "Lifecycle tick refused");
			auto state = world.getSimulationSnapshot().airlocks.at(0);
			require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Lifecycle interlock violated");
			if (!state.cycleComplete) require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed, "Lifecycle opening during cycle");
			return state;
		}
	};

	void permissionsAndMobility(smoke::Context const&)
	{
		for (bool corridor : { false, true })
		{
			Scene scene(corridor); scene.world.pauseSimulation();
			auto leftKey = scene.world.addAccessPermission("Left button");
			auto rightKey = scene.world.addAccessPermission("Right button");
			auto locationKey = scene.world.addAccessPermission("Destination");
			require(scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(0), { leftKey })
				&& scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(1), { rightKey }), "Outside protection refused");
			auto target = scene.world.getGraph()->getVertexForObject(scene.marker.sector->getObject(scene.marker.index));
			auto choose = [&] { return scene.world.getGraph()->calculatePath(scene.agent, target); };
			require(!choose(), "Unauthorized outside operation planned");
			require(scene.world.setAgentIndividualPermissionAdherence(scene.id, false), "Adherence edit refused");
			require(!choose(), "Non-adherence authorized closed outside operation");
			require(scene.world.grantAgentAccessPermission(scene.id, leftKey), "Grant refused");
			require(bool(choose()), "Opposite outside requirement incorrectly restricted entry");
			require(scene.world.setLocationPermissionRequirement(scene.right, { locationKey }), "Location protection refused");
			require(!choose(), "Non-adherence bypassed destination Location");
			require(scene.world.grantAgentAccessPermission(scene.id, locationKey), "Location grant refused");
			auto permitted = choose(); require(bool(permitted), "Authorized journey unavailable");
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
			require(scene.world.setAgentIndividualMobilityProfile(scene.id, mobility), "Mobility edit refused");
			require(!choose(), "Forbidden Buttons route selected");
			// A stale Path cannot bypass the runtime eligibility gate.
			scene.agent->setPath(permitted, true); scene.world.resumeSimulation();
			for (uint32_t tick = 0; tick < 400; ++tick)
			{
				auto state = scene.step();
				require(state.occupants.empty() && state.reservations.empty(), "Forbidden button Agent admitted");
			}
			scene.world.pauseSimulation();
			mobility.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
			require(scene.world.setAgentIndividualMobilityProfile(scene.id, mobility), "Mobility edit refused");
			require(scene.world.setAgentIndividualPermissionAdherence(scene.id, true), "Adherence edit refused");
			require(bool(choose()), "Last-resort Buttons failed fallback");
			scene.route(); scene.world.resumeSimulation();
			bool arrived = false;
			for (uint32_t tick = 0; tick < 2400; ++tick)
			{
				scene.step();
				if (scene.agent->getSector()->getIndex() == scene.right) { arrived = true; break; }
			}
			require(arrived, "Last-resort runtime journey stalled: state=" + std::to_string((int)scene.agent->getState())
				+ " node=" + std::to_string(scene.agent->getPathTargetNodeIndex()) + " sector=" + std::to_string(scene.agent->getSector()->getIndex()) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " active=" + std::to_string(scene.agent->isActive())
				+ " path=" + std::to_string(bool(scene.agent->getPath())));
			// The other outside button requires its own grant, independently.
			scene.world.pauseSimulation();
			auto originMarker = scene.world.addSectorMarker(scene.left, 0, 1.5f);
			scene.world.finishBuild(); scene.world.pauseSimulation();
			auto origin = scene.world.getGraph()->getVertexForObject(originMarker.sector->getObject(originMarker.index));
			require(!scene.world.getGraph()->calculatePath(scene.agent, origin), "Left grant authorized right outside operation");
			require(scene.world.grantAgentAccessPermission(scene.id, rightKey)
				&& scene.world.getGraph()->calculatePath(scene.agent, origin), "Right outside grant did not admit return route");
		}
	}

	void staleAuthorization(smoke::Context const&)
	{
		for (bool location : { false, true })
			for (bool corridor : { false, true })
			{
				Scene scene(corridor);
				auto destination = scene.world.getGraph()->getVertexForObject(scene.marker.sector->getObject(scene.marker.index));
				auto stale = scene.world.getGraph()->calculatePath(scene.agent, destination);
				require(bool(stale), "Stale authorization fixture unreachable");
				scene.world.pauseSimulation();
				auto key = scene.world.addAccessPermission("New requirement");
				require(location ? scene.world.setLocationPermissionRequirement(scene.right, { key })
					: scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(0), { key }), "Requirement refused");
				scene.agent->setPath(stale, true); scene.world.resumeSimulation();
				for (uint32_t tick = 0; tick < 1200; ++tick)
				{
					auto state = scene.step();
					require(state.occupants.empty() && scene.agent->getSector()->getIndex() == scene.left,
						"Stale Path bypassed current admission authorization");
				}
			}
	}

	void alternativeCosts(smoke::Context const&)
	{
		bool reversed = false;
		for (uint32_t detour = 10; detour < 36 && !reversed; detour += 2)
		{
			core::World world("Airlock alternatives", 40, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
			auto right = world.addRoom("Right", 0, 0, 5, 35, 1);
			auto chamberIndex = world.addAirlock(0, 0, 3, 2, 1);
			world.addRoom("Detour", 1, 0, 0, 40, 1);
			world.addSectorDoor(0, 0, 0, {}); world.addSectorDoor(0, 0, detour, {});
			auto marker = world.addSectorMarker(right, 0, 1.5f);
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Chooser", left, 0, 1.5f);
			auto actor = world.lookupAgent(id).entity;
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(chamberIndex));
			auto target = world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index));
			auto chooseAirlock = [&] {
				auto path = world.getGraph()->calculatePath(actor, target);
				require(bool(path), "Alternative route unavailable");
				return std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node) {
					return node.edge && node.edge->getTraversalResourceId() == chamber->getTraversalResourceId();
				});
			};
			require(world.setAgentIndividualWaitingAversion(id, 0.5f), "Waiting preference refused");
			bool low = chooseAirlock();
			require(world.setAgentIndividualWaitingAversion(id, 3), "Waiting preference refused");
			bool high = chooseAirlock();
			reversed = low && !high;
			if (!low) continue;
			core::MobilityProfile mobility;
			mobility.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
			require(world.setAgentIndividualMobilityProfile(id, mobility)
				&& world.setAgentIndividualWaitingAversion(id, 0.5f), "Last-resort alternative fixture failed");
			require(!chooseAirlock(), "Last-resort Airlock chosen despite ordinary alternative");
			mobility.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
			require(world.setAgentIndividualMobilityProfile(id, mobility) && !chooseAirlock(), "Forbidden Buttons displaced ordinary route");
		}
		require(reversed, "Waiting aversion did not reverse Airlock versus walking choice");
	}

	void committedAuthorizationChanges(smoke::Context const&)
	{
		for (bool corridor : { false, true })
			for (bool tighten : { false, true })
			{
				Scene scene(corridor); scene.world.pauseSimulation();
				auto key = scene.world.addAccessPermission("Journey permission");
				if (!tighten)
				{
					require(scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(0), { key })
						&& scene.world.setLocationPermissionRequirement(scene.right, { key })
						&& scene.world.grantAgentAccessPermission(scene.id, key), "Authorization fixture failed");
				}
				scene.route(); scene.world.resumeSimulation();
				bool boarded = false;
				for (uint32_t tick = 0; tick < 1600; ++tick)
					if (!scene.step().occupants.empty()) { boarded = true; break; }
				require(boarded, "Permission fixture never boarded");
				scene.world.pauseSimulation();
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(scene.id, mobility), "Committed occupant mobility edit refused");
				scene.world.resumeSimulation();
				if (tighten)
				{
					scene.world.pauseSimulation();
					require(scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(0), { key })
						&& scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(1), { key })
						&& scene.world.setLocationPermissionRequirement(scene.right, { key }), "Tightening refused");
					scene.world.resumeSimulation();
				}
				else require(scene.world.setAgentRuntimeAccessPermissionGrant(scene.id, key, false), "Runtime permission loss refused");
				bool exited = false;
				for (uint32_t tick = 0; tick < 2400; ++tick)
				{
					auto state = scene.step();
					if (scene.agent->getSector()->getIndex() == scene.right)
					{ require(state.occupants.empty(), "Exit retained capacity"); exited = true; break; }
				}
				require(exited, "Authorization change trapped/reversed committed occupant: tighten=" + std::to_string(tighten)
					+ " state=" + std::to_string((int)scene.agent->getState()) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " sector=" + std::to_string(scene.agent->getSector()->getIndex())
					+ " doors=" + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[0])
					+ "," + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[1])
					+ " path=" + std::to_string(bool(scene.agent->getPath())));
			}
	}

	void localAdherence(smoke::Context const&)
	{
		Scene scene; scene.route();
		// The public route seam sees a usable entrance only from its approach.
		for (uint32_t tick = 0; tick < 1200; ++tick)
		{
			auto state = scene.step();
			if (state.doors[0] != core::DoorSnapshotState::Open) continue;
			scene.world.pauseSimulation();
			auto key = scene.world.addAccessPermission("Outside operator");
			require(scene.world.setInteractionPointPermissionRequirement(scene.chamber()->getControl(0), { key }), "Local requirement refused");
			auto id = scene.world.createAgent("Observer", scene.left, 0, 1.5f);
			auto observer = scene.world.lookupAgent(id).entity;
			auto destination = scene.world.getGraph()->getVertexForObject(scene.marker.sector->getObject(scene.marker.index));
			require(!scene.world.getGraph()->calculatePath(observer, destination), "Adhering observer used protected open entrance");
			require(scene.world.setAgentIndividualPermissionAdherence(id, false), "Observer adherence edit refused");
			auto opportunistic = scene.world.getGraph()->calculatePath(observer, destination);
			require(bool(opportunistic), "Non-adhering observer ignored locally open entrance");
			core::RouteDecisionContext remote{ observer, {}, {}, nullptr, observer->getWalkSpeed(), &scene.world };
			for (auto const& edge : scene.world.getGraph()->getEdges())
				if (edge->getTraversalResourceId() == scene.chamber()->getTraversalResourceId())
					for (uint32_t side = 0; side < 2; ++side)
					{
						auto target = edge->getVertex(side);
						if (target->getSector().get() == scene.chamber().get()
							&& edge->getOtherVertex(target)->getSector()->getIndex() == scene.left)
							require(!core::RouteTraversalInputs::capture(*edge, target, remote).evaluate(remote).feasible,
								"Remote live entrance authorized route");
					}
			observer->setPath(opportunistic, true); scene.world.resumeSimulation();
			bool admitted = false, arrived = false;
			for (uint32_t wait = 0; wait < 3000; ++wait)
			{
				auto next = scene.step();
				admitted = admitted || std::find(next.occupants.begin(), next.occupants.end(), id) != next.occupants.end();
				if (observer->getSector()->getIndex() == scene.right) { arrived = true; break; }
			}
			require(admitted && arrived, "Eligible local observer did not use spare boarding-window capacity");
			return;
		}
		throw std::runtime_error("Local adherence fixture did not open entrance");
	}

	void exitSideReadmission(smoke::Context const&)
	{
		Scene scene; scene.route();
		bool arrived = false;
		for (uint32_t tick = 0; tick < 2400; ++tick)
		{
			scene.step();
			if (scene.agent->getSector()->getIndex() == scene.right && scene.agent->getState() == core::Agent::State::Idle)
			{ arrived = true; break; }
		}
		require(arrived, "Readmission fixture did not complete exit");
		bool cycled = !scene.chamber()->isCycleComplete();
		require(bool(scene.world.requestInteraction(scene.chamber()->getControl(1), scene.id)), "Previous exit-side call refused");
		bool reopened = false;
		for (uint32_t tick = 0; tick < 1800; ++tick)
		{
			auto state = scene.step();
			if (!state.cycleComplete) cycled = true;
			if (state.doors[1] == core::DoorSnapshotState::Opening)
			{
				require(cycled && state.cycleComplete, "Previous exit side reopened before post-exit cycle");
				reopened = true; break;
			}
		}
		require(cycled && reopened, "Post-exit call lost or cycle never completed");
	}

	void basicEstimates(smoke::Context const&)
	{
		Scene scene;
		core::RouteDecisionContext unseen{ scene.agent, {}, {}, nullptr, scene.agent->getWalkSpeed(), &scene.world };
		std::vector<std::pair<std::shared_ptr<const core::Edge>, std::shared_ptr<const core::Vertex>>> arcs;
		std::vector<core::DirectedTraversalFacts> baseline;
		for (auto const& edge : scene.world.getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.chamber()->getTraversalResourceId())
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto target = edge->getVertex(side);
					auto facts = core::RouteTraversalInputs::capture(*edge, target, unseen).evaluate(unseen);
					auto direct = edge->getDirectedTraversalFacts(target, unseen);
					require(facts.feasible && facts.objectiveDurationSeconds == direct.objectiveDurationSeconds
						&& facts.components.interactionUnits == unseen.policy.thresholdInteraction
							+ (target->getSector().get() == scene.chamber().get() ? unseen.policy.remoteDoorInteraction : 0),
						"Basic estimate omitted required interaction");
					float expectedWait = target->getSector().get() == scene.chamber().get()
						? CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME : 3 + 2 * CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
					require(std::abs(facts.components.expectedWaitSeconds - expectedWait) < 0.001f
						&& facts.components.motionSeconds >= edge->getLength() / scene.agent->getWalkSpeed(), "Basic estimate omitted cycle/movement");
					if (target->getSector().get() != scene.chamber().get())
						require(std::abs(facts.components.motionSeconds - edge->getLength() / scene.agent->getWalkSpeed()) < 0.001f,
							"Automatic exit estimate retained internal-button walking or interaction time");
					core::EffectiveRoutingProfile averse;
					averse.waitingAversion = 3; averse.interactionAversion = 3;
					auto neutralCost = unseen.policy.evaluate(facts, unseen.profile);
					auto averseCost = unseen.policy.evaluate(facts, averse);
					require(neutralCost && averseCost && averseCost->perceivedCost > neutralCost->perceivedCost
						&& averseCost->objectiveDurationSeconds == neutralCost->objectiveDurationSeconds,
						"Airlock preferences changed objective timing instead of perceived cost");
					arcs.emplace_back(edge, target); baseline.push_back(facts);
				}
		require(arcs.size() == 4, "Missing directed Airlock arcs");
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < 600; ++tick)
		{
			scene.step();
			for (uint32_t i = 0; i < arcs.size(); ++i)
			{
				auto facts = core::RouteTraversalInputs::capture(*arcs[i].first, arcs[i].second, unseen).evaluate(unseen);
				require(facts.objectiveDurationSeconds == baseline[i].objectiveDurationSeconds
					&& facts.components.expectedWaitSeconds == baseline[i].components.expectedWaitSeconds,
					"Unobserved live Airlock state leaked into route estimate");
			}
		}
	}

	void localQueueObservations(smoke::Context const&)
	{
		Scene scene; scene.world.pauseSimulation();
		auto marker = scene.world.addSectorMarker(scene.left, 0, 1.5f); scene.world.finishBuild();
		auto rightId = scene.world.createAgent("Remote queue", scene.right, 0, 0.5f);
		auto remoteAgent = scene.world.lookupAgent(rightId).entity;
		auto target = scene.world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index));
		std::shared_ptr<const core::Edge> entrance;
		std::shared_ptr<const core::Vertex> inside;
		for (auto const& edge : scene.world.getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.chamber()->getTraversalResourceId())
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto vertex = edge->getVertex(side);
					if (vertex->getSector().get() == scene.chamber().get()
						&& edge->getOtherVertex(vertex)->getSector()->getIndex() == scene.left)
					{ entrance = edge; inside = vertex; }
				}
		require(bool(entrance), "Queue estimate fixture lacks entrance");
		core::RouteDecisionContext local{ scene.agent, {}, {}, scene.world.getSector(scene.left).get(), scene.agent->getWalkSpeed(), &scene.world };
		core::RouteDecisionContext remote{ scene.agent, {}, {}, nullptr, scene.agent->getWalkSpeed(), &scene.world };
		auto capture = [&](auto const& context) { return core::RouteTraversalInputs::capture(*entrance, inside, context).evaluate(context); };
		auto baseline = capture(local), unseen = capture(remote);
		auto path = scene.world.getGraph()->calculatePath(remoteAgent, target);
		require(bool(path), "Remote queue route unavailable"); remoteAgent->setPath(path, true); scene.world.resumeSimulation();
		bool queued = false;
		for (uint32_t tick = 0; tick < 400; ++tick)
		{
			scene.step();
			auto snapshot = scene.world.getSimulationSnapshot();
			queued = std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(), [&](auto const& request) {
				return request.owner == rightId && request.resource == scene.chamber()->getTraversalResourceId();
			});
			if (queued) break;
		}
		require(queued, "Opposing queue fixture never requested entry");
		auto observed = capture(local);
		require(observed.objectiveDurationSeconds == baseline.objectiveDurationSeconds
			&& observed.components.crowdingUnits == baseline.components.crowdingUnits
			&& capture(remote).objectiveDurationSeconds == unseen.objectiveDurationSeconds,
			"Opposing unobservable queue leaked into left approach estimate");
	}

	void emptyCalls(smoke::Context const&)
	{
		Scene scene;
		auto other = scene.world.createAgent("Right caller", scene.right, 0, 1.5f);
		for (int side : { 0, 0, 1, 1, 0 })
		{
			auto request = scene.world.requestInteraction(scene.chamber()->getControl(side), side ? other : scene.id);
			require(bool(request), "Empty outside button refused");
			bool opened = false, cycling = false;
			uint64_t firstOpen = 0, closedAt = 0;
			for (uint32_t tick = 0; tick < 1800; ++tick)
			{
				auto state = scene.step();
				require(state.occupants.empty() && state.reservations.empty(), "Empty call occupied chamber");
				if (state.doors[side] == core::DoorSnapshotState::Open)
				{ opened = true; if (!firstOpen) firstOpen = scene.world.getSimulationTick(); }
				if (opened && !state.cycleComplete)
				{
					cycling = true;
					if (!closedAt) closedAt = scene.world.getSimulationTick();
					if (std::abs(state.remainingCycleSeconds - 3) < 0.001f)
						require(closedAt - firstOpen >= core::secondsToTicks(CORE_BULKHEAD_DOOR_STAY_OPEN_TIME + CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME, scene.world.getFixedTimestep()), "Empty door did not honour Bulkhead timeout");
				}
				if (cycling && state.cycleComplete) break;
			}
			require(opened && cycling, "Empty operation stalled without internal press");
		}
		// Request the previous entrance while closed-door cycling, not afterwards.
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < 1800 && scene.step().cycleComplete; ++tick) {}
		auto remaining = scene.chamber()->getRemainingCycleSeconds();
		require(remaining > 0, "Missing empty cycle");
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < (uint32_t)std::lround(remaining / scene.world.getFixedTimestep()) - 1; ++tick)
			require(!scene.step().cycleComplete, "Same-side request bypassed cycle");
		bool reopened = false;
		for (uint32_t tick = 0; tick < 600; ++tick) if (scene.step().doors[0] == core::DoorSnapshotState::Open) { reopened = true; break; }
		require(reopened, "Same-side request lost during cycle");
	}

	void pauseResetAndPersistence(smoke::Context const&)
	{
		for (bool duringCycle : { false, true })
		{
			Scene scene; scene.route();
			bool reached = false;
			for (uint32_t tick = 0; tick < 2400; ++tick)
			{
				auto state = scene.step();
				if (!state.occupants.empty() && (duringCycle ? !state.cycleComplete : !state.crossings.empty()))
				{ reached = true; break; }
			}
			require(reached, "Pause fixture failed to reach occupied boundary: duringCycle=" + std::to_string(duringCycle) + " sector=" + std::to_string(scene.agent->getSector()->getIndex()) + " state=" + std::to_string((int)scene.agent->getState()));
			scene.world.pauseSimulation();
			auto before = scene.world.getSimulationSnapshot().airlocks.at(0);
			auto tick = scene.world.getSimulationTick();
			scene.world.update(100);
			require(!scene.world.advanceTick() && scene.world.getSimulationTick() == tick
				&& scene.chamber()->getRemainingCycleSeconds() == before.remainingCycleSeconds
				&& scene.world.getSimulationSnapshot().airlocks[0].occupants == before.occupants, "Pause changed cycle/capacity");
			require(scene.world.resumeSimulation(), "Occupied resume refused");
			for (uint32_t step = 0; step < 2400 && scene.agent->getSector()->getIndex() != scene.right; ++step) scene.step();
			require(scene.agent->getSector()->getIndex() == scene.right, "Paused journey did not resume: duringCycle=" + std::to_string(duringCycle) + " state=" + std::to_string((int)scene.agent->getState()) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " doors=" + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[0]) + "," + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[1]) + " requests=" + std::to_string(scene.world.getSimulationSnapshot().traversalRequests.size()));
			scene.world.resetSimulation();
			auto reset = scene.world.getSimulationSnapshot().airlocks.at(0);
			require(reset.occupants.empty() && reset.reservations.empty() && reset.crossings.empty()
				&& reset.cycleComplete && reset.remainingCycleSeconds == 0
				&& reset.doors[0] == core::DoorSnapshotState::Closed && reset.doors[1] == core::DoorSnapshotState::Closed, "Reset retained Airlock work");
		}
		for (bool occupied : { false, true })
		{
			Scene scene;
			if (occupied) scene.route();
			else scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
			bool reached = false;
			for (uint32_t tick = 0; tick < 1200; ++tick)
				if (auto state = scene.step(); occupied ? !state.cycleComplete && !state.occupants.empty()
					: state.doors[0] == core::DoorSnapshotState::Open) { reached = true; break; }
			require(reached, "Reset fixture did not reach transient work");
			scene.world.resetSimulation();
			auto state = scene.world.getSimulationSnapshot().airlocks.at(0);
			require(state.occupants.empty() && state.reservations.empty() && state.crossings.empty()
				&& state.entrySide == -1 && state.cycleComplete && state.remainingCycleSeconds == 0
				&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed,
				"Reset did not discard transient entry/cycle/occupancy");
			scene.agent = scene.world.lookupAgent(scene.id).entity;
			if (occupied)
			{
				scene.world.wakeAllAgents();
				for (uint32_t tick = 0; tick < 3600 && scene.agent->getSector()->getIndex() != scene.right; ++tick) scene.step();
				require(scene.agent->getSector()->getIndex() == scene.right, "Reset journey did not repeat: state=" + std::to_string((int)scene.agent->getState()) + " sector=" + std::to_string(scene.agent->getSector()->getIndex()) + " path=" + std::to_string(bool(scene.agent->getPath())) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " occupants=" + std::to_string(scene.world.getSimulationSnapshot().airlocks[0].occupants.size()) + " doors=" + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[0]) + "," + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[1]));
			}
			else
			{
				for (uint32_t tick = 0; tick < 120; ++tick)
					require(scene.step().doors[0] == core::DoorSnapshotState::Closed, "Reset retained empty request");
			}
		}
		for (int direction : { 0, 1 })
		{
			Scene scene;
			auto target = scene.marker;
			if (direction)
			{
				scene.world.pauseSimulation();
				scene.world.removeAgent(scene.id);
				scene.id = scene.world.createAgent("Reverse traveller", scene.right, 0, 1.5f);
				scene.agent = scene.world.lookupAgent(scene.id).entity;
				target = scene.world.addSectorMarker(scene.left, 0, 1.5f);
				scene.world.finishBuild(); require(scene.world.resumeSimulation(), "Reverse fixture resume refused");
			}
			auto path = scene.world.getGraph()->calculatePath(scene.agent, scene.world.getGraph()->getVertexForObject(target.sector->getObject(target.index)));
			require(bool(path), "Saved journey route unavailable"); scene.agent->setPath(path, true);
			for (uint32_t tick = 0; tick < 900 && scene.step().occupants.empty(); ++tick) {}
			require(!scene.world.getSimulationSnapshot().airlocks[0].occupants.empty(), "Save fixture did not board");
			auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
			work.markSerializedUnmodified = false; scene.world.serialize(*writer, work); writer->serialize();
			auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
			core::World loaded("Loaded", 1, 1); require(loaded.deserialize(*reader, work), "Saved journey refused");
			auto initial = loaded.getSimulationSnapshot().airlocks.at(0);
			require(initial.occupants.empty() && initial.cycleComplete && initial.crossings.empty(), "Load retained transient journey");
			auto agent = loaded.lookupAgent(scene.id).entity;
			require(agent && agent->getSector()->getIndex() == (direction ? scene.right : scene.left), "Load did not restore authored origin");
			loaded.wakeAllAgents();
			for (uint32_t tick = 0; tick < 2400 && agent->getSector()->getIndex() != (direction ? scene.left : scene.right); ++tick)
			{
				loaded.advanceTick(); auto state = loaded.getSimulationSnapshot().airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Loaded journey interlock violated");
			}
			require(agent->getSector()->getIndex() == (direction ? scene.left : scene.right), "Loaded journey stalled");
		}
	}
}
void registerAirlockCrawling(std::vector<smoke::Check>& checks);

void registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/structuralEditSafety", editSafety });
	checks.push_back({ "airlocks/singleAgentJourneys", journeys });
	registerAirlockCrawling(checks);
	checks.push_back({ "airlocks/batchesAndOpposingQueues", batches });
	checks.push_back({ "airlocks/approachingBatch", approachingBatch });
	checks.push_back({ "airlocks/boardingDeadline", boardingDeadline });
	checks.push_back({ "airlocks/sharedOutsideCall", sharedOutsideCall });
	checks.push_back({ "airlocks/lostReservationDoesNotRefill", lostReservation });
	checks.push_back({ "airlocks/interruptedJourneys", interruptedJourneys });
	checks.push_back({ "airlocks/abandonedBoarding", abandonedBoarding });
	checks.push_back({ "airlocks/ordinaryOpenBulkheadRegression", ordinaryOpenPassage });
	checks.push_back({ "airlocks/emptyCalls", emptyCalls });
	checks.push_back({ "airlocks/basicEstimates", basicEstimates });
	checks.push_back({ "airlocks/permissionsAndMobility", permissionsAndMobility });
	checks.push_back({ "airlocks/committedAuthorizationChanges", committedAuthorizationChanges });
	checks.push_back({ "airlocks/localAdherence", localAdherence });
	checks.push_back({ "airlocks/staleAuthorization", staleAuthorization });
	checks.push_back({ "airlocks/alternativeCosts", alternativeCosts });
	checks.push_back({ "airlocks/localQueueObservations", localQueueObservations });
	checks.push_back({ "airlocks/exitSideReadmission", exitSideReadmission });
	checks.push_back({ "airlocks/pauseResetAndPersistence", pauseResetAndPersistence });
}
