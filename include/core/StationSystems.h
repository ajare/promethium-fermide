#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "core/ScriptExecution.h"

namespace core
{
	// Slice 1a has no physical processes or assumed resource production.
	struct StationSystemsSettings
	{
		bool enabled{false};
		float observationIntervalSeconds{1.0f}; // bounded event sampling, not a clock
		bool operator==(StationSystemsSettings const&) const = default;
	};
	bool validateStationSystemsSettings(StationSystemsSettings const&, std::string& diagnostic);
	inline constexpr std::array<char const*, 4> StationSystemNames{
		"Power", "ECLSS", "Thermal control", "Monitoring/supervision"};
	struct StationSystemsSnapshot
	{
		bool enabled{false};
		bool healthy{true};
		uint64_t worldTick{0};
		double elapsedSeconds{0};
		std::array<uint64_t, 4> completedTicks{};
		bool operator==(StationSystemsSnapshot const&) const = default;
	};
	struct StationSystemsEvent
	{
		uint64_t worldTick{0};
		std::string message;
		bool operator==(StationSystemsEvent const&) const = default;
	};
	struct StationSystemsDiagnostic
	{
		uint64_t worldTick{0};
		std::string system;
		ScriptExecutionFailure failure{ScriptExecutionFailure::None};
		std::string message;
	};

	// Application Resource integration. The loader supplies immutable source;
	// executed objects are never shared. Without an application, use the exact
	// same compiled bundled asset (as for the bundled Human Agent type).
	using StationSystemsSourceLoader = std::function<std::string()>;
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader);
	std::string bundledStationSystemsSource();

	class StationSystemsRuntime;
}
