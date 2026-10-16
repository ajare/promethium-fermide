#pragma once

#include <array>
#include <span>
#include <string_view>

namespace core
{
	enum class StationPortDirection { Input, Output };
	// Structural declarations only. Material, unit and hazard compatibility
	// remain the configuration validator's responsibility. Types without a
	// descriptor are projected by Flow but cannot yet be authored there.
	struct PortDescriptor
	{
		std::string_view nodeType;
		std::string_view name;
		StationPortDirection direction;
		std::string_view kind;
		std::string_view counterpartKind;
		size_t maximumLinks; // zero means unbounded
	};
	inline std::span<PortDescriptor const> stationPortDescriptors()
	{
		static constexpr std::array descriptors{
			PortDescriptor{"Store", "Inventory in", StationPortDirection::Input, "inventory", "material", 0},
			PortDescriptor{"Store", "Inventory out", StationPortDirection::Output, "inventory", "material", 0}};
		return descriptors;
	}
}
