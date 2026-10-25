#include "InjectableFaultContract.h"
#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// #844: the thermal-plant Fault catalogue. Chiller compressor-failure
// (availability zero) and refrigerant-leak (a finite refrigerant charge
// store whose remaining charge scales cooling capacity, leaking through
// the ledger into a compartment); radiator panel-puncture (coolant-loss
// booking into the compartment, rejection capped by remaining area);
// heat-engine mechanical-seizure (availability zero). Every assertion is
// made through the station runtime boundary - commands, snapshots, events
// and persistence - never on ledger internals.
namespace
{
using namespace smoke::station;

core::ThermalReservoirSettings plantReservoir(std::string const& id,
	std::string const& thermalClass, double capacityKJPerK, double startingC)
{
	core::ThermalReservoirSettings r;
	r.id = id;
	r.thermalClass = thermalClass;
	r.heatCapacityKJPerK = capacityKJPerK;
	r.startingTemperatureC = startingC;
	r.minTemperatureC = -50.0;
	r.maxTemperatureC = 300.0;
	return r;
}

// The thermal-plant fixture: a moderate-class cooling chain (loop ->
// chiller -> sink reservoir -> radiator) beside a live heat-engine pair,
// one compartment to receive every hazard booking, a battery as the
// acceptance the engine needs, and the repair-parts stores every machine
// is wired to. All four catalogue modes are opted in.
core::StationSystemsSettings thermalPlantStation()
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.systemsStepWorldTicks = 1;
	s.thermalReservoirs = {
		plantReservoir("sink", "moderate", 10000.0, 20.0),
		plantReservoir("condenser", "moderate", 10000.0, 25.0),
		plantReservoir("hot", "warm", 20000.0, 90.0),
		plantReservoir("reject", "warm", 20000.0, 20.0)};
	core::CoolantLoopSettings loop;
	loop.id = "loop"; loop.reservoirId = "sink";
	loop.coolantCapacityKg = loop.startingCoolantKg = 100;
	loop.specificHeatKJPerKgK = 4; loop.startingTemperatureC = 20;
	loop.ratedFlowKgPerS = 10; loop.pumpPowerKW = 1;
	loop.exchangerConductanceKWPerK = 5;
	s.coolantLoops = {loop};
	core::ChillerSettings chiller;
	chiller.id = "chiller"; chiller.thermalClass = "moderate";
	chiller.coldLoopId = "loop"; chiller.condenserReservoirId = "condenser";
	chiller.ratedCoolingCapacityKW = 5; chiller.cop = 2;
	chiller.auxiliaryPowerKW = 0.1;
	chiller.chilledSetpointC = 5; chiller.maxCondensingTemperatureC = 45;
	chiller.compressorFailure = true;
	chiller.refrigerantLeak = core::RefrigerantLeakSettings{"room", 10.0, 0.5, 2.0};
	s.chillers = {chiller};
	core::RadiatorSettings radiator;
	radiator.id = "rad"; radiator.reservoirId = "sink";
	radiator.areaM2 = 10; radiator.emissivity = 0.9; radiator.viewFactor = 1.0;
	radiator.sinkTemperatureC = -270.0;
	radiator.panelPuncture = core::PanelPunctureSettings{"room", 20.0, 1.0, 4.0};
	s.radiators = {radiator};
	core::HeatEngineSettings engine;
	engine.id = "orc";
	engine.hotReservoirId = "hot"; engine.rejectionReservoirId = "reject";
	engine.thermalClass = "warm";
	engine.ratedThermalInputKW = 100; engine.commandedInputKW = 100;
	engine.carnotFraction = 0.5; engine.auxiliaryPowerKW = 1;
	engine.minHotTemperatureC = 30; engine.maxSinkTemperatureC = 25;
	engine.mechanicalSeizure = true;
	s.heatEngines = {engine};
	core::BatterySettings bank;
	bank.id = "buffer";
	bank.nameplateKWh = 1000.0; bank.usableKWh = 900.0; bank.startingStoredKWh = 100.0;
	bank.maxChargeKW = 500.0; bank.maxDischargeKW = 500.0;
	s.batteries = {bank};
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	core::SolarArraySettings solar;
	solar.id = "solar"; solar.capacityKW = 20;
	s.solarArrays = {solar};
	core::ResourceStoreSettings parts;
	parts.id = "parts"; parts.unit = "kg"; parts.capacity = 100; parts.startingQualified = 50;
	core::ResourceStoreSettings waste;
	waste.id = "waste"; waste.unit = "kg"; waste.capacity = 100; waste.startingQualified = 0;
	s.stores = {parts, waste};
	core::RepairPartsSettings chillerWiring;
	chillerWiring.partsStoreId = "parts";
	chillerWiring.repairs = {
		core::FaultRepairSettings{"compressor-failure", 10.0, 2.0, "waste"},
		core::FaultRepairSettings{"refrigerant-leak", 10.0, 3.0, "waste"}};
	s.chillers[0].repairParts = chillerWiring;
	core::RepairPartsSettings radiatorWiring;
	radiatorWiring.partsStoreId = "parts";
	radiatorWiring.repairs = {core::FaultRepairSettings{"panel-puncture", 10.0, 2.0, "waste"}};
	s.radiators[0].repairParts = radiatorWiring;
	core::RepairPartsSettings engineWiring;
	engineWiring.partsStoreId = "parts";
	engineWiring.repairs = {core::FaultRepairSettings{"mechanical-seizure", 10.0, 4.0, "waste"}};
	s.heatEngines[0].repairParts = engineWiring;
	return s;
}

