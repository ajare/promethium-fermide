#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// #845: the power-source Fault catalogue. Generator failed-start (the
// start attempt fails while its cranking energy still dissipates into the
// sink as accounted heat), solar string-loss (a declared fraction of
// installed capacity is gone) and tracking-failure (a fixed low
// availability), and battery thermal-runaway (a one-shot heat and
// toxic-species booking into the declared compartment, availability
// zero). Every assertion is made through the station runtime boundary -
// commands, snapshots, events and persistence - never on ledger
// internals. Crew health is out of scope: the toxic contamination is
// observed closing through the atmosphere ledger, not through anyone
// falling ill.
namespace
{
using namespace smoke::station;

// The power-source fixture: a fuelled generator beside a solar array and
// a battery bank sharing one electrical pool, one warm sink for every
// generator heat term, one small load that leaves the array in surplus so
// the bank is visibly charging, and one compartment to receive the
// runaway's hazard booking. All four catalogue modes are opted in.
core::StationSystemsSettings powerSourceStation()
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.systemsStepWorldTicks = 1;
	core::ThermalReservoirSettings sink;
	sink.id = "sink"; sink.thermalClass = "warm";
	sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
	sink.minTemperatureC = -50.0; sink.maxTemperatureC = 300.0;
	s.thermalReservoirs = {sink};
	core::ResourceStoreSettings fuel;
	fuel.id = "fuel"; fuel.unit = "kWh"; fuel.capacity = 1000; fuel.startingQualified = 500;
	s.stores = {fuel};
	core::GeneratorSettings generator;
	generator.id = "gen"; generator.fuelStoreId = "fuel";
	generator.thermalClass = "warm"; generator.heatSinkId = "sink";
	generator.ratedOutputKW = 20; generator.efficiency = 0.4;
	generator.maxRampKWPerSecond = 100;
	generator.commandedOutputKW = 0; // stopped until commanded live
	generator.startupPowerKW = 10; generator.startupSeconds = 1.0;
	generator.shutdownHeatKW = 1; generator.shutdownSeconds = 1.0;
	generator.maxSinkTemperatureC = 90;
	generator.failedStart = true;
	s.generators = {generator};
	core::SolarArraySettings array;
	array.id = "array"; array.capacityKW = 20;
	array.stringLoss = core::SolarStringLossSettings{0.4};
	array.trackingFailure = core::SolarTrackingFailureSettings{0.25};
	s.solarArrays = {array};
	core::BatterySettings bank;
	bank.id = "bank";
	bank.nameplateKWh = 100.0; bank.usableKWh = 90.0; bank.startingStoredKWh = 50.0;
	bank.maxChargeKW = 50.0; bank.maxDischargeKW = 50.0;
	bank.thermalRunaway = core::BatteryThermalRunawaySettings{"room", 500.0, 2.0};
	s.batteries = {bank};
	core::EquipmentLoadSettings load;
	load.id = "shop"; load.electricalDemandKW = 5; load.heatOutputKW = 0.0;
	load.thermalClass = "warm";
	load.coolingDisposition = core::StationCoolingDisposition::Exported;
	s.equipmentLoads = {load};
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	return s;
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

// Every mode injects, applies its effect shape, publishes in the
// snapshot with derived impact and logs its onset; refusals never mutate.
void powerSourceInjection(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = powerSourceStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	auto const before = run.snapshot();
	auto const events = run.events();
	// The machines are live before any Fault: the caps below are real
	// removals, not a fixture that never worked.
	require(before.power.sources[0].availableKW == 20.0
		&& before.power.sources[0].availabilityFactor == 1.0,
		"solar array was not delivering its full capacity before the Fault");
	require(before.power.batteries[0].lastChargeKW > 0.0,
		"battery was not charging from the solar surplus before the Fault");
	std::string diagnostic;
	// Unknown installations and cross-kind injections are refused whole.
	for (auto const& request : std::vector<std::pair<std::string, std::string>>{
		{"missing", "failed-start"}, {"missing", "string-loss"},
		{"missing", "tracking-failure"}, {"missing", "thermal-runaway"},
		{"array", "failed-start"}, {"array", "thermal-runaway"},
		{"bank", "failed-start"}, {"bank", "string-loss"},
		{"bank", "tracking-failure"},
		{"gen", "string-loss"}, {"gen", "tracking-failure"}, {"gen", "thermal-runaway"},
		{"chiller", "failed-start"}, {"rad", "thermal-runaway"}})
	{
		require(!run.injectFault(request.first, request.second, {}, &diagnostic)
			&& !diagnostic.empty(),
			("invalid power-source fault injection accepted or unexplained: "
				+ request.first + "/" + request.second).c_str());
		require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
			"refused command mutated state");
	}
	// String loss: a declared fraction of installed capacity is gone.
	require(run.injectFault("array", "string-loss",
		{core::FaultCause::Kind::ScenarioEvent, "strike"}),
		"valid string-loss injection refused");
	require(run.snapshot() == before && run.events() == events,
		"command applied before tick boundary");
	require(!run.injectFault("array", "string-loss", {}, &diagnostic),
		"pending duplicate accepted");
	require(run.advanceTo(2), "string-loss drive failed audit");
	auto const lost = run.snapshot();
	require(lost.healthy, "string-loss broke the conservation audit");
	auto const& solar = lost.power.sources[0];
	require(std::abs(solar.availabilityFactor - 0.6) < 1e-12
		&& std::abs(solar.availableKW - 12.0) < 1e-9,
		"string-loss did not remove the declared capacity fraction");
	auto const* stringLoss = findFault(solar.activeFaults, "string-loss");
	require(stringLoss && stringLoss->installationId == "array"
		&& stringLoss->onsetTick == before.worldTick + 1
		&& stringLoss->causeKind == core::FaultCause::Kind::ScenarioEvent
		&& stringLoss->causeDetail == "strike"
		&& std::abs(stringLoss->derivedImpact - 0.4) < 1e-12,
		"string-loss snapshot lost onset or derived impact");
	require(countCategory(run, "fault") == 1, "string-loss onset missing from the event log");
	require(!run.injectFault("array", "string-loss", {}, &diagnostic)
		&& run.snapshot() == lost, "active duplicate mutated state");
	// Tracking failure: the stuck array holds its fixed low availability.
	// It composes with the string loss by minimum, never multiplication:
	// min(0.6, 0.25) = 0.25 of installed capacity.
	require(run.injectFault("array", "tracking-failure",
		{core::FaultCause::Kind::AgentAction, "agent:7"}),
		"valid tracking-failure injection refused");
	require(run.advanceTo(3), "tracking-failure drive failed audit");
	auto const stuck = run.snapshot();
	require(stuck.healthy, "tracking-failure broke the conservation audit");
	require(std::abs(stuck.power.sources[0].availabilityFactor - 0.25) < 1e-12
		&& std::abs(stuck.power.sources[0].availableKW - 5.0) < 1e-9,
		"tracking-failure did not pin the array at its fixed availability");
	auto const* tracking = findFault(stuck.power.sources[0].activeFaults, "tracking-failure");
	require(tracking && tracking->derivedImpact == 0.75
		&& tracking->causeKind == core::FaultCause::Kind::AgentAction
		&& tracking->causeDetail == "agent:7",
		"tracking-failure snapshot missing or misattributed");
	require(countCategory(run, "fault") == 2,
		"the two solar onsets did not each log one fault event");
}

