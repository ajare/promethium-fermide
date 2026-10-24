#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <limits>

namespace
{
using namespace smoke::station;
core::StationSystemsSettings punctureStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::ThermalReservoirSettings reservoir;
	reservoir.id = "sink"; reservoir.thermalClass = "moderate";
	reservoir.heatCapacityKJPerK = 10000;
	reservoir.startingTemperatureC = 20;
	s.thermalReservoirs = {reservoir};
	core::CoolantLoopSettings loop;
	loop.id = "loop"; loop.reservoirId = "sink";
	loop.coolantCapacityKg = loop.startingCoolantKg = 100;
	loop.specificHeatKJPerKgK = 4; loop.startingTemperatureC = 20;
	loop.ratedFlowKgPerS = 10; loop.pumpPowerKW = 1;
	loop.coolantLeak = core::CoolantLeakSettings{"room", 40};
	s.coolantLoops = {loop};
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	core::SolarArraySettings solar;
	solar.id = "solar"; solar.capacityKW = 10;
	s.solarArrays = {solar};
	return s;
}
void faultInjection(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (bool critical : {false, true})
	{
		auto settings = punctureStation();
		settings.coolantLoops[0].criticalService = critical;
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.advanceTo(1), "initial drive failed");
		auto const before = run.snapshot();
		auto const events = run.events();
		std::string diagnostic;
		for (auto const& request : std::vector<std::pair<std::string, std::string>>{
			{"missing", "coolant-leak"}, {"loop", "missing"}, {"solar", "coolant-leak"}})
		{
			require(!run.injectFault(request.first, request.second, {}, &diagnostic) && !diagnostic.empty(),
				"invalid fault injection accepted or unexplained");
			require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
				"refused command mutated state");
		}
		require(run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::AgentAction, "agent:7"}),
			"valid injection refused");
		require(run.snapshot() == before && run.events() == events, "command applied before tick boundary");
		require(!run.injectFault("loop", "coolant-leak", {}, &diagnostic), "pending duplicate accepted");
		require(run.advanceTo(1.25), "fault booking failed audit");
		auto const snap = run.snapshot();
		auto const& loop = snap.coolantLoops[0];
		require(snap.healthy && loop.coolantKg == 60 && snap.atmospheres[0].spilledCoolantKg == 40,
			"coolant mass was not conservatively transferred");
		require(loop.leakedCoolantEnergyKJ == snap.atmospheres[0].spilledCoolantEnergyKJ
			&& std::abs(loop.residualKJ) < 1e-7, "carried coolant heat did not close");
		require(std::abs(loop.flowKgPerS - 6) < 1e-10 && loop.availabilityFactor == 0.6,
			"remaining charge was not the circulation cap");
		require(loop.activeFaults.size() == 1 && loop.activeFaults[0].modeId == "coolant-leak"
			&& loop.activeFaults[0].onsetTick == before.worldTick + 1
			&& loop.activeFaults[0].causeKind == core::FaultCause::Kind::AgentAction
			&& loop.activeFaults[0].causeDetail == "agent:7"
			&& loop.activeFaults[0].derivedImpact == 0.4, "fault snapshot lost onset or impact");
		require(std::count_if(run.events().begin(), run.events().end(), [](auto const& e) { return e.category == "fault"; }) == 1,
			"onset missing from bounded fault event log");
		auto const faultEvents = run.events();
		require(!run.injectFault("loop", "coolant-leak", {}, &diagnostic)
			&& run.snapshot() == snap && run.events() == faultEvents, "active duplicate mutated state");
		require(run.advanceTo(10) && run.snapshot().coolantLoops[0].coolantKg == 60,
			"one-shot leak booked more than once");
		auto const histories = run.metrics();
		auto const spill = std::find_if(histories.begin(), histories.end(), [](auto const& history)
			{ return history.descriptor.id == "coolant-spill/room/coolant/inventory"; });
		require(spill != histories.end() && !spill->samples.empty() && spill->samples.back().actual == 40,
			"retained spill disappeared from accounted material inventory history");
	}
}
void faultOutages(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (double cap : {0.0, 0.3, 0.8})
	{
		auto settings = punctureStation();
		core::InstallationOutageSettings outage;
		outage.id = "outage"; outage.installationId = "loop"; outage.capacityFraction = cap;
		outage.firstOccurrenceSeconds = 0; outage.durationSeconds = 2;
		settings.outages = {outage};
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(1), "outaged leak failed");
		auto const loop = run.snapshot().coolantLoops[0];
		require(loop.availabilityFactor == std::min(cap, 0.6)
			&& std::abs(loop.flowKgPerS - 10 * std::min(cap, 0.6)) < 1e-10,
			"fault and Outage multiplied instead of deepest-wins");
		require(run.advanceTo(3) && run.snapshot().coolantLoops[0].availabilityFactor == 0.6,
			"Outage restoration cleared the fault cap");
	}
	for (double mass : {100.0, 200.0})
	{
		auto settings = punctureStation(); settings.coolantLoops[0].coolantLeak->releasedKg = mass;
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(3), "dry leak failed");
		require(run.snapshot().coolantLoops[0].coolantKg == 0
			&& run.snapshot().coolantLoops[0].flowKgPerS == 0
			&& run.snapshot().atmospheres[0].spilledCoolantKg == 100, "dry loop invented coolant or flow");
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
		"fault replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes, "fault command log did not round-trip byte-exactly");
	require(restored->advanceTo(12), "restored fault failed subsequent audited drive");
}
void faultReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = punctureStation();
	auto second = settings.coolantLoops[0]; second.id = "second";
	settings.coolantLoops.push_back(second);
	core::StationSystemsRuntime run(settings, 0.25f);
	require(run.advanceTo(1) && run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::ScenarioEvent, "strike"}), "inject failed");
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.coolantLeak->releasedKg = 5; }),
		"live booking-size edit failed");
	require(run.updateNode<core::SolarArraySettings>("solar", [](auto& a) { a.capacityKW = 7; }),
		"live supply edit failed");
	// Pending commands survive saves without prematurely applying the booking.
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(2) && run.injectFault("second", "coolant-leak", {core::FaultCause::Kind::Editor, "debug"})
		&& run.advanceTo(10), "scripted injection sequence failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.snapshot().coolantLoops[0].leakedCoolantKg == 40, "retuning changed an accepted fault's effective parameters");
	// Settings opt-in also crosses both persistence encodings, with a schema guard.
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, settings); out->endMap(); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored; core::readStationSystemsSettingsMap(*in, 112, restored);
		require(restored == settings, "fault settings did not survive serialization");
	}
	core::StationConfigurationDocument document; document.settings = settings;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 112") != std::string::npos, "fault opt-in did not bump the wire schema");
	yaml.replace(yaml.find("schemaVersion: 112"), std::string("schemaVersion: 112").size(), "schemaVersion: 111");
	std::string diagnostic;
	require(!core::parseStationConfiguration(yaml, document, &diagnostic)
		&& diagnostic.find("schema 112") != std::string::npos, "pre-112 document silently accepted faults");
	{
		auto bad = replayBytes<core::YamlSerializer>(run);
		auto const at = bad.find("causeKind: 1");
		require(at != std::string::npos, "missing persisted cause");
		bad.replace(at, std::string("causeKind: 1").size(), "causeKind: 99");
		auto in = core::YamlSerializer::fromString(bad); in->deserialize(); in->beginMap("root");
		bool refused = false;
		try { core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "invalid persisted fault cause accepted");
	}
}
void faultTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = punctureStation();
	core::StationSystemsRuntime run(settings, 0.25f);
	auto const before = run.snapshot(); auto const events = run.events();
	std::string diagnostic;
	require(!run.injectFault("loop", "coolant-leak", {static_cast<core::FaultCause::Kind>(99), ""}, &diagnostic)
		&& !run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::Editor, std::string(1025, 'x')}, &diagnostic)
		&& run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
		"invalid causes were not rejected whole");
	require(!run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.coolantLeak.reset(); }, &diagnostic),
		"fault opt-in topology changed live");
	auto rewired = settings; rewired.coolantLoops[0].coolantLeak->compartmentId = "missing";
	require(!run.updateSettings(rewired, &diagnostic) && run.effectiveSettings() == settings,
		"fault endpoint was rewired live");
	require(run.injectFault("loop", "coolant-leak", {}), "rollback injection failed");
	// The ordinary audit must roll back the hazard booking and its event,
	// along with all other ledger mutations, if a later subsystem fails.
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.specificHeatKJPerKgK = std::numeric_limits<double>::infinity(); }), "audit poison refused");
	require(!run.advanceTo(1) && !run.snapshot().healthy, "broken hazard booking did not fail the audit");
	require(run.snapshot().coolantLoops[0].coolantKg == 100
		&& run.snapshot().coolantLoops[0].activeFaults.empty()
		&& run.snapshot().atmospheres[0].spilledCoolantKg == 0 && run.events() == events,
		"failed audit published a partial fault, spill or event");
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = settings; offered.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.advanceTo(1) && stepped.injectFault("loop", "coolant-leak", {})
			&& stepped.advanceTo(5), "fault failed under a supported Station ratio");
		auto const snap = stepped.snapshot();
		require(snap.coolantLoops[0].coolantKg == 60 && snap.atmospheres[0].spilledCoolantKg == 40
			&& snap.coolantLoops[0].availabilityFactor == 0.6, "fault booking depended on step ratio");
	}
}
void faultOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto enabled = punctureStation(), absent = enabled;
	absent.coolantLoops[0].coolantLeak.reset();
	core::StationSystemsRuntime a(enabled, 0.25f), b(absent, 0.25f);
	std::string diagnostic;
	require(!b.injectFault("loop", "coolant-leak", {}, &diagnostic) && !diagnostic.empty(), "absent opt-in accepted fault");
	for (double seconds : {1., 2., 5., 10.})
	{
		require(a.advanceTo(seconds) && b.advanceTo(seconds), "opt-in baseline drive failed");
		require(a.snapshot() == b.snapshot() && a.events() == b.events(), "unused opt-in changed legacy behavior");
	}
	for (double mass : {-1., 0., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto invalid = enabled; invalid.coolantLoops[0].coolantLeak->releasedKg = mass;
		require(!core::validateStationSystemsSettings(invalid, diagnostic), "invalid booking size accepted");
	}
	auto invalid = enabled; invalid.coolantLoops[0].coolantLeak->compartmentId = "missing";
	require(!core::validateStationSystemsSettings(invalid, diagnostic), "unknown hazard destination accepted");
}
}
void registerStationFaultChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"faultInjection", faultInjection});
	checks.push_back({"faultOutages", faultOutages});
	checks.push_back({"faultReplay", faultReplay});
	checks.push_back({"faultOptIn", faultOptIn});
	checks.push_back({"faultTransactions", faultTransactions});
}