void thermalPlantMonitors(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto base = thermalPlantStation();
	base.coolantLoops[0].coolantLeak = core::CoolantLeakSettings{"room", 60};
	core::applyStationThermalPlantDefaultMonitors(base);
	require(base.monitors.size() == 11, "thermal default coverage incomplete");
	std::string diagnostic;
	require(core::validateStationMonitors(base, diagnostic), diagnostic.c_str());
	auto const original = base.monitors;
	core::applyStationThermalPlantDefaultMonitors(base);
	require(base.monitors == original, "thermal defaults not idempotent");
	for (auto const& m : original)
	{
		require(!m.enabled && m.provenance != core::MonitorProvenance::Custom, "default enabled or lacks provenance");
		require(core::stationMonitorSubjectExists(base, m.subjectType, m.subjectId), "default subject unresolved");
	}
	// Test the complete table, including dynamically resolved authored bounds.
	std::vector<core::AlertRule> const rules{
		{0.8, core::AlertDirection::Below, 120, 60}, {0.5, core::AlertDirection::Below, 60, 60},
		{0.9, core::AlertDirection::Below, 60, 30}, {7, core::AlertDirection::Below, 60, 120},
		{0.1, core::AlertDirection::Above, 120, 60}, {0, core::AlertDirection::Above, 10, 30},
		{0.8, core::AlertDirection::Below, 300, 120}, {0, core::AlertDirection::Above, 0, 0},
		{5, core::AlertDirection::Below, 120, 120}, {0.9, core::AlertDirection::Below, 120, 60},
		{295, core::AlertDirection::Above, 120, 120}};
	std::vector<core::AlertSeverity> const severities{
		core::AlertSeverity::Warning, core::AlertSeverity::Critical, core::AlertSeverity::Warning,
		core::AlertSeverity::Critical, core::AlertSeverity::Warning, core::AlertSeverity::Critical,
		core::AlertSeverity::Warning, core::AlertSeverity::Critical, core::AlertSeverity::Advisory,
		core::AlertSeverity::Advisory, core::AlertSeverity::Advisory};
	for (size_t i = 0; i < rules.size(); ++i)
	{
		require(original[i].rule == rules[i] && original[i].severity == severities[i], "thermal default rule differs");
		require(original[i].provenance == (i == 8 || i == 10 ? core::MonitorProvenance::Provisional
			: core::MonitorProvenance::Rated), "thermal provenance differs");
	}
	base.monitors[0].rule.threshold = 0.7;
	core::applyStationThermalPlantDefaultMonitors(base);
	require(base.monitors[0].rule.threshold == 0.7, "thermal reapplication overwrote tuning");
	base.monitors = original;
	core::StationConfigurationDocument document; document.settings = base;
	core::StationConfigurationDocument loaded;
	require(core::parseStationConfiguration(core::serializeStationConfiguration(document), loaded, &diagnostic)
		&& loaded.settings == base, "thermal Monitors did not round-trip");
	core::StationSystemsRuntime disabled(base, 1);
	require(disabled.injectFault("loop", "coolant-leak", {}) && disabled.advanceTo(400)
		&& disabled.snapshot().activeAlerts.empty() && disabled.snapshot().closedAlerts.empty(),
		"disabled thermal defaults emitted Alerts");
	// A separate real runtime per symptom prevents unrelated Alerts from
	// masking the boundary. Faults are applied through the production queue.
	for (auto dt : {0.25f, 1.0f})
	for (size_t index = 0; index < original.size(); ++index)
	{
		auto s = base;
		auto m = original[index]; m.enabled = true; smoke::station::isolateMonitor(s, m);
		std::string faultTarget, faultMode;
		if (index <= 2) { faultTarget = "loop"; faultMode = "coolant-leak"; }
		if (index == 3) { s.coolantLoops[0].startingTemperatureC = 0; s.coolantLoops[0].exchangerConductanceKWPerK = 0; }
		if (index == 4) { faultTarget = "loop"; faultMode = "coolant-leak"; }
		if (index == 5) s.chillers[0].maxCondensingTemperatureC = 24;
		if (index == 6) { faultTarget = "chiller"; faultMode = "refrigerant-leak"; }
		if (index == 7) s.heatEngines[0].maxSinkTemperatureC = 19;
		if (index == 8) s.heatEngines[0].minHotTemperatureC = 89;
		if (index == 9) { faultTarget = "rad"; faultMode = "panel-puncture"; }
		if (index == 10)
		{
			s.thermalReservoirs[0].startingTemperatureC = 299;
			s.thermalReservoirs[0].heatCapacityKJPerK = 1e9;
		}
		core::StationSystemsRuntime run(s, dt);
		if (!faultMode.empty()) require(run.injectFault(faultTarget, faultMode, {}), "symptom fault refused");
		double firstOutside = 0;
		bool fired = false;
		for (double t = dt; t <= 400; t += dt)
		{
			require(run.advanceTo(t), "thermal Monitor drive failed");
			auto snap = run.snapshot();
			auto value = core::readStationMonitorMetric(s, snap, m.subjectType, m.subjectId, m.metric);
			require(value.has_value(), "thermal metric unavailable");
			bool outside = m.rule.direction == core::AlertDirection::Above ? *value > m.rule.threshold : *value < m.rule.threshold;
			if (outside && !firstOutside) firstOutside = t;
			if (!outside) firstOutside = 0;
			bool due = firstOutside && t - firstOutside + dt >= m.rule.triggerOnSeconds;
			require((!snap.activeAlerts.empty()) == due, ("thermal Alert fired off boundary: " + m.id).c_str());
			if (due)
			{
				auto const& a = snap.activeAlerts[0];
				require(a.firedAtSeconds == t && a.severity == m.severity && a.provenance == m.provenance
					&& a.currentValue == *value, "thermal Alert metadata wrong");
				fired = true; break;
			}
		}
		require(fired, ("thermal symptom never fired: " + m.id + " value=" + std::to_string(
			core::readStationMonitorMetric(s, run.snapshot(), m.subjectType, m.subjectId, m.metric).value_or(-999))
			+ " threshold=" + std::to_string(m.rule.threshold)).c_str());
		// Recover protection/gradient symptoms by changing the physical gate,
		// leaving the Monitor and its persistence state untouched.
		if (index == 5 || index == 7 || index == 8)
		{
			double const fireTime = run.snapshot().elapsedSeconds;
			if (index == 5) s.chillers[0].maxCondensingTemperatureC = 1000;
			if (index == 7) s.heatEngines[0].maxSinkTemperatureC = 1000;
			if (index == 8) s.heatEngines[0].minHotTemperatureC = 0;
			require(run.updateSettings(s), "physical recovery refused");
			double const clearTime = fireTime + std::max<double>(dt, m.rule.triggerOffSeconds);
			if (clearTime - dt > fireTime)
				require(run.advanceTo(clearTime - dt) && run.snapshot().activeAlerts.size() == 1,
					"thermal Alert cleared early");
			require(run.advanceTo(clearTime), "thermal recovery drive failed");
			auto recovered = run.snapshot();
			require(recovered.activeAlerts.empty() && recovered.closedAlerts.size() == 1
				&& recovered.closedAlerts[0].clearedAtSeconds == clearTime
				&& recovered.closedAlerts[0].clearReason == core::AlertClearReason::AutoClear,
				"thermal Alert did not auto-clear at boundary");
		}
	}
	// No finite charge model must not look like an empty refrigerant tank.
	base.chillers[0].refrigerantLeak.reset(); base.monitors.clear();
	core::StationSystemsRuntime uncharged(base, 1);
	require(uncharged.advanceTo(1) && core::readStationMonitorMetric(base, uncharged.snapshot(),
		"chiller", "chiller", "refrigerantFraction") == 1.0, "unmodelled charge reported empty");
}

