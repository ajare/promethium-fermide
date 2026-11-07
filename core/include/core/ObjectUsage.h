#pragma once

#include <string_view>

namespace core
{
	enum class ObjectUsage { Arms, None, RemoteControl };

	constexpr bool isValidObjectUsage(ObjectUsage mode)
	{
		return mode == ObjectUsage::Arms || mode == ObjectUsage::None || mode == ObjectUsage::RemoteControl;
	}

	constexpr char const* objectUsageName(ObjectUsage mode)
	{
		return mode == ObjectUsage::Arms ? "Arms" : mode == ObjectUsage::RemoteControl ? "Remote control" : "None";
	}

	constexpr char const* objectUsageWireName(ObjectUsage mode)
	{
		return mode == ObjectUsage::Arms ? "arms" : mode == ObjectUsage::RemoteControl ? "remote_control" : "none";
	}

	constexpr bool parseObjectUsage(std::string_view name, ObjectUsage& mode)
	{
		if (name == "arms") mode = ObjectUsage::Arms;
		else if (name == "none") mode = ObjectUsage::None;
		else if (name == "remote_control") mode = ObjectUsage::RemoteControl;
		else return false;
		return true;
	}
}