// The battery's one-shot booking closes through the atmosphere ledger:
// the compartment keeps exactly the heat and toxic mass the bank booked,
// the heat enters the compartment's own thermal solve, and the bank is
// held out of service in both directions while the Fault stands.
void powerSourceRunaway(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = powerSourceStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	require(run.snapshot().power.batteries[0].lastChargeKW > 0.0,
		"battery was not charging before the runaway");
	require(run.injectFault("bank", "thermal-runaway",
		{core::FaultCause::Kind::ScenarioEvent, "cell failure"}),
		"valid thermal-runaway injection refused");
	require(run.advanceTo(2), "thermal-runaway booking failed audit");
	auto const burned = run.snapshot();
	require(burned.healthy, "thermal-runaway booking broke the conservation audit");
	auto const& room = burned.atmospheres[0];
	require(room.toxicContaminantKg == 2.0 && room.hazardHeatKJ == 500.0,
		"the one-shot toxic-species and heat booking did not land in the compartment");
	// The heat entered the compartment's own thermal solve: 500 kJ into
	// 10 MJ/K raises it 0.05 K, and the ledger closing is the point -
	// crew health is out of scope, the contamination is observed in the
	// atmosphere ledger, not in anyone falling ill.
	require(room.temperatureC > 20.049 && room.temperatureC < 20.051,
		"the booked hazard heat did not warm the compartment through its thermal solve");
	require(std::abs(room.currentEnergyKJ
		- (room.startingEnergyKJ + room.hazardHeatKJ)) < 1e-6,
		"the compartment energy ledger did not close over the hazard heat");
	auto const* runaway = findFault(burned.power.batteries[0].activeFaults, "thermal-runaway");
	require(runaway && runaway->installationId == "bank"
		&& runaway->derivedImpact == 1.0
		&& runaway->causeKind == core::FaultCause::Kind::ScenarioEvent
		&& runaway->causeDetail == "cell failure",
		"thermal-runaway snapshot missing onset, cause or full impact");
	require(sawMessage(run, "one-shot heat and toxic species booked into compartment 'room'"),
		"thermal-runaway onset missing from the event log");
	// The bank is out of service: no charge on the surplus...
	require(burned.power.batteries[0].lastChargeKW == 0.0
		&& burned.power.batteries[0].lastDischargeKW == 0.0,
		"runaway bank still transacted power on onset");
	require(run.advanceTo(6), "post-runaway drive failed audit");
	require(run.snapshot().power.batteries[0].lastChargeKW == 0.0
		&& run.snapshot().power.batteries[0].chargedKWh == burned.power.batteries[0].chargedKWh,
		"runaway bank kept charging while the Fault stood");
	// ...and no discharge when the load outruns the sun. Raise the shop
	// above the array: the deficit stays unmet, the broken bank never
	// comes to the rescue.
	require(run.updateNode<core::EquipmentLoadSettings>("shop", [](auto& load)
		{ load.electricalDemandKW = 30.0; }), "live load raise refused");
	require(run.advanceTo(8), "starved drive failed audit");
	auto const starved = run.snapshot();
	require(starved.healthy, "starved load over a runaway bank broke the audit");
	require(starved.power.batteries[0].lastDischargeKW == 0.0,
		"runaway bank discharged into the deficit");
	require(starved.power.totalUnmetKW > 5.0,
		"the deficit was silently covered instead of reported unmet");
	// The booking is one-shot: it never repeats.
	require(run.advanceTo(12), "long post-runaway drive failed audit");
	require(run.snapshot().atmospheres[0].toxicContaminantKg == 2.0
		&& run.snapshot().atmospheres[0].hazardHeatKJ == 500.0,
		"the one-shot runaway booking repeated");
}

