// #810: the prefab expander core.
//
// A pure, headless module: it parses a prefab template whose internal entries
// are written in the existing stationSystems entry vocabulary (read through
// the authoritative reader so the existing format and validation rules apply
// verbatim), and expands a template for one instance into ordinary draft
// entries with namespaced `<instance>.<templateEntryId>` ids, all internal
// links written, exposed-port aliases resolved to namespaced pins, and
// instance overrides applied to their mapped internal fields. Nothing here
// touches the editor; the simulation and the authoritative reader consume
// the flattened result exactly like hand-authored entries.
#include "core/StationPrefab.h"

#include "core/StationReference.h"
#include "core/YamlSerializer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace core
{
	namespace
	{
	// Visit every authored entry of a settings fragment as (nodeType, id) in a
	// fixed type order, so iteration - and therefore expansion - is
	// deterministic regardless of container internals.
	template<class F>
	void forEachPrefabEntry(StationSystemsSettings const& settings, F fn)
	{
		auto const visit = [&](std::string_view type, auto const& entries)
		{
			for (auto const& entry : entries) fn(type, entry.id);
		};
		visit("Store", settings.stores);
		visit("Transfer", settings.transfers);
		visit("Conversion", settings.conversions);
		visit("Solar", settings.solarArrays);
		visit("Equipment", settings.equipmentLoads);
		visit("Battery", settings.batteries);
		visit("Delivery", settings.deliveries);
		visit("Waste ejection", settings.wasteEjections);
		visit("Outage", settings.outages);
		visit("Reservoir", settings.thermalReservoirs);
		visit("Radiator", settings.radiators);
		visit("Coolant loop", settings.coolantLoops);
		visit("Chiller", settings.chillers);
		visit("Heat engine", settings.heatEngines);
		visit("Generator", settings.generators);
		visit("Atmosphere", settings.atmospheres);
		visit("Atmosphere pathway", settings.atmospherePathways);
		visit("Thermal link", settings.thermalLinks);
		visit("Airlock cycle", settings.airlockCycles);
		visit("Crew", settings.crews);
		visit("Water collector", settings.waterCollectors);
		visit("Water treatment", settings.waterTreatments);
		visit("Electrolyzer", settings.electrolyzers);
		visit("CO2 capture", settings.co2Captures);
		visit("Fuel cell", settings.fuelCells);
		visit("Sabatier", settings.sabatierReactors);
		visit("Methane processor", settings.methaneProcessors);
		visit("Microbial processor", settings.microbialProcessors);
		visit("Thermal buffer", settings.thermalBuffers);
		visit("Crops", settings.cropPopulations);
		visit("Algae", settings.algaeCultures);
		visit("Maintenance", settings.maintenanceJobs);
	}

	bool prefabEntryExists(StationSystemsSettings const& settings,
		std::string_view nodeType, std::string_view entryId)
	{
		bool found = false;
		forEachPrefabEntry(settings, [&](std::string_view type, std::string const& id)
		{
			if (type == nodeType && id == entryId) found = true;
		});
		return found;
	}

	bool prefabPortExists(std::string_view nodeType, std::string_view portName)
	{
		return std::any_of(stationPortDescriptors().begin(), stationPortDescriptors().end(),
			[&](PortDescriptor const& descriptor)
			{
				return descriptor.nodeType == nodeType && descriptor.name == portName;
			});
	}

	template<class T>
	T* findPrefabEntryById(std::vector<T>& entries, std::string_view entryId)
	{
		for (auto& entry : entries)
			if (entry.id == entryId) return &entry;
		return nullptr;
	}

	// The dial-addressable fields of the bundled machine types: the exposed
	// parameters of a template may map onto these and nothing else, so the
	// dial set stays an explicit, checked interface rather than a free-form
	// poke at internal settings.
	struct PrefabFieldTarget
	{
		PrefabParameterKind kind{PrefabParameterKind::Number};
		double* number{nullptr};
		bool* flag{nullptr};
		bool found() const { return number != nullptr || flag != nullptr; }
	};

	PrefabFieldTarget resolvePrefabField(StationSystemsSettings& settings,
		std::string_view nodeType, std::string_view entryId, std::string_view field)
	{
		if (nodeType == "Chiller")
		{
			auto* entry = findPrefabEntryById(settings.chillers, entryId);
			if (!entry) return {};
			if (field == "chilledSetpointC") return {PrefabParameterKind::Number, &entry->chilledSetpointC, nullptr};
			if (field == "ratedCoolingCapacityKW") return {PrefabParameterKind::Number, &entry->ratedCoolingCapacityKW, nullptr};
			if (field == "cop") return {PrefabParameterKind::Number, &entry->cop, nullptr};
			if (field == "auxiliaryPowerKW") return {PrefabParameterKind::Number, &entry->auxiliaryPowerKW, nullptr};
			if (field == "maxCondensingTemperatureC") return {PrefabParameterKind::Number, &entry->maxCondensingTemperatureC, nullptr};
			if (field == "criticalService") return {PrefabParameterKind::Boolean, nullptr, &entry->criticalService};
			return {};
		}
		if (nodeType == "Coolant loop")
		{
			auto* entry = findPrefabEntryById(settings.coolantLoops, entryId);
			if (!entry) return {};
			if (field == "freezeProtectionSetpointC") return {PrefabParameterKind::Number, &entry->freezeProtectionSetpointC, nullptr};
			if (field == "pumpPowerKW") return {PrefabParameterKind::Number, &entry->pumpPowerKW, nullptr};
			if (field == "ratedFlowKgPerS") return {PrefabParameterKind::Number, &entry->ratedFlowKgPerS, nullptr};
			if (field == "exchangerConductanceKWPerK") return {PrefabParameterKind::Number, &entry->exchangerConductanceKWPerK, nullptr};
			if (field == "heaterCapacityKW") return {PrefabParameterKind::Number, &entry->heaterCapacityKW, nullptr};
			if (field == "startingCoolantKg") return {PrefabParameterKind::Number, &entry->startingCoolantKg, nullptr};
			if (field == "criticalService") return {PrefabParameterKind::Boolean, nullptr, &entry->criticalService};
			return {};
		}
		if (nodeType == "Reservoir")
		{
			auto* entry = findPrefabEntryById(settings.thermalReservoirs, entryId);
			if (!entry) return {};
			if (field == "heatCapacityKJPerK") return {PrefabParameterKind::Number, &entry->heatCapacityKJPerK, nullptr};
			if (field == "startingTemperatureC") return {PrefabParameterKind::Number, &entry->startingTemperatureC, nullptr};
			if (field == "absorbedHeatKW") return {PrefabParameterKind::Number, &entry->absorbedHeatKW, nullptr};
			return {};
		}
		if (nodeType == "Radiator")
		{
			auto* entry = findPrefabEntryById(settings.radiators, entryId);
			if (!entry) return {};
			if (field == "areaM2") return {PrefabParameterKind::Number, &entry->areaM2, nullptr};
			if (field == "emissivity") return {PrefabParameterKind::Number, &entry->emissivity, nullptr};
			if (field == "viewFactor") return {PrefabParameterKind::Number, &entry->viewFactor, nullptr};
			if (field == "sinkTemperatureC") return {PrefabParameterKind::Number, &entry->sinkTemperatureC, nullptr};
			return {};
		}
		return {};
	}

	PrefabParameter const* findPrefabParameter(PrefabTemplate const& tmpl, std::string const& name)
	{
		auto const found = std::find_if(tmpl.parameters.begin(), tmpl.parameters.end(),
			[&](PrefabParameter const& parameter) { return parameter.name == name; });
		return found == tmpl.parameters.end() ? nullptr : &*found;
	}

	// The per-type ceilings the validator enforces, as (type, cap) pairs.
	// A fragment that consumes more slots than the draft leaves free must be
	// refused before anything lands, so a half-expanded unit can never
	// appear in the draft.
	template<class F>
	void forEachPrefabCap(F fn)
	{
		fn("Store", MaxStationResourceStores);
		fn("Transfer", MaxStationResourceTransfers);
		fn("Conversion", MaxStationResourceConversions);
		fn("Solar", MaxStationSolarArrays);
		fn("Equipment", MaxStationEquipmentLoads);
		fn("Battery", MaxStationBatteries);
		fn("Delivery", MaxStationResourceDeliveries);
		fn("Waste ejection", MaxStationWasteEjections);
		fn("Outage", MaxStationInstallationOutages);
		fn("Maintenance", MaxStationMaintenanceJobs);
		fn("Reservoir", MaxStationThermalReservoirs);
		fn("Radiator", MaxStationRadiators);
		fn("Coolant loop", MaxStationCoolantLoops);
		fn("Chiller", MaxStationChillers);
		fn("Heat engine", MaxStationHeatEngines);
		fn("Generator", MaxStationGenerators);
	}

	size_t countFragmentType(StationSystemsSettings const& fragment, std::string_view type)
	{
		size_t count = 0;
		forEachPrefabEntry(fragment, [&](std::string_view entryType, std::string const&)
		{
			if (entryType == type) ++count;
		});
		return count;
	}

	// Count every slot the fragment consumes against the draft's current
	// counts and the per-type ceilings; refuse the whole thing on any
	// shortfall.
	bool checkFragmentCaps(std::string const& subject,
		StationSystemsSettings const& fragment,
		StationSystemsSettings const& draft, std::string& reason)
	{
		bool refused = false;
		forEachPrefabCap([&](std::string_view type, size_t cap)
		{
			if (refused) return;
			auto const need = countFragmentType(fragment, type);
			if (need == 0) return;
			auto const held = countFragmentType(draft, type);
			if (held + need > cap)
			{
				reason = subject + " needs " + std::to_string(need) + " " + std::string(type)
					+ " slot(s) but the draft already holds " + std::to_string(held)
					+ " of a " + std::to_string(cap) + "-entry cap";
				refused = true;
			}
		});
		return !refused;
	}

	// Append one fragment's entries onto the matching draft vectors. Called
	// only on a candidate copy after every pre-check has passed.
	void appendFragment(StationSystemsSettings& draft, StationSystemsSettings const& fragment)
	{
		auto const append = [&](auto& target, auto const& source)
		{
			target.insert(target.end(), source.begin(), source.end());
		};
		append(draft.stores, fragment.stores);
		append(draft.transfers, fragment.transfers);
		append(draft.conversions, fragment.conversions);
		append(draft.solarArrays, fragment.solarArrays);
		append(draft.equipmentLoads, fragment.equipmentLoads);
		append(draft.batteries, fragment.batteries);
		append(draft.deliveries, fragment.deliveries);
		append(draft.wasteEjections, fragment.wasteEjections);
		append(draft.outages, fragment.outages);
		append(draft.thermalReservoirs, fragment.thermalReservoirs);
		append(draft.radiators, fragment.radiators);
		append(draft.coolantLoops, fragment.coolantLoops);
		append(draft.chillers, fragment.chillers);
		append(draft.heatEngines, fragment.heatEngines);
		append(draft.generators, fragment.generators);
		append(draft.atmospheres, fragment.atmospheres);
		append(draft.atmospherePathways, fragment.atmospherePathways);
		append(draft.thermalLinks, fragment.thermalLinks);
		append(draft.airlockCycles, fragment.airlockCycles);
		append(draft.crews, fragment.crews);
		append(draft.waterCollectors, fragment.waterCollectors);
		append(draft.waterTreatments, fragment.waterTreatments);
		append(draft.electrolyzers, fragment.electrolyzers);
		append(draft.co2Captures, fragment.co2Captures);
		append(draft.fuelCells, fragment.fuelCells);
		append(draft.sabatierReactors, fragment.sabatierReactors);
		append(draft.methaneProcessors, fragment.methaneProcessors);
		append(draft.microbialProcessors, fragment.microbialProcessors);
		append(draft.thermalBuffers, fragment.thermalBuffers);
		append(draft.cropPopulations, fragment.cropPopulations);
		append(draft.algaeCultures, fragment.algaeCultures);
		append(draft.maintenanceJobs, fragment.maintenanceJobs);
		// Connection properties keyed by the namespaced ids travel with the
		// expansion: an internal link's authored throughput cap or physical
		// type is part of what the template contributes to the draft.
		for (auto const& [key, properties] : fragment.connections)
			draft.connections.emplace(key, properties);
	}
	}

	bool parsePrefabTemplate(std::string const& yaml, PrefabTemplate& out,
		std::string* diagnostic)
	{
		auto const refuse = [&](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		try
		{
			auto serializer = YamlSerializer::fromString(yaml);
			serializer->deserialize();
			// The document root is the prefab map, opened descriptively like
			// the World and reference-station readers open their roots; the
			// internal entries live in the root's 'stationSystems' child.
			if (!serializer->hasField("stationSystems"))
				return refuse("Prefab document has no internal 'stationSystems' entries");
			serializer->beginMap("prefab");
			PrefabTemplate tmpl;
			tmpl.id = serializer->readString("id");
			if (tmpl.id.empty()) return refuse("Prefab id must not be empty");
			tmpl.displayName = serializer->readString("displayName", true);
			// The template's internal entries are read under a World schema
			// version like any stationSystems map; the default is the newest
			// schema this reader knows.
			std::uint32_t version = 110;
			if (serializer->hasField("schemaVersion"))
			{
				version = static_cast<std::uint32_t>(serializer->readInt64("schemaVersion"));
				if (version < 65 || version > 110)
					return refuse("Prefab '" + tmpl.id + "' schema version must be from 65 to 110");
			}
			// Internal entries: the existing stationSystems entry vocabulary,
			// read through the authoritative reader. Full configuration
			// validation of the internal entries runs last, after the
			// template-level checks, so each refusal carries its own most
			// specific reason; a broken template is still refused atomically
			// here rather than at placement.
			readStationSystemsSettingsMap(*serializer, version, tmpl.entries, false);
			// Namespacing rule: a template entry id carrying the separator
			// would make a namespaced id ambiguous, so it is refused.
			std::string separatorOffender;
			forEachPrefabEntry(tmpl.entries, [&](std::string_view, std::string const& id)
			{
				if (separatorOffender.empty() && id.find(PrefabIdSeparator) != std::string::npos)
					separatorOffender = id;
			});
			if (!separatorOffender.empty())
				return refuse("Prefab '" + tmpl.id + "' entry id '" + separatorOffender
					+ "' contains the reserved separator '" + PrefabIdSeparator + "'");
			if (serializer->hasField("exposedPorts"))
			{
				serializer->beginArray("exposedPorts");
				while (serializer->nextArrayItem())
				{
					serializer->beginMap("");
					PrefabExposedPort port;
					port.name = serializer->readString("name");
					port.nodeType = serializer->readString("nodeType");
					port.entryId = serializer->readString("entryId");
					port.portName = serializer->readString("portName");
					serializer->endMap();
					if (port.name.empty())
						return refuse("Prefab '" + tmpl.id + "' exposed port name must not be empty");
					if (std::any_of(tmpl.exposedPorts.begin(), tmpl.exposedPorts.end(),
						[&](PrefabExposedPort const& existing) { return existing.name == port.name; }))
						return refuse("Prefab '" + tmpl.id + "' exposed port '" + port.name + "' is duplicated");
					if (!prefabEntryExists(tmpl.entries, port.nodeType, port.entryId))
						return refuse("Prefab '" + tmpl.id + "' exposed port '" + port.name
							+ "' aliases unknown entry '" + port.entryId + "' of type '" + port.nodeType + "'");
					if (!prefabPortExists(port.nodeType, port.portName))
						return refuse("Prefab '" + tmpl.id + "' exposed port '" + port.name
							+ "' aliases unknown port '" + port.portName + "' on '" + port.nodeType + "'");
					tmpl.exposedPorts.push_back(std::move(port));
				}
				serializer->endArray();
			}
			if (serializer->hasField("parameters"))
			{
				serializer->beginArray("parameters");
				while (serializer->nextArrayItem())
				{
					serializer->beginMap("");
					PrefabParameter parameter;
					parameter.name = serializer->readString("name");
					parameter.nodeType = serializer->readString("nodeType");
					parameter.entryId = serializer->readString("entryId");
					parameter.field = serializer->readString("field");
					auto const kind = serializer->readString("kind", true, "number");
					if (kind == "boolean") parameter.kind = PrefabParameterKind::Boolean;
					else if (kind == "number") parameter.kind = PrefabParameterKind::Number;
					else
					{
						serializer->endMap();
						return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
							+ "' has unknown kind '" + kind + "' (expected 'number' or 'boolean')");
					}
					if (parameter.kind == PrefabParameterKind::Number)
					{
						parameter.defaultValue = serializer->readDouble("default");
						parameter.minValue = serializer->readDouble("min");
						parameter.maxValue = serializer->readDouble("max");
					}
					else
						parameter.boolDefault = serializer->readBool("default", false, false);
					serializer->endMap();
					if (parameter.name.empty())
						return refuse("Prefab '" + tmpl.id + "' parameter name must not be empty");
					if (findPrefabParameter(tmpl, parameter.name))
						return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name + "' is duplicated");
					if (!prefabEntryExists(tmpl.entries, parameter.nodeType, parameter.entryId))
						return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
							+ "' maps unknown entry '" + parameter.entryId + "' of type '" + parameter.nodeType + "'");
					auto const target = resolvePrefabField(tmpl.entries, parameter.nodeType,
						parameter.entryId, parameter.field);
					if (!target.found())
						return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
							+ "' maps non-dial-addressable field '" + parameter.field + "' on '"
							+ parameter.nodeType + "'");
					if (target.kind != parameter.kind)
						return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
							+ "' kind does not match field '" + parameter.field + "'");
					if (parameter.kind == PrefabParameterKind::Number)
					{
						if (!std::isfinite(parameter.minValue) || !std::isfinite(parameter.maxValue)
							|| parameter.minValue > parameter.maxValue)
							return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
								+ "' range must be finite with min at or below max");
						if (!std::isfinite(parameter.defaultValue)
							|| parameter.defaultValue < parameter.minValue
							|| parameter.defaultValue > parameter.maxValue)
							return refuse("Prefab '" + tmpl.id + "' parameter '" + parameter.name
								+ "' default is outside its declared range");
					}
					tmpl.parameters.push_back(std::move(parameter));
				}
				serializer->endArray();
			}
			// The internal entries must stand on their own under the existing
			// configuration validator: the template is the expansion in
			// isolation, so the same rules that judge a draft judge the
			// template, and a failing template never reaches placement.
			std::string validationDiagnostic;
			if (!validateStationSystemsSettings(tmpl.entries, validationDiagnostic))
				return refuse("Prefab '" + tmpl.id + "' internal entries do not validate: "
					+ validationDiagnostic);
			serializer->endMap();
			out = std::move(tmpl);
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& exception)
		{
			return refuse(exception.what());
		}
	}

	bool expandPrefab(PrefabTemplate const& tmpl, PrefabOverrides const& overrides,
		std::string const& instanceId, PrefabExpansion& out, std::string& reason)
	{
		auto const refuse = [&](std::string message)
		{
			reason = std::move(message);
			return false;
		};
		if (instanceId.empty()) return refuse("Prefab instance id must not be empty");
		if (instanceId.find(PrefabIdSeparator) != std::string::npos)
			return refuse("Prefab instance id '" + instanceId + "' contains the reserved separator '"
				+ PrefabIdSeparator + "'");
		// Validate every override against the dial table before touching a
		// partial expansion, so a refusal never leaves a half-overridden
		// instance behind.
		for (auto const& [name, value] : overrides)
		{
			auto const* parameter = findPrefabParameter(tmpl, name);
			if (!parameter)
				return refuse("Override '" + name + "' names no exposed parameter of prefab '"
					+ tmpl.id + "'");
			if (parameter->kind == PrefabParameterKind::Number)
			{
				auto const* number = std::get_if<double>(&value);
				if (!number)
					return refuse("Override for parameter '" + name + "' must be a number");
				if (!std::isfinite(*number) || *number < parameter->minValue
					|| *number > parameter->maxValue)
					return refuse("Override for parameter '" + name + "' is outside its range ["
						+ std::to_string(parameter->minValue) + ", "
						+ std::to_string(parameter->maxValue) + "]");
			}
			else if (!std::get_if<bool>(&value))
				return refuse("Override for parameter '" + name + "' must be boolean");
		}
		// Namespace every internal entry. The rename machinery is the same
		// core-owned reference rewriting the editor uses, so every mapped
		// reference field - and the connection properties keyed by the old
		// ids - is carried through by one mechanism. Types the core rename
		// mechanism does not cover (heat engines, whose reservoir references
		// are not in the port mapping) cannot be namespaced safely and are
		// refused here atomically rather than landed with dangling peers.
		// The template copy is discarded on any failure, so a half-namespaced
		// instance can never escape this function.
		auto expanded = tmpl.entries;
		std::string failure;
		forEachPrefabEntry(tmpl.entries, [&](std::string_view type, std::string const& id)
		{
			if (!failure.empty()) return;
			std::string const namespaced = instanceId + PrefabIdSeparator + id;
			if (namespaced.size() > MaxStationStoreIdLength)
			{
				failure = "Namespaced id '" + namespaced + "' exceeds the "
					+ std::to_string(MaxStationStoreIdLength) + "-character id limit";
				return;
			}
			StationNodeRename plan;
			std::string diagnostic;
			std::string const typeName{type};
			if (!planStationNodeRename(expanded, typeName, id, namespaced, plan, &diagnostic)
				|| !renameStationNode(expanded, plan))
				failure = "Namespacing entry '" + typeName + "' '" + id + "' as '"
					+ namespaced + "' failed: " + diagnostic;
		});
		if (!failure.empty()) return refuse(failure);
		// Apply the overrides to their mapped internal fields.
		for (auto const& [name, value] : overrides)
		{
			auto const* parameter = findPrefabParameter(tmpl, name);
			std::string const targetId = instanceId + PrefabIdSeparator + parameter->entryId;
			auto const target = resolvePrefabField(expanded, parameter->nodeType,
				targetId, parameter->field);
			if (!target.found())
				return refuse("Override for parameter '" + name + "' lost its target field '"
					+ parameter->field + "' during namespacing");
			if (parameter->kind == PrefabParameterKind::Number)
				*target.number = std::get<double>(value);
			else
				*target.flag = std::get<bool>(value);
		}
		// The expansion validates against the existing configuration
		// validator: the flattened entries must stand on their own exactly
		// like hand-authored draft entries.
		std::string diagnostic;
		if (!validateStationSystemsSettings(expanded, diagnostic))
			return refuse("Expansion of prefab '" + tmpl.id + "' as instance '" + instanceId
				+ "' does not validate: " + diagnostic);
		PrefabExpansion expansion;
		expansion.entries = std::move(expanded);
		expansion.links = stationAuthoredLinks(expansion.entries);
		for (auto const& port : tmpl.exposedPorts)
			expansion.exposedPins[port.name] = StationPinRef{port.nodeType,
				instanceId + PrefabIdSeparator + port.entryId, port.portName};
		out = std::move(expansion);
		return true;
	}

	bool checkPrefabCaps(PrefabTemplate const& tmpl,
		StationSystemsSettings const& draft, std::string& reason)
	{
		return checkFragmentCaps("Prefab '" + tmpl.id + "'", tmpl.entries, draft, reason);
	}

	bool applyPrefabExpansion(StationSystemsSettings& draft,
		PrefabExpansion const& expansion, std::string& reason)
	{
		// Pre-checks run against a candidate copy so a refusal leaves the
		// draft untouched.
		if (!checkFragmentCaps("Prefab expansion", expansion.entries, draft, reason))
			return false;
		std::string collision;
		forEachPrefabEntry(expansion.entries, [&](std::string_view type, std::string const& id)
		{
			if (!collision.empty()) return;
			if (prefabEntryExists(draft, type, id))
				collision = "The draft already holds a " + std::string(type) + " named '" + id + "'";
		});
		// Installation ids share one namespace across generators, coolant
		// loops, chillers and heat engines: a cross-type collision is the
		// same collision the supervision contract cannot tolerate, so it is
		// refused here rather than left to poison the draft.
		if (collision.empty())
		{
			auto const draftInstallations = stationInstallationIdentities(draft);
			for (auto const& installation : stationInstallationIdentities(expansion.entries))
			{
				if (std::any_of(draftInstallations.begin(), draftInstallations.end(),
					[&](StationInstallationIdentity const& existing) {
						return existing.id == installation.id;
					}))
				{
					collision = "The draft already holds an installation named '"
						+ installation.id + "'";
					break;
				}
			}
		}
		if (!collision.empty())
		{
			reason = collision;
			return false;
		}
		auto candidate = draft;
		appendFragment(candidate, expansion.entries);
		draft = std::move(candidate);
		return true;
	}
}