int countCategory(core::StationSystemsRuntime const& run, std::string const& category)
{
	auto const events = run.events();
	return static_cast<int>(std::count_if(events.begin(), events.end(),
		[&](auto const& e) { return e.category == category; }));
}
bool sawMessage(core::StationSystemsRuntime const& run, std::string const& needle)
{
	auto const events = run.events();
	return std::any_of(events.begin(), events.end(), [&](auto const& e)
	{
		return e.message.find(needle) != std::string::npos;
	});
}
const core::StationFaultSnapshot* findFault(
	std::vector<core::StationFaultSnapshot> const& faults, std::string const& mode)
{
	auto const it = std::find_if(faults.begin(), faults.end(),
		[&](auto const& f) { return f.modeId == mode; });
	return it == faults.end() ? nullptr : &*it;
}
const core::StationMaintenanceSnapshot* findJob(
	core::StationSystemsSnapshot const& snap, std::string const& id)
{
	auto const it = std::find_if(snap.maintenanceJobs.begin(), snap.maintenanceJobs.end(),
		[&](auto const& job) { return job.id == id; });
	return it == snap.maintenanceJobs.end() ? nullptr : &*it;
}

// Every mode injects, applies its effect shape, publishes in the
// snapshot with derived impact and logs its onset; refusals never mutate.
void thermalPlantInjection(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	requireInjectableFaultContract(thermalPlantStation());
	auto const s = thermalPlantStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	auto const before = run.snapshot();
	auto const events = run.events();
	// The machines are live before any Fault: the caps below are real
	// removals, not the fixture never having worked.
	require(before.chillers[0].availabilityFactor == 1.0
		&& before.chillers[0].coolingKW > 0.0,
		"chiller was not delivering cold service before the Fault");
	require(before.heatEngines[0].availabilityFactor == 1.0
		&& before.heatEngines[0].lastElectricalOutputKW > 0.0,
		"heat engine was not generating before the Fault");
	std::string diagnostic;
	// Unknown installations and cross-kind injections are refused whole.
	for (auto const& request : std::vector<std::pair<std::string, std::string>>{
		{"missing", "compressor-failure"}, {"missing", "refrigerant-leak"},
		{"missing", "panel-puncture"}, {"missing", "mechanical-seizure"},
		{"loop", "compressor-failure"}, {"loop", "refrigerant-leak"},
		{"chiller", "panel-puncture"}, {"chiller", "mechanical-seizure"},
		{"rad", "compressor-failure"}, {"rad", "refrigerant-leak"},
		{"rad", "mechanical-seizure"},
		{"orc", "compressor-failure"}, {"orc", "refrigerant-leak"},
		{"orc", "panel-puncture"}})
	{
		require(!run.injectFault(request.first, request.second, {}, &diagnostic)
			&& !diagnostic.empty(),
			("invalid thermal-plant fault injection accepted or unexplained: "
				+ request.first + "/" + request.second).c_str());
		require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
			"refused command mutated state");
	}
	// Refrigerant leak: the finite charge drains through the ledger and
	// the remaining charge fraction caps the cooling capacity.
	require(run.injectFault("chiller", "refrigerant-leak",
		{core::FaultCause::Kind::ScenarioEvent, "strike"}),
		"valid refrigerant-leak injection refused");
	require(run.snapshot() == before && run.events() == events,
		"command applied before tick boundary");
	require(!run.injectFault("chiller", "refrigerant-leak", {}, &diagnostic),
		"pending duplicate accepted");
	require(run.advanceTo(2), "refrigerant leak booking failed audit");
	auto const leaked = run.snapshot();
	require(leaked.healthy, "refrigerant leak booking broke the conservation audit");
	auto const& chiller = leaked.chillers[0];
	require(chiller.refrigerantChargeKg == 10.0 && chiller.refrigerantKg == 9.5
		&& chiller.leakedRefrigerantKg == 0.5,
		"refrigerant charge did not drain at the authored rate");
	require(leaked.atmospheres[0].spilledCoolantKg == 0.5
		&& leaked.atmospheres[0].spilledCoolantEnergyKJ
			== chiller.leakedRefrigerantEnergyKJ
		&& leaked.atmospheres[0].spilledCoolantEnergyKJ > 0.0,
		"refrigerant spill did not close into the compartment");
	require(std::abs(chiller.availabilityFactor - 0.95) < 1e-12,
		"remaining charge did not cap the cooling capacity");
	auto const* fault = findFault(chiller.activeFaults, "refrigerant-leak");
	require(fault && fault->installationId == "chiller"
		&& fault->onsetTick == before.worldTick + 1
		&& fault->causeKind == core::FaultCause::Kind::ScenarioEvent
		&& fault->causeDetail == "strike"
		&& std::abs(fault->derivedImpact - 0.05) < 1e-12,
		"refrigerant-leak snapshot lost onset or derived impact");
	require(countCategory(run, "fault") == 1, "refrigerant-leak onset missing from the event log");
	require(!run.injectFault("chiller", "refrigerant-leak", {}, &diagnostic)
		&& run.snapshot() == leaked, "active duplicate mutated state");
	// The charge is finite: it drains to empty and the cold service dies
	// with it; the leak never invents refrigerant.
	require(run.advanceTo(21), "full drain drive failed");
	auto const drained = run.snapshot();
	require(drained.chillers[0].refrigerantKg == 0.0
		&& drained.chillers[0].leakedRefrigerantKg == 10.0
		&& drained.chillers[0].availabilityFactor == 0.0
		&& drained.chillers[0].coolingKW == 0.0
		&& drained.atmospheres[0].spilledCoolantKg == 10.0,
		"empty refrigerant charge did not zero the cold service");
	require(findFault(drained.chillers[0].activeFaults, "refrigerant-leak")->derivedImpact == 1.0,
		"fully drained leak did not report full impact");
	// Compressor failure: total removal from service.
	require(run.injectFault("chiller", "compressor-failure",
		{core::FaultCause::Kind::AgentAction, "agent:9"}),
		"valid compressor-failure injection refused");
	require(run.snapshot() == drained, "command applied before tick boundary");
	require(run.advanceTo(22), "compressor failure drive failed");
	auto const seized = run.snapshot();
	require(seized.chillers[0].availabilityFactor == 0.0
		&& seized.chillers[0].coolingKW == 0.0
		&& seized.chillers[0].powerSuppliedKW == 0.0,
		"compressor failure left the chiller drawing or cooling");
	auto const* compressor = findFault(seized.chillers[0].activeFaults, "compressor-failure");
	require(compressor && compressor->derivedImpact == 1.0
		&& compressor->causeKind == core::FaultCause::Kind::AgentAction
		&& compressor->causeDetail == "agent:9",
		"compressor-failure snapshot missing or misattributed");
	// Panel puncture: coolant books into the compartment and the
	// remaining area caps rejection.
	auto const rejectionBefore = seized.thermal.radiators[0].lastRejectedKW;
	require(rejectionBefore > 0.0, "radiator was not rejecting before the puncture");
	require(run.injectFault("rad", "panel-puncture",
		{core::FaultCause::Kind::Editor, "debug"}),
		"valid panel-puncture injection refused");
	require(run.advanceTo(23), "panel puncture booking failed audit");
	auto const punctured = run.snapshot();
	require(punctured.healthy, "panel puncture booking broke the conservation audit");
	auto const& panel = punctured.thermal.radiators[0];
	require(panel.panelCoolantCapacityKg == 20.0 && panel.panelCoolantKg == 19.0
		&& panel.leakedCoolantKg == 1.0,
		"panel charge did not bleed at the authored rate");
	require(std::abs(punctured.atmospheres[0].spilledCoolantKg - 11.0) < 1e-9,
		"punctured coolant did not book into the compartment");
	require(std::abs(panel.remainingAreaFraction - 0.95) < 1e-12
		&& std::abs(panel.availabilityFactor - 0.95) < 1e-12,
		"remaining charge did not become the area cap");
	require(std::abs(findFault(panel.activeFaults, "panel-puncture")->derivedImpact
		- panel.leakedCoolantKg / panel.panelCoolantCapacityKg) < 1e-12,
		"panel-puncture derived impact wrong");
	// The rejection is capped by the remaining area: the ratio of the
	// capped rejection to the uncapped baseline tracks the area fraction
	// (the reservoir's own temperature drifts a little between the two
	// observations, so the check is loose but discriminating).
	require(panel.lastRejectedKW < rejectionBefore
		&& panel.lastRejectedKW > rejectionBefore * (panel.remainingAreaFraction - 0.05),
		"rejection was not capped by the remaining panel area");
	// Mechanical seizure: the engine comes fully out of service.
	require(run.injectFault("orc", "mechanical-seizure",
		{core::FaultCause::Kind::AgentAction, "agent:3"}),
		"valid mechanical-seizure injection refused");
	require(run.advanceTo(24), "seizure drive failed");
	auto const stopped = run.snapshot();
	require(stopped.heatEngines[0].availabilityFactor == 0.0
		&& stopped.heatEngines[0].lastThermalInputKW == 0.0
		&& stopped.heatEngines[0].lastElectricalOutputKW == 0.0
		&& stopped.heatEngines[0].lastRejectionKW == 0.0,
		"seized engine kept converting or rejecting");
	auto const* seizure = findFault(stopped.heatEngines[0].activeFaults, "mechanical-seizure");
	require(seizure && seizure->derivedImpact == 1.0
		&& seizure->causeKind == core::FaultCause::Kind::AgentAction
		&& seizure->causeDetail == "agent:3",
		"mechanical-seizure snapshot missing or misattributed");
	require(countCategory(run, "fault") == 4,
		"the four catalogue onsets did not each log one fault event");
}

