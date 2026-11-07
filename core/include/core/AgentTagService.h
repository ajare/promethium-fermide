#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/AgentPropertyDescriptors.h"
#include "core/AgentTag.h"

namespace core
{
	class AgentTagRegistry;
	class World;

	// What the open-time tag inspection plans for one sampled property:
	// leave it, drop the orphaned sample, or resample it from the current
	// definition. Moved out of World with the tag service (#682); World
	// keeps a private alias so its historical qualified names still
	// resolve.
	enum class AgentTagSampleRepairAction
	{
		None,
		Clear,
		Resample
	};

	// One Agent's planned tag-reconciliation transaction: the before-state
	// of the coupled object-usage pair plus the per-sampled-property repair
	// plans. A caller inspects a complete prospective registry, then
	// applies every plan as one non-refusing commit.
	struct AgentTagReconciliation
	{
		AgentId agent{};
		ObjectUsage objectUsageBefore{ ObjectUsage::Arms };
		float objectUsageDistanceBefore{ 0.f };
		bool objectUsageChanged{ false };
		AgentTagSampleRepairAction walkSpeedAction{ AgentTagSampleRepairAction::None };
		AgentTagId walkSpeedSource{};
		AgentWalkSpeedModifierProperty walkSpeedProperty{};
		AgentTagSampleRepairAction heightAction{ AgentTagSampleRepairAction::None };
		AgentTagId heightSource{};
		AgentHeightModifierProperty heightProperty{};
		AgentTagSampleRepairAction stairSpeedAction{ AgentTagSampleRepairAction::None };
		AgentTagId stairSpeedSource{};
		AgentStairSpeedModifierProperty stairSpeedProperty{};
		AgentTagSampleRepairAction ladderSpeedAction{ AgentTagSampleRepairAction::None };
		AgentTagId ladderSpeedSource{};
		AgentLadderSpeedModifierProperty ladderSpeedProperty{};
		AgentTagSampleRepairAction interactionAversionAction{ AgentTagSampleRepairAction::None };
		AgentTagId interactionAversionSource{};
		AgentInteractionAversionProperty interactionAversionProperty{};
		AgentTagSampleRepairAction effortAversionAction{ AgentTagSampleRepairAction::None };
		AgentTagId effortAversionSource{};
		AgentEffortAversionProperty effortAversionProperty{};
		AgentTagSampleRepairAction waitingAversionAction{ AgentTagSampleRepairAction::None };
		AgentTagId waitingAversionSource{};
		AgentWaitingAversionProperty waitingAversionProperty{};
		AgentTagSampleRepairAction crowdAversionAction{ AgentTagSampleRepairAction::None };
		AgentTagId crowdAversionSource{};
		AgentCrowdAversionProperty crowdAversionProperty{};
		AgentTagSampleRepairAction riskAversionAction{ AgentTagSampleRepairAction::None };
		AgentTagId riskAversionSource{};
		AgentRiskAversionProperty riskAversionProperty{};
		AgentTagSampleRepairAction routeFamiliarityAction{ AgentTagSampleRepairAction::None };
		AgentTagId routeFamiliaritySource{};
		AgentRouteFamiliarityProperty routeFamiliarityProperty{};
		AgentTagSampleRepairAction routePersistenceAction{ AgentTagSampleRepairAction::None };
		AgentTagId routePersistenceSource{};
		AgentRoutePersistenceProperty routePersistenceProperty{};
		AgentTagSampleRepairAction minimumRoutePlanningTimeAction{ AgentTagSampleRepairAction::None };
		AgentTagId minimumRoutePlanningTimeSource{};
		AgentMinimumRoutePlanningTimeProperty minimumRoutePlanningTimeProperty{};
		AgentTagSampleRepairAction maximumRoutePlanningTimeAction{ AgentTagSampleRepairAction::None };
		AgentTagId maximumRoutePlanningTimeSource{};
		AgentMaximumRoutePlanningTimeProperty maximumRoutePlanningTimeProperty{};
	};

	// The canonical order of the thirteen sampled Agent properties. It is
	// the order of the #677 tag-sample table rows, so a sample set indexed
	// here lines up row-for-row with World::tagSampleTable().
	inline constexpr std::size_t SampledAgentPropertyCount = 13;

	// One Agent's thirteen sampled-property values, in the canonical order.
	using AgentTagSampleSet =
		std::array<std::optional<AgentPropertySample>, SampledAgentPropertyCount>;

	// Owns the Agent tag assignment behaviour family on behalf of the World
	// that owns it (ADR 0022, #682).
	//
	// This collaborator moves the whole per-Agent tag behaviour out of
	// World: the assign/remove transactions with their inheritance-conflict
	// gates (canAssign/canRemove), the tag-wide clearing family
	// (countAssignments, clearAssignments, allAssignmentsCanBeCleared,
	// clearAllAssignmentsAndSamples), the height-modifier application
	// gate, the paste/restore validation pair (validateAssignments,
	// restoreAssignments), and the open-time reconciliation family
	// (inspectAssignments, applyReconciliations, reconcileAssignments)
	// that repairs samples when a registry's definitions change.
	//
	// It is built on the two ADR 0022 de-duplication templates: the tag
	// registry attach/detach state machine stays on the #676
	// RegistryBinding<AgentTagRegistry> in World, and every per-property
	// sample step here is driven by the #677 property-descriptor table -
	// one row per sampled property carries the definition-side reads, the
	// Agent sample accessors, and the reconciliation-field bindings, so
	// the thirteen-way repetition the replaced methods carried by hand
	// collapses into one row loop per step.
	//
	// The state itself - the Agent entities carrying tag IDs and samples,
	// the tag registry pointer and reference - stays with the World
	// (ADR 0001). WorldSerialization.cpp and construction-record replay
	// are untouched: behaviour moved out, state did not. The service owns
	// no state: it reaches the World's Agent registry, the pause gate,
	// the snapshot invalidation, the usage/replanning side effects, and
	// the tag-sample table through friend access, following the
	// SimulationCoordinator, PlacementValidator, AgentGroupRegistry,
	// PermissionRequirementStore, AgentTypeCatalog, MarkerStore,
	// TraversalResourceRegistry, DeviceConditionStore, and FurnitureStore
	// pattern.
	//
	// World keeps ownership of every entity registry (ADR 0001) and
	// remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class.
	class AgentTagService
	{
	public:
		explicit AgentTagService(World& world);

