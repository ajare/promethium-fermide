#include "MaterialObservations.h"
#include "core/World.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
	void close(double actual, double expected)
	{
		require(std::isfinite(actual) && std::abs(actual - expected) < 1e-8, "material inventory numerical mismatch");
	}
	core::ResourceStoreSettings store(std::string id, std::string unit, double capacity,
		double qualified, double unqualified, std::string material = {})
	{
		core::ResourceStoreSettings s;
		s.id = std::move(id); s.unit = std::move(unit); s.capacity = capacity;
		s.startingQualified = qualified; s.startingUnqualified = unqualified; s.materialId = std::move(material);
		return s;
	}
	core::StationSystemsSettings rig()
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		// Labels are deliberately misleading. The second store resolves only
		// through the validated, material-preserving transfer relationship.
		s.stores = {store("oxygen", "kg", 100, 10, 5, "water"), store("spare", "kg", 100, 0, 0),
			store("grams", "g", 10000, 1000, 1000, "water"), store("water", "kg", 100, 3, 1, "oxygen"),
			store("hazard", "kg", 100, 2, 1, "water"), store("mystery", "kg", 100, 7, 2),
			store("litres", "L", 10000, 1000, 500, "water")};
		s.stores[4].hazardClass = "hazardous";
		s.stores[2].composition = core::ResourceCompositionSettings{0.25, 0.1};
		s.stores[5].composition = core::ResourceCompositionSettings{0.1, 1};
		s.stores[0].protectedReserve = 2;
		s.transfers = {{"move", "oxygen", "spare", "kg", 2}};
		return s;
	}
	core::StationChartDefinition chart(core::World const& world, std::string const& id)
	{
		auto const catalogue = world.getStationSystemsObservationCatalogue();
		auto found = std::find_if(catalogue.begin(), catalogue.end(), [&](auto const& c) { return c.id == id; });
		require(found != catalogue.end(), "expected material property missing");
		return *found;
	}
	void run(core::World& world, uint64_t ticks = 60)
	{
		world.resumeSimulation(); require(world.advanceTicks(ticks), "material observation run failed"); world.pauseSimulation();
	}
	void materialObservations(smoke::Context const&)
	{
		core::World world("Materials", 2, 2); world.pauseSimulation(); world.finishBuild();
		auto settings = rig(); std::string diagnostic;
		require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		world.markSaved();
		auto const waterDefinition = chart(world, "stored-material/water/kg");
		require(waterDefinition.series.size() == 5 && waterDefinition.partialAccounting
			&& waterDefinition.accountingNote.find("1 unclassified") != std::string::npos,
			"stored coverage omitted ownership membership or unclassified inventory");
		require(waterDefinition.label.find("Stored material inventory") == 0,
			"Resource-store-only inventory claimed system coverage");
		run(world);
		require(world.getStationSystemsQueryWork().queries == 0, "recording prepared unselected material views");
		auto const before = world.getStationSystemsSnapshot();
		auto const revision = world.getStationSystemsObservationRevision();
		auto const water = world.queryStationSystemsChart("stored-material/water/kg");
		auto const oxygen = world.queryStationSystemsChart("stored-material/oxygen/kg");
		auto const volume = world.queryStationSystemsChart("stored-material/water/m3");
		auto const unknown = world.queryStationSystemsChart("unclassified/mystery");
		require(water.incomplete && water.series[0].values.size() == 10,
			"unknown inventory was hidden or unselected history was not recorded");
		for (size_t row = 0; row < water.series[0].values.size(); ++row)
		{
			close(water.series[0].values[row], 20); // qualified + unqualified, all hazards, grams converted
			close(oxygen.series[0].values[row], 4); // same unit, different material
			close(volume.series[0].values[row], 1.5); // no guessed mass/volume conversion
			close(unknown.series[0].values[row], 9); // quantity visible, never assigned to water
			double stack = 0;
			for (size_t i = 1; i < water.series.size(); ++i)
			{
				close(water.series[i].lower[row], stack); stack += water.series[i].values[row];
				close(water.series[i].upper[row], stack);
			}
			close(stack, water.series[0].values[row]);
		}
		require(before.stores[0].currentQualified < 10 && before.stores[1].currentQualified > 0,
			"fixture did not exercise internal transfer");
		require(world.getStationSystemsSnapshot() == before && !world.isModified()
			&& world.getStationSystemsObservationRevision() == revision
			&& world.getStationSystemsSettings() == settings, "query mutated applied run or authored identity");
		// Missing expected ownership produces gaps, not a plausible smaller sum.
		auto history = world.getStationSystemsMetricHistory();
		require(std::count_if(history.begin(), history.end(), [](auto const& h) {
			return h.descriptor.equipmentType == "Resource store";
		}) == static_cast<int>(settings.stores.size()), "store inventory registered more than once");
		for (auto const& h : history) if (h.descriptor.equipmentType == "Resource store")
			require(!h.descriptor.requestedSemantics && !h.samples[0].requested, "inventory manufactured a requested value");
		std::erase_if(history, [](auto const& h) { return h.descriptor.instanceId == "grams"; });
		auto const missing = core::queryStationChart(waterDefinition, history);
		require(missing.incomplete && std::isnan(missing.series[0].values[0])
			&& std::isnan(missing.series[1].upper[0]), "missing inventory became zero or a complete stack");
		run(world, 1800);
		auto const coarse = world.queryStationSystemsChart("stored-material/water/kg");
		require(coarse.truncated && coarse.series[0].values.size() <= 256, "material history did not remain bounded");
		for (auto value : coarse.series[0].values) close(value, 20);
		auto const oldRun = world.getStationSystemsObservationRevision().run;
		settings.stores[5].materialId = "oxygen";
		require(world.configureStationSystems(settings, true), "identity assignment apply failed");
		require(world.getStationSystemsObservationRevision().run != oldRun
			&& world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].times.empty(),
			"identity application joined incompatible histories");
		require(!chart(world, "stored-material/water/kg").partialAccounting
			&& world.queryStationSystemsChart("unclassified/mystery").series.empty(), "assigned identity stayed unclassified");
		run(world, 6); close(world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].values[0], 13);
		// Failed identity application preserves the old catalogue and history.
		auto const appliedRevision = world.getStationSystemsObservationRevision();
		auto invalid = settings; invalid.stores[5].materialId = "Invalid identity";
		require(!world.configureStationSystems(invalid, true) && world.getStationSystemsObservationRevision() == appliedRevision,
			"refused identity application reset the run");
		// Contradictory evidence anywhere in the transfer component excludes
		// every member; no traversal order or explicit identity gets priority.
		settings.stores[1].materialId = "oxygen";
		require(world.configureStationSystems(settings, true), "conflicting observation metadata changed physics validation");
		require(chart(world, "unclassified/oxygen").partialAccounting && chart(world, "unclassified/spare").partialAccounting,
			"explicit transfer identity conflict was silently reinterpreted");
		run(world, 6); close(world.queryStationSystemsChart("stored-material/water/kg").series[0].values[0], 5);
	}

	void materialCanonicalUnits(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		for (auto unit : core::StationStoreUnits)
			s.stores.push_back(store(unit, unit, 10000, 1, 1, "test-material"));
		core::World world("Canonical units", 2, 2); world.pauseSimulation(); world.finishBuild();
		require(world.configureStationSystems(s, false), "canonical unit fixture refused"); run(world, 6);
		close(world.queryStationSystemsChart("stored-material/test-material/kg").series[0].values[0], 2.002);
		close(world.queryStationSystemsChart("stored-material/test-material/m3").series[0].values[0], 2.002002);
		close(world.queryStationSystemsChart("stored-material/test-material/kWh").series[0].values[0],
			2 + 0.002 + 2.0 / 3600 + 2.0 / 3.6);
		close(world.queryStationSystemsChart("stored-material/test-material/mol").series[0].values[0], 2);
		require(world.getStationSystemsObservationCatalogue().size() == 5,
			"compatible units were split or incompatible physical quantities were merged");
	}

	void materialRelationshipObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("not-water", "kg", 100, 10, 2), store("not-oxygen", "kg", 100, 3, 1),
			store("not-hydrogen", "kg", 100, 1, 1), store("water", "kg", 100, 2, 2)};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "low"; sink.heatCapacityKJPerK = 10000;
		sink.startingTemperatureC = 10; sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		s.thermalReservoirs = {sink};
		core::ElectrolyzerSettings e;
		e.id = "splitter"; e.hazardClass = "normal";
		e.waterStoreId = "not-water"; e.oxygenStoreId = "not-oxygen"; e.hydrogenStoreId = "not-hydrogen";
		e.thermalClass = "low"; e.heatSinkId = "sink"; e.specificEnergyKWhPerKgWater = 4.5;
		e.maxSinkTemperatureC = 60;
		s.electrolyzers = {e}; // known inactive process still supplies validated identity evidence
		core::World world("Relationship identities", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 6);
		close(world.queryStationSystemsChart("stored-material/water/kg").series[0].values[0], 12);
		close(world.queryStationSystemsChart("stored-material/oxygen/kg").series[0].values[0], 4);
		close(world.queryStationSystemsChart("stored-material/hydrogen/kg").series[0].values[0], 2);
		close(world.queryStationSystemsChart("unclassified/water").series[0].values[0], 4);
		require(world.getStationSystemsSettings() == s, "legacy resolution rewrote authored identity");
		// Explicit conflicts with intrinsic evidence are not reinterpreted either.
		s.stores[1].materialId = "argon";
		require(world.configureStationSystems(s, true), "explicit conflict metadata rejected by physics");
		require(chart(world, "unclassified/not-oxygen").partialAccounting
			&& world.queryStationSystemsChart("stored-material/argon/kg").series.empty()
			&& world.queryStationSystemsChart("stored-material/oxygen/kg").series.empty(), "intrinsic conflict guessed an identity");
		// Opposing intrinsic evidence connected through a valid transfer is ambiguous.
		s.stores[1].materialId.clear(); s.transfers = {{"ambiguous", "not-oxygen", "not-hydrogen", "kg", 1}};
		require(world.configureStationSystems(s, true), "ambiguous relationship configuration refused");
		require(chart(world, "unclassified/not-oxygen").partialAccounting
			&& chart(world, "unclassified/not-hydrogen").partialAccounting, "contradictory relationships chose one material");
	}
}

void registerMaterialObservationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"materialObservations", materialObservations});
	checks.push_back({"materialCanonicalUnits", materialCanonicalUnits});
	checks.push_back({"materialRelationshipObservations", materialRelationshipObservations});
}
