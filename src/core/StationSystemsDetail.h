#pragma once
#include "StationSystemsRuntime.h"
#include "core/StationConnectionLedger.h"
#include "core/StationMaterialConnections.h"
#include "core/World.h"
#include "core/SerializationException.h"
#include "ScriptSandbox.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <set>


namespace core::station_detail
{

	constexpr size_t EventLimit = StationEventHistoryLimit;
	constexpr size_t DiagnosticByteLimit = 2048;
	// #418: the per-series sample budget for bounded trends. Storage
	// never exceeds this many samples per series no matter how long or
	// how fast the World runs; overflow coarsens the history instead
	// of growing it.
	constexpr size_t TrendSampleLimit = StationTrendSampleLimit;
	// Fuel energy is authored in the store's unit; the generator's
	// physics runs in kWh. One factor converts store units to kWh so
	// host and sandbox agree on every draw (#399). The scenario
	// forecast reads the same contract to project generator fuel draw
	// in the store's own unit (#594).
	inline double stationEnergyUnitsToKWh(std::string const& unit)
	{
		if (unit == "kJ") return 1.0 / 3600.0;
		if (unit == "MJ") return 1000.0 / 3600.0;
		if (unit == "Wh") return 1.0 / 1000.0;
		return 1.0;
	}
	// Every public event producer appends through this one bounded
	// helper (#551): the history never retains more than the newest
	// EventLimit entries, evicting oldest-first, so a tick that
	// announces several transitions at once cannot push the history
	// past the bound, and later appends keep evicting no matter how
	// the vector grew. Declaration order within a tick is preserved.
	// The category (#418) names the subsystem family on the stable
	// presentation contract so incidents render without message parsing.
	inline void appendBoundedEvent(std::vector<StationSystemsEvent>& events,
		uint64_t tick, std::string category, std::string message)
	{
		while (events.size() >= EventLimit) events.erase(events.begin());
		events.push_back({tick, std::move(message), std::move(category)});
	}
	// #418/#603: one bounded trend observation with cadence-aware
	// decimation. A series at downsampleLevel L records one sample
	// every 2^L base observations: consecutive samples always stand
	// exactly sampleIntervalSeconds = base x 2^L of simulation time
	// apart, so every sample in the series covers the same amount of
	// time and the pairwise merge average stays time-weighted-correct
	// - already-merged samples and newly recorded ones carry equal
	// weight because they carry equal spans. Observations that fall
	// inside the current stride are not recorded (the live value
	// stays visible in the snapshot); the history skips them instead
	// of appending raw fine-cadence samples onto a coarsened series
	// the way the old merge-then-append-at-base-rate scheme did,
	// which made the declared stride fictitious and averaged unequal
	// time spans against each other (#603). Coarsening halves the
	// count and doubles the stride on every overflow, so storage
	// never exceeds TrendSampleLimit; the level is capped at
	// StationTrendDownsampleLevelCap, past which the window slides
	// oldest-first instead of doubling again, keeping the stride and
	// every 2^level shift finite for any run length. Samples older
	// than the authored retention are dropped outright. Both
	// operations mark the series truncated: the presentation shows
	// the gap rather than imply complete history. Deterministic and
	// read-only with respect to the simulated state - trends never
	// feed back into the ledger.
	inline void appendBoundedTrendSampleImpl(std::vector<StationTrendSeries>& series,
		std::string const& subsystem, std::string const& id,
		std::string const& unit, double baseIntervalSeconds,
		double retentionSeconds,
		uint64_t tick, double elapsedSeconds, double requested, double actual)
	{
		// The base cadence in whole ticks, matching the gate the
		// runtime uses to call this at the observation boundary, so
		// the decimation stride is exact on the tick grid instead of
		// re-derived from the floating-point seconds.
		auto const baseTicks = std::max<uint64_t>(1,
			static_cast<uint64_t>(std::ceil(baseIntervalSeconds / World::getFixedTimestep())));
		auto found = std::ranges::find(series, std::make_pair(subsystem, id),
			[](StationTrendSeries const& entry)
			{ return std::make_pair(entry.subsystem, entry.id); });
		if (found == series.end())
		{
			StationTrendSeries fresh;
			fresh.subsystem = subsystem;
			fresh.id = id;
			fresh.unit = unit;
			fresh.sampleIntervalSeconds = baseIntervalSeconds;
			series.push_back(std::move(fresh));
			found = prev(series.end());
		}
		auto& target = *found;
		if (target.samples.size() >= TrendSampleLimit)
		{
			if (target.downsampleLevel < StationTrendDownsampleLevelCap)
			{
				// Every sample stands for the same span, so the
				// pairwise mean is the correct time-weighted merge.
				std::vector<StationTrendSample> merged;
				merged.reserve(TrendSampleLimit / 2 + 1);
				size_t index = 0;
				for (; index + 1 < target.samples.size(); index += 2)
				{
					auto const& first = target.samples[index];
					auto const& second = target.samples[index + 1];
					merged.push_back({second.worldTick, second.elapsedSeconds,
						(first.requested + second.requested) / 2.0,
						(first.actual + second.actual) / 2.0});
				}
				if (index < target.samples.size()) merged.push_back(target.samples[index]);
				target.samples = std::move(merged);
				++target.downsampleLevel;
				// Exact in binary floating point: scaling by a power
				// of two never rounds, so the stride stays the true
				// cadence instead of a drifting product.
				target.sampleIntervalSeconds = std::ldexp(baseIntervalSeconds,
					static_cast<int>(target.downsampleLevel));
				target.truncated = true;
			}
			else
			{
				// At the cap the stride never doubles again: the
				// window slides oldest-first so the count stays
				// bounded and the published stride and every shift of
				// the level stay finite forever.
				while (target.samples.size() >= TrendSampleLimit)
				{
					target.samples.erase(target.samples.begin());
					target.truncated = true;
				}
			}
		}
		// Cadence-aware decimation: with the coarsening above already
		// applied, the series' current stride is the minimum spacing
		// a new sample may have. An observation inside it is skipped
		// rather than appended, so the uniform-spacing invariant and
		// the equal-span averaging survive every coarsening.
		auto const level = std::min(target.downsampleLevel, StationTrendDownsampleLevelCap);
		auto const strideTicks = baseTicks << level;
		if (!target.samples.empty()
			&& tick - target.samples.back().worldTick < strideTicks)
			return;
		target.samples.push_back({tick, elapsedSeconds, requested, actual});
		while (target.samples.size() > 1
			&& elapsedSeconds - target.samples.front().elapsedSeconds > retentionSeconds)
		{
			target.samples.erase(target.samples.begin());
			target.truncated = true;
		}
	}
	// Borrow the Lua error only long enough to copy a bounded string. Never
	// coerce arbitrary error objects outside a protected allocation boundary.
	inline std::string errorMessage(lua_State* state, char const* fallback)
	{
		if (lua_type(state, -1) != LUA_TSTRING) return fallback;
		size_t length = 0;
		auto const* text = lua_tolstring(state, -1, &length);
		return std::string(text, std::min(length, DiagnosticByteLimit));
	}
	struct ScriptObject
	{
		// The station lifecycle model grows with each systems slice;
		// the sandbox budget is sized to the compiled footprint of the
		// bundled model plus headroom, not to a scenario cost - the
		// per-tick cost follows the configured scenario, and compile
		// footprint is what the construction pass allocates. The algae
		// culture slice (#411) pushed the compiled model past the
		// previous 512 KiB ceiling; the living biological constituent
		// inventories (#571), which the sandbox mirrors tick for tick
		// alongside the host ledger, pushed it past 1 MiB. The budget
		// rises with the model as the crop and algae slices did before
		// it.
		script::ScratchBudget budget{2 * 1024 * 1024, 250000};
		script::ModuleLoader loader;
		std::unique_ptr<lua_State, script::StateCloser> state;
		int reference{LUA_NOREF};
		ScriptObject() : state(lua_newstate(script::budgetedAllocate, &budget))
		{
			if (!state) throw SerializationException("Cannot allocate Station systems sandbox");
			lua_atpanic(state.get(), script::luaPanic);
			std::string diagnostic;
			if (script::runScratchSetup(state.get(), &loader, nullptr, diagnostic) != LUA_OK)
				throw SerializationException(diagnostic);
			lua_settop(state.get(), 0);
		}
	};
	inline std::string declaredUnitsList()
	{
		std::string list;
		for (auto const* unit : StationStoreUnits)
		{
			if (!list.empty()) list += ", ";
			list += unit;
		}
		return list;
	}
	inline std::string declaredThermalClasses()
	{
		std::string list;
		for (auto const* thermalClass : StationThermalServiceClasses)
		{
			if (!list.empty()) list += ", ";
			list += thermalClass;
		}
		return list;
	}
	// Compound Resource materials are mass-denominated (#391); their
	// constituents are accounted in kg and the energy term in kJ.
	inline bool isMassUnit(std::string const& unit)
	{
		return unit == "g" || unit == "kg";
	}
	inline double massToKg(std::string const& unit, double mass)
	{
		return unit == "g" ? mass * 0.001 : mass;
	}
	// Inverse of massToKg: a magnitude expressed in kilograms rendered
	// in a store's own declared mass unit (#402, where the recovery
	// rates are authored in kg/s against stores that may be grams).
	inline double kgToMass(std::string const& unit, double kg)
	{
		return unit == "g" ? kg * 1000.0 : kg;
	}
	inline bool compositionsClose(ResourceCompositionSettings const& left,
		ResourceCompositionSettings const& right)
	{
		auto const close = [](double a, double b)
		{ return std::abs(a - b) <= StationBalanceTolerance; };
		return close(left.moisture, right.moisture)
			&& close(left.nutrientNitrogen, right.nutrientNitrogen)
			&& close(left.atmosphericNitrogen, right.atmosphericNitrogen)
			&& close(left.phosphorus, right.phosphorus)
			&& close(left.potassium, right.potassium)
			&& close(left.otherMinerals, right.otherMinerals)
			&& close(left.energyKJPerKgDry, right.energyKJPerKgDry);
	}
	// Biological uptake role purity (#572). A crop or culture uptake
	// spends the drawn mass exactly as the role's own material:
	// irrigation/culture water is spent as water, and each tracked
	// nutrient draw is spent as that nutrient. A plain store with no
	// declared composition is accepted as the role material by
	// convention; a compound store must declare itself to BE that
	// material - pure in the role constituent, with no moisture for
	// the nutrient roles and no other constituent riding along,
	// because the uptake owns no residual stream to account for
	// anything else the drawn mass carries. A P-only compound can
	// never fund nitrogen-containing biomass, atmospheric nitrogen is
	// never a match for the bound-nitrogen role, and one tracked
	// nutrient never substitutes for another: crossing those
	// boundaries is a conversion the uptake contract does not own,
	// so it is refused at configuration. Roles: 0 water, 1 nitrogen,
	// 2 phosphorus, 3 potassium, 4 minerals.
	inline bool isPureUptakeComposition(ResourceCompositionSettings const& composition,
		size_t role)
	{
		auto const close = [](double a, double b)
		{ return std::abs(a - b) <= StationBalanceTolerance; };
		double const fractions[5]{composition.moisture, composition.nutrientNitrogen,
			composition.phosphorus, composition.potassium, composition.otherMinerals};
		if (!close(fractions[role], 1.0)) return false;
		if (role == 0)
		{
			for (size_t field = 1; field < 5; ++field)
				if (!close(fractions[field], 0.0)) return false;
		}
		else
		{
			if (!close(fractions[0], 0.0)) return false;
			for (size_t field = 1; field < 5; ++field)
				if (field != role && !close(fractions[field], 0.0)) return false;
		}
		return true;
	}
	using ConstituentLedger = std::map<std::string, double>;
	// Add the constituent amounts of a compound wet mass to a ledger.
	// The seven mass constituents partition the wet mass exactly: water
	// is the moisture term and dryOrganics is the dry-mass balance, so
	// constituent accounting closes over the whole inventory. Energy is
	// the declared chemical energy term, in kJ.
	inline void addComposition(ConstituentLedger& ledger,
		ResourceCompositionSettings const& composition, double wetMassKg, double sign)
	{
		auto const dry = wetMassKg * (1.0 - composition.moisture);
		ledger["water"] += sign * wetMassKg * composition.moisture;
		ledger["nutrientNitrogen"] += sign * dry * composition.nutrientNitrogen;
		ledger["atmosphericNitrogen"] += sign * dry * composition.atmosphericNitrogen;
		ledger["phosphorus"] += sign * dry * composition.phosphorus;
		ledger["potassium"] += sign * dry * composition.potassium;
		ledger["otherMinerals"] += sign * dry * composition.otherMinerals;
		ledger["dryOrganics"] += sign * dry * (1.0 - (composition.nutrientNitrogen
			+ composition.atmosphericNitrogen + composition.phosphorus
			+ composition.potassium + composition.otherMinerals));
		ledger["energy"] += sign * dry * composition.energyKJPerKgDry;
	}
	// The internal alias for the published biological constituent
	// inventory (#571): the actual dry-mass amounts of respirable
	// organic matter and bound nutrients a biological population
	// carries, routes and loses.
	using BiologicalConstituents = StationBiologicalConstituents;
	// The declared organic fraction of a dry biological composition:
	// the share of dry mass that is respirable organic matter.
	inline double organicFractionOf(ResourceCompositionSettings const& composition)
	{
		return 1.0 - (composition.nutrientNitrogen + composition.phosphorus
			+ composition.potassium + composition.otherMinerals);
	}
	// The chemical-energy basis for the organic material (#571): the
	// declared dry-biomass chemical energy is carried wholly by the
	// organic fraction, because the bound nutrients are not fuel. So
	// a kilogram of organic matter carries the dry-biomass energy
	// divided by the organic fraction, which makes the energy fixed
	// during growth equal to the energy later respired plus the energy
	// still carried in the living and harvested organic matter. A
	// composition with no organic matter carries no chemical energy.
	inline double chemicalEnergyKJPerKgOrganic(ResourceCompositionSettings const& composition)
	{
		auto const organicFraction = organicFractionOf(composition);
		if (organicFraction <= 0.0) return 0.0;
		return composition.energyKJPerKgDry / organicFraction;
	}
	// A share of an inventory, for splitting a routed mass across the
	// constituents it actually carries rather than a declared mix.
	inline BiologicalConstituents scaledConstituents(BiologicalConstituents const& inventory,
		double share)
	{
		auto result = BiologicalConstituents{};
		result.organicKg = inventory.organicKg * share;
		result.nutrientNitrogenKg = inventory.nutrientNitrogenKg * share;
		result.phosphorusKg = inventory.phosphorusKg * share;
		result.potassiumKg = inventory.potassiumKg * share;
		result.mineralsKg = inventory.mineralsKg * share;
		return result;
	}
	// Keep the living inventory consistent with the biomass ledger
	// (#571): the organic share is whatever dry mass the population
	// holds beyond its tracked nutrients, so the inventory always
	// sums to the living biomass exactly and the nutrients are never
	// silently resized by unit roundoff.
	inline void settleLivingOrganic(BiologicalConstituents& inventory, double livingBiomassKg)
	{
		inventory.organicKg = livingBiomassKg - (inventory.nutrientNitrogenKg
			+ inventory.phosphorusKg + inventory.potassiumKg + inventory.mineralsKg);
	}
	// Take a share of the living inventory, returning what came off
	// (#571). The nutrients come off as real tracked amounts and the
	// organic share is re-closed against the remaining biomass ledger,
	// so the inventory keeps summing to the living biomass exactly.
	inline BiologicalConstituents takeConstituents(BiologicalConstituents& inventory, double share,
		double remainingBiomassKg)
	{
		auto const removed = scaledConstituents(inventory, share);
		inventory.nutrientNitrogenKg -= removed.nutrientNitrogenKg;
		inventory.phosphorusKg -= removed.phosphorusKg;
		inventory.potassiumKg -= removed.potassiumKg;
		inventory.mineralsKg -= removed.mineralsKg;
		settleLivingOrganic(inventory, remainingBiomassKg);
		return removed;
	}
	// The constituent inventory a declared dry biomass carries at a
	// declared composition (#571). Nothing new is authored: the
	// inventory is derived wholly from settings that already exist, so
	// reset and persistence behaviour is untouched.
	inline BiologicalConstituents constituentsOf(double dryMassKg,
		ResourceCompositionSettings const& composition)
	{
		auto inventory = BiologicalConstituents{};
		inventory.nutrientNitrogenKg = dryMassKg * composition.nutrientNitrogen;
		inventory.phosphorusKg = dryMassKg * composition.phosphorus;
		inventory.potassiumKg = dryMassKg * composition.potassium;
		inventory.mineralsKg = dryMassKg * composition.otherMinerals;
		settleLivingOrganic(inventory, dryMassKg);
		return inventory;
	}
	// Add one ledger into another (#601).
	inline void addLedger(ConstituentLedger& target, ConstituentLedger const& source,
		double sign)
	{
		for (auto const& entry : source)
			target[entry.first] += sign * entry.second;
	}
	// Fixed-yield processes must not turn a changed feed into unfunded
	// constituents (or energy). Empty means the recipe is feasible.
	inline std::string unfundedConstituent(ConstituentLedger const& funded,
		ConstituentLedger const& required)
	{
		for (auto const& [key, amount] : required)
		{
			auto const available = funded.count(key) ? funded.at(key) : 0.0;
			if (amount > available + StationBalanceTolerance * std::max(1.0, std::abs(available)))
				return key;
		}
		return {};
	}
	// The constituent ledger a declared composition carries at a wet
	// mass, returned instead of added in place (#601).
	inline ConstituentLedger compositionLedgerOf(ResourceCompositionSettings const& composition,
		double wetMassKg, double sign)
	{
		auto ledger = ConstituentLedger{};
		addComposition(ledger, composition, wetMassKg, sign);
		return ledger;
	}
	// The constituent ledger a routed biological mass actually carries
	// (#601): the population's tracked dry constituents plus the
	// chemical energy the organic share carries on the organic basis.
	// Biological residue is dry material, so it brings no water.
	inline ConstituentLedger biologicalLedgerOf(BiologicalConstituents const& inventory,
		double energyKJPerKgOrganic)
	{
		auto ledger = ConstituentLedger{};
		ledger["water"] += 0.0;
		ledger["nutrientNitrogen"] += inventory.nutrientNitrogenKg;
		ledger["atmosphericNitrogen"] += 0.0;
		ledger["phosphorus"] += inventory.phosphorusKg;
		ledger["potassium"] += inventory.potassiumKg;
		ledger["otherMinerals"] += inventory.mineralsKg;
		ledger["dryOrganics"] += inventory.organicKg;
		ledger["energy"] += inventory.organicKg * energyKJPerKgOrganic;
		return ledger;
	}
	// The composition a compound store's material actually has (#601):
	// the authored declaration plus the drift of everything the store
	// has really received, renormalised to the store's current mass.
	// With no drift this is exactly the authored composition; with
	// drift the fractions describe the blend that is actually in the
	// store, so a process sizing its yields from the store's material
	// tracks the actual constituents instead of the original
	// declaration. The ledger is the source of truth: every fraction
	// is the actual constituent over the actual dry (or wet) mass.
	inline ResourceCompositionSettings effectiveCompositionOf(
		ResourceCompositionSettings const& authored, double totalKg,
		ConstituentLedger const& drift)
	{
		if (drift.empty() || totalKg <= 0.0) return authored;
		auto const actual = compositionLedgerOf(authored, totalKg, 1.0);
		auto blended = actual;
		for (auto const& entry : drift)
			blended[entry.first] += entry.second;
		auto result = authored;
		result.moisture = std::max(0.0, std::min(1.0, blended["water"] / totalKg));
		auto const dry = std::max(0.0, totalKg * (1.0 - result.moisture));
		if (dry > 0.0)
		{
			result.nutrientNitrogen = std::max(0.0, blended["nutrientNitrogen"] / dry);
			result.atmosphericNitrogen = std::max(0.0, blended["atmosphericNitrogen"] / dry);
			result.phosphorus = std::max(0.0, blended["phosphorus"] / dry);
			result.potassium = std::max(0.0, blended["potassium"] / dry);
			result.otherMinerals = std::max(0.0, blended["otherMinerals"] / dry);
			result.energyKJPerKgDry = std::max(0.0, blended["energy"] / dry);
		}
		else
		{
			result.nutrientNitrogen = 0.0;
			result.atmosphericNitrogen = 0.0;
			result.phosphorus = 0.0;
			result.potassium = 0.0;
			result.otherMinerals = 0.0;
			result.energyKJPerKgDry = 0.0;
		}
		return result;
	}
}
