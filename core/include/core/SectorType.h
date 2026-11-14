#pragma once

#include <optional>
#include <string>
#include <vector>
#include "core/Platform.h"


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
		Chamber,
		Dumbwaiter
	};

	PF_CORE_API std::string getSectorTypeString(SectorType type);

	// Every Sector type, in the order the enum declares them.
	//
	// A per-type table - the author's floor depth scheme, for one - walks this
	// instead of spelling the eleven names out again, so a type added to the
	// enum arrives in the table rather than having to be remembered there. The
	// list is built once, in declaration order, which makes the table's row
	// order stable across runs and platforms rather than whatever a container
	// happened to iterate.
	PF_CORE_API std::vector<SectorType> const& allSectorTypes();

	// The Sector type a written name names, or nothing where no type answers to
	// it. The inverse of `getSectorTypeString`, and deliberately total: a
	// document naming a type this build does not have is data to refuse or
	// ignore, not a switch case to crash on. Names are matched exactly, so the
	// wire form is the same string the writer produced and nothing else.
	PF_CORE_API std::optional<SectorType> sectorTypeFromString(std::string const& name);

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
	PF_CORE_API bool isLocationLike(SectorType type);

} // core
