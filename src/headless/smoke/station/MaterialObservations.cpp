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
		auto const normal = world.queryStationSystemsChart("stored-usable/water/kg/normal");
		auto const hazardous = world.queryStationSystemsChart("stored-usable/water/kg/hazardous");
		auto const hazard = world.queryStationSystemsChart("stored-material/water/kg/by-hazard");
		auto const aggregate = world.queryStationSystemsChart("stored-material/water/kg/aggregate");
		require(aggregate.series.size() == 1 && hazard.series.size() == 3,
			"generic aggregate or hazard grouping membership incorrect");
		for (size_t row = 0; row < normal.series[0].values.size(); ++row)
		{
			close(normal.series[0].values[row], 11); // reserve remains qualified; unqualified never does
			close(hazardous.series[0].values[row], 2); // never folded into normal supply
			close(hazard.series[1].values[row], 3); close(hazard.series[2].values[row], 17);
			close(hazard.series.back().upper[row], aggregate.series[0].values[row]);
			close(normal.series.back().upper[row], normal.series[0].values[row]);
		}
		require(chart(world, "stored-usable/water/kg/normal").accountingNote.find("NOT discretionary drawability") != std::string::npos,
			"usable inventory implied protected reserves were drawable");
		require(before.stores[0].currentQualified < 10 && before.stores[1].currentQualified > 0,
			"fixture did not exercise internal transfer");
		require(world.getStationSystemsSnapshot() == before && !world.isModified()
			&& world.getStationSystemsObservationRevision() == revision
			&& world.getStationSystemsSettings() == settings, "query mutated applied run or authored identity");
		// Missing expected ownership produces gaps, not a plausible smaller sum.
		auto history = world.getStationSystemsMetricHistory();
		require(std::count_if(history.begin(), history.end(), [](auto const& h) {
			return h.descriptor.equipmentType == "Resource store" && !h.descriptor.constituentId;
		}) == static_cast<int>(2 * settings.stores.size()), "total and qualified observations not registered once per store");
		for (auto const& h : history) if (h.descriptor.equipmentType == "Resource store")
			require(!h.descriptor.requestedSemantics && !h.samples[0].requested, "inventory manufactured a requested value");
		std::erase_if(history, [](auto const& h) { return h.descriptor.instanceId == "grams"; });
		auto const missing = core::queryStationChart(waterDefinition, history);
		require(missing.incomplete && std::isnan(missing.series[0].values[0])
			&& std::isnan(missing.series[1].upper[0]), "missing inventory became zero or a complete stack");
		for (auto const& id : {"stored-material/water/kg/by-hazard", "stored-usable/water/kg/normal"})
		{
			auto const gap = core::queryStationChart(chart(world, id), history);
			require(gap.incomplete && std::isnan(gap.series[0].values[0]) && std::isnan(gap.series.back().upper[0]),
				"grouped/usable missing contribution was concealed by zero filling");
		}
		run(world, 1800);
		auto const coarse = world.queryStationSystemsChart("stored-material/water/kg");
		require(coarse.truncated && coarse.series[0].values.size() <= 256, "material history did not remain bounded");
		for (auto value : coarse.series[0].values) close(value, 20);
		auto const coarseHazard = world.queryStationSystemsChart("stored-material/water/kg/by-hazard");
		auto const coarseUsable = world.queryStationSystemsChart("stored-usable/water/kg/normal");
		for (size_t row = 0; row < coarse.series[0].values.size(); ++row)
		{
			require(coarseHazard.series[0].times[row] == coarse.series[0].times[row]
				&& coarseUsable.series[0].times[row] == coarse.series[0].times[row], "coarsened inventories lost alignment");
			close(coarseHazard.series.back().upper[row], coarse.series[0].values[row]);
			close(coarseUsable.series.back().upper[row], coarseUsable.series[0].values[row]);
			close(coarseUsable.series[0].values[row], 11);
		}
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

	void fluidInventoryObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("raw", "kg", 100, 5, 1, "water")};
		core::AtmosphereSettings air;
		air.id = "shared-name"; air.hazardClass = "normal"; air.volumeM3 = 100;
		air.startingOxygenKg = 20; air.startingNitrogenKg = 80;
		air.startingCarbonDioxideKg = 0.5; air.startingWaterVapourKg = 2;
		air.startingTemperatureC = 25; air.thermalCapacityKJPerK = 10000;
		air.minPressureKPa = 1; air.maxPressureKPa = 200;
		s.atmospheres = {air};
		air.id = "hazard"; air.hazardClass = "hazardous"; air.startingWaterVapourKg = 1;
		s.atmospheres.push_back(air);
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "moderate"; sink.heatCapacityKJPerK = 10000;
		sink.startingTemperatureC = 10; sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		s.thermalReservoirs = {sink};
		core::CoolantLoopSettings loop;
		loop.id = "shared-name"; loop.hazardClass = "normal"; loop.reservoirId = "sink";
		loop.coolantCapacityKg = 100; loop.startingCoolantKg = 100; loop.specificHeatKJPerKgK = 4;
		loop.startingTemperatureC = 5; loop.freezeProtectionSetpointC = 0;
		loop.pumpPowerKW = 0.1; loop.ratedFlowKgPerS = 10;
		s.coolantLoops = {loop};
		loop.id = "dry"; loop.startingCoolantKg = 0; s.coolantLoops.push_back(loop);
		core::SolarArraySettings solar; solar.id = "solar"; solar.capacityKW = 10; s.solarArrays = {solar};
		core::WaterCollectorSettings collector;
		collector.id = "collector"; collector.hazardClass = "normal"; collector.atmosphereId = "shared-name";
		collector.coolingLoopId = "shared-name"; collector.thermalClass = "moderate";
		collector.destStoreId = "raw"; collector.maxRateKgPerS = 0.01; collector.targetRelativeHumidity = 0.5;
		s.waterCollectors = {collector};
		core::World world("Fluid owners", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 600);
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.waterCollectors[0].collectedKg > 0, "fixture did not exchange store/atmosphere water");
		auto const water = world.queryStationSystemsChart("accounted-material/water/kg");
		auto const stored = world.queryStationSystemsChart("stored-material/water/kg");
		require(water.incomplete && chart(world, "accounted-material/water/kg").partialAccounting,
			"pending adapters claimed complete system accounting");
		for (size_t row = 0; row < water.series[0].values.size(); ++row)
		{
			close(water.series[0].values[row], 9);
			close(water.series.back().upper[row], 9);
		}
		require(stored.series[0].values.back() > 6, "stored-only boundary did not track condensation");
		close(water.series[0].values.back(), snap.stores[0].currentQualified + snap.stores[0].currentUnqualified
			+ snap.atmospheres[0].waterVapourKg + snap.atmospheres[1].waterVapourKg);
		close(world.queryStationSystemsChart("accounted-material/oxygen/kg").series[0].values.back(), 40);
		close(world.queryStationSystemsChart("accounted-material/nitrogen/kg").series[0].values.back(), 160);
		close(world.queryStationSystemsChart("accounted-material/carbon-dioxide/kg").series[0].values.back(), 1);
		close(world.queryStationSystemsChart("accounted-material/coolant/kg").series[0].values.back(), 100);
		auto const hazard = world.queryStationSystemsChart("accounted-material/water/kg/by-hazard");
		close(hazard.series[1].values.back(), 1); close(hazard.series[2].values.back(), 8);
		auto const equipment = world.queryStationSystemsChart("accounted-material/water/kg/by-equipment");
		close(equipment.series.back().upper.back(), 9);
		// Compartment stock is not qualified store supply, nor circulating flow.
		close(world.queryStationSystemsChart("stored-usable/water/kg/normal").series[0].values.back(),
			snap.stores[0].currentQualified);
		auto histories = world.getStationSystemsMetricHistory();
		require(std::count_if(histories.begin(), histories.end(), [](auto const& h) {
			return h.descriptor.exchangeRole == "retained-inventory";
		}) == 10, "non-store stock registered twice or omitted");
		for (auto const& h : histories) if (h.descriptor.exchangeRole == "retained-inventory")
			require(!h.descriptor.requestedSemantics && !h.samples[0].requested, "stock invented requested semantics");
		std::erase_if(histories, [](auto const& h) { return h.descriptor.id == "atmosphere/shared-name/water/inventory"; });
		for (auto id : {"accounted-material/water/kg", "accounted-material/water/kg/by-equipment",
			"accounted-material/water/kg/by-hazard"})
		{
			auto const missing = core::queryStationChart(chart(world, id), histories);
			require(missing.incomplete && std::isnan(missing.series[0].values[0])
				&& std::isnan(missing.series.back().upper[0]), "missing fluid inventory became a zero or valid stack");
		}
		auto const revision = world.getStationSystemsObservationRevision();
		world.resetSimulation();
		require(world.getStationSystemsObservationRevision().run != revision.run
			&& world.queryStationSystemsChart("accounted-material/water/kg").series[0].times.empty(),
			"Reset joined fluid inventory runs");
		run(world, 600);
		require(world.getStationSystemsSnapshot() == snap, "fluid observation changed deterministic replay");
	}

	void compoundInventoryObservations(smoke::Context const&)
	{
		core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
		s.stores = {store("food", "kg", 100, 8, 2, "food"), store("buffer", "kg", 100, 0, 0),
			store("waste", "g", 10000, 2000, 2000, "organic-waste"),
			store("hazard", "kg", 100, 1, 1, "organic-waste"), store("unknown", "kg", 100, 3, 0)};
		core::ResourceCompositionSettings composition{0.25, 0.1, 0.2, 0.1, 0.1, 0.1, 3600};
		for (size_t i = 0; i < 4; ++i) s.stores[i].composition = composition;
		s.stores[3].hazardClass = "hazardous"; s.stores[0].protectedReserve = 2;
		s.transfers = {{"transfer", "food", "buffer", "kg", 1}};
		// Unqualified waste is retained but not ejectable, so ejection can
		// remove only the qualified half. The loss ledger is not extra stock.
		s.wasteEjections = {{"eject", "waste", "g", 3000, 0.5}};
		core::World world("Compound owners", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		run(world, 60);
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.stores[1].currentQualified > 0 && snap.wasteEjections[0].ejectedTotal == 2000,
			"compound fixture did not transfer/eject material");
		double partition = 0;
		for (auto key : core::StationMassConstituents)
		{
			auto const id = std::string("stored-constituent/") + key + "/kg";
			auto const view = world.queryStationSystemsChart(id);
			require(!view.series.empty() && view.incomplete, "constituent coverage claimed complete system accounting");
			double actual = 0, usable = 0;
			for (size_t i = 0; i < 4; ++i)
			{
				auto const& owner = snap.stores[i];
				actual += owner.actualConstituents.at(key);
				if (owner.hazardClass == "normal")
					usable += owner.actualConstituents.at(key) * owner.currentQualified
						/ (owner.currentQualified + owner.currentUnqualified);
			}
			close(view.series[0].values.back(), actual);
			close(view.series.back().upper.back(), actual);
			partition += actual;
			auto const qualified = world.queryStationSystemsChart(std::string("stored-usable-constituent/") + key + "/kg/normal");
			close(qualified.series[0].values.back(), usable);
			close(qualified.series.back().upper.back(), usable);
			auto const hazard = world.queryStationSystemsChart(id + "/by-hazard");
			close(hazard.series.back().upper.back(), actual);
		}
		close(partition, 14); // 10 food + 2 remaining unqualified waste + 2 hazardous waste
		close(world.queryStationSystemsChart("stored-constituent/water/kg").series[0].values.back(), 3.5);
		close(world.queryStationSystemsChart("stored-constituent/nutrientNitrogen/kg").series[0].values.back(), 1.05);
		close(world.queryStationSystemsChart("stored-constituent/atmosphericNitrogen/kg").series[0].values.back(), 2.1);
		close(world.queryStationSystemsChart("stored-constituent/energy/kWh").series[0].values.back(), 10.5);
		close(world.queryStationSystemsChart("stored-material/food/kg").series[0].values.back(), 10);
		close(world.queryStationSystemsChart("accounted-material/organic-waste/kg").series[0].values.back(), 4);
		require(world.queryStationSystemsChart("accounted-material/water/kg").series.empty(),
			"bound moisture was silently added to free-water material");
		auto const definition = chart(world, "stored-constituent/water/kg");
		require(definition.accountingNote.find("retained waste") != std::string::npos
			&& definition.accountingNote.find("not a system total") != std::string::npos,
			"compound boundary labels missing");
		auto history = world.getStationSystemsMetricHistory();
		for (auto const& h : history) if (h.descriptor.constituentId)
			require(!h.descriptor.materialId && !h.descriptor.requestedSemantics && !h.samples[0].requested,
				"constituent manufactured material ownership or demand");
		std::erase_if(history, [](auto const& h) { return h.descriptor.instanceId == "waste"; });
		auto const missing = core::queryStationChart(definition, history);
		require(std::isnan(missing.series[0].values[0]) && std::isnan(missing.series.back().upper[0]),
			"missing compound owner shrank aggregate membership");
		world.resetSimulation(); run(world, 60);
		require(world.getStationSystemsSnapshot() == snap, "compound recording altered deterministic replay");
	}

	void livingInventoryObservations(smoke::Context const&)
	{
		for (int scenario = 0; scenario < 4; ++scenario)
		{
			core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
			core::ResourceCompositionSettings composition;
			composition.nutrientNitrogen = 0.5; composition.phosphorus = 0.1;
			composition.potassium = 0.1; composition.otherMinerals = 0.1;
			composition.energyKJPerKgDry = 3600;
			s.stores = {store("water", "kg", 1000, 100, 0, "water")};
			for (auto id : {"N", "P", "K", "minerals", "food", "waste", "recovery"})
			{
				s.stores.push_back(store(id, "kg", 100, s.stores.size() < 5 ? 10 : 0, 0, "compound"));
				s.stores.back().composition = composition;
			}
			for (size_t i = 1; i <= 4; ++i)
			{
				core::ResourceCompositionSettings pure;
				if (i == 1) pure.nutrientNitrogen = 1;
				if (i == 2) pure.phosphorus = 1;
				if (i == 3) pure.potassium = 1;
				if (i == 4) pure.otherMinerals = 1;
				s.stores[i].composition = pure;
			}
			core::AtmosphereSettings air;
			air.id = "habitat"; air.hazardClass = "normal"; air.volumeM3 = 100;
			air.startingOxygenKg = 40; air.startingNitrogenKg = 80; air.startingCarbonDioxideKg = 5;
			air.startingWaterVapourKg = 1; air.startingTemperatureC = 22;
			air.thermalCapacityKJPerK = 20000; air.minPressureKPa = 1; air.maxPressureKPa = 200;
			s.atmospheres = {air};
			core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
			auto configure = [&](auto& p) {
				p.id = "same-id"; p.atmosphereId = "habitat"; p.waterStoreId = "water";
				p.nitrogenStoreId = "N"; p.phosphorusStoreId = "P";
				p.potassiumStoreId = "K"; p.mineralsStoreId = "minerals";
				p.composition = composition; p.initialBiomassKg = 1;
				p.lampRatedKW = 3.6; p.circulationPowerKW = 0.1; p.growthYieldKgPerKWh = 0.5;
				p.illuminationProfile.constantLevel = scenario >= 2 ? 0 : 1;
				p.respirationPerHour = scenario >= 2 ? 600 : 0;
				p.foodStoreId = "food"; p.wasteStoreId = scenario == 3 ? "" : "waste";
				p.harvestTriggerKg = scenario == 1 ? 1 : 0; p.harvestFraction = 0.5;
			};
			core::CropPopulationSettings crop; configure(crop); crop.growingAreaM2 = 10;
			core::AlgaeCultureSettings algae; configure(algae); algae.cultureVolumeL = 10000;
			algae.recoveryStoreId = "recovery";
			s.cropPopulations = {crop}; s.algaeCultures = {algae};
			core::World world("Living inventories", 2, 2); world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			if (!world.configureStationSystems(s, false, &diagnostic)) throw std::runtime_error(diagnostic);
			run(world, 300);
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0]; auto const& a = snap.algaeCultures[0];
			auto value = [&](std::string const& id) {
				auto const q = world.queryStationSystemsChart(id);
				require(!q.series.empty() && !q.series[0].values.empty(), "living chart missing");
				return q.series[0].values.back();
			};
			std::string const biomass = "accounted-material/living-biomass/kg";
			close(value(biomass), c.livingBiomassKg + a.livingBiomassKg);
			close(c.livingConstituents.organicKg + c.livingConstituents.nutrientNitrogenKg
				+ c.livingConstituents.phosphorusKg + c.livingConstituents.potassiumKg
				+ c.livingConstituents.mineralsKg, c.livingBiomassKg);
			close(a.livingConstituents.organicKg + a.livingConstituents.nutrientNitrogenKg
				+ a.livingConstituents.phosphorusKg + a.livingConstituents.potassiumKg
				+ a.livingConstituents.mineralsKg, a.livingBiomassKg);
			// Uptake and harvest only move bound nitrogen between owners;
			// uncontained death is the sole loss from this closed boundary.
			close(value("accounted-constituent/nutrientNitrogen/kg"), scenario == 3 ? 10 : 11);
			close(value(biomass + "/aggregate"), value(biomass));
			close(value(biomass + "/by-equipment"), value(biomass));
			auto const stack = world.queryStationSystemsChart(biomass + "/by-equipment");
			close(stack.series.back().upper.back(), value(biomass));
			core::StationBiologicalConstituents living = c.livingConstituents;
			living += a.livingConstituents;
			double const masses[] = {living.organicKg, living.nutrientNitrogenKg, living.phosphorusKg,
				living.potassiumKg, living.mineralsKg};
			char const* keys[] = {"dryOrganics", "nutrientNitrogen", "phosphorus", "potassium", "otherMinerals"};
			for (size_t i = 0; i < 5; ++i)
			{
				auto id = std::string("accounted-constituent/") + keys[i] + "/kg";
				double expected = masses[i];
				for (auto const& owner : snap.stores) if (!owner.actualConstituents.empty())
					expected += owner.actualConstituents.at(keys[i]);
				close(value(id), expected); close(value(id + "/aggregate"), expected);
				close(world.queryStationSystemsChart(id + "/by-equipment").series.back().upper.back(), expected);
				close(value(id), value(std::string("stored-constituent/") + keys[i] + "/kg") + masses[i]);
			}
			close(value("accounted-constituent/energy/kWh"), value("stored-constituent/energy/kWh")
				+ (c.livingChemicalEnergyKJ + a.livingChemicalEnergyKJ) / 3600);
			// No inferred culture liquid or biological moisture; free water and
			// compound water remain their existing, disjoint boundaries.
			close(value("accounted-constituent/water/kg"), 0);
			for (auto const& h : world.getStationSystemsMetricHistory())
				if (h.descriptor.equipmentType == "Crop population" || h.descriptor.equipmentType == "Algae culture")
					require(h.descriptor.materialId == "living-biomass" || h.descriptor.constituentId,
						"biology invented a water or capacity inventory");
			if (scenario == 0) require(c.livingBiomassKg > 1 && a.livingBiomassKg > 1, "biology did not grow");
			if (scenario == 1) require(c.harvestedKg > 0 && a.harvestedKg > 0, "biology did not harvest");
			if (scenario >= 2) require(c.dead && a.dead, "biology did not starve");
			auto const definition = chart(world, "accounted-constituent/nutrientNitrogen/kg");
			require(definition.partialAccounting && definition.accountingNote.find("biological water") != std::string::npos,
				"biological coverage not declared");
			auto history = world.getStationSystemsMetricHistory();
			std::erase_if(history, [](auto const& h) { return h.descriptor.equipmentType == "Algae culture"; });
			auto const missing = core::queryStationChart(definition, history);
			require(std::isnan(missing.series[0].values.back()) && std::isnan(missing.series.back().upper.back()),
				"unavailable biological owner silently shrank membership");
			world.resetSimulation(); run(world, 300);
			require(world.getStationSystemsSnapshot() == snap, "living observation changed deterministic Reset");
		}
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
		auto const catalogue = world.getStationSystemsObservationCatalogue();
		require(std::count_if(catalogue.begin(), catalogue.end(), [](auto const& c) {
			return c.id.starts_with("stored-material/") && c.id == c.propertyId;
		}) == 4, "compatible units were split or incompatible physical quantities were merged");
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
	checks.push_back({"fluidInventoryObservations", fluidInventoryObservations});
	checks.push_back({"compoundInventoryObservations", compoundInventoryObservations});
	checks.push_back({"livingInventoryObservations", livingInventoryObservations});
	checks.push_back({"materialCanonicalUnits", materialCanonicalUnits});
	checks.push_back({"materialRelationshipObservations", materialRelationshipObservations});
}