// The failed start: the crank runs, its energy dissipates into the sink
// as accounted heat, and the attempt is zeroed - the machine never
// reaches Running and delivers nothing.
void powerSourceFailedStart(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = powerSourceStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	require(run.snapshot().power.generators[0].phase == core::StationGeneratorPhase::Stopped,
		"generator was not stopped at baseline");
	require(run.injectFault("gen", "failed-start",
		{core::FaultCause::Kind::Editor, "debug"}),
		"valid failed-start injection refused");
	require(run.advanceTo(2), "failed-start onset failed audit");
	auto const onset = run.snapshot();
	require(onset.healthy, "failed-start onset broke the conservation audit");
	auto const* fault = findFault(onset.power.generators[0].activeFaults, "failed-start");
	require(fault && fault->installationId == "gen" && fault->derivedImpact == 1.0
		&& fault->causeKind == core::FaultCause::Kind::Editor
		&& fault->causeDetail == "debug",
		"failed-start snapshot missing onset, cause or full impact");
	require(countCategory(run, "fault") == 1, "failed-start onset missing from the event log");
	// Command the start. The crank runs on the pool's surplus; the
	// startup energy is drawn and dissipated exactly as a healthy start
	// would book it.
	require(run.updateNode<core::GeneratorSettings>("gen", [](auto& g)
		{ g.commandedOutputKW = 20.0; }), "live start command refused");
	require(run.advanceTo(2.25), "crank tick failed audit");
	auto const cranking = run.snapshot();
	require(cranking.power.generators[0].phase == core::StationGeneratorPhase::Starting,
		"the generator did not begin cranking on command");
	require(cranking.power.generators[0].startupDrawKW == 10.0
		&& cranking.power.generators[0].averageOutputKW == 0.0,
		"the crank did not draw its authored power at zero output");
	// The full crank is one second at 10 kW: 10 kJ, all of it heat in
	// the sink. At the end of the fourth tick the attempt fails.
	require(run.advanceTo(3.0), "crank completion failed audit");
	auto const failed = run.snapshot();
	require(failed.healthy, "the failed start broke the conservation audit");
	require(failed.power.generators[0].phase == core::StationGeneratorPhase::Stopped,
		"the failed attempt left the machine in a non-stopped phase");
	require(failed.power.generators[0].startupReceivedKJ == 0.0,
		"the failed attempt did not reset its crank");
	require(std::abs(failed.power.generators[0].startupHeatKJ - 10.0) < 1e-9,
		"the cranking energy did not dissipate as the full authored amount");
	require(failed.power.generators[0].electricalKJ == 0.0
		&& failed.power.generators[0].outputKW == 0.0
		&& failed.power.generators[0].fuelDrawKWh == 0.0,
		"the zeroed attempt delivered output or burned fuel");
	// The heat landed in the sink: 10 kJ into 10 MJ/K is 0.001 K.
	require(failed.thermal.reservoirs[0].temperatureC > 20.0009,
		"the cranking heat did not reach the sink reservoir");
	require(sawMessage(run, "start attempt failed"),
		"the failed attempt was not logged as a fault event");
	// While the Fault stands every attempt ends the same way: command it
	// again and the crank runs and fails once more, dissipating another
	// accounted 10 kJ and producing nothing.
	require(run.advanceTo(4.0), "second attempt failed audit");
	auto const second = run.snapshot();
	require(second.healthy && second.power.generators[0].phase == core::StationGeneratorPhase::Stopped,
		"the second attempt did not fail back to stopped");
	require(std::abs(second.power.generators[0].startupHeatKJ - 20.0) < 1e-9
		&& second.power.generators[0].electricalKJ == 0.0,
		"the second attempt did not dissipate its own cranking energy");
	// Take the command away: the machine rests stopped, and the Fault
	// stays on the ledger with its full impact.
	require(run.updateNode<core::GeneratorSettings>("gen", [](auto& g)
		{ g.commandedOutputKW = 0.0; }), "live stop command refused");
	require(run.advanceTo(6.0), "post-command drive failed audit");
	require(findFault(run.snapshot().power.generators[0].activeFaults, "failed-start") != nullptr,
		"the failed-start Fault vanished");
	// Both zero-duration and zero-power starts are legal. Neither may
	// bypass the Fault by transitioning straight from Stopped to Running.
	for (bool zeroDuration : {false, true})
	{
		auto instant = s;
		instant.generators[0].commandedOutputKW = 20.0;
		if (zeroDuration) instant.generators[0].startupSeconds = 0.0;
		else instant.generators[0].startupPowerKW = 0.0;
		core::StationSystemsRuntime instantRun(instant, 0.25f);
		require(instantRun.injectFault("gen", "failed-start", {})
			&& instantRun.advanceTo(1.0), "zero-energy failed-start drive failed");
		auto const result = instantRun.snapshot();
		auto const& generator = result.power.generators[0];
		require(result.healthy && generator.phase == core::StationGeneratorPhase::Stopped
			&& generator.electricalKJ == 0.0 && generator.fuelDrawKWh == 0.0
			&& generator.startupHeatKJ == 0.0 && generator.startupReceivedKJ == 0.0,
			"zero-energy start bypassed failed-start or invented crank heat");
		require(sawMessage(instantRun, "start attempt failed"),
			"zero-energy failed attempt was not logged");
	}
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
		"power-source fault replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes,
		"power-source fault command log did not round-trip byte-exactly");
	require(restored->advanceTo(40), "restored power-source faults failed subsequent audited drive");
}

