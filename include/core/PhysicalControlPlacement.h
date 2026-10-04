#pragma once

#include <cstdint>
#include <vector>

namespace core::physicalControl
{
	// Authored data only. Runtime identities and prior assignments are absent.
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
		int quarterOffset{ 0 }; // Only 0..3; 1.0 belongs to the next host.
		int64_t centreKey() const;
		float centreX() const;
		static Candidate explicitHost(uint32_t cellX, int quarterOffset);
	};
	struct Demand
	{
		std::vector<Candidate> candidates;
		uint32_t defaultCandidate{ 0 };
		Owner owner;
	};

	bool canonicalLess(Owner const& a, Owner const& b);
	// One Layer/Level, across all hosting Locations. Same-Location stacks of
	// at most four preserve each independent control.
	std::vector<uint32_t> allocateCanonical(std::vector<Demand> const& demands);
}
