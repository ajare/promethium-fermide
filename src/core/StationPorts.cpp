#include "core/StationPorts.h"

#include <algorithm>
#include <utility>
#include <cctype>

namespace core
{
namespace
{
	void eraseConnectionProperties(StationSystemsSettings& settings, std::string const& type,
		std::string const& id, std::string const& reference)
	{
		std::erase_if(settings.connections, [&](auto const& item) {
			auto const& key = item.first;
			return key.ownerType == type && key.ownerId == id && key.reference == reference;
		});
	}

	PortDescriptor const* findPort(std::string_view nodeType, std::string_view portName)
	{
		for (auto const& descriptor : stationPortDescriptors())
			if (descriptor.nodeType == nodeType && descriptor.name == portName) return &descriptor;
		return nullptr;
	}

	std::string pinLabel(StationPinRef const& pin)
	{
		return std::string(pin.nodeType) + " '" + pin.nodeId + "' " + std::string(pin.portName);
	}

	// "Conversion input 3" with prefix "Conversion input " -> 2 (zero-based
	// flow index); -1 when the name carries no 1-based slot number.
	int conversionFlowIndex(std::string_view portName, std::string_view prefix)
	{
		if (portName.substr(0, std::min(portName.size(), prefix.size())) != prefix) return -1;
		auto const digits = portName.substr(prefix.size());
		if (digits.empty() || digits.size() > 1) return -1;
		int const index = digits[0] - '0';
		if (index < 1 || index > static_cast<int>(MaxStationConversionFlows)) return -1;
		return index - 1;
	}

	// Which flow a conversion field names, as a store id pointer. Returns
	// nullptr when the field is malformed or the flow does not exist.
	std::string* conversionFlowField(StationSystemsSettings& settings,
		std::string const& ownerId, std::string const& field)
	{
		auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
			[&](ResourceConversionSettings const& conversion) { return conversion.id == ownerId; });
		if (found == settings.conversions.end()) return nullptr;
		if (field.rfind("Input ", 0) == 0)
		{
			auto const index = conversionFlowIndex(field, "Input ");
			if (index < 0 || size_t(index) >= found->inputs.size()) return nullptr;
			return &found->inputs[size_t(index)].storeId;
		}
		if (field.rfind("Output ", 0) == 0)
		{
			auto const index = conversionFlowIndex(field, "Output ");
			if (index < 0 || size_t(index) >= found->outputs.size()) return nullptr;
			return &found->outputs[size_t(index)].storeId;
		}
		return nullptr;
	}

	// Which reference field a link writes. This is the one mapping between a
	// connector and configuration: the editor draws the connector, the seam
	// writes the field, and the enumeration below reads it back the same way.
	struct LinkMapping
	{
		std::string ownerType;
		std::string ownerId;
		std::string field;
		StationPinRef peer;
	};

