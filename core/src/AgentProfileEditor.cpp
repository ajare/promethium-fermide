#include "core/AgentProfileEditor.h"

#include <utility>

#include "core/SimulationCoordinator.h"
#include "core/World.h"

using namespace std;

namespace core
{
	namespace
	{
		// Builds one plain float override row: the value is validated as a
		// degenerate range { v, v } exactly as the replaced facade methods
		// did, and written straight through to the Agent's individual
		// property with no further side effects.
		IndividualOverrideDescriptor<float> plainFloatOverride(
			std::string unchangedMessage,
			bool (*validator)(AgentModifierRange const&, std::string*),
			std::optional<float> const& (Agent::*read)() const,
			void (Agent::*write)(std::optional<float>))
		{
			IndividualOverrideDescriptor<float> descriptor;
			descriptor.unchangedMessage = std::move(unchangedMessage);
			descriptor.validate = [validator](float const& value,
				std::string* diagnostic)
			{ return validator({ value, value }, diagnostic); };
			descriptor.read = [read](Agent const& agent)
				-> std::optional<float> const& { return (agent.*read)(); };
			descriptor.apply = [write](World&, AgentId, Agent& agent,
				std::optional<float> const& value) { (agent.*write)(value); };
			return descriptor;
		}
	}

	// The one override table (ADR 0022, #677), moved out of World with the
	// profile editor (#683). Built once, inside this class's member scope:
	// the special rows' closures reach World-owned state - the height-fit
	// preflight, the adherence replanning seam, and the simulation
	// coordinator behind the remote object-control flags - through this
	// scope's friend access (the ADR 0022 access seam). Object usage and
	// its distance stay on their own composite path below: they are a
	// coupled pair with a shared validity rule and a joint replanning side
	// effect, not two independent single-property rows.
	IndividualOverrideTable const& AgentProfileEditor::overrideTable()
	{
		static IndividualOverrideTable const table = []()
		{
			IndividualOverrideTable t;
			t.colour = []()
			{
				IndividualOverrideDescriptor<AgentColour> d;
				d.unchangedMessage = "The individual Agent Colour is unchanged";
				d.read = [](Agent const& agent)
					-> std::optional<AgentColour> const&
					{ return agent.getIndividualColour(); };
				d.apply = [](World&, AgentId, Agent& agent,
					std::optional<AgentColour> const& value)
					{ agent.setIndividualColour(value); };
				return d;
			}();
			t.escalatorWalkingChance = []()
			{
				IndividualOverrideDescriptor<float> d;
				d.unchangedMessage =
					"The individual Escalator walking chance is unchanged";
				d.validate = [](float const& value, std::string* diagnostic)
				{ return agentEscalatorWalkingChanceIsValid(value, diagnostic); };
				d.read = [](Agent const& agent) -> std::optional<float> const&
				{ return agent.getIndividualEscalatorWalkingChance(); };
				d.apply = [](World&, AgentId, Agent& agent,
					std::optional<float> const& value)
					{ agent.setIndividualEscalatorWalkingChance(value); };
				return d;
			}();
			t.walkSpeedModifier = plainFloatOverride(
				"The individual Walk speed modifier is unchanged",
				agentWalkSpeedModifierRangeIsValid,
				&Agent::getIndividualWalkSpeedModifier,
				&Agent::setIndividualWalkSpeedModifier);
			t.heightModifier = []()
			{
				IndividualOverrideDescriptor<float> d;
				d.unchangedMessage =
					"The individual Height modifier is unchanged";
				d.validate = [](float const& value, std::string* diagnostic)
				{
					return agentHeightModifierRangeIsValid(
						{ value, value }, diagnostic);
				};
				d.read = [](Agent const& agent) -> std::optional<float> const&
				{ return agent.getIndividualHeightModifier(); };
				// Clearing the override reveals the sampled (or default
				// 1.0) inherited value: the Agent's live pose must fit the
				// next effective height before the write, exactly as the
				// replaced method checked.
				d.preflight = [](World& world, Agent const& agent,
					std::optional<float> const& value, std::string* diagnostic)
				{
					auto const next = value.value_or(agent.getHeightModifierSample()
						? agent.getHeightModifierSample()->value : 1.0f);
					return world.agentHeightStateFits(agent, next, diagnostic);
				};
				d.apply = [](World&, AgentId, Agent& agent,
					std::optional<float> const& value)
					{ agent.setIndividualHeightModifier(value); };
				return d;
			}();
			t.stairSpeedModifier = plainFloatOverride(
				"The individual Stair speed modifier is unchanged",
				agentStairSpeedModifierRangeIsValid,
				&Agent::getIndividualStairSpeedModifier,
				&Agent::setIndividualStairSpeedModifier);
			t.ladderSpeedModifier = plainFloatOverride(
				"The individual Ladder speed modifier is unchanged",
				agentLadderSpeedModifierRangeIsValid,
				&Agent::getIndividualLadderSpeedModifier,
				&Agent::setIndividualLadderSpeedModifier);
			t.interactionAversion = plainFloatOverride(
				"The individual Interaction aversion is unchanged",
				agentInteractionAversionRangeIsValid,
				&Agent::getIndividualInteractionAversion,
				&Agent::setIndividualInteractionAversion);
			t.effortAversion = plainFloatOverride(
				"The individual Effort aversion is unchanged",
				agentEffortAversionRangeIsValid,
				&Agent::getIndividualEffortAversion,
				&Agent::setIndividualEffortAversion);
			t.waitingAversion = plainFloatOverride(
				"The individual Waiting aversion is unchanged",
				agentWaitingAversionRangeIsValid,
				&Agent::getIndividualWaitingAversion,
				&Agent::setIndividualWaitingAversion);
			t.crowdAversion = plainFloatOverride(
				"The individual Crowd aversion is unchanged",
				agentCrowdAversionRangeIsValid,
				&Agent::getIndividualCrowdAversion,
				&Agent::setIndividualCrowdAversion);
			t.riskAversion = plainFloatOverride(
				"The individual Risk aversion is unchanged",
				agentRiskAversionRangeIsValid,
				&Agent::getIndividualRiskAversion,
				&Agent::setIndividualRiskAversion);
			t.routeFamiliarity = plainFloatOverride(
				"The individual Route familiarity is unchanged",
				agentRouteFamiliarityRangeIsValid,
				&Agent::getIndividualRouteFamiliarity,
				&Agent::setIndividualRouteFamiliarity);
			t.routePersistence = plainFloatOverride(
				"The individual Route persistence is unchanged",
				agentRoutePersistenceRangeIsValid,
				&Agent::getIndividualRoutePersistence,
				&Agent::setIndividualRoutePersistence);
			t.minimumRoutePlanningTime = plainFloatOverride(
				"The individual Minimum route planning time is unchanged",
				agentMinimumRoutePlanningTimeRangeIsValid,
				&Agent::getIndividualMinimumRoutePlanningTime,
				&Agent::setIndividualMinimumRoutePlanningTime);
			t.maximumRoutePlanningTime = plainFloatOverride(
				"The individual Maximum route planning time is unchanged",
				agentMaximumRoutePlanningTimeRangeIsValid,
				&Agent::getIndividualMaximumRoutePlanningTime,
				&Agent::setIndividualMaximumRoutePlanningTime);
			t.permissionAdherence = []()
			{
				IndividualOverrideDescriptor<bool> d;
				d.unchangedMessage =
					"The individual Permission adherence is unchanged";
				d.read = [](Agent const& agent) -> std::optional<bool> const&
				{ return agent.getIndividualPermissionAdherence(); };
				// The effective adherence can flip even though only one
				// source changed; the replanning seam wraps the write
				// itself, as the replaced method did.
				d.apply = [](World& world, AgentId id, Agent& agent,
					std::optional<bool> const& value)
				{
					auto const before =
						agent.getEffectivePermissionAdherence().value;
					agent.setIndividualPermissionAdherence(value);
					auto const after =
						agent.getEffectivePermissionAdherence().value;
					if (before != after)
					{
						if (after) world.replanAgentAfterAuthorizationRefusal(id);
						else world.beginVoluntaryRoutePlanning(id);
					}
				};
				return d;
			}();
			t.remoteAccessPanels = []()
			{
				IndividualOverrideDescriptor<bool> d;
				d.unchangedMessage =
					"The individual Remote Access panels is unchanged";
				d.read = [](Agent const& agent) -> std::optional<bool> const&
				{ return agent.getIndividualRemoteAccessPanels(); };
				d.apply = [](World& world, AgentId id, Agent& agent,
					std::optional<bool> const& value)
				{
					agent.setIndividualRemoteAccessPanels(value);
					world.mSimulationCoordinator.agentObjectUsageChanged(id);
				};
				return d;
			}();
			t.remoteBoothWindowShutters = []()
			{
				IndividualOverrideDescriptor<bool> d;
				d.unchangedMessage =
					"The individual Remote BoothWindow shutters is unchanged";
				d.read = [](Agent const& agent) -> std::optional<bool> const&
				{ return agent.getIndividualRemoteBoothWindowShutters(); };
				d.apply = [](World& world, AgentId id, Agent& agent,
					std::optional<bool> const& value)
				{
					agent.setIndividualRemoteBoothWindowShutters(value);
					world.mSimulationCoordinator.agentObjectUsageChanged(id);
				};
				return d;
			}();
			t.mobilityProfile = []()
			{
				IndividualOverrideDescriptor<MobilityProfile> d;
				d.unchangedMessage =
					"The individual Mobility profile is unchanged";
				d.validate = [](MobilityProfile const& value,
					std::string* diagnostic)
				{
					if (!mobilityProfileIsValid(value))
					{
						if (diagnostic)
							*diagnostic = "The individual Mobility profile "
								"contains an invalid Mobility use";
						return false;
					}
					return true;
				};
				d.read = [](Agent const& agent)
					-> std::optional<MobilityProfile> const&
					{ return agent.getIndividualMobilityProfile(); };
				d.apply = [](World&, AgentId, Agent& agent,
					std::optional<MobilityProfile> const& value)
					{ agent.setIndividualMobilityProfile(value); };
				return d;
			}();
			return t;
		}();
		return table;
	}

