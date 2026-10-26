#include "StationRig.h"
#include "Smoke.h"
#include "core/BinarySerializer.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationConnections.h"
#include "core/StationPrefab.h"
#include "core/WorldDocument.h"
#include <limits>

namespace
{
using namespace smoke::station;

core::StationSystemsSettings fixture(double output = 10.0)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	core::ThermalReservoirSettings r;
	r.id = "hot"; r.thermalClass = "generation";
	r.heatCapacityKJPerK = 100.0; r.startingTemperatureC = 20.0;
	r.minTemperatureC = 0.0; r.maxTemperatureC = 40.0;
	s.thermalReservoirs = {r};
	s.heatSources = {{"primary", "hot", "generation", output}};
	return s;
}
void configure(core::World& world, core::StationSystemsSettings const& s)
{
	world.pauseSimulation(); world.finishBuild();
	std::string why;
	auto const configured = world.configureStationSystems(s, false, &why);
	require(configured, why.c_str());
}
void near(double a, double b, char const* why)
{
	require(std::abs(a - b) < 1e-7 * std::max(1.0, std::abs(b)), why);
}
void heatSourceDecay(smoke::Context const&)
{
	auto s = fixture(); s.systemsStepWorldTicks = 60;
	s.thermalReservoirs[0].heatCapacityKJPerK = 1e6;
	core::InstallationWearSettings model;
	model.fadeHalfLifeHours = 0.01;
	model.initialAgeHours = model.fadeHalfLifeHours * 30.0 / 87.7;
	s.heatSources[0].wear = model;
	core::World aged("aged", 2, 2), fresh("fresh", 2, 2), consumed("consumed", 2, 2);
	configure(aged, s);
	near(aged.getStationSystemsSnapshot().heatSources[0].decayedOutputKW,
		10 * std::exp2(-model.initialAgeHours / model.fadeHalfLifeHours), "initial age not applied at configuration");
	auto birth = s; birth.heatSources[0].wear->initialAgeHours = 0;
	configure(fresh, birth);
	auto plant = s;
	auto sink = plant.thermalReservoirs[0]; sink.id = "reject"; sink.startingTemperatureC = 0;
	plant.thermalReservoirs.push_back(sink);
	core::HeatEngineSettings engine;
	engine.id = "converter"; engine.hotReservoirId = "hot"; engine.rejectionReservoirId = "reject";
	engine.thermalClass = "generation"; engine.ratedThermalInputKW = engine.commandedInputKW = 1;
	engine.minHotTemperatureC = 10; engine.maxSinkTemperatureC = 5;
	plant.heatEngines = {engine};
	core::EquipmentLoadSettings load; load.id = "load"; load.electricalDemandKW = 1; load.thermalClass = "moderate";
	load.coolingDisposition = core::StationCoolingDisposition::Exported;
	plant.equipmentLoads = {load};
	configure(consumed, plant);
	aged.resumeSimulation(); fresh.resumeSimulation(); consumed.resumeSimulation();
	require(advanceSeconds(aged, 36) && advanceSeconds(fresh, 36) && advanceSeconds(consumed, 36), "decay drive failed");
	auto a = aged.getStationSystemsSnapshot(); auto f = fresh.getStationSystemsSnapshot();
	auto const expected = std::exp2(-(model.initialAgeHours + a.elapsedSeconds / 3600) / model.fadeHalfLifeHours);
	near(a.heatSources[0].decayedOutputKW, 10 * expected, "age offset did not follow the same exponential curve");
	near(a.heatSources[0].decayedOutputKW / f.heatSources[0].decayedOutputKW,
		std::exp2(-model.initialAgeHours / model.fadeHalfLifeHours), "age offset changed level but not slope");
	require(a.heatSources[0].decayFraction == consumed.getStationSystemsSnapshot().heatSources[0].decayFraction,
		"consumption altered calendar decay");
	require(consumed.getStationSystemsSnapshot().heatEngines[0].lastElectricalOutputKW > 0, "consumer did not actually use heat");
	near(a.heatSources[0].deliveredOutputKW, 10 * std::exp2(-(model.initialAgeHours + (a.elapsedSeconds - a.systemsTimestepSeconds) / 3600) / model.fadeHalfLifeHours),
		"delivered output ignored decay");
	// A live rate change prices only future exposure; initial age and opt-in
	// remain topology, so neither can erase fade while a run is active.
	std::string why;
	s.heatSources[0].wear->fadeHalfLifeHours *= 2;
	require(aged.updateStationSystemsSettings(s, &why), why.c_str());
	require(advanceSeconds(aged, 36), "retuned decay drive failed");
	auto b = aged.getStationSystemsSnapshot();
	near(b.heatSources[0].decayedOutputKW / a.heatSources[0].decayedOutputKW,
		std::exp2(-(b.elapsedSeconds - a.elapsedSeconds) / 3600 / s.heatSources[0].wear->fadeHalfLifeHours), "rate retune repriced banked decay");
	auto invalid = s; invalid.heatSources[0].wear->initialAgeHours = 0;
	require(!aged.updateStationSystemsSettings(invalid, &why), "live initial-age rewrite accepted");
	invalid = s; invalid.heatSources[0].wear.reset();
	require(!aged.updateStationSystemsSettings(invalid, &why), "live decay removal accepted");
}