		AgentTagService(AgentTagService const&) = delete;
		AgentTagService& operator=(AgentTagService const&) = delete;

		virtual ~AgentTagService() = default;

		// ------------------------------------------------------------------
		// Per-Agent assignment
		// ------------------------------------------------------------------

		// The Agent's assigned tag IDs; throws if the Agent is unknown.
		std::set<AgentTagId> const& tags(AgentId agent) const;

		// The assign/remove gate: paused-only, the tag must be defined in
		// the attached registry, and the assignment/removal must not create
		// a duplicate inheritance source or break the object-usage rule.
		bool canAssign(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr) const;
		bool assign(AgentId agent, AgentTagId tag, std::string* diagnostic);
		bool canRemove(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr) const;
		bool remove(AgentId agent, AgentTagId tag, std::string* diagnostic);

		// ------------------------------------------------------------------
		// Tag-wide clearing
		// ------------------------------------------------------------------

		uint32_t countAssignments(AgentTagId id) const;

		// Removes the tag from every Agent that carries it, dropping only
		// the samples that tag sourced. Throws if any affected Agent's
		// height state would not fit the removal.
		void clearAssignments(AgentTagId id);

		bool allAssignmentsCanBeCleared(std::string* diagnostic) const;
		void clearAllAssignmentsAndSamples();

		// Whether every Agent whose effective Height comes from this tag
		// (or samples it) still fits its pose/clearance state if the tag's
		// height contribution were applied/removed at the given range.
		bool canApplyHeightModifier(AgentTagId id,
			std::optional<AgentModifierRange> range,
			std::string* diagnostic = nullptr) const;

		// ------------------------------------------------------------------
		// Paste validation and restoration
		// ------------------------------------------------------------------

		// Validates a complete tag/sample set against the attached
		// registry: no duplicate inheritance sources, and every sample
		// present, current, and in range for the property its tags inherit.
		bool validateAssignments(std::set<AgentTagId> const& tags,
			AgentTagSampleSet const& samples,
			std::string* diagnostic = nullptr) const;

		// Restores a validated tag/sample set onto an Agent (clipboard
		// paste and undo-style restoration); an identical set is a no-op.
		bool restoreAssignments(AgentId agent, std::set<AgentTagId> const& tags,
			AgentTagSampleSet const& samples, std::string* diagnostic);

		// ------------------------------------------------------------------
		// Open-time inspection and reconciliation
		// ------------------------------------------------------------------

		// Checks every assigned tag and inherited property against a
		// prospective registry. When allowSampleReconciliation is set,
		// sample mismatches are planned as repairs instead of refused.
		bool inspectAssignments(AgentTagRegistry const& registry,
			bool allowSampleReconciliation,
			std::vector<AgentTagReconciliation>* repairs,
			std::string* diagnostic = nullptr) const;
		bool assignmentsAreValid(AgentTagRegistry const& registry,
			std::string* diagnostic = nullptr) const;

		// Commits precomputed repairs; sampling and application cannot
		// refuse, so all repairs commit together.
		void applyReconciliations(
			std::vector<AgentTagReconciliation> const& repairs);
		void reconcileAssignments(AgentTagRegistry const& registry);

	private:
		// One row of the service's sampled-property table (ADR 0022,
		// #682): the #677 tag-sample descriptor for the Agent sample side,
		// the definition-side reads through the tag's own property getters,
		// and the reconciliation-field bindings shared by the inspect/apply
		// pair. The rows borrow World's tag-sample table and own no state.
		struct SampledPropertyRow
		{
			// Display name used in every diagnostic, e.g. "Walk speed
			// modifier".
			char const* name;
			SampledAgentPropertyType type;
			// Reads the definition's property range; nullptr when the
			// definition does not carry the property.
			AgentModifierRange const* (*definitionRange)(AgentTag const& definition);
			uint64_t (*definitionRevision)(AgentTag const& definition);
			// The Agent sample accessors, borrowed from World's #677 table.
			TagSampleDescriptor const* sample;
			// The reconciliation fields this row owns.
			AgentTagSampleRepairAction AgentTagReconciliation::* action;
			AgentTagId AgentTagReconciliation::* source;
			// Copies the definition's property into the repair's typed
			// property field for a resample.
			void (*captureRepair)(AgentTagReconciliation& repair,
				AgentTag const& definition);
			AgentModifierRange const& (*repairRange)(
				AgentTagReconciliation const& repair);
			uint64_t (*repairRevision)(
				AgentTagReconciliation const& repair);
		};

		struct SampledPropertyTable
		{
			std::array<SampledPropertyRow, SampledAgentPropertyCount> rows;
		};

		// Built once on first use from World's tag-sample table.
		static SampledPropertyTable const& sampledRows();

		// Drops every sample on the Agent that the given tag sourced.
		static void clearSamplesSourcedBy(Agent& agent, AgentTagId tag);

		World& mWorld;
	};
}