	AgentProfileEditor::AgentProfileEditor(World& world)
		: mWorld(world)
	{
	}

	// The one individual-override setter path (ADR 0012 through ADR 0022):
	// every single-property setter forwards here with its table row.
	// Every refusal happens before any authored state changes, in the
	// order the replaced methods established: unknown Agent, invalid value,
	// running simulation, unchanged value, then the property's own
	// preflight; only then is the snapshot invalidated, the write and its
	// side effects applied, and the World dirtied.
	template <typename T>
	bool AgentProfileEditor::setOverride(
		IndividualOverrideDescriptor<T> const& descriptor,
		AgentId id, optional<T> value, string* diagnostic)
	{
		auto lookup = mWorld.lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && descriptor.validate && !descriptor.validate(*value, diagnostic))
			return false;
		if (!mWorld.mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		auto* agent = lookup.entity;
		if (descriptor.read(*agent) == value)
		{
			if (diagnostic) *diagnostic = descriptor.unchangedMessage;
			return false;
		}
		if (descriptor.preflight
			&& !descriptor.preflight(mWorld, *agent, value, diagnostic)) return false;
		mWorld.invalidateSimulationSnapshot();
		descriptor.apply(mWorld, id, *agent, value);
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentProfileEditor::setColour(AgentId agent, optional<AgentColour> value,
		string* diagnostic)
	{
		return setOverride(overrideTable().colour, agent, value, diagnostic);
	}

	bool AgentProfileEditor::setEscalatorWalkingChance(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().escalatorWalkingChance, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setWalkSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().walkSpeedModifier, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setHeightModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().heightModifier, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setStairSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().stairSpeedModifier, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setLadderSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().ladderSpeedModifier, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setInteractionAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().interactionAversion, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setEffortAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().effortAversion, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setWaitingAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().waitingAversion, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setCrowdAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().crowdAversion, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setRiskAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().riskAversion, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setRouteFamiliarity(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().routeFamiliarity, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setRoutePersistence(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().routePersistence, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setMinimumRoutePlanningTime(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().minimumRoutePlanningTime, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setMaximumRoutePlanningTime(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return setOverride(overrideTable().maximumRoutePlanningTime, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setPermissionAdherence(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return setOverride(overrideTable().permissionAdherence, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setRemoteAccessPanels(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return setOverride(overrideTable().remoteAccessPanels, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setRemoteBoothWindowShutters(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return setOverride(overrideTable().remoteBoothWindowShutters, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setMobilityProfile(AgentId agent,
		optional<MobilityProfile> value, string* diagnostic)
	{
		return setOverride(overrideTable().mobilityProfile, agent,
			value, diagnostic);
	}

	bool AgentProfileEditor::setObjectUsage(AgentId agent,
		optional<ObjectUsage> value, string* diagnostic)
	{
		auto lookup = mWorld.lookupAgent(agent);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		return setObjectUsageOverrides(agent, value,
			lookup.entity->getIndividualObjectUsageDistance(), diagnostic);
	}

	bool AgentProfileEditor::setObjectUsageDistance(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		auto lookup = mWorld.lookupAgent(agent);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		return setObjectUsageOverrides(agent,
			lookup.entity->getIndividualObjectUsage(), value, diagnostic);
	}

	bool AgentProfileEditor::setObjectUsageOverrides(AgentId agent,
		optional<ObjectUsage> mode, optional<float> distance, string* diagnostic)
	{
		auto lookup = mWorld.lookupAgent(agent);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (!mWorld.mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		auto* entity = lookup.entity;
		if (!entity->objectUsageOverridesAreValid(mode, distance))
		{
			if (diagnostic) *diagnostic = "Object usage must be Arms, None or Remote control; usable distance must be finite and positive";
			return false;
		}
		if (entity->getIndividualObjectUsage() == mode
			&& entity->getIndividualObjectUsageDistance() == distance)
		{
			if (diagnostic) *diagnostic = "The individual Object usage properties are unchanged";
			return false;
		}
		auto const beforeMode = entity->getObjectUsage();
		auto const beforeDistance = entity->getObjectUsageDistance();
		entity->setIndividualObjectUsage(mode);
		entity->setIndividualObjectUsageDistance(distance);
		mWorld.agentObjectUsageChanged(agent, beforeMode, beforeDistance);
		mWorld.invalidateSimulationSnapshot();
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
}
