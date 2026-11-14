#include "core/SectorType.h"
#include "core/Exceptions.h"

#include <array>


namespace core
{

	using namespace std;

	string getSectorTypeString(SectorType type)
	{
		switch (type)
		{
		case SectorType::Location:
			return "Location";

		case SectorType::Background:
			return "Background";

		case SectorType::Facade:
			return "Facade";

		case SectorType::Ladder:
			return "Ladder";

		case SectorType::Lift:
			return "Lift";

		case SectorType::Shuttle:
			return "Shuttle";

		case SectorType::Stairwell:
			return "Stairwell";

		case SectorType::Airlock:
			return "Airlock";
		case SectorType::Dumbwaiter:
			return "Dumbwaiter";
		case SectorType::Chamber:
			return "Chamber";

		case SectorType::Staircase:
			return "Staircase";

		default:
			throw UnhandledException(type, "SectorType");
		}
	}

	namespace
	{
		// Declaration order, spelled out once. C++ has no reflection over an
		// enum's enumerators, so the list that says "every Sector type" is
		// written by hand and the array's own size is what pins it: adding an
		// enumerator without listing it trips the assert below rather than
		// quietly missing a row off the author's table.
		constexpr array<SectorType, 11> kAllSectorTypes{
			SectorType::Location,
			SectorType::Background,
			SectorType::Facade,
			SectorType::Ladder,
			SectorType::Lift,
			SectorType::Shuttle,
			SectorType::Stairwell,
			SectorType::Staircase,
			SectorType::Airlock,
			SectorType::Chamber,
			SectorType::Dumbwaiter
		};

		static_assert(kAllSectorTypes.size() == static_cast<size_t>(SectorType::Dumbwaiter) + 1,
			"allSectorTypes lists every Sector type by hand; a type added to the "
			"enum has to be listed here as well");
	}

	vector<SectorType> const& allSectorTypes()
	{
		// Built once and read-only afterwards, which is what lets every caller
		// hold the same list by reference instead of rebuilding one per row.
		static vector<SectorType> const types(kAllSectorTypes.begin(),
			kAllSectorTypes.end());
		return types;
	}

	optional<SectorType> sectorTypeFromString(string const& name)
	{
		// Matched against the writer's own strings rather than a second table of
		// literals, so the two cannot disagree about what a type is called. The
		// walk is over eleven entries and only ever runs on a document read, so
		// the linear scan costs nothing worth trading away for a map that could
		// drift from `getSectorTypeString`.
		for (auto const type : allSectorTypes())
			if (getSectorTypeString(type) == name) return type;
		return nullopt;
	}

	bool isLocationLike(SectorType type)
	{
		// A Room, a Corridor (a Location), or a Facade: every Sector that is a
		// Location in the traversal sense.  See the header note for which checks
		// may use this and which must stay Location-only.
		return type == SectorType::Location || type == SectorType::Facade;
	}

} // core
