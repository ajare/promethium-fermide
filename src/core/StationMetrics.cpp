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

	std::vector<StationChartDefinition> stationCharts(std::vector<StationMetricDescriptor> const& descriptors)
	{
		auto charts = stationCharts();
		// Domain catalogue adapter: rendering and query arithmetic do not know
		// equipment kinds. Keep expected contributors even before the first sample.
		std::vector<StationMetricDescriptor const*> stored, capacity;
		for (auto const& d : descriptors)
			if (d.equipmentType == "Battery" && d.quantity == StationPhysicalQuantity::Energy
				&& d.unit == StationCanonicalUnit::KilowattHour)
			{
				if (d.role == StationMetricRole::Inventory) stored.push_back(&d);
				if (d.role == StationMetricRole::Capacity) capacity.push_back(&d);
			}
		if (stored.empty()) return charts;
		std::sort(stored.begin(), stored.end(), [](auto a, auto b) { return a->instanceId < b->instanceId; });
		StationChartDefinition energy{"battery-energy", "Batteries", "Stored battery energy",
			"Stored battery energy (applied simulation)", StationPhysicalQuantity::Energy,
			StationCanonicalUnit::KilowattHour, {}};
		StationChartSeriesDefinition total;
		total.metricId = "battery.energy.total"; total.label = "Station total";
		total.operation = StationChartOperation::Sum;
		for (auto const* d : stored) total.contributors.push_back(d->id);
		energy.series.push_back(total);
		for (auto const* d : stored)
		{
			StationChartSeriesDefinition contribution;
			contribution.metricId = d->id; contribution.label = d->instanceId; contribution.stacked = true;
			energy.series.push_back(std::move(contribution));
		}
		StationChartDefinition charge{"battery-charge", "Batteries", "Battery state of charge",
			"Capacity-weighted battery state of charge (applied simulation)", StationPhysicalQuantity::Fraction,
			StationCanonicalUnit::Percent, {}};
		total.metricId = "battery.charge.total"; total.label = "Capacity-weighted charge";
		total.operation = StationChartOperation::Ratio;
		for (auto const* d : stored)
		{
			auto const match = std::find_if(capacity.begin(), capacity.end(), [&](auto c) { return c->instanceId == d->instanceId; });
			// A missing descriptor must not silently shrink the denominator's population.
			total.denominator.push_back(match == capacity.end() ? d->id + "/missing-capacity" : (*match)->id);
		}
		charge.series.push_back(std::move(total));
		charts.push_back(std::move(energy)); charts.push_back(std::move(charge));
		return charts;
	}

	std::vector<std::string> stationChartMetricIds(StationChartDefinition const& chart)
	{
		std::vector<std::string> ids;
		auto add = [&](std::string const& id) {
			if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
		};
		for (auto const& line : chart.series)
		{
			if (line.operation == StationChartOperation::Value) add(line.metricId);
			else for (auto const& id : line.contributors) add(id);
			for (auto const& id : line.denominator) add(id);
		}
		return ids;
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
		case StationCanonicalUnit::Percent: return "%";
		case StationCanonicalUnit::Unspecified: return "unspecified";
		}
		return "";
	}

	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history)
	{
		StationChartData result;
		double const gap = std::numeric_limits<double>::quiet_NaN();
		auto find = [&](std::string const& id) -> StationMetricHistory const* {
			auto it = std::find_if(history.begin(), history.end(), [&](auto const& h) { return h.descriptor.id == id; });
			return it == history.end() ? nullptr : &*it;
		};
		std::vector<double> times;
		for (auto const& id : stationChartMetricIds(chart))
		{
			auto const* h = find(id);
			if (!h) { result.incomplete = true; continue; }
			result.truncated |= h->truncated;
			result.sampleIntervalSeconds = std::max(result.sampleIntervalSeconds, h->sampleIntervalSeconds);
			if (times.empty()) for (auto const& p : h->samples) times.push_back(p.elapsedSeconds);
		}
		for (auto const& definition : chart.series)
		{
			StationChartSeries line;
			line.id = definition.metricId; line.label = definition.label; line.stacked = definition.stacked;
			line.times = times;
			auto const numerator = definition.operation == StationChartOperation::Value
				? std::vector<std::string>{definition.metricId} : definition.contributors;
			bool const ratio = definition.operation == StationChartOperation::Ratio;
			auto const* reference = numerator.empty() ? nullptr : find(numerator.front());
			// Ratios require matching physical numerator/denominator units, not
			// matching their dimensionless output axis. Sums must match the axis.
			auto quantity = ratio && reference ? reference->descriptor.quantity : chart.quantity;
			auto unit = ratio && reference ? reference->descriptor.unit : chart.unit;
			for (size_t row = 0; row < times.size(); ++row)
			{
				bool valid = std::isfinite(times[row]) && (!ratio || (chart.quantity == StationPhysicalQuantity::Fraction
					&& (chart.unit == StationCanonicalUnit::Percent || chart.unit == StationCanonicalUnit::Fraction)));
				double coverage = 1;
				auto sum = [&](std::vector<std::string> const& ids) {
					long double value = 0;
					if (ids.empty()) { valid = false; coverage = 0; }
					for (auto const& id : ids)
					{
						auto const* h = find(id);
						if (!h || h->descriptor.quantity != quantity || h->descriptor.unit != unit
							|| h->descriptor.coverage != StationMetricCoverage::Complete
							|| h->descriptor.validity != StationMetricValidity::Valid
							|| row >= h->samples.size() || h->samples[row].elapsedSeconds != times[row]
							|| (definition.value == StationMetricValue::Requested && !h->descriptor.requestedSemantics))
						{ valid = false; coverage = 0; continue; }
						auto const& p = h->samples[row];
						auto const v = definition.value == StationMetricValue::Requested ? p.requested : p.actual;
						coverage = std::min(coverage, std::isfinite(p.coverage) ? p.coverage : 0.0);
						if (p.validity != StationMetricValidity::Valid || p.coverage != 1 || !v || !std::isfinite(*v))
						{ valid = false; continue; }
						value += *v;
					}
					return value;
				};
				auto value = sum(numerator);
				if (ratio)
				{
					auto const denominator = sum(definition.denominator);
					if (denominator <= 0) valid = false;
					else value = value / denominator * (chart.unit == StationCanonicalUnit::Percent ? 100 : 1);
				}
				valid &= std::isfinite(static_cast<double>(value));
				line.coverage.push_back(coverage);
				line.values.push_back(valid ? static_cast<double>(value) : gap);
				result.incomplete |= !valid;
			}
			result.incomplete |= times.empty();
			result.series.push_back(std::move(line));
		}
		// Generic stack preparation. A missing expected contribution leaves a
		// gap in the whole stack, rather than making the known subset look total.
		for (auto& line : result.series) if (line.stacked)
		{ line.lower.resize(times.size(), gap); line.upper.resize(times.size(), gap); }
		for (size_t row = 0; row < times.size(); ++row)
		{
			long double total = 0;
			bool valid = true;
			for (auto const& line : result.series) if (line.stacked)
			{ valid &= std::isfinite(line.values[row]); total += line.values[row]; }
			if (!valid || !std::isfinite(static_cast<double>(total))) continue;
			total = 0;
			for (auto& line : result.series) if (line.stacked)
			{
				line.lower[row] = static_cast<double>(total); total += line.values[row];
				line.upper[row] = static_cast<double>(total);
			}
		}
		return result;
	}
}
