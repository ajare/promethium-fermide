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
			&& std::none_of(mImpl->methaneProcessors.begin(), mImpl->methaneProcessors.end(), [&](auto const& s) { return s.authored.id == id; }))
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
				[](auto const& loop) { return loop.authored.condition.has_value(); })
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
				else
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
		readStationSystemsSettingsMap(s, 114, initial);
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
				readStationSystemsSettingsMap(s, 114, update);
				if (!runtime->updateSettings(update, &diagnostic))
					throw SerializationException("Invalid fault replay settings: " + diagnostic);
			}
			else if (s.hasField("commandKind"))
			{
				auto const kind = s.readUint32("commandKind");
				if (kind != 1 && kind != 2) throw SerializationException("Unknown fault replay command kind");
				auto const id = s.readString("installationId");
				if (!(kind == 2 ? runtime->resetTrip(id, &diagnostic)
					: runtime->restoreCondition(id, s.readDouble("targetCondition"), &diagnostic)))
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
				if (mode == "overtemp-runaway") heat = s.readDouble("heatSpikeKJ");
				else parameters = {s.readString("compartmentId"), s.readDouble("releasedKg")};
				if (!isInjectableCause(static_cast<FaultCause::Kind>(kind))
					|| !runtime->injectFault(id, mode, {static_cast<FaultCause::Kind>(kind), detail}, &diagnostic))
					throw SerializationException("Invalid fault replay: " + diagnostic);
				if (runtime->mImpl->faultCommands.back().parameters != parameters
					|| runtime->mImpl->faultCommands.back().heatSpikeKJ != heat)
					throw SerializationException("Fault replay effective parameters disagree with the command");
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
