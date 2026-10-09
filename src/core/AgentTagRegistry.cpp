#include "core/AgentTagRegistry.h"
#include "core/MobilityProfile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/World.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace core
{
	namespace
	{
		std::filesystem::path normalizedDocumentPath(
			std::filesystem::path const& filepath)
		{
			std::error_code error;
			auto normalized = std::filesystem::weakly_canonical(filepath, error);
			if (!error) return normalized;
			normalized = std::filesystem::absolute(filepath, error);
			return (error ? filepath : normalized).lexically_normal();
		}

		std::string readDocument(std::filesystem::path const& filepath)
		{
			std::ifstream input(filepath, std::ios::binary);
			if (!input)
				throw SerializationException(std::format(
					"Could not read Agent tag registry {}", filepath.string()));
			return { std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>() };
		}

		std::string generateUuid()
		{
			std::random_device source;
			std::array<uint8_t, 16> bytes{};
			for (auto& byte : bytes) byte = static_cast<uint8_t>(source());

			// RFC 4122 variant, version 4. The UUID is document identity rather
			// than an allocator value and remains unchanged for the file's life.
			bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
			bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
			return std::format(
				"{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
				bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
				bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
		}
	}

	AgentTagRegistry::AgentTagRegistry(std::string uuid)
		: mUuid(std::move(uuid))
	{
	}

	std::shared_ptr<AgentTagRegistry> AgentTagRegistry::create()
	{
		return std::shared_ptr<AgentTagRegistry>(new AgentTagRegistry(generateUuid()));
	}

	std::shared_ptr<AgentTagRegistry> AgentTagRegistry::loadFrom(std::string const& filepath)
	{
		auto const path = normalizedDocumentPath(filepath);
		auto contents = readDocument(path);
		auto registry = std::shared_ptr<AgentTagRegistry>(new AgentTagRegistry(""));
		auto serializer = YamlSerializer::fromString(contents);
		serializer->deserialize();
		SerializationWorkData workData;
		if (!registry->deserialize(*serializer, workData))
		{
			throw SerializationException("Could not deserialize Agent tag registry");
		}
		// Serializable::deserialize clears the modified flag even though a
		// legacy document just gained backfilled display Colours. Surface that
		// here so the user is prompted to persist them.
		if (registry->mBackfilledDisplayColour)
		{
			registry->mBackfilledDisplayColour = false;
			registry->modify();
		}
		registry->mDocumentPath = path;
		registry->mSavedDocumentContents = std::move(contents);
		return registry;
	}

	std::shared_ptr<AgentTagRegistry> AgentTagRegistry::copyWithNewUuid(
		AgentTagRegistry const& source)
	{
		auto copy = create();
		while (copy->mUuid == source.mUuid) copy = create();
		for (auto const& [id, sourceTag] : source.mTags.entries())
		{
			auto tag = AgentTag::create(sourceTag->getName());
			tag->setDisplayColour(sourceTag->getDisplayColour());
			if (auto const* chance = sourceTag->getEscalatorWalkingChance())
				tag->setEscalatorWalkingChance(*chance);
			if (auto const* colour = sourceTag->getColour()) tag->setColour(*colour);
			if (auto const* walkSpeed = sourceTag->getWalkSpeedModifier())
				tag->setWalkSpeedModifier(*walkSpeed);
			if (auto const* height = sourceTag->getHeightModifier())
				tag->setHeightModifier(*height);
			if (auto const* stairSpeed = sourceTag->getStairSpeedModifier())
				tag->setStairSpeedModifier(*stairSpeed);
			if (auto const* ladderSpeed = sourceTag->getLadderSpeedModifier())
				tag->setLadderSpeedModifier(*ladderSpeed);
			if (auto const* interaction = sourceTag->getInteractionAversion())
				tag->setInteractionAversion(*interaction);
			if (auto const* effort = sourceTag->getEffortAversion())
				tag->setEffortAversion(*effort);
			if (auto const* waiting = sourceTag->getWaitingAversion())
				tag->setWaitingAversion(*waiting);
			if (auto const* crowd = sourceTag->getCrowdAversion())
				tag->setCrowdAversion(*crowd);
			if (auto const* risk = sourceTag->getRiskAversion())
				tag->setRiskAversion(*risk);
			if (auto const* familiarity = sourceTag->getRouteFamiliarity())
				tag->setRouteFamiliarity(*familiarity);
			if (auto const* persistence = sourceTag->getRoutePersistence())
				tag->setRoutePersistence(*persistence);
			if (auto const* minimumPlanningTime = sourceTag->getMinimumRoutePlanningTime())
				tag->setMinimumRoutePlanningTime(*minimumPlanningTime);
			if (auto const* maximumPlanningTime = sourceTag->getMaximumRoutePlanningTime())
				tag->setMaximumRoutePlanningTime(*maximumPlanningTime);
			if (auto const* property = sourceTag->getObjectUsage()) tag->setObjectUsage(*property);
			if (auto const* property = sourceTag->getObjectUsageDistance()) tag->setObjectUsageDistance(*property);
			if (auto const* adherence = sourceTag->getPermissionAdherence())
				tag->setPermissionAdherence(*adherence);
			if (auto const* remotePanels = sourceTag->getRemoteAccessPanels())
				tag->setRemoteAccessPanels(*remotePanels);
			if (auto const* remoteShutters = sourceTag->getRemoteBoothWindowShutters())
				tag->setRemoteBoothWindowShutters(*remoteShutters);
			if (auto const* mobility = sourceTag->getMobilityProfile())
				tag->setMobilityProfile(*mobility);
			if (!copy->mTags.restore(id, std::move(tag)))
				throw std::logic_error("Could not preserve an Agent tag ID while copying a registry");
		}
		if (!copy->mTags.restoreNextId(source.mTags.nextId()))
			throw std::logic_error("Could not preserve the Agent tag allocator while copying a registry");
		copy->mNextPropertyRevision = source.mNextPropertyRevision;
		return copy;
	}

	bool AgentTagRegistry::hasEquivalentDefinitions(AgentTagRegistry const& other) const
	{
		if (mTags.nextId() != other.mTags.nextId()
			|| mNextPropertyRevision != other.mNextPropertyRevision
			|| mTags.entries().size() != other.mTags.entries().size()) return false;
		for (auto const& [id, tag] : mTags.entries())
		{
			auto const* candidate = other.mTags.find(id);
			if (!candidate || tag->getName() != candidate->getName()
				|| tag->getDisplayColour() != candidate->getDisplayColour()) return false;
			auto optionalPropertyMatches = [](auto const* left, auto const* right)
			{
				return (!left && !right) || (left && right && *left == *right);
			};
			if (!optionalPropertyMatches(tag->getEscalatorWalkingChance(), candidate->getEscalatorWalkingChance())
				|| !optionalPropertyMatches(tag->getColour(), candidate->getColour())
				|| !optionalPropertyMatches(tag->getWalkSpeedModifier(),
					candidate->getWalkSpeedModifier())
				|| !optionalPropertyMatches(tag->getHeightModifier(),
					candidate->getHeightModifier())
				|| !optionalPropertyMatches(tag->getStairSpeedModifier(),
					candidate->getStairSpeedModifier())
				|| !optionalPropertyMatches(tag->getLadderSpeedModifier(),
					candidate->getLadderSpeedModifier())
				|| !optionalPropertyMatches(tag->getInteractionAversion(),
					candidate->getInteractionAversion())
				|| !optionalPropertyMatches(tag->getEffortAversion(),
					candidate->getEffortAversion())
				|| !optionalPropertyMatches(tag->getWaitingAversion(),
					candidate->getWaitingAversion())
				|| !optionalPropertyMatches(tag->getCrowdAversion(),
					candidate->getCrowdAversion())
				|| !optionalPropertyMatches(tag->getRiskAversion(),
					candidate->getRiskAversion())
				|| !optionalPropertyMatches(tag->getRouteFamiliarity(),
					candidate->getRouteFamiliarity())
				|| !optionalPropertyMatches(tag->getRoutePersistence(),
					candidate->getRoutePersistence())
				|| !optionalPropertyMatches(tag->getMinimumRoutePlanningTime(),
					candidate->getMinimumRoutePlanningTime())
				|| !optionalPropertyMatches(tag->getMaximumRoutePlanningTime(),
					candidate->getMaximumRoutePlanningTime())
				|| !optionalPropertyMatches(tag->getObjectUsage(), candidate->getObjectUsage())
				|| !optionalPropertyMatches(tag->getObjectUsageDistance(), candidate->getObjectUsageDistance())
				|| !optionalPropertyMatches(tag->getPermissionAdherence(),
					candidate->getPermissionAdherence())
				|| !optionalPropertyMatches(tag->getRemoteAccessPanels(),
					candidate->getRemoteAccessPanels())
				|| !optionalPropertyMatches(tag->getRemoteBoothWindowShutters(),
					candidate->getRemoteBoothWindowShutters())
				|| !optionalPropertyMatches(tag->getMobilityProfile(),
					candidate->getMobilityProfile())) return false;
		}
		return true;
	}

	bool AgentTagRegistry::uuidIsValid(std::string const& uuid)
	{
		if (uuid.size() != 36 || uuid[8] != '-' || uuid[13] != '-'
			|| uuid[18] != '-' || uuid[23] != '-') return false;
		for (size_t index = 0; index < uuid.size(); ++index)
		{
			if (index == 8 || index == 13 || index == 18 || index == 23) continue;
			auto const character = static_cast<unsigned char>(uuid[index]);
			if (!std::isxdigit(character) || std::isupper(character)) return false;
		}
		return uuid[14] == '4' && (uuid[19] == '8' || uuid[19] == '9'
			|| uuid[19] == 'a' || uuid[19] == 'b');
	}

	std::string const& AgentTagRegistry::getUuid() const
	{
		return mUuid;
	}

	uint64_t AgentTagRegistry::getNextAgentTagId() const
	{
		return mTags.nextId();
	}

	uint64_t AgentTagRegistry::getNextPropertyRevision() const
	{
		return mNextPropertyRevision;
	}

	uint32_t AgentTagRegistry::getAgentTagCount() const
	{
		return static_cast<uint32_t>(mTags.entries().size());
	}

	std::vector<AgentTagId> AgentTagRegistry::getAgentTagIds() const
	{
		std::vector<AgentTagId> ids;
		ids.reserve(mTags.entries().size());
		for (auto const& [id, tag] : mTags.entries())
		{
			(void)tag;
			ids.push_back(id);
		}
		return ids;
	}

	std::vector<AgentTagId> AgentTagRegistry::getAgentTagIdsAlphabetically() const
	{
		auto ids = getAgentTagIds();
		std::sort(ids.begin(), ids.end(), [this](AgentTagId left, AgentTagId right)
		{
			auto const& leftName = mTags.find(left)->getName();
			auto const& rightName = mTags.find(right)->getName();
			return leftName == rightName ? left < right : leftName < rightName;
		});
		return ids;
	}

	AgentTag const* AgentTagRegistry::lookupAgentTag(AgentTagId id) const
	{
		return mTags.find(id);
	}

	std::string const& AgentTagRegistry::getAgentTagName(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format("Agent tag {} is not defined in this registry", id.value));
		return tag->getName();
	}

	AgentColour AgentTagRegistry::getAgentTagDisplayColour(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getDisplayColour();
	}

	bool AgentTagRegistry::setAgentTagDisplayColour(AgentTagId id, AgentColour colour,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getDisplayColour() == colour)
			return reject("The tag display Colour is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		tag->setDisplayColour(colour);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AgentColourProperty const* AgentTagRegistry::getAgentTagColour(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getColour();
	}

	AgentEscalatorWalkingChanceProperty const* AgentTagRegistry::getAgentTagEscalatorWalkingChance(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getEscalatorWalkingChance();
	}

	AgentWalkSpeedModifierProperty const*
	AgentTagRegistry::getAgentTagWalkSpeedModifier(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getWalkSpeedModifier();
	}

	AgentHeightModifierProperty const*
	AgentTagRegistry::getAgentTagHeightModifier(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getHeightModifier();
	}

	AgentStairSpeedModifierProperty const*
	AgentTagRegistry::getAgentTagStairSpeedModifier(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getStairSpeedModifier();
	}

	AgentLadderSpeedModifierProperty const*
	AgentTagRegistry::getAgentTagLadderSpeedModifier(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getLadderSpeedModifier();
	}

	AgentInteractionAversionProperty const*
	AgentTagRegistry::getAgentTagInteractionAversion(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getInteractionAversion();
	}

	AgentEffortAversionProperty const*
	AgentTagRegistry::getAgentTagEffortAversion(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getEffortAversion();
	}

	AgentWaitingAversionProperty const*
	AgentTagRegistry::getAgentTagWaitingAversion(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getWaitingAversion();
	}

	AgentCrowdAversionProperty const*
	AgentTagRegistry::getAgentTagCrowdAversion(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getCrowdAversion();
	}

	AgentRiskAversionProperty const*
	AgentTagRegistry::getAgentTagRiskAversion(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getRiskAversion();
	}

	AgentRouteFamiliarityProperty const*
	AgentTagRegistry::getAgentTagRouteFamiliarity(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getRouteFamiliarity();
	}

	AgentRoutePersistenceProperty const*
	AgentTagRegistry::getAgentTagRoutePersistence(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getRoutePersistence();
	}

	AgentMinimumRoutePlanningTimeProperty const*
	AgentTagRegistry::getAgentTagMinimumRoutePlanningTime(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getMinimumRoutePlanningTime();
	}

	AgentMaximumRoutePlanningTimeProperty const*
	AgentTagRegistry::getAgentTagMaximumRoutePlanningTime(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getMaximumRoutePlanningTime();
	}

	AgentPermissionAdherenceProperty const*
	AgentTagRegistry::getAgentTagPermissionAdherence(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getPermissionAdherence();
	}
	AgentRemoteAccessPanelsProperty const*
	AgentTagRegistry::getAgentTagRemoteAccessPanels(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getRemoteAccessPanels();
	}
	AgentRemoteBoothWindowShuttersProperty const*
	AgentTagRegistry::getAgentTagRemoteBoothWindowShutters(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getRemoteBoothWindowShutters();
	}

	AgentMobilityProfileProperty const*
	AgentTagRegistry::getAgentTagMobilityProfile(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getMobilityProfile();
	}

	void AgentTagRegistry::registerWorld(World& world)
	{
		mLoadedWorlds.insert(&world);
	}

	void AgentTagRegistry::unregisterWorld(World& world)
	{
		mLoadedWorlds.erase(&world);
	}

	std::vector<LoadedAgentTagUsage> AgentTagRegistry::getLoadedAgentTagUsage(
		AgentTagId id) const
	{
		if (!mTags.find(id))
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		std::vector<LoadedAgentTagUsage> usage;
		usage.reserve(mLoadedWorlds.size());
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			usage.push_back({ world, world->countAgentTagAssignments(id) });
		}
		return usage;
	}

	uint64_t AgentTagRegistry::getLoadedAgentTagUsageCount(AgentTagId id) const
	{
		uint64_t count{ 0 };
		for (auto const& entry : getLoadedAgentTagUsage(id)) count += entry.agentCount;
		return count;
	}

	bool AgentTagRegistry::hasLoadedWorld(World const* world) const
	{
		return world && mLoadedWorlds.contains(const_cast<World*>(world));
	}

	bool AgentTagRegistry::hasLoadedWorlds() const
	{
		return !mLoadedWorlds.empty();
	}

	bool AgentTagRegistry::fileHasExternalChanges(std::string const& filepath) const
	{
		if (!mDocumentPath) return false;
		auto const path = normalizedDocumentPath(filepath);
		if (path != *mDocumentPath)
		{
			throw SerializationException(std::format(
				"Agent tag registry is loaded from {}, not {}",
				mDocumentPath->string(), path.string()));
		}
		try
		{
			return readDocument(path) != mSavedDocumentContents;
		}
		catch (SerializationException const&)
		{
			return true;
		}
	}

	bool AgentTagRegistry::replaceDefinitionsFrom(AgentTagRegistry&& replacement,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (replacement.mUuid != mUuid)
		{
			return reject(std::format(
				"Agent tag registry UUID mismatch: loaded {}, file contains {}",
				mUuid, replacement.mUuid));
		}
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		struct WorldRepairs
		{
			World* world{ nullptr };
			std::vector<World::AgentTagReconciliation> repairs;
		};
		std::vector<WorldRepairs> transaction;
		transaction.reserve(mLoadedWorlds.size());
		for (auto* world : mLoadedWorlds)
		{
			if (!world) continue;
			WorldRepairs entry;
			entry.world = world;
			std::string validationDiagnostic;
			if (!world->inspectAgentTagAssignments(replacement, true,
				&entry.repairs, &validationDiagnostic))
			{
				return reject(std::format("World '{}': {}",
					world->getName(), validationDiagnostic));
			}
			transaction.push_back(std::move(entry));
		}

		// Every definition and every loaded Agent has passed validation. Moving the
		// temporary state and applying precomputed repairs are non-refusing steps.
		mTags = std::move(replacement.mTags);
		mNextPropertyRevision = replacement.mNextPropertyRevision;
		mDocumentPath = std::move(replacement.mDocumentPath);
		mSavedDocumentContents = std::move(replacement.mSavedDocumentContents);
		markUnmodified();
		for (auto& entry : transaction)
			entry.world->applyAgentTagReconciliations(entry.repairs);
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::definitionEditsAreAllowed(std::string* diagnostic) const
	{
		for (auto const* world : mLoadedWorlds)
		{
			if (world && !world->isSimulationPaused())
			{
				if (diagnostic)
				{
					*diagnostic = std::format(
						"Pause World '{}' before editing this Agent tag registry",
						world->getName());
				}
				return false;
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::nameIsUnique(std::string const& name, AgentTagId except) const
	{
		for (auto const& [id, tag] : mTags.entries())
		{
			if (id != except && tag->getName() == name) return false;
		}
		return true;
	}

	uint64_t AgentTagRegistry::allocatePropertyRevision()
	{
		// Zero is reserved as "no revision". As with stable entity IDs, refuse
		// exhaustion rather than wrapping and reusing a value.
		if (mNextPropertyRevision == 0
			|| mNextPropertyRevision == std::numeric_limits<uint64_t>::max())
		{
			throw std::overflow_error(
				"This registry has issued every Agent property revision");
		}
		return mNextPropertyRevision++;
	}

	bool AgentTagRegistry::colourAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getColour()) continue;
					return reject(std::format(
						"Cannot add Colour to Agent tag #{}: Agent '{}' in World '{}' already inherits Colour from #{}",
						target->getName(), agent->getName(), world->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::escalatorWalkingChanceAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getEscalatorWalkingChance()) continue;
					return reject(std::format(
						"Cannot add Escalator walking chance to Agent tag #{}: Agent '{}' in World '{}' already inherits Escalator walking chance from #{}",
						target->getName(), agent->getName(), world->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::walkSpeedModifierAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getWalkSpeedModifier()) continue;
					return reject(std::format(
						"Cannot add Walk speed modifier to Agent tag #{}: Agent '{}' in World '{}' already inherits Walk speed modifier from #{}",
						target->getName(), agent->getName(), world->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::stairSpeedModifierAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getStairSpeedModifier()) continue;
					return reject(std::format(
						"Cannot add Stair speed modifier to Agent tag #{}: Agent '{}' in World '{}' already inherits Stair speed modifier from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::ladderSpeedModifierAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getLadderSpeedModifier()) continue;
					return reject(std::format(
						"Cannot add Ladder speed modifier to Agent tag #{}: Agent '{}' in World '{}' already inherits Ladder speed modifier from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::interactionAversionAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getInteractionAversion()) continue;
					return reject(std::format(
						"Cannot add Interaction aversion to Agent tag #{}: Agent '{}' in World '{}' already inherits Interaction aversion from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::effortAversionAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getEffortAversion()) continue;
					return reject(std::format(
						"Cannot add Effort aversion to Agent tag #{}: Agent '{}' in World '{}' already inherits Effort aversion from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::waitingAversionAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getWaitingAversion()) continue;
					return reject(std::format(
						"Cannot add Waiting aversion to Agent tag #{}: Agent '{}' in World '{}' already inherits Waiting aversion from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::crowdAversionAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getCrowdAversion()) continue;
					return reject(std::format(
						"Cannot add Crowd aversion to Agent tag #{}: Agent '{}' in World '{}' already inherits Crowd aversion from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::riskAversionAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getRiskAversion()) return reject(std::format(
						"Cannot add Risk aversion to Agent tag #{}: Agent '{}' in World '{}' already inherits Risk aversion from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::routeFamiliarityAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getRouteFamiliarity()) return reject(std::format(
						"Cannot add Route familiarity to Agent tag #{}: Agent '{}' in World '{}' already inherits Route familiarity from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::routePersistenceAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getRoutePersistence()) return reject(std::format(
						"Cannot add Route persistence to Agent tag #{}: Agent '{}' in World '{}' already inherits Route persistence from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::minimumRoutePlanningTimeAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getMinimumRoutePlanningTime()) return reject(std::format(
						"Cannot add Minimum route planning time to Agent tag #{}: Agent '{}' in World '{}' already inherits Minimum route planning time from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::maximumRoutePlanningTimeAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getMaximumRoutePlanningTime()) return reject(std::format(
						"Cannot add Maximum route planning time to Agent tag #{}: Agent '{}' in World '{}' already inherits Maximum route planning time from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::permissionAdherenceAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getPermissionAdherence()) return reject(std::format(
						"Cannot add Permission adherence to Agent tag #{}: Agent '{}' in World '{}' already inherits Permission adherence from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::remoteAccessPanelsAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getRemoteAccessPanels()) return reject(std::format(
						"Cannot add Remote Access panels to Agent tag #{}: Agent '{}' in World '{}' already inherits Remote Access panels from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::remoteBoothWindowShuttersAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto const* target = mTags.find(id);
		if (!target) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
			if (world) for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (source && source->getRemoteBoothWindowShutters()) return reject(std::format(
						"Cannot add Remote BoothWindow shutters to Agent tag #{}: Agent '{}' in World '{}' already inherits Remote BoothWindow shutters from #{}",
						target->getName(), agent->getName(), world->getName(), source->getName()));
				}
			}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::mobilityProfileAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getMobilityProfile()) continue;
					return reject(std::format(
						"Cannot add Mobility profile to Agent tag #{}: Agent '{}' in World '{}' already inherits Mobility profile from #{}",
						target->getName(), agent->getName(), world->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::heightModifierAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* world : mLoadedWorlds)
		{
			if (!world) continue;
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getHeightModifier()) continue;
					return reject(std::format(
						"Cannot add Height modifier to Agent tag #{}: Agent '{}' in World '{}' already inherits Height modifier from #{}",
						target->getName(), agent->getName(), world->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AgentTagId AgentTagRegistry::addAgentTag(std::string const& name)
	{
		std::string diagnostic;
		if (!AgentTag::nameIsValid(name, &diagnostic))
			throw std::invalid_argument(diagnostic);
		if (!nameIsUnique(name))
			throw std::invalid_argument(std::format("The Agent tag #{} already exists", name));
		if (!definitionEditsAreAllowed(&diagnostic))
			throw std::invalid_argument(diagnostic);
		if (mTags.exhausted())
			throw std::overflow_error("This registry has issued every Agent tag ID");

		auto const id = mTags.tryAdd(AgentTag::create(name));
		if (!id) throw std::overflow_error("This registry has issued every Agent tag ID");
		modify();
		return *id;
	}

	bool AgentTagRegistry::renameAgentTag(AgentTagId id, std::string const& name,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!AgentTag::nameIsValid(name, diagnostic)) return false;
		if (tag->getName() == name)
			return reject("The Agent tag name is unchanged");
		if (!nameIsUnique(name, id))
			return reject(std::format("The Agent tag #{} already exists", name));
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		tag->setName(name);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::deleteAgentTag(AgentTagId id, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		// Judge every dependent World before mutating any of them. Even a
		// World with no assignment depends on this shared definition document.
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		for (auto* world : mLoadedWorlds)
			if (world && !world->canApplyAgentTagHeightModifier(id, std::nullopt, diagnostic)) return false;

		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				auto tags = agent->getAgentTagIds(); tags.erase(id);
				if (!agent->objectUsageConfigurationIsValid(this, tags, agent->getIndividualObjectUsage(),
					agent->getIndividualObjectUsageDistance(), diagnostic)) return false;
			}

		// Every loaded assignment goes before the definition. From the first
		// write onward no loaded World can be left with a stale reference, and
		// no later step can refuse after the complete preflight above.
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagAssignments(id);
		mTags.remove(id);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagColour(AgentTagId id, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getColour())
			return reject(std::format("Agent tag #{} already has Colour", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!colourAdditionIsValid(id, diagnostic)) return false;

		try
		{
			tag->setColour({ EditorDefaultAgentColour, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagColour(AgentTagId id, AgentColour colour,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getColour();
		if (!current)
			return reject(std::format("Agent tag #{} has no Colour", tag->getName()));
		if (current->value == colour)
			return reject("The Agent Colour is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		try
		{
			tag->setColour({ colour, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagColour(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getColour())
			return reject(std::format("Agent tag #{} has no Colour", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		tag->removeColour();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagEscalatorWalkingChance(AgentTagId id, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getEscalatorWalkingChance())
			return reject(std::format("Agent tag #{} already has Escalator walking chance", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!escalatorWalkingChanceAdditionIsValid(id, diagnostic)) return false;

		try
		{
			tag->setEscalatorWalkingChance({ 0.0f, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagEscalatorWalkingChance(AgentTagId id, float value,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getEscalatorWalkingChance();
		if (!current)
			return reject(std::format("Agent tag #{} has no Escalator walking chance", tag->getName()));
		if (!agentEscalatorWalkingChanceIsValid(value, diagnostic)) return false;
		if (current->value == value)
			return reject("The Agent Escalator walking chance is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		try
		{
			tag->setEscalatorWalkingChance({ value, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagEscalatorWalkingChance(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getEscalatorWalkingChance())
			return reject(std::format("Agent tag #{} has no Escalator walking chance", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		tag->removeEscalatorWalkingChance();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagWalkSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getWalkSpeedModifier())
			return reject(std::format(
				"Agent tag #{} already has Walk speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!walkSpeedModifierAdditionIsValid(id, diagnostic)) return false;

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWalkSpeedModifier({ DefaultAgentWalkSpeedModifierRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagWalkSpeedModifierSamples(
				id, *tag->getWalkSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagWalkSpeedModifier(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getWalkSpeedModifier();
		if (!current)
			return reject(std::format(
				"Agent tag #{} has no Walk speed modifier", tag->getName()));
		if (!agentWalkSpeedModifierRangeIsValid(range, diagnostic)) return false;
		if (current->range == range)
			return reject("The Agent Walk speed modifier range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWalkSpeedModifier({ range, revision });
		// Install the new definition before sampling so every replacement carries
		// the same newly allocated provenance. Each loaded assigned Agent is visited
		// once; Worlds with no such Agent remain untouched.
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagWalkSpeedModifierSamples(
				id, *tag->getWalkSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagWalkSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getWalkSpeedModifier())
			return reject(std::format(
				"Agent tag #{} has no Walk speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagWalkSpeedModifierSamples(id);
		tag->removeWalkSpeedModifier();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagHeightModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getHeightModifier())
			return reject(std::format(
				"Agent tag #{} already has Height modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!heightModifierAdditionIsValid(id, diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world && !world->canApplyAgentTagHeightModifier(id, DefaultAgentHeightModifierRange, diagnostic)) return false;

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setHeightModifier({ DefaultAgentHeightModifierRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagHeightModifierSamples(
				id, *tag->getHeightModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagHeightModifier(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getHeightModifier();
		if (!current)
			return reject(std::format(
				"Agent tag #{} has no Height modifier", tag->getName()));
		if (!agentHeightModifierRangeIsValid(range, diagnostic)) return false;
		if (current->range == range)
			return reject("The Agent Height modifier range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world && !world->canApplyAgentTagHeightModifier(id, range, diagnostic)) return false;

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setHeightModifier({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagHeightModifierSamples(
				id, *tag->getHeightModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagHeightModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getHeightModifier())
			return reject(std::format(
				"Agent tag #{} has no Height modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world && !world->canApplyAgentTagHeightModifier(id, std::nullopt, diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagHeightModifierSamples(id);
		tag->removeHeightModifier();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagStairSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getStairSpeedModifier()) return reject(std::format(
			"Agent tag #{} already has Stair speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!stairSpeedModifierAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setStairSpeedModifier({ DefaultAgentStairSpeedModifierRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagStairSpeedModifierSamples(id, *tag->getStairSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagStairSpeedModifier(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getStairSpeedModifier();
		if (!current) return reject(std::format("Agent tag #{} has no Stair speed modifier", tag->getName()));
		if (!agentStairSpeedModifierRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Stair speed modifier range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setStairSpeedModifier({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagStairSpeedModifierSamples(id, *tag->getStairSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagStairSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getStairSpeedModifier()) return reject(std::format(
			"Agent tag #{} has no Stair speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagStairSpeedModifierSamples(id);
		tag->removeStairSpeedModifier();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagLadderSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getLadderSpeedModifier()) return reject(std::format(
			"Agent tag #{} already has Ladder speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!ladderSpeedModifierAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setLadderSpeedModifier({ DefaultAgentLadderSpeedModifierRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagLadderSpeedModifierSamples(id, *tag->getLadderSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagLadderSpeedModifier(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getLadderSpeedModifier();
		if (!current) return reject(std::format("Agent tag #{} has no Ladder speed modifier", tag->getName()));
		if (!agentLadderSpeedModifierRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Ladder speed modifier range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setLadderSpeedModifier({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagLadderSpeedModifierSamples(id, *tag->getLadderSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagLadderSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getLadderSpeedModifier()) return reject(std::format(
			"Agent tag #{} has no Ladder speed modifier", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagLadderSpeedModifierSamples(id);
		tag->removeLadderSpeedModifier();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagInteractionAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getInteractionAversion()) return reject(std::format(
			"Agent tag #{} already has Interaction aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!interactionAversionAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setInteractionAversion({ DefaultAgentInteractionAversionRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagInteractionAversionSamples(id, *tag->getInteractionAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagInteractionAversion(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getInteractionAversion();
		if (!current) return reject(std::format("Agent tag #{} has no Interaction aversion", tag->getName()));
		if (!agentInteractionAversionRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Interaction aversion range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setInteractionAversion({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagInteractionAversionSamples(id, *tag->getInteractionAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagInteractionAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getInteractionAversion()) return reject(std::format(
			"Agent tag #{} has no Interaction aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagInteractionAversionSamples(id);
		tag->removeInteractionAversion();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagEffortAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getEffortAversion()) return reject(std::format(
			"Agent tag #{} already has Effort aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!effortAversionAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setEffortAversion({ DefaultAgentEffortAversionRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagEffortAversionSamples(id, *tag->getEffortAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagWaitingAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getWaitingAversion()) return reject(std::format(
			"Agent tag #{} already has Waiting aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!waitingAversionAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWaitingAversion({ DefaultAgentWaitingAversionRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagWaitingAversionSamples(id, *tag->getWaitingAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagCrowdAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getCrowdAversion()) return reject(std::format(
			"Agent tag #{} already has Crowd aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!crowdAversionAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setCrowdAversion({ DefaultAgentCrowdAversionRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagCrowdAversionSamples(id, *tag->getCrowdAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagRiskAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getRiskAversion()) return reject(std::format(
			"Agent tag #{} already has Risk aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!riskAversionAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRiskAversion({ DefaultAgentRiskAversionRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRiskAversionSamples(id, *tag->getRiskAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagRouteFamiliarity(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getRouteFamiliarity()) return reject(std::format(
			"Agent tag #{} already has Route familiarity", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!routeFamiliarityAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRouteFamiliarity({ DefaultAgentRouteFamiliarityRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRouteFamiliaritySamples(id, *tag->getRouteFamiliarity());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagRoutePersistence(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getRoutePersistence()) return reject(std::format(
			"Agent tag #{} already has Route persistence", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!routePersistenceAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRoutePersistence({ DefaultAgentRoutePersistenceRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRoutePersistenceSamples(id, *tag->getRoutePersistence());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagMinimumRoutePlanningTime(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getMinimumRoutePlanningTime()) return reject(std::format(
			"Agent tag #{} already has Minimum route planning time", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!minimumRoutePlanningTimeAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setMinimumRoutePlanningTime({ DefaultAgentMinimumRoutePlanningTimeRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagMinimumRoutePlanningTimeSamples(id, *tag->getMinimumRoutePlanningTime());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagMaximumRoutePlanningTime(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getMaximumRoutePlanningTime()) return reject(std::format(
			"Agent tag #{} already has Maximum route planning time", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!maximumRoutePlanningTimeAdditionIsValid(id, diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setMaximumRoutePlanningTime({ DefaultAgentMaximumRoutePlanningTimeRange, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagMaximumRoutePlanningTimeSamples(id, *tag->getMaximumRoutePlanningTime());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AgentObjectUsageProperty const* AgentTagRegistry::getAgentTagObjectUsage(AgentTagId id) const
	{ auto const* tag = lookupAgentTag(id); return tag ? tag->getObjectUsage() : nullptr; }
	AgentObjectUsageDistanceProperty const* AgentTagRegistry::getAgentTagObjectUsageDistance(AgentTagId id) const
	{ auto const* tag = lookupAgentTag(id); return tag ? tag->getObjectUsageDistance() : nullptr; }

	bool AgentTagRegistry::editObjectUsageProperty(AgentTagId id, bool distanceProperty,
		ObjectUsagePropertyEdit operation, ObjectUsage mode, float distance, std::string* diagnostic)
	{
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject("Agent tag is not defined in this registry");
		bool present = distanceProperty ? tag->getObjectUsageDistance() != nullptr : tag->getObjectUsage() != nullptr;
		if ((operation == ObjectUsagePropertyEdit::Add && present)
			|| (operation != ObjectUsagePropertyEdit::Add && !present))
			return reject("Object usage property is already present or absent");
		if (!distanceProperty && !isValidObjectUsage(mode))
			return reject("Object usage must be Arms, None or Remote control");
		if (operation == ObjectUsagePropertyEdit::Set && (distanceProperty ? tag->getObjectUsageDistance()->value == distance
			: tag->getObjectUsage()->value == mode)) return reject("Object usage property is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		// Validate a separate prospective definition set, including masked conflicts
		// and effective configurations in every loaded World, before allocating a revision.
		auto prospective = copyWithNewUuid(*this);
		auto apply = [&](AgentTag& target, uint64_t revision)
		{
			if (distanceProperty)
			{
				if (operation == ObjectUsagePropertyEdit::Remove) target.removeObjectUsageDistance();
				else target.setObjectUsageDistance({ distance, revision });
			}
			else
			{
				if (operation == ObjectUsagePropertyEdit::Remove) target.removeObjectUsage();
				else target.setObjectUsage({ mode, revision });
			}
		};
		apply(*prospective->mTags.find(id), mNextPropertyRevision);
		if (!loadedWorldAssignmentsAreValid(*prospective, diagnostic)) return false;
		struct Change { World* world; AgentId agent; ObjectUsage mode; float distance; };
		std::vector<Change> changes;
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id))
					changes.push_back({ world, agentId, agent->getObjectUsage(), agent->getObjectUsageDistance() });
		uint64_t revision{ 0 };
		try { if (operation != ObjectUsagePropertyEdit::Remove) revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		apply(*tag, revision);
		for (auto const& change : changes)
			change.world->agentObjectUsageChanged(change.agent, change.mode, change.distance);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagObjectUsage(AgentTagId id, std::string* out)
	{ return editObjectUsageProperty(id, false, ObjectUsagePropertyEdit::Add, ObjectUsage::Arms, .25f, out); }
	bool AgentTagRegistry::setAgentTagObjectUsage(AgentTagId id, ObjectUsage value, std::string* out)
	{ return editObjectUsageProperty(id, false, ObjectUsagePropertyEdit::Set, value, .25f, out); }
	bool AgentTagRegistry::removeAgentTagObjectUsage(AgentTagId id, std::string* out)
	{ return editObjectUsageProperty(id, false, ObjectUsagePropertyEdit::Remove, ObjectUsage::Arms, .25f, out); }
	bool AgentTagRegistry::addAgentTagObjectUsageDistance(AgentTagId id, std::string* out)
	{ return editObjectUsageProperty(id, true, ObjectUsagePropertyEdit::Add, ObjectUsage::Arms, .25f, out); }
	bool AgentTagRegistry::setAgentTagObjectUsageDistance(AgentTagId id, float value, std::string* out)
	{ return editObjectUsageProperty(id, true, ObjectUsagePropertyEdit::Set, ObjectUsage::Arms, value, out); }
	bool AgentTagRegistry::removeAgentTagObjectUsageDistance(AgentTagId id, std::string* out)
	{ return editObjectUsageProperty(id, true, ObjectUsagePropertyEdit::Remove, ObjectUsage::Arms, .25f, out); }

	bool AgentTagRegistry::addAgentTagPermissionAdherence(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getPermissionAdherence()) return reject(std::format(
			"Agent tag #{} already has Permission adherence", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!permissionAdherenceAdditionIsValid(id, diagnostic)) return false;
		try { tag->setPermissionAdherence({ true, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::addAgentTagRemoteAccessPanels(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getRemoteAccessPanels()) return reject(std::format(
			"Agent tag #{} already has Remote Access panels", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!remoteAccessPanelsAdditionIsValid(id, diagnostic)) return false;
		try { tag->setRemoteAccessPanels({ true, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::addAgentTagRemoteBoothWindowShutters(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (tag->getRemoteBoothWindowShutters()) return reject(std::format(
			"Agent tag #{} already has Remote BoothWindow shutters", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!remoteBoothWindowShuttersAdditionIsValid(id, diagnostic)) return false;
		try { tag->setRemoteBoothWindowShutters({ true, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagPermissionAdherence(AgentTagId id, bool value,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getPermissionAdherence();
		if (!current) return reject(std::format("Agent tag #{} has no Permission adherence", tag->getName()));
		if (current->value == value) return reject("The Agent Permission adherence is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		try { tag->setPermissionAdherence({ value, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id) && !agent->getIndividualPermissionAdherence())
				{
					if (value) world->replanAgentAfterAuthorizationRefusal(agentId);
					else world->beginVoluntaryRoutePlanning(agentId);
				}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::setAgentTagRemoteAccessPanels(AgentTagId id, bool value,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getRemoteAccessPanels();
		if (!current) return reject(std::format("Agent tag #{} has no Remote Access panels", tag->getName()));
		if (current->value == value) return reject("The Agent Remote Access panels is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		try { tag->setRemoteAccessPanels({ value, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id) && !agent->getIndividualRemoteAccessPanels())
				{
					world->mSimulationCoordinator.agentObjectUsageChanged(agentId);
				}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::setAgentTagRemoteBoothWindowShutters(AgentTagId id, bool value,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getRemoteBoothWindowShutters();
		if (!current) return reject(std::format("Agent tag #{} has no Remote BoothWindow shutters", tag->getName()));
		if (current->value == value) return reject("The Agent Remote BoothWindow shutters is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		try { tag->setRemoteBoothWindowShutters({ value, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id) && !agent->getIndividualRemoteBoothWindowShutters())
				{
					world->mSimulationCoordinator.agentObjectUsageChanged(agentId);
				}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagPermissionAdherence(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getPermissionAdherence()) return reject(std::format(
			"Agent tag #{} has no Permission adherence", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		auto const adherenceChanged = !tag->getPermissionAdherence()->value;
		tag->removePermissionAdherence();
		if (adherenceChanged)
			for (auto* world : mLoadedWorlds) if (world)
				for (auto const& [agentId, agent] : world->mAgents.entries())
					if (agent && agent->hasAgentTag(id) && !agent->getIndividualPermissionAdherence())
						world->replanAgentAfterAuthorizationRefusal(agentId);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::removeAgentTagRemoteAccessPanels(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getRemoteAccessPanels()) return reject(std::format(
			"Agent tag #{} has no Remote Access panels", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		tag->removeRemoteAccessPanels();
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id) && !agent->getIndividualRemoteAccessPanels())
					world->mSimulationCoordinator.agentObjectUsageChanged(agentId);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool AgentTagRegistry::removeAgentTagRemoteBoothWindowShutters(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getRemoteBoothWindowShutters()) return reject(std::format(
			"Agent tag #{} has no Remote BoothWindow shutters", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		tag->removeRemoteBoothWindowShutters();
		for (auto* world : mLoadedWorlds) if (world)
			for (auto const& [agentId, agent] : world->mAgents.entries())
				if (agent && agent->hasAgentTag(id) && !agent->getIndividualRemoteBoothWindowShutters())
					world->mSimulationCoordinator.agentObjectUsageChanged(agentId);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagMobilityProfile(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		if (tag->getMobilityProfile()) return reject(std::format(
			"Agent tag #{} already has Mobility profile", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		if (!mobilityProfileAdditionIsValid(id, diagnostic)) return false;
		try { tag->setMobilityProfile({ {}, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagEffortAversion(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getEffortAversion();
		if (!current) return reject(std::format("Agent tag #{} has no Effort aversion", tag->getName()));
		if (!agentEffortAversionRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Effort aversion range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setEffortAversion({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagEffortAversionSamples(id, *tag->getEffortAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagWaitingAversion(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getWaitingAversion();
		if (!current) return reject(std::format("Agent tag #{} has no Waiting aversion", tag->getName()));
		if (!agentWaitingAversionRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Waiting aversion range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWaitingAversion({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagWaitingAversionSamples(id, *tag->getWaitingAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagCrowdAversion(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getCrowdAversion();
		if (!current) return reject(std::format("Agent tag #{} has no Crowd aversion", tag->getName()));
		if (!agentCrowdAversionRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Crowd aversion range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setCrowdAversion({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagCrowdAversionSamples(id, *tag->getCrowdAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagRiskAversion(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getRiskAversion();
		if (!current) return reject(std::format("Agent tag #{} has no Risk aversion", tag->getName()));
		if (!agentRiskAversionRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Risk aversion range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRiskAversion({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRiskAversionSamples(id, *tag->getRiskAversion());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagRouteFamiliarity(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getRouteFamiliarity();
		if (!current) return reject(std::format("Agent tag #{} has no Route familiarity", tag->getName()));
		if (!agentRouteFamiliarityRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Route familiarity range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRouteFamiliarity({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRouteFamiliaritySamples(id, *tag->getRouteFamiliarity());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagRoutePersistence(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getRoutePersistence();
		if (!current) return reject(std::format("Agent tag #{} has no Route persistence", tag->getName()));
		if (!agentRoutePersistenceRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Route persistence range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setRoutePersistence({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagRoutePersistenceSamples(id, *tag->getRoutePersistence());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagMinimumRoutePlanningTime(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getMinimumRoutePlanningTime();
		if (!current) return reject(std::format("Agent tag #{} has no Minimum route planning time", tag->getName()));
		if (!agentMinimumRoutePlanningTimeRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Minimum route planning time range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setMinimumRoutePlanningTime({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagMinimumRoutePlanningTimeSamples(id, *tag->getMinimumRoutePlanningTime());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagMaximumRoutePlanningTime(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getMaximumRoutePlanningTime();
		if (!current) return reject(std::format("Agent tag #{} has no Maximum route planning time", tag->getName()));
		if (!agentMaximumRoutePlanningTimeRangeIsValid(range, diagnostic)) return false;
		if (current->range == range) return reject("The Agent Maximum route planning time range is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setMaximumRoutePlanningTime({ range, revision });
		for (auto* world : mLoadedWorlds)
			if (world) world->addAgentTagMaximumRoutePlanningTimeSamples(id, *tag->getMaximumRoutePlanningTime());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagMobilityProfile(AgentTagId id,
		MobilityProfile value, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getMobilityProfile();
		if (!current) return reject(std::format(
			"Agent tag #{} has no Mobility profile", tag->getName()));
		if (!mobilityProfileIsValid(value))
			return reject("The Agent Mobility profile contains an invalid Mobility use");
		if (current->value == value)
			return reject("The Agent Mobility profile is unchanged");
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		try { tag->setMobilityProfile({ value, allocatePropertyRevision() }); }
		catch (std::exception const& error) { return reject(error.what()); }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagEffortAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getEffortAversion()) return reject(std::format(
			"Agent tag #{} has no Effort aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagEffortAversionSamples(id);
		tag->removeEffortAversion();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagWaitingAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getWaitingAversion()) return reject(std::format(
			"Agent tag #{} has no Waiting aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagWaitingAversionSamples(id);
		tag->removeWaitingAversion();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagCrowdAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getCrowdAversion()) return reject(std::format(
			"Agent tag #{} has no Crowd aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagCrowdAversionSamples(id);
		tag->removeCrowdAversion();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagRiskAversion(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getRiskAversion()) return reject(std::format(
			"Agent tag #{} has no Risk aversion", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagRiskAversionSamples(id);
		tag->removeRiskAversion();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagRouteFamiliarity(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getRouteFamiliarity()) return reject(std::format(
			"Agent tag #{} has no Route familiarity", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagRouteFamiliaritySamples(id);
		tag->removeRouteFamiliarity();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagRoutePersistence(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getRoutePersistence()) return reject(std::format(
			"Agent tag #{} has no Route persistence", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagRoutePersistenceSamples(id);
		tag->removeRoutePersistence();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagMinimumRoutePlanningTime(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getMinimumRoutePlanningTime()) return reject(std::format(
			"Agent tag #{} has no Minimum route planning time", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagMinimumRoutePlanningTimeSamples(id);
		tag->removeMinimumRoutePlanningTime();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagMaximumRoutePlanningTime(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!tag->getMaximumRoutePlanningTime()) return reject(std::format(
			"Agent tag #{} has no Maximum route planning time", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto* world : mLoadedWorlds)
			if (world) world->clearAgentTagMaximumRoutePlanningTimeSamples(id);
		tag->removeMaximumRoutePlanningTime();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagMobilityProfile(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag) return reject(std::format(
			"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getMobilityProfile()) return reject(std::format(
			"Agent tag #{} has no Mobility profile", tag->getName()));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		try { (void)allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->removeMobilityProfile();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::loadedWorldAssignmentsAreValid(
		AgentTagRegistry const& definitions, std::string* diagnostic,
		std::vector<World const*> const& excludedWorlds) const
	{
		for (auto const* world : mLoadedWorlds)
		{
			if (!world || std::find(excludedWorlds.begin(), excludedWorlds.end(),
				world) != excludedWorlds.end()) continue;
			if (!world->agentTagAssignmentsAreValid(definitions, diagnostic))
				return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::childrenModified() const
	{
		return false;
	}

	void AgentTagRegistry::serializeImpl(Serializer& serializer, SerializationWorkData&) const
	{
		if (!uuidIsValid(mUuid))
		{
			throw SerializationException("Cannot serialize an Agent tag registry with an invalid UUID");
		}
		serializer.beginMap("agentTagRegistry");
		serializer.writeUint32("version", 17);
		serializer.writeString("uuid", mUuid);
		serializer.writeUint64("nextAgentTagId", mTags.nextId());
		serializer.writeUint64("nextPropertyRevision", mNextPropertyRevision);
		serializer.beginArray("tags");
		// Identity order is deliberately independent of the alphabetical order
		// used by the panel, so a rename never moves a serialized definition.
		for (auto const& [id, tag] : mTags.entries())
		{
			serializer.beginMap("");
			serializer.writeUint64("id", id.value);
			serializer.writeString("name", tag->getName());
			serializer.beginMap("displayColour");
			serializer.writeUint8("r", tag->getDisplayColour().r);
			serializer.writeUint8("g", tag->getDisplayColour().g);
			serializer.writeUint8("b", tag->getDisplayColour().b);
			serializer.endMap();
			auto const* colour = tag->getColour();
			auto const* walkSpeed = tag->getWalkSpeedModifier();
			auto const* height = tag->getHeightModifier();
			auto const* stairSpeed = tag->getStairSpeedModifier();
			auto const* ladderSpeed = tag->getLadderSpeedModifier();
			auto const* interaction = tag->getInteractionAversion();
			auto const* effort = tag->getEffortAversion();
			auto const* waiting = tag->getWaitingAversion();
			auto const* crowd = tag->getCrowdAversion();
			auto const* risk = tag->getRiskAversion();
			auto const* familiarity = tag->getRouteFamiliarity();
			auto const* persistence = tag->getRoutePersistence();
			auto const* minimumPlanningTime = tag->getMinimumRoutePlanningTime();
			auto const* maximumPlanningTime = tag->getMaximumRoutePlanningTime();
			auto const* chance = tag->getEscalatorWalkingChance();
			auto const* usage = tag->getObjectUsage();
			auto const* distance = tag->getObjectUsageDistance();
			auto const* adherence = tag->getPermissionAdherence();
			auto const* remotePanels = tag->getRemoteAccessPanels();
			auto const* remoteShutters = tag->getRemoteBoothWindowShutters();
			auto const* mobility = tag->getMobilityProfile();
			if (colour || walkSpeed || height || stairSpeed || ladderSpeed || interaction || effort || waiting || crowd || risk || familiarity || persistence || minimumPlanningTime || maximumPlanningTime || chance || usage || distance || adherence || remotePanels || remoteShutters || mobility)
			{
				serializer.beginArray("properties");
				if (chance)
				{
					serializer.beginMap("");
					serializer.writeString("type", "escalatorWalkingChance");
					serializer.writeUint64("revision", chance->revision);
					serializer.writeFloat("value", chance->value);
					serializer.endMap();
				}
				if (colour)
				{
					serializer.beginMap("");
					serializer.writeString("type", "colour");
					serializer.writeUint64("revision", colour->revision);
					serializer.writeUint8("r", colour->value.r);
					serializer.writeUint8("g", colour->value.g);
					serializer.writeUint8("b", colour->value.b);
					serializer.endMap();
				}
				auto writeModifier = [&serializer](char const* type,
					auto const& modifier)
				{
					serializer.beginMap("");
					serializer.writeString("type", type);
					serializer.writeUint64("revision", modifier.revision);
					serializer.writeFloat("min", modifier.range.minimum);
					serializer.writeFloat("max", modifier.range.maximum);
					serializer.endMap();
				};
				if (walkSpeed) writeModifier("walkSpeedModifier", *walkSpeed);
				if (height) writeModifier("heightModifier", *height);
				if (stairSpeed) writeModifier("stairSpeedModifier", *stairSpeed);
				if (ladderSpeed) writeModifier("ladderSpeedModifier", *ladderSpeed);
				if (interaction) writeModifier("interactionAversion", *interaction);
				if (effort) writeModifier("effortAversion", *effort);
				if (waiting) writeModifier("waitingAversion", *waiting);
				if (crowd) writeModifier("crowdAversion", *crowd);
				if (risk) writeModifier("riskAversion", *risk);
				if (familiarity) writeModifier("routeFamiliarity", *familiarity);
				if (persistence) writeModifier("routePersistence", *persistence);
				if (minimumPlanningTime) writeModifier("minimumRoutePlanningTime", *minimumPlanningTime);
				if (maximumPlanningTime) writeModifier("maximumRoutePlanningTime", *maximumPlanningTime);
				if (usage)
				{
					serializer.beginMap("");
					serializer.writeString("type", "objectUsage");
					serializer.writeUint64("revision", usage->revision);
					serializer.writeString("value", objectUsageWireName(usage->value));
					serializer.endMap();
				}
				if (distance)
				{
					serializer.beginMap("");
					serializer.writeString("type", "objectUsageDistance");
					serializer.writeUint64("revision", distance->revision);
					serializer.writeFloat("value", distance->value);
					serializer.endMap();
				}
				if (adherence)
				{
					serializer.beginMap("");
					serializer.writeString("type", "permissionAdherence");
					serializer.writeUint64("revision", adherence->revision);
					serializer.writeBool("value", adherence->value);
					serializer.endMap();
				}
				if (remotePanels)
				{
					serializer.beginMap("");
					serializer.writeString("type", "remoteAccessPanels");
					serializer.writeUint64("revision", remotePanels->revision);
					serializer.writeBool("value", remotePanels->value);
					serializer.endMap();
				}
				if (remoteShutters)
				{
					serializer.beginMap("");
					serializer.writeString("type", "remoteBoothWindowShutters");
					serializer.writeUint64("revision", remoteShutters->revision);
					serializer.writeBool("value", remoteShutters->value);
					serializer.endMap();
				}
				if (mobility)
				{
					serializer.beginMap("");
					serializer.writeString("type", "mobilityProfile");
					serializer.writeUint64("revision", mobility->revision);
					serializeMobilityProfile(serializer, mobility->value);
					serializer.endMap();
				}
				serializer.endArray();
			}
			serializer.endMap();
		}
		serializer.endArray();
		serializer.endMap();
	}

	bool AgentTagRegistry::deserializeImpl(Serializer& serializer, SerializationWorkData&)
	{
		serializer.beginMap("agentTagRegistry");
		auto const version = serializer.readUint32("version");
		if (version < 1 || version > 17)
		{
			throw SerializationException("Unsupported Agent tag registry serialization version");
		}
		auto uuid = serializer.readString("uuid");
		if (!uuidIsValid(uuid))
		{
			throw SerializationException("Agent tag registry UUID is invalid");
		}
		auto const nextAgentTagId = serializer.readUint64("nextAgentTagId");
		auto const nextPropertyRevision = serializer.readUint64("nextPropertyRevision");
		if (nextPropertyRevision == 0)
		{
			throw SerializationException("Agent tag property revision allocator cannot be zero");
		}

		EntityRegistry<AgentTagId, AgentTag> tags;
		std::set<std::string> names;
		std::set<uint64_t> propertyRevisions;
		uint64_t greatestPropertyRevision{ 0 };
		bool backfilledDisplayColour{ false };
		serializer.beginArray("tags");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const id = AgentTagId{ serializer.readUint64("id") };
			auto name = serializer.readString("name");
			auto tag = AgentTag::create(name);
			// The display Colour is intrinsic tag data, not an Agent property: it
			// has no revision and never participates in inheritance conflicts.
			// Documents authored before it existed load without the field; those
			// tags keep the random pastel assigned at creation and the registry is
			// marked modified so the backfill is persisted. Editor snapshots and
			// undo/redo always carry the field, so restores stay exact.
			if (serializer.hasField("displayColour"))
			{
				serializer.beginMap("displayColour");
				tag->setDisplayColour({ serializer.readUint8("r"),
					serializer.readUint8("g"), serializer.readUint8("b") });
				serializer.endMap();
			}
			else backfilledDisplayColour = true;
			if (serializer.hasField("properties"))
			{
				bool hasColour{ false };
				bool hasWalkSpeedModifier{ false };
				bool hasHeightModifier{ false };
				bool hasStairSpeedModifier{ false };
				bool hasLadderSpeedModifier{ false };
				bool hasInteractionAversion{ false };
				bool hasEffortAversion{ false };
				bool hasWaitingAversion{ false };
				bool hasCrowdAversion{ false };
				bool hasRiskAversion{ false };
				bool hasRouteFamiliarity{ false };
				bool hasRoutePersistence{ false };
				bool hasMinimumRoutePlanningTime{ false };
				bool hasMaximumRoutePlanningTime{ false };
				bool hasPermissionAdherence{ false };
				bool hasRemoteAccessPanels{ false };
				bool hasRemoteBoothWindowShutters{ false };
				bool hasMobilityProfile{ false };
				serializer.beginArray("properties");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					auto const type = serializer.readString("type");
					if (type != "colour" && type != "walkSpeedModifier"
						&& type != "heightModifier" && type != "escalatorWalkingChance"
						&& !(version >= 2 && type == "mobilityProfile")
						&& !(version >= 3 && type == "interactionAversion")
						&& !(version >= 4 && type == "stairSpeedModifier")
						&& !(version >= 5 && type == "effortAversion")
						&& !(version >= 6 && type == "waitingAversion")
						&& !(version >= 7 && type == "crowdAversion")
						&& !(version >= 8 && type == "ladderSpeedModifier")
						&& !(version >= 10 && type == "riskAversion")
						&& !(version >= 11 && type == "routeFamiliarity")
						&& !(version >= 12 && type == "routePersistence")
						&& !(version >= 13 && type == "minimumRoutePlanningTime")
						&& !(version >= 13 && type == "maximumRoutePlanningTime")
						&& !(version >= 14 && type == "permissionAdherence")
						&& !(version >= 16 && type == "remoteAccessPanels")
						&& !(version >= 17 && type == "remoteBoothWindowShutters")
						&& !(version >= 15 && (type == "objectUsage" || type == "objectUsageDistance")))
					{
						throw SerializationException(std::format(
							"Unsupported Agent property type '{}'", type));
					}
					if (type == "colour" && hasColour)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Colour", name));
					if (type == "walkSpeedModifier" && hasWalkSpeedModifier)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Walk speed modifier",
							name));
					if (type == "heightModifier" && hasHeightModifier)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Height modifier",
							name));
					if (type == "stairSpeedModifier" && hasStairSpeedModifier)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Stair speed modifier", name));
					if (type == "ladderSpeedModifier" && hasLadderSpeedModifier)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Ladder speed modifier", name));
					if (type == "interactionAversion" && hasInteractionAversion)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Interaction aversion", name));
					if (type == "effortAversion" && hasEffortAversion)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Effort aversion", name));
					if (type == "waitingAversion" && hasWaitingAversion)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Waiting aversion", name));
					if (type == "crowdAversion" && hasCrowdAversion)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Crowd aversion", name));
					if (type == "riskAversion" && hasRiskAversion)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Risk aversion", name));
					if (type == "routeFamiliarity" && hasRouteFamiliarity)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Route familiarity", name));
					if (type == "routePersistence" && hasRoutePersistence)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Route persistence", name));
					if (type == "minimumRoutePlanningTime" && hasMinimumRoutePlanningTime)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Minimum route planning time", name));
					if (type == "maximumRoutePlanningTime" && hasMaximumRoutePlanningTime)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Maximum route planning time", name));
					if (type == "permissionAdherence" && hasPermissionAdherence)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Permission adherence", name));
					if (type == "remoteAccessPanels" && hasRemoteAccessPanels)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Remote Access panels", name));
					if (type == "remoteBoothWindowShutters" && hasRemoteBoothWindowShutters)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Remote BoothWindow shutters", name));
					if (type == "mobilityProfile" && hasMobilityProfile)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Mobility profile",
							name));
					auto const revision = serializer.readUint64("revision");
					if (revision == 0)
						throw SerializationException(
							"Serialized Agent property revision cannot be zero");

					if (type == "escalatorWalkingChance")
					{
						auto const value = serializer.readFloat("value");
						if (tag->getEscalatorWalkingChance() || !agentEscalatorWalkingChanceIsValid(value))
							throw SerializationException("Duplicate or invalid Escalator walking chance");
						tag->setEscalatorWalkingChance({ value, revision });
					}
					else if (type == "colour")
					{
						AgentColour const colour{
							serializer.readUint8("r"), serializer.readUint8("g"),
							serializer.readUint8("b") };
						tag->setColour({ colour, revision });
						hasColour = true;
					}
					else if (type == "objectUsage")
					{
						auto value = serializer.readString("value");
						ObjectUsage mode;
						if (tag->getObjectUsage() || !parseObjectUsage(value, mode))
							throw SerializationException("Duplicate or invalid Object usage");
						tag->setObjectUsage({ mode, revision });
					}
					else if (type == "objectUsageDistance")
					{
						if (tag->getObjectUsageDistance()) throw SerializationException("Duplicate Object usage distance");
						tag->setObjectUsageDistance({ serializer.readFloat("value"), revision });
					}
					else if (type == "permissionAdherence")
					{
						tag->setPermissionAdherence({ serializer.readBool("value"), revision });
						hasPermissionAdherence = true;
					}
					else if (type == "remoteAccessPanels")
					{
						tag->setRemoteAccessPanels({ serializer.readBool("value"), revision });
						hasRemoteAccessPanels = true;
					}
					else if (type == "remoteBoothWindowShutters")
					{
						tag->setRemoteBoothWindowShutters({ serializer.readBool("value"), revision });
						hasRemoteBoothWindowShutters = true;
					}
					else if (type == "mobilityProfile")
					{
						auto const value = deserializeMobilityProfile(serializer);
						tag->setMobilityProfile({ value, revision });
						hasMobilityProfile = true;
					}
					else
					{
						AgentModifierRange const range{
							serializer.readFloat("min"), serializer.readFloat("max") };
						std::string rangeDiagnostic;
						if (type == "walkSpeedModifier")
						{
							if (!agentWalkSpeedModifierRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Walk speed modifier range is invalid: "
									+ rangeDiagnostic);
							tag->setWalkSpeedModifier({ range, revision });
							hasWalkSpeedModifier = true;
						}
						else if (type == "heightModifier")
						{
							if (!agentHeightModifierRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Height modifier range is invalid: " + rangeDiagnostic);
							tag->setHeightModifier({ range, revision });
							hasHeightModifier = true;
						}
						else if (type == "stairSpeedModifier")
						{
							if (!agentStairSpeedModifierRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Stair speed modifier range is invalid: " + rangeDiagnostic);
							tag->setStairSpeedModifier({ range, revision });
							hasStairSpeedModifier = true;
						}
						else if (type == "ladderSpeedModifier")
						{
							if (!agentLadderSpeedModifierRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Ladder speed modifier range is invalid: " + rangeDiagnostic);
							tag->setLadderSpeedModifier({ range, revision });
							hasLadderSpeedModifier = true;
						}
						else if (type == "interactionAversion")
						{
							if (!agentInteractionAversionRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Interaction aversion range is invalid: " + rangeDiagnostic);
							tag->setInteractionAversion({ range, revision });
							hasInteractionAversion = true;
						}
						else if (type == "effortAversion")
						{
							if (!agentEffortAversionRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Effort aversion range is invalid: " + rangeDiagnostic);
							tag->setEffortAversion({ range, revision });
							hasEffortAversion = true;
						}
						else if (type == "waitingAversion")
						{
							if (!agentWaitingAversionRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Waiting aversion range is invalid: " + rangeDiagnostic);
							tag->setWaitingAversion({ range, revision });
							hasWaitingAversion = true;
						}
						else if (type == "crowdAversion")
						{
							if (!agentCrowdAversionRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Crowd aversion range is invalid: " + rangeDiagnostic);
							tag->setCrowdAversion({ range, revision });
							hasCrowdAversion = true;
						}
						else if (type == "riskAversion")
						{
							if (!agentRiskAversionRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Risk aversion range is invalid: " + rangeDiagnostic);
							tag->setRiskAversion({ range, revision });
							hasRiskAversion = true;
						}
						else if (type == "routeFamiliarity")
						{
							if (!agentRouteFamiliarityRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Route familiarity range is invalid: " + rangeDiagnostic);
							tag->setRouteFamiliarity({ range, revision });
							hasRouteFamiliarity = true;
						}
						else if (type == "routePersistence")
						{
							if (!agentRoutePersistenceRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Route persistence range is invalid: " + rangeDiagnostic);
							tag->setRoutePersistence({ range, revision });
							hasRoutePersistence = true;
						}
						else if (type == "minimumRoutePlanningTime")
						{
							if (!agentMinimumRoutePlanningTimeRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Minimum route planning time range is invalid: " + rangeDiagnostic);
							tag->setMinimumRoutePlanningTime({ range, revision });
							hasMinimumRoutePlanningTime = true;
						}
						else
						{
							if (!agentMaximumRoutePlanningTimeRangeIsValid(range, &rangeDiagnostic))
								throw SerializationException(
									"Serialized Maximum route planning time range is invalid: " + rangeDiagnostic);
							tag->setMaximumRoutePlanningTime({ range, revision });
							hasMaximumRoutePlanningTime = true;
						}
					}
					serializer.endMap();
					if (!propertyRevisions.insert(revision).second)
						throw SerializationException(std::format(
							"Serialized Agent property revision {} is reused", revision));
					greatestPropertyRevision = std::max(greatestPropertyRevision, revision);
				}
				serializer.endArray();
			}
			serializer.endMap();

			if (!id) throw SerializationException("Serialized Agent tag ID cannot be zero");
			std::string diagnostic;
			if (!AgentTag::nameIsValid(name, &diagnostic))
				throw SerializationException("Serialized Agent tag name is invalid: " + diagnostic);
			if (!names.insert(name).second)
				throw SerializationException(std::format(
					"Serialized Agent tag names must be unique (#{} appears twice)", name));
			if (!tags.restore(id, std::move(tag)))
				throw SerializationException(std::format(
					"Serialized Agent tag IDs must be unique ({} appears twice)", id.value));
		}
		serializer.endArray();
		serializer.endMap();

		if (!tags.restoreNextId(nextAgentTagId))
		{
			throw SerializationException(
				"Agent tag allocator must be above every serialized Agent tag ID");
		}
		if (greatestPropertyRevision >= nextPropertyRevision)
		{
			throw SerializationException(
				"Agent property revision allocator must be above every serialized property revision");
		}

		// Commit only after the complete document has passed validation. This is
		// also what makes undo restoration transactional on the shared instance.
		mUuid = std::move(uuid);
		mTags = std::move(tags);
		// A history snapshot may legitimately restore an older property revision,
		// but revisions already issued by this live registry must never become
		// available again. Freshly loaded registries start at one, so ordinary load
		// still adopts the persisted high-water mark exactly.
		mNextPropertyRevision = std::max(mNextPropertyRevision, nextPropertyRevision);
		if (backfilledDisplayColour) mBackfilledDisplayColour = true;
		return true;
	}

	void AgentTagRegistry::saveTo(std::string const& filepath)
	{
		auto const path = normalizedDocumentPath(filepath);
		if (mDocumentPath)
		{
			if (path != *mDocumentPath)
			{
				throw SerializationException(std::format(
					"Agent tag registry is loaded from {}, not {}",
					mDocumentPath->string(), path.string()));
			}
			if (fileHasExternalChanges(path.string()))
			{
				throw SerializationException(std::format(
					"Agent tag registry {} changed outside the editor; reload it before saving",
					path.string()));
			}
		}

		auto serializer = YamlSerializer::toFile(path.string());
		SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		serialize(*serializer, workData);
		serializer->serialize();
		// Record exactly what reached disk. If this read is refused, retain dirty
		// state rather than claiming a revision that cannot be checked later.
		auto contents = readDocument(path);
		mDocumentPath = path;
		mSavedDocumentContents = std::move(contents);
		markUnmodified();
	}
}
