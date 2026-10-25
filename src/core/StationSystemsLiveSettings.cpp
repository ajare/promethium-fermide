// Live Station settings mutation under a frozen topology.
//
// The runtime is built from a StationSystemsSettings taken by value, and until
// now the only way to change anything was configureStationSystems, which
// resets the whole World. This file adds the narrower operation: replace the
// authored *parameters* of a running scenario without disturbing the live
// accumulated state, so a running or paused simulation can react to a
// configuration change.
//
// The contract is deliberately narrow, and enforced rather than assumed:
//
//   * Topology is frozen. The entity id set of every collection, each
//     atmosphere pathway's endpoints, and the connection map keys must match
//     the running configuration exactly. Nothing may be added or removed, and
//     no opening may be re-pointed at a different compartment.
//   * Parameters go live: areas, capacities, rates, limits, thermal
//     properties and priorities take effect from the next step.
//   * Live state is preserved: inventories, temperatures, current openness,
//     net totals, fired flags and trend history are left alone.
//   * Initial-condition fields are NOT applied. See preserveInitial below.
//
// Because every priority ordering lives in its own index vector rather than in
// the physical order of the state vectors, a live priority change is picked up
// by re-sorting those index vectors and invalidates nothing.
#include "StationSystemsImpl.h"
#include "core/StationSystemsRuntime.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>

namespace core
{
	namespace
	{
		// Initial-condition fields are never pushed live. The conservation
		// audit derives its baseline from the authored starting values at
		// snapshot time - reservoirResult.startingEnergyKJ is
		// heatCapacityKJPerK * startingTemperatureC, and residualKJ is then
		// audited against that - so mutating a starting value mid-run would
		// shift the baseline retroactively and report a false conservation
		// violation, or mask a real one. A starting value describes how the
		// run began, not how it should now be; changing one is a reset.
		//
		// Returns true when the caller actually touched a preserved field, so
		// a targeted edit can report the refusal instead of dropping it.
		template <class T, class... Fields>
		bool restoreInitial(T const& running, T& offered, Fields... fields)
		{
			bool touched = false;
			auto one = [&](auto field) {
				if ((offered.*field) != (running.*field))
				{
					offered.*field = running.*field;
					touched = true;
				}
			};
			(one(fields), ...);
			return touched;
		}

		template <class T, class... Fields>
		void preserveInitial(T const& running, T& offered, Fields... fields)
		{
			restoreInitial(running, offered, fields...);
		}

		// Mirror the live authored state back into the runtime's own settings
		// copy, so anything reading settings directly sees the same values the
		// tick does - including the preserved initial conditions.
		template <class State, class Settings>
		void syncAuthored(std::vector<Settings>& into, std::vector<State> const& from)
		{
			into.clear();
			into.reserve(from.size());
			for (auto const& entry : from) into.push_back(entry.authored);
		}

