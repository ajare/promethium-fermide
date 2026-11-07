#pragma once

#include "core/Platform.h"
#include "core/SensorType.h"

namespace core
{

	class PF_CORE_API SensorSource
	{
	public:

		SensorSource() = default;

		virtual ~SensorSource() = default;
	
		virtual bool canSense(SensorType sensor) const = 0;
	};

} // core
