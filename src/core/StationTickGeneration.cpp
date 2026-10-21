#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::scheduledState()
	{
		// Positive integration uses forcing at its left boundary; the
		// zero-time boundary pass publishes starts/restorations at the end.
		auto const outageElapsed = continuous ? elapsed - seconds : elapsed;
		for (auto& generator : runtime.generators) generator.availabilityFactor = 1.0;
		for (auto& loop : runtime.coolantLoops) loop.availabilityFactor = 1.0;
		for (auto& chiller : runtime.chillers) chiller.availabilityFactor = 1.0;
		// Newly due outage starts and restorations are staged across all
		// schedules and announced in the declared scenario event order
		// (#577): occurrence simulation time ascending, then service
		// priority descending, then declaration order - the same rule
		// that governs resupply deliveries and terminal ejections (#392).
		// The occurrence counters still advance per schedule, so every
		// window's start and restoration is staged exactly once however
		// many ticks an accelerated update batches together. When one
		// schedule's restoration lands on the same instant as its own
		// next start (recurrence == duration) the closing window is
		// announced restored before the opening window begins, so the
		// event log reads in causal order.

		for (size_t index = 0; index < runtime.outages.size(); ++index)
		{
			auto& outage = runtime.outages[index];
			auto const& schedule = outage.authored;
			auto const startedDue = stationOutageOccurrencesStarted(
				schedule.firstOccurrenceSeconds, schedule.durationSeconds,
				schedule.recurrenceSeconds, outageElapsed);
			auto const completedDue = stationOutageOccurrencesCompleted(
				schedule.firstOccurrenceSeconds, schedule.durationSeconds,
				schedule.recurrenceSeconds, outageElapsed);
			// Stage each newly opened window and each newly closed
			// (restored) window with its occurrence time, so the event
			// log brackets every outage even when several start and end
			// between two sampled ticks.
			for (uint64_t occurrence = outage.occurrencesStarted;
				occurrence < startedDue; ++occurrence)
			{
				auto const when = schedule.recurrenceSeconds > 0.0
					? schedule.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * schedule.recurrenceSeconds
					: schedule.firstOccurrenceSeconds;
				outage.lastStartSeconds = when;
				dueOutages.push_back({{when, schedule.priority, index}, false,
					"Outage '" + schedule.id + "' on " + outage.installationKind + " '"
					+ schedule.installationId + "' began at " + std::to_string(when)
					+ " s: capacity reduced to " + std::to_string(schedule.capacityFraction)});
			}
			for (uint64_t occurrence = outage.occurrencesCompleted;
				occurrence < completedDue; ++occurrence)
			{
				auto const when = schedule.recurrenceSeconds > 0.0
					? schedule.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * schedule.recurrenceSeconds
						+ schedule.durationSeconds
					: schedule.firstOccurrenceSeconds + schedule.durationSeconds;
				outage.lastRestoreSeconds = when;
				dueOutages.push_back({{when, schedule.priority, index}, true,
					"Outage '" + schedule.id + "' on " + outage.installationKind + " '"
					+ schedule.installationId + "' restored full declared availability at "
					+ std::to_string(when) + " s (no inventory replenished)"});
			}
			outage.occurrencesStarted = startedDue;
			outage.occurrencesCompleted = completedDue;
			outage.lastActive = startedDue > completedDue;
			outage.exhausted = schedule.recurrenceSeconds <= 0.0 && completedDue >= 1;
		}
		std::sort(dueOutages.begin(), dueOutages.end(),
			[](DueOutageEvent const& a, DueOutageEvent const& b)
			{
				if (a.key < b.key) return true;
				if (b.key < a.key) return false;
				// Same schedule, same instant: the closing window's
				// restoration precedes the opening window's start.
				return a.isRestore && !b.isRestore;
			});
		for (auto const& event : dueOutages)
			appendBoundedEvent(runtime.events, tick, "outage", event.message);
		// Apply the reduction to each active outage's target, deepest
		// window wins so the result never depends on the declaration
		// order of coincident outages.
		for (auto& outage : runtime.outages)
		{
			if (!outage.lastActive) continue;
			auto& factor = outage.target == OutageState::Target::Generator
				? runtime.generators[outage.targetIndex].availabilityFactor
				: outage.target == OutageState::Target::CoolantLoop
					? runtime.coolantLoops[outage.targetIndex].availabilityFactor
					: runtime.chillers[outage.targetIndex].availabilityFactor;
			factor = std::min(factor, outage.authored.capacityFraction);
		}
		// Resource-constrained maintenance (#414) runs right behind
		// the outage pass: an Active job holds its target installation
		// at zero availability on top of any outage ceiling, and its
		// reservation is evaluated against the same staged inventory
		// every other process draws from this tick. Jobs run in the
		// declared deterministic order (priority descending,
		// declaration order), so when two jobs need the same tool or
		// part the higher-precedence job holds it and the lower one
		// waits - a unit of material is never spent twice.
		//
		// The tick runs three ordered phases:
		//  1. Reservation integrity. Every reservation an Active job
		//     holds must still be covered by this tick's staged
		//     inventory and waste headroom. Where another process
		//     drew the reserved material away, the lowest-precedence
		//     job holding that store is interrupted first: its
		//     reservation is released and its progress discarded.
		//     Nothing was spent, so nothing needs refunding, and the
		//     material that got away stays with whoever took it -
		//     no double-spend either way.
		//  2. Progress and completion. An Active job accrues the
		//     fixed tick against its duration. A job that completes
		//     releases its reservation before any start attempt, so
		//     the tools it returns are available to waiting work in
		//     the same tick. Completion spends for real: consumed
		//     parts and consumables leave their sources and land in
		//     the waste store as qualified, disposal-ready material;
		//     reusable tools are simply returned - the same tools,
		//     not replenished ones.
		//  3. Start attempts. A job whose window is open reserves
		//     every prerequisite from the qualified inventory that
		//     higher-precedence work has not already taken. No
		//     reservation, no work: the job is Blocked with the
		//     binding prerequisite named, and nothing completes for
		//     free. A window that closes before the job ever started
		//     is a missed occurrence, reported and never back-filled.
		// Maintenance is isolated over the interval following a boundary.
		// Other sampled profiles retain their existing endpoint convention.
		auto const sampledElapsed = elapsed;
		if (continuous) elapsed -= seconds;
		if (!runtime.maintenanceJobs.empty())
		{
			auto const occurrenceTime = [](MaintenanceState const& job, uint64_t occurrence)
			{
				return job.authored.recurrenceSeconds > 0.0
					? job.authored.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * job.authored.recurrenceSeconds
					: job.authored.firstOccurrenceSeconds;
			};
			auto const hasOccurrence = [](MaintenanceState const& job, uint64_t occurrence)
			{
				return job.authored.recurrenceSeconds > 0.0 || occurrence == 0;
			};
			// A reservation covers its need when the drawable
			// inventory reaches the quantity within the declared
			// balance tolerance: float noise at the boundary is
			// never a spurious block, and a genuine shortage still is.
			auto const covers = [](double available, double needed)
			{
				return available + StationBalanceTolerance * std::max(1.0, needed) >= needed;
			};
			auto const interruptJob = [&](MaintenanceState& job, std::string const& reason)
			{
				for (auto& requirement : job.requirements) requirement.reserved = 0.0;
				job.phase = StationMaintenancePhase::Blocked;
				job.progressSeconds = 0.0;
				job.blockReason = "interrupted: " + reason;
				appendBoundedEvent(runtime.events, tick, "maintenance",
					"Maintenance '" + job.authored.id + "' on " + job.installationKind
					+ " '" + job.authored.installationId + "' interrupted at "
					+ std::to_string(elapsed) + " s: " + reason
					+ "; progress discarded, reservation released");
			};
			// Phase 1: reservation integrity. Interrupt the
			// lowest-precedence Active job holding any store that
			// can no longer cover the total reservation on it,
			// until every held reservation is covered again.
			for (;;)
			{
				std::map<size_t, double> totalReserved;
				std::map<size_t, double> totalWasteClaim;
				for (auto const& job : runtime.maintenanceJobs)
				{
					if (job.phase != StationMaintenancePhase::Active) continue;
					for (auto const& requirement : job.requirements)
						totalReserved[requirement.storeIndex] += requirement.reserved;
					if (job.wasteStoreIndex == SIZE_MAX) continue;
					for (size_t requirementIndex = 0;
						requirementIndex < job.requirements.size(); ++requirementIndex)
						if (job.authored.requirements[requirementIndex].consumed)
							totalWasteClaim[job.wasteStoreIndex] += job.requirements[requirementIndex].reserved;
				}
				std::string violatedStore;
				bool violatedWaste = false;
				size_t violatedStoreIndex = SIZE_MAX;
				for (auto const& [storeIndex, reserved] : totalReserved)
				{
					if (covers(qualified[storeIndex], reserved)) continue;
					violatedStore = runtime.stores[storeIndex].authored.id;
					violatedStoreIndex = storeIndex;
					break;
				}
				if (violatedStore.empty())
				{
					for (auto const& [storeIndex, claim] : totalWasteClaim)
					{
						auto const headroom = runtime.stores[storeIndex].authored.capacity
							- qualified[storeIndex] - unqualified[storeIndex];
						if (covers(headroom, claim)) continue;
						violatedStore = runtime.stores[storeIndex].authored.id;
						violatedStoreIndex = storeIndex;
						violatedWaste = true;
						break;
					}
				}
				if (violatedStore.empty()) break;
				// Lowest precedence first: walk the declared order
				// backwards and interrupt the last job holding the
				// violated store, so the most important work keeps
				// its reservation as long as the inventory lasts.
				bool released = false;
				for (auto orderIt = runtime.maintenanceOrder.rbegin();
					orderIt != runtime.maintenanceOrder.rend() && !released; ++orderIt)
				{
					auto& job = runtime.maintenanceJobs[*orderIt];
					if (job.phase != StationMaintenancePhase::Active) continue;
					bool holds = violatedWaste
						? job.wasteStoreIndex == violatedStoreIndex
						: std::any_of(job.requirements.begin(), job.requirements.end(),
							[&](MaintenanceState::RequirementState const& requirement)
							{ return requirement.storeIndex == violatedStoreIndex
								&& requirement.reserved > 0.0; });
					if (!holds) continue;
					interruptJob(job, violatedWaste
						? "waste store '" + violatedStore
							+ "' lost the headroom for the reserved consumed material"
						: "store '" + violatedStore
							+ "' fell below the total reserved material");
					released = true;
				}
				if (!released) break;
			}
			// Phase 2: progress and completion. Completions release
			// their reservations before start attempts run, so a
			// returned tool is available to waiting work this tick.
			for (auto const index : runtime.maintenanceOrder)
			{
				auto& job = runtime.maintenanceJobs[index];
				if (job.phase != StationMaintenancePhase::Active) continue;
				if (!continuous)
					job.progressSeconds = std::max(0.0, elapsed - job.lastStartSeconds);
				if (elapsed < job.lastStartSeconds + job.authored.durationSeconds) continue;
				job.progressSeconds = job.authored.durationSeconds;
				double consumedTotal = 0.0;
				for (size_t requirementIndex = 0;
					requirementIndex < job.requirements.size(); ++requirementIndex)
				{
					auto& requirement = job.requirements[requirementIndex];
					if (!job.authored.requirements[requirementIndex].consumed)
					{
						// Reusable tool: returned, never consumed.
						requirement.lastConsumed = 0.0;
						requirement.reserved = 0.0;
						continue;
					}
					auto const spent = requirement.reserved;
					auto const spentSourceKgBefore = massToKg(
						runtime.stores[requirement.storeIndex].authored.unit,
						qualified[requirement.storeIndex]
							+ unqualified[requirement.storeIndex]);
					qualified[requirement.storeIndex] -= spent;
					flowOut[requirement.storeIndex] += spent;
					qualified[job.wasteStoreIndex] += spent;
					flowIn[job.wasteStoreIndex] += spent;
					// The consumed material keeps its actual makeup on
					// the way into waste: validation pins the waste
					// store to the requirement store's declared
					// composition, so any drift the material really
					// carries rides with it instead of being laundered
					// into the waste store's declaration (#601).
					moveStoreDrift(requirement.storeIndex, job.wasteStoreIndex,
						spentSourceKgBefore, massToKg(
							runtime.stores[requirement.storeIndex].authored.unit, spent));
					consumedTotal += spent;
					requirement.lastConsumed = spent;
					requirement.consumedTotal += spent;
					requirement.reserved = 0.0;
				}
				job.phase = StationMaintenancePhase::Completed;
				++job.occurrencesCompleted;
				job.lastCompleteSeconds = elapsed;
				job.blockReason.clear();
				appendBoundedEvent(runtime.events, tick, "maintenance",
					"Maintenance '" + job.authored.id + "' on " + job.installationKind
					+ " '" + job.authored.installationId + "' completed at "
					+ std::to_string(elapsed)
					+ " s: " + std::to_string(consumedTotal)
					+ " unit(s) of consumed material booked into waste store '"
					+ (job.wasteStoreIndex == SIZE_MAX ? std::string("none")
						: runtime.stores[job.wasteStoreIndex].authored.id)
					+ "', reusable tools returned, installation restored (no supplies replenished)");
				// Advance past this occurrence; any occurrence whose
				// window opened while this one was still running is
				// missed - a late job never back-fills the schedule.
				++job.occurrence;
				while (hasOccurrence(job, job.occurrence)
					&& occurrenceTime(job, job.occurrence) + StationScheduleTolerance < elapsed)
				{
					++job.occurrencesMissed;
					appendBoundedEvent(runtime.events, tick, "maintenance",
						"Maintenance '" + job.authored.id + "' on " + job.installationKind
						+ " '" + job.authored.installationId + "' missed the window at "
						+ std::to_string(occurrenceTime(job, job.occurrence))
						+ " s while earlier work was still running");
					++job.occurrence;
				}
				if (!hasOccurrence(job, job.occurrence)) job.exhausted = true;
			}
			// Phase 3: start attempts in the declared order. The
			// running reservation total carries the material held
			// by every job that already reserved this tick, so a
			// later job never aims at kilograms a higher-precedence
			// job has taken. The waste side needs the same ledger
			// (#586): a job that completes later still owes its
			// consumed material to its waste store, so the pending
			// waste claim of every Active job - plus every job that
			// starts ahead in this pass - is subtracted from the
			// headroom before a start is granted. Without it two
			// jobs each reserving into the same tight waste store
			// both start, only for the lower one to be interrupted
			// and restarted against the very headroom it can never
			// get, cycling its installation off service for nothing.
			std::map<size_t, double> reservedByStore;
			std::map<size_t, double> wasteClaimByStore;
			for (auto const& job : runtime.maintenanceJobs)
			{
				if (job.phase != StationMaintenancePhase::Active) continue;
				for (auto const& requirement : job.requirements)
					reservedByStore[requirement.storeIndex] += requirement.reserved;
				if (job.wasteStoreIndex == SIZE_MAX) continue;
				for (size_t requirementIndex = 0;
					requirementIndex < job.requirements.size(); ++requirementIndex)
					if (job.authored.requirements[requirementIndex].consumed)
						wasteClaimByStore[job.wasteStoreIndex]
							+= job.requirements[requirementIndex].reserved;
			}
			for (auto const index : runtime.maintenanceOrder)
			{
				auto& job = runtime.maintenanceJobs[index];
				if (job.exhausted) continue;
				// Jobs that are Active were advanced in phase 2; only
				// idle, completed and blocked jobs attempt a start.
				if (job.phase == StationMaintenancePhase::Active) continue;
				// One window policy for every non-Active phase (#587):
				// an occurrence whose window closed before the job ever
				// started is recorded missed, reported, and never back-
				// filled - whether the job sat Idle, Completed an earlier
				// run, or was Blocked without its prerequisites. The
				// deadline is the authored window, not the tick grid: a
				// sub-tick window that no fixed tick could open is missed
				// the same way, and every window already closed at this
				// tick's time is skipped in this pass so a late pass never
				// replays an old window as startable. Only a window that
				// is still open admits a start attempt.
				std::string missedReason = job.blockReason;
				while (hasOccurrence(job, job.occurrence)
					&& elapsed >= occurrenceTime(job, job.occurrence)
						+ job.authored.durationSeconds)
				{
					++job.occurrencesMissed;
					appendBoundedEvent(runtime.events, tick, "maintenance",
						"Maintenance '" + job.authored.id + "' on "
						+ job.installationKind + " '" + job.authored.installationId
						+ "' missed its window at "
						+ std::to_string(occurrenceTime(job, job.occurrence))
						+ " s: " + (missedReason.empty()
							? std::string("the window closed before the job started")
							: missedReason));
					++job.occurrence;
					job.progressSeconds = 0.0;
					job.blockReason.clear();
					missedReason.clear();
					job.phase = StationMaintenancePhase::Idle;
					if (!hasOccurrence(job, job.occurrence))
					{
						job.exhausted = true;
						break;
					}
				}
				if (job.exhausted) continue;
				// The current occurrence's window has not opened yet:
				// nothing to attempt this tick.
				if (!hasOccurrence(job, job.occurrence)
					|| elapsed < occurrenceTime(job, job.occurrence))
					continue;
				std::string blockReason;
				double consumedTotal = 0.0;
				for (size_t requirementIndex = 0;
					requirementIndex < job.requirements.size(); ++requirementIndex)
				{
					auto const& requirement = job.requirements[requirementIndex];
					auto const& source = runtime.stores[requirement.storeIndex].authored;
					auto const drawable = qualified[requirement.storeIndex]
						- reservedByStore[requirement.storeIndex];
					if (!covers(drawable, job.authored.requirements[requirementIndex].quantity))
					{
						blockReason = std::string(job.authored.requirements[requirementIndex].consumed
							? "part/consumable store '" : "tool store '")
							+ source.id + "' cannot cover "
							+ std::to_string(job.authored.requirements[requirementIndex].quantity)
							+ " " + source.unit + " (drawable "
							+ std::to_string(std::max(0.0, drawable)) + ")";
						break;
					}
					if (job.authored.requirements[requirementIndex].consumed)
						consumedTotal += job.authored.requirements[requirementIndex].quantity;
				}
				if (blockReason.empty() && job.wasteStoreIndex != SIZE_MAX)
				{
					auto const& waste = runtime.stores[job.wasteStoreIndex].authored;
					// Free space is what the store can still take once
					// every other job's pending waste claim is held out
					// (#586): the claim lands on completion, but the
					// space is committed the moment the job starts.
					auto const headroom = waste.capacity
						- qualified[job.wasteStoreIndex] - unqualified[job.wasteStoreIndex]
						- wasteClaimByStore[job.wasteStoreIndex];
					if (!covers(headroom, consumedTotal))
						blockReason = "waste store '" + waste.id
							+ "' lacks headroom for the consumed material ("
							+ std::to_string(consumedTotal) + " " + waste.unit
							+ " needed, " + std::to_string(std::max(0.0, headroom)) + " free)";
				}
				if (!blockReason.empty())
				{
					if (job.phase != StationMaintenancePhase::Blocked)
						appendBoundedEvent(runtime.events, tick, "maintenance",
							"Maintenance '" + job.authored.id + "' on " + job.installationKind
							+ " '" + job.authored.installationId + "' blocked at "
							+ std::to_string(elapsed) + " s: " + blockReason);
					job.phase = StationMaintenancePhase::Blocked;
					job.blockReason = std::move(blockReason);
					continue;
				}
				// Reserved: the work starts now, the installation
				// goes off service, and every prerequisite is held
				// ahead of the lower-precedence jobs still to come.
				for (size_t requirementIndex = 0;
					requirementIndex < job.requirements.size(); ++requirementIndex)
				{
					job.requirements[requirementIndex].reserved
						= job.authored.requirements[requirementIndex].quantity;
					reservedByStore[job.requirements[requirementIndex].storeIndex]
						+= job.requirements[requirementIndex].reserved;
				}
				if (job.wasteStoreIndex != SIZE_MAX && consumedTotal > 0.0)
					wasteClaimByStore[job.wasteStoreIndex] += consumedTotal;
				job.phase = StationMaintenancePhase::Active;
				job.progressSeconds = 0.0;
				job.blockReason.clear();
				++job.occurrencesStarted;
				job.lastStartSeconds = elapsed;
				appendBoundedEvent(runtime.events, tick, "maintenance",
					"Maintenance '" + job.authored.id + "' on " + job.installationKind
					+ " '" + job.authored.installationId + "' started at "
					+ std::to_string(elapsed)
					+ " s: prerequisites reserved, installation isolated for "
					+ std::to_string(job.authored.durationSeconds) + " s of work");
			}
			// Isolation: an Active maintenance job holds its target
			// installation at zero availability on top of any
			// outage ceiling - work and rated service never happen
			// at the same time on the same machine.
			for (auto& job : runtime.maintenanceJobs)
			{
				job.lastIsolated = job.phase == StationMaintenancePhase::Active;
				if (!job.lastIsolated) continue;
				auto& factor = job.target == MaintenanceState::Target::Generator
					? runtime.generators[job.targetIndex].availabilityFactor
					: job.target == MaintenanceState::Target::CoolantLoop
						? runtime.coolantLoops[job.targetIndex].availabilityFactor
						: runtime.chillers[job.targetIndex].availabilityFactor;
				factor = std::min(factor, 0.0);
			}
		}
		elapsed = sampledElapsed;
		return true;
	}

	bool StationSystemsRuntime::Impl::Tick::generation()
	{
		if (!scheduledState()) return false;
		// Fuelled generators run ahead of the load allocation (#399):
		// their electrical output joins the shared generation pool for
		// this tick's loads, their startup cranking draws ride the same
		// pool ahead of discretionary demand, and their waste, startup
		// and decay heat land in the thermal integration below - each
		// booked exactly once at the sink boundary. A generator is a
		// dispatchable source, not an unlimited power setting: output
		// is bounded by the installed rating, the ramp limit and the
		// fuel actually drawable, and it only runs once the startup
		// energy has been received while cooling is qualified.
		generatorHeatBySink = std::vector<double>(runtime.reservoirs.size(), 0.0);
		totalGeneratorOutputKW = double{0.0};
		totalGeneratorStartupKW = double{0.0};
		totalGeneratorWasteKW = double{0.0};
		totalGeneratorDecayKW = double{0.0};
		// Fuel energy is authored in the store's unit; the generator's
		// physics runs in kWh. One factor converts store units to kWh
		// and its reciprocal converts the required fuel energy back, so
		// host and sandbox agree on every draw.

		// #415: automatic electrical dispatch. When the controller is
		// Automatic it commands the dispatchable sources against the net
		// load the sun leaves behind: the station-wide electrical demand
		// of the last committed tick minus this tick's solar. Generators
		// are commanded first and fuel-cell backups second, so the
		// life-support reactant stores are spent only when the fuelled
		// fleet cannot cover the load. Every command is capped by what
		// the machine can physically deliver this tick - the installed
		// rating under outages, the fuel or reactants actually drawable
		// above the protected reserves, and qualified cooling - so
		// dispatch never fabricates sustained supply: what no available
		// source can take stays explicit as the dispatch shortfall,
		// beside the unmet critical demand the allocation publishes.
		// A source that cannot run this tick is passed over, never
		// reserved: the target flows on to the next source that can.
		// In Manual mode the controller is inert and every source keeps
		// its authored commanded output.
		// #578: a command is not capacity. The dispatcher still commands
		// a generator toward the level it should hold, but only the
		// output the machine can actually put on the bus this tick is
		// booked against the target: a stopped machine serves nothing
		// while it cranks, a starting machine serves nothing, a machine
		// in shutdown decay serves nothing, and a running machine is
		// bounded by the ramp step it can climb from its current output.
		// The residual - the load the primary fleet cannot serve this
		// tick - flows on to the backup instead of being reserved by a
		// machine that will deliver zero, and the prorated cranking draw
		// of a legitimate start rides the target as demand of its own,
		// so an available backup funds the start rather than the
		// stopped machine deadlocking forever with nothing to crank on.
		// A start is still never self-funded: the stopped machine's own
		// output is not in the pool that starts it.
		dispatchAutomatic = runtime.settings.dispatchMode == StationDispatchMode::Automatic;
		dispatchLoadTargetKW = dispatchAutomatic
			? std::max(0.0, runtime.lastDispatchDemandKW - availableGeneration)
			: 0.0;
		dispatchDtSeconds = static_cast<double>(seconds);
		// The cranking draw a machine owes the pool this tick: the same
		// prorated purchase the phase machine makes - never more than
		// the authored startup power, never more than the startup energy
		// still owed (#562).

		dispatchStartupSupportKW = double{0.0};
		dispatchRemainingKW = dispatchLoadTargetKW;
		for (auto& generator : runtime.generators)
		{
			generator.dispatchCommandKW = 0.0;
			auto ports = materialPorts("Generator", generator.authored.id);
			if (!dispatchAutomatic) continue;
			auto const effectiveRatedKW = generator.authored.ratedOutputKW
				* generator.availabilityFactor;
			if (effectiveRatedKW <= 0.0) continue;
			auto const coolingAvailable = sinkQualified(generator.sinkIndex, generator.authored.maxSinkTemperatureC);
			if (!coolingAvailable) continue;
			auto const kwhPerUnit = energyUnitsToKWh(runtime.stores[generator.fuelStoreIndex].authored.unit);
			auto const drawableKWh = std::max(0.0,
				ports.input(generator.fuelStoreIndex, qualified[generator.fuelStoreIndex] - runtime.stores[generator.fuelStoreIndex].authored.protectedReserve))
				* kwhPerUnit;
			if (drawableKWh <= 0.0) continue;
			// The fuel cap is the output the drawable fuel can pay for at
			// this tick's rate: a machine with nothing drawable above the
			// protected reserve cannot run and is passed over, so the
			// target never reserves capacity that does not exist.
			auto const fuelCapKW = drawableKWh * generator.authored.efficiency / hours;
			auto const capKW = std::min(effectiveRatedKW, fuelCapKW);
			auto const startupRequiredKJ = generator.authored.startupPowerKW
				* generator.authored.startupSeconds;
			// A machine already cranking owes the pool its startup draw
			// whether or not it is still commanded - the crank runs on at
			// zero output, and the load must not be left blind to it.
			if (generator.phase == StationGeneratorPhase::Starting)
				dispatchStartupSupportKW += connections.inputDemand(powerPort("Generator", generator.authored.id), startupDrawForKW(generator, startupRequiredKJ));
			auto const generationPort = connections.port("Generator", generator.authored.id, "Generation");
			if (connections.efficiency(generationPort) == 0.0) continue;
			auto const commandKW = std::min(capKW, connections.required(generationPort, dispatchRemainingKW));
			if (commandKW <= 0.0) continue;
			generator.dispatchCommandKW = commandKW;
			if (generator.phase == StationGeneratorPhase::Running
				|| (generator.phase == StationGeneratorPhase::Stopped && startupRequiredKJ <= 0))
			{
				// Book the same integrated mean that production can deliver,
				// not its larger ramp endpoint. Backups cover the real gap.
				auto const startKW = generator.phase == StationGeneratorPhase::Running
					? std::min(generator.outputKW, effectiveRatedKW) : 0.0;
				auto const ramp = integrateGeneratorRamp(startKW, commandKW,
					generator.authored.maxRampKWPerSecond, dispatchDtSeconds);
				auto const deliverableKW = std::min(fuelCapKW, ramp.meanKW);
				dispatchRemainingKW -= connections.available(generationPort, std::max(0.0, deliverableKW));
			}
			else if (generator.phase == StationGeneratorPhase::Stopped)
			{
				// The command starts the machine: it will crank at zero
				// output this tick, so its startup draw rides the target
				// for the backup to fund beside the load it cannot yet
				// serve.
				dispatchStartupSupportKW += connections.inputDemand(powerPort("Generator", generator.authored.id), startupDrawForKW(generator, startupRequiredKJ));
			}
			// A machine in shutdown decay is commanded but serves
			// nothing this tick; its residual flows on untouched.
		}
		// The cranking demand rides the target beside the load: the
		// backup is commanded for the load the primary fleet cannot
		// serve this tick plus the startup energy a legitimate start
		// still needs, and never for generation that is merely
		// commanded but cannot deliver.
		dispatchRemainingKW += dispatchStartupSupportKW;
		backupEngaged = bool{false};
		for (auto& cell : runtime.fuelCells)
		{
			cell.dispatchCommandKW = 0.0;
			auto ports = materialPorts("Fuel cell", cell.authored.id);
			if (!dispatchAutomatic) continue;
			if (cell.authored.ratedOutputKW <= 0.0) continue;
			if (!sinkQualified(cell.sinkIndex, cell.authored.maxSinkTemperatureC)) continue;
			auto const o2PerH2 = StationFuelCellOxygenMassPerHydrogenKg;
			auto const elecKJPerKgH2 = cell.authored.efficiency
				* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
			auto const& hydrogen = runtime.stores[cell.hydrogenStoreIndex].authored;
			auto const& oxygen = runtime.stores[cell.oxygenStoreIndex].authored;
			auto const& water = runtime.stores[cell.waterStoreIndex].authored;
			auto const h2DrawableKg = std::max(0.0, massToKg(hydrogen.unit,
				ports.input(cell.hydrogenStoreIndex, qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve)));
			auto const o2DrawableKg = std::max(0.0, massToKg(oxygen.unit,
				ports.input(cell.oxygenStoreIndex, qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve)));
			auto const waterHeadroomKg = std::max(0.0, massToKg(water.unit,
				ports.output(cell.waterStoreIndex, water.capacity - (qualified[cell.waterStoreIndex]
					+ unqualified[cell.waterStoreIndex]))));
			// Every finite axis caps the backup the same way: the largest
			// output the reactants and the product-water headroom can
			// sustain this tick. A cell with nothing drawable above its
			// protected reserves is passed over - protected breathing
			// oxygen never becomes dispatchable capacity.
			auto capKW = cell.authored.ratedOutputKW;
			capKW = std::min(capKW, h2DrawableKg * elecKJPerKgH2 / static_cast<double>(seconds));
			capKW = std::min(capKW, (o2DrawableKg / o2PerH2) * elecKJPerKgH2
				/ static_cast<double>(seconds));
			capKW = std::min(capKW, (waterHeadroomKg / (1.0 + o2PerH2)) * elecKJPerKgH2
				/ static_cast<double>(seconds));
			if (capKW <= 0.0) continue;
			auto const generationPort = connections.port("Fuel cell", cell.authored.id, "Backup generation");
			if (connections.efficiency(generationPort) == 0.0) continue;
			auto const commandKW = std::min(capKW, connections.required(generationPort, dispatchRemainingKW));
			if (commandKW <= 0.0) continue;
			cell.dispatchCommandKW = commandKW;
			backupEngaged = true;
			dispatchRemainingKW -= connections.available(generationPort, commandKW);
		}
		runtime.lastDispatchTargetKW = dispatchLoadTargetKW + dispatchStartupSupportKW;
		runtime.lastDispatchCommittedKW = runtime.lastDispatchTargetKW - dispatchRemainingKW;
		runtime.lastDispatchShortfallKW = dispatchRemainingKW;
		runtime.lastBackupEngaged = backupEngaged;
		// Fuel-cell backup (#406) runs on the supply side alongside the
		// generators - and, since #578, ahead of the generator phase
		// machine: the backup's output joins the shared pool before the
		// generators run, so an available backup can fund a legitimate
		// primary start's cranking draw instead of the stopped machine
		// finding an empty pool. The start is still never self-funded -
		// only the backup's delivered output is in the pool, never the
		// stopped machine's own. Its electrical output joins the shared
		// generation pool the loads allocate against, its waste heat
		// lands in the class-matched sink booked exactly once, and its
		// reactants are drawn only from its own dedicated stores above
		// their protected reserves - the cell can never reach protected
		// breathing oxygen because it can only ever see its own oxygen
		// store, which is refilled only through controlled surplus
		// transfers that themselves preserve the supplying reserve.
		// Output is bounded by the installed rating, the dispatch
		// setpoint, the hydrogen and oxygen actually drawable, and the
		// product water store's remaining headroom: a backed-up water
		// destination caps production rather than vanishing product
		// mass. Cooling is required: a sink at or above the maximum sink
		// temperature refuses the cell whole - heat with nowhere to go
		// is heat that cannot be made. Cells run in declaration order.
		fuelCellHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		totalFuelCellOutputKW = double{0.0};
		totalFuelCellWasteKW = double{0.0};
		for (auto& cell : runtime.fuelCells)
		{
			auto ports = materialPorts("Fuel cell", cell.authored.id);
			auto const disabled = cell.authored.ratedOutputKW <= 0.0;
			// #415: under automatic dispatch the dispatcher's command for
			// this tick replaces the authored setpoint; the cell still
			// enforces its reactant, cooling and product-headroom limits.
			auto const commanded = disabled
				? 0.0 : (dispatchAutomatic
					? cell.dispatchCommandKW
					: cell.authored.commandedOutputKW);
			cell.lastCommandedKW = commanded;
			auto const dtSeconds = static_cast<double>(seconds);
			auto const o2PerH2 = StationFuelCellOxygenMassPerHydrogenKg;
			auto const elecKJPerKgH2 = cell.authored.efficiency
				* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
			auto const& hydrogen = runtime.stores[cell.hydrogenStoreIndex].authored;
			auto const& oxygen = runtime.stores[cell.oxygenStoreIndex].authored;
			auto const& water = runtime.stores[cell.waterStoreIndex].authored;
			cell.lastOutputKW = 0.0;
			cell.lastHydrogenKg = 0.0;
			cell.lastOxygenKg = 0.0;
			cell.lastWaterKg = 0.0;
			cell.lastWasteHeatKW = 0.0;
			cell.lastUnmetOutputKW = 0.0;
			cell.lastLimiter.clear();
			cell.lastBlockReason.clear();
			// The backup clock: usable reactants are the qualified
			// contents drawable above each store's own protected
			// reserve, balanced stoichiometrically, converted to the
			// electrical energy this cell's efficiency can extract and
			// divided by the reference output - the cell's actual
			// output this tick, or its installed rating when the cell
			// is not running. Protected oxygen never counts toward it.
			auto const publishEndurance = [&]()
			{
				ports.finish();
				auto const remainingH2Kg = std::max(0.0, massToKg(hydrogen.unit,
					qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve));
				auto const remainingO2Kg = std::max(0.0, massToKg(oxygen.unit,
					qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve));
				auto const usableH2Kg = std::min(remainingH2Kg * connections.efficiency(connections.port("Fuel cell", cell.authored.id, "Hydrogen")),
					remainingO2Kg * connections.efficiency(connections.port("Fuel cell", cell.authored.id, "Oxygen")) / o2PerH2);
				auto const referenceKW = cell.lastOutputKW > 0.0
					? cell.lastOutputKW : cell.authored.ratedOutputKW;
				cell.enduranceSeconds = referenceKW > 0.0
					? usableH2Kg * elecKJPerKgH2 / referenceKW : 0.0;
			};
			cell.lastCoolingAvailable = !disabled
				&& sinkQualified(cell.sinkIndex, cell.authored.maxSinkTemperatureC);
			if (disabled)
			{
				publishEndurance();
				cell.status = StationTransferStatus::Idle;
				continue;
			}
			if (!cell.lastCoolingAvailable)
			{
				cell.lastBlockReason = temperatureRefusal("cooling is not qualified: sink at or above the maximum sink temperature");
				cell.lastUnmetOutputKW = commanded;
				publishEndurance();
				cell.status = StationTransferStatus::Rejected;
				continue;
			}
			if (commanded <= 0.0)
			{
				publishEndurance();
				cell.status = StationTransferStatus::Idle;
				continue;
			}
			auto const hydrogenNeed = kgToMass(hydrogen.unit, commanded * dtSeconds / elecKJPerKgH2);
			auto const oxygenNeed = kgToMass(oxygen.unit, commanded * dtSeconds * o2PerH2 / elecKJPerKgH2);
			ports.limitDemand(ports.request(cell.hydrogenStoreIndex,
				ports.input(cell.hydrogenStoreIndex, qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve), hydrogenNeed), hydrogenNeed);
			ports.limitDemand(ports.request(cell.oxygenStoreIndex,
				ports.input(cell.oxygenStoreIndex, qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve), oxygenNeed), oxygenNeed);
			// Every finite axis caps the output the same way: the
			// largest output the axis can sustain this tick. The
			// binding one names itself; nothing is silently clamped.
			auto const h2DrawableKg = std::max(0.0, massToKg(hydrogen.unit,
				ports.input(cell.hydrogenStoreIndex, qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve)));
			auto const o2DrawableKg = std::max(0.0, massToKg(oxygen.unit,
				ports.input(cell.oxygenStoreIndex, qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve)));
			auto const waterHeadroomKg = std::max(0.0, massToKg(water.unit,
				ports.output(cell.waterStoreIndex, water.capacity - (qualified[cell.waterStoreIndex]
					+ unqualified[cell.waterStoreIndex]))));
			auto const byH2KW = h2DrawableKg * elecKJPerKgH2 / dtSeconds;
			auto const byO2KW = (o2DrawableKg / o2PerH2) * elecKJPerKgH2 / dtSeconds;
			auto const byWaterKW = (waterHeadroomKg / (1.0 + o2PerH2))
				* elecKJPerKgH2 / dtSeconds;
			ports.limitDemand(byWaterKW, commanded);
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](double capKW, std::string const& reason)
			{
				if (capKW >= scale * commanded) return;
				scale = capKW / commanded;
				limiter = reason;
			};
			constrain(connections.remaining(connections.port("Fuel cell", cell.authored.id, "Backup generation")),
				"electrical generation connection throughput");
			constrain(byH2KW, "hydrogen store '" + hydrogen.id
				+ "' has nothing drawable above its protected reserve");
			constrain(byO2KW, "oxygen store '" + oxygen.id
				+ "' has nothing drawable above its protected reserve");
			constrain(byWaterKW, "product water store '" + water.id
				+ "' is full: the water has nowhere to go");
			scale = std::max(0.0, std::min(1.0, scale));
			cell.lastLimiter = limiter;
			auto outputKW = commanded * scale;
			if (outputKW <= 1e-15)
			{
				// Nothing can burn this tick: the cell is refused whole,
				// consumes nothing and delivers nothing, and the whole
				// commanded output stays visible as unmet service.
				cell.lastUnmetOutputKW = commanded;
				cell.lastBlockReason = limiter;
				publishEndurance();
				cell.status = StationTransferStatus::Rejected;
				continue;
			}
			// Derive the reaction from the capped output: hydrogen is
			// the energy-carrying reactant, oxygen rides the declared
			// stoichiometric ratio, and product water is the sum of
			// the reactants actually consumed, so the mass ledger
			// closes by construction. Each flow is clamped to its own
			// drawable or headroom so a one-ulp roundoff can never
			// overshoot (#556), and the delivered output is recomputed
			// from the hydrogen that actually moved - the pool never
			// receives more than the reactants paid for.
			auto h2Kg = std::min(outputKW * dtSeconds / elecKJPerKgH2, h2DrawableKg);
			auto o2Kg = std::min(h2Kg * o2PerH2, o2DrawableKg);
			h2Kg = std::min(h2Kg, o2Kg / o2PerH2);
			auto const waterKg = h2Kg + o2Kg;
			auto const chemicalKJ = h2Kg
				* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
			outputKW = cell.authored.efficiency * chemicalKJ / dtSeconds;
			auto const electricalKJ = outputKW * dtSeconds;
			auto const wasteKJ = chemicalKJ - electricalKJ;
			auto const h2Units = std::min(kgToMass(hydrogen.unit, h2Kg),
				std::max(0.0, ports.input(cell.hydrogenStoreIndex, qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve)));
			auto const h2ActualKg = massToKg(hydrogen.unit, h2Units);
			auto const o2Units = std::min(kgToMass(oxygen.unit, o2Kg),
				std::max(0.0, ports.input(cell.oxygenStoreIndex, qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve)));
			auto const o2ActualKg = massToKg(oxygen.unit, o2Units);
			auto const waterUnits = std::min(kgToMass(water.unit, waterKg),
				std::max(0.0, ports.output(cell.waterStoreIndex, water.capacity
					- (qualified[cell.waterStoreIndex] + unqualified[cell.waterStoreIndex]))));
			auto const waterActualKg = massToKg(water.unit, waterUnits);
			qualified[cell.hydrogenStoreIndex] -= h2Units;
			flowOut[cell.hydrogenStoreIndex] += h2Units;
			stagedUnitLoss[hydrogen.unit] += h2Units;
			qualified[cell.oxygenStoreIndex] -= o2Units;
			flowOut[cell.oxygenStoreIndex] += o2Units;
			stagedUnitLoss[oxygen.unit] += o2Units;
			qualified[cell.waterStoreIndex] += waterUnits;
			flowIn[cell.waterStoreIndex] += waterUnits;
			stagedUnitLoss[water.unit] -= waterUnits;
			// Energy: the reactants' chemical energy left as
			// electricity into the shared pool or as waste heat into
			// the sink - booked once, never a third destination.
			fuelCellHeatByReservoir[cell.sinkIndex] += connections.routeHeat(connections.port("Fuel cell", cell.authored.id, "Heat"), wasteKJ / dtSeconds);
			auto const toPoolKW = connections.send(connections.port("Fuel cell", cell.authored.id, "Backup generation"), outputKW).delivered;
			availableGeneration += toPoolKW; poolGenerationKW += toPoolKW;
			cell.lastOutputKW = outputKW;
			cell.lastHydrogenKg = h2ActualKg;
			cell.lastOxygenKg = o2ActualKg;
			cell.lastWaterKg = waterActualKg;
			cell.lastWasteHeatKW = wasteKJ / dtSeconds;
			cell.lastUnmetOutputKW = std::max(0.0, commanded - outputKW);
			cell.hydrogenConsumedKg += h2ActualKg;
			cell.oxygenConsumedKg += o2ActualKg;
			cell.waterProducedKg += waterActualKg;
			cell.chemicalEnergyKJ += chemicalKJ;
			cell.electricalKJ += electricalKJ;
			cell.wasteHeatKJ += wasteKJ;
			totalFuelCellOutputKW += outputKW;
			totalFuelCellWasteKW += wasteKJ / dtSeconds;
			publishEndurance();
			if (outputKW + 1e-12 >= commanded)
				cell.status = StationTransferStatus::Full;
			else cell.status = StationTransferStatus::Partial;
		}
		for (auto& generator : runtime.generators)
		{
			auto ports = materialPorts("Generator", generator.authored.id);
			auto const disabled = generator.authored.ratedOutputKW <= 0.0;
			// The availability factor from the outage contract (#413) is
			// a hard ceiling on the deliverable rated capacity this tick:
			// an outage that takes the whole rating away disables the
			// machine outright (it cannot start, and a running machine
			// trips to shutdown), while a partial reduction caps the
			// output the machine can deliver. The ceiling bites the moment
			// the window opens rather than ramping out of it.
			auto const effectiveRatedKW = generator.authored.ratedOutputKW
				* generator.availabilityFactor;
			auto const capacityDisabled = effectiveRatedKW <= 0.0;
			// #415: under automatic dispatch the dispatcher's command for
			// this tick replaces the authored setpoint; the machine itself
			// still enforces its startup, ramp, fuel and cooling physics,
			// so the command can never buy supply the machine cannot make.
			auto const commanded = disabled || capacityDisabled
				? 0.0 : (dispatchAutomatic
					? generator.dispatchCommandKW
					: generator.authored.commandedOutputKW);
			generator.lastCommandedKW = commanded;
			auto const dtSeconds = static_cast<double>(seconds);
			// Cooling is qualified while the sink is below the maximum
			// sink temperature: at or above it the interface cannot
			// accept the generator's heat, so the generator cannot run.
			auto const coolingAvailable = !disabled
				&& sinkQualified(generator.sinkIndex, generator.authored.maxSinkTemperatureC);
			auto const kwhPerUnit = energyUnitsToKWh(runtime.stores[generator.fuelStoreIndex].authored.unit);
			if (coolingAvailable && commanded > 0.0)
			{
				auto const need = std::min(commanded, effectiveRatedKW) * hours / generator.authored.efficiency / kwhPerUnit;
				ports.limitDemand(ports.request(generator.fuelStoreIndex,
					ports.input(generator.fuelStoreIndex, qualified[generator.fuelStoreIndex]
						- runtime.stores[generator.fuelStoreIndex].authored.protectedReserve), need), need);
				if (generator.phase == StationGeneratorPhase::Shutdown
					|| (generator.phase != StationGeneratorPhase::Running && generator.authored.startupSeconds > 0.0
						&& powerAvailable("Generator", generator.authored.id) + 1e-9 < generator.authored.startupPowerKW))
					ports.limitDemand(0.0, 1.0);
			}
			auto const drawableUnits = std::max(0.0,
				ports.input(generator.fuelStoreIndex, qualified[generator.fuelStoreIndex] - runtime.stores[generator.fuelStoreIndex].authored.protectedReserve));
			auto const drawableKWh = drawableUnits * kwhPerUnit;
			auto const startupRequiredKJ = generator.authored.startupPowerKW * generator.authored.startupSeconds;
			generator.lastAverageOutputKW = 0.0;
			generator.lastFuelDrawKWh = 0.0;
			generator.lastWasteHeatKW = 0.0;
			generator.lastStartupDrawKW = 0.0;
			generator.lastStartupUnmetKW = 0.0;
			generator.lastDecayHeatKW = 0.0;
			generator.lastCoolingAvailable = coolingAvailable;
			generator.lastBlockReason.clear();
			// Resolve zero-duration phases without spending a positive interval.
			// Preserve every phase event even when several occur in one step.
			auto const generatorIndex = static_cast<size_t>(&generator - runtime.generators.data());
			bool exhaustedAtEnd = false;
			if (generator.phase == StationGeneratorPhase::Shutdown
				&& generator.shutdownElapsedSeconds >= generator.authored.shutdownSeconds)
			{
				appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
					+ "' shutdown complete: decay heat dissipated after "
					+ std::to_string(generator.authored.shutdownSeconds) + " s");
				generator.phase = StationGeneratorPhase::Stopped;
				previousGeneratorPhase[generatorIndex] = generator.phase;
			}
			// A zero-duration phase transition must not consume a whole
			// integration interval before the crank can receive energy.
			if (generator.phase == StationGeneratorPhase::Stopped)
			{
				generator.outputKW = 0.0;
				if (commanded > 0.0)
				{
					// A commanded start proceeds only when every
					// precondition holds this tick: qualified cooling,
					// drawable fuel above the protected reserve, and a
					// pool that can actually supply the cranking draw.
					// A start is never self-funded: a stopped
					// generator produces nothing, so its own output
					// can never appear in the pool that starts it.
					if (!coolingAvailable)
						generator.lastBlockReason = temperatureRefusal("cooling is not qualified: sink at or above the maximum sink temperature");
					else if (drawableKWh <= 0.0)
						generator.lastBlockReason = "no fuel is drawable above the protected reserve";
					else if (startupRequiredKJ > 0.0
						&& powerAvailable("Generator", generator.authored.id) + 1e-9
							< generator.authored.startupPowerKW)
						generator.lastBlockReason = "the pool cannot supply the startup energy draw";
					else
					{
						generator.startupReceivedKJ = 0.0;
						generator.phase = startupRequiredKJ > 0.0
							? StationGeneratorPhase::Starting : StationGeneratorPhase::Running;
						if (generator.phase == StationGeneratorPhase::Starting)
						{
							appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
								+ "' starting: cranking at " + std::to_string(generator.authored.startupPowerKW)
								+ " kW for " + std::to_string(generator.authored.startupSeconds) + " s");
							previousGeneratorPhase[generatorIndex] = generator.phase;
						}
					}
				}
			}
			switch (generator.phase)
			{
			case StationGeneratorPhase::Stopped:
				break;
			case StationGeneratorPhase::Starting:
				generator.outputKW = 0.0;
				if (!coolingAvailable)
				{
					// Cooling lost mid-crank: the start fails. The
					// cranking energy already spent is accounted as
					// heat in the sink; the attempt restarts from zero.
					generator.phase = StationGeneratorPhase::Stopped;
					generator.startupReceivedKJ = 0.0;
					generator.lastBlockReason = "cooling lost during startup";
				}
				else if (drawableKWh <= 0.0)
				{
					generator.phase = StationGeneratorPhase::Stopped;
					generator.startupReceivedKJ = 0.0;
					generator.lastBlockReason = "fuel depleted during startup";
				}
				else
				{
					// #562: the crank buys only the energy still owed.
					// A startup duration that is not a whole number of
					// fixed ticks must not round the final purchase up to
					// a full tick at full startup power: the last interval
					// is prorated to the remaining requirement, so the
					// machine receives and dissipates exactly
					// startupPowerKW x startupSeconds, never more. The
					// prorated average bus draw stays within the authored
					// startup power, and the pool only has to fund what is
					// actually owed this tick.
					auto const remainingStartupKJ = std::max(0.0,
						startupRequiredKJ - generator.startupReceivedKJ);
					auto const requestKW = dtSeconds > 0.0
						? std::min(generator.authored.startupPowerKW,
							remainingStartupKJ / dtSeconds)
						: generator.authored.startupPowerKW;
					auto const poolKW = powerAvailable("Generator", generator.authored.id);
					if (poolKW + 1e-9 < requestKW)
					{
						// The startup energy support that qualified the
						// start has gone away mid-crank: the start
						// fails whole rather than cranking on a trickle.
						generator.phase = StationGeneratorPhase::Stopped;
						generator.startupReceivedKJ = 0.0;
						generator.lastBlockReason = "startup energy support lost mid-crank";
						break;
					}
					auto const suppliedKW = drawPower("Generator", generator.authored.id, requestKW);
					generator.lastStartupDrawKW = suppliedKW;
					// Cranking energy dissipates into the same sink
					// the running machine will use: booked once here.
					generatorHeatBySink[generator.sinkIndex] += connections.routeHeat(connections.port("Generator", generator.authored.id, "Heat"), suppliedKW);
					generator.startupHeatKJ += suppliedKW * dtSeconds;
					generator.startupReceivedKJ += suppliedKW * dtSeconds;
					if (generator.startupReceivedKJ + 1e-9 >= startupRequiredKJ)
						generator.phase = StationGeneratorPhase::Running;
				}
				break;
			case StationGeneratorPhase::Running:
				if (!coolingAvailable)
				{
					// Cooling lost while running: the generator trips to
					// shutdown. Electrical output and fuel burn stop
					// immediately; the configured decay heat carries
					// on below.
					generator.phase = StationGeneratorPhase::Shutdown;
					generator.shutdownElapsedSeconds = 0.0;
					generator.outputKW = 0.0;
					generator.lastBlockReason = "cooling lost: generator tripped to shutdown";
				}
				else if (commanded <= 0.0)
				{
					generator.phase = StationGeneratorPhase::Shutdown;
					generator.shutdownElapsedSeconds = 0.0;
					generator.outputKW = 0.0;
				}
				else
				{
					// Ramp toward the commanded output: the output may
					// move at most ramp x dt this tick, up or down, so
					// no tick commands an instantaneous step the
					// machine cannot physically make.
					auto cableCapKW = connections.remaining(connections.port("Generator", generator.authored.id, "Generation"));
					for (auto const& binding : connections.storePorts("Generator", generator.authored.id, runtime.settings))
						if (binding.input && binding.store == generator.fuelStoreIndex)
							cableCapKW = std::min(cableCapKW, connections.remaining(binding.port)
								* connections.efficiency(binding.port) * kwhPerUnit * 3600 * generator.authored.efficiency);
					auto const startKW = std::min({generator.outputKW, effectiveRatedKW, cableCapKW});
					auto const targetKW = std::min({commanded, effectiveRatedKW, cableCapKW});
					auto const ramp = integrateGeneratorRamp(startKW, targetKW,
						generator.authored.maxRampKWPerSecond, dtSeconds);
					generator.outputKW = ramp.endpointKW;
					generator.lastAverageOutputKW = ramp.meanKW;
					// The outage ceiling is hard: a window that opens while
					// the machine is above the reduced capacity clamps the
					// output down at once, so no output continues through
					// capacity the outage has taken away even for the ticks
					// a ramp-down would otherwise take (#413).
					if (generator.outputKW > effectiveRatedKW)
						generator.outputKW = effectiveRatedKW;
					// Fuel feasibility: the fuel drawable above the
					// protected reserve caps the output this tick. A
					// generator with no fuel left cannot run at all.
					auto const neededFuelKWh = generator.lastAverageOutputKW * hours / generator.authored.efficiency;
					if (neededFuelKWh > drawableKWh)
					{
						if (drawableKWh <= 0.0)
						{
							// Fuel depletion stops electrical output
							// immediately; the decay period follows.
							generator.phase = StationGeneratorPhase::Shutdown;
							generator.shutdownElapsedSeconds = 0.0;
							generator.outputKW = 0.0;
							generator.lastBlockReason = "fuel depleted: generator shut down";
							generator.lastAverageOutputKW = 0.0;
							break;
						}
						generator.lastAverageOutputKW = drawableKWh * generator.authored.efficiency / hours;
						generator.outputKW = std::min(generator.outputKW, generator.lastAverageOutputKW);
					}
					// A saturated inlet is not an empty inventory. Only the
					// latter starts shutdown; both still book their exact dose.
					auto inventoryUnits = std::max(0.0, qualified[generator.fuelStoreIndex]
						- runtime.stores[generator.fuelStoreIndex].authored.protectedReserve);
					for (auto const& binding : connections.storePorts("Generator", generator.authored.id, runtime.settings))
						if (binding.input && binding.store == generator.fuelStoreIndex)
							inventoryUnits *= connections.efficiency(binding.port);
					exhaustedAtEnd = neededFuelKWh >= inventoryUnits * kwhPerUnit;
					auto const drawnUnits = neededFuelKWh >= drawableKWh ? drawableUnits
						: std::min(drawableUnits, generator.lastAverageOutputKW * hours / generator.authored.efficiency / kwhPerUnit);
					auto const drawnKWh = drawnUnits * kwhPerUnit;
					generator.lastAverageOutputKW = drawnKWh * 3600.0 * generator.authored.efficiency / dtSeconds;
					qualified[generator.fuelStoreIndex] -= drawnUnits;
					flowOut[generator.fuelStoreIndex] += drawnUnits;
					stagedUnitLoss[runtime.stores[generator.fuelStoreIndex].authored.unit] += drawnUnits;
					generator.lastFuelDrawKWh = drawnKWh;
					auto const fuelKJ = drawnKWh * 3600.0;
					auto const electricalKJ = generator.lastAverageOutputKW * dtSeconds;
					// Waste heat is the fuel energy the conversion did
					// not deliver as electricity: booked once into the
					// sink, never also exported elsewhere.
					auto const wasteKJ = fuelKJ - electricalKJ;
					generator.fuelEnergyKJ += fuelKJ;
					generator.electricalKJ += electricalKJ;
					generator.wasteHeatKJ += wasteKJ;
					auto const wasteKW = wasteKJ / dtSeconds;
					generator.lastWasteHeatKW = wasteKW;
					generatorHeatBySink[generator.sinkIndex] += connections.routeHeat(connections.port("Generator", generator.authored.id, "Heat"), wasteKW);
					// The generator's output joins the shared pool the
					// loads allocate against this tick - generation
					// earned with fuel, not sunshine.
					auto const toPoolKW = connections.send(connections.port("Generator", generator.authored.id, "Generation"), generator.lastAverageOutputKW).delivered;
					availableGeneration += toPoolKW; poolGenerationKW += toPoolKW;
					totalGeneratorOutputKW += generator.lastAverageOutputKW;
					totalGeneratorWasteKW += wasteKW;
					if (exhaustedAtEnd)
					{
						generator.phase = StationGeneratorPhase::Shutdown;
						generator.shutdownElapsedSeconds = 0.0;
						generator.outputKW = 0.0;
						generator.lastBlockReason = "fuel depleted: generator shut down";
					}
				}
				break;
			case StationGeneratorPhase::Shutdown:
				generator.outputKW = 0.0;
				break;
			}
			// #563: decay heat books from the very tick the electrical
			// output stopped - no transition-tick gap - and the final
			// decay interval is prorated to the remaining configured
			// time, so the sink receives exactly
			// shutdownHeatKW * shutdownSeconds, never a rounded-up
			// tick. The decay heat cannot be refused - it happens
			// whether or not cooling is available - so it books
			// regardless, and the sink warms under it. A zero decay
			// period books nothing. Completion is a physical boundary,
			// not an extra whole-step wait. Fuel exhausted at this interval's
			// endpoint starts decay in the following positive interval.
			bool decayedThisTick = false;
			if (generator.phase == StationGeneratorPhase::Shutdown
				&& previousGeneratorPhase[generatorIndex] != StationGeneratorPhase::Shutdown)
			{
				appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
					+ "' shutting down: " + (generator.lastBlockReason.empty() ? std::string("commanded off") : generator.lastBlockReason)
					+ "; shutdown heat " + std::to_string(generator.authored.shutdownHeatKW)
					+ " kW for " + std::to_string(generator.authored.shutdownSeconds) + " s");
				previousGeneratorPhase[generatorIndex] = generator.phase;
			}
			if (generator.phase == StationGeneratorPhase::Shutdown && !exhaustedAtEnd)
			{
				auto const remainingSeconds = generator.authored.shutdownSeconds
					- generator.shutdownElapsedSeconds;
				if (remainingSeconds <= 0.0)
				{
					// The shutdown event was already staged above; an empty
					// decay phase completes without a positive-time wait.
					generator.phase = StationGeneratorPhase::Stopped;
				}
				else
				{
					auto const intervalSeconds = std::min(dtSeconds, remainingSeconds);
					auto const bookedKJ = generator.authored.shutdownHeatKW * intervalSeconds;
					// The published rate is the booked energy spread over
					// the fixed tick, so rate x dt equals the energy the
					// sink actually received this tick even on the
					// prorated final interval.
					generator.lastDecayHeatKW = bookedKJ / dtSeconds;
					generatorHeatBySink[generator.sinkIndex] += connections.routeHeat(connections.port("Generator", generator.authored.id, "Heat"), generator.lastDecayHeatKW);
					generator.decayHeatKJ += bookedKJ;
					totalGeneratorDecayKW += generator.lastDecayHeatKW;
					generator.shutdownElapsedSeconds += intervalSeconds;
					if (intervalSeconds >= remainingSeconds)
					{
						generator.shutdownElapsedSeconds = generator.authored.shutdownSeconds;
						generator.phase = StationGeneratorPhase::Stopped;
					}
					decayedThisTick = bookedKJ > 0.0;
				}
			}
			// The cooling obligation is exposed on every interval that
			// actually produces decay heat - including the tick on
			// which the last of it is booked, whatever phase that tick
			// ends in - and while the machine is running (#563).
			auto const heatPort = connections.port("Generator", generator.authored.id, "Heat");
			generatorHeatBySink[generator.sinkIndex] += connections.routeHeat(heatPort, 0.0);
			auto const* heat = connections.snapshot(heatPort);
			generator.lastCoolingRequired = generator.phase == StationGeneratorPhase::Running
				|| generator.lastAverageOutputKW > 0.0 || decayedThisTick || (heat && (heat->retainedHeatKJ > 0.0 || heat->lastInputRate > 0.0));
			totalGeneratorStartupKW += generator.lastStartupDrawKW;
			// Publish the per-generator service status against the
			// commanded output: full at command, partial under ramp
			// or fuel limits, rejected when commanded with nothing
			// produced, idle when not commanded.
			if (disabled || commanded <= 0.0)
				generator.status = StationTransferStatus::Idle;
			else if (generator.outputKW + 1e-12 >= commanded)
				generator.status = StationTransferStatus::Full;
			else if (generator.outputKW > 1e-12)
				generator.status = StationTransferStatus::Partial;
			else generator.status = StationTransferStatus::Rejected;
		}

		return true;
	}
}
