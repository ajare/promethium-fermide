#include "core/StationMetrics.h"
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace core
{
	// Compartment state is instantaneous rather than an exchange: no per-tick
	// rate conversion and no cumulative differencing. The read callback hands
	// back the snapshot field and the recorder samples it on the existing
	// observation cadence.
	void addAtmosphereState(std::vector<StationExchangeObservation>& result,
		std::string id, std::string instance, std::string role,
		StationPhysicalQuantity quantity, StationCanonicalUnit unit, double scale,
		double StationAtmosphereSnapshot::*member)
	{
		StationExchangeObservation o;
		auto& d = o.metric;
		d.id = std::move(id);
		d.equipmentType = "Atmosphere compartment";
		d.instanceId = std::move(instance);
		d.exchangeRole = std::move(role);
		d.role = StationMetricRole::Condition;
		d.quantity = quantity;
		d.unit = unit;
		d.observationNote = "Instantaneous compartment state sampled at the observation cadence, "
			"not an interval average. A compartment absent from the snapshot is a gap, never a zero.";
		o.sourceToCanonicalScale = scale;
		o.perTick = false;
		o.cumulativeRate = false;
		auto const instanceId = d.instanceId;
		o.read = [instanceId, member](StationSystemsSnapshot const& snap,
			StationObservationIndex& index) {
			StationMetricSample v;
			auto const& items = snap.atmospheres;
			auto const found = index.find(items, instanceId, [](auto const& a) { return a.id; });
			if (found != items.end()) v.actual = (*found).*member;
			return v;
		};
		result.push_back(std::move(o));
	}

	// Unsigned throughput: every species contributes its own movement
	namespace
{
	// Unsigned throughput: every species contributes its own movement
	// whichever way it is going. A signed net would let oxygen leaving and
	// carbon dioxide arriving cancel into a figure smaller than the gas
	// actually crossing, and an explicitly undirected opening has no usable
	// net direction anyway. This matches the hatch node readout.
	double crossingTotal(StationAtmospherePathwaySnapshot const& pathway)
	{
		double total = 0.0;
		for (auto const rate : pathway.flowRateKgPerS) total += std::abs(rate);
		return total;
	}
}

void appendStationAtmosphereStateObservations(StationSystemsSettings const& settings,
		std::vector<StationExchangeObservation>& result)
	{
		for (auto const& compartment : settings.atmospheres)
		{
			auto const& id = compartment.id;
			addAtmosphereState(result, "atmosphere/" + id + "/temperature", id, "temperature",
				StationPhysicalQuantity::Temperature, StationCanonicalUnit::Celsius, 1.0,
				&StationAtmosphereSnapshot::temperatureC);
			// The snapshot carries relative humidity as 0..1; the Percent
			// unit wants 0..100, so the scale lives here rather than in the
			// chart layer where it would be invisible to a reader.
			addAtmosphereState(result, "atmosphere/" + id + "/relative-humidity", id,
				"relative-humidity", StationPhysicalQuantity::Fraction,
				StationCanonicalUnit::Percent, 100.0,
				&StationAtmosphereSnapshot::relativeHumidity);
			addAtmosphereState(result, "atmosphere/" + id + "/pressure", id, "total-pressure",
				StationPhysicalQuantity::Pressure, StationCanonicalUnit::Kilopascal, 1.0,
				&StationAtmosphereSnapshot::pressureKPa);
			constexpr std::array<double StationAtmosphereSnapshot::* , 4> partials{
				&StationAtmosphereSnapshot::oxygenPartialPressureKPa,
				&StationAtmosphereSnapshot::nitrogenPartialPressureKPa,
				&StationAtmosphereSnapshot::carbonDioxidePartialPressureKPa,
				&StationAtmosphereSnapshot::waterVapourPartialPressureKPa};
			for (size_t gas = 0; gas < partials.size(); ++gas)
				addAtmosphereState(result,
					"atmosphere/" + id + "/partial-pressure/" + StationPathwayGasIds[gas], id,
					std::string("partial-pressure/") + StationPathwayGasIds[gas],
					StationPhysicalQuantity::Pressure, StationCanonicalUnit::Kilopascal, 1.0,
					partials[gas]);
		}
	}

	void appendStationPathwayCrossingObservations(StationSystemsSettings const& settings,
		std::vector<StationExchangeObservation>& result)
	{
		for (auto const& pathway : settings.atmospherePathways)
		{
			auto const id = pathway.id;
			// gasIndex past the end of the array means "every species,
			// unsigned sum" rather than one species on its own.
			auto add = [&](std::string metricId, std::string role, size_t gasIndex)
			{
				StationExchangeObservation o;
				auto& d = o.metric;
				d.id = std::move(metricId);
				d.equipmentType = "Atmosphere pathway";
				d.instanceId = id;
				d.exchangeRole = std::move(role);
				d.role = StationMetricRole::Throughput;
				d.quantity = StationPhysicalQuantity::MassRate;
				d.unit = StationCanonicalUnit::KilogramPerSecond;
				d.observationNote = "Unsigned throughput across the opening at the observation "
					"cadence: the sum of each species' own movement, so opposing flows add rather "
					"than cancel. Matches the hatch node readout. A pathway absent from the "
					"snapshot is a gap, never a zero.";
				o.sourceToCanonicalScale = 1.0;
				o.perTick = false;
				o.cumulativeRate = false;
				o.read = [id, gasIndex](StationSystemsSnapshot const& snap,
					StationObservationIndex& index) {
					StationMetricSample v;
					auto const& items = snap.atmospherePathways;
					auto const found = index.find(items, id, [](auto const& a) { return a.id; });
					if (found == items.end()) return v;
					if (gasIndex < found->flowRateKgPerS.size())
						v.actual = std::abs((*found).flowRateKgPerS[gasIndex]);
					else v.actual = crossingTotal(*found);
					return v;
				};
				result.push_back(std::move(o));
			};
			add("pathway/" + id + "/crossing", "crossing", 4);
			for (size_t gas = 0; gas < StationPathwayGasIds.size(); ++gas)
				add("pathway/" + id + "/crossing/" + StationPathwayGasIds[gas],
					std::string("crossing/") + StationPathwayGasIds[gas], gas);
		}
	}
}