void heatSourceDecayComposition(smoke::Context const&)
{
	for (auto ceiling : {1.0, 0.6})
	{
		auto s = fixture(); s.systemsStepWorldTicks = 60;
		core::InstallationWearSettings model;
		model.fadeHalfLifeHours = 0.01; model.fadePerCalendarHour = 10;
		model.maxWearFraction = ceiling;
		s.heatSources[0].wear = model;
		core::World world("decay", 2, 2); configure(world, s); world.resumeSimulation();
		require(advanceSeconds(world, 36), "composition drive failed");
		auto snap = world.getStationSystemsSnapshot();
		auto h = snap.elapsedSeconds / 3600;
		near(snap.heatSources[0].decayFraction,
			std::min(ceiling, 1 - (1 - h * model.fadePerCalendarHour) * std::exp2(-h / model.fadeHalfLifeHours)),
			"linear and exponential fade did not compose multiplicatively");
		require(advanceSeconds(world, 360), "clamp drive failed");
		require(world.getStationSystemsSnapshot().heatSources[0].decayFraction == ceiling, "linear fade failed to reach clamp");
		s.heatSources[0].wear->fadePerCalendarHour = 0;
		core::World exponential("exponential", 2, 2); configure(exponential, s); exponential.resumeSimulation();
		require(advanceSeconds(exponential, 3600), "exponential clamp drive failed");
		auto const fraction = exponential.getStationSystemsSnapshot().heatSources[0].decayFraction;
		require(ceiling == 1.0 ? fraction < 1.0 : fraction == ceiling,
			"exponential asymptote or lower Wear clamp is incorrect");
	}
}

void heatSourceDecayPersistence(smoke::Context const&)
{
	auto s = fixture(); s.systemsStepWorldTicks = 60;
	core::InstallationWearSettings model; model.fadeHalfLifeHours = 0.01; model.initialAgeHours = 0.02;
	s.heatSources[0].wear = model;
	core::StationConfigurationDocument doc; doc.settings = s;
	auto bytes = core::serializeStationConfiguration(doc);
	require(bytes.find("schemaVersion: 128") != std::string::npos, "decay schema not claimed");
	core::StationConfigurationDocument loaded; std::string diagnostic;
	require(core::parseStationConfiguration(bytes, loaded, &diagnostic) && loaded.settings == s, "decay settings lost on round trip");
	core::World world("decay", 2, 2); configure(world, s); world.resumeSimulation();
	require(advanceSeconds(world, 10), "decay replay drive failed");
	for (bool binary : {false, true})
	{
		auto out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData data; world.serialize(*out, data); out->serialize();
		auto const saved = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		auto in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(saved))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(saved));
		core::World restored("restored", 2, 2); restored.pauseSimulation(); restored.finishBuild();
		in->deserialize(); restored.deserialize(*in, data);
		require(restored.getStationSystemsSnapshot().heatSources == world.getStationSystemsSnapshot().heatSources,
			"save/load healed accumulated decay");
	}
	auto badWire = bytes;
	auto const field = badWire.find("fadeHalfLifeHours");
	badWire.replace(field, std::string("fadeHalfLifeHours").size(), "unknownDecayField");
	require(!core::parseStationConfiguration(badWire, loaded, &diagnostic), "unknown Wear field accepted");
	badWire = bytes; badWire.replace(badWire.find("schemaVersion: 128"), 18, "schemaVersion: 127");
	require(!core::parseStationConfiguration(badWire, loaded, &diagnostic), "old schema accepted new Wear fields");
	for (auto value : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; bad.heatSources[0].wear->fadeHalfLifeHours = value; std::string why;
		require(!core::validateStationSystemsSettings(bad, why), "invalid half-life accepted");
		bad = s; bad.heatSources[0].wear->initialAgeHours = value;
		require(!core::validateStationSystemsSettings(bad, why), "invalid initial age accepted");
	}
}

