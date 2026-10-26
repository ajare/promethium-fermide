#include "StationSystemsImpl.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{
	// The permanent Wear layer (#843): capacity a machine consumes by
	// actually running, which nothing can heal.
	//
	// Wear is neither a Fault nor Condition: it is used-up capacity. The
	// accumulators count only real operation - cycles on real charge and
	// discharge, operating hours while the machine really works,
	// throughput on real flowed mass - plus calendar time, which acts
	// whether the machine works or rests. Each interval's fade is priced
	// at the rates current when the operation happened, so a later live
	// retune can neither cheapen nor worsen what was already spent: the
	// fade follows the machine's history, not its current paperwork.
	//
	// Repair never reverses Wear. A repair job clears a Fault and a funded
	// restore lifts Condition; neither touches the faded rating. "Fixed"
	// and "replaced" stay meaningfully different, and replacement is a
	// topology change - a reset, per the live-edit contract - not a
	// live edit.
	void StationSystemsRuntime::Impl::Tick::advanceWear(WearState& wear,
		InstallationWearSettings const& model,
		double cycles, double operatingHours, double throughputKg, double hours,
		std::optional<double> committedFloor)
	{
		auto const floor = committedFloor.value_or(wear.fraction);
		wear.cycles += cycles;
		wear.operatingHours += operatingHours;
		wear.throughputKg += throughputKg;
		wear.calendarHours += hours;
		// The fade delta is priced at this interval's rates. The raw fade
		// total is kept so the reported fraction can be re-clamped without
		// losing what a maxWearFraction clamp had hidden; a negative delta
		// unwinds only operation being taken back (a refunded discharge),
		// and the max(0, ...) keeps the total honest if unwinding meets an
		// accumulator that has since grown elsewhere.
		wear.fade += cycles * model.fadePerCycle
			+ operatingHours * model.fadePerOperatingHour
			+ throughputKg * model.fadePerThroughputKg
			+ hours * model.fadePerCalendarHour;
		// Normal operation is permanent even if the ceiling was lowered.
		// A refund may unwind provisional fade, but not the Wear committed
		// before this interval's discharge was reserved.
		auto fade = std::max(0.0, wear.fade);
		// Keep the disabled path's arithmetic exactly unchanged. Linear
		// damage spends capacity first; isotope decay acts on what is
		// left, never on the beginning-of-life rating again. Bank exposure
		// at the current rate, so a retune cannot reprice past decay.
		if (model.fadeHalfLifeHours > 0.0)
		{
			wear.decayHalfLives = std::min(1074.0,
				wear.decayHalfLives + hours / model.fadeHalfLifeHours);
		}
		if (wear.decayHalfLives > 0.0 && fade < 1.0)
			fade += (1.0 - fade)
				* -std::expm1(-std::log(2.0) * wear.decayHalfLives);
		// A pure exponential approaches complete fade but never reaches
		// it; keep that distinction even when binary64 rounds to one.
		if (wear.decayHalfLives > 0.0 && wear.fade < 1.0)
			fade = std::min(fade, std::nextafter(1.0, 0.0));
		wear.fraction = std::max(floor, std::min(model.maxWearFraction, fade));
	}

	bool StationSystemsRuntime::Impl::Tick::poisonFeed(std::optional<StationFaultSnapshot>& fault,
		WearState& wear, CatalystPoisoningSettings const& model, std::string const& id,
		size_t feed, bool exposed)
	{
		// Targeted live edits deliberately defer validation to the audited
		// boundary. Refuse malformed rates before they can unwind permanent
		// fade, including an edit made while the feed is clean or idle.
		if (!requireFinite(model.triggerConcentration, "catalyst contaminant threshold for '" + id + "'")
			|| !requireFinite(model.wearPerHour, "catalyst Wear rate for '" + id + "'")) return false;
		if (model.triggerConcentration < 0.0 || model.triggerConcentration >= 1.0 || model.wearPerHour <= 0.0)
			return violation = "Invalid catalyst-poisoning parameters for '" + id + "'", false;
		if (!exposed || qualified[feed] + unqualified[feed] <= 0.0) return true;
		auto const quality = waterQuality(feed, {}, 0.0, "catalyst", true);
		// Proportional drift draws can leave an ulp-scale cancellation
		// remainder after a pure-water refill. It is not a contaminant
		// exposure: do not latch a permanent Fault on numerical dust.
		if (quality.totalDissolved <= std::max(model.triggerConcentration,
			32.0 * std::numeric_limits<double>::epsilon())) return true;
		if (!fault)
		{
			fault = StationFaultSnapshot{id, "catalyst-poisoning", tick,
				FaultCause::Kind::FeedContamination, runtime.stores[feed].authored.id, 0.0};
			appendBoundedEvent(runtime.events, tick, "fault", "Fault 'catalyst-poisoning' on '"
				+ id + "': contaminated feed '" + runtime.stores[feed].authored.id + "'");
		}
		InstallationWearSettings fade;
		fade.fadePerOperatingHour = model.wearPerHour;
		advanceWear(wear, fade, 0.0, hours, 0.0, 0.0);
		return requireFinite(wear.fade, "catalyst poisoning Wear for '" + id + "'")
			&& requireFinite(wear.operatingHours, "catalyst exposure hours for '" + id + "'");
	}

	bool StationSystemsRuntime::Impl::Tick::wear()
	{
		if (!continuous) return true;
		// Check every shared model, including idle machines and targeted live
		// edits, before any accumulator can bank an invalid decay rate.
		auto validDecay = [&](auto const& model) {
			if (!model) return true;
			if (!requireFinite(model->fadeHalfLifeHours, "Wear half-life")
				|| !requireFinite(model->initialAgeHours, "Wear initial age")) return false;
			if (model->fadeHalfLifeHours < 0.0 || model->initialAgeHours < 0.0
				|| (model->fadeHalfLifeHours == 0.0 && model->initialAgeHours != 0.0))
				return violation = "Invalid Wear half-life or initial age", false;
			return true;
		};
		for (auto& battery : runtime.batteries)
		{
			if (!validDecay(battery.authored.capacityFade)) return false;
			if (!battery.authored.capacityFade) continue;
			system = "Installation Wear";
			auto const& model = *battery.authored.capacityFade;
			if (!requireFinite(model.fadePerCycle, "capacity fade per cycle for '" + battery.authored.id + "'")
				|| !requireFinite(model.fadePerOperatingHour, "capacity fade per operating hour for '" + battery.authored.id + "'")
				|| !requireFinite(model.fadePerCalendarHour, "capacity fade per calendar hour for '" + battery.authored.id + "'")
				|| !requireFinite(model.maxWearFraction, "capacity fade ceiling for '" + battery.authored.id + "'"))
				return false;
			// A battery operates in an interval in which it actually
			// charged or discharged; the cycle accumulators were already
			// fed at those sites with the energy that really moved.
			// Calendar ageing acts whether the bank works or sits idle.
			auto const operated = battery.lastChargeKW > 0.0 || battery.lastDischargeKW > 0.0;
			advanceWear(battery.wear, model, 0.0, operated ? hours : 0.0, 0.0, hours);
			// The accumulators and the fade they drive must stay finite and
			// in range; a poisoned rate fails the tick and rolls back with
			// everything else the boundary touched.
			if (!requireFinite(battery.wear.cycles, "accumulated cycles for '" + battery.authored.id + "'")
				|| !requireFinite(battery.wear.operatingHours, "accumulated operating hours for '" + battery.authored.id + "'")
				|| !requireFinite(battery.wear.calendarHours, "accumulated calendar hours for '" + battery.authored.id + "'")
				|| !requireFinite(battery.wear.fade, "accumulated capacity fade for '" + battery.authored.id + "'")
				|| battery.wear.fraction < 0.0 || battery.wear.fraction > 1.0)
				return violation = "Wear state left the declared range for '" + battery.authored.id + "'", false;
		}
		for (auto& loop : runtime.coolantLoops)
		{
			if (!validDecay(loop.authored.wear)) return false;
			if (!loop.authored.wear) continue;
			system = "Installation Wear";
			auto const& model = *loop.authored.wear;
			if (!requireFinite(model.fadePerOperatingHour, "wear per operating hour for '" + loop.authored.id + "'")
				|| !requireFinite(model.fadePerThroughputKg, "wear per throughput kilogram for '" + loop.authored.id + "'")
				|| !requireFinite(model.fadePerCalendarHour, "wear per calendar hour for '" + loop.authored.id + "'")
				|| !requireFinite(model.maxWearFraction, "wear ceiling for '" + loop.authored.id + "'"))
				return false;
			// A loop operates in an interval in which coolant actually
			// circulated, and its throughput is the mass that actually
			// moved: an idle, dry or isolated loop banks no operating
			// hours and no throughput, but it still ages on the calendar.
			// Flow is kg/s, so the mass integrates over the interval's
			// seconds, not its hours.
			auto const flowedKg = std::max(0.0, loop.lastFlowKgPerS)
				* static_cast<double>(seconds);
			advanceWear(loop.wear, model, 0.0, flowedKg > 0.0 ? hours : 0.0, flowedKg, hours);
			if (!requireFinite(loop.wear.operatingHours, "accumulated operating hours for '" + loop.authored.id + "'")
				|| !requireFinite(loop.wear.throughputKg, "accumulated throughput for '" + loop.authored.id + "'")
				|| !requireFinite(loop.wear.calendarHours, "accumulated calendar hours for '" + loop.authored.id + "'")
				|| !requireFinite(loop.wear.fade, "accumulated wear fade for '" + loop.authored.id + "'")
				|| loop.wear.fraction < 0.0 || loop.wear.fraction > 1.0)
				return violation = "Wear state left the declared range for '" + loop.authored.id + "'", false;
		}
		// #848: the catalogue machine Wear advance. Every mode names its own
		// accumulator and its own authored model, so a machine can only ever
		// accrue the fade it opted into. None of these modes counts cycles -
		// no machine in the catalogue has a counted cycle - so the cycle term
		// stays zero and validation refuses a cycle rate outright.
		//
		// `activity` gates the operating-hour term: a machine that did
		// nothing during the systems tick just closed accrues no operating
		// hours, so an idle machine never wears toward its ceiling. The
		// calendar term runs regardless, which is what makes an opted-in
		// machine unwearable to keep forever. `mass` is the material the mode
		// metered - zero where the mode meters no mass at all.
		//
		// The fade is monotone by construction and re-clamped against the
		// authored ceiling, so retuning `maxWearFraction` mid-run can stop
		// future fade but never heal what already happened. Repair is not
		// called here and never touches these fields: a repaired machine comes
		// back exactly as worn as it went in.
		auto advanceMachine = [&](auto& machine, WearState& wear,
			std::optional<InstallationWearSettings> const& model, char const* mode,
			double activity, double mass) {
			if (!validDecay(model)) return false;
			if (!model) return true;
			system = "Installation Wear";
			if (!requireFinite(model->fadePerOperatingHour, "Wear operating rate")
				|| !requireFinite(model->fadePerThroughputKg, "Wear throughput rate")
				|| !requireFinite(model->fadePerCalendarHour, "Wear calendar rate")
				|| !requireFinite(model->maxWearFraction, "Wear ceiling")) return false;
			if (model->fadePerCycle != 0.0 || model->fadePerOperatingHour < 0.0
				|| model->fadePerThroughputKg < 0.0 || model->fadePerCalendarHour < 0.0
				|| model->maxWearFraction <= 0.0 || model->maxWearFraction > 1.0)
				return violation = "Invalid machine Wear model for '" + machine.authored.id + "'", false;
			auto const before = wear.fraction;
			advanceWear(wear, *model, 0.0, activity > 0.0 ? hours : 0.0, mass, hours);
			// Retuning a ceiling may stop future fade, never heal past Wear.
			wear.fraction = std::max(before, wear.fraction);
			if (!requireFinite(wear.fade, "accumulated machine Wear")
				|| !requireFinite(wear.operatingHours, "Wear operating hours")
				|| !requireFinite(wear.throughputKg, "Wear throughput")
				|| !requireFinite(wear.calendarHours, "Wear calendar hours")) return false;
			if (before == 0.0 && wear.fraction > 0.0)
				appendBoundedEvent(runtime.events, tick, "fault", "Wear '" + std::string(mode)
					+ "' on '" + machine.authored.id + "': permanent capability fade began");
			return true;
		};
		// The activity each mode reads is the work it actually did, and the
		// mass it metered where the mode meters mass. Solar is gated on the
		// power the array actually delivered rather than on illumination
		// alone: an array whose string is lost or whose tracker has failed
		// sees no sun it cannot collect, so it does not wear while shaded.
		for (auto& machine : runtime.heatSources)
			if (!advanceMachine(machine, machine.wear, machine.authored.wear,
				"decay", machine.deliveredOutputKW, 0.0)) return false;
		for (auto& machine : runtime.solarArrays)
			if (!advanceMachine(machine, machine.cellDegradation, machine.authored.cellDegradation,
				"cell-degradation", machine.lastAvailableKW, 0.0)) return false;
		for (auto& machine : runtime.generators)
			if (!advanceMachine(machine, machine.fouledInjector, machine.authored.fouledInjector,
				"fouled-injector", machine.lastAverageOutputKW, 0.0)) return false;
		for (auto& machine : runtime.heatEngines)
			if (!advanceMachine(machine, machine.efficiencyDrift, machine.authored.efficiencyDrift,
				"efficiency-drift", machine.lastThermalInputKW, 0.0)) return false;
		for (auto& machine : runtime.electrolyzers)
			if (!advanceMachine(machine, machine.membraneDegradation, machine.authored.membraneDegradation,
				"membrane-degradation", machine.lastSplitKg, machine.lastSplitKg)) return false;
		for (auto& machine : runtime.waterTreatments)
			if (!advanceMachine(machine, machine.membraneFouling, machine.authored.membraneFouling,
				"membrane-fouling", machine.lastProcessedKg, machine.lastProcessedKg)) return false;
		for (auto& machine : runtime.co2Captures)
			if (!advanceMachine(machine, machine.absorbentDegradation, machine.authored.absorbentDegradation,
				"absorbent-degradation", machine.lastCapturedKg, machine.lastCapturedKg)) return false;
		for (auto& machine : runtime.fuelCells)
			if (!advanceMachine(machine, machine.stackDegradation, machine.authored.stackDegradation,
				"stack-degradation", machine.lastOutputKW, machine.lastHydrogenKg)) return false;
		for (auto& machine : runtime.sabatierReactors)
			if (!advanceMachine(machine, machine.catalystDeactivation, machine.authored.catalystDeactivation,
				"catalyst-deactivation", machine.lastCo2ConsumedKg, machine.lastCo2ConsumedKg)) return false;
		for (auto& machine : runtime.methaneProcessors)
			if (!advanceMachine(machine, machine.catalystDeactivation, machine.authored.catalystDeactivation,
				"catalyst-deactivation", machine.lastMethaneConsumedKg, machine.lastMethaneConsumedKg)) return false;
		return true;
	}
}
