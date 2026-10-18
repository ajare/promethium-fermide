#include "core/StationMetrics.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{
	namespace
	{
		struct TrendBinding
		{
			char const* subsystem;
			StationMetricDescriptor metric;
		};
		std::vector<TrendBinding> const& bindings()
		{
			static auto const result = [] {
				StationMetricDescriptor service;
				service.id = "station.power.service";
				service.role = StationMetricRole::Input;
				service.requestedSemantics = StationRequestedSemantics::Demand;
				StationMetricDescriptor generation;
				generation.id = "station.power.generation";
				generation.role = StationMetricRole::Generation;
				return std::vector<TrendBinding>{{"power", service}, {"power-generation", generation}};
			}();
			return result;
		}
	}

	std::vector<StationChartDefinition> const& stationCharts()
	{
		static std::vector<StationChartDefinition> const charts{{"power", "Power", "Power",
			"Power demand and generation (applied simulation)", StationPhysicalQuantity::Power,
			StationCanonicalUnit::Kilowatt, {{"station.power.service", "Demand", StationMetricValue::Requested},
				{"station.power.generation", "Generation", StationMetricValue::Actual}}}};
		return charts;
	}

	char const* stationUnitLabel(StationCanonicalUnit unit)
	{
		switch (unit)
		{
		case StationCanonicalUnit::Kilowatt: return "kW";
		case StationCanonicalUnit::KilowattHour: return "kWh";
		case StationCanonicalUnit::Kilogram: return "kg";
		case StationCanonicalUnit::CubicMetre: return "m3";
		case StationCanonicalUnit::Kelvin: return "K";
		case StationCanonicalUnit::Fraction: return "fraction";
		case StationCanonicalUnit::KilogramPerSecond: return "kg/s";
		case StationCanonicalUnit::CubicMetrePerSecond: return "m3/s";
		}
		return "";
	}

	std::vector<StationMetricHistory> adaptStationTrends(std::vector<StationTrendSeries> const& trends)
	{
		std::vector<StationMetricHistory> result;
		for (auto const& binding : bindings())
		{
			StationMetricHistory metric;
			metric.descriptor = binding.metric;
			auto const source = std::find_if(trends.begin(), trends.end(), [&](auto const& series) {
				return series.subsystem == binding.subsystem && series.id.empty();
			});
			if (source != trends.end() && source->unit == stationUnitLabel(metric.descriptor.unit))
			{
				metric.descriptor.validity = StationMetricValidity::Valid;
				metric.sampleIntervalSeconds = source->sampleIntervalSeconds;
				metric.truncated = source->truncated;
				for (auto const& sample : source->samples)
				{
					StationMetricSample point;
					point.elapsedSeconds = sample.elapsedSeconds;
					if (std::isfinite(sample.actual)) point.actual = sample.actual;
					if (metric.descriptor.requestedSemantics && std::isfinite(sample.requested))
						point.requested = sample.requested;
					point.validity = point.actual && (!metric.descriptor.requestedSemantics || point.requested)
						? StationMetricValidity::Valid : StationMetricValidity::Invalid;
					metric.samples.push_back(point);
				}
			}
			result.push_back(std::move(metric)); // Expected identity survives absent measurements.
		}
		return result;
	}

	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history)
	{
		StationChartData result;
		for (auto const& definition : chart.series)
		{
			StationChartSeries line;
			line.id = definition.metricId;
			line.label = definition.label;
			auto const metric = std::find_if(history.begin(), history.end(), [&](auto const& value) {
				return value.descriptor.id == definition.metricId;
			});
			if (metric == history.end() || metric->descriptor.quantity != chart.quantity
				|| metric->descriptor.unit != chart.unit
				|| (definition.value == StationMetricValue::Requested && !metric->descriptor.requestedSemantics))
				result.incomplete = true;
			else
			{
				result.incomplete |= metric->descriptor.coverage != StationMetricCoverage::Complete
					|| metric->descriptor.validity != StationMetricValidity::Valid || metric->samples.empty();
				result.truncated |= metric->truncated;
				result.sampleIntervalSeconds = std::max(result.sampleIntervalSeconds, metric->sampleIntervalSeconds);
				for (auto const& point : metric->samples)
				{
					if (!std::isfinite(point.elapsedSeconds)) { result.incomplete = true; continue; }
					auto const value = definition.value == StationMetricValue::Requested ? point.requested : point.actual;
					bool const valid = point.validity == StationMetricValidity::Valid && value && std::isfinite(*value);
					line.times.push_back(point.elapsedSeconds);
					line.values.push_back(valid ? *value : std::numeric_limits<double>::quiet_NaN());
					result.incomplete |= !valid;
				}
			}
			result.series.push_back(std::move(line));
		}
		return result;
	}
}
