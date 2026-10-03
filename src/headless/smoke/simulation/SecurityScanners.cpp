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
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include <cmath>
#include <map>
#include <limits>

namespace core
{
	// Narrow test-only access to existing structural restoration, following the
	// GraphSourceIndexTestAccess pattern. No public occupancy mutation API.
	struct WorldAgentRestorationTestAccess
	{
		static void restoreAt(World& world, AgentId id, Vector2 position)
		{
			auto carried = world.captureAgentsForReplay();
			std::vector<World::CarriedAgent> selected;
			for (auto saved : carried) if (saved.id == id)
			{
				saved.position = position; selected.push_back(saved);
			}
			smoke::require(world.removeAgent(id).removed, "Carried fixture removal refused");
			world.restoreCarriedAgents(selected, false);
		}
	};
}

namespace
{
	using smoke::require;
	core::MarkerId markerId(core::World::CreateObjectResult const& marker)
	{
		return std::static_pointer_cast<core::MarkerSectorObject>(marker.sector->getObject(marker.index))->getMarker()->getId();
	}

	void safeCapacity(core::World const& world, uint32_t index)
	{
		auto snapshot = world.getSimulationSnapshot();
		auto const& state = snapshot.securityScanners.at(0);
		require(world.getSector(index)->getAgents().size() <= 1, "Physical scanner capacity exceeded");
		require(state.reservations.size() + (state.occupant ? 1 : 0) <= 1 && state.crossings.size() <= 1,
			"Scanner slot/crossing overbooked");
		require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed,
			"Scanner interlock violated");
		if (state.phase == "Pre-delay" || state.phase == "Scanning" || state.phase == "Post-pause")
			require(state.occupant && world.getSector(index)->getAgents().size() == 1
				&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed,
				"Scan without one sealed occupant");
	}

	void editSafety(smoke::Context const&)
	{
		for (bool forward : { false, true })
			for (int stage = 0; stage < 7; ++stage)
			{
				core::World world("Scanner edit safety", 14, 2);
				for (uint32_t row = 0; row < 2; ++row)
				{
					world.addRoom("Left", 0, row, 0, 4, 1);
					world.addCorridor(0, row, stage == 4 && row == 1 ? 7 : 6, 4, 1);
				}
				auto index = world.addSecurityScanner(0, 0, 4, 2, forward);
				auto source = forward ? 0u : 1u, destination = 1 - source;
				auto marker = world.addSectorMarker(destination, 0, 2.0f);
				auto reverseMarker = world.addSectorMarker(source, 0, 1.0f);
				world.finishBuild();
				auto id = world.createAgent("Traveller", source, 0, 2.0f);
				auto agent = world.lookupAgent(id).entity;
				agent->setPath(world.getGraph()->calculatePath(agent,
					world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				core::AgentId waiterId;
				if (stage == 0 || stage >= 4)
				{
					waiterId = world.createAgent("Waiting traveller", source, 0, 2.0f);
					auto waiter = world.lookupAgent(waiterId).entity;
					waiter->setPath(world.getGraph()->calculatePath(waiter,
						world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
				}
				bool reached = false;
				for (unsigned tick = 0; tick < 2000 && !reached; ++tick)
				{
					world.advanceTick();
					auto state = world.getSimulationSnapshot().securityScanners.at(0);
					if (stage == 0 || stage >= 4) reached = !state.reservations.empty() && state.crossings.empty() && !state.occupant;
					else if (stage == 1) reached = !state.crossings.empty() && !state.occupant;
					else if (stage == 2) reached = state.occupant && state.crossings.empty();
					else reached = !state.crossings.empty() && state.occupant;
				}
				require(reached, "Scanner edit fixture failed journey boundary stage=" + std::to_string(stage));
				world.pauseSimulation();
				if (stage > 0 && stage < 4)
				{
					auto before = world.getSimulationSnapshot().securityScanners.at(0);
					world.markSaved();
					for (auto plan : { world.planResizeSecurityScanner(index, 4, 1, 2, forward),
						world.planResizeSecurityScanner(index, 4, 1, 3, forward),
						world.planResizeSecurityScanner(index, 4, 0, 2, !forward), world.planRemoveSecurityScanner(index) })
					{
						require(!plan.valid, "Occupied/crossing scanner edit planned");
						plan.valid = true;
						bool refused = false;
						try { world.applySecurityScannerEdit(plan); } catch (core::Exception const&) { refused = true; }
						require(refused && !world.isModified() && world.getNumSectors() == 5, "Forged occupied plan mutated World");
					}
					world.advanceTicks(60);
					auto after = world.getSimulationSnapshot().securityScanners.at(0);
					require(after.occupant == before.occupant && after.crossings == before.crossings
						&& after.reservations == before.reservations && after.doors == before.doors
						&& after.phase == before.phase && after.scanProgress == before.scanProgress,
						"Paused refusal changed journey");
				}
				else
				{
					auto stale = world.planRemoveSecurityScanner(index);
					world.resumeSimulation(); world.advanceTicks(1);
					bool refused = false;
					try { world.applySecurityScannerEdit(stale); } catch (core::Exception const&) { refused = true; }
					require(refused, "Running stale scanner plan accepted");
					world.pauseSimulation();
					auto plan = stage == 5 ? world.planRemoveSecurityScanner(index)
						: world.planResizeSecurityScanner(index, 4,
							stage == 6 ? 0 : 1, stage == 4 ? 3 : 2, stage == 6 ? !forward : forward);
					require(plan.valid, "Reserved empty scanner edit refused");
					require(world.getSimulationSnapshot().traversalRequests.size() >= 2, "Edit fixture omitted waiting ticket");
					index = world.applySecurityScannerEdit(plan);
					auto state = world.getSimulationSnapshot();
					require(state.traversalRequests.empty() && state.traversalPermits.empty(), "Scanner edit retained requests/permits");
					if (stage == 5)
						require(state.securityScanners.empty() && state.traversalResources.empty(), "Deleted scanner authority survived");
					else
					{
						require(state.securityScanners.at(0).reservations.empty() && !state.securityScanners.at(0).occupant
							&& state.securityScanners.at(0).crossings.empty(), "Scanner edit retained stale admission");
						if (stage != 6) index = world.applySecurityScannerEdit(world.planResizeSecurityScanner(index, 4, 0, 2, forward));
					}
				}
				world.resumeSimulation(); agent = world.lookupAgent(id).entity;
				require(agent, "Replay lost waiting Agent identity");
				world.advanceTicks(6000);
				require(agent->getSector()->getIndex() == ((stage == 5 || stage == 6) ? source : destination),
					"Scanner edited/refused route recovery failed stage=" + std::to_string(stage)
					+ " sector=" + std::to_string(agent->getSector()->getIndex())
					+ " phase=" + (stage == 5 ? "deleted" : world.getSimulationSnapshot().securityScanners.at(0).phase));
				if (waiterId)
					require(world.lookupAgent(waiterId).entity->getSector()->getIndex()
						== ((stage == 5 || stage == 6) ? source : destination), "Queued Agent did not recover after scanner edit");
				if (stage == 6)
				{
					// Reversed routing and gates also serve a new real forward journey.
					auto reverseObject = world.getSector(source)->getObject(reverseMarker.index);
					auto otherId = world.createAgent("Reverse traveller", destination, 0, 2.0f);
					auto other = world.lookupAgent(otherId).entity;
					other->setPath(world.getGraph()->calculatePath(other,
						world.getGraph()->getVertexForObject(reverseObject)), true);
					world.advanceTicks(2400);
					require(other->getSector()->getIndex() == source, "Reversed scanner real forward journey failed");
				}
			}
	}

	void contention(smoke::Context const&)
	{
		for (uint32_t width : { 1u, 2u, 5u })
			for (bool direction : { false, true })
			{
				core::World world("Scanner contention", width + 16, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 7, 1), world.addCorridor(0, 0, width + 7, 7, 1) };
				auto index = world.addSecurityScanner(0, 0, 7, width, direction);
				int entry = direction ? 0 : 1, exit = 1 - entry;
				auto destination = markerId(world.addSectorMarker(ends[exit], 0, 3.5f));
				world.finishBuild();
				std::vector<core::AgentId> actors;
				for (unsigned i = 0; i < 3; ++i)
					actors.push_back(world.createAgent("Contender", ends[entry], 0, entry ? 0.5f : 6.5f));
				// Install simultaneous intent in reverse identity order: service is
				// determined by observed queue tickets, not registry order.
				for (auto it = actors.rbegin(); it != actors.rend(); ++it)
					require(world.moveAgentToMarker(*it, destination).accepted(), "Contender command refused");
				bool late = false, queuePaused = false;
				core::AgentId previousOccupant;
				unsigned admissions = 0;
				std::map<core::AgentId, core::QueueTicketId> tickets;
				core::QueueTicketId previousTicket;
				bool complete = false;
				for (unsigned tick = 0; tick < 12000; ++tick)
				{
					world.advanceTick(); safeCapacity(world, index);
					auto snapshot = world.getSimulationSnapshot(); auto state = snapshot.securityScanners.at(0);
					for (auto const& request : snapshot.traversalRequests)
						if (request.destinationSector.value == index + 1 && request.queueTicket)
							tickets[request.owner] = request.queueTicket;
					if (state.occupant && state.occupant != previousOccupant)
					{
						++admissions;
						auto ticket = tickets.at(state.occupant);
						require(!previousTicket || previousTicket < ticket, "Later ticket overtook eligible predecessor");
						for (auto const& [owner, waitingTicket] : tickets)
							if (world.lookupAgent(owner).entity->getSector()->getIndex() == ends[entry])
								require(ticket < waitingTicket, "Admission skipped oldest waiting ticket");
						previousTicket = ticket;
					}
					previousOccupant = state.occupant;
					if (!queuePaused && late && state.phase == "Scanning")
					{
						queuePaused = true;
						std::map<core::TraversalRequestId, core::QueueTicketId> pending;
						for (auto const& request : snapshot.traversalRequests)
							if (request.destinationSector.value == index + 1 && request.state == core::TraversalRequestState::Pending)
								pending[request.id] = request.queueTicket;
						require(pending.size() >= 2, "Contended pause had no waiting tickets");
						world.pauseSimulation(); world.advanceTicks(100);
						require(world.resumeSimulation(), "Contended pause resume refused");
						auto resumed = world.getSimulationSnapshot();
						for (auto const& [id, ticket] : pending)
						{
							unsigned matches = 0;
							for (auto const& request : resumed.traversalRequests)
								if (request.id == id && request.queueTicket == ticket) ++matches;
							require(matches == 1, "Pause recreated or duplicated scanner waiting ticket");
						}
					}
					if (!late && state.phase == "Boarding")
					{
						late = true;
						auto id = world.createAgent("Open entry piggyback", ends[entry], 0, entry ? 0.3f : 6.7f);
						actors.push_back(id);
						require(world.moveAgentToMarker(id, destination).accepted(), "Late arrival refused");
					}
					if (state.phase == "Exit closing")
						require(state.reservations.empty() && state.doors[entry] == core::DoorSnapshotState::Closed,
							"Next entry reserved/opened before exit closed");
					complete = late && admissions == actors.size() && state.phase == "Idle";
					for (auto id : actors) complete = complete && world.lookupAgent(id).entity->getSector()->getIndex() == ends[exit]
						&& world.lookupAgent(id).entity->getState() == core::Agent::State::Idle;
					if (complete) break;
				}
				std::string diagnostic = "Repeated contended journeys stalled width=" + std::to_string(width)
					+ " direction=" + std::to_string(direction) + " admissions=" + std::to_string(admissions)
					+ " phase=" + world.getSimulationSnapshot().securityScanners.at(0).phase
					+ " doors=" + std::to_string((int)world.getSimulationSnapshot().securityScanners.at(0).doors[0])
					+ "," + std::to_string((int)world.getSimulationSnapshot().securityScanners.at(0).doors[1]);
				for (auto id : actors) diagnostic += " actor=" + std::to_string(id.value) + " sector="
					+ std::to_string(world.lookupAgent(id).entity->getSector()->getIndex()) + " x="
					+ std::to_string(world.lookupAgent(id).entity->getGlobalPosition().x);
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					diagnostic += " request=" + std::to_string(request.id.value) + " owner=" + std::to_string(request.owner.value)
						+ " state=" + std::to_string((int)request.state) + " source=" + std::to_string(request.sourceSector.value)
						+ " dest=" + std::to_string(request.destinationSector.value);
				require(complete, diagnostic);
				world.advanceTicks(5);
				auto snapshot = world.getSimulationSnapshot();
				require(snapshot.securityScanners.at(0).reservations.empty() && snapshot.securityScanners.at(0).crossings.empty()
					&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Repeated use retained claims");
			}
	}

	void abandonment(smoke::Context const&)
	{
		for (std::string stage : { "Entry opening", "Boarding" })
			for (bool deactivate : { false, true })
			{
				// Movement cancellation preserves an in-flight crossing. Before
				// grant it abandons entry; after grant it must finish safely and
				// release capacity. Deactivation exercises safe-source rollback.
				bool committedCancellation = stage == "Boarding" && !deactivate;
				core::World world("Scanner abandonment", 14, 2);
				auto left = world.addRoom("Left", 0, 0, 0, 5, 1);
				auto right = world.addCorridor(0, 0, 8, 5, 1);
				auto index = world.addSecurityScanner(0, 0, 5, 3);
				auto destination = markerId(world.addSectorMarker(right, 0, 2.5f)); world.finishBuild();
				auto first = world.createAgent("Abandon", left, 0, 4.5f);
				require(world.moveAgentToMarker(first, destination).accepted(), "Abandon command refused");
				bool reached = false;
				for (unsigned tick = 0; tick < 1000; ++tick)
				{
					world.advanceTick(); safeCapacity(world, index);
					auto state = world.getSimulationSnapshot().securityScanners.at(0);
					if (state.phase == stage && !state.reservations.empty()) { reached = true; break; }
				}
				require(reached, "Abandon fixture failed before " + stage);
				if (deactivate)
				{
					world.pauseSimulation();
					require(world.setAgentActive(first, false), "Boarder deactivation refused");
					require(world.getSimulationSnapshot().securityScanners.at(0).reservations.empty(), "Inactive boarder retained reservation");
					require(world.resumeSimulation(), "Abandon resume refused");
				}
				else require(world.cancelAgentMovement(first).accepted(), "Boarder cancellation refused");
				bool recovered = false;
				for (unsigned tick = 0; tick < 2400; ++tick)
				{
					world.advanceTick(); safeCapacity(world, index);
					auto state = world.getSimulationSnapshot().securityScanners.at(0);
					if (!committedCancellation)
						require(!state.occupant && state.scanProgress == 0 && state.remainingSeconds == 0,
							"Abandoned boarder caused scan at " + stage + " deactivate=" + std::to_string(deactivate));
					else require(!state.occupant || state.occupant == first, "Cancelled crossing admitted another occupant");
					if (state.phase == "Idle" && state.reservations.empty() && state.crossings.empty()) { recovered = true; break; }
				}
				require(recovered && world.lookupAgent(first).entity->getSector()->getIndex() == (committedCancellation ? right : left), "Abandoned admission stranded scanner");
				auto next = world.createAgent("Successor", left, 0, 4.5f);
				require(world.moveAgentToMarker(next, destination).accepted(), "Successor command refused");
				bool exited = false;
				for (unsigned tick = 0; tick < 2400; ++tick)
				{
					world.advanceTick(); safeCapacity(world, index);
					exited = world.lookupAgent(next).entity->getSector()->getIndex() == right;
					if (exited && world.getSimulationSnapshot().securityScanners.at(0).phase == "Idle") break;
				}
				require(exited, "Successor could not reuse abandoned scanner");
			}
	}

	void interruptions(smoke::Context const&)
	{
		for (bool direction : { false, true })
			for (std::string stage : { "Positioning", "Pre-delay", "Scanning", "Post-pause", "Exiting", "Exit crossing" })
				for (std::string change : { "destination", "replacementPath", "path", "publicPath", "grant", "requirement", "inactive" })
				{
					core::World world("Committed scanner exit", 16, 2);
					uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 5, 1), world.addCorridor(0, 0, 8, 5, 1) };
					auto index = world.addSecurityScanner(0, 0, 5, 3, direction);
					int entry = direction ? 0 : 1, exit = 1 - entry;
					auto destination = markerId(world.addSectorMarker(ends[exit], 0, 3.5f));
					auto replacement = markerId(world.addSectorMarker(ends[exit], 0, 1.5f));
					world.finishBuild(); world.pauseSimulation();
					auto permission = world.addAccessPermission("Destination");
					auto id = world.createAgent("Occupant", ends[entry], 0, entry ? 0.5f : 4.5f);
					require(world.grantAgentAccessPermission(id, permission), "Initial grant refused");
					if (change == "grant") require(world.setLocationPermissionRequirement(ends[exit], { permission }), "Initial requirement refused");
					require(world.moveAgentToMarker(id, destination).accepted() && world.resumeSimulation(), "Interruption fixture refused");
					auto actor = world.lookupAgent(id).entity;
					bool reached = false;
					for (unsigned tick = 0; tick < 2400; ++tick)
					{
						world.advanceTick(); safeCapacity(world, index);
						auto state = world.getSimulationSnapshot().securityScanners.at(0);
						bool atStage = state.phase == stage;
						if (stage == "Exit crossing")
							for (auto const& request : world.getSimulationSnapshot().traversalRequests)
								atStage = atStage || (request.owner == id && request.sourceSector.value == index + 1
									&& request.destinationSector.value == ends[exit] + 1 && request.state == core::TraversalRequestState::Granted);
						if (atStage && state.occupant == id) { reached = true; break; }
					}
					require(reached, "Interruption fixture stalled at " + stage);
					world.pauseSimulation();
					if (change == "destination") require(world.moveAgentToMarker(id, replacement).accepted(), "Replacement refused");
					if (change == "path") actor->clearPath();
					if (change == "publicPath") require(world.clearAgentPath(id), "Public Path loss refused");
					if (change == "replacementPath")
					{
						for (auto const& node : actor->getPath()->nodes)
							if (node.targetVertex->getSector()->getIndex() == ends[exit])
							{
								auto path = world.getGraph()->calculatePath(actor, node.targetVertex);
								require(bool(path), "Replacement Path unavailable"); actor->setPath(path, true); break;
							}
					}
					if (change == "grant") require(world.revokeAgentAccessPermission(id, permission), "Revoke refused");
					if (change == "requirement")
					{
						auto other = world.addAccessPermission("New requirement");
						require(world.setLocationPermissionRequirement(ends[exit], { other }), "Requirement change refused");
					}
					if (change == "inactive") require(world.setAgentActive(id, false), "Deactivation refused");
					auto frozen = world.getSimulationSnapshot().securityScanners.at(0);
					auto position = actor->getGlobalPosition();
					world.advanceTicks(100);
					auto after = world.getSimulationSnapshot().securityScanners.at(0);
					require(after.occupant == id && after.phase == frozen.phase && after.remainingSeconds == frozen.remainingSeconds
						&& after.doors == frozen.doors && actor->getGlobalPosition() == position, "Pause lost commitment");
					auto follower = world.createAgent("Follower", ends[entry], 0, entry ? 0.5f : 4.5f);
					require(world.moveAgentToMarker(follower, destination).accepted(), "Follower refused");
					require(world.resumeSimulation(), "Interrupted resume refused");
					if (change == "inactive")
					{
						for (unsigned tick = 0; tick < 1000; ++tick)
						{
							world.advanceTick(); safeCapacity(world, index);
							require(world.getSimulationSnapshot().securityScanners.at(0).occupant == id
								&& actor->getGlobalPosition() == position && world.lookupAgent(follower).entity->getSector()->getIndex() == ends[entry],
								"Inactive occupant released capacity or moved");
						}
						if (stage != "Positioning") require(world.getSimulationSnapshot().securityScanners.at(0).phase == "Exiting", "Inactive automatic operation stalled");
						world.pauseSimulation(); require(world.setAgentActive(id, true) && world.resumeSimulation(), "Reactivation refused");
					}
					bool exited = false;
					for (unsigned tick = 0; tick < 2400; ++tick)
					{
						world.advanceTick(); safeCapacity(world, index);
						require(actor->getSector()->getIndex() != ends[entry], "Committed journey returned through entry");
						if (actor->getSector()->getIndex() == ends[exit]) { exited = true; break; }
						require(world.getSimulationSnapshot().securityScanners.at(0).occupant == id, "Path interruption abandoned occupancy");
					}
					require(exited, "Interrupted exit stalled: " + stage + " / " + change);
					if (change == "destination")
					{
						world.advanceTicks(1000);
						require(actor->getState() == core::Agent::State::Idle && std::abs(actor->getGlobalPosition().x
							- (world.getSector(ends[exit])->getPosition().x + 1.5f)) < 0.001f, "Replacement not planned after exit");
					}
				}
	}

	void admissionAuthorization(smoke::Context const&)
	{
		for (bool tighten : { false, true })
		{
			core::World world("Interrupted admission", 14, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 5, 1);
			auto right = world.addCorridor(0, 0, 8, 5, 1);
			auto index = world.addSecurityScanner(0, 0, 5, 3);
			auto destination = markerId(world.addSectorMarker(right, 0, 2.5f)); world.finishBuild(); world.pauseSimulation();
			auto key = world.addAccessPermission("Entry eligibility");
			auto id = world.createAgent("Boarder", left, 0, 4.5f);
			if (!tighten) require(world.setLocationPermissionRequirement(right, { key }) && world.grantAgentAccessPermission(id, key), "Admission authorization fixture refused");
			require(world.moveAgentToMarker(id, destination).accepted() && world.resumeSimulation(), "Boarder command refused");
			bool reserved = false;
			for (unsigned tick = 0; tick < 1600; ++tick)
			{
				world.advanceTick();
				auto state = world.getSimulationSnapshot().securityScanners.at(0);
				if (state.phase == "Entry opening" && !state.reservations.empty()) { reserved = true; break; }
			}
			require(reserved, "No pre-admission reservation");
			world.pauseSimulation();
			if (tighten) require(world.setLocationPermissionRequirement(right, { key }), "Admission tightening refused");
			else require(world.revokeAgentAccessPermission(id, key), "Admission revoke refused");
			require(world.resumeSimulation(), "Admission interruption resume refused");
			for (unsigned tick = 0; tick < 800; ++tick)
			{
				world.advanceTick(); safeCapacity(world, index);
				require(!world.getSimulationSnapshot().securityScanners.at(0).occupant
					&& world.lookupAgent(id).entity->getSector()->getIndex() == left, "Ineligible reserved boarder entered");
			}
			require(world.getSimulationSnapshot().securityScanners.at(0).reservations.empty(), "Ineligible reservation retained");
			world.pauseSimulation(); require(world.grantAgentAccessPermission(id, key) && world.resumeSimulation(), "Restored grant refused");
			require(world.moveAgentToMarker(id, destination).accepted(), "Restored entry command refused");
			world.advanceTicks(2400);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == right, "Restored authorization could not enter");
		}
	}

	void routeObservations(smoke::Context const&)
	{
		bool reversed = false;
		for (uint32_t detour = 10; detour < 36 && !reversed; detour += 2)
		{
			core::World world("Scanner alternatives", 40, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
			auto right = world.addRoom("Right", 0, 0, 5, 35, 1);
			auto index = world.addSecurityScanner(0, 0, 3, 2);
			world.addRoom("Detour", 1, 0, 0, 40, 1);
			world.addSectorDoor(0, 0, 0, {}); world.addSectorDoor(0, 0, detour, {});
			auto marker = world.addSectorMarker(right, 0, 1.5f); world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Chooser", left, 0, 2.5f);
			auto actor = world.lookupAgent(id).entity;
			auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
			auto target = world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index));
			auto choose = [&] {
				auto path = world.getGraph()->calculatePath(actor, target); require(bool(path), "Scanner alternative unavailable");
				return std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node) {
					return node.edge && node.edge->getTraversalResourceId() == chamber->getTraversalResourceId();
				});
			};
			require(world.setAgentIndividualWaitingAversion(id, 0.5f), "Low waiting aversion refused");
			bool low = choose();
			require(world.setAgentIndividualWaitingAversion(id, 3), "High waiting aversion refused");
			bool high = choose(); reversed = low && !high;
			if (!reversed) continue;
			// Observe the same entry edge with and without local knowledge while
			// real routed contenders fill its approach. Remote facts must not change.
			core::MobilityProfile mobility; mobility.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
			require(world.setAgentIndividualMobilityProfile(id, mobility) && world.setAgentIndividualWaitingAversion(id, 0.5f), "Buttons prohibition refused");
			auto path = world.getGraph()->calculatePath(actor, target); require(bool(path), "Automatic route required Buttons");
			core::PathNode const* entryNode = nullptr;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getTraversalResourceId() == chamber->getTraversalResourceId()
					&& node.targetVertex->getSector()->getIndex() == index) entryNode = &node;
			require(entryNode != nullptr, "Automatic alternative omitted scanner");
			core::RouteDecisionContext remote{ actor, {}, {}, nullptr, actor->getWalkSpeed(), &world };
			core::RouteDecisionContext local{ actor, {}, {}, actor->getSector(), actor->getWalkSpeed(), &world };
			auto before = entryNode->edge->getDirectedTraversalFacts(entryNode->targetVertex, remote);
			for (unsigned i = 0; i < 4; ++i)
			{
				auto waiter = world.createAgent("Queue", left, 0, 2.5f);
				require(world.setAgentIndividualMobilityProfile(waiter, mobility) && world.setAgentIndividualWaitingAversion(waiter, 0.5f)
					&& world.moveAgentToMarker(waiter, markerId(marker)).accepted(), "Queue fixture refused");
			}
			require(world.resumeSimulation(), "Observation resume refused");
			world.advanceTicks(220); safeCapacity(world, index);
			auto unseen = entryNode->edge->getDirectedTraversalFacts(entryNode->targetVertex, remote);
			auto seen = entryNode->edge->getDirectedTraversalFacts(entryNode->targetVertex, local);
			require(unseen.components.expectedWaitSeconds == before.components.expectedWaitSeconds && unseen.components.knownWaitSeconds == 0
				&& unseen.components.crowdingUnits == 0, "Remote scanner state leaked into costs");
			require(seen.components.knownWaitSeconds > 0 && seen.components.crowdingUnits > 0
				&& seen.components.interactionUnits == 0 && seen.components.expectedWaitSeconds == before.components.expectedWaitSeconds,
				"Local scanner queue omitted or introduced button costs");
		}
		require(reversed, "Automated duration did not affect route alternatives");
	}

	void occupancyViolation(smoke::Context const&)
	{
		core::World world("Defensive occupancy", 14, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 5, 1);
		auto right = world.addCorridor(0, 0, 8, 5, 1);
		auto index = world.addSecurityScanner(0, 0, 5, 3);
		auto destination = markerId(world.addSectorMarker(right, 0, 2.5f)); world.finishBuild();
		auto traveller = world.createAgent("Traveller", left, 0, 4.5f);
		require(world.moveAgentToMarker(traveller, destination).accepted(), "Defensive fixture command refused");
		bool scanning = false;
		for (unsigned tick = 0; tick < 1600; ++tick)
		{
			world.advanceTick(); safeCapacity(world, index);
			if (world.getSimulationSnapshot().securityScanners.at(0).phase == "Scanning") { scanning = true; break; }
		}
		require(scanning, "Defensive fixture did not scan"); world.pauseSimulation();
		bool refused = false;
		try { world.createAgent("Unsupported placement", index, 0, 0.5f); } catch (std::exception const&) { refused = true; }
		require(refused, "Public placement overbooked scanner");
		// Exercise the existing structural-restoration seam, not a new corruption
		// API. An inconsistent carried occupant must not be scanned successfully.
		auto extra = world.createAgent("Carried occupant", left, 0, 1.5f);
		core::WorldAgentRestorationTestAccess::restoreAt(world, extra, { 5.5f, 0 });
		require(world.getSector(index)->getAgents().size() == 2, "Restoration seam did not exercise defensive occupancy");
		auto waiting = world.createAgent("Blocked admission", left, 0, 4.5f);
		require(world.moveAgentToMarker(waiting, destination).accepted(), "Defensive waiting command refused");
		require(world.resumeSimulation(), "Defensive resume refused");
		for (unsigned tick = 0; tick < 800; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			require(world.lookupAgent(waiting).entity->getSector()->getIndex() == left, "Fault admitted another Agent");
			for (auto const& request : snapshot.traversalRequests)
				if (request.resource == std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index))->getTraversalResourceId())
					require(request.state != core::TraversalRequestState::Granted, "Fault issued a traversal permit");
		}
		auto state = world.getSimulationSnapshot().securityScanners.at(0);
		auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
		require(state.phase == "Occupancy violation: multiple Agents" && !chamber->isTraversalAvailable()
			&& state.remainingSeconds == 0 && state.scanProgress == 0 && state.reservations.empty() && state.crossings.empty()
			&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed,
			"Multiple occupancy completed a scan or was not reported");
		world.pauseSimulation(); require(world.removeAgent(extra).removed, "Fault occupant removal refused");
		require(world.resumeSimulation(), "Fault latch resume refused"); world.advanceTicks(10);
		require(!chamber->isTraversalAvailable(), "Occupancy fault resumed without reset");
		world.resetSimulation();
		require(world.getSimulationSnapshot().securityScanners.at(0).phase == "Idle", "Reset retained occupancy fault");
	}

	void sensing(smoke::Context const&)
	{
		for (bool direction : { false, true })
			for (bool entryApproach : { false, true })
			for (float sensor : { 0.0f, 0.25f, 0.5f, 1.0f })
			for (float gap : { std::max(0.0f, sensor - 0.01f), sensor, sensor + 0.01f })
			{
				core::World world("Scanner sensor", 10, 2);
				uint32_t ends[] = { world.addRoom("Left", 0, 0, 0, 3, 1), world.addCorridor(0, 0, 5, 3, 1) };
				auto index = world.addSecurityScanner(0, 0, 3, 2, direction);
				int entry = direction ? 0 : 1;
				int side = entryApproach ? entry : 1 - entry;
				auto away = markerId(world.addSectorMarker(ends[side], 0, 1.5f)); world.finishBuild();
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
				auto id = world.createAgent("Presence only", ends[side], 0, 1.5f);
				auto actor = world.lookupAgent(id).entity;
				auto halfDoor = chamber->getDoor(side)->getSize().x * 0.5f;
				float target = (side ? 5.0f : 3.0f) + (side ? 1 : -1) * (gap + halfDoor + actor->getWidth() * 0.5f);
				// Recreate at the exact nearest-edge gap using the public placement API.
				world.pauseSimulation();
				require(world.setSecurityScannerConfiguration(index, sensor, 1, 2, 1), "Sensor endpoint refused");
				require(world.removeAgent(id).removed, "Sensor fixture removal refused");
				id = world.createAgent("Presence only", ends[side], 0, target - world.getSector(ends[side])->getPosition().x);
				require(world.resumeSimulation(), "Sensor resume refused");
				require(world.moveAgentToMarker(id, away).accepted(), "Passing Agent command refused");
				bool opened = false, closed = false;
				uint64_t openedAt = 0, closingAt = 0;
				for (unsigned tick = 0; tick < 1300; ++tick)
				{
					world.advanceTick(); auto state = world.getSimulationSnapshot().securityScanners.at(0);
					if (!openedAt && state.doors[entry] == core::DoorSnapshotState::Open) openedAt = world.getSimulationTick();
					if (!closingAt && state.phase == "Entry closing") closingAt = world.getSimulationTick();
					opened = opened || state.doors[side] == core::DoorSnapshotState::Open;
					closed = closed || (opened && state.phase == "Idle");
					require(!state.occupant && state.reservations.empty() && state.scanProgress == 0
						&& state.remainingSeconds == 0 && state.doors[1 - entry] == core::DoorSnapshotState::Closed,
						"Empty presence opening scanned or violated interlock");
				}
				require(opened == (entryApproach && gap <= sensor) && (!opened || closed), "Entry/exit sensor boundary or empty opening timeout incorrect");
				if (opened)
				{
					auto hold = core::secondsToTicks(CORE_BULKHEAD_DOOR_STAY_OPEN_TIME, world.getFixedTimestep());
					require(closingAt >= openedAt + hold && closingAt <= openedAt + hold + 1,
						"Empty opening did not use existing Door-open timeout");
				}
				auto snapshot = world.getSimulationSnapshot();
				require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
					&& snapshot.securityScanners.at(0).phase == "Idle", "Passing Agent retained stale opening claims");
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

	void configuration(smoke::Context const&)
	{
		for (float pre : { 0.0f, 10.0f }) for (float scan : { 0.1f, 10.0f }) for (float post : { 0.0f, 10.0f })
		{
			core::World world("Configured scan", 12, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
			auto right = world.addCorridor(0, 0, 5, 3, 1);
			auto index = world.addSecurityScanner(0, 0, 3, 2);
			auto target = world.addSectorMarker(right, 0, 1.5f);
			auto destination = markerId(target);
			world.finishBuild(); world.pauseSimulation();
			require(world.setSecurityScannerConfiguration(index, 0.5f, pre, scan, post), "Timing endpoint refused");
			world.markSaved();
			for (float bad : { -1.0f, 11.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				require(!world.setSecurityScannerConfiguration(index, 0.5f, bad, scan, post)
					&& !world.setSecurityScannerConfiguration(index, 0.5f, pre, bad, post)
					&& !world.setSecurityScannerConfiguration(index, 0.5f, pre, scan, bad), "Invalid timing accepted");
				if (bad < 0 || !std::isfinite(bad)) require(!world.setSecurityScannerConfiguration(index, bad, pre, scan, post), "Invalid sensor accepted");
			}
			require(!world.setSecurityScannerConfiguration(index, 0.5f, pre, 0.099f, post) && !world.isModified(), "Rejected edit mutated document");
			auto actor = world.createAgent("Traveller", left, 0, 2.5f);
			auto entity = world.lookupAgent(actor).entity;
			auto path = world.getGraph()->calculatePath(entity, world.getGraph()->getVertexForObject(target.sector->getObject(target.index)));
			core::RouteDecisionContext remote{ entity, {}, {}, nullptr, entity->getWalkSpeed(), &world };
			float estimated = 0;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getType() == core::EdgeType::BulkheadDoor)
				{
					auto facts = node.edge->getDirectedTraversalFacts(node.targetVertex, remote);
					estimated += facts.components.expectedWaitSeconds;
					require(facts.components.interactionUnits == 0, "Configured route retained button assumptions");
				}
			require(std::abs(estimated - (3 * CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME + pre + scan + post)) < 0.0001f,
				"Route estimate ignored authored timings");
			require(world.moveAgentToMarker(actor, destination).accepted() && world.resumeSimulation(), "Configured journey refused");
			require(!world.setSecurityScannerConfiguration(index, 1, 1, 1, 1), "Running edit accepted");
			std::map<std::string, uint64_t> starts;
			bool edited = false;
			for (unsigned tick = 0; tick < 5000; ++tick)
			{
				world.advanceTick(); safeCapacity(world, index);
				auto state = world.getSimulationSnapshot().securityScanners.at(0);
				starts.emplace(state.phase, world.getSimulationTick());
				if (state.phase == "Scanning")
				{
					auto duration = core::secondsToTicks(scan, world.getFixedTimestep()) * world.getFixedTimestep();
					require(std::abs(state.scanProgress - (1 - state.remainingSeconds / duration)) < 0.0001f, "Configured progress incorrect");
					if (!edited)
					{
						edited = true; world.pauseSimulation();
						require(world.setSecurityScannerConfiguration(index, 100, 1, 2, 1), "Paused active edit refused");
						world.advanceTicks(100);
						auto frozen = world.getSimulationSnapshot().securityScanners.at(0);
						require(frozen.remainingSeconds == state.remainingSeconds && frozen.scanProgress == state.scanProgress, "Paused edit changed active clock");
						require(world.resumeSimulation(), "Configured resume refused");
					}
				}
				if (starts.count("Exit opening")) break;
			}
			require(edited && starts.count("Exit opening"), "Configured scan stalled");
			if (pre) require(starts.at("Scanning") - starts.at("Pre-delay") == core::secondsToTicks(pre, world.getFixedTimestep()), "Pre-delay finished early");
			else require(!starts.count("Pre-delay"), "Zero pre-delay added pause");
			auto scanEnd = post ? starts.at("Post-pause") : starts.at("Exit opening");
			require(scanEnd - starts.at("Scanning") == core::secondsToTicks(scan, world.getFixedTimestep()), "Scan finished early");
			if (post) require(starts.at("Exit opening") - scanEnd == core::secondsToTicks(post, world.getFixedTimestep()), "Post-pause finished early");
			else require(!starts.count("Post-pause"), "Zero post-pause added delay");
			bool idle = false;
			for (unsigned tick = 0; tick < 2000; ++tick)
			{
				world.advanceTick();
				if (world.getSimulationSnapshot().securityScanners.at(0).phase == "Idle") { idle = true; break; }
			}
			require(idle, "First configured journey did not exit");
			auto next = world.createAgent("Next scan", left, 0, 2.5f);
			require(world.moveAgentToMarker(next, destination).accepted(), "Next scan command refused");
			starts.clear();
			for (unsigned tick = 0; tick < 2000; ++tick)
			{
				world.advanceTick();
				auto state = world.getSimulationSnapshot().securityScanners.at(0);
				starts.emplace(state.phase, world.getSimulationTick());
				if (state.phase == "Exit opening") break;
			}
			require(starts.count("Exit opening") && starts.at("Scanning") - starts.at("Pre-delay") == core::secondsToTicks(1, world.getFixedTimestep())
				&& starts.at("Post-pause") - starts.at("Scanning") == core::secondsToTicks(2, world.getFixedTimestep())
				&& starts.at("Exit opening") - starts.at("Post-pause") == core::secondsToTicks(1, world.getFixedTimestep()), "Next scan did not use edited timings");
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
	checks.push_back({ "securityScanners/editSafety", editSafety });
	checks.push_back({ "securityScanners/committedInterruptions", interruptions });
	checks.push_back({ "securityScanners/admissionAuthorizationChanges", admissionAuthorization });
	checks.push_back({ "securityScanners/localRouteObservations", routeObservations });
	checks.push_back({ "securityScanners/automaticJourneys", journeys });
	checks.push_back({ "securityScanners/configuration", configuration });
	checks.push_back({ "securityScanners/contentionAndReuse", contention });
	checks.push_back({ "securityScanners/abandonedAdmission", abandonment });
	checks.push_back({ "securityScanners/defensiveOccupancy", occupancyViolation });
	checks.push_back({ "securityScanners/presenceAndEmptyTimeout", sensing });
	checks.push_back({ "securityScanners/resetAndLoad", restoration });
	checks.push_back({ "securityScanners/destinationAndMobilityGates", permissions });
}
