#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/World.h"

namespace core
{
	// Owns the behaviour of the World's authored `*PermissionRequirement`
	// get/set surface on behalf of the World that owns it (ADR 0022).
	//
	// Permission requirements are authored World data, not simulation state:
	// which Access permissions an interaction point, a transport destination
	// stop, a Location, or a manual ordinary Door demands. The state itself -
	// the pending requirement map, the authored control-requirement index,
	// the construction records, and the entity registries - stays with the
	// World (ADR 0001). This collaborator moves only the behaviour: the
	// pause gate, eligibility judging, validation, the edit of the live
	// requirement together with its authored construction-record mirror, and
	// the route reconsiderations an authorization change triggers.
	//
	// Every operation is judged before anything is written, so a refusal
	// leaves the World exactly as it was found. WorldSerialization.cpp and
	// construction-record replay are untouched: behaviour moved out, state
	// did not.
	//
	// World keeps ownership of every entity registry (ADR 0001) and remains
	// the facade (design pattern) through which callers reach these
	// operations; no caller outside World names this class. The store owns
	// no state: it reaches the World's registries, the pause gate, and the
	// authorization replanning seams through the World it was given,
	// following the SimulationCoordinator, PlacementValidator, and
	// AgentGroupRegistry pattern.
	class PermissionRequirementStore
	{
	public:

		explicit PermissionRequirementStore(World& world);

		PermissionRequirementStore(PermissionRequirementStore const&) = delete;
		PermissionRequirementStore& operator=(PermissionRequirementStore const&) = delete;

		virtual ~PermissionRequirementStore() = default;

		// ------------------------------------------------------------------
		// Static Location requirements (#273)
		// ------------------------------------------------------------------

		// Only Rooms and Corridors carry a Location permission requirement.
		bool isLocationEligible(uint32_t sectorIndex) const;

		// The Location's required Access permissions; empty means
		// unrestricted. Throws if the sector is missing or not a Location.
		std::vector<AccessPermissionId> locationRequirement(uint32_t sectorIndex) const;

		// Paused-only edit. Validates every permission and rejects
		// duplicates before writing; a refusal changes nothing. On a real
		// change the live Location, its authored construction record, and
		// any Agent whose path enters the Location now without the
		// permission are all updated together.
		bool setLocationRequirement(uint32_t sectorIndex,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Transport destination requirements (historical Lift names)
		// ------------------------------------------------------------------

		// The permissions a destination stop requires; empty means
		// unrestricted. Throws if the stop is unknown. Omit objectIndex for
		// a Lift or Shuttle; specify the Room object for a Platform lift.
		std::vector<AccessPermissionId> destinationRequirement(uint32_t sectorIndex,
			uint32_t stopIndex, uint32_t objectIndex = ~0u) const;

		// Paused-only edit. Permissions are validated and sorted before
		// writing. On a real change, Agents that just lost a grant for a
		// newly required permission are reconsidered, and Agents for a
		// dropped requirement get voluntary route planning.
		bool setDestinationRequirement(uint32_t sectorIndex, uint32_t stopIndex,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr, uint32_t objectIndex = ~0u);

		// ------------------------------------------------------------------
		// Interaction-point requirements
		// ------------------------------------------------------------------

		// A point is eligible unless it is missing, sector-less, owned by an
		// access panel, or bound to a Lift/Shuttle destination-select
		// command (those carry their requirement on the destination stop).
		bool isInteractionPointEligible(InteractionPointId point) const;

		// The point's required Access permissions; empty means
		// unrestricted. Throws if the point is unknown.
		std::vector<AccessPermissionId> interactionPointRequirement(
			InteractionPointId point) const;

		// Paused-only edit. On a real change the live point requirement is
		// written and mirrored to the authored construction record of every
		// control family that owns the point (Booth window panel, Location
		// light switch, Dumbwaiter landing button, generated door/bridge/
		// airlock/ladder controls, and the landing-control index), so a
		// replay restores the same requirement. Agents routed through
		// controls using the point are replanned for the changed gate.
		bool setInteractionPointRequirement(InteractionPointId point,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Manual ordinary Door requirements
		// ------------------------------------------------------------------

		// Ordinary Doors only: a manual, buttonless ordinary Door (no
		// bulkhead, lift, or shuttle) may carry a requirement.
		bool isManualDoorEligible(TraversalResourceId door) const;

		// The Door's required Access permissions; empty means
		// unrestricted. Throws if the resource is not a Door.
		std::vector<AccessPermissionId> manualDoorRequirement(
			TraversalResourceId door) const;

		// Paused-only edit. The authored Door construction record and the
		// live Door requirement are updated together, and affected Agents
		// are replanned. Authored edits are paused and undoable; live edits
		// never dirty the document or change its initial condition.
		bool setManualDoorRequirement(TraversalResourceId door,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr);

	private:

		// The authored record kind is World's private nested vocabulary; the
		// store's friendship (ADR 0022) makes the alias legal here.
		using ConstructionType = World::ConstructionType;

		// The authored record backing a Location's permission requirement,
		// or nullptr when the sector is not a Room/Corridor record.
		World::ConstructionRecord const* findLocationPermissionRecord(
			uint32_t sectorIndex) const;

		// The store owns nothing: it reads and edits the World's registries
		// and authored state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
