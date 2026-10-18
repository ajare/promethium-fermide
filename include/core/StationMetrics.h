#pragma once
#include "StationSystems.h"
#include <optional>
#include <string>
#include <vector>

namespace core
{
	// Observation metadata is independent of diagram nodes and presentation.
	enum class StationPhysicalQuantity { Power, Energy, Mass, Volume, Temperature, Fraction, MassRate, VolumeRate };
	enum class StationCanonicalUnit { Kilowatt, KilowattHour, Kilogram, CubicMetre, Kelvin, Fraction, KilogramPerSecond, CubicMetrePerSecond };
	enum class StationMetricRole { Demand, Generation, Inventory, Input, Output, Loss, InventoryChange, StateOfCharge };
	enum class StationRequestedSemantics { Demand };
	enum class StationMetricValidity { Valid, Unavailable, Invalid };
	enum class StationMetricCoverage { Complete, Partial, Unclassified };
	struct StationMetricDescriptor
	{
		std::string id;
		StationPhysicalQuantity quantity{StationPhysicalQuantity::Power};
		StationCanonicalUnit unit{StationCanonicalUnit::Kilowatt};
		StationMetricRole role{StationMetricRole::Output};
		std::optional<StationRequestedSemantics> requestedSemantics;
		std::string equipmentType, instanceId, exchangeRole;
		std::optional<std::string> materialId, constituentId;
		std::optional<std::string> hazardClass;
		StationMetricCoverage coverage{StationMetricCoverage::Complete};
		StationMetricValidity validity{StationMetricValidity::Unavailable};
	};
	struct StationMetricSample
	{
		double elapsedSeconds{0};
		std::optional<double> actual, requested;
		StationMetricValidity validity{StationMetricValidity::Unavailable};
	};
	struct StationMetricHistory
	{
		StationMetricDescriptor descriptor;
		std::vector<StationMetricSample> samples;
		double sampleIntervalSeconds{0};
		bool truncated{false};
	};
	enum class StationMetricValue { Actual, Requested };
	struct StationChartSeriesDefinition
	{
		std::string metricId, label;
		StationMetricValue value{StationMetricValue::Actual};
	};
	struct StationChartDefinition
	{
		std::string id, group, label, title;
		StationPhysicalQuantity quantity;
		StationCanonicalUnit unit;
		std::vector<StationChartSeriesDefinition> series;
	};
	struct StationChartSeries
	{
		std::string id, label;
		std::vector<double> times, values; // Missing/invalid measurements are NaN gaps, never zero.
	};
	struct StationChartData
	{
		std::vector<StationChartSeries> series;
		double sampleIntervalSeconds{0};
		bool truncated{false}, incomplete{false};
	};
	std::vector<StationChartDefinition> const& stationCharts();
	char const* stationUnitLabel(StationCanonicalUnit unit);
	// Temporary 1a adapter. The World-owned recorder replaces this in 1b;
	// it does not sample, tick, or own a second history.
	std::vector<StationMetricHistory> adaptStationTrends(std::vector<StationTrendSeries> const& trends);
	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history);
}
