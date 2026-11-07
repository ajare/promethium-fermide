#pragma once

#include <string>
#include "core/Platform.h"


namespace core
{

	enum struct SensorType
	{
		AgentBlocking
	};

	PF_CORE_API std::string getSensorTypeString(SensorType type);

} // core
