#pragma once

namespace core
{
	// Runtime bodily stance, independent of locomotion and authored properties.
	enum class Pose { Standing, Sitting, Lying, Crouching, Crawling };

	// Lower-case Lua/scripting vocabulary for the runtime pose. Shared by the
	// validated Action effect and the read-only Agent inspection view.
	inline constexpr char const* poseName(Pose pose)
	{
		switch (pose)
		{
		case Pose::Standing: return "standing";
		case Pose::Sitting: return "sitting";
		case Pose::Lying: return "lying";
		case Pose::Crouching: return "crouching";
		case Pose::Crawling: return "crawling";
		}
		return "standing";
	}
}
