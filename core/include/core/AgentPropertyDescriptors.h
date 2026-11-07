#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/Agent.h"
#include "core/AgentTag.h"

namespace core
{
	class World;

	// One row of the individual-override descriptor table (ADR 0022, #677).
	//
	// The World::setAgentIndividual* facade family differed only by property
	// type, validator, and side effects. A row here names exactly those
	// differences so one shared setter path in World drives them all. The
	// shared path keeps the ordering contract of the methods it replaces:
	// lookup, value validation, the pause gate, the unchanged check, the
	// optional preflight, snapshot invalidation, the write with its side
	// effects, then dirtying the World - which is what makes ADR 0012's
	// "individual Agent properties override tag properties" rule a single
	// code path rather than dozens.
	//
	// The descriptor owns no state. Rows are built once inside World's own
	// member scope because their closures reach World-owned state through
	// that scope's access (the ADR 0022 access seam).
	template <typename T>
	struct IndividualOverrideDescriptor
	{
		// Diagnostic reported when the new value equals the current one,
		// e.g. "The individual Walk speed modifier is unchanged".
		std::string unchangedMessage;
		// Optional value validation; a refusal changes nothing.
		std::function<bool(T const& value, std::string* diagnostic)> validate;
		// Reads the current individual override for the unchanged check.
		std::function<std::optional<T> const&(Agent const& agent)> read;
		// Optional extra refusal after the unchanged check and before the
		// snapshot invalidation (the Height modifier's pose/clearance fit).
		std::function<bool(World& world, Agent const& agent,
			std::optional<T> const& value, std::string* diagnostic)> preflight;
		// Performs the write together with the property's side effects.
		// The shared path has already invalidated the simulation snapshot.
		std::function<void(World& world, AgentId id, Agent& agent,
			std::optional<T> const& value)> apply;
	};

	// One row of the tag-sample descriptor table (ADR 0022, #677).
	//
	// The World::addAgentTag*Samples / World::clearAgentTag*Samples facade
	// family differed only by the sampled property type and the Agent
	// accessors it drove. The shared add/clear path in World visits each
	// assigned Agent once and applies the row, so tag-supplied samples and
	// individual overrides stay two halves of one ADR 0012 mechanism.
	struct TagSampleDescriptor
	{
		SampledAgentPropertyType type{ SampledAgentPropertyType::WalkSpeedModifier };
		std::function<std::optional<AgentPropertySample> const&(Agent const& agent)>
			readSample;
		std::function<void(Agent& agent, AgentPropertySample const& sample)>
			writeSample;
		std::function<void(Agent& agent)> clearSample;
	};

	// The full tables: one row per collapsed facade method. Object usage and
	// its distance are deliberately absent - they are a coupled pair with a
	// shared validity rule and a joint replanning side effect, so they keep
	// their own single composite path (World::setAgentObjectUsageOverrides)
	// rather than pretending to be two independent single-property rows.
	struct IndividualOverrideTable
	{
		IndividualOverrideDescriptor<AgentColour> colour;
		IndividualOverrideDescriptor<float> escalatorWalkingChance;
		IndividualOverrideDescriptor<float> walkSpeedModifier;
		IndividualOverrideDescriptor<float> heightModifier;
		IndividualOverrideDescriptor<float> stairSpeedModifier;
		IndividualOverrideDescriptor<float> ladderSpeedModifier;
		IndividualOverrideDescriptor<float> interactionAversion;
		IndividualOverrideDescriptor<float> effortAversion;
		IndividualOverrideDescriptor<float> waitingAversion;
		IndividualOverrideDescriptor<float> crowdAversion;
		IndividualOverrideDescriptor<float> riskAversion;
		IndividualOverrideDescriptor<float> routeFamiliarity;
		IndividualOverrideDescriptor<float> routePersistence;
		IndividualOverrideDescriptor<float> minimumRoutePlanningTime;
		IndividualOverrideDescriptor<float> maximumRoutePlanningTime;
		IndividualOverrideDescriptor<bool> permissionAdherence;
		IndividualOverrideDescriptor<bool> remoteAccessPanels;
		IndividualOverrideDescriptor<bool> remoteBoothWindowShutters;
		IndividualOverrideDescriptor<MobilityProfile> mobilityProfile;
	};

	struct TagSampleTable
	{
		TagSampleDescriptor walkSpeedModifier;
		TagSampleDescriptor heightModifier;
		TagSampleDescriptor stairSpeedModifier;
		TagSampleDescriptor ladderSpeedModifier;
		TagSampleDescriptor interactionAversion;
		TagSampleDescriptor effortAversion;
		TagSampleDescriptor waitingAversion;
		TagSampleDescriptor crowdAversion;
		TagSampleDescriptor riskAversion;
		TagSampleDescriptor routeFamiliarity;
		TagSampleDescriptor routePersistence;
		TagSampleDescriptor minimumRoutePlanningTime;
		TagSampleDescriptor maximumRoutePlanningTime;
	};
}
