#include "StationSystemsImpl.h"
#include "core/Serializer.h"
#include "core/StationReference.h"

namespace core
{
bool StationSystemsRuntime::injectFault(std::string_view id, std::string_view mode,
	FaultCause const& cause, std::string* diagnostic)
{
	auto refuse = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
	if (!mImpl->settings.enabled || !mImpl->snapshot.healthy) return refuse("Station runtime is not running or healthy");
	auto const identities = stationInstallationIdentities(mImpl->settings);
	if (std::none_of(identities.begin(), identities.end(), [&](auto const& i) { return i.id == id; }))
		return refuse("Unknown installation '" + std::string(id) + "'");
	auto loop = std::find_if(mImpl->coolantLoops.begin(), mImpl->coolantLoops.end(),
		[&](auto const& l) { return l.authored.id == id; });
	if (loop == mImpl->coolantLoops.end() || mode != "coolant-leak")
		return refuse("Unknown fault mode '" + std::string(mode) + "' for '" + std::string(id) + "'");
	if (!loop->authored.coolantLeak) return refuse("coolant-leak is not enabled for '" + std::string(id) + "'");
	if (loop->fault) return refuse("Fault mode is already active or pending on '" + std::string(id) + "'");
	if (cause.kind != FaultCause::Kind::AgentAction && cause.kind != FaultCause::Kind::ScenarioEvent
		&& cause.kind != FaultCause::Kind::Editor) return refuse("Unknown fault cause kind");
	if (cause.detail.size() > 1024) return refuse("Fault cause detail exceeds 1024 bytes");
	auto const& parameters = *loop->authored.coolantLeak;
	if (!std::isfinite(parameters.releasedKg) || parameters.releasedKg <= 0
		|| std::none_of(mImpl->atmospheres.begin(), mImpl->atmospheres.end(),
			[&](auto const& a) { return a.authored.id == parameters.compartmentId; }))
		return refuse("Invalid coolant-leak parameters");
	// All refusal paths precede mutation, including pending duplicate detection.
	Impl::FaultCommand command;
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

bool StationSystemsRuntime::hasFaultCommands() const
{
	return std::any_of(mImpl->faultCommands.begin(), mImpl->faultCommands.end(),
		[](auto const& command) { return !command.settingsUpdate; });
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
		if (command.settingsUpdate)
			writeStationSystemsSettingsMap(s, *command.settingsUpdate);
		else
		{
			s.writeString("installationId", command.installationId);
			s.writeString("modeId", command.modeId);
			s.writeUint32("causeKind", static_cast<uint32_t>(command.cause.kind));
			s.writeString("causeDetail", command.cause.detail);
			s.writeString("compartmentId", command.parameters.compartmentId);
			s.writeDouble("releasedKg", command.parameters.releasedKg);
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
	readStationSystemsSettingsMap(s, 112, initial);
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
			readStationSystemsSettingsMap(s, 112, update);
			if (!runtime->updateSettings(update, &diagnostic))
				throw SerializationException("Invalid fault replay settings: " + diagnostic);
		}
		else
		{
			auto const id = s.readString("installationId");
			auto const mode = s.readString("modeId");
			auto const kind = s.readUint32("causeKind");
			auto const detail = s.readString("causeDetail");
			CoolantLeakSettings const parameters{s.readString("compartmentId"), s.readDouble("releasedKg")};
			if (kind > static_cast<uint32_t>(FaultCause::Kind::Editor)
				|| !runtime->injectFault(id, mode, {static_cast<FaultCause::Kind>(kind), detail}, &diagnostic))
				throw SerializationException("Invalid fault replay: " + diagnostic);
			if (runtime->mImpl->faultCommands.back().parameters != parameters)
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