void simultaneousChillerRepairs();

// Each mode clears through the repair seam: the repair isolates the
// machine for its full duration, completion lifts the Fault, and the
// physical consequence of the leak - the charge that already left -
// stays gone.
void thermalPlantRepairs(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	simultaneousChillerRepairs();
	auto const s = thermalPlantStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	std::string diagnostic;
	// Refusals: no fault to repair, and no plan for a mode the wiring
	// does not name.
	require(!run.repairFault("chiller", "compressor-failure", nullptr, &diagnostic)
		&& !diagnostic.empty(), "repair without a Fault accepted");
	require(run.injectFault("chiller", "refrigerant-leak", {}) && run.advanceTo(2),
		"leak setup failed");
	require(!run.repairFault("chiller", "mechanical-seizure", nullptr, &diagnostic),
		"cross-kind repair plan accepted");
	// Refrigerant leak repair: seal the leak, keep the drained charge.
	// The leak keeps bleeding while the repair runs - the crew has not
	// sealed it until completion - so the cleared charge is captured at
	// completion rather than assumed.
	std::string jobId;
	require(run.repairFault("chiller", "refrigerant-leak", &jobId),
		"valid refrigerant-leak repair refused");
	require(jobId == "repair-1", "repair job id not as expected");
	require(run.advanceTo(3), "repair start failed audit");
	require(run.snapshot().chillers[0].availabilityFactor == 0.0,
		"repair did not isolate the chiller");
	require(run.advanceTo(12), "refrigerant-leak repair completion failed audit");
	auto const sealed = run.snapshot();
	require(findFault(sealed.chillers[0].activeFaults, "refrigerant-leak") == nullptr,
		"refrigerant-leak did not clear on repair completion");
	auto const drainedAtSeal = sealed.chillers[0].leakedRefrigerantKg;
	require(drainedAtSeal > 0.5, "the leak stopped bleeding during the repair");
	require(std::abs(sealed.chillers[0].refrigerantKg - (10.0 - drainedAtSeal)) < 1e-9
		&& std::abs(sealed.chillers[0].availabilityFactor
			- (10.0 - drainedAtSeal) / 10.0) < 1e-12,
		"repair refilled the charge or lifted the cap it should keep");
	require(sawMessage(run, "Fault 'refrigerant-leak' on chiller 'chiller' cleared by repair"),
		"repair clear missing from the fault event log");
	require(run.advanceTo(22), "post-repair drive failed");
	require(run.snapshot().chillers[0].leakedRefrigerantKg == drainedAtSeal
		&& run.snapshot().atmospheres[0].spilledCoolantKg == drainedAtSeal,
		"sealed leak kept bleeding");
	// Compressor failure repair restores the machine to full service -
	// full service being the charge-capped fraction it had.
	require(run.injectFault("chiller", "compressor-failure", {core::FaultCause::Kind::Editor, "x"})
		&& run.advanceTo(23), "compressor failure setup failed");
	require(run.snapshot().chillers[0].availabilityFactor == 0.0, "failure cap missing");
	require(run.repairFault("chiller", "compressor-failure", &jobId) && jobId == "repair-2",
		"valid compressor-failure repair refused");
	require(run.advanceTo(24), "compressor repair start failed audit");
	require(run.snapshot().chillers[0].availabilityFactor == 0.0,
		"compressor repair did not isolate");
	require(run.advanceTo(34), "compressor repair completion failed audit");
	require(findFault(run.snapshot().chillers[0].activeFaults, "compressor-failure") == nullptr
		&& std::abs(run.snapshot().chillers[0].availabilityFactor
			- (10.0 - drainedAtSeal) / 10.0) < 1e-12,
		"compressor repair did not restore the charge-capped service");
	// Panel puncture repair: seal the puncture, keep the drained area.
	require(run.injectFault("rad", "panel-puncture", {}) && run.advanceTo(35),
		"puncture setup failed");
	require(run.repairFault("rad", "panel-puncture", &jobId) && jobId == "repair-3",
		"valid panel-puncture repair refused");
	require(run.advanceTo(36), "puncture repair start failed audit");
	require(run.snapshot().thermal.radiators[0].availabilityFactor == 0.0,
		"puncture repair did not isolate the panel");
	require(run.advanceTo(46), "puncture repair completion failed audit");
	auto const patched = run.snapshot();
	require(findFault(patched.thermal.radiators[0].activeFaults, "panel-puncture") == nullptr,
		"panel-puncture did not clear on repair completion");
	auto const bledAtSeal = patched.thermal.radiators[0].leakedCoolantKg;
	require(bledAtSeal > 1.0, "the puncture stopped bleeding during the repair");
	require(std::abs(patched.thermal.radiators[0].panelCoolantKg - (20.0 - bledAtSeal)) < 1e-9
		&& std::abs(patched.thermal.radiators[0].availabilityFactor
			- (20.0 - bledAtSeal) / 20.0) < 1e-12,
		"puncture repair refilled the panel or lifted the area cap");
	require(run.advanceTo(56), "post-puncture-repair drive failed");
	require(run.snapshot().thermal.radiators[0].leakedCoolantKg == bledAtSeal,
		"sealed puncture kept bleeding");
	// Mechanical seizure repair returns the engine to service.
	require(run.injectFault("orc", "mechanical-seizure", {}) && run.advanceTo(57),
		"seizure setup failed");
	require(run.repairFault("orc", "mechanical-seizure", &jobId) && jobId == "repair-4",
		"valid mechanical-seizure repair refused");
	require(run.advanceTo(58), "seizure repair start failed audit");
	require(run.snapshot().heatEngines[0].availabilityFactor == 0.0,
		"seizure repair did not isolate the engine");
	require(run.advanceTo(68), "seizure repair completion failed audit");
	require(findFault(run.snapshot().heatEngines[0].activeFaults, "mechanical-seizure") == nullptr
		&& run.snapshot().heatEngines[0].availabilityFactor == 1.0,
		"seizure repair did not restore the engine to full service");
	require(countCategory(run, "fault") == 8,
		"four onsets and four clears did not each log a fault event");
}

