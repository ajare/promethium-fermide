#include "ObservationIntegration.h"
#include "MixedObservationFixture.h"
#include "core/StationHistory.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace
{
	using smoke::require;
	void close(double a, double b)
	{
		require((std::isnan(a) && std::isnan(b)) || (std::isfinite(a) && std::isfinite(b)
			&& std::abs(a - b) <= 1e-8 * std::max({1.0, std::abs(a), std::abs(b)})), "mixed chart numerical mismatch");
	}
	std::string document(core::World const& w)
	{
		auto s = core::YamlSerializer::toString(); core::SerializationWorkData data;
		w.serialize(*s, data); s->serialize(); return s->getSerializedString();
	}
	void run(core::World& w, size_t ticks)
	{
		w.resumeSimulation(); require(w.advanceTicks(ticks), "mixed observation run failed"); w.pauseSimulation();
	}
	void observationIntegration(smoke::Context const&)
	{
		core::World w("Mixed Meridian observations", 2, 2); w.pauseSimulation(); w.finishBuild();
		auto settings = smoke::station::mixedObservationFixture(); std::string why;
		if (!w.configureStationSystems(settings, false, &why)) throw std::runtime_error(why);
		w.markSaved();
		require(!w.getStationSystemsRecordingStatus().limited, "mixed station refused recording");
		auto const catalogue = w.getStationSystemsObservationCatalogue();
		require(core::stationSeriesColourKey("battery/audit-small/stored-energy") == 1368418525u,
			"stable contribution colour identity changed");
		run(w, 60);
		auto const firstWork = w.getStationSystemsQueryWork();
		require(firstWork.queries == 0 && firstWork.observationLookups > 0 && firstWork.observationIndexEntries > 0,
			"hidden sampling queried charts or did not use observation indices");
		run(w, 60);
		auto const work = w.getStationSystemsQueryWork();
		require(work.observationIndexEntries == firstWork.observationIndexEntries
			&& work.observationLookups == 2 * firstWork.observationLookups,
			"sampling rebuilt indices or observation work did not scale with boundaries");
		auto const snapshot = w.getStationSystemsSnapshot(); auto const text = document(w);
		auto const revision = w.getStationSystemsObservationRevision();
		auto const histories = w.getStationSystemsMetricHistory();
		std::set<std::string> ids, families;
		for (auto const& h : histories)
		{
			require(ids.insert(h.descriptor.id).second, ("mixed station duplicated metric: " + h.descriptor.id).c_str());
			families.insert(h.descriptor.equipmentType);
			if (h.descriptor.equipmentType == "Airlock cycle" && h.descriptor.role == core::StationMetricRole::Inventory)
			{
				auto const cycle = std::find_if(snapshot.airlockCycles.begin(), snapshot.airlockCycles.end(), [&](auto const& c) { return c.id == h.descriptor.instanceId; });
				require(cycle != snapshot.airlockCycles.end() && h.samples.back().actual.has_value(), "Airlock receiver inventory missing");
				close(*h.samples.back().actual, cycle->recoveredTotal);
			}
			if (h.descriptor.exchangeRole == "Retained source heat")
			{
				auto const owner = h.descriptor.equipmentType == "Generator" ? "Generator" : "Equipment";
				auto const c = std::find_if(snapshot.connections.begin(), snapshot.connections.end(), [&](auto const& c) {
					return c.key.ownerType == owner && c.key.ownerId == h.descriptor.instanceId && c.key.relation == "Heat";
				});
				require(c != snapshot.connections.end() && h.samples.back().actual.has_value(), "retained heat owner missing");
				close(*h.samples.back().actual, c->retainedHeatKJ / 3600);
				require(h.descriptor.thermalClass.has_value(), "retained heat lost thermal class");
			}
		}
		for (auto type : {"Resource store", "Resource transfer", "Resource conversion", "Solar array", "Generator",
			"Battery", "External equipment load group", "Thermal reservoir", "Radiator", "Coolant loop", "Chiller",
			"Thermal buffer", "Atmosphere", "Crew", "Airlock cycle", "Water collector", "Water treatment", "Electrolyzer",
			"CO2 capture", "Fuel cell", "Sabatier", "Methane processor", "Microbial processor", "Crops", "Algae",
			"Resupply", "Waste ejection", "Maintenance"})
			require(families.contains(type), (std::string("mixed audit missing family: ") + type).c_str());
		size_t conversionOutputs = 0;
		for (auto const& c : catalogue)
		{
			if (c.breakdown == "Aggregate" && c.id.starts_with("exchange/Resource conversion/Output/")) ++conversionOutputs;
			auto const before = w.getStationSystemsQueryWork();
			auto const actual = w.queryStationSystemsChart(c.id);
			auto const after = w.getStationSystemsQueryWork();
			require(after.queries == before.queries + 1
				&& after.metricsExported - before.metricsExported == core::stationChartMetricIds(c).size(),
				"selection exported unrelated histories");
			auto const expected = core::queryStationChart(c, histories);
			require(actual.series.size() == expected.series.size(), "mixed selected/exported query populations differ");
			for (size_t i = 0; i < actual.series.size(); ++i)
			{
				auto const& a = actual.series[i]; auto const& b = expected.series[i];
				require(a.id == b.id && a.times == b.times && a.values.size() == 20, "hidden-view history or stable identity lost");
				for (size_t row = 0; row < a.values.size(); ++row) close(a.values[row], b.values[row]);
				if (a.stacked) for (size_t row = 0; row < a.values.size(); ++row) close(a.upper[row] - a.lower[row], a.values[row]);
			}
			(void)w.queryStationSystemsChart(c.id);
			require(w.getStationSystemsQueryWork().queries == after.queries, "unchanged selected query rebuilt");
		}
		require(conversionOutputs == 1, "mixed conversion output disappeared");
		double energy = 0, capacity = 0;
		for (auto const& b : snapshot.power.batteries) { energy += b.storedKWh; capacity += b.nameplateKWh; }
		close(w.queryStationSystemsChart("battery-energy").series[0].values.back(), energy);
		close(w.queryStationSystemsChart("battery-charge").series[0].values.back(), 100 * energy / capacity);
		// Compound inventories are not added to their own constituent decomposition.
		close(w.queryStationSystemsChart("stored-material/audit-product/kg").series[0].values.back(), 0.01 * snapshot.elapsedSeconds);
		require(w.getStationSystemsSnapshot() == snapshot && document(w) == text && !w.isModified()
			&& w.getStationSystemsObservationRevision() == revision, "viewing mutated simulation/document");
		require(!w.advanceTick(), "pause advanced mixed station");
		auto bad = settings; bad.stores.front().capacity = -1;
		require(!w.configureStationSystems(bad, true) && w.getStationSystemsObservationRevision() == revision,
			"refused apply replaced mixed history");
		require(w.configureStationSystems(settings, true) && w.getStationSystemsObservationRevision() == revision,
			"unchanged apply replaced mixed history");
		w.resetSimulation(); require(w.getStationSystemsObservationRevision().run != revision.run, "Reset reused run identity");
		require(w.queryStationSystemsChart("battery-charge").series[0].values.empty(), "Reset retained old history");
		run(w, 120); require(w.getStationSystemsSnapshot() == snapshot, "mixed Reset replay was not deterministic");
		core::World loaded("Replacement", 2, 2); core::SerializationWorkData data;
		auto reader = core::YamlSerializer::fromString(text); reader->deserialize(); loaded.deserialize(*reader, data);
		require(loaded.getStationSystemsSettings() == settings && loaded.getStationSystemsObservationRevision().run != revision.run
			&& loaded.queryStationSystemsChart("battery-energy").series[0].values.empty(), "replacement persisted live observations");
		settings.batteries.clear(); require(w.configureStationSystems(settings, true), "mixed changed apply failed");
		require(w.queryStationSystemsChart("battery-charge").series.empty(), "disappeared mixed property remained applicable");
	}
	void observationBounds(smoke::Context const&)
	{
		// Extreme limits are cheap to reach at the reusable recorder seam, without
		// spending millions of World ticks or introducing another sampling path.
		std::vector<core::StationRecordingSlot> slots;
		for (size_t i = 0; i < 512; ++i)
		{
			core::StationRecordingSlot s; auto& d = s.metric;
			d.id = "store/" + std::to_string(i); d.instanceId = std::to_string(i); d.materialId = "water";
			d.equipmentType = "Resource store"; d.exchangeRole = "stored-inventory";
			d.quantity = core::StationPhysicalQuantity::Mass; d.unit = core::StationCanonicalUnit::Kilogram;
			d.role = core::StationMetricRole::Inventory; d.hazardClass = i % 2 ? "normal" : "hazardous";
			slots.push_back(s); s.metric.id += "/usable"; s.metric.exchangeRole = "usable-inventory"; slots.push_back(s);
		}
		core::StationRecordingLimits limits; limits.samples = 8;
		core::StationHistory history(slots, 1, 100000, limits);
		require(!history.status().limited && history.status().recordingBytes <= limits.memoryBytes, "high cardinality refused or exceeded bound");
		std::vector<core::StationMetricSample> values(slots.size());
		for (auto& v : values) { v.actual = 1; v.validity = core::StationMetricValidity::Valid; }
		for (int t = 1; t <= 4096; ++t)
		{
			values[0].validity = t == 2 ? core::StationMetricValidity::Unavailable : core::StationMetricValidity::Valid;
			history.observe(t, values);
		}
		auto const exported = history.metrics();
		require(history.status().coarseningLevel > 1 && history.status().truncated && exported.size() == 1024,
			"long high-cardinality recording did not coarsen together");
		for (auto const& h : exported)
		{
			require(h.samples.size() <= 8 && h.samples.size() == exported[0].samples.size(), "unbounded or unaligned high-cardinality history");
			for (size_t r = 0; r < h.samples.size(); ++r) require(h.samples[r].elapsedSeconds == exported[0].samples[r].elapsedSeconds, "coarsening grid diverged");
		}
		require(!exported[0].samples[0].actual && exported[0].samples[0].coverage < 1
			&& exported[1].samples[0].actual == 1, "coarsening concealed missing span or lost valid zero/constant");
		for (auto const& c : history.catalogue())
		{
			auto const& q = history.query(c.id);
			for (auto const& line : q.series) require(line.times.size() <= 8, "selected query exceeded row bound");
		}
		limits.samples = 1;
		require(core::StationHistory(slots, 1, 10, limits).status().limited, "minimum two-row failure was not explicit");
		limits = {}; limits.memoryBytes = 4096;
		require(core::StationHistory(slots, 1, 10, limits).status().limited, "minimum memory failure silently selected subset");
		limits = {}; slots[0].metric.observationNote.assign(limits.metadataBytes + 1, 'x');
		require(core::StationHistory(slots, 1, 10, limits).status().limited, "long accounting note bypassed metadata bound");
		slots[0].metric.observationNote.clear(); slots.push_back(slots.back());
		require(core::StationHistory(slots, 1, 10).status().limited, "excessive cardinality silently dropped family");
	}
}
void registerObservationIntegrationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"observationIntegration", observationIntegration});
	checks.push_back({"observationBounds", observationBounds});
}
