#include "core/StationMetrics.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{

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
		case StationCanonicalUnit::Unspecified: return "unspecified";
		}
		return "";
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
					bool const valid = point.validity == StationMetricValidity::Valid && point.coverage == 1
						&& value && std::isfinite(*value);
					line.coverage.push_back(point.coverage);
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
