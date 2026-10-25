#include "StationRig.h"
#include "Smoke.h"
#include "core/StationConfigurationDocument.h"

namespace
{
using smoke::require;
void table(smoke::Context const&)
{
	core::StationSystemsSettings s;
	s.waterTreatments.resize(1); s.waterTreatments[0].id = "water";
	s.electrolyzers.resize(1); s.electrolyzers[0].id = "electro";
	s.co2Captures.resize(1); s.co2Captures[0].id = "capture";
	s.sabatierReactors.resize(1); s.sabatierReactors[0].id = "sabatier";
	s.methaneProcessors.resize(1); s.methaneProcessors[0].id = "methane";
	s.fuelCells.resize(1); s.fuelCells[0].id = "fuel";
	s.microbialProcessors.resize(1); s.microbialProcessors[0].id = "microbial";
	s.cropPopulations.resize(1); s.cropPopulations[0].id = "crop";
	s.algaeCultures.resize(1); s.algaeCultures[0].id = "algae";
	core::applyStationEclssBiologyDefaultMonitors(s);
	require(s.monitors.size() == 12, "ECLSS/biology default coverage differs");
	std::string diagnostic;
	require(core::validateStationMonitors(s, diagnostic), diagnostic.c_str());
	for (auto const& m : s.monitors)
	{
		require(!m.enabled && m.provenance == core::MonitorProvenance::Rated, "default enabled or not rated");
		core::AlertRule expected{0.1, core::AlertDirection::Above, 120, 60};
		auto severity = core::AlertSeverity::Warning;
		if (m.metric == "condition") { expected = {0.5, core::AlertDirection::Below, 300, 120}; severity = core::AlertSeverity::Advisory; }
		if (m.metric == "biomassFraction") expected = {0.2, core::AlertDirection::Below, 3600, 600};
		if (m.metric == "unmetResourceFraction") expected = {0.1, core::AlertDirection::Above, 600, 300};
		require(m.rule == expected && m.severity == severity, "default rule differs from specification");
	}
	auto original = s.monitors;
	core::applyStationEclssBiologyDefaultMonitors(s);
	require(s.monitors == original, "default expansion not idempotent");
	s.monitors[0].rule.threshold = 0.7;
	core::applyStationEclssBiologyDefaultMonitors(s);
	require(s.monitors[0].rule.threshold == 0.7, "expansion overwrote tuning");
	// Readers share demand-normalisation, zero-demand and missing-subject semantics.
	core::StationSystemsSnapshot snap; snap.systemsTimestepSeconds = 0.25;
	snap.waterTreatments.resize(1); auto& water = snap.waterTreatments[0]; water.id = "water"; water.throughputKgPerS = 8; water.lastProcessedKg = 1;
	snap.electrolyzers.resize(1); auto& electro = snap.electrolyzers[0]; electro.id = "electro"; electro.lastRequestedKg = 2; electro.lastSplitKg = 1;
	snap.co2Captures.resize(1); auto& capture = snap.co2Captures[0]; capture.id = "capture"; capture.lastRequestedKg = 2; capture.lastCapturedKg = 1;
	snap.sabatierReactors.resize(1); auto& sabatier = snap.sabatierReactors[0]; sabatier.id = "sabatier"; sabatier.lastRequestedKg = 2; sabatier.lastCo2ConsumedKg = 1;
	snap.methaneProcessors.resize(1); auto& methane = snap.methaneProcessors[0]; methane.id = "methane"; methane.lastRequestedKg = 2; methane.lastMethaneConsumedKg = 1;
	snap.fuelCells.resize(1); auto& fuel = snap.fuelCells[0]; fuel.id = "fuel"; fuel.lastOutputKW = 1; fuel.lastUnmetOutputKW = 1;
	snap.microbialProcessors.resize(1); auto& microbial = snap.microbialProcessors[0]; microbial.id = "microbial"; microbial.lastRequestedKg = 2; microbial.lastFeedConsumedKg = 1; microbial.condition = 0.4;
	for (auto const& m : original)
		if (m.metric == "unmetFraction")
		{
			require(core::readStationMonitorMetric(s, snap, m.subjectType, m.subjectId, m.metric) == 0.5, "service fraction wrong");
			require(!core::readStationMonitorMetric(s, snap, m.subjectType, "missing", m.metric), "missing subject supplied a sample");
		}
	require(core::readStationMonitorMetric(s, snap, "microbial-processor", "microbial", "condition") == 0.4, "Condition reader wrong");
	electro.lastRequestedKg = electro.lastSplitKg = 0;
	require(core::readStationMonitorMetric(s, snap, "electrolyzer", "electro", "unmetFraction") == 0, "zero demand divided by zero");
}
core::StationSystemsSettings biology(bool algae, bool noLight)
{
	core::StationSystemsSettings s; s.enabled = true; s.systemsStepWorldTicks = 1;
	for (auto const* id : {"water", "n", "p", "k", "minerals", "waste"})
		s.stores.push_back({id, "kg", 10000, 5000});
	core::AtmosphereSettings air; air.id = "air"; air.volumeM3 = 100;
	air.startingOxygenKg = 20; air.startingNitrogenKg = 80;
	air.startingCarbonDioxideKg = noLight ? 10 : 0;
	air.startingTemperatureC = 22; air.thermalCapacityKJPerK = 1e9;
	air.minPressureKPa = 1; air.maxPressureKPa = 200;
	s.atmospheres = {air};
	core::SolarArraySettings solar; solar.id = "solar"; solar.capacityKW = noLight ? 0 : 100;
	s.solarArrays = {solar};
	auto setup = [](auto& c) {
		c.id = "population"; c.atmosphereId = "air"; c.waterStoreId = "water";
		c.nitrogenStoreId = "n"; c.phosphorusStoreId = "p"; c.potassiumStoreId = "k";
		c.mineralsStoreId = "minerals"; c.wasteStoreId = "waste";
		c.initialBiomassKg = 1; c.lampRatedKW = 1; c.circulationPowerKW = 0;
		c.composition.energyKJPerKgDry = 1700;
		c.illuminationProfile.constantLevel = 1; c.respirationPerHour = 0;
		c.minGrowingTemperatureC = 10; c.maxGrowingTemperatureC = 35;
	};
	if (algae) { core::AlgaeCultureSettings c; setup(c); c.cultureVolumeL = 100; c.maxBiomassDensityKgPerL = 1; s.algaeCultures = {c}; }
	else { core::CropPopulationSettings c; setup(c); c.growingAreaM2 = 100; c.maxBiomassDensityKgPerM2 = 1; s.cropPopulations = {c}; }
	core::applyStationEclssBiologyDefaultMonitors(s);
	return s;
}
void boundaries(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (auto dt : {0.25f, 1.0f})
	{
		// Real electrically starved electrolyzer: demand stays present, no power is conjured.
		core::StationSystemsSettings s; s.enabled = true; s.systemsStepWorldTicks = 1;
		s.stores = {{"water", "kg", 10000, 5000}, {"o2", "kg", 10000, 0}, {"h2", "kg", 10000, 0}};
		core::ThermalReservoirSettings sink; sink.id = "sink"; sink.thermalClass = "low"; sink.heatCapacityKJPerK = 1e9; sink.startingTemperatureC = 20;
		s.thermalReservoirs = {sink};
		core::ElectrolyzerSettings e; e.id = "electro"; e.waterStoreId = "water"; e.oxygenStoreId = "o2"; e.hydrogenStoreId = "h2"; e.heatSinkId = "sink"; e.thermalClass = "low"; e.rateKgPerS = 0.001;
		s.electrolyzers = {e};
		core::SolarArraySettings solar; solar.id = "power"; solar.capacityKW = 0; s.solarArrays = {solar};
		core::applyStationEclssBiologyDefaultMonitors(s); s.monitors[0].enabled = true;
		std::string diagnostic;
		require(core::validateStationSystemsSettings(s, diagnostic), diagnostic.c_str());
		core::StationSystemsRuntime run(s, dt);
		if (!run.advanceTo(120 - dt))
		{
			for (auto const& d : run.diagnostics()) std::fprintf(stderr, "%s\n", d.message.c_str());
			require(false, "machine advance failed");
		}
		require(run.snapshot().activeAlerts.empty(), "machine fired early");
		require(run.advanceTo(120), "machine boundary drive failed");
		auto snap = run.snapshot();
		require(snap.activeAlerts.size() == 1 && snap.activeAlerts[0].firedAtSeconds == 120
			&& snap.activeAlerts[0].severity == core::AlertSeverity::Warning, "machine Alert boundary wrong");
		s.solarArrays[0].capacityKW = 100;
		require(run.updateSettings(s), "machine recovery command refused");
		require(run.advanceTo(180 - dt) && run.snapshot().activeAlerts.size() == 1, "machine cleared early");
		require(run.advanceTo(180) && run.snapshot().activeAlerts.empty()
			&& run.snapshot().closedAlerts[0].clearedAtSeconds == 180, "machine clear boundary wrong");
		// Both populations, missing lamp power and missing CO2, at the exact default boundary.
		for (bool algae : {false, true}) for (bool light : {false, true})
		{
			auto b = biology(algae, light);
			for (auto& m : b.monitors) m.enabled = m.metric == "unmetResourceFraction";
			auto valid = core::validateStationSystemsSettings(b, diagnostic);
			require(valid, ("biology fixture: " + diagnostic).c_str());
			core::StationSystemsRuntime population(b, dt);
			require(population.advanceTo(600 - dt) && population.snapshot().activeAlerts.empty(), "starvation fired early");
			require(population.advanceTo(600), "starvation boundary drive failed");
			auto p = population.snapshot();
			require(p.activeAlerts.size() == 1 && p.activeAlerts[0].firedAtSeconds == 600
				&& p.activeAlerts[0].currentValue > 0.1 && p.activeAlerts[0].provenance == core::MonitorProvenance::Rated,
				"starvation Alert missing or wrong timing");
			// Deliberate dark phase requests no light or growth resources; not starvation.
			if (algae) b.algaeCultures[0].illuminationProfile.constantLevel = 0;
			else b.cropPopulations[0].illuminationProfile.constantLevel = 0;
			auto updated = population.updateSettings(b, &diagnostic);
			require(updated, ("biology update: " + diagnostic).c_str());
			require(population.advanceTo(900 - dt) && population.snapshot().activeAlerts.size() == 1, "starvation cleared early");
			require(population.advanceTo(900) && population.snapshot().activeAlerts.empty()
				&& population.snapshot().closedAlerts[0].clearedAtSeconds == 900, "starvation clear boundary wrong");
			if (!light)
			{
				// Disabled defaults stay silent even with low biomass. Enable only
				// that rule in a fresh run and prove its hour-long persistence.
				for (auto& m : b.monitors) m.enabled = false;
				core::StationSystemsRuntime disabled(b, dt);
				require(disabled.advanceTo(3600) && disabled.snapshot().activeAlerts.empty()
					&& disabled.snapshot().closedAlerts.empty(), "disabled biology defaults emitted Alerts");
				for (auto& m : b.monitors) m.enabled = m.metric == "biomassFraction";
				core::StationConfigurationDocument doc; doc.settings = b;
				core::StationConfigurationDocument loaded;
				auto parsed = core::parseStationConfiguration(core::serializeStationConfiguration(doc), loaded, &diagnostic);
				require(parsed && loaded.settings == b, "biology Monitor persistence differs");
				core::StationSystemsRuntime low(b, dt);
				require(low.advanceTo(3600 - dt) && low.snapshot().activeAlerts.empty(), "biomass fired early");
				require(low.advanceTo(3600) && low.snapshot().activeAlerts.size() == 1
					&& low.snapshot().activeAlerts[0].firedAtSeconds == 3600, "biomass Alert boundary wrong");
			}
		}
	}
}
}
void registerStationEclssBiologyMonitorChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"eclssBiologyMonitorTable", table});
	checks.push_back({"eclssBiologyMonitorBoundaries", boundaries});
}