		// Every priority ordering uses this same predicate, so one helper
		// refreshes all of them.
		template <class State>
		void resortPriority(std::vector<size_t>& order, std::vector<State>& states)
		{
			order.resize(states.size());
			std::iota(order.begin(), order.end(), size_t{0});
			std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right)
				{ return states[left].authored.priority > states[right].authored.priority; });
		}

		// Replace one collection's authored settings, matched by id. The
		// running and offered id sets must be identical, which the equal size
		// plus the per-entry lookup enforces given unique ids.
		template <class State, class Settings, class Preserve>
		bool rebindById(std::vector<State>& running,
			std::vector<Settings> const& offered, std::string_view kind,
			Preserve&& preserve, std::string* diagnostic)
		{
			if (running.size() != offered.size())
			{
				if (diagnostic)
					*diagnostic = std::string(kind)
						+ ": a live update cannot add or remove entries ("
						+ std::to_string(running.size()) + " running, "
						+ std::to_string(offered.size()) + " offered)";
				return false;
			}
			for (auto& entry : running)
			{
				auto const found = std::find_if(offered.begin(), offered.end(),
					[&](Settings const& next) { return next.id == entry.authored.id; });
				if (found == offered.end())
				{
					if (diagnostic)
						*diagnostic = std::string(kind) + ": running entry '"
							+ entry.authored.id + "' is missing from the offered settings";
					return false;
				}
				Settings fresh = *found;
				preserve(entry.authored, fresh);
				entry.authored = std::move(fresh);
			}
			return true;
		}

		// A collection carrying no initial-condition fields.
		template <class State, class Settings>
		bool rebindById(std::vector<State>& running,
			std::vector<Settings> const& offered, std::string_view kind,
			std::string* diagnostic)
		{
			return rebindById(running, offered, kind,
				[](Settings const&, Settings&) {}, diagnostic);
		}
	}

	bool StationSystemsRuntime::updateSettings(
		StationSystemsSettings const& offered, std::string* diagnostic)
	{
		auto& s = *mImpl;
		for (auto const& next : offered.coolantLoops)
		{
			auto const old = std::find_if(s.coolantLoops.begin(), s.coolantLoops.end(),
				[&](auto const& loop) { return loop.authored.id == next.id; });
			if (old != s.coolantLoops.end() && LiveNode<CoolantLoopSettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Coolant loop '" + next.id + "': fault opt-in and endpoints are frozen topology";
				return false;
			}
		}
		// #844: the thermal-plant fault opt-ins - compressor failure,
		// refrigerant leak, panel puncture, mechanical seizure - and the
		// repair wiring are run-shape decisions, not live knobs:
		// preflight the whole collection before anything can land, so a
		// mixed offer is atomic.
		for (auto const& next : offered.chillers)
		{
			auto const old = std::find_if(s.chillers.begin(), s.chillers.end(),
				[&](auto const& chiller) { return chiller.authored.id == next.id; });
			if (old != s.chillers.end() && LiveNode<ChillerSettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Chiller '" + next.id + "': fault opt-in, leak destination and repair wiring are frozen topology";
				return false;
			}
		}
		for (auto const& next : offered.radiators)
		{
			auto const old = std::find_if(s.radiators.begin(), s.radiators.end(),
				[&](auto const& radiator) { return radiator.authored.id == next.id; });
			if (old != s.radiators.end() && LiveNode<RadiatorSettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Radiator '" + next.id + "': puncture opt-in, destination and repair wiring are frozen topology";
				return false;
			}
		}
		// #843: whether a battery bank has a capacity-fade model is a
		// run-shape decision, not a live knob: preflight the whole
		// collection before anything can land, so a mixed offer is atomic.
		for (auto const& next : offered.batteries)
		{
			auto const old = std::find_if(s.batteries.begin(), s.batteries.end(),
				[&](auto const& battery) { return battery.authored.id == next.id; });
			if (old != s.batteries.end() && LiveNode<BatterySettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Battery '" + next.id + "': capacity-fade opt-in, thermal-runaway opt-in and destination are frozen topology";
				return false;
			}
		}
		// #845: the power-source fault opt-ins are run-shape decisions
		// like every earlier batch: which modes a machine can suffer
		// cannot move under a live run. The solar fractions inside the
		// blocks stay live-tunable parameters that never reach a Fault
		// already injected.
		for (auto const& next : offered.generators)
		{
			auto const old = std::find_if(s.generators.begin(), s.generators.end(),
				[&](auto const& generator) { return generator.authored.id == next.id; });
			if (old != s.generators.end() && LiveNode<GeneratorSettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Generator '" + next.id + "': failed-start opt-in is frozen topology";
				return false;
			}
		}
		for (auto const& next : offered.solarArrays)
		{
			auto const old = std::find_if(s.solarArrays.begin(), s.solarArrays.end(),
				[&](auto const& array) { return array.authored.id == next.id; });
			if (old != s.solarArrays.end() && LiveNode<SolarArraySettings>::topologyChanged(old->authored, next))
			{
				if (diagnostic) *diagnostic = "Solar '" + next.id + "': string-loss and tracking-failure opt-ins are frozen topology";
				return false;
			}
		}

		// An opening is topology: which two compartments it joins cannot move
		// under a running simulation, because the live pressure state and the
		// resolved compartment indices both belong to the original pairing.
		for (auto const& next : offered.atmospherePathways)
		{
			auto const found = std::find_if(s.atmospherePathways.begin(),
				s.atmospherePathways.end(),
				[&](auto const& pathway) { return pathway.authored.id == next.id; });
			if (found == s.atmospherePathways.end()) continue;
			if (found->authored.aAtmosphereId != next.aAtmosphereId
				|| found->authored.bAtmosphereId != next.bAtmosphereId)
			{
				if (diagnostic)
					*diagnostic = "atmosphere pathway '" + next.id
						+ "' endpoints are topology and cannot be rewired live";
				return false;
			}
		}

		// A Thermal link likewise resolves its endpoints once at construction.
		// Its conductance is a live thermal parameter, but its identity and
		// endpoints are frozen topology. Check the complete collection before
		// mutating any other live authored state, so a mixed offer is atomic.
		if (s.thermalLinks.size() != offered.thermalLinks.size())
		{
			if (diagnostic)
				*diagnostic = "thermal links: a live update cannot add or remove entries ("
					+ std::to_string(s.thermalLinks.size()) + " running, "
					+ std::to_string(offered.thermalLinks.size()) + " offered)";
			return false;
		}
		for (auto const& link : s.thermalLinks)
		{
			auto const found = std::find_if(offered.thermalLinks.begin(),
				offered.thermalLinks.end(), [&](ThermalLinkSettings const& next)
				{ return next.id == link.authored.id; });
			if (found == offered.thermalLinks.end())
			{
				if (diagnostic)
					*diagnostic = "thermal link: running entry '" + link.authored.id
						+ "' is missing from the offered settings";
				return false;
			}
			if (link.authored.aAtmosphereId != found->aAtmosphereId
				|| link.authored.bAtmosphereId != found->bAtmosphereId)
			{
				if (diagnostic)
					*diagnostic = "thermal link '" + link.authored.id
						+ "' endpoints are topology and cannot be rewired live";
				return false;
			}
		}

		// Engine reservoir indices and installation bindings are resolved once.
		// Preflight the entire collection before any other parameter can land.
		if (s.heatEngines.size() != offered.heatEngines.size())
		{
			if (diagnostic) *diagnostic = "heat engines: a live update cannot add or remove entries";
			return false;
		}
		for (auto const& engine : s.heatEngines)
		{
			auto const found = std::find_if(offered.heatEngines.begin(), offered.heatEngines.end(),
				[&](auto const& next) { return next.id == engine.authored.id; });
			if (found == offered.heatEngines.end()
				|| LiveNode<HeatEngineSettings>::topologyChanged(engine.authored, *found))
			{
				if (diagnostic) *diagnostic = "heat engine '" + engine.authored.id
					+ "': identity and endpoints are topology and cannot be changed live";
				return false;
			}
		}

		// The connection map keys are the wiring graph. Properties on a key
		// are parameters and may change; the key set may not.
		if (s.settings.connections.size() != offered.connections.size())
		{
			if (diagnostic)
				*diagnostic = "a live update cannot add or remove connections ("
					+ std::to_string(s.settings.connections.size()) + " running, "
					+ std::to_string(offered.connections.size()) + " offered)";
			return false;
		}
		for (auto const& [key, properties] : offered.connections)
		{
			if (!s.settings.connections.contains(key))
			{
				if (diagnostic)
					*diagnostic = "connection '" + key.ownerType + "/" + key.ownerId
						+ "/" + key.reference + "' is not wired in the running scenario";
				return false;
			}
		}

		// Reject the whole update before touching anything, so a partial
		// application can never leave the scenario half-changed.
		if (!rebindById(s.stores, offered.stores, "stores",
			[](ResourceStoreSettings const& running, ResourceStoreSettings& fresh)
			{
				preserveInitial(running, fresh,
					&ResourceStoreSettings::startingQualified,
					&ResourceStoreSettings::startingUnqualified);
			}, diagnostic)) return false;
		if (!rebindById(s.transfers, offered.transfers, "transfers", diagnostic)) return false;
		if (!rebindById(s.conversions, offered.conversions, "conversions", diagnostic)) return false;
		if (!rebindById(s.solarArrays, offered.solarArrays, "solar arrays", diagnostic)) return false;
		if (!rebindById(s.loads, offered.equipmentLoads, "equipment loads", diagnostic)) return false;
		if (!rebindById(s.batteries, offered.batteries, "batteries",
			[](BatterySettings const& running, BatterySettings& fresh)
			{
				preserveInitial(running, fresh, &BatterySettings::startingStoredKWh);
			}, diagnostic)) return false;
		if (!rebindById(s.deliveries, offered.deliveries, "deliveries", diagnostic)) return false;
		if (!rebindById(s.wasteEjections, offered.wasteEjections, "waste ejections", diagnostic)) return false;
		if (!rebindById(s.outages, offered.outages, "outages", diagnostic)) return false;
		if (!rebindById(s.reservoirs, offered.thermalReservoirs, "thermal reservoirs",
			[](ThermalReservoirSettings const& running, ThermalReservoirSettings& fresh)
			{
				preserveInitial(running, fresh,
					&ThermalReservoirSettings::startingTemperatureC);
			}, diagnostic)) return false;
		if (!rebindById(s.radiators, offered.radiators, "radiators", diagnostic)) return false;
		if (!rebindById(s.coolantLoops, offered.coolantLoops, "coolant loops",
			[](CoolantLoopSettings const& running, CoolantLoopSettings& fresh)
			{
				preserveInitial(running, fresh,
					&CoolantLoopSettings::startingCoolantKg,
					&CoolantLoopSettings::startingTemperatureC);
			}, diagnostic)) return false;
		if (!rebindById(s.chillers, offered.chillers, "chillers", diagnostic)) return false;
		if (!rebindById(s.heatEngines, offered.heatEngines, "heat engines", diagnostic)) return false;
		if (!rebindById(s.generators, offered.generators, "generators", diagnostic)) return false;
		if (!rebindById(s.atmospheres, offered.atmospheres, "atmospheres",
			[](AtmosphereSettings const& running, AtmosphereSettings& fresh)
			{
				// #802: advectedSpecificHeatKJPerKgK is a thermal
				// parameter under this contract - a live change takes effect
				// at the next step, like every non-starting field. The
				// accumulated advectedKJ totals are net totals and are
				// left alone: a live specific-heat edit re-decides what
				// future venting and inflow carry, it never rewrites what
				// already moved.
				preserveInitial(running, fresh,
					&AtmosphereSettings::startingCarbonDioxideKg,
					&AtmosphereSettings::startingNitrogenKg,
					&AtmosphereSettings::startingOxygenKg,
					&AtmosphereSettings::startingTemperatureC,
					&AtmosphereSettings::startingWaterVapourKg);
			}, diagnostic)) return false;
		if (!rebindById(s.atmospherePathways, offered.atmospherePathways,
			"atmosphere pathways", diagnostic)) return false;
		if (!rebindById(s.thermalLinks, offered.thermalLinks,
			"thermal links", diagnostic)) return false;
		if (!rebindById(s.airlockCycles, offered.airlockCycles, "airlock cycles", diagnostic)) return false;
		if (!rebindById(s.crews, offered.crews, "crews", diagnostic)) return false;
		if (!rebindById(s.waterCollectors, offered.waterCollectors, "water collectors", diagnostic)) return false;
		if (!rebindById(s.waterTreatments, offered.waterTreatments, "water treatments", diagnostic)) return false;
		if (!rebindById(s.electrolyzers, offered.electrolyzers, "electrolyzers", diagnostic)) return false;
		if (!rebindById(s.co2Captures, offered.co2Captures, "CO2 captures", diagnostic)) return false;
		if (!rebindById(s.fuelCells, offered.fuelCells, "fuel cells", diagnostic)) return false;
		if (!rebindById(s.sabatierReactors, offered.sabatierReactors, "Sabatier reactors", diagnostic)) return false;
		if (!rebindById(s.methaneProcessors, offered.methaneProcessors, "methane processors", diagnostic)) return false;
		if (!rebindById(s.microbialProcessors, offered.microbialProcessors,
			"microbial processors",
			[](MicrobialProcessorSettings const& running, MicrobialProcessorSettings& fresh)
			{
				preserveInitial(running, fresh,
					&MicrobialProcessorSettings::initialCondition);
			}, diagnostic)) return false;
		if (!rebindById(s.thermalBuffers, offered.thermalBuffers, "thermal buffers",
			[](ThermalBufferSettings const& running, ThermalBufferSettings& fresh)
			{
				preserveInitial(running, fresh,
					&ThermalBufferSettings::startingEnergyKJ);
			}, diagnostic)) return false;
		if (!rebindById(s.cropPopulations, offered.cropPopulations, "crop populations",
			[](CropPopulationSettings const& running, CropPopulationSettings& fresh)
			{
				preserveInitial(running, fresh,
					&CropPopulationSettings::initialBiomassKg);
			}, diagnostic)) return false;
		if (!rebindById(s.algaeCultures, offered.algaeCultures, "algae cultures",
			[](AlgaeCultureSettings const& running, AlgaeCultureSettings& fresh)
			{
				preserveInitial(running, fresh,
					&AlgaeCultureSettings::initialBiomassKg);
			}, diagnostic)) return false;
		if (!rebindById(s.maintenanceJobs, offered.maintenanceJobs, "maintenance jobs", diagnostic)) return false;

		// Connection properties are parameters; the key set was checked above.
		for (auto const& [key, properties] : offered.connections)
			s.settings.connections[key] = properties;

		// Priorities may have moved. Re-sort the index vectors only; the state
		// vectors keep their declaration order, so stored indices hold.
		resortPriority(s.transferOrder, s.transfers);
		resortPriority(s.conversionOrder, s.conversions);
		resortPriority(s.loadOrder, s.loads);
		resortPriority(s.electrolyzerOrder, s.electrolyzers);
		resortPriority(s.co2CaptureOrder, s.co2Captures);
		resortPriority(s.sabatierOrder, s.sabatierReactors);
		resortPriority(s.methaneProcessorOrder, s.methaneProcessors);
		resortPriority(s.microbialProcessorOrder, s.microbialProcessors);
		resortPriority(s.cropOrder, s.cropPopulations);
		resortPriority(s.algaeOrder, s.algaeCultures);
		resortPriority(s.waterTreatmentOrder, s.waterTreatments);
		resortPriority(s.maintenanceOrder, s.maintenanceJobs);

		// Keep the runtime's own settings copy in step with the live authored
		// state, so a later snapshot or audit reads what the tick is using.
		s.settings = offered;
		syncAuthored(s.settings.stores, s.stores);
		syncAuthored(s.settings.transfers, s.transfers);
		syncAuthored(s.settings.conversions, s.conversions);
		syncAuthored(s.settings.solarArrays, s.solarArrays);
		syncAuthored(s.settings.equipmentLoads, s.loads);
		syncAuthored(s.settings.batteries, s.batteries);
		syncAuthored(s.settings.deliveries, s.deliveries);
		syncAuthored(s.settings.wasteEjections, s.wasteEjections);
		syncAuthored(s.settings.outages, s.outages);
		syncAuthored(s.settings.thermalReservoirs, s.reservoirs);
		syncAuthored(s.settings.radiators, s.radiators);
		syncAuthored(s.settings.coolantLoops, s.coolantLoops);
		syncAuthored(s.settings.chillers, s.chillers);
		syncAuthored(s.settings.heatEngines, s.heatEngines);
		syncAuthored(s.settings.generators, s.generators);
		syncAuthored(s.settings.atmospheres, s.atmospheres);
		syncAuthored(s.settings.atmospherePathways, s.atmospherePathways);
		syncAuthored(s.settings.thermalLinks, s.thermalLinks);
		syncAuthored(s.settings.airlockCycles, s.airlockCycles);
		syncAuthored(s.settings.crews, s.crews);
		syncAuthored(s.settings.waterCollectors, s.waterCollectors);
		syncAuthored(s.settings.waterTreatments, s.waterTreatments);
		syncAuthored(s.settings.electrolyzers, s.electrolyzers);
		syncAuthored(s.settings.co2Captures, s.co2Captures);
		syncAuthored(s.settings.fuelCells, s.fuelCells);
		syncAuthored(s.settings.sabatierReactors, s.sabatierReactors);
		syncAuthored(s.settings.methaneProcessors, s.methaneProcessors);
		syncAuthored(s.settings.microbialProcessors, s.microbialProcessors);
		syncAuthored(s.settings.thermalBuffers, s.thermalBuffers);
		syncAuthored(s.settings.cropPopulations, s.cropPopulations);
		syncAuthored(s.settings.algaeCultures, s.algaeCultures);
		syncAuthored(s.settings.maintenanceJobs, s.maintenanceJobs);
		s.recordFaultSettings();
		return true;
	}

	StationSystemsSettings const& StationSystemsRuntime::effectiveSettings() const
	{
		return mImpl->settings;
	}

	namespace
	{
		// One scan of the one collection a settings type owns. Deliberately
		// free of any Impl reference so it can sit outside the member
		// definitions; the caller passes the vector in.
		template <class State>
		auto findLiveAuthored(std::vector<State>& states, std::string_view nodeId, size_t* index)
			-> decltype(&states.front().authored)
		{
			if (states.empty()) return nullptr;
			auto const found = std::find_if(states.begin(), states.end(),
				[&](State const& entry) { return entry.authored.id == nodeId; });
			if (found == states.end()) return nullptr;
			if (index) *index = static_cast<size_t>(std::distance(states.begin(), found));
			return &found->authored;
		}

		// One line per live-editable type: settings type -> live state vector.
#define PF_LIVE_ENTRY(SETTINGS, MEMBER) \
	if constexpr (std::is_same_v<Settings, SETTINGS>) \
		return findLiveAuthored(mImpl->MEMBER, nodeId, index); \
	else
		// settings type -> the priority ordering it participates in, if any.
#define PF_LIVE_RESORT(SETTINGS, ORDER, STATES) \
	if constexpr (std::is_same_v<Settings, SETTINGS>) \
		resortPriority(mImpl->ORDER, mImpl->STATES); \
	else
	}

	template <class Settings>
	Settings* StationSystemsRuntime::liveAuthoredEntry(std::string_view nodeId, size_t* index)
	{
		PF_LIVE_ENTRY(AtmospherePathwaySettings, atmospherePathways)
		PF_LIVE_ENTRY(ResourceStoreSettings, stores)
		PF_LIVE_ENTRY(ResourceTransferSettings, transfers)
		PF_LIVE_ENTRY(ResourceConversionSettings, conversions)
		PF_LIVE_ENTRY(SolarArraySettings, solarArrays)
		PF_LIVE_ENTRY(EquipmentLoadSettings, loads)
		PF_LIVE_ENTRY(BatterySettings, batteries)
		PF_LIVE_ENTRY(ResourceDeliverySettings, deliveries)
		PF_LIVE_ENTRY(WasteEjectionSettings, wasteEjections)
		PF_LIVE_ENTRY(InstallationOutageSettings, outages)
		PF_LIVE_ENTRY(ThermalReservoirSettings, reservoirs)
		PF_LIVE_ENTRY(RadiatorSettings, radiators)
		PF_LIVE_ENTRY(CoolantLoopSettings, coolantLoops)
		PF_LIVE_ENTRY(ChillerSettings, chillers)
		PF_LIVE_ENTRY(HeatEngineSettings, heatEngines)
		PF_LIVE_ENTRY(GeneratorSettings, generators)
		PF_LIVE_ENTRY(AtmosphereSettings, atmospheres)
		PF_LIVE_ENTRY(ThermalLinkSettings, thermalLinks)
		PF_LIVE_ENTRY(AirlockCycleSettings, airlockCycles)
		PF_LIVE_ENTRY(CrewSettings, crews)
		PF_LIVE_ENTRY(WaterCollectorSettings, waterCollectors)
		PF_LIVE_ENTRY(WaterTreatmentSettings, waterTreatments)
		PF_LIVE_ENTRY(ElectrolyzerSettings, electrolyzers)
		PF_LIVE_ENTRY(Co2CaptureSettings, co2Captures)
		PF_LIVE_ENTRY(FuelCellSettings, fuelCells)
		PF_LIVE_ENTRY(SabatierReactorSettings, sabatierReactors)
		PF_LIVE_ENTRY(MethaneProcessorSettings, methaneProcessors)
		PF_LIVE_ENTRY(MicrobialProcessorSettings, microbialProcessors)
		PF_LIVE_ENTRY(ThermalBufferSettings, thermalBuffers)
		PF_LIVE_ENTRY(CropPopulationSettings, cropPopulations)
		PF_LIVE_ENTRY(AlgaeCultureSettings, algaeCultures)
		PF_LIVE_ENTRY(MaintenanceJobSettings, maintenanceJobs)
		{
			static_assert(sizeof(Settings) == 0,
				"settings type has no live-editable collection; add it to PF_LIVE_ENTRY");
			return nullptr;
		}
	}

	template <class Settings>
	void StationSystemsRuntime::syncLiveNodeMirror(size_t index, Settings const& value)
	{
		// The settings copy is kept index-parallel with the live states, so the
		// changed entry lands at the same position. Only this one collection
		// is touched.
		auto& mirror = LiveNode<Settings>::of(mImpl->settings);
		if (index < mirror.size()) mirror[index] = value;
		mImpl->recordFaultSettings();
	}

	template <class Settings>
	void StationSystemsRuntime::resortLiveNode()
	{
		PF_LIVE_RESORT(ResourceTransferSettings, transferOrder, transfers)
		PF_LIVE_RESORT(ResourceConversionSettings, conversionOrder, conversions)
		PF_LIVE_RESORT(EquipmentLoadSettings, loadOrder, loads)
		PF_LIVE_RESORT(WaterTreatmentSettings, waterTreatmentOrder, waterTreatments)
		PF_LIVE_RESORT(ElectrolyzerSettings, electrolyzerOrder, electrolyzers)
		PF_LIVE_RESORT(Co2CaptureSettings, co2CaptureOrder, co2Captures)
		PF_LIVE_RESORT(SabatierReactorSettings, sabatierOrder, sabatierReactors)
		PF_LIVE_RESORT(MethaneProcessorSettings, methaneProcessorOrder, methaneProcessors)
		PF_LIVE_RESORT(MicrobialProcessorSettings, microbialProcessorOrder, microbialProcessors)
		PF_LIVE_RESORT(CropPopulationSettings, cropOrder, cropPopulations)
		PF_LIVE_RESORT(AlgaeCultureSettings, algaeOrder, algaeCultures)
		PF_LIVE_RESORT(MaintenanceJobSettings, maintenanceOrder, maintenanceJobs)
		{}
	}

	// Explicit instantiations: the header template calls these, and the
	// mutator lambda never crosses into this translation unit.
