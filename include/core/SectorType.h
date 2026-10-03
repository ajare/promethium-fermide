#pragma once

#include <string>


namespace core
{

	enum struct SectorType
	{
		Location,
		Background,
		Facade,
		Ladder,
		Lift,
		Shuttle,
		Stairwell,
		Staircase,
		Airlock,
		SecurityScanner
	};

	std::string getSectorTypeString(SectorType type);

	// A Facade is a Location in every traversal respect: whatever may sit next to
	// a Location and share a walkable boundary with it must accept a Facade too,
	// and vice versa, and whatever may land on a Location may land on a Facade.
	// Adjacency checks - graph cross-sector Vertex creation across open ends,
	// wall-removal validation, floor-level matching - admit Facade through this
	// helper so the widening lives in one place (ADR 0003, ticket #45), and so
	// does transit landing validation: a Lift, Staircase, Ladder, Stairwell or
	// Shuttle lands on a Facade exactly as it lands on a Room, and the landing
	// Vertex of a Facade is a VertexType::Location (ticket #52).
	//
	// Type-specific rendering keeps testing SectorType::Location directly.
	// Footprint editing does not use this helper: a Facade is occupiable and
	// resizes or deletes through its own plan entry points, with the same
	// Agent-and-object cascade as a Room while preserving its Facade identity.
	bool isLocationLike(SectorType type);

} // core
