#pragma once

#include <string>
#include "core/Platform.h"


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

	PF_CORE_API std::string getSectorObjectTypeString(SectorObjectType type);

} // core
