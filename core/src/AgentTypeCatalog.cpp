#include <algorithm>
#include <string>
#include <utility>

#include "core/AgentTypeCatalog.h"

#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/Sector.h"
#include "core/SerializationException.h"
#include "core/World.h"
#include "core/WorldDocument.h"

namespace core
{
	using namespace std;

	AgentTypeCatalog::AgentTypeCatalog(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Registration
	// ----------------------------------------------------------------------

	void AgentTypeCatalog::registerBundledAgentTypes()
	{
		if (mWorld.mAgentTypes.contains("Human")) return;
		auto definition = std::make_shared<AgentTypeDefinition>(
			bundledHumanAgentType());
		mWorld.mAgentTypes.emplace(definition->typeId, std::move(definition));
	}

	bool AgentTypeCatalog::attachAgentType(std::string resourceName, std::string source,
		std::string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		auto reject = [&](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (resourceName.empty())
			return reject("Agent type resource name cannot be empty");
		auto preflight = AgentTypeRuntimeAdapter::preflightType(resourceName, source);
		if (!preflight.loaded)
			return reject(preflight.diagnostic);
		if (!agentTypeIdIsValid(preflight.typeId))
			return reject("Agent type has an invalid type_id: " + preflight.typeId);
		if (!agentTypeDisplayNameIsValid(preflight.displayName))
			return reject("Agent type has an invalid display_name");
		if (mWorld.mAgentTypes.contains(preflight.typeId))
			return reject("Agent type ID '" + preflight.typeId
				+ "' is already registered in this World");
		auto definition = std::make_shared<AgentTypeDefinition>();
		definition->typeId = std::move(preflight.typeId);
		definition->displayName = std::move(preflight.displayName);
		definition->resourceName = std::move(resourceName);
		definition->source = std::move(source);
		bool const hasStairs = any_of(mWorld.mSectors.begin(), mWorld.mSectors.end(),
			[](auto const& sector) {
				return sector && sector->getType() == SectorType::Stairwell;
			});
		bool widenStairs = false;
		if (hasStairs)
			if (auto baseline = agentTypeDefinitionBaseline(*definition); baseline
				&& baseline->supportsPose(Pose::Standing))
			{
				widenStairs = baseline->width > mWorld.maximumInstalledStandingWidth();
				if (baseline->width >= 2.f)
					return reject("Agent Standing width leaves no room for Stairwell platforms");
				if (widenStairs && mWorld.mBuildFinished)
				{
					if (!mWorld.mSimulationPaused)
						return reject("Pause before installing a type that changes Stairwell platforms");
					for (auto const& [id, agent] : mWorld.mAgents.entries())
						if (agent && agent->getSector()
							&& agent->getSector()->getType() == SectorType::Stairwell)
							return reject("Cannot change Stairwell platform geometry while an Agent occupies it");
				}
			}
		auto const typeId = definition->typeId;
		mWorld.mAgentTypes.emplace(typeId, std::move(definition));
		if (widenStairs && mWorld.mBuildFinished && !mWorld.rebuildTraversalTopology())
		{
			mWorld.mAgentTypes.erase(typeId);
			return reject(mWorld.mTopologyDiagnostic);
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTypeCatalog::detachUnusedAgentType(std::string_view typeId)
	{
		if (typeId == "Human") return false;
		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			(void)id;
			if (agent->getTypeId() == typeId) return false;
		}
		return mWorld.mAgentTypes.erase(std::string(typeId)) != 0;
	}

	// ----------------------------------------------------------------------
	// Resolution
	// ----------------------------------------------------------------------

	std::shared_ptr<const AgentTypeDefinition> AgentTypeCatalog::resolveAgentType(
		std::string const& typeId, std::string const& resourceName, bool reconstruct)
	{
		if (reconstruct && !resourceName.empty())
		{
			// Reconstruction, unlike ordinary creation, resolves the current revision.
			// Directly attached in-memory definitions have no file dependency; retain
			// their registered source when no managed resource supplies a revision.
			auto resolved = core::resolveAgentTypeResource(resourceName);
			if (resolved)
			{
				if (resolved->typeId != typeId)
					throw SerializationException("Agent type resource '" + resourceName
						+ "' declares type ID '" + resolved->typeId
						+ "' but reconstruction expects '" + typeId + "'");
				auto found = mWorld.mAgentTypes.find(typeId);
				if (found != mWorld.mAgentTypes.end()
					&& found->second->resourceName != resourceName)
					throw SerializationException("Agent type ID '" + typeId
						+ "' is declared by competing resources in this World");
				auto definition = std::make_shared<AgentTypeDefinition>(std::move(*resolved));
				mWorld.mAgentTypes[typeId] = definition;
				return definition;
			}
			if (!resolveCatalogSource("AgentType", resourceName).empty())
				throw SerializationException("Missing or invalid Agent type '" + typeId
					+ "' resource '" + resourceName + "'");
		}
		if (!resourceName.empty())
		{
			// An explicit resource reference is authoritative: if the named
			// resource is already registered, its declared type ID must match
			// the saved record.
			for (auto const& [id, definition] : mWorld.mAgentTypes)
			{
				(void)id;
				if (definition->resourceName != resourceName) continue;
				if (definition->typeId != typeId)
					throw SerializationException(
						"Agent type resource '" + resourceName + "' declares type ID '"
						+ definition->typeId + "' but the document expects '" + typeId + "'");
				return definition;
			}
			// Not registered yet: resolve the managed resource (ADR 0010/0019)
			// and verify its declared type ID before registering it. A missing,
			// unreadable, or invalid resource fails clearly; it never falls back
			// to Human.
			auto resolved = core::resolveAgentTypeResource(resourceName);
			if (!resolved)
				throw SerializationException(
					"Missing Agent type resource '" + resourceName + "'");
			if (resolved->typeId != typeId)
				throw SerializationException(
					"Agent type resource '" + resourceName + "' declares type ID '"
					+ resolved->typeId + "' but the document expects '" + typeId + "'");
			if (mWorld.mAgentTypes.contains(resolved->typeId))
				throw SerializationException(
					"Agent type ID '" + resolved->typeId
					+ "' is declared by competing resources in this World");
			auto definition = std::make_shared<AgentTypeDefinition>(*resolved);
			auto const stored = definition->typeId;
			mWorld.mAgentTypes.emplace(stored, std::move(definition));
			return mWorld.mAgentTypes.at(stored);
		}
		// Legacy record: the stable type ID names a bundled or attached type.
		auto found = mWorld.mAgentTypes.find(typeId);
		if (found == mWorld.mAgentTypes.end())
			throw SerializationException("Unsupported Agent type '" + typeId + "'");
		return found->second;
	}

	// ----------------------------------------------------------------------
	// Queries
	// ----------------------------------------------------------------------

	bool AgentTypeCatalog::hasAgentType(std::string_view typeId) const
	{
		return mWorld.mAgentTypes.contains(std::string(typeId));
	}

	std::string AgentTypeCatalog::agentTypeResourceName(std::string_view typeId) const
	{
		auto found = mWorld.mAgentTypes.find(std::string(typeId));
		return found == mWorld.mAgentTypes.end() ? std::string{} : found->second->resourceName;
	}

	std::string AgentTypeCatalog::agentTypeDisplayName(std::string_view typeId) const
	{
		auto found = mWorld.mAgentTypes.find(std::string(typeId));
		return found == mWorld.mAgentTypes.end() ? std::string{} : found->second->displayName;
	}

	optional<AgentTypeDefinition> AgentTypeCatalog::registeredAgentTypeDefinition(
		string_view typeId, string_view resourceName) const
	{
		auto found = mWorld.mAgentTypes.find(string(typeId));
		if (found == mWorld.mAgentTypes.end() || found->second->resourceName != resourceName)
			return nullopt;
		return *found->second;
	}

	optional<AgentPhysicalBaseline> AgentTypeCatalog::registeredAgentTypeBaseline(
		string_view typeId, string_view resourceName) const
	{
		auto const definition = registeredAgentTypeDefinition(typeId, resourceName);
		if (!definition) return nullopt;
		auto found = mWorld.mAgentTypePreviewBaselines.find(string(typeId));
		if (found == mWorld.mAgentTypePreviewBaselines.end()
			|| found->second.first != definition->source)
		{
			auto baseline = agentTypeDefinitionBaseline(*definition);
			if (!baseline) return nullopt;
			found = mWorld.mAgentTypePreviewBaselines.insert_or_assign(string(typeId),
				make_pair(definition->source, *baseline)).first;
		}
		return found->second.second;
	}
}
