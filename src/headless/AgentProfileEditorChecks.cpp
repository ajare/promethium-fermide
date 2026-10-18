#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentProfileEditor.h"
#include "core/AgentTag.h"
#include "core/ObjectUsage.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition)
			throw std::runtime_error("Agent profile-editor check failed: " + what);
		++checksRun;
	}
}

int main()
{
	using namespace core;
	using namespace std;

	try
	{
		// The editor is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the individual-
		// profile edit family is unit-testable without driving the World
		// facade. World keeps the state it edits (ADR 0001); the
		// collaborator holds only a reference and reaches it through
		// friend access.

		string diagnostic;
		World world("profile-editor", 10, 3);
		AgentProfileEditor editor(world);
		auto const corridor = world.addCorridor(0, 0, 10);
		world.finishBuild();
		auto const ada = world.createAgent("Ada", corridor, 0, 1.0f);
		auto* adaEntity = world.lookupAgent(ada).entity;

		// Unknown Agents are refused with the lookup diagnostic before
		// anything else is judged.
		require(!editor.setColour(AgentId{ 9999 }, AgentColour{}, &diagnostic)
				&& diagnostic == "Agent handle 9999 is invalid or has been removed",
			"an unknown Agent is refused with the lookup diagnostic");
		require(!editor.setObjectUsageOverrides(AgentId{ 9999 },
				ObjectUsage::Arms, 1.0f, &diagnostic)
				&& diagnostic == "Agent handle 9999 is invalid or has been removed",
			"the object-usage pair refuses unknown Agents too");

		// The pause gate fires while the simulation runs, ahead of the
		// unchanged check and any validation of the property itself.
		require(!world.isSimulationPaused(), "the freshly built World simulates");
		require(!editor.setColour(ada, AgentColour{}, &diagnostic)
				&& diagnostic == "Pause the simulation before editing individual Agent properties",
			"the colour setter refuses while running");
		require(!editor.setObjectUsageOverrides(ada, ObjectUsage::Arms, -1.0f,
				&diagnostic)
				&& diagnostic == "Pause the simulation before editing individual Agent properties",
			"the object-usage pair refuses while running, before validity");

		world.pauseSimulation();

		// Invalid values are refused before any authored state changes.
		require(!editor.setEscalatorWalkingChance(ada, 1.5f, &diagnostic)
				&& diagnostic == "Escalator walking chance must be finite and in [0, 1]",
			"an out-of-range escalator walking chance is refused");
		require(!adaEntity->getIndividualEscalatorWalkingChance().has_value(),
			"a refused write leaves the override absent");
		require(!editor.setWalkSpeedModifier(ada, 2.0f, &diagnostic)
				&& !diagnostic.empty(),
			"an out-of-range walk speed modifier is refused with a reason");
		MobilityProfile brokenProfile;
		brokenProfile.set(TraversalKind::Staircase,
			static_cast<MobilityUse>(99));
		require(!editor.setMobilityProfile(ada, brokenProfile, &diagnostic)
				&& diagnostic == "The individual Mobility profile contains an invalid Mobility use",
			"an invalid Mobility profile is refused");

		// The unchanged check reports the row's own message and changes
		// nothing.
		AgentColour teal{ 26, 153, 128 };
		require(editor.setColour(ada, teal, &diagnostic)
				&& adaEntity->getIndividualColour() == optional<AgentColour>{ teal },
			"a colour override writes through");
		require(!editor.setColour(ada, teal, &diagnostic)
				&& diagnostic == "The individual Agent Colour is unchanged",
			"an unchanged colour is refused with the row message");

		// The Height modifier's preflight runs after the unchanged check:
		// an out-of-range value is refused, a valid in-range value that
		// fits the Agent's live pose commits, and clearing back to the
		// default 1.0 also fits.
		require(!editor.setHeightModifier(ada, 1.5f, &diagnostic)
				&& !adaEntity->getIndividualHeightModifier().has_value(),
			"an out-of-range height modifier is refused");
		require(editor.setHeightModifier(ada, 0.9f, &diagnostic)
				&& adaEntity->getIndividualHeightModifier()
					== optional<float>{ 0.9f },
			"a fitting height modifier commits");
		require(editor.setHeightModifier(ada, std::nullopt, &diagnostic)
				&& !adaEntity->getIndividualHeightModifier().has_value(),
			"clearing the height modifier back to the default fits");

		// Permission adherence wraps the write in the replanning seam:
		// flipping the individual override changes the effective value.
		require(editor.setPermissionAdherence(ada, false, &diagnostic)
				&& adaEntity->getIndividualPermissionAdherence()
					== optional<bool>{ false }
				&& !adaEntity->getEffectivePermissionAdherence().value,
			"refusing adherence flips the effective value");
		require(editor.setPermissionAdherence(ada, true, &diagnostic)
				&& adaEntity->getEffectivePermissionAdherence().value,
			"restoring adherence flips the effective value back");

		// The remote object-control flags commit with their coordinator
		// notification.
		require(editor.setRemoteAccessPanels(ada, true, &diagnostic)
				&& adaEntity->getIndividualRemoteAccessPanels()
					== optional<bool>{ true },
			"the remote Access panel flag commits");
		require(editor.setRemoteBoothWindowShutters(ada, true, &diagnostic)
				&& adaEntity->getIndividualRemoteBoothWindowShutters()
					== optional<bool>{ true },
			"the remote BoothWindow shutter flag commits");

		// The coupled object-usage pair: the shared validity rule is
		// judged on the full prospective state.
		require(!editor.setObjectUsageOverrides(ada, ObjectUsage::Arms, -1.0f,
				&diagnostic)
				&& diagnostic == "Object usage must be Arms, None or Remote control; usable distance must be finite and positive",
			"Arms with a non-positive distance is refused");
		require(!adaEntity->getIndividualObjectUsage().has_value()
				&& !adaEntity->getIndividualObjectUsageDistance().has_value(),
			"a refused pair leaves both overrides absent");
		require(editor.setObjectUsageOverrides(ada, ObjectUsage::RemoteControl,
				4.0f, &diagnostic)
				&& adaEntity->getIndividualObjectUsage()
					== optional<ObjectUsage>{ ObjectUsage::RemoteControl }
				&& adaEntity->getIndividualObjectUsageDistance()
					== optional<float>{ 4.0f },
			"a valid pair commits both overrides");
		require(!editor.setObjectUsageOverrides(ada, ObjectUsage::RemoteControl,
				4.0f, &diagnostic)
				&& diagnostic == "The individual Object usage properties are unchanged",
			"an unchanged pair is refused with the pair message");

		// The single-field setters read the Agent's other override so the
		// pair's rule is judged on the full prospective state: switching
		// the mode keeps the committed distance.
		require(editor.setObjectUsage(ada, ObjectUsage::Arms, &diagnostic)
				&& adaEntity->getIndividualObjectUsage()
					== optional<ObjectUsage>{ ObjectUsage::Arms }
				&& adaEntity->getIndividualObjectUsageDistance()
					== optional<float>{ 4.0f },
			"the usage-only setter keeps the committed distance");
		// None usage tolerates any distance, including a cleared one.
		require(editor.setObjectUsageDistance(ada, std::nullopt, &diagnostic)
				&& editor.setObjectUsage(ada, ObjectUsage::None, &diagnostic)
				&& adaEntity->getIndividualObjectUsage()
					== optional<ObjectUsage>{ ObjectUsage::None }
				&& !adaEntity->getIndividualObjectUsageDistance().has_value(),
			"None usage commits with a cleared distance");
		// Arms with a cleared distance falls back to the valid baseline;
		// an explicit negative distance is not.
		require(editor.setObjectUsage(ada, ObjectUsage::Arms, &diagnostic)
				&& adaEntity->getObjectUsage() == ObjectUsage::Arms
				&& adaEntity->getObjectUsageDistance() > 0.0f,
			"Arms with a cleared distance falls back to the baseline");
		require(!editor.setObjectUsageDistance(ada, -2.0f, &diagnostic)
				&& diagnostic == "Object usage must be Arms, None or Remote control; usable distance must be finite and positive",
			"an explicit negative distance is refused");

		// The World facade forwards to the same editor: a facade write is
		// visible through the editor's view of the Agent and vice versa.
		AgentColour amber{ 230, 179, 51 };
		require(world.setAgentIndividualColour(ada, amber, &diagnostic)
				&& adaEntity->getIndividualColour() == optional<AgentColour>{ amber },
			"the facade colour write is visible on the Agent");
		require(!editor.setColour(ada, amber, &diagnostic)
				&& diagnostic == "The individual Agent Colour is unchanged",
			"the editor sees the facade write as unchanged");
		require(world.setAgentIndividualWalkSpeedModifier(ada, 1.05f,
				&diagnostic)
				&& editor.setWalkSpeedModifier(ada, 1.05f, &diagnostic) == false
				&& diagnostic == "The individual Walk speed modifier is unchanged",
			"facade and editor share one override record");

		cout << "Agent profile-editor checks passed (" << checksRun
			<< " checks)" << endl;
	}
	catch (exception const& error)
	{
		cerr << error.what() << endl;
		return 1;
	}
	return 0;
}