// A scripted sequence across all four modes, with a live parameter
// retune, replays exactly through both serializers.
void powerSourceReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = powerSourceStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	require(run.injectFault("array", "string-loss",
		{core::FaultCause::Kind::ScenarioEvent, "strike"}), "string-loss inject failed");
	require(run.advanceTo(2), "string-loss drive failed");
	// Retune the loss fraction live: the accepted injection froze the
	// old fraction, so the running Fault keeps 0.4 regardless.
	require(run.updateNode<core::SolarArraySettings>("array", [](auto& a)
		{ a.stringLoss->lossFraction = 0.9; }), "live loss-fraction retune failed");
	require(run.advanceTo(4), "retuned drive failed");
	require(std::abs(run.snapshot().power.sources[0].availabilityFactor - 0.6) < 1e-12,
		"live retune reached a frozen string-loss's effective fraction");
	require(run.injectFault("array", "tracking-failure", {core::FaultCause::Kind::Editor, "x"})
		&& run.injectFault("gen", "failed-start", {core::FaultCause::Kind::AgentAction, "agent:1"})
		&& run.advanceTo(6), "tracking/failed-start drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.updateNode<core::GeneratorSettings>("gen", [](auto& g)
		{ g.commandedOutputKW = 20.0; })
		&& run.advanceTo(8), "crank-in-flight snapshot failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.injectFault("bank", "thermal-runaway",
		{core::FaultCause::Kind::ScenarioEvent, "cell failure"}), "runaway inject failed");
	require(run.advanceTo(12), "runaway drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(20), "long replay drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	// Pending commands, including retunes accepted after injection at the
	// same boundary, must retain command order and the original booking.
	core::StationSystemsRuntime pending(s, 0.25f);
	require(pending.injectFault("bank", "thermal-runaway", {})
		&& pending.updateNode<core::BatterySettings>("bank", [](auto& b)
			{ b.thermalRunaway->heatKJ = 900.0; }), "pending replay setup failed");
	roundTrip<core::BinarySerializer>(pending); roundTrip<core::YamlSerializer>(pending);
	// Frozen stream parameters must agree with injection-time settings.
	// Test valid-but-inconsistent values as well as invalid values: replay
	// must not silently pin either onto an otherwise validated command.
	for (auto const& request : std::vector<std::pair<std::string, std::string>>{
		{"string-loss", "lossFraction"}, {"tracking-failure", "availability"},
		{"thermal-runaway", "heatKJ"}, {"thermal-runaway", "toxicKg"}})
	{
		core::StationSystemsRuntime source(s, 0.25f);
		require(source.injectFault(request.first == "thermal-runaway" ? "bank" : "array",
			request.first, {}), "replay consistency setup failed");
		for (auto const& replacement : {"0.8", "-1"})
		{
			auto bytes = replayBytes<core::YamlSerializer>(source);
			auto const field = bytes.rfind(request.second + ": ");
			require(field != std::string::npos, "replay parameter field missing");
			auto const start = field + request.second.size() + 2;
			bytes.replace(start, bytes.find('\n', start) - start, replacement);
			bool refused = false;
			try
			{
				auto in = core::YamlSerializer::fromString(bytes);
				in->deserialize(); in->beginMap("root");
				core::StationSystemsRuntime::readFaultReplay(*in, s, 0.25f);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused, "replay accepted inconsistent power-source parameters");
		}
	}
}