	bool isScheduleType(std::string_view type)
	{
		return type == "Delivery" || type == "Waste ejection" || type == "Outage" || type == "Maintenance";
	}
	bool isInstallationType(std::string_view type)
	{
		return type == "Generator" || type == "Coolant loop" || type == "Chiller";
	}
	std::string installationNodeType(StationSystemsSettings const& settings, std::string const& id)
	{
		std::string type = "Installation";
		for (auto const& identity : stationInstallationIdentities(settings))
			if (identity.id == id)
			{
				type = identity.kind;
				type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(type[0])));
				break;
			}
		return type;
	}

	bool mapLink(CandidateLink const& link, LinkMapping& mapping)
	{
		auto const* from = findPort(link.from.nodeType, link.from.portName);
		auto const* to = findPort(link.to.nodeType, link.to.portName);
		if (!from || !to) return false;
		if (from->direction == StationPortDirection::Input
			&& to->direction == StationPortDirection::Output)
			return mapLink({link.to, link.from}, mapping);
		if (from->direction != StationPortDirection::Output
			|| to->direction != StationPortDirection::Input) return false;
		StationSystemsSettings schedules;
		schedules.deliveries.emplace_back(); schedules.wasteEjections.emplace_back();
		schedules.outages.emplace_back(); schedules.maintenanceJobs.emplace_back();
		schedules.maintenanceJobs.back().requirements.resize(MaxStationMaintenanceRequirements);
		bool scheduled = false;
		visitStationScheduleReferences(schedules, [&](auto type, auto const&, auto port, auto field,
			auto&, bool input, std::string_view peerType, auto peerPort) {
			auto const& owner = input ? link.to : link.from;
			auto const& peer = input ? link.from : link.to;
			if (owner.nodeType == type && owner.portName == port
				&& (peer.nodeType == peerType || (peerType == "Installation" && isInstallationType(peer.nodeType)))
				&& peer.portName == peerPort)
			{ mapping = {type, owner.nodeId, std::string(field), peer}; scheduled = true; }
		});
		if (scheduled) return true;
		StationSystemsSettings probe;
		probe.crews.emplace_back(); probe.cropPopulations.emplace_back();
		probe.algaeCultures.emplace_back(); probe.microbialProcessors.emplace_back();
		bool biological = false;
		visitStationBiologyReferences(probe, [&](auto type, auto const&, auto port, auto field,
			auto&, bool input, auto peerType, auto peerPort) {
			auto const& owner = input ? link.to : link.from;
			auto const& peer = input ? link.from : link.to;
			if (owner.nodeType == type && owner.portName == port
				&& peer.nodeType == peerType && peer.portName == peerPort)
			{ mapping = {type, owner.nodeId, field, peer}; biological = true; }
		});
		if (biological) return true;
		if (link.to.nodeType == "Water collector" && link.to.portName == "Atmosphere"
			&& link.from.nodeType == "Atmosphere" && link.from.portName == "Gas out")
		{
			mapping = {"Water collector", link.to.nodeId, "atmosphereId", link.from};
			return true;
		}
		if (link.to.nodeType == "Water collector" && link.to.portName == "Cooling loop"
			&& link.from.nodeType == "Coolant loop" && link.from.portName == "Cold surfaces")
		{
			mapping = {"Water collector", link.to.nodeId, "coolingLoopId", link.from};
			return true;
		}
		if (link.from.nodeType == "Water collector" && link.from.portName == "Raw water out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"Water collector", link.from.nodeId, "destStoreId", link.to};
			return true;
		}
		if (link.to.nodeType == "Water treatment" && link.to.portName == "Raw water in"
			&& link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			mapping = {"Water treatment", link.to.nodeId, "sourceStoreId", link.from};
			return true;
		}
		if (link.from.nodeType == "Water treatment" && link.from.portName == "Treated water out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"Water treatment", link.from.nodeId, "treatedStoreId", link.to};
			return true;
		}
		if (link.from.nodeType == "Water treatment" && link.from.portName == "Concentrate out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"Water treatment", link.from.nodeId, "concentrateStoreId", link.to};
			return true;
		}
		if (link.to.nodeType == "Water treatment" && link.to.portName == "Consumable in"
			&& link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			mapping = {"Water treatment", link.to.nodeId, "consumableStoreId", link.from};
			return true;
		}
		if (link.to.nodeType == "Water treatment" && link.to.portName == "Clearance in"
			&& link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			mapping = {"Water treatment", link.to.nodeId, "clearanceStoreId", link.from};
			return true;
		}
		if (link.to.nodeType == "Electrolyzer" && link.to.portName == "Water in"
			&& link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			mapping = {"Electrolyzer", link.to.nodeId, "waterStoreId", link.from};
			return true;
		}
		if (link.from.nodeType == "Electrolyzer" && link.from.portName == "Oxygen out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"Electrolyzer", link.from.nodeId, "oxygenStoreId", link.to};
			return true;
		}
		if (link.from.nodeType == "Electrolyzer" && link.from.portName == "Hydrogen out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"Electrolyzer", link.from.nodeId, "hydrogenStoreId", link.to};
			return true;
		}
		if (link.from.nodeType == "Electrolyzer" && link.from.portName == "Compartment oxygen"
			&& link.to.nodeType == "Atmosphere" && link.to.portName == "Oxygen in")
		{
			mapping = {"Electrolyzer", link.from.nodeId, "atmosphereId", link.to};
			return true;
		}
		if (link.from.nodeType == "Electrolyzer" && link.from.portName == "Heat out"
			&& link.to.nodeType == "Reservoir" && link.to.portName == "Heat in")
		{
			mapping = {"Electrolyzer", link.from.nodeId, "heatSinkId", link.to};
			return true;
		}
		if (link.to.nodeType == "CO2 capture" && link.to.portName == "Atmosphere"
			&& link.from.nodeType == "Atmosphere" && link.from.portName == "Gas out")
		{
			mapping = {"CO2 capture", link.to.nodeId, "atmosphereId", link.from};
			return true;
		}
		if (link.from.nodeType == "CO2 capture" && link.from.portName == "Captured CO2 out"
			&& link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			mapping = {"CO2 capture", link.from.nodeId, "captureStoreId", link.to};
			return true;
		}
		if (link.to.nodeType == "CO2 capture" && link.to.portName == "Consumable in"
			&& link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			mapping = {"CO2 capture", link.to.nodeId, "consumableStoreId", link.from};
			return true;
		}
		if (link.from.nodeType == "Atmosphere" && link.from.portName == "Heat out"
			&& link.to.nodeType == "Reservoir" && link.to.portName == "Heat in")
		{
			mapping = {"Atmosphere", link.from.nodeId, "reservoirId", link.to};
			return true;
		}
		if (link.to.nodeType == "Airlock cycle" && link.to.portName == "Atmosphere"
			&& link.from.nodeType == "Atmosphere" && link.from.portName == "Gas out")
		{
			mapping = {"Airlock cycle", link.to.nodeId, "atmosphereId", link.from};
			return true;
		}
		if (link.to.nodeType == "Transfer" && link.to.portName == "Source" && link.from.nodeType == "Store")
		{
			mapping = {"Transfer", link.to.nodeId, "Source", link.from};
			return true;
		}
		if (link.from.nodeType == "Transfer" && link.from.portName == "Destination" && link.to.nodeType == "Store")
		{
			mapping = {"Transfer", link.from.nodeId, "Destination", link.to};
			return true;
		}
		// A conversion connector stands for the numbered flow it names:
		// "Conversion input 2" writes inputs[1].storeId, "Conversion output 3"
		// writes outputs[2].storeId. The field keeps the same 1-based name the
		// descriptor and the identity use, so nothing re-derives the position.
		if (link.from.nodeType == "Store" && link.from.portName == "Inventory out"
			&& link.to.nodeType == "Conversion")
		{
			auto const index = conversionFlowIndex(link.to.portName, "Conversion input ");
			if (index >= 0)
			{
				mapping = {"Conversion", link.to.nodeId, "Input " + std::to_string(index + 1), link.from};
				return true;
			}
			return false;
		}
		if (link.from.nodeType == "Conversion" && link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			auto const index = conversionFlowIndex(link.from.portName, "Conversion output ");
			if (index >= 0)
			{
				mapping = {"Conversion", link.from.nodeId, "Output " + std::to_string(index + 1), link.to};
				return true;
			}
			return false;
		}
		if (link.from.nodeType == "Reservoir" && link.from.portName == "Hosted buffers"
			&& link.to.nodeType == "Thermal buffer" && link.to.portName == "Host reservoir")
		{
			mapping = {"Thermal buffer", link.to.nodeId, "Host reservoir", link.from};
			return true;
		}
		if (link.from.nodeType == "Coolant loop" && link.from.portName == "Buffer gates"
			&& link.to.nodeType == "Thermal buffer" && link.to.portName == "Circulation gate")
		{
			mapping = {"Thermal buffer", link.to.nodeId, "Circulation gate", link.from};
			return true;
		}
		if (link.from.nodeType == "Coolant loop" && link.from.portName == "Cold interface"
			&& link.to.nodeType == "Chiller" && link.to.portName == "Cold loop")
		{
			mapping = {"Chiller", link.to.nodeId, "Cold loop", link.from};
			return true;
		}
		// A power or conversion chain connector (#647) owns the reference
		// field its port names on the draft entry: the machine side of the
		// oriented pair is the owner, and the Store face or the Reservoir
		// heat-in face is the peer. The field name comes from the same
		// table the enumeration and the write resolve through, so a link,
		// its identity and its clearing can never disagree.
		if (auto const field = stationMachineField(link.from.nodeType, link.from.portName);
			!field.empty())
		{
			bool const heat = field == "Heat sink" || field == "Host reservoir" || field == "Condenser reservoir";
			bool const peerMatches = heat
				? (link.to.nodeType == "Reservoir" && link.to.portName == "Heat in")
				: (link.to.nodeType == "Store" && link.to.portName == "Inventory in");
			if (peerMatches)
			{
				mapping = {std::string(link.from.nodeType), link.from.nodeId,
					std::string(field), link.to};
				return true;
			}
			return false;
		}
		if (link.from.nodeType == "Store" && link.from.portName == "Inventory out")
		{
			if (auto const field = stationMachineField(link.to.nodeType, link.to.portName);
				!field.empty() && field != "Heat sink")
			{
				mapping = {std::string(link.to.nodeType), link.to.nodeId,
					std::string(field), link.from};
				return true;
			}
			return false;
		}
		// #657: the radiator's host mount. The radiator owns the reference
		// on the input side of the oriented pair (reservoir -> radiator),
		// so it resolves through this explicit branch rather than the
		// machine-output branch above; the field name still comes from the
		// shared stationMachineField table so write, clear and enumeration
		// resolve the same connector to the same member.
		if (link.from.nodeType == "Reservoir" && link.from.portName == "Hosted radiators"
			&& link.to.nodeType == "Radiator" && link.to.portName == "Host reservoir")
		{
			mapping = {"Radiator", link.to.nodeId,
				std::string(stationMachineField("Radiator", "Host reservoir")), link.from};
			return true;
		}
		return false;
	}

	std::string linkIdentity(LinkMapping const& mapping)
	{
		return mapping.ownerType + ":" + mapping.ownerId + ":" + mapping.field;
	}

	std::string linkIdentity(char const* ownerType, std::string const& ownerId, std::string const& field)
	{
		return std::string(ownerType) + ":" + ownerId + ":" + field;
	}

	// "Material in 2" with prefix "Material in " -> 1 (zero-based slot on
	// that face); -1 when the field carries no 1-based slot number. The
	// slot counts exchanges of that direction in vector order, so a pin
	// never depends on the other face's entries.
	int loadExchangeSlot(std::string_view field, std::string_view prefix)
	{
		if (field.substr(0, std::min(field.size(), prefix.size())) != prefix) return -1;
		auto const digits = field.substr(prefix.size());
		if (digits.empty() || digits.size() > 1) return -1;
		int const index = digits[0] - '0';
		if (index < 1 || index > static_cast<int>(MaxStationLoadMaterialExchanges)) return -1;
		return index - 1;
	}

	bool isMachineOwnerType(std::string const& ownerType)
	{
		// The radiator (#657) is not a machine, but its host mount routes
		// through the same field resolver for write and clear, so it shares
		// the routing predicate.
		return isScheduleType(ownerType) || ownerType == "Crew" || ownerType == "Crops" || ownerType == "Algae" || ownerType == "Microbial processor"
			|| ownerType == "Water collector" || ownerType == "Water treatment" || ownerType == "Electrolyzer" || ownerType == "CO2 capture" || ownerType == "Atmosphere" || ownerType == "Airlock cycle"
			|| ownerType == "Equipment" || ownerType == "Generator" || ownerType == "Fuel cell"
			|| ownerType == "Sabatier" || ownerType == "Methane processor" || ownerType == "Radiator"
			|| ownerType == "Coolant loop" || ownerType == "Chiller" || ownerType == "Thermal buffer";
	}

	// The reference field a machine link names, as a pointer into the draft
	// entry. Returns nullptr when the entry or the field does not exist,
	// leaving the draft untouched. The field names are the ones
	// stationMachineField() declares, so write, clear, enumeration and the
	// scoped rule all resolve the same connector to the same member.
	// Templated so the scoped rule can read through it with a const draft.
	template <typename SettingsT>
	auto machineField(SettingsT& settings, std::string const& ownerType,
		std::string const& ownerId, std::string const& field)
		-> std::remove_reference_t<decltype((settings.equipmentLoads[0].heatSinkId))>*
	{
		using Pointer = std::remove_reference_t<decltype((settings.crews[0].atmosphereId))>*;
		Pointer scheduled = nullptr;
		visitStationScheduleReferences(settings, [&](auto type, auto const& id, auto, auto name,
			auto& reference, bool, auto, auto) {
			if (ownerType == type && ownerId == id && field == name) scheduled = &reference;
		});
		if (scheduled) return scheduled;
		Pointer biological = nullptr;
		visitStationBiologyReferences(settings, [&](auto type, auto const& id, auto, auto name,
			auto& reference, bool, auto, auto) {
			if (ownerType == type && ownerId == id && field == name) biological = &reference;
		});
		if (biological) return biological;
		if (ownerType == "Water collector")
		{
			auto found = std::find_if(settings.waterCollectors.begin(), settings.waterCollectors.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.waterCollectors.end()) return nullptr;
			if (field == "atmosphereId") return &found->atmosphereId;
			if (field == "coolingLoopId") return &found->coolingLoopId;
			if (field == "destStoreId") return &found->destStoreId;
			return nullptr;
		}
		if (ownerType == "Water treatment")
		{
			auto found = std::find_if(settings.waterTreatments.begin(), settings.waterTreatments.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.waterTreatments.end()) return nullptr;
			if (field == "sourceStoreId") return &found->sourceStoreId;
			if (field == "treatedStoreId") return &found->treatedStoreId;
			if (field == "concentrateStoreId") return &found->concentrateStoreId;
			if (field == "consumableStoreId") return &found->consumableStoreId;
			if (field == "clearanceStoreId") return &found->clearanceStoreId;
			return nullptr;
		}
		if (ownerType == "Electrolyzer")
		{
			auto found = std::find_if(settings.electrolyzers.begin(), settings.electrolyzers.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.electrolyzers.end()) return nullptr;
			if (field == "waterStoreId") return &found->waterStoreId;
			if (field == "oxygenStoreId") return &found->oxygenStoreId;
			if (field == "hydrogenStoreId") return &found->hydrogenStoreId;
			if (field == "atmosphereId") return &found->atmosphereId;
			if (field == "heatSinkId") return &found->heatSinkId;
			return nullptr;
		}
		if (ownerType == "CO2 capture")
		{
			auto found = std::find_if(settings.co2Captures.begin(), settings.co2Captures.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.co2Captures.end()) return nullptr;
			if (field == "atmosphereId") return &found->atmosphereId;
			if (field == "captureStoreId") return &found->captureStoreId;
			if (field == "consumableStoreId") return &found->consumableStoreId;
			return nullptr;
		}
		if (ownerType == "Atmosphere")
		{
			auto found = std::find_if(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.atmospheres.end()) return nullptr;
			if (field == "reservoirId") return &found->reservoirId;
			return nullptr;
		}
		if (ownerType == "Airlock cycle")
		{
			auto found = std::find_if(settings.airlockCycles.begin(), settings.airlockCycles.end(),
				[&](auto const& entry) { return entry.id == ownerId; });
			if (found == settings.airlockCycles.end()) return nullptr;
			if (field == "atmosphereId") return &found->atmosphereId;
			return nullptr;
		}
		if (ownerType == "Equipment")
		{
			auto const found = std::find_if(settings.equipmentLoads.begin(),
				settings.equipmentLoads.end(),
				[&](EquipmentLoadSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.equipmentLoads.end()) return nullptr;
			if (field == "Heat sink") return &found->heatSinkId;
			if (auto const slot = loadExchangeSlot(field, "Material in "); slot >= 0)
			{
				int seen = 0;
				for (auto& exchange : found->materialExchanges)
					if (exchange.consumes && seen++ == slot) return &exchange.storeId;
				return nullptr;
			}
			if (auto const slot = loadExchangeSlot(field, "Material return "); slot >= 0)
			{
				int seen = 0;
				for (auto& exchange : found->materialExchanges)
					if (!exchange.consumes && seen++ == slot) return &exchange.storeId;
				return nullptr;
			}
			return nullptr;
		}
		if (ownerType == "Generator")
		{
			auto const found = std::find_if(settings.generators.begin(), settings.generators.end(),
				[&](GeneratorSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.generators.end()) return nullptr;
			static constexpr std::pair<char const*, std::string GeneratorSettings::*> const members[] = {
				{"Fuel store", &GeneratorSettings::fuelStoreId},
				{"Heat sink", &GeneratorSettings::heatSinkId}};
			for (auto const& [name, member] : members)
				if (field == name) return &((*found).*member);
			return nullptr;
		}
		if (ownerType == "Fuel cell")
		{
			auto const found = std::find_if(settings.fuelCells.begin(), settings.fuelCells.end(),
				[&](FuelCellSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.fuelCells.end()) return nullptr;
			static constexpr std::pair<char const*, std::string FuelCellSettings::*> const members[] = {
				{"Hydrogen store", &FuelCellSettings::hydrogenStoreId},
				{"Oxygen store", &FuelCellSettings::oxygenStoreId},
				{"Water store", &FuelCellSettings::waterStoreId},
				{"Heat sink", &FuelCellSettings::heatSinkId}};
			for (auto const& [name, member] : members)
				if (field == name) return &((*found).*member);
			return nullptr;
		}
		if (ownerType == "Sabatier")
		{
			auto const found = std::find_if(settings.sabatierReactors.begin(),
				settings.sabatierReactors.end(),
				[&](SabatierReactorSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.sabatierReactors.end()) return nullptr;
			static constexpr std::pair<char const*, std::string SabatierReactorSettings::*> const members[] = {
				{"CO2 store", &SabatierReactorSettings::co2StoreId},
				{"Hydrogen store", &SabatierReactorSettings::hydrogenStoreId},
				{"Methane store", &SabatierReactorSettings::methaneStoreId},
				{"Water store", &SabatierReactorSettings::waterStoreId},
				{"Heat sink", &SabatierReactorSettings::heatSinkId}};
			for (auto const& [name, member] : members)
				if (field == name) return &((*found).*member);
			return nullptr;
		}
		if (ownerType == "Methane processor")
		{
			auto const found = std::find_if(settings.methaneProcessors.begin(),
				settings.methaneProcessors.end(),
				[&](MethaneProcessorSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.methaneProcessors.end()) return nullptr;
			static constexpr std::pair<char const*, std::string MethaneProcessorSettings::*> const members[] = {
				{"Methane store", &MethaneProcessorSettings::methaneStoreId},
				{"Hydrogen store", &MethaneProcessorSettings::hydrogenStoreId},
				{"Carbon store", &MethaneProcessorSettings::carbonStoreId},
				{"Heat sink", &MethaneProcessorSettings::heatSinkId}};
			for (auto const& [name, member] : members)
				if (field == name) return &((*found).*member);
			return nullptr;
		}
		if (ownerType == "Thermal buffer")
		{
			auto const found = std::find_if(settings.thermalBuffers.begin(), settings.thermalBuffers.end(),
				[&](ThermalBufferSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.thermalBuffers.end()) return nullptr;
			if (field == "Host reservoir") return &found->reservoirId;
			if (field == "Circulation gate") return &found->circulationLoopId;
			return nullptr;
		}
		if (ownerType == "Chiller")
		{
			auto const found = std::find_if(settings.chillers.begin(), settings.chillers.end(),
				[&](ChillerSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.chillers.end()) return nullptr;
			if (field == "Cold loop") return &found->coldLoopId;
			if (field == "Condenser reservoir") return &found->condenserReservoirId;
			return nullptr;
		}
		if (ownerType == "Coolant loop")
		{
			auto const found = std::find_if(settings.coolantLoops.begin(), settings.coolantLoops.end(),
				[&](CoolantLoopSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.coolantLoops.end()) return nullptr;
			if (field == "Host reservoir") return &found->reservoirId;
			return nullptr;
		}
		if (ownerType == "Radiator")
		{
			// #657: the host mount writes the radiator's own reservoirId.
			// One field, one connector; the radiator owns no other reference.
			auto const found = std::find_if(settings.radiators.begin(), settings.radiators.end(),
				[&](RadiatorSettings const& entry) { return entry.id == ownerId; });
			if (found == settings.radiators.end()) return nullptr;
			if (field == "Host reservoir") return &found->reservoirId;
			return nullptr;
		}
		return nullptr;
	}

	ResourceStoreSettings const* findStoreById(StationSystemsSettings const& settings,
		std::string const& storeId)
	{
		auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
			[&](ResourceStoreSettings const& store) { return store.id == storeId; });
		return found == settings.stores.end() ? nullptr : &*found;
	}

	ThermalReservoirSettings const* findReservoirById(StationSystemsSettings const& settings,
		std::string const& reservoirId)
	{
		auto const found = std::find_if(settings.thermalReservoirs.begin(),
			settings.thermalReservoirs.end(),
			[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == reservoirId; });
		return found == settings.thermalReservoirs.end() ? nullptr : &*found;
	}

	// The heat-sink rules one link can break, in the whole-config
	// validator's own wording: the machine's thermal service class must
	// be one of the declared classes, the sink must exist and share the
	// machine's thermal service class - spare capacity in one class is
	// never usable by another. Returns the refusal text, empty when the
	// sink is satisfied or not yet named.
	std::string scopedHeatSinkWiring(StationSystemsSettings const& settings,
		std::string const& where, std::string const& heatSinkId,
		std::string const& thermalClass, bool fuelCellWorded)
	{
		if (heatSinkId.empty()) return {};
		// A heat link routes under the machine's declared service class;
		// if that class is not one the configuration declares, the link
		// would route into a service that cannot exist. Mirrors the whole-
		// config validator's own entry rule, checked before the sink is
		// resolved so the more fundamental defect answers first.
		if (std::find(StationThermalServiceClasses.begin(),
				StationThermalServiceClasses.end(), thermalClass)
			== StationThermalServiceClasses.end())
			return where + " has unknown thermal service class '" + thermalClass
				+ "'; declared classes are: low, moderate, warm, generation";
		auto const* sink = findReservoirById(settings, heatSinkId);
		if (!sink)
			return where + (fuelCellWorded ? " heat sink is unknown thermal reservoir '"
				: " heat sink references unknown thermal reservoir '") + heatSinkId + "'";
		if (sink->thermalClass != thermalClass)
		{
			if (fuelCellWorded)
				return where + " thermal class '" + thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another";
			return where + " thermal class '" + thermalClass
				+ "' is incompatible with heat sink reservoir '" + sink->id + "' of class '"
				+ sink->thermalClass + "'; heat routes only to the same service class";
		}
		return {};
	}

	// The store-role rules one link can break, mirroring the whole-config
	// validator's wording for the family: the store must exist, be plain
	// mass where the family demands it, and carry the family's own hazard
	// class - a process may only touch stores of its own class. Empty
	// hazardClass skips the hazard rule (equipment exchanges carry no
	// class of their own). Returns the refusal text, empty when satisfied.
	std::string scopedRoleStoreWiring(StationSystemsSettings const& settings,
		std::string const& where, std::string const& role, std::string const& storeId,
		std::string const& hazardClass, bool massDenominated, char const* massPhrase,
		char const* hazardFamily)
	{
		if (storeId.empty()) return {};
		auto const* store = findStoreById(settings, storeId);
		if (!store)
			return where + " " + role + " store '" + storeId + "' does not exist";
		if (massDenominated && store->unit != "g" && store->unit != "kg")
			return where + " " + role + " store '" + store->id
				+ "' must be a mass unit; " + massPhrase;
		if (massDenominated && store->composition)
			return where + " " + role + " store '" + store->id
				+ (std::string_view(hazardFamily) == "water"
					? "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate"
					: "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
		if (!hazardClass.empty() && store->hazardClass != hazardClass)
			return where + " " + role + " store '" + store->id + "' is authored "
				+ store->hazardClass + "-class but this is a " + hazardClass + "-class "
				+ hazardFamily + " process; a process may only touch stores of its own hazard class";
		return {};
	}

	// The peers already joined to one connector, for a refusal that names
	// what is in the way rather than just saying "full".
	std::vector<std::string> joinedPeers(std::vector<AuthoredLink> const& authored,
		StationPinRef const& pin, bool incoming)
	{
		std::vector<std::string> peers;
		for (auto const& link : authored)
		{
			auto const& self = incoming ? link.to : link.from;
			if (!(self == pin)) continue;
			peers.push_back(pinLabel(incoming ? link.from : link.to));
		}
		return peers;
	}

	std::string occupiedReason(StationPinRef const& pin, size_t limit, std::vector<std::string> const& peers)
	{
		std::string reason = pinLabel(pin) + " already joins ";
		for (size_t i = 0; i < peers.size(); ++i) reason += (i == 0 ? "" : ", ") + peers[i];
		return reason + "; it takes at most " + std::to_string(limit)
			+ (limit == 1 ? " link" : " links") + " - disconnect one first.";
	}

	LinkVerdict refuse(std::string reason)
	{
		LinkVerdict verdict;
		verdict.allowed = false;
		verdict.reason = std::move(reason);
		return verdict;
	}

	LinkVerdict allow()
	{
		LinkVerdict verdict;
		verdict.allowed = true;
		return verdict;
	}

	// The endpoint-level rules a conversion link can violate, judged with the
	// sibling flows the user has not wired yet tolerated: they are the user's
	// next action, not this link's defect. Rates and the constituent balance
	// belong to the complete entry and stay with the whole-configuration
	// validator; what one connector can break is the store it names: it must
	// exist, be compound, and be used once. Hazard-class agreement between
	// endpoints is no longer a rule (#750): a cross-class conversion link is
	// judged only on what the connector itself breaks.
	bool scopedConversionWiring(StationSystemsSettings const& settings,
		ResourceConversionSettings const& conversion, std::string const& field,
		std::string& diagnostic)
	{
		auto const where = "Station resource conversion '" + conversion.id + "'";
		bool isInput = false;
		size_t index = 0;
		if (field.rfind("Input ", 0) == 0)
		{
			isInput = true;
			index = size_t(conversionFlowIndex(field, "Input "));
			if (index >= conversion.inputs.size()) return true;
		}
		else if (field.rfind("Output ", 0) == 0)
		{
			index = size_t(conversionFlowIndex(field, "Output "));
			if (index >= conversion.outputs.size()) return true;
		}
		else return true;
		auto const kind = isInput ? "input" : "output";
		auto const& storeId = isInput ? conversion.inputs[index].storeId : conversion.outputs[index].storeId;
		if (storeId.empty()) return true; // the link wrote nothing judgeable
		auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
		{
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == id; });
			return it == settings.stores.end() ? nullptr : &*it;
		};
		auto const* store = findStore(storeId);
		if (!store)
		{
			diagnostic = where + " references unknown " + kind + " store '" + storeId + "'";
			return true;
		}
		if (!store->composition)
		{
			diagnostic = where + " " + kind + " store '" + store->id
			+ "' has no declared composition; conversions require compound stores";
			return true;
		}
		// Every other resolvable wired endpoint of this conversion must not
		// already be this same store; class agreement is no longer judged.
		for (size_t other = 0; other < conversion.inputs.size(); ++other)
		{
			if (isInput && other == index) continue;
			auto const& otherId = conversion.inputs[other].storeId;
			if (otherId.empty()) continue;
			if (otherId == storeId)
			{
				diagnostic = where + " uses store '" + storeId + "' more than once";
				return true;
			}
		}
		for (size_t other = 0; other < conversion.outputs.size(); ++other)
		{
			if (!isInput && other == index) continue;
			auto const& otherId = conversion.outputs[other].storeId;
			if (otherId.empty()) continue;
			if (otherId == storeId)
			{
				diagnostic = where + " uses store '" + storeId + "' more than once";
				return true;
			}
		}
		return true;
	}

	// The validator's own rule for the entry a link writes, judged with the
	// counterpart endpoint the user has not wired yet tolerated. diagnostic
	// is empty when the entry is satisfied and otherwise carries the rule's
	// own wording. Returns false when the owner type has no scoped rule yet -
	// a fan-out ticket adds its entry's rule here beside its descriptors.
	// Link-local counterpart of the validator's chiller rules. Check each
	// available interface independently: a missing draft reference must not
	// mask a class, exclusivity or same-reservoir violation on another.
	std::string scopedChillerWiring(StationSystemsSettings const& settings, ChillerSettings const& chiller)
	{
		auto const where = "Station chiller '" + chiller.id + "'";
		if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
			chiller.thermalClass) == StationThermalServiceClasses.end())
			return where + " has unknown thermal service class '" + chiller.thermalClass
				+ "'; declared classes are: low, moderate, warm, generation";
		auto const loop = std::find_if(settings.coolantLoops.begin(), settings.coolantLoops.end(),
			[&](CoolantLoopSettings const& entry) { return entry.id == chiller.coldLoopId; });
		if (loop != settings.coolantLoops.end())
			for (auto const& other : settings.chillers)
				if (&other != &chiller && other.coldLoopId == chiller.coldLoopId)
					return where + " shares cold loop '" + chiller.coldLoopId
						+ "' with another chiller; one evaporator serves one chiller";
		auto const* sink = findReservoirById(settings, chiller.condenserReservoirId);
		auto const* host = loop == settings.coolantLoops.end() ? nullptr
			: findReservoirById(settings, loop->reservoirId);
		if (host && host->thermalClass != chiller.thermalClass)
			return where + " thermal class '" + chiller.thermalClass
				+ "' is incompatible with cold loop '" + loop->id + "' serving reservoir '"
				+ host->id + "' of class '" + host->thermalClass + "'; cold services are class-matched";
		if (sink && sink->thermalClass != chiller.thermalClass)
			return where + " thermal class '" + chiller.thermalClass
				+ "' is incompatible with condenser sink reservoir '" + sink->id
				+ "' of class '" + sink->thermalClass + "'; heat routes only to the same service class";
		if (host && sink && host->id == sink->id)
			return where + " rejects its condenser heat into reservoir '" + sink->id
				+ "', the same reservoir its cold side cools; that is a heater, not a cooling service";
		if (!chiller.coldLoopId.empty() && loop == settings.coolantLoops.end())
			return where + " cold-side interface references unknown coolant loop '" + chiller.coldLoopId + "'";
		if (!chiller.condenserReservoirId.empty() && !sink)
			return where + " condenser sink references unknown thermal reservoir '" + chiller.condenserReservoirId + "'";
		return {};
	}

	bool scopedWiringRule(StationSystemsSettings const& settings, LinkMapping const& mapping,
		std::string& diagnostic)
	{
		diagnostic.clear();
		if (isScheduleType(mapping.ownerType))
		{
			auto validate = [&](auto const& entries) {
				for (auto const& entry : entries)
					if (entry.id == mapping.ownerId)
					{ validateStationScheduleWiring(settings, entry, diagnostic, true); return true; }
				return false;
			};
			if (mapping.ownerType == "Delivery") return validate(settings.deliveries);
			if (mapping.ownerType == "Waste ejection") return validate(settings.wasteEjections);
			if (mapping.ownerType == "Outage") return validate(settings.outages);
			return validate(settings.maintenanceJobs);
		}
		if (mapping.ownerType == "Crew" || mapping.ownerType == "Crops"
			|| mapping.ownerType == "Algae" || mapping.ownerType == "Microbial processor")
		{
			auto scoped = settings;
			auto retain = [&](auto& entries, char const* type) {
				std::erase_if(entries, [&](auto const& entry) {
					return mapping.ownerType != type || entry.id != mapping.ownerId;
				});
			};
			retain(scoped.crews, "Crew"); retain(scoped.cropPopulations, "Crops");
			retain(scoped.algaeCultures, "Algae"); retain(scoped.microbialProcessors, "Microbial processor");
			validateStationBiologyWiring(scoped, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Water collector")
		{
			auto found = std::find_if(settings.waterCollectors.begin(), settings.waterCollectors.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.waterCollectors.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Water treatment")
		{
			auto found = std::find_if(settings.waterTreatments.begin(), settings.waterTreatments.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.waterTreatments.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Electrolyzer")
		{
			auto found = std::find_if(settings.electrolyzers.begin(), settings.electrolyzers.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.electrolyzers.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "CO2 capture")
		{
			auto found = std::find_if(settings.co2Captures.begin(), settings.co2Captures.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.co2Captures.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Atmosphere")
		{
			auto found = std::find_if(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.atmospheres.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Airlock cycle")
		{
			auto found = std::find_if(settings.airlockCycles.begin(), settings.airlockCycles.end(),
				[&](auto const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.airlockCycles.end()) return false;
			validateStationWaterGasWiring(settings, *found, diagnostic, true);
			return true;
		}
		if (mapping.ownerType == "Conversion")
		{
			auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
				[&](ResourceConversionSettings const& conversion) { return conversion.id == mapping.ownerId; });
			if (found == settings.conversions.end()) return false;
			std::string text;
			scopedConversionWiring(settings, *found, mapping.field, text);
			if (!text.empty()) diagnostic = std::move(text);
			return true;
		}
		if (mapping.ownerType == "Equipment")
		{
			auto const found = std::find_if(settings.equipmentLoads.begin(),
				settings.equipmentLoads.end(),
				[&](EquipmentLoadSettings const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.equipmentLoads.end()) return false;
			auto const where = "Station equipment load '" + found->id + "'";
			if (mapping.field == "Heat sink")
			{
				// A cooling-service load needs a cooling service of its
				// class, not a passive sink: the validator refuses the
				// combination itself, before the sink is even resolved.
				if (!found->heatSinkId.empty()
					&& found->coolingDisposition != StationCoolingDisposition::Exported)
					diagnostic = where + " declares a heat sink but requires a cooling service; a cooling-service load needs a cooling service of its class, not a heat sink";
				else diagnostic = scopedHeatSinkWiring(settings, where, found->heatSinkId,
						found->thermalClass, false);
				return true;
			}
			if (mapping.field.rfind("Material in ", 0) == 0
				|| mapping.field.rfind("Material return ", 0) == 0)
			{
				auto const* storeId = machineField(settings, mapping.ownerType,
					mapping.ownerId, mapping.field);
				if (!storeId) return true;
				if (storeId->empty()) return true;
				if (!findStoreById(settings, *storeId))
				{
					diagnostic = where + " material exchange references unknown store '"
						+ *storeId + "'";
					return true;
				}
				// One store may stand for one exchange of this load only:
				// two exchanges against the same inventory spend the same
				// stock twice against rates that expect separate pools.
				for (auto const& exchange : found->materialExchanges)
					if (&exchange.storeId != storeId && exchange.storeId == *storeId)
					{
						diagnostic = where + " exchanges with store '" + *storeId
							+ "' more than once";
						return true;
					}
				return true;
			}
			return true;
		}
		if (mapping.ownerType == "Generator")
		{
			auto const found = std::find_if(settings.generators.begin(), settings.generators.end(),
				[&](GeneratorSettings const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.generators.end()) return false;
			auto const where = "Station generator '" + found->id + "'";
			if (mapping.field == "Fuel store")
			{
				if (found->fuelStoreId.empty()) return true;
				auto const* fuel = findStoreById(settings, found->fuelStoreId);
				if (!fuel)
				{
					diagnostic = where + " fuel inventory references unknown Resource store '"
						+ found->fuelStoreId + "'";
					return true;
				}
				if (fuel->unit != "kJ" && fuel->unit != "MJ" && fuel->unit != "Wh"
					&& fuel->unit != "kWh")
				{
					diagnostic = where + " fuel store unit '" + fuel->unit
						+ "' is not an energy unit; fuelled generators burn kJ, MJ, Wh or kWh";
					return true;
				}
				return true;
			}
			if (mapping.field == "Heat sink")
			{
				diagnostic = scopedHeatSinkWiring(settings, where, found->heatSinkId,
					found->thermalClass, false);
				return true;
			}
			return true;
		}
		if (mapping.ownerType == "Fuel cell" || mapping.ownerType == "Sabatier"
			|| mapping.ownerType == "Methane processor")
		{
			// The chain families: every store the machine touches carries
			// the machine's own hazard class, the streams are chemically
			// distinct roles that may not alias one store, and the heat
			// sink shares the machine's thermal service class.
			std::string where;
			std::string hazardClass;
			std::string thermalClass;
			struct RoleRef
			{
				char const* field;
				char const* role;
				std::string const* value;
			};
			std::vector<RoleRef> roles;
			if (mapping.ownerType == "Fuel cell")
			{
				auto const found = std::find_if(settings.fuelCells.begin(), settings.fuelCells.end(),
					[&](FuelCellSettings const& entry) { return entry.id == mapping.ownerId; });
				if (found == settings.fuelCells.end()) return false;
				where = "Station fuel cell '" + found->id + "'";
				hazardClass = found->hazardClass;
				thermalClass = found->thermalClass;
				roles = {{"Hydrogen store", "hydrogen reactant", &found->hydrogenStoreId},
					{"Oxygen store", "oxygen reactant", &found->oxygenStoreId},
					{"Water store", "product water", &found->waterStoreId}};
			}
			else if (mapping.ownerType == "Sabatier")
			{
				auto const found = std::find_if(settings.sabatierReactors.begin(),
					settings.sabatierReactors.end(),
					[&](SabatierReactorSettings const& entry) { return entry.id == mapping.ownerId; });
				if (found == settings.sabatierReactors.end()) return false;
				where = "Station Sabatier reactor '" + found->id + "'";
				hazardClass = found->hazardClass;
				thermalClass = found->thermalClass;
				roles = {{"CO2 store", "CO2 feed", &found->co2StoreId},
					{"Hydrogen store", "hydrogen feed", &found->hydrogenStoreId},
					{"Methane store", "methane", &found->methaneStoreId},
					{"Water store", "product water", &found->waterStoreId}};
			}
			else
			{
				auto const found = std::find_if(settings.methaneProcessors.begin(),
					settings.methaneProcessors.end(),
					[&](MethaneProcessorSettings const& entry) { return entry.id == mapping.ownerId; });
				if (found == settings.methaneProcessors.end()) return false;
				where = "Station methane processor '" + found->id + "'";
				hazardClass = found->hazardClass;
				thermalClass = found->thermalClass;
				roles = {{"Methane store", "methane feed", &found->methaneStoreId},
					{"Hydrogen store", "recovered hydrogen", &found->hydrogenStoreId},
					{"Carbon store", "solid carbon", &found->carbonStoreId}};
			}
			if (mapping.field == "Heat sink")
			{
				auto const* sinkField = machineField(settings, mapping.ownerType,
					mapping.ownerId, mapping.field);
				diagnostic = sinkField
					? scopedHeatSinkWiring(settings, where, *sinkField, thermalClass, true)
					: std::string{};
				return true;
			}
			for (auto const& entry : roles)
			{
				if (mapping.field != entry.field) continue;
				auto const role = std::string(entry.role);
				auto const& value = *entry.value;
				if (value.empty()) return true;
				// Aliased roles: one store may not stand for two chemically
				// distinct streams the ledger books separately.
				for (auto const& other : roles)
					if (other.value != entry.value && !other.value->empty() && *other.value == value)
					{
						diagnostic = where + " names the same store '" + value
							+ "' for the " + role + " and the " + other.role
							+ ": chemically distinct streams may not share one store";
						return true;
					}
				bool const water = role == "product water";
				diagnostic = scopedRoleStoreWiring(settings, where, role, value, hazardClass,
						true, water ? "water recovery is mass-denominated (g or kg)"
							: "gas handling is mass-denominated (g or kg)",
						water ? "water" : "gas");
				return true;
			}
			return true;
		}
		if (mapping.ownerType == "Thermal buffer")
		{
			auto const found = std::find_if(settings.thermalBuffers.begin(), settings.thermalBuffers.end(),
				[&](ThermalBufferSettings const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.thermalBuffers.end()) return false;
			auto const where = "Station thermal buffer '" + found->id + "'";
			if (mapping.field == "Host reservoir")
			{
				if (!found->reservoirId.empty() && !findReservoirById(settings, found->reservoirId))
					diagnostic = where + " references unknown host thermal reservoir '" + found->reservoirId + "'";
				return true;
			}
			if (mapping.field == "Circulation gate")
			{
				auto const loop = std::find_if(settings.coolantLoops.begin(), settings.coolantLoops.end(),
					[&](CoolantLoopSettings const& entry) { return entry.id == found->circulationLoopId; });
				if (!found->circulationLoopId.empty() && loop == settings.coolantLoops.end())
					diagnostic = where + " circulation access references unknown coolant loop '" + found->circulationLoopId + "'";
				else if (loop != settings.coolantLoops.end() && !found->reservoirId.empty()
					&& loop->reservoirId != found->reservoirId)
					diagnostic = where + " circulation loop '" + loop->id + "' is coupled to reservoir '"
						+ loop->reservoirId + "', not the buffer's host reservoir '" + found->reservoirId + "'";
				return true;
			}
			return true;
		}
		if (mapping.ownerType == "Chiller")
		{
			auto const found = std::find_if(settings.chillers.begin(), settings.chillers.end(),
				[&](ChillerSettings const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.chillers.end()) return false;
			diagnostic = scopedChillerWiring(settings, *found);
			return true;
		}
		if (mapping.ownerType == "Coolant loop")
		{
			// Referenced loops preserve both chiller and collector interfaces,
			// whichever end of the processing line was wired first.
			auto const* host = machineField(settings, mapping.ownerType,
				mapping.ownerId, mapping.field);
			if (!host) return false;
			if (!host->empty() && !findReservoirById(settings, *host))
				diagnostic = "Station coolant loop '" + mapping.ownerId
					+ "' is coupled to unknown thermal reservoir '" + *host + "'";
			if (diagnostic.empty())
				for (auto const& chiller : settings.chillers)
					if (chiller.coldLoopId == mapping.ownerId)
					{
						diagnostic = scopedChillerWiring(settings, chiller);
						if (!diagnostic.empty()) break;
					}
			if (diagnostic.empty())
				for (auto const& collector : settings.waterCollectors)
					if (collector.coolingLoopId == mapping.ownerId
						&& !validateStationWaterGasWiring(settings, collector, diagnostic, true)) break;
			return true;
		}
		if (mapping.ownerType == "Radiator")
		{
			// #657: the radiator's host must be a declared reservoir. The
			// wording mirrors the whole-config validator's own entry rule, so
			// the same boundary reads the same from either seam. Many radiators
			// may share one host; there is no radiator thermal class, so no
			// class rule applies here.
			auto const found = std::find_if(settings.radiators.begin(), settings.radiators.end(),
				[&](RadiatorSettings const& entry) { return entry.id == mapping.ownerId; });
			if (found == settings.radiators.end()) return false;
			auto const where = "Station radiator '" + found->id + "'";
			if (found->reservoirId.empty()) return true;
			if (!findReservoirById(settings, found->reservoirId))
				diagnostic = where + " is attached to unknown thermal reservoir '"
					+ found->reservoirId + "'";
			return true;
		}
		if (mapping.ownerType != "Transfer") return false;
		auto const found = std::find_if(settings.transfers.begin(), settings.transfers.end(),
			[&](ResourceTransferSettings const& transfer) { return transfer.id == mapping.ownerId; });
		if (found == settings.transfers.end()) return false;
		std::string text;
		if (!validateStationTransferWiring(settings, *found, text, true)) diagnostic = std::move(text);
		return true;
	}

	// Keep existence, removal, collision checks and rename on the same complete
	// Authored-node dispatch. Derived service nodes own no configuration entry.
	template<class Settings, class F>
	bool withNodeEntries(Settings& settings, std::string_view type, F fn)
	{
		if (type == "Store") return fn(settings.stores);
		if (type == "Transfer") return fn(settings.transfers);
		if (type == "Conversion") return fn(settings.conversions);
		if (type == "Solar") return fn(settings.solarArrays);
		if (type == "Equipment") return fn(settings.equipmentLoads);
		if (type == "Battery") return fn(settings.batteries);
		if (type == "Delivery") return fn(settings.deliveries);
		if (type == "Waste ejection") return fn(settings.wasteEjections);
		if (type == "Outage") return fn(settings.outages);
		if (type == "Reservoir") return fn(settings.thermalReservoirs);
		if (type == "Radiator") return fn(settings.radiators);
		if (type == "Coolant loop") return fn(settings.coolantLoops);
		if (type == "Chiller") return fn(settings.chillers);
		if (type == "Generator") return fn(settings.generators);
		if (type == "Atmosphere") return fn(settings.atmospheres);
		if (type == "Airlock cycle") return fn(settings.airlockCycles);
		if (type == "Crew") return fn(settings.crews);
		if (type == "Water collector") return fn(settings.waterCollectors);
		if (type == "Water treatment") return fn(settings.waterTreatments);
		if (type == "Electrolyzer") return fn(settings.electrolyzers);
		if (type == "CO2 capture") return fn(settings.co2Captures);
		if (type == "Fuel cell") return fn(settings.fuelCells);
		if (type == "Sabatier") return fn(settings.sabatierReactors);
		if (type == "Methane processor") return fn(settings.methaneProcessors);
		if (type == "Microbial processor") return fn(settings.microbialProcessors);
		if (type == "Thermal buffer") return fn(settings.thermalBuffers);
		if (type == "Crops") return fn(settings.cropPopulations);
		if (type == "Algae") return fn(settings.algaeCultures);
		if (type == "Maintenance") return fn(settings.maintenanceJobs);
		return false;
	}
}

std::vector<AuthoredLink> stationAuthoredLinks(StationSystemsSettings const& settings)
{
	std::vector<AuthoredLink> links;
	visitStationBiologyReferences(settings, [&](auto type, auto const& id, auto port, auto field,
		auto const& reference, bool input, auto peerType, auto peerPort) {
		if (reference.empty()) return;
		StationPinRef owner{type, id, port}, peer{peerType, reference, peerPort};
		links.push_back({linkIdentity(type, id, field), input ? peer : owner, input ? owner : peer});
	});

	for (auto const& entry : settings.waterCollectors)
	{
		if (!entry.atmosphereId.empty())
			links.push_back({linkIdentity("Water collector", entry.id, "atmosphereId"),
				{"Atmosphere", entry.atmosphereId, "Gas out"}, {"Water collector", entry.id, "Atmosphere"}});
		if (!entry.coolingLoopId.empty())
			links.push_back({linkIdentity("Water collector", entry.id, "coolingLoopId"),
				{"Coolant loop", entry.coolingLoopId, "Cold surfaces"}, {"Water collector", entry.id, "Cooling loop"}});
		if (!entry.destStoreId.empty())
			links.push_back({linkIdentity("Water collector", entry.id, "destStoreId"),
				{"Water collector", entry.id, "Raw water out"}, {"Store", entry.destStoreId, "Inventory in"}});
	}
	for (auto const& entry : settings.waterTreatments)
	{
		if (!entry.sourceStoreId.empty())
			links.push_back({linkIdentity("Water treatment", entry.id, "sourceStoreId"),
				{"Store", entry.sourceStoreId, "Inventory out"}, {"Water treatment", entry.id, "Raw water in"}});
		if (!entry.treatedStoreId.empty())
			links.push_back({linkIdentity("Water treatment", entry.id, "treatedStoreId"),
				{"Water treatment", entry.id, "Treated water out"}, {"Store", entry.treatedStoreId, "Inventory in"}});
		if (!entry.concentrateStoreId.empty())
			links.push_back({linkIdentity("Water treatment", entry.id, "concentrateStoreId"),
				{"Water treatment", entry.id, "Concentrate out"}, {"Store", entry.concentrateStoreId, "Inventory in"}});
		if (!entry.consumableStoreId.empty())
			links.push_back({linkIdentity("Water treatment", entry.id, "consumableStoreId"),
				{"Store", entry.consumableStoreId, "Inventory out"}, {"Water treatment", entry.id, "Consumable in"}});
		if (!entry.clearanceStoreId.empty())
			links.push_back({linkIdentity("Water treatment", entry.id, "clearanceStoreId"),
				{"Store", entry.clearanceStoreId, "Inventory out"}, {"Water treatment", entry.id, "Clearance in"}});
	}
	for (auto const& entry : settings.electrolyzers)
	{
		if (!entry.waterStoreId.empty())
			links.push_back({linkIdentity("Electrolyzer", entry.id, "waterStoreId"),
				{"Store", entry.waterStoreId, "Inventory out"}, {"Electrolyzer", entry.id, "Water in"}});
		if (!entry.oxygenStoreId.empty())
			links.push_back({linkIdentity("Electrolyzer", entry.id, "oxygenStoreId"),
				{"Electrolyzer", entry.id, "Oxygen out"}, {"Store", entry.oxygenStoreId, "Inventory in"}});
		if (!entry.hydrogenStoreId.empty())
			links.push_back({linkIdentity("Electrolyzer", entry.id, "hydrogenStoreId"),
				{"Electrolyzer", entry.id, "Hydrogen out"}, {"Store", entry.hydrogenStoreId, "Inventory in"}});
		if (!entry.atmosphereId.empty())
			links.push_back({linkIdentity("Electrolyzer", entry.id, "atmosphereId"),
				{"Electrolyzer", entry.id, "Compartment oxygen"}, {"Atmosphere", entry.atmosphereId, "Oxygen in"}});
		if (!entry.heatSinkId.empty())
			links.push_back({linkIdentity("Electrolyzer", entry.id, "heatSinkId"),
				{"Electrolyzer", entry.id, "Heat out"}, {"Reservoir", entry.heatSinkId, "Heat in"}});
	}
	for (auto const& entry : settings.co2Captures)
	{
		if (!entry.atmosphereId.empty())
			links.push_back({linkIdentity("CO2 capture", entry.id, "atmosphereId"),
				{"Atmosphere", entry.atmosphereId, "Gas out"}, {"CO2 capture", entry.id, "Atmosphere"}});
		if (!entry.captureStoreId.empty())
			links.push_back({linkIdentity("CO2 capture", entry.id, "captureStoreId"),
				{"CO2 capture", entry.id, "Captured CO2 out"}, {"Store", entry.captureStoreId, "Inventory in"}});
		if (!entry.consumableStoreId.empty())
			links.push_back({linkIdentity("CO2 capture", entry.id, "consumableStoreId"),
				{"Store", entry.consumableStoreId, "Inventory out"}, {"CO2 capture", entry.id, "Consumable in"}});
	}
	for (auto const& entry : settings.atmospheres)
	{
		if (!entry.reservoirId.empty())
			links.push_back({linkIdentity("Atmosphere", entry.id, "reservoirId"),
				{"Atmosphere", entry.id, "Heat out"}, {"Reservoir", entry.reservoirId, "Heat in"}});
	}
	for (auto const& entry : settings.airlockCycles)
	{
		if (!entry.atmosphereId.empty())
			links.push_back({linkIdentity("Airlock cycle", entry.id, "atmosphereId"),
				{"Atmosphere", entry.atmosphereId, "Gas out"}, {"Airlock cycle", entry.id, "Atmosphere"}});
	}
	for (auto const& transfer : settings.transfers)
	{
		// An empty reference is an unconnected connector, not a link to an
		// empty store: the draft stays editable while the user wires it.
		if (!transfer.sourceId.empty())
			links.push_back({linkIdentity("Transfer", transfer.id, "Source"),
				{"Store", transfer.sourceId, "Inventory out"},
				{"Transfer", transfer.id, "Source"}});
		if (!transfer.destinationId.empty())
			links.push_back({linkIdentity("Transfer", transfer.id, "Destination"),
				{"Transfer", transfer.id, "Destination"},
				{"Store", transfer.destinationId, "Inventory in"}});
	}
	// Each wired conversion flow is one authored link, named by the same
	// 1-based slot the connector carries. An empty flow is an unconnected
	// connector, not a link to an empty store.
	for (auto const& conversion : settings.conversions)
	{
		for (size_t slot = 0; slot < conversion.inputs.size(); ++slot)
		{
			if (conversion.inputs[slot].storeId.empty()) continue;
			auto const suffix = std::to_string(slot + 1);
			links.push_back({linkIdentity("Conversion", conversion.id, "Input " + suffix),
				{"Store", conversion.inputs[slot].storeId, "Inventory out"},
				{"Conversion", conversion.id, "Conversion input " + suffix}});
		}
		for (size_t slot = 0; slot < conversion.outputs.size(); ++slot)
		{
			if (conversion.outputs[slot].storeId.empty()) continue;
			auto const suffix = std::to_string(slot + 1);
			links.push_back({linkIdentity("Conversion", conversion.id, "Output " + suffix),
				{"Conversion", conversion.id, "Conversion output " + suffix},
				{"Store", conversion.outputs[slot].storeId, "Inventory in"}});
		}
	}
	// The power and conversion chain (#647): every authored reference of
	// the six machine types is presented as the connector pair that writes
	// it. An equipment load's exchange slots are counted per face, so a
	// producing exchange never shifts the numbering of a consuming one.
	for (auto const& load : settings.equipmentLoads)
	{
		size_t inSlot = 0;
		size_t outSlot = 0;
		for (auto const& exchange : load.materialExchanges)
		{
			if (exchange.consumes)
			{
				++inSlot;
				if (exchange.storeId.empty()) continue;
				auto const suffix = std::to_string(inSlot);
				links.push_back({linkIdentity("Equipment", load.id, "Material in " + suffix),
					{"Store", exchange.storeId, "Inventory out"},
					{"Equipment", load.id, "Equipment input " + suffix}});
			}
			else
			{
				++outSlot;
				if (exchange.storeId.empty()) continue;
				auto const suffix = std::to_string(outSlot);
				links.push_back({linkIdentity("Equipment", load.id, "Material return " + suffix),
					{"Equipment", load.id, "Equipment return " + suffix},
					{"Store", exchange.storeId, "Inventory in"}});
			}
		}
		if (!load.heatSinkId.empty())
			links.push_back({linkIdentity("Equipment", load.id, "Heat sink"),
				{"Equipment", load.id, "Heat out"},
				{"Reservoir", load.heatSinkId, "Heat in"}});
	}
	for (auto const& generator : settings.generators)
	{
		if (!generator.fuelStoreId.empty())
			links.push_back({linkIdentity("Generator", generator.id, "Fuel store"),
				{"Store", generator.fuelStoreId, "Inventory out"},
				{"Generator", generator.id, "Fuel in"}});
		if (!generator.heatSinkId.empty())
			links.push_back({linkIdentity("Generator", generator.id, "Heat sink"),
				{"Generator", generator.id, "Heat out"},
				{"Reservoir", generator.heatSinkId, "Heat in"}});
	}
	for (auto const& cell : settings.fuelCells)
	{
		if (!cell.hydrogenStoreId.empty())
			links.push_back({linkIdentity("Fuel cell", cell.id, "Hydrogen store"),
				{"Store", cell.hydrogenStoreId, "Inventory out"},
				{"Fuel cell", cell.id, "Hydrogen in"}});
		if (!cell.oxygenStoreId.empty())
			links.push_back({linkIdentity("Fuel cell", cell.id, "Oxygen store"),
				{"Store", cell.oxygenStoreId, "Inventory out"},
				{"Fuel cell", cell.id, "Oxygen in"}});
		if (!cell.waterStoreId.empty())
			links.push_back({linkIdentity("Fuel cell", cell.id, "Water store"),
				{"Fuel cell", cell.id, "Water out"},
				{"Store", cell.waterStoreId, "Inventory in"}});
		if (!cell.heatSinkId.empty())
			links.push_back({linkIdentity("Fuel cell", cell.id, "Heat sink"),
				{"Fuel cell", cell.id, "Heat out"},
				{"Reservoir", cell.heatSinkId, "Heat in"}});
	}
	for (auto const& reactor : settings.sabatierReactors)
	{
		if (!reactor.co2StoreId.empty())
			links.push_back({linkIdentity("Sabatier", reactor.id, "CO2 store"),
				{"Store", reactor.co2StoreId, "Inventory out"},
				{"Sabatier", reactor.id, "CO2 in"}});
		if (!reactor.hydrogenStoreId.empty())
			links.push_back({linkIdentity("Sabatier", reactor.id, "Hydrogen store"),
				{"Store", reactor.hydrogenStoreId, "Inventory out"},
				{"Sabatier", reactor.id, "Hydrogen in"}});
		if (!reactor.methaneStoreId.empty())
			links.push_back({linkIdentity("Sabatier", reactor.id, "Methane store"),
				{"Sabatier", reactor.id, "Methane out"},
				{"Store", reactor.methaneStoreId, "Inventory in"}});
		if (!reactor.waterStoreId.empty())
			links.push_back({linkIdentity("Sabatier", reactor.id, "Water store"),
				{"Sabatier", reactor.id, "Water out"},
				{"Store", reactor.waterStoreId, "Inventory in"}});
		if (!reactor.heatSinkId.empty())
			links.push_back({linkIdentity("Sabatier", reactor.id, "Heat sink"),
				{"Sabatier", reactor.id, "Heat out"},
				{"Reservoir", reactor.heatSinkId, "Heat in"}});
	}
	for (auto const& processor : settings.methaneProcessors)
	{
		if (!processor.methaneStoreId.empty())
			links.push_back({linkIdentity("Methane processor", processor.id, "Methane store"),
				{"Store", processor.methaneStoreId, "Inventory out"},
				{"Methane processor", processor.id, "Methane in"}});
		if (!processor.hydrogenStoreId.empty())
			links.push_back({linkIdentity("Methane processor", processor.id, "Hydrogen store"),
				{"Methane processor", processor.id, "Hydrogen out"},
				{"Store", processor.hydrogenStoreId, "Inventory in"}});
		if (!processor.carbonStoreId.empty())
			links.push_back({linkIdentity("Methane processor", processor.id, "Carbon store"),
				{"Methane processor", processor.id, "Carbon out"},
				{"Store", processor.carbonStoreId, "Inventory in"}});
		if (!processor.heatSinkId.empty())
			links.push_back({linkIdentity("Methane processor", processor.id, "Heat sink"),
				{"Methane processor", processor.id, "Heat out"},
				{"Reservoir", processor.heatSinkId, "Heat in"}});
	}
	// #657: each radiator's host mount is one authored link, named by the
	// radiator that owns it. An empty reservoirId is an unconnected
	// connector, not a link to an empty reservoir. The link is enumerated
	// even when the named reservoir does not exist, so the inspector can
	// present a Disconnect for a broken host rather than hide it.
	for (auto const& radiator : settings.radiators)
	{
		if (radiator.reservoirId.empty()) continue;
		links.push_back({linkIdentity("Radiator", radiator.id, "Host reservoir"),
			{"Reservoir", radiator.reservoirId, "Hosted radiators"},
			{"Radiator", radiator.id, "Host reservoir"}});
	}
	for (auto const& loop : settings.coolantLoops)
	{
		if (loop.reservoirId.empty()) continue;
		links.push_back({linkIdentity("Coolant loop", loop.id, "Host reservoir"),
			{"Coolant loop", loop.id, "Heat exchanger"},
			{"Reservoir", loop.reservoirId, "Heat in"}});
	}
	for (auto const& buffer : settings.thermalBuffers)
	{
		if (!buffer.reservoirId.empty())
			links.push_back({linkIdentity("Thermal buffer", buffer.id, "Host reservoir"),
				{"Reservoir", buffer.reservoirId, "Hosted buffers"},
				{"Thermal buffer", buffer.id, "Host reservoir"}});
		if (!buffer.circulationLoopId.empty())
			links.push_back({linkIdentity("Thermal buffer", buffer.id, "Circulation gate"),
				{"Coolant loop", buffer.circulationLoopId, "Buffer gates"},
				{"Thermal buffer", buffer.id, "Circulation gate"}});
	}
	for (auto const& chiller : settings.chillers)
	{
		if (!chiller.coldLoopId.empty())
			links.push_back({linkIdentity("Chiller", chiller.id, "Cold loop"),
				{"Coolant loop", chiller.coldLoopId, "Cold interface"},
				{"Chiller", chiller.id, "Cold loop"}});
		if (!chiller.condenserReservoirId.empty())
			links.push_back({linkIdentity("Chiller", chiller.id, "Condenser reservoir"),
				{"Chiller", chiller.id, "Condenser heat"},
				{"Reservoir", chiller.condenserReservoirId, "Heat in"}});
	}
	visitStationScheduleReferences(settings, [&](auto type, auto const& id, auto port, auto field,
		auto const& reference, bool input, std::string_view peerType, auto peerPort) {
		if (reference.empty()) return;
		StationPinRef owner{type, id, std::string(port)};
		StationPinRef peer{peerType == "Installation" ? installationNodeType(settings, reference)
			: std::string(peerType), reference, peerPort};
		links.push_back({linkIdentity(type, id, std::string(field)), input ? peer : owner, input ? owner : peer});
	});
	return links;
}

bool applyStationLink(StationSystemsSettings& settings, CandidateLink const& link)
{
	LinkMapping mapping;
	if (!mapLink(link, mapping)) return false;
	auto write = [&](std::string& target) {
		if (target != mapping.peer.nodeId)
			eraseConnectionProperties(settings, mapping.ownerType, mapping.ownerId, mapping.field);
		target = mapping.peer.nodeId;
		return true;
	};
	if (mapping.ownerType == "Conversion")
	{
		auto* field = conversionFlowField(settings, mapping.ownerId, mapping.field);
		return field && write(*field);
	}
	if (isMachineOwnerType(mapping.ownerType))
	{
		auto* field = machineField(settings, mapping.ownerType, mapping.ownerId, mapping.field);
		return field && write(*field);
	}
	auto const found = std::find_if(settings.transfers.begin(), settings.transfers.end(),
		[&](ResourceTransferSettings const& transfer) { return transfer.id == mapping.ownerId; });
	if (found == settings.transfers.end()) return false;
	if (mapping.field == "Source") return write(found->sourceId);
	if (mapping.field == "Destination") return write(found->destinationId);
	return false;
}

bool clearStationLink(StationSystemsSettings& settings, std::string const& identity)
{
	auto const first = identity.find(':');
	auto const last = identity.rfind(':');
	if (first == std::string::npos || last == std::string::npos || last <= first) return false;
	auto const ownerType = identity.substr(0, first);
	auto const field = identity.substr(last + 1);
	auto const ownerId = identity.substr(first + 1, last - first - 1);
	auto clear = [&](std::string& target) {
		eraseConnectionProperties(settings, ownerType, ownerId, field);
		target.clear();
		return true;
	};
	if (ownerType == "Conversion")
	{
		auto* target = conversionFlowField(settings, ownerId, field);
		if (!target) return false;
		return clear(*target);
	}
	if (isMachineOwnerType(ownerType))
	{
		auto* target = machineField(settings, ownerType, ownerId, field);
		if (!target) return false;
		return clear(*target);
	}
	if (ownerType != "Transfer") return false;
	for (auto& transfer : settings.transfers)
	{
		if (transfer.id != ownerId) continue;
		if (field == "Source") return clear(transfer.sourceId);
		if (field == "Destination") return clear(transfer.destinationId);
		return false;
	}
	return false;
}

bool planStationNodeRemoval(StationSystemsSettings const& settings,
	std::string const& nodeType, std::string const& nodeId, StationNodeRemoval& plan)
{
	auto const exists = [&](auto const& entries)
	{
		return std::any_of(entries.begin(), entries.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
	};
	if (!withNodeEntries(settings, nodeType, exists)) return false;
	StationNodeRemoval candidate{nodeType, nodeId, {}};
	for (auto const& link : stationAuthoredLinks(settings))
		if ((link.from.nodeType == nodeType && link.from.nodeId == nodeId)
			|| (link.to.nodeType == nodeType && link.to.nodeId == nodeId))
			candidate.linkIdentities.push_back(link.identity);
	plan = std::move(candidate);
	return true;
}

bool removeStationNode(StationSystemsSettings& settings, StationNodeRemoval const& plan)
{
	StationNodeRemoval current;
	if (!planStationNodeRemoval(settings, plan.nodeType, plan.nodeId, current)
		|| current.linkIdentities != plan.linkIdentities) return false;
	// Preflight and apply on a copy: even a stale or malformed reference must
	// not leave a partially cleared configuration behind.
	auto candidate = settings;
	for (auto const& identity : current.linkIdentities)
		if (!clearStationLink(candidate, identity)) return false;
	if (!withNodeEntries(candidate, plan.nodeType, [&](auto& entries) {
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&](auto const& entry) { return entry.id == plan.nodeId; }), entries.end());
		return true;
	})) return false;
	std::erase_if(candidate.connections, [&](auto const& item) {
		return (item.first.ownerType == plan.nodeType && item.first.ownerId == plan.nodeId)
			|| (item.first.peerType == plan.nodeType && item.first.peerId == plan.nodeId);
	});
	settings = std::move(candidate);
	return true;
}

bool planStationNodeRename(StationSystemsSettings const& settings,
	std::string const& nodeType, std::string const& oldNodeId,
	std::string const& newNodeId, StationNodeRename& plan, std::string* diagnostic)
{
	auto refuse = [&](std::string message) {
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (newNodeId.empty()) return refuse("A node name cannot be empty.");
	if (newNodeId == oldNodeId) return refuse("Enter a different node name.");
	if (newNodeId.size() > MaxStationStoreIdLength)
		return refuse("A node name is too long.");
	StationNodeRemoval removal;
	if (!planStationNodeRemoval(settings, nodeType, oldNodeId, removal))
		return refuse("This node has no authored configuration entry to rename.");
	auto nameUsed = [&](auto const& entries) {
		return std::any_of(entries.begin(), entries.end(), [&](auto const& entry) {
			return entry.id == newNodeId && entry.id != oldNodeId;
		});
	};
	bool used = withNodeEntries(settings, nodeType, nameUsed);
	// Supervision targets share an installation namespace across these types.
	if (isInstallationType(nodeType))
		for (auto const& identity : stationInstallationIdentities(settings))
			if (identity.id == newNodeId)
				return refuse("An installation named '" + newNodeId + "' already exists.");
	if (used) return refuse("A " + nodeType + " named '" + newNodeId + "' already exists.");
	plan = {nodeType, oldNodeId, newNodeId, std::move(removal.linkIdentities)};
	if (diagnostic) diagnostic->clear();
	return true;
}

bool renameStationNode(StationSystemsSettings& settings, StationNodeRename const& plan)
{
	StationNodeRename current;
	if (!planStationNodeRename(settings, plan.nodeType, plan.oldNodeId, plan.newNodeId, current)
		|| current.linkIdentities != plan.linkIdentities) return false;
	// Preserve the complete links before clearing their mapped fields. Reapplying
	// them after changing the owner identity updates both incoming references and
	// references owned by the renamed node in one atomic candidate.
	auto const authored = stationAuthoredLinks(settings);
	std::vector<AuthoredLink> affected;
	for (auto const& identity : current.linkIdentities)
	{
		auto found = std::find_if(authored.begin(), authored.end(), [&](AuthoredLink const& link) {
			return link.identity == identity;
		});
		if (found == authored.end()) return false;
		affected.push_back(*found);
	}
	auto candidate = settings;
	for (auto const& link : affected)
		if (!clearStationLink(candidate, link.identity)) return false;
	auto rename = [&](auto& entries) {
		auto found = std::find_if(entries.begin(), entries.end(), [&](auto const& entry) {
			return entry.id == current.oldNodeId;
		});
		if (found == entries.end()) return false;
		found->id = current.newNodeId;
		return true;
	};
	if (!withNodeEntries(candidate, current.nodeType, rename)) return false;
	for (auto link : affected)
	{
		if (link.from.nodeType == current.nodeType && link.from.nodeId == current.oldNodeId)
			link.from.nodeId = current.newNodeId;
		if (link.to.nodeType == current.nodeType && link.to.nodeId == current.oldNodeId)
			link.to.nodeId = current.newNodeId;
		if (!applyStationLink(candidate, {link.from, link.to})) return false;
	}
	// Clearing/reapplying references is an implementation detail of rename,
	// not a disconnect: carry every design attribute through the operation.
	candidate.connections.clear();
	for (auto const& [oldKey, properties] : settings.connections)
	{
		auto key = oldKey;
		if (key.ownerType == current.nodeType && key.ownerId == current.oldNodeId)
			key.ownerId = current.newNodeId;
		if (key.peerType == current.nodeType && key.peerId == current.oldNodeId)
			key.peerId = current.newNodeId;
		candidate.connections.emplace(std::move(key), properties);
	}
	settings = std::move(candidate);
	return true;
}

LinkVerdict evaluateStationLink(StationSystemsSettings const& settings, CandidateLink const& link)
{
	// A derived service node owns no authored connector: refuse by name before
	// anything else, so the reason says "derived" rather than "unknown".
	if (stationIsDerivedNodeType(link.from.nodeType) || stationIsDerivedNodeType(link.to.nodeType))
	{
		auto const& derived = stationIsDerivedNodeType(link.from.nodeType) ? link.from : link.to;
		return refuse("'" + derived.nodeId + "' is a derived service node: it shows a service the "
			"configuration derives, has no authored connectors, and cannot be wired.");
	}
	// Structural layer: the descriptor table.
	auto const* from = findPort(link.from.nodeType, link.from.portName);
	auto const* to = findPort(link.to.nodeType, link.to.portName);
	if (!from) return refuse("Unknown connector '" + pinLabel(link.from) + "'");
	if (!to) return refuse("Unknown connector '" + pinLabel(link.to) + "'");
	// A drag may start at either end; the descriptors say which way round
	// the pair belongs, so the caller never has to orient it.
	CandidateLink oriented{link};
	if (from->direction == StationPortDirection::Input
		&& to->direction == StationPortDirection::Output)
	{
		std::swap(from, to);
		oriented = CandidateLink{link.to, link.from};
	}
	if (from->direction != StationPortDirection::Output || to->direction != StationPortDirection::Input)
		return refuse(pinLabel(oriented.from) + " and " + pinLabel(oriented.to)
			+ " cannot join: a link runs from an output connector to an input connector.");
	if (from->counterpartKind != to->kind || to->counterpartKind != from->kind)
	{
		if (!from->intermediary.empty())
			return refuse(std::string(from->nodeType) + " '" + oriented.from.nodeId + "' cannot feed "
				+ std::string(to->nodeType) + " '" + oriented.to.nodeId + "' directly; connect them through "
				+ std::string(from->intermediary) + ".");
		return refuse(pinLabel(oriented.from) + " cannot join " + pinLabel(oriented.to) + ": that input takes a '"
			+ std::string(to->counterpartKind) + "' output, not a '" + std::string(from->kind) + "' one.");
	}

	// Cardinality: the connector's declared ceiling, counted over the
	// references the draft already carries.
	auto const authored = stationAuthoredLinks(settings);
	for (auto const& existing : authored)
		if (existing.from == oriented.from && existing.to == oriented.to)
			return refuse(pinLabel(oriented.to) + " is already joined to " + pinLabel(oriented.from) + ".");
	if (to->maximumLinks != 0)
	{
		auto const peers = joinedPeers(authored, oriented.to, true);
		if (peers.size() >= to->maximumLinks) return refuse(occupiedReason(oriented.to, to->maximumLinks, peers));
	}
	if (from->maximumLinks != 0)
	{
		auto const peers = joinedPeers(authored, oriented.from, false);
		if (peers.size() >= from->maximumLinks)
			return refuse(occupiedReason(oriented.from, from->maximumLinks, peers));
	}

	// Semantic layer: the configuration validator is the sole authority. The
	// link is judged by the rule for the entry it writes, diffed against that
	// same rule before the link, so only a failure this link causes becomes
	// the rejection reason — an unrelated defect elsewhere in a half-built
	// draft never stands in as this link's reason.
	LinkMapping mapping;
	if (!mapLink(oriented, mapping))
		return refuse("No configuration reference stands for " + pinLabel(oriented.from) + " -> "
			+ pinLabel(oriented.to) + ".");
	auto candidate = settings;
	if (!applyStationLink(candidate, oriented))
		return refuse("Connector pair names no entry in the draft: " + linkIdentity(mapping) + ".");
	// In the scoped pass, dangling references are unwired draft connectors,
	// not failures of this drop. Clear them only in validation copies, before
	// applying the candidate: a new missing endpoint must still be refused.
	// This also lets first-failure validators reach new semantic violations
	// beyond a broken sibling (including dependants of a rewired loop).
	auto wiringBefore = settings;
	for (auto const& existing : stationAuthoredLinks(settings))
	{
		auto exists = [&](StationPinRef const& pin) {
			return withNodeEntries(settings, pin.nodeType, [&](auto const& entries) {
				return std::any_of(entries.begin(), entries.end(),
					[&](auto const& entry) { return entry.id == pin.nodeId; });
			});
		};
		if (!exists(existing.from) || !exists(existing.to))
			clearStationLink(wiringBefore, existing.identity);
	}
	auto wiringAfter = wiringBefore;
	applyStationLink(wiringAfter, oriented);
	std::string beforeWiring;
	std::string afterWiring;
	bool const scoped = scopedWiringRule(wiringAfter, mapping, afterWiring);
	if (scoped)
	{
		scopedWiringRule(wiringBefore, mapping, beforeWiring);
		if (!afterWiring.empty() && (afterWiring != beforeWiring
			|| isScheduleType(mapping.ownerType)
			|| mapping.ownerType == "Crew" || mapping.ownerType == "Crops"
			|| mapping.ownerType == "Algae" || mapping.ownerType == "Microbial processor"
			|| mapping.ownerType == "Chiller" || mapping.ownerType == "Coolant loop"
			|| mapping.ownerType == "Water collector" || mapping.ownerType == "Water treatment"
			|| mapping.ownerType == "Electrolyzer" || mapping.ownerType == "CO2 capture"
			|| mapping.ownerType == "Atmosphere" || mapping.ownerType == "Airlock cycle"))
			return refuse(afterWiring);
	}
	// The whole-configuration pass is the outer net: it says whether the link
	// changed the draft's standing at all.
	std::string before;
	std::string after;
	bool const afterValid = validateStationSystemsSettings(candidate, after);
	if (afterValid) return allow();
	bool const beforeValid = validateStationSystemsSettings(settings, before);
	if (beforeValid) return refuse(after);
	if (after == before) return allow();
	// The first-failure pass reports a new wording this link is the only
	// change behind. When the scoped rule already cleared the entry, that
	// wording is the counterpart endpoint the user has not wired yet — the
	// next drag, not a defect of this link. An owner type with no scoped rule
	// is refused rather than waved through.
	if (scoped) return allow();
	return refuse(after);
}

bool stationPortSetStatus(StationSystemsSettings const& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, StationPortSetStatus& status)
{
	if (nodeType == "Maintenance")
	{
		if (set != StationPortSet::MaintenanceRequirements) return false;
		auto const found = std::find_if(settings.maintenanceJobs.begin(), settings.maintenanceJobs.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
		if (found == settings.maintenanceJobs.end()) return false;
		status = {};
		status.count = found->requirements.size();
		status.maximum = MaxStationMaintenanceRequirements;
		status.canGrow = status.count < status.maximum;
		status.canShrink = status.count > status.minimum;
		if (!status.canGrow) status.growReason = "A maintenance job takes at most eight requirements.";
		if (!status.canShrink) status.shrinkReason = "A maintenance job needs at least one requirement.";
		return true;
	}
	if (nodeType == "Equipment")
	{
		if (set != StationPortSet::LoadExchangesIn && set != StationPortSet::LoadExchangesOut)
			return false;
		auto const found = std::find_if(settings.equipmentLoads.begin(),
			settings.equipmentLoads.end(),
			[&](EquipmentLoadSettings const& entry) { return entry.id == nodeId; });
		if (found == settings.equipmentLoads.end()) return false;
		size_t inCount = 0;
		for (auto const& exchange : found->materialExchanges)
			if (exchange.consumes) ++inCount;
		size_t const total = found->materialExchanges.size();
		bool const input = set == StationPortSet::LoadExchangesIn;
		status = {};
		status.count = input ? inCount : total - inCount;
		// A load may run with no material exchange at all: the floor is
		// zero, so the set can always be emptied. The ceiling is shared:
		// both faces together carry at most MaxStationLoadMaterialExchanges.
		status.minimum = 0;
		status.maximum = MaxStationLoadMaterialExchanges;
		status.canGrow = total < MaxStationLoadMaterialExchanges;
		status.canShrink = status.count > 0;
		if (!status.canGrow)
			status.growReason = "a load carries at most "
				+ std::to_string(MaxStationLoadMaterialExchanges)
				+ " material exchanges across both faces - the set is full.";
		return true;
	}
	if (nodeType != "Conversion") return false;
	auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& conversion) { return conversion.id == nodeId; });
	if (found == settings.conversions.end()) return false;
	size_t count = 0;
	if (set == StationPortSet::ConversionInputs) count = found->inputs.size();
	else count = found->outputs.size();
	status = {};
	status.count = count;
	status.minimum = 1;
	status.maximum = MaxStationConversionFlows;
	status.canGrow = status.count < status.maximum;
	status.canShrink = status.count > status.minimum;
	auto const kind = set == StationPortSet::ConversionInputs ? "input" : "output";
	if (!status.canGrow)
		status.growReason = "a conversion takes at most " + std::to_string(status.maximum)
			+ " " + kind + "s - the set is full.";
	if (!status.canShrink)
		status.shrinkReason = std::string("a conversion needs at least ")
			+ std::to_string(status.minimum) + " " + kind + " - removing it would leave a shape the validator rejects.";
	return true;
}

bool growStationPortSet(StationSystemsSettings& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, std::string& reason)
{
	reason.clear();
	StationPortSetStatus status;
	if (!stationPortSetStatus(settings, nodeType, nodeId, set, status))
	{
		reason = "'" + nodeId + "' has no growable "
			+ (stationPortSetIsInputSide(set) ? "input" : "output") + " set.";
		return false;
	}
	if (!status.canGrow)
	{
		reason = status.growReason;
		return false;
	}
	if (nodeType == "Maintenance")
	{
		auto& job = *std::find_if(settings.maintenanceJobs.begin(), settings.maintenanceJobs.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
		job.requirements.emplace_back();
		return true;
	}
	if (nodeType == "Equipment")
	{
		auto& load = *std::find_if(settings.equipmentLoads.begin(),
			settings.equipmentLoads.end(),
			[&](EquipmentLoadSettings const& entry) { return entry.id == nodeId; });
		// The new connector is a real exchange: empty until the user wires
		// it, with the rate left for the entry's own fields. The face the
		// affordance sits on decides the direction of the exchange.
		EquipmentMaterialExchangeSettings exchange;
		exchange.consumes = set == StationPortSet::LoadExchangesIn;
		load.materialExchanges.push_back(exchange);
		return true;
	}
	auto& conversion = *std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& entry) { return entry.id == nodeId; });
	// The new connector is a real flow: empty until the user wires it, with
	// the rate left for the entry's own fields.
	if (set == StationPortSet::ConversionInputs) conversion.inputs.emplace_back();
	else conversion.outputs.emplace_back();
	return true;
}

bool shrinkStationPortSet(StationSystemsSettings& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, std::string& reason)
{
	reason.clear();
	StationPortSetStatus status;
	if (!stationPortSetStatus(settings, nodeType, nodeId, set, status))
	{
		reason = "'" + nodeId + "' has no shrinkable "
			+ (stationPortSetIsInputSide(set) ? "input" : "output") + " set.";
		return false;
	}
	if (!status.canShrink)
	{
		reason = status.shrinkReason;
		return false;
	}
	// Drop the removed face's ratings even if it was already unwired.
	auto const reference = nodeType == "Maintenance" ? "Requirement "
		: nodeType == "Equipment" ? (set == StationPortSet::LoadExchangesIn ? "Material in " : "Material return ")
		: (set == StationPortSet::ConversionInputs ? "Input " : "Output ");
	eraseConnectionProperties(settings, nodeType, nodeId, reference + std::to_string(status.count));
	if (nodeType == "Maintenance")
	{
		auto& job = *std::find_if(settings.maintenanceJobs.begin(), settings.maintenanceJobs.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
		job.requirements.pop_back();
		return true;
	}
	if (nodeType == "Equipment")
	{
		auto& load = *std::find_if(settings.equipmentLoads.begin(),
			settings.equipmentLoads.end(),
			[&](EquipmentLoadSettings const& entry) { return entry.id == nodeId; });
		// The newest connector on this face goes, wired or not: its
		// reference leaves with it, as one undoable change owned by the
		// caller's history.
		bool const input = set == StationPortSet::LoadExchangesIn;
		for (auto it = load.materialExchanges.rbegin(); it != load.materialExchanges.rend(); ++it)
			if (it->consumes == input)
			{
				load.materialExchanges.erase(std::next(it).base());
				break;
			}
		return true;
	}
	auto& conversion = *std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& entry) { return entry.id == nodeId; });
	// The newest connector goes, wired or not: its reference leaves with it,
	// as one undoable change owned by the caller's history.
	if (set == StationPortSet::ConversionInputs) conversion.inputs.pop_back();
	else conversion.outputs.pop_back();
	return true;
}
}
