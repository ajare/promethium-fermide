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
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store" && d.role == StationMetricRole::Inventory
			&& d.exchangeRole == "stored-inventory")
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
				StationChartOperation::Value, {}, {}, true, d->hazardClass.value_or("unclassified")});
			charts.push_back(std::move(chart));
			// Qualified inventory is never summed across incompatible hazards.
			std::map<std::string, std::vector<StationMetricDescriptor const*>> usable;
			for (auto d : members) usable[d->hazardClass.value_or("unclassified")].push_back(d);
			for (auto const& [hazard, contributions] : usable)
			{
				StationChartDefinition qualified{"stored-usable/" + material + "/" + stationUnitLabel(unit) + "/" + hazard,
					"Material inventories", "Usable stored material: " + material + " (" + stationUnitLabel(unit) + "; " + hazard + ")",
					"Qualified usable inventory: " + material + " — " + hazard + " (applied simulation)", quantity, unit, {}};
				qualified.accountingNote = "Resource stores only; qualified contents only, excludes unqualified contents. Protected resource reserves remain included: usable is NOT discretionary drawability. Hazard classes are not interchangeable; hazardous supply cannot meet normal demand. "
					+ std::to_string(unclassified) + " unclassified or non-canonical inventories excluded from material subtotals.";
				qualified.partialAccounting = unclassified != 0;
				StationChartSeriesDefinition subtotal;
				subtotal.metricId = qualified.id + "/subtotal"; subtotal.label = "Classified qualified stores subtotal (" + hazard + ")";
				subtotal.operation = StationChartOperation::Sum;
				for (auto d : contributions) subtotal.contributors.push_back(d->id + "/usable");
				qualified.series.push_back(std::move(subtotal));
				for (auto d : contributions) qualified.series.push_back({d->id + "/usable", d->instanceId + " (" + hazard + ")",
					StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true, hazard});
				charts.push_back(std::move(qualified));
			}
		}
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store" && d.exchangeRole == "stored-inventory"
			&& (!d.materialId || d.unit == StationCanonicalUnit::Unspecified))
		{
			StationChartDefinition chart{"unclassified/" + d.instanceId, "Unclassified inventories",
				"Unclassified inventory: " + d.instanceId, "Unclassified inventory (applied simulation)",
				d.quantity, d.unit, {{d.id, d.instanceId}}};
			chart.accountingNote = note + " Missing, contradictory material evidence or unsupported canonical unit; no material inferred.";
			chart.partialAccounting = true; charts.push_back(std::move(chart));
		}
		// A broader boundary is deliberately separate from stored-only and usable
		// queries. Gas in a compartment is not qualified store supply. Only
		// ownership descriptors participate, never transfer/flow observations.
		std::map<Key, std::vector<StationMetricDescriptor const*>> accounted;
		for (auto const& d : descriptors)
			if (d.role == StationMetricRole::Inventory
				&& (d.exchangeRole == "stored-inventory" || d.exchangeRole == "retained-inventory")
				&& d.materialId && d.unit != StationCanonicalUnit::Unspecified)
				accounted[{*d.materialId, d.quantity, d.unit}].push_back(&d);
		for (auto& [key, members] : accounted)
		{
			auto const& [material, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->id < b->id; });
			StationChartDefinition chart{"accounted-material/" + material + "/" + stationUnitLabel(unit), "Material inventories",
				"Accounted material inventory: " + material + " (" + stationUnitLabel(unit) + ")",
				"Accounted material inventory: " + material + " (applied simulation)", quantity, unit, {}};
			// Material ownership and constituent decomposition are independent
			// populations. Never add compound constituents to this material sum.
			chart.partialAccounting = true;
			chart.accountingNote = "Accounted owners: Resource stores (qualified + unqualified), atmosphere gases and water vapour, closed-loop coolant. "
				"Partial system accounting: biological inventory adapters pending; compound constituents are separate views, not additional material ownership. No inferred bound materials, culture liquid, or equipment holdup. "
				+ std::to_string(unclassified) + " unclassified or non-canonical stores excluded. Different canonical quantities are separate; no inferred density or molar conversion. Not usable supply or discretionary drawability.";
			StationChartSeriesDefinition total;
			total.metricId = chart.id + "/subtotal"; total.label = "Accounted owners subtotal";
			total.operation = StationChartOperation::Sum;
			for (auto d : members) total.contributors.push_back(d->id);
			chart.series.push_back(std::move(total));
			for (auto d : members) chart.series.push_back({d->id, d->equipmentType + ": " + d->instanceId,
				StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true,
				d->hazardClass.value_or("unclassified"), d->equipmentType});
			charts.push_back(std::move(chart));
		}
		// Constituents describe chemical form on their own accounting boundary.
		// In particular nutrientNitrogen is never atmospheric nitrogen, and
		// moisture is not added to both compound mass and free-water stock.
		std::map<Key, std::vector<StationMetricDescriptor const*>> constituents;
		size_t uncharacterised = 0;
		std::set<std::string> characterised;
		for (auto const& d : descriptors) if (d.exchangeRole == "stored-constituent") characterised.insert(d.instanceId);
		for (auto const& d : descriptors)
		{
			if (d.exchangeRole == "stored-inventory" && d.equipmentType == "Resource store"
				&& !characterised.contains(d.instanceId)) ++uncharacterised;
			if (d.role == StationMetricRole::Inventory && d.exchangeRole == "stored-constituent" && d.constituentId)
				constituents[{*d.constituentId, d.quantity, d.unit}].push_back(&d);
		}
		for (auto& [key, members] : constituents)
		{
			auto const& [constituent, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->id < b->id; });
			auto make = [&](std::vector<StationMetricDescriptor const*> const& population, std::string hazard) {
				bool const usable = !hazard.empty();
				std::string const prefix = usable ? "stored-usable-constituent/" : "stored-constituent/";
				std::string const label = usable ? "Usable stored constituent: " : "Stored constituent inventory: ";
				StationChartDefinition chart{prefix + constituent + "/" + stationUnitLabel(unit) + (usable ? "/" + hazard : ""),
					"Constituent inventories", label + constituent + " (" + stationUnitLabel(unit) + (usable ? "; " + hazard : "") + ")",
					label + constituent + " (applied simulation)", quantity, unit, {}};
				chart.partialAccounting = true;
				chart.accountingNote = "Compound Resource stores only, including retained waste and material awaiting treatment already owned by stores; not a system total. "
					"Actual blended constituents: water is wet-mass moisture; other mass constituents partition dry mass; energy is chemical energy, not mass. "
					"Bound nutrient nitrogen and atmospheric nitrogen remain distinct. Material ownership totals and constituent decomposition are separate, never added together. "
					"Excludes living biomass, free gases/liquids and uncharacterised stores (" + std::to_string(uncharacterised) + "). ";
				chart.accountingNote += usable
					? "Qualified blend only; excludes unqualified contents. Protected reserves remain included: NOT discretionary drawability. Hazard classes are not interchangeable."
					: "Includes qualified and unqualified contents; hazard totals do not imply interchangeability or usable supply.";
				StationChartSeriesDefinition total;
				total.metricId = chart.id + "/subtotal"; total.label = "Characterised stores subtotal";
				total.operation = StationChartOperation::Sum;
				for (auto d : population) total.contributors.push_back(d->id + (usable ? "/usable" : ""));
				chart.series.push_back(std::move(total));
				for (auto d : population) chart.series.push_back({d->id + (usable ? "/usable" : ""), d->instanceId,
					StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true,
					d->hazardClass.value_or("unclassified"), d->equipmentType});
				charts.push_back(std::move(chart));
			};
			make(members, {});
			std::map<std::string, std::vector<StationMetricDescriptor const*>> hazards;
			for (auto d : members) hazards[d->hazardClass.value_or("unclassified")].push_back(d);
			for (auto const& [hazard, population] : hazards) make(population, hazard);
		}
		appendStationChartBreakdowns(charts);
		return charts;
	}

	void appendStationChartBreakdowns(std::vector<StationChartDefinition>& charts)
	{
		auto const count = charts.size();
		for (size_t i = 0; i < count; ++i)
		{
			auto& chart = charts[i];
			if (std::none_of(chart.series.begin(), chart.series.end(), [](auto const& s) { return s.stacked; })) continue;
			chart.propertyId = chart.id; chart.breakdown = "Instance";
			auto aggregate = chart;
			aggregate.id += "/aggregate"; aggregate.breakdown = "Aggregate";
			std::erase_if(aggregate.series, [](auto const& s) { return s.stacked; });
			auto hazard = aggregate;
			hazard.id = chart.id + "/by-hazard"; hazard.breakdown = "Hazard class";
			std::map<std::string, std::vector<std::string>> groups;
			for (auto const& s : chart.series) if (s.stacked && !s.hazardClass.empty())
			{
				auto& ids = groups[s.hazardClass];
				if (s.operation == StationChartOperation::Value) ids.push_back(s.metricId);
				else ids.insert(ids.end(), s.contributors.begin(), s.contributors.end());
			}
			for (auto const& [key, ids] : groups)
				hazard.series.push_back({chart.propertyId + "/hazard/" + key, key, StationMetricValue::Actual,
					StationChartOperation::Sum, ids, {}, true, key});
			auto equipment = aggregate;
			equipment.id = chart.id + "/by-equipment"; equipment.breakdown = "Equipment type";
			std::map<std::string, std::vector<std::string>> equipmentGroups;
			for (auto const& s : chart.series) if (s.stacked && !s.equipmentType.empty())
				equipmentGroups[s.equipmentType].push_back(s.metricId);
			for (auto const& [key, ids] : equipmentGroups)
				equipment.series.push_back({chart.propertyId + "/equipment/" + key, key, StationMetricValue::Actual,
					StationChartOperation::Sum, ids, {}, true});
			charts.push_back(std::move(aggregate));
			if (groups.size() > 1) charts.push_back(std::move(hazard));
			if (equipmentGroups.size() > 1) charts.push_back(std::move(equipment));
		}
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