// Fault reductions compose with scheduled Outages under
// deepest-reduction-wins, never by multiplication.
void thermalPlantOutages(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (double outageCap : {0.0, 0.5, 0.9})
	{
		auto s = thermalPlantStation();
		core::InstallationOutageSettings outage;
		outage.id = "outage"; outage.installationId = "chiller";
		outage.capacityFraction = outageCap;
		outage.firstOccurrenceSeconds = 0; outage.durationSeconds = 100;
		s.outages = {outage};
		core::StationSystemsRuntime run(s, 0.25f);
		require(run.injectFault("chiller", "refrigerant-leak", {}) && run.advanceTo(3),
			"outaged leak failed");
		// Three seconds of leak at 0.5 kg/s: charge 7.0, fault cap 0.7.
		auto const& chiller = run.snapshot().chillers[0];
		require(std::abs(chiller.refrigerantKg - 7.0) < 1e-9, "leak under outage miscounted");
		require(std::abs(chiller.availabilityFactor - std::min(outageCap, 0.7)) < 1e-12,
			"fault cap and Outage multiplied instead of deepest-wins");
	}
	// A seized engine under an outage stays at zero either way.
	auto s = thermalPlantStation();
	core::InstallationOutageSettings outage;
	outage.id = "outage"; outage.installationId = "orc";
	outage.capacityFraction = 0.8;
	outage.firstOccurrenceSeconds = 0; outage.durationSeconds = 100;
	s.outages = {outage};
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.injectFault("orc", "mechanical-seizure", {}) && run.advanceTo(2),
		"seizure under outage failed");
	require(run.snapshot().heatEngines[0].availabilityFactor == 0.0
		&& run.snapshot().heatEngines[0].lastElectricalOutputKW == 0.0,
		"seized engine under outage still produced");
}

