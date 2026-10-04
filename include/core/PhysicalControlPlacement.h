#pragma once

#include <cstdint>
#include <vector>
#include "core/Defines.h"

namespace core::physicalControl
{
	// Authored data only. Runtime identities are deliberately absent. These keys
	// are available to future policy implementations, but legacy allocation does
	// not sort by them or change its history-preserving tie breaks.
	enum class OwnerType { Airlock, BulkheadDoor, Door, Dumbwaiter, ForceBridge,
		Ladder, Lift, LocationLightSwitch, PlatformLift, Shuttle };
	struct Geometry
	{
		uint32_t layer{ 0 }, x{ 0 }, baseLevel{ 0 }, width{ 1 }, height{ 1 };
	};
	struct Role
	{
		// Left/right or lower/upper, then authored Stop/doorway position.
		uint32_t order{ 0 }, x{ 0 }, level{ 0 };
	};
	struct Owner
	{
		OwnerType type{ OwnerType::Door };
		Geometry geometry;
		Geometry hostingLocation;
		Role role;
	};
	struct Candidate
	{
		uint32_t cellX{ 0 };
		// Cell registration is separate from the physical centre coordinate.
		int side{ CORE_SIDE_MIDDLE };
		int quarterOffset{ -1 }; // -1 preserves legacy left/middle/right (0/2/4).
		int64_t centreKey() const;
		float centreX() const;
		static Candidate explicitHost(uint32_t cellX, int quarterOffset, int registrationSide);
	};
	struct Demand
	{
		std::vector<Candidate> candidates;
		uint32_t defaultCandidate{ 0 }, currentCandidate{ 0 };
		Owner owner;
		bool hasOwner{ false }; // Legacy callers migrate independently.
	};

	std::vector<Candidate> legacyCandidates(uint32_t x, int side,
		uint32_t alternateX = ~0u, int alternateSide = -1);
	// Legacy policy only: no new host rules, canonical ordering or stacking.
	// Returns an assignment without modifying demands or production objects.
	std::vector<uint32_t> allocateLegacy(std::vector<Demand> const& demands);
}
