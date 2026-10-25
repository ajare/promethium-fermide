#include "StationSystemsImpl.h"
#include "core/Serializer.h"
#include "core/StationReference.h"

#include <algorithm>
#include <cmath>

namespace core
{
	namespace
	{
		// DegradationCrossing and ThermalGate name simulation-owned onsets;
		// neither may be forged by a client.
		bool isInjectableCause(FaultCause::Kind kind)
		{
			return kind == FaultCause::Kind::AgentAction || kind == FaultCause::Kind::ScenarioEvent
				|| kind == FaultCause::Kind::Editor;
		}
	}

	bool StationSystemsRuntime::injectFault(std::string_view id, std::string_view mode,
		FaultCause const& cause, std::string* diagnostic)
	{
		auto refuse = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!mImpl->settings.enabled || !mImpl->snapshot.healthy) return refuse("Station runtime is not running or healthy");
		auto const identities = stationInstallationIdentities(mImpl->settings);
		if (std::none_of(identities.begin(), identities.end(), [&](auto const& i) { return i.id == id; })
			&& std::none_of(mImpl->sabatierReactors.begin(), mImpl->sabatierReactors.end(), [&](auto const& s) { return s.authored.id == id; })
			&& std::none_of(mImpl->methaneProcessors.begin(), mImpl->methaneProcessors.end(), [&](auto const& s) { return s.authored.id == id; })
			// #844: radiators carry a puncture Fault even though they are
			// not outage/maintenance-registered installations; the fault
			// namespace reaches them directly.
			&& std::none_of(mImpl->radiators.begin(), mImpl->radiators.end(), [&](auto const& r) { return r.authored.id == id; })
			// #845: the power-source catalogue reaches solar arrays and
			// battery banks directly, as the thermal-plant slice reached
			// radiators: neither is an outage-registered installation, but
			// both carry fault modes in the same injection namespace.
			&& std::none_of(mImpl->solarArrays.begin(), mImpl->solarArrays.end(), [&](auto const& a) { return a.authored.id == id; })
			&& std::none_of(mImpl->batteries.begin(), mImpl->batteries.end(), [&](auto const& b) { return b.authored.id == id; }))
			return refuse("Unknown installation '" + std::string(id) + "'");
		if (!isInjectableCause(cause.kind))
			return refuse("Fault cause kind is not injectable: it names a Fault the simulation raises itself");
		if (cause.detail.size() > 1024) return refuse("Fault cause detail exceeds 1024 bytes");
		if (mode == "overtemp-runaway")
		{
			auto inject = [&](auto& states) {
				for (auto& state : states)
				{
					if (state.authored.id != id) continue;
					if (!state.authored.overtempRunaway) return refuse("overtemp-runaway is not enabled");
					if (state.trip) return refuse("Trip is already active or pending");
					Impl::FaultCommand command;
					command.afterStep = mImpl->clock.steps(); command.installationId = id;
					command.modeId = mode; command.cause = cause;
					command.heatSpikeKJ = state.authored.overtempRunaway->heatSpikeKJ;
					mImpl->faultCommands.push_back(command);
					state.trip = StationFaultSnapshot{std::string(id), std::string(mode),
						mImpl->clock.steps() + 1, cause.kind, cause.detail, 1.0};
					state.pendingTripHeat = command.heatSpikeKJ;
					if (diagnostic) diagnostic->clear();
					return true;
				}
				return refuse("Unknown overtemp-runaway installation");
			};
			if (std::any_of(mImpl->sabatierReactors.begin(), mImpl->sabatierReactors.end(),
				[&](auto const& s) { return s.authored.id == id; })) return inject(mImpl->sabatierReactors);
			return inject(mImpl->methaneProcessors);
		}
		auto loop = std::find_if(mImpl->coolantLoops.begin(), mImpl->coolantLoops.end(),
			[&](auto const& l) { return l.authored.id == id; });
		// #844: the thermal-plant catalogue. Each mode is injectable only
		// on a machine of the kind that owns it, only when that machine
		// opted in, and only once: an active or pending instance of the
		// same mode is refused with no mutation. The injected mode names
		// the machine kind, so an id that is not that kind's machine is
		// an unknown installation for this mode.
		if (mode == "compressor-failure" || mode == "refrigerant-leak")
		{
			auto chiller = std::find_if(mImpl->chillers.begin(), mImpl->chillers.end(),
				[&](auto const& c) { return c.authored.id == id; });
			if (chiller == mImpl->chillers.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (mode == "compressor-failure")
			{
				if (!chiller->authored.compressorFailure)
					return refuse("compressor-failure is not enabled");
				if (chiller->compressorFailure)
					return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
				Impl::FaultCommand command;
				command.kind = Impl::FaultCommand::Kind::Injection;
				command.afterStep = mImpl->clock.steps(); command.installationId = id;
				command.modeId = mode; command.cause = cause;
				mImpl->faultCommands.push_back(std::move(command));
				chiller->compressorFailure = StationFaultSnapshot{std::string(id), std::string(mode),
					mImpl->clock.steps() + 1, cause.kind, cause.detail, 1.0};
				chiller->compressorFailurePending = true;
				if (diagnostic) diagnostic->clear();
				return true;
			}
			if (!chiller->authored.refrigerantLeak)
				return refuse("refrigerant-leak is not enabled");
			if (chiller->refrigerantLeak)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			auto const& parameters = *chiller->authored.refrigerantLeak;
			if (!std::isfinite(parameters.chargeKg) || parameters.chargeKg <= 0
				|| !std::isfinite(parameters.leakRateKgPerS) || parameters.leakRateKgPerS <= 0
				|| !std::isfinite(parameters.specificHeatKJPerKgK) || parameters.specificHeatKJPerKgK <= 0
				|| std::none_of(mImpl->atmospheres.begin(), mImpl->atmospheres.end(),
					[&](auto const& a) { return a.authored.id == parameters.compartmentId; }))
				return refuse("Invalid refrigerant-leak parameters");
			// All refusal paths precede mutation. The command freezes the
			// effective leak parameters; the charge store starts full and
			// the leak runs from the next audited boundary.
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause; command.refrigerantLeak = parameters;
			mImpl->faultCommands.push_back(std::move(command));
			chiller->refrigerantLeak = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail, 0};
			chiller->refrigerantLeakPending = true;
			chiller->refrigerantLeakRateKgPerS = parameters.leakRateKgPerS;
			chiller->refrigerantCpKJPerKgK = parameters.specificHeatKJPerKgK;
			chiller->refrigerantCompartmentId = parameters.compartmentId;
			if (chiller->refrigerantChargeKg <= 0.0)
			{
				chiller->refrigerantChargeKg = parameters.chargeKg;
				chiller->refrigerantKg = parameters.chargeKg;
			}
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (mode == "panel-puncture")
		{
			auto radiator = std::find_if(mImpl->radiators.begin(), mImpl->radiators.end(),
				[&](auto const& r) { return r.authored.id == id; });
			if (radiator == mImpl->radiators.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (!radiator->authored.panelPuncture)
				return refuse("panel-puncture is not enabled");
			if (radiator->puncture)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			auto const& parameters = *radiator->authored.panelPuncture;
			if (!std::isfinite(parameters.coolantKg) || parameters.coolantKg <= 0
				|| !std::isfinite(parameters.leakRateKgPerS) || parameters.leakRateKgPerS <= 0
				|| !std::isfinite(parameters.specificHeatKJPerKgK) || parameters.specificHeatKJPerKgK <= 0
				|| std::none_of(mImpl->atmospheres.begin(), mImpl->atmospheres.end(),
					[&](auto const& a) { return a.authored.id == parameters.compartmentId; }))
				return refuse("Invalid panel-puncture parameters");
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause; command.panelPuncture = parameters;
			mImpl->faultCommands.push_back(std::move(command));
			radiator->puncture = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail, 0};
			radiator->puncturePending = true;
			radiator->punctureLeakRateKgPerS = parameters.leakRateKgPerS;
			radiator->punctureCpKJPerKgK = parameters.specificHeatKJPerKgK;
			radiator->punctureCompartmentId = parameters.compartmentId;
			if (radiator->panelCoolantCapacityKg <= 0.0)
			{
				radiator->panelCoolantCapacityKg = parameters.coolantKg;
				radiator->panelCoolantKg = parameters.coolantKg;
			}
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (mode == "mechanical-seizure")
		{
			auto engine = std::find_if(mImpl->heatEngines.begin(), mImpl->heatEngines.end(),
				[&](auto const& e) { return e.authored.id == id; });
			if (engine == mImpl->heatEngines.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (!engine->authored.mechanicalSeizure)
				return refuse("mechanical-seizure is not enabled");
			if (engine->seizure)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause;
			mImpl->faultCommands.push_back(std::move(command));
			engine->seizure = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail, 1.0};
			engine->seizurePending = true;
			if (diagnostic) diagnostic->clear();
			return true;
		}
		// #845: the power-source catalogue. Same discipline as every
		// earlier batch: the mode names the machine kind that owns it, the
		// machine must have opted in, an active or pending instance of the
		// same mode is refused, and every refusal path precedes any
		// mutation. The accepted command freezes the effective
		// parameters; a later live retune cannot reach the Fault it
		// started.
		if (mode == "failed-start")
		{
			auto generator = std::find_if(mImpl->generators.begin(), mImpl->generators.end(),
				[&](auto const& g) { return g.authored.id == id; });
			if (generator == mImpl->generators.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (!generator->authored.failedStart)
				return refuse("failed-start is not enabled");
			if (generator->failedStart)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			// The failed crank is the machine's own authored crank: the
			// mode freezes no parameters because there are none to freeze.
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause;
			mImpl->faultCommands.push_back(std::move(command));
			generator->failedStart = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail, 1.0};
			generator->failedStartPending = true;
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (mode == "string-loss" || mode == "tracking-failure")
		{
			auto array = std::find_if(mImpl->solarArrays.begin(), mImpl->solarArrays.end(),
				[&](auto const& a) { return a.authored.id == id; });
			if (array == mImpl->solarArrays.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (mode == "string-loss")
			{
				if (!array->authored.stringLoss)
					return refuse("string-loss is not enabled");
				if (array->stringLoss)
					return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
				auto const& parameters = *array->authored.stringLoss;
				if (!std::isfinite(parameters.lossFraction) || parameters.lossFraction <= 0.0
					|| parameters.lossFraction > 1.0)
					return refuse("Invalid string-loss fraction");
				Impl::FaultCommand command;
				command.kind = Impl::FaultCommand::Kind::Injection;
				command.afterStep = mImpl->clock.steps(); command.installationId = id;
				command.modeId = mode; command.cause = cause; command.stringLoss = parameters;
				mImpl->faultCommands.push_back(std::move(command));
				array->stringLoss = StationFaultSnapshot{std::string(id), std::string(mode),
					mImpl->clock.steps() + 1, cause.kind, cause.detail, parameters.lossFraction};
				array->stringLossPending = true;
				array->stringLossFraction = parameters.lossFraction;
				if (diagnostic) diagnostic->clear();
				return true;
			}
			if (!array->authored.trackingFailure)
				return refuse("tracking-failure is not enabled");
			if (array->trackingFailure)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			auto const& parameters = *array->authored.trackingFailure;
			if (!std::isfinite(parameters.availability) || parameters.availability < 0.0
				|| parameters.availability >= 1.0)
				return refuse("Invalid tracking-failure availability");
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause; command.trackingFailure = parameters;
			mImpl->faultCommands.push_back(std::move(command));
			array->trackingFailure = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail,
				1.0 - parameters.availability};
			array->trackingFailurePending = true;
			array->trackingAvailability = parameters.availability;
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (mode == "thermal-runaway")
		{
			auto battery = std::find_if(mImpl->batteries.begin(), mImpl->batteries.end(),
				[&](auto const& b) { return b.authored.id == id; });
			if (battery == mImpl->batteries.end())
				return refuse("Unknown " + std::string(mode) + " installation");
			if (!battery->authored.thermalRunaway)
				return refuse("thermal-runaway is not enabled");
			if (battery->thermalRunaway)
				return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
			auto const& parameters = *battery->authored.thermalRunaway;
			if (!std::isfinite(parameters.heatKJ) || parameters.heatKJ <= 0
				|| !std::isfinite(parameters.toxicKg) || parameters.toxicKg <= 0
				|| std::none_of(mImpl->atmospheres.begin(), mImpl->atmospheres.end(),
					[&](auto const& a) { return a.authored.id == parameters.compartmentId; }))
				return refuse("Invalid thermal-runaway parameters");
			// The one-shot booking lands on the first audited boundary; the
			// effective amounts are frozen here so a live retune of the
			// authored block cannot change what this Fault books.
			Impl::FaultCommand command;
			command.kind = Impl::FaultCommand::Kind::Injection;
			command.afterStep = mImpl->clock.steps(); command.installationId = id;
			command.modeId = mode; command.cause = cause; command.thermalRunaway = parameters;
			mImpl->faultCommands.push_back(std::move(command));
			battery->thermalRunaway = StationFaultSnapshot{std::string(id), std::string(mode),
				mImpl->clock.steps() + 1, cause.kind, cause.detail, 1.0};
			battery->thermalRunawayPending = true;
			battery->runawayHeatKJ = parameters.heatKJ;
			battery->runawayToxicKg = parameters.toxicKg;
			battery->runawayCompartmentId = parameters.compartmentId;
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (loop == mImpl->coolantLoops.end() || mode != "coolant-leak")
			return refuse("Unknown fault mode '" + std::string(mode) + "' for '" + std::string(id) + "'");
		if (!loop->authored.coolantLeak) return refuse("coolant-leak is not enabled for '" + std::string(id) + "'");
		if (loop->fault) return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
		auto const& parameters = *loop->authored.coolantLeak;
		if (!std::isfinite(parameters.releasedKg) || parameters.releasedKg <= 0
			|| std::none_of(mImpl->atmospheres.begin(), mImpl->atmospheres.end(),
				[&](auto const& a) { return a.authored.id == parameters.compartmentId; }))
			return refuse("Invalid coolant-leak parameters");
		// All refusal paths precede mutation, including pending duplicate detection.
		Impl::FaultCommand command;
		command.kind = Impl::FaultCommand::Kind::Injection;
		command.afterStep = mImpl->clock.steps(); command.installationId = id;
		command.modeId = mode; command.cause = cause; command.parameters = parameters;
		mImpl->faultCommands.push_back(std::move(command));
		loop->fault = StationFaultSnapshot{std::string(id), std::string(mode), mImpl->clock.steps() + 1,
			cause.kind, cause.detail, 0};
		loop->pendingLeak = parameters;
		loop->leakCompartmentId = parameters.compartmentId;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// Funded Condition recovery (#840). Validated whole before any mutation,
	// then queued: the command names a target, and the audited drive heals
	// toward it at the authored bound, paying for every point from the
	// service-stock store above that store's protected reserve. Nothing is
	// healed for free and nothing is healed instantly - a restore that
	// cannot be funded is refused here rather than silently stalling, and
	// one that runs out of stock part-way stops with its target unmet and
	// says so in the snapshot.
	bool StationSystemsRuntime::restoreCondition(std::string_view id, double target,
		std::string* diagnostic)
	{
		auto refuse = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!mImpl->settings.enabled || !mImpl->snapshot.healthy) return refuse("Station runtime is not running or healthy");
		auto const identities = stationInstallationIdentities(mImpl->settings);
		if (std::none_of(identities.begin(), identities.end(), [&](auto const& i) { return i.id == id; }))
			return refuse("Unknown installation '" + std::string(id) + "'");
		auto loop = std::find_if(mImpl->coolantLoops.begin(), mImpl->coolantLoops.end(),
			[&](auto const& l) { return l.authored.id == id; });
		if (loop == mImpl->coolantLoops.end())
			return refuse("Installation '" + std::string(id) + "' has no Condition model to restore");
		auto const& model = loop->authored.condition;
		if (!model) return refuse("Installation '" + std::string(id) + "' has no Condition model to restore");
		if (!std::isfinite(target) || target < 0.0 || target > 1.0)
			return refuse("restoreCondition target must be finite and from 0 to 1");
		if (target <= loop->condition + StationConditionEpsilon)
			return refuse("restoreCondition target must be above the installation's current Condition");
		if (!loop->hasRestoreStock)
			return refuse("Installation '" + std::string(id)
				+ "' declares no service-stock store to fund Condition recovery");
		if (!std::isfinite(model->restorePerHour) || !std::isfinite(model->stockUnitsPerConditionPoint)
			|| model->restorePerHour <= 0.0 || model->stockUnitsPerConditionPoint <= 0.0)
			return refuse("Installation '" + std::string(id)
				+ "' declares a restore that its bounded rate or per-point cost cannot deliver");
		auto const& stock = mImpl->stores[loop->restoreStoreIndex];
		auto const drawable = std::max(0.0, stock.currentQualified - stock.authored.protectedReserve);
		if (drawable <= 0.0)
			return refuse("Service stock '" + stock.authored.id
				+ "' has none qualified above its protected reserve to fund Condition recovery");
		Impl::FaultCommand command;
		command.kind = Impl::FaultCommand::Kind::RestoreCondition;
		command.afterStep = mImpl->clock.steps(); command.installationId = id;
		command.restoreTarget = target;
		mImpl->faultCommands.push_back(std::move(command));
		loop->pendingRestoreTarget = target;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// Repair as a runtime-started maintenance job (#842). Validated whole
	// before any mutation, then the job is instantiated immediately in its
	// pre-start state so the snapshot shows the repair pending, and the
	// ordinary audited maintenance pass reserves its parts, isolates the
	// installation and runs the full duration at the next boundary. The
	// command journal freezes the repair identity; the plan's effective
	// duration and parts are frozen into the job here, so a later live
	// retune of the plan cannot reach a repair already started.
	bool StationSystemsRuntime::repairFault(std::string_view id, std::string_view mode,
		std::string* jobIdOut, std::string* diagnostic)
	{
		auto refuse = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!mImpl->settings.enabled || !mImpl->snapshot.healthy) return refuse("Station runtime is not running or healthy");
		if (mode == "overtemp-runaway")
			return refuse("A latching Trip is not a repairable Fault: clear it with resetTrip");
		// #844: the repairable mode names the machine kind that owns it,
		// so the target resolves by kind - no cross-kind guessing. The
		// thermal-plant catalogue joins the coolant-loop pilot in one
		// repair starter: same reservation, isolation and completion
		// machinery, only the target family differs.
		using RepairTarget = Impl::MaintenanceState::Target;
		RepairTarget target{RepairTarget::CoolantLoop};
		size_t targetIndex{0};
		std::string installationKind;
		const RepairPartsSettings* wiring{nullptr};
		bool faultPresent{false};
		auto const unknownInstallation = [&](std::string_view machineKind)
		{
			auto const identities = stationInstallationIdentities(mImpl->settings);
			if (std::none_of(identities.begin(), identities.end(), [&](auto const& i) { return i.id == id; })
				&& std::none_of(mImpl->sabatierReactors.begin(), mImpl->sabatierReactors.end(),
					[&](auto const& s) { return s.authored.id == id; })
				&& std::none_of(mImpl->methaneProcessors.begin(), mImpl->methaneProcessors.end(),
					[&](auto const& s) { return s.authored.id == id; })
				&& std::none_of(mImpl->radiators.begin(), mImpl->radiators.end(),
					[&](auto const& r) { return r.authored.id == id; }))
				return refuse("Unknown installation '" + std::string(id) + "'");
			return refuse("Installation '" + std::string(id) + "' is not a "
				+ std::string(machineKind) + "; the repairable mode '"
				+ std::string(mode) + "' names one");
		};
		if (mode == "coolant-leak" || mode == "pump-seizure")
		{
			auto const loop = std::find_if(mImpl->coolantLoops.begin(), mImpl->coolantLoops.end(),
				[&](auto const& l) { return l.authored.id == id; });
			if (loop == mImpl->coolantLoops.end())
			{
				auto const identities = stationInstallationIdentities(mImpl->settings);
				if (std::none_of(identities.begin(), identities.end(), [&](auto const& i) { return i.id == id; })
					&& std::none_of(mImpl->sabatierReactors.begin(), mImpl->sabatierReactors.end(),
						[&](auto const& s) { return s.authored.id == id; })
					&& std::none_of(mImpl->methaneProcessors.begin(), mImpl->methaneProcessors.end(),
						[&](auto const& s) { return s.authored.id == id; }))
					return refuse("Unknown installation '" + std::string(id) + "'");
				return refuse("Installation '" + std::string(id) + "' declares no repair-parts wiring");
			}
			wiring = loop->authored.repairParts ? &*loop->authored.repairParts : nullptr;
			target = RepairTarget::CoolantLoop;
			targetIndex = static_cast<size_t>(std::distance(mImpl->coolantLoops.begin(), loop));
			installationKind = "coolant loop";
			faultPresent = mode == "coolant-leak" ? loop->fault.has_value() : loop->seizure.has_value();
		}
		else if (mode == "compressor-failure" || mode == "refrigerant-leak")
		{
			auto const chiller = std::find_if(mImpl->chillers.begin(), mImpl->chillers.end(),
				[&](auto const& c) { return c.authored.id == id; });
			if (chiller == mImpl->chillers.end()) return unknownInstallation("chiller");
			wiring = chiller->authored.repairParts ? &*chiller->authored.repairParts : nullptr;
			target = RepairTarget::Chiller;
			targetIndex = static_cast<size_t>(std::distance(mImpl->chillers.begin(), chiller));
			installationKind = "chiller";
			faultPresent = mode == "compressor-failure"
				? chiller->compressorFailure.has_value() : chiller->refrigerantLeak.has_value();
		}
		else if (mode == "panel-puncture")
		{
			auto const radiator = std::find_if(mImpl->radiators.begin(), mImpl->radiators.end(),
				[&](auto const& r) { return r.authored.id == id; });
			if (radiator == mImpl->radiators.end()) return unknownInstallation("radiator");
			wiring = radiator->authored.repairParts ? &*radiator->authored.repairParts : nullptr;
			target = RepairTarget::Radiator;
			targetIndex = static_cast<size_t>(std::distance(mImpl->radiators.begin(), radiator));
			installationKind = "radiator";
			faultPresent = radiator->puncture.has_value();
		}
		else if (mode == "mechanical-seizure")
		{
			auto const engine = std::find_if(mImpl->heatEngines.begin(), mImpl->heatEngines.end(),
				[&](auto const& e) { return e.authored.id == id; });
			if (engine == mImpl->heatEngines.end()) return unknownInstallation("heat engine");
			wiring = engine->authored.repairParts ? &*engine->authored.repairParts : nullptr;
			target = RepairTarget::HeatEngine;
			targetIndex = static_cast<size_t>(std::distance(mImpl->heatEngines.begin(), engine));
			installationKind = "heat engine";
			faultPresent = engine->seizure.has_value();
		}
		else
			return refuse("Unknown fault mode '" + std::string(mode) + "' for repair on '" + std::string(id) + "'");
		if (!wiring)
			return refuse("Installation '" + std::string(id) + "' declares no repair-parts wiring");
		auto const plan = std::find_if(wiring->repairs.begin(), wiring->repairs.end(),
			[&](auto const& p) { return p.modeId == mode; });
		if (plan == wiring->repairs.end())
			return refuse("Installation '" + std::string(id) + "' declares no repair plan for fault mode '" + std::string(mode) + "'");
		if (!faultPresent)
			return refuse("No active or pending '" + std::string(mode) + "' Fault on '" + std::string(id) + "' to repair");
		if (std::any_of(mImpl->repairJobs.begin(), mImpl->repairJobs.end(), [&](auto const& job)
			{
				return !job.exhausted && job.authored.installationId == id && job.repairModeId == mode;
			}))
			return refuse("A repair of '" + std::string(mode) + "' on '" + std::string(id) + "' is already running or pending");
		if (!std::isfinite(plan->durationSeconds) || plan->durationSeconds <= 0.0
			|| !std::isfinite(plan->partsQuantity) || plan->partsQuantity <= 0.0)
			return refuse("Repair plan for '" + std::string(mode) + "' on '" + std::string(id)
				+ "' has a non-positive duration or parts quantity");
		size_t partsStoreIndex{SIZE_MAX};
		size_t wasteStoreIndex{SIZE_MAX};
		for (size_t index = 0; index < mImpl->stores.size(); ++index)
		{
			if (mImpl->stores[index].authored.id == wiring->partsStoreId) partsStoreIndex = index;
			if (mImpl->stores[index].authored.id == plan->wasteStoreId) wasteStoreIndex = index;
		}
		if (partsStoreIndex == SIZE_MAX)
			return refuse("Repair parts store '" + wiring->partsStoreId + "' resolved to no configured store");
		if (wasteStoreIndex == SIZE_MAX)
			return refuse("Repair waste store '" + plan->wasteStoreId + "' resolved to no configured store");
		// Reservable now: the parts the live reservations of running work
		// have not already taken, and the waste headroom the pending
		// consumed claims have not already committed. A repair whose parts
		// are not there to be reserved is refused here, not queued to
		// block forever.
		auto const covers = [](double available, double needed)
		{
			return available + StationBalanceTolerance * std::max(1.0, needed) >= needed;
		};
		double reservedParts{0.0};
		double pendingWaste{0.0};
		auto const sumClaims = [&](auto const& jobs)
		{
			for (auto const& job : jobs)
			{
				if (job.phase != StationMaintenancePhase::Active) continue;
				for (size_t requirementIndex = 0;
					requirementIndex < job.requirements.size(); ++requirementIndex)
				{
					auto const& requirement = job.requirements[requirementIndex];
					if (requirement.storeIndex == partsStoreIndex) reservedParts += requirement.reserved;
					if (job.wasteStoreIndex == wasteStoreIndex
						&& job.authored.requirements[requirementIndex].consumed)
						pendingWaste += requirement.reserved;
				}
			}
		};
		sumClaims(mImpl->maintenanceJobs);
		sumClaims(mImpl->repairJobs);
		auto const& parts = mImpl->stores[partsStoreIndex];
		auto const drawable = parts.currentQualified - reservedParts;
		if (!covers(drawable, plan->partsQuantity))
			return refuse("Repair parts store '" + wiring->partsStoreId + "' cannot cover "
				+ std::to_string(plan->partsQuantity) + " " + parts.authored.unit
				+ " for the repair (drawable " + std::to_string(std::max(0.0, drawable)) + ")");
		auto const& waste = mImpl->stores[wasteStoreIndex];
		auto const headroom = waste.authored.capacity - waste.currentQualified
			- waste.currentUnqualified - pendingWaste;
		if (!covers(headroom, plan->partsQuantity))
			return refuse("Repair waste store '" + plan->wasteStoreId
				+ "' lacks headroom for the consumed parts ("
				+ std::to_string(plan->partsQuantity) + " " + waste.authored.unit
				+ " needed, " + std::to_string(std::max(0.0, headroom)) + " free)");
		// Accepted: journal the command, then mint the job in its
		// pre-start state. The maintenance pass reserves and isolates at
		// the next boundary exactly as it would for an authored job whose
		// window just opened.
		Impl::FaultCommand command;
		command.kind = Impl::FaultCommand::Kind::RepairFault;
		command.afterStep = mImpl->clock.steps(); command.installationId = id; command.modeId = mode;
		mImpl->faultCommands.push_back(std::move(command));
		Impl::MaintenanceState job;
		job.authored.id = "repair-" + std::to_string(++mImpl->repairJobCounter);
		job.authored.installationId = std::string(id);
		job.authored.firstOccurrenceSeconds = mImpl->clock.elapsedSeconds();
		job.authored.durationSeconds = plan->durationSeconds;
		job.authored.recurrenceSeconds = 0.0;
		job.authored.priority = 0;
		job.authored.requirements = {MaintenanceRequirementSettings{wiring->partsStoreId, plan->partsQuantity, true}};
		job.authored.wasteStoreId = plan->wasteStoreId;
		job.installationKind = installationKind;
		job.target = target;
		job.targetIndex = targetIndex;
		Impl::MaintenanceState::RequirementState requirementState;
		requirementState.storeIndex = partsStoreIndex;
		job.requirements.push_back(requirementState);
		job.wasteStoreIndex = wasteStoreIndex;
		job.isRepair = true;
		job.repairModeId = std::string(mode);
		job.phase = StationMaintenancePhase::Blocked;
		job.blockReason = "repair commanded; awaiting reservation";
		mImpl->repairJobs.push_back(std::move(job));
		if (jobIdOut) *jobIdOut = mImpl->repairJobs.back().authored.id;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool StationSystemsRuntime::resetTrip(std::string_view id, std::string* diagnostic)
	{
		auto reset = [&](auto& states) {
			for (auto& state : states)
			{
				if (state.authored.id != id || !state.trip || state.pendingTripHeat) continue;
				Impl::FaultCommand command;
				command.kind = Impl::FaultCommand::Kind::ResetTrip;
				command.afterStep = mImpl->clock.steps(); command.installationId = id;
				mImpl->faultCommands.push_back(command);
				state.trip.reset();
				appendBoundedEvent(mImpl->events, mImpl->clock.steps(), "trip", "Trip reset on '" + std::string(id) + "'");
				if (diagnostic) diagnostic->clear();
				return true;
			}
			return false;
		};
		if (mImpl->settings.enabled && mImpl->snapshot.healthy
			&& (reset(mImpl->sabatierReactors) || reset(mImpl->methaneProcessors))) return true;
		if (diagnostic) *diagnostic = "resetTrip requires a latched Trip on '" + std::string(id) + "'";
		return false;
	}

	bool StationSystemsRuntime::hasFaultCommands() const
	{
		return std::any_of(mImpl->faultCommands.begin(), mImpl->faultCommands.end(),
			[](auto const& command) { return command.kind != Impl::FaultCommand::Kind::SettingsUpdate; });
	}

	bool StationSystemsRuntime::hasFaultReplayState() const
	{
		return hasFaultCommands() || (mImpl->settings.enabled
			&& (std::any_of(mImpl->coolantLoops.begin(), mImpl->coolantLoops.end(),
				[](auto const& loop) { return loop.authored.condition.has_value()
					// #843: Wear integrates over the run, so a save of a
					// worn machine needs the replay block even when no client
					// ever issued a command: without it the machine would
					// come back unworn, which is a lie about its capacity.
					|| loop.authored.wear.has_value(); })
			|| std::any_of(mImpl->batteries.begin(), mImpl->batteries.end(),
				[](auto const& battery) { return battery.authored.capacityFade.has_value(); })
			|| std::any_of(mImpl->sabatierReactors.begin(), mImpl->sabatierReactors.end(),
				[](auto const& s) { return s.authored.overtempRunaway.has_value(); })
			|| std::any_of(mImpl->methaneProcessors.begin(), mImpl->methaneProcessors.end(),
				[](auto const& s) { return s.authored.overtempRunaway.has_value(); })));
	}

	void StationSystemsRuntime::writeFaultReplay(Serializer& s) const
	{
		s.beginMap("stationFaultReplay");
		s.writeUint64("completedSteps", mImpl->clock.steps());
		writeStationSystemsSettingsMap(s, mImpl->faultReplayInitialSettings.value_or(mImpl->settings));
		s.beginArray("commands");
		for (auto const& command : mImpl->faultCommands)
		{
			s.beginMap("");
			s.writeUint64("afterStep", command.afterStep);
			if (command.kind == Impl::FaultCommand::Kind::SettingsUpdate)
				writeStationSystemsSettingsMap(s, *command.settingsUpdate);
			else if (command.kind == Impl::FaultCommand::Kind::ResetTrip)
			{
				s.writeUint32("commandKind", 2);
				s.writeString("installationId", command.installationId);
			}
			else if (command.kind == Impl::FaultCommand::Kind::RepairFault)
			{
				s.writeUint32("commandKind", 3);
				s.writeString("installationId", command.installationId);
				s.writeString("modeId", command.modeId);
			}
			else if (command.kind == Impl::FaultCommand::Kind::RestoreCondition)
			{
				s.writeUint32("commandKind", 1);
				s.writeString("installationId", command.installationId);
				s.writeDouble("targetCondition", command.restoreTarget);
			}
			else
			{
				s.writeString("installationId", command.installationId);
				s.writeString("modeId", command.modeId);
				s.writeUint32("causeKind", static_cast<uint32_t>(command.cause.kind));
				s.writeString("causeDetail", command.cause.detail);
				if (command.modeId == "overtemp-runaway") s.writeDouble("heatSpikeKJ", command.heatSpikeKJ);
				else if (command.modeId == "refrigerant-leak")
				{
					s.writeString("compartmentId", command.refrigerantLeak->compartmentId);
					s.writeDouble("chargeKg", command.refrigerantLeak->chargeKg);
					s.writeDouble("leakRateKgPerS", command.refrigerantLeak->leakRateKgPerS);
					s.writeDouble("specificHeatKJPerKgK", command.refrigerantLeak->specificHeatKJPerKgK);
				}
				else if (command.modeId == "panel-puncture")
				{
					s.writeString("compartmentId", command.panelPuncture->compartmentId);
					s.writeDouble("coolantKg", command.panelPuncture->coolantKg);
					s.writeDouble("leakRateKgPerS", command.panelPuncture->leakRateKgPerS);
					s.writeDouble("specificHeatKJPerKgK", command.panelPuncture->specificHeatKJPerKgK);
				}
				// #845: the power-source injections. `failed-start` is a
				// parameterless mode like the AV=0 thermal-plant modes; the
				// solar modes freeze their declared fraction/availability;
				// the runaway freezes the one-shot booking it will make.
				else if (command.modeId == "string-loss")
					s.writeDouble("lossFraction", command.stringLoss->lossFraction);
				else if (command.modeId == "tracking-failure")
					s.writeDouble("availability", command.trackingFailure->availability);
				else if (command.modeId == "thermal-runaway")
				{
					s.writeString("compartmentId", command.thermalRunaway->compartmentId);
					s.writeDouble("heatKJ", command.thermalRunaway->heatKJ);
					s.writeDouble("toxicKg", command.thermalRunaway->toxicKg);
				}
				else if (command.modeId != "compressor-failure" && command.modeId != "mechanical-seizure"
					&& command.modeId != "failed-start")
				{
					s.writeString("compartmentId", command.parameters.compartmentId);
					s.writeDouble("releasedKg", command.parameters.releasedKg);
				}
			}
			s.endMap();
		}
		s.endArray();
		s.endMap();
	}

	std::unique_ptr<StationSystemsRuntime> StationSystemsRuntime::readFaultReplay(Serializer& s,
		StationSystemsSettings const& settings, float baseSeconds)
	{
		s.beginMap("stationFaultReplay");
		auto const steps = s.readUint64("completedSteps");
		StationSystemsSettings initial;
		readStationSystemsSettingsMap(s, 118, initial);
		auto runtime = std::make_unique<StationSystemsRuntime>(initial, baseSeconds);
		s.beginArray("commands");
		uint64_t previous = 0;
		while (s.nextArrayItem())
		{
			s.beginMap("");
			auto const after = s.readUint64("afterStep");
			if (after < previous || after > steps)
				throw SerializationException("Invalid fault replay command ordering");
			if (!runtime->advanceTo(after * runtime->mImpl->clock.stepSeconds()))
				throw SerializationException("Fault replay advance failed");
			std::string diagnostic;
			if (s.hasField("stationSystems"))
			{
				StationSystemsSettings update;
				readStationSystemsSettingsMap(s, 118, update);
				if (!runtime->updateSettings(update, &diagnostic))
					throw SerializationException("Invalid fault replay settings: " + diagnostic);
			}
			else if (s.hasField("commandKind"))
			{
				auto const kind = s.readUint32("commandKind");
				if (kind != 1 && kind != 2 && kind != 3) throw SerializationException("Unknown fault replay command kind");
				auto const id = s.readString("installationId");
				bool const accepted = kind == 2 ? runtime->resetTrip(id, &diagnostic)
					: kind == 1 ? runtime->restoreCondition(id, s.readDouble("targetCondition"), &diagnostic)
					: runtime->repairFault(id, s.readString("modeId"), nullptr, &diagnostic);
				if (!accepted)
					throw SerializationException("Invalid fault replay recovery command: " + diagnostic);
			}
			else
			{
				auto const id = s.readString("installationId");
				auto const mode = s.readString("modeId");
				auto const kind = s.readUint32("causeKind");
				auto const detail = s.readString("causeDetail");
				CoolantLeakSettings parameters;
				double heat = 0;
				std::optional<RefrigerantLeakSettings> refrigerant;
				std::optional<PanelPunctureSettings> puncture;
				std::optional<SolarStringLossSettings> stringLoss;
				std::optional<SolarTrackingFailureSettings> trackingFailure;
				std::optional<BatteryThermalRunawaySettings> thermalRunaway;
				if (mode == "overtemp-runaway") heat = s.readDouble("heatSpikeKJ");
				else if (mode == "refrigerant-leak")
					refrigerant = RefrigerantLeakSettings{s.readString("compartmentId"),
						s.readDouble("chargeKg"), s.readDouble("leakRateKgPerS"),
						s.readDouble("specificHeatKJPerKgK")};
				else if (mode == "panel-puncture")
					puncture = PanelPunctureSettings{s.readString("compartmentId"),
						s.readDouble("coolantKg"), s.readDouble("leakRateKgPerS"),
						s.readDouble("specificHeatKJPerKgK")};
				else if (mode == "string-loss")
					stringLoss = SolarStringLossSettings{s.readDouble("lossFraction")};
				else if (mode == "tracking-failure")
					trackingFailure = SolarTrackingFailureSettings{s.readDouble("availability")};
				else if (mode == "thermal-runaway")
					thermalRunaway = BatteryThermalRunawaySettings{s.readString("compartmentId"),
						s.readDouble("heatKJ"), s.readDouble("toxicKg")};
				else if (mode != "compressor-failure" && mode != "mechanical-seizure"
					&& mode != "failed-start")
					parameters = {s.readString("compartmentId"), s.readDouble("releasedKg")};
				if (!isInjectableCause(static_cast<FaultCause::Kind>(kind))
					|| !runtime->injectFault(id, mode, {static_cast<FaultCause::Kind>(kind), detail}, &diagnostic))
					throw SerializationException("Invalid fault replay: " + diagnostic);
				// Settings updates replay in command order, so injection must
				// derive the same frozen parameters at this point in history.
				// Never overwrite them with unvalidated stream values before
				// comparing: that would turn this consistency check into a no-op.
				if (runtime->mImpl->faultCommands.back().parameters != parameters
					|| runtime->mImpl->faultCommands.back().heatSpikeKJ != heat
					|| runtime->mImpl->faultCommands.back().refrigerantLeak != refrigerant
					|| runtime->mImpl->faultCommands.back().panelPuncture != puncture
					|| runtime->mImpl->faultCommands.back().stringLoss != stringLoss
					|| runtime->mImpl->faultCommands.back().trackingFailure != trackingFailure
					|| runtime->mImpl->faultCommands.back().thermalRunaway != thermalRunaway)
					throw SerializationException("Fault replay effective parameters disagree with the command: " + mode);
			}
			s.endMap();
			previous = after;
		}
		s.endArray();
		s.endMap();
		if (runtime->effectiveSettings() != settings)
			throw SerializationException("Fault replay final settings disagree with the document");
		if (!runtime->advanceTo(steps * runtime->mImpl->clock.stepSeconds()))
			throw SerializationException("Fault replay advance failed");
		return runtime;
	}
}
