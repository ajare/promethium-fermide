#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/World.h"

namespace core
{
	class Door;
	class ForceBridge;
	class Ladder;
	class Lift;
	class Object;
	class Shuttle;
	class Stairwell;
	class Window;

	// Owns the traversal-resource record behaviour on behalf of the
	// World that owns it (ADR 0022).
	//
	// A traversal resource is the World's registry entry for a physical
	// opening or vehicle that Agents cross: door, window, ladder, lift,
	// open-platform lift, shuttle, stairwell, or force bridge. The
	// registry owns the record family: validated creation with its
	// geometry/capacity/timing derivation, lookup by handle, resolution
	// from a physical object to the resource that owns its agent queue,
	// guarded removal, the lifecycle-event snapshot projection, the
	// Agent-ownership queries and releases keyed to resources, and the
	// waiting/geometry policy validation and upkeep.
	//
	// The registry owns no state: `mTraversalResources` and both policy
	// structs stay World-owned (ADR 0001); the registry reaches them,
	// the pause/structural-edit gates, and the event log through the
	// World it was given, following the SimulationCoordinator,
	// PlacementValidator, AgentGroupRegistry, PermissionRequirementStore,
	// AgentTypeCatalog, and MarkerStore pattern.
	//
	// This is the resource registry, not the admission/queue logic: the
	// traversal scheduling itself - request creation, queue tickets,
	// grants, permits, and the lift dispatch loop - stays in
	// SimulationCoordinator (ADR 0004). The ownership and snapshot
	// operations here are reached through the coordinator where their
	// implementation is coupled to that scheduling (cancelling requests,
	// releasing permits, lift stop-request upkeep); the registry is the
	// facade seam through which World exposes them, not a second
	// implementation.
	//
	// Every creation is judged before anything is written, so a refusal
	// leaves the World exactly as it was found. WorldSerialization.cpp
	// and construction-record replay are untouched - behaviour moved
	// out, state did not.
	//
	// World keeps ownership of every entity registry (ADR 0001) and
	// remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class.
	class TraversalResourceRegistry
	{
	public:

		explicit TraversalResourceRegistry(World& world);

		TraversalResourceRegistry(TraversalResourceRegistry const&) = delete;
		TraversalResourceRegistry& operator=(TraversalResourceRegistry const&) = delete;

		virtual ~TraversalResourceRegistry() = default;

		// ------------------------------------------------------------------
		// Creation
		// ------------------------------------------------------------------

		// A bare resource with no physical object: the generic seam.
		TraversalResourceId createTraversalResource(std::string const& name);

		TraversalResourceId createDoorTraversalResource(std::string const& name,
			std::shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds);

		TraversalResourceId createWindowTraversalResource(std::string const& name,
			std::shared_ptr<Window> window);

		TraversalResourceId createLadderTraversalResource(std::string const& name,
			std::shared_ptr<Ladder> ladder, SectorId ladderSector,
			uint32_t directionalBatchLimit);

		TraversalResourceId createLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId liftSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createOpenPlatformLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId locationSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1,
			float stopDurationSeconds = CORE_PLATFORM_LIFT_STOP_DURATION);

		TraversalResourceId createShuttleTraversalResource(std::string const& name,
			std::shared_ptr<Shuttle> shuttle, SectorId shuttleSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createForceBridgeTraversalResource(std::string const& name,
			std::shared_ptr<ForceBridge> forceBridge);

		TraversalResourceId createStairwellTraversalResource(std::string const& name,
			std::shared_ptr<Stairwell> stairwell, SectorId stairwellSector,
			uint32_t capacity, uint32_t directionalBatchLimit);

		// ------------------------------------------------------------------
		// Identity and lookup
		// ------------------------------------------------------------------

		EntityLookup<TraversalResource> lookupTraversalResource(TraversalResourceId id);

		EntityLookup<TraversalResource const> lookupTraversalResource(TraversalResourceId id) const;

		// Resolves physical geometry to the resource that owns its agent
		// queue. Transport landing doors resolve to their vehicle coordinator.
		TraversalResourceId getTraversalResourceId(Object const* object) const;

		// ------------------------------------------------------------------
		// Removal
		// ------------------------------------------------------------------

		// Refused while the resource still holds active ownership: an
		// occupied or queued resource cannot be replaced safely.
		EntityRemovalResult removeTraversalResource(TraversalResourceId id);

		// ------------------------------------------------------------------
		// Ownership and snapshots
		// ------------------------------------------------------------------

		// The lifecycle-event projection of a resource. Implemented by the
		// coordinator, which owns the snapshot machinery (ADR 0004).
		TraversalResourceSnapshot makeTraversalResourceSnapshot(TraversalResourceId id,
			TraversalResource const& resource) const;

		// True while the Agent holds any traversal claim: a request, a
		// permit, or a resource-side slot, lease, or manifest entry.
		bool holdsTraversalOwnership(AgentId id) const;

		// Surrender one Agent's claims on one resource, keeping mirrored
		// aliases consistent with whatever occupancy remains.
		void releaseAgentFromResource(TraversalResource& resource, AgentId id);

		// Surrender every traversal claim an Agent holds across the World.
		void releaseTraversalOwnership(AgentId id);

		// ------------------------------------------------------------------
		// Policies
		// ------------------------------------------------------------------

		TraversalWaitingPolicy const& getTraversalWaitingPolicy() const;

		void setTraversalWaitingPolicy(TraversalWaitingPolicy policy);

		TraversalGeometryPolicy const& getTraversalGeometryPolicy() const;

		// Validated, then propagated: existing enclosed lift cars and
		// their car-side boarding lanes are re-packed to the new clearance.
		void setTraversalGeometryPolicy(TraversalGeometryPolicy policy);

	private:

		// The registry owns nothing: it reads and edits the World's
		// traversal-resource state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
