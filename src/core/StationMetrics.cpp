#include "core/StationMetrics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <numeric>
#include <tuple>

namespace core
{
	std::vector<StationStoreObservation> stationStoreObservations(StationSystemsSettings const& settings)
	{
		// Identity evidence belongs to ownership boundaries, never labels or units.
		// Transfers preserve material; collect ALL evidence in each connected component
		// before resolving, so ordering cannot conceal a contradiction.
		std::map<std::string, size_t> index;
		std::vector<size_t> parent(settings.stores.size());
		std::iota(parent.begin(), parent.end(), 0);
		for (size_t i = 0; i < settings.stores.size(); ++i) index[settings.stores[i].id] = i;
		auto root = [&](size_t i) { while (parent[i] != i) i = parent[i]; return i; };
		for (auto const& transfer : settings.transfers)
			parent[root(index.at(transfer.destinationId))] = root(index.at(transfer.sourceId));
		std::vector<std::set<std::string>> evidence(parent.size());
		auto add = [&](std::string const& id, std::string material) {
			if (!id.empty()) evidence[root(index.at(id))].insert(std::move(material));
		};
		for (auto const& store : settings.stores) if (!store.materialId.empty()) add(store.id, store.materialId);
		for (auto const& e : settings.electrolyzers)
		{ add(e.waterStoreId, "water"); add(e.oxygenStoreId, "oxygen"); add(e.hydrogenStoreId, "hydrogen"); }
		for (auto const& e : settings.fuelCells)
		{ add(e.waterStoreId, "water"); add(e.oxygenStoreId, "oxygen"); add(e.hydrogenStoreId, "hydrogen"); }
		for (auto const& e : settings.co2Captures) add(e.captureStoreId, "carbon-dioxide");
		for (auto const& e : settings.sabatierReactors)
		{ add(e.co2StoreId, "carbon-dioxide"); add(e.hydrogenStoreId, "hydrogen"); add(e.methaneStoreId, "methane"); add(e.waterStoreId, "water"); }
		for (auto const& e : settings.methaneProcessors)
		{ add(e.methaneStoreId, "methane"); add(e.hydrogenStoreId, "hydrogen"); add(e.carbonStoreId, "carbon"); }
		for (auto const& e : settings.waterCollectors) add(e.destStoreId, "water");
		for (auto const& e : settings.waterTreatments)
		{ add(e.sourceStoreId, "water"); add(e.treatedStoreId, "water"); }
		for (auto const& e : settings.crews) add(e.waterStoreId, "water");
		for (auto const& e : settings.microbialProcessors)
		{ add(e.oxygenStoreId, "oxygen"); add(e.waterStoreId, "water"); add(e.recoveredWaterStoreId, "water"); add(e.co2StoreId, "carbon-dioxide"); }
		// Nutrient uptake roles constrain constituents, not the containing
		// material: a compound nutrient feed is not necessarily pure nitrogen.
		for (auto const& e : settings.cropPopulations) add(e.waterStoreId, "water");
		for (auto const& e : settings.algaeCultures) add(e.waterStoreId, "water");
		std::vector<StationStoreObservation> result;
		for (size_t i = 0; i < settings.stores.size(); ++i)
		{
			auto const& store = settings.stores[i];
			StationStoreObservation observation;
			auto& d = observation.metric;
			d.id = "store/" + store.id + "/inventory"; d.instanceId = store.id;
			d.equipmentType = "Resource store"; d.exchangeRole = "stored-inventory";
			d.role = StationMetricRole::Inventory; d.hazardClass = store.hazardClass;
			d.quantity = StationPhysicalQuantity::Unspecified; d.unit = StationCanonicalUnit::Unspecified;
			if (store.unit == "kg" || store.unit == "g")
			{
				d.quantity = StationPhysicalQuantity::Mass; d.unit = StationCanonicalUnit::Kilogram;
				observation.sourceToCanonicalScale = store.unit == "g" ? 0.001 : 1;
			}
			if (store.unit == "m3" || store.unit == "L" || store.unit == "mL")
			{
				d.quantity = StationPhysicalQuantity::Volume; d.unit = StationCanonicalUnit::CubicMetre;
				observation.sourceToCanonicalScale = store.unit == "mL" ? 1e-6 : store.unit == "L" ? 0.001 : 1;
			}
			if (store.unit == "kWh" || store.unit == "Wh" || store.unit == "kJ" || store.unit == "MJ")
			{
				d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour;
				observation.sourceToCanonicalScale = store.unit == "Wh" ? 0.001 : store.unit == "kJ" ? 1.0 / 3600
					: store.unit == "MJ" ? 1.0 / 3.6 : 1;
			}
			if (store.unit == "mol") { d.quantity = StationPhysicalQuantity::AmountOfSubstance; d.unit = StationCanonicalUnit::Mole; }
			auto const& candidates = evidence[root(i)];
			if (candidates.size() == 1) d.materialId = *candidates.begin();
			// Conflicting explicit identities are retained in authored settings,
			// but excluded from classified accounting, not silently reinterpreted.
			result.push_back(std::move(observation));
		}
		return result;
	}

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
		if (!stored.empty())
		{
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
		}
		using Key = std::tuple<std::string, StationPhysicalQuantity, StationCanonicalUnit>;
		std::map<Key, std::vector<StationMetricDescriptor const*>> materials;
		size_t unclassified = 0;
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store" && d.role == StationMetricRole::Inventory)
		{
			if (d.materialId && d.unit != StationCanonicalUnit::Unspecified)
				materials[{*d.materialId, d.quantity, d.unit}].push_back(&d);
			else ++unclassified;
		}
		auto note = "Resource stores only; includes qualified and unqualified contents. Excludes atmosphere, living biomass and other non-store inventories. "
			+ std::to_string(unclassified) + " unclassified or non-canonical inventories excluded from material subtotals.";
		for (auto& [key, members] : materials)
		{
			auto const& [material, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->instanceId < b->instanceId; });
			StationChartDefinition chart{"stored-material/" + material + "/" + stationUnitLabel(unit), "Material inventories",
				"Stored material inventory: " + material + " (" + stationUnitLabel(unit) + ")",
				"Stored material inventory: " + material + " (applied simulation)", quantity, unit, {}};
			chart.accountingNote = note; chart.partialAccounting = unclassified != 0;
			StationChartSeriesDefinition total;
			total.metricId = chart.id + "/subtotal"; total.label = "Classified stores subtotal";
			total.operation = StationChartOperation::Sum;
			for (auto d : members) total.contributors.push_back(d->id);
			chart.series.push_back(std::move(total));
			for (auto d : members) chart.series.push_back({d->id, d->instanceId, StationMetricValue::Actual,
				StationChartOperation::Value, {}, {}, true});
			charts.push_back(std::move(chart));
		}
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store"
			&& (!d.materialId || d.unit == StationCanonicalUnit::Unspecified))
		{
			StationChartDefinition chart{"unclassified/" + d.instanceId, "Unclassified inventories",
				"Unclassified inventory: " + d.instanceId, "Unclassified inventory (applied simulation)",
				d.quantity, d.unit, {{d.id, d.instanceId}}};
			chart.accountingNote = note + " Missing, contradictory material evidence or unsupported canonical unit; no material inferred.";
			chart.partialAccounting = true; charts.push_back(std::move(chart));
		}
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
		case StationCanonicalUnit::Mole: return "mol";
		}
		return "";
	}

	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history)
	{
		StationChartData result;
		result.incomplete = chart.partialAccounting;
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
