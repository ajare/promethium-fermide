#pragma once

#include <optional>
#include <string>

#include "core/Agent.h"
#include "core/AgentPropertyDescriptors.h"
#include "core/AgentTag.h"

namespace core
{
	class World;

	// Owns the per-Agent individual-profile edit family on behalf of the
	// World that owns it (ADR 0022, #683).
	//
	// This collaborator moves the whole setAgentIndividual* behaviour out
	// of World: the nineteen single-property override setters driven by
	// the #677 descriptor table, and the coupled object-usage pair with
	// its shared validity rule and joint replanning side effect.
	//
	// It is built on the #677 de-duplication template: one row per
	// property carries the validator, the unchanged-check read, the
	// optional preflight (the Height modifier's pose/clearance fit), and
	// the write with its side effects (the Permission-adherence replanning
	// wrap, the remote object-control coordinator notification), so
	// ADR 0012's "individual Agent properties override tag properties"
	// precedence stays one code path. Object usage and its distance are
	// deliberately not table rows: they keep their own composite path.
	//
	// The state itself - the Agent entities carrying the individual
	// override records - stays with the World (ADR 0001).
	// WorldSerialization.cpp and construction-record replay are untouched:
	// behaviour moved out, state did not. The editor owns no state: it
	// reaches the World's Agent registry, the pause gate, the snapshot
	// invalidation, the usage/replanning side effects, and the height-fit
	// check through friend access, following the SimulationCoordinator,
	// PlacementValidator, AgentGroupRegistry, PermissionRequirementStore,
	// AgentTypeCatalog, MarkerStore, TraversalResourceRegistry,
	// DeviceConditionStore, FurnitureStore, and AgentTagService pattern.
	//
	// World keeps ownership of every entity registry (ADR 0001) and
	// remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class.
	class AgentProfileEditor
	{
	public:
		explicit AgentProfileEditor(World& world);

		AgentProfileEditor(AgentProfileEditor const&) = delete;
		AgentProfileEditor& operator=(AgentProfileEditor const&) = delete;

		virtual ~AgentProfileEditor() = default;

		// ------------------------------------------------------------------
		// Descriptor-driven single-property overrides
		// ------------------------------------------------------------------

		// Each setter refuses, in order: an unknown Agent, an invalid
		// value, a running simulation, an unchanged value, then the
		// property's own preflight; only then is the snapshot invalidated,
		// the write and its side effects applied, and the World dirtied.
		// Every refusal changes nothing.

		bool setColour(AgentId agent, std::optional<AgentColour> value,
			std::string* diagnostic = nullptr);
		bool setEscalatorWalkingChance(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setWalkSpeedModifier(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setHeightModifier(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setStairSpeedModifier(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setLadderSpeedModifier(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setInteractionAversion(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setEffortAversion(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setWaitingAversion(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setCrowdAversion(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setRiskAversion(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setRouteFamiliarity(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setRoutePersistence(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setMinimumRoutePlanningTime(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setMaximumRoutePlanningTime(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);
		bool setPermissionAdherence(AgentId agent, std::optional<bool> value,
			std::string* diagnostic = nullptr);
		bool setRemoteAccessPanels(AgentId agent, std::optional<bool> value,
			std::string* diagnostic = nullptr);
		bool setRemoteBoothWindowShutters(AgentId agent, std::optional<bool> value,
			std::string* diagnostic = nullptr);
		bool setMobilityProfile(AgentId agent, std::optional<MobilityProfile> value,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// The coupled object-usage pair
		// ------------------------------------------------------------------

		// The two single-field setters read the Agent's other override so
		// the pair's shared validity rule is always judged on the full
		// prospective state, then commit through the composite path below.
		bool setObjectUsage(AgentId agent, std::optional<ObjectUsage> value,
			std::string* diagnostic = nullptr);
		bool setObjectUsageDistance(AgentId agent, std::optional<float> value,
			std::string* diagnostic = nullptr);

		// Sets both overrides at once: refused on an unknown Agent, a
		// running simulation, an invalid pair (usage must be Arms, None
		// or Remote control; distance finite and positive), or an
		// unchanged pair. On success the joint replanning side effect
		// fires through the World's usage-change seam.
		bool setObjectUsageOverrides(AgentId agent, std::optional<ObjectUsage> mode,
			std::optional<float> distance, std::string* diagnostic = nullptr);

	private:
		// The one override table (ADR 0022, #677): built once on first
		// use inside this class's member scope, so the special rows'
		// closures reach World-owned state - the height-fit preflight,
		// the adherence replanning seam, and the simulation coordinator
		// behind the remote object-control flags - through this scope's
		// friend access (the ADR 0022 access seam). The table owns no
		// state.
		static IndividualOverrideTable const& overrideTable();

		// The one individual-override setter path (ADR 0012 through
		// ADR 0022): every single-property setter forwards here with its
		// table row.
		template <typename T>
		bool setOverride(
			IndividualOverrideDescriptor<T> const& descriptor,
			AgentId id, std::optional<T> value, std::string* diagnostic);

		World& mWorld;
	};
}
