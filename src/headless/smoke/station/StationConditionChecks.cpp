#include "StationRig.h"
#include "Smoke.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"

namespace
{
using namespace smoke::station;
core::StationSystemsSettings conditionStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::ThermalReservoirSettings sink;
	sink.id = "sink"; sink.thermalClass = "moderate";
	sink.heatCapacityKJPerK = 10000; sink.startingTemperatureC = 20;
	s.thermalReservoirs = {sink};
	core::CoolantLoopSettings loop;
	loop.id = "loop"; loop.reservoirId = "sink";
	loop.coolantCapacityKg = loop.startingCoolantKg = 100;
	loop.specificHeatKJPerKgK = 4; loop.startingTemperatureC = 20;
	loop.ratedFlowKgPerS = 10; loop.pumpPowerKW = 1;
	loop.condition = core::InstallationConditionSettings{};
	loop.condition->seizureThreshold = 0.15;
	loop.condition->restoreStoreId = "stock";
	loop.condition->restorePerHour = 360; // 0.1 point/s: deliberately accelerated fixture
	loop.condition->stockUnitsPerConditionPoint = 10;
	s.coolantLoops = {loop};
	core::ResourceStoreSettings stock;
	stock.id = "stock"; stock.unit = "kg";
	stock.capacity = stock.startingQualified = 10; stock.protectedReserve = 2;
	s.stores = {stock};
	core::SolarArraySettings solar;
	solar.id = "solar"; solar.capacityKW = 10;
	s.solarArrays = {solar};
	return s;
}
void conditionStress(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (bool critical : {false, true})
	{
		auto s = conditionStation(); auto& loop = s.coolantLoops[0];
		loop.criticalService = critical;
		loop.condition->overheatStressC = 10;
		loop.condition->overheatDegradePerHour = 360;
		core::StationSystemsRuntime run(s, 0.25f);
		require(run.advanceTo(2), "Condition degradation failed audit");
		auto snap = run.snapshot();
		require(std::abs(snap.coolantLoops[0].condition - 0.8) < 1e-12
			&& snap.coolantLoops[0].conditionStressors == "overheat"
			&& std::abs(snap.coolantLoops[0].flowKgPerS - 8) < 1e-10
			&& snap.coolantLoops[0].conditionCapability == snap.coolantLoops[0].condition,
			"Condition did not continuously scale delivered capability");
		require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
			{ l.condition->overheatStressC = 100; }), "stress removal refused");
		require(run.advanceTo(4) && run.snapshot().coolantLoops[0].condition == snap.coolantLoops[0].condition
			&& run.snapshot().coolantLoops[0].conditionStressors.empty(), "removed stress kept degrading");
	}
	// Cavitation must still act after a coolant-leak, but not on an isolated pump.
	auto s = conditionStation();
	s.coolantLoops[0].startingCoolantKg = 20;
	s.coolantLoops[0].condition->cavitationDegradePerHour = 360;
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1) && run.snapshot().coolantLoops[0].conditionStressors == "cavitation",
		"low-charge running pump did not cavitate");
	core::InstallationOutageSettings outage;
	outage.id = "isolate"; outage.installationId = "loop"; outage.capacityFraction = 0;
	outage.firstOccurrenceSeconds = 0; outage.durationSeconds = 10;
	s.outages = {outage};
	core::StationSystemsRuntime idle(s, 0.25f);
	require(idle.advanceTo(1) && idle.snapshot().coolantLoops[0].condition == 1,
		"isolated pump cavitated");
}
void conditionCrossing(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = conditionStation();
	auto& model = *s.coolantLoops[0].condition;
	model.initialCondition = 0.2; model.overheatStressC = 10; model.overheatDegradePerHour = 360;
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "seizure crossing failed audit");
	auto snap = run.snapshot(); auto const& loop = snap.coolantLoops[0];
	require(loop.activeFaults.size() == 1 && loop.activeFaults[0].modeId == "pump-seizure"
		&& loop.activeFaults[0].causeKind == core::FaultCause::Kind::DegradationCrossing
		&& loop.activeFaults[0].onsetTick <= snap.worldTick
		&& loop.activeFaults[0].derivedImpact == 1 && loop.availabilityFactor == 0 && loop.flowKgPerS == 0,
		"crossing did not spawn an attributed, service-stopping seizure");
	require(run.advanceTo(5), "post-seizure drive failed");
	require(std::count_if(run.events().begin(), run.events().end(), [](auto const& e)
		{ return e.category == "fault"; }) == 1, "seizure onset duplicated");
	// Distinct modes coexist: a leak may precede a degradation crossing.
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room}; s.coolantLoops[0].coolantLeak = core::CoolantLeakSettings{"room", 40};
	core::StationSystemsRuntime both(s, 0.25f);
	require(both.injectFault("loop", "coolant-leak", {}) && both.advanceTo(1)
		&& both.snapshot().coolantLoops[0].activeFaults.size() == 2
		&& both.snapshot().coolantLoops[0].flowKgPerS == 0,
		"leak suppressed the independent pump-seizure mode");
}
void conditionRestore(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = conditionStation(); s.coolantLoops[0].condition->initialCondition = 0.5;
	s.stores[0].startingQualified = 3; // only one unit above reserve: 0.1 point funded
	core::StationSystemsRuntime run(s, 0.25f);
	auto before = run.snapshot(); auto events = run.events(); std::string diagnostic;
	for (double target : {0.5, -1., 2., std::numeric_limits<double>::quiet_NaN()})
		require(!run.restoreCondition("loop", target, &diagnostic) && !diagnostic.empty(), "invalid restore accepted");
	require(!run.restoreCondition("missing", 1, &diagnostic)
		&& run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
		"refused restore mutated runtime");
	require(run.restoreCondition("loop", 1, &diagnostic) && diagnostic.empty()
		&& run.snapshot() == before && run.events() == events, "restore healed before tick boundary");
	require(run.advanceTo(0.25), "funded recovery failed audit");
	auto snap = run.snapshot();
	require(std::abs(snap.coolantLoops[0].condition - 0.525) < 1e-12
		&& snap.coolantLoops[0].restoring && snap.stores[0].currentQualified == 2.75,
		"restore was not rate-bounded and stock-funded");
	require(run.advanceTo(3), "stock-limited recovery failed audit");
	snap = run.snapshot();
	require(std::abs(snap.coolantLoops[0].condition - 0.6) < 1e-12
		&& snap.stores[0].currentQualified == 2 && !snap.coolantLoops[0].restoring
		&& std::abs(snap.coolantLoops[0].restoreStockConsumedUnits - 1) < 1e-12,
		"recovery consumed reserve or healed for free");
	require(!run.restoreCondition("loop", 1, &diagnostic) && diagnostic.find("reserve") != std::string::npos,
		"unfunded restore was not explained");
	// Recovery under a stressor waits without spending stock.
	s = conditionStation(); s.coolantLoops[0].condition->initialCondition = 0.5;
	s.coolantLoops[0].condition->overheatStressC = 10;
	s.coolantLoops[0].condition->overheatDegradePerHour = 36;
	core::StationSystemsRuntime stressed(s, 0.25f);
	require(stressed.restoreCondition("loop", 0.6) && stressed.advanceTo(1)
		&& stressed.snapshot().stores[0].currentQualified == 10, "stressed restore spent stock");
	auto const stressedCondition = stressed.snapshot().coolantLoops[0].condition;
	require(stressed.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.condition->overheatDegradePerHour = 0; })
		&& stressed.advanceTo(2) && stressed.snapshot().stores[0].currentQualified == 10
		&& stressed.snapshot().coolantLoops[0].condition == stressedCondition,
		"zero-rate active stressor allowed recovery");
	require(stressed.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.condition->overheatStressC = 100; })
		&& stressed.advanceTo(4), "benign restore failed");
	require(stressed.snapshot().coolantLoops[0].condition == 0.6
		&& stressed.snapshot().coolantLoops[0].restoreTargetCondition == 0,
		"recovery did not stop at target");
}
template<class Serializer>
void conditionRoundTrip(core::StationSystemsRuntime const& run)
{
	auto out = Serializer::toString();
	out->beginMap("root"); run.writeFaultReplay(*out); out->endMap(); out->serialize();
	auto const bytes = out->getSerializedString();
	auto in = Serializer::fromString(bytes); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"Condition replay lost exact state or events");
	auto again = Serializer::toString();
	again->beginMap("root"); restored->writeFaultReplay(*again); again->endMap(); again->serialize();
	require(again->getSerializedString() == bytes, "Condition replay bytes changed");
}
void conditionReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = conditionStation();
	s.coolantLoops[0].condition->overheatStressC = 10;
	s.coolantLoops[0].condition->overheatDegradePerHour = 360;
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(2) && run.hasFaultReplayState() && !run.hasFaultCommands(), "autonomous Condition state not saveable");
	conditionRoundTrip<core::YamlSerializer>(run); conditionRoundTrip<core::BinarySerializer>(run);
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.condition->overheatStressC = 100; })
		&& run.restoreCondition("loop", 1), "restore setup failed");
	conditionRoundTrip<core::YamlSerializer>(run); conditionRoundTrip<core::BinarySerializer>(run);
	require(run.advanceTo(5), "recovery drive failed");
	conditionRoundTrip<core::YamlSerializer>(run); conditionRoundTrip<core::BinarySerializer>(run);
	core::StationSystemsRuntime seized(s, 0.25f);
	require(seized.advanceTo(10) && !seized.snapshot().coolantLoops[0].activeFaults.empty(), "autonomous seizure failed");
	conditionRoundTrip<core::YamlSerializer>(seized); conditionRoundTrip<core::BinarySerializer>(seized);
	// A World with no leak opt-in and no commands must still retain autonomous degradation.
	core::World world("Condition", 2, 2); world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	world.resumeSimulation(); require(advanceSeconds(world, 2), "World Condition drive failed"); world.pauseSimulation();
	for (bool binary : {false, true})
	{
		auto out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work; world.serialize(*out, work); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		auto in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); core::World restored("Restored", 2, 2);
		require(restored.deserialize(*in, work), "World Condition reload failed");
		require(restored.getStationSystemsSnapshot() == world.getStationSystemsSnapshot(), "World load reset Condition");
	}
}
void conditionTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = conditionStation();
	s.coolantLoops[0].condition->initialCondition = 0.5;
	core::StationSystemsRuntime run(s, 0.25f);
	auto before = run.snapshot(); auto events = run.events();
	require(run.restoreCondition("loop", 1), "rollback restore refused");
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.specificHeatKJPerKgK = std::numeric_limits<double>::infinity(); }), "audit poison refused");
	require(!run.advanceTo(1) && !run.snapshot().healthy, "poisoned recovery passed audit");
	auto after = run.snapshot();
	require(after.coolantLoops[0].condition == before.coolantLoops[0].condition
		&& after.coolantLoops[0].restoreStockConsumedUnits == 0
		&& after.stores[0].currentQualified == before.stores[0].currentQualified
		&& run.events() == events, "failed tick published recovery or spent stock");
	// Two accepted jobs cannot double-spend shared stock, even within one step.
	s.stores[0].startingQualified = 2.1;
	auto second = s.coolantLoops[0]; second.id = "second"; s.coolantLoops.push_back(second);
	core::StationSystemsRuntime shared(s, 0.25f);
	require(shared.restoreCondition("loop", 1) && shared.restoreCondition("second", 1)
		&& shared.advanceTo(1), "shared stock recovery failed audit");
	after = shared.snapshot();
	require(after.stores[0].currentQualified == 2
		&& std::abs(after.coolantLoops[0].restoreStockConsumedUnits
			+ after.coolantLoops[1].restoreStockConsumedUnits - 0.1) < 1e-12,
		"shared recovery double-spent reserve-protected stock");
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = conditionStation(); offered.systemsStepWorldTicks = ratio;
		offered.coolantLoops[0].condition->overheatStressC = 10;
		offered.coolantLoops[0].condition->overheatDegradePerHour = 360;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.advanceTo(2), "Condition step-ratio drive failed");
		auto snap = stepped.snapshot();
		require(std::abs(snap.coolantLoops[0].condition - (1 - 0.1 * snap.elapsedSeconds)) < 1e-10,
			"constant-stressor Condition did not integrate elapsed seconds");
	}
}
void conditionValidation(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = conditionStation(); std::string diagnostic;
	core::StationConfigurationDocument document; document.settings = s;
	auto bytes = core::serializeStationConfiguration(document);
	require(bytes.find("schemaVersion: 113") != std::string::npos, "Condition schema not versioned");
	core::StationConfigurationDocument loaded;
	require(core::parseStationConfiguration(bytes, loaded, &diagnostic) && loaded.settings == s, "Condition document round-trip failed");
	bytes.replace(bytes.find("schemaVersion: 113"), std::string("schemaVersion: 113").size(), "schemaVersion: 112");
	require(!core::parseStationConfiguration(bytes, loaded, &diagnostic), "old schema silently accepted Condition");
	for (auto member : {&core::InstallationConditionSettings::overheatDegradePerHour,
		&core::InstallationConditionSettings::restorePerHour, &core::InstallationConditionSettings::stockUnitsPerConditionPoint})
	for (double value : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; (*bad.coolantLoops[0].condition).*member = value;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid Condition rate accepted");
	}
	core::StationSystemsRuntime run(s, 0.25f);
	require(!run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.condition.reset(); }, &diagnostic)
		&& !run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.condition->restoreStoreId.clear(); }, &diagnostic),
		"Condition topology changed live");
	// Feature-absent wire output stays at 111 and contains no Condition fields.
	s.coolantLoops[0].condition.reset(); document.settings = s;
	bytes = core::serializeStationConfiguration(document);
	require(bytes.find("schemaVersion: 111") != std::string::npos && bytes.find("condition:") == std::string::npos,
		"feature-absent document gained Condition fields");
	core::StationSystemsRuntime absent(s, 0.25f);
	require(!absent.hasFaultReplayState() && !absent.restoreCondition("loop", 1, &diagnostic), "absent Condition enabled recovery/replay");
	core::StationSystemsRuntime unused(conditionStation(), 0.25f);
	for (double seconds : {1., 2., 5.})
	{
		require(absent.advanceTo(seconds) && unused.advanceTo(seconds), "opt-in comparison drive failed");
		require(absent.snapshot() == unused.snapshot() && absent.events() == unused.events(),
			"unstressed unused Condition changed legacy service or events");
	}
}
}
void registerStationConditionChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"conditionStress", conditionStress});
	checks.push_back({"conditionCrossing", conditionCrossing});
	checks.push_back({"conditionRestore", conditionRestore});
	checks.push_back({"conditionReplay", conditionReplay});
	checks.push_back({"conditionValidation", conditionValidation});
	checks.push_back({"conditionTransactions", conditionTransactions});
}
