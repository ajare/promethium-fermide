#include "StationSystemsImpl.h"

#include <algorithm>
#include <cmath>

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
		double cycles, double operatingHours, double throughputKg, double hours)
	{
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
		wear.fraction = std::min(model.maxWearFraction, std::max(0.0, wear.fade));
	}

	bool StationSystemsRuntime::Impl::Tick::wear()
	{
		if (!continuous) return true;
		for (auto& battery : runtime.batteries)
		{
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
		return true;
	}
}