template<class Serializer>
std::string replayBytes(core::StationSystemsRuntime const& run)
{
	auto out = Serializer::toString();
	out->beginMap("root"); run.writeFaultReplay(*out); out->endMap(); out->serialize();
	return out->getSerializedString();
}
template<class Serializer>
void roundTrip(core::StationSystemsRuntime const& run)
{
	auto const bytes = replayBytes<Serializer>(run);
	auto in = Serializer::fromString(bytes); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"thermal-plant fault replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes,
		"thermal-plant fault command log did not round-trip byte-exactly");
	require(restored->advanceTo(80), "restored thermal-plant faults failed subsequent audited drive");
}

// #858: paying for either chiller repair must not clear its other Fault.
// Exercise both orders with enough charge left to observe continued leakage.
void simultaneousChillerRepairs()
{
	for (bool compressorFirst : {true, false})
	{
		auto s = thermalPlantStation();
		s.chillers[0].refrigerantLeak->leakRateKgPerS = 0.1;
		core::StationSystemsRuntime run(s, 0.25f);
		require(run.advanceTo(1)
			&& run.injectFault("chiller", "compressor-failure",
				{core::FaultCause::Kind::AgentAction, "agent:compressor"})
			&& run.injectFault("chiller", "refrigerant-leak",
				{core::FaultCause::Kind::ScenarioEvent, "leak-strike"})
			&& run.advanceTo(2), "simultaneous chiller Fault setup failed");
		auto const initial = run.snapshot();
		require(initial.chillers[0].activeFaults.size() == 2,
			"both chiller Faults were not published");
		std::string const first = compressorFirst ? "compressor-failure" : "refrigerant-leak";
		std::string const second = compressorFirst ? "refrigerant-leak" : "compressor-failure";
		auto const originalOther = *findFault(initial.chillers[0].activeFaults, second);
		std::string jobId;
		require(run.repairFault("chiller", first, &jobId) && jobId == "repair-1"
			&& run.advanceTo(3), "first simultaneous chiller repair failed to start");
		auto const active = run.snapshot();
		require(findJob(active, jobId)->isolated
			&& active.chillers[0].availabilityFactor == 0.0,
			"first chiller repair did not isolate the installation");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.advanceTo(12), "first simultaneous chiller repair completion failed");
		auto const repaired = run.snapshot();
		auto const& chiller = repaired.chillers[0];
		require(chiller.activeFaults.size() == 1 && !findFault(chiller.activeFaults, first),
			("repairing " + first + " also cleared the unrelated chiller Fault").c_str());
		auto const* other = findFault(chiller.activeFaults, second);
		require(other && other->onsetTick == originalOther.onsetTick
			&& other->causeKind == originalOther.causeKind
			&& other->causeDetail == originalOther.causeDetail,
			"unrepaired chiller Fault lost its onset or attribution");
		auto const* completed = findJob(repaired, jobId);
		auto const firstCost = compressorFirst ? 2.0 : 3.0;
		require(completed->phase == core::StationMaintenancePhase::Completed
			&& completed->faultModeId == first && !completed->isolated
			&& completed->requirements[0].consumedTotal == firstCost
			&& countCategory(run, "fault") == 3
			&& countCategory(run, "repair") == 2
			&& sawMessage(run, "Fault '" + first + "' on chiller 'chiller' cleared by repair 'repair-1'")
			&& !sawMessage(run, "Fault '" + second + "' on chiller 'chiller' cleared by repair"),
			"first repair spent or logged work for the wrong Fault");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.advanceTo(14), "unrepaired chiller effect drive failed");
		auto const after = run.snapshot();
		if (compressorFirst)
			require(after.chillers[0].leakedRefrigerantKg > chiller.leakedRefrigerantKg
				&& after.atmospheres[0].spilledCoolantKg > repaired.atmospheres[0].spilledCoolantKg
				&& after.chillers[0].availabilityFactor > 0.0
				&& std::abs(after.chillers[0].availabilityFactor
					- after.chillers[0].refrigerantKg / after.chillers[0].refrigerantChargeKg) < 1e-12,
				"compressor repair stopped the unrepaired leak or its charge cap");
		else
			require(after.chillers[0].leakedRefrigerantKg == chiller.leakedRefrigerantKg
				&& after.chillers[0].availabilityFactor == 0.0
				&& after.chillers[0].coolingKW == 0.0,
				"leak repair restored the unrepaired compressor or failed to seal the leak");
		require(run.repairFault("chiller", second, &jobId) && jobId == "repair-2"
			&& run.advanceTo(15), "remaining chiller Fault could not be repaired");
		auto const secondActive = run.snapshot();
		require(findJob(secondActive, jobId)->isolated,
			"second chiller repair did not isolate");
		require(run.advanceTo(25), "second chiller repair completion failed");
		auto const done = run.snapshot();
		require(done.chillers[0].activeFaults.empty()
			&& done.chillers[0].availabilityFactor > 0.0
			&& findJob(done, "repair-2")->requirements[0].consumedTotal == 5.0 - firstCost
			&& countCategory(run, "fault") == 4 && countCategory(run, "repair") == 4
			&& sawMessage(run, "Fault '" + second + "' on chiller 'chiller' cleared by repair 'repair-2'"),
			"second chiller repair did not independently spend, clear and restore service");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	}
}

