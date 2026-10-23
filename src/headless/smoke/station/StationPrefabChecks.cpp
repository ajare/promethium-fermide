// #810: prefab expansion headless checks.
//
// The bundled aircon-unit template - a chiller, its coolant loop, the
// cold-side reservoir the loop serves, the condenser sink reservoir and the
// radiator that terminates the condenser heat inside the box - is the
// reference fixture. The checks cover template parsing and refusals,
// namespaced expansion with internal links written, override application
// and refusals, per-type cap refusal that leaves the draft untouched, and
// determinism.
#include "StationPrefabChecks.h"
#include "StationRig.h"

#include "core/StationPrefab.h"

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
}

void registerStationPrefabChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"prefabTemplateParse", prefabTemplateParse});
	checks.push_back({"prefabTemplateRefusals", prefabTemplateRefusals});
	checks.push_back({"prefabExpansion", prefabExpansion});
	checks.push_back({"prefabOverrides", prefabOverrides});
	checks.push_back({"prefabCapRefusal", prefabCapRefusal});
	checks.push_back({"prefabDeterminism", prefabDeterminism});
}