// Opt-in and validation: absent opt-ins refuse every mode and keep the
// previous wire shape; malformed blocks are refused at authoring time.
void powerSourceOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// Feature-absent: every mode refused, no new wire fields.
	auto bare = powerSourceStation();
	bare.generators[0].failedStart = false;
	bare.solarArrays[0].stringLoss.reset();
	bare.solarArrays[0].trackingFailure.reset();
	bare.batteries[0].thermalRunaway.reset();
	core::StationSystemsRuntime run(bare, 0.25f);
	require(run.advanceTo(1), "bare drive failed");
	std::string diagnostic;
	for (auto const& request : std::vector<std::pair<std::string, std::string>>{
		{"gen", "failed-start"}, {"array", "string-loss"},
		{"array", "tracking-failure"}, {"bank", "thermal-runaway"}})
		require(!run.injectFault(request.first, request.second, {}, &diagnostic)
			&& diagnostic.find("not enabled") != std::string::npos,
			("mode without opt-in accepted or unexplained: " + request.second).c_str());
	// The feature-absent array still delivers its full capacity: the
	// fault layer is invisible when it is not opted in.
	require(run.snapshot().power.sources[0].availabilityFactor == 1.0
		&& run.snapshot().power.sources[0].availableKW == 20.0,
		"feature-absent solar array did not keep its previous output");
	core::StationConfigurationDocument bareDoc; bareDoc.settings = bare;
	auto bareYaml = core::serializeStationConfiguration(bareDoc);
	require(bareYaml.find("schemaVersion: 111") != std::string::npos,
		"feature-absent station did not keep its previous schema tier");
	require(bareYaml.find("stringLoss") == std::string::npos
		&& bareYaml.find("trackingFailure") == std::string::npos
		&& bareYaml.find("failedStart") == std::string::npos
		&& bareYaml.find("thermalRunaway") == std::string::npos,
		"feature-absent station carried power-source fields");
	// The full fixture stamps the new tier and round-trips through both
	// serializers.
	auto const full = powerSourceStation();
	core::StationConfigurationDocument fullDoc; fullDoc.settings = full;
	auto const yaml = core::serializeStationConfiguration(fullDoc);
	require(yaml.find("schemaVersion: 118") != std::string::npos,
		"power-source opt-in did not bump the wire schema");
	core::StationConfigurationDocument restored;
	require(core::parseStationConfiguration(yaml, restored, &diagnostic)
		&& restored.settings == full, "power-source settings did not round-trip");
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
		core::readStationSystemsSettingsMap(*in, 118, restoredSettings);
		require(restoredSettings == full, "power-source settings did not survive serialization");
		// A pre-118 reader refuses the new blocks.
		bool refused = false;
		try
		{
			core::StationSystemsSettings oldReader;
			in->deserialize(); in->beginMap("root");
			core::readStationSystemsSettingsMap(*in, 117, oldReader);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "pre-118 document silently accepted power-source blocks");
	}
	// Authoring validation refuses malformed blocks.
	auto bad = powerSourceStation();
	bad.solarArrays[0].stringLoss = core::SolarStringLossSettings{0.0};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("string-loss") != std::string::npos,
		"zero string-loss fraction accepted");
	bad = powerSourceStation();
	bad.solarArrays[0].stringLoss = core::SolarStringLossSettings{1.5};
	require(!core::validateStationSystemsSettings(bad, diagnostic),
		"over-one string-loss fraction accepted");
	bad = powerSourceStation();
	bad.solarArrays[0].trackingFailure = core::SolarTrackingFailureSettings{1.0};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("tracking-failure") != std::string::npos,
		"full-availability tracking failure accepted");
	bad = powerSourceStation();
	bad.batteries[0].thermalRunaway = core::BatteryThermalRunawaySettings{"room", 0, 2};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("thermal-runaway") != std::string::npos,
		"zero runaway heat accepted");
	bad = powerSourceStation();
	bad.batteries[0].thermalRunaway = core::BatteryThermalRunawaySettings{"room", 500, -1};
	require(!core::validateStationSystemsSettings(bad, diagnostic),
		"negative runaway toxic mass accepted");
	bad = powerSourceStation();
	bad.batteries[0].thermalRunaway = core::BatteryThermalRunawaySettings{"nowhere", 500, 2};
	require(!core::validateStationSystemsSettings(bad, diagnostic),
		"unknown runaway destination accepted");
}