// A scripted sequence across all four modes, with repairs and a live
// rate retune, replays exactly through both serializers.
void thermalPlantReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = thermalPlantStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	require(run.injectFault("chiller", "refrigerant-leak",
		{core::FaultCause::Kind::ScenarioEvent, "strike"}), "leak inject failed");
	require(run.advanceTo(2), "leak drive failed");
	// Retune the bleed rate live: the accepted injection froze the old
	// rate, so the running leak keeps 0.5 kg/s regardless.
	require(run.updateNode<core::ChillerSettings>("chiller", [](auto& c)
		{ c.refrigerantLeak->leakRateKgPerS = 2.0; }), "live rate retune failed");
	require(run.advanceTo(4), "retuned drive failed");
	require(std::abs(run.snapshot().chillers[0].leakedRefrigerantKg - 1.5) < 1e-9,
		"live retune reached a frozen leak's effective rate");
	require(run.injectFault("rad", "panel-puncture", {core::FaultCause::Kind::Editor, "debug"})
		&& run.injectFault("orc", "mechanical-seizure", {core::FaultCause::Kind::AgentAction, "agent:1"})
		&& run.advanceTo(6), "puncture/seizure drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.repairFault("orc", "mechanical-seizure", nullptr)
		&& run.advanceTo(8), "repair-in-flight snapshot failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(20), "repair completion drive failed");
	require(run.injectFault("chiller", "compressor-failure", {core::FaultCause::Kind::AgentAction, "agent:2"})
		&& run.advanceTo(30), "late compressor failure drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
}

// Opt-in and validation: absent opt-ins refuse every mode and keep the
// previous wire shape; malformed blocks are refused at authoring time.
void thermalPlantOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// Feature-absent: every mode refused, no new wire fields.
	auto bare = thermalPlantStation();
	bare.chillers[0].compressorFailure = false;
	bare.chillers[0].refrigerantLeak.reset();
	bare.chillers[0].repairParts.reset();
	bare.radiators[0].panelPuncture.reset();
	bare.radiators[0].repairParts.reset();
	bare.heatEngines[0].mechanicalSeizure = false;
	bare.heatEngines[0].repairParts.reset();
	core::StationSystemsRuntime run(bare, 0.25f);
	require(run.advanceTo(1), "bare drive failed");
	std::string diagnostic;
	for (auto const& request : std::vector<std::pair<std::string, std::string>>{
		{"chiller", "compressor-failure"}, {"chiller", "refrigerant-leak"},
		{"rad", "panel-puncture"}, {"orc", "mechanical-seizure"}})
		require(!run.injectFault(request.first, request.second, {}, &diagnostic)
			&& diagnostic.find("not enabled") != std::string::npos,
			("mode without opt-in accepted or unexplained: " + request.second).c_str());
	require(!run.repairFault("chiller", "compressor-failure", nullptr, &diagnostic),
		"repair without wiring accepted");
	core::StationConfigurationDocument bareDoc; bareDoc.settings = bare;
	auto bareYaml = core::serializeStationConfiguration(bareDoc);
	require(bareYaml.find("schemaVersion: 111") != std::string::npos,
		"feature-absent station did not keep its previous schema tier");
	require(bareYaml.find("refrigerantLeak") == std::string::npos
		&& bareYaml.find("panelPuncture") == std::string::npos
		&& bareYaml.find("compressorFailure") == std::string::npos
		&& bareYaml.find("mechanicalSeizure") == std::string::npos,
		"feature-absent station carried thermal-plant fields");
	// The full fixture stamps the new tier and round-trips through both
	// serializers.
	auto const full = thermalPlantStation();
	core::StationConfigurationDocument fullDoc; fullDoc.settings = full;
	auto const yaml = core::serializeStationConfiguration(fullDoc);
	require(yaml.find("schemaVersion: 117") != std::string::npos,
		"thermal-plant opt-in did not bump the wire schema");
	core::StationConfigurationDocument restored;
	require(core::parseStationConfiguration(yaml, restored, &diagnostic)
		&& restored.settings == full, "thermal-plant settings did not round-trip");
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, full); out->endMap(); out->serialize();
		auto const bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restoredSettings;
		core::readStationSystemsSettingsMap(*in, 117, restoredSettings);
		require(restoredSettings == full, "thermal-plant settings did not survive serialization");
		// A pre-117 reader refuses the new blocks.
		bool refused = false;
		try
		{
			core::StationSystemsSettings oldReader;
			in->deserialize(); in->beginMap("root");
			core::readStationSystemsSettingsMap(*in, 116, oldReader);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "pre-117 document silently accepted thermal-plant blocks");
	}
	// Authoring validation refuses malformed blocks.
	auto bad = thermalPlantStation();
	bad.chillers[0].refrigerantLeak = core::RefrigerantLeakSettings{"missing", 10, 0.5, 2};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("refrigerant leak") != std::string::npos,
		"unknown refrigerant leak destination accepted");
	bad = thermalPlantStation();
	bad.chillers[0].refrigerantLeak = core::RefrigerantLeakSettings{"room", 0, 0.5, 2};
	require(!core::validateStationSystemsSettings(bad, diagnostic), "zero charge accepted");
	bad = thermalPlantStation();
	bad.chillers[0].refrigerantLeak = core::RefrigerantLeakSettings{"room", 10, -1, 2};
	require(!core::validateStationSystemsSettings(bad, diagnostic), "negative bleed rate accepted");
	bad = thermalPlantStation();
	bad.radiators[0].panelPuncture = core::PanelPunctureSettings{"missing", 20, 1, 4};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("panel puncture") != std::string::npos,
		"unknown puncture destination accepted");
	bad = thermalPlantStation();
	bad.radiators[0].panelPuncture = core::PanelPunctureSettings{"room", 20, 1, 0};
	require(!core::validateStationSystemsSettings(bad, diagnostic), "zero specific heat accepted");
	bad = thermalPlantStation();
	bad.radiators[0].repairParts = core::RepairPartsSettings{"parts",
		{core::FaultRepairSettings{"compressor-failure", 10, 2, "waste"}}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("no repairable fault mode") != std::string::npos,
		"cross-kind repair plan accepted on a radiator");
	bad = thermalPlantStation();
	bad.heatEngines[0].repairParts = core::RepairPartsSettings{"parts",
		{core::FaultRepairSettings{"panel-puncture", 10, 2, "waste"}}};
	require(!core::validateStationSystemsSettings(bad, diagnostic),
		"cross-kind repair plan accepted on a heat engine");
	bad = thermalPlantStation();
	bad.chillers[0].repairParts = core::RepairPartsSettings{"parts",
		{core::FaultRepairSettings{"coolant-leak", 10, 2, "waste"}}};
	require(!core::validateStationSystemsSettings(bad, diagnostic),
		"loop repair plan accepted on a chiller");
}

