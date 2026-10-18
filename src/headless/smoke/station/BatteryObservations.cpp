#include "BatteryObservations.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
	void close(double actual, double expected)
	{
		require(std::isfinite(actual) && std::abs(actual - expected) < 1e-8, "battery observation numerical mismatch");
	}
	core::StationSystemsSettings rig(bool charging)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.trendRetentionSeconds = 3600;
		// Deliberately reverse identity order; percentage means 2/10, not
		// the unweighted average of 100% and 0%.
		s.batteries = {{"z-small", 2, 2, 2, 4, 4}, {"a-large", 8, 4, 0, 4, 4}};
		if (charging) s.solarArrays = {{"sun", 2}};
		else
		{
			core::EquipmentLoadSettings load;
			load.id = "load"; load.electricalDemandKW = 1; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			s.equipmentLoads = {load};
		}
		return s;
	}
	std::string document(core::World const& world)
	{
		auto serializer = core::YamlSerializer::toString(); core::SerializationWorkData data;
		world.serialize(*serializer, data); serializer->serialize(); return serializer->getSerializedString();
	}
	void batteryObservations(smoke::Context const&)
	{
		for (bool charging : {false, true})
		{
			core::World world("Battery observations", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			require(world.configureStationSystems(rig(charging), false, &diagnostic), diagnostic.c_str());
			world.markSaved();
			auto const catalogue = world.getStationSystemsObservationCatalogue();
			require(catalogue.size() >= 4 && catalogue[1].id == "battery-energy" && catalogue[2].id == "battery-charge",
				"battery properties were not discovered before recording");
			world.resumeSimulation(); require(world.advanceTicks(60), "battery observation run failed"); world.pauseSimulation();
			require(world.getStationSystemsQueryWork().queries == 0, "recording built unselected views");
			auto const before = world.getStationSystemsSnapshot();
			auto const beforeDocument = document(world);
			auto const revision = world.getStationSystemsObservationRevision();
			auto const energy = world.queryStationSystemsChart("battery-energy");
			auto const charge = world.queryStationSystemsChart("battery-charge");
			require(energy.series.size() == 3 && charge.series.size() == 1 && !energy.incomplete && !charge.incomplete,
				"complete batteries produced incomplete queries");
			require(energy.series[1].label == "a-large" && energy.series[2].label == "z-small"
				&& energy.series[1].stacked && !energy.series[0].stacked, "stack grouping/identity was not stable");
			require(energy.series[0].values.size() == 10 && energy.series[0].times == charge.series[0].times,
				"selecting a battery view lost previously recorded history");
			double stored = 0, capacity = 0;
			for (auto const& battery : before.power.batteries) { stored += battery.storedKWh; capacity += battery.nameplateKWh; }
			close(energy.series[0].values.back(), stored);
			close(charge.series[0].values.back(), stored / capacity * 100);
			require(charging ? stored > 2 : stored < 2, "fixture did not charge/discharge");
			require(charge.series[0].values.back() < 21, "percentages were summed or averaged without capacity weighting");
			auto checkStack = [&](core::StationChartData const& e, core::StationChartData const& c) {
				for (size_t row = 0; row < e.series[0].times.size(); ++row)
				{
					close(e.series[0].values[row], e.series[1].values[row] + e.series[2].values[row]);
					close(e.series[1].lower[row], 0);
					close(e.series[2].lower[row], e.series[1].upper[row]);
					close(e.series[2].upper[row], e.series[0].values[row]);
					close(c.series[0].values[row], e.series[0].values[row] / 10 * 100);
					require(e.series[1].times == e.series[0].times && e.series[2].times == c.series[0].times,
						"stack/sum/ratio timelines differ");
				}
			};
			checkStack(energy, charge);
			require(world.getStationSystemsObservationRevision() == revision && !world.isModified()
				&& world.getStationSystemsSnapshot() == before && document(world) == beforeDocument,
				"battery selection mutated simulation or document state");
			// Fault injection at the public exported observation seam, retaining
			// the applied expected membership rather than rediscovering a subset.
			auto history = world.getStationSystemsMetricHistory();
			auto missing = history;
			std::erase_if(missing, [](auto const& h) { return h.descriptor.id == "battery/a-large/stored-energy"; });
			auto partial = core::queryStationChart(catalogue[1], missing);
			auto partialCharge = core::queryStationChart(catalogue[2], missing);
			require(partial.incomplete && partialCharge.incomplete && std::isnan(partial.series[0].values[0])
				&& std::isnan(partial.series[2].upper[0]) && std::isnan(partialCharge.series[0].values[0]),
				"missing expected contribution became zero or a plausible partial total/stack");
			for (auto& h : history) if (h.descriptor.id == "battery/a-large/capacity") h.samples[0].elapsedSeconds += 0.01;
			auto unaligned = core::queryStationChart(catalogue[2], history);
			require(unaligned.incomplete && std::isnan(unaligned.series[0].values[0])
				&& std::isfinite(unaligned.series[0].values[1]), "ratio joined incompatible timestamps or lost valid spans");
			for (auto& h : history) if (h.descriptor.role == core::StationMetricRole::Capacity)
				h.descriptor.unit = core::StationCanonicalUnit::Kilogram;
			auto const incompatible = core::queryStationChart(catalogue[2], history);
			require(incompatible.incomplete && std::isnan(incompatible.series[0].values[1]),
				"ratio accepted incompatible physical units");
			world.resumeSimulation(); require(world.advanceTicks(1800), "battery coarsening run failed"); world.pauseSimulation();
			auto const coarseEnergy = world.queryStationSystemsChart("battery-energy");
			auto const coarseCharge = world.queryStationSystemsChart("battery-charge");
			require(coarseEnergy.truncated && coarseCharge.truncated && coarseEnergy.series[0].values.size() <= 256
				&& coarseEnergy.sampleIntervalSeconds > energy.sampleIntervalSeconds
				&& coarseEnergy.series[0].times == coarseCharge.series[0].times, "coarsened battery history is unbounded or unaligned");
			checkStack(coarseEnergy, coarseCharge);
			// Real property removal: the catalogue follows the newly applied run.
			auto empty = rig(charging); empty.batteries.clear();
			require(world.configureStationSystems(empty, true), "battery removal failed");
			auto const remaining = world.getStationSystemsObservationCatalogue();
			require(std::none_of(remaining.begin(), remaining.end(), [](auto const& c) { return c.group == "Batteries"; })
				&& world.queryStationSystemsChart("battery-charge").series.empty(), "absent batteries manufactured a percentage view");
		}
		core::World zero("Zero capacity", 2, 2); zero.pauseSimulation(); zero.finishBuild();
		auto s = rig(true); s.batteries = {{"zero", 0, 0, 0}};
		require(zero.configureStationSystems(s, false), "zero-capacity rig refused");
		zero.resumeSimulation(); require(zero.advanceTicks(6), "zero-capacity run failed"); zero.pauseSimulation();
		auto const energy = zero.queryStationSystemsChart("battery-energy");
		auto const charge = zero.queryStationSystemsChart("battery-charge");
		close(energy.series[0].values[0], 0);
		require(charge.incomplete && std::isnan(charge.series[0].values[0]), "zero denominator generated a finite percentage or infinity");
		// Invalid observations must survive coarsening as gaps, not averages
		// over the subset of samples that remained valid.
		auto recorded = zero.getStationSystemsMetricHistory();
		std::vector<core::StationRecordingSlot> slots;
		std::vector<core::StationMetricSample> values;
		for (auto const& h : recorded) { core::StationRecordingSlot slot; slot.metric = h.descriptor; slots.push_back(slot); values.push_back(h.samples[0]); }
		core::StationRecordingLimits limits; limits.samples = 4;
		core::StationHistory recorder(slots, 1, 100, limits);
		for (int t = 1; t <= 8; ++t)
		{
			for (size_t i = 0; i < slots.size(); ++i)
				if (slots[i].metric.role == core::StationMetricRole::Inventory)
					values[i].validity = t == 2 ? core::StationMetricValidity::Invalid : core::StationMetricValidity::Valid;
			recorder.observe(t, values);
		}
		auto const invalid = recorder.query("battery-energy");
		require(invalid.truncated && invalid.incomplete && std::isnan(invalid.series[0].values[0])
			&& std::isnan(invalid.series[1].upper[0]) && invalid.series[0].coverage[0] == 0.5,
			"coarsening turned an invalid energy interval into a complete stack");
	}
}

void registerBatteryObservationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"batteryObservations", batteryObservations});
}
