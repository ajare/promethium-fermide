// #810: prefab expansion headless checks.
//
// The bundled aircon-unit template - a chiller, its coolant loop, the
// cold-side reservoir the loop serves, the condenser sink reservoir and the
// radiator that terminates the condenser heat inside the box - is the
// reference fixture. The checks cover template parsing and refusals,
// namespaced expansion with internal links written, override application
// and refusals, per-type cap refusal that leaves the draft untouched, and
// determinism.
//
// #811 adds the composite-boundary checks: the exposed-pin resolver maps
// a composite port reference to the aliased internal pin, a link through
// the composite lands the same reference field, verdict and refusal
// reason as the equivalent direct link, and catalogue-load template
// validation runs the existing validator over the isolated expansion
// with stub peers on the exposed ports.
#include "StationPrefabChecks.h"
#include "StationRig.h"

#include "core/StationPrefab.h"
#include "core/StationConfigurationDocument.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace
{
	using namespace smoke::station;

	// The bundled aircon template. The chiller's condenser sink must differ
	// from the reservoir its cold loop serves - the validator refuses a
	// chiller that rejects into the reservoir it cools - so the unit carries
	// two reservoirs: the cooled side and the radiator-served sink.
	char const* airconPrefabYaml()
	{
		return R"YAML(
id: aircon-unit
displayName: Aircon Unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  thermalReservoirs:
    - id: coldside
      thermalClass: warm
      heatCapacityKJPerK: 500
      startingTemperatureC: 24
      minTemperatureC: 0
      maxTemperatureC: 40
      absorbedHeatKW: 2
    - id: sink
      thermalClass: warm
      heatCapacityKJPerK: 200
      startingTemperatureC: 20
      minTemperatureC: 0
      maxTemperatureC: 40
      absorbedHeatKW: 0
  coolantLoops:
    - id: loop
      reservoirId: coldside
      coolantCapacityKg: 100
      startingCoolantKg: 80
      specificHeatKJPerKgK: 4.0
      startingTemperatureC: 20
      freezeProtectionSetpointC: 5
      pumpPowerKW: 0.5
      ratedFlowKgPerS: 1.0
      exchangerConductanceKWPerK: 2.0
      heaterCapacityKW: 1.0
      criticalService: false
  chillers:
    - id: machine
      thermalClass: warm
      coldLoopId: loop
      condenserReservoirId: sink
      ratedCoolingCapacityKW: 5
      cop: 3
      auxiliaryPowerKW: 0.2
      chilledSetpointC: 5
      maxCondensingTemperatureC: 45
      criticalService: false
  radiators:
    - id: radiator
      reservoirId: sink
      areaM2: 4
      emissivity: 0.9
      viewFactor: 1.0
      sinkTemperatureC: -30
exposedPorts:
  - name: Cold supply
    nodeType: Coolant loop
    entryId: loop
    portName: Cold interface
  - name: Supervision
    nodeType: Chiller
    entryId: machine
    portName: Supervision
parameters:
  - name: setpoint
    nodeType: Chiller
    entryId: machine
    field: chilledSetpointC
    default: 5
    min: 0
    max: 15
  - name: capacity
    nodeType: Chiller
    entryId: machine
    field: ratedCoolingCapacityKW
    default: 5
    min: 0
    max: 20
  - name: criticalService
    nodeType: Chiller
    entryId: machine
    field: criticalService
    kind: boolean
    default: false
)YAML";
	}

	core::PrefabTemplate parseAircon()
	{
		core::PrefabTemplate tmpl;
		std::string diagnostic;
		require(core::parsePrefabTemplate(airconPrefabYaml(), tmpl, &diagnostic),
			("bundled aircon template must parse: " + diagnostic).c_str());
		return tmpl;
	}

	// A draft holding the placed aircon unit as instance ac1 plus one
	// outside Outage node that can target the unit's Supervision port.
	struct CompositeFixture
	{
		core::StationSystemsSettings draft;
		core::PlacedPrefab placed;
	};

	CompositeFixture placeAirconComposite()
	{
		auto const tmpl = parseAircon();
		core::PrefabExpansion expansion;
		std::string reason;
		require(core::expandPrefab(tmpl, {}, "ac1", expansion, reason),
			("expansion for the boundary checks must succeed: " + reason).c_str());
		CompositeFixture fixture;
		fixture.draft.enabled = true;
		require(core::applyPrefabExpansion(fixture.draft, expansion, reason),
			("applying the expansion must succeed: " + reason).c_str());
		core::InstallationOutageSettings outage;
		outage.id = "ac-outage";
		outage.firstOccurrenceSeconds = 0.0;
		outage.durationSeconds = 60.0;
		fixture.draft.outages.push_back(outage);
		fixture.placed = core::PlacedPrefab{"aircon-unit", "ac1", expansion.exposedPins};
		return fixture;
	}

	// Splice an extra exposed-port entry into the bundled template YAML,
	// just before its parameters section.
	std::string withExtraExposedPort(std::string const& yaml, char const* extraPort)
	{
		auto const pos = yaml.find("parameters:");
		require(pos != std::string::npos, "bundled template yaml should carry a parameters section");
		return yaml.substr(0, pos) + std::string(extraPort) + yaml.substr(pos);
	}

	void prefabTemplateParse(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		require(tmpl.id == "aircon-unit", "template id should be aircon-unit");
		require(tmpl.displayName == "Aircon Unit", "template display name should be Aircon Unit");
		// Internal entries are ordinary stationSystems entries read through
		// the authoritative reader.
		require(tmpl.entries.chillers.size() == 1
			&& tmpl.entries.chillers.front().id == "machine",
			"template should carry the chiller as an internal entry");
		require(tmpl.entries.coolantLoops.size() == 1
			&& tmpl.entries.coolantLoops.front().id == "loop",
			"template should carry the coolant loop as an internal entry");
		require(tmpl.entries.thermalReservoirs.size() == 2,
			"template should carry the cooled-side and condenser-sink reservoirs");
		require(tmpl.entries.radiators.size() == 1
			&& tmpl.entries.radiators.front().id == "radiator",
			"template should carry the radiator as an internal entry");
		// The internal links are authored in the entry vocabulary verbatim.
		require(tmpl.entries.chillers.front().coldLoopId == "loop"
			&& tmpl.entries.chillers.front().condenserReservoirId == "sink"
			&& tmpl.entries.coolantLoops.front().reservoirId == "coldside"
			&& tmpl.entries.radiators.front().reservoirId == "sink",
			"template internal references should be read as authored");
		// Exposed-port aliases and parameter mappings.
		require(tmpl.exposedPorts.size() == 2, "template should expose two ports");
		require(tmpl.exposedPorts[0].name == "Cold supply"
			&& tmpl.exposedPorts[0].nodeType == "Coolant loop"
			&& tmpl.exposedPorts[0].entryId == "loop"
			&& tmpl.exposedPorts[0].portName == "Cold interface",
			"Cold supply should alias the coolant loop's Cold interface");
		require(tmpl.exposedPorts[1].name == "Supervision"
			&& tmpl.exposedPorts[1].nodeType == "Chiller"
			&& tmpl.exposedPorts[1].entryId == "machine"
			&& tmpl.exposedPorts[1].portName == "Supervision",
			"Supervision should alias the chiller's Supervision input");
		require(tmpl.parameters.size() == 3, "template should expose three dials");
		require(tmpl.parameters[0].name == "setpoint"
			&& tmpl.parameters[0].field == "chilledSetpointC"
			&& tmpl.parameters[0].kind == core::PrefabParameterKind::Number
			&& tmpl.parameters[0].defaultValue == 5.0
			&& tmpl.parameters[0].minValue == 0.0
			&& tmpl.parameters[0].maxValue == 15.0,
			"setpoint dial should map the chiller setpoint with its declared range");
		require(tmpl.parameters[2].name == "criticalService"
			&& tmpl.parameters[2].kind == core::PrefabParameterKind::Boolean
			&& tmpl.parameters[2].boolDefault == false,
			"criticalService dial should be boolean with a false default");
	}

	void prefabTemplateRefusals(smoke::Context const&)
	{
		auto refuse = [](char const* yaml, char const* expected) {
			core::PrefabTemplate tmpl;
			std::string diagnostic;
			auto const ok = core::parsePrefabTemplate(yaml, tmpl, &diagnostic);
			require(!ok && diagnostic.find(expected) != std::string::npos,
				("expected refusal containing '" + std::string(expected)
					+ "', got ok=" + std::to_string(ok) + " diagnostic=" + diagnostic).c_str());
		};
		// A template entry id carrying the namespacing separator is refused.
		refuse(R"YAML(
id: bad-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  thermalReservoirs:
    - id: a.b
      thermalClass: warm
      heatCapacityKJPerK: 100
      startingTemperatureC: 20
      minTemperatureC: 0
      maxTemperatureC: 40
      absorbedHeatKW: 0
)YAML",
			"reserved separator");
		// A malformed internal entry is refused by the existing reader and
		// validator, atomically.
		refuse(R"YAML(
id: broken-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  chillers:
    - id: machine
      thermalClass: not-a-class
      coldLoopId: loop
      condenserReservoirId: sink
      ratedCoolingCapacityKW: 5
      cop: 3
      auxiliaryPowerKW: 0.2
      chilledSetpointC: 5
      maxCondensingTemperatureC: 45
)YAML",
			"thermal service class");
		// An exposed port aliasing an unknown entry or unknown connector is
		// refused.
		refuse(R"YAML(
id: ghost-port-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
exposedPorts:
  - name: Ghost
    nodeType: Chiller
    entryId: nowhere
    portName: Supervision
)YAML",
			"unknown entry");
		refuse(R"YAML(
id: ghost-connector-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  chillers:
    - id: machine
      thermalClass: warm
      coldLoopId: loop
      condenserReservoirId: sink
      ratedCoolingCapacityKW: 5
      cop: 3
      auxiliaryPowerKW: 0.2
      chilledSetpointC: 5
      maxCondensingTemperatureC: 45
exposedPorts:
  - name: Ghost
    nodeType: Chiller
    entryId: machine
    portName: Nonexistent port
)YAML",
			"unknown port");
		// A parameter mapping a non-dial-addressable field is refused.
		refuse(R"YAML(
id: bad-dial-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  chillers:
    - id: machine
      thermalClass: warm
      coldLoopId: loop
      condenserReservoirId: sink
      ratedCoolingCapacityKW: 5
      cop: 3
      auxiliaryPowerKW: 0.2
      chilledSetpointC: 5
      maxCondensingTemperatureC: 45
parameters:
  - name: sneaky
    nodeType: Chiller
    entryId: machine
    field: condenserReservoirId
    default: 1
    min: 0
    max: 2
)YAML",
			"non-dial-addressable");
		// A numeric default outside the declared range is refused.
		refuse(R"YAML(
id: bad-default-unit
stationSystems:
  enabled: true
  observationIntervalSeconds: 1.0
  chillers:
    - id: machine
      thermalClass: warm
      coldLoopId: loop
      condenserReservoirId: sink
      ratedCoolingCapacityKW: 5
      cop: 3
      auxiliaryPowerKW: 0.2
      chilledSetpointC: 5
      maxCondensingTemperatureC: 45
parameters:
  - name: setpoint
    nodeType: Chiller
    entryId: machine
    field: chilledSetpointC
    default: 99
    min: 0
    max: 15
)YAML",
			"outside its declared range");
		// A document with no prefab section is refused.
		refuse("somethingElse:\n  id: nope\n", "no internal 'stationSystems' entries");
	}

	void prefabExpansion(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		core::PrefabExpansion expansion;
		std::string reason;
		require(core::expandPrefab(tmpl, {}, "ac1", expansion, reason),
			("plain expansion must succeed: " + reason).c_str());
		// Every internal entry lands with a namespaced id and nothing else.
		require(expansion.entries.chillers.size() == 1
			&& expansion.entries.chillers.front().id == "ac1.machine",
			"chiller should be namespaced ac1.machine");
		require(expansion.entries.coolantLoops.size() == 1
			&& expansion.entries.coolantLoops.front().id == "ac1.loop",
			"coolant loop should be namespaced ac1.loop");
		require(expansion.entries.thermalReservoirs.size() == 2,
			"both reservoirs should be namespaced");
		require(expansion.entries.radiators.size() == 1
			&& expansion.entries.radiators.front().id == "ac1.radiator",
			"radiator should be namespaced ac1.radiator");
		// All internal links are written to the namespaced ids.
		require(expansion.entries.chillers.front().coldLoopId == "ac1.loop"
			&& expansion.entries.chillers.front().condenserReservoirId == "ac1.sink",
			"chiller references should point at namespaced peers");
		require(expansion.entries.coolantLoops.front().reservoirId == "ac1.coldside",
			"loop host reference should point at the namespaced reservoir");
		require(expansion.entries.radiators.front().reservoirId == "ac1.sink",
			"radiator host reference should point at the namespaced sink");
		// The internal links enumerate as authored-link records over the
		// namespaced pins.
		require(std::any_of(expansion.links.begin(), expansion.links.end(),
			[](core::AuthoredLink const& link) {
				return link.identity == "Chiller:ac1.machine:Cold loop"
					&& link.from == core::StationPinRef{"Coolant loop", "ac1.loop", "Cold interface"}
					&& link.to == core::StationPinRef{"Chiller", "ac1.machine", "Cold loop"};
			}),
			"chiller-to-loop internal link should enumerate with its namespaced pins");
		require(std::any_of(expansion.links.begin(), expansion.links.end(),
			[](core::AuthoredLink const& link) {
				return link.identity == "Radiator:ac1.radiator:Host reservoir"
					&& link.from == core::StationPinRef{"Reservoir", "ac1.sink", "Hosted radiators"}
					&& link.to == core::StationPinRef{"Radiator", "ac1.radiator", "Host reservoir"};
			}),
			"radiator-to-sink internal link should enumerate with its namespaced pins");
		// Exposed-port aliases resolve to namespaced pins.
		require(expansion.exposedPins.size() == 2, "both exposed pins should resolve");
		require(expansion.exposedPins.at("Cold supply")
			== core::StationPinRef{"Coolant loop", "ac1.loop", "Cold interface"},
			"Cold supply should resolve to the namespaced loop pin");
		require(expansion.exposedPins.at("Supervision")
			== core::StationPinRef{"Chiller", "ac1.machine", "Supervision"},
			"Supervision should resolve to the namespaced chiller pin");
		// The expansion passes the existing configuration validator on its
		// own, exactly like hand-authored draft entries.
		std::string diagnostic;
		require(core::validateStationSystemsSettings(expansion.entries, diagnostic),
			("expansion must validate: " + diagnostic).c_str());
		// An instance id carrying the separator is refused.
		core::PrefabExpansion dotted;
		require(!core::expandPrefab(tmpl, {}, "a.b", dotted, reason)
			&& reason.find("reserved separator") != std::string::npos,
			("dotted instance id must be refused: " + reason).c_str());
	}

	void prefabOverrides(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		core::PrefabExpansion expansion;
		std::string reason;
		core::PrefabOverrides overrides;
		overrides["setpoint"] = 8.0;
		overrides["capacity"] = 12.0;
		overrides["criticalService"] = true;
		require(core::expandPrefab(tmpl, overrides, "ac1", expansion, reason),
			("override expansion must succeed: " + reason).c_str());
		// Overrides apply to their mapped internal fields on the namespaced
		// entry.
		require(expansion.entries.chillers.front().chilledSetpointC == 8.0,
			"setpoint override should land on chilledSetpointC");
		require(expansion.entries.chillers.front().ratedCoolingCapacityKW == 12.0,
			"capacity override should land on ratedCoolingCapacityKW");
		require(expansion.entries.chillers.front().criticalService,
			"criticalService override should land on the chiller flag");
		// Unmapped fields keep the template values.
		require(expansion.entries.chillers.front().cop == 3.0,
			"unmapped cop should keep the template value");
		// Unknown override names are refused.
		core::PrefabExpansion bad;
		core::PrefabOverrides unknown;
		unknown["temperature"] = 5.0;
		require(!core::expandPrefab(tmpl, unknown, "ac2", bad, reason)
			&& reason.find("no exposed parameter") != std::string::npos,
			("unknown override must be refused: " + reason).c_str());
		// Out-of-range overrides are refused at both ends.
		core::PrefabOverrides tooHigh;
		tooHigh["setpoint"] = 99.0;
		require(!core::expandPrefab(tmpl, tooHigh, "ac2", bad, reason)
			&& reason.find("outside its range") != std::string::npos,
			("above-range override must be refused: " + reason).c_str());
		core::PrefabOverrides tooLow;
		tooLow["capacity"] = -1.0;
		require(!core::expandPrefab(tmpl, tooLow, "ac2", bad, reason)
			&& reason.find("outside its range") != std::string::npos,
			("below-range override must be refused: " + reason).c_str());
		// A kind mismatch is refused in both directions.
		core::PrefabOverrides boolForNumber;
		boolForNumber["setpoint"] = true;
		require(!core::expandPrefab(tmpl, boolForNumber, "ac2", bad, reason)
			&& reason.find("must be a number") != std::string::npos,
			("boolean for a numeric dial must be refused: " + reason).c_str());
		core::PrefabOverrides numberForBool;
		numberForBool["criticalService"] = 1.0;
		require(!core::expandPrefab(tmpl, numberForBool, "ac2", bad, reason)
			&& reason.find("must be boolean") != std::string::npos,
			("number for a boolean dial must be refused: " + reason).c_str());
	}

	// #817: live dial editing on a placed instance. An accepted edit
	// moves the expanded internal entry and the provenance override
	// together in one call; out-of-range, non-finite, kind-mismatched,
	// unknown and orphaned dials are refused with no mutation; the
	// validator net catches an in-range value that would break the
	// flattened unit; and the edited overrides survive the provenance
	// round trip.
	void prefabDialEdit(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		auto fixture = placeAirconComposite();
		auto& draft = fixture.draft;
		auto& placed = fixture.placed;
		std::string reason;
		// A numeric dial lands on the expanded entry and the override in
		// the same step - the two never diverge.
		require(core::applyPrefabParameterOverride(draft, tmpl, placed, "setpoint", 8.0, reason),
			("numeric dial edit must succeed: " + reason).c_str());
		require(draft.chillers.front().chilledSetpointC == 8.0,
			"the dial edit must land on the expanded entry");
		require(placed.overrides.size() == 1 && std::get<double>(placed.overrides.at("setpoint")) == 8.0,
			"the dial edit must record the override in the same step");
		// The boolean dial likewise.
		require(core::applyPrefabParameterOverride(draft, tmpl, placed, "criticalService", true, reason),
			("boolean dial edit must succeed: " + reason).c_str());
		require(draft.chillers.front().criticalService,
			"the boolean dial edit must land on the expanded entry");
		require(placed.overrides.size() == 2 && std::get<bool>(placed.overrides.at("criticalService")),
			"the boolean dial edit must record the override");
		// Out-of-range input is refused at both ends, and a non-finite
		// value never reaches the field, with no mutation of either half.
		auto const snapshot = draft;
		auto const overrides = placed.overrides;
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "setpoint", 99.0, reason)
			&& reason.find("outside its range") != std::string::npos,
			("above-range dial edit must be refused: " + reason).c_str());
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "capacity", -1.0, reason)
			&& reason.find("outside its range") != std::string::npos,
			("below-range dial edit must be refused: " + reason).c_str());
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "capacity",
			std::numeric_limits<double>::quiet_NaN(), reason),
			"a non-finite dial edit must be refused");
		require(draft == snapshot && placed.overrides == overrides,
			"a refused dial edit must leave the draft and the overrides untouched");
		// A kind mismatch is refused in both directions.
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "setpoint", true, reason)
			&& reason.find("must be set with a number") != std::string::npos,
			("boolean for a numeric dial must be refused: " + reason).c_str());
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "criticalService", 1.0, reason)
			&& reason.find("must be set with a boolean") != std::string::npos,
			("number for a boolean dial must be refused: " + reason).c_str());
		// An unknown dial is refused.
		require(!core::applyPrefabParameterOverride(draft, tmpl, placed, "cop", 2.0, reason)
			&& reason.find("no exposed parameter") != std::string::npos,
			("unknown dial edit must be refused: " + reason).c_str());
		// A unit that no longer owns the mapped internal entry is refused.
		core::StationSystemsSettings orphan;
		orphan.enabled = true;
		auto const orphanBefore = orphan;
		require(!core::applyPrefabParameterOverride(orphan, tmpl, placed, "setpoint", 7.0, reason)
			&& reason.find("lost its target field") != std::string::npos,
			("a dial whose entry is gone must be refused: " + reason).c_str());
		require(orphan == orphanBefore, "a refused orphan edit must leave the draft untouched");
		// The dial reader reports the entry's own value, and refuses what
		// the template does not expose or the unit does not own.
		core::PrefabOverrideValue read{};
		require(core::readPrefabParameterValue(draft, tmpl, "ac1", "setpoint", read)
			&& std::get<double>(read) == 8.0,
			"the dial reader must return the expanded entry's value");
		require(core::readPrefabParameterValue(draft, tmpl, "ac1", "criticalService", read)
			&& std::get<bool>(read),
			"the dial reader must return the boolean entry's value");
		require(!core::readPrefabParameterValue(draft, tmpl, "ac9", "setpoint", read),
			"the dial reader must refuse a unit that is not there");
		require(!core::readPrefabParameterValue(draft, tmpl, "ac1", "cop", read),
			"the dial reader must refuse a field the template does not expose");
		// The validator net: a value inside a dial's declared range that
		// would leave the flattened unit invalid is refused without
		// mutation, while a valid in-range value still lands.
		std::string const wideYaml = std::string(airconPrefabYaml())
			+ R"YAML(  - name: efficiency
    nodeType: Chiller
    entryId: machine
    field: cop
    default: 3
    min: -5
    max: 10
)YAML";
		core::PrefabTemplate wide;
		std::string parseDiag;
		require(core::parsePrefabTemplate(wideYaml, wide, &parseDiag),
			("the wide-range template must parse: " + parseDiag).c_str());
		auto const guarded = draft;
		auto const guardedOverrides = placed.overrides;
		require(!core::applyPrefabParameterOverride(draft, wide, placed, "efficiency", -1.0, reason),
			("an in-range value the validator refuses must be refused: " + reason).c_str());
		require(draft == guarded && placed.overrides == guardedOverrides,
			"a validator-refused dial edit must leave both halves untouched");
		require(core::applyPrefabParameterOverride(draft, wide, placed, "efficiency", 4.0, reason),
			("a valid in-range dial edit must succeed: " + reason).c_str());
		require(draft.chillers.front().cop == 4.0
			&& std::get<double>(placed.overrides.at("efficiency")) == 4.0,
			"the accepted dial edit must move entry and override together");
		// The edited overrides survive the provenance round trip: a
		// reconstruction from the saved block re-expands to the same
		// dialed values.
		core::PrefabProvenance provenance{"aircon-unit", "ac1", placed.overrides};
		core::PlacedPrefab rebuilt;
		require(core::reconstructPlacedPrefab(wide, provenance, rebuilt, reason),
			("the edited provenance must reconstruct: " + reason).c_str());
		core::PrefabExpansion reexpanded;
		require(core::expandPrefab(wide, rebuilt.overrides, "ac1", reexpanded, reason),
			("the reconstructed overrides must re-expand: " + reason).c_str());
		require(reexpanded.entries.chillers.front().chilledSetpointC == 8.0
			&& reexpanded.entries.chillers.front().criticalService
			&& reexpanded.entries.chillers.front().cop == 4.0,
			"the round-tripped overrides must carry the dialed values");
	}

	void prefabCapRefusal(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		// A draft already at the chiller cap cannot take another aircon
		// unit: the pre-check counts every slot the template consumes and
		// refuses the whole expansion with a reason.
		core::StationSystemsSettings draft;
		draft.enabled = true;
		for (int i = 0; i < static_cast<int>(core::MaxStationChillers); ++i)
		{
			core::ChillerSettings chiller;
			chiller.id = "full" + std::to_string(i);
			draft.chillers.push_back(chiller);
		}
		std::string reason;
		require(!core::checkPrefabCaps(tmpl, draft, reason)
			&& reason.find("Chiller") != std::string::npos,
			("cap pre-check must refuse with the type named: " + reason).c_str());
		// Applying refuses atomically and leaves the draft untouched.
		core::PrefabExpansion expansion;
		require(core::expandPrefab(tmpl, {}, "ac1", expansion, reason),
			("expansion for the apply check must succeed: " + reason).c_str());
		auto const snapshot = draft;
		require(!core::applyPrefabExpansion(draft, expansion, reason),
			("apply at the cap must refuse: " + reason).c_str());
		require(draft == snapshot, "refused apply must leave the draft untouched");
		// One slot of headroom under the cap applies cleanly.
		core::StationSystemsSettings room;
		room.enabled = true;
		for (int i = 0; i < static_cast<int>(core::MaxStationChillers) - 1; ++i)
		{
			core::ChillerSettings chiller;
			chiller.id = "spare" + std::to_string(i);
			room.chillers.push_back(chiller);
		}
		require(core::applyPrefabExpansion(room, expansion, reason),
			("apply with headroom must succeed: " + reason).c_str());
		require(room.chillers.size() == core::MaxStationChillers
			&& room.chillers.back().id == "ac1.machine"
			&& room.coolantLoops.size() == 1
			&& room.thermalReservoirs.size() == 2
			&& room.radiators.size() == 1,
			"the whole expansion should land as ordinary draft entries");
		// A second instance of the same namespaced ids collides and is
		// refused without disturbing the draft.
		auto const applied = room;
		require(!core::applyPrefabExpansion(room, expansion, reason),
			("duplicate instance ids must be refused: " + reason).c_str());
		require(room == applied, "refused duplicate apply must leave the draft untouched");
	}

	void prefabDeterminism(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		core::PrefabOverrides overrides;
		overrides["setpoint"] = 7.5;
		core::PrefabExpansion first;
		core::PrefabExpansion second;
		std::string reason;
		require(core::expandPrefab(tmpl, overrides, "ac1", first, reason),
			("first expansion must succeed: " + reason).c_str());
		require(core::expandPrefab(tmpl, overrides, "ac1", second, reason),
			("second expansion must succeed: " + reason).c_str());
		// Identical inputs produce identical outputs, entry order included.
		require(first.entries == second.entries,
			"identical inputs must produce identical expanded entries");
		require(first.links == second.links,
			"identical inputs must produce identical expanded links");
		require(first.exposedPins == second.exposedPins,
			"identical inputs must produce identical exposed pins");
	}

	void prefabExposedPinResolution(smoke::Context const&)
	{
		auto const fixture = placeAirconComposite();
		// Each exposed composite pin resolves to the aliased internal pin.
		core::StationPinRef resolved{};
		std::string reason;
		require(core::resolveExposedPin(fixture.placed,
			{"aircon-unit", "ac1", "Cold supply"}, resolved, reason),
			("Cold supply must resolve through the boundary: " + reason).c_str());
		require(resolved == core::StationPinRef{"Coolant loop", "ac1.loop", "Cold interface"},
			"Cold supply should resolve to the aliased namespaced loop pin");
		require(core::resolveExposedPin(fixture.placed,
			{"aircon-unit", "ac1", "Supervision"}, resolved, reason),
			("Supervision must resolve through the boundary: " + reason).c_str());
		require(resolved == core::StationPinRef{"Chiller", "ac1.machine", "Supervision"},
			"Supervision should resolve to the aliased namespaced chiller pin");
		// A connector the template keeps private is refused at the boundary.
		require(!core::resolveExposedPin(fixture.placed,
				{"aircon-unit", "ac1", "Condenser heat"}, resolved, reason)
			&& reason.find("not an exposed port") != std::string::npos,
			("a private internal connector must be refused: " + reason).c_str());
		// A pin that does not name this composite is refused by the
		// single-composite resolver rather than silently resolved.
		require(!core::resolveExposedPin(fixture.placed,
				{"Chiller", "ac1", "Supervision"}, resolved, reason)
			&& reason.find("does not name the composite") != std::string::npos,
			("a pin naming the wrong composite type must be refused: " + reason).c_str());
		// Link resolution: a composite end resolves, an ordinary end
		// passes through unchanged, so the two mix in one candidate link.
		core::CandidateLink mixed{
			{"Outage", "ac-outage", "Target installation"},
			{"aircon-unit", "ac1", "Supervision"}};
		core::CandidateLink resolvedLink;
		require(core::resolvePrefabLink({fixture.placed}, mixed, resolvedLink, reason),
			("the mixed link must resolve: " + reason).c_str());
		require(resolvedLink.from == mixed.from,
			"the ordinary end should pass through unchanged");
		require(resolvedLink.to == core::StationPinRef{"Chiller", "ac1.machine", "Supervision"},
			"the composite end should resolve to the aliased internal pin");
		// A composite end naming an unexposed port refuses the whole link.
		core::CandidateLink privateEnd{
			{"Outage", "ac-outage", "Target installation"},
			{"aircon-unit", "ac1", "Cold loop"}};
		require(!core::resolvePrefabLink({fixture.placed}, privateEnd, resolvedLink, reason)
			&& reason.find("not an exposed port") != std::string::npos,
			("a link to an unexposed port must be refused: " + reason).c_str());
		// A link with no composite end resolves to itself.
		core::CandidateLink ordinary{
			{"Outage", "ac-outage", "Target installation"},
			{"Chiller", "ac1.machine", "Supervision"}};
		require(core::resolvePrefabLink({fixture.placed}, ordinary, resolvedLink, reason)
			&& resolvedLink == ordinary,
			("an ordinary link should resolve to itself: " + reason).c_str());
	}

	void prefabLinkThroughComposite(smoke::Context const&)
	{
		auto const fixture = placeAirconComposite();
		// Allowed direction: the Outage joining the composite's Supervision
		// port returns the same verdict and reason as joining the aliased
		// chiller pin directly.
		core::CandidateLink throughComposite{
			{"Outage", "ac-outage", "Target installation"},
			{"aircon-unit", "ac1", "Supervision"}};
		core::CandidateLink direct{
			{"Outage", "ac-outage", "Target installation"},
			{"Chiller", "ac1.machine", "Supervision"}};
		auto const viaBoundary = core::evaluatePrefabLink(fixture.draft, {fixture.placed}, throughComposite);
		auto const directVerdict = core::evaluateStationLink(fixture.draft, direct);
		require(viaBoundary.allowed,
			("joining the exposed Supervision port must be allowed: " + viaBoundary.reason).c_str());
		require(viaBoundary.allowed == directVerdict.allowed
			&& viaBoundary.reason == directVerdict.reason,
			"the boundary verdict must be exactly the direct verdict");
		// The link through the composite writes the same reference field
		// as the equivalent direct link: two twin drafts, one written
		// through the boundary and one directly, end up identical.
		auto throughDraft = fixture.draft;
		auto directDraft = fixture.draft;
		std::string reason;
		require(core::applyPrefabLink(throughDraft, {fixture.placed}, throughComposite, reason),
			("applying through the boundary must succeed: " + reason).c_str());
		require(core::applyStationLink(directDraft, direct),
			"applying the direct link must succeed");
		require(throughDraft == directDraft,
			"the boundary write must land exactly the direct write");
		require(throughDraft.outages.front().installationId == "ac1.machine",
			"the outage installation field should carry the namespaced chiller id");
		// Refused direction: an illegal pair through the composite returns
		// the same refusal reason as the direct pair - the boundary adds
		// nothing and hides nothing.
		core::CandidateLink mismatchThrough{
			{"Reservoir", "ac1.sink", "Hosted radiators"},
			{"aircon-unit", "ac1", "Supervision"}};
		core::CandidateLink mismatchDirect{
			{"Reservoir", "ac1.sink", "Hosted radiators"},
			{"Chiller", "ac1.machine", "Supervision"}};
		auto const mismatchBoundary = core::evaluatePrefabLink(fixture.draft, {fixture.placed}, mismatchThrough);
		auto const mismatchDirectVerdict = core::evaluateStationLink(fixture.draft, mismatchDirect);
		require(!mismatchBoundary.allowed && !mismatchDirectVerdict.allowed,
			"the mismatched pair must be refused both ways");
		require(mismatchBoundary.reason == mismatchDirectVerdict.reason
			&& !mismatchBoundary.reason.empty(),
			"the boundary refusal must carry exactly the direct refusal reason");
		// A connector the template does not expose is refused by the
		// boundary itself, before the evaluator is ever reached: the
		// reason names the boundary, not the connector rules.
		core::CandidateLink privateThrough{
			{"aircon-unit", "ac1", "Heat exchanger"},
			{"Reservoir", "ac1.sink", "Heat in"}};
		core::CandidateLink privateDirect{
			{"Coolant loop", "ac1.loop", "Heat exchanger"},
			{"Reservoir", "ac1.sink", "Heat in"}};
		auto const privateVerdict = core::evaluatePrefabLink(fixture.draft, {fixture.placed}, privateThrough);
		require(!privateVerdict.allowed
			&& privateVerdict.reason.find("not an exposed port") != std::string::npos,
			("an unexposed connector must be refused by the boundary: " + privateVerdict.reason).c_str());
		auto const privateDirectVerdict = core::evaluateStationLink(fixture.draft, privateDirect);
		require(privateDirectVerdict.reason != privateVerdict.reason,
			"the boundary refusal must be the boundary's own, not the direct pair's verdict");
		// A refused link leaves the previously written field untouched.
		require(!core::applyPrefabLink(throughDraft, {fixture.placed}, mismatchThrough, reason),
			("applying a refused pair must fail: " + reason).c_str());
		require(throughDraft.outages.front().installationId == "ac1.machine",
			"the refused mismatch must not have changed the previously written field");
	}

	// #815: the whole-unit rename. Every namespaced internal id and every
	// reference to it - the internal plumbing and the outside outage that
	// targets the unit through its exposed Supervision port - is rewritten
	// together, and nothing under the old namespace survives.
	void prefabInstanceRenameCascade(smoke::Context const&)
	{
		auto fixture = placeAirconComposite();
		std::string reason;
		core::CandidateLink supervision{
			{"Outage", "ac-outage", "Target installation"},
			{"aircon-unit", "ac1", "Supervision"}};
		require(core::applyPrefabLink(fixture.draft, {fixture.placed}, supervision, reason),
			("wiring the outage to the unit must succeed: " + reason).c_str());
		require(fixture.draft.outages.front().installationId == "ac1.machine",
			"the outage should target the namespaced chiller before the rename");
		core::PrefabInstanceRename plan;
		std::string diagnostic;
		require(core::planPrefabInstanceRename(fixture.draft, "ac1", "ac2", plan, &diagnostic),
			("the whole-unit rename must plan cleanly: " + diagnostic).c_str());
		require(plan.entries.size() == 5,
			("the unit owns five internal entries, saw "
				+ std::to_string(plan.entries.size())).c_str());
		require(!plan.linkIdentities.empty(),
			"the rename plan must name the references it rewrites");
		require(core::renamePrefabInstance(fixture.draft, plan, reason),
			("the whole-unit rename must apply: " + reason).c_str());
		// Every internal entry carries the new namespace and every internal
		// reference points at the renamed peers.
		require(fixture.draft.chillers.size() == 1
			&& fixture.draft.chillers[0].id == "ac2.machine"
			&& fixture.draft.chillers[0].coldLoopId == "ac2.loop"
			&& fixture.draft.chillers[0].condenserReservoirId == "ac2.sink",
			"the chiller was not renamed with its internal references");
		require(fixture.draft.coolantLoops.size() == 1
			&& fixture.draft.coolantLoops[0].id == "ac2.loop"
			&& fixture.draft.coolantLoops[0].reservoirId == "ac2.coldside",
			"the coolant loop was not renamed with its internal references");
		require(fixture.draft.radiators.size() == 1
			&& fixture.draft.radiators[0].id == "ac2.radiator"
			&& fixture.draft.radiators[0].reservoirId == "ac2.sink",
			"the radiator was not renamed with its internal references");
		require(fixture.draft.thermalReservoirs.size() == 2,
			"the unit's reservoirs should still be there after the rename");
		// The outside reference was rewritten too: the outage names the
		// renamed chiller, not the id it was wired to.
		require(fixture.draft.outages.front().installationId == "ac2.machine",
			"the outside reference was not rewritten by the unit rename");
		// Nothing under the old namespace remains, in the entries or in
		// any authored reference.
		for (auto const& [type, id] : core::prefabEntryOrder(fixture.draft))
			require(id.rfind("ac1.", 0) != 0,
				("an entry kept the old namespace: " + type + " '" + id + "'").c_str());
		for (auto const& link : core::stationAuthoredLinks(fixture.draft))
			require(link.from.nodeId.rfind("ac1.", 0) != 0
					&& link.to.nodeId.rfind("ac1.", 0) != 0,
				"an authored reference kept the old namespace");
		// The renamed unit still stands on its own as a valid draft.
		std::string validation;
		require(core::validateStationSystemsSettings(fixture.draft, validation),
			("the renamed expansion must still validate: " + validation).c_str());
	}

	// #815: the rename refusals. A bad target name, an unknown unit, a
	// namespace already in use, and a stale plan are each refused with a
	// reason and leave the draft exactly as it was.
	void prefabInstanceRenameRefusals(smoke::Context const&)
	{
		auto fixture = placeAirconComposite();
		core::PrefabInstanceRename plan;
		std::string diagnostic;
		require(!core::planPrefabInstanceRename(fixture.draft, "ac1", "", plan, &diagnostic)
			&& !diagnostic.empty(), "an empty unit name must be refused with a reason");
		require(!core::planPrefabInstanceRename(fixture.draft, "ac1", "ac1", plan, &diagnostic),
			"renaming a unit to its own name must be refused");
		require(!core::planPrefabInstanceRename(fixture.draft, "ac1", "a.c", plan, &diagnostic)
			&& diagnostic.find("separator") != std::string::npos,
			("the reserved separator must be refused: " + diagnostic).c_str());
		require(!core::planPrefabInstanceRename(fixture.draft, "nosuch", "ac2", plan, &diagnostic),
			"renaming a unit that is not placed must be refused");
		// A target namespace already owned by another placed unit is
		// refused: two units can never be merged into one namespace.
		auto const tmpl = parseAircon();
		core::PrefabExpansion second;
		std::string reason;
		require(core::expandPrefab(tmpl, {}, "ac2", second, reason)
			&& core::applyPrefabExpansion(fixture.draft, second, reason),
			("the second instance must place cleanly: " + reason).c_str());
		require(!core::planPrefabInstanceRename(fixture.draft, "ac1", "ac2", plan, &diagnostic),
			"renaming into a namespace another unit already owns must be refused");
		// A stale plan is refused: the draft gained an entry under the
		// namespace after the plan was made, so the recorded view no
		// longer matches and nothing may be renamed from it.
		core::StationSystemsSettings staleBase = fixture.draft;
		require(core::planPrefabInstanceRename(staleBase, "ac1", "ac9", plan, &diagnostic),
			("the pre-stale plan must succeed: " + diagnostic).c_str());
		core::ResourceStoreSettings extra;
		extra.id = "ac1.extra"; extra.unit = "kg"; extra.capacity = 10;
		staleBase.stores.push_back(extra);
		require(!core::renamePrefabInstance(staleBase, plan, reason) && !reason.empty(),
			("a stale rename plan must be refused with a reason: " + reason).c_str());
		require(staleBase.chillers.front().id == "ac1.machine",
			"a refused rename must leave the draft untouched");
	}

	// #815: the whole-unit cascade delete. One removal takes the whole
	// expansion: every internal entry, the internal plumbing, and the
	// outside reference that targeted the unit - no orphaned internal and
	// no dangling reference is left behind.
	void prefabInstanceCascadeDelete(smoke::Context const&)
	{
		auto fixture = placeAirconComposite();
		std::string reason;
		core::CandidateLink supervision{
			{"Outage", "ac-outage", "Target installation"},
			{"aircon-unit", "ac1", "Supervision"}};
		require(core::applyPrefabLink(fixture.draft, {fixture.placed}, supervision, reason),
			("wiring the outage to the unit must succeed: " + reason).c_str());
		core::PrefabInstanceRemoval plan;
		std::string diagnostic;
		require(core::planPrefabInstanceRemoval(fixture.draft, "ac1", plan, &diagnostic),
			("the cascade delete must plan cleanly: " + diagnostic).c_str());
		require(plan.entries.size() == 5,
			("the cascade owns five internal entries, saw "
				+ std::to_string(plan.entries.size())).c_str());
		require(!plan.linkIdentities.empty(),
			"the cascade plan must name the connections it removes");
		require(core::removePrefabInstance(fixture.draft, plan, reason),
			("the cascade delete must apply: " + reason).c_str());
		require(fixture.draft.chillers.empty() && fixture.draft.coolantLoops.empty()
			&& fixture.draft.radiators.empty() && fixture.draft.thermalReservoirs.empty(),
			"the whole expansion must be gone");
		require(fixture.draft.outages.size() == 1,
			"the outside node must survive its target's removal");
		require(fixture.draft.outages.front().installationId.empty(),
			"the outside reference must be cleared, not left dangling");
		for (auto const& [type, id] : core::prefabEntryOrder(fixture.draft))
			require(id.rfind("ac1.", 0) != 0,
				("an orphaned internal remains: " + type + " '" + id + "'").c_str());
		for (auto const& link : core::stationAuthoredLinks(fixture.draft))
			require(link.from.nodeId.rfind("ac1.", 0) != 0
					&& link.to.nodeId.rfind("ac1.", 0) != 0,
				"a dangling reference to the removed unit remains");
		for (auto const& [key, properties] : fixture.draft.connections)
			require(key.ownerId.rfind("ac1.", 0) != 0 && key.peerId.rfind("ac1.", 0) != 0,
				"a connection property survived the unit it belonged to");
		// A unit that is not there is refused rather than silently empty.
		require(!core::planPrefabInstanceRemoval(fixture.draft, "ac1", plan, &diagnostic)
			&& !diagnostic.empty(), "removing an absent unit must be refused with a reason");
	}

	void prefabTemplateLoadValidation(smoke::Context const&)
	{
		// The bundled template passes the load gate: its isolated
		// expansion validates and every exposed port has a stub peer with
		// room to join.
		auto const tmpl = parseAircon();
		std::string reason;
		require(core::validatePrefabTemplate(tmpl, reason),
			("the bundled template must pass load validation: " + reason).c_str());
		// A template exposing a connector the template itself already
		// wires is refused: the chiller's Cold loop input takes one link
		// and the internal loop holds it, so the exposed port could never
		// be joined from outside.
		std::string const fullYaml = withExtraExposedPort(airconPrefabYaml(),
			"  - name: Cold in\n"
			"    nodeType: Chiller\n"
			"    entryId: machine\n"
			"    portName: Cold loop\n");
		core::PrefabTemplate fullTmpl;
		std::string diagnostic;
		require(!core::parsePrefabTemplate(fullYaml, fullTmpl, &diagnostic)
			&& diagnostic.find("capacity is used inside the box") != std::string::npos,
			("an already-full exposed port must be refused at load: " + diagnostic).c_str());
		// A template whose isolated expansion does not validate is
		// refused atomically with the validator's diagnostic.
		auto broken = parseAircon();
		broken.entries.chillers.front().coldLoopId = "ghost";
		require(!core::validatePrefabTemplate(broken, reason)
			&& reason.find("does not validate") != std::string::npos,
			("a non-validating isolated expansion must be refused: " + reason).c_str());
		// A prefab id carrying the namespacing separator is refused at
		// load: it would poison the isolation instance id.
		std::string dottedYaml = airconPrefabYaml();
		dottedYaml.replace(dottedYaml.find("id: aircon-unit"),
			std::strlen("id: aircon-unit"), "id: air.con.unit");
		core::PrefabTemplate dottedTmpl;
		require(!core::parsePrefabTemplate(dottedYaml, dottedTmpl, &diagnostic)
			&& diagnostic.find("reserved separator") != std::string::npos,
			("a dotted prefab id must be refused: " + diagnostic).c_str());
	}
	// #812: the catalogue document. The same template shape, wrapped in a
	// 'prefabCatalogue' root with a 'prefabs' array, loaded as a whole.
	std::string catalogueYaml(std::string const& items)
	{
		return "schemaVersion: 111\nprefabCatalogue:\n  prefabs:\n" + items;
	}

	// Re-indent a standalone template document into one `- ` item of a
	// catalogue's `prefabs` array. `dash` is the column of the dash, so the
	// item's own fields land two columns further in.
	std::string catalogueItem(std::string const& yaml, int dash)
	{
		std::string out;
		std::istringstream stream(yaml);
		std::string line;
		bool first = true;
		while (std::getline(stream, line))
		{
			if (line.empty()) continue;
			out += std::string(first ? dash : dash + 2, ' ');
			if (first)
			{
				out += "- ";
				first = false;
			}
			out += line + "\n";
		}
		return out;
	}

	std::string renamedTemplate(std::string const& yaml, char const* newId)
	{
		std::string out = yaml;
		auto const pos = out.find("id: aircon-unit");
		require(pos != std::string::npos, "template should carry its own id line");
		out.replace(pos, std::strlen("id: aircon-unit"), std::string("id: ") + newId);
		return out;
	}

	void prefabCatalogueParse(smoke::Context const& ctx)
	{
		std::string diagnostic;
		// The shipped catalogue is the real artifact the editor loads, so it
		// is the one asserted here rather than a test-local copy.
		std::ifstream in(ctx.fixture("resources/prefabs/aircon-unit.yaml"), std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		core::PrefabCatalogue catalogue;
		require(core::parsePrefabCatalogue(text.str(), catalogue, &diagnostic),
			("bundled prefab catalogue must parse: " + diagnostic).c_str());
		require(catalogue.templates.size() == 1,
			"bundled catalogue should ship one template");
		require(catalogue.find("nope") == nullptr, "an unknown id must not resolve");
		auto const* tmpl = catalogue.find("aircon-unit");
		require(tmpl != nullptr, "bundled catalogue should resolve aircon-unit");
		if (!tmpl) return;
		require(tmpl->displayName == "Aircon Unit",
			"bundled template should keep its display name");
		// The shipped prefix gives the spec's `ac1.machine` instance naming.
		require(tmpl->instancePrefix == "ac",
			"bundled template should declare the 'ac' instance prefix");
		require(tmpl->entries.chillers.size() == 1
				&& tmpl->entries.coolantLoops.size() == 1
				&& tmpl->entries.thermalReservoirs.size() == 2
				&& tmpl->entries.radiators.size() == 1,
			"bundled template should carry its five internal entries");
		require(tmpl->exposedPorts.size() == 2 && tmpl->parameters.size() == 3,
			"bundled template should keep its two exposed ports and three dials");
		// A template parsed from a catalogue behaves exactly like the same
		// template parsed standalone: same expansion, same ids.
		core::PrefabExpansion fromCatalogue, fromStandalone;
		std::string reason;
		require(core::expandPrefab(*tmpl, {}, "ac9", fromCatalogue, reason)
				&& core::expandPrefab(parseAircon(), {}, "ac9", fromStandalone, reason)
				&& fromCatalogue.entries == fromStandalone.entries,
			("a catalogue template must expand like the standalone template: " + reason).c_str());
		// Two good templates in one catalogue both load, in file order.
		auto const pair = catalogueItem(airconPrefabYaml(), 4)
			+ catalogueItem(renamedTemplate(airconPrefabYaml(), "heater-unit"), 4);
		core::PrefabCatalogue two;
		require(core::parsePrefabCatalogue(catalogueYaml(pair), two, &diagnostic)
				&& two.templates.size() == 2
				&& two.find("aircon-unit") != nullptr
				&& two.find("heater-unit") != nullptr,
			("two good templates should both load: " + diagnostic).c_str());
	}

	void prefabCatalogueRefusals(smoke::Context const&)
	{
		std::string diagnostic;
		core::PrefabCatalogue catalogue;
		// A document with no 'prefabs' array is not a catalogue.
		require(!core::parsePrefabCatalogue("schemaVersion: 111\nprefabCatalogue:\n  other: 1\n",
				catalogue, &diagnostic)
			&& diagnostic.find("prefabs") != std::string::npos,
			("a catalogue with no prefabs array must be refused: " + diagnostic).c_str());
		// One malformed template among good ones refuses the WHOLE catalogue:
		// the caller must never see the good templates alongside the bad one,
		// which is what keeps a half-loaded unit out of the Place menu.
		auto const good = catalogueItem(airconPrefabYaml(), 4);
		auto const mixed = good
			+ catalogueItem(renamedTemplate(airconPrefabYaml(), "broken.unit"), 4);
		core::PrefabCatalogue refused;
		require(!core::parsePrefabCatalogue(catalogueYaml(mixed), refused, &diagnostic),
			("a catalogue holding a malformed template must be refused: " + diagnostic).c_str());
		require(refused.templates.empty(),
			"a refused catalogue must leave no templates behind, not even the good ones");
		require(diagnostic.find("broken.unit") != std::string::npos,
			("the diagnostic should name the template that broke: " + diagnostic).c_str());
		// Duplicate template ids across one catalogue are refused the same way.
		core::PrefabCatalogue dupes;
		require(!core::parsePrefabCatalogue(catalogueYaml(good + good), dupes, &diagnostic)
			&& diagnostic.find("duplicate template id") != std::string::npos,
			("duplicate ids must refuse the catalogue: " + diagnostic).c_str());
		require(dupes.templates.empty(), "a refused catalogue must be empty");
	}

	void prefabInstanceAllocation(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		// The test template declares no prefix, so the prefab id is the prefix.
		require(tmpl.instancePrefix == "aircon-unit",
			"an absent instance prefix should default to the prefab id");
		core::StationSystemsSettings draft;
		draft.enabled = true;
		require(core::allocatePrefabInstanceId(tmpl, draft) == "aircon-unit1",
			"an empty draft should yield the first instance");
		// Take the first instance's namespace with one ordinary entry of a
		// type the template does not even use: the allocator claims the whole
		// `<instance>.` namespace, not just the template's own types.
		core::InstallationOutageSettings outage;
		outage.id = "aircon-unit1.machine";
		outage.firstOccurrenceSeconds = 0.0;
		outage.durationSeconds = 1.0;
		draft.outages.push_back(outage);
		require(core::allocatePrefabInstanceId(tmpl, draft) == "aircon-unit2",
			"a taken namespace must be skipped");
		// Both namespaces taken: the allocator must step past both.
		core::InstallationOutageSettings second;
		second.id = "aircon-unit2.machine";
		second.firstOccurrenceSeconds = 0.0;
		second.durationSeconds = 1.0;
		draft.outages.push_back(second);
		require(core::allocatePrefabInstanceId(tmpl, draft) == "aircon-unit3",
			"consecutive taken namespaces must both be skipped");
		// A prefix carrying the namespacing separator would break the
		// namespace rule, so it is refused at parse time.
		std::string badYaml = airconPrefabYaml();
		badYaml.insert(badYaml.find("displayName:"), "instancePrefix: a.c\n");
		core::PrefabTemplate bad;
		std::string diagnostic;
		require(!core::parsePrefabTemplate(badYaml, bad, &diagnostic)
			&& diagnostic.find("reserved separator") != std::string::npos,
			("a dotted instance prefix must be refused: " + diagnostic).c_str());
	}

	void prefabEntryOrderIsFixed(smoke::Context const&)
	{
		auto const tmpl = parseAircon();
		auto const order = core::prefabEntryOrder(tmpl.entries);
		require(order.size() == 5, "the bundled template has five internal entries");
		// The order is the core's fixed type order, not authoring order: the
		// placement grid depends on it being reproducible.
		require((order[0] == std::pair<std::string, std::string>{"Reservoir", "coldside"})
			&& (order[1] == std::pair<std::string, std::string>{"Reservoir", "sink"})
			&& (order[2] == std::pair<std::string, std::string>{"Radiator", "radiator"})
			&& (order[3] == std::pair<std::string, std::string>{"Coolant loop", "loop"})
			&& (order[4] == std::pair<std::string, std::string>{"Chiller", "machine"}),
			"entry order should follow the fixed type order");
		// The dial defaults come back as overrides, kinds matching declarations.
		auto const overrides = core::defaultPrefabOverrides(tmpl);
		require(overrides.size() == 3, "every declared dial should yield a default");
		require(std::holds_alternative<double>(overrides.at("setpoint"))
				&& std::get<double>(overrides.at("setpoint")) == 5.0,
			"the numeric dial default should land as a double");
		require(std::holds_alternative<bool>(overrides.at("criticalService"))
				&& !std::get<bool>(overrides.at("criticalService")),
			"the boolean dial default should land as a bool");
		// Applying the defaults is a legal expansion (defaults are in range).
		core::PrefabExpansion expansion;
		std::string reason;
		require(core::expandPrefab(tmpl, overrides, "ac1", expansion, reason),
			("default overrides must be accepted: " + reason).c_str());
	}

	// #814: the provenance block in the station configuration document.
	// A placed unit's {prefab id, instance id, overrides} round-trips
	// losslessly, the flattened expansion validates on its own without
	// any catalogue, and the schema boundary refuses old readers rather
	// than letting them silently drop the block.
	std::string placedUnitYaml()
	{
		auto const tmpl = parseAircon();
		std::string reason;
		// Non-default overrides of both kinds: the values that travel must
		// be exactly the ones the author placed with.
		core::PrefabOverrides overrides{
			{"setpoint", core::PrefabOverrideValue{8.5}},
			{"capacity", core::PrefabOverrideValue{12.0}},
			{"criticalService", core::PrefabOverrideValue{true}}};
		core::PrefabExpansion expansion;
		require(core::expandPrefab(tmpl, overrides, "ac1", expansion, reason), reason.c_str());
		core::StationConfigurationDocument document;
		document.settings = expansion.entries;
		document.prefabs.push_back({"aircon-unit", "ac1", overrides});
		return core::serializeStationConfiguration(document);
	}

	void replaceAllText(std::string& text, std::string const& from, std::string const& to)
	{
		for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
			text.replace(at, from.size(), to);
	}

	// Drop the whole `key:` block (the key line at column 0 and every
	// indented line after it) from the serialized document.
	void replaceBlock(std::string& yaml, std::string const& key)
	{
		auto const at = yaml.find(key + ":");
		require(at != std::string::npos && (at == 0 || yaml[at - 1] == '\n'),
			("document should carry a " + key + " block at the root").c_str());
		auto end = yaml.find('\n', at);
		while (end != std::string::npos)
		{
			auto const next = yaml.find('\n', end + 1);
			auto const chunk = yaml.substr(end + 1, next == std::string::npos
				? std::string::npos : next - end - 1);
			if (!chunk.empty() && chunk[0] == ' ')
			{
				end = next;
				continue;
			}
			break;
		}
		yaml.erase(at, (end == std::string::npos ? yaml.size() : end + 1) - at);
	}

	std::string replaceOnce(std::string const& text, std::string const& from, std::string const& to)
	{
		auto out = text;
		auto const at = out.find(from);
		require(at != std::string::npos, ("fixture text missing: " + from).c_str());
		out.replace(at, from.size(), to);
		return out;
	}

	void prefabProvenanceRoundTrip(smoke::Context const&)
	{
		auto const yaml = placedUnitYaml();
		require(yaml.find("schemaVersion: 111") != std::string::npos
				&& yaml.find("prefabId: aircon-unit") != std::string::npos
				&& yaml.find("instanceId: ac1") != std::string::npos
				&& yaml.find("kind: number") != std::string::npos
				&& yaml.find("kind: boolean") != std::string::npos,
			"the provenance block did not reach the YAML");
		core::StationConfigurationDocument loaded;
		std::string diagnostic;
		require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
		core::StationConfigurationDocument original;
		require(core::parseStationConfiguration(yaml, original, &diagnostic), diagnostic.c_str());
		require(loaded.settings == original.settings, "the expansion changed on round trip");
		require(loaded.prefabs == original.prefabs, "the provenance block did not round trip");
		require(loaded.prefabs.size() == 1
				&& loaded.prefabs[0].prefabId == "aircon-unit"
				&& loaded.prefabs[0].instanceId == "ac1"
				&& loaded.prefabs[0].overrides.size() == 3
				&& std::get<double>(loaded.prefabs[0].overrides.at("setpoint")) == 8.5
				&& std::get<bool>(loaded.prefabs[0].overrides.at("criticalService")),
			"the provenance values did not survive the round trip intact");
		// Re-serializing what the reader produced writes the same bytes: the
		// reader lands exactly the shape the writer wrote, so save/re-save
		// never drifts.
		require(core::serializeStationConfiguration(loaded) == yaml,
			"the provenance block did not re-serialize identically");
		// The composite boundary record rebuilds from provenance with the
		// same exposed-pin aliases the original expansion produced.
		core::PlacedPrefab placed;
		std::string reason;
		require(core::reconstructPlacedPrefab(parseAircon(), loaded.prefabs[0], placed, reason),
			reason.c_str());
		core::PrefabExpansion expansion;
		require(core::expandPrefab(parseAircon(), loaded.prefabs[0].overrides, "ac1",
				expansion, reason), reason.c_str());
		require(placed.prefabId == "aircon-unit" && placed.instanceId == "ac1"
				&& placed.overrides == loaded.prefabs[0].overrides
				&& placed.exposedPins == expansion.exposedPins,
			"reconstruction from provenance drifted from the original expansion");
	}

	void prefabProvenanceBoundary(smoke::Context const&)
	{
		auto const yaml = placedUnitYaml();
		core::StationConfigurationDocument loaded;
		std::string diagnostic;
		// Old-reader discipline: a document stamped before schema 111 that
		// carries the provenance block is refused as conversion-required
		// rather than silently dropping the provenance the author placed.
		auto stale = yaml;
		replaceAllText(stale, "schemaVersion: 111", "schemaVersion: 110");
		require(!core::parseStationConfiguration(stale, loaded, &diagnostic)
				&& diagnostic.find("Prefab provenance requires World schema 111")
					!= std::string::npos
				&& diagnostic.find("conversion required") != std::string::npos,
			("a pre-111 document carrying provenance was not refused as conversion-required: "
				+ diagnostic).c_str());
		require(loaded.prefabs.empty() && loaded.settings == core::StationSystemsSettings{},
			"a refused document touched the caller's document");
		// A version beyond what this reader knows is refused outright at the
		// version boundary, the same refusal every newer-schema document
		// meets.
		auto future = yaml;
		replaceAllText(future, "schemaVersion: 111", "schemaVersion: 112");
		require(!core::parseStationConfiguration(future, loaded, &diagnostic)
				&& diagnostic.find("Unsupported Station configuration schemaVersion")
					!= std::string::npos,
			("a future-schema document was not refused at the version boundary: "
				+ diagnostic).c_str());
		// A pre-111 document WITHOUT the block still loads: the bump gates
		// the new fields, not the old format.
		auto legacy = yaml;
		replaceAllText(legacy, "schemaVersion: 111", "schemaVersion: 110");
		replaceBlock(legacy, "prefabs");
		require(core::parseStationConfiguration(legacy, loaded, &diagnostic)
				&& loaded.prefabs.empty(),
			("a pre-111 document without provenance must still load: " + diagnostic).c_str());
	}

	void prefabProvenanceMalformed(smoke::Context const&)
	{
		auto const yaml = placedUnitYaml();
		core::StationConfigurationDocument loaded;
		std::string diagnostic;
		auto refuseWith = [&](std::string const& bad, std::string const& expect, char const* what)
		{
			require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
					&& diagnostic.find(expect) != std::string::npos,
				(std::string(what) + ": " + diagnostic).c_str());
			require(loaded.prefabs.empty() && loaded.settings == core::StationSystemsSettings{},
				"a refused provenance document touched the caller's document");
		};
		refuseWith(replaceOnce(yaml, "instanceId: ac1", "instanceId: \"\""),
			"prefab provenance", "an empty instance id must be refused");
		refuseWith(replaceOnce(yaml, "prefabId: aircon-unit", "prefabId: \"\""),
			"prefab provenance", "an empty prefab id must be refused");
		refuseWith(replaceOnce(yaml, "kind: number", "kind: string"),
			"Unknown prefab override kind", "an unknown override kind must be refused");
		// Two overrides naming the same dial: the second is a duplicate the
		// reader cannot resolve to one value.
		refuseWith(replaceOnce(yaml, "name: capacity", "name: setpoint"),
			"duplicate prefab override", "a duplicated override name must be refused");
	}

	void prefabProvenanceCatalogueAbsent(smoke::Context const&)
	{
		// The document path never consults a prefab catalogue: the flattened
		// expansion carries every entry and reference the station needs, so
		// a saved document loads with the catalogue absent and the
		// settings stand on their own.
		auto const yaml = placedUnitYaml();
		core::StationConfigurationDocument loaded;
		std::string diagnostic;
		require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
		require(loaded.settings.chillers.size() == 1 && loaded.settings.coolantLoops.size() == 1
				&& loaded.settings.radiators.size() == 1
				&& loaded.settings.thermalReservoirs.size() == 2,
			"the flattened expansion did not load whole");
		// The loaded settings validate as a standalone configuration with
		// no template, no catalogue and no instance machinery in sight.
		std::string validationError;
		require(core::validateStationSystemsSettings(loaded.settings, validationError),
			("the expansion must validate on its own: " + validationError).c_str());
		// The overrides landed inside the expansion: the document's own
		// entries carry the tuned values, not the template defaults.
		require(loaded.settings.chillers[0].chilledSetpointC == 8.5
				&& loaded.settings.chillers[0].ratedCoolingCapacityKW == 12.0
				&& loaded.settings.chillers[0].criticalService,
			"the overrides did not land in the self-sufficient expansion");
	}
}

void registerStationPrefabChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"prefabCatalogueParse", prefabCatalogueParse});
	checks.push_back({"prefabCatalogueRefusals", prefabCatalogueRefusals});
	checks.push_back({"prefabInstanceAllocation", prefabInstanceAllocation});
	checks.push_back({"prefabEntryOrderIsFixed", prefabEntryOrderIsFixed});
	checks.push_back({"prefabTemplateParse", prefabTemplateParse});
	checks.push_back({"prefabTemplateRefusals", prefabTemplateRefusals});
	checks.push_back({"prefabExpansion", prefabExpansion});
	checks.push_back({"prefabOverrides", prefabOverrides});
	checks.push_back({"prefabDialEdit", prefabDialEdit});
	checks.push_back({"prefabCapRefusal", prefabCapRefusal});
	checks.push_back({"prefabDeterminism", prefabDeterminism});
	checks.push_back({"prefabExposedPinResolution", prefabExposedPinResolution});
	checks.push_back({"prefabLinkThroughComposite", prefabLinkThroughComposite});
	checks.push_back({"prefabTemplateLoadValidation", prefabTemplateLoadValidation});
	checks.push_back({"prefabInstanceRenameCascade", prefabInstanceRenameCascade});
	checks.push_back({"prefabInstanceRenameRefusals", prefabInstanceRenameRefusals});
	checks.push_back({"prefabInstanceCascadeDelete", prefabInstanceCascadeDelete});
	checks.push_back({"prefabProvenanceRoundTrip", prefabProvenanceRoundTrip});
	checks.push_back({"prefabProvenanceBoundary", prefabProvenanceBoundary});
	checks.push_back({"prefabProvenanceMalformed", prefabProvenanceMalformed});
	checks.push_back({"prefabProvenanceCatalogueAbsent", prefabProvenanceCatalogueAbsent});
}