void heatSourceTracer(smoke::Context const&)
{
	for (auto ratio : {1u, 6u, 60u})
	{
		auto s = fixture(); s.systemsStepWorldTicks = ratio;
		s.heatSources.push_back({"second", "hot", "generation", 5.0});
		s.thermalReservoirs[0].absorbedHeatKW = 2.0;
		core::World world("PrimaryHeat", 2, 2); configure(world, s);
		world.resumeSimulation(); require(advanceSeconds(world, 60.0), "heat source run failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy && snap.heatSources.size() == 2, "source not published or audit failed");
		auto const& r = snap.thermal.reservoirs.at(0);
		near(r.temperatureC, 30.2, "primary heat did not warm reservoir");
		near(r.primaryHeatInputEnergyKJ, 900.0, "primary input not booked separately");
		near(r.absorbedEnergyKJ, 120.0, "environmental input changed");
		near(r.routedEnergyKJ, 0.0, "primary heat booked as waste");
		near(r.residualKJ, 0.0, "thermal conservation failed");
		near(snap.thermal.totalPrimaryHeatInputKW, 15.0, "primary rollup wrong");
		near(snap.heatSources[0].ratedOutputKW, 10.0, "rating missing");
		near(snap.heatSources[0].deliveredOutputKW, 10.0, "delivery missing");
		near(snap.heatSources[0].deliveredEnergyKJ, 600.0, "source energy wrong");
		near(snap.power.availableGenerationKW, 0.0, "heat source generated electricity");
	}
	// The zero producer is inert, including against an already hot sink.
	core::World zero("ZeroHeat", 2, 2), absent("NoHeat", 2, 2);
	auto s = fixture(0.0); configure(zero, s); s.heatSources.clear(); configure(absent, s);
	zero.resumeSimulation(); absent.resumeSimulation();
	require(advanceSeconds(zero, 30.0) && advanceSeconds(absent, 30.0), "zero-source run failed");
	auto a = zero.getStationSystemsSnapshot(), b = absent.getStationSystemsSnapshot();
	require(a.thermal == b.thermal && a.power == b.power, "zero heat source changed existing services");
	near(a.heatSources[0].deliveredEnergyKJ, 0.0, "disabled source delivered heat");
	// Primary heat uses the same finite, lossy transport ledger as waste heat.
	auto constrained = fixture();
	for (auto const& link : core::station_connections::build(constrained).links)
		if (link.designKey.ownerType == "Heat source") constrained.connections[link.designKey] = {
			core::StationPhysicalConnection::Automatic, 4.0, 0.5};
	core::World capped("CappedPrimaryHeat", 2, 2); configure(capped, constrained);
	capped.resumeSimulation(); require(advanceSeconds(capped, 60.0), "constrained primary run failed");
	auto const c = capped.getStationSystemsSnapshot();
	require(c.healthy, "constrained source failed audit");
	near(c.heatSources[0].deliveredEnergyKJ, 120.0, "constrained source did not honor transport cap/loss");
	near(c.thermal.reservoirs[0].primaryHeatInputEnergyKJ, 120.0, "reservoir counted unreceived primary heat");
	for (auto const& connection : c.connections) if (connection.key.ownerType == "Heat source")
	{
		near(connection.producedHeatKJ, 600.0, "primary production missing from transport ledger");
		near(connection.retainedHeatKJ, 360.0, "unaccepted primary heat disappeared");
		near(connection.lostTotal, 120.0, "primary transport loss missing");
	}
}
void heatSourceLive(smoke::Context const&)
{
	core::World world("LivePrimaryHeat", 2, 2); auto s = fixture(); configure(world, s);
	world.resumeSimulation(); require(advanceSeconds(world, 10.0), "initial live run failed");
	s.heatSources[0].ratedOutputKW = 25.0; std::string why;
	require(world.updateStationSystemsSettings(s, &why), why.c_str());
	require(advanceSeconds(world, 10.0), "retuned run failed");
	near(world.getStationSystemsSnapshot().heatSources[0].deliveredEnergyKJ, 350.0, "live tuning repriced banked heat");
	auto invalid = s; invalid.heatSources[0].targetReservoirId.clear();
	require(!world.updateStationSystemsSettings(invalid, &why), "live endpoint rewiring accepted");
	invalid = s; invalid.heatSources.clear();
	require(!world.updateStationSystemsSettings(invalid, &why), "live source removal accepted");
	s.heatSources[0].ratedOutputKW = 0.0;
	require(world.updateStationSystemsSettings(s, &why), why.c_str());
	require(advanceSeconds(world, 10.0), "disabled live run failed");
	auto const snap = world.getStationSystemsSnapshot();
	require(snap.healthy && snap.heatSources[0].deliveredOutputKW == 0.0, "live zero did not disable source");
	near(snap.heatSources[0].deliveredEnergyKJ, 350.0, "disabled source changed cumulative heat");
}
void heatSourceValidation(smoke::Context const&)
{
	core::World world("HeatRefusals", 2, 2); configure(world, fixture());
	auto const before = world.getStationSystemsSettings();
	auto refused = [&](auto change, char const* expected) {
		auto s = before; change(s); std::string why;
		require(!world.configureStationSystems(s, false, &why), "invalid heat source accepted");
		require(why.find(expected) != std::string::npos, why.c_str());
		require(world.getStationSystemsSettings() == before, "refusal mutated world");
	};
	refused([](auto& s) { s.heatSources[0].ratedOutputKW = -1.0; }, "rated output");
	refused([](auto& s) { s.heatSources[0].ratedOutputKW = std::numeric_limits<double>::infinity(); }, "rated output");
	refused([](auto& s) { s.heatSources[0].ratedOutputKW = std::numeric_limits<double>::quiet_NaN(); }, "rated output");
	refused([](auto& s) { s.heatSources[0].thermalClass = "unknown"; }, "unknown thermal");
	refused([](auto& s) { s.heatSources[0].thermalClass = "warm"; }, "incompatible");
	refused([](auto& s) { s.heatSources[0].targetReservoirId = "missing"; }, "unknown thermal reservoir");
	refused([](auto& s) { s.heatSources[0].id.clear(); }, "id must");
	refused([](auto& s) { s.heatSources.push_back(s.heatSources[0]); }, "duplicated");
	refused([](auto& s) { s.heatSources.resize(core::MaxStationHeatSources + 1); }, "limited");
}
void heatSourcePersistence(smoke::Context const&)
{
	auto s = fixture(); std::string why;
	core::StationConfigurationDocument doc; doc.settings = s;
	auto const text = core::serializeStationConfiguration(doc);
	require(text.find("schemaVersion: 127") != std::string::npos, "source schema not claimed");
	core::StationConfigurationDocument restored;
	require(core::parseStationConfiguration(text, restored, &why), why.c_str());
	require(core::serializeStationConfiguration(restored) == text, "configuration roundtrip not byte stable");
	auto old = text; old.replace(old.find("schemaVersion: 127"), 18, "schemaVersion: 126");
	require(!core::parseStationConfiguration(old, restored, &why), "old schema accepted heat source");
	auto unknown = text; auto pos = unknown.find("ratedOutputKW:");
	unknown.replace(pos, 13, "unknownField:");
	require(!core::parseStationConfiguration(unknown, restored, &why) && why.find("unknown") != std::string::npos,
		"unknown source field silently dropped");
	doc.settings.heatSources.clear();
	auto const legacy = core::serializeStationConfiguration(doc);
	require(legacy.find("heatSources") == std::string::npos && legacy.find("schemaVersion: 111") != std::string::npos,
		"absent source moved legacy document schema/shape");
	for (bool binary : {false, true})
	{
		core::World original("HeatPersistence", 2, 2), loaded("LoadedHeat", 2, 2);
		configure(original, s); loaded.pauseSimulation(); loaded.finishBuild();
		auto save = [&](core::World const& w) {
			auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
			core::SerializationWorkData work; w.serialize(*writer, work); writer->serialize();
			return binary ? static_cast<core::BinarySerializer&>(*writer).getSerializedString()
				: static_cast<core::YamlSerializer&>(*writer).getSerializedString();
		};
		auto const bytes = save(original);
		auto reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		reader->deserialize(); core::SerializationWorkData work; loaded.deserialize(*reader, work);
		require(loaded.getStationSystemsSettings() == s, "world lost heat source settings");
		require(save(loaded) == bytes, "world roundtrip not byte stable");
		original.resumeSimulation(); loaded.resumeSimulation();
		require(advanceSeconds(original, 30.0) && advanceSeconds(loaded, 30.0), "reloaded run failed");
		require(original.getStationSystemsSnapshot().heatSources == loaded.getStationSystemsSnapshot().heatSources,
			"reloaded source changed delivery");
	}
}
void heatSourcePrefab(smoke::Context const&)
{
	core::PrefabTemplate tmpl; std::string why;
	auto const parsed = core::parsePrefabTemplate(R"(
id: heater
displayName: Heater
schemaVersion: 127
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  thermalReservoirs:
    - id: hot
      thermalClass: generation
      heatCapacityKJPerK: 100
      startingTemperatureC: 20
      minTemperatureC: 0
      maxTemperatureC: 40
      absorbedHeatKW: 0
  heatSources:
    - id: primary
      targetReservoirId: hot
      thermalClass: generation
      ratedOutputKW: 10
parameters:
  - name: output
    nodeType: Heat source
    entryId: primary
    field: ratedOutputKW
    kind: number
    min: 0
    max: 100
    default: 10
)", tmpl, &why);
	require(parsed, why.c_str());
	core::PrefabExpansion expanded;
	auto const expandedOk = core::expandPrefab(tmpl, {}, "unit", expanded, why);
	require(expandedOk, why.c_str());
	require(expanded.entries.heatSources.size() == 1, "prefab lost heat source");
	require(expanded.entries.heatSources[0].id == "unit.primary"
		&& expanded.entries.heatSources[0].targetReservoirId == "unit.hot", "prefab did not namespace source wiring");
	core::World world("PrefabHeat", 2, 2); configure(world, expanded.entries);
	world.resumeSimulation(); require(advanceSeconds(world, 10.0), "prefab run failed");
	near(world.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC, 21.0, "prefab source inert");
	auto draft = fixture(); draft.heatSources[0].targetReservoirId.clear();
	core::CandidateLink link{{"Heat source", "primary", "Heat out"}, {"Reservoir", "hot", "Heat in"}};
	require(core::evaluateStationLink(draft, link).allowed, "source link rejected");
	require(core::applyStationLink(draft, link), "source link not written");
	require(([&] { core::StationNodeRename plan; return core::planStationNodeRename(draft, "Reservoir", "hot", "renamed", plan, &why)
		&& core::renameStationNode(draft, plan, &why); }()), why.c_str());
	require(draft.heatSources[0].targetReservoirId == "renamed", "reservoir rename lost source link");
}
void heatSourceOverheat(smoke::Context const&)
{
	auto s = fixture(100.0);
	// No power initially: the load sheds while the primary source warms its sink.
	core::EquipmentLoadSettings load;
	load.id = "load"; load.electricalDemandKW = 1.0; load.heatOutputKW = 1.0;
	load.heatSinkId = "hot"; load.thermalClass = "generation"; load.maxCurtailment = 0.0;
	load.operatingProfile = constantProfile(1.0);
	s.equipmentLoads = {load};
	s.solarArrays = {{"sun", 0.0, constantProfile(1.0)}};
	core::World world("SourceOverheat", 2, 2); configure(world, s);
	world.resumeSimulation(); require(advanceSeconds(world, 30.0), "overheat run failed");
	auto snap = world.getStationSystemsSnapshot();
	require(snap.healthy && snap.thermal.reservoirs[0].temperatureC > 40.0, "source capped at service limit");
	require(std::any_of(snap.activeAlerts.begin(), snap.activeAlerts.end(), [](auto const& alert) {
		return alert.subjectType == "thermal-reservoir" && alert.subjectId == "hot"
			&& alert.monitorId.ends_with(":above-limit") && alert.severity == core::AlertSeverity::Warning;
	}), "reservoir-wide above-limit Warning absent");
	require(snap.power.loads[0].recoveryStage == core::StationLoadRecoveryStage::Shed, "unpowered load did not shed");
	// Restore ample power without changing topology; heat must still bar restart.
	s.solarArrays[0].capacityKW = 10.0; std::string why;
	require(world.updateStationSystemsSettings(s, &why), why.c_str());
	require(advanceSeconds(world, 10.0), "restart run failed");
	snap = world.getStationSystemsSnapshot();
	require(snap.healthy && snap.power.loads[0].recoveryStage == core::StationLoadRecoveryStage::Shed
		&& snap.power.loads[0].recoveryHoldReason == "restart held: heat sink at or above its maximum service temperature",
		"primary-heated sink did not hold load restart");
}
}
void registerStationHeatSourceChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"heatSourceDecay", heatSourceDecay});
	checks.push_back({"heatSourceDecayComposition", heatSourceDecayComposition});
	checks.push_back({"heatSourceDecayPersistence", heatSourceDecayPersistence});
	checks.push_back({"heatSourceTracer", heatSourceTracer});
	checks.push_back({"heatSourceLive", heatSourceLive});
	checks.push_back({"heatSourceValidation", heatSourceValidation});
	checks.push_back({"heatSourcePersistence", heatSourcePersistence});
	checks.push_back({"heatSourcePrefab", heatSourcePrefab});
	checks.push_back({"heatSourceOverheat", heatSourceOverheat});
}