#define PF_LIVE_INSTANTIATE(SETTINGS) \
	template SETTINGS* StationSystemsRuntime::liveAuthoredEntry<SETTINGS>(std::string_view, size_t*); \
	template void StationSystemsRuntime::syncLiveNodeMirror<SETTINGS>(size_t, SETTINGS const&); \
	template void StationSystemsRuntime::resortLiveNode<SETTINGS>();

	PF_LIVE_INSTANTIATE(ResourceStoreSettings)
	PF_LIVE_INSTANTIATE(ResourceTransferSettings)
	PF_LIVE_INSTANTIATE(ResourceConversionSettings)
	PF_LIVE_INSTANTIATE(SolarArraySettings)
	PF_LIVE_INSTANTIATE(EquipmentLoadSettings)
	PF_LIVE_INSTANTIATE(BatterySettings)
	PF_LIVE_INSTANTIATE(ResourceDeliverySettings)
	PF_LIVE_INSTANTIATE(WasteEjectionSettings)
	PF_LIVE_INSTANTIATE(InstallationOutageSettings)
	PF_LIVE_INSTANTIATE(ThermalReservoirSettings)
	PF_LIVE_INSTANTIATE(RadiatorSettings)
	PF_LIVE_INSTANTIATE(CoolantLoopSettings)
	PF_LIVE_INSTANTIATE(ChillerSettings)
	PF_LIVE_INSTANTIATE(HeatEngineSettings)
	PF_LIVE_INSTANTIATE(GeneratorSettings)
	PF_LIVE_INSTANTIATE(AtmosphereSettings)
	PF_LIVE_INSTANTIATE(AtmospherePathwaySettings)
	PF_LIVE_INSTANTIATE(ThermalLinkSettings)
	PF_LIVE_INSTANTIATE(AirlockCycleSettings)
	PF_LIVE_INSTANTIATE(CrewSettings)
	PF_LIVE_INSTANTIATE(WaterCollectorSettings)
	PF_LIVE_INSTANTIATE(WaterTreatmentSettings)
	PF_LIVE_INSTANTIATE(ElectrolyzerSettings)
	PF_LIVE_INSTANTIATE(Co2CaptureSettings)
	PF_LIVE_INSTANTIATE(FuelCellSettings)
	PF_LIVE_INSTANTIATE(SabatierReactorSettings)
	PF_LIVE_INSTANTIATE(MethaneProcessorSettings)
	PF_LIVE_INSTANTIATE(MicrobialProcessorSettings)
	PF_LIVE_INSTANTIATE(ThermalBufferSettings)
	PF_LIVE_INSTANTIATE(CropPopulationSettings)
	PF_LIVE_INSTANTIATE(AlgaeCultureSettings)
	PF_LIVE_INSTANTIATE(MaintenanceJobSettings)
}