// The opt-in parameters stay live-tunable where the contract says so and
// frozen where it says so: fractions retune before injection, never
// after; the opt-ins themselves never move.
void powerSourceLiveTuning(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const s = powerSourceStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1), "baseline drive failed");
	// Before any injection the declared fractions are live parameters.
	require(run.updateNode<core::SolarArraySettings>("array", [](auto& a)
		{ a.trackingFailure->availability = 0.5; }), "live tracking-availability retune refused");
	require(run.updateNode<core::BatterySettings>("bank", [](auto& b)
		{ b.thermalRunaway->heatKJ = 800.0; }), "live runaway heat retune refused");
	// The opt-in presence and the runaway destination are frozen.
	require(!run.updateNode<core::SolarArraySettings>("array", [](auto& a)
		{ a.stringLoss.reset(); }), "live opt-in drop accepted");
	require(!run.updateNode<core::GeneratorSettings>("gen", [](auto& g)
		{ g.failedStart = false; }), "live generator opt-in drop accepted");
	require(!run.updateNode<core::BatterySettings>("bank", [](auto& b)
		{ b.thermalRunaway->compartmentId = "elsewhere"; }), "live destination edit accepted");
	// An accepted injection freezes its effective fraction: a later
	// retune cannot reach the running Fault.
	require(run.injectFault("array", "string-loss", {}) && run.advanceTo(2),
		"string-loss drive failed");
	require(run.updateNode<core::SolarArraySettings>("array", [](auto& a)
		{ a.stringLoss->lossFraction = 0.9; }), "post-injection retune refused");
	require(run.advanceTo(4), "post-injection drive failed");
	require(std::abs(run.snapshot().power.sources[0].availabilityFactor - 0.6) < 1e-12,
		"post-injection retune reached the frozen string-loss fraction");
	// The tracking failure runs at its retuned 0.5 availability; the
	// two compose by minimum: min(0.6, 0.5) = 0.5.
	require(run.injectFault("array", "tracking-failure", {}) && run.advanceTo(5),
		"tracking-failure drive failed");
	require(std::abs(run.snapshot().power.sources[0].availabilityFactor - 0.5) < 1e-12,
		"retuned tracking availability did not take effect live");
	// The runaway books the heat that was live when it was accepted.
	require(run.injectFault("bank", "thermal-runaway", {}) && run.advanceTo(6),
		"runaway drive failed");
	require(run.snapshot().atmospheres[0].hazardHeatKJ == 800.0,
		"the runaway did not book the live-retuned heat frozen at injection");
}
}
void registerStationPowerSourceFaultChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"powerSourceInjection", powerSourceInjection});
	checks.push_back({"powerSourceRunaway", powerSourceRunaway});
	checks.push_back({"powerSourceFailedStart", powerSourceFailedStart});
	checks.push_back({"powerSourceReplay", powerSourceReplay});
	checks.push_back({"powerSourceOptIn", powerSourceOptIn});
	checks.push_back({"powerSourceLiveTuning", powerSourceLiveTuning});
}