// The opt-in parameters stay live-tunable where the contract says so, and
// frozen where it says so: rates retune, the charge and destination do
// not move under a running scenario.
void thermalPlantLiveTuning(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = thermalPlantStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	// Rates and repair-plan quantities are live parameters.
	require(run.updateNode<core::ChillerSettings>("chiller", [](auto& c)
		{ c.refrigerantLeak->specificHeatKJPerKgK = 3.0; }), "live cp retune refused");
	require(run.updateNode<core::RadiatorSettings>("rad", [](auto& r)
		{ r.panelPuncture->leakRateKgPerS = 2.0; }), "live puncture rate retune refused");
	require(run.updateNode<core::HeatEngineSettings>("orc", [](auto& e)
		{ e.repairParts->repairs[0].durationSeconds = 20.0; }), "live repair duration retune refused");
	// The charge size and the destination compartment are frozen topology.
	require(!run.updateNode<core::ChillerSettings>("chiller", [](auto& c)
		{ c.refrigerantLeak->chargeKg = 99.0; }), "live charge-size edit accepted");
	require(!run.updateNode<core::RadiatorSettings>("rad", [](auto& r)
		{ r.panelPuncture->compartmentId = "elsewhere"; }), "live destination edit accepted");
	// The opt-in itself is frozen: a running scenario cannot add or drop
	// a fault mode.
	require(!run.updateNode<core::HeatEngineSettings>("orc", [](auto& e)
		{ e.mechanicalSeizure = false; }), "live opt-in drop accepted");
	require(run.injectFault("rad", "panel-puncture", {}) && run.advanceTo(2),
		"puncture drive after live tuning failed");
	// The puncture runs at the retuned 2.0 kg/s rate; the refrigerant
	// leak was never injected so its retuned cp never books anything.
	require(std::abs(run.snapshot().thermal.radiators[0].leakedCoolantKg - 2.0) < 1e-9,
		"retuned puncture rate did not take effect live");
	require(run.snapshot().chillers[0].leakedRefrigerantKg == 0.0,
		"uninjected leak booked mass");
}
}
void registerStationThermalPlantFaultChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"thermalPlantMonitors", thermalPlantMonitors});
	checks.push_back({"thermalPlantInjection", thermalPlantInjection});
	checks.push_back({"thermalPlantRepairs", thermalPlantRepairs});
	checks.push_back({"thermalPlantOutages", thermalPlantOutages});
	checks.push_back({"thermalPlantReplay", thermalPlantReplay});
	checks.push_back({"thermalPlantOptIn", thermalPlantOptIn});
	checks.push_back({"thermalPlantLiveTuning", thermalPlantLiveTuning});
}
