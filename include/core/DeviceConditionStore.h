#pragma once

#include <cstdint>
#include <optional>

#include "core/Coordination.h"
#include "core/DeviceCondition.h"
#include "core/World.h"

namespace core
{
	class Agent;
	class Sector;

	// Owns the authored-vs-runtime Broken behaviour family on behalf of the
	// World that owns it (ADR 0022).
	//
	// A device carries two condition facts: the authored `initiallyBroken`
	// flag mirrored on its construction record, and the live `mBroken` flag
	// the simulation freezes it with. This collaborator moves the behaviour
	// around both: the runtime `set*Broken` setters across ordinary and
	// Bulkhead Doors, Escalators, Lifts, Shuttles, and extensible
	// Ladders/Force Bridges; the paused-only `set*InitiallyBroken` edits
	// that move the authored construction-record mirror and the live device
	// together; the Door sensor-observation and speed-override surfaces; and
	// the read-side queries - whether a control's target device is Broken,
	// and what an Agent can currently know of a transport's condition.
	//
	// The state itself - the traversal-resource registry, the door/lift/
	// shuttle/extensible entities carrying `mBroken`/`mInitiallyBroken`,
	// the construction records, and the sensor-observation maps - stays
	// with the World (ADR 0001). WorldSerialization.cpp and construction-
	// record replay are untouched: behaviour moved out, state did not.
	//
	// Device *operations* stay in SimulationCoordinator (ADR 0004): when a
	// device breaks, the store asks the coordinator to fail the pending
	// operations that target it, rather than reaching into the operations
	// registry itself.
	//
	// World keeps ownership of every entity registry (ADR 0001) and remains
	// the facade (design pattern) through which callers reach these
	// operations; no caller outside World names this class. The store owns
	// no state: it reaches the World's registries, the pause gate, the
	// snapshot invalidation, and the coordinator's operation-failure seam
	// through the World it was given, following the SimulationCoordinator,
	// PlacementValidator, and PermissionRequirementStore pattern.
	class DeviceConditionStore
	{
	public:

		explicit DeviceConditionStore(World& world);

		DeviceConditionStore(DeviceConditionStore const&) = delete;
		DeviceConditionStore& operator=(DeviceConditionStore const&) = delete;

		virtual ~DeviceConditionStore() = default;

		// ------------------------------------------------------------------
		// Escalator condition
		// ------------------------------------------------------------------

		// Live runtime edit: freezes or restores an Escalator in place.
		// Existing admitted movement keeps its position and target; only
		// fresh local observations change route intent or remote Agent
		// knowledge.
		bool setEscalatorBroken(uint32_t sectorIndex, bool broken);

		// Paused-only authored edit: moves the authored construction-record
		// `initiallyBroken` mirror and the live Escalator together.
		bool setEscalatorInitiallyBroken(uint32_t sectorIndex, bool broken);

		// ------------------------------------------------------------------
		// Lift and Shuttle condition
		// ------------------------------------------------------------------

		// Live runtime edit. Vehicle-wide: every supported landing Door of
		// the transport fails and recovers with the vehicle, since owned
		// Doors cannot fail independently.
		bool setLiftBroken(TraversalResourceId lift, bool broken);
		bool setShuttleBroken(TraversalResourceId shuttle, bool broken);

		// Paused-only authored edits: the transport's construction-record
		// `initiallyBroken` mirror and the live vehicle move together.
		bool setLiftInitiallyBroken(TraversalResourceId lift, bool broken);
		bool setShuttleInitiallyBroken(TraversalResourceId shuttle, bool broken);

		// ------------------------------------------------------------------
		// Door condition
		// ------------------------------------------------------------------

		// Live runtime edit for a breakable (ordinary or standalone
		// Bulkhead) Door. Breaking fails any pending or running OpenDoor
		// operation targeting the Door; Agents replan from observation,
		// not from this edit.
		bool setDoorBroken(TraversalResourceId door, bool broken);

		// Paused-only authored edit. Authoring a broken Door also freezes
		// it at its authored broken-open position, rather than whatever
		// fraction it happened to hold live.
		bool setDoorInitiallyBroken(TraversalResourceId door, bool broken);

		// ------------------------------------------------------------------
		// Extensible (Ladder / Force Bridge) condition
		// ------------------------------------------------------------------

		// Live runtime edit. A pending safe retract is retained, but
		// Broken fully extended equipment is usable floor/climbing space
		// until restoration resumes that operation.
		bool setExtensibleBroken(TraversalResourceId resource, bool broken);

		// Paused-only authored edit: the Ladder/Force-Bridge
		// construction-record `initiallyBroken` mirror and the live device
		// move together.
		bool setExtensibleInitiallyBroken(TraversalResourceId resource, bool broken);

		// ------------------------------------------------------------------
		// Door sensor and speed surfaces
		// ------------------------------------------------------------------

		// Sensors report facts; the observation is recorded on the
		// World-owned resource's sensor map (Clear erases it).
		bool setDoorSensorObservation(TraversalResourceId resource, DoorSensorId sensor,
			DoorSensorObservation observation);

		// Paused-only per-instance speed override. Standalone Doors mirror
		// onto their own construction record; transport Landing Doors
		// mirror into the owning Lift/Shuttle record's packed
		// `transportDoorSpeeds` slot.
		bool setDoorSpeedOverride(TraversalResourceId resource, std::optional<float> speed);

		// ------------------------------------------------------------------
		// Condition queries
		// ------------------------------------------------------------------

		// Whether any object an interaction point's commands target (a
		// Door, extensible Ladder/Force Bridge, Lift, or Shuttle) is
		// Broken. The Button rendering uses this to tint a controlling
		// Button red instead of green.
		bool isControlTargetBroken(InteractionPointId point) const;

		// What `agent`, observing from `observationSector`, currently
		// knows of the transport's condition: a live reading when the
		// vehicle (or the observed landing) is visible from that sector,
		// otherwise the Agent's remembered condition. nullopt when the
		// resource is not a transport or nothing is knowable.
		std::optional<DeviceCondition> knownTransportCondition(TraversalResourceId resource,
			Agent const* agent, Sector const* observationSector) const;

	private:

		// The authored record kind is World's private nested vocabulary; the
		// store's friendship (ADR 0022) makes the alias legal here.
		using ConstructionType = World::ConstructionType;

		// Vehicle-wide Broken propagation shared by Lift and Shuttle: sets
		// the vehicle condition, mirrors it onto every supported landing
		// Door, and (on failure) asks the coordinator to fail the pending
		// operations and selector requests riding on the vehicle.
		bool setTransportBroken(TraversalResourceId transport, bool broken);

		// The store owns nothing: it reads and edits the World's registries
		// and authored state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
