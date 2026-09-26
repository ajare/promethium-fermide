#include "core/SimulationMetricsCollector.h"
#include "core/World.h"
#include "core/Sector.h"
#include "core/Staircase.h"
#include "core/SectorObject.h"
#include <cctype>
#include "MetricNames.h"
#include <algorithm>
#include <stdexcept>

namespace core {
void MetricsRegistry::add(std::string const& name, MetricType type, MetricLabels labels, double value) {
    auto it = families.find(name);
    if (it != families.end() && it->second.type != type) throw std::invalid_argument("Metric family type conflict");
    size_t size = 0;
    for (auto const& entry : families) size += entry.second.samples.size() * (entry.second.type == MetricType::Histogram ? 12 : 1);
    auto cost = type == MetricType::Histogram ? size_t{12} : size_t{1};
    if (size + cost > maxSeries && (it == families.end() || !it->second.samples.count(labels))) return;
    auto& family = families[name];
    family.type = type;
    family.help = name + " simulation metric";
    auto& sample = family.samples[labels];
    sample.labels = std::move(labels);
    sample.value += value;
}
void MetricsRegistry::observe(std::string const& name, MetricLabels labels, double seconds) {
    add(name, MetricType::Histogram, labels, seconds);
    auto it = families.find(name);
    if (it == families.end() || !it->second.samples.count(labels)) return;
    auto& sample = it->second.samples.at(labels);
    ++sample.count;
    for (double bound : {0.1, 0.5, 1., 2.5, 5., 10., 30., 60., 300.}) sample.buckets[bound] += seconds <= bound;
}
void MetricsRegistry::clearGauges() {
    for (auto it = families.begin(); it != families.end();) {
        if (it->second.type == MetricType::Gauge) it = families.erase(it); else ++it;
    }
}
SimulationMetricsCollector::SimulationMetricsCollector(World const& w, bool d) : world(w), detail(d) { refresh(); }
MetricsRegistry SimulationMetricsCollector::snapshot() const { std::lock_guard lock(mutex); return registry; }
void SimulationMetricsCollector::refresh() noexcept {
    try { auto s = world.getSimulationSnapshot(); std::lock_guard lock(mutex); collect(s, {}); } catch (...) { /* Observation must never stop simulation. */ }
}
void SimulationMetricsCollector::onTick(uint64_t, std::vector<SimulationEvent> const& events) noexcept {
    try {
        auto s = world.getSimulationSnapshot();
        std::lock_guard lock(mutex);
        registry.add("pf_simulation_ticks_total", MetricType::Counter, {}, 1);
        collect(s, events);
    } catch (...) { /* Fail soft, including allocation failure. */ }
}
void SimulationMetricsCollector::collect(SimulationSnapshot const& s, std::vector<SimulationEvent> const& events) {
    registry.clearGauges();
    auto gauge = [&](std::string name, double value, MetricLabels labels = {}) { registry.add("pf_" + name, MetricType::Gauge, std::move(labels), value); };
    auto counter = [&](std::string name, MetricLabels labels = {}, double value = 1) { registry.add("pf_" + name + "_total", MetricType::Counter, std::move(labels), value); };
    gauge("simulation_tick", static_cast<double>(s.tick));
    gauge("simulation_paused", s.paused);
    gauge("simulation_agents", static_cast<double>(s.agents.size()));
    gauge("build_info", 1, {{"version", PF_BUILD_VERSION}, {"config",
#ifdef NDEBUG
        "release"
#else
        "debug"
#endif
    }});
    for (auto state : {AgentPathState::Idle, AgentPathState::MovingToVertex, AgentPathState::WaitingForTraversal, AgentPathState::TraversingEdge, AgentPathState::AwaitingTraversalCommit}) gauge("agents", 0, {{"state", metricName(state)}});
    gauge("agents_active", 0); gauge("agents_inactive", 0);
    for (auto const& a : s.agents) { gauge("agents", 1, {{"state", metricName(a.state)}}); gauge(a.active ? "agents_active" : "agents_inactive", 1); }
    std::map<SectorId, size_t> occupants;
    for (auto const& a : s.agents) ++occupants[a.sectorId];
    for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer) for (auto const& sector : world.getSectors(layer)) {
        auto type = getSectorTypeString(sector->getType());
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto count = static_cast<double>(occupants[SectorId{uint64_t(sector->getIndex()) + 1}]);
        gauge("sectors", 1, {{"type", type}}); gauge("sector_occupants", count, {{"type", type}});
        if (detail) gauge("sector_occupants_detail", count, {{"type", type}, {"sector", sector->getName()}});
    }
    gauge("interaction_points", static_cast<double>(s.interactionPoints.size()));
    for (auto result : {InteractionResult::Pending, InteractionResult::Succeeded, InteractionResult::SucceededWithBestEffortFailure, InteractionResult::Failed, InteractionResult::Rejected, InteractionResult::Cancelled}) gauge("interaction_requests", 0, {{"result", metricName(result)}});
    if (detail) for (auto const& point : s.interactionPoints) gauge("interaction_queue_length", 0, {{"point", point.name}});
    for (auto const& r : s.interactionRequests) {
        gauge("interaction_requests", 1, {{"result", metricName(r.result)}});
        if (detail && r.result == InteractionResult::Pending) {
            auto p = std::find_if(s.interactionPoints.begin(), s.interactionPoints.end(), [&](auto const& point) { return point.id == r.point; });
            if (p != s.interactionPoints.end()) gauge("interaction_queue_length", 1, {{"point", p->name}});
        }
    }
    for (auto state : {DeviceOperationState::Pending, DeviceOperationState::Running, DeviceOperationState::Succeeded, DeviceOperationState::Failed, DeviceOperationState::Rejected, DeviceOperationState::Cancelled}) gauge("device_operations", 0, {{"state", metricName(state)}});
    for (auto const& o : s.deviceOperations) gauge("device_operations", 1, {{"state", metricName(o.state)}});
    gauge("traversal_permits_active", static_cast<double>(std::count_if(s.traversalPermits.begin(), s.traversalPermits.end(), [](auto const& p) { return p.state == TraversalPermitState::Active; })));
    gauge("traversal_requests", 0, {{"state", "pending"}}); gauge("traversal_requests", 0, {{"state", "granted"}});
    for (auto const& r : s.traversalRequests) if (r.state == TraversalRequestState::Pending || r.state == TraversalRequestState::Granted) gauge("traversal_requests", 1, {{"state", metricName(r.state)}});
    auto resourceType = [](TraversalResourceSnapshot const& r) -> std::string { return r.isOpenPlatformLift ? "platform_lift" : r.isShuttle ? "shuttle" : r.isLift ? "lift" : r.isLadder ? "ladder" : r.isForceBridge ? "force_bridge" : r.isWindow ? "window" : r.isDoor ? "door" : r.isNarrowStairwell ? "stairwell" : "other"; };
    for (auto const& e : events) {
        if (e.type == SimulationEventType::PhaseCompleted) counter("simulation_phase_completed", {{"phase", metricName(e.phase)}});
        switch (e.type) {
        case SimulationEventType::AgentAdded: counter("agent_events", {{"event", "added"}}); break;
        case SimulationEventType::AgentRemoved: counter("agent_events", {{"event", "removed"}}); break;
        case SimulationEventType::AgentActivated: counter("agent_events", {{"event", "activated"}}); break;
        case SimulationEventType::AgentDeactivated: counter("agent_events", {{"event", "deactivated"}}); break;
        case SimulationEventType::DestinationReached: counter("agent_events", {{"event", "destination_reached"}}); break;
        case SimulationEventType::MovementCancelled: counter("agent_events", {{"event", "movement_cancelled"}}); break;
        case SimulationEventType::RouteLost: counter("agent_route_loss", {{"reason", metricName(e.routeLossReason)}}); break;
        case SimulationEventType::InteractionRequestAdded:
        case SimulationEventType::InteractionRequestChanged:
            if (e.interactionRequest.result != InteractionResult::Pending) counter("agent_interactions", {{"result", metricName(e.interactionRequest.result)}});
            break;
        case SimulationEventType::DeviceOperationAdded:
            if (e.deviceOperation.hasCommand) counter("device_commands", {{"command", metricName(e.deviceOperation.command.type)}});
            break;
        case SimulationEventType::DeviceOperationChanged: counter("device_operation_transitions", {{"state", metricName(e.deviceOperation.state)}}); break;
        case SimulationEventType::TraversalRequestChanged: {
            auto const& r = e.traversalRequest;
            if (r.state != TraversalRequestState::Pending) counter("traversal_request_transitions", {{"state", metricName(r.state)}});
            auto resource = std::find_if(s.traversalResources.begin(), s.traversalResources.end(), [&](auto const& v) { return v.id == r.resource; });
            MetricLabels labels{{"resource_type", resource == s.traversalResources.end() ? "other" : resourceType(*resource)}};
            if (resource != s.traversalResources.end()) {
                labels["resource"] = resource->name;
                labels["resource_id"] = std::to_string(resource->id.value);
            }
            if (r.state == TraversalRequestState::Denied) counter("traversal_denials", {{"reason", metricName(r.failureReason)}});
            if (r.failureReason == TraversalFailureReason::PermitExpired) counter("traversal_permits_expired", labels);
            if (r.state == TraversalRequestState::Granted) {
                granted[r.id] = s.tick;
                if (resource != s.traversalResources.end() && resource->isLadder) counter("ladder_admissions", labels);
                auto resource = std::find_if(s.traversalResources.begin(), s.traversalResources.end(), [&](auto const& v) { return v.id == r.resource; });
                registry.observe("pf_agent_queue_wait_seconds", {{"resource_type", resource == s.traversalResources.end() ? "other" : resourceType(*resource)}}, static_cast<double>(s.tick - r.queuedAtTick) / 60.);
            }
            if (r.state == TraversalRequestState::Committed) {
                counter("agent_traversals", {{"edge_type", metricName(r.edgeType)}});
                auto edge = metricName(r.edgeType);
                if (edge == "door" || edge == "bulkhead_door" || edge == "window") counter("door_crossings", labels);
                if (r.edgeType == EdgeType::Staircase) {
                    std::shared_ptr<Staircase const> staircase;
                    std::string staircaseName;
                    for (auto id : {r.sourceSector, r.destinationSector}) if (id) {
                        auto sector = world.getSector(static_cast<uint32_t>(id.value - 1));
                        if (sector && sector->getType() == SectorType::Staircase) {
                            staircaseName = sector->getName();
                            for (uint32_t index = 0; index < sector->getNumObjects(); ++index) {
                                auto object = std::dynamic_pointer_cast<Staircase const>(sector->getObject(index)->_getObject());
                                if (object) staircase = object;
                            }
                        }
                    }
                    bool escalator = staircase && staircase->isEscalator();
                    labels["resource_type"] = escalator ? "escalator" : "staircase";
                    if (!staircaseName.empty()) labels["resource"] = staircaseName;
                    if (escalator) labels["direction"] = staircase->getSpeed() > 0 ? "ascending" : "descending";
                    counter(escalator ? "escalator_crossings" : "staircase_crossings", labels);
                }
                auto start = granted.find(r.id);
                if (start != granted.end()) registry.observe("pf_agent_traversal_seconds", {{"edge_type", metricName(r.edgeType)}}, static_cast<double>(s.tick - start->second) / 60.);
            }
            if (r.state != TraversalRequestState::Granted && r.state != TraversalRequestState::Pending) granted.erase(r.id);
            break;
        }
        case SimulationEventType::TraversalRequestRemoved: granted.erase(e.traversalRequest.id); break;
        default: break;
        }
    }
    std::map<DeviceOperationId, DeviceOperationState> nextOperations;
    for (auto const& operation : s.deviceOperations) {
        auto before = previousOperations.find(operation.id);
        auto seenOperation = [&](SimulationEventType type) { return std::any_of(events.begin(), events.end(), [&](auto const& e) { return e.type == type && e.deviceOperation.id == operation.id; }); };
        if (initialized && before == previousOperations.end() && operation.hasCommand && !seenOperation(SimulationEventType::DeviceOperationAdded)) counter("device_commands", {{"command", metricName(operation.command.type)}});
        if (before != previousOperations.end() && before->second != operation.state && !seenOperation(SimulationEventType::DeviceOperationChanged)) counter("device_operation_transitions", {{"state", metricName(operation.state)}});
        nextOperations.emplace(operation.id, operation.state);
    }
    previousOperations = std::move(nextOperations);
    // Authored lifecycle changes can occur between ticks, outside the event
    // slice. Reconcile those without counting an in-tick event twice.
    std::map<AgentId, AgentSnapshot> nextAgents;
    auto seen = [&](AgentId id, SimulationEventType type) { return std::any_of(events.begin(), events.end(), [&](auto const& e) { return e.type == type && e.agent.id == id; }); };
    for (auto const& a : s.agents) {
        if (initialized) {
            auto before = previousAgents.find(a.id);
            if (before == previousAgents.end() && !seen(a.id, SimulationEventType::AgentAdded)) counter("agent_events", {{"event", "added"}});
            if (before != previousAgents.end() && before->second.active != a.active && !seen(a.id, a.active ? SimulationEventType::AgentActivated : SimulationEventType::AgentDeactivated)) counter("agent_events", {{"event", a.active ? "activated" : "deactivated"}});
        }
        nextAgents.emplace(a.id, a);
    }
    for (auto const& [id, a] : previousAgents) if (!nextAgents.count(id) && !seen(id, SimulationEventType::AgentRemoved)) counter("agent_events", {{"event", "removed"}});
    previousAgents = std::move(nextAgents); initialized = true;
    std::map<MetricLabels, std::pair<double, size_t>> doorRatios;
    std::map<TraversalResourceId, TraversalResourceSnapshot> next;
    for (auto const& r : s.traversalResources) {
        MetricLabels labels{{"resource", r.name}, {"resource_id", std::to_string(r.id.value)}, {"resource_type", resourceType(r)}};
        auto g = [&](std::string const& name, double value) { gauge(name, value, labels); };
        auto hot = [&](std::string const& name, std::string const& key, std::string const& value) {
            std::vector<std::string> states;
            if (name == "door_state") states = {"not_a_door", "closed", "opening", "open", "closing"};
            else if (name == "door_activation") states = {"automatic", "manual", "remote_controlled", "unavailable"};
            else if (name == "lift_phase") states = {"idle", "moving", "opening", "disembarking", "boarding", "closing"};
            else states = {"none", "ascending", "descending"};
            for (auto const& state : states) { auto l = labels; l[key] = state; gauge(name, state == value, l); }
        };
        size_t queues = 0, positions = 0, lanes = 0, capacity = 0;
        for (size_t laneIndex = 0; laneIndex < r.queueLanes.size(); ++laneIndex) {
            auto const& lane = r.queueLanes[laneIndex]; queues += lane.queue.size();
            for (auto const& p : lane.positions) {
                positions += bool(p.owner);
                if (detail) { auto l = labels; l["lane"] = std::to_string(laneIndex); l["position"] = std::to_string(p.index); gauge("traversal_queue_position_occupied", bool(p.owner), l); }
            }
        }
        for (auto const& lane : r.crossingLanes) lanes += bool(lane.owner);
        for (auto const& p : r.capacityPositions) capacity += bool(p.occupant);
        g("traversal_queue_length", static_cast<double>(queues)); g("traversal_queue_positions_occupied", static_cast<double>(positions)); g("traversal_crossing_lanes_in_use", static_cast<double>(lanes)); g("traversal_capacity_positions_occupied", static_cast<double>(capacity));
        auto old = previous.find(r.id);
        auto duration = [&](std::string const& name, bool begin, bool end) {
            auto key = std::make_pair(r.id, name);
            if (begin) started.try_emplace(key, s.tick);
            auto it = started.find(key);
            if (end && it != started.end()) { registry.observe("pf_" + name + "_seconds", labels, double(s.tick - it->second) / 60.); started.erase(it); }
        };
        duration("door_open", old != previous.end() && old->second.doorState != DoorSnapshotState::Open && r.doorState == DoorSnapshotState::Open, r.doorState == DoorSnapshotState::Closed);
        duration("extensible_extension", old != previous.end() && !old->second.extended && r.extended, !r.extended);
        duration("lift_dwell", old != previous.end() && old->second.liftStopPhase != LiftStopPhase::Opening && r.liftStopPhase == LiftStopPhase::Opening, r.liftStopPhase == LiftStopPhase::Closing);
        if (r.isDoor || r.isWindow) {
            hot("door_state", "state", metricName(r.doorState));
            g("door_presence", r.presenceObserved); g("door_obstruction", r.obstructionObserved);
            g("door_queue_length", static_cast<double>(queues));
            hot("door_activation", "mode", metricName(r.doorActivationMode));
            doorRatios[labels].first += std::clamp(double(r.doorOpenPercentage), 0., 1.);
            ++doorRatios[labels].second;
            for (auto const& lease : {std::pair{"preparation", r.preparationLeaseCount}, {"crossing", r.crossingLeaseCount}, {"external_hold_open", r.externalOpenLeaseCount}}) { auto l = labels; l["kind"] = lease.first; gauge("door_open_leases", lease.second, l); }
            if (r.isWindow) g("window_normally_traversable", r.windowNormallyTraversable);
            if (old != previous.end() && old->second.doorState != r.doorState) { auto l = labels; l["transition"] = metricName(old->second.doorState) + "_to_" + metricName(r.doorState); counter("door_state_transitions", l); }
        }
        if (r.isLift || r.isShuttle || r.isOpenPlatformLift) {
            hot("lift_phase", "phase", metricName(r.liftStopPhase)); hot("lift_direction", "direction", metricName(r.liftDirection));
            g("lift_occupants", r.occupantCount); g("lift_capacity", r.capacity); g("lift_admission_queue_length", static_cast<double>(r.admissionQueue.size()));
            g("lift_accepting_boarders", r.liftAcceptingBoarders); g("lift_draining", r.liftDraining);
            g("lift_position", r.liftPosition); g("lift_current_stop", r.liftCurrentStop); g("lift_target_stop", r.liftTargetStop);
            if (r.isOpenPlatformLift) g("platform_lift_virtual_crossings", r.virtualBoundaryCrossingCount);
            if (old != previous.end()) {
                for (auto const& a : r.liftAgents) if (a.state == LiftAgentState::InLift && std::none_of(old->second.liftAgents.begin(), old->second.liftAgents.end(), [&](auto const& b) { return b.agent == a.agent && b.state == LiftAgentState::InLift; })) counter("lift_boardings", labels);
                for (auto const& a : old->second.liftAgents) if (a.state == LiftAgentState::InLift && std::any_of(r.liftAgents.begin(), r.liftAgents.end(), [&](auto const& b) { return b.agent == a.agent && b.state == LiftAgentState::Exiting; })) counter("lift_alightings", labels);
            }
            if (old != previous.end()) {
                if (old->second.liftStopPhase == LiftStopPhase::Moving && r.liftStopPhase == LiftStopPhase::Opening) counter("lift_trips", labels);
                if (old->second.liftStopPhase != LiftStopPhase::Closing && r.liftStopPhase == LiftStopPhase::Closing) counter("lift_stops_served", labels);
                for (auto const& a : r.liftAgents) {
                    auto key = std::make_pair(r.id, a.agent);
                    auto before = std::find_if(old->second.liftAgents.begin(), old->second.liftAgents.end(), [&](auto const& b) { return b.agent == a.agent; });
                    if (a.state == LiftAgentState::InLift && (before == old->second.liftAgents.end() || before->state != LiftAgentState::InLift)) boarded[key] = s.tick;
                    if (a.state == LiftAgentState::Exiting && boarded.count(key)) { registry.observe("pf_agent_transport_journey_seconds", {{"resource_type", resourceType(r)}}, double(s.tick - boarded.at(key)) / 60.); boarded.erase(key); }
                }
            }
            for (size_t stop = 0; stop < r.liftStopRequestOwnerCounts.size(); ++stop) { auto l = labels; l["stop"] = std::to_string(stop); gauge("lift_stop_request_owners", r.liftStopRequestOwnerCounts[stop], l); }
            for (auto const& zone : r.shuttleAccessZones) { auto l = labels; l["stop"] = std::to_string(zone.stopIndex); l["zone"] = std::to_string(zone.accessZoneIndex); gauge("shuttle_access_zone_queue_length", double(zone.queue.size()), l); }
            for (auto const& c : r.shuttleCarriages) {
                auto l = labels; l["carriage"] = std::to_string(c.index);
                gauge("shuttle_carriage_occupants", c.occupantCount, l); gauge("shuttle_carriage_capacity", c.capacity, l);
                if (old != previous.end()) {
                    auto before = std::find_if(old->second.shuttleCarriages.begin(), old->second.shuttleCarriages.end(), [&](auto const& carriage) { return carriage.index == c.index; });
                    if (before != old->second.shuttleCarriages.end()) {
                        for (auto const& p : c.positions) if (p.occupant && std::none_of(before->positions.begin(), before->positions.end(), [&](auto const& b) { return b.occupant == p.occupant; })) counter("shuttle_boardings", l);
                        for (auto const& p : before->positions) if (p.occupant && std::none_of(c.positions.begin(), c.positions.end(), [&](auto const& b) { return b.occupant == p.occupant; })) counter("shuttle_alightings", l);
                    }
                }
            }
        }
        if (r.isLadder) {
            g("ladder_occupants", r.occupantCount); g("ladder_capacity", r.capacity); g("ladder_admission_reservations", r.admissionReservationCount); g("ladder_admission_queue_length", static_cast<double>(r.admissionQueue.size()));
            hot("ladder_active_direction", "direction", metricName(r.activeDirection));
            auto waitingLabels = labels; waitingLabels["direction"] = "ascending"; gauge("ladder_waiting", r.ascendingWaitingCount, waitingLabels); waitingLabels["direction"] = "descending"; gauge("ladder_waiting", r.descendingWaitingCount, waitingLabels);
            g("ladder_batch_count", r.directionalBatchCount); g("ladder_batch_limit", r.directionalBatchLimit);
            if (old != previous.end() && old->second.activeDirection != TraversalDirection::None && r.activeDirection != TraversalDirection::None && old->second.activeDirection != r.activeDirection) counter("ladder_direction_reversals", labels);
        }
        if (r.isNarrowStairwell) { g("stairwell_occupants", r.occupantCount); g("stairwell_capacity", r.capacity); }
        if (r.isExtensible) { g("extensible_extended", r.extended); g("extensible_retraction_pending", r.retractionPending); g("extensible_request_leases", r.extensionRequestLeaseCount); g("extensible_occupant_leases", r.extensionOccupantLeaseCount); }
        next.emplace(r.id, r);
    }
    for (auto const& [labels, sum] : doorRatios) gauge("door_open_ratio", sum.first / double(sum.second), labels);
    // Aggregate ratios use summed numerators and denominators, never sums of
    // individual ratios (which could exceed one).
    for (auto const& prefix : {std::string("lift"), std::string("shuttle_carriage")}) {
        auto capacity = registry.families.find("pf_" + prefix + "_capacity");
        auto occupantsFamily = registry.families.find("pf_" + prefix + "_occupants");
        if (capacity != registry.families.end() && occupantsFamily != registry.families.end()) for (auto const& [labels, sample] : capacity->second.samples) {
            auto occupant = occupantsFamily->second.samples.find(labels);
            if (occupant != occupantsFamily->second.samples.end()) {
                gauge(prefix + "_utilization_ratio", sample.value ? std::clamp(occupant->second.value / sample.value, 0., 1.) : 0., labels);
            }
        }
    }
    // Prune timing state when resources or passengers disappear mid-operation.
    for (auto it = started.begin(); it != started.end();) { if (!next.count(it->first.first)) it = started.erase(it); else ++it; }
    for (auto it = boarded.begin(); it != boarded.end();) {
        auto r = next.find(it->first.first);
        if (r == next.end() || std::none_of(r->second.liftAgents.begin(), r->second.liftAgents.end(), [&](auto const& a) { return a.agent == it->first.second; })) it = boarded.erase(it); else ++it;
    }
    previous = std::move(next);
}
}
