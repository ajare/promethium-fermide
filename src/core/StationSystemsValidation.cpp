#include "StationSystemsDetail.h"
#include "core/StationPowerBuses.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string_view>
namespace core
{
using namespace station_detail;

namespace
{
	// #843: the shared Wear model contract. Fade rates are fractions of
	// rated capacity per accumulator unit: finite and non-negative, with a
	// ceiling in (0, 1], and at least one rate must actually fade - an
	// all-zero model is a pointless opt-in. A rate for an accumulator the
	// machine cannot physically wire must be zero, so no author can pay
	// for wear that can never be counted: a battery flows no counted mass,
	// a coolant loop is not cycle-counted.
	char const* wearModelViolation(InstallationWearSettings const& model,
		bool allowsCycles, bool allowsThroughput)
	{
		auto const fin = [](double v) { return std::isfinite(v); };
		if (!fin(model.fadePerCycle) || !fin(model.fadePerOperatingHour)
			|| !fin(model.fadePerThroughputKg) || !fin(model.fadePerCalendarHour)
			|| !fin(model.maxWearFraction))
			return " wear rates must all be finite";
		if (model.fadePerCycle < 0.0 || model.fadePerOperatingHour < 0.0
			|| model.fadePerThroughputKg < 0.0 || model.fadePerCalendarHour < 0.0)
			return " wear rates must be non-negative";
		if (model.maxWearFraction <= 0.0 || model.maxWearFraction > 1.0)
			return " wear ceiling must be greater than 0 and at most 1";
		if (!allowsCycles && model.fadePerCycle != 0.0)
			return " is not cycle-counted: fadePerCycle must be zero";
		if (!allowsThroughput && model.fadePerThroughputKg != 0.0)
			return " flows no counted mass: fadePerThroughputKg must be zero";
		if (model.fadePerCycle == 0.0 && model.fadePerOperatingHour == 0.0
			&& model.fadePerThroughputKg == 0.0 && model.fadePerCalendarHour == 0.0)
			return " wear model declares no fade rate";
		return nullptr;
	}
	// #844: the shared repair-parts wiring contract for every machine
	// kind that can opt in (#842 established it on the coolant loop).
	// `allowedModes` names the repairable fault modes the machine kind
	// owns; a plan for any other mode is an authoring error. The
	// consumed-parts stream must land in a declared waste store that can
	// actually receive it: same unit and same declared composition, as
	// for scheduled maintenance (#414).
	template <class Refuse, class Missing>
	bool validateRepairPartsWiring(RepairPartsSettings const& wiring,
		std::string const& where, std::string_view machineKind,
		StationSystemsSettings const& settings,
		std::initializer_list<std::string_view> allowedModes,
		Refuse const& refuse, Missing const& missing)
	{
		auto const findRepairStore = [&](std::string const& id) -> ResourceStoreSettings const*
		{
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == id; });
			return it == settings.stores.end() ? nullptr : &*it;
		};
		if (wiring.partsStoreId.empty())
			return refuse(where + " repair-parts store id must not be empty");
		auto const* parts = findRepairStore(wiring.partsStoreId);
		if (!parts)
			return missing(where + " repair-parts wiring references unknown store '"
				+ wiring.partsStoreId + "'");
		if (wiring.repairs.size() > MaxStationFaultRepairs)
			return refuse(where + " repair plans are limited to "
				+ std::to_string(MaxStationFaultRepairs) + " entries");
		std::set<std::string> planModes;
		for (auto const& plan : wiring.repairs)
		{
			auto const item = where + " repair plan '" + plan.modeId + "'";
			if (std::find(allowedModes.begin(), allowedModes.end(), plan.modeId) == allowedModes.end())
				return refuse(item + " names no repairable fault mode on "
					+ std::string(machineKind));
			if (!planModes.insert(plan.modeId).second)
				return refuse(where + " declares a repair plan for mode '"
					+ plan.modeId + "' more than once");
			if (!std::isfinite(plan.durationSeconds) || plan.durationSeconds <= 0.0)
				return refuse(item + " duration must be finite and positive");
			if (!std::isfinite(plan.partsQuantity) || plan.partsQuantity <= 0.0)
				return refuse(item + " parts quantity must be finite and positive");
			if (plan.wasteStoreId.empty())
				return refuse(item + " must name the waste store its consumed parts land in");
			if (plan.wasteStoreId == wiring.partsStoreId)
				return refuse(item + " consumes the parts store into itself;"
					" the waste store may not also be the parts source");
			auto const* waste = findRepairStore(plan.wasteStoreId);
			if (!waste)
				return missing(item + " references unknown waste store '"
					+ plan.wasteStoreId + "'");
			if (parts->unit != waste->unit)
				return refuse(item + " parts store unit '" + parts->unit
					+ "' does not match the waste store unit '" + waste->unit + "'");
			if (parts->composition || waste->composition)
			{
				if (!parts->composition || !waste->composition
					|| !compositionsClose(*parts->composition, *waste->composition))
					return refuse(item + " crosses a compound-material boundary into the"
						" waste store; consumed parts may only land in a store with the"
						" same declared composition");
			}
		}
		return true;
	}
	// #844: the shared finite-charge leak-block contract for the
	// refrigerant leak and the panel puncture: a charge that can bleed
	// needs a positive finite charge, a positive finite bleed rate, a
	// positive finite carried-heat coefficient and an existing
	// Atmosphere destination.
	bool validateChargeLeakBlock(std::string const& compartmentId, double chargeKg,
		double leakRateKgPerS, double specificHeatKJPerKgK, std::string_view modeName,
		StationSystemsSettings const& settings)
	{
		return std::isfinite(chargeKg) && chargeKg > 0.0
			&& std::isfinite(leakRateKgPerS) && leakRateKgPerS > 0.0
			&& std::isfinite(specificHeatKJPerKgK) && specificHeatKJPerKgK > 0.0
			&& std::any_of(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](auto const& air) { return air.id == compartmentId; });
	}
}
	bool isValidStationMaterialId(std::string const& id)
	{
		if (id.empty()) return true;
		if (id.size() > MaxStationMaterialIdLength || id.front() < 'a' || id.front() > 'z'
			|| id.back() == '-') return false;
		bool hyphen = false;
		for (auto const c : id)
		{
			if (c == '-')
			{
				if (hyphen) return false;
				hyphen = true;
			}
			else
			{
				if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
				hyphen = false;
			}
		}
		return true;
	}

	bool validateStationProfile(ProfileSettings const& profile, std::string const& where,
		std::string& diagnostic)
	{
		auto refuse = [&](std::string text) { diagnostic = where + ": " + std::move(text); return false; };
		if (!std::isfinite(profile.periodSeconds) || profile.periodSeconds < 0.0)
			return refuse("period must be finite and non-negative");
		if (profile.periodSeconds == 0.0)
		{
			if (!profile.segments.empty())
				return refuse("a constant profile must not declare segments");
			if (!std::isfinite(profile.constantLevel)
				|| profile.constantLevel < 0.0 || profile.constantLevel > 1.0)
				return refuse("constant level must be finite and from 0 to 1");
			return true;
		}
		if (profile.segments.empty() || profile.segments.size() > MaxStationProfileSegments)
			return refuse("a periodic profile needs between 1 and "
				+ std::to_string(MaxStationProfileSegments) + " segments");
		if (profile.segments.front().startSeconds != 0.0)
			return refuse("the first segment must start at 0");
		for (size_t index = 0; index < profile.segments.size(); ++index)
		{
			auto const& segment = profile.segments[index];
			if (!std::isfinite(segment.startSeconds) || segment.startSeconds < 0.0
				|| segment.startSeconds >= profile.periodSeconds)
				return refuse("segment start must be finite and inside the period");
			if (!std::isfinite(segment.level) || segment.level < 0.0 || segment.level > 1.0)
				return refuse("segment level must be finite and from 0 to 1");
			if (index > 0 && segment.startSeconds <= profile.segments[index - 1].startSeconds)
				return refuse("segments must start in strictly increasing order");
		}
		return true;
	}
	bool validateStationTransferSettings(ResourceTransferSettings const& transfer, std::string& diagnostic)
	{
		auto refuse = [&](char const* reason) { diagnostic = "Station resource transfer '" + transfer.id + "' " + reason; return false; };
		if (!std::isfinite(transfer.ratePerSecond) || !std::isfinite(transfer.quantity)
			|| !std::isfinite(transfer.firstOccurrenceSeconds) || !std::isfinite(transfer.recurrenceSeconds))
			return refuse("numbers must be finite");
		if (transfer.mode == ResourceTransferMode::Continuous)
			return transfer.ratePerSecond > 0.0 || refuse("rate must be positive");
		if (transfer.mode != ResourceTransferMode::Scheduled) return refuse("mode is invalid");
		if (transfer.quantity <= 0.0) return refuse("quantity must be positive");
		if (transfer.firstOccurrenceSeconds < 0.0) return refuse("first occurrence must be non-negative");
		if (transfer.recurrenceSeconds < 0.0 || (transfer.recurrenceSeconds > 0.0
			&& transfer.recurrenceSeconds + StationScheduleTolerance < static_cast<double>(StationScheduleQuantumSeconds)))
			return refuse("recurrence must be zero or at least one fixed tick");
		return true;
	}

	bool validateStationTransferWiring(StationSystemsSettings const& settings,
		ResourceTransferSettings const& transfer, std::string& diagnostic,
		bool ignoreUnconnected)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto const where = "Station resource transfer '" + transfer.id + "'";
		auto const openSource = ignoreUnconnected && transfer.sourceId.empty();
		auto const openDestination = ignoreUnconnected && transfer.destinationId.empty();
		if (openSource && openDestination) return true;
		if (!openSource && !openDestination && transfer.sourceId == transfer.destinationId)
			return refuse(where + " source and destination must differ");
		auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
		{
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == id; });
			return it == settings.stores.end() ? nullptr : &*it;
		};
		auto const* source = openSource ? nullptr : findStore(transfer.sourceId);
		auto const* destination = openDestination ? nullptr : findStore(transfer.destinationId);
		if (!openSource && !source)
			return refuse(where + " references unknown source store '" + transfer.sourceId + "'");
		if (!openDestination && !destination)
			return refuse(where + " references unknown destination store '" + transfer.destinationId + "'");
		// Same-unit endpoints only: cross-unit movement is conversion, which
		// later slices introduce with explicit composition accounting. With
		// one end still open only the connected end can be judged.
		if (source && destination)
		{
			if (transfer.unit != source->unit || transfer.unit != destination->unit)
				return refuse(where + " unit '" + transfer.unit
					+ "' does not match both stores ('" + source->unit + "' -> '" + destination->unit + "')");
		}
		else if (source && transfer.unit != source->unit)
			return refuse(where + " unit '" + transfer.unit + "' does not match store '"
				+ source->id + "' ('" + source->unit + "')");
		else if (destination && transfer.unit != destination->unit)
			return refuse(where + " unit '" + transfer.unit + "' does not match store '"
				+ destination->id + "' ('" + destination->unit + "')");
		if (!source || !destination) return true;
		// Compound material only moves between identical declared
		// compositions. Crossing a composition boundary — including bound
		// nutrient nitrogen versus atmospheric nitrogen, which are never
		// freely interchangeable — is conversion and requires an
		// explicitly declared, constituent-balanced conversion process.
		auto const waterDestination = std::any_of(settings.waterTreatments.begin(), settings.waterTreatments.end(),
				[&](auto const& t) { return destination->id == t.sourceStoreId
					|| destination->id == t.treatedStoreId || destination->id == t.concentrateStoreId; });
		auto const dilution = !source->composition && destination->composition
			&& isMassUnit(source->unit) && waterDestination;
		if (!dilution && (source->composition || destination->composition))
		{
			if (!source->composition || !destination->composition
				|| !compositionsClose(*source->composition, *destination->composition))
				return refuse(where + " crosses a compound-material boundary; transfers between stores with different declared compositions require an explicit conversion");
		}
		// Cross-class transfers are legal (#750): the hazard class no
		// longer gates wiring, so a transfer between stores of differing
		// class is accepted and the moved mass is the only report.
		return true;
	}
	namespace
	{
		// Unwind signal for draft-tolerant validation (#642): an entry whose
		// reference cannot be resolved raises this so the enclosing per-entry
		// loop can record the entry as broken and carry on. It never escapes
		// the validator.
		struct BrokenDraftEntry
		{
		};
	}
	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		AtmosphereSettings const& atmosphere, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto const where = "Station atmosphere '" + atmosphere.id + "'";
		if (atmosphere.id.empty()) return refuse("Station atmosphere id must not be empty");
		if (atmosphere.id == StationExteriorAtmosphereId)
			return refuse(where + " id is reserved for exterior vacuum");
		if (atmosphere.id.size() > MaxStationStoreIdLength)
			return refuse("Station atmosphere id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (!std::isfinite(atmosphere.volumeM3) || atmosphere.volumeM3 <= 0.0)
			return refuse(where + " volume must be finite and positive");
		auto const masses = {atmosphere.startingOxygenKg, atmosphere.startingNitrogenKg,
			atmosphere.startingCarbonDioxideKg, atmosphere.startingWaterVapourKg};
		for (auto const mass : masses)
			if (!std::isfinite(mass) || mass < 0.0)
				return refuse(where + " starting gas masses must all be finite and non-negative");
		if (!std::isfinite(atmosphere.startingTemperatureC)
			|| atmosphere.startingTemperatureC <= -StationCelsiusOffset)
			return refuse(where + " starting temperature must be finite and above absolute zero (-273.15 C)");
		if (!std::isfinite(atmosphere.thermalCapacityKJPerK) || atmosphere.thermalCapacityKJPerK <= 0.0)
			return refuse(where + " effective thermal capacity must be finite and positive");
		// #802: the bulk specific heat carried by this compartment's gas
		// when it crosses a pathway must be finite and strictly positive -
		// a zero or negative advected specific heat is refused rather
		// than silently inert, same discipline as the Thermal link's
		// conductance.
		if (!std::isfinite(atmosphere.advectedSpecificHeatKJPerKgK)
			|| atmosphere.advectedSpecificHeatKJPerKgK <= 0.0)
			return refuse(where + " advected specific heat must be finite and positive");
		if (!atmosphere.reservoirId.empty())
		{
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == atmosphere.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return missing(where + " is coupled to unknown thermal reservoir '" + atmosphere.reservoirId + "'");
		}
		if (!std::isfinite(atmosphere.exchangerConductanceKWPerK) || atmosphere.exchangerConductanceKWPerK < 0.0)
			return refuse(where + " exchanger conductance must be finite and non-negative");
		if (atmosphere.occupants < 0)
			return refuse(where + " occupant count must not be negative");
		auto const rates = {atmosphere.oxygenUsePerOccupantKgPerS,
			atmosphere.co2ProductionPerOccupantKgPerS, atmosphere.sensibleHeatPerOccupantKW,
			atmosphere.moistureAdditionPerOccupantKgPerS};
		for (auto const rate : rates)
			if (!std::isfinite(rate) || rate < 0.0)
				return refuse(where + " per-occupant load rates must all be finite and non-negative");
		if (atmosphere.fire)
		{
			auto const& f = *atmosphere.fire;
			for (auto const value : {f.oxygenKgPerS, f.carbonMonoxideKgPerS, f.carbonDioxideKgPerS, f.heatKW})
				if (!std::isfinite(value) || value <= 0.0) return refuse(where + " fire rates must be finite and positive");
			if (!std::isfinite(f.oxygenThresholdKg) || f.oxygenThresholdKg < 0.0)
				return refuse(where + " fire oxygen threshold must be finite and non-negative");
		}
		if (!std::isfinite(atmosphere.leakRateKgPerS) || atmosphere.leakRateKgPerS < 0.0)
			return refuse(where + " leak rate must be finite and non-negative");
		if (!std::isfinite(atmosphere.minPressureKPa) || !std::isfinite(atmosphere.maxPressureKPa)
			|| atmosphere.minPressureKPa <= 0.0 || atmosphere.minPressureKPa >= atmosphere.maxPressureKPa)
			return refuse(where + " pressure limits must be finite and positive with min below max");
		if (!std::isfinite(atmosphere.minOxygenPartialPressureKPa) || atmosphere.minOxygenPartialPressureKPa < 0.0)
			return refuse(where + " minimum oxygen partial pressure must be finite and non-negative");
		if (!std::isfinite(atmosphere.maxCarbonDioxidePartialPressureKPa)
			|| atmosphere.maxCarbonDioxidePartialPressureKPa < 0.0)
			return refuse(where + " maximum carbon dioxide partial pressure must be finite and non-negative");
		// #565: every authored field being finite does not make the
		// starting state publishable. The ideal-gas division by a tiny
		// volume can overflow, and a compartment whose starting
		// inventory already derives a non-finite pressure is an
		// inconsistent authored input: it is refused here, before any
		// runtime exists, so no initial or reset snapshot can ever
		// publish a non-finite derived pressure as a healthy state.
		{
			auto const startingOxygenKPa = stationAtmospherePartialPressureKPa(
				atmosphere.startingOxygenKg, StationMolarMassOxygenKgPerMol,
				atmosphere.volumeM3, atmosphere.startingTemperatureC);
			auto const startingNitrogenKPa = stationAtmospherePartialPressureKPa(
				atmosphere.startingNitrogenKg, StationMolarMassNitrogenKgPerMol,
				atmosphere.volumeM3, atmosphere.startingTemperatureC);
			auto const startingCarbonDioxideKPa = stationAtmospherePartialPressureKPa(
				atmosphere.startingCarbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
				atmosphere.volumeM3, atmosphere.startingTemperatureC);
			auto const startingVapourKPa = stationAtmospherePartialPressureKPa(
				atmosphere.startingWaterVapourKg, StationMolarMassWaterKgPerMol,
				atmosphere.volumeM3, atmosphere.startingTemperatureC);
			if (!std::isfinite(startingOxygenKPa) || !std::isfinite(startingNitrogenKPa)
				|| !std::isfinite(startingCarbonDioxideKPa) || !std::isfinite(startingVapourKPa)
				|| !std::isfinite(startingOxygenKPa + startingNitrogenKPa
					+ startingCarbonDioxideKPa + startingVapourKPa))
				return refuse(where + " starting inventory in this volume and temperature derives a non-finite pressure");
		}
		return true;
	}

	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		AirlockCycleSettings const& cycle, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto const where = "Station airlock cycle '" + cycle.id + "'";
		if (cycle.id.empty()) return refuse("Station airlock cycle id must not be empty");
		if (cycle.id.size() > MaxStationStoreIdLength)
			return refuse("Station airlock cycle id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (!allowIncomplete || !cycle.atmosphereId.empty())
		{
			auto const it = std::find_if(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == cycle.atmosphereId; });
			if (it == settings.atmospheres.end())
				return missing(where + " draws from unknown atmosphere compartment '" + cycle.atmosphereId + "'");
		}

		if (cycle.interlockFailure)
		{
			auto const& f = *cycle.interlockFailure;
			if (!std::isfinite(f.breachAreaM2) || f.breachAreaM2 <= 0.0)
				return refuse(where + " breach area must be finite and positive");
			auto const p = std::find_if(settings.atmospherePathways.begin(), settings.atmospherePathways.end(),
				[&](auto const& path) { return path.id == f.pathwayId; });
			if (p == settings.atmospherePathways.end()) return missing(where + " interlock-failure references unknown pathway");
			if (!((p->aAtmosphereId == cycle.atmosphereId && p->bAtmosphereId == StationExteriorAtmosphereId)
				|| (p->bAtmosphereId == cycle.atmosphereId && p->aAtmosphereId == StationExteriorAtmosphereId)))
				return refuse(where + " breach pathway must join its compartment to Exterior vacuum");
		}
		if (!std::isfinite(cycle.gasMassKg) || cycle.gasMassKg <= 0.0)
			return refuse(where + " cycled gas mass must be finite and positive");
		if (!std::isfinite(cycle.firstOccurrenceSeconds) || cycle.firstOccurrenceSeconds < 0.0)
			return refuse(where + " first occurrence must be finite and non-negative");
		if (!std::isfinite(cycle.recurrenceSeconds) || cycle.recurrenceSeconds < 0.0)
			return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
		if (cycle.recurrenceSeconds > 0.0
			&& cycle.recurrenceSeconds + StationScheduleTolerance
				< static_cast<double>(StationScheduleQuantumSeconds))
			return refuse(where + " recurrence must be zero or at least one World fixed tick ("
				+ std::to_string(StationScheduleQuantumSeconds) + " seconds)");
		if (!std::isfinite(cycle.recoveredFraction) || cycle.recoveredFraction < 0.0
			|| cycle.recoveredFraction > 1.0)
			return refuse(where + " recovered fraction must be finite and from 0 to 1");
		return true;
	}

	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		WaterCollectorSettings const& collector, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowIncomplete && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			return true;
		};
		auto const where = "Station water collector '" + collector.id + "'";
		if (collector.id.empty()) return refuse("Station water collector id must not be empty");
		if (collector.id.size() > MaxStationStoreIdLength)
			return refuse("Station water collector id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
			collector.thermalClass) == StationThermalServiceClasses.end())
			return refuse(where + " has unknown thermal service class '" + collector.thermalClass
				+ "'; declared classes are: " + declaredThermalClasses());
		if (!std::isfinite(collector.maxRateKgPerS) || collector.maxRateKgPerS < 0.0)
			return refuse(where + " maximum collection rate must be finite and non-negative");
		if (!std::isfinite(collector.targetRelativeHumidity)
			|| collector.targetRelativeHumidity < 0.0 || collector.targetRelativeHumidity > 1.0)
			return refuse(where + " target relative humidity must be finite and from 0 to 1");
		if (!allowIncomplete || !collector.atmosphereId.empty())
		{
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
				{ return atmosphere.id == collector.atmosphereId; });
			if (compartment == settings.atmospheres.end())
				return missing(where + " condenses from unknown atmosphere compartment '" + collector.atmosphereId + "'");
			// #752: no class match is required. Condensate is just water,
			// whatever air it condensed from, so a collector may serve a
			// compartment of any class.
		}

		if (!allowIncomplete || !collector.coolingLoopId.empty())
		{
			auto const loop = std::find_if(settings.coolantLoops.begin(), settings.coolantLoops.end(),
				[&](CoolantLoopSettings const& candidate) { return candidate.id == collector.coolingLoopId; });
			if (loop == settings.coolantLoops.end())
				return missing(where + " cold surface is served by unknown coolant loop '" + collector.coolingLoopId + "'");
			if (!allowIncomplete || !loop->reservoirId.empty())
			{
				auto const host = std::find_if(settings.thermalReservoirs.begin(),
					settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
					{ return reservoir.id == loop->reservoirId; });
				if (host == settings.thermalReservoirs.end())
					return missing(where + " coolant loop '" + loop->id + "' has no known host reservoir");
				if (host->thermalClass != collector.thermalClass)
					return refuse(where + " thermal class '" + collector.thermalClass + "' does not match coolant loop '"
						+ loop->id + "' host reservoir class '" + host->thermalClass
						+ "'; the cold surface serves only its own service class");
			}
		}

		if (!declareWaterStore(collector.destStoreId, "raw water destination", where, false))
			return false;
		return true;
	}

	std::map<std::string, double> waterTreatmentEffluent(
		ResourceCompositionSettings const& blend, WaterTreatmentSettings const& treatment)
	{
		auto result = compositionLedgerOf(blend, 1.0, 1.0);
		for (auto& [name, mass] : result)
		{
			if (name == "water") mass *= treatment.recoveryRatio;
			else
			{
				auto const found = treatment.removalFractions.find(name == "energy" ? "dryOrganics" : name);
				mass = found == treatment.removalFractions.end() ? 0.0 : mass - mass * found->second;
			}
		}
		return result;
	}

	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		WaterTreatmentSettings const& treatment, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			return true;
		};
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowIncomplete && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			return true;
		};
		auto const where = "Station water treatment '" + treatment.id + "'";
		if (treatment.id.empty()) return refuse("Station water treatment id must not be empty");
		if (treatment.id.size() > MaxStationStoreIdLength)
			return refuse("Station water treatment id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (!std::isfinite(treatment.throughputKgPerS) || treatment.throughputKgPerS < 0.0)
			return refuse(where + " throughput must be finite and non-negative");
		if (!std::isfinite(treatment.recoveryRatio) || treatment.recoveryRatio <= 0.0
			|| treatment.recoveryRatio > 1.0)
			return refuse(where + " recovery ratio must be finite and above zero up to 1: a process that recovers nothing is not a recovery process");
		if (!std::isfinite(treatment.specificEnergyKWhPerKg) || treatment.specificEnergyKWhPerKg < 0.0)
			return refuse(where + " specific energy must be finite and non-negative");
		if (!std::isfinite(treatment.consumablePerKg) || treatment.consumablePerKg < 0.0)
			return refuse(where + " consumable per kg must be finite and non-negative");
		char const* streamRoles[3]{"source", "treated", "concentrate"};
		std::string const streamIds[3]{treatment.sourceStoreId, treatment.treatedStoreId,
			treatment.concentrateStoreId};
		for (auto const& [name, fraction] : treatment.removalFractions)
		{
			if (std::find(StationWaterDryConstituents.begin(), StationWaterDryConstituents.end(), name)
				== StationWaterDryConstituents.end())
				return refuse(where + " unknown removal constituent '" + name + "'");
			if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
				return refuse(where + " removal fraction '" + name + "' must be finite and within [0, 1]");
		}
		auto source = std::find_if(settings.stores.begin(), settings.stores.end(),
			[&](auto const& store) { return store.id == treatment.sourceStoreId; });
		if (source != settings.stores.end() && source->composition)
		{
			auto const load = compositionLedgerOf(*source->composition, 1.0, 1.0);
			for (auto const& [name, mass] : load)
			{
				if (name == "water" || name == "energy" || mass <= 0.0) continue;
				auto const f = treatment.removalFractions.find(name);
				auto const removal = f == treatment.removalFractions.end() ? 1.0 : f->second;
				for (size_t outlet : {size_t{1}, size_t{2}})
				{
					if ((outlet == 1 && removal == 1.0) || (outlet == 2 && removal == 0.0)) continue;
					auto dest = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](auto const& store) { return store.id == streamIds[outlet]; });
					if ((streamIds[outlet].empty() && !allowIncomplete && !brokenReferences)
						|| (dest != settings.stores.end() && !dest->composition))
						return refuse(where + " constituent '" + name + "' requires a compound "
							+ streamRoles[outlet] + " store; dry load must go somewhere");
				}
			}
		}
		for (size_t stream = 0; stream < 3; ++stream)
		{
			for (size_t other = stream + 1; other < 3; ++other)
				if (!streamIds[stream].empty() && streamIds[stream] == streamIds[other])
					return refuse(where + " " + streamRoles[stream] + " store and " + streamRoles[other]
						+ " store are the same store '" + streamIds[stream]
						+ "'; the recovery split needs distinct source, treated and concentrate stores");
			if (!declareWaterStore(streamIds[stream], streamRoles[stream], where, true))
				return false;
		}
		if (!declareWaterStore(treatment.consumableStoreId, "consumable", where, true))
			return false;
		return true;
	}

	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		ElectrolyzerSettings const& electrolyzer, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			return true;
		};
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowIncomplete && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			return true;
		};
		auto declareGasStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowIncomplete && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; gas handling is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
			return true;
		};
		auto const where = "Station electrolyzer '" + electrolyzer.id + "'";
		if (electrolyzer.id.empty()) return refuse("Station electrolyzer id must not be empty");
		if (electrolyzer.id.size() > MaxStationStoreIdLength)
			return refuse("Station electrolyzer id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
			electrolyzer.thermalClass) == StationThermalServiceClasses.end())
			return refuse(where + " has unknown thermal service class '" + electrolyzer.thermalClass
				+ "'; declared classes are: " + declaredThermalClasses());
		if (!std::isfinite(electrolyzer.rateKgPerS) || electrolyzer.rateKgPerS < 0.0)
			return refuse(where + " rated water split must be finite and non-negative");
		if (!std::isfinite(electrolyzer.specificEnergyKWhPerKgWater)
			|| electrolyzer.specificEnergyKWhPerKgWater * 3600.0
				< StationElectrolysisChemicalEnergyKJPerKgWater
				- StationBalanceTolerance * std::max(1.0,
					StationElectrolysisChemicalEnergyKJPerKgWater))
			return refuse(where + " specific energy is below the chemical energy the produced hydrogen retains: the electrical dose may not fund less than the products carry");
		if (electrolyzer.thermalBody)
		{
			auto const& body = *electrolyzer.thermalBody;
			if (!std::isfinite(body.heatCapacityKJPerK) || body.heatCapacityKJPerK <= 0.0
				|| !std::isfinite(body.sinkConductanceKWPerK) || body.sinkConductanceKWPerK <= 0.0
				|| !std::isfinite(body.startingTemperatureC) || body.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " thermalBody requires positive finite capacity/conductance and temperature above absolute zero");
		}
		if (!std::isfinite(electrolyzer.maxSinkTemperatureC)
			|| electrolyzer.maxSinkTemperatureC <= -StationCelsiusOffset)
			return refuse(where + " maximum sink temperature must be finite and above absolute zero");
		auto const oxygenDestinationError = where
			+ " must configure exactly one oxygen destination: a qualified gas store or one compartment atmosphere";
		if (!electrolyzer.oxygenStoreId.empty() && !electrolyzer.atmosphereId.empty())
			return refuse(oxygenDestinationError);
		if (!allowIncomplete && electrolyzer.oxygenStoreId.empty() && electrolyzer.atmosphereId.empty()
			&& !missing(oxygenDestinationError)) return false;
		if (!electrolyzer.atmosphereId.empty())
		{
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
				{ return atmosphere.id == electrolyzer.atmosphereId; });
			// Draft loading tolerates this broken reference, but must still
			// validate the surviving stores and heat sink below.
			if (compartment == settings.atmospheres.end()
				&& !missing(where + " delivers oxygen to unknown atmosphere compartment '" + electrolyzer.atmosphereId + "'"))
				return false;
		}
		// Aliased stream roles are refused (#585): the two products are
		// chemically distinct, and the tick sizes each product against
		// its own store's headroom before either is added. Pointing both
		// at one store spends the same headroom twice and overfills it -
		// authored configuration turning a finite physical limit into a
		// terminal runtime failure - so the alias never reaches the tick.
		if (!electrolyzer.oxygenStoreId.empty()
			&& electrolyzer.oxygenStoreId == electrolyzer.hydrogenStoreId)
			return refuse(where + " names the same store '" + electrolyzer.oxygenStoreId
				+ "' for both the oxygen and the hydrogen product: the two products are chemically distinct streams and may not share one store, whose headroom the split would spend twice");
		if (!electrolyzer.waterStoreId.empty()
			&& (electrolyzer.waterStoreId == electrolyzer.hydrogenStoreId
			|| (!electrolyzer.oxygenStoreId.empty()
				&& electrolyzer.waterStoreId == electrolyzer.oxygenStoreId)))
			return refuse(where + " names the same store for the feed water and a gas product: the machine cannot treat one store as both the water it draws and the gas it produces");
		if (!declareWaterStore(electrolyzer.waterStoreId, "feed water", where, true))
			return false;
		if (!electrolyzer.oxygenStoreId.empty()
			&& !declareGasStore(electrolyzer.oxygenStoreId, "oxygen", where, false))
			return false;
		if (!declareGasStore(electrolyzer.hydrogenStoreId, "hydrogen", where, false))
			return false;
		if ((!allowIncomplete && !electrolyzer.thermalBody) || !electrolyzer.heatSinkId.empty())
		{
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == electrolyzer.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " heat sink is unknown thermal reservoir '" + electrolyzer.heatSinkId + "'");
			if (electrolyzer.thermalBody && sink->heatCapacityKJPerK <= 0.0)
				return refuse(where + " thermalBody sink requires positive thermal capacity");
			if (sink->thermalClass != electrolyzer.thermalClass)
				return refuse(where + " thermal class '" + electrolyzer.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}

		return true;
	}

	bool validateStationWaterGasWiring(StationSystemsSettings const& settings,
		Co2CaptureSettings const& capture, std::string& diagnostic, [[maybe_unused]] bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto declareGasStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowIncomplete && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; gas handling is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
			return true;
		};
		auto const where = "Station CO2 capture machine '" + capture.id + "'";
		if (capture.id.empty()) return refuse("Station CO2 capture machine id must not be empty");
		if (capture.id.size() > MaxStationStoreIdLength)
			return refuse("Station CO2 capture machine id exceeds "
				+ std::to_string(MaxStationStoreIdLength) + " characters");
		if (!std::isfinite(capture.maxRateKgPerS) || capture.maxRateKgPerS < 0.0)
			return refuse(where + " capture capacity must be finite and non-negative");
		if (!std::isfinite(capture.specificEnergyKWhPerKg) || capture.specificEnergyKWhPerKg < 0.0)
			return refuse(where + " specific energy must be finite and non-negative");
		if (!std::isfinite(capture.consumablePerKg) || capture.consumablePerKg < 0.0)
			return refuse(where + " consumable per kg must be finite and non-negative");
		if (!allowIncomplete && capture.consumablePerKg > 0.0 && capture.consumableStoreId.empty())
			return refuse(where + " declares a consumable demand but names no consumable store: a process cannot spend media it has no way to hold");
		if (!allowIncomplete || !capture.atmosphereId.empty())
		{
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
				{ return atmosphere.id == capture.atmosphereId; });
			if (compartment == settings.atmospheres.end())
				return missing(where + " scrubs unknown atmosphere compartment '" + capture.atmosphereId + "'");
		}

		if (!declareGasStore(capture.captureStoreId, "capture", where, false))
			return false;
		if (!capture.consumableStoreId.empty()
			&& !declareGasStore(capture.consumableStoreId, "consumable", where, true))
			return false;
		return true;
	}

	bool validateStationScheduleWiring(StationSystemsSettings const& settings,
		ResourceDeliverySettings const& delivery, std::string& diagnostic, bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string reason) { diagnostic = std::move(reason); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto const where = "Station resupply delivery '" + delivery.id + "'";
		if (allowIncomplete && delivery.storeId.empty()) return true;
		auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
			[&](ResourceStoreSettings const& store) { return store.id == delivery.storeId; });
		if (it == settings.stores.end())
			return missing(where + " targets unknown Resource store '" + delivery.storeId + "'");
		// A delivery brings a store's own material in; it never converts
		// units on the way through the boundary.
		if (delivery.unit != it->unit)
			return refuse(where + " unit '" + delivery.unit
				+ "' does not match destination store unit '" + it->unit + "'");
		return true;
	}
	bool validateStationScheduleWiring(StationSystemsSettings const& settings,
		WasteEjectionSettings const& ejection, std::string& diagnostic, bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string reason) { diagnostic = std::move(reason); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto const where = "Station waste ejection '" + ejection.id + "'";
		if (allowIncomplete && ejection.storeId.empty()) return true;
		auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
			[&](ResourceStoreSettings const& store) { return store.id == ejection.storeId; });
		if (it == settings.stores.end())
			return missing(where + " targets unknown Resource store '" + ejection.storeId + "'");
		// Ejection carries a store's own material out; it never
		// converts units on the way through the boundary.
		if (ejection.unit != it->unit)
			return refuse(where + " unit '" + ejection.unit
				+ "' does not match target store unit '" + it->unit + "'");
		return true;
	}
	bool validateStationScheduleWiring(StationSystemsSettings const& settings,
		InstallationOutageSettings const& outage, std::string& diagnostic, bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string reason) { diagnostic = std::move(reason); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			return true;
		};
		auto const where = "Station installation outage '" + outage.id + "'";
		auto const registeredInstallations = stationInstallationIdentities(settings);
		if (!(outage.installationId.empty() && allowIncomplete) && std::none_of(registeredInstallations.begin(), registeredInstallations.end(),
			[&](StationInstallationIdentity const& installation) {
				return installation.id == outage.installationId;
			}))
			return missing(where + " targets unknown installation '" + outage.installationId
				+ "'; only installations registered with the availability contract can be outaged");

		return true;
	}
	bool validateStationScheduleWiring(StationSystemsSettings const& settings,
		MaintenanceJobSettings const& job, std::string& diagnostic, bool allowIncomplete, std::vector<std::string>* brokenReferences)
	{
		diagnostic.clear();
		auto refuse = [&](std::string reason) { diagnostic = std::move(reason); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			return true;
		};
		auto const where = "Station maintenance job '" + job.id + "'";
		auto const registeredInstallations = stationInstallationIdentities(settings);
		if (!(job.installationId.empty() && allowIncomplete) && std::none_of(registeredInstallations.begin(), registeredInstallations.end(),
			[&](StationInstallationIdentity const& installation) {
				return installation.id == job.installationId;
			}))
			if (!missing(where + " targets unknown installation '" + job.installationId
				+ "'; only installations registered with the availability contract can be maintained")) return false;
		auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
		{
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == id; });
			return it == settings.stores.end() ? nullptr : &*it;
		};
		ResourceStoreSettings const* waste = nullptr;
		if (!job.wasteStoreId.empty())
		{
			waste = findStore(job.wasteStoreId);
			if (!waste)
				if (!missing(where + " references unknown waste store '" + job.wasteStoreId + "'")) return false;
		}
		bool anyConsumed = false;
		std::set<std::string> requirementStores;
		for (auto const& requirement : job.requirements)
		{
			auto const item = where + " requirement '" + requirement.storeId + "'";
			if (!allowIncomplete && (!std::isfinite(requirement.quantity) || requirement.quantity <= 0.0))
				return refuse(item + " quantity must be finite and positive");
			if (allowIncomplete && requirement.storeId.empty()) continue;
			if (requirement.storeId.empty() && !brokenReferences)
				return refuse(where + " requirement store id must not be empty");
			if (!requirement.storeId.empty() && !requirementStores.insert(requirement.storeId).second)
				return refuse(item + " is duplicated within the job");
			auto const* source = findStore(requirement.storeId);
			if (!source && !missing(item + " references unknown store")) return false;
			if (!requirement.consumed) continue;
			anyConsumed = true;
			if (!requirement.storeId.empty() && requirement.storeId == job.wasteStoreId)
				return refuse(item + " is consumed into itself; the waste store may not also be its own source");
			if (!waste && (allowIncomplete || brokenReferences)) continue;
			if (!waste)
				return refuse(where + " consumes from " + item
					+ " but declares no waste store; removed parts and used material must be booked into appropriate waste");
			if (!source) continue;
			if (source->unit != waste->unit)
				return refuse(item + " unit '" + source->unit
					+ "' does not match the waste store unit '" + waste->unit + "'");
			if (source->composition || waste->composition)
			{
				if (!source->composition || !waste->composition
					|| !compositionsClose(*source->composition, *waste->composition))
					return refuse(item
						+ " crosses a compound-material boundary into the waste store; consumed material may only land in a store with the same declared composition");
			}
			// Consumed material no longer has to stay on its own side of
			// a class boundary (#751): the waste store simply receives
			// whatever the job consumed.
		}
		if (!allowIncomplete && !anyConsumed && !job.wasteStoreId.empty())
			return refuse(where + " declares a waste store but consumes nothing; the waste store would never receive material");
		return true;
	}


	bool validateStationMonitors(StationSystemsSettings const& settings, std::string& diagnostic)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		std::set<std::string> monitorIds;
		for (auto const& monitor : settings.monitors)
		{
			auto fail = [&](std::string reason) { return refuse("Monitor '" + monitor.id + "': " + reason); };
			if (monitor.id.starts_with("breaker:")) return fail("breaker: IDs are reserved for always-on Bus Trip Alerts");
			if (monitor.id.empty() || !monitorIds.insert(monitor.id).second)
				return fail("id must be non-empty and unique");
			auto const& catalogue = stationMonitorMetricCatalogue(monitor.subjectType);
			if (catalogue.empty()) return fail("unknown subject type");
			if (std::none_of(catalogue.begin(), catalogue.end(),
				[&](auto const& entry) { return entry.name == monitor.metric; }))
				return fail("unknown metric for subject type");
			if (!stationMonitorSubjectExists(settings, monitor.subjectType, monitor.subjectId))
				return fail("subject does not exist");
			auto const& rule = monitor.rule;
			if (!std::isfinite(rule.threshold)) return fail("threshold must be finite");
			if (monitor.subjectType == "atmosphere")
			{
				if (monitor.metric == "relativeHumidity"
					&& (rule.threshold < 0 || rule.threshold > 1))
					return fail("threshold must be in fraction range [0, 1]");
				if (monitor.metric == "temperatureC" ? rule.threshold < -273.15 : rule.threshold < 0)
					return fail("threshold is outside the metric's physical unit range");
			}
			if (!std::isfinite(rule.triggerOnSeconds) || rule.triggerOnSeconds < 0
				|| !std::isfinite(rule.triggerOffSeconds) || rule.triggerOffSeconds < 0)
				return fail("trigger times must be finite and non-negative");
			if (rule.direction != AlertDirection::Above && rule.direction != AlertDirection::Below)
				return fail("unknown direction");
			if (monitor.severity != AlertSeverity::Advisory && monitor.severity != AlertSeverity::Warning
				&& monitor.severity != AlertSeverity::Critical) return fail("unknown severity");
			if (monitor.provenance < MonitorProvenance::Custom || monitor.provenance > MonitorProvenance::Provisional)
				return fail("unknown provenance");
		}
		diagnostic.clear();
		return true;
	}
	bool validateStationSystemsSettings(StationSystemsSettings const& settings, std::string& diagnostic,
		std::vector<std::string>* brokenReferences)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		if (!validateStationMonitors(settings, diagnostic)) return false;
		std::set<std::string> busIds;
		if (settings.powerBuses.empty()) return refuse("Station requires at least one Power Bus");
		for (auto const& bus : settings.powerBuses)
		{
			if (bus.id.empty() || bus.id.size() > MaxStationStoreIdLength || !busIds.insert(bus.id).second)
				return refuse("Power Bus id '" + bus.id + "' must be non-empty, bounded and unique");
			if (!std::isfinite(bus.ratedCapacityKW) || bus.ratedCapacityKW < 0.0)
				return refuse("Power Bus '" + bus.id + "' rated capacity must be finite and non-negative");
			if (!std::isfinite(bus.tripPersistenceSeconds) || bus.tripPersistenceSeconds < 0.0)
				return refuse("Power Bus '" + bus.id + "' trip persistence must be finite and non-negative");
		}
		bool powerValuesValid = true;
		forEachStationPowerMember(settings, [&](auto const& entry) {
			auto membership = [&](auto const& ids) {
				if (!powerValuesValid) return;
				if (ids.empty()) {
					auto const text = "Station '" + entry.id + "' requires Power Bus membership";
					if (brokenReferences) brokenReferences->push_back(text);
					else powerValuesValid = refuse(text);
					return;
				}
				std::set<std::string> unique;
				for (auto const& id : ids)
				{
					if (!unique.insert(id).second)
						powerValuesValid = refuse("Station '" + entry.id + "' has duplicate Power Bus '" + id + "'; Transfer primary and backup must be distinct");
					if (!busIds.count(id))
					{
						auto const text = "Station '" + entry.id + "' has unknown Power Bus '" + id + "'";
						if (brokenReferences) brokenReferences->push_back(text);
						else powerValuesValid = refuse(text);
					}
				}
			};
			if constexpr (requires { entry.busIds; }) membership(entry.busIds);
			if constexpr (requires { entry.supplyBusIds; }) membership(entry.supplyBusIds);
			if constexpr (requires { entry.supplyBusIds; })
			{
				auto const count = entry.supplyBusIds.size();
				if (powerValuesValid && ((entry.supplyMode == StationSupplyMode::Single && count != 1)
					|| (entry.supplyMode == StationSupplyMode::Transfer && count != 2)
					|| (entry.supplyMode == StationSupplyMode::Shared && count < 2)))
				{
					auto const text = "Station '" + entry.id + "' Supply mode requires Single = 1, Transfer = 2 distinct buses, Shared >= 2 buses";
					if (brokenReferences && (count == 0 || (entry.supplyMode != StationSupplyMode::Single && count < 2))) brokenReferences->push_back(text);
					else powerValuesValid = refuse(text);
				}
			}
			if constexpr (requires { entry.supplyBusIds; })
				if (powerValuesValid && (!std::isfinite(entry.transferReturnDwellSeconds)
					|| entry.transferReturnDwellSeconds < 0.0
					|| static_cast<unsigned>(entry.supplyMode) > static_cast<unsigned>(StationSupplyMode::Shared)))
					powerValuesValid = refuse("Station '" + entry.id + "' supply mode and return dwell must be valid, finite and non-negative");
		});
		if (!powerValuesValid) return false;
		// #848: every catalogue machine validates its own named Wear
		// opt-in against the same model rules. None of these modes counts
		// cycles, so a cycle rate is refused; `mass` says whether the mode
		// meters throughput in kilograms at all.
		auto validateMachineWear = [&](auto const& machines, auto const& optIn, bool mass) {
			for (auto const& machine : machines)
			{
				auto const& model = machine.*optIn;
				if (model)
					if (auto const reason = wearModelViolation(*model, false, mass))
						return refuse("Installation '" + machine.id + "'" + reason);
			}
			return true;
		};
		if (!validateMachineWear(settings.solarArrays, &SolarArraySettings::cellDegradation, false)) return false;
		if (!validateMachineWear(settings.generators, &GeneratorSettings::fouledInjector, false)) return false;
		if (!validateMachineWear(settings.heatEngines, &HeatEngineSettings::efficiencyDrift, false)) return false;
		if (!validateMachineWear(settings.electrolyzers, &ElectrolyzerSettings::membraneDegradation, true)) return false;
		if (!validateMachineWear(settings.waterTreatments, &WaterTreatmentSettings::membraneFouling, true)) return false;
		if (!validateMachineWear(settings.co2Captures, &Co2CaptureSettings::absorbentDegradation, true)) return false;
		if (!validateMachineWear(settings.fuelCells, &FuelCellSettings::stackDegradation, true)) return false;
		if (!validateMachineWear(settings.sabatierReactors, &SabatierReactorSettings::catalystDeactivation, true)) return false;
		if (!validateMachineWear(settings.methaneProcessors, &MethaneProcessorSettings::catalystDeactivation, true)) return false;

		if (settings.systemsStepWorldTicks < 1 || settings.systemsStepWorldTicks > CORE_MAX_SYSTEMS_STEP_WORLD_TICKS)
			return refuse("Station systemsStepWorldTicks " + std::to_string(settings.systemsStepWorldTicks)
				+ " is outside allowed range [1, " + std::to_string(CORE_MAX_SYSTEMS_STEP_WORLD_TICKS) + "]");
		// Draft-tolerant mode (#642): with a brokenReferences list supplied,
		// an unresolvable reference is recorded and the entry carrying it
		// stops being validated, so the draft opens with the reference shown
		// as broken instead of the whole configuration being refused. Every
		// other rule keeps its full strictness; without the list (the World
		// path) this behaves exactly as refuse.
		auto missing = [&](std::string text) -> bool
		{
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto const validateGates = [&](auto const& entries)
		{
			for (auto const& entry : entries)
			{
				auto const& gate = entry.waterQuality;
				if (gate.maxTotalDissolved && (!std::isfinite(*gate.maxTotalDissolved) || *gate.maxTotalDissolved < 0.0))
					return refuse("Station '" + entry.id + "' water quality total tolerance must be finite and non-negative");
				for (auto const& [name, value] : gate.maxConcentrations)
					if (std::find(StationWaterDryConstituents.begin(), StationWaterDryConstituents.end(), name)
						== StationWaterDryConstituents.end() || !std::isfinite(value) || value < 0.0)
						return refuse("Station '" + entry.id + "' water quality tolerance '" + name + "' must name a dry constituent and be finite and non-negative");
			}
			return true;
		};
		if (!validateGates(settings.crews) || !validateGates(settings.microbialProcessors)
			|| !validateGates(settings.cropPopulations) || !validateGates(settings.algaeCultures)) return false;
		for (auto const& [key, properties] : settings.connections)
		{
			if (key.ownerType.empty() || key.ownerId.empty() || key.reference.empty() || key.relation.empty()
				|| key.peerType.empty() || key.peerId.empty()
				|| static_cast<size_t>(properties.physicalType) >= StationPhysicalConnectionNames.size()
				|| !std::isfinite(properties.maximumThroughput) || properties.maximumThroughput <= 0)
				return refuse("Station connection properties require a valid type and positive finite maximum throughput");
			if (!std::isfinite(properties.efficiency) || properties.efficiency < 0 || properties.efficiency > 1)
				return refuse("Station connection efficiency must be finite, from 0 to 1 inclusive");
		}
		if (!std::isfinite(settings.observationIntervalSeconds)
			|| settings.observationIntervalSeconds < 0.1f || settings.observationIntervalSeconds > 60.0f)
			return refuse("Station observation interval must be finite, from 0.1 to 60 seconds");
		if (!std::isfinite(settings.trendRetentionSeconds)
			|| settings.trendRetentionSeconds < 1.0f || settings.trendRetentionSeconds > 86400.0f)
			return refuse("Station trend retention must be finite, from 1 to 86400 seconds");
		if (settings.stores.size() > MaxStationResourceStores)
			return refuse("Station resource stores are limited to "
				+ std::to_string(MaxStationResourceStores) + " entries");
		std::set<std::string> ids;
		for (auto const& store : settings.stores)
		{
			auto const where = "Station resource store '" + store.id + "'";
			if (store.id.empty()) return refuse("Station resource store id must not be empty");
			if (store.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource store id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!ids.insert(store.id).second) return refuse(where + " id is duplicated");
			if (!isValidStationMaterialId(store.materialId))
				return refuse(where + " material identity must be empty (unclassified) or at most 64 characters: "
					"lowercase ASCII words/digits starting with a letter, separated by single hyphens");
			if (std::find(StationStoreUnits.begin(), StationStoreUnits.end(), store.unit)
				== StationStoreUnits.end())
				return refuse(where + " has unknown unit '" + store.unit
					+ "'; declared units are: " + declaredUnitsList());
			if (!std::isfinite(store.capacity) || store.capacity <= 0.0)
				return refuse(where + " capacity must be finite and positive");
			if (!std::isfinite(store.startingQualified) || !std::isfinite(store.startingUnqualified))
				return refuse(where + " starting inventory must be finite");
			if (store.startingQualified < 0.0 || store.startingUnqualified < 0.0)
				return refuse(where + " starting inventory must not be negative");
			if (store.startingQualified + store.startingUnqualified > store.capacity)
				return refuse(where + " starting inventory exceeds capacity");
			if (!std::isfinite(store.protectedReserve) || store.protectedReserve < 0.0)
				return refuse(where + " protected reserve must be finite and non-negative");
			if (store.protectedReserve > store.startingQualified)
				return refuse(where + " protected reserve exceeds starting usable inventory");
			if (store.composition)
			{
				auto const& composition = *store.composition;
				if (!isMassUnit(store.unit))
					return refuse(where + " declares a composition but unit '" + store.unit
						+ "' is not a mass unit; compound materials are mass-denominated (g or kg)");
				if (!std::isfinite(composition.moisture)
					|| composition.moisture < 0.0 || composition.moisture >= 1.0)
					return refuse(where + " moisture fraction must be finite and from 0 up to but below 1");
				auto const dryFractions = {composition.nutrientNitrogen,
					composition.atmosphericNitrogen, composition.phosphorus,
					composition.potassium, composition.otherMinerals};
				for (auto const fraction : dryFractions)
					if (!std::isfinite(fraction) || fraction < 0.0)
						return refuse(where + " dry-mass fractions must all be finite and non-negative");
				auto const drySum = composition.nutrientNitrogen + composition.atmosphericNitrogen
					+ composition.phosphorus + composition.potassium + composition.otherMinerals;
				if (drySum > 1.0)
					return refuse(where + " dry-mass fractions sum to " + std::to_string(drySum)
						+ ", exceeding the available dry mass");
				if (!std::isfinite(composition.energyKJPerKgDry) || composition.energyKJPerKgDry < 0.0)
					return refuse(where + " energy term must be finite and non-negative");
			}
		}
		if (settings.transfers.size() > MaxStationResourceTransfers)
			return refuse("Station resource transfers are limited to "
				+ std::to_string(MaxStationResourceTransfers) + " entries");
		std::set<std::string> transferIds;
		for (auto const& transfer : settings.transfers)
		{
			auto const where = "Station resource transfer '" + transfer.id + "'";
			if (transfer.id.empty()) return refuse("Station resource transfer id must not be empty");
			if (transfer.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource transfer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!transferIds.insert(transfer.id).second) return refuse(where + " id is duplicated");
			if (!validateStationTransferWiring(settings, transfer, diagnostic)) return false;
			if (!validateStationTransferSettings(transfer, diagnostic)) return false;
		}
		if (settings.conversions.size() > MaxStationResourceConversions)
			return refuse("Station resource conversions are limited to "
				+ std::to_string(MaxStationResourceConversions) + " entries");
		std::set<std::string> conversionIds;
		for (auto const& conversion : settings.conversions) try
		{
			auto const where = "Station resource conversion '" + conversion.id + "'";
			if (conversion.id.empty()) return refuse("Station resource conversion id must not be empty");
			if (conversion.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource conversion id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!conversionIds.insert(conversion.id).second) return refuse(where + " id is duplicated");
			if (conversion.inputs.empty() || conversion.inputs.size() > MaxStationConversionFlows)
				return refuse(where + " needs between 1 and "
					+ std::to_string(MaxStationConversionFlows) + " inputs");
			if (conversion.outputs.empty() || conversion.outputs.size() > MaxStationConversionFlows)
				return refuse(where + " needs between 1 and "
					+ std::to_string(MaxStationConversionFlows) + " outputs");
			std::set<std::string> endpoints;
			ConstituentLedger inflow, outflow;
			// Cross-class conversion endpoints are legal (#750): a
			// conversion whose input and output stores differ in hazard
			// class is accepted and produces its declared outputs.
			auto const addFlow = [&](std::string const& storeId, double rate, bool input)
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == storeId; });
				if (it == settings.stores.end())
					return missing(where + " references unknown " + (input ? "input" : "output")
						+ " store '" + storeId + "'");
				if (!endpoints.insert(storeId).second)
					return refuse(where + " uses store '" + storeId + "' more than once");
				if (!it->composition)
					return refuse(where + " " + (input ? "input" : "output") + " store '" + storeId
						+ "' has no declared composition; conversions require compound stores");
				if (!std::isfinite(rate) || rate <= 0.0)
					return refuse(where + " " + (input ? "input rate" : "output yield")
						+ " for store '" + storeId + "' must be finite and positive");
				addComposition(input ? inflow : outflow, *it->composition,
					massToKg(it->unit, rate), 1.0);
				return true;
			};
			for (auto const& flow : conversion.inputs)
				if (!addFlow(flow.storeId, flow.ratePerSecond, true)) return false;
			for (auto const& flow : conversion.outputs)
				if (!addFlow(flow.storeId, flow.yieldPerSecond, false)) return false;
			// Constituent and energy balance at declared full throughput:
			// an impossible configured yield that would create a tracked
			// constituent is rejected here rather than allowed to create
			// material at runtime. Losses below the input are legal and are
			// reported as explicit losses by the runtime ledger.
			for (auto const& [constituent, produced] : outflow)
			{
				auto const supplied = inflow.count(constituent) ? inflow.at(constituent) : 0.0;
				if (produced > supplied + StationBalanceTolerance * std::max(1.0, std::abs(supplied)))
					return refuse(where + " would create constituent '" + constituent
						+ "': declared output " + std::to_string(produced)
						+ " exceeds declared input " + std::to_string(supplied));
			}
		} catch (BrokenDraftEntry const&) {}
		std::vector<std::string> contaminationIds;
		auto gatherIds = [&](auto const& entries) {
			for (auto const& entry : entries) contaminationIds.push_back(entry.id);
		};
		gatherIds(settings.generators); gatherIds(settings.coolantLoops); gatherIds(settings.chillers);
		gatherIds(settings.heatEngines); gatherIds(settings.radiators); gatherIds(settings.solarArrays);
		gatherIds(settings.batteries); gatherIds(settings.waterTreatments); gatherIds(settings.electrolyzers);
		gatherIds(settings.fuelCells); gatherIds(settings.sabatierReactors); gatherIds(settings.methaneProcessors);
		gatherIds(settings.atmospheres); gatherIds(settings.airlockCycles);
		auto uniqueContaminationId = [&](std::string const& id) {
			return std::count(contaminationIds.begin(), contaminationIds.end(), id) == 1;
		};
		for (auto const& air : settings.atmospheres)
			if (air.fire && !uniqueContaminationId(air.id))
				return refuse("Ambiguous fire installation id '" + air.id + "'");
		for (auto const& cycle : settings.airlockCycles)
			if ((cycle.interruptedCycle || cycle.interlockFailure) && !uniqueContaminationId(cycle.id))
				return refuse("Ambiguous airlock fault installation id '" + cycle.id + "'");
		auto validatePoisoning = [&](auto const& cells) {
			for (auto const& cell : cells) if (cell.catalystPoisoning)
			{
				auto const& model = *cell.catalystPoisoning;
				if (!uniqueContaminationId(cell.id) || !std::isfinite(model.triggerConcentration) || model.triggerConcentration < 0.0
					|| model.triggerConcentration >= 1.0 || !std::isfinite(model.wearPerHour)
					|| model.wearPerHour <= 0.0)
					return refuse("Invalid catalyst-poisoning parameters for '" + cell.id + "'");
			}
			return true;
		};
		if (!validatePoisoning(settings.electrolyzers) || !validatePoisoning(settings.fuelCells)) return false;
		for (auto const& treatment : settings.waterTreatments) if (treatment.breakthrough)
		{
			if (!uniqueContaminationId(treatment.id)) return refuse("Ambiguous breakthrough installation id");
			auto const outlet = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](auto const& store) { return store.id == treatment.treatedStoreId; });
			if (outlet != settings.stores.end() && !outlet->composition)
				return refuse("breakthrough requires a compound treated store to retain species");
		}
		if (settings.solarArrays.size() > MaxStationSolarArrays)
			return refuse("Station solar arrays are limited to "
				+ std::to_string(MaxStationSolarArrays) + " entries");
		std::set<std::string> solarIds;
		for (auto const& array : settings.solarArrays)
		{
			auto const where = "Station solar array '" + array.id + "'";
			if (array.id.empty()) return refuse("Station solar array id must not be empty");
			if (array.id.size() > MaxStationStoreIdLength)
				return refuse("Station solar array id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!solarIds.insert(array.id).second) return refuse(where + " id is duplicated");
			// Zero installed capacity is legal and disables the array; it is
			// never mistaken for a source that can carry a load.
			if (!std::isfinite(array.capacityKW) || array.capacityKW < 0.0)
				return refuse(where + " installed capacity must be finite and non-negative (0 disables the array)");
			if (!validateStationProfile(array.illumination, where + " illumination profile", diagnostic))
				return false;
			// #845: the solar fault opt-ins. A string loss that takes no
			// capacity is not a loss, and one that takes more than the
			// array has is not a fraction; a tracking failure that pins
			// the array at full availability is not a failure at all.
			if (array.stringLoss && (!std::isfinite(array.stringLoss->lossFraction)
				|| array.stringLoss->lossFraction <= 0.0 || array.stringLoss->lossFraction > 1.0))
				return refuse(where + " string-loss fraction must be finite and greater than 0 up to 1");
			if (array.trackingFailure && (!std::isfinite(array.trackingFailure->availability)
				|| array.trackingFailure->availability < 0.0 || array.trackingFailure->availability >= 1.0))
				return refuse(where + " tracking-failure availability must be finite and from 0 up to (but not including) 1");
		}
		if (settings.equipmentLoads.size() > MaxStationEquipmentLoads)
			return refuse("Station external equipment loads are limited to "
				+ std::to_string(MaxStationEquipmentLoads) + " entries");
		std::set<std::string> loadIds;
		for (auto const& load : settings.equipmentLoads) try
		{
			auto const where = "Station equipment load '" + load.id + "'";
			if (load.id.empty()) return refuse("Station equipment load id must not be empty");
			if (load.id.size() > MaxStationStoreIdLength)
				return refuse("Station equipment load id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!loadIds.insert(load.id).second) return refuse(where + " id is duplicated");
			if (!std::isfinite(load.electricalDemandKW) || load.electricalDemandKW < 0.0)
				return refuse(where + " electrical demand must be finite and non-negative");
			if (!std::isfinite(load.heatOutputKW) || load.heatOutputKW < 0.0)
				return refuse(where + " heat output must be finite and non-negative");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				load.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + load.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			if (load.coolingDisposition != StationCoolingDisposition::Exported
				&& load.coolingDisposition != StationCoolingDisposition::CoolingService)
				return refuse(where + " has an unknown cooling disposition");
			if (!std::isfinite(load.criticalFloorKW) || load.criticalFloorKW < 0.0)
				return refuse(where + " critical floor must be finite and non-negative");
			if (load.criticalFloorKW > load.electricalDemandKW)
				return refuse(where + " critical floor exceeds the declared electrical demand");
			if (!std::isfinite(load.maxCurtailment) || load.maxCurtailment < 0.0 || load.maxCurtailment > 1.0)
				return refuse(where + " allowed curtailment must be finite and from 0 to 1");
			// #417 recovery settings. All four default to zero (no recovery
			// protection, clean restart). When declared they must be finite
			// and within range; the hysteresis fraction is capped at 1 so a
			// restart never demands more than double the load's own demand as
			// spare headroom.
			if (!std::isfinite(load.recoveryDwellSeconds) || load.recoveryDwellSeconds < 0.0)
				return refuse(where + " recovery dwell must be finite and non-negative");
			if (!std::isfinite(load.recoveryHysteresisFraction) || load.recoveryHysteresisFraction < 0.0
				|| load.recoveryHysteresisFraction > 1.0)
				return refuse(where + " recovery hysteresis fraction must be finite and from 0 to 1");
			if (!std::isfinite(load.restartSurgePowerKW) || load.restartSurgePowerKW < 0.0)
				return refuse(where + " restart surge power must be finite and non-negative");
			if (!std::isfinite(load.restartSurgeSeconds) || load.restartSurgeSeconds < 0.0)
				return refuse(where + " restart surge seconds must be finite and non-negative");
			if (load.restartSurgeSeconds > 0.0 && load.restartSurgePowerKW <= 0.0)
				return refuse(where + " restart surge seconds are declared with no surge power");
			if (!validateStationProfile(load.operatingProfile, where + " operating profile", diagnostic))
				return false;
			if (load.materialExchanges.size() > MaxStationLoadMaterialExchanges)
				return refuse(where + " material exchanges are limited to "
					+ std::to_string(MaxStationLoadMaterialExchanges) + " entries");
			std::set<std::string> exchangeStores;
			for (auto const& exchange : load.materialExchanges)
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == exchange.storeId; });
				if (it == settings.stores.end())
					return missing(where + " material exchange references unknown store '" + exchange.storeId + "'");
				if (!exchangeStores.insert(exchange.storeId).second)
					return refuse(where + " exchanges with store '" + exchange.storeId + "' more than once");
				if (!std::isfinite(exchange.ratePerSecond) || exchange.ratePerSecond <= 0.0)
					return refuse(where + " material exchange rate for store '" + exchange.storeId
						+ "' must be finite and positive");
			}
			// Heat sink routing (#395): emitted heat goes to exactly one
			// declared reservoir, of the same thermal service class. Spare
			// capacity in another class is never usable, and a
			// cooling-service load needs a cooling service rather than a
			// passive sink.
			if (!load.heatSinkId.empty())
			{
				if (load.coolingDisposition != StationCoolingDisposition::Exported)
					return refuse(where + " declares a heat sink but requires a cooling service; a cooling-service load needs a cooling service of its class, not a heat sink");
				auto const sink = std::find_if(settings.thermalReservoirs.begin(),
					settings.thermalReservoirs.end(),
					[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == load.heatSinkId; });
				if (sink == settings.thermalReservoirs.end())
					return missing(where + " heat sink references unknown thermal reservoir '" + load.heatSinkId + "'");
				if (sink->thermalClass != load.thermalClass)
					return refuse(where + " thermal class '" + load.thermalClass
						+ "' is incompatible with heat sink reservoir '" + sink->id + "' of class '"
						+ sink->thermalClass + "'; heat routes only to the same service class");
			}
		} catch (BrokenDraftEntry const&) {}
		if (settings.batteries.size() > MaxStationBatteries)
			return refuse("Station batteries are limited to "
				+ std::to_string(MaxStationBatteries) + " entries");
		std::set<std::string> batteryIds;
		for (auto const& battery : settings.batteries)
		{
			auto const where = "Station battery '" + battery.id + "'";
			if (battery.id.empty()) return refuse("Station battery id must not be empty");
			if (battery.id.size() > MaxStationStoreIdLength)
				return refuse("Station battery id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!batteryIds.insert(battery.id).second) return refuse(where + " id is duplicated");
			if (!std::isfinite(battery.nameplateKWh) || battery.nameplateKWh < 0.0)
				return refuse(where + " nameplate energy must be finite and non-negative");
			// Usable energy is the cyclable slice above the reserve floor; it
			// can never exceed the installed nameplate, so no policy can
			// invent storage the battery does not have.
			if (!std::isfinite(battery.usableKWh) || battery.usableKWh < 0.0)
				return refuse(where + " usable energy must be finite and non-negative");
			if (battery.usableKWh > battery.nameplateKWh)
				return refuse(where + " usable energy exceeds the nameplate energy");
			if (!std::isfinite(battery.startingStoredKWh) || battery.startingStoredKWh < 0.0)
				return refuse(where + " starting stored energy must be finite and non-negative");
			if (battery.startingStoredKWh > battery.nameplateKWh)
				return refuse(where + " starting stored energy exceeds the nameplate energy");
			if (!std::isfinite(battery.maxChargeKW) || battery.maxChargeKW < 0.0)
				return refuse(where + " maximum charge power must be finite and non-negative");
			if (!std::isfinite(battery.maxDischargeKW) || battery.maxDischargeKW < 0.0)
				return refuse(where + " maximum discharge power must be finite and non-negative");
			if (!std::isfinite(battery.chargeEfficiency)
				|| battery.chargeEfficiency <= 0.0 || battery.chargeEfficiency > 1.0)
				return refuse(where + " charge efficiency must be finite and greater than 0 up to 1");
			if (!std::isfinite(battery.dischargeEfficiency)
				|| battery.dischargeEfficiency <= 0.0 || battery.dischargeEfficiency > 1.0)
				return refuse(where + " discharge efficiency must be finite and greater than 0 up to 1");
			// #843: the opt-in capacity-fade model. A battery cycles and
			// ages but flows no counted mass.
			if (battery.capacityFade)
				if (auto const reason = wearModelViolation(*battery.capacityFade, true, false))
					return refuse(where + reason);
			// #845: the thermal-runaway opt-in names a one-shot heat and
			// toxic-species booking that must land in an existing
			// compartment; both amounts must be finite and positive, so
			// the onset always books a real hazard and never a zero.
			if (battery.thermalRunaway)
			{
				auto const& runaway = *battery.thermalRunaway;
				if (!std::isfinite(runaway.heatKJ) || runaway.heatKJ <= 0.0)
					return refuse(where + " thermal-runaway heat must be finite and positive");
				if (!std::isfinite(runaway.toxicKg) || runaway.toxicKg <= 0.0)
					return refuse(where + " thermal-runaway toxic species mass must be finite and positive");
				if (std::none_of(settings.atmospheres.begin(), settings.atmospheres.end(),
					[&](auto const& air) { return air.id == runaway.compartmentId; }))
					return missing(where + " thermal-runaway references unknown compartment '"
						+ runaway.compartmentId + "'");
			}
		}
		if (settings.thermalReservoirs.size() > MaxStationThermalReservoirs)
			return refuse("Station thermal reservoirs are limited to "
				+ std::to_string(MaxStationThermalReservoirs) + " entries");
		std::set<std::string> reservoirIds;
		for (auto const& reservoir : settings.thermalReservoirs)
		{
			auto const where = "Station thermal reservoir '" + reservoir.id + "'";
			if (reservoir.id.empty()) return refuse("Station thermal reservoir id must not be empty");
			if (reservoir.id.size() > MaxStationStoreIdLength)
				return refuse("Station thermal reservoir id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!reservoirIds.insert(reservoir.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				reservoir.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + reservoir.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			if (!std::isfinite(reservoir.heatCapacityKJPerK) || reservoir.heatCapacityKJPerK <= 0.0)
				return refuse(where + " effective thermal capacity must be finite and positive");
			if (!std::isfinite(reservoir.minTemperatureC) || !std::isfinite(reservoir.maxTemperatureC)
				|| reservoir.minTemperatureC >= reservoir.maxTemperatureC)
				return refuse(where + " temperature limits must be finite with min below max");
			if (!std::isfinite(reservoir.startingTemperatureC))
				return refuse(where + " starting temperature must be finite");
			// Temperatures below absolute zero describe no material; refuse
			// them rather than let the integration wander into nonsense.
			if (reservoir.minTemperatureC <= -StationCelsiusOffset
				|| reservoir.maxTemperatureC <= -StationCelsiusOffset
				|| reservoir.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " temperatures must be above absolute zero (-273.15 C)");
			if (reservoir.startingTemperatureC < reservoir.minTemperatureC
				|| reservoir.startingTemperatureC > reservoir.maxTemperatureC)
				return refuse(where + " starting temperature is outside its declared limits");
			if (!std::isfinite(reservoir.absorbedHeatKW) || reservoir.absorbedHeatKW < 0.0)
				return refuse(where + " absorbed heat must be finite and non-negative");
		}
		if (settings.radiators.size() > MaxStationRadiators)
			return refuse("Station radiators are limited to "
				+ std::to_string(MaxStationRadiators) + " entries");
		std::set<std::string> radiatorIds;
		for (auto const& radiator : settings.radiators) try
		{
			auto const where = "Station radiator '" + radiator.id + "'";
			if (radiator.id.empty()) return refuse("Station radiator id must not be empty");
			if (radiator.id.size() > MaxStationStoreIdLength)
				return refuse("Station radiator id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!radiatorIds.insert(radiator.id).second) return refuse(where + " id is duplicated");
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == radiator.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return missing(where + " is attached to unknown thermal reservoir '" + radiator.reservoirId + "'");
			if (!std::isfinite(radiator.areaM2) || radiator.areaM2 <= 0.0)
				return refuse(where + " area must be finite and positive");
			if (!std::isfinite(radiator.emissivity) || radiator.emissivity <= 0.0 || radiator.emissivity > 1.0)
				return refuse(where + " emissivity must be finite and greater than 0 up to 1");
			if (!std::isfinite(radiator.viewFactor) || radiator.viewFactor <= 0.0 || radiator.viewFactor > 1.0)
				return refuse(where + " view factor must be finite and greater than 0 up to 1");
			if (!std::isfinite(radiator.sinkTemperatureC)
				|| radiator.sinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " sink temperature must be finite and above absolute zero (-273.15 C)");
			// #844: the panel puncture opt-in names a finite panel charge
			// that bleeds into an existing compartment, and the repair-parts
			// wiring may plan only for the puncture mode this kind owns.
			if (radiator.panelPuncture)
			{
				auto const& leak = *radiator.panelPuncture;
				if (!validateChargeLeakBlock(leak.compartmentId, leak.coolantKg,
					leak.leakRateKgPerS, leak.specificHeatKJPerKgK, "panel-puncture", settings))
					return refuse(where + " panel puncture needs a finite positive charge,"
						" bleed rate and specific heat, and an existing destination compartment");
			}
			if (radiator.repairParts)
			{
				if (!validateRepairPartsWiring(*radiator.repairParts, where, "a radiator", settings,
					{"panel-puncture"}, refuse, missing)) return false;
			}
		} catch (BrokenDraftEntry const&) {}
		if (settings.coolantLoops.size() > MaxStationCoolantLoops)
			return refuse("Station coolant loops are limited to "
				+ std::to_string(MaxStationCoolantLoops) + " entries");
		std::set<std::string> loopIds;
		for (auto const& loop : settings.coolantLoops) try
		{
			auto const where = "Station coolant loop '" + loop.id + "'";
			if (loop.id.empty()) return refuse("Station coolant loop id must not be empty");
			if (loop.id.size() > MaxStationStoreIdLength)
				return refuse("Station coolant loop id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!loopIds.insert(loop.id).second) return refuse(where + " id is duplicated");
			// A loop couples heat-only to one declared thermal reservoir.
			// Coolant never touches a Resource store: it is not potable or
			// nutrient water, and the loop's inventory is its own.
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == loop.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return missing(where + " is coupled to unknown thermal reservoir '" + loop.reservoirId + "'");
			if (!std::isfinite(loop.coolantCapacityKg) || loop.coolantCapacityKg <= 0.0)
				return refuse(where + " coolant capacity must be finite and positive");
			if (!std::isfinite(loop.startingCoolantKg) || loop.startingCoolantKg < 0.0)
				return refuse(where + " starting coolant must be finite and non-negative");
			if (loop.coolantLeak)
			{
				auto const& leak = *loop.coolantLeak;
				if (!std::isfinite(leak.releasedKg) || leak.releasedKg <= 0.0)
					return refuse(where + " coolant leak releasedKg must be finite and positive");
				if (std::none_of(settings.atmospheres.begin(), settings.atmospheres.end(),
					[&](auto const& air) { return air.id == leak.compartmentId; }))
					return missing(where + " coolant leak references unknown compartment '" + leak.compartmentId + "'");
			}
			// #840: the Condition model is authored whole or not at all, and
			// every rate, threshold and funding reference is checked here so
			// the runtime never meets a model it cannot honour.
			if (loop.condition)
			{
				auto const& model = *loop.condition;
				if (!std::isfinite(model.initialCondition) || model.initialCondition < 0.0 || model.initialCondition > 1.0)
					return refuse(where + " initial Condition must be finite and from 0 to 1");
				if (!std::isfinite(model.overheatStressC) || model.overheatStressC <= -StationCelsiusOffset)
					return refuse(where + " overheat stress temperature must be finite and above absolute zero (-273.15 C)");
				if (!std::isfinite(model.overheatDegradePerHour) || model.overheatDegradePerHour < 0.0)
					return refuse(where + " overheat degradation rate must be finite and non-negative");
				if (!std::isfinite(model.cavitationFillFraction) || model.cavitationFillFraction < 0.0 || model.cavitationFillFraction > 1.0)
					return refuse(where + " cavitation fill fraction must be finite and from 0 to 1");
				if (!std::isfinite(model.cavitationDegradePerHour) || model.cavitationDegradePerHour < 0.0)
					return refuse(where + " cavitation degradation rate must be finite and non-negative");
				if (!std::isfinite(model.seizureThreshold) || model.seizureThreshold < 0.0 || model.seizureThreshold > 1.0)
					return refuse(where + " seizure threshold must be finite and from 0 to 1");
				if (!std::isfinite(model.restorePerHour) || model.restorePerHour < 0.0)
					return refuse(where + " restore rate must be finite and non-negative");
				if (!std::isfinite(model.stockUnitsPerConditionPoint) || model.stockUnitsPerConditionPoint < 0.0)
					return refuse(where + " restore stock cost per Condition point must be finite and non-negative");
				// A restore that can heal nothing is not a funded restore;
				// refusing it here keeps a commanded restore from silently
				// stalling forever at zero rate.
				if (!model.restoreStoreId.empty()
					&& (model.restorePerHour <= 0.0 || model.stockUnitsPerConditionPoint <= 0.0))
					return refuse(where + " a funded restore needs a positive restore rate and stock cost per Condition point");
				if (!model.restoreStoreId.empty() && std::none_of(settings.stores.begin(), settings.stores.end(),
					[&](auto const& store) { return store.id == model.restoreStoreId; }))
					return missing(where + " Condition restore references unknown service stock '" + model.restoreStoreId + "'");
				// A model that can never spawn its Fault is dead weight, but a
				// machine starting below the threshold would have spawned on
				// a tick that already passed; refuse the incoherent start.
				if (model.initialCondition < model.seizureThreshold)
					return refuse(where + " initial Condition must not start below its seizure threshold");
			}
			// #842: repair-parts wiring names the store holding this loop's
			// spare parts and the per-mode repair plans that draw from it.
			// A plan is legal only for a repairable coolant-loop fault mode,
			// and its consumed-parts stream must land in a declared waste
			// store that can actually receive it: same unit and same
			// declared composition, as for scheduled maintenance (#414).
			if (loop.repairParts)
			{
				if (!validateRepairPartsWiring(*loop.repairParts, where, "a coolant loop", settings,
					{"coolant-leak", "pump-seizure"}, refuse, missing)) return false;
			}
			if (loop.startingCoolantKg > loop.coolantCapacityKg)
				return refuse(where + " starting coolant exceeds the loop capacity");
			if (!std::isfinite(loop.specificHeatKJPerKgK) || loop.specificHeatKJPerKgK <= 0.0)
				return refuse(where + " specific heat must be finite and positive");
			if (!std::isfinite(loop.startingTemperatureC)
				|| loop.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " starting temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(loop.freezeProtectionSetpointC)
				|| loop.freezeProtectionSetpointC <= -StationCelsiusOffset)
				return refuse(where + " freeze-protection setpoint must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(loop.pumpPowerKW) || loop.pumpPowerKW < 0.0)
				return refuse(where + " pump power must be finite and non-negative");
			if (!std::isfinite(loop.ratedFlowKgPerS) || loop.ratedFlowKgPerS < 0.0)
				return refuse(where + " rated flow must be finite and non-negative");
			if (!std::isfinite(loop.exchangerConductanceKWPerK) || loop.exchangerConductanceKWPerK < 0.0)
				return refuse(where + " exchanger conductance must be finite and non-negative");
			if (!std::isfinite(loop.heaterCapacityKW) || loop.heaterCapacityKW < 0.0)
				return refuse(where + " heater capacity must be finite and non-negative");
			// #843: the opt-in Wear model. A loop operates and moves mass
			// but is not cycle-counted.
			if (loop.wear)
				if (auto const reason = wearModelViolation(*loop.wear, false, true))
					return refuse(where + reason);
		} catch (BrokenDraftEntry const&) {}
		// Chiller contracts (#397): the cold-side interface is a declared
		// coolant loop and the hot-side sink a declared thermal reservoir,
		// both of the chiller's own service class — spare capacity in
		// another class is never usable, and a chiller that rejects its
		// condenser heat into the very reservoir its cold side cools is a
		// heater, not a cooling service, so it is refused rather than
		// silently circulating heat in a circle.
		if (settings.chillers.size() > MaxStationChillers)
			return refuse("Station chillers are limited to "
				+ std::to_string(MaxStationChillers) + " entries");
		std::set<std::string> chillerIds;
		std::set<std::string> chilledLoops;
		for (auto const& chiller : settings.chillers) try
		{
			auto const where = "Station chiller '" + chiller.id + "'";
			if (chiller.id.empty()) return refuse("Station chiller id must not be empty");
			if (chiller.id.size() > MaxStationStoreIdLength)
				return refuse("Station chiller id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!chillerIds.insert(chiller.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				chiller.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + chiller.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			auto const loop = std::find_if(settings.coolantLoops.begin(),
				settings.coolantLoops.end(),
				[&](CoolantLoopSettings const& candidate) { return candidate.id == chiller.coldLoopId; });
			if (loop == settings.coolantLoops.end())
				return missing(where + " cold-side interface references unknown coolant loop '"
					+ chiller.coldLoopId + "'");
			if (!chilledLoops.insert(chiller.coldLoopId).second)
				return refuse(where + " shares cold loop '" + chiller.coldLoopId
					+ "' with another chiller; one evaporator serves one chiller");
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == chiller.condenserReservoirId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " condenser sink references unknown thermal reservoir '"
					+ chiller.condenserReservoirId + "'");
			// Class compatibility on both interfaces: the cold loop's host
			// reservoir and the condenser sink must each declare the
			// service class the chiller supplies.
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == loop->reservoirId; });
			if (host->thermalClass != chiller.thermalClass)
				return refuse(where + " thermal class '" + chiller.thermalClass
					+ "' is incompatible with cold loop '" + loop->id + "' serving reservoir '"
					+ host->id + "' of class '" + host->thermalClass
					+ "'; cold services are class-matched");
			if (sink->thermalClass != chiller.thermalClass)
				return refuse(where + " thermal class '" + chiller.thermalClass
					+ "' is incompatible with condenser sink reservoir '" + sink->id
					+ "' of class '" + sink->thermalClass + "'; heat routes only to the same service class");
			if (sink->id == host->id)
				return refuse(where + " rejects its condenser heat into reservoir '" + sink->id
					+ "', the same reservoir its cold side cools; that is a heater, not a cooling service");
			if (!std::isfinite(chiller.ratedCoolingCapacityKW) || chiller.ratedCoolingCapacityKW < 0.0)
				return refuse(where + " rated cooling capacity must be finite and non-negative");
			if (!std::isfinite(chiller.cop) || chiller.cop <= 0.0)
				return refuse(where + " COP must be finite and positive");
			if (!std::isfinite(chiller.auxiliaryPowerKW) || chiller.auxiliaryPowerKW < 0.0)
				return refuse(where + " auxiliary power must be finite and non-negative");
			if (!std::isfinite(chiller.chilledSetpointC)
				|| chiller.chilledSetpointC <= -StationCelsiusOffset)
				return refuse(where + " chilled setpoint must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(chiller.maxCondensingTemperatureC)
				|| chiller.maxCondensingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum condensing temperature must be finite and above absolute zero (-273.15 C)");
			if (chiller.chilledSetpointC >= chiller.maxCondensingTemperatureC)
				return refuse(where + " chilled setpoint must be below the maximum condensing temperature");
			// #844: the refrigerant leak opt-in names a finite charge that
			// bleeds into an existing compartment, and the repair-parts
			// wiring may plan only for the modes this kind owns.
			if (chiller.refrigerantLeak)
			{
				auto const& leak = *chiller.refrigerantLeak;
				if (!validateChargeLeakBlock(leak.compartmentId, leak.chargeKg,
					leak.leakRateKgPerS, leak.specificHeatKJPerKgK, "refrigerant-leak", settings))
					return refuse(where + " refrigerant leak needs a finite positive charge,"
						" bleed rate and specific heat, and an existing destination compartment");
			}
			if (chiller.repairParts)
			{
				if (!validateRepairPartsWiring(*chiller.repairParts, where, "a chiller", settings,
					{"compressor-failure", "refrigerant-leak"}, refuse, missing)) return false;
			}
		} catch (BrokenDraftEntry const&) {}
		// Heat engine contracts (#804): a Carnot-bounded heat-to-power
		// crossing between two declared thermal reservoirs. The second law
		// is enforced at authoring time, not rediscovered at runtime: a
		// carnotFraction outside (0, 1] is an over-unity machine and is
		// refused as the authoring error it is. The gates must admit a
		// positive-gradient window: a config whose minimum hot gate sits at
		// or below its maximum sink gate can never run legally, and is
		// refused rather than shipped dead. Every refusal names the
		// offending entry and leaves the World unmutated.
		if (settings.heatEngines.size() > MaxStationHeatEngines)
			return refuse("Station heat engines are limited to "
				+ std::to_string(MaxStationHeatEngines) + " entries");
		std::set<std::string> heatEngineIds;
		for (auto const& engine : settings.heatEngines) try
		{
			auto const where = "Station heat engine '" + engine.id + "'";
			if (engine.id.empty()) return refuse("Station heat engine id must not be empty");
			if (engine.id.size() > MaxStationStoreIdLength)
				return refuse("Station heat engine id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!heatEngineIds.insert(engine.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				engine.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + engine.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			// A single-temperature engine is a perpetual-motion machine:
			// refuse the same-reservoir pair before anything else resolves.
			if (!engine.hotReservoirId.empty() && engine.hotReservoirId == engine.rejectionReservoirId)
				return refuse(where + " hot and rejection endpoints resolve to the same reservoir '"
					+ engine.hotReservoirId + "'; a single-temperature engine is a perpetual-motion machine");
			auto const hot = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == engine.hotReservoirId; });
			if (hot == settings.thermalReservoirs.end())
				return missing(where + " hot source references unknown thermal reservoir '"
					+ engine.hotReservoirId + "'");
			auto const rejection = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == engine.rejectionReservoirId; });
			if (rejection == settings.thermalReservoirs.end())
				return missing(where + " rejection sink references unknown thermal reservoir '"
					+ engine.rejectionReservoirId + "'");
			// The rejection heat carries the engine's service class, and the
			// rejection reservoir must accept exactly that class: spare
			// capacity in another class is never usable, as for every other
			// machine's sink.
			if (rejection->thermalClass != engine.thermalClass)
				return refuse(where + " thermal class '" + engine.thermalClass
					+ "' is incompatible with rejection reservoir '" + rejection->id
					+ "' of class '" + rejection->thermalClass + "'; heat routes only to the same service class");
			if (!std::isfinite(engine.ratedThermalInputKW) || engine.ratedThermalInputKW < 0.0)
				return refuse(where + " rated thermal input must be finite and non-negative");
			// The intake dispatch is a cap in [0, rated] (#805): a command
			// below zero is nonsense and a command above rated is a request
			// for intake the machine was never built to draw. Both are
			// authoring errors, refused by name with the World unmutated.
			if (!std::isfinite(engine.commandedInputKW) || engine.commandedInputKW < 0.0)
				return refuse(where + " commanded input must be finite and non-negative");
			if (engine.commandedInputKW > engine.ratedThermalInputKW)
				return refuse(where + " commanded input "
					+ std::to_string(engine.commandedInputKW)
					+ " kW exceeds rated thermal input "
					+ std::to_string(engine.ratedThermalInputKW)
					+ " kW; the command caps intake, it can never exceed the rating");
			// The second law as a validation error: a fraction above 1 is
			// an over-unity machine, and one at or below zero converts
			// nothing however live the gradient. Both are authoring errors.
			if (!std::isfinite(engine.carnotFraction) || engine.carnotFraction <= 0.0
				|| engine.carnotFraction > 1.0)
				return refuse(where + " carnotFraction must be finite and greater than 0 up to 1");
			if (!std::isfinite(engine.auxiliaryPowerKW) || engine.auxiliaryPowerKW < 0.0)
				return refuse(where + " auxiliary power must be finite and non-negative");
			if (!std::isfinite(engine.minHotTemperatureC)
				|| engine.minHotTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum hot temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(engine.maxSinkTemperatureC)
				|| engine.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero (-273.15 C)");
			// A dead gradient window: with the minimum hot gate at or below
			// the maximum sink gate there is no state in which the gates
			// alone guarantee a live gradient, so the config can never run
			// legally and is refused rather than shipped dead.
			if (engine.minHotTemperatureC <= engine.maxSinkTemperatureC)
				return refuse(where + " minimum hot temperature must be above the maximum sink temperature:"
					" the gates must admit a positive-gradient window");
			// #844: the engine's repair-parts wiring may plan only for the
			// mechanical-seizure mode this kind owns.
			if (engine.repairParts)
			{
				if (!validateRepairPartsWiring(*engine.repairParts, where, "a heat engine", settings,
					{"mechanical-seizure"}, refuse, missing)) return false;
			}
		} catch (BrokenDraftEntry const&) {}
		// Thermal buffer contracts (#398): the buffer couples to a declared
		// host reservoir; a circulation-access buffer additionally names a
		// coolant loop coupled to that same host, because the buffer's
		// transfer path runs through the loop's circulation and a gate on
		// a different reservoir would let a blackout on one side absorb
		// through the other. The store is finite and its starting energy
		// sits inside its capacity; the usable temperature band is finite,
		// above absolute zero and properly ordered; the rate limits are
		// finite and non-negative with at least one direction alive; a
		// passive buffer must declare a positive transfer conductance.
		if (settings.thermalBuffers.size() > MaxStationThermalBuffers)
			return refuse("Station thermal buffers are limited to "
				+ std::to_string(MaxStationThermalBuffers) + " entries");
		std::set<std::string> thermalBufferIds;
		for (auto const& buffer : settings.thermalBuffers) try
		{
			auto const where = "Station thermal buffer '" + buffer.id + "'";
			if (buffer.id.empty()) return refuse("Station thermal buffer id must not be empty");
			if (buffer.id.size() > MaxStationStoreIdLength)
				return refuse("Station thermal buffer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!thermalBufferIds.insert(buffer.id).second) return refuse(where + " id is duplicated");
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == buffer.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return missing(where + " references unknown host thermal reservoir '" + buffer.reservoirId + "'");
			if (std::find(StationThermalBufferAccessModes.begin(),
				StationThermalBufferAccessModes.end(), buffer.accessMode)
				== StationThermalBufferAccessModes.end())
				return refuse(where + " has unknown access mode '" + buffer.accessMode
					+ "'; declared modes are: circulation, passive");
			if (buffer.accessMode == "circulation")
			{
				auto const gate = std::find_if(settings.coolantLoops.begin(),
					settings.coolantLoops.end(),
					[&](CoolantLoopSettings const& loop) { return loop.id == buffer.circulationLoopId; });
				if (gate == settings.coolantLoops.end())
					return missing(where + " circulation access references unknown coolant loop '"
						+ buffer.circulationLoopId + "'");
				if (gate->reservoirId != buffer.reservoirId)
					return refuse(where + " circulation loop '" + gate->id
						+ "' is coupled to reservoir '" + gate->reservoirId
						+ "', not the buffer's host reservoir '" + buffer.reservoirId + "'");
			}
			else
			{
				if (!std::isfinite(buffer.transferConductanceKWPerK)
					|| buffer.transferConductanceKWPerK <= 0.0)
					return refuse(where + " passive transfer conductance must be finite and positive");
			}
			if (!std::isfinite(buffer.capacityKJ) || buffer.capacityKJ <= 0.0)
				return refuse(where + " capacity must be finite and positive");
			if (!std::isfinite(buffer.startingEnergyKJ) || buffer.startingEnergyKJ < 0.0
				|| buffer.startingEnergyKJ > buffer.capacityKJ)
				return refuse(where + " starting energy must be finite and within capacity");
			if (!std::isfinite(buffer.minUsableTemperatureC)
				|| buffer.minUsableTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum usable temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(buffer.maxUsableTemperatureC)
				|| buffer.maxUsableTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum usable temperature must be finite and above absolute zero (-273.15 C)");
			if (buffer.minUsableTemperatureC >= buffer.maxUsableTemperatureC)
				return refuse(where + " usable temperature band needs the minimum below the maximum");
			if (!std::isfinite(buffer.maxChargeKW) || buffer.maxChargeKW < 0.0)
				return refuse(where + " charge rate must be finite and non-negative");
			if (!std::isfinite(buffer.maxDischargeKW) || buffer.maxDischargeKW < 0.0)
				return refuse(where + " discharge rate must be finite and non-negative");
			if (buffer.maxChargeKW <= 0.0 && buffer.maxDischargeKW <= 0.0)
				return refuse(where + " that can neither charge nor discharge is not a buffer");
		} catch (BrokenDraftEntry const&) {}
		// Generator contracts (#399): the fuel inventory is an energy-unit
		// Resource store, the waste-heat sink a declared thermal reservoir
		// of the generator's own service class - spare capacity in another
		// class is never usable - and the conversion, ramp, startup and
		// shutdown-heat terms are all finite and physically ordered.
		if (settings.generators.size() > MaxStationGenerators)
			return refuse("Station generators are limited to "
				+ std::to_string(MaxStationGenerators) + " entries");
		std::set<std::string> generatorIds;
		for (auto const& generator : settings.generators) try
		{
			auto const where = "Station generator '" + generator.id + "'";
			if (generator.id.empty()) return refuse("Station generator id must not be empty");
			if (generator.id.size() > MaxStationStoreIdLength)
				return refuse("Station generator id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!generatorIds.insert(generator.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				generator.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + generator.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			// Fuel is energy the generator can burn: the store must be
			// denominated in an energy unit so the fuel ledger closes in
			// the same energy terms as the electrical output.
			auto const fuel = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == generator.fuelStoreId; });
			if (fuel == settings.stores.end())
				return missing(where + " fuel inventory references unknown Resource store '"
					+ generator.fuelStoreId + "'");
			if (fuel->unit != "kJ" && fuel->unit != "MJ" && fuel->unit != "Wh" && fuel->unit != "kWh")
				return refuse(where + " fuel store unit '" + fuel->unit
					+ "' is not an energy unit; fuelled generators burn kJ, MJ, Wh or kWh");
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == generator.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " heat sink references unknown thermal reservoir '"
					+ generator.heatSinkId + "'");
			if (sink->thermalClass != generator.thermalClass)
				return refuse(where + " thermal class '" + generator.thermalClass
					+ "' is incompatible with heat sink reservoir '" + sink->id
					+ "' of class '" + sink->thermalClass + "'; heat routes only to the same service class");
			// Zero installed capacity is legal and disables the generator;
			// it is never mistaken for a source that can carry a load.
			if (!std::isfinite(generator.ratedOutputKW) || generator.ratedOutputKW < 0.0)
				return refuse(where + " installed capacity must be finite and non-negative (0 disables the generator)");
			if (!std::isfinite(generator.efficiency)
				|| generator.efficiency <= 0.0 || generator.efficiency > 1.0)
				return refuse(where + " conversion efficiency must be finite and greater than 0 up to 1");
			// A generator that cannot change its output at all could never
			// reach any commanded level, so the ramp limit must be positive.
			if (!std::isfinite(generator.maxRampKWPerSecond) || generator.maxRampKWPerSecond <= 0.0)
				return refuse(where + " ramp limit must be finite and positive");
			if (!std::isfinite(generator.commandedOutputKW) || generator.commandedOutputKW < 0.0)
				return refuse(where + " commanded output must be finite and non-negative");
			if (generator.commandedOutputKW > generator.ratedOutputKW)
				return refuse(where + " commanded output exceeds the installed capacity");
			if (!std::isfinite(generator.startupPowerKW) || generator.startupPowerKW < 0.0)
				return refuse(where + " startup power must be finite and non-negative");
			if (!std::isfinite(generator.startupSeconds) || generator.startupSeconds < 0.0)
				return refuse(where + " startup duration must be finite and non-negative");
			if (!std::isfinite(generator.shutdownHeatKW) || generator.shutdownHeatKW < 0.0)
				return refuse(where + " shutdown heat must be finite and non-negative");
			if (!std::isfinite(generator.shutdownSeconds) || generator.shutdownSeconds < 0.0)
				return refuse(where + " shutdown heat duration must be finite and non-negative");
			if (!std::isfinite(generator.maxSinkTemperatureC)
				|| generator.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero (-273.15 C)");
		} catch (BrokenDraftEntry const&) {}
		if (settings.deliveries.size() > MaxStationResourceDeliveries)
			return refuse("Station resupply deliveries are limited to "
				+ std::to_string(MaxStationResourceDeliveries) + " entries");
		std::set<std::string> deliveryIds;
		for (auto const& delivery : settings.deliveries) try
		{
			auto const where = "Station resupply delivery '" + delivery.id + "'";
			if (delivery.id.empty()) return refuse("Station resupply delivery id must not be empty");
			if (delivery.id.size() > MaxStationStoreIdLength)
				return refuse("Station resupply delivery id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!deliveryIds.insert(delivery.id).second) return refuse(where + " id is duplicated");
			std::string wiringReason;
			if (!validateStationScheduleWiring(settings, delivery, wiringReason, false, brokenReferences))
				return refuse(wiringReason);
			// A delivery that brings nothing specifies nothing; refusing it
			// keeps every fired occurrence a real inflow.
			if (!std::isfinite(delivery.quantity) || delivery.quantity <= 0.0)
				return refuse(where + " quantity must be finite and positive");
			if (!std::isfinite(delivery.firstOccurrenceSeconds)
				|| delivery.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			// Zero is a one-off; a recurring schedule must not repeat faster
			// than one World fixed tick, so no tick is ever asked to absorb an
			// unbounded burst of catch-up arrivals.
			if (!std::isfinite(delivery.recurrenceSeconds) || delivery.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (delivery.recurrenceSeconds > 0.0
				&& delivery.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(StationScheduleQuantumSeconds))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(StationScheduleQuantumSeconds) + " seconds)");
		} catch (BrokenDraftEntry const&) {}
		// Terminal-waste ejection schedules (#403): a scenario removal of
		// disposal-ready (qualified) material from one waste store at
		// authored times. The same schedule discipline as resupply
		// deliveries governs them; what makes them different is direction:
		// ejection can only remove what the store actually holds as
		// eligible, so it never needs a capacity-shaped rule, but it must
		// still name a real store in that store's own unit.
		if (settings.wasteEjections.size() > MaxStationWasteEjections)
			return refuse("Station waste ejections are limited to "
				+ std::to_string(MaxStationWasteEjections) + " entries");
		std::set<std::string> ejectionIds;
		for (auto const& ejection : settings.wasteEjections) try
		{
			auto const where = "Station waste ejection '" + ejection.id + "'";
			if (ejection.eligibility != WasteEjectionEligibility::QualifiedOnly
				&& ejection.eligibility != WasteEjectionEligibility::HeldOnly
				&& ejection.eligibility != WasteEjectionEligibility::QualifiedAndHeld)
				return refuse(where + " eligibility must be qualifiedOnly, heldOnly or qualifiedAndHeld");
			if (ejection.id.empty()) return refuse("Station waste ejection id must not be empty");
			if (ejection.id.size() > MaxStationStoreIdLength)
				return refuse("Station waste ejection id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!ejectionIds.insert(ejection.id).second) return refuse(where + " id is duplicated");
			std::string wiringReason;
			if (!validateStationScheduleWiring(settings, ejection, wiringReason, false, brokenReferences))
				return refuse(wiringReason);
			// An ejection that removes nothing specifies nothing; refusing
			// it keeps every fired occurrence a real outflow.
			if (!std::isfinite(ejection.quantity) || ejection.quantity <= 0.0)
				return refuse(where + " quantity must be finite and positive");
			if (!std::isfinite(ejection.firstOccurrenceSeconds)
				|| ejection.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			// Zero is a one-off; a recurring schedule must not repeat faster
			// than one World fixed tick, so no tick is ever asked to absorb
			// an unbounded burst of catch-up ejections.
			if (!std::isfinite(ejection.recurrenceSeconds) || ejection.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (ejection.recurrenceSeconds > 0.0
				&& ejection.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(StationScheduleQuantumSeconds))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(StationScheduleQuantumSeconds) + " seconds)");
		} catch (BrokenDraftEntry const&) {}
		// Installation outage and capacity-reduction schedules (#413): a
		// scenario change to one registered installation's available rated
		// capacity at authored times. The outage must name an installation
		// that has registered with the shared availability contract, so a
		// typo or an unwired target is refused rather than silently doing
		// nothing. The capacity fraction is the share of the rating that
		// stays available during the window: 0 is a full outage, a value
		// below 1 is a reduction; a fraction of 1 changes nothing and is
		// refused so every fired occurrence is a real reduction. The
		// window must be positive and, when recurring, no shorter than the
		// window itself, so a schedule's own windows never overlap.
		if (settings.outages.size() > MaxStationInstallationOutages)
			return refuse("Station installation outages are limited to "
				+ std::to_string(MaxStationInstallationOutages) + " entries");
		auto const registeredInstallations = stationInstallationIdentities(settings);
		// The shared installation namespace must be unambiguous (#576):
		// an outage names its target by installationId alone, so two
		// installations of different kinds sharing an id would leave the
		// contract unable to say which machine it caps - and resolution
		// would silently pick whichever kind it consults first. Refuse
		// the collision at the crossing, transactionally, so no file or
		// editor draft carrying it can ever be applied.
		std::map<std::string, std::string> installedKindById;
		for (auto const& installation : registeredInstallations)
		{
			auto const [existing, inserted] = installedKindById.emplace(
				installation.id, installation.kind);
			if (!inserted)
				return refuse("Station installation id '" + installation.id
					+ "' is ambiguous: registered as both a " + existing->second
					+ " and a " + installation.kind
					+ "; installation ids must be unique across generators, coolant loops, chillers and heat engines");
		}
		std::set<std::string> outageIds;
		for (auto const& outage : settings.outages) try
		{
			auto const where = "Station installation outage '" + outage.id + "'";
			if (outage.id.empty()) return refuse("Station installation outage id must not be empty");
			if (outage.id.size() > MaxStationStoreIdLength)
				return refuse("Station installation outage id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!outageIds.insert(outage.id).second) return refuse(where + " id is duplicated");
			if (outage.installationId.empty() && !brokenReferences)
				return refuse(where + " installation id must not be empty");
			if (outage.installationId.size() > MaxStationStoreIdLength)
				return refuse(where + " installation id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			std::string wiringReason;
			if (!validateStationScheduleWiring(settings, outage, wiringReason, false, brokenReferences))
				return refuse(wiringReason);
			if (!std::isfinite(outage.capacityFraction) || outage.capacityFraction < 0.0
				|| outage.capacityFraction >= 1.0)
				return refuse(where + " capacity fraction must be finite and from 0 up to but not including 1"
					+ " (0 is a full outage; 1 would change nothing)");
			if (!std::isfinite(outage.firstOccurrenceSeconds)
				|| outage.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			if (!std::isfinite(outage.durationSeconds) || outage.durationSeconds <= 0.0)
				return refuse(where + " duration must be finite and positive");
			if (!std::isfinite(outage.recurrenceSeconds) || outage.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (outage.recurrenceSeconds > 0.0
				&& outage.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(StationScheduleQuantumSeconds))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(StationScheduleQuantumSeconds) + " seconds)");
			if (outage.recurrenceSeconds > 0.0
				&& outage.recurrenceSeconds + StationScheduleTolerance < outage.durationSeconds)
				return refuse(where + " recurrence must be at least the outage duration so its windows never overlap");
		} catch (BrokenDraftEntry const&) {}
		// Authored scheduled maintenance jobs (#414): timed work whose
		// completion must be paid for with declared tools, spare parts and
		// consumables. The target must be a registered installation, every
		// requirement must name an existing store with a positive finite
		// quantity, and the consumed stream must land in a declared waste
		// store that can actually hold it: same unit (cross-unit movement
		// is conversion, not maintenance) and same declared composition (a
		// compound boundary is never crossed silently). A reusable tool may come from anywhere - it is
		// returned, not moved - but a consumed requirement may not draw
		// from the waste store itself, and a waste store with nothing to
		// fill it is refused rather than carried as dead configuration.
		if (settings.maintenanceJobs.size() > MaxStationMaintenanceJobs)
			return refuse("Station maintenance jobs are limited to "
				+ std::to_string(MaxStationMaintenanceJobs) + " entries");
		std::set<std::string> maintenanceIds;
		for (auto const& job : settings.maintenanceJobs) try
		{
			auto const where = "Station maintenance job '" + job.id + "'";
			if (job.id.empty()) return refuse("Station maintenance job id must not be empty");
			if (job.id.size() > MaxStationStoreIdLength)
				return refuse("Station maintenance job id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!maintenanceIds.insert(job.id).second) return refuse(where + " id is duplicated");
			if (job.installationId.empty() && !brokenReferences)
				return refuse(where + " installation id must not be empty");
			if (job.installationId.size() > MaxStationStoreIdLength)
				return refuse(where + " installation id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			std::string wiringReason;
			if (!validateStationScheduleWiring(settings, job, wiringReason, false, brokenReferences))
				return refuse(wiringReason);
			if (!std::isfinite(job.firstOccurrenceSeconds)
				|| job.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			if (!std::isfinite(job.durationSeconds) || job.durationSeconds <= 0.0)
				return refuse(where + " duration must be finite and positive");
			if (!std::isfinite(job.recurrenceSeconds) || job.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (job.recurrenceSeconds > 0.0
				&& job.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(StationScheduleQuantumSeconds))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(StationScheduleQuantumSeconds) + " seconds)");
			if (job.recurrenceSeconds > 0.0
				&& job.recurrenceSeconds + StationScheduleTolerance < job.durationSeconds)
				return refuse(where + " recurrence must be at least the maintenance duration so its windows never overlap");
			if (job.requirements.empty())
				return refuse(where + " needs at least one declared tool/part/consumable requirement; maintenance without prerequisites would complete for free");
			if (job.requirements.size() > MaxStationMaintenanceRequirements)
				return refuse(where + " requirements are limited to "
					+ std::to_string(MaxStationMaintenanceRequirements) + " entries");

		} catch (BrokenDraftEntry const&) {}
		// Compartment atmosphere contracts (#400): a fixed volume with a
		// finite owned gas inventory, an authored effective thermal
		// capacity, an optional heat-only reservoir coupling and an
		// authored occupancy/load assignment. Only explicit pathways exchange
		// gas between compartments; exterior pathways, leaks and airlock cycles book external loss.
		if (settings.atmospheres.size() > MaxStationAtmospheres)
			return refuse("Station atmospheres are limited to "
				+ std::to_string(MaxStationAtmospheres) + " entries");
		std::set<std::string> atmosphereIds;
		for (auto const& atmosphere : settings.atmospheres) try
		{
			if (!atmosphereIds.insert(atmosphere.id).second) return refuse("Station atmosphere '" + atmosphere.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, atmosphere, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		if (settings.atmospherePathways.size() > MaxStationPathways)
			return refuse("Station atmosphere pathways are limited to "
				+ std::to_string(MaxStationPathways) + " entries");
		std::set<std::string> pathwayIds;
		for (auto const& pathway : settings.atmospherePathways) try
		{
			auto const where = "Station atmosphere pathway '" + pathway.id + "'";
			if (pathway.id.empty() || pathway.id.size() > MaxStationStoreIdLength)
				return refuse(where + " id must be non-empty and at most "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!pathwayIds.insert(pathway.id).second)
				return refuse(where + " id is duplicated");
			if (!std::isfinite(pathway.areaM2) || pathway.areaM2 <= 0.0)
				return refuse(where + " areaM2 must be finite and positive");
			if (!std::isfinite(pathway.openFraction)
				|| pathway.openFraction < 0.0 || pathway.openFraction > 1.0)
				return refuse(where + " openFraction must be finite and within [0, 1]");
			// A hatch with both ends unwired is an opening being built, not a
			// self-loop: the distinctness rule is about two named compartments,
			// and two empty strings name nothing. Each empty end is refused on
			// its own below as an unresolved reference, which the draft layer
			// treats as an unconnected connector.
			if (!pathway.aAtmosphereId.empty() && pathway.aAtmosphereId == pathway.bAtmosphereId)
				return refuse(where + " endpoints must be distinct (self-loop)");
			for (auto const& endpoint : {pathway.aAtmosphereId, pathway.bAtmosphereId})
				if (endpoint != StationExteriorAtmosphereId && !atmosphereIds.contains(endpoint))
				{
					auto text = where + " references unknown atmosphere compartment '" + endpoint + "'";
					if (!brokenReferences) return refuse(std::move(text));
					brokenReferences->push_back(std::move(text));
					throw BrokenDraftEntry{};
				}
		} catch (BrokenDraftEntry const&) {}
		// Thermal link contracts (#794): an undirected conduction pair with
		// a declared conductance. Zero conductance can never pass anything,
		// which is incoherent rather than merely imprudent - it matches the
		// zero-area pathway refusal. Exterior vacuum is refused as an
		// endpoint: it has no temperature to conduct against, and heat loss
		// to space is a radiative T^4 law that already lives on the
		// radiator, not a linear conduction boundary.
		if (settings.thermalLinks.size() > MaxStationThermalLinks)
			return refuse("Station thermal links are limited to "
				+ std::to_string(MaxStationThermalLinks) + " entries");
		std::set<std::string> thermalLinkIds;
		using NodeKey = std::pair<ThermalNodeKind, std::string>;
		std::set<std::pair<NodeKey, NodeKey>> thermalPairs;
		for (auto const& link : settings.thermalLinks) try
		{
			auto const where = "Station thermal link '" + link.id + "'";
			if (link.id.empty() || link.id.size() > MaxStationStoreIdLength)
				return refuse(where + " id must be non-empty and at most "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!thermalLinkIds.insert(link.id).second)
				return refuse(where + " id is duplicated");
			if (!std::isfinite(link.conductanceKWPerK) || link.conductanceKWPerK <= 0.0)
				return refuse(where + " conductanceKWPerK must be finite and positive");
			auto a = NodeKey{link.aKind, link.aAtmosphereId};
			auto b = NodeKey{link.bKind, link.bAtmosphereId};
			if (!a.second.empty() && a == b)
				return refuse(where + " endpoints must be distinct (self-loop)");
			if (b < a) std::swap(a, b);
			for (auto const& [kind, endpoint] : {a, b})
			{
				if (kind != ThermalNodeKind::Atmosphere && kind != ThermalNodeKind::Reservoir
					&& kind != ThermalNodeKind::MachineBody)
					return refuse(where + " endpoint kind is unsupported");
				if (kind == ThermalNodeKind::Atmosphere && endpoint == StationExteriorAtmosphereId)
					return refuse(where + " may not use Exterior vacuum as an endpoint: "
						"it has no temperature to conduct against");
				double capacity = 0.0;
				bool resolved = false;
				if (kind == ThermalNodeKind::Atmosphere)
					for (auto const& node : settings.atmospheres)
						if (node.id == endpoint) { capacity = node.thermalCapacityKJPerK; resolved = true; break; }
				if (kind == ThermalNodeKind::Reservoir)
					for (auto const& node : settings.thermalReservoirs)
						if (node.id == endpoint) { capacity = node.heatCapacityKJPerK; resolved = true; break; }
				if (kind == ThermalNodeKind::MachineBody)
					for (auto const& node : settings.electrolyzers)
						if (node.id == endpoint)
						{
							if (!node.thermalBody) return refuse(where + " machine endpoint '" + endpoint + "' has no thermal body");
							capacity = node.thermalBody->heatCapacityKJPerK; resolved = true; break;
						}
				if (!resolved)
				{
					auto text = where + " references unknown thermal node '" + endpoint + "'";
					if (!brokenReferences) return refuse(std::move(text));
					brokenReferences->push_back(std::move(text));
					throw BrokenDraftEntry{};
				}
				if (!std::isfinite(capacity) || capacity <= 0.0)
					return refuse(where + " endpoint must have finite positive thermal capacity");
			}
			// Unconnected draft ends are broken references, not duplicate
			// physical pairs. Only resolved pairs participate in uniqueness.
			if (!thermalPairs.insert({a, b}).second)
				return refuse(where + " duplicates an undirected thermal node pair");
		} catch (BrokenDraftEntry const&) {}
		// Scheduled airlock cycle contracts (#400): each cycle draws from
		// one declared compartment on the shared scenario schedule rule
		// and splits its removal into recovered and lost by the authored
		// recovery fraction.
		if (settings.airlockCycles.size() > MaxStationAirlockCycles)
			return refuse("Station airlock cycles are limited to "
				+ std::to_string(MaxStationAirlockCycles) + " entries");
		std::set<std::string> airlockCycleIds;
		for (auto const& cycle : settings.airlockCycles) try
		{
			if (!airlockCycleIds.insert(cycle.id).second) return refuse("Station airlock cycle '" + cycle.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, cycle, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		// Water recovery contracts (#402). A collector condenses vapour out
		// of one compartment onto a coolant-loop cold surface; a treatment
		// turns raw water into treated water plus retained concentrate. A
		// collector may serve a compartment of any class (#752): condensate
		// is just water, whatever air it condensed from, and the heat-only
		// exchanger through which no coolant ever crossed is the containment.
		// The stores the collector and the treatment touch are no longer
		// class-bound (#751), so a process may be wired to any store
		// whatever class it carries.
		// Treatment streams may be compound (#740): dry load is
		// retained in concentrate, while treated output is pure water. Other
		// water processes retain their existing plain-store contracts.
		if (settings.waterCollectors.size() > MaxStationWaterCollectors)
			return refuse("Station water collectors are limited to "
				+ std::to_string(MaxStationWaterCollectors) + " entries");
		if (settings.waterTreatments.size() > MaxStationWaterTreatments)
			return refuse("Station water treatments are limited to "
				+ std::to_string(MaxStationWaterTreatments) + " entries");
		// Unlike an entry-level unwind, an unresolved store ends only that
		// endpoint's check, so other resolved endpoints still undergo validation.
		auto missingStore = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			return true;
		};
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missingStore(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			return true;
		};
		std::set<std::string> waterIds;
		for (auto const& collector : settings.waterCollectors) try
		{
			if (!waterIds.insert(collector.id).second) return refuse("Station water collector '" + collector.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, collector, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		for (auto const& treatment : settings.waterTreatments) try
		{
			if (!waterIds.insert(treatment.id).second) return refuse("Station water treatment '" + treatment.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, treatment, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		// Oxygen production and CO2 capture contracts (#404). An
		// electrolyzer splits qualified water into oxygen and hydrogen: the
		// split is stoichiometric, so the authored electrical dose must at
		// least cover the chemical energy the produced hydrogen retains -
		// a dose below it would create energy-rich products out of less
		// energy than they carry, which no machine can do. Exactly one
		// oxygen destination must be configured: a qualified gas store or
		// one compartment atmosphere, never both and never neither. All gas
		// and water stores in the chain are plain mass stores: a compound
		// store would create dry constituents out of pure water or pure
		// gas. The heat sink must share the cell's thermal service
		// class, because spare capacity in one class is
		// never usable by another. The stores the cell touches are no
		// longer bound to the cell's class (#751). A CO2 capture machine
		// scrubs one compartment into a finite capture store;
		// if it declares a consumable demand it must name a consumable
		// store, because a process cannot spend media it has no way to
		// hold.
		if (settings.electrolyzers.size() > MaxStationElectrolyzers)
			return refuse("Station electrolyzers are limited to "
				+ std::to_string(MaxStationElectrolyzers) + " entries");
		if (settings.co2Captures.size() > MaxStationCo2Captures)
			return refuse("Station CO2 capture machines are limited to "
				+ std::to_string(MaxStationCo2Captures) + " entries");
		if (settings.fuelCells.size() > MaxStationFuelCells)
			return refuse("Station fuel cells are limited to "
				+ std::to_string(MaxStationFuelCells) + " entries");
		auto declareGasStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missingStore(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; gas handling is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
			return true;
		};
		std::set<std::string> gasIds;
		for (auto const& electrolyzer : settings.electrolyzers) try
		{
			if (!gasIds.insert(electrolyzer.id).second) return refuse("Station electrolyzer '" + electrolyzer.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, electrolyzer, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		for (auto const& capture : settings.co2Captures) try
		{
			if (!gasIds.insert(capture.id).second) return refuse("Station CO2 capture machine '" + capture.id + "' id is duplicated");
			std::string reason;
			if (!validateStationWaterGasWiring(settings, capture, reason, false, brokenReferences))
				return refuse(reason);
		} catch (BrokenDraftEntry const&) {}
		// Fuel-cell backup contracts (#406). A cell generates electrical
		// power from two dedicated reactant stores and can only ever see
		// its own stores, so protected breathing oxygen is never drawn
		// implicitly: refilling the oxygen store is a controlled surplus
		// transfer that itself preserves the supplying service's protected
		// reserve. Efficiency is electrical out per chemical energy in and
		// must sit above zero and at most one - a cell delivering more
		// electrical energy than the reactants' chemical energy is a
		// perpetual-motion claim, not a machine. The dispatch setpoint may
		// not exceed the installed rating. All three stores in the chain
		// are plain mass stores; none of them is bound to the cell's own
		// hazard class (#751), and the heat sink must share the cell's
		// thermal service class.
		for (auto const& cell : settings.fuelCells) try
		{
			auto const where = "Station fuel cell '" + cell.id + "'";
			if (cell.id.empty()) return refuse("Station fuel cell id must not be empty");
			if (cell.id.size() > MaxStationStoreIdLength)
				return refuse("Station fuel cell id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(cell.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				cell.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + cell.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(cell.ratedOutputKW) || cell.ratedOutputKW < 0.0)
				return refuse(where + " rated electrical output must be finite and non-negative");
			if (!std::isfinite(cell.efficiency) || cell.efficiency <= 0.0 || cell.efficiency > 1.0)
				return refuse(where + " efficiency must be greater than 0 and at most 1: a cell cannot deliver more electrical energy than the reactants' chemical energy");
			if (!std::isfinite(cell.commandedOutputKW) || cell.commandedOutputKW < 0.0
				|| cell.commandedOutputKW > cell.ratedOutputKW)
				return refuse(where + " commanded output must be finite and within the installed rating");
			if (!std::isfinite(cell.maxSinkTemperatureC)
				|| cell.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			// The same aliased-role rule the electrolyzer carries (#585):
			// the cell draws each reactant from its own store's drawable
			// stock, so one store named for both reactants spends the same
			// stock twice against a stoichiometry that expects two.
			if (!cell.hydrogenStoreId.empty() && cell.hydrogenStoreId == cell.oxygenStoreId)
				return refuse(where + " names the same store '" + cell.hydrogenStoreId
					+ "' for both the hydrogen and the oxygen reactant: the cell draws both reactants independently and may not spend one store's stock as both");
			if (!declareGasStore(cell.hydrogenStoreId, "hydrogen reactant", where, cell.catalystPoisoning.has_value()))
				return false;
			if (!declareGasStore(cell.oxygenStoreId, "oxygen reactant", where, false))
				return false;
			if (!declareWaterStore(cell.waterStoreId, "product water", where, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == cell.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " heat sink is unknown thermal reservoir '" + cell.heatSinkId + "'");
			if (sink->thermalClass != cell.thermalClass)
				return refuse(where + " thermal class '" + cell.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		} catch (BrokenDraftEntry const&) {}
		// Carbon recovery contracts (#405). A Sabatier reactor converts
		// captured CO2 and hydrogen into methane and water; the conversion
		// is stoichiometric under the declared molar masses, so the mass
		// ledger closes by construction. All four stores in the chain are
		// plain mass stores; none of them is bound to the reactor's own
		// hazard class (#751), and the heat sink must share the reactor's
		// thermal service class.
		// A methane processor splits methane into recovered hydrogen and
		// solid carbon; the split is stoichiometric, and because the
		// reaction is endothermic the authored electrical dose must at
		// least cover the declared reaction heat demand - a dose below it
		// would drive a reaction the machine cannot heat. The recovered
		// hydrogen returns to the hydrogen store as recycled feed: the
		// Sabatier stage consumes twice as much hydrogen per methane as
		// the processor returns, so the combined loop retains a net
		// hydrogen demand and the return is never free continuing supply.
		auto tripIdentities = stationInstallationIdentities(settings);
		for (auto const& s : settings.sabatierReactors) tripIdentities.push_back({s.id, "Sabatier"});
		for (auto const& s : settings.methaneProcessors) tripIdentities.push_back({s.id, "Methane processor"});
		auto validTrip = [&](auto const& s) {
			return !s.overtempRunaway || (std::count_if(tripIdentities.begin(), tripIdentities.end(),
				[&](auto const& i) { return i.id == s.id; }) == 1
				&& std::isfinite(s.overtempRunaway->triggerTemperatureC)
				&& s.overtempRunaway->triggerTemperatureC > -273.15
				&& std::isfinite(s.overtempRunaway->heatSpikeKJ) && s.overtempRunaway->heatSpikeKJ > 0.0);
		};
		for (auto const& s : settings.sabatierReactors)
			if (!validTrip(s)) return refuse("Invalid Sabatier overtemp-runaway parameters");
		for (auto const& s : settings.methaneProcessors)
			if (!validTrip(s)) return refuse("Invalid methane processor overtemp-runaway parameters");
		if (settings.sabatierReactors.size() > MaxStationSabatierReactors)
			return refuse("Station Sabatier reactors are limited to "
				+ std::to_string(MaxStationSabatierReactors) + " entries");
		if (settings.methaneProcessors.size() > MaxStationMethaneProcessors)
			return refuse("Station methane processors are limited to "
				+ std::to_string(MaxStationMethaneProcessors) + " entries");
		for (auto const& reactor : settings.sabatierReactors) try
		{
			auto const where = "Station Sabatier reactor '" + reactor.id + "'";
			if (reactor.id.empty()) return refuse("Station Sabatier reactor id must not be empty");
			if (reactor.id.size() > MaxStationStoreIdLength)
				return refuse("Station Sabatier reactor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(reactor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				reactor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + reactor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(reactor.rateKgPerS) || reactor.rateKgPerS < 0.0)
				return refuse(where + " rated CO2 conversion must be finite and non-negative");
			if (!std::isfinite(reactor.specificEnergyKWhPerKgCO2)
				|| reactor.specificEnergyKWhPerKgCO2 < 0.0)
				return refuse(where + " specific energy must be finite and non-negative");
			if (!std::isfinite(reactor.maxSinkTemperatureC)
				|| reactor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			// Aliased stream roles are refused (#599), the same rule the
			// electrolyzer and the fuel cell carry (#585): the four streams
			// are chemically distinct, and the tick sizes each product
			// against its own store's headroom before either is added, so
			// one store named for two product roles has the same headroom
			// spent twice and breaks the finite-capacity audit on the very
			// first tick. Feed/feed and feed/product aliases are refused on
			// the same principle even where no capacity error is reached:
			// one inventory may not stand for two different substances the
			// ledger books separately. Every pair of the reactor's own
			// stream roles must name a distinct store; sharing between
			// different machines - one reactor's methane feeding a
			// processor - is untouched.
			std::pair<char const*, std::string const*> const reactorRoles[] = {
				{"CO2 feed", &reactor.co2StoreId},
				{"hydrogen feed", &reactor.hydrogenStoreId},
				{"methane product", &reactor.methaneStoreId},
				{"product water", &reactor.waterStoreId},
			};
			for (size_t left = 0; left < 4; ++left)
				for (size_t right = left + 1; right < 4; ++right)
					if (!reactorRoles[left].second->empty()
						&& *reactorRoles[left].second == *reactorRoles[right].second)
						return refuse(where + " names the same store '"
							+ *reactorRoles[left].second + "' for the "
							+ reactorRoles[left].first + " and the "
							+ reactorRoles[right].first
							+ ": chemically distinct streams may not share one store");
			if (!declareGasStore(reactor.co2StoreId, "CO2 feed", where, false))
				return false;
			if (!declareGasStore(reactor.hydrogenStoreId, "hydrogen feed", where, false))
				return false;
			if (!declareGasStore(reactor.methaneStoreId, "methane", where, false))
				return false;
			if (!declareWaterStore(reactor.waterStoreId, "product water", where, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == reactor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " heat sink is unknown thermal reservoir '" + reactor.heatSinkId + "'");
			if (sink->thermalClass != reactor.thermalClass)
				return refuse(where + " thermal class '" + reactor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		} catch (BrokenDraftEntry const&) {}
		for (auto const& processor : settings.methaneProcessors) try
		{
			auto const where = "Station methane processor '" + processor.id + "'";
			if (processor.id.empty()) return refuse("Station methane processor id must not be empty");
			if (processor.id.size() > MaxStationStoreIdLength)
				return refuse("Station methane processor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(processor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				processor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + processor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(processor.rateKgPerS) || processor.rateKgPerS < 0.0)
				return refuse(where + " rated methane processing must be finite and non-negative");
			if (!std::isfinite(processor.specificEnergyKWhPerKgCH4)
				|| processor.specificEnergyKWhPerKgCH4 * 3600.0
					< StationMethaneProcessingReactionHeatKJPerKgMethane
					- StationBalanceTolerance * std::max(1.0,
						StationMethaneProcessingReactionHeatKJPerKgMethane))
				return refuse(where + " specific energy is below the endothermic reaction heat the split must absorb: the electrical dose may not fund less than the reaction demands");
			if (!std::isfinite(processor.maxSinkTemperatureC)
				|| processor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			// The same aliased-role rule as the Sabatier reactor (#599):
			// the methane feed, the recovered hydrogen and the solid
			// carbon are three chemically distinct streams, and the tick
			// sizes each product against its own store's headroom before
			// either is added, so one store named for two roles spends the
			// same headroom twice. One inventory standing for two booked
			// substances is refused whatever the fill levels.
			std::pair<char const*, std::string const*> const processorRoles[] = {
				{"methane feed", &processor.methaneStoreId},
				{"recovered hydrogen", &processor.hydrogenStoreId},
				{"solid carbon", &processor.carbonStoreId},
			};
			for (size_t left = 0; left < 3; ++left)
				for (size_t right = left + 1; right < 3; ++right)
					if (!processorRoles[left].second->empty()
						&& *processorRoles[left].second == *processorRoles[right].second)
						return refuse(where + " names the same store '"
							+ *processorRoles[left].second + "' for the "
							+ processorRoles[left].first + " and the "
							+ processorRoles[right].first
							+ ": chemically distinct streams may not share one store");
			if (!declareGasStore(processor.methaneStoreId, "methane feed", where, false))
				return false;
			if (!declareGasStore(processor.hydrogenStoreId, "recovered hydrogen", where, false))
				return false;
			if (!declareGasStore(processor.carbonStoreId, "solid carbon", where, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == processor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return missing(where + " heat sink is unknown thermal reservoir '" + processor.heatSinkId + "'");
			if (sink->thermalClass != processor.thermalClass)
				return refuse(where + " thermal class '" + processor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		} catch (BrokenDraftEntry const&) {}
		// Nutrient recovery contracts (#407). A microbial processor digests
		// a compound organic-waste feedstock and recovers its bound
		// nutrients per constituent: the recovery yields are fractions of
		// the feed's own bound nitrogen, phosphorus, potassium and grouped
		// mineral content, so the recovered nutrients are funded by the
		// feedstock alone. Atmospheric nitrogen in the feed is never a
		// nutrient source - no configured process in this slice converts
		// it, so it passes through to the residue and can never
		// substitute for bound N. The feed must declare a composition:
		// there is nothing to recover from an uncharacterised stream, and
		// the product stores (nutrients, residue) must be plain mass
		// stores, because a compound destination would create tracked
		// constituents out of nothing. The residue rate is the
		// mass-balance remainder of the declared inputs and outputs per
		// kilogram of feed: a configuration whose declared outputs would
	// exceed its inputs is refused rather than gifting mass. The
		// exothermic reaction heat is capped by the feedstock's declared
		// chemical energy per kilogram of feed - the released heat is
		// paid for by the feed, never free - and the operating window
		// must be a real window: minimum below maximum, both above
		// absolute zero.
		if (!validateStationBiologyWiring(settings, diagnostic, false, brokenReferences)) return false;
		diagnostic.clear(); return true;
	}
	bool validateStationBiologyWiring(StationSystemsSettings const& settings, std::string& diagnostic,
		bool allowUnwired, std::vector<std::string>* brokenReferences)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		auto missing = [&](std::string text) -> bool {
			if (!brokenReferences) return refuse(std::move(text));
			brokenReferences->push_back(std::move(text));
			throw BrokenDraftEntry{};
		};
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowUnwired && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			return true;
		};
		auto declareGasStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool allowCompound) -> bool
		{
			if (allowUnwired && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; gas handling is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
			return true;
		};
		// Crew contracts (#401): the crew is an authored population and
		// activity assignment, never derived from World Agents. All four
		// matter stores must be compound mass stores so the declared
		// compositions can carry the nutrient/element closure: at full
		// activity the emitted mass (CO2 + moisture + urine + organic
		// waste) may not exceed the consumed mass (food + water +
		// oxygen); every tracked constituent returned in the urine and
		// organic-waste streams (plus respired moisture) must be funded
		// by the consumed food and water; and the declared metabolic
		// heat may not exceed the declared food energy minus the energy
		// still bound in the waste streams. A crew that would create
		// mass, constituents or energy out of nothing is refused before
		// anything runs; a crew whose returns fall short of its
		// consumption retains the difference as explicit body mass,
		// reported in the ledger rather than discarded.
		if (settings.crews.size() > MaxStationCrews)
			return refuse("Station crews are limited to "
				+ std::to_string(MaxStationCrews) + " entries");
		std::set<std::string> crewIds;
		for (auto const& crew : settings.crews) try
		{
			auto const where = "Station crew '" + crew.id + "'";
			if (crew.id.empty()) return refuse("Station crew id must not be empty");
			if (crew.id.size() > MaxStationStoreIdLength)
				return refuse("Station crew id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!crewIds.insert(crew.id).second) return refuse(where + " id is duplicated");
			auto const habitat = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == crew.atmosphereId; });
			if (habitat == settings.atmospheres.end() && !(allowUnwired && crew.atmosphereId.empty()))
				return missing(where + " breathes in unknown atmosphere compartment '" + crew.atmosphereId + "'");
			if (crew.crewCount < 0) return refuse(where + " crew count must not be negative");
			if (!validateStationProfile(crew.activityProfile, where + " activity profile", diagnostic))
				return false;
			auto const rates = {crew.foodPerCrewPerS, crew.waterPerCrewPerS,
				crew.oxygenPerCrewKgPerS, crew.co2PerCrewKgPerS, crew.moisturePerCrewKgPerS,
				crew.urinePerCrewPerS, crew.organicWastePerCrewPerS, crew.metabolicHeatPerCrewKW};
			for (auto const rate : rates)
				if (!std::isfinite(rate) || rate < 0.0)
					return refuse(where + " metabolic rates must be finite and non-negative");
			ConstituentLedger greywater;
			double greywaterLoad = 0.0;
			if (!std::isfinite(crew.greywaterKgPerCrewDay) || crew.greywaterKgPerCrewDay < 0.0)
				return refuse(where + " greywater mass must be finite and non-negative");
			for (auto const& [name, mass] : crew.greywaterLoadKgPerCrewDay)
			{
				if ((name != "dryOrganics" && name != "nutrientNitrogen") || !std::isfinite(mass) || mass < 0.0)
					return refuse(where + " greywater load '" + name + "' must be finite, non-negative dissolved organics or nitrogen");
				greywater[name] = mass / 86400.0;
				greywaterLoad += mass;
			}
			if (greywaterLoad > crew.greywaterKgPerCrewDay)
				return refuse(where + " greywater dissolved load exceeds total greywater mass");
			greywater["water"] = (crew.greywaterKgPerCrewDay - greywaterLoad) / 86400.0;
			if (crew.greywaterKgPerCrewDay > 0.0 && crew.greywaterStoreId.empty() && !allowUnwired)
				return refuse(where + " greywater requires a destination store");
			if (!crew.greywaterStoreId.empty())
			{
				if (!declareWaterStore(crew.greywaterStoreId, "greywater", where, true)) return false;
				auto const destination = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](auto const& s) { return s.id == crew.greywaterStoreId; });
				if (destination != settings.stores.end() && !destination->composition)
					return refuse(where + " greywater destination must be compound");
				for (auto const& id : {crew.foodStoreId, crew.waterStoreId, crew.urineStoreId, crew.organicWasteStoreId})
					if (id == crew.greywaterStoreId) return refuse(where + " greywater needs a distinct store");
			}
			ResourceStoreSettings const* streamStores[4]{nullptr, nullptr, nullptr, nullptr};
			char const* streamRoles[4]{"food", "water", "urine", "organic waste"};
			std::string const streamIds[4]{crew.foodStoreId, crew.waterStoreId,
				crew.urineStoreId, crew.organicWasteStoreId};
			for (size_t stream = 0; stream < 4; ++stream)
			{
				if (allowUnwired && streamIds[stream].empty()) continue;
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == streamIds[stream]; });
				if (found == settings.stores.end())
					return missing(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' must be a mass unit; crew metabolism is mass-denominated (g or kg)");
				if (!found->composition)
					return refuse(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' has no declared composition; crew streams require compound stores to close the element balance");
				// The crew's stores are no longer pinned to the breathing
				// compartment's hazard class (#751): a crew may be wired to
				// stores of any class, and what it sees is the quantity
				// consequence, not a wall.
				streamStores[stream] = &*found;
			}
			// Aliased stream roles are refused (#600), the same rule the
			// electrolyzer and fuel cell carry (#585) and the carbon-recovery
			// machines carry (#599): food, drinking water, urine and organic
			// waste are four physically distinct streams the ledger books
			// separately. The tick sizes every draw and return against the
			// store's inventory or headroom measured from the same starting
			// state, then books them sequentially, so one store named for two
			// roles has the same inventory spent twice: with aliased inputs the
			// second draw finds the store already drained while the returns
			// still use the original scale, driving retained body mass
			// negative and pausing the World at tick zero; with aliased outputs
			// the second return is clipped after both inputs were consumed,
			// silently changing the metabolic balance the author sized. Every
			// pair of the crew's own stream roles must name a distinct store;
			// sharing between different crews is untouched, because crews book
			// sequentially against the live inventory.
			for (size_t left = 0; left < 4; ++left)
				for (size_t right = left + 1; right < 4; ++right)
					if (!streamIds[left].empty() && streamIds[left] == streamIds[right])
						return refuse(where + " names the same store '"
							+ streamIds[left] + "' for the "
							+ streamRoles[left] + " and the " + streamRoles[right]
							+ ": chemically distinct streams may not share one store");
			if (allowUnwired && std::any_of(std::begin(streamStores), std::end(streamStores),
				[](auto store) { return store == nullptr; })) continue;
			auto const* food = streamStores[0];
			auto const* water = streamStores[1];
			auto const* urine = streamStores[2];
			auto const* waste = streamStores[3];
			auto const foodKg = massToKg(food->unit, crew.foodPerCrewPerS);
			auto const waterKg = massToKg(water->unit, crew.waterPerCrewPerS);
			auto const urineKg = massToKg(urine->unit, crew.urinePerCrewPerS);
			auto const wasteKg = massToKg(waste->unit, crew.organicWastePerCrewPerS);
			auto const consumedKg = foodKg + waterKg + crew.oxygenPerCrewKgPerS;
			auto const emittedKg = urineKg + wasteKg
				+ crew.moisturePerCrewKgPerS + crew.co2PerCrewKgPerS + crew.greywaterKgPerCrewDay / 86400.0;
			if (emittedKg > consumedKg + StationBalanceTolerance * std::max(1.0, consumedKg))
				return refuse(where + " declared returns exceed declared consumption: metabolism may not create mass");
			ConstituentLedger funded;
			addComposition(funded, *food->composition, foodKg, 1.0);
			addComposition(funded, *water->composition, waterKg, 1.0);
			ConstituentLedger returned;
			addComposition(returned, *urine->composition, urineKg, 1.0);
			addComposition(returned, *waste->composition, wasteKg, 1.0);
			addLedger(returned, greywater, 1.0);
			returned["water"] += crew.moisturePerCrewKgPerS;
			for (auto const& entry : returned)
			{
				auto const available = funded.count(entry.first) ? funded.at(entry.first) : 0.0;
				if (entry.second > available + StationBalanceTolerance
					* std::max(1.0, std::abs(available)))
					return refuse(where + " returns more '" + entry.first
						+ "' than the consumed food and water carry: the element balance may not be created");
			}
			auto const streamEnergy = [](ResourceCompositionSettings const& composition, double wetKg)
			{
				return wetKg * (1.0 - composition.moisture) * composition.energyKJPerKgDry;
			};
			auto const releasedKJPerS = streamEnergy(*food->composition, foodKg)
				- streamEnergy(*urine->composition, urineKg)
				- streamEnergy(*waste->composition, wasteKg);
			if (crew.metabolicHeatPerCrewKW > releasedKJPerS
				+ StationBalanceTolerance * std::max(1.0, std::abs(releasedKJPerS)))
				return refuse(where + " metabolic heat exceeds the consumed food energy minus the energy still bound in the waste streams: heat may not be created from nothing");
		} catch (BrokenDraftEntry const&) {}
		if (settings.microbialProcessors.size() > MaxStationMicrobialProcessors)
			return refuse("Station microbial processors are limited to "
				+ std::to_string(MaxStationMicrobialProcessors) + " entries");
		auto declareMicrobialStore = [&](std::string const& storeId, char const* role,
			std::string const& where, bool requireCompound) -> bool
		{
			if (allowUnwired && storeId.empty()) return true;
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return missing(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; nutrient recovery is mass-denominated (g or kg)");
			if (requireCompound && !found->composition)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares no composition; a microbial processor recovers bound nutrients from a declared compound feedstock, not from an uncharacterised stream");
			if (!requireCompound && found->composition)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered product streams land as plain mass, so a compound destination would create tracked constituents out of nothing");
			return true;
		};
		std::set<std::string> microbialIds;
		for (auto const& processor : settings.microbialProcessors) try
		{
			auto const where = "Station microbial processor '" + processor.id + "'";
			if (processor.id.empty()) return refuse("Station microbial processor id must not be empty");
			if (processor.id.size() > MaxStationStoreIdLength)
				return refuse("Station microbial processor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!microbialIds.insert(processor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				processor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + processor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(processor.rateKgPerS) || processor.rateKgPerS < 0.0)
				return refuse(where + " rated feed processing must be finite and non-negative");
			auto const derived = processor.rateBasis == StationRateBasis::CompositionDerived;
			if (!derived && processor.rateBasis != StationRateBasis::FixedPerFeedMass)
				return refuse(where + " has unknown rate basis");
			if (derived)
			{
				double const fractions[]{processor.organicOxidationFraction, processor.nitrificationFraction,
					processor.heatReleaseFraction, processor.processWaterTargetMoisture};
				char const* names[]{"organicOxidationFraction", "nitrificationFraction", "heatReleaseFraction", "processWaterTargetMoisture"};
				for (size_t i = 0; i < 4; ++i)
					if (!std::isfinite(fractions[i]) || fractions[i] < 0.0 || fractions[i] > 1.0
						|| (i == 3 && fractions[i] == 1.0))
						return refuse(where + " " + names[i] + " must be finite and in [0, 1] (target moisture strictly below 1)");
				double const rates[]{processor.o2PerKgOrganicOxidized, processor.o2PerKgNitrifiedN,
					processor.co2PerKgOrganicOxidized, processor.consumablePerKgOrganicOxidized};
				for (double rate : rates)
					if (!std::isfinite(rate) || rate < 0.0)
						return refuse(where + " composition-derived rates must be finite and non-negative");
			}
			double const rateFields[]{processor.specificEnergyKWhPerKgFeed,
				processor.oxygenPerKgFeedKg, processor.processWaterPerKgFeedKg,
				processor.consumablePerKgFeed, processor.co2PerKgFeedKg,
				processor.reactionHeatKJPerKgFeed};
			char const* rateNames[]{"specific energy", "oxygen per kg feed",
				"process water per kg feed", "consumable per kg feed",
				"CO2 per kg feed", "reaction heat per kg feed"};
			for (size_t field = 0; field < (derived ? 1u : 6u); ++field)
				if (!std::isfinite(rateFields[field]) || rateFields[field] < 0.0)
					return refuse(where + " " + rateNames[field] + " must be finite and non-negative");
			double const recoveryFields[]{processor.nitrogenRecovery,
				processor.phosphorusRecovery, processor.potassiumRecovery,
				processor.mineralsRecovery, processor.waterRecovery};
			char const* recoveryNames[]{"nitrogen recovery", "phosphorus recovery",
				"potassium recovery", "minerals recovery", "water recovery"};
			for (size_t field = 0; field < 5; ++field)
				if (!std::isfinite(recoveryFields[field]) || recoveryFields[field] < 0.0
					|| recoveryFields[field] > 1.0)
					return refuse(where + " " + recoveryNames[field]
						+ " must be finite and from 0 to 1: a yield cannot recover more than the feed carries");
			if (!std::isfinite(processor.minOperatingTemperatureC)
				|| processor.minOperatingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum operating temperature must be finite and above absolute zero");
			if (!std::isfinite(processor.maxSinkTemperatureC)
				|| processor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (processor.minOperatingTemperatureC >= processor.maxSinkTemperatureC)
				return refuse(where + " minimum operating temperature must be below the maximum sink temperature: an empty operating window is never a service");
			// #408 condition/recovery contract. The coefficients are
			// provisional biological assumptions: the validation pins
			// them finite and in range, not biologically correct.
			if (!std::isfinite(processor.initialCondition)
				|| processor.initialCondition < 0.0 || processor.initialCondition > 1.0)
				return refuse(where + " initial culture condition must be finite and from 0 to 1");
			double const biologyFields[]{processor.starvationDegradePerHour,
				processor.consumableExhaustionDegradePerHour,
				processor.coldStressDegradePerHour, processor.heatStressDegradePerHour,
				processor.recoveryPerHour};
			char const* biologyNames[]{"starvation degradation per hour",
				"consumable exhaustion degradation per hour",
				"cold stress degradation per hour", "heat stress degradation per hour",
				"recovery per hour"};
			for (size_t field = 0; field < 5; ++field)
				if (!std::isfinite(biologyFields[field]) || biologyFields[field] < 0.0)
					return refuse(where + " " + biologyNames[field]
						+ " must be finite and non-negative (provisional biological assumption)");
			if (!std::isfinite(processor.stockUnitsPerConditionPoint)
				|| processor.stockUnitsPerConditionPoint < 0.0)
				return refuse(where + " stock units per condition point must be finite and non-negative");
			if (!processor.stockStoreId.empty() && processor.stockUnitsPerConditionPoint <= 0.0)
				return refuse(where + " stock units per condition point must be positive when a replacement biological stock store is declared: recovery would otherwise restore capacity while consuming nothing per point");
			std::string const chainIds[9]{processor.feedStoreId, processor.oxygenStoreId,
				processor.waterStoreId, processor.consumableStoreId, processor.nutrientStoreId,
				processor.recoveredWaterStoreId, processor.co2StoreId, processor.residueStoreId,
				processor.stockStoreId};
			char const* chainRoles[9]{"feed", "oxygen", "process water", "consumable",
				"nutrient", "recovered water", "CO2", "residue", "replacement stock"};
			for (size_t stream = 0; stream < 9; ++stream)
				for (size_t other = stream + 1; other < 9; ++other)
					if (!chainIds[stream].empty() && chainIds[stream] == chainIds[other])
						return refuse(where + " " + chainRoles[stream] + " store and "
							+ chainRoles[other] + " store are the same store '"
							+ chainIds[stream] + "'; the recovery chain needs distinct stores for every stream");
			if (!declareMicrobialStore(processor.feedStoreId, "feed", where, true))
				return false;
			if (!declareGasStore(processor.oxygenStoreId, "oxygen", where, false))
				return false;
			if (!declareWaterStore(processor.waterStoreId, "process water", where, true))
				return false;
			if (!allowUnwired || !processor.consumableStoreId.empty())
			{
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == processor.consumableStoreId; });
				if (found == settings.stores.end())
					return missing(where + " consumable store '" + processor.consumableStoreId
						+ "' does not exist");
				if (derived && !isMassUnit(found->unit))
					return refuse(where + " composition-derived consumable must use a mass unit (g or kg)");
				// The consumable's hazard class no longer binds the
				// processor (#751): a processor may draw media of any class,
				// and the quantity it draws is the whole of the consequence.
			}
			if (!declareMicrobialStore(processor.nutrientStoreId, "nutrient", where, false))
				return false;
			for (auto const& [name, fraction] : processor.recoveredWaterCarryoverFractions)
			{
				if (std::find(StationWaterDryConstituents.begin(), StationWaterDryConstituents.end(), name)
					== StationWaterDryConstituents.end() || !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
					return refuse(where + " recovered-water carry-over '" + name + "' must name a known dry constituent with a finite fraction in [0, 1]");
				if (fraction > 0.0)
				{
					auto const destination = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](auto const& s) { return s.id == processor.recoveredWaterStoreId; });
					if (destination != settings.stores.end() && !destination->composition)
						return refuse(where + " carried '" + name + "' requires a compound recovered water destination");
				}
			}
			if (!declareWaterStore(processor.recoveredWaterStoreId, "recovered water", where, true))
				return false;
			if (!declareGasStore(processor.co2StoreId, "CO2", where, false))
				return false;
			if (!declareMicrobialStore(processor.residueStoreId, "residue", where, false))
				return false;
			// The replacement biological stock store (#408) is an optional
			// consumed input like the consumable: it must exist, and its
			// hazard class no longer binds the processor (#751).
			if (!processor.stockStoreId.empty())
			{
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == processor.stockStoreId; });
				if (found == settings.stores.end())
					return missing(where + " replacement biological stock store '"
						+ processor.stockStoreId + "' does not exist");
			}

			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == processor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end() && !(allowUnwired && processor.heatSinkId.empty()))
				return missing(where + " heat sink is unknown thermal reservoir '" + processor.heatSinkId + "'");
			if (sink != settings.thermalReservoirs.end() && sink->thermalClass != processor.thermalClass)
				return refuse(where + " thermal class '" + processor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
			if (allowUnwired && processor.feedStoreId.empty()) continue;
			auto const feed = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == processor.feedStoreId; });
			auto const& composition = *feed->composition;
			auto const dryPerKgFeed = 1.0 - composition.moisture;
			auto const nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
				* processor.nitrogenRecovery + composition.phosphorus * processor.phosphorusRecovery
				+ composition.potassium * processor.potassiumRecovery
				+ composition.otherMinerals * processor.mineralsRecovery);
			double carriedPerKgFeed = 0.0;
			for (auto const& [name, mass] : stationMicrobialCarryover(processor, composition))
			{ (void)name; carriedPerKgFeed += mass; }
			auto const recoveredWaterPerKgFeed = composition.moisture * processor.waterRecovery + carriedPerKgFeed;
			auto const rates = stationMicrobialRates(processor, composition);
			auto const residuePerKgFeed = 1.0 + rates.oxygen
				+ rates.water + (derived ? rates.consumable : 0.0) - nutrientPerKgFeed
				- recoveredWaterPerKgFeed - rates.co2;
			if (!std::isfinite(residuePerKgFeed) || !std::isfinite(rates.heat))
				return refuse(where + " derived rates overflow at the declared composition");
			if (residuePerKgFeed < -StationBalanceTolerance)
				return refuse(where + " declared yields would create mass: the outputs exceed the feed plus oxygen plus process water, and an unrecoverable residue cannot be negative");
			auto const feedEnergyKJPerKgFeed = dryPerKgFeed * composition.energyKJPerKgDry;
			if (rates.heat > feedEnergyKJPerKgFeed
				+ StationBalanceTolerance * std::max(1.0, std::abs(feedEnergyKJPerKgFeed)))
				return refuse(where + " declared reaction heat exceeds the feedstock's declared chemical energy: the digestion may not release more heat than the feed carries");
		} catch (BrokenDraftEntry const&) {}
		// Crop production contracts (#409). A crop population grows a dry
		// Living biomass inside one Atmosphere compartment under the
		// resources actually delivered: lamp power through the declared
		// illumination profile, CO2 from the compartment, irrigation water
		// and each tracked nutrient from its own separate store, inside a
		// declared growing temperature window and below the declared
		// Growing capacity (growing area x maximum supported density).
		// The declared composition fixes the dry-mass basis: the organic
		// balance is what growth builds and respiration burns, and the
		// bound nutrient fractions are funded by the matching stores
		// alone, so one abundant nutrient can never substitute for an
		// exhausted one. Atmospheric nitrogen is refused: crops take up
		// bound nutrient nitrogen, not air, and no configured process here
		// converts one form into the other. Moisture is refused too: Living
		// biomass is tracked as dry mass, so a water fraction in this
		// declaration would count water the population never carries.
		// The growth yield may not fix more chemical energy per kWh of
		// light than a kWh of light carries: photosynthesis is never a
		// free source of chemical energy. All biological coefficients are
		// provisional assumptions: the validation pins them finite and in
		// range, not agronomically correct.
		if (settings.cropPopulations.size() > MaxStationCropPopulations)
			return refuse("Station crop populations are limited to "
				+ std::to_string(MaxStationCropPopulations) + " entries");
		std::set<std::string> cropIds;
		for (auto const& crop : settings.cropPopulations) try
		{
			auto const where = "Station crop population '" + crop.id + "'";
			for (auto const& [name, fraction] : crop.irrigationUptakeFractions)
			{
				if (name != "nutrientNitrogen" && name != "phosphorus" && name != "potassium")
					return refuse(where + " irrigationUptakeFractions has unsupported constituent '" + name + "'");
				if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
					return refuse(where + " irrigationUptakeFractions " + name + " must be finite and in [0, 1]");
			}
			if (crop.id.empty()) return refuse("Station crop population id must not be empty");
			if (crop.id.size() > MaxStationStoreIdLength)
				return refuse("Station crop population id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!cropIds.insert(crop.id).second) return refuse(where + " id is duplicated");
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == crop.atmosphereId; });
			if (compartment == settings.atmospheres.end() && !(allowUnwired && crop.atmosphereId.empty()))
				return missing(where + " grows in unknown atmosphere compartment '" + crop.atmosphereId + "'");
			std::string const uptakeIds[5]{crop.waterStoreId, crop.nitrogenStoreId,
				crop.phosphorusStoreId, crop.potassiumStoreId, crop.mineralsStoreId};
			char const* uptakeRoles[5]{"irrigation water", "nitrogen", "phosphorus",
				"potassium", "minerals"};
			char const* uptakePurityNames[5]{"water", "bound nutrient nitrogen",
				"phosphorus", "potassium", "other minerals"};
			for (size_t stream = 0; stream < 5; ++stream)
			{
				if (allowUnwired && uptakeIds[stream].empty()) continue;
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == uptakeIds[stream]; });
				if (found == settings.stores.end())
					return missing(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' must be a mass unit; crop uptake is mass-denominated (g or kg)");
				if (found->composition && stream != 0 && !isPureUptakeComposition(*found->composition, stream))
					return refuse(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' declares a compound composition that is not pure "
						+ uptakePurityNames[stream] + ": the uptake spends the drawn mass exactly as "
						+ uptakePurityNames[stream]
						+ " and owns no residual stream to account for any other constituent; only an explicit conversion may cross that boundary");
				for (size_t other = stream + 1; other < 5; ++other)
					if (uptakeIds[stream] == uptakeIds[other])
						return refuse(where + " " + uptakeRoles[stream] + " store and "
							+ uptakeRoles[other] + " store are the same store '"
							+ uptakeIds[stream] + "'; every tracked nutrient and the irrigation water need distinct stores so depletion stays visible per resource");
			}
			auto const& composition = crop.composition;
			if (!std::isfinite(composition.moisture) || composition.moisture != 0.0)
				return refuse(where + " declared composition carries moisture; Living biomass is tracked as dry mass, so the crop declaration must not carry a water fraction");
			if (!std::isfinite(composition.atmosphericNitrogen) || composition.atmosphericNitrogen != 0.0)
				return refuse(where + " declared composition carries atmospheric nitrogen; crops take up bound nutrient nitrogen, not air, and no configured process converts one into the other");
			double const dryFractions[]{composition.nutrientNitrogen, composition.phosphorus,
				composition.potassium, composition.otherMinerals};
			char const* dryNames[]{"nutrient nitrogen", "phosphorus", "potassium", "other minerals"};
			for (size_t field = 0; field < 4; ++field)
				if (!std::isfinite(dryFractions[field]) || dryFractions[field] < 0.0)
					return refuse(where + " declared " + dryNames[field]
						+ " fraction must be finite and non-negative");
			if (dryFractions[0] + dryFractions[1] + dryFractions[2] + dryFractions[3]
				> 1.0 + StationBalanceTolerance)
				return refuse(where + " declared dry nutrient fractions exceed 1: the organic dry-mass balance would be negative");
			if (!std::isfinite(composition.energyKJPerKgDry) || composition.energyKJPerKgDry <= 0.0)
				return refuse(where + " declared chemical energy per dry kg must be finite and positive: the respiration heat and fixed-energy ledger depend on it");
			if (!std::isfinite(crop.growingAreaM2) || crop.growingAreaM2 < 0.0)
				return refuse(where + " growing area must be finite and non-negative");
			if (!std::isfinite(crop.maxBiomassDensityKgPerM2) || crop.maxBiomassDensityKgPerM2 <= 0.0)
				return refuse(where + " maximum biomass density must be finite and positive");
			auto const capacityKg = crop.growingAreaM2 * crop.maxBiomassDensityKgPerM2;
			// #581: the two authored factors are each validated finite, but
			// their product - the installation's Growing capacity - can still
			// overflow to infinity. A non-finite capacity passes every upper-
			// bound comparison downstream (inf > x is always true, inf > inf
			// is false), so the finite installation bound the snapshot promises
			// would silently vanish. The overflow is refused here, before any
			// runtime construction or document mutation, rather than published
			// as a healthy tick.
			if (!std::isfinite(capacityKg))
				return refuse(where + " derived Growing capacity overflows to a non-finite magnitude: growing area times maximum biomass density must stay finite so the installation bound stays a publishable, comparable number");
			if (!std::isfinite(crop.initialBiomassKg) || crop.initialBiomassKg < 0.0)
				return refuse(where + " initial living biomass must be finite and non-negative");
			if (crop.initialBiomassKg > capacityKg + StationBalanceTolerance)
				return refuse(where + " initial living biomass exceeds the declared Growing capacity: the population cannot start fuller than the installation can support");
			if (!std::isfinite(crop.lampRatedKW) || crop.lampRatedKW < 0.0)
				return refuse(where + " lamp rated power must be finite and non-negative");
			if (!validateStationProfile(crop.illuminationProfile, where + " illumination profile", diagnostic))
				return false;
			if (!std::isfinite(crop.circulationPowerKW) || crop.circulationPowerKW < 0.0)
				return refuse(where + " circulation power must be finite and non-negative");
			if (!std::isfinite(crop.growthYieldKgPerKWh) || crop.growthYieldKgPerKWh < 0.0)
				return refuse(where + " growth yield per kWh must be finite and non-negative (provisional biological assumption)");
			if (crop.growthYieldKgPerKWh * composition.energyKJPerKgDry
				> 3600.0 + StationBalanceTolerance * 3600.0)
				return refuse(where + " declared growth yield would fix more chemical energy per kWh of light than the light carries: photosynthesis is never a free source of chemical energy");
			if (!std::isfinite(crop.respirationPerHour) || crop.respirationPerHour < 0.0)
				return refuse(where + " respiration per hour must be finite and non-negative (provisional biological assumption)");
			if (!std::isfinite(crop.transpirationKgPerKgGrowth) || crop.transpirationKgPerKgGrowth < 0.0)
				return refuse(where + " transpiration per kg growth must be finite and non-negative (provisional biological assumption)");
			if (!std::isfinite(crop.minGrowingTemperatureC)
				|| crop.minGrowingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum growing temperature must be finite and above absolute zero");
			if (!std::isfinite(crop.maxGrowingTemperatureC)
				|| crop.maxGrowingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum growing temperature must be finite and above absolute zero");
			if (crop.minGrowingTemperatureC >= crop.maxGrowingTemperatureC)
				return refuse(where + " minimum growing temperature must be below the maximum growing temperature: an empty growing window is never a service");
			// Harvest contract (#410). The edible and harvest fractions are
			// declared shares of real material, so they must sit inside
			// their physical ranges; a harvest trigger above the declared
			// Growing capacity could never fire and is refused as a dead
			// rule; and enabling a harvest without both destinations would
			// remove living material with nowhere accounted to put it.
			// Destination stores are validated whenever named, and required
			// whenever the harvest is enabled: mass-denominated, distinct
			// from each other, and composition-compatible with the crop's
			// declared dry mass so the harvested constituents enter the
			// stores exactly as declared rather than reinterpreted.
			if (!std::isfinite(crop.edibleFraction) || crop.edibleFraction < 0.0
				|| crop.edibleFraction > 1.0)
				return refuse(where + " edible fraction must be finite and from 0 to 1: the edible share of the harvested dry mass cannot exceed the harvest");
			if (!std::isfinite(crop.harvestFraction) || crop.harvestFraction <= 0.0
				|| crop.harvestFraction > 1.0)
				return refuse(where + " harvest fraction must be finite and from above 0 to 1: a harvest that removes nothing is not a harvest");
			if (!std::isfinite(crop.harvestTriggerKg) || crop.harvestTriggerKg < 0.0)
				return refuse(where + " harvest trigger must be finite and non-negative (0 disables the harvest)");
			if (crop.harvestTriggerKg > capacityKg + StationBalanceTolerance)
				return refuse(where + " harvest trigger exceeds the declared Growing capacity: the harvest could never fire, so the rule is dead as configured");
			std::string const destinationIds[2]{crop.foodStoreId, crop.wasteStoreId};
			char const* destinationRoles[2]{"food", "waste"};
			for (size_t stream = 0; stream < 2; ++stream)
			{
				if (destinationIds[stream].empty())
				{
					if (!allowUnwired && crop.harvestTriggerKg > 0.0)
						return refuse(where + " harvest is enabled but the " + destinationRoles[stream]
							+ " destination store is not named: harvested living material must have a declared destination");
					continue;
				}
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == destinationIds[stream]; });
				if (found == settings.stores.end())
					return missing(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' must be a mass unit; harvested dry biomass is mass-denominated (g or kg)");
				if (!destinationIds[0].empty() && destinationIds[0] == destinationIds[1])
					return refuse(where + " food store and waste store are the same store '" + destinationIds[0]
						+ "'; edible and inedible yield need distinct destinations so the split stays visible");
				if (found->composition && !compositionsClose(*found->composition, composition))
					return refuse(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' declares a composition different from the crop's declared dry-mass composition: harvested material enters a store exactly as declared, and a different compound material is a boundary only an explicit conversion may cross");
			}
		} catch (BrokenDraftEntry const&) {}
		// Algae culture contracts (#411). A separate Biological population
		// with its own installation limits: the Growing capacity here is the
		// active culture volume times the supported dry-mass density per
		// litre - never derived from, shared with, or substitutable by a
		// crop's illuminated-area capacity. The lifecycle rules are the
		// same as the crops' by design: dry-mass composition with no
		// moisture and no atmospheric nitrogen, a growth yield that can
		// never fix more chemical energy than the light delivers, a real
		// growing window, and a start within the declared capacity. The
		// harvest contract partitions the harvested dry mass into three
		// declared destinations - food, recovery and waste - so enabling
		// the harvest requires all three, pairwise distinct, mass-
		// denominated and composition-compatible with the culture's
		// declared dry mass; the food and recovery fractions together may
		// not exceed the harvest, because the unrecoverable remainder is
		// the waste share and a partition that spends more than 100% of
		// the harvest is not conservative accounting.
		if (settings.algaeCultures.size() > MaxStationAlgaeCultures)
			return refuse("Station algae cultures are limited to "
				+ std::to_string(MaxStationAlgaeCultures) + " entries");
		std::set<std::string> algaeIds;
		for (auto const& algae : settings.algaeCultures) try
		{
			auto const where = "Station algae culture '" + algae.id + "'";
			if (algae.id.empty()) return refuse("Station algae culture id must not be empty");
			if (algae.id.size() > MaxStationStoreIdLength)
				return refuse("Station algae culture id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!algaeIds.insert(algae.id).second) return refuse(where + " id is duplicated");
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == algae.atmosphereId; });
			if (compartment == settings.atmospheres.end() && !(allowUnwired && algae.atmosphereId.empty()))
				return missing(where + " runs in unknown atmosphere compartment '" + algae.atmosphereId + "'");
			std::string const uptakeIds[5]{algae.waterStoreId, algae.nitrogenStoreId,
				algae.phosphorusStoreId, algae.potassiumStoreId, algae.mineralsStoreId};
			char const* uptakeRoles[5]{"culture water", "nitrogen", "phosphorus",
				"potassium", "minerals"};
			char const* uptakePurityNames[5]{"water", "bound nutrient nitrogen",
				"phosphorus", "potassium", "other minerals"};
			for (size_t stream = 0; stream < 5; ++stream)
			{
				if (allowUnwired && uptakeIds[stream].empty()) continue;
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == uptakeIds[stream]; });
				if (found == settings.stores.end())
					return missing(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' must be a mass unit; culture uptake is mass-denominated (g or kg)");
				if (found->composition && stream != 0 && !isPureUptakeComposition(*found->composition, stream))
					return refuse(where + " " + uptakeRoles[stream] + " store '" + uptakeIds[stream]
						+ "' declares a compound composition that is not pure "
						+ uptakePurityNames[stream] + ": the uptake spends the drawn mass exactly as "
						+ uptakePurityNames[stream]
						+ " and owns no residual stream to account for any other constituent; only an explicit conversion may cross that boundary");
				for (size_t other = stream + 1; other < 5; ++other)
					if (uptakeIds[stream] == uptakeIds[other])
						return refuse(where + " " + uptakeRoles[stream] + " store and "
							+ uptakeRoles[other] + " store are the same store '"
							+ uptakeIds[stream] + "'; every tracked nutrient and the culture water need distinct stores so depletion stays visible per resource");
			}
			auto const& composition = algae.composition;
			if (!std::isfinite(composition.moisture) || composition.moisture != 0.0)
				return refuse(where + " declared composition carries moisture; Living biomass is tracked as dry mass, so the culture declaration must not carry a water fraction");
			if (!std::isfinite(composition.atmosphericNitrogen) || composition.atmosphericNitrogen != 0.0)
				return refuse(where + " declared composition carries atmospheric nitrogen; the culture takes up bound nutrient nitrogen, not air, and no configured process converts one into the other");
			double const dryFractions[]{composition.nutrientNitrogen, composition.phosphorus,
				composition.potassium, composition.otherMinerals};
			char const* dryNames[]{"nutrient nitrogen", "phosphorus", "potassium", "other minerals"};
			for (size_t field = 0; field < 4; ++field)
				if (!std::isfinite(dryFractions[field]) || dryFractions[field] < 0.0)
					return refuse(where + " declared " + dryNames[field]
						+ " fraction must be finite and non-negative");
			if (dryFractions[0] + dryFractions[1] + dryFractions[2] + dryFractions[3]
				> 1.0 + StationBalanceTolerance)
				return refuse(where + " declared dry nutrient fractions exceed 1: the organic dry-mass balance would be negative");
			if (!std::isfinite(composition.energyKJPerKgDry) || composition.energyKJPerKgDry <= 0.0)
				return refuse(where + " declared chemical energy per dry kg must be finite and positive: the respiration heat and fixed-energy ledger depend on it");
			if (!std::isfinite(algae.cultureVolumeL) || algae.cultureVolumeL < 0.0)
				return refuse(where + " active culture volume must be finite and non-negative");
			if (!std::isfinite(algae.maxBiomassDensityKgPerL) || algae.maxBiomassDensityKgPerL <= 0.0)
				return refuse(where + " maximum biomass density per litre must be finite and positive");
			auto const capacityKg = algae.cultureVolumeL * algae.maxBiomassDensityKgPerL;
			// #581: the same product-overflow contract as the crops - the
			// culture volume and the density per litre are each validated
			// finite, but their product, the culture's installation bound,
			// can still overflow to infinity and would then pass every
			// upper-bound comparison downstream. Refused before any runtime
			// construction or document mutation.
			if (!std::isfinite(capacityKg))
				return refuse(where + " derived culture capacity overflows to a non-finite magnitude: active culture volume times maximum biomass density per litre must stay finite so the installation bound stays a publishable, comparable number");
			if (!std::isfinite(algae.initialBiomassKg) || algae.initialBiomassKg < 0.0)
				return refuse(where + " initial living biomass must be finite and non-negative");
			if (algae.initialBiomassKg > capacityKg + StationBalanceTolerance)
				return refuse(where + " initial living biomass exceeds the declared culture capacity: the culture cannot start fuller than the installation can support");
			if (!std::isfinite(algae.lampRatedKW) || algae.lampRatedKW < 0.0)
				return refuse(where + " lamp rated power must be finite and non-negative");
			if (!validateStationProfile(algae.illuminationProfile, where + " illumination profile", diagnostic))
				return false;
			if (!std::isfinite(algae.circulationPowerKW) || algae.circulationPowerKW < 0.0)
				return refuse(where + " culture circulation power must be finite and non-negative");
			if (!std::isfinite(algae.growthYieldKgPerKWh) || algae.growthYieldKgPerKWh < 0.0)
				return refuse(where + " growth yield per kWh must be finite and non-negative (provisional biological assumption)");
			if (algae.growthYieldKgPerKWh * composition.energyKJPerKgDry
				> 3600.0 + StationBalanceTolerance * 3600.0)
				return refuse(where + " declared growth yield would fix more chemical energy per kWh of light than the light carries: photosynthesis is never a free source of chemical energy");
			if (!std::isfinite(algae.respirationPerHour) || algae.respirationPerHour < 0.0)
				return refuse(where + " respiration per hour must be finite and non-negative (provisional biological assumption)");
			if (!std::isfinite(algae.evaporationKgPerKgGrowth) || algae.evaporationKgPerKgGrowth < 0.0)
				return refuse(where + " evaporation per kg growth must be finite and non-negative (provisional biological assumption)");
			if (!std::isfinite(algae.minGrowingTemperatureC)
				|| algae.minGrowingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum growing temperature must be finite and above absolute zero");
			if (!std::isfinite(algae.maxGrowingTemperatureC)
				|| algae.maxGrowingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum growing temperature must be finite and above absolute zero");
			if (algae.minGrowingTemperatureC >= algae.maxGrowingTemperatureC)
				return refuse(where + " minimum growing temperature must be below the maximum growing temperature: an empty growing window is never a service");
			// Harvest partition contract (#411): the food and recovery
			// fractions are declared shares of the harvested dry mass and
			// together may not exceed it - the unrecoverable remainder is
			// the waste share, so a partition spending more than 100% of
			// the harvest is refused as non-conservative. A trigger above
			// the declared culture capacity could never fire and is refused
			// as a dead rule; enabling the harvest requires all three
			// destinations, pairwise distinct, mass-denominated and
			// composition-compatible with the culture's declared dry mass.
			if (!std::isfinite(algae.foodFraction) || algae.foodFraction < 0.0
				|| algae.foodFraction > 1.0)
				return refuse(where + " food fraction must be finite and from 0 to 1: the edible share of the harvested dry mass cannot exceed the harvest");
			if (!std::isfinite(algae.recoveryFraction) || algae.recoveryFraction < 0.0
				|| algae.recoveryFraction > 1.0)
				return refuse(where + " recovery fraction must be finite and from 0 to 1: the recoverable share of the harvested dry mass cannot exceed the harvest");
			if (algae.foodFraction + algae.recoveryFraction > 1.0 + StationBalanceTolerance)
				return refuse(where + " food and recovery fractions together exceed 1: the harvest partition must leave a non-negative unrecoverable remainder for the waste path, so the declared destinations can never claim more than the harvest");
			if (!std::isfinite(algae.harvestFraction) || algae.harvestFraction <= 0.0
				|| algae.harvestFraction > 1.0)
				return refuse(where + " harvest fraction must be finite and from above 0 to 1: a harvest that removes nothing is not a harvest");
			if (!std::isfinite(algae.harvestTriggerKg) || algae.harvestTriggerKg < 0.0)
				return refuse(where + " harvest trigger must be finite and non-negative (0 disables the harvest)");
			if (algae.harvestTriggerKg > capacityKg + StationBalanceTolerance)
				return refuse(where + " harvest trigger exceeds the declared culture capacity: the harvest could never fire, so the rule is dead as configured");
			std::string const destinationIds[3]{algae.foodStoreId, algae.recoveryStoreId,
				algae.wasteStoreId};
			char const* destinationRoles[3]{"food", "recovery", "waste"};
			for (size_t stream = 0; stream < 3; ++stream)
			{
				if (destinationIds[stream].empty())
				{
					if (!allowUnwired && algae.harvestTriggerKg > 0.0)
						return refuse(where + " harvest is enabled but the " + destinationRoles[stream]
							+ " destination store is not named: harvested living material must have a declared destination");
					continue;
				}
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == destinationIds[stream]; });
				if (found == settings.stores.end())
					return missing(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' must be a mass unit; harvested dry biomass is mass-denominated (g or kg)");
				for (size_t other = stream + 1; other < 3; ++other)
					if (!destinationIds[other].empty() && destinationIds[stream] == destinationIds[other])
						return refuse(where + " " + destinationRoles[stream] + " store and "
							+ destinationRoles[other] + " store are the same store '"
							+ destinationIds[stream] + "'; the food, recovery and waste yield need distinct destinations so the partition stays visible");
				if (found->composition && !compositionsClose(*found->composition, composition))
					return refuse(where + " " + destinationRoles[stream] + " store '" + destinationIds[stream]
						+ "' declares a composition different from the culture's declared dry-mass composition: harvested material enters a store exactly as declared, and a different compound material is a boundary only an explicit conversion may cross");
			}
		} catch (BrokenDraftEntry const&) {}
		diagnostic.clear(); return true;
	}

	namespace
	{
		std::string rangeNumber(double value)
		{
			char buffer[32]{};
			std::snprintf(buffer, sizeof(buffer), "%g", value);
			return std::string(buffer);
		}

		template<size_t N>
		bool fieldIsOneOf(std::string_view field, std::array<char const*, N> const& names)
		{
			for (auto const* name : names)
				if (field == name) return true;
			return false;
		}
	}

	StationFieldRange stationFieldRange(std::string_view field)
	{
		// Celsius readings and setpoints. The cold of space, a chilled-water
		// plant and a freeze-protection setpoint all live below zero, so a
		// floor of zero would refuse values the author plainly needs. These
		// are unbounded: what matters is that they are degrees, not that they
		// are positive.
		static constexpr std::array<char const*, 16> celsiusFields{
			"chilled setpoint C", "freeze setpoint C", "max condensing temperature C",
			"max growing temperature C", "max sink temperature C", "max temperature C",
			"max usable temperature C", "min growing temperature C", "min operating temperature C",
			"min temperature C", "min usable temperature C",
			// #849: a Condition stressor's onset and a runaway Trip's trigger
			// are both degrees, so neither may be floored at zero.
			"overheat stress C", "runaway trigger temperature C",
			"sink temperature C", "starting temperature C", "min hot temperature C"};
		// Values that are a share of something: fractions, ratios,
		// efficiencies, an emissivity, a view factor. They are declared
		// explicitly rather than matched on the word "fraction", because
		// "evaporation", "respiration" and "duration" all contain "ratio"
		// without being one, and a substring rule would quietly clamp a rate
		// to 1.0 per second.
		static constexpr std::array<char const*, 39> unitIntervalFields{
			"atmospheric nitrogen fraction", "cavitation fill fraction", "charge efficiency",
			"composition atmospheric nitrogen fraction", "composition moisture fraction",
			"composition nutrient nitrogen fraction", "composition other minerals fraction",
			"composition phosphorus fraction", "composition potassium fraction",
			"discharge efficiency", "edible fraction of harvest", "efficiency",
			"efficiency (0..1]", "emissivity", "failed availability",
			"food fraction of harvest", "harvest fraction of living biomass", "heat release fraction",
			"initial condition", "max curtailment", "max wear fraction",
			"moisture fraction", "nitrification fraction",
			"nutrient nitrogen fraction", "open fraction", "organic oxidation fraction",
			"other minerals fraction", "phosphorus fraction", "potassium fraction",
			"process water target moisture", "recovered fraction",
			"recovery fraction of harvest", "recovery hysteresis fraction", "recovery ratio",
			"seizure threshold", "string loss fraction", "target RH", "view factor", "Carnot fraction"};
		if (fieldIsOneOf(field, celsiusFields)) return StationFieldRange{false, 0.0, false, 0.0};
		if (fieldIsOneOf(field, unitIntervalFields)) return StationFieldRange{true, 0.0, true, 1.0};
		return StationFieldRange{true, 0.0, false, 0.0};
	}

	bool stationFieldAccepts(std::string_view field, double value, std::string& reason)
	{
		if (!std::isfinite(value))
		{
			reason = std::string(field) + " must be a finite number";
			return false;
		}
		auto const range = stationFieldRange(field);
		if (range.hasMinimum && value < range.minimum)
		{
			reason = std::string(field) + " must be at least " + rangeNumber(range.minimum);
			return false;
		}
		if (range.hasMaximum && value > range.maximum)
		{
			reason = std::string(field) + " must be at most " + rangeNumber(range.maximum);
			return false;
		}
		return true;
	}

}
