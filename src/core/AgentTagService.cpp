#include "core/AgentTagService.h"

#include <format>
#include <stdexcept>
#include <utility>

#include "core/AgentPropertyDescriptors.h"
#include "core/AgentTagRegistry.h"
#include "core/Exceptions.h"
#include "core/SimulationCoordinator.h"
#include "core/World.h"

using namespace std;

namespace core
{
	namespace
	{
		// The Height modifier's slot in the canonical sampled-property
		// order: the one sampled property with pose/clearance consequences.
		constexpr size_t HeightModifierIndex = 1;
	}

	// The one sampled-property table (ADR 0022, #682): every per-property
	// step in this service - sample construction on assign, source-matched
	// clearing on remove, reconciliation capture and application, and the
	// paste/open-time sample validation - is one row loop over these rows.
	// Each row borrows its Agent sample accessors from World's #677
	// tag-sample table, reads the definition side through the tag's own
	// property getters, and binds the reconciliation fields the
	// inspect/apply pair shares. Built once on first use; owns no state.
	AgentTagService::SampledPropertyTable const& AgentTagService::sampledRows()
	{
		static SampledPropertyTable const table = []()
		{
			auto const& samples = World::tagSampleTable();
			SampledPropertyTable t;
			t.rows = { {
				{
					"Walk speed modifier",
					SampledAgentPropertyType::WalkSpeedModifier,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getWalkSpeedModifier();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getWalkSpeedModifier();
						return property ? property->revision : 0;
					},
					&samples.walkSpeedModifier,
					&AgentTagReconciliation::walkSpeedAction,
					&AgentTagReconciliation::walkSpeedSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.walkSpeedProperty = *definition.getWalkSpeedModifier(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.walkSpeedProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.walkSpeedProperty.revision; },
				},
				{
					"Height modifier",
					SampledAgentPropertyType::HeightModifier,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getHeightModifier();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getHeightModifier();
						return property ? property->revision : 0;
					},
					&samples.heightModifier,
					&AgentTagReconciliation::heightAction,
					&AgentTagReconciliation::heightSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.heightProperty = *definition.getHeightModifier(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.heightProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.heightProperty.revision; },
				},
				{
					"Stair speed modifier",
					SampledAgentPropertyType::StairSpeedModifier,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getStairSpeedModifier();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getStairSpeedModifier();
						return property ? property->revision : 0;
					},
					&samples.stairSpeedModifier,
					&AgentTagReconciliation::stairSpeedAction,
					&AgentTagReconciliation::stairSpeedSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.stairSpeedProperty = *definition.getStairSpeedModifier(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.stairSpeedProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.stairSpeedProperty.revision; },
				},
				{
					"Ladder speed modifier",
					SampledAgentPropertyType::LadderSpeedModifier,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getLadderSpeedModifier();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getLadderSpeedModifier();
						return property ? property->revision : 0;
					},
					&samples.ladderSpeedModifier,
					&AgentTagReconciliation::ladderSpeedAction,
					&AgentTagReconciliation::ladderSpeedSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.ladderSpeedProperty = *definition.getLadderSpeedModifier(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.ladderSpeedProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.ladderSpeedProperty.revision; },
				},
				{
					"Interaction aversion",
					SampledAgentPropertyType::InteractionAversion,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getInteractionAversion();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getInteractionAversion();
						return property ? property->revision : 0;
					},
					&samples.interactionAversion,
					&AgentTagReconciliation::interactionAversionAction,
					&AgentTagReconciliation::interactionAversionSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.interactionAversionProperty = *definition.getInteractionAversion(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.interactionAversionProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.interactionAversionProperty.revision; },
				},
				{
					"Effort aversion",
					SampledAgentPropertyType::EffortAversion,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getEffortAversion();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getEffortAversion();
						return property ? property->revision : 0;
					},
					&samples.effortAversion,
					&AgentTagReconciliation::effortAversionAction,
					&AgentTagReconciliation::effortAversionSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.effortAversionProperty = *definition.getEffortAversion(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.effortAversionProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.effortAversionProperty.revision; },
				},
				{
					"Waiting aversion",
					SampledAgentPropertyType::WaitingAversion,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getWaitingAversion();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getWaitingAversion();
						return property ? property->revision : 0;
					},
					&samples.waitingAversion,
					&AgentTagReconciliation::waitingAversionAction,
					&AgentTagReconciliation::waitingAversionSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.waitingAversionProperty = *definition.getWaitingAversion(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.waitingAversionProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.waitingAversionProperty.revision; },
				},
				{
					"Crowd aversion",
					SampledAgentPropertyType::CrowdAversion,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getCrowdAversion();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getCrowdAversion();
						return property ? property->revision : 0;
					},
					&samples.crowdAversion,
					&AgentTagReconciliation::crowdAversionAction,
					&AgentTagReconciliation::crowdAversionSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.crowdAversionProperty = *definition.getCrowdAversion(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.crowdAversionProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.crowdAversionProperty.revision; },
				},
				{
					"Risk aversion",
					SampledAgentPropertyType::RiskAversion,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getRiskAversion();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getRiskAversion();
						return property ? property->revision : 0;
					},
					&samples.riskAversion,
					&AgentTagReconciliation::riskAversionAction,
					&AgentTagReconciliation::riskAversionSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.riskAversionProperty = *definition.getRiskAversion(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.riskAversionProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.riskAversionProperty.revision; },
				},
				{
					"Route familiarity",
					SampledAgentPropertyType::RouteFamiliarity,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getRouteFamiliarity();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getRouteFamiliarity();
						return property ? property->revision : 0;
					},
					&samples.routeFamiliarity,
					&AgentTagReconciliation::routeFamiliarityAction,
					&AgentTagReconciliation::routeFamiliaritySource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.routeFamiliarityProperty = *definition.getRouteFamiliarity(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.routeFamiliarityProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.routeFamiliarityProperty.revision; },
				},
				{
					"Route persistence",
					SampledAgentPropertyType::RoutePersistence,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getRoutePersistence();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getRoutePersistence();
						return property ? property->revision : 0;
					},
					&samples.routePersistence,
					&AgentTagReconciliation::routePersistenceAction,
					&AgentTagReconciliation::routePersistenceSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.routePersistenceProperty = *definition.getRoutePersistence(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.routePersistenceProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.routePersistenceProperty.revision; },
				},
				{
					"Minimum route planning time",
					SampledAgentPropertyType::MinimumRoutePlanningTime,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getMinimumRoutePlanningTime();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getMinimumRoutePlanningTime();
						return property ? property->revision : 0;
					},
					&samples.minimumRoutePlanningTime,
					&AgentTagReconciliation::minimumRoutePlanningTimeAction,
					&AgentTagReconciliation::minimumRoutePlanningTimeSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.minimumRoutePlanningTimeProperty = *definition.getMinimumRoutePlanningTime(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.minimumRoutePlanningTimeProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.minimumRoutePlanningTimeProperty.revision; },
				},
				{
					"Maximum route planning time",
					SampledAgentPropertyType::MaximumRoutePlanningTime,
					[](AgentTag const& definition) -> AgentModifierRange const*
					{
						auto const* property = definition.getMaximumRoutePlanningTime();
						return property ? &property->range : nullptr;
					},
					[](AgentTag const& definition) -> uint64_t
					{
						auto const* property = definition.getMaximumRoutePlanningTime();
						return property ? property->revision : 0;
					},
					&samples.maximumRoutePlanningTime,
					&AgentTagReconciliation::maximumRoutePlanningTimeAction,
					&AgentTagReconciliation::maximumRoutePlanningTimeSource,
					[](AgentTagReconciliation& repair,
						AgentTag const& definition)
					{ repair.maximumRoutePlanningTimeProperty = *definition.getMaximumRoutePlanningTime(); },
					[](AgentTagReconciliation const& repair)
						-> AgentModifierRange const&
					{ return repair.maximumRoutePlanningTimeProperty.range; },
					[](AgentTagReconciliation const& repair) -> uint64_t
					{ return repair.maximumRoutePlanningTimeProperty.revision; },
				},
			} };
			return t;
		}();
		return table;
	}

	AgentTagService::AgentTagService(World& world)
		: mWorld(world)
	{
	}

	// Drops every sample on this Agent that the given tag sourced, through
	// the descriptor rows - the shared tail of the remove and tag-wide
	// clear transactions.
	void AgentTagService::clearSamplesSourcedBy(Agent& agent, AgentTagId tag)
	{
		for (auto const& row : sampledRows().rows)
		{
			auto const& sample = row.sample->readSample(agent);
			if (sample && sample->sourceTag == tag) row.sample->clearSample(agent);
		}
	}

	set<AgentTagId> const& AgentTagService::tags(AgentId agent) const
	{
		auto const lookup = mWorld.lookupAgent(agent);
		if (!lookup) throw WorldException(&mWorld, lookup.diagnostic);
		return lookup.entity->getAgentTagIds();
	}

	uint32_t AgentTagService::countAssignments(AgentTagId id) const
	{
		uint32_t count{ 0 };
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (agent && agent->hasAgentTag(id)) ++count;
		}
		return count;
	}

	bool AgentTagService::canAssign(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		auto const agentLookup = mWorld.lookupAgent(agent);
		if (!agentLookup) return reject(agentLookup.diagnostic);
		if (!mWorld.mSimulationPaused)
			return reject("Pause the simulation before assigning an Agent tag");
		if (!mWorld.mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");
		if (!tag || !mWorld.mAgentTagRegistry->lookupAgentTag(tag))
			return reject(format("Agent tag {} is not defined in the attached registry", tag.value));
		if (agentLookup.entity->hasAgentTag(tag))
			return reject(format("Agent '{}' is already assigned to Agent tag #{}",
				agentLookup.entity->getName(), mWorld.mAgentTagRegistry->getAgentTagName(tag)));

		auto const* assignedDefinition = mWorld.mAgentTagRegistry->lookupAgentTag(tag);
		if (auto height = assignedDefinition->getHeightModifier(); height
			&& !mWorld.agentHeightStateFits(*agentLookup.entity, agentLookup.entity->getIndividualHeightModifier()
				.value_or(height->range.maximum), diagnostic)) return false;
		for (auto const existing : agentLookup.entity->getAgentTagIds())
		{
			auto const* source = mWorld.mAgentTagRegistry->lookupAgentTag(existing);
			if (!source) continue;
			if (assignedDefinition->getColour() && source->getColour())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Colour is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(),
					source->getName()));
			}
			if (assignedDefinition->getEscalatorWalkingChance() && source->getEscalatorWalkingChance())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Escalator walking chance is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(),
					source->getName()));
			}
			for (auto const& row : sampledRows().rows)
			{
				if (row.definitionRange(*assignedDefinition)
					&& row.definitionRange(*source))
				{
					return reject(format(
						"Agent '{}' cannot be assigned to #{} because {} is already inherited from #{}",
						agentLookup.entity->getName(), assignedDefinition->getName(),
						row.name, source->getName()));
				}
			}
			if (assignedDefinition->getPermissionAdherence() && source->getPermissionAdherence())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Permission adherence is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getRemoteAccessPanels() && source->getRemoteAccessPanels())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Remote Access panels is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getRemoteBoothWindowShutters() && source->getRemoteBoothWindowShutters())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Remote BoothWindow shutters is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getMobilityProfile() && source->getMobilityProfile())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Mobility profile is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(),
					source->getName()));
			}
		}
		auto prospectiveTags = agentLookup.entity->getAgentTagIds();
		prospectiveTags.insert(tag);
		return agentLookup.entity->objectUsageConfigurationIsValid(mWorld.mAgentTagRegistry.get(), prospectiveTags,
			agentLookup.entity->getIndividualObjectUsage(), agentLookup.entity->getIndividualObjectUsageDistance(), diagnostic);
	}

	bool AgentTagService::assign(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!canAssign(agent, tag, diagnostic)) return false;
		auto* target = mWorld.mAgents.find(agent);
		auto const* definition = mWorld.mAgentTagRegistry->lookupAgentTag(tag);
		// Build every prospective sample first, in row order: the
		// height-fit gate reads the sampled value before any write
		// happens, so the whole transaction still commits or refuses as
		// one step.
		AgentTagSampleSet samples;
		for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
		{
			auto const& row = sampledRows().rows[i];
			if (auto const* range = row.definitionRange(*definition))
				samples[i] = AgentPropertySample{ row.type, tag,
					row.definitionRevision(*definition), sampleAgentModifier(*range) };
		}
		if (auto const& heightSample = samples[HeightModifierIndex]; heightSample
			&& !mWorld.agentHeightStateFits(*target,
				target->getIndividualHeightModifier().value_or(heightSample->value), diagnostic)) return false;
		auto const adherenceBefore = target->getEffectivePermissionAdherence().value;
		auto const usageBefore = target->getObjectUsage();
		auto const distanceBefore = target->getObjectUsageDistance();
		target->assignAgentTag(tag);
		mWorld.agentObjectUsageChanged(agent, usageBefore, distanceBefore);
		for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
			if (samples[i]) sampledRows().rows[i].sample->writeSample(*target, *samples[i]);
		auto const adherenceAfter = target->getEffectivePermissionAdherence().value;
		if (adherenceBefore != adherenceAfter)
		{
			if (adherenceAfter) mWorld.replanAgentAfterAuthorizationRefusal(agent);
			else mWorld.beginVoluntaryRoutePlanning(agent);
		}
		mWorld.modify();
		return true;
	}

	bool AgentTagService::canRemove(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		auto const agentLookup = mWorld.lookupAgent(agent);
		if (!agentLookup) return reject(agentLookup.diagnostic);
		if (!mWorld.mSimulationPaused)
			return reject("Pause the simulation before removing an Agent tag");
		if (!mWorld.mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");
		if (!tag || !mWorld.mAgentTagRegistry->lookupAgentTag(tag))
			return reject(format("Agent tag {} is not defined in the attached registry", tag.value));
		if (!agentLookup.entity->hasAgentTag(tag))
			return reject(format("Agent '{}' is not assigned to Agent tag #{}",
				agentLookup.entity->getName(), mWorld.mAgentTagRegistry->getAgentTagName(tag)));
		auto const* target = agentLookup.entity;
		if (target->getHeightModifierSample() && target->getHeightModifierSample()->sourceTag == tag
			&& !mWorld.agentHeightStateFits(*target, target->getIndividualHeightModifier().value_or(1.0f), diagnostic)) return false;
		auto prospectiveTags = target->getAgentTagIds();
		prospectiveTags.erase(tag);
		return target->objectUsageConfigurationIsValid(mWorld.mAgentTagRegistry.get(), prospectiveTags,
			target->getIndividualObjectUsage(), target->getIndividualObjectUsageDistance(), diagnostic);
	}

	bool AgentTagService::remove(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!canRemove(agent, tag, diagnostic)) return false;
		auto* target = mWorld.mAgents.find(agent);
		auto const adherenceBefore = target->getEffectivePermissionAdherence().value;
		auto const usageBefore = target->getObjectUsage();
		auto const distanceBefore = target->getObjectUsageDistance();
		target->removeAgentTag(tag);
		mWorld.agentObjectUsageChanged(agent, usageBefore, distanceBefore);
		clearSamplesSourcedBy(*target, tag);
		auto const adherenceAfter = target->getEffectivePermissionAdherence().value;
		if (adherenceBefore != adherenceAfter)
		{
			if (adherenceAfter) mWorld.replanAgentAfterAuthorizationRefusal(agent);
			else mWorld.beginVoluntaryRoutePlanning(agent);
		}
		mWorld.modify();
		return true;
	}

	void AgentTagService::clearAssignments(AgentTagId id)
	{
		std::string diagnostic;
		if (!canApplyHeightModifier(id, std::nullopt, &diagnostic)) throw invalid_argument(diagnostic);
		mWorld.invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mWorld.mAgents.entries())
		{
			if (!agent || !agent->hasAgentTag(id)) continue;
			auto const adherenceBefore = agent->getEffectivePermissionAdherence().value;
			auto const usageBefore = agent->getObjectUsage();
			auto const distanceBefore = agent->getObjectUsageDistance();
			agent->removeAgentTag(id);
			mWorld.agentObjectUsageChanged(agentId, usageBefore, distanceBefore);
			clearSamplesSourcedBy(*agent, id);
			auto const adherenceAfter = agent->getEffectivePermissionAdherence().value;
			if (adherenceBefore != adherenceAfter)
			{
				if (adherenceAfter) mWorld.replanAgentAfterAuthorizationRefusal(agentId);
				else mWorld.beginVoluntaryRoutePlanning(agentId);
			}
			changed = true;
		}
		if (changed) mWorld.modify();
	}

	bool AgentTagService::allAssignmentsCanBeCleared(string* diagnostic) const
	{
		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			(void)id;
			if (agent && (!mWorld.agentHeightStateFits(*agent, agent->getIndividualHeightModifier().value_or(1.f), diagnostic)
				|| !agent->objectUsageConfigurationIsValid(nullptr, {}, agent->getIndividualObjectUsage(),
					agent->getIndividualObjectUsageDistance(), diagnostic))) return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	void AgentTagService::clearAllAssignmentsAndSamples()
	{
		string diagnostic;
		if (!allAssignmentsCanBeCleared(&diagnostic)) throw invalid_argument(diagnostic);
		mWorld.invalidateSimulationSnapshot();
		for (auto& [agentId, agent] : mWorld.mAgents.entries())
		{
			if (!agent) continue;
			auto const adherenceBefore = agent->getEffectivePermissionAdherence().value;
			auto const usageBefore = agent->getObjectUsage();
			auto const distanceBefore = agent->getObjectUsageDistance();
			agent->setAgentTags({});
			mWorld.agentObjectUsageChanged(agentId, usageBefore, distanceBefore);
			// The cleared-sample set is exactly the one the replaced
			// World method carried: the route-planning samples are left
			// in place, as they were before this extraction.
			agent->clearWalkSpeedModifierSample();
			agent->clearHeightModifierSample();
			agent->clearStairSpeedModifierSample();
			agent->clearLadderSpeedModifierSample();
			agent->clearInteractionAversionSample();
			agent->clearEffortAversionSample();
			agent->clearWaitingAversionSample();
			agent->clearCrowdAversionSample();
			auto const adherenceAfter = agent->getEffectivePermissionAdherence().value;
			if (adherenceBefore != adherenceAfter)
			{
				if (adherenceAfter) mWorld.replanAgentAfterAuthorizationRefusal(agentId);
				else mWorld.beginVoluntaryRoutePlanning(agentId);
			}
		}
	}

	bool AgentTagService::canApplyHeightModifier(AgentTagId id,
		optional<AgentModifierRange> range, string* diagnostic) const
	{
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (!agent || agent->getIndividualHeightModifier()) continue;
			if (range ? !agent->hasAgentTag(id) : (!agent->getHeightModifierSample()
				|| agent->getHeightModifierSample()->sourceTag != id)) continue;
			if (!mWorld.agentHeightStateFits(*agent, range ? range->maximum : 1.0f, diagnostic)) return false;
		}
		return true;
	}

	bool AgentTagService::validateAssignments(set<AgentTagId> const& tags,
		AgentTagSampleSet const& samples, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (tags.empty())
		{
			for (auto const& sample : samples)
				if (sample) return reject("An untagged Agent cannot carry modifier samples");
			return true;
		}
		if (!mWorld.mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");

		AgentTagId escalatorWalkingChanceSource{};
		AgentTagId colourSource{};
		AgentTagId objectUsageSource{}, objectUsageDistanceSource{};
		AgentTagId permissionAdherenceSource{};
		AgentTagId remoteAccessPanelsSource{};
		AgentTagId remoteBoothWindowShuttersSource{};
		AgentTagId mobilityProfileSource{};
		array<AgentTagId, SampledAgentPropertyCount> sources{};
		array<AgentModifierRange const*, SampledAgentPropertyCount> ranges{};
		array<uint64_t, SampledAgentPropertyCount> revisions{};
		for (auto const tag : tags)
		{
			auto const* definition = mWorld.mAgentTagRegistry->lookupAgentTag(tag);
			if (!definition)
				return reject(format("Agent tag {} is not defined in the attached registry",
					tag.value));
			if (definition->getColour())
			{
				if (colourSource)
					return reject(format("Colour is inherited from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(colourSource), definition->getName()));
				colourSource = tag;
			}
			if (definition->getEscalatorWalkingChance())
			{
				if (escalatorWalkingChanceSource)
					return reject(format("Escalator walking chance is inherited from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(escalatorWalkingChanceSource), definition->getName()));
				escalatorWalkingChanceSource = tag;
			}
			for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
			{
				auto const& row = sampledRows().rows[i];
				if (auto const* range = row.definitionRange(*definition))
				{
					if (sources[i])
						return reject(format(
							"{} is inherited from both #{} and #{}",
							row.name, mWorld.mAgentTagRegistry->getAgentTagName(sources[i]),
							definition->getName()));
					sources[i] = tag;
					ranges[i] = range;
					revisions[i] = row.definitionRevision(*definition);
				}
			}
			if (definition->getObjectUsage())
			{
				if (objectUsageSource) return reject("Object usage is inherited from more than one Agent tag");
				objectUsageSource = tag;
			}
			if (definition->getObjectUsageDistance())
			{
				if (objectUsageDistanceSource) return reject("Object usage distance is inherited from more than one Agent tag");
				objectUsageDistanceSource = tag;
			}
			if (definition->getPermissionAdherence())
			{
				if (permissionAdherenceSource)
					return reject(format("Permission adherence is inherited from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(permissionAdherenceSource), definition->getName()));
				permissionAdherenceSource = tag;
			}
			if (definition->getRemoteAccessPanels())
			{
				if (remoteAccessPanelsSource)
					return reject(format("Agent inherits Remote Access panels from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(remoteAccessPanelsSource), definition->getName()));
				remoteAccessPanelsSource = tag;
			}
			if (definition->getRemoteBoothWindowShutters())
			{
				if (remoteBoothWindowShuttersSource)
					return reject(format("Agent inherits Remote BoothWindow shutters from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(remoteBoothWindowShuttersSource), definition->getName()));
				remoteBoothWindowShuttersSource = tag;
			}
			if (definition->getMobilityProfile())
			{
				if (mobilityProfileSource)
					return reject(format("Mobility profile is inherited from both #{} and #{}",
						mWorld.mAgentTagRegistry->getAgentTagName(mobilityProfileSource), definition->getName()));
				mobilityProfileSource = tag;
			}
		}

		auto validateSample = [&](char const* name, SampledAgentPropertyType type,
			AgentTagId source, AgentModifierRange const* range, uint64_t revision,
			optional<AgentPropertySample> const& sample)
		{
			if (!source)
			{
				if (sample) return reject(format(
					"The pasted Agent has a {} sample without an inherited property", name));
				return true;
			}
			if (!sample)
				return reject(format("The pasted Agent has no sample for {} from #{}", name,
					mWorld.mAgentTagRegistry->getAgentTagName(source)));
			if (sample->type != type || sample->sourceTag != source)
				return reject(format("The pasted Agent's {} sample has the wrong source", name));
			if (sample->propertyRevision != revision)
				return reject(format("The pasted Agent's {} sample is stale", name));
			if (!isfinite(sample->value) || sample->value < range->minimum
				|| sample->value > range->maximum)
			{
				return reject(format(
					"The pasted Agent's {} sample is outside the current property range", name));
			}
			return true;
		};

		for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
		{
			if (!validateSample(sampledRows().rows[i].name, sampledRows().rows[i].type,
				sources[i], ranges[i], revisions[i], samples[i])) return false;
		}
		return true;
	}

	bool AgentTagService::restoreAssignments(AgentId agent,
		set<AgentTagId> const& tags, AgentTagSampleSet const& samples,
		string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		if (diagnostic) diagnostic->clear();
		auto const lookup = mWorld.lookupAgent(agent);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		if (!mWorld.mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Pause the simulation before restoring Agent tag assignments";
			return false;
		}
		if (!validateAssignments(tags, samples, diagnostic)) return false;

		auto* target = mWorld.mAgents.find(agent);
		bool unchanged = target->getAgentTagIds() == tags;
		for (size_t i = 0; unchanged && i < SampledAgentPropertyCount; ++i)
			unchanged = sampledRows().rows[i].sample->readSample(*target) == samples[i];
		if (unchanged) return true;

		if (!mWorld.agentHeightStateFits(*target, target->getIndividualHeightModifier().value_or(
			samples[HeightModifierIndex] ? samples[HeightModifierIndex]->value : 1.0f), diagnostic)) return false;
		if (!target->objectUsageConfigurationIsValid(mWorld.mAgentTagRegistry.get(), tags,
			target->getIndividualObjectUsage(), target->getIndividualObjectUsageDistance(), diagnostic)) return false;
		auto const usageBefore = target->getObjectUsage();
		auto const distanceBefore = target->getObjectUsageDistance();
		target->setAgentTags(tags);
		mWorld.agentObjectUsageChanged(agent, usageBefore, distanceBefore);
		for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
		{
			if (samples[i]) sampledRows().rows[i].sample->writeSample(*target, *samples[i]);
			else sampledRows().rows[i].sample->clearSample(*target);
		}
		mWorld.modify();
		return true;
	}

	bool AgentTagService::inspectAssignments(AgentTagRegistry const& registry,
		bool allowSampleReconciliation, vector<AgentTagReconciliation>* repairs,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		if (repairs) repairs->clear();
		auto reject = [diagnostic](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};

		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			if (!agent) continue;
			AgentTagId escalatorWalkingChanceSource{};
			AgentTagId colourSource{};
			AgentTagId permissionAdherenceSource{};
			AgentTagId remoteAccessPanelsSource{};
			AgentTagId remoteBoothWindowShuttersSource{};
			AgentTagId mobilityProfileSource{};
			array<AgentTagId, SampledAgentPropertyCount> sources{};
			array<AgentModifierRange const*, SampledAgentPropertyCount> ranges{};
			array<uint64_t, SampledAgentPropertyCount> revisions{};
			for (auto const tag : agent->getAgentTagIds())
			{
				auto const* definition = registry.lookupAgentTag(tag);
				if (!definition)
				{
					return reject(format(
						"Agent '{}' is assigned to Agent tag {}, which the attached registry does not define",
						agent->getName(), tag.value));
				}
				if (definition->getColour())
				{
					if (colourSource)
					{
						return reject(format(
							"Agent '{}' inherits Colour from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(colourSource),
							definition->getName()));
					}
					colourSource = tag;
				}
				if (definition->getEscalatorWalkingChance())
				{
					if (escalatorWalkingChanceSource)
					{
						return reject(format(
							"Agent '{}' inherits Escalator walking chance from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(escalatorWalkingChanceSource),
							definition->getName()));
					}
					escalatorWalkingChanceSource = tag;
				}
				for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
				{
					auto const& row = sampledRows().rows[i];
					if (auto const* range = row.definitionRange(*definition))
					{
						if (sources[i])
						{
							return reject(format(
								"Agent '{}' inherits {} from both #{} and #{}",
								agent->getName(), row.name,
								registry.getAgentTagName(sources[i]),
								definition->getName()));
						}
						sources[i] = tag;
						ranges[i] = range;
						revisions[i] = row.definitionRevision(*definition);
					}
				}
				if (definition->getPermissionAdherence())
				{
					if (permissionAdherenceSource)
						return reject(format(
							"Agent '{}' inherits Permission adherence from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(permissionAdherenceSource),
							definition->getName()));
					permissionAdherenceSource = tag;
				}
				if (definition->getRemoteAccessPanels())
				{
					if (remoteAccessPanelsSource)
						return reject(format(
							"Agent '{}' inherits Remote Access panels from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(remoteAccessPanelsSource),
							definition->getName()));
					remoteAccessPanelsSource = tag;
				}
				if (definition->getRemoteBoothWindowShutters())
				{
					if (remoteBoothWindowShuttersSource)
						return reject(format(
							"Agent '{}' inherits Remote BoothWindow shutters from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(remoteBoothWindowShuttersSource),
							definition->getName()));
					remoteBoothWindowShuttersSource = tag;
				}
				if (definition->getMobilityProfile())
				{
					if (mobilityProfileSource)
						return reject(format(
							"Agent '{}' inherits Mobility profile from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(mobilityProfileSource),
							definition->getName()));
					mobilityProfileSource = tag;
				}
			}

			if (!agent->objectUsageConfigurationIsValid(&registry, agent->getAgentTagIds(),
				agent->getIndividualObjectUsage(), agent->getIndividualObjectUsageDistance(), diagnostic)) return false;
			AgentTagReconciliation repair;
			repair.agent = agentId;
			repair.objectUsageBefore = agent->getObjectUsage();
			repair.objectUsageDistanceBefore = agent->getObjectUsageDistance();
			auto const prospectiveUsage = agent->resolveObjectUsage(&registry, agent->getAgentTagIds(),
				agent->getIndividualObjectUsage(), agent->getIndividualObjectUsageDistance());
			repair.objectUsageChanged = prospectiveUsage.first != repair.objectUsageBefore
				|| (prospectiveUsage.first != ObjectUsage::None && prospectiveUsage.second != repair.objectUsageDistanceBefore);
			auto inspectSample = [&](char const* name, SampledAgentPropertyType type,
				AgentTagId source, AgentModifierRange const* range, uint64_t revision,
				optional<AgentPropertySample> const& sample,
				AgentTagSampleRepairAction& action)
			{
				if (!source)
				{
					if (!sample) return true;
					if (!allowSampleReconciliation)
					{
						return reject(format(
							"Agent '{}' has a {} sample without an inherited property",
							agent->getName(), name));
					}
					action = AgentTagSampleRepairAction::Clear;
					return true;
				}
				if (!sample)
				{
					if (!allowSampleReconciliation)
					{
						return reject(format(
							"Agent '{}' has no sample for {} from #{}", agent->getName(),
							name, registry.getAgentTagName(source)));
					}
					action = AgentTagSampleRepairAction::Resample;
					return true;
				}
				if (sample->type != type || sample->sourceTag != source)
				{
					auto const* sampledTag = registry.lookupAgentTag(sample->sourceTag);
					auto const sampledSource = sampledTag
						? "#" + sampledTag->getName()
						: format("Agent tag {}", sample->sourceTag.value);
					return reject(format(
						"Agent '{}' has a {} sample from {}, but inherits that property from #{}",
						agent->getName(), name, sampledSource,
						registry.getAgentTagName(source)));
				}
				if (sample->propertyRevision != revision)
				{
					if (!allowSampleReconciliation)
					{
						return reject(format(
							"Agent '{}' has a stale {} sample for #{}", agent->getName(),
							name, registry.getAgentTagName(source)));
					}
					action = AgentTagSampleRepairAction::Resample;
					return true;
				}
				if (!isfinite(sample->value))
				{
					return reject(format(
						"Agent '{}' has a non-finite current-revision {} sample for #{}",
						agent->getName(), name, registry.getAgentTagName(source)));
				}
				if (sample->value < range->minimum || sample->value > range->maximum)
				{
					return reject(format(
						"Agent '{}' has current-revision {} sample {} outside #{} range [{}, {}]",
						agent->getName(), name, sample->value,
						registry.getAgentTagName(source), range->minimum, range->maximum));
				}
				return true;
			};

			for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
			{
				auto const& row = sampledRows().rows[i];
				if (!inspectSample(row.name, row.type, sources[i], ranges[i], revisions[i],
					row.sample->readSample(*agent), repair.*(row.action))) return false;
			}

			for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
			{
				auto const& row = sampledRows().rows[i];
				if (repair.*(row.action) == AgentTagSampleRepairAction::Resample)
				{
					repair.*(row.source) = sources[i];
					row.captureRepair(repair, *registry.lookupAgentTag(sources[i]));
				}
			}
			if (repair.heightAction != AgentTagSampleRepairAction::None
				&& !mWorld.agentHeightStateFits(*agent, agent->getIndividualHeightModifier().value_or(
					repair.heightAction == AgentTagSampleRepairAction::Resample ? repair.heightProperty.range.maximum : 1.f),
					diagnostic)) return false;
			bool anyRepair = repair.objectUsageChanged;
			for (auto const& row : sampledRows().rows)
				if (repair.*(row.action) != AgentTagSampleRepairAction::None) anyRepair = true;
			if (repairs && anyRepair) repairs->push_back(repair);
		}
		return true;
	}

	bool AgentTagService::assignmentsAreValid(AgentTagRegistry const& registry,
		string* diagnostic) const
	{
		return inspectAssignments(registry, false, nullptr, diagnostic);
	}

	void AgentTagService::applyReconciliations(
		vector<AgentTagReconciliation> const& repairs)
	{
		mWorld.invalidateSimulationSnapshot();
		// A caller validates the complete transaction before reaching this
		// seam. Sampling and application cannot refuse, so all repairs
		// commit together.
		for (auto const& repair : repairs)
		{
			auto* agent = mWorld.mAgents.find(repair.agent);
			if (repair.objectUsageChanged)
				mWorld.agentObjectUsageChanged(repair.agent, repair.objectUsageBefore, repair.objectUsageDistanceBefore);
			else mWorld.mSimulationCoordinator.agentObjectUsageChanged(repair.agent);
			for (size_t i = 0; i < SampledAgentPropertyCount; ++i)
			{
				auto const& row = sampledRows().rows[i];
				if (repair.*(row.action) == AgentTagSampleRepairAction::Clear)
					row.sample->clearSample(*agent);
				else if (repair.*(row.action) == AgentTagSampleRepairAction::Resample)
				{
					row.sample->writeSample(*agent, AgentPropertySample{
						row.type, repair.*(row.source), row.repairRevision(repair),
						sampleAgentModifier(row.repairRange(repair)) });
				}
				// The pose sync keeps its place from the replaced method:
				// right after the Height modifier's repair, before the
				// remaining rows are written.
				if (i == HeightModifierIndex) agent->syncPoseToSector();
			}
		}
		if (!repairs.empty()) mWorld.modify();
	}

	void AgentTagService::reconcileAssignments(AgentTagRegistry const& registry)
	{
		mWorld.invalidateSimulationSnapshot();
		vector<AgentTagReconciliation> repairs;
		string diagnostic;
		if (!inspectAssignments(registry, true, &repairs, &diagnostic))
			throw runtime_error(diagnostic);
		applyReconciliations(repairs);
	}
}
