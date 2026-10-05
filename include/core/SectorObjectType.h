#pragma once

#include <string>


namespace core
{

	enum struct SectorObjectType
	{
		None,
		BulkheadDoor,
		Door,
		ForceBridge,
		InteractionPoint,
		Ladder,
		Lift,
		Marker,
		Shuttle,
		Walkway,
		Window,
		BoothWindow,
		AccessPanel
	};

	inline bool isWindowAperture(SectorObjectType type)
	{
		return type == SectorObjectType::Window || type == SectorObjectType::BoothWindow;
	}

	std::string getSectorObjectTypeString(SectorObjectType type);

} // core
