#pragma once
#include "core/Simulation.h"
namespace core {
inline std::string metricName(AgentPathState value) { switch(value) {
case AgentPathState::Idle: return "idle";
case AgentPathState::RoutePlanning: return "route_planning";
case AgentPathState::MovingToVertex: return "moving_to_vertex";
case AgentPathState::WaitingForTraversal: return "waiting_for_traversal";
case AgentPathState::TraversingEdge: return "traversing_edge";
case AgentPathState::AwaitingTraversalCommit: return "awaiting_traversal_commit";
} return "unknown"; }
inline std::string metricName(DoorSnapshotState value) { switch(value) {
case DoorSnapshotState::NotADoor: return "not_a_door";
case DoorSnapshotState::Closed: return "closed";
case DoorSnapshotState::Opening: return "opening";
case DoorSnapshotState::Open: return "open";
case DoorSnapshotState::Closing: return "closing";
} return "unknown"; }
inline std::string metricName(LiftAgentState value) { switch(value) {
case LiftAgentState::QueuingAtDoor: return "queuing_at_door";
case LiftAgentState::Entering: return "entering";
case LiftAgentState::InLift: return "in_lift";
case LiftAgentState::Exiting: return "exiting";
} return "unknown"; }
inline std::string metricName(SimulationPhase value) { switch(value) {
case SimulationPhase::None: return "none";
case SimulationPhase::ResourceAdvancement: return "resource_advancement";
case SimulationPhase::IntentCollection: return "intent_collection";
case SimulationPhase::Allocation: return "allocation";
case SimulationPhase::Movement: return "movement";
case SimulationPhase::Commit: return "commit";
case SimulationPhase::CleanupAndEventPublication: return "cleanup_and_event_publication";
} return "unknown"; }
inline std::string metricName(MovementCommandStatus value) { switch(value) {
case MovementCommandStatus::Accepted: return "accepted";
case MovementCommandStatus::Superseded: return "superseded";
case MovementCommandStatus::UnavailableAction: return "unavailable_action";
case MovementCommandStatus::NoOp: return "no_op";
case MovementCommandStatus::UnknownAgent: return "unknown_agent";
case MovementCommandStatus::InactiveAgent: return "inactive_agent";
case MovementCommandStatus::UnknownMarker: return "unknown_marker";
case MovementCommandStatus::AgentBusy: return "agent_busy";
case MovementCommandStatus::TopologyUnavailable: return "topology_unavailable";
case MovementCommandStatus::BehaviourOwned: return "behaviour_owned";
case MovementCommandStatus::NoOccupiableSector: return "no_occupiable_sector";
} return "unknown"; }
inline std::string metricName(RouteLossReason value) { switch(value) {
case RouteLossReason::None: return "none";
case RouteLossReason::Unreachable: return "unreachable";
case RouteLossReason::TopologyChanged: return "topology_changed";
case RouteLossReason::DestinationRemoved: return "destination_removed";
} return "unknown"; }
inline std::string metricName(MovementCancellationReason value) { switch(value) {
case MovementCancellationReason::None: return "none";
case MovementCancellationReason::Explicit: return "explicit";
case MovementCancellationReason::Superseded: return "superseded";
case MovementCancellationReason::TargetDeleted: return "target_deleted";
} return "unknown"; }
inline std::string metricName(SimulationEventType value) { switch(value) {
case SimulationEventType::AgentAdded: return "agent_added";
case SimulationEventType::AgentChanged: return "agent_changed";
case SimulationEventType::PhaseCompleted: return "phase_completed";
case SimulationEventType::AgentRemoved: return "agent_removed";
case SimulationEventType::InteractionPointAdded: return "interaction_point_added";
case SimulationEventType::InteractionPointRemoved: return "interaction_point_removed";
case SimulationEventType::InteractionRequestAdded: return "interaction_request_added";
case SimulationEventType::InteractionRequestChanged: return "interaction_request_changed";
case SimulationEventType::InteractionRequestRemoved: return "interaction_request_removed";
case SimulationEventType::DeviceOperationAdded: return "device_operation_added";
case SimulationEventType::DeviceOperationChanged: return "device_operation_changed";
case SimulationEventType::DeviceOperationRemoved: return "device_operation_removed";
case SimulationEventType::TraversalResourceAdded: return "traversal_resource_added";
case SimulationEventType::TraversalResourceRemoved: return "traversal_resource_removed";
case SimulationEventType::TraversalRequestAdded: return "traversal_request_added";
case SimulationEventType::TraversalRequestChanged: return "traversal_request_changed";
case SimulationEventType::TraversalRequestRemoved: return "traversal_request_removed";
case SimulationEventType::TraversalPermitAdded: return "traversal_permit_added";
case SimulationEventType::TraversalPermitChanged: return "traversal_permit_changed";
case SimulationEventType::TraversalPermitRemoved: return "traversal_permit_removed";
case SimulationEventType::SimulationPaused: return "simulation_paused";
case SimulationEventType::TopologyRebuilt: return "topology_rebuilt";
case SimulationEventType::TopologyRebuildFailed: return "topology_rebuild_failed";
case SimulationEventType::SimulationResumed: return "simulation_resumed";
case SimulationEventType::DestinationReached: return "destination_reached";
case SimulationEventType::MovementCancelled: return "movement_cancelled";
case SimulationEventType::RouteLost: return "route_lost";
case SimulationEventType::AgentActivated: return "agent_activated";
case SimulationEventType::AgentDeactivated: return "agent_deactivated";
} return "unknown"; }
inline std::string metricName(TraversalDirection value) { switch(value) {
case TraversalDirection::None: return "none";
case TraversalDirection::Ascending: return "ascending";
case TraversalDirection::Descending: return "descending";
} return "unknown"; }
inline std::string metricName(LiftStopPhase value) { switch(value) {
case LiftStopPhase::Idle: return "idle";
case LiftStopPhase::Moving: return "moving";
case LiftStopPhase::Opening: return "opening";
case LiftStopPhase::Disembarking: return "disembarking";
case LiftStopPhase::Boarding: return "boarding";
case LiftStopPhase::Closing: return "closing";
} return "unknown"; }
inline std::string metricName(DoorActivationMode value) { switch(value) {
case DoorActivationMode::Automatic: return "automatic";
case DoorActivationMode::Manual: return "manual";
case DoorActivationMode::RemoteControlled: return "remote_controlled";
case DoorActivationMode::Unavailable: return "unavailable";
} return "unknown"; }
inline std::string metricName(DeviceCommandType value) { switch(value) {
case DeviceCommandType::SetSectorLights: return "set_sector_lights";
case DeviceCommandType::OpenDoor: return "open_door";
case DeviceCommandType::SetExtendedState: return "set_extended_state";
case DeviceCommandType::CallLift: return "call_lift";
case DeviceCommandType::SelectLiftDestination: return "select_lift_destination";
case DeviceCommandType::CallShuttle: return "call_shuttle";
case DeviceCommandType::SelectShuttleDestination: return "select_shuttle_destination";
case DeviceCommandType::RequestAirlock: return "request_airlock";
case DeviceCommandType::SetBoothWindowState: return "set_booth_window_state";
case DeviceCommandType::ToggleBoothWindow: return "toggle_booth_window";
case DeviceCommandType::PressDumbwaiterLanding: return "press_dumbwaiter_landing";
case DeviceCommandType::SetAccessPanelState: return "set_access_panel_state";
} return "unknown"; }
inline std::string metricName(DoorOpenLeaseKind value) { switch(value) {
case DoorOpenLeaseKind::Preparation: return "preparation";
case DoorOpenLeaseKind::Crossing: return "crossing";
case DoorOpenLeaseKind::ExternalHoldOpen: return "external_hold_open";
} return "unknown"; }
inline std::string metricName(DoorSensorObservation value) { switch(value) {
case DoorSensorObservation::Clear: return "clear";
case DoorSensorObservation::Presence: return "presence";
case DoorSensorObservation::Obstruction: return "obstruction";
} return "unknown"; }
inline std::string metricName(InteractionBindingRequirement value) { switch(value) {
case InteractionBindingRequirement::Required: return "required";
case InteractionBindingRequirement::BestEffort: return "best_effort";
} return "unknown"; }
inline std::string metricName(InteractionResult value) { switch(value) {
case InteractionResult::Pending: return "pending";
case InteractionResult::Succeeded: return "succeeded";
case InteractionResult::SucceededWithBestEffortFailure: return "succeeded_with_best_effort_failure";
case InteractionResult::Failed: return "failed";
case InteractionResult::Rejected: return "rejected";
case InteractionResult::Cancelled: return "cancelled";
} return "unknown"; }
inline std::string metricName(DeviceOperationState value) { switch(value) {
case DeviceOperationState::Pending: return "pending";
case DeviceOperationState::Running: return "running";
case DeviceOperationState::Succeeded: return "succeeded";
case DeviceOperationState::Failed: return "failed";
case DeviceOperationState::Rejected: return "rejected";
case DeviceOperationState::Cancelled: return "cancelled";
} return "unknown"; }
inline std::string metricName(TraversalRequestState value) { switch(value) {
case TraversalRequestState::Pending: return "pending";
case TraversalRequestState::Granted: return "granted";
case TraversalRequestState::Denied: return "denied";
case TraversalRequestState::Cancelled: return "cancelled";
case TraversalRequestState::Committed: return "committed";
} return "unknown"; }
inline std::string metricName(TraversalFailureReason value) { switch(value) {
case TraversalFailureReason::None: return "none";
case TraversalFailureReason::NoReachableControl: return "no_reachable_control";
case TraversalFailureReason::ControlRejected: return "control_rejected";
case TraversalFailureReason::PreparationFailed: return "preparation_failed";
case TraversalFailureReason::ResourceDisabled: return "resource_disabled";
case TraversalFailureReason::LocalGoalUnreachable: return "local_goal_unreachable";
case TraversalFailureReason::PermitExpired: return "permit_expired";
} return "unknown"; }
inline std::string metricName(TraversalPermitState value) { switch(value) {
case TraversalPermitState::Active: return "active";
case TraversalPermitState::Committed: return "committed";
case TraversalPermitState::Cancelled: return "cancelled";
} return "unknown"; }
inline std::string metricName(EdgeType value) { switch(value) {
case EdgeType::Location: return "location";
case EdgeType::Door: return "door";
case EdgeType::BulkheadDoor: return "bulkhead_door";
case EdgeType::Window: return "window";
case EdgeType::Gap: return "gap";
case EdgeType::ForceBridge: return "force_bridge";
case EdgeType::LadderMount: return "ladder_mount";
case EdgeType::Ladder: return "ladder";
case EdgeType::Lift: return "lift";
case EdgeType::LiftMount: return "lift_mount";
case EdgeType::Shuttle: return "shuttle";
case EdgeType::ShuttleMount: return "shuttle_mount";
case EdgeType::Stairwell: return "stairwell";
case EdgeType::StairwellMount: return "stairwell_mount";
case EdgeType::Staircase: return "staircase";
case EdgeType::StaircaseMount: return "staircase_mount";
} return "unknown"; }
}
