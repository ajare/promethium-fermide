#include "StationRig.h"
#include "Smoke.h"
#include "core/StationPrefab.h"
#include "core/StationConfigurationDocument.h"
#include <fstream>
#include <sstream>

namespace
{
using namespace smoke::station;
void near(double actual, double expected, char const* message)
{
	require(std::abs(actual - expected) < 1e-7 * std::max(1.0, std::abs(expected)), message);
}
core::PrefabTemplate rtg(smoke::Context const& ctx)
{
	std::ifstream file(ctx.fixture("resources/prefabs/rtg-unit.yaml"));
	std::ostringstream text; text << file.rdbuf();
	core::PrefabCatalogue catalogue; std::string why;
	auto const ok = core::parsePrefabCatalogue(text.str(), catalogue, &why);
	require(ok, ("RTG catalogue: " + why).c_str());
	require(catalogue.find("rtg-unit"), "catalogue has no RTG");
	return *catalogue.find("rtg-unit");
}
core::PrefabExpansion expand(core::PrefabTemplate const& t, core::PrefabOverrides const& overrides)
{
	core::PrefabExpansion e; std::string why;
	auto const ok = core::expandPrefab(t, overrides, "rtg1", e, why);
	require(ok, why.c_str()); return e;
}
core::StationSystemsSettings plant(core::PrefabTemplate const& t, core::PrefabOverrides const& overrides = {})
{
	auto e = expand(t, overrides);
	auto& s = e.entries; s.systemsStepWorldTicks = 60;
	core::ThermalReservoirSettings sink;
	sink.id = "sink"; sink.thermalClass = s.heatEngines[0].thermalClass;
	sink.heatCapacityKJPerK = 10000; sink.startingTemperatureC = 30;
	sink.minTemperatureC = -50; sink.maxTemperatureC = 100;
	s.thermalReservoirs.push_back(sink);
	core::RadiatorSettings radiator;
	radiator.id = "panel"; radiator.reservoirId = "sink";
	radiator.areaM2 = 5; radiator.emissivity = .9; radiator.viewFactor = 1;
	radiator.sinkTemperatureC = -100; s.radiators.push_back(radiator);
	core::EquipmentLoadSettings load;
	load.id = "load"; load.electricalDemandKW = 1000; load.thermalClass = "moderate";
	load.coolingDisposition = core::StationCoolingDisposition::Exported;
	s.equipmentLoads.push_back(load);
	core::PlacedPrefab placed{t.id, "rtg1", e.exposedPins}; std::string why;
	auto const wired = core::applyPrefabLink(s, {placed},
		{{t.id, "rtg1", "Rejection out"}, {"Reservoir", "sink", "Heat in"}}, why);
	require(wired, why.c_str());
	// Power out is an automatic source connection to the default main bus.
	require(e.exposedPins.at("Electrical out") == core::StationPinRef{"Heat engine", "rtg1.thermocouple", "Power out"},
		"electrical boundary did not resolve");
	return s;
}
void configure(core::World& w, core::StationSystemsSettings const& s)
{
	w.pauseSimulation(); w.finishBuild(); std::string why;
	auto const ok = w.configureStationSystems(s, false, &why); require(ok, why.c_str()); w.resumeSimulation();
}
void rtgDials(smoke::Context const& ctx)
{
	auto const t = rtg(ctx);
	require(t.parameters.size() == 5, "RTG must expose exactly five identity-preserving dials");
	for (auto const& p : t.parameters)
	{
		require(p.field != "throttleable", "RTG exposed throttleability");
		for (auto const& target : p.additionalTargets)
			require(target.field != "throttleable", "RTG indirectly exposed throttleability");
	}
	auto e = expand(t, {{"Beginning-of-life thermal output (kW)", 10.0}, {"Thermal class", 2.0}});
	require(core::prefabEntryOrder(e.entries).size() == 3, "RTG must flatten to exactly three entries");
	require(e.entries.heatSources[0].targetReservoirId == "rtg1.hot"
		&& e.entries.heatEngines[0].hotReservoirId == "rtg1.hot", "internal links not namespaced");
	require(!e.entries.heatSources[0].throttleable, "RTG became throttleable");
	near(e.entries.heatSources[0].ratedOutputKW, 10, "source sizing dial failed");
	near(e.entries.heatEngines[0].ratedThermalInputKW, 10, "engine rating not coupled to source");
	near(e.entries.heatEngines[0].commandedInputKW, 10, "engine command not coupled to rating");
	require(e.entries.heatEngines[0].thermalClass == "warm", "class dial failed");
	require(e.exposedPins.at("Hot tap") == core::StationPinRef{"Reservoir", "rtg1.hot", "Heat out"}, "hot tap is not upstream");
	core::PlacedPrefab placed{t.id, "rtg1", e.exposedPins}; std::string why;
	auto const before = e.entries;
	require(!core::applyPrefabParameterOverride(e.entries, t, placed, "Thermal class", 1.5, why), "fractional class accepted");
	require(e.entries == before && placed.overrides.empty(), "refused dial mutated placement");
	require(core::applyPrefabParameterOverride(e.entries, t, placed, "Beginning-of-life thermal output (kW)", .5, why), why.c_str());
	near(e.entries.heatEngines[0].commandedInputKW, .5, "live authoring dial missed coupled target");
	core::PrefabOverrideValue read;
	require(core::readPrefabParameterValue(e.entries, t, "rtg1", "Thermal class", read) && std::get<double>(read) == 2,
		"class readback failed");
	core::PrefabExpansion refused;
	require(!core::expandPrefab(t, {{"throttleable", true}}, "rtg1", refused, why), "hidden dial accepted");
	for (auto const& p : t.parameters)
		for (double value : {p.minValue, p.maxValue})
			expand(t, {{p.name, value}}); // every advertised endpoint is a coherent composition
	// A coupled write cannot partially succeed if an internal target disappeared.
	auto orphan = e.entries; orphan.heatEngines.clear(); auto const orphanBefore = orphan;
	auto const overridesBefore = placed.overrides;
	require(!core::applyPrefabParameterOverride(orphan, t, placed, "Beginning-of-life thermal output (kW)", 8.0, why),
		"missing coupled target accepted");
	require(orphan == orphanBefore && placed.overrides == overridesBefore, "coupled refusal partially wrote the source");
	// The class selector must not bypass the existing external sink compatibility rule.
	auto wired = plant(t); auto const wiredBefore = wired;
	require(!core::applyPrefabParameterOverride(wired, t, placed, "Thermal class", 2.0, why), "incompatible wired class accepted");
	require(wired == wiredBefore, "incompatible class edit mutated draft");
	std::ifstream file(ctx.fixture("resources/prefabs/rtg-unit.yaml"));
	std::ostringstream text; text << file.rdbuf(); auto malformed = text.str();
	auto const field = malformed.find("field: commandedInputKW");
	require(field != std::string::npos, "sizing fixture missing coupled command");
	malformed.replace(field, std::string("field: commandedInputKW").size(), "field: throttleable");
	core::PrefabCatalogue catalogue;
	require(!core::parsePrefabCatalogue(malformed, catalogue, &why) && catalogue.templates.empty(),
		"invalid coupled mapping did not refuse catalogue atomically");
}
void rtgConversionLedger(smoke::Context const& ctx)
{
	auto const t = rtg(ctx); auto s = plant(t);
	core::World w("RTG", 2, 2); configure(w, s);
	auto previous = w.getStationSystemsSnapshot();
	for (int second = 0; second < 120; ++second)
	{
		require(advanceSeconds(w, 1), "RTG drive failed");
		auto const snap = w.getStationSystemsSnapshot(); auto const& engine = snap.heatEngines[0];
		require(snap.healthy, "RTG failed station audit");
		double const source = snap.heatSources[0].deliveredEnergyKJ - previous.heatSources[0].deliveredEnergyKJ;
		double const power = engine.electricalEnergyKJ - previous.heatEngines[0].electricalEnergyKJ;
		double const rejection = engine.rejectionEnergyKJ - previous.heatEngines[0].rejectionEnergyKJ;
		double const storage = s.thermalReservoirs[0].heatCapacityKJPerK *
			(snap.thermal.reservoirs[0].temperatureC - previous.thermal.reservoirs[0].temperatureC);
		near(source, power + rejection + storage, "RTG whole-chain per-tick ledger does not close");
		require(engine.lastRejectionKW > 12 * engine.lastElectricalOutputKW, "RTG is not low-efficiency thermoelectric conversion");
		if (second == 0)
		{
			near(engine.lastElectricalOutputKW, 2 * .115 * (1 - 303.15 / 773.15), "RTG calibration wrong");
			require(snap.power.loads[0].suppliedKW > 0, "RTG power did not reach consumer");
		}
		previous = snap;
	}
	require(previous.thermal.reservoirs[1].rejectedEnergyKJ > 0, "radiator chain rejected no heat");
	auto cooler = s; cooler.thermalReservoirs[0].startingTemperatureC = 300;
	core::World cold("Cooler RTG", 2, 2); configure(cold, cooler);
	require(advanceSeconds(cold, 1), "cooler RTG drive failed");
	require(previous.heatEngines[0].lastElectricalOutputKW > cold.getStationSystemsSnapshot().heatEngines[0].lastElectricalOutputKW,
		"conversion ignored actual hot temperature");
}
void rtgDecayStandalone(smoke::Context const& ctx)
{
	auto const t = rtg(ctx);
	// Actual years at the opening boundary; accelerated isotope for the slope check.
	auto old = plant(t, {{"Initial age (hours)", 30 * 365.25 * 24.0}});
	core::World aged("30-year RTG", 2, 2); configure(aged, old);
	near(aged.getStationSystemsSnapshot().heatSources[0].decayedOutputKW, 2 * std::exp2(-30.0 / 87.7), "30-year opening not mid-curve");
	auto s = plant(t, {{"Half-life (hours)", .01}, {"Initial age (hours)", .01 * 30 / 87.7}});
	core::StationConfigurationDocument doc; doc.settings = s; doc.prefabs.push_back({t.id, "rtg1", {}});
	auto yaml = core::serializeStationConfiguration(doc);
	// Strip metadata from the serialized document, not just the in-memory model.
	auto at = yaml.find("prefabs:"); require(at != std::string::npos, "missing provenance");
	auto end = yaml.find('\n', at);
	while (end != std::string::npos && end + 1 < yaml.size() && yaml[end + 1] == ' ')
		end = yaml.find('\n', end + 1);
	yaml.erase(at, (end == std::string::npos ? yaml.size() : end + 1) - at);
	core::StationConfigurationDocument loaded; std::string why;
	auto const ok = core::parseStationConfiguration(yaml, loaded, &why); require(ok, why.c_str());
	require(loaded.prefabs.empty(), "provenance survived stripping");
	core::World original("Original", 2, 2), standalone("Standalone", 2, 2), idle("Idle", 2, 2);
	configure(original, s); configure(standalone, loaded.settings);
	auto noDemand = s; noDemand.equipmentLoads.clear(); configure(idle, noDemand);
	require(advanceSeconds(original, 36) && advanceSeconds(standalone, 36) && advanceSeconds(idle, 36), "RTG decay drive failed");
	auto const a = original.getStationSystemsSnapshot(), b = standalone.getStationSystemsSnapshot(), c = idle.getStationSystemsSnapshot();
	require(a == b, "flattened standalone simulation diverged");
	near(a.heatSources[0].decayedOutputKW, 2 * std::exp2(-(30.0 / 87.7 + 1)), "RTG decay restarted or wrong slope");
	require(a.heatSources[0].decayFraction == c.heatSources[0].decayFraction, "RTG decay depends on electrical consumption");
	require(a.heatEngines[0].electricalEnergyKJ > 0 && c.heatEngines[0].electricalEnergyKJ == 0, "decay contrast did not exercise demand acceptance");
}
}
void registerStationRtgChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"rtgDials", rtgDials});
	checks.push_back({"rtgConversionLedger", rtgConversionLedger});
	checks.push_back({"rtgDecayStandalone", rtgDecayStandalone});
}
