#pragma once
#include "StationSystems.h"
#include <optional>
#include <string>
#include <vector>

namespace core
{
	// Observation metadata is independent of diagram nodes and presentation.
	enum class StationPhysicalQuantity { Power, Energy, Mass, Volume, Temperature, Fraction, MassRate, VolumeRate, Unspecified, AmountOfSubstance };
	enum class StationCanonicalUnit { Kilowatt, KilowattHour, Kilogram, CubicMetre, Kelvin, Fraction, KilogramPerSecond, CubicMetrePerSecond, Percent, Unspecified, Mole };
	enum class StationMetricRole { Demand, Generation, Inventory, Input, Output, Loss, InventoryChange, StateOfCharge, Capacity };
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
		double coverage{1};
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
	enum class StationChartOperation { Value, Sum, Ratio };
	struct StationChartSeriesDefinition
	{
		std::string metricId, label;
		StationMetricValue value{StationMetricValue::Actual};
		StationChartOperation operation{StationChartOperation::Value};
		// Expected membership, compiled from applied descriptors, never from
		// the subset of measurements that happens to be available this frame.
		std::vector<std::string> contributors, denominator;
		bool stacked{false};
		std::string hazardClass; // Optional grouping key of this contribution.
		std::string equipmentType; // Optional owner-type breakdown, independent of instance names.
	};
	struct StationChartDefinition
	{
		std::string id, group, label, title;
		StationPhysicalQuantity quantity;
		StationCanonicalUnit unit;
		std::vector<StationChartSeriesDefinition> series;
		std::string accountingNote;
		bool partialAccounting{false};
		// Generic presentation variants share one selector property.
		std::string propertyId, breakdown;
	};
	struct StationChartSeries
	{
		std::string id, label;
		std::vector<double> coverage;
		std::vector<double> times, values; // Missing/invalid measurements are NaN gaps, never zero.
		bool stacked{false};
		std::vector<double> lower, upper; // Prepared cumulative stack bounds; values remain individual contributions.
	};
	struct StationChartData
	{
		std::vector<StationChartSeries> series;
		double sampleIntervalSeconds{0};
		bool truncated{false}, incomplete{false};
	};
	struct StationStoreObservation
	{
		StationMetricDescriptor metric;
		double sourceToCanonicalScale{1};
	};
	// Applied configuration must already have passed domain validation.
	std::vector<StationStoreObservation> stationStoreObservations(StationSystemsSettings const& settings);
	std::vector<StationChartDefinition> const& stationCharts();
	std::vector<StationChartDefinition> stationCharts(std::vector<StationMetricDescriptor> const& descriptors);
	void appendStationChartBreakdowns(std::vector<StationChartDefinition>& charts);
	std::vector<std::string> stationChartMetricIds(StationChartDefinition const& chart);
	char const* stationUnitLabel(StationCanonicalUnit unit);
	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history);
}
