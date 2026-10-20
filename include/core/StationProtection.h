#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace core
{
	// Protection-only certification. This is not a convergence certificate for
	// every observable (#773), nor a physical cap on environmental conditions.
	// Ratio 1 retains sampled legacy protection; coarse qualification is tested
	// at every integer ratio 2..6. See the evidence linked by each registry row.
	inline constexpr uint32_t StationProtectionCertifiedStepRatio = 6;
	// The same Celsius association allowance used by runtime qualification.
	inline constexpr double StationProtectionTemperatureOvershootC = 1e-9;
	// Temperature bounds apply to the envelope of admitted work, not to
	// unconditional environmental/decay/retained heat or a denied service.
	struct StationProtectionBound
	{
		std::string_view limit;
		double overshoot;
		std::string_view unit;
		std::string_view basis;
		std::string_view verifiedBy;
		std::string_view evidence;
	};
	inline constexpr auto StationProtectionBounds = std::to_array<StationProtectionBound>({
		{"Generator.maxSinkTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"FuelCell.maxSinkTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Electrolyzer.maxSinkTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Sabatier.maxSinkTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"MethaneProcessor.maxSinkTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"MicrobialProcessor.operatingTemperature", StationProtectionTemperatureOvershootC, "C", "pre-commit lower and upper gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Crop.growingTemperature", StationProtectionTemperatureOvershootC, "C", "pre-commit compartment gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Algae.growingTemperature", StationProtectionTemperatureOvershootC, "C", "pre-commit compartment gross-rate qualification", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Chiller.maxCondensingTemperatureC", StationProtectionTemperatureOvershootC, "C", "pre-commit gross-rate qualification including protected credit", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"Chiller.chilledSetpointC", StationProtectionTemperatureOvershootC, "C", "finite cold-side energy dose including exchange and pump work", "rateAwareProtection", "docs/tickets/771-rate-aware-protection.md"},
		{"CoolantLoop.freezeProtectionSetpointC", StationProtectionTemperatureOvershootC, "C", "pre-commit heater request; requires sufficient heating and electrical supply", "protectionFreeze", "docs/tickets/771-rate-aware-protection.md"},
		{"ResourceStore.protectedReserve", 0, "native units", "rate-times-duration drawable cap and crossing resolution", "saturationCrossing", "docs/tickets/771-rate-aware-protection.md"},
		{"ResourceStore.capacity", 0, "native units", "rate-times-duration headroom cap and crossing resolution", "saturationCrossing", "docs/tickets/771-rate-aware-protection.md"}
	});
}
