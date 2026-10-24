#include "StationSystemsImpl.h"

#include <algorithm>
#include <cmath>

namespace core
{
	// The Condition layer (#840): the #408 microbial pattern read as a
	// machine rule rather than a biology rule.
	//
	// Degradation is neither a random draw nor a timer. It is a rate that
	// acts only while its stressor actually acts on the machine, so
	// removing the stressor stops the damage on the very next tick - the
	// machine never keeps degrading because it once ran hot, and it never
	// degrades on a schedule nobody asked for.
	//
	// Recovery is the mirror image, and keeps the #570 rule the microbial
	// layer already enforces: recovery is gated on the *absence* of active
	// stressors, not on the summed rate. Buying a repair while the fault
	// still bites would spend real stock to stand still, so the restore
	// waits, visibly, until the machine stops being hurt.
	//
	// Both rates integrate over the interval's own duration. Constant-stressor
	// Condition trajectories are independent of how advance calls are batched;
	// coupled thermal feedback still samples the interval's starting state.
	//
	// Crossing the model's seizure threshold is the catalogue's
	// DegradationCrossing onset: the machine's own wear spawns the Fault,
	// attributed to the crossing rather than to anyone who struck it. The
	// spawn is edge-triggered on the tick's whole change, so a machine that
	// sits below the threshold raises the Fault exactly once however many
	// ticks it stays there - and a machine restored above the threshold and
	// then worn back down again raises a fresh instance with a fresh onset.
	bool StationSystemsRuntime::Impl::Tick::condition()
	{
		if (!continuous) return true;
		for (auto& loop : runtime.coolantLoops)
		{
			if (!loop.authored.condition)
			{
				// No Condition model: the machine is exactly what it was
				// before this feature, and says so.
				loop.lastConditionDelta = 0.0;
				loop.lastStressors.clear();
				loop.lastRestoring = false;
				loop.lastRestoreStockUnits = 0.0;
				continue;
			}
			auto const& model = *loop.authored.condition;
			system = "Installation Condition";
			for (double value : {model.initialCondition, model.overheatStressC, model.overheatDegradePerHour,
				model.cavitationFillFraction, model.cavitationDegradePerHour, model.seizureThreshold,
				model.restorePerHour, model.stockUnitsPerConditionPoint})
				if (!requireFinite(value, "Condition model for '" + loop.authored.id + "'")) return false;
			if (model.overheatDegradePerHour < 0 || model.cavitationDegradePerHour < 0
				|| model.restorePerHour < 0 || model.stockUnitsPerConditionPoint < 0
				|| (loop.hasRestoreStock && model.stockUnitsPerConditionPoint <= 0))
				return violation = "Invalid Condition rates for '" + loop.authored.id + "'", false;
			if (loop.pendingRestoreTarget)
			{
				loop.restoreTarget = *loop.pendingRestoreTarget;
				appendBoundedEvent(runtime.events, tick, "repair", "Condition restore started on '"
					+ loop.authored.id + "' toward " + std::to_string(loop.restoreTarget));
				loop.pendingRestoreTarget.reset();
			}
			auto const before = loop.condition;
			std::string stressors;
			auto const stress = [&](bool active, double rate, char const* name)
			{
				if (!active) return;
				if (!stressors.empty()) stressors += '+';
				stressors += name;
				loop.condition = std::max(0.0, loop.condition - rate * hours);
			};
			// Heat stress is judged on the coolant the pump is actually
			// pushing: a loop running above its authored stress temperature
			// is a pump working against its rating.
			stress(loop.temperatureC + 1e-9 >= model.overheatStressC,
				model.overheatDegradePerHour, "overheat");
			// Cavitation is a running pump starved of fluid, not an idle
			// loop: with the pump seized (Fault active) there is no running
			// pump to cavitate, and the Fault already speaks for itself.
			stress(loop.coolantKg > 0.0 && !loop.seizure && loop.availabilityFactor > 0.0
					&& loop.condition > 0.0 && loop.authored.ratedFlowKgPerS > 0.0
					&& loop.coolantKg / loop.authored.coolantCapacityKg
						<= model.cavitationFillFraction + 1e-12,
				model.cavitationDegradePerHour, "cavitation");
			loop.lastStressors = stressors;
			// Funded recovery: bounded by the authored per-hour rate, by
			// the stock actually drawable above the store's protected
			// reserve, and by the distance still left to the commanded
			// target. The draw is booked through the ordinary store ledger
			// as the material it actually is, so the per-unit boundary
			// closes over the spent stock like any other consumption and
			// the protected reserve is never touched.
			loop.lastRestoring = false;
			loop.lastRestoreStockUnits = 0.0;
			if (stressors.empty() && loop.restoreTarget > loop.condition + StationConditionEpsilon)
			{
				auto points = std::min(loop.restoreTarget - loop.condition, model.restorePerHour * hours);
				if (loop.hasRestoreStock && points > 0.0)
				{
					auto const index = loop.restoreStoreIndex;
					auto const& stock = runtime.stores[index];
					auto const drawable = std::max(0.0,
						qualified[index] - stock.authored.protectedReserve);
					points = std::min(points, drawable / model.stockUnitsPerConditionPoint);
					if (points > 0.0)
					{
						auto const units = std::min(drawable, points * model.stockUnitsPerConditionPoint);
						points = units / model.stockUnitsPerConditionPoint;
						addLedger(stagedLoss, drawStoreActual(index,
							massToKg(stock.authored.unit, qualified[index] + unqualified[index]),
							massToKg(stock.authored.unit, units)), 1.0);
						qualified[index] -= units;
						flowOut[index] += units;
						stagedUnitLoss[stock.authored.unit] += units;
						loop.lastRestoreStockUnits = units;
						loop.restoreStockUnits += units;
						loop.condition += points;
						loop.lastRestoring = true;
					}
				}
				// The target is spent once reached: a restore is a job with
				// an end, not a permanent drain on the store.
				if (loop.condition >= loop.restoreTarget - StationConditionEpsilon)
				{
					loop.restoreTarget = 0.0;
					appendBoundedEvent(runtime.events, tick, "repair", "Condition restore completed on '"
						+ loop.authored.id + "'");
				}
			}
			loop.lastConditionDelta = loop.condition - before;
			// The crossing is judged on the tick's whole change: a machine
			// that fell through the threshold this tick spawns its Fault
			// now, whatever the path that took it there.
			if (!loop.seizure && before >= model.seizureThreshold
				&& loop.condition < model.seizureThreshold)
			{
				loop.seizure = StationFaultSnapshot{loop.authored.id, "pump-seizure", tick,
					FaultCause::Kind::DegradationCrossing,
					"Condition fell through " + std::to_string(model.seizureThreshold)
						+ " while " + (stressors.empty() ? "stressed" : stressors), 0};
				loop.availabilityFactor = 0.0;
				appendBoundedEvent(runtime.events, tick, "fault", "Fault 'pump-seizure' on '"
					+ loop.authored.id + "': Condition fell through "
					+ std::to_string(model.seizureThreshold) + " (" + stressors + ")");
			}
		}
		return true;
	}
}
