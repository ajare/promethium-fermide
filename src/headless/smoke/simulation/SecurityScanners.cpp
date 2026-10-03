#include "Checks.h"
#include "core/World.h"
#include "core/SecurityScannerTransit.h"
#include "core/Graph.h"
#include "core/Agent.h"
#include "core/Path.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/BulkheadDoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"
#include <cmath>
#include <map>

namespace
{
	using smoke::require;
	void sensing(smoke::Context const&)
	{
		for (bool direction : { false, true })
			for (float gap : { 0.49f, 0.51f })
			{
				core::World world("Scanner sensor", 10, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 3, 1), world.addCorridor(0, 0, 5, 3, 1) };
				auto index = world.addSecurityScanner(0, 0, 3, 2, direction); world.finishBuild();
				int side = direction ? 0 : 1;
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
				auto id = world.createAgent("Presence only", ends[side], 0, 1.5f);
				auto actor = world.lookupAgent(id).entity;
				auto halfDoor = chamber->getDoor(side)->getSize().x * 0.5f;
				float target = (side ? 5.0f : 3.0f) + (side ? 1 : -1) * (gap + halfDoor + actor->getWidth() * 0.5f);
				// Recreate at the exact nearest-edge gap using the public placement API.
				world.pauseSimulation(); require(world.removeAgent(id).removed, "Sensor fixture removal refused");
				id = world.createAgent("Presence only", ends[side], 0, target - world.getSector(ends[side])->getPosition().x);
				require(world.resumeSimulation(), "Sensor resume refused");
				bool opened = false, closed = false;
				for (unsigned tick = 0; tick < 1300; ++tick)
				{
					world.advanceTick(); auto state = world.getSimulationSnapshot().securityScanners.at(0);
					opened = opened || state.doors[side] == core::DoorSnapshotState::Open;
					closed = closed || (opened && state.phase == "Idle");
					require(!state.occupant && state.reservations.empty() && state.scanProgress == 0
						&& state.remainingSeconds == 0 && state.doors[1 - side] == core::DoorSnapshotState::Closed,
						"Empty presence opening scanned or violated interlock");
				}
				require(opened == (gap < 0.5f) && (gap > 0.5f || closed), "Sensor gap boundary or empty opening timeout incorrect");
			}
	}

	void permissions(smoke::Context const&)
	{
		for (bool direction : { false, true })
			{
				core::World world("Scanner destination permission", 10, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 3, 1), world.addCorridor(0, 0, 5, 3, 1) };
				world.addSecurityScanner(0, 0, 3, 2, direction);
				int entry = direction ? 0 : 1, exit = 1 - entry;
				auto marker = world.addSectorMarker(ends[exit], 0, 1.5f); world.finishBuild(); world.pauseSimulation();
				auto id = world.createAgent("Traveller", ends[entry], 0, entry ? 0.5f : 2.5f);
				auto actor = world.lookupAgent(id).entity;
				auto target = world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index));
				auto stale = world.getGraph()->calculatePath(actor, target); require(bool(stale), "Unprotected scanner route missing");
				auto permission = world.addAccessPermission("Destination");
				require(world.setLocationPermissionRequirement(ends[exit], { permission }), "Destination requirement refused");
				require(!world.getGraph()->calculatePath(actor, target), "Scanner route ignored destination Location requirement");
				actor->setPath(stale, true); world.resumeSimulation();
				for (unsigned tick = 0; tick < 600; ++tick)
				{
					world.advanceTick(); auto state = world.getSimulationSnapshot().securityScanners.at(0);
					require(!state.occupant && state.reservations.empty() && actor->getSector()->getIndex() == ends[entry],
						"Stale scanner Path bypassed destination authorization");
				}
				world.pauseSimulation(); require(world.grantAgentAccessPermission(id, permission), "Destination grant refused");
				auto permitted = world.getGraph()->calculatePath(actor, target); require(bool(permitted), "Authorized scanner route refused");
				core::MobilityProfile mobility; mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
				require(world.setAgentIndividualMobilityProfile(id, mobility), "Door restriction refused");
				require(!world.getGraph()->calculatePath(actor, target), "Scanner route ignored Door Mobility use");
				actor->setPath(permitted, true); world.resumeSimulation(); world.advanceTicks(600);
				require(actor->getSector()->getIndex() == ends[entry] && !world.getSimulationSnapshot().securityScanners.at(0).occupant,
					"Runtime scanner admission ignored Door Mobility use");
			}
	}

	void restoration(smoke::Context const&)
	{
		for (std::string stage : { "Boarding", "Positioning", "Pre-delay", "Scanning", "Post-pause", "Exiting", "Exit closing" })
			for (bool binary : { false, true })
			{
				core::World world("Scanner restoration", 12, 2);
				world.addRoom("Left", 0, 0, 0, 3, 1); auto right = world.addCorridor(0, 0, 6, 3, 1);
				auto index = world.addSecurityScanner(0, 0, 3, 3);
				auto marker = world.addSectorMarker(right, 0, 1.5f); world.finishBuild();
				auto id = world.createAgent("Traveller", 0, 0, 2.5f);
				auto actor = world.lookupAgent(id).entity;
				actor->setPath(world.getGraph()->calculatePath(actor, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				bool reached = false;
				for (unsigned tick = 0; tick < 2000 && !reached; ++tick)
				{
					world.advanceTick(); reached = world.getSimulationSnapshot().securityScanners.at(0).phase == stage;
				}
				require(reached, "Restoration fixture stalled before " + stage);
				world.pauseSimulation();
				auto frozen = world.getSimulationSnapshot().securityScanners.at(0);
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
				float doorPositions[] = { chamber->getDoor(0)->getOpenPercentage(), chamber->getDoor(1)->getOpenPercentage() };
				world.advanceTicks(100);
				auto after = world.getSimulationSnapshot().securityScanners.at(0);
				require(after.phase == frozen.phase && after.remainingSeconds == frozen.remainingSeconds && after.scanProgress == frozen.scanProgress
					&& chamber->getDoor(0)->getOpenPercentage() == doorPositions[0] && chamber->getDoor(1)->getOpenPercentage() == doorPositions[1],
					"Pause advanced scanner phase or Door motion at " + stage);
				require(!world.planResizeAirlock(index, 3, 0, 2).valid && !world.planRemoveAirlock(index).valid
					&& !world.planResizeLocation(index, 3, 0, 2, 1).valid && !world.planRemoveLocation(index).valid,
					"Scanner structural edit accepted during journey");
				bool refused = false;
				try { world.createAgent("Direct placement", index, 0, 0.5f); } catch (std::exception const&) { refused = true; }
				require(refused, "Direct scanner placement accepted during journey");
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				auto write = [&](auto output) {
					world.serialize(*output, work); output->serialize(); return output->getSerializedString();
				};
				auto saved = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
				std::unique_ptr<core::Serializer> input = binary
					? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(saved))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(saved));
				input->deserialize(); core::World loaded("Load", 1, 1);
				require(loaded.deserialize(*input, work), "Occupied scanner document refused");
				world.resetSimulation();
				for (auto restored : { &world, &loaded })
				{
					auto snapshot = restored->getSimulationSnapshot(); auto state = snapshot.securityScanners.at(0);
					require(state.phase == "Idle" && state.scanProgress == 0 && state.remainingSeconds == 0 && !state.occupant
						&& state.reservations.empty() && state.crossings.empty() && state.doors[0] == core::DoorSnapshotState::Closed
						&& state.doors[1] == core::DoorSnapshotState::Closed && snapshot.traversalRequests.empty()
						&& snapshot.traversalPermits.empty() && snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty(),
						"Reset/load retained journey operations");
					auto restoredAgent = restored->lookupAgent(id).entity;
					require(restoredAgent && restoredAgent->getSector()->getIndex() == 0 && restoredAgent->getGlobalPosition().x == 2.5f,
						"Reset/load did not restore authored Agent placement");
					for (auto const& resource : snapshot.traversalResources)
						for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Reset/load retained queue");
					require(restored->resumeSimulation(), "Restored scanner resume refused");
					for (unsigned tick = 0; tick < 2400 && restoredAgent->getSector()->getIndex() != right; ++tick) restored->advanceTick();
					require(restoredAgent->getSector()->getIndex() == right, "Reset/load could not complete restored routed journey");
				}
			}
	}

	void journeys(smoke::Context const&)
	{
		for (uint32_t width : { 1u, 2u, 5u })
			for (bool direction : { false, true })
			{
				core::World world("Scanner journey", width + 8, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 3, 1), world.addCorridor(0, 0, width + 3, 3, 1) };
				auto index = world.addSecurityScanner(0, 0, 3, width, direction);
				int entry = direction ? 0 : 1, exit = 1 - entry;
				auto marker = world.addSectorMarker(ends[exit], 0, 1.5f);
				auto reverseMarker = world.addSectorMarker(ends[entry], 0, 1.5f);
				world.finishBuild(); world.pauseSimulation();
				auto id = world.createAgent("Traveller", ends[entry], 0, 1.5f);
				auto actor = world.lookupAgent(id).entity;
				core::MobilityProfile mobility; mobility.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
				require(world.setAgentIndividualMobilityProfile(id, mobility), "Buttons property refused");
				auto path = world.getGraph()->calculatePath(actor, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
				require(bool(path), "Automatic scanner route unavailable without Buttons"); actor->setPath(path, true);
				core::RouteDecisionContext remote{ actor, {}, {}, nullptr, actor->getWalkSpeed(), &world };
				unsigned thresholds = 0;
				for (auto const& node : path->nodes)
					if (node.edge && node.edge->getType() == core::EdgeType::BulkheadDoor)
					{
						++thresholds;
						auto facts = node.edge->getDirectedTraversalFacts(node.targetVertex, remote);
						require(facts.feasible && facts.components.interactionUnits == 0
							&& facts.components.expectedWaitSeconds >= CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME,
							"Scanner baseline omitted automatic wait or added phantom button costs");
					}
				require(thresholds == 2, "Route bypassed scanner thresholds");
				auto reverseId = world.createAgent("Reverse", ends[exit], 0, exit ? 0.5f : 2.5f);
				auto reverse = world.lookupAgent(reverseId).entity;
				auto reverseTarget = world.getGraph()->getVertexForObject(reverseMarker.sector->getObject(reverseMarker.index));
				require(!world.getGraph()->calculatePath(reverse, reverseTarget), "Reverse topology admitted scanner");
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
				std::map<std::string, uint64_t> starts;
				std::string previous;
				bool boarded = false, exited = false, reverseGate = false, reverseDenied = false, paused = false;
				float previousEntryPct = 0;
				core::AgentId follower;
				require(world.resumeSimulation(), "Scanner resume refused");
				for (unsigned tick = 0; tick < 2400; ++tick)
				{
					require(world.advanceTick(), "Scanner tick refused");
					auto snapshot = world.getSimulationSnapshot(); auto state = snapshot.securityScanners.at(0);
					require(state.leftToRight == direction && (state.doors[0] == core::DoorSnapshotState::Closed
						|| state.doors[1] == core::DoorSnapshotState::Closed), "Scanner interlock/direction violated");
					require(state.reservations.size() + (state.occupant ? 1 : 0) <= 1, "Scanner overbooked capacity");
					if (state.phase == "Entry opening")
					{
						auto pct = chamber->getDoor(entry)->getOpenPercentage();
						require(pct - previousEntryPct <= world.getFixedTimestep() / CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME + 0.001f,
							"Door advanced twice in a tick"); previousEntryPct = pct;
					}
					if (!follower && !state.reservations.empty())
					{
						follower = world.createAgent("Follower", ends[entry], 0, entry ? 0.5f : 2.5f);
						auto next = world.lookupAgent(follower).entity;
						next->setPath(world.getGraph()->calculatePath(next, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
					}
					if (!exited && follower) require(world.lookupAgent(follower).entity->getSector()->getIndex() == ends[entry], "Follower piggybacked reserved slot");
					require(snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty(), "Automatic scanner used hidden buttons");
					if (state.phase != previous)
					{
						starts[state.phase] = world.getSimulationTick(); previous = state.phase;
						if (state.phase == "Pre-delay" || state.phase == "Scanning" || state.phase == "Post-pause")
							require(std::abs(state.remainingSeconds - (state.phase == "Scanning" ? 2.0f : 1.0f)) < 0.001f, "Timer boundary omitted first tick");
					}
					if (state.phase == "Entry closing" && state.occupant)
						require(actor->getGlobalPosition().distanceTo(chamber->getPosition() + core::Vector2{ width * 0.5f, 0 }) < 0.001f,
							"Entry closure began before occupant positioned");
					if (state.phase == "Pre-delay" || state.phase == "Scanning" || state.phase == "Post-pause")
					{
						require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed,
							"Timed scan ran with open threshold");
						require(actor->getGlobalPosition().distanceTo(chamber->getPosition() + core::Vector2{ width * 0.5f, 0 }) < 0.001f, "Scan began before occupant centred");
					}
					if (state.phase == "Scanning") require(std::abs(state.scanProgress - (1 - state.remainingSeconds / 2)) < 0.001f, "Unstable normalized scan progress");
					if (state.phase == "Exit opening") require(state.scanProgress == 1, "Exit opened before successful scan");
					if (state.phase == "Scanning" && !paused)
					{
						paused = true; world.pauseSimulation();
						auto position = actor->getGlobalPosition(); auto frozen = world.getSimulationSnapshot().securityScanners.at(0);
						world.advanceTicks(100);
						auto after = world.getSimulationSnapshot().securityScanners.at(0);
						require(after.phase == frozen.phase && after.remainingSeconds == frozen.remainingSeconds
							&& after.scanProgress == frozen.scanProgress && after.doors == frozen.doors
							&& actor->getGlobalPosition().distanceTo(position) == 0, "Global pause advanced scan/motion");
						require(world.resumeSimulation(), "Occupied scanner resume refused");
					}
					if (state.doors[exit] == core::DoorSnapshotState::Open)
					{
						require(!world.getGraph()->calculatePath(reverse, reverseTarget), "Open exit admitted reverse route");
						if (!reverseGate)
						{
							reverseGate = true;
							for (auto const& node : path->nodes)
								if (node.edge && node.edge->getType() == core::EdgeType::BulkheadDoor && node.targetVertex->getSector()->getIndex() == ends[exit])
								{
									auto source = node.targetVertex, target = node.edge->getOtherVertex(source);
									// Install stale intent through the production Path API; runtime
									// admission must still refuse it even though the exit is open.
									auto stale = std::make_shared<core::Path>();
									stale->nodes = { { {}, source, 0, {}, {} }, { node.edge, target, 0, {}, {} } };
									reverse->setPath(stale, true);
									auto ordinary = std::make_shared<core::BulkheadDoorEdge>(std::const_pointer_cast<core::BulkheadDoor>(chamber->getDoor(exit)));
									require(!ordinary->isTraversable(target, {}) && !ordinary->getDirectedTraversalFacts(target, remote).feasible,
										"Ordinary Bulkhead edge bypassed scanner");
								}
						}
					}
					for (auto const& request : snapshot.traversalRequests)
					{
						if (request.owner == reverseId && request.state == core::TraversalRequestState::Denied) reverseDenied = true;
						if (request.resource == chamber->getTraversalResourceId() && request.state == core::TraversalRequestState::Granted)
						{
							bool entering = request.destinationSector.value == index + 1;
							require(state.doors[entering ? entry : exit] == core::DoorSnapshotState::Open,
								"Crossing without fully open Door");
							require(entering ? state.reservations.size() == 1 : state.occupant == request.owner,
								"Crossing without reserved/occupied capacity");
						}
					}
					require(reverse->getSector()->getIndex() == ends[exit], "Runtime open-exit reverse entry succeeded");
					if (state.occupant) { boarded = true; require(state.occupant == id, "Unexpected occupant"); }
					if (boarded && actor->getSector()->getIndex() == ends[exit]) exited = true;
					if (exited && actor->getState() == core::Agent::State::Idle && state.phase == "Idle") break;
				}
				require(boarded && exited && reverseGate && reverseDenied && paused && actor->getState() == core::Agent::State::Idle, "Scanner journey stalled: " + previous);
				require(starts.at("Scanning") - starts.at("Pre-delay") == core::secondsToTicks(1, world.getFixedTimestep())
					&& starts.at("Post-pause") - starts.at("Scanning") == core::secondsToTicks(2, world.getFixedTimestep())
					&& starts.at("Exit opening") - starts.at("Post-pause") == core::secondsToTicks(1, world.getFixedTimestep()), "Default timing sequence incorrect");
			}
	}
}

void registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/automaticJourneys", journeys });
	checks.push_back({ "securityScanners/presenceAndEmptyTimeout", sensing });
	checks.push_back({ "securityScanners/resetAndLoad", restoration });
	checks.push_back({ "securityScanners/destinationAndMobilityGates", permissions });
}
