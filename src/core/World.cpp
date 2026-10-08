#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Defines.h"
#include "core/World.h"
#include "core/ChamberTransit.h"
#include "core/AirlockTransit.h"
#include "core/RestorationTiming.h"
#include "core/OccupantPacking.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/WorldDocument.h"
#include "core/AgentTagRegistry.h"
#include "core/Background.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/SectorObjectType.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/PlatformLift.h"
#include "core/BulkheadDoor.h"
#include "core/DoorEdge.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"


namespace core
{
	namespace
	{
		// An external registry/catalogue reference is an application Resource name
		// (ADR 0010), not a file path: a non-empty name with no directory part.
		void requireCatalogResourceName(std::string const& resourceName, char const* kind)
		{
			std::filesystem::path const path(resourceName);
			if (resourceName.empty() || path.is_absolute() || path.has_parent_path()
				|| path.filename().string() != resourceName)
				throw std::invalid_argument(std::string("A ") + kind
					+ " reference must be a Resource name");
		}
	}


	using namespace std;

	// The shuttle door-offset helper - expanding a carriage's door mask into the cells
	// its doors occupy - lives in SimulationCoordinator with the shuttle door
	// assignment family that consumes that indexing (ADR 0004).

	World::CreateDoorOptions World::ManualDoor1Options{ 1, Door::Height::Regular, { false, false }, DoorActivationMode::Manual };
	World::CreateDoorOptions World::RemoteControlledDoor1Options{ 1, Door::Height::Regular, { true, true }, DoorActivationMode::RemoteControlled };
	World::CreateDoorOptions World::UnavailableDoor1Options{ 1, Door::Height::Regular, { false, false }, DoorActivationMode::Unavailable };
	World::CreateDoorOptions World::ManualDoor2Options{ 2, Door::Height::Regular, { false, false }, DoorActivationMode::Manual };
	World::CreateDoorOptions World::RemoteControlledDoor2Options{ 2, Door::Height::Regular, { true, true }, DoorActivationMode::RemoteControlled };
	World::CreateDoorOptions World::UnavailableDoor2Options{ 2, Door::Height::Regular, { false, false }, DoorActivationMode::Unavailable };

	/*
	World
	--------

	This class essentially holds a game map, with all the sub-structures within it.
	
	It is responsible for map creation, acting as a facade for sub-structures such as
	Sector, Location and SectorObject.
	
	It also generates a Graph which is the master path-finding source.  While each Agent
	may have their own internal Graph, World's is the one which these are initially
	generated from.

	A World has two Layers, and there is quite a bit of hard-coding and reliance around
	this, which is to say that increasing to three or more would be a lot of work.

	One important concept to bear in mind is the API difference between "y" and "levelIndex".
	"y" is used as an absolute value within the Layer, whereas "levelIndex" is used as an absolute
	value within a Sector, ie it is relative to a Sector's base y offset within the Layer.

	Worlds are created piece by piece, and must be valid at every stage of their construction.
	There is no post-build validation, this happens after each construction command.

	Rules for creation of Worlds:
	
	There are two types of Sector: Locations and Transits

	Locations:
	- Locations can go on either Layer
	- Locations on different Layers connect to each other via Doors
	- Locations on the same Layer connect to each other via BulkheadDoors
	
	Transits:
	- Transits can only go on the Back Layer, and are designed to connect Locations

	Any object which connects Locations - eg Doors and Transits - need to be placed after the two
	Locations being connected are placed.

	Windows:
	- Windows can be placed on either Layer.  If they are placed on the Back Layer, they will show
	  space/the void.  The same if they are placed on the Fore Layer with no Sector behind them.

	Buttons etc which control objects (eg Doors)
	- These are placed on the right of the object by default, unless there is no space, in which case
	  they are placed on the left (as viewed by the player).

	*/

	World::World(string const& name, uint32_t cellsWide, uint32_t levelsHigh,
		AgentBehaviourRuntimeLimits behaviourRuntimeLimits,
		AgentTypeRuntimeLimits agentTypeRuntimeLimits)
		: mName(name)
		, mClipboardIdentity(AgentTagRegistry::create()->getUuid())
		, mCellsWide(cellsWide)
		, mLevelsHigh(levelsHigh)
		, mLayers(2)
		, mLayerNames{ defaultLayerName(0), defaultLayerName(1) }
		, mSimulationCoordinator(*this)
		, mAgentTypeRuntime(std::make_unique<AgentTypeRuntimeAdapter>(
			agentTypeRuntimeLimits))
		, mAgentBehaviourRuntime(std::make_unique<AgentBehaviourRuntimeAdapter>(
			behaviourRuntimeLimits))
	{
		// Refuse an unsupported size before any Layer is allocated or any cell
		// buffer is sized, so a wrapped product can never become an empty Layer
		// that still validates in-bounds reads (#184).
		string dimensionDiagnostic;
		if (!dimensionsAreSupported(cellsWide, levelsHigh,
			static_cast<uint32_t>(mLayers.size()), &dimensionDiagnostic))
		{
			throw WorldException(this, dimensionDiagnostic);
		}

		for (uint32_t i = 0; i < mLayers.size(); ++i)
		{
			mLayers[i] = make_shared<Layer>(this, cellsWide, levelsHigh, i);
		}

		for (uint32_t level = 0; level < levelsHigh; ++level)
			mLevelNames.push_back(format("Level {}", level));
		mGraph = make_shared<Graph>(this);
		registerBundledAgentTypes();
	}

	std::string const& World::getLevelName(uint32_t level) const
	{
		return mLevelNames.at(level);
	}

	void World::setLevelName(uint32_t level, std::string name)
	{
		if (name.empty()) throw WorldException(this, "Level name cannot be empty");
		mLevelNames.at(level) = std::move(name);
		modify();
	}

	bool World::dimensionsAreSupported(uint32_t cellsWide, uint32_t levelsHigh,
		uint32_t layerCount, string* diagnostic)
	{
		if (diagnostic) diagnostic->clear();

		auto const reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (cellsWide == 0 || levelsHigh == 0)
		{
			return reject("World dimensions must be positive");
		}

		if (levelsHigh > CORE_MAX_LEVELS)
			return reject(format("World Level count exceeds the {} Level limit", CORE_MAX_LEVELS));

		if (layerCount < 2 || layerCount > CORE_MAX_LAYERS)
		{
			return reject(format("World layer count {} is out of range", layerCount));
		}

		// One CellDefinition per (x, level) per Layer. Evaluating the product in
		// 64 bits keeps an overflowing pair from wrapping back under the limit.
		uint64_t const cellsPerLayer =
			static_cast<uint64_t>(cellsWide) * levelsHigh;
		uint64_t const totalCells = cellsPerLayer * layerCount;
		if (totalCells > CORE_MAX_WORLD_CELLS)
		{
			return reject(format(
				"World dimensions {} x {} over {} layers exceed the {} cell limit",
				cellsWide, levelsHigh, layerCount, CORE_MAX_WORLD_CELLS));
		}

		return true;
	}

	World::~World()
	{
		// Lua teardown needs the final World and Agent value views, so it runs
		// before registries and domain storage begin destruction. on_stop is
		// best-effort and cannot prevent the World from closing.
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::WorldClose);
		if (mAgentTagRegistry) mAgentTagRegistry->unregisterWorld(*this);
		if (mAgentBehaviourRegistry) mAgentBehaviourRegistry->unregisterWorld(*this);
	}

	string const& World::getName() const
	{
		return mName;
	}

	bool World::setRandomSeed(uint64_t seed, string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (diagnostic) diagnostic->clear();
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic =
				"Pause the simulation before changing the World random seed";
			return false;
		}
		if (mRandomSeed == seed)
		{
			if (diagnostic) *diagnostic = "The World random seed is unchanged";
			return false;
		}
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Reset);
		mRandomSeed = seed;
		for (auto const& [agentId, agent] : mAgents.entries())
			if (agent && agent->getBehaviourAssignment())
				mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
		modify();
		return true;
	}

	bool World::hasAgentTagRegistryReference() const
	{
		return mAgentTagRegistryReference.has_value();
	}

	bool World::hasAttachedAgentTagRegistry() const
	{
		return mAgentTagRegistry != nullptr;
	}

	string const& World::getAgentTagRegistryResourceName() const
	{
		if (!mAgentTagRegistryReference)
			throw runtime_error("The World has no Agent tag registry reference");
		return mAgentTagRegistryReference->resourceName;
	}

	string const& World::getExpectedAgentTagRegistryUuid() const
	{
		if (!mAgentTagRegistryReference)
			throw runtime_error("The World has no Agent tag registry reference");
		return mAgentTagRegistryReference->expectedUuid;
	}

	shared_ptr<AgentTagRegistry> const& World::getAgentTagRegistry() const
	{
		return mAgentTagRegistry;
	}

	uint64_t World::getAgentTagAssignmentCount() const
	{
		uint64_t count{ 0 };
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (agent) count += agent->getAgentTagIds().size();
		}
		return count;
	}

	uint32_t World::getAgentTagAssignedAgentCount() const
	{
		uint32_t count{ 0 };
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (agent && !agent->getAgentTagIds().empty()) ++count;
		}
		return count;
	}

	uint64_t World::getAgentTagSampleCount() const
	{
		uint64_t count{ 0 };
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent) continue;
			if (agent->getWalkSpeedModifierSample()) ++count;
			if (agent->getHeightModifierSample()) ++count;
			if (agent->getStairSpeedModifierSample()) ++count;
			if (agent->getLadderSpeedModifierSample()) ++count;
			if (agent->getInteractionAversionSample()) ++count;
			if (agent->getEffortAversionSample()) ++count;
			if (agent->getWaitingAversionSample()) ++count;
			if (agent->getCrowdAversionSample()) ++count;
			if (agent->getRiskAversionSample()) ++count;
			if (agent->getRouteFamiliaritySample()) ++count;
			if (agent->getRoutePersistenceSample()) ++count;
			if (agent->getMinimumRoutePlanningTimeSample()) ++count;
			if (agent->getMaximumRoutePlanningTimeSample()) ++count;
		}
		return count;
	}

	void World::attachAgentTagRegistry(string resourceName,
		shared_ptr<AgentTagRegistry> registry)
	{
		invalidateSimulationSnapshot();
		requireCatalogResourceName(resourceName, "Agent tag registry");
		if (!registry || !AgentTagRegistry::uuidIsValid(registry->getUuid()))
			throw invalid_argument("Cannot attach an invalid Agent tag registry");
		if (mAgentTagRegistryReference
			&& mAgentTagRegistryReference->resourceName == resourceName
			&& mAgentTagRegistryReference->expectedUuid == registry->getUuid()
			&& mAgentTagRegistry == registry) return;
		if (getAgentTagAssignmentCount() != 0)
		{
			throw invalid_argument(
				"Cannot switch Agent tag registries while Agent tag assignments exist; use the confirmed destructive action to clear assignments and samples first");
		}
		string diagnostic;
		if (!agentTagAssignmentsAreValid(*registry, &diagnostic))
			throw invalid_argument(diagnostic);

		AgentTagRegistryReference replacement{ std::move(resourceName), registry->getUuid() };
		auto const registryChanges = mAgentTagRegistry != registry;
		if (registryChanges) registry->registerWorld(*this);
		if (registryChanges && mAgentTagRegistry)
			mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistryReference = std::move(replacement);
		mAgentTagRegistry = std::move(registry);
		modify();
	}

	void World::attachAgentTagRegistryAndClearAssignments(string resourceName,
		shared_ptr<AgentTagRegistry> registry)
	{
		invalidateSimulationSnapshot();
		requireCatalogResourceName(resourceName, "Agent tag registry");
		if (!registry || !AgentTagRegistry::uuidIsValid(registry->getUuid()))
			throw invalid_argument("Cannot attach an invalid Agent tag registry");
		if (mAgentTagRegistryReference
			&& mAgentTagRegistryReference->resourceName == resourceName
			&& mAgentTagRegistryReference->expectedUuid == registry->getUuid()
			&& mAgentTagRegistry == registry) return;

		// Construct and register the replacement before changing any authored state.
		// Everything after registration is non-refusing, so clearing assignments,
		// clearing samples, and changing namespace commit as one operation.
		AgentTagRegistryReference replacement{ std::move(resourceName), registry->getUuid() };
		auto const registryChanges = mAgentTagRegistry != registry;
		if (registryChanges) registry->registerWorld(*this);
		clearAllAgentTagAssignmentsAndSamples();
		if (registryChanges && mAgentTagRegistry)
			mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistryReference = std::move(replacement);
		mAgentTagRegistry = std::move(registry);
		modify();
	}

	void World::detachAgentTagRegistry()
	{
		invalidateSimulationSnapshot();
		if (!mAgentTagRegistryReference) return;
		if (getAgentTagAssignmentCount() != 0 || getAgentTagSampleCount() != 0)
		{
			throw invalid_argument(
				"Cannot detach the Agent tag registry while assignments or samples exist; use the confirmed destructive action to clear them first");
		}
		if (mAgentTagRegistry) mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistry.reset();
		mAgentTagRegistryReference.reset();
		modify();
	}

	void World::detachAgentTagRegistryAndClearAssignments()
	{
		invalidateSimulationSnapshot();
		if (!mAgentTagRegistryReference) return;
		clearAllAgentTagAssignmentsAndSamples();
		if (mAgentTagRegistry) mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistry.reset();
		mAgentTagRegistryReference.reset();
		modify();
	}

	void World::resolveAgentTagRegistry(shared_ptr<AgentTagRegistry> registry)
	{
		invalidateSimulationSnapshot();
		if (!mAgentTagRegistryReference)
			throw invalid_argument("The World has no Agent tag registry reference to resolve");
		if (!registry)
			throw invalid_argument("Cannot resolve a null Agent tag registry");
		if (registry->getUuid() != mAgentTagRegistryReference->expectedUuid)
		{
			throw runtime_error(format(
				"Agent tag registry UUID mismatch: World expects {}, file contains {}",
				mAgentTagRegistryReference->expectedUuid, registry->getUuid()));
		}

		// Reconciliation is completed before registration, so a refusal neither
		// exposes this World through the shared registry nor changes any Agent.
		reconcileAgentTagAssignments(*registry);
		if (mAgentTagRegistry) mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistry = std::move(registry);
		mAgentTagRegistry->registerWorld(*this);

		// Saved route intent stays unevaluated until reconciliation has made every
		// tag-supplied routing property and persisted sample effective (#194, #221).
		rebuildRestoredAgentPaths();
	}

	void World::replaceAgentTagRegistryWithIndependentCopy(string resourceName,
		shared_ptr<AgentTagRegistry> registry)
	{
		invalidateSimulationSnapshot();
		requireCatalogResourceName(resourceName, "Agent tag registry");
		if (!mAgentTagRegistry || !mAgentTagRegistryReference)
			throw invalid_argument("The World has no attached Agent tag registry to copy");
		if (!registry || !AgentTagRegistry::uuidIsValid(registry->getUuid()))
			throw invalid_argument("Cannot attach an invalid Agent tag registry copy");
		if (registry == mAgentTagRegistry
			|| registry->getUuid() == mAgentTagRegistry->getUuid())
			throw invalid_argument("An Agent tag registry copy must have a new UUID");
		if (!mAgentTagRegistry->hasEquivalentDefinitions(*registry))
			throw invalid_argument(
				"An Agent tag registry copy must preserve every definition and allocator");

		string diagnostic;
		if (!agentTagAssignmentsAreValid(*registry, &diagnostic))
			throw invalid_argument(diagnostic);

		AgentTagRegistryReference replacement{ std::move(resourceName), registry->getUuid() };
		registry->registerWorld(*this);
		mAgentTagRegistry->unregisterWorld(*this);
		mAgentTagRegistryReference = std::move(replacement);
		mAgentTagRegistry = std::move(registry);
		modify();
	}

	bool World::hasAgentBehaviourRegistryReference() const
	{
		return mAgentBehaviourRegistryReference.has_value();
	}

	bool World::hasAttachedAgentBehaviourRegistry() const
	{
		return mAgentBehaviourRegistry != nullptr;
	}

	string const& World::getAgentBehaviourRegistryResourceName() const
	{
		if (!mAgentBehaviourRegistryReference)
			throw runtime_error("The World has no Agent behaviour registry reference");
		return mAgentBehaviourRegistryReference->resourceName;
	}

	string const& World::getExpectedAgentBehaviourRegistryUuid() const
	{
		if (!mAgentBehaviourRegistryReference)
			throw runtime_error("The World has no Agent behaviour registry reference");
		return mAgentBehaviourRegistryReference->expectedUuid;
	}

	shared_ptr<AgentBehaviourRegistry> const& World::getAgentBehaviourRegistry() const
	{
		return mAgentBehaviourRegistry;
	}

	namespace
	{
		bool validateConfigurationRecord(World const& world,
			vector<AgentBehaviourSchemaField> const& fields,
			AgentBehaviourConfigurationRecord& record, string const& path,
			size_t depth, string* diagnostic);

		bool validateConfigurationValue(World const& world,
			AgentBehaviourSchemaField const& field,
			AgentBehaviourConfigurationValue& value, string const& path,
			size_t depth, string* diagnostic)
		{
			auto reject = [diagnostic, &path](string message)
			{
				if (diagnostic) *diagnostic = format(
					"Configuration field '{}': {}", path, message);
				return false;
			};
			if (depth > MaxAgentBehaviourConfigurationDepth)
				return reject("maximum nesting depth 16 exceeded");
			auto const actual = string(agentBehaviourConfigurationValueTypeName(value));
			if (actual != agentBehaviourSchemaTypeName(field.type))
				return reject(format("has type {}, expected {}", actual,
					agentBehaviourSchemaTypeName(field.type)));
			if (auto const* number = agentBehaviourConfigurationGetIf<double>(&value);
				number && !isfinite(*number))
				return reject("must be a finite Number");
			if (auto const* duration =
				agentBehaviourConfigurationGetIf<AgentBehaviourDuration>(&value);
				duration && duration->ticks == 0)
				return reject("Duration must be at least one tick");
			if (auto const* marker = agentBehaviourConfigurationGetIf<MarkerId>(&value);
				marker && (!*marker || !world.lookupMarker(*marker)))
				return reject(format("references unknown Marker {}", marker->value));
			if (auto const* action = agentBehaviourConfigurationGetIf<AgentBehaviourAction>(&value);
				action && action->reference != IdleAction && action->reference != UseFurnitureAction
				&& (!world.actionRegistry() || !world.actionRegistry()->find(action->reference)))
				return reject("references unknown Action " + action->reference);
			if (auto* list = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationList>(&value))
			{
				if (list->size() > MaxAgentBehaviourListElements)
					return reject("contains more than 4096 List elements");
				for (size_t index = 0; index < list->size(); ++index)
					if (!validateConfigurationValue(world, field.children.front(),
						(*list)[index], path + "[" + to_string(index) + "]",
						depth + 1, diagnostic)) return false;
			}
			if (auto* nested = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationRecord>(&value))
				return validateConfigurationRecord(world, field.children, *nested,
					path, depth + 1, diagnostic);
			return true;
		}

		bool validateConfigurationRecord(World const& world,
			vector<AgentBehaviourSchemaField> const& fields,
			AgentBehaviourConfigurationRecord& record, string const& path,
			size_t depth, string* diagnostic)
		{
			for (auto const& [name, value] : record)
			{
				(void)value;
				auto found = find_if(fields.begin(), fields.end(),
					[&](auto const& field) { return field.name == name; });
				if (found == fields.end())
				{
					if (diagnostic) *diagnostic = format(
						"Configuration field '{}{}{}' is not declared",
						path, path.empty() ? "" : ".", name);
					return false;
				}
			}
			for (auto const& field : fields)
			{
				auto const fieldPath = path.empty() ? field.name : path + "." + field.name;
				auto found = record.find(field.name);
				if (found == record.end())
				{
					if (field.required)
					{
						if (diagnostic) *diagnostic = format(
							"Required configuration field '{}' is missing", fieldPath);
						return false;
					}
					if (!field.defaultValue)
					{
						if (diagnostic) *diagnostic = format(
							"Optional configuration field '{}' has no default", fieldPath);
						return false;
					}
					found = record.emplace(field.name, *field.defaultValue).first;
				}
				if (!validateConfigurationValue(world, field, found->second,
					fieldPath, depth, diagnostic)) return false;
			}
			return true;
		}

		bool validateBehaviourConfiguration(World const& world,
			AgentBehaviourRegistry const& registry, AgentBehaviourId behaviourId,
			uint64_t revision, AgentBehaviourConfiguration const& configuration,
			AgentBehaviourConfiguration* normalized, string* diagnostic)
		{
			if (diagnostic) diagnostic->clear();
			auto reject = [diagnostic](string message)
			{
				if (diagnostic) *diagnostic = std::move(message);
				return false;
			};
			auto const* behaviour = registry.lookupAgentBehaviour(behaviourId);
			if (!behaviour)
				return reject(format("Agent behaviour {} is not defined in the attached registry",
					behaviourId.value));
			if (revision == 0 || revision != behaviour->getRevision())
				return reject(format("Agent behaviour '{}' revision is {}, expected {}",
					behaviour->getName(), revision, behaviour->getRevision()));

			AgentBehaviourConfiguration candidate = configuration;
			if (!validateConfigurationRecord(world, behaviour->getSchema(),
				candidate, {}, 1, diagnostic)) return false;
			if (normalized) *normalized = std::move(candidate);
			return true;
		}
	}

	void World::attachAgentBehaviourRegistry(string resourceName,
		shared_ptr<AgentBehaviourRegistry> registry)
	{
		invalidateSimulationSnapshot();
		if (!isSimulationPaused())
			throw invalid_argument("Pause the World before changing its Agent behaviour registry");
		requireCatalogResourceName(resourceName, "Agent behaviour registry");
		if (!registry || !AgentBehaviourRegistry::uuidIsValid(registry->getUuid()))
			throw invalid_argument("Cannot attach an invalid Agent behaviour registry");
		if (mAgentBehaviourRegistryReference
			&& mAgentBehaviourRegistryReference->resourceName == resourceName
			&& mAgentBehaviourRegistryReference->expectedUuid == registry->getUuid()
			&& mAgentBehaviourRegistry == registry) return;

		auto const sameNamespace = mAgentBehaviourRegistryReference
			&& mAgentBehaviourRegistryReference->expectedUuid == registry->getUuid();
		if (countAgentBehaviourAssignments() != 0 && !sameNamespace)
		{
			throw invalid_argument(
				"Cannot replace a used Agent behaviour registry; use the confirmed destructive action to clear every assignment and configuration first");
		}
		string assignmentDiagnostic;
		if (!inspectAgentBehaviourAssignments(*registry, &assignmentDiagnostic))
			throw invalid_argument(assignmentDiagnostic);

		// Build every assigned factory in a private candidate runtime before the
		// shared dependency list, persisted reference, assignments, or live runtime
		// changes. A repaired expected package therefore becomes usable as one
		// operation and a bad factory leaves the unresolved World untouched.
		unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
		vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
		if (!AgentBehaviourRuntimeAdapter::prepareReload(*this, *registry,
			candidateRuntime, runtimeDiagnostics))
		{
			string details;
			for (auto const& item : runtimeDiagnostics)
				details += (details.empty() ? "" : "\n") + format(
					"Agent '{}' ({}), module '{}': {}", item.agentName,
					item.agent.value, item.moduleName, item.diagnostic);
			throw invalid_argument("Agent behaviour runtime preflight failed"
				+ (details.empty() ? string{} : ":\n" + details));
		}

		AgentBehaviourRegistryReference replacement{ resourceName, registry->getUuid() };
		auto const referenceChanges = !mAgentBehaviourRegistryReference
			|| mAgentBehaviourRegistryReference->resourceName != replacement.resourceName
			|| mAgentBehaviourRegistryReference->expectedUuid != replacement.expectedUuid;
		auto previousRegistry = mAgentBehaviourRegistry;
		auto const registryChanges = previousRegistry != registry;
		if (registryChanges) registry->registerWorld(*this);
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Reload);
		candidateRuntime->appendDiagnostics(
			mAgentBehaviourRuntime->consumeDiagnostics());
		mAgentBehaviourRuntime = std::move(candidateRuntime);
		if (registryChanges && previousRegistry)
			previousRegistry->unregisterWorld(*this);
		mAgentBehaviourRegistryReference = std::move(replacement);
		mAgentBehaviourRegistry = std::move(registry);
		mAgentBehaviourDependencyDiagnostic.clear();
		for (auto const& [agentId, agent] : mAgents.entries())
			if (agent && agent->getBehaviourAssignment())
				mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
		if (referenceChanges) modify();
	}

	void World::attachAgentBehaviourRegistryAndClearAssignments(string resourceName,
		shared_ptr<AgentBehaviourRegistry> registry)
	{
		invalidateSimulationSnapshot();
		if (!isSimulationPaused())
			throw invalid_argument("Pause the World before replacing its Agent behaviour registry");
		requireCatalogResourceName(resourceName, "Agent behaviour registry");
		if (!registry || !AgentBehaviourRegistry::uuidIsValid(registry->getUuid()))
			throw invalid_argument("Cannot attach an invalid Agent behaviour registry");

		// Allocation and shared registration are the only potentially refusing
		// in-memory steps. Complete them before any authored configuration is lost.
		auto candidateRuntime = make_unique<AgentBehaviourRuntimeAdapter>(
			mAgentBehaviourRuntime->getLimits());
		auto previousRegistry = mAgentBehaviourRegistry;
		auto const registryChanges = previousRegistry != registry;
		if (registryChanges) registry->registerWorld(*this);
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Unassignment);
		candidateRuntime->appendDiagnostics(
			mAgentBehaviourRuntime->consumeDiagnostics());
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			if (!agent || !agent->getBehaviourAssignment()) continue;
			mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
			agent->clearBehaviourAssignment();
		}
		if (registryChanges && previousRegistry)
			previousRegistry->unregisterWorld(*this);
		mAgentBehaviourRuntime = std::move(candidateRuntime);
		mAgentBehaviourRegistryReference = AgentBehaviourRegistryReference{
			std::move(resourceName), registry->getUuid() };
		mAgentBehaviourRegistry = std::move(registry);
		mAgentBehaviourDependencyDiagnostic.clear();
		modify();
	}

	void World::detachAgentBehaviourRegistry()
	{
		invalidateSimulationSnapshot();
		if (!mAgentBehaviourRegistryReference) return;
		if (!isSimulationPaused())
			throw invalid_argument("Pause the World before detaching its Agent behaviour registry");
		if (countAgentBehaviourAssignments() != 0)
			throw invalid_argument(
				"Cannot detach a used Agent behaviour registry; use the confirmed destructive action to clear every assignment and configuration first");
		if (mAgentBehaviourRegistry) mAgentBehaviourRegistry->unregisterWorld(*this);
		mAgentBehaviourRegistry.reset();
		mAgentBehaviourRegistryReference.reset();
		mAgentBehaviourDependencyDiagnostic.clear();
		modify();
	}

	void World::detachAgentBehaviourRegistryAndClearAssignments()
	{
		invalidateSimulationSnapshot();
		if (!mAgentBehaviourRegistryReference) return;
		if (!isSimulationPaused())
			throw invalid_argument("Pause the World before detaching its Agent behaviour registry");
		auto candidateRuntime = make_unique<AgentBehaviourRuntimeAdapter>(
			mAgentBehaviourRuntime->getLimits());
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Unassignment);
		candidateRuntime->appendDiagnostics(
			mAgentBehaviourRuntime->consumeDiagnostics());
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			if (!agent || !agent->getBehaviourAssignment()) continue;
			mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
			agent->clearBehaviourAssignment();
		}
		if (mAgentBehaviourRegistry) mAgentBehaviourRegistry->unregisterWorld(*this);
		mAgentBehaviourRuntime = std::move(candidateRuntime);
		mAgentBehaviourRegistry.reset();
		mAgentBehaviourRegistryReference.reset();
		mAgentBehaviourDependencyDiagnostic.clear();
		modify();
	}

	void World::resolveAgentBehaviourRegistry(shared_ptr<AgentBehaviourRegistry> registry)
	{
		invalidateSimulationSnapshot();
		if (!mAgentBehaviourRegistryReference)
			throw invalid_argument("The World has no Agent behaviour registry reference to resolve");
		if (!registry)
			throw invalid_argument("Cannot resolve a null Agent behaviour registry");
		if (registry->getUuid() != mAgentBehaviourRegistryReference->expectedUuid)
		{
			throw runtime_error(format(
				"Agent behaviour registry UUID mismatch: World expects {}, file contains {}",
				mAgentBehaviourRegistryReference->expectedUuid, registry->getUuid()));
		}
		auto previousRegistry = mAgentBehaviourRegistry;
		auto const registryChanges = previousRegistry != registry;
		if (registryChanges) registry->registerWorld(*this);
		auto rollbackRegistry = [&]
		{
			if (registryChanges) registry->unregisterWorld(*this);
		};

		AgentBehaviourSchemaMigrationPreview preview;
		string previewDiagnostic;
		if (!registry->previewDefinitionsFrom(*registry, preview, &previewDiagnostic))
		{
			rollbackRegistry();
			throw invalid_argument(previewDiagnostic);
		}
		map<AgentId, AgentBehaviourAssignment> reconciled;
		vector<string> mismatches;
		for (auto const& item : preview.configurations)
		{
			if (item.world != this) continue;
			if (item.fromRevision == item.toRevision)
			{
				string details;
				for (auto const& field : item.fields)
					details += (details.empty() ? "" : "; ") + field.diagnostic;
				mismatches.push_back(format(
					"World '{}' / Agent '{}' ({}): {}", getName(),
					item.agentName, item.agent.value, details));
				continue;
			}
			if (item.compatibility == AgentBehaviourSchemaCompatibility::Incompatible)
			{
				string fields;
				for (auto const& field : item.fields)
					fields += (fields.empty() ? "" : ", ") + field.path
						+ " (" + field.diagnostic + ")";
				mismatches.push_back(format("World '{}' / Agent '{}' ({}) / {}: {}",
					getName(), item.agentName, item.agent.value,
					item.behaviourName, fields));
				continue;
			}
			auto const* agent = mAgents.find(item.agent);
			auto const* definition = registry->lookupAgentBehaviour(item.behaviour);
			if (!agent || !agent->getBehaviourAssignment() || !definition)
			{
				rollbackRegistry();
				throw invalid_argument("An Agent behaviour reconciliation target disappeared");
			}
			AgentBehaviourConfiguration normalized;
			string validation;
			if (!validateAgentBehaviourAssignmentAgainst(*registry,
				item.behaviour, definition->getRevision(),
				agent->getBehaviourAssignment()->configuration, &normalized, &validation))
			{
				rollbackRegistry();
				throw invalid_argument(validation);
			}
			reconciled[item.agent] = AgentBehaviourAssignment{
				item.behaviour, definition->getRevision(), std::move(normalized) };
		}
		if (!mismatches.empty())
		{
			mAgentBehaviourRegistry = registry;
			mAgentBehaviourDependencyDiagnostic.clear();
			for (auto const& mismatch : mismatches)
				mAgentBehaviourDependencyDiagnostic +=
					(mAgentBehaviourDependencyDiagnostic.empty() ? "" : "\n") + mismatch;
			mSimulationPaused = true;
			if (registryChanges && previousRegistry)
				previousRegistry->unregisterWorld(*this);
			return;
		}

		unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
		vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
		if (!AgentBehaviourRuntimeAdapter::prepareReload(*this, *registry,
			candidateRuntime, runtimeDiagnostics,
			reconciled.empty() ? nullptr : &reconciled))
		{
			rollbackRegistry();
			string details;
			for (auto const& item : runtimeDiagnostics)
				details += (details.empty() ? "" : "\n") + format(
					"Agent '{}' ({}), module '{}': {}", item.agentName,
					item.agent.value, item.moduleName, item.diagnostic);
			throw invalid_argument("Agent behaviour runtime preflight failed"
				+ (details.empty() ? string{} : ":\n" + details));
		}

		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Reload);
		candidateRuntime->appendDiagnostics(
			mAgentBehaviourRuntime->consumeDiagnostics());
		mAgentBehaviourRuntime = std::move(candidateRuntime);
		mAgentBehaviourRegistry = registry;
		for (auto& [agentId, assignment] : reconciled)
		{
			auto* agent = mAgents.find(agentId);
			if (agent) agent->setBehaviourAssignment(std::move(assignment));
		}
		if (!reconciled.empty()) modify();
		mAgentBehaviourDependencyDiagnostic.clear();
		if (registryChanges && previousRegistry)
			previousRegistry->unregisterWorld(*this);
		for (auto const& [agentId, agent] : mAgents.entries())
			if (agent && agent->getBehaviourAssignment())
				mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
	}

	void World::markAgentBehaviourRegistryUnavailable(string diagnostic)
	{
		invalidateSimulationSnapshot();
		if (!mAgentBehaviourRegistryReference) return;
		// An admitted in-memory registry/runtime is not discarded merely because a
		// later disk resolution attempt failed. Freshly deserialized Worlds have
		// no attachment here; explicit replacement failures therefore preserve the
		// old dependency and runtime as well.
		mAgentBehaviourDependencyDiagnostic = format(
			"Agent behaviour registry dependency '{}' is unavailable. {} authored assignment{} remain{} unresolved.\n{}",
			mAgentBehaviourRegistryReference->resourceName,
			countAgentBehaviourAssignments(),
			countAgentBehaviourAssignments() == 1 ? "" : "s",
			countAgentBehaviourAssignments() == 1 ? "s" : "",
			std::move(diagnostic));
		mSimulationPaused = true;
	}

	void World::replaceAgentBehaviourRegistryWithIndependentCopy(
		string resourceName, shared_ptr<AgentBehaviourRegistry> registry)
	{
		invalidateSimulationSnapshot();
		if (!isSimulationPaused())
			throw invalid_argument("Pause the World before replacing its Agent behaviour registry with a Save As copy");
		requireCatalogResourceName(resourceName, "Agent behaviour registry");
		if (!mAgentBehaviourRegistry || !registry)
			throw invalid_argument("Save As requires attached source and copied Agent behaviour registries");
		if (registry == mAgentBehaviourRegistry
			|| registry->getUuid() == mAgentBehaviourRegistry->getUuid())
			throw invalid_argument("An Agent behaviour registry copy must have a new UUID");
		if (!mAgentBehaviourRegistry->hasEquivalentDefinitions(*registry))
			throw invalid_argument(
				"An Agent behaviour registry copy must preserve every definition, revision, and allocator");
		string assignmentDiagnostic;
		if (!inspectAgentBehaviourAssignments(*registry, &assignmentDiagnostic))
			throw invalid_argument(assignmentDiagnostic);

		unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
		vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
		if (!AgentBehaviourRuntimeAdapter::prepareReload(*this, *registry,
			candidateRuntime, runtimeDiagnostics))
			throw invalid_argument("Copied Agent behaviour package runtime preflight failed");

		registry->registerWorld(*this);
		auto previous = mAgentBehaviourRegistry;
		mAgentBehaviourRuntime->teardownAll(*this,
			AgentBehaviourTeardownReason::Reload);
		candidateRuntime->appendDiagnostics(
			mAgentBehaviourRuntime->consumeDiagnostics());
		mAgentBehaviourRuntime = std::move(candidateRuntime);
		previous->unregisterWorld(*this);
		mAgentBehaviourRegistryReference = AgentBehaviourRegistryReference{
			std::move(resourceName), registry->getUuid() };
		mAgentBehaviourRegistry = std::move(registry);
		mAgentBehaviourDependencyDiagnostic.clear();
		modify();
	}

	bool World::validateAgentBehaviourAssignment(AgentBehaviourId behaviour,
		uint64_t revision, AgentBehaviourConfiguration const& configuration,
		AgentBehaviourConfiguration* normalized, string* diagnostic) const
	{
		if (!mAgentBehaviourRegistry)
		{
			if (diagnostic) *diagnostic =
				"This World has no attached Agent behaviour registry";
			return false;
		}
		return validateAgentBehaviourAssignmentAgainst(*mAgentBehaviourRegistry,
			behaviour, revision, configuration, normalized, diagnostic);
	}

	bool World::validateAgentBehaviourAssignmentAgainst(
		AgentBehaviourRegistry const& registry, AgentBehaviourId behaviour,
		uint64_t revision, AgentBehaviourConfiguration const& configuration,
		AgentBehaviourConfiguration* normalized, string* diagnostic) const
	{
		return validateBehaviourConfiguration(*this, registry, behaviour, revision,
			configuration, normalized, diagnostic);
	}

	bool World::setAgentBehaviourAssignment(AgentId agentId,
		AgentBehaviourId behaviour, uint64_t revision,
		AgentBehaviourConfiguration const& configuration, string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (diagnostic) diagnostic->clear();
		auto const lookup = lookupAgent(agentId);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic =
				"Pause the simulation before assigning an Agent behaviour";
			return false;
		}
		AgentBehaviourConfiguration normalized;
		if (!validateAgentBehaviourAssignment(behaviour, revision, configuration,
			&normalized, diagnostic)) return false;
		AgentBehaviourAssignment assignment{ behaviour, revision, std::move(normalized) };
		if (lookup.entity->getBehaviourAssignment() == optional<AgentBehaviourAssignment>{ assignment })
		{
			if (diagnostic) *diagnostic = "The Agent behaviour assignment is unchanged";
			return false;
		}
		// Assignment hands movement authority to a fresh instance. A manual route,
		// or runtime intent from the assignment being replaced, must not survive
		// into that instance.
		mAgentBehaviourRuntime->removeInstance(*this, agentId,
			AgentBehaviourTeardownReason::Unassignment);
		mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
		mAgents.find(agentId)->setBehaviourAssignment(std::move(assignment));
		modify();
		return true;
	}

	bool World::clearAgentBehaviourAssignment(AgentId agentId, string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (diagnostic) diagnostic->clear();
		auto const lookup = lookupAgent(agentId);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic =
				"Pause the simulation before clearing an Agent behaviour";
			return false;
		}
		if (!lookup.entity->getBehaviourAssignment())
		{
			if (diagnostic) *diagnostic = "The Agent has no behaviour assignment to clear";
			return false;
		}
		mAgentBehaviourRuntime->removeInstance(*this, agentId,
			AgentBehaviourTeardownReason::Unassignment);
		mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
		mAgents.find(agentId)->clearBehaviourAssignment();
		modify();
		return true;
	}

	optional<AgentBehaviourAssignment> const& World::getAgentBehaviourAssignment(
		AgentId agent) const
	{
		auto const lookup = lookupAgent(agent);
		if (!lookup) throw WorldException(this, lookup.diagnostic);
		return lookup.entity->getBehaviourAssignment();
	}

	uint32_t World::countAgentBehaviourAssignments() const
	{
		uint32_t count = 0;
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent && agent->getBehaviourAssignment()) ++count;
		}
		return count;
	}

	uint32_t World::getAgentBehaviourAssignmentCount() const
	{
		return countAgentBehaviourAssignments();
	}

	bool World::agentBehaviourOwnsMovement(AgentId id) const
	{
		auto agent = mAgents.find(id);
		if (!agent || !agent->getBehaviourAssignment()) return false;
		if (!mAgentBehaviourRuntime->isInstanceDisabled(id)) return true;
		// A failed instance stops issuing commands immediately, but an already
		// committed crossing still drains through the ordinary safe-cancellation
		// protocol before manual controls return.
		auto goal = mMovementGoals.find(id);
		return goal != mMovementGoals.end() && goal->second.behaviourOwned;
	}

	vector<AgentBehaviourRuntimeDiagnostic>
	World::getAgentBehaviourRuntimeDiagnostics() const
	{
		return mAgentBehaviourRuntime->getDiagnostics();
	}

	vector<AgentBehaviourRuntimeDiagnostic>
	World::consumeAgentBehaviourRuntimeDiagnostics()
	{
		invalidateSimulationSnapshot();
		return mAgentBehaviourRuntime->consumeDiagnostics();
	}

	AgentBehaviourRuntimeLimits World::getAgentBehaviourRuntimeLimits() const
	{
		return mAgentBehaviourRuntime->getLimits();
	}

	bool World::inspectAgentBehaviourAssignments(
		AgentBehaviourRegistry const& registry, string* diagnostic) const
	{
		vector<string> failures;
		for (auto const& [id, agent] : mAgents.entries())
		{
			if (!agent || !agent->getBehaviourAssignment()) continue;
			auto const& assignment = *agent->getBehaviourAssignment();
			string fieldDiagnostic;
			if (!validateAgentBehaviourAssignmentAgainst(registry, assignment.behaviour,
				assignment.revision, assignment.configuration, nullptr, &fieldDiagnostic))
			{
				failures.push_back(format("Agent '{}' ({}): {}",
					agent->getName(), id.value, fieldDiagnostic));
			}
		}
		if (failures.empty())
		{
			if (diagnostic) diagnostic->clear();
			return true;
		}
		if (diagnostic)
		{
			*diagnostic = format(
				"Agent behaviour dependency validation found {} assignment diagnostic(s):",
				failures.size());
			for (auto const& failure : failures) *diagnostic += "\n" + failure;
		}
		return false;
	}

	bool World::inspectAgentTagAssignments(AgentTagRegistry const& registry,
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

		for (auto const& [agentId, agent] : mAgents.entries())
		{
			if (!agent) continue;
			AgentTagId escalatorWalkingChanceSource{};
			AgentTagId colourSource{};
			AgentTagId walkSpeedSource{};
			AgentTagId heightSource{};
			AgentTagId stairSpeedSource{};
			AgentTagId ladderSpeedSource{};
			AgentTagId interactionAversionSource{};
			AgentTagId effortAversionSource{};
			AgentTagId waitingAversionSource{};
			AgentTagId crowdAversionSource{};
			AgentTagId riskAversionSource{};
			AgentTagId routeFamiliaritySource{};
			AgentTagId routePersistenceSource{};
			AgentTagId minimumRoutePlanningTimeSource{};
			AgentTagId maximumRoutePlanningTimeSource{};
			AgentTagId permissionAdherenceSource{};
			AgentTagId mobilityProfileSource{};
			AgentWalkSpeedModifierProperty const* walkSpeedProperty{ nullptr };
			AgentHeightModifierProperty const* heightProperty{ nullptr };
			AgentStairSpeedModifierProperty const* stairSpeedProperty{ nullptr };
			AgentLadderSpeedModifierProperty const* ladderSpeedProperty{ nullptr };
			AgentInteractionAversionProperty const* interactionAversionProperty{ nullptr };
			AgentEffortAversionProperty const* effortAversionProperty{ nullptr };
			AgentWaitingAversionProperty const* waitingAversionProperty{ nullptr };
			AgentCrowdAversionProperty const* crowdAversionProperty{ nullptr };
			AgentRiskAversionProperty const* riskAversionProperty{ nullptr };
			AgentRouteFamiliarityProperty const* routeFamiliarityProperty{ nullptr };
			AgentRoutePersistenceProperty const* routePersistenceProperty{ nullptr };
			AgentMinimumRoutePlanningTimeProperty const* minimumRoutePlanningTimeProperty{ nullptr };
			AgentMaximumRoutePlanningTimeProperty const* maximumRoutePlanningTimeProperty{ nullptr };
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
				if (auto const* property = definition->getWalkSpeedModifier())
				{
					if (walkSpeedSource)
					{
						return reject(format(
							"Agent '{}' inherits Walk speed modifier from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(walkSpeedSource),
							definition->getName()));
					}
					walkSpeedSource = tag;
					walkSpeedProperty = property;
				}
				if (auto const* property = definition->getHeightModifier())
				{
					if (heightSource)
					{
						return reject(format(
							"Agent '{}' inherits Height modifier from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(heightSource),
							definition->getName()));
					}
					heightSource = tag;
					heightProperty = property;
				}
				if (auto const* property = definition->getStairSpeedModifier())
				{
					if (stairSpeedSource)
						return reject(format(
							"Agent '{}' inherits Stair speed modifier from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(stairSpeedSource),
							definition->getName()));
					stairSpeedSource = tag;
					stairSpeedProperty = property;
				}
				if (auto const* property = definition->getLadderSpeedModifier())
				{
					if (ladderSpeedSource)
						return reject(format(
							"Agent '{}' inherits Ladder speed modifier from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(ladderSpeedSource),
							definition->getName()));
					ladderSpeedSource = tag;
					ladderSpeedProperty = property;
				}
				if (auto const* property = definition->getInteractionAversion())
				{
					if (interactionAversionSource)
						return reject(format(
							"Agent '{}' inherits Interaction aversion from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(interactionAversionSource),
							definition->getName()));
					interactionAversionSource = tag;
					interactionAversionProperty = property;
				}
				if (auto const* property = definition->getEffortAversion())
				{
					if (effortAversionSource)
						return reject(format(
							"Agent '{}' inherits Effort aversion from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(effortAversionSource),
							definition->getName()));
					effortAversionSource = tag;
					effortAversionProperty = property;
				}
				if (auto const* property = definition->getWaitingAversion())
				{
					if (waitingAversionSource)
						return reject(format(
							"Agent '{}' inherits Waiting aversion from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(waitingAversionSource),
							definition->getName()));
					waitingAversionSource = tag;
					waitingAversionProperty = property;
				}
				if (auto const* property = definition->getCrowdAversion())
				{
					if (crowdAversionSource)
						return reject(format(
							"Agent '{}' inherits Crowd aversion from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(crowdAversionSource),
							definition->getName()));
					crowdAversionSource = tag;
					crowdAversionProperty = property;
				}
				if (auto const* property = definition->getRiskAversion())
				{
					if (riskAversionSource)
						return reject(format(
							"Agent '{}' inherits Risk aversion from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(riskAversionSource),
							definition->getName()));
					riskAversionSource = tag;
					riskAversionProperty = property;
				}
				if (auto const* property = definition->getRouteFamiliarity())
				{
					if (routeFamiliaritySource)
						return reject(format(
							"Agent '{}' inherits Route familiarity from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(routeFamiliaritySource),
							definition->getName()));
					routeFamiliaritySource = tag;
					routeFamiliarityProperty = property;
				}
				if (auto const* property = definition->getRoutePersistence())
				{
					if (routePersistenceSource)
						return reject(format(
							"Agent '{}' inherits Route persistence from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(routePersistenceSource),
							definition->getName()));
					routePersistenceSource = tag;
					routePersistenceProperty = property;
				}
				if (auto const* property = definition->getMinimumRoutePlanningTime())
				{
					if (minimumRoutePlanningTimeSource)
						return reject(format(
							"Agent '{}' inherits Minimum route planning time from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(minimumRoutePlanningTimeSource),
							definition->getName()));
					minimumRoutePlanningTimeSource = tag;
					minimumRoutePlanningTimeProperty = property;
				}
				if (auto const* property = definition->getMaximumRoutePlanningTime())
				{
					if (maximumRoutePlanningTimeSource)
						return reject(format(
							"Agent '{}' inherits Maximum route planning time from both #{} and #{}",
							agent->getName(), registry.getAgentTagName(maximumRoutePlanningTimeSource),
							definition->getName()));
					maximumRoutePlanningTimeSource = tag;
					maximumRoutePlanningTimeProperty = property;
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

			AgentTagReconciliation repair;
			repair.agent = agentId;
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

			if (!inspectSample("Walk speed modifier",
				SampledAgentPropertyType::WalkSpeedModifier, walkSpeedSource,
				walkSpeedProperty ? &walkSpeedProperty->range : nullptr,
				walkSpeedProperty ? walkSpeedProperty->revision : 0,
				agent->getWalkSpeedModifierSample(), repair.walkSpeedAction)) return false;
			if (!inspectSample("Height modifier", SampledAgentPropertyType::HeightModifier,
				heightSource, heightProperty ? &heightProperty->range : nullptr,
				heightProperty ? heightProperty->revision : 0,
				agent->getHeightModifierSample(), repair.heightAction)) return false;
			if (!inspectSample("Stair speed modifier", SampledAgentPropertyType::StairSpeedModifier,
				stairSpeedSource, stairSpeedProperty ? &stairSpeedProperty->range : nullptr,
				stairSpeedProperty ? stairSpeedProperty->revision : 0,
				agent->getStairSpeedModifierSample(), repair.stairSpeedAction)) return false;
			if (!inspectSample("Ladder speed modifier", SampledAgentPropertyType::LadderSpeedModifier,
				ladderSpeedSource, ladderSpeedProperty ? &ladderSpeedProperty->range : nullptr,
				ladderSpeedProperty ? ladderSpeedProperty->revision : 0,
				agent->getLadderSpeedModifierSample(), repair.ladderSpeedAction)) return false;
			if (!inspectSample("Interaction aversion", SampledAgentPropertyType::InteractionAversion,
				interactionAversionSource,
				interactionAversionProperty ? &interactionAversionProperty->range : nullptr,
				interactionAversionProperty ? interactionAversionProperty->revision : 0,
				agent->getInteractionAversionSample(), repair.interactionAversionAction)) return false;
			if (!inspectSample("Effort aversion", SampledAgentPropertyType::EffortAversion,
				effortAversionSource,
				effortAversionProperty ? &effortAversionProperty->range : nullptr,
				effortAversionProperty ? effortAversionProperty->revision : 0,
				agent->getEffortAversionSample(), repair.effortAversionAction)) return false;
			if (!inspectSample("Waiting aversion", SampledAgentPropertyType::WaitingAversion,
				waitingAversionSource,
				waitingAversionProperty ? &waitingAversionProperty->range : nullptr,
				waitingAversionProperty ? waitingAversionProperty->revision : 0,
				agent->getWaitingAversionSample(), repair.waitingAversionAction)) return false;
			if (!inspectSample("Crowd aversion", SampledAgentPropertyType::CrowdAversion,
				crowdAversionSource,
				crowdAversionProperty ? &crowdAversionProperty->range : nullptr,
				crowdAversionProperty ? crowdAversionProperty->revision : 0,
				agent->getCrowdAversionSample(), repair.crowdAversionAction)) return false;
			if (!inspectSample("Risk aversion", SampledAgentPropertyType::RiskAversion,
				riskAversionSource,
				riskAversionProperty ? &riskAversionProperty->range : nullptr,
				riskAversionProperty ? riskAversionProperty->revision : 0,
				agent->getRiskAversionSample(), repair.riskAversionAction)) return false;
			if (!inspectSample("Route familiarity", SampledAgentPropertyType::RouteFamiliarity,
				routeFamiliaritySource,
				routeFamiliarityProperty ? &routeFamiliarityProperty->range : nullptr,
				routeFamiliarityProperty ? routeFamiliarityProperty->revision : 0,
				agent->getRouteFamiliaritySample(), repair.routeFamiliarityAction)) return false;
			if (!inspectSample("Route persistence", SampledAgentPropertyType::RoutePersistence,
				routePersistenceSource,
				routePersistenceProperty ? &routePersistenceProperty->range : nullptr,
				routePersistenceProperty ? routePersistenceProperty->revision : 0,
				agent->getRoutePersistenceSample(), repair.routePersistenceAction)) return false;
			if (!inspectSample("Minimum route planning time", SampledAgentPropertyType::MinimumRoutePlanningTime,
				minimumRoutePlanningTimeSource,
				minimumRoutePlanningTimeProperty ? &minimumRoutePlanningTimeProperty->range : nullptr,
				minimumRoutePlanningTimeProperty ? minimumRoutePlanningTimeProperty->revision : 0,
				agent->getMinimumRoutePlanningTimeSample(), repair.minimumRoutePlanningTimeAction)) return false;
			if (!inspectSample("Maximum route planning time", SampledAgentPropertyType::MaximumRoutePlanningTime,
				maximumRoutePlanningTimeSource,
				maximumRoutePlanningTimeProperty ? &maximumRoutePlanningTimeProperty->range : nullptr,
				maximumRoutePlanningTimeProperty ? maximumRoutePlanningTimeProperty->revision : 0,
				agent->getMaximumRoutePlanningTimeSample(), repair.maximumRoutePlanningTimeAction)) return false;

			if (repair.walkSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				repair.walkSpeedSource = walkSpeedSource;
				repair.walkSpeedProperty = *walkSpeedProperty;
			}
			if (repair.heightAction == AgentTagSampleRepairAction::Resample)
			{
				repair.heightSource = heightSource;
				repair.heightProperty = *heightProperty;
			}
			if (repair.stairSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				repair.stairSpeedSource = stairSpeedSource;
				repair.stairSpeedProperty = *stairSpeedProperty;
			}
			if (repair.ladderSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				repair.ladderSpeedSource = ladderSpeedSource;
				repair.ladderSpeedProperty = *ladderSpeedProperty;
			}
			if (repair.interactionAversionAction == AgentTagSampleRepairAction::Resample)
			{
				repair.interactionAversionSource = interactionAversionSource;
				repair.interactionAversionProperty = *interactionAversionProperty;
			}
			if (repair.effortAversionAction == AgentTagSampleRepairAction::Resample)
			{
				repair.effortAversionSource = effortAversionSource;
				repair.effortAversionProperty = *effortAversionProperty;
			}
			if (repair.waitingAversionAction == AgentTagSampleRepairAction::Resample)
			{
				repair.waitingAversionSource = waitingAversionSource;
				repair.waitingAversionProperty = *waitingAversionProperty;
			}
			if (repair.crowdAversionAction == AgentTagSampleRepairAction::Resample)
			{
				repair.crowdAversionSource = crowdAversionSource;
				repair.crowdAversionProperty = *crowdAversionProperty;
			}
			if (repair.riskAversionAction == AgentTagSampleRepairAction::Resample)
			{
				repair.riskAversionSource = riskAversionSource;
				repair.riskAversionProperty = *riskAversionProperty;
			}
			if (repair.routeFamiliarityAction == AgentTagSampleRepairAction::Resample)
			{
				repair.routeFamiliaritySource = routeFamiliaritySource;
				repair.routeFamiliarityProperty = *routeFamiliarityProperty;
			}
			if (repair.routePersistenceAction == AgentTagSampleRepairAction::Resample)
			{
				repair.routePersistenceSource = routePersistenceSource;
				repair.routePersistenceProperty = *routePersistenceProperty;
			}
			if (repair.minimumRoutePlanningTimeAction == AgentTagSampleRepairAction::Resample)
			{
				repair.minimumRoutePlanningTimeSource = minimumRoutePlanningTimeSource;
				repair.minimumRoutePlanningTimeProperty = *minimumRoutePlanningTimeProperty;
			}
			if (repair.maximumRoutePlanningTimeAction == AgentTagSampleRepairAction::Resample)
			{
				repair.maximumRoutePlanningTimeSource = maximumRoutePlanningTimeSource;
				repair.maximumRoutePlanningTimeProperty = *maximumRoutePlanningTimeProperty;
			}
			if (repairs && (repair.walkSpeedAction != AgentTagSampleRepairAction::None
				|| repair.heightAction != AgentTagSampleRepairAction::None
				|| repair.stairSpeedAction != AgentTagSampleRepairAction::None
				|| repair.ladderSpeedAction != AgentTagSampleRepairAction::None
				|| repair.interactionAversionAction != AgentTagSampleRepairAction::None
				|| repair.effortAversionAction != AgentTagSampleRepairAction::None
				|| repair.waitingAversionAction != AgentTagSampleRepairAction::None
				|| repair.crowdAversionAction != AgentTagSampleRepairAction::None
				|| repair.riskAversionAction != AgentTagSampleRepairAction::None
				|| repair.routeFamiliarityAction != AgentTagSampleRepairAction::None
				|| repair.routePersistenceAction != AgentTagSampleRepairAction::None
				|| repair.minimumRoutePlanningTimeAction != AgentTagSampleRepairAction::None
				|| repair.maximumRoutePlanningTimeAction != AgentTagSampleRepairAction::None))
			{
				repairs->push_back(repair);
			}
		}
		return true;
	}

	bool World::agentTagAssignmentsAreValid(AgentTagRegistry const& registry,
		string* diagnostic) const
	{
		return inspectAgentTagAssignments(registry, false, nullptr, diagnostic);
	}

	void World::applyAgentTagReconciliations(
		vector<AgentTagReconciliation> const& repairs)
	{
		invalidateSimulationSnapshot();
		// A caller validates the complete transaction before reaching this seam.
		// Sampling and application cannot refuse, so all repairs commit together.
		for (auto const& repair : repairs)
		{
			auto* agent = mAgents.find(repair.agent);
			if (repair.walkSpeedAction == AgentTagSampleRepairAction::Clear)
				agent->clearWalkSpeedModifierSample();
			else if (repair.walkSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setWalkSpeedModifierSample({
					SampledAgentPropertyType::WalkSpeedModifier, repair.walkSpeedSource,
					repair.walkSpeedProperty.revision,
					sampleAgentModifier(repair.walkSpeedProperty.range) });
			}

			if (repair.heightAction == AgentTagSampleRepairAction::Clear)
				agent->clearHeightModifierSample();
			else if (repair.heightAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setHeightModifierSample({
					SampledAgentPropertyType::HeightModifier, repair.heightSource,
					repair.heightProperty.revision,
					sampleAgentModifier(repair.heightProperty.range) });
			}
			if (repair.stairSpeedAction == AgentTagSampleRepairAction::Clear)
				agent->clearStairSpeedModifierSample();
			else if (repair.stairSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setStairSpeedModifierSample({
					SampledAgentPropertyType::StairSpeedModifier, repair.stairSpeedSource,
					repair.stairSpeedProperty.revision,
					sampleAgentModifier(repair.stairSpeedProperty.range) });
			}
			if (repair.ladderSpeedAction == AgentTagSampleRepairAction::Clear)
				agent->clearLadderSpeedModifierSample();
			else if (repair.ladderSpeedAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setLadderSpeedModifierSample({
					SampledAgentPropertyType::LadderSpeedModifier, repair.ladderSpeedSource,
					repair.ladderSpeedProperty.revision,
					sampleAgentModifier(repair.ladderSpeedProperty.range) });
			}
			if (repair.interactionAversionAction == AgentTagSampleRepairAction::Clear)
				agent->clearInteractionAversionSample();
			else if (repair.interactionAversionAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setInteractionAversionSample({
					SampledAgentPropertyType::InteractionAversion, repair.interactionAversionSource,
					repair.interactionAversionProperty.revision,
					sampleAgentModifier(repair.interactionAversionProperty.range) });
			}
			if (repair.effortAversionAction == AgentTagSampleRepairAction::Clear)
				agent->clearEffortAversionSample();
			else if (repair.effortAversionAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setEffortAversionSample({
					SampledAgentPropertyType::EffortAversion, repair.effortAversionSource,
					repair.effortAversionProperty.revision,
					sampleAgentModifier(repair.effortAversionProperty.range) });
			}
			if (repair.waitingAversionAction == AgentTagSampleRepairAction::Clear)
				agent->clearWaitingAversionSample();
			else if (repair.waitingAversionAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setWaitingAversionSample({
					SampledAgentPropertyType::WaitingAversion, repair.waitingAversionSource,
					repair.waitingAversionProperty.revision,
					sampleAgentModifier(repair.waitingAversionProperty.range) });
			}
			if (repair.crowdAversionAction == AgentTagSampleRepairAction::Clear)
				agent->clearCrowdAversionSample();
			else if (repair.crowdAversionAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setCrowdAversionSample({
					SampledAgentPropertyType::CrowdAversion, repair.crowdAversionSource,
					repair.crowdAversionProperty.revision,
					sampleAgentModifier(repair.crowdAversionProperty.range) });
			}
			if (repair.riskAversionAction == AgentTagSampleRepairAction::Clear)
				agent->clearRiskAversionSample();
			else if (repair.riskAversionAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setRiskAversionSample({
					SampledAgentPropertyType::RiskAversion, repair.riskAversionSource,
					repair.riskAversionProperty.revision,
					sampleAgentModifier(repair.riskAversionProperty.range) });
			}
			if (repair.routeFamiliarityAction == AgentTagSampleRepairAction::Clear)
				agent->clearRouteFamiliaritySample();
			else if (repair.routeFamiliarityAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setRouteFamiliaritySample({
					SampledAgentPropertyType::RouteFamiliarity, repair.routeFamiliaritySource,
					repair.routeFamiliarityProperty.revision,
					sampleAgentModifier(repair.routeFamiliarityProperty.range) });
			}
			if (repair.routePersistenceAction == AgentTagSampleRepairAction::Clear)
				agent->clearRoutePersistenceSample();
			else if (repair.routePersistenceAction == AgentTagSampleRepairAction::Resample)
			{
				agent->setRoutePersistenceSample({
					SampledAgentPropertyType::RoutePersistence, repair.routePersistenceSource,
					repair.routePersistenceProperty.revision,
					sampleAgentModifier(repair.routePersistenceProperty.range) });
			}
			if (repair.minimumRoutePlanningTimeAction == AgentTagSampleRepairAction::Clear)
				agent->clearMinimumRoutePlanningTimeSample();
			else if (repair.minimumRoutePlanningTimeAction == AgentTagSampleRepairAction::Resample)
				agent->setMinimumRoutePlanningTimeSample({
					SampledAgentPropertyType::MinimumRoutePlanningTime, repair.minimumRoutePlanningTimeSource,
					repair.minimumRoutePlanningTimeProperty.revision,
					sampleAgentModifier(repair.minimumRoutePlanningTimeProperty.range) });
			if (repair.maximumRoutePlanningTimeAction == AgentTagSampleRepairAction::Clear)
				agent->clearMaximumRoutePlanningTimeSample();
			else if (repair.maximumRoutePlanningTimeAction == AgentTagSampleRepairAction::Resample)
				agent->setMaximumRoutePlanningTimeSample({
					SampledAgentPropertyType::MaximumRoutePlanningTime, repair.maximumRoutePlanningTimeSource,
					repair.maximumRoutePlanningTimeProperty.revision,
					sampleAgentModifier(repair.maximumRoutePlanningTimeProperty.range) });
		}
		if (!repairs.empty()) modify();
	}

	void World::reconcileAgentTagAssignments(AgentTagRegistry const& registry)
	{
		invalidateSimulationSnapshot();
		vector<AgentTagReconciliation> repairs;
		string diagnostic;
		if (!inspectAgentTagAssignments(registry, true, &repairs, &diagnostic))
			throw runtime_error(diagnostic);
		applyAgentTagReconciliations(repairs);
	}

	uint32_t World::countAgentTagAssignments(AgentTagId id) const
	{
		uint32_t count{ 0 };
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (agent && agent->hasAgentTag(id)) ++count;
		}
		return count;
	}

	void World::clearAgentTagAssignments(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			if (!agent || !agent->hasAgentTag(id)) continue;
			auto const adherenceBefore = agent->getEffectivePermissionAdherence().value;
			agent->removeAgentTag(id);
			if (agent->getWalkSpeedModifierSample()
				&& agent->getWalkSpeedModifierSample()->sourceTag == id)
				agent->clearWalkSpeedModifierSample();
			if (agent->getHeightModifierSample()
				&& agent->getHeightModifierSample()->sourceTag == id)
				agent->clearHeightModifierSample();
			if (agent->getStairSpeedModifierSample()
				&& agent->getStairSpeedModifierSample()->sourceTag == id)
				agent->clearStairSpeedModifierSample();
			if (agent->getLadderSpeedModifierSample()
				&& agent->getLadderSpeedModifierSample()->sourceTag == id)
				agent->clearLadderSpeedModifierSample();
			if (agent->getInteractionAversionSample()
				&& agent->getInteractionAversionSample()->sourceTag == id)
				agent->clearInteractionAversionSample();
			if (agent->getEffortAversionSample()
				&& agent->getEffortAversionSample()->sourceTag == id)
				agent->clearEffortAversionSample();
			if (agent->getWaitingAversionSample()
				&& agent->getWaitingAversionSample()->sourceTag == id)
				agent->clearWaitingAversionSample();
			if (agent->getCrowdAversionSample()
				&& agent->getCrowdAversionSample()->sourceTag == id)
				agent->clearCrowdAversionSample();
			if (agent->getRiskAversionSample()
				&& agent->getRiskAversionSample()->sourceTag == id)
				agent->clearRiskAversionSample();
			if (agent->getRouteFamiliaritySample()
				&& agent->getRouteFamiliaritySample()->sourceTag == id)
				agent->clearRouteFamiliaritySample();
			if (agent->getRoutePersistenceSample()
				&& agent->getRoutePersistenceSample()->sourceTag == id)
				agent->clearRoutePersistenceSample();
			if (agent->getMinimumRoutePlanningTimeSample()
				&& agent->getMinimumRoutePlanningTimeSample()->sourceTag == id)
				agent->clearMinimumRoutePlanningTimeSample();
			if (agent->getMaximumRoutePlanningTimeSample()
				&& agent->getMaximumRoutePlanningTimeSample()->sourceTag == id)
				agent->clearMaximumRoutePlanningTimeSample();
			auto const adherenceAfter = agent->getEffectivePermissionAdherence().value;
			if (adherenceBefore != adherenceAfter)
			{
				if (adherenceAfter) replanAgentAfterAuthorizationRefusal(agentId);
				else beginVoluntaryRoutePlanning(agentId);
			}
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAllAgentTagAssignmentsAndSamples()
	{
		invalidateSimulationSnapshot();
		for (auto& [agentId, agent] : mAgents.entries())
		{
			if (!agent) continue;
			auto const adherenceBefore = agent->getEffectivePermissionAdherence().value;
			agent->setAgentTags({});
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
				if (adherenceAfter) replanAgentAfterAuthorizationRefusal(agentId);
				else beginVoluntaryRoutePlanning(agentId);
			}
		}
	}

	void World::addAgentTagWalkSpeedModifierSamples(AgentTagId id,
		AgentWalkSpeedModifierProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setWalkSpeedModifierSample({
				SampledAgentPropertyType::WalkSpeedModifier, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagWalkSpeedModifierSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getWalkSpeedModifierSample()
				|| agent->getWalkSpeedModifierSample()->sourceTag != id) continue;
			agent->clearWalkSpeedModifierSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagHeightModifierSamples(AgentTagId id,
		AgentHeightModifierProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setHeightModifierSample({
				SampledAgentPropertyType::HeightModifier, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagHeightModifierSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getHeightModifierSample()
				|| agent->getHeightModifierSample()->sourceTag != id) continue;
			agent->clearHeightModifierSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagStairSpeedModifierSamples(AgentTagId id,
		AgentStairSpeedModifierProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setStairSpeedModifierSample({
				SampledAgentPropertyType::StairSpeedModifier, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagStairSpeedModifierSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getStairSpeedModifierSample()
				|| agent->getStairSpeedModifierSample()->sourceTag != id) continue;
			agent->clearStairSpeedModifierSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagLadderSpeedModifierSamples(AgentTagId id,
		AgentLadderSpeedModifierProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setLadderSpeedModifierSample({
				SampledAgentPropertyType::LadderSpeedModifier, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagLadderSpeedModifierSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getLadderSpeedModifierSample()
				|| agent->getLadderSpeedModifierSample()->sourceTag != id) continue;
			agent->clearLadderSpeedModifierSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagEffortAversionSamples(AgentTagId id,
		AgentEffortAversionProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setEffortAversionSample({
				SampledAgentPropertyType::EffortAversion, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagEffortAversionSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getEffortAversionSample()
				|| agent->getEffortAversionSample()->sourceTag != id) continue;
			agent->clearEffortAversionSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagWaitingAversionSamples(AgentTagId id,
		AgentWaitingAversionProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setWaitingAversionSample({
				SampledAgentPropertyType::WaitingAversion, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagWaitingAversionSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getWaitingAversionSample()
				|| agent->getWaitingAversionSample()->sourceTag != id) continue;
			agent->clearWaitingAversionSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagCrowdAversionSamples(AgentTagId id,
		AgentCrowdAversionProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setCrowdAversionSample({
				SampledAgentPropertyType::CrowdAversion, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagCrowdAversionSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getCrowdAversionSample()
				|| agent->getCrowdAversionSample()->sourceTag != id) continue;
			agent->clearCrowdAversionSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagRiskAversionSamples(AgentTagId id,
		AgentRiskAversionProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setRiskAversionSample({
				SampledAgentPropertyType::RiskAversion, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagRiskAversionSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getRiskAversionSample()
				|| agent->getRiskAversionSample()->sourceTag != id) continue;
			agent->clearRiskAversionSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagRouteFamiliaritySamples(AgentTagId id,
		AgentRouteFamiliarityProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setRouteFamiliaritySample({
				SampledAgentPropertyType::RouteFamiliarity, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagRouteFamiliaritySamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getRouteFamiliaritySample()
				|| agent->getRouteFamiliaritySample()->sourceTag != id) continue;
			agent->clearRouteFamiliaritySample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagRoutePersistenceSamples(AgentTagId id,
		AgentRoutePersistenceProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setRoutePersistenceSample({
				SampledAgentPropertyType::RoutePersistence, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagRoutePersistenceSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getRoutePersistenceSample()
				|| agent->getRoutePersistenceSample()->sourceTag != id) continue;
			agent->clearRoutePersistenceSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id,
		AgentMinimumRoutePlanningTimeProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setMinimumRoutePlanningTimeSample({
				SampledAgentPropertyType::MinimumRoutePlanningTime, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getMinimumRoutePlanningTimeSample()
				|| agent->getMinimumRoutePlanningTimeSample()->sourceTag != id) continue;
			agent->clearMinimumRoutePlanningTimeSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id,
		AgentMaximumRoutePlanningTimeProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setMaximumRoutePlanningTimeSample({
				SampledAgentPropertyType::MaximumRoutePlanningTime, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getMaximumRoutePlanningTimeSample()
				|| agent->getMaximumRoutePlanningTimeSample()->sourceTag != id) continue;
			agent->clearMaximumRoutePlanningTimeSample();
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagInteractionAversionSamples(AgentTagId id,
		AgentInteractionAversionProperty const& property)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			agent->setInteractionAversionSample({
				SampledAgentPropertyType::InteractionAversion, id, property.revision,
				sampleAgentModifier(property.range) });
			changed = true;
		}
		if (changed) modify();
	}

	void World::clearAgentTagInteractionAversionSamples(AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->getInteractionAversionSample()
				|| agent->getInteractionAversionSample()->sourceTag != id) continue;
			agent->clearInteractionAversionSample();
			changed = true;
		}
		if (changed) modify();
	}

	uint32_t World::getCellsWide() const
	{
		return mCellsWide;
	}

	uint32_t World::getLevelsHigh() const
	{
		return mLevelsHigh;
	}

	uint32_t World::getLayerCount() const
	{
		return static_cast<uint32_t>(mLayers.size());
	}

	string const& World::getLayerName(uint32_t layerIndex) const
	{
		validateLayer("World::getLayerName", layerIndex);
		return mLayerNames[layerIndex];
	}

	void World::setLayerName(uint32_t layerIndex, std::string name)
	{
		invalidateSimulationSnapshot();
		validateLayer("World::setLayerName", layerIndex);
		mLayerNames[layerIndex] = std::move(name);
		modify();
	}

	string World::defaultLayerName(uint32_t layer)
	{
		return format("Layer {}", layer);
	}

	uint32_t World::addLayer()
	{
		invalidateSimulationSnapshot();
		auto const layerIndex = static_cast<uint32_t>(mLayers.size());

		if (layerIndex >= CORE_MAX_LAYERS)
		{
			throw WorldException(this,
				format("World cannot have more than {} layers", CORE_MAX_LAYERS));
		}

		// Adding a Layer repeats the same cells the existing Layers own, so the
		// World's total cell budget must still hold with the new Layer counted.
		string dimensionDiagnostic;
		if (!dimensionsAreSupported(mCellsWide, mLevelsHigh, layerIndex + 1,
			&dimensionDiagnostic))
		{
			throw WorldException(this, dimensionDiagnostic);
		}

		mLayers.push_back(make_shared<Layer>(this, mCellsWide, mLevelsHigh, layerIndex));
		mLayerNames.push_back(defaultLayerName(layerIndex));
		modify();

		return layerIndex;
	}

	uint32_t World::getNumSectors() const
	{
		return (uint32_t)mSectors.size();
	}

	void World::validateCellOccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.occupied())
		{
			throw WorldException(this, format("{} - cell at {},{} is not occupied.", caller, x, y));
		}
	}

	void World::validateCellUnoccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.occupied())
		{
			throw WorldException(this, format("{} - cell at {},{} is occupied.", caller, x, y));
		}
	}

	void World::validateCellIsInSector(string const& caller, uint32_t x, uint32_t y, shared_ptr<const Sector> sector) const
	{
		auto layer = getLayer(sector->getLayerIndex());

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sectorIndex = sector->getIndex();

		if (cellDef.sectorIndex != sectorIndex)
		{
			throw WorldException(this, format("{} - cell at {},{} is not in sector {}.", caller, x, y, sectorIndex));
		}
	}

	void World::validateCellHasNoObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.hasObject())
		{
			throw WorldException(this, format("{} - cell at {},{} has an object.", caller, x, y));
		}
	}

	void World::validateCellHasNoDoor(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.sectorObjectType == SectorObjectType::Door)
		{
			throw WorldException(this, format("{} - cell at {},{} has a door.", caller, x, y));
		}
	}


	void World::validateCellTraversableOnFoot(string const& caller, string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.isTraversableOnFoot())
		{
			throw WorldException(this, format("{} - cell at {},{} is not traversable, which blocks {} being placed", caller, x, y, desiredObject));
		}
	}

	void World::validateLayer(string const& caller, uint32_t layerIndex) const
	{
		if (layerIndex >= mLayers.size())
		{
			throw WorldException(this, format("{} - layerIndex={} is out of bounds", caller, layerIndex));
		}
	}

	void World::validateBounds(string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		if (x >= mCellsWide)
		{
			throw WorldException(this, format("{} - x={} is out of bounds", caller, x));
		}

		if (cellsWide > mCellsWide - x)
		{
			throw WorldException(this, format("{} - cellsWide={} is out of bounds", caller, cellsWide));
		}

		if (y >= mLevelsHigh)
		{
			throw WorldException(this, format("{} - y={} is out of bounds", caller, y));
		}

		if (levelsHigh > mLevelsHigh - y)
		{
			throw WorldException(this, format("{} - levelsHigh={} is out of bounds", caller, levelsHigh));
		}
	}

	void World::validateLayerSpace(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		auto layer = getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				validateCellUnoccupied(caller, layerIndex, ix, iy);
			}
		}
	}

	void World::validateObjectAllowedInSector(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = getSector(sectorIndex);

		if (!sector->sectorSupportsObjectType(type))
		{
			throw WorldException(this, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void World::validateObjectAllowedInSectorAsLookTarget(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = getSector(sectorIndex);

		if (!sector->sectorSupportsObjectAsLookTarget(type))
		{
			throw WorldException(this, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void World::validateSpaceOnlyInOneSector(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, bool allowAllBackgroundSpan) const
	{
		auto layer = getLayer(layerIndex);

		// A Window's back Layer is the one place where several Sectors behind one
		// aperture make sense: Backgrounds are seen, never entered, so a wide Window
		// looking across the seam between two of them still sees an unbroken view.
		// Half room-interior and half sky is a different matter - there should be a
		// wall where the room ends - so a mixed span is refused outright.
		if (allowAllBackgroundSpan)
		{
			bool seesBackground = false;
			bool seesOccupiedOther = false;
			for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
				for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				{
					auto const& cellDef = layer->getCellDefinition(ix, iy);
					if (cellDef.sectorIndex == ~0u) continue;
					if (mSectors[cellDef.sectorIndex]->getType() == SectorType::Background)
						seesBackground = true;
					else
						seesOccupiedOther = true;
				}
			if (seesBackground && !seesOccupiedOther) return;
			if (seesBackground && seesOccupiedOther)
				throw WorldException(this, format(
					"{} - bounds {},{} -> {},{} mix a Background with a Location or Transit on Layer {}; a Window cannot look half into a room and half into a Background, since there should be a wall where the room ends",
					caller, x, y, x + cellsWide, y + levelsHigh, layerIndex));
		}

		auto sectorIndex = layer->getCellDefinition(x, y).sectorIndex;

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = layer->getCellDefinition(ix, iy);

				if (cellDef.sectorIndex != sectorIndex)
				{
					throw WorldException(this, format("{} - bounds {},{} -> {},{} cross multiple Sectors", caller, x, y, x + cellsWide, y + levelsHigh));
				}
			}
		}
	}

	void World::validateSectorDoorOptions(string const& caller, CreateDoorOptions const& options) const
	{
		if (!Door::heightScaleIsValid(options.heightScale)
			|| (options.heightScale && options.height != Door::Height::Regular))
			throw WorldException(this, caller + ": Height scale requires a Regular Door and a finite value from 0.1 to 1.0");
		if (!Door::speedIsValid(options.speedOverride))
			throw WorldException(this, caller + ": Door speed must be finite and positive");
		if (!Door::brokenOpenPercentageIsValid(options.brokenOpenPercentage))
			throw WorldException(this, caller + ": Broken open percentage must be a finite value from 0 to 1");
		// A zero-width Door would cover no cell: its placement loop runs zero times,
		// leaving the Door's two Sectors unset for the queue configuration to
		// dereference. A crossing-lane count above the usable threshold width is not
		// buildable either, and until now it was only caught after the Door's
		// objects and traversal resource existed. Both are refused here, before any
		// object or resource exists (ticket #196).
		if (options.width == 0)
			throw WorldException(this, format("{} - a Door must be at least one cell wide", caller));
		if (options.crossingLanes > options.width)
			throw WorldException(this, format(
				"{} - Door crossing lane count {} exceeds the usable threshold width {}",
				caller, options.crossingLanes, options.width));
		if (options.height != Door::Height::Regular && options.height != Door::Height::Tall)
			throw WorldException(this, format("{} - unknown Door height", caller));
		// Finite as well as non-negative: NaN slips past every `< 0.0f` range
		// check and infinity is non-negative, and either would reach the
		// float-to-tick conversion of the Door's traversal resource (#198).
		if (!isFiniteTiming(options.holdOpenSeconds))
		{
			throw WorldException(this, format("{} - Door hold-open time must be finite and non-negative.", caller));
		}
		if (!mDeserializingConstruction)
			for (auto const& requirement : options.controlPermissionRequirements)
				for (auto permission : requirement)
					if (!lookupAccessPermission(permission))
						throw WorldException(this, format("{} - a Door control requirement references an unknown Access permission", caller));
	}

	void World::validateSectorDoorPlacement(string const& caller, uint32_t layerIndex, uint32_t y,
		uint32_t x, CreateDoorOptions const& options, bool controlsAreExternallyBound) const
	{
		auto const cellsWide = options.width;

		// A Door is authored on the front Layer of the pair it crosses.
		validateLayer(caller, layerIndex);
		if (layerIndex + 1 >= getLayerCount())
		{
			throw WorldException(this,
				format("{} - a Door needs a Layer directly behind the Layer it is authored on", caller));
		}
		auto const backLayer = layerBehind(layerIndex);

		validateSectorDoorOptions(caller, options);
		if (!controlsAreExternallyBound && options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
		{
			throw WorldException(this,
				format("{} - physical controls require remote-controlled activation", caller));
		}
		constexpr uint32_t levelsHigh = 1;
		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, cellsWide, levelsHigh);
		validateSpaceOnlyInOneSector(caller, backLayer, x, y, cellsWide, levelsHigh);

		auto const frontLayer = getLayer(layerIndex);
		auto const behindLayer = getLayer(backLayer);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef0 = frontLayer->getCellDefinition(ix, iy);
				auto const& cellDef1 = behindLayer->getCellDefinition(ix, iy);

				if (cellDef0.sectorIndex == ~0u)
				{
					throw WorldException(this, format("{} - front Layer cell at {},{} is not occupied.", caller, ix, iy));
				}

				if (options.height == Door::Height::Tall)
				{
					auto const room = dynamic_pointer_cast<const Location>(getSector(cellDef0.sectorIndex));
					if (!room || room->getType() != SectorType::Location || room->isCorridor())
						throw WorldException(this, format("{} - a tall Door is only available in a Room", caller));
				}

				if (cellDef1.sectorIndex == ~0u)
				{
					throw WorldException(this, format("{} - back Layer cell at {},{} is not occupied.", caller, ix, iy));
				}

				if (cellDef0.hasObject() || !cellDef0.markers.empty())
				{
					throw WorldException(this, format(
						"{} - another object occupies front Layer cell at {},{}", caller, ix, iy));
				}

				// The levels above the threshold are the opening's headroom: a Walkway or
				// other floor there runs through the opening.
				if (iy != y && (cellDef0.floorType != CellFloorType::None
					|| cellDef1.floorType != CellFloorType::None))
				{
					throw WorldException(this, format(
						"{} - a Door cannot open through a Walkway above its threshold", caller));
				}

				validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef0.sectorIndex);
				validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef1.sectorIndex);
			}
		if (options.heightScale && (!isLocationLike(getSector(frontLayer->getCellDefinition(x, y).sectorIndex)->getType())
			|| !isLocationLike(getSector(behindLayer->getCellDefinition(x, y).sectorIndex)->getType())))
			throw WorldException(this, caller + ": Height scale is only available for ordinary Doors between Locations");
		for (auto layer : {layerIndex, backLayer})
			validatePanelWallRectangle(mLayers[layer]->getCellDefinition(x, y).sectorIndex,
				{float(x) + CORE_DOOR_X_INSET, float(y)},
				{float(x + cellsWide) - CORE_DOOR_X_INSET, float(y)
					+ Door::effectiveHeight(options.height, options.heightScale)});
		if (!controlsAreExternallyBound)
			for (uint32_t side = 0; side < 2; ++side)
			{
				if (!options.controls[side]) continue;
				auto layer = side == 0 ? layerIndex : backLayer;
				auto sector = mSectors[mLayers[layer]->getCellDefinition(x, y).sectorIndex];
				auto demand = doorControlDemand(sector, x, y, cellsWide, side);
				(void)planPhysicalControls(layer, sector->getIndex(), y, &demand);
			}
	}

	void World::validateSectorForceBridgeOptions(string const& caller, CreateForceBridgeOptions const& options) const
	{
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			throw WorldException(this, format("{} - ForceBridge width must be [1,{}], not {}",
				caller, CORE_FORCEBRIDGE_MAX_SIZE, options.width));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			throw WorldException(this, format("{} - Physical control count must be [1,2] for a controlled ForceBridge, not {}", caller, options.controlCount));
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			throw WorldException(this, format("{} - A non-extensible ForceBridge must be permanently extended and have no controls", caller));
		for (size_t side = 0; side < 2; ++side)
		{
			bool const hasControl = options.extensible && (options.controlCount > 1
				|| (options.controlCount == 1 && static_cast<int>(side) == options.fromSide));
			if (!hasControl && !options.controlPermissionRequirements[side].empty())
				throw WorldException(this, format("{} - A Force Bridge permission requirement requires a control on that side", caller));
			set<AccessPermissionId> seen;
			for (auto permission : options.controlPermissionRequirements[side])
				if (!permission || permission.value > AccessPermission::Capacity
					|| (!mDeserializingConstruction && !lookupAccessPermission(permission))
					|| !seen.insert(permission).second)
					throw WorldException(this, format("{} - Invalid or duplicate Access permission requirement", caller));
		}
	}

	void World::validateSectorLadderOptions(string const& caller, CreateLadderOptions const& options) const
	{
		if (options.directionalBatchLimit == 0)
		{
			throw WorldException(this, format("{} - Ladder directional batch limit must be positive.", caller));
		}
		for (auto const& requirement : options.controlPermissionRequirements)
		{
			if (!options.extensible && !requirement.empty())
				throw WorldException(this, format("{} - A Ladder permission requirement requires an extensible Ladder", caller));
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || permission.value > AccessPermission::Capacity
					|| (!mDeserializingConstruction && !lookupAccessPermission(permission))
					|| !seen.insert(permission).second)
					throw WorldException(this, format("{} - Invalid or duplicate Access permission requirement", caller));
		}
	}

	void World::validateLiftOptions(string const& caller, CreateLiftOptions const& options) const
	{
		for (float speed : options.doorSpeeds)
			if (!std::isfinite(speed) || speed < 0.0f) throw WorldException(this, caller + ": Invalid landing Door speed");
		if (options.cellsWide == 0)
		{
			throw WorldException(this, format("{} - Lift width must be positive.", caller));
		}
		if (options.stopOffsets.size() < 2)
		{
			throw WorldException(this, format("{} - Lift must have at least 2 stops.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
		{
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
			{
				throw WorldException(this, format("{} - Lift stop offsets must be strictly increasing; stop {} ({}) is not above stop {} ({}).",
					caller, i, options.stopOffsets[i], i - 1, options.stopOffsets[i - 1]));
			}
		}
		if (options.capacity == 0)
		{
			throw WorldException(this, format("{} - Lift capacity must be positive.", caller));
		}
		if (options.initialStop >= options.stopOffsets.size())
		{
			throw WorldException(this, format("{} - Lift initial stop is out of range.", caller));
		}
		auto representablePositions = (uint32_t)floor((float)options.cellsWide / CORE_RESOURCE_SLOT_WIDTH);
		if (options.capacity > representablePositions)
		{
			throw WorldException(this, format("{} - Lift capacity {} exceeds {} representable interior standing positions.",
				caller, options.capacity, representablePositions));
		}
		// Finite as well as ordered: a NaN dwell or boarding window makes every
		// comparison false, and positive infinity stays non-negative (#198).
		if (!isFiniteTiming(options.minimumDwellSeconds)
			|| !isFiniteTiming(options.maximumBoardingSeconds)
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
		{
			throw WorldException(this, format("{} - Lift timing requires finite values with 0 <= minimum dwell <= maximum boarding time.", caller));
		}
		if (!options.landingControlPermissionRequirements.empty()
			&& options.landingControlPermissionRequirements.size() != options.stopOffsets.size())
			throw WorldException(this, format("{} - Lift landing requirements must be parallel to its stops.", caller));
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || !seen.insert(permission).second
					|| (!mDeserializingConstruction && !lookupAccessPermission(permission)))
					throw WorldException(this, format("{} - Invalid or duplicate Lift landing Access permission requirement.", caller));
		}
	}

	void World::validateShuttleOptions(string const& caller, CreateShuttleOptions const& options) const
	{
		for (float speed : options.doorSpeeds)
			if (!std::isfinite(speed) || speed < 0.0f) throw WorldException(this, caller + ": Invalid landing Door speed");
		if (options.numCars == 0)
			throw WorldException(this, format("{} - Shuttle must have at least one carriage.", caller));
		if (options.carWidth < 3 || options.carWidth > 5)
		{
			throw WorldException(this, format("{} - Shuttle car width must be between 3 and 5.", caller));
		}
		if (options.doorMask == 0 || (options.doorMask >> options.carWidth) != 0)
		{
			throw WorldException(this, format("{} - Shuttle carriage door layout must select at least one cell and remain within the carriage width.", caller));
		}

		if (options.stopOffsets.size() < 2)
			throw WorldException(this, format("{} - Shuttle must have at least two stops.", caller));
		if (options.initialStop >= (uint32_t)options.stopOffsets.size())
		{
			throw WorldException(this, format("{} - Shuttle initialStop parameter out of bounds.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
				throw WorldException(this, format("{} - Shuttle stop offsets must be strictly increasing.", caller));
		auto const representablePositions = maximumShuttleCarriageCapacity(options.carWidth);
		if (options.capacity == 0 || options.capacity > representablePositions)
			throw WorldException(this, format("{} - Shuttle capacity cannot be represented by buffered carriage standing positions.", caller));
		// Finite as well as ordered: a NaN dwell or boarding window makes every
		// comparison false, and positive infinity stays non-negative (#198).
		if (!isFiniteTiming(options.minimumDwellSeconds)
			|| !isFiniteTiming(options.maximumBoardingSeconds)
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
			throw WorldException(this, format("{} - Shuttle timing requires finite values with 0 <= minimum dwell <= maximum boarding time.", caller));
		auto const landingSlots = options.stopOffsets.size() * options.numCars
			* SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask).size();
		if (!options.landingControlPermissionRequirements.empty()
			&& options.landingControlPermissionRequirements.size() != landingSlots)
			throw WorldException(this, format("{} - Shuttle landing requirements must use its fixed landing grid.", caller));
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || !seen.insert(permission).second
					|| (!mDeserializingConstruction && !lookupAccessPermission(permission)))
					throw WorldException(this, format("{} - Invalid or duplicate Shuttle landing Access permission requirement.", caller));
		}
	}

	shared_ptr<const Layer> World::getLayer(uint32_t layerIndex) const
	{
		validateLayer(format("World::getLayer({})", layerIndex), layerIndex);
		return mLayers[layerIndex];
	}

	shared_ptr<Layer> World::getLayer(uint32_t layerIndex)
	{
		validateLayer(format("World::getLayer({})", layerIndex), layerIndex);
		return mLayers[layerIndex];
	}

	uint32_t World::createLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor)
	{
		invalidateSimulationSnapshot();
		auto sectorIndex = (uint32_t)mSectors.size();

		auto location = make_shared<Location>(name, type, layerIndex, sectorIndex, x, y, cellsWide, levelsHigh, topLevelHeight, ~0u, isCorridor);
		
		mSectors.push_back(location);
		return sectorIndex;
	}

	uint32_t World::createLadder(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options)
	{
		invalidateSimulationSnapshot();
		auto y0 = y;
		auto y1 = y + options.levelsHigh - 1;

		// Get Locations this Ladder connects.  A Transit on layerIndex lands on the
		// Layer directly in front of it, never on a Layer of its own choosing.
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto const& cellDef0 = landing->getCellDefinition(x, y0);
		auto const& cellDef1 = landing->getCellDefinition(x, y1);
		
		shared_ptr<const Sector> sectors[2] = {
			getSector(cellDef0.sectorIndex),
			getSector(cellDef1.sectorIndex)
		};

		// Transit stops
		vector<TransitStop> stops = {
			{ sectors[CORE_LADDER_ENDPOINT_LOW], (int)x - (int)sectors[0]->getCellX(), (int)y - (int)sectors[0]->getCellY() },
			{ sectors[CORE_LADDER_ENDPOINT_HIGH], (int)x - (int)sectors[1]->getCellX(), (int)y - (int)sectors[1]->getCellY() }
		};

		auto sectorIndex = (uint32_t)mSectors.size();
		auto ladder = make_shared<LadderTransit>(sectorIndex, layerIndex, x, y, options.levelsHigh, stops, options.extensible, options.startExtended);

		mSectors.push_back(ladder);
		return sectorIndex;
	}

	uint32_t World::createStairwell(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t levelsHigh, int mountSide)
	{
		invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(mountSide);

		// Get Locations this Stairwell connects.  Because a Stairwell is two cells wide, we
		// just check the first horizontal cell, ie xOffset==0.  Every landing sits on the
		// Layer directly in front of the Transit.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			auto const& cellDef = landing->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({ 
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto stairwell = make_shared<StairwellTransit>(sectorIndex, layerIndex, x, y, levelsHigh, mountSide, stops);

		mSectors.push_back(stairwell);
		return sectorIndex;
	}

	uint32_t World::createStaircase(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
		int riseSide, float speed)
	{
		invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(riseSide);
		uint32_t const lowerX = riseSide == CORE_SIDE_RIGHT ? x : x + cellsWide - 1;
		uint32_t const upperX = riseSide == CORE_SIDE_RIGHT ? x + cellsWide - 1 : x;
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto const& lowerCell = landing->getCellDefinition(lowerX, y);
		auto const& upperCell = landing->getCellDefinition(upperX, y + 1);
		auto lower = getSector(lowerCell.sectorIndex);
		auto upper = getSector(upperCell.sectorIndex);
		vector<TransitStop> stops{
			{ lower, (int)lowerX - (int)lower->getCellX(), (int)y - (int)lower->getCellY() },
			{ upper, (int)upperX - (int)upper->getCellX(), (int)(y + 1) - (int)upper->getCellY() }
		};
		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<StaircaseTransit>(sectorIndex, layerIndex, x, y, cellsWide,
			riseSide, speed, stops));
		static_pointer_cast<StaircaseTransit>(mSectors.back())->getStaircase()->mSectorIndex = sectorIndex;
		return sectorIndex;
	}

	World::CreateObjectResult World::createLift(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
		uint32_t levelsHigh, vector<uint32_t> const& stopOffsets)
	{
		invalidateSimulationSnapshot();
		// Get Locations this Lift connects, all on the Layer directly in front.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (auto stopOffset : stopOffsets)
		{
			uint32_t iy = y + stopOffset;

			auto const& cellDef = landing->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto lift = make_shared<LiftTransit>(sectorIndex, layerIndex, x, y, cellsWide, levelsHigh, stops);

		mSectors.push_back(lift);
		
		return {
			~0u,
			SectorObjectType::Lift,
			lift
		};
	}

	World::CreateObjectResult World::createShuttle(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, vector<uint32_t> const& stopOffsets)
	{
		invalidateSimulationSnapshot();
		// Get Locations this Shuttle connects, all on the Layer directly in front.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (auto stopOffset : stopOffsets)
		{
			uint32_t ix = x + stopOffset;
			shared_ptr<const Sector> sector;
			for (uint32_t car = 0; car < numCars && !sector; ++car)
			{
				uint32_t cx = ix + car * (carWidth + 1) + 1;
				auto const& cellDef = landing->getCellDefinition(cx, y);
				bool supported = cellDef.sectorIndex != ~0u;
				if (supported && carWidth == 4)
					supported = landing->getCellDefinition(cx + 1, y).sectorIndex
						== cellDef.sectorIndex;
				if (supported) sector = getSector(cellDef.sectorIndex);
			}
			assert(sector);
			stops.push_back({
				sector,
				(int)ix - (int)sector->getCellX(),
				(int)y - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();

		// Need to account for proper track dimensions, width is not necessarily
		// the last stop offset
		auto shuttle = make_shared<ShuttleTransit>(sectorIndex, layerIndex, x, y, cellsWide, numCars, carWidth, stops);

		mSectors.push_back(shuttle);
		
		return {
			~0u,
			SectorObjectType::Shuttle,
			shuttle
		};
	}

	uint32_t World::addLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::addLocation({}, {}, {}, {}, {}, {}, {} {})", name, getSectorTypeString(type), layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight);
		
		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);

		// Create sector
		auto sectorIndex = createLocation(name, type, layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight, isCorridor);

		// Set layers
		auto layer = getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		return sectorIndex;
	}

	World::CreateObjectResult World::createDoor(uint32_t layerIndex, uint32_t x, uint32_t y,
		uint32_t cellsWide, Door::Height height, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::createDoor({}, {}, {}, {})", layerIndex, x, y, cellsWide);

		// A Door is authored on the front Layer of its pair and opens into the Layer
		// directly behind it.
		auto const backLayer = layerBehind(layerIndex);

		validateCellOccupied(caller, layerIndex, x, y);
		validateCellOccupied(caller, backLayer, x, y);
		// A threshold is owned by the Layer where it is authored. Objects on the
		// destination Layer coexist with it and do not block its placement.
		validateCellHasNoDoor(caller, layerIndex, x, y);

		// Find Sectors that the Door is connecting.
		auto foreSector = _getSector(mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		auto backSector = _getSector(mLayers[backLayer]->getCellDefinition(x, y).sectorIndex);

		// A Door's front Sector is a Location or a Facade: a Facade follows
		// the Room hosting rule (ADR 0003, ticket #48).
		assert(isLocationLike(foreSector->getType()));

		// Create door in Fore Location and add to Back.
		uint32_t doorIndex = foreSector->createDoor(foreSector, backSector, x, y, cellsWide, height, vertexIdentifier);
		backSector->addDoor(dynamic_pointer_cast<DoorSectorObject>(foreSector->_getObject(doorIndex)));

		return {
			doorIndex,
			SectorObjectType::Door,
			foreSector
		};
	}

	World::CreateObjectResult World::createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier, bool boothWindow)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::createWindow({}, {}, {}, {}, {})", layerIndex, x, y, cellsWide, levelsHigh);

		// A Window is authored on the front Layer of the pair it crosses.  The Layer
		// behind it is only absent when a map written before the back-most Layer rule
		// is replayed: authoring such a Window is refused by canAddSectorWindow(), and a
		// Layer deletion removes one it would strand there.
		bool const hasBack = layerIndex + 1 < getLayerCount();

		validateCellOccupied(caller, layerIndex, x, y);
		if (hasBack)
			validateCellOccupied(caller, layerBehind(layerIndex), x, y);

		// Find sectors that the Window is connecting, if any.
		shared_ptr<Sector> foreSector{ nullptr };
		shared_ptr<Sector> backSector{ nullptr };

		foreSector = _getSector(mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		if (hasBack)
		{
			auto const& cellDef1 = mLayers[layerBehind(layerIndex)]->getCellDefinition(x, y);

			// The back Sector is the Sector behind the Window's first cell.  A Window
			// may span several Backgrounds (#36), so for such a Window this is the
			// first Background in the span and is non-authoritative; the cell grid
			// holds the truth about what the whole aperture looks into.
			backSector = cellDef1.sectorIndex != ~0u ? _getSector(cellDef1.sectorIndex) : nullptr;
		}

		// Create window in fore Location and add to back
		auto windowIndex = foreSector->createWindow(foreSector, backSector, x, y, cellsWide, levelsHigh, vertexIdentifier, boothWindow);

		if (backSector)
		{
			backSector->addWindow(dynamic_pointer_cast<WindowSectorObject>(foreSector->_getObject(windowIndex)));
		}

		return {
			windowIndex,
			boothWindow ? SectorObjectType::BoothWindow : SectorObjectType::Window,
			foreSector
		};
	}

	World::CreateObjectResult World::createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side)
	{
		invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(side);

		string caller = format("World::createBulkheadDoor({}, {}, {}, {})", layerIndex, x, y, side);
		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		validateCellOccupied(caller, layerIndex, cx0, y);
		validateCellOccupied(caller, layerIndex, cx1, y);

		// Find locations that the Door is connecting.
		shared_ptr<Sector> leftSector{ nullptr };
		shared_ptr<Sector> rightSector{ nullptr };

		auto layer = getLayer(layerIndex);

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		leftSector = _getSector(cellDef0.sectorIndex);
		rightSector = _getSector(cellDef1.sectorIndex);

		// Create bulkhead door in one location and add to the other
		auto doorIndex = leftSector->createBulkheadDoor(leftSector, rightSector, y - leftSector->getCellY(), CORE_SIDE_LEFT);

		rightSector->addBulkheadDoor(dynamic_pointer_cast<BulkheadDoorSectorObject>(leftSector->_getObject(doorIndex)), y - rightSector->getCellY(), CORE_SIDE_RIGHT);
		
		return {
			doorIndex,
			SectorObjectType::BulkheadDoor,
			leftSector
		};
	}

	World::CreateObjectResult World::createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::createWalkway({}, {}, {})", layerIndex, x, y);
		
		validateCellOccupied(caller, layerIndex, x, y);

		// Create Walkway
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createWalkway(sector, x, y, vertexIdentifier),
			SectorObjectType::Walkway,
			sector
		};
	}

	World::CreateObjectResult World::createMarker(uint32_t layerIndex, uint32_t x,
		uint32_t y, float xOffset, MarkerId id, string name,
		MarkerProperties properties, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::createMarker({}, {}, {}, {})", layerIndex, x, y, xOffset);

		float xPos = x + xOffset;
		x = (uint32_t)xPos;
		

		validateCellOccupied(caller, layerIndex, x, y);

		// Create Marker
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createMarker(sector, id, std::move(name), properties,
				x, y, xPos - x, vertexIdentifier),
			SectorObjectType::Marker,
			sector
		};
	}

	World::CreateObjectResult World::createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y, CreateForceBridgeOptions const& options)
	{
		invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(options.fromSide);

		string caller = format("World::createForceBridge({}, {}, {}, {}, {}, {})", layerIndex, x, y, options.width, options.fromSide, options.startExtended);

		validateCellOccupied(caller, layerIndex, x, y);

		// Create ForceBridge
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createForceBridge(sector, x, y, options.width, options.fromSide, options.extensible, options.startExtended),
			SectorObjectType::ForceBridge,
			sector
		};
	}

	World::CreateObjectResult World::createLadderSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		auto layer = getLayer(layerIndex);

		string caller = format("World::createLadderSectorObject({}, {}, {}, {}, {})", layerIndex, x, y, options.startExtended, options.levelsHigh);

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createLadder(sector, x, y, options.extensible, options.startExtended, options.levelsHigh, vertexIdentifier),
			SectorObjectType::Ladder,
			sector
		};
	}

	World::CreateObjectResult World::createPlatformLiftSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLiftOptions const& options, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		auto layer = getLayer(layerIndex);

		string caller = format("World::createPlatformLiftSectorObject({}, {}, {})", layerIndex, x, y);

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			// LiftSectorObject and Lift both expect offsets relative to the global
			// base y. Passing global stop positions here would apply y twice and make
			// PlatformLifts in offset Rooms start above their ground floor.
			sector->createPlatformLift(sector, x, y, options.cellsWide,
				options.stopOffsets, vertexIdentifier),
			SectorObjectType::Lift,
			sector
		};
	}

	shared_ptr<Sector> World::_getSector(uint32_t index)
	{
		invalidateSimulationSnapshot();
		if (index >= getNumSectors())
		{
			throw WorldException(this, format("World::getSector({}) - index={} is out of range.", index, index));
		}

		return mSectors[index];
	}

	shared_ptr<const Sector> World::getSector(uint32_t index) const
	{
		if (index >= getNumSectors())
		{
			throw WorldException(this, format("World::getSector({}) - index={} is out of range.", index, index));
		}

		return mSectors[index];
	}

	vector<shared_ptr<const Sector>> World::getSectorsInBounds(uint32_t layerIndex, float x, float y, float width, float height) const
	{
		auto layer = getLayer(layerIndex);

		// Convert screen bounds to cell bounds
		int cellX0 = max((int)(x / CORE_CELL_WIDTH_PIXELS), 0);
		int cellY0 = max((int)(y / CORE_LEVEL_HEIGHT_PIXELS), 0);
		int cellX1 = min((int)((x + width) / CORE_CELL_WIDTH_PIXELS), (int)mCellsWide - 1);
		int cellY1 = min((int)((y + height) / CORE_LEVEL_HEIGHT_PIXELS), (int)mLevelsHigh - 1);

		// Add all Locations to a set, as a single Location will have a reference for every CellDefinition,
		// but we only want it once.
		set<shared_ptr<const Sector>> sectors;

		for (int y = cellY0; y <= cellY1; ++y)
		{
			for (int x = cellX0; x <= cellX1; ++x)
			{
				auto const& cellDef = layer->getCellDefinition(x, y);

				if (cellDef.sectorIndex != ~0u)
				{
					auto location = getSector(cellDef.sectorIndex);
					sectors.insert(location);
				}
			}
		}

		return vector<shared_ptr<const Sector>>(sectors.begin(), sectors.end());
	}

	vector<shared_ptr<const Sector>> World::getSectors(uint32_t layerIndex) const
	{
		vector<shared_ptr<const Sector>> sectors;

		for (auto sector : mSectors)
		{
			if (sector->getLayerIndex() == layerIndex)
			{
				sectors.push_back(sector);
			}
		}

		return sectors;
	}

	shared_ptr<const Graph> World::getGraph() const
	{
		return mGraph;
	}

	Log const& World::getBuildLog() const
	{
		return mBuildLog;
	}

	uint32_t World::addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		invalidateSimulationSnapshot();
		return addCorridor(0, y, x, cellsWide, levelsHigh);
	}

	uint32_t World::addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		invalidateSimulationSnapshot();
		string const caller = format("World::addCorridor({}, {}, {}, {}, {})", layerIndex, y, x, cellsWide, levelsHigh);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		validateLayer(caller, layerIndex);
		// Minimum (1,1). A zero-sized Location would pass the bounds checks by covering
		// nothing, leaving a Sector no cell references: invisible, unselectable, and
		// unreachable forever (ticket #64).
		if (cellsWide == 0)
			throw WorldException(this, format("{} - a Corridor must be at least one cell wide", caller));
		if (levelsHigh == 0)
			throw WorldException(this, format("{} - a Corridor must be at least one level high", caller));
		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, true);
		beginStructuralEdit("addCorridor");
		auto const result = addLocation("Corridor", SectorType::Location, layerIndex, x, y, cellsWide, levelsHigh, CORE_CORRIDOR_HEIGHT, true);
		reflowAllPhysicalControls();
		ConstructionRecord record{ ConstructionType::Corridor };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		recordConstruction(std::move(record));
		return result;
	}

	uint32_t World::addRoom(string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight)
	{
		invalidateSimulationSnapshot();
		string const caller = format("World::addRoom({}, {}, {}, {}, {}, {}, {})", name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		// Written as a negated in-range test so a NaN topLevelHeight is rejected
		// too: NaN fails both comparisons, so the plain < / > pair would let it
		// through (ticket #55).
		if (!(topLevelHeight >= CORE_ROOM_MIN_HEIGHT && topLevelHeight <= CORE_ROOM_MAX_HEIGHT))
		{
			throw WorldException(this, format("{} - topLevelHeight={} is out of range", caller, topLevelHeight));
		}
		// Minimum (1,1), the same invariant Background and Facade enforce: a
		// zero-sized Room would pass the bounds checks by covering nothing,
		// leaving a Sector no cell references (ticket #64).
		if (cellsWide == 0)
			throw WorldException(this, format("{} - a Room must be at least one cell wide", caller));
		if (levelsHigh == 0)
			throw WorldException(this, format("{} - a Room must be at least one level high", caller));

		validateLayer(caller, layerIndex);
		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, true);
		beginStructuralEdit("addRoom");

		auto const result = addLocation(name, SectorType::Location, layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight, false);
		reflowAllPhysicalControls();
		ConstructionRecord record{ ConstructionType::Room };
		record.name = name;
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = levelsHigh;
		record.x = topLevelHeight;
		recordConstruction(std::move(record));
		return result;
	}

	bool World::canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("World::addBackground({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, levelsHigh);

		try
		{
			validateLayer(caller, layerIndex);

			// Minimum (1,1). A zero-sized block would pass the bounds checks by covering
			// nothing, which is not a Background worth authoring.
			if (cellsWide == 0)
				throw WorldException(this, format("{} - a Background must be at least one cell wide", caller));
			if (levelsHigh == 0)
				throw WorldException(this, format("{} - a Background must be at least one level high", caller));

			validateBounds(caller, x, y, cellsWide, levelsHigh);

			// Every cell must be unoccupied on the Background's own Layer, which is the
			// same rule a Location plays by. Any Layer is legal, front-most and back-most
			// included: a "back layers only" rule would re-introduce exactly the
			// Fore/Back special-casing that ADR 0002 removed.
			validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t World::addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, BackgroundColour const& colour)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddBackground(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic))
			throw WorldException(this, diagnostic);

		validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, false);
		beginStructuralEdit("addBackground");

		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<Background>("Background", layerIndex, sectorIndex,
			x, y, cellsWide, levelsHigh, colour));

		auto layer = getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Background owns no walkable floor and hosts no object, so its cells
				// carry no floor and no SectorObject.
				cellDef.floorType = Background::cellFloorType();
				cellDef.sectorObjectType = SectorObjectType::None;
			}
		}

		reflowAllPhysicalControls();
		ConstructionRecord record{ ConstructionType::Background };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		// The whole Background appearance fits one integer, so no new record field is
		// needed to persist its colour.
		record.f = packBackgroundColour(colour);
		recordConstruction(std::move(record));

		return sectorIndex;
	}

	bool World::canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("World::addFacade({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, levelsHigh);

		try
		{
			validateLayer(caller, layerIndex);

			// A Facade is placed exactly as a Room is: minimum (1,1), inside the
			// World bounds, and on cells unoccupied on its own Layer.
			if (cellsWide == 0)
				throw WorldException(this, format("{} - a Facade must be at least one cell wide", caller));
			if (levelsHigh == 0)
				throw WorldException(this, format("{} - a Facade must be at least one level high", caller));
			// Same negated in-range test as addRoom: a NaN topLevelHeight must
			// not sail through the < / > pair (ticket #55).
			if (!(topLevelHeight >= CORE_ROOM_MIN_HEIGHT && topLevelHeight <= CORE_ROOM_MAX_HEIGHT))
				throw WorldException(this, format("{} - topLevelHeight={} is out of range", caller, topLevelHeight));

			validateBounds(caller, x, y, cellsWide, levelsHigh);
			validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t World::addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		invalidateSimulationSnapshot();
		return addFacade(Facade::defaultName(), layerIndex, y, x, cellsWide, levelsHigh,
			topLevelHeight, colour);
	}

	uint32_t World::addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, &diagnostic))
			throw WorldException(this, diagnostic);

		validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, false);
		beginStructuralEdit("addFacade");

		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<Facade>(name, layerIndex, sectorIndex,
			x, y, cellsWide, levelsHigh, topLevelHeight, colour));

		auto layer = getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Facade owns walkable floor exactly as a Location does: ground
				// on the bottom level, upper levels reached by Walkways.
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		reflowAllPhysicalControls();
		ConstructionRecord record{ ConstructionType::Facade };
		record.name = name;
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		record.x = topLevelHeight;
		// The Facade reuses Background's colour packing, so the editor and the
		// headless checks round-trip the same arithmetic (ADR 0003).
		record.f = packBackgroundColour(colour);
		recordConstruction(std::move(record));

		return sectorIndex;
	}

	bool World::canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Ladder must sit on a Layer that has a Layer in front of it to land on");
		if (levelsHigh < 2) return reject("A Ladder must span at least two levels");
		if (x >= mCellsWide || y >= mLevelsHigh || y + levelsHigh > mLevelsHigh)
			return reject("The Ladder is outside the World bounds");
		auto const& transitLayer = mLayers[layerIndex];
		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			if (transitLayer->getCellDefinition(x, iy).occupied())
				return reject(format("A Sector at {},{} blocks the Ladder", x, iy));

		auto upperY = y + levelsHigh - 1;
		auto const& lower = landing->getCellDefinition(x, y);
		auto const& upper = landing->getCellDefinition(x, upperY);
		if (lower.sectorIndex == ~0u)
			return reject(format("A landing Location is required at {},{}", x, y));
		if (upper.sectorIndex == ~0u)
			return reject(format("A landing Location is required at {},{}", x, upperY));
		if (lower.sectorIndex == upper.sectorIndex)
			return reject("A Ladder must connect two different landing Locations");
		auto lowerSector = mSectors[lower.sectorIndex];
		auto upperSector = mSectors[upper.sectorIndex];
		// A Ladder lands on any location-like Sector: a Room, a Corridor, or a
		// Facade.  A Facade takes part in traversal exactly as a Room does
		// (ADR 0003, ticket #52), so it is a valid Ladder landing.
		if (!lowerSector || !isLocationLike(lowerSector->getType()))
			return reject(format("A landing Location is required at {},{}", x, y));
		if (!upperSector || !isLocationLike(upperSector->getType()))
			return reject(format("A landing Location is required at {},{}", x, upperY));
		if (!lower.isTraversableOnFoot())
			return reject(format("The landing floor at {},{} is not traversable", x, y));
		if (!upper.isTraversableOnFoot())
			return reject(format("The landing floor at {},{} is not traversable", x, upperY));
		return true;
	}

	World::CreateLadderResult World::addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLadderOptions const& options)
	{
		invalidateSimulationSnapshot();
		// A Ladder Transit sits on layerIndex and lands on the Layer directly in front.
		auto const landingLayer = layerInFront(layerIndex);
		auto transitLayer = getLayer(layerIndex);

		// Checks
		string caller = format("World::addLadder({}, {}, {}, {}, {})", layerIndex, y, x, options.levelsHigh, options.startExtended);

		validateLayer(caller, layerIndex);
		if (isFrontMostLayer(layerIndex))
		{
			throw WorldException(this, format("{} - a Ladder cannot be placed on the front-most Layer, because it has no Layer in front to land on", caller));
		}

		if (options.levelsHigh < 2)
		{
			throw WorldException(this, format("{} - Ladder at {},{} must be at least 2 levels high", caller, x, y));
		}

		validateBounds(caller, x, y, 1, options.levelsHigh);
		validateLayerSpace(caller, layerIndex, x, y, 1, options.levelsHigh);

		auto y0 = y;
		auto y1 = y + options.levelsHigh - 1;

		// Make sure the landing cells have a Sector
		auto const& cellDef0 = mLayers[landingLayer]->getCellDefinition(x, y0);
		auto const& cellDef1 = mLayers[landingLayer]->getCellDefinition(x, y1);
		auto foreSectorIndex0 = cellDef0.sectorIndex;
		auto foreSectorIndex1 = cellDef1.sectorIndex;

		if (foreSectorIndex0 == ~0u)
		{
			throw WorldException(this, format("{} - landing cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y0));
		}
		if (foreSectorIndex1 == ~0u)
		{
			throw WorldException(this, format("{} - landing cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y1));
		}
		if (foreSectorIndex0 == foreSectorIndex1)
		{
			throw WorldException(this, format("{} - landing cells from {},{} to {},{} are the same sector, which blocks ladder being placed", caller, x, y0, x, y1));
		}

		// Ladders can only connect location-like Sectors: Rooms, Corridors, and
		// Facades (ADR 0003, ticket #52).
		auto const& foreSector0 = _getSector(foreSectorIndex0);
		auto const& foreSector1 = _getSector(foreSectorIndex1);

		if (!isLocationLike(foreSector0->getType()))
		{
			throw WorldException(this, format("{} - landing cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y0));
		}
		if (!isLocationLike(foreSector1->getType()))
		{
			throw WorldException(this, format("{} - landing cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y1));
		}

		// Ladders ends must not be in the air
		validateCellTraversableOnFoot(caller, "Ladder", landingLayer, x, y0);
		validateCellTraversableOnFoot(caller, "Ladder", landingLayer, x, y1);

		validateSectorLadderOptions(caller, options);

		physicalControl::Geometry controlGeometry{ layerIndex, x, y, 1, options.levelsHigh };
		if (options.extensible)
			validatePhysicalControlAdditions({
				transportControlDemand(foreSector0, physicalControl::OwnerType::Ladder, controlGeometry, x, y0, 1),
				transportControlDemand(foreSector1, physicalControl::OwnerType::Ladder, controlGeometry, x, y1, 1) });
		beginStructuralEdit("addLadder");

		// Create ladder
		auto sectorIndex = createLadder(layerIndex, x, y, options);

		// Set layers
		for (uint32_t iy = y; iy < y + options.levelsHigh; ++iy)
		{
			auto& cellDef = transitLayer->getCellDefinition(x, iy);

			cellDef.sectorIndex = sectorIndex;
		}

		auto ladderSector = _getSector(sectorIndex);
		auto ladderTransit = dynamic_pointer_cast<LadderTransit>(ladderSector);
		auto ladder = ladderTransit->getLadder();
		ladder->mInitiallyBroken = ladder->mBroken = options.extensible && options.initiallyBroken;
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalBatchLimit);
		configureLadderQueueLanes(traversalResource,
			{ SectorId{ (uint64_t)foreSector0->getIndex() + 1 },
				SectorId{ (uint64_t)foreSector1->getIndex() + 1 } },
			{ Vector2{ (float)x + 0.5f, (float)y0 },
				Vector2{ (float)x + 0.5f, (float)y1 } });
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult& control, size_t endpoint)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createPhysicalControlInteractionPoint("Ladder extension control",
				control, (float)object->getCellY(), 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
			if (auto interaction = mInteractionPoints.find(point))
				for (auto permission : options.controlPermissionRequirements[endpoint])
					interaction->mPermissionRequirement.set(permission.value - 1);
			addTraversalControl(traversalResource, point);
		};

		CreateObjectResult createdControls[2];
		if (options.extensible)
		{
			createdControls[CORE_LADDER_ENDPOINT_LOW] = createPhysicalControl("Ladder button", landingLayer, y0,
				transportControlDemand(foreSector0, physicalControl::OwnerType::Ladder, controlGeometry, x, y0, 1), 0);
			registerExtensionControl(createdControls[CORE_LADDER_ENDPOINT_LOW], CORE_LADDER_ENDPOINT_LOW);

			createdControls[CORE_LADDER_ENDPOINT_HIGH] = createPhysicalControl("Ladder button", landingLayer, y1,
				transportControlDemand(foreSector1, physicalControl::OwnerType::Ladder, controlGeometry, x, y1, 1), 0);
			registerExtensionControl(createdControls[CORE_LADDER_ENDPOINT_HIGH], CORE_LADDER_ENDPOINT_HIGH);
		}

		CreateLadderResult result{
			{ ~0u, SectorObjectType::Ladder, ladderSector },
			{ createdControls[0], createdControls[1] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::Ladder };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.levelsHigh; record.d = options.directionalBatchLimit;
		record.p = options.extensible; record.q = options.startExtended;
		record.initiallyBroken = options.extensible && options.initiallyBroken;
		for (size_t endpoint = 0; endpoint < 2; ++endpoint)
			for (auto permission : options.controlPermissionRequirements[endpoint])
				record.controlPermissionRequirements[endpoint].push_back(
					static_cast<uint32_t>(permission.value));
		recordConstruction(std::move(record));
		return result;
	}

	bool World::canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Stairwell must sit on a Layer that has a Layer in front of it to land on");
		if (levelsHigh < 2) return reject("A Stairwell must span at least two levels");
		if (x >= mCellsWide || y >= mLevelsHigh || x + 2 > mCellsWide
			|| y + levelsHigh > mLevelsHigh)
			return reject("The Stairwell is outside the World bounds");
		auto const& transitLayer = mLayers[layerIndex];
		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			auto const& first = landing->getCellDefinition(x, iy);
			if (first.sectorIndex == ~0u)
				return reject(format("A landing Location is required at {},{}", x, iy));
			auto sector = mSectors[first.sectorIndex];
			// A Stairwell lands on any location-like Sector, a Facade included
			// (ADR 0003, ticket #52).
			if (!sector || !isLocationLike(sector->getType()))
				return reject(format("A landing Location is required at {},{}", x, iy));
			for (uint32_t ix = x; ix < x + 2; ++ix)
			{
				auto const& fore = landing->getCellDefinition(ix, iy);
				if (fore.sectorIndex != first.sectorIndex)
					return reject(format("The Stairwell spans different landing Locations at level {}", iy));
				if (!fore.isTraversableOnFoot())
					return reject(format("The landing floor at {},{} is not traversable", ix, iy));
				auto const occupant = transitLayer->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u)
					return reject(format("A Sector at {},{} blocks the Stairwell", ix, iy));
			}
		}
		return true;
	}

	uint32_t World::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, int mountSide)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("addStairwell");
		return addStairwell(layerIndex, y, x, CreateStairwellOptions{ levelsHigh, mountSide }).sectorIndex;
	}

	World::CreateStairwellResult World::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateStairwellOptions const& options)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("addStairwell");
		auto levelsHigh = options.levelsHigh;
		auto mountSide = options.mountSide;
		ASSERT_SIDE_OK(mountSide);

		// A Stairwell Transit sits on layerIndex and lands on the Layer directly in front.
		auto const landingLayer = layerInFront(layerIndex);
		auto transitLayer = getLayer(layerIndex);
		const uint32_t cellsWide = 2;

		// Checks
		string caller = format("World::addStairwell({}, {}, {}, {}, {})", layerIndex, y, x, levelsHigh, mountSide);

		validateLayer(caller, layerIndex);
		if (isFrontMostLayer(layerIndex))
		{
			throw WorldException(this, format("{} - a Stairwell cannot be placed on the front-most Layer, because it has no Layer in front to land on", caller));
		}

		if (levelsHigh < 2)
		{
			throw WorldException(this, format("{} - Stairwell at {},{} must be at least 2 levels high", caller, x, y));
		}

		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			auto const& cellDef0 = mLayers[landingLayer]->getCellDefinition(x, iy);
			auto levelSectorIndex = cellDef0.sectorIndex;

			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = mLayers[landingLayer]->getCellDefinition(ix, iy);
				auto foreSectorIndex = cellDef.sectorIndex;

				// Make sure the landing cells have a Sector, and that the horizontal Sectors are not different:
				// Stairwells cannot span different Sectors horizontally, due to placement of the door leading to them.
				if (foreSectorIndex != levelSectorIndex)
				{
					throw WorldException(this, format("{} - the stairwell horizontally spans different landing Sectors between {},{} and {},{}, which is not allowed", caller, x, iy, x + 1, iy));
				}

				// Landing Sector can't be empty
				if (foreSectorIndex == ~0u)
				{
					throw WorldException(this, format("{} - landing cell at {},{} is not occupied, which blocks stairwell being placed", caller, ix, iy));
				}

				// Stairwells can only connect location-like Sectors: Rooms,
				// Corridors, and Facades (ADR 0003, ticket #52).
				auto const& foreSector = getSector(foreSectorIndex);

				if (!isLocationLike(foreSector->getType()))
				{
					throw WorldException(this, format("{} - landing cell at {},{} is not a Location, which blocks stairwell being placed", caller, ix, iy));
				}

				// Stairwells must not be in the air
				validateCellTraversableOnFoot(caller, "Stairwell", landingLayer, ix, iy);
			}
		}

		if (options.directionalCapacity > 0 && options.directionalBatchLimit == 0)
		{
			throw WorldException(this, format("{} - Narrow stairwell directional batch limit must be positive.", caller));
		}

		// Create stairwell
		auto sectorIndex = createStairwell(layerIndex, x, y, levelsHigh, mountSide);

		// Set layers
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = transitLayer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
			}
		}

		TraversalResourceId traversalResource;
		if (options.directionalCapacity > 0)
		{
			auto stairwellTransit = dynamic_pointer_cast<StairwellTransit>(_getSector(sectorIndex));
			auto stairwell = stairwellTransit->getStairwell();
			traversalResource = createStairwellTraversalResource("Narrow stairwell capacity",
				stairwell, SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalCapacity,
				options.directionalBatchLimit);
			stairwell->configureTraversal(traversalResource);
		}

		ConstructionRecord record{ ConstructionType::Stairwell };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.levelsHigh;
		record.i = options.mountSide; record.d = options.directionalCapacity; record.e = options.directionalBatchLimit;
		recordConstruction(std::move(record));
		return { sectorIndex, traversalResource };
	}

	bool World::validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
		int /*riseSide*/, string& diagnostic) const
	{
		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		if (!cell.occupied())
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		auto location = dynamic_pointer_cast<const Location>(mSectors[cell.sectorIndex]);
		if (!location)
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		// Lower landings and Corridor landings require ordinary walkable floor.
		// An upper Room landing may instead terminate at an open side wall whose
		// adjacent Location supplies the walkable landing floor.
		if (!upperEndpoint || location->isCorridor())
		{
			if (cell.isTraversableOnFoot()) return true;
			diagnostic = format("Traversable landing floor is required at {},{}", x, y);
			return false;
		}

		auto const roomLevel = y - location->getCellY();
		auto openLandingAt = [&](int wallSide)
		{
			uint32_t const boundaryX = wallSide == CORE_SIDE_LEFT
				? location->getCellX0() : location->getCellX1();
			if (x != boundaryX || location->getEndType(roomLevel, wallSide) != SectorEndType::None)
				return false;
			int const adjacentX = wallSide == CORE_SIDE_LEFT ? (int)x - 1 : (int)x + 1;
			if (adjacentX < 0 || adjacentX >= (int)mCellsWide) return false;
			auto const& adjacent = mLayers[layerIndex]
				->getCellDefinition((uint32_t)adjacentX, y);
			if (!adjacent.occupied() || !adjacent.isTraversableOnFoot()) return false;
			auto adjacentLocation = dynamic_pointer_cast<const Location>(mSectors[adjacent.sectorIndex]);
			if (!adjacentLocation || y < adjacentLocation->getCellY()) return false;
			auto const adjacentLevel = y - adjacentLocation->getCellY();
			return adjacentLevel < adjacentLocation->getLevelsHigh()
				&& adjacentLocation->getEndType(adjacentLevel, 1 - wallSide) == SectorEndType::None;
		};
		if (openLandingAt(CORE_SIDE_LEFT) || openLandingAt(CORE_SIDE_RIGHT)) return true;
		diagnostic = format("The upper Staircase endpoint at {},{} must meet an open Room wall with adjacent traversable floor", x, y);
		return false;
	}

	bool World::canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, string* diagnostic) const
	{
		auto reject = [&](string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Staircase must sit on a Layer that has a Layer in front of it to land on");
		if (riseSide != CORE_SIDE_LEFT && riseSide != CORE_SIDE_RIGHT)
			return reject("The Staircase rise direction is invalid");
		if (cellsWide < 2) return reject("A Staircase must be at least two cells wide");
		if (x >= mCellsWide || y >= mLevelsHigh || cellsWide > mCellsWide - x || y + 1 >= mLevelsHigh)
			return reject("The Staircase is outside the World bounds");
		for (uint32_t iy = y; iy <= y + 1; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				if (mLayers[layerIndex]->getCellDefinition(ix, iy).occupied())
					return reject(format("A Sector at {},{} blocks the Staircase", ix, iy));

		auto const landingLayer = layerInFront(layerIndex);
		uint32_t const lowerX = riseSide == CORE_SIDE_RIGHT ? x : x + cellsWide - 1;
		uint32_t const upperX = riseSide == CORE_SIDE_RIGHT ? x + cellsWide - 1 : x;
		string endpointDiagnostic;
		if (!validateStaircaseEndpoint(landingLayer, lowerX, y, false, riseSide, endpointDiagnostic)
			|| !validateStaircaseEndpoint(landingLayer, upperX, y + 1, true, riseSide, endpointDiagnostic))
			return reject(std::move(endpointDiagnostic));
		return true;
	}

	uint32_t World::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, float speed)
	{
		invalidateSimulationSnapshot();
		return addStaircase(layerIndex, y, x, CreateStaircaseOptions{ cellsWide, riseSide, speed });
	}

	bool World::setEscalatorInitiallyBroken(uint32_t sectorIndex, bool broken)
	{
		if (!mSimulationPaused || sectorIndex >= mSectors.size()) return false;
		auto transit = dynamic_pointer_cast<StaircaseTransit>(mSectors[sectorIndex]);
		if (!transit || !transit->getStaircase()->isEscalator()) return false;
		auto found = find_if(mConstructionRecords.begin(), mConstructionRecords.end(), [&](auto const& record)
		{
			return record.type == ConstructionType::Staircase && record.layer == transit->getLayerIndex()
				&& record.a == transit->getCellY() && record.b == transit->getCellX();
		});
		if (found == mConstructionRecords.end()) return false;
		if (found->initiallyBroken == broken) return true;
		found->initiallyBroken = transit->getStaircase()->mInitiallyBroken = broken;
		setEscalatorBroken(sectorIndex, broken);
		modify();
		return true;
	}

	bool World::setEscalatorBroken(uint32_t sectorIndex, bool broken)
	{
		if (sectorIndex >= mSectors.size()) return false;
		auto transit = dynamic_pointer_cast<StaircaseTransit>(mSectors[sectorIndex]);
		if (!transit || !transit->getStaircase()->isEscalator()) return false;
		invalidateSimulationSnapshot();
		transit->getStaircase()->mBroken = broken;
		// Existing admitted movement keeps its position and target. Only fresh
		// local observations can change route intent or remote Agent knowledge.
		return true;
	}

	uint32_t World::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, CreateStaircaseOptions const& options)
	{
		if (options.initiallyBroken && options.speed == 0.0f)
			throw WorldException(this, "Stationary Staircases cannot be Broken");
		invalidateSimulationSnapshot();
		beginStructuralEdit("addStaircase");
		string diagnostic;
		if (!isfinite(options.speed))
			throw WorldException(this, "A Staircase speed must be finite");
		if (!canAddStaircase(layerIndex, y, x, options.cellsWide, options.riseSide, &diagnostic))
			throw WorldException(this, format("World::addStaircase({}, {}, {}, {}) - {}", layerIndex, y, x, options.cellsWide, diagnostic));
		auto sectorIndex = createStaircase(layerIndex, x, y, options.cellsWide, options.riseSide, options.speed);
		auto staircase = static_pointer_cast<StaircaseTransit>(mSectors[sectorIndex])->getStaircase();
		staircase->mInitiallyBroken = staircase->mBroken = options.initiallyBroken;
		auto transitLayer = getLayer(layerIndex);
		for (uint32_t iy = y; iy <= y + 1; ++iy)
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
				transitLayer->getCellDefinition(ix, iy).sectorIndex = sectorIndex;
		ConstructionRecord record{ ConstructionType::Staircase };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.cellsWide; record.i = options.riseSide;
		record.x = options.speed;
		record.initiallyBroken = options.initiallyBroken;
		recordConstruction(std::move(record));
		return sectorIndex;
	}

	std::vector<World::LiftLandingRow> World::getLiftLandingRows(uint32_t layerIndex,
		uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		vector<LiftLandingRow> rows;
		if (layerIndex >= mLayers.size() || cellsWide == 0 || levelsHigh == 0)
			return rows;
		// A Transit lands on the Layer directly in front of the Layer it occupies, so
		// the front-most Layer - which has nothing in front of it - has no landings.
		if (isFrontMostLayer(layerIndex)) return rows;
		if (x >= mCellsWide || y >= mLevelsHigh || cellsWide > mCellsWide - x
			|| levelsHigh > mLevelsHigh - y) return rows;

		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			LiftLandingRow row;
			row.offset = iy - y;
			auto const& firstCell = landing->getCellDefinition(x, iy);
			if (firstCell.sectorIndex != ~0u)
				row.location = dynamic_pointer_cast<const Location>(mSectors[firstCell.sectorIndex]);
			if (!row.location)
			{
				rows.push_back(row);
				continue;
			}
			bool complete = true;
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = landing->getCellDefinition(ix, iy);
				complete = complete && cell.sectorIndex == firstCell.sectorIndex
					&& cell.isTraversableOnFoot();
				row.obstructed = row.obstructed || cell.hasObject() || !cell.markers.empty();
			}
			row.fullyOverlapping = complete;
			if (row.fullyOverlapping)
				row.callButtonSpace = !(x == row.location->getCellX0()
					&& x + cellsWide - 1 == row.location->getCellX1());
			rows.push_back(row);
		}
		return rows;
	}

	World::CreateLiftResult World::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		invalidateSimulationSnapshot();
		CreateLiftOptions options;
		options.cellsWide = cellsWide;
		options.levelsHigh = levelsHigh;
		if (cellsWide == 0 || cellsWide > 2 || levelsHigh == 0)
			throw WorldException(this, "Editor lifts must be one or two cells wide and at least one level high");
		validateLayer(format("World::addLift({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw WorldException(this, "A Lift cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		for (auto const& row : getLiftLandingRows(layerIndex, y, x, cellsWide, levelsHigh))
		{
			if (!row.fullyOverlapping) continue;
			if (row.obstructed)
				throw WorldException(this, format("An object blocks the Lift landing at level {}", y + row.offset));
			options.stopOffsets.push_back(row.offset);
		}
		return addLift(layerIndex, y, x, options);
	}

	World::CreateLiftResult World::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLiftOptions const& options)
	{
		invalidateSimulationSnapshot();
		// The Lift Transit occupies layerIndex; its landings are the fore Layer of the
		// pair it forms, which is the Layer directly in front.
		validateLayer(format("World::addLift({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw WorldException(this, "A Lift cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		auto foreLayer = getLayer(layerInFront(layerIndex));
		auto backLayer = getLayer(layerIndex);

		// Checks
		string caller = format("World::addLift({}, {}, {}, {}, <stopOffsts>)", layerIndex, y, x, options.cellsWide);

		// Option and timing checks run before beginStructuralEdit() so a refused
		// add is a true no-op and no non-finite timing reaches the tick
		// conversion (#198).
		validateLiftOptions(caller, options);
		if (options.cellsWide > 2)
			throw WorldException(this, format("{} - enclosed Lift width must be one or two cells.", caller));

		auto levelsHigh = options.levelsHigh ? options.levelsHigh : options.stopOffsets.back() + 1;
		if (options.stopOffsets.back() >= levelsHigh)
			throw WorldException(this, format("{} - Lift stop is outside the shaft bounds.", caller));

		validateBounds(caller, x, y, options.cellsWide, levelsHigh);
		validateLayerSpace(caller, layerIndex, x, y, options.cellsWide, levelsHigh);

		for (auto stopOffset : options.stopOffsets)
		{
			auto iy = y + stopOffset;
		
			auto const& cellDef = foreLayer->getCellDefinition(x, iy);
			auto levelSectorIndex = cellDef.sectorIndex;

			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto const& cellDef = foreLayer->getCellDefinition(ix, iy);
				auto foreSectorIndex = cellDef.sectorIndex;

				// Make sure the foreground cells have a Sector, and that the horizontal Sectors are not different:
				// Lifts cannot span different Sectors horizontally, due to placement of the door leading to them.
				if (foreSectorIndex != levelSectorIndex)
				{
					throw WorldException(this, format("{} - the lift horizontally spans different foreground Sectors between {},{} and {},{}, which is not allowed", caller, x, iy, x + 1, iy));
				}

				// Fore Sector can't be empty
				if (foreSectorIndex == ~0u)
				{
					throw WorldException(this, format("{} - foreground cell at {},{} is not occupied, which blocks lift being placed", caller, ix, iy));
				}

				// Enclosed lifts connect fully overlapping Fore-layer Locations.
				auto const& foreSector = getSector(foreSectorIndex);
				auto location = dynamic_pointer_cast<const Location>(foreSector);

				if (!location)
				{
					throw WorldException(this, format("{} - foreground cell at {},{} is not a Location, which blocks lift being placed", caller, ix, iy));
				}

				// Lifts must not be in the air and every intersecting landing must be clear.
				validateCellTraversableOnFoot(caller, "Lift", layerInFront(layerIndex), ix, iy);
				if (cellDef.hasObject() || !cellDef.markers.empty())
					throw WorldException(this, format("{} - an object blocks the Lift landing at {},{}", caller, ix, iy));
			}
		}
		vector<physicalControl::Demand> landingDemands;
		for (auto offset : options.stopOffsets)
		{
			auto level = y + offset;
			auto location = getSector(foreLayer->getCellDefinition(x, level).sectorIndex);
			landingDemands.push_back(transportControlDemand(location, physicalControl::OwnerType::Lift,
				{layerIndex, x, y, options.cellsWide, levelsHigh}, x, level, options.cellsWide));
		}
		validatePhysicalControlAdditions(landingDemands);
		beginStructuralEdit("addLift");

		// Create lift
		auto liftObject = createLift(layerIndex, x, y, options.cellsWide, levelsHigh, options.stopOffsets);

		auto liftTransit = dynamic_pointer_cast<LiftTransit>(liftObject.sector);
		auto lift = liftTransit->getLift();
		lift->mInitiallyBroken = options.initiallyBroken;

		// Set layers
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto& cellDef = backLayer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = liftObject.sector->getIndex();
			}
		}

		CreateLiftResult liftRes;

		liftRes.lift = liftObject;

		// Landing controls are physical InteractionPoints; the lift coordinator owns
		// scheduling, door interlocks, and operation completion.
		for (size_t stopIndex = 0; stopIndex < options.stopOffsets.size(); ++stopIndex)
		{
			// A Lift entrance reads as a centre-opening pair, so its generated Doors
			// are authored OpenApart unless the Lift's record carries a per-stop
			// override.  Shuttle-owned Doors keep OpenUp.
			CreateDoorOptions stopDoorOptions;
			stopDoorOptions.width = options.cellsWide;
			stopDoorOptions.controls[0] = true;
			stopDoorOptions.activationMode = DoorActivationMode::Unavailable;
			stopDoorOptions.openStyle = Door::OpenStyle::OpenApart;
			if (stopIndex < options.stopDoorOpenStyles.size()
				&& options.stopDoorOpenStyles[stopIndex] != ~0u)
				stopDoorOptions.openStyle =
					static_cast<Door::OpenStyle>(options.stopDoorOpenStyles[stopIndex]);
			if (stopIndex < options.doorSpeeds.size() && options.doorSpeeds[stopIndex] > 0.0f)
				stopDoorOptions.speedOverride = options.doorSpeeds[stopIndex];
			auto doorRes = _addSectorDoor(layerInFront(layerIndex), y + options.stopOffsets[stopIndex],
				x, stopDoorOptions, true);
			liftRes.doors.push_back(doorRes);
		}

		// The replacement lift coordinator owns the car manifest and schedule. Each
		// landing remains a distinct threshold resource, linked to this coordinator.
		vector<LiftStop> liftStops;
		for (uint32_t i = 0; i < options.stopOffsets.size(); ++i)
		{
			auto location = getSector(foreLayer->getCellDefinition(x, y + options.stopOffsets[i]).sectorIndex);
			liftStops.push_back({ SectorId{ (uint64_t)location->getIndex() + 1 },
				(float)(y + options.stopOffsets[i]), liftRes.doors[i].traversalResource, {} });
		}
		auto coordinator = createLiftTraversalResource("Lift journey", lift,
			SectorId{ (uint64_t)liftTransit->getIndex() + 1 }, liftStops, options.capacity,
			options.minimumDwellSeconds, options.maximumBoardingSeconds);
		lift->configureTraversal(coordinator, options.capacity, options.minimumDwellSeconds);
		liftRes.traversalResource = coordinator;
		auto liftResource = mTraversalResources.find(coordinator);
		liftResource->mLiftCurrentStop = options.initialStop;
		liftResource->mLiftPosition = liftStops[options.initialStop].globalPosition;
		lift->setCoordinatedPosition(liftResource->mLiftPosition);
		// Pack every authored capacity slot inside the car. The World's desired
		// clearance is used where it fits; compact packing reduces only the gap when
		// the authored width is tighter, without reducing capacity or leaving the car.
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto const occupantTargets = packOccupants(options.capacity, 0,
			{ halfAgentWidth, lift->getSize().x - halfAgentWidth }, CORE_RESOURCE_SLOT_WIDTH,
			mTraversalGeometryPolicy.occupantClearance, OccupantPackingOrder::Forward,
			OccupantPackingLayout::Compact);
		for (uint32_t i = 0; i < options.capacity; ++i)
			liftResource->mCapacityPositions[i] = { occupantTargets[i], 0.0f };
		for (uint32_t i = 0; i < liftRes.doors.size(); ++i)
		{
			auto landing = mTraversalResources.find(liftRes.doors[i].traversalResource);
			landing->mLiftCoordinator = coordinator;
			landing->mLiftStopIndex = i;
			// Lift landing Doors use their physical opening for clearance, like
			// Bulkhead and Chamber thresholds, without a height-authoring override.
			landing->mDoor->mLiftOwned = true;
			// The car-side lane represents standing capacity, not corridor waiting
			// space. Give it exactly one spot per passenger position.
			for (auto& lane : landing->mQueueLanes)
			{
				if (lane.sector != liftResource->mLiftSector) continue;
				lane.positions.clear();
				for (auto const& position : liftResource->mCapacityPositions)
					lane.positions.push_back({ liftTransit->getPosition().x + position.x,
						liftStops[i].globalPosition + position.y });
				lane.positionOwners.assign(lane.positions.size(), {});
				if (!lane.positions.empty())
				{
					lane.origin = lane.positions.front();
					lane.direction = Vector2::UNIT_X;
					lane.extent = lane.positions.back().x - lane.positions.front().x;
				}
			}

			auto& control = liftRes.doors[i].controls[0];
			DeviceCommand call;
			call.type = DeviceCommandType::CallLift;
			call.traversalResource = coordinator;
			call.stopIndex = i;
			auto point = createPhysicalControlInteractionPoint("Lift landing call", control,
				(float)(y + options.stopOffsets[i]), 0.15f, getFixedTimestep(),
				{ { call, InteractionBindingRequirement::Required } });
			if (i < options.landingControlPermissionRequirements.size())
				for (auto permission : options.landingControlPermissionRequirements[i])
					mInteractionPoints.find(point)->mPermissionRequirement.set(permission.value - 1);
			mAuthoredControlRequirements[point] = { mDeserializingConstruction ? mConstructionReplayIndex : mConstructionRecords.size(), i };
			landing->mControls.push_back(point);
			liftResource->mLiftStops[i].callControl = point;

			DeviceCommand select;
			select.type = DeviceCommandType::SelectLiftDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Lift destination selector",
				liftResource->mLiftSector, { x + options.cellsWide * 0.5f, liftResource->mLiftPosition },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			liftResource->mControls.push_back(selector);
			if (i == 0) liftRes.interiorSelector = selector;
		}
		liftResource->mLiftSelector = liftRes.interiorSelector;
		setLiftBroken(coordinator, options.initiallyBroken);

		ConstructionRecord record{ ConstructionType::Lift };
		record.initiallyBroken = options.initiallyBroken;
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.cellsWide; record.d = options.capacity;
		record.e = levelsHigh; record.g = options.initialStop;
		record.x = options.minimumDwellSeconds; record.y = options.maximumBoardingSeconds;
		record.values = options.stopOffsets;
		// The per-stop Door styles are authored Lift data, not just initial Door
		// state: record them so save/load and later rebuilds replay them, the
		// way the Shuttle path records its doorOpenStyles.
		record.overrides = options.stopDoorOpenStyles;
		record.transportDoorSpeeds = options.doorSpeeds;
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			record.landingControlPermissionRequirements.emplace_back();
			for (auto permission : requirement)
				record.landingControlPermissionRequirements.back().push_back(
					static_cast<uint32_t>(permission.value));
		}
		recordConstruction(std::move(record));
		return liftRes;
	}

	World::CreateShuttleResult World::addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options)
	{
		invalidateSimulationSnapshot();
		// The Shuttle Transit occupies layerIndex; its landings are the fore Layer of
		// the pair it forms, which is the Layer directly in front.
		validateLayer(format("World::addShuttle({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw WorldException(this, "A Shuttle cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		auto foreLayer = getLayer(layerInFront(layerIndex));
		auto backLayer = getLayer(layerIndex);

		// Checks
		string caller = format("World::addShuttle({}, {}, {}, {}, <options>)", layerIndex, y, x, cellsWide);

		// Bear in mind that we may have to allow extra space on the transit layer beyond the x/x+cellsWide
		// extents.  Eg, for where a=x and b=x+cellsWide, and s=stop offsets:
		//       a        b
		//        s      s 
		// BACK: XDX-------
		// FORE: -D-....-D-

		// Option and timing checks run before beginStructuralEdit() so a refused
		// add is a true no-op and no non-finite timing reaches the tick
		// conversion (#198).
		validateShuttleOptions(caller, options);
		validateBounds(caller, x, y, cellsWide, 1);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, 1);

		auto shuttleWidth = options.carWidth * options.numCars + (options.numCars - 1);

		// Make sure the shuttle track is long enough
		if (shuttleWidth > cellsWide)
		{
			throw WorldException(this, format("{} - shuttle track is not wide enough for shuttle", caller));
		}

		auto numStops = (uint32_t)options.stopOffsets.size();
		for (uint32_t i = 0; i < numStops; ++i)
		{
			auto ix = x + options.stopOffsets[i];

			// Each stop must be at least shuttleWidth cells apart
			if (i < numStops - 1)
			{
				auto ix1 = x + options.stopOffsets[i + 1];

				if (ix1 - ix < shuttleWidth)
				{
					throw WorldException(this, format("{} - shuttle is too wide to fit between stop offsets {} and {}", caller, i, i + 1));
				}
			}

			// Must be able to fit whole shuttle into stop
			if (ix + shuttleWidth > (x + cellsWide))
			{
				throw WorldException(this, format("{} - shuttle is too wide to fit at stop offset {}", caller, i));
			}

			// A stop remains usable when at least one configured carriage door has
			// a Location landing. Partial mode omits unsupported individual doors.
			bool hasLanding = false;
			auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask);
			for (uint32_t car = 0; car < options.numCars; ++car)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
				{
					uint32_t cx = ix + car * (options.carWidth + 1) + doorOffsets[door];
					auto const& cell = foreLayer->getCellDefinition(cx, y);
					// A carriage door lands on any location-like Sector: a Room, a
					// Corridor, or a Facade (ADR 0003, ticket #52).
					bool supported = cell.sectorIndex != ~0u
						&& isLocationLike(getSector(cell.sectorIndex)->getType());
					if (!supported && !options.allowPartialLandings)
						throw WorldException(this, format("{} - door {} of carriage {} at stop offset {} has no supported landing",
							caller, door, car, options.stopOffsets[i]));
					if (supported)
					{
						validateCellTraversableOnFoot(caller, "Shuttle", layerInFront(layerIndex), cx, y);
						if (cell.hasObject() || !cell.markers.empty())
							throw WorldException(this, format("{} - an object blocks the Shuttle landing at {},{}", caller, cx, y));
					}
					hasLanding = hasLanding || supported;
				}
			if (!hasLanding)
				throw WorldException(this, format("{} - stop offset {} has no supported carriage landing", caller, options.stopOffsets[i]));
		}

		vector<physicalControl::Demand> landingDemands;
		auto authoredDoorOffsets = SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask);
		for (auto offset : options.stopOffsets)
			for (uint32_t car = 0; car < options.numCars; ++car)
				for (auto doorOffset : authoredDoorOffsets)
				{
					auto doorwayX = x + offset + car * (options.carWidth + 1) + doorOffset;
					auto index = foreLayer->getCellDefinition(doorwayX, y).sectorIndex;
					if (index == ~0u || !isLocationLike(getSector(index)->getType())) continue;
					landingDemands.push_back(transportControlDemand(getSector(index), physicalControl::OwnerType::Shuttle,
						{layerIndex, x, y, cellsWide, 1}, doorwayX, y, 1));
				}
		validatePhysicalControlAdditions(landingDemands);
		beginStructuralEdit("addShuttle");

		// Create shuttle
		auto shuttleObject = createShuttle(layerIndex, x, y, cellsWide, options.numCars, options.carWidth, options.stopOffsets);

		auto shuttleTransit = dynamic_pointer_cast<ShuttleTransit>(shuttleObject.sector);
		auto shuttle = shuttleTransit->getShuttle();

		// Set layers
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef = backLayer->getCellDefinition(ix, y);

			cellDef.sectorIndex = shuttleObject.sector->getIndex();
		}

		CreateShuttleResult shuttleRes;

		shuttleRes.shuttle = shuttleObject;

		// Create landing thresholds and physical InteractionPoints. Keep a fixed
		// stop/carriage/door result grid so absent partial landings remain explicit.
		auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask);
		auto doorResultIndex = [&](uint32_t stop, uint32_t car, uint32_t door)
		{
			return (stop * options.numCars + car) * doorOffsets.size() + door;
		};
		shuttleRes.doors.resize(options.stopOffsets.size() * options.numCars * doorOffsets.size());
		for (uint32_t stop = 0; stop < options.stopOffsets.size(); ++stop)
			for (uint32_t car = 0; car < options.numCars; ++car)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
				{
					auto globalX = x + options.stopOffsets[stop]
						+ car * (options.carWidth + 1) + doorOffsets[door];
					auto const& cell = foreLayer->getCellDefinition(globalX, y);
					// Same rule as the validation pass above: only a location-like
					// landing is supported, a Facade included. A Background behind a
					// carriage door is omitted, never built into a threshold that
					// touches it.
					bool supported = cell.sectorIndex != ~0u
						&& isLocationLike(getSector(cell.sectorIndex)->getType());
					if (supported)
					{
						// A Shuttle landing reads as a hatch, so its generated Doors
						// are authored OpenUp unless the Shuttle's record carries a
						// per-Door override on this grid slot.
						CreateDoorOptions stopDoorOptions;
						stopDoorOptions.width = 1;
						stopDoorOptions.controls[0] = true;
						stopDoorOptions.activationMode = DoorActivationMode::Unavailable;
						auto const slot = doorResultIndex(stop, car, door);
						if (slot < options.doorOpenStyles.size()
							&& options.doorOpenStyles[slot] != ~0u)
							stopDoorOptions.openStyle =
								static_cast<Door::OpenStyle>(options.doorOpenStyles[slot]);
						if (slot < options.doorSpeeds.size() && options.doorSpeeds[slot] > 0.0f)
							stopDoorOptions.speedOverride = options.doorSpeeds[slot];
						shuttleRes.doors[slot] = _addSectorDoor(layerInFront(layerIndex),
							y, globalX, stopDoorOptions, true);
					}
				}

		vector<LiftStop> stops;
		for (uint32_t i = 0; i < options.stopOffsets.size(); ++i)
		{
			uint32_t doorIndex = i * options.numCars * (uint32_t)doorOffsets.size();
			auto stopEnd = (i + 1) * options.numCars * (uint32_t)doorOffsets.size();
			while (doorIndex < stopEnd && !shuttleRes.doors[doorIndex].traversalResource) ++doorIndex;
			assert(doorIndex < stopEnd);
			auto location = shuttleRes.doors[doorIndex].door.sector;
			stops.push_back({ SectorId{ (uint64_t)location->getIndex() + 1 },
				(float)(x + options.stopOffsets[i]), shuttleRes.doors[doorIndex].traversalResource, {} });
		}
		auto coordinator = createShuttleTraversalResource("Shuttle journey", shuttle,
			SectorId{ (uint64_t)shuttleTransit->getIndex() + 1 }, stops, options.capacity,
			options.minimumDwellSeconds, options.maximumBoardingSeconds);
		shuttle->configureTraversal(coordinator);
		shuttleRes.traversalResource = coordinator;
		auto shuttleResource = mTraversalResources.find(coordinator);
		shuttleResource->mLiftCurrentStop = options.initialStop;
		shuttleResource->mLiftPosition = stops[options.initialStop].globalPosition;
		shuttle->setCoordinatedPosition(shuttleResource->mLiftPosition);

		// A Location sector is one connected platform access zone. Doors opening
		// onto the same sector share a logical queue; different sectors do not.
		for (uint32_t stop = 0; stop < stops.size(); ++stop)
		{
			map<SectorId, uint32_t> accessZones;
			for (uint32_t carriage = 0; carriage < options.numCars; ++carriage)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
			{
				auto doorIndex = doorResultIndex(stop, carriage, door);
				auto& doorResult = shuttleRes.doors[doorIndex];
				if (!doorResult.traversalResource) continue;
				auto landing = mTraversalResources.find(doorResult.traversalResource);
				auto doorX = x + options.stopOffsets[stop]
					+ carriage * (options.carWidth + 1) + doorOffsets[door];
				auto location = getSector(foreLayer->getCellDefinition(doorX, y).sectorIndex);
				auto locationId = SectorId{ (uint64_t)location->getIndex() + 1 };
				auto [zone, inserted] = accessZones.emplace(locationId, (uint32_t)accessZones.size());
				(void)inserted;
				landing->mLiftCoordinator = coordinator;
				landing->mLiftStopIndex = stop;
				// Shuttle landing Doors use their physical opening for clearance, like
				// Lift landing Doors, without a height-authoring override.
				landing->mDoor->mShuttleOwned = true;
				shuttleResource->mShuttleDoors.push_back({ stop, carriage, zone->second,
					locationId, doorResult.traversalResource,
					carriage * (options.carWidth + 1.0f) + doorOffsets[door] + 0.5f });
				auto& carriageState = shuttleResource->mShuttleCarriages[carriage];
				carriageState.stopDoors[stop].push_back(doorResult.traversalResource);
				// Car-side Door spots mirror this carriage's declared capacity. They
				// remain independent from the platform-side queue, even where the two
				// layers' spots overlap visually.
				for (auto& lane : landing->mQueueLanes)
				{
					if (lane.sector != shuttleResource->mLiftSector) continue;
					lane.positions.clear();
					for (uint32_t position = 0; position < carriageState.capacity; ++position)
					{
						auto capacityIndex = carriageState.firstCapacityPosition + position;
						auto const& local = shuttleResource->mCapacityPositions[capacityIndex];
						lane.positions.push_back({ stops[stop].globalPosition + local.x,
							(float)y + local.y });
					}
					lane.positionOwners.assign(lane.positions.size(), {});
					if (!lane.positions.empty())
					{
						lane.origin = lane.positions.front();
						lane.direction = Vector2::UNIT_X;
						lane.extent = lane.positions.back().x - lane.positions.front().x;
					}
				}

				auto& control = doorResult.controls[0];
				DeviceCommand call;
				call.type = DeviceCommandType::CallShuttle;
				call.traversalResource = coordinator;
				call.stopIndex = stop;
				auto point = createPhysicalControlInteractionPoint("Shuttle landing call", control,
					(float)y, 0.15f, getFixedTimestep(),
					{ { call, InteractionBindingRequirement::Required } });
				if (doorIndex < options.landingControlPermissionRequirements.size())
					for (auto permission : options.landingControlPermissionRequirements[doorIndex])
						mInteractionPoints.find(point)->mPermissionRequirement.set(permission.value - 1);
				mAuthoredControlRequirements[point] = { mDeserializingConstruction ? mConstructionReplayIndex : mConstructionRecords.size(), doorIndex };
				landing->mControls.push_back(point);
				if (!shuttleResource->mLiftStops[stop].callControl)
					shuttleResource->mLiftStops[stop].callControl = point;
			}
		}

		// Destination selection is serialized vehicle-wide. The interaction point
		// is moved to the acting passenger, so passengers retain carriage positions.
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			DeviceCommand select;
			select.type = DeviceCommandType::SelectShuttleDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Shuttle destination selector",
				shuttleResource->mLiftSector,
				{ shuttleResource->mLiftPosition + options.carWidth * 0.5f, (float)y },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			shuttleResource->mControls.push_back(selector);
			if (i == 0) shuttleRes.interiorSelector = selector;
		}
		shuttleResource->mLiftSelector = shuttleRes.interiorSelector;

		shuttle->mInitiallyBroken = options.initiallyBroken;
		setShuttleBroken(coordinator, options.initiallyBroken);
		ConstructionRecord record{ ConstructionType::Shuttle };
		record.initiallyBroken = options.initiallyBroken;
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = options.numCars;
		record.e = options.carWidth; record.f = options.initialStop; record.g = options.capacity;
		record.h = options.doorMask; record.p = options.allowPartialLandings;
		record.x = options.minimumDwellSeconds; record.y = options.maximumBoardingSeconds;
		record.values = options.stopOffsets;
		record.overrides = options.doorOpenStyles;
		record.transportDoorSpeeds = options.doorSpeeds;
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			record.landingControlPermissionRequirements.emplace_back();
			for (auto permission : requirement)
				record.landingControlPermissionRequirements.back().push_back(
					static_cast<uint32_t>(permission.value));
		}
		recordConstruction(std::move(record));
		return shuttleRes;
	}

	bool World::canRemoveLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
			return reject("A Location wall side must be left or right");
		if (sectorIndex >= mSectors.size()) return reject("The Location does not exist");
		auto sector = mSectors[sectorIndex];
		if (sector->getType() == SectorType::Facade)
			return reject("A Facade has no walls: its perimeter is open by construction");
		if (sector->getType() != SectorType::Location)
			return reject("Walls can only be edited on Rooms and Corridors");
		if (levelIndex >= sector->getLevelsHigh()) return reject("The Location level does not exist");

		auto const globalY = sector->getCellY() + levelIndex;
		int const neighbourX = side == CORE_SIDE_LEFT
			? (int)sector->getCellX() - 1
			: (int)sector->getCellX() + (int)sector->getCellsWide();
		if (neighbourX < 0 || neighbourX >= (int)getCellsWide())
			return reject("The wall is on the outside of the World");
		auto const& neighbourCell = mLayers[sector->getLayerIndex()]
			->getCellDefinition((uint32_t)neighbourX, globalY);
		if (neighbourCell.sectorIndex == ~0u || neighbourCell.sectorIndex == sectorIndex)
			return reject("No adjacent Room or Corridor shares this wall");
		auto neighbour = mSectors[neighbourCell.sectorIndex];
		// A Facade may share a boundary with a Room whose wall is being opened,
		// and vice versa: the Facade's half is open by construction, and
		// refusing the Room's half would let a Facade merge outward while its
		// neighbour could not merge inward.  The shared location-like rule
		// (ticket #45) admits both here; a Facade itself never has a wall to
		// remove, so the sector-side type check above still refuses it.
		if (!isLocationLike(neighbour->getType()))
			return reject("The adjacent sector is not a Room, Corridor or Facade");
		if (globalY < neighbour->getCellY()
			|| globalY >= neighbour->getCellY() + neighbour->getLevelsHigh())
			return reject("The adjacent Location does not occupy this level");
		auto const neighbourLevel = globalY - neighbour->getCellY();
		if (neighbour->getType() == SectorType::Facade)
		{
			// The Facade's half of the boundary is open by construction; only
			// the Room's own wall stands in the way.
			if (neighbour->getEndType(neighbourLevel, 1 - side) != SectorEndType::None)
				return reject("The Facade side of the boundary is not open");
		}
		else if (neighbour->getEndType(neighbourLevel, 1 - side) != SectorEndType::Wall)
			return reject("The shared boundary is not a pair of walls");
		if (sector->getEndType(levelIndex, side) != SectorEndType::Wall)
			return reject("The shared boundary is not a pair of walls");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::canAddLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
			return reject("A Location wall side must be left or right");
		if (sectorIndex >= mSectors.size()) return reject("The Location does not exist");
		auto sector = mSectors[sectorIndex];
		if (sector->getType() == SectorType::Facade)
			return reject("A Facade has no walls: its perimeter is open by construction");
		if (sector->getType() != SectorType::Location)
			return reject("Walls can only be edited on Rooms and Corridors");
		if (levelIndex >= sector->getLevelsHigh()) return reject("The Location level does not exist");

		auto const globalY = sector->getCellY() + levelIndex;
		int const neighbourX = side == CORE_SIDE_LEFT
			? (int)sector->getCellX() - 1
			: (int)sector->getCellX() + (int)sector->getCellsWide();
		if (neighbourX < 0 || neighbourX >= (int)getCellsWide())
			return reject("The wall is on the outside of the World");
		auto const& neighbourCell = mLayers[sector->getLayerIndex()]
			->getCellDefinition((uint32_t)neighbourX, globalY);
		if (neighbourCell.sectorIndex == ~0u || neighbourCell.sectorIndex == sectorIndex)
			return reject("No adjacent Room or Corridor shares this opening");
		auto neighbour = mSectors[neighbourCell.sectorIndex];
		if (neighbour->getType() != SectorType::Location)
			return reject("The adjacent sector is not a Room or Corridor");
		if (globalY < neighbour->getCellY()
			|| globalY >= neighbour->getCellY() + neighbour->getLevelsHigh())
			return reject("The adjacent Location does not occupy this level");
		auto const neighbourLevel = globalY - neighbour->getCellY();
		if (sector->getEndType(levelIndex, side) != SectorEndType::None
			|| neighbour->getEndType(neighbourLevel, 1 - side) != SectorEndType::None)
			return reject("The shared boundary is not an open wall");
		try
		{
			validatePhysicalControlBoundary(sector->getLayerIndex(), globalY,
				side == CORE_SIDE_LEFT ? sector->getCellX() : sector->getCellX() + sector->getCellsWide());
		}
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	void World::removeLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canRemoveLocationWall(sectorIndex, levelIndex, side, &diagnostic))
			throw WorldException(this, "World::removeLocationWall - " + diagnostic);
		auto wallSector = _getSector(sectorIndex);
		auto const boundaryX = side == CORE_SIDE_LEFT ? wallSector->getCellX()
			: wallSector->getCellX() + wallSector->getCellsWide();
		(void)planPhysicalControls(wallSector->getLayerIndex(), ~0u,
			wallSector->getCellY() + levelIndex, nullptr, ~0u, boundaryX);
		beginStructuralEdit("removeLocationWall");

		auto sector = _getSector(sectorIndex);
		auto const globalY = sector->getCellY() + levelIndex;
		auto const neighbourX = side == CORE_SIDE_LEFT
			? sector->getCellX() - 1
			: sector->getCellX() + sector->getCellsWide();
		auto const neighbourIndex = mLayers[sector->getLayerIndex()]
			->getCellDefinition(neighbourX, globalY).sectorIndex;
		auto neighbour = _getSector(neighbourIndex);
		auto const neighbourLevel = globalY - neighbour->getCellY();
		sector->removeEndWall(levelIndex, side);
		neighbour->removeEndWall(neighbourLevel, 1 - side);
		reflowAllPhysicalControls();

		ConstructionRecord record{ ConstructionType::RemoveWall };
		record.a = sectorIndex; record.b = levelIndex; record.i = side;
		recordConstruction(std::move(record));
	}

	void World::addLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddLocationWall(sectorIndex, levelIndex, side, &diagnostic))
			throw WorldException(this, "World::addLocationWall - " + diagnostic);
		beginStructuralEdit("addLocationWall");

		auto sector = _getSector(sectorIndex);
		auto const globalY = sector->getCellY() + levelIndex;
		auto const boundaryX = side == CORE_SIDE_LEFT
			? sector->getCellX() : sector->getCellX() + sector->getCellsWide();
		auto const neighbourX = side == CORE_SIDE_LEFT
			? sector->getCellX() - 1
			: sector->getCellX() + sector->getCellsWide();
		auto const neighbourIndex = mLayers[sector->getLayerIndex()]
			->getCellDefinition(neighbourX, globalY).sectorIndex;
		auto neighbour = _getSector(neighbourIndex);
		auto const neighbourLevel = globalY - neighbour->getCellY();
		sector->addEndWall(levelIndex, side);
		neighbour->addEndWall(neighbourLevel, 1 - side);
		reflowAllPhysicalControls();

		// An open wall is persisted as a RemoveWall command. Restoring the wall
		// removes either side's command for this same physical boundary.
		mConstructionRecords.erase(remove_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				if (record.type != ConstructionType::RemoveWall
					|| record.a >= mSectors.size()) return false;
				auto commandSector = mSectors[record.a];
				if (record.b >= commandSector->getLevelsHigh()
					|| commandSector->getLayerIndex() != sector->getLayerIndex()) return false;
				auto const commandX = record.i == CORE_SIDE_LEFT
					? commandSector->getCellX()
					: commandSector->getCellX() + commandSector->getCellsWide();
				auto const commandY = commandSector->getCellY() + record.b;
				return commandX == boundaryX && commandY == globalY;
			}), mConstructionRecords.end());
	}

	World::CreateObjectResult World::_createSectorButton(string const& name, shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index)
	{
		invalidateSimulationSnapshot();
		uint32_t buttonX = sector->getCellX() + x;
		physicalControl::Demand demand;
		demand.owner = { physicalControl::OwnerType::LocationLightSwitch,
			{ sector->getLayerIndex(), buttonX, y, 1, 1 },
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { 0, buttonX, y } };
		auto const& obj = createPhysicalControl(name, sector->getLayerIndex(), y, demand, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	World::CreateObjectResult World::_createDoorButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index, uint32_t approachSide)
	{
		invalidateSimulationSnapshot();
		auto demand = doorControlDemand(sector, x, y, cellsWide, approachSide);
		auto obj = createPhysicalControl("Door button", sector->getLayerIndex(), y, demand, flags);
		if (index) *index = obj.index;
		return obj;
	}

	World::CreateObjectResult World::_createBulkheadDoorButton(shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index)
	{
		invalidateSimulationSnapshot();
		uint32_t thresholdX = side == CORE_SIDE_LEFT ? sector->getCellX1() + 1 : sector->getCellX0();
		auto demand = insetControlDemand(sector, physicalControl::OwnerType::BulkheadDoor,
			{ sector->getLayerIndex(), thresholdX, y, 2, 1 }, y, side);
		auto obj = createPhysicalControl("BulkheadDoor button", sector->getLayerIndex(), y, demand, 0);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	World::CreateObjectResult World::_createForceBridgeButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index)
	{
		invalidateSimulationSnapshot();
		auto demand = insetControlDemand(sector, physicalControl::OwnerType::ForceBridge,
			{ sector->getLayerIndex(), x, y, cellsWide, 1 }, y, side);
		auto obj = createPhysicalControl("ForceBridge button", sector->getLayerIndex(), y, demand, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	bool World::getLiftLandingGeometry(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t& landingX, uint32_t& landingWidth) const
	{
		if (layerIndex >= getLayerCount()) return false;
		if (x >= mCellsWide || y >= mLevelsHigh) return false;
		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		if (cell.sectorIndex == ~0u) return false;
		auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[cell.sectorIndex]);
		if (!lift) return false;
		landingX = lift->getCellX();
		landingWidth = lift->getCellsWide();
		return y >= lift->getCellY() && y <= lift->getCellY1();
	}

	bool World::isLiftOwnedDoor(shared_ptr<const SectorObject> const& object,
		uint32_t* liftSectorIndex, uint32_t* stopIndex) const
	{
		auto doorObject = dynamic_pointer_cast<const DoorSectorObject>(object);
		if (!doorObject) return false;
		auto resource = mTraversalResources.find(doorObject->getDoor()->getTraversalResourceId());
		if (!resource || !resource->mLiftCoordinator) return false;
		auto coordinator = mTraversalResources.find(resource->mLiftCoordinator);
		if (!coordinator || !coordinator->mLift || !coordinator->mLiftSector) return false;
		if (liftSectorIndex) *liftSectorIndex = (uint32_t)coordinator->mLiftSector.value - 1;
		if (stopIndex) *stopIndex = resource->mLiftStopIndex;
		return true;
	}

	bool World::isLiftOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* liftSectorIndex, uint32_t* stopIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<const Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		auto point = mInteractionPoints.find(button->getInteractionPointId());
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			if (binding.command.type != DeviceCommandType::CallLift) continue;
			auto resource = mTraversalResources.find(binding.command.traversalResource);
			if (!resource || !resource->mLift || !resource->mLiftSector) continue;
			if (liftSectorIndex) *liftSectorIndex = (uint32_t)resource->mLiftSector.value - 1;
			if (stopIndex) *stopIndex = binding.command.stopIndex;
			return true;
		}
		return false;
	}

	bool World::isShuttleOwnedDoor(shared_ptr<const SectorObject> const& object,
		uint32_t* shuttleSectorIndex, uint32_t* stopIndex, uint32_t* carriageIndex,
		uint32_t* doorIndex) const
	{
		auto doorObject = dynamic_pointer_cast<const DoorSectorObject>(object);
		if (!doorObject) return false;
		auto landingId = doorObject->getDoor()->getTraversalResourceId();
		auto landing = mTraversalResources.find(landingId);
		if (!landing || !landing->mLiftCoordinator) return false;
		auto coordinator = mTraversalResources.find(landing->mLiftCoordinator);
		if (!coordinator || !coordinator->mShuttle || !coordinator->mLiftSector) return false;
		auto mapping = find_if(coordinator->mShuttleDoors.begin(), coordinator->mShuttleDoors.end(),
			[landingId](auto const& value) { return value.landingResource == landingId; });
		if (mapping == coordinator->mShuttleDoors.end()) return false;
		// Every output is independently optional (#104): the Shuttle sector index is
		// kept in a local so resolving doorIndex never dereferences a null output.
		auto const sectorIndex = (uint32_t)coordinator->mLiftSector.value - 1;
		if (shuttleSectorIndex) *shuttleSectorIndex = sectorIndex;
		if (stopIndex) *stopIndex = mapping->stopIndex;
		if (carriageIndex) *carriageIndex = mapping->carriageIndex;
		if (doorIndex)
		{
			// The doorIndex is the position of this Door's cell within the
			// carriage's selected doorMask cells, matching the per-Door override
			// grid used by setShuttleDoorOpenStyle.
			*doorIndex = ~0u;
			uint32_t producerIndex = 0;
			for (auto const& record : mConstructionRecords)
			{
				if (!constructionTypeCreatesSector(record.type)) continue;
				if (producerIndex++ != sectorIndex) continue;
				if (record.type != ConstructionType::Shuttle) break;
				if (mapping->stopIndex >= record.values.size()
					|| mapping->carriageIndex >= record.d) break;
				auto const doorMask = record.h ? record.h : (1u << 1);
				auto const doorOffsets = SimulationCoordinator::shuttleDoorOffsets(record.e, doorMask);
				auto const base = record.b + record.values[mapping->stopIndex]
					+ mapping->carriageIndex * (record.e + 1);
				for (size_t d = 0; d < doorOffsets.size(); ++d)
					if (base + doorOffsets[d] == doorObject->getCellX())
					{
						*doorIndex = static_cast<uint32_t>(d);
						break;
					}
				break;
			}
		}
		return true;
	}

	bool World::isShuttleOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* shuttleSectorIndex, uint32_t* stopIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<const Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		auto point = mInteractionPoints.find(button->getInteractionPointId());
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			if (binding.command.type != DeviceCommandType::CallShuttle) continue;
			auto resource = mTraversalResources.find(binding.command.traversalResource);
			if (!resource || !resource->mShuttle || !resource->mLiftSector) continue;
			if (shuttleSectorIndex) *shuttleSectorIndex = (uint32_t)resource->mLiftSector.value - 1;
			if (stopIndex) *stopIndex = binding.command.stopIndex;
			return true;
		}
		return false;
	}

	bool World::isControlTargetBroken(InteractionPointId pointId) const
	{
		auto point = mInteractionPoints.find(pointId);
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			auto resource = mTraversalResources.find(binding.command.traversalResource);
			if (!resource) continue;
			switch (binding.command.type)
			{
			case DeviceCommandType::OpenDoor:
				if (resource->mDoor && resource->mDoor->isBroken()) return true;
				break;
			case DeviceCommandType::SetExtendedState:
				if (resource->mExtensible && resource->mExtensible->isBroken()) return true;
				break;
			case DeviceCommandType::CallLift:
			case DeviceCommandType::SelectLiftDestination:
				if (resource->mLift && resource->mLift->isBroken()) return true;
				break;
			case DeviceCommandType::CallShuttle:
			case DeviceCommandType::SelectShuttleDestination:
				if (resource->mShuttle && resource->mShuttle->isBroken()) return true;
				break;
			default:
				break;
			}
		}
		return false;
	}

	vector<uint32_t> World::getValidShuttleStopOffsets(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t numCars, uint32_t carWidth,
		bool allowPartialLandings, uint32_t doorMask) const
	{
		vector<uint32_t> result;
		if (layerIndex == 0 || layerIndex >= getLayerCount()) return result;
		if (y >= mLevelsHigh || x >= mCellsWide || cellsWide > mCellsWide - x
			|| numCars == 0 || carWidth < 3 || carWidth > 5
			|| doorMask == 0 || (doorMask >> carWidth) != 0
			|| numCars > (cellsWide + 1) / (carWidth + 1)) return result;
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto shuttleWidth = numCars * carWidth + numCars - 1;
		auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(carWidth, doorMask);
		if (shuttleWidth > cellsWide) return result;
		for (uint32_t offset = 0; offset + shuttleWidth <= cellsWide; ++offset)
		{
			bool any = false, all = true;
			for (uint32_t car = 0; car < numCars; ++car)
				for (auto doorOffset : doorOffsets)
				{
					auto doorX = x + offset + car * (carWidth + 1) + doorOffset;
					auto const& first = landing->getCellDefinition(doorX, y);
					// A Shuttle stop lands on any location-like Sector, a Facade
					// included (ADR 0003, ticket #52).
					bool supported = first.sectorIndex != ~0u
						&& isLocationLike(mSectors[first.sectorIndex]->getType());
					supported = supported && first.isTraversableOnFoot()
						&& !first.hasObject() && first.markers.empty();
					if (supported)
					{
						auto sector = mSectors[first.sectorIndex];
						supported = doorX != sector->getCellX0() || doorX != sector->getCellX1();
					}
					any = any || supported;
					all = all && supported;
				}
			if (any && (allowPartialLandings || all)) result.push_back(offset);
		}
		return result;
	}

	bool World::getShuttleOptions(Shuttle const* shuttle, CreateShuttleOptions& options) const
	{
		if (!shuttle) return false;
		uint32_t sectorIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(record.type)) continue;
			if (record.type == ConstructionType::Shuttle && sectorIndex < mSectors.size())
			{
				auto transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
				if (transit && transit->getShuttle().get() == shuttle)
				{
					options = { record.d, record.e, record.values, record.f, record.g,
						record.x, record.y, record.p, record.h ? record.h : (1u << 1),
						record.overrides, {}, record.initiallyBroken, record.transportDoorSpeeds };
					return true;
				}
			}
			++sectorIndex;
		}
		return false;
	}

	vector<World::ShuttleStopCandidate> World::getShuttleStopCandidatesForDoor(
		uint32_t shuttleLayer, uint32_t y, uint32_t doorX) const
	{
		vector<ShuttleStopCandidate> result;
		uint32_t sectorIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(record.type)) continue;
			// Only a Shuttle on the requested Layer can serve a Door on that pair.
			if (record.type == ConstructionType::Shuttle && record.layer == shuttleLayer && record.a == y)
			{
				auto doorMask = record.h ? record.h : (1u << 1);
				auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(record.e, doorMask);
				for (auto offset : getValidShuttleStopOffsets(record.layer, record.a, record.b, record.c,
					record.d, record.e, record.p, doorMask))
				{
					if (find(record.values.begin(), record.values.end(), offset) != record.values.end()) continue;
					auto vehicleWidth = record.d * record.e + record.d - 1;
					if (any_of(record.values.begin(), record.values.end(), [&](auto existing)
						{ return max(existing, offset) - min(existing, offset) < vehicleWidth; })) continue;
					bool matchesDoor = false;
					for (uint32_t car = 0; car < record.d && !matchesDoor; ++car)
						for (auto doorOffset : doorOffsets)
							matchesDoor = matchesDoor || doorX == record.b + offset
								+ car * (record.e + 1) + doorOffset;
					if (matchesDoor) result.push_back({ sectorIndex, offset });
				}
			}
			++sectorIndex;
		}
		return result;
	}

	bool World::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, string* diagnostic) const
	{
		uint32_t liftX, liftWidth;
		if (getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			CreateDoorOptions options;
			options.width = liftWidth;
			return canAddCorridorDoor(layerIndex, y, liftX, options, diagnostic);
		}
		return canAddCorridorDoor(layerIndex, y, x, CreateDoorOptions{}, diagnostic);
	}

	bool World::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		// A Door is authored on the front Layer of the pair it crosses.
		if (layerIndex + 1 >= getLayerCount())
			return reject("A Door needs a Layer directly behind the Layer it is authored on");
		auto const backLayer = layerBehind(layerIndex);
		constexpr uint32_t levelsHigh = 1;
		// Door authoring reserves the final column as the world boundary.
		if (options.width == 0 || x >= mCellsWide || options.width > mCellsWide - x
			|| x + options.width >= mCellsWide || y + levelsHigh > mLevelsHigh)
			return reject("Door position is outside the world");
		try
		{
			string const caller = "World::canAddCorridorDoor";
			validateSectorDoorOptions(caller, options);
			uint32_t liftX, liftWidth;
			bool const liftLanding = getLiftLandingGeometry(backLayer, y, x, liftX, liftWidth);
			if (liftLanding && options.heightScale) return reject("Transport-owned Doors refuse Height scale");
			if (liftLanding && (x != liftX || options.width != liftWidth))
				return reject(format("Lift landing doors must start at {} and be {} cells wide", liftX, liftWidth));
			if (options.activationMode != DoorActivationMode::RemoteControlled
				&& (options.controls[0] || options.controls[1]))
				return reject("Physical controls require a remote-controlled Door");
			validateBounds(caller, x, y, options.width, levelsHigh);
			// The whole Door rectangle stays inside one Sector on each Layer of its pair:
			// a tall opening that straddles a Sector boundary on the Layer behind would
			// join two Sectors with one threshold.
			validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.width, levelsHigh);
			validateSpaceOnlyInOneSector(caller, backLayer, x, y, options.width, levelsHigh);
			shared_ptr<const Sector> sectors[2];
			for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
				for (uint32_t ix = x; ix < x + options.width; ++ix)
			{
				auto const& foreCell = mLayers[layerIndex]->getCellDefinition(ix, iy);
				auto const& backCell = mLayers[backLayer]->getCellDefinition(ix, iy);
				if (!foreCell.occupied()) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (!backCell.occupied()) return reject("A Room, Corridor, Facade or Lift on the Layer behind is required here");
				sectors[0] = mSectors[foreCell.sectorIndex];
				sectors[1] = mSectors[backCell.sectorIndex];
				auto fore = dynamic_pointer_cast<const Location>(sectors[0]);
				auto back = dynamic_pointer_cast<const Location>(sectors[1]);
				if (!fore) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (options.height == Door::Height::Tall
					&& (fore->getType() != SectorType::Location || fore->isCorridor()))
					return reject("A tall Door is only available in a Room");
				if (liftLanding)
				{
					if (sectors[1]->getType() != SectorType::Lift)
						return reject("The complete lift width must overlap one corridor");
				}
				else if (!back) return reject("A Room, Corridor or Facade on the Layer behind is required here");
				// Only the authored Layer owns the Door's cell occupancy. Objects and
				// Markers on the destination Layer remain independent.
				if (foreCell.hasObject() || !foreCell.markers.empty())
					return reject("Another object blocks Door placement");
				// Only the threshold level carries the walking floor.  The levels above it
				// are the opening's headroom, so they need no floor of their own.
				if (iy == y && (!foreCell.isTraversableOnFoot() || !backCell.isTraversableOnFoot()))
					return reject("Door placement requires a traversable floor on both layers");
				// A Walkway or other floor above the threshold level runs through the
				// opening, so the Door cannot cover it.
				if (iy != y && (foreCell.floorType != CellFloorType::None
					|| backCell.floorType != CellFloorType::None))
					return reject("A Door cannot open through a Walkway above its threshold");
				validateObjectAllowedInSector(caller, SectorObjectType::Door, foreCell.sectorIndex);
				validateObjectAllowedInSector(caller, SectorObjectType::Door, backCell.sectorIndex);
			}
			if (!liftLanding)
				for (auto sector : sectors)
					validatePanelWallRectangle(sector->getIndex(), {float(x) + CORE_DOOR_X_INSET, float(y)},
						{float(x + options.width) - CORE_DOOR_X_INSET, float(y) + Door::effectiveHeight(options.height, options.heightScale)});
			if (liftLanding)
			{
				auto const lift = dynamic_pointer_cast<const LiftTransit>(sectors[1]);
				for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
				{
					auto const& existing = lift->getStop(stop);
					if ((uint32_t)((int)existing.sector->getCellY() + existing.sectorOffsetY) == y)
						return reject("This lift already has a stop on this level");
				}
				auto demand = doorControlDemand(sectors[0], x, y, options.width, 0);
				(void)planPhysicalControls(layerIndex, sectors[0]->getIndex(), y, &demand);
			}
			else for (uint32_t side = 0; side < 2; ++side)
			{
				if (!options.controls[side]) continue;
				auto demand = doorControlDemand(sectors[side], x, y, options.width, side);
				(void)planPhysicalControls(sectors[side]->getLayerIndex(), sectors[side]->getIndex(), y, &demand);
			}
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::getSectorDoorOptions(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
		CreateDoorOptions& options) const
	{
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == layerIndex
					&& record.a == y && record.b == x && record.c == width;
			});
		if (found == mConstructionRecords.rend()) return false;
		options.width = found->c;
		options.height = static_cast<Door::Height>(found->e);
		options.controls[0] = found->p;
		options.controls[1] = found->q;
		options.activationMode = static_cast<DoorActivationMode>(found->i);
		options.holdOpenSeconds = found->x;
		options.crossingLanes = found->d;
		options.openStyle = static_cast<Door::OpenStyle>(found->j);
		options.initiallyBroken = found->initiallyBroken;
		options.brokenOpenPercentage = found->brokenOpenPercentage;
		options.speedOverride = found->doorSpeed;
		options.heightScale = found->doorHeightScale;
		return true;
	}

	bool World::hasActiveDoorCrossing(Door const* door) const
	{
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->mState != Agent::State::TraversingEdge
				&& agent->mState != Agent::State::AwaitingTraversalCommit) continue;
			if (!agent->mTraversalTask) continue;
			auto edge = dynamic_pointer_cast<DoorEdge const>(agent->mTraversalTask->edge);
			if (edge && edge->getDoor().get() == door) return true;
		}
		return false;
	}

	bool World::setSectorDoorHeightScale(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t width, std::optional<float> scale, std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!mSimulationPaused) return reject("Pause simulation to change Height scale");
		CreateDoorOptions options;
		if (!getSectorDoorOptions(layerIndex, y, x, width, options)) return reject("No authored ordinary Door");
		if (options.height != Door::Height::Regular || !Door::heightScaleIsValid(scale))
			return reject("Height scale requires a Regular Door and a finite value from 0.1 to 1.0");
		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		auto object = dynamic_pointer_cast<DoorSectorObject>(mSectors[cell.sectorIndex]->_getObject(cell.sectorObjectIndex));
		if (!object || isLiftOwnedDoor(object) || isShuttleOwnedDoor(object)) return reject("Transport-owned Door");
		auto door = object->getDoor();
		if (typeid(*door) != typeid(Door) || door->isChamberOwned()
			|| !isLocationLike(door->getFrontSector()->getType()) || !isLocationLike(door->getBackSector()->getType()))
			return reject("Height scale is only available for ordinary Doors between Locations");
		if (hasActiveDoorCrossing(door.get())) return reject("Cannot change Door height during an active crossing");
		if (options.heightScale == scale) { if (diagnostic) diagnostic->clear(); return true; }
		try
		{
			for (auto layer : {layerIndex, layerIndex + 1})
				validatePanelWallRectangle(mLayers[layer]->getCellDefinition(x, y).sectorIndex,
					{float(x) + CORE_DOOR_X_INSET, float(y)},
					{float(x + width) - CORE_DOOR_X_INSET, float(y) + Door::effectiveHeight(options.height, scale)});
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		invalidateSimulationSnapshot();
		door->setHeightScale(scale);
		for (auto it = mConstructionRecords.rbegin(); it != mConstructionRecords.rend(); ++it)
			if (it->type == ConstructionType::Door && it->layer == layerIndex && it->a == y && it->b == x && it->c == width)
			{ it->doorHeightScale = scale; break; }
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setRoomHeightScale(uint32_t sectorIndex, std::optional<float> scale,
		std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!mSimulationPaused) return reject("Pause simulation to change Room height scale");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]) return reject("No such Room");
		auto location = dynamic_pointer_cast<Location>(mSectors[sectorIndex]);
		if (!location || !location->isRoom()) return reject("Height scale is only available for Rooms");
		if (location->getLevelsHigh() != 1) return reject("Height scale only applies to one-level-high Rooms");
		if (!Location::roomHeightScaleIsValid(scale)) return reject("Height scale must be a finite value from 0.2 to 1.0");
		if (location->getHeightScale() == scale) { if (diagnostic) diagnostic->clear(); return true; }
		invalidateSimulationSnapshot();
		if (!location->setHeightScale(scale)) return reject("Height scale could not be applied");
		for (auto it = mConstructionRecords.rbegin(); it != mConstructionRecords.rend(); ++it)
			if (it->type == ConstructionType::Room && it->a == location->getLayerIndex()
				&& it->b == location->getCellY() && it->c == location->getCellX()
				&& it->d == location->getCellsWide() && it->e == location->getLevelsHigh())
			{ it->roomHeightScale = scale; break; }
		for (auto* agent : location->getAgents()) agent->syncPoseToSector();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setSectorDoorHeight(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
		Door::Height height, std::string* diagnostic)
	{
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause simulation to change Door height";
			return false;
		}
		CreateDoorOptions options;
		if (!getSectorDoorOptions(layerIndex, y, x, width, options))
		{
			if (diagnostic) *diagnostic = "The selected Door no longer has an authored definition";
			return false;
		}
		if (height != Door::Height::Regular && height != Door::Height::Tall)
		{
			if (diagnostic) *diagnostic = "Unknown Door height";
			return false;
		}
		if (height == Door::Height::Tall && options.heightScale)
		{
			if (diagnostic) *diagnostic = "Reset Height scale before choosing Tall";
			return false;
		}
		auto const front = getSectorAtPosition(layerIndex, (float)x + 0.5f, (float)y + 0.5f);
		auto const room = dynamic_pointer_cast<const Location>(front);
		if (height == Door::Height::Tall
			&& (!room || room->getType() != SectorType::Location || room->isCorridor()))
		{
			if (diagnostic) *diagnostic = "A tall Door is only available in a Room";
			return false;
		}
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door && record.layer == layerIndex
					&& record.a == y && record.b == x && record.c == width;
			});
		if (found == mConstructionRecords.rend()) return false;
		auto const& doorCell = mLayers[layerIndex]->getCellDefinition(x, y);
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[doorCell.sectorIndex]->_getObject(doorCell.sectorObjectIndex));
		if (doorObject && hasActiveDoorCrossing(doorObject->getDoor().get()))
		{
			if (diagnostic) *diagnostic = "Cannot change Door height during an active crossing";
			return false;
		}
		try
		{
			for (auto layer : {layerIndex, layerIndex + 1})
				validatePanelWallRectangle(mLayers[layer]->getCellDefinition(x, y).sectorIndex,
					{float(x) + CORE_DOOR_X_INSET, float(y)},
					{float(x + width) - CORE_DOOR_X_INSET, float(y)
						+ Door::effectiveHeight(height, options.heightScale)});
		}
		catch (Exception const& error) { if (diagnostic) *diagnostic = error.getMessage(); return false; }
		if (options.height == height) return true;
		invalidateSimulationSnapshot();
		found->e = static_cast<uint32_t>(height);

		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		if (cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex]
			&& cell.sectorObjectIndex < mSectors[cell.sectorIndex]->getNumObjects())
		{
			auto object = dynamic_pointer_cast<DoorSectorObject>(
				mSectors[cell.sectorIndex]->_getObject(cell.sectorObjectIndex));
			if (object && object->getDoor()) object->getDoor()->setHeight(height);
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setLiftInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mSimulationPaused) return false;
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mLift) return false;
		auto record = findLiftDestinationRecord(*resource);
		if (!record || (record->type != ConstructionType::Lift
			&& record->type != ConstructionType::PlatformLift)) return false;
		auto& authored = mConstructionRecords[static_cast<size_t>(record - mConstructionRecords.data())];
		if (authored.initiallyBroken == broken) return true;
		authored.initiallyBroken = resource->mLift->mInitiallyBroken = broken;
		setLiftBroken(id, broken);
		markModified();
		return true;
	}

	bool World::setLiftBroken(TraversalResourceId id, bool broken)
	{
		return mSimulationCoordinator.setLiftBroken(id, broken);
	}

	bool World::setShuttleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mSimulationPaused) return false;
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mShuttle) return false;
		auto record = findLiftDestinationRecord(*resource);
		if (!record || record->type != ConstructionType::Shuttle) return false;
		auto& authored = mConstructionRecords[static_cast<size_t>(record - mConstructionRecords.data())];
		if (authored.initiallyBroken == broken) return true;
		authored.initiallyBroken = resource->mShuttle->mInitiallyBroken = broken;
		setShuttleBroken(id, broken);
		markModified();
		return true;
	}

	bool World::setShuttleBroken(TraversalResourceId id, bool broken)
	{
		return mSimulationCoordinator.setShuttleBroken(id, broken);
	}

	optional<DeviceCondition> World::knownTransportCondition(TraversalResourceId id,
		Agent const* agent, Sector const* observationSector) const
	{
		auto requestedId = id;
		auto requested = mTraversalResources.find(id);
		auto resource = requested;
		if (resource && resource->mLiftCoordinator)
		{
			id = resource->mLiftCoordinator;
			resource = mTraversalResources.find(id);
		}
		if (!resource || (!resource->mLift && !resource->mShuttle)) return nullopt;
		auto const sector = observationSector
			? SectorId{ static_cast<uint64_t>(observationSector->getIndex()) + 1 } : SectorId{};
		bool const visible = sector && (sector == resource->mLiftSector
			|| (resource->mShuttle ? any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
				[&](auto const& door) { return door.locationSector == sector; })
				: any_of(resource->mLiftStops.begin(), resource->mLiftStops.end(),
					[&](auto const& stop) { return stop.locationSector == sector; })));
		if (!visible)
		{
			if (!agent) return nullopt;
			auto vehicle = agent->rememberedDeviceCondition(id);
			if (resource->mShuttle && requestedId != id)
			{
				if (auto remembered = agent->rememberedDeviceCondition(requestedId))
				{
					// Failure/restoration is vehicle-wide. A fresh observation at
					// another approach supersedes stale vehicle knowledge,
					// without refreshing that remote Door's remembered aperture.
					if (vehicle)
					{
						remembered->broken = vehicle->broken;
						remembered->position = vehicle->position;
						remembered->atStop = vehicle->atStop;
					}
					return remembered;
				}
				if (vehicle) vehicle->doorsOpen = false; // Never borrow another Carriage's aperture.
			}
			return vehicle;
		}
		bool const aligned = !resource->mLiftMoving && resource->mLiftCurrentStop < resource->mLiftStops.size();
		if (resource->mShuttle)
		{
			bool const doorVisible = sector == resource->mLiftSector
				|| any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
					[&](auto const& door) { return door.landingResource == requestedId && door.locationSector == sector; });
			auto rememberedDoor = agent ? agent->rememberedDeviceCondition(requestedId) : nullopt;
			bool const open = aligned && (requestedId != id
				? requested->mLiftStopIndex == resource->mLiftCurrentStop && requested->mDoor
					&& (doorVisible ? requested->mDoor->isOpen() : rememberedDoor && rememberedDoor->doorsOpen)
				: any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(), [&](auto const& door)
					{ auto landing = mTraversalResources.find(door.landingResource);
						return door.stopIndex == resource->mLiftCurrentStop && landing && landing->mDoor->isOpen(); }));
			return DeviceCondition{ resource->mShuttle->isBroken(), resource->mLiftPosition, aligned, open };
		}
		auto landing = aligned ? mTraversalResources.find(
			resource->mLiftStops[resource->mLiftCurrentStop].landingResource) : nullptr;
		return DeviceCondition{ resource->mLift->isBroken(), resource->mLiftPosition, aligned,
			aligned && (resource->mOpenPlatformLift
				|| (landing && landing->mDoor && landing->mDoor->isOpen() && resource->mLiftCarDoorOpen)) };
	}

	bool World::setDoorInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mSimulationPaused) return false;
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mDoor || !resource->mDoor->isBreakable()) return false;
		auto door = resource->mDoor;
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(door))
					return record.type == ConstructionType::BulkheadDoor
						&& record.a == bulkhead->getFrontLayer()
						&& record.b == static_cast<uint32_t>(bulkhead->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(bulkhead->getPosition().x + CORE_BULKHEAD_DOOR_WIDTH * 0.5f);
				return record.type == ConstructionType::Door && record.layer == door->getFrontLayer()
					&& record.a == static_cast<uint32_t>(door->getPosition().y)
					&& record.b == static_cast<uint32_t>(door->getPosition().x)
					&& record.c == door->getCellsWide();
			});
		if (found == mConstructionRecords.rend()) return false;
		found->initiallyBroken = door->mInitiallyBroken = broken;
		// Authoring a broken Door also freezes it at its authored broken-open
		// position, rather than whatever fraction it happened to hold live.
		if (broken) door->mOpenPct = door->mBrokenOpenPercentage;
		setDoorBroken(id, broken);
		modify();
		return true;
	}

	bool World::setDoorBrokenOpenPercentage(TraversalResourceId id, float openPercentage)
	{
		if (!mSimulationPaused || !Door::brokenOpenPercentageIsValid(openPercentage)) return false;
		auto resource = mTraversalResources.find(id);
		// Transport and Chamber Doors are not independently breakable, so the
		// breakable guard already confines this to ordinary and standalone
		// Bulkhead Doors.
		if (!resource || !resource->mDoor || !resource->mDoor->isBreakable()) return false;
		auto door = resource->mDoor;
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(door))
					return record.type == ConstructionType::BulkheadDoor
						&& record.a == bulkhead->getFrontLayer()
						&& record.b == static_cast<uint32_t>(bulkhead->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(bulkhead->getPosition().x + CORE_BULKHEAD_DOOR_WIDTH * 0.5f);
				return record.type == ConstructionType::Door && record.layer == door->getFrontLayer()
					&& record.a == static_cast<uint32_t>(door->getPosition().y)
					&& record.b == static_cast<uint32_t>(door->getPosition().x)
					&& record.c == door->getCellsWide();
			});
		if (found == mConstructionRecords.rend()) return false;
		found->brokenOpenPercentage = door->mBrokenOpenPercentage = openPercentage;
		if (door->isBroken()) door->mOpenPct = openPercentage;
		invalidateSimulationSnapshot();
		modify();
		return true;
	}

	bool World::setDoorSpeedOverride(TraversalResourceId id, std::optional<float> speed)
	{
		if (!mSimulationPaused || !Door::speedIsValid(speed)) return false;
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mDoor || resource->mDoor->isChamberOwned()) return false;
		auto door = resource->mDoor;
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(door))
					return record.type == ConstructionType::BulkheadDoor
						&& record.a == bulkhead->getFrontLayer()
						&& record.b == static_cast<uint32_t>(bulkhead->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(bulkhead->getPosition().x + CORE_BULKHEAD_DOOR_WIDTH * 0.5f);
				return record.type == ConstructionType::Door && record.layer == door->getFrontLayer()
					&& record.a == static_cast<uint32_t>(door->getPosition().y)
					&& record.b == static_cast<uint32_t>(door->getPosition().x)
					&& record.c == door->getCellsWide();
			});
		if (found == mConstructionRecords.rend())
		{
			// Landing Doors store their per-instance speed in the transport record.
			uint32_t owner = ~0u, stop = 0, carriage = 0, index = 0;
			bool liftOwned = false, located = false;
			for (auto const& sector : mSectors)
			{
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects() && !located; ++i)
				{
					auto object = dynamic_pointer_cast<const DoorSectorObject>(sector->getObject(i));
					if (!object || object->getDoor() != door) continue;
					liftOwned = isLiftOwnedDoor(object, &owner, &stop);
					located = liftOwned || isShuttleOwnedDoor(object, &owner, &stop, &carriage, &index);
				}
				if (located) break;
			}
			if (!located) return false;
			uint32_t producer = 0;
			for (auto& record : mConstructionRecords)
			{
				if (!constructionTypeCreatesSector(record.type)) continue;
				if (producer++ != owner) continue;
				if (record.type != (liftOwned ? ConstructionType::Lift : ConstructionType::Shuttle)) return false;
				auto count = liftOwned ? 1u : static_cast<uint32_t>(SimulationCoordinator::shuttleDoorOffsets(record.e,
					record.h ? record.h : (1u << 1)).size());
				auto slot = liftOwned ? stop : (stop * record.d + carriage) * count + index;
				record.transportDoorSpeeds.resize(std::max(record.transportDoorSpeeds.size(), size_t(slot + 1)), 0.0f);
				record.transportDoorSpeeds[slot] = speed.value_or(0.0f);
				while (!record.transportDoorSpeeds.empty() && record.transportDoorSpeeds.back() == 0.0f)
					record.transportDoorSpeeds.pop_back();
				invalidateSimulationSnapshot();
				door->setSpeedOverride(speed);
				modify();
				return true;
			}
			return false;
		}
		if (found->doorSpeed == speed) return true;
		invalidateSimulationSnapshot();
		found->doorSpeed = speed;
		door->setSpeedOverride(speed);
		modify();
		return true;
	}

	bool World::setDoorBroken(TraversalResourceId id, bool broken)
	{
		return mSimulationCoordinator.setDoorBroken(id, broken);
	}

	bool World::setExtensibleBroken(TraversalResourceId id, bool broken)
	{
		return mSimulationCoordinator.setExtensibleBroken(id, broken);
	}

	bool World::setExtensibleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mSimulationPaused) return false;
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mExtensible) return false;
		auto device = resource->mExtensible;
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				if (record.type == ConstructionType::Ladder)
					return resource->mLadder && record.layer == mSectors[resource->mLadderSector.value - 1]->getLayerIndex()
						&& record.b == static_cast<uint32_t>(device->getPosition().x)
						&& record.a == static_cast<uint32_t>(device->getPosition().y);
				if (record.type != ConstructionType::SectorLadder && record.type != ConstructionType::ForceBridge)
					return false;
				if ((record.type == ConstructionType::SectorLadder) != (resource->mLadder != nullptr)) return false;
				auto const owner = resource->mLadder ? resource->mLadderSector : resource->mQueueLanes[0].sector;
				if (record.a + 1 != owner.value) return false;
				auto sector = mSectors[record.a];
				return record.c + sector->getCellX() == static_cast<uint32_t>(device->getPosition().x)
					&& record.b + sector->getCellY() == static_cast<uint32_t>(device->getPosition().y);
			});
		if (found == mConstructionRecords.rend()) return false;
		found->initiallyBroken = device->mInitiallyBroken = broken;
		setExtensibleBroken(id, broken);
		modify();
		return true;
	}

	bool World::setSectorDoorOpenStyle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
		Door::OpenStyle style, std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// Same patch shape as a Background recolour: the authored record is the
		// persistence boundary, so the record's style and the live Door move
		// together.  The most recent matching record wins, matching
		// getSectorDoorOptions' read-back.
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == layerIndex
					&& record.a == y && record.b == x && record.c == width;
			});
		if (found == mConstructionRecords.rend())
		{
			if (diagnostic)
				*diagnostic = "The selected Door no longer has an authored definition";
			return false;
		}
		found->j = static_cast<int32_t>(style);

		// The live Door rides with its record so the viewport and the Selection
		// panel show the new style without a rebuild.  The record's coordinates
		// are the Door's own left cell on its front Layer.
		if (layerIndex < mLayers.size() && mLayers[layerIndex])
		{
			auto const layer = mLayers[layerIndex].get();
			if (x < layer->getCellsWide() && y < layer->getLevelsHigh())
			{
				auto const& cell = layer->getCellDefinition(x, y);
				if (cell.sectorObjectType == SectorObjectType::Door
					&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
				{
					auto const sector = mSectors[cell.sectorIndex];
					if (cell.sectorObjectIndex < sector->getNumObjects())
					{
						auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
							sector->_getObject(cell.sectorObjectIndex));
						if (doorObject && doorObject->getDoor())
							doorObject->getDoor()->setOpenStyle(style);
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateDoorResult World::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x)
	{
		invalidateSimulationSnapshot();
		return addSectorDoor(layerIndex, y, x, CreateDoorOptions{});
	}

	bool World::setLiftStopDoorOpenStyle(uint32_t liftSectorIndex, uint32_t stopIndex,
		Door::OpenStyle style, std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// A Lift's landing Doors have no Door records of their own; the Lift's
		// producing record owns them.  The per-stop override therefore rides in
		// that record, which is the persistence boundary: save/load and the
		// editor's snapshot-based undo/redo carry the choice, and any later
		// rebuild replays it.  The Lift's topology is not touched.
		uint32_t producerIndex = 0;
		auto found = mConstructionRecords.end();
		for (auto it = mConstructionRecords.begin(); it != mConstructionRecords.end(); ++it)
		{
			if (!constructionTypeCreatesSector(it->type)) continue;
			if (producerIndex++ == liftSectorIndex) { found = it; break; }
		}
		if (found == mConstructionRecords.end() || found->type != ConstructionType::Lift)
		{
			if (diagnostic) *diagnostic = "The selected Lift no longer has an authored definition";
			return false;
		}
		if (stopIndex >= found->values.size())
		{
			if (diagnostic)
				*diagnostic = "The selected stop is outside the Lift's authored topology";
			return false;
		}
		// Normalize with a preserving resize: a creation/load vector shorter
		// than the stop list keeps every authored prefix entry, and only the
		// newly added slots take the default sentinel.  assign() would discard
		// accepted overrides on earlier stops.
		if (found->overrides.size() < found->values.size())
			found->overrides.resize(found->values.size(), ~0u);
		found->overrides[stopIndex] = static_cast<uint32_t>(style);

		// The live landing Door rides with its record so the viewport and the
		// Selection panel show the new style without a rebuild.  The Door is
		// authored on the Layer in front of the Lift, at the stop's level.
		if (liftSectorIndex < mSectors.size() && mSectors[liftSectorIndex])
		{
			auto const lift = dynamic_pointer_cast<const LiftTransit>(mSectors[liftSectorIndex]);
			if (lift && lift->getLayerIndex() > 0)
			{
				auto const frontLayer = layerInFront(lift->getLayerIndex());
				auto const doorY = lift->getCellY() + found->values[stopIndex];
				if (frontLayer < mLayers.size() && mLayers[frontLayer]
					&& lift->getCellX() < mLayers[frontLayer]->getCellsWide()
					&& doorY < mLayers[frontLayer]->getLevelsHigh())
				{
					auto const& cell = mLayers[frontLayer]->getCellDefinition(lift->getCellX(), doorY);
					if (cell.sectorObjectType == SectorObjectType::Door
						&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
					{
						auto const sector = mSectors[cell.sectorIndex];
						if (cell.sectorObjectIndex < sector->getNumObjects())
						{
							auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
								sector->_getObject(cell.sectorObjectIndex));
							if (doorObject && doorObject->getDoor())
								doorObject->getDoor()->setOpenStyle(style);
						}
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setShuttleDoorOpenStyle(uint32_t shuttleSectorIndex, uint32_t stopIndex,
		uint32_t carriageIndex, uint32_t doorIndex, Door::OpenStyle style, std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// A Shuttle's landing Doors have no Door records of their own; the Shuttle's
		// producing record owns them.  The per-Door override therefore rides in
		// that record, which is the persistence boundary: save/load and the
		// editor's snapshot-based undo/redo carry the choice, and any later
		// rebuild replays it.  The Shuttle's topology is not touched, and the
		// override addresses one cell of the fixed stop/carriage/door grid, so
		// no sibling Door at the same or another stop is reached.
		uint32_t producerIndex = 0;
		auto found = mConstructionRecords.end();
		for (auto it = mConstructionRecords.begin(); it != mConstructionRecords.end(); ++it)
		{
			if (!constructionTypeCreatesSector(it->type)) continue;
			if (producerIndex++ == shuttleSectorIndex) { found = it; break; }
		}
		if (found == mConstructionRecords.end() || found->type != ConstructionType::Shuttle)
		{
			if (diagnostic) *diagnostic = "The selected Shuttle no longer has an authored definition";
			return false;
		}
		auto const doorMask = found->h ? found->h : (1u << 1);
		auto const doorOffsets = SimulationCoordinator::shuttleDoorOffsets(found->e, doorMask);
		if (stopIndex >= found->values.size() || carriageIndex >= found->d
			|| doorIndex >= doorOffsets.size())
		{
			if (diagnostic)
				*diagnostic = "The selected Door is outside the Shuttle's authored topology";
			return false;
		}
		auto const totalSlots = found->values.size() * found->d * doorOffsets.size();
		found->overrides.resize(totalSlots, ~0u);
		auto const slot = (stopIndex * found->d + carriageIndex) * doorOffsets.size() + doorIndex;
		found->overrides[slot] = static_cast<uint32_t>(style);

		// The live landing Door rides with its record so the viewport and the
		// Selection panel show the new style without a rebuild.  The Door is
		// authored on the Layer in front of the Shuttle, at the stop's column.
		if (shuttleSectorIndex < mSectors.size() && mSectors[shuttleSectorIndex])
		{
			auto const transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[shuttleSectorIndex]);
			if (transit && transit->getLayerIndex() > 0)
			{
				auto const frontLayer = layerInFront(transit->getLayerIndex());
				auto const doorX = found->b + found->values[stopIndex]
					+ carriageIndex * (found->e + 1) + doorOffsets[doorIndex];
				if (frontLayer < mLayers.size() && mLayers[frontLayer]
					&& doorX < mLayers[frontLayer]->getCellsWide()
					&& found->a < mLayers[frontLayer]->getLevelsHigh())
				{
					auto const& cell = mLayers[frontLayer]->getCellDefinition(doorX, found->a);
					if (cell.sectorObjectType == SectorObjectType::Door
						&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
					{
						auto const sector = mSectors[cell.sectorIndex];
						if (cell.sectorObjectIndex < sector->getNumObjects())
						{
							auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
								sector->_getObject(cell.sectorObjectIndex));
							if (doorObject && doorObject->getDoor())
								doorObject->getDoor()->setOpenStyle(style);
						}
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateDoorResult World::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options)
	{
		string const caller = format("World::addSectorDoor({}, {}, {}, {})", layerIndex, y, x, options.width);
		// Every rejecting check runs before beginStructuralEdit() so a refused call
		// stays a true no-op: no SectorObjects, traversal resources, events, or
		// construction records, no modified flag, and no dirty topology
		// (ticket #196).
		validateSectorDoorOptions(caller, options);
		uint32_t liftX, liftWidth;
		if (getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			if (options.heightScale) throw WorldException(this, "Transport-owned Doors refuse Height scale");
			string diagnostic;
			CreateDoorOptions normalized;
			normalized.width = liftWidth;
			if (!canAddCorridorDoor(layerIndex, y, liftX, normalized, &diagnostic))
				throw WorldException(this, diagnostic);
			auto const liftIndex = mLayers[layerBehind(layerIndex)]->getCellDefinition(liftX, y).sectorIndex;
			auto lift = dynamic_pointer_cast<LiftTransit>(_getSector(liftIndex));
			auto idlePlan = planResizeLift(liftIndex, lift->getCellX(), lift->getCellY(),
				lift->getCellsWide(), lift->getLevelsHigh());
			if (!idlePlan.valid) throw WorldException(this, idlePlan.diagnostic);
			idlePlan.stopOffsets.clear();
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& value = lift->getStop(stop);
				idlePlan.stopOffsets.push_back((uint32_t)((int)value.sector->getCellY()
					+ value.sectorOffsetY - (int)lift->getCellY()));
			}
			idlePlan.stopOffsets.push_back(y - lift->getCellY());
			sort(idlePlan.stopOffsets.begin(), idlePlan.stopOffsets.end());
			vector<ConstructionRecord> records;
			if (!prepareLiftEdit(idlePlan, records, diagnostic)) throw WorldException(this, diagnostic);
			// The plan is complete and every authored record is built, so the edit
			// can commit without a later refusal stranding a dirty World.
			beginStructuralEdit("addSectorDoor");
			rebuildFromConstructionRecords(std::move(records));
			auto const& cell = mLayers[layerIndex]->getCellDefinition(liftX, y);
			auto sector = _getSector(cell.sectorIndex);
			CreateObjectResult doorResult{ cell.sectorObjectIndex, SectorObjectType::Door, sector };
			auto doorObject = dynamic_pointer_cast<DoorSectorObject>(sector->_getObject(cell.sectorObjectIndex));
			return { doorResult, {}, doorObject->getDoor()->getTraversalResourceId() };
		}
		// Complete option and placement preflight, still before any mutation.
		validateSectorDoorPlacement(caller, layerIndex, y, x, options, false);
		beginStructuralEdit("addSectorDoor");
		auto result = _addSectorDoor(layerIndex, y, x, options);
		ConstructionRecord record{ ConstructionType::Door };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.width; record.d = options.crossingLanes;
		record.e = static_cast<uint32_t>(options.height);
		record.p = options.controls[0]; record.q = options.controls[1];
		record.i = static_cast<int32_t>(options.activationMode); record.x = options.holdOpenSeconds;
		record.j = static_cast<int32_t>(options.openStyle);
		record.initiallyBroken = options.initiallyBroken;
		record.brokenOpenPercentage = options.brokenOpenPercentage;
		record.doorSpeed = options.speedOverride;
		record.doorHeightScale = options.heightScale;
		for (size_t side = 0; side < 2; ++side)
			for (auto permission : options.controlPermissionRequirements[side])
				record.controlPermissionRequirements[side].push_back(
					static_cast<uint32_t>(permission.value));
		recordConstruction(std::move(record));
		return result;
	}

	void World::addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex)
	{
		invalidateSimulationSnapshot();
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
		{
			throw WorldException(this, "The selected Door no longer exists");
		}

		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (doorObject && (isLiftOwnedDoor(doorObject) || isShuttleOwnedDoor(doorObject)))
			throw WorldException(this, "Transport-owned Doors are read-only; their call button is managed by the transport");
		if (!doorObject)
		{
			throw WorldException(this, "The selected object is not a Door");
		}
		auto door = doorObject->getDoor();
		auto sector = mSectors[sectorIndex];
		// A Door owns one Sector in each Layer of the pair it crosses. The selected
		// Sector may sit on either side of that pair; giving the Door a Button
		// always gives it Buttons on both sides.
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector)
		{
			throw WorldException(this, "The selected Door does not belong to this Sector");
		}
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;

		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mConstructionRecords.end())
		{
			throw WorldException(this, "This Door does not support an added Door Button");
		}
		if (source->p && source->q)
		{
			throw WorldException(this, "This Door already has Door Buttons on both sides");
		}
		// The edit is all-or-nothing: every side that still lacks a Button must
		// have space for one before either side is created.
		shared_ptr<const Sector> sides[2];
		sides[0] = door->getFrontSector();
		sides[1] = door->getBackSector();
		bool const hasButton[2] = { source->p, source->q };
		for (uint32_t side = 0; side < 2; ++side)
		{
			if (hasButton[side]) continue;
			auto demand = doorControlDemand(sides[side], doorObject->getCellX(),
				doorObject->getCellY(), door->getCellsWide(), side);
			(void)planPhysicalControls(sides[side]->getLayerIndex(), sides[side]->getIndex(),
				doorObject->getCellY(), &demand);
		}

		beginStructuralEdit("addSectorDoorButton");

		for (uint32_t side = 0; side < 2; ++side)
		{
			if (hasButton[side]) continue;
			auto control = _createDoorButton(sides[side], doorObject->getCellX(),
				doorObject->getCellY(), door->getCellsWide(), CORE_BUTTON_F_AUTO_REENABLE, nullptr, side);
			// A Door Button renders like its Door: solid on the Layer the Door was
			// authored on, an outline from every other Layer.
			auto button = static_pointer_cast<Button>(
				control.sector->getObject(control.index)->_getObject());
			button->_setThresholdLayer(door->getFrontLayer());
			DeviceCommand command;
			command.type = DeviceCommandType::OpenDoor;
			command.desiredState = true;
			command.traversalResource = door->getTraversalResourceId();
			auto point = createPhysicalControlInteractionPoint("Door button", control,
				(float)doorObject->getCellY(), CORE_RESOURCE_SLOT_STANDING_HEIGHT * 0.4f,
				getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
			auto interaction = mInteractionPoints.find(point);
			for (auto permission : source->values)
			{
				interaction->mPermissionRequirement.set(permission - 1);
				source->controlPermissionRequirements[side].push_back(permission);
			}
			if (!addTraversalControl(door->getTraversalResourceId(), point))
			{
				throw WorldException(this, "Could not bind the Door Button to its Door");
			}
		}

		// A Door with a physical open control uses remote-controlled preparation.
		auto resource = mTraversalResources.find(door->getTraversalResourceId());
		resource->mDoorActivationMode = DoorActivationMode::RemoteControlled;
		door->configureTraversal(DoorActivationMode::RemoteControlled,
			door->getTraversalResourceId(), door->mHoldOpenTime);
		// Record the mode the Door had before its first editor-added Button so
		// removal can restore it. Doors loaded with existing Buttons have no such
		// history; removing those Buttons falls back to manual activation.
		if (!source->p && !source->q)
		{
			source->preButtonActivationMode = source->i;
		}
		source->i = static_cast<int32_t>(DoorActivationMode::RemoteControlled);
		source->p = true;
		source->q = true;
		// The direct interaction no longer exists. Its access intent now belongs
		// independently to each generated control.
		source->values.clear();
		door->mPermissionRequirement.reset();
	}

	bool World::canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!doorObject || isLiftOwnedDoor(doorObject) || isShuttleOwnedDoor(doorObject)) return false;
		auto door = doorObject->getDoor();
		auto sector = mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector) return false;
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mConstructionRecords.end() || (source->p && source->q)) return false;
		try
		{
			shared_ptr<const Sector> sides[] = { door->getFrontSector(), door->getBackSector() };
			for (uint32_t side = 0; side < 2; ++side)
			{
				if (side == 0 ? source->p : source->q) continue;
				auto demand = doorControlDemand(sides[side], doorObject->getCellX(),
					doorObject->getCellY(), door->getCellsWide(), side);
				(void)planPhysicalControls(sides[side]->getLayerIndex(), sides[side]->getIndex(),
					doorObject->getCellY(), &demand);
			}
		}
		catch (exception const&) { return false; }
		return true;
	}

	bool World::canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!doorObject || isLiftOwnedDoor(doorObject) || isShuttleOwnedDoor(doorObject)) return false;
		auto door = doorObject->getDoor();
		auto sector = mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector) return false;
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto source = find_if(mConstructionRecords.cbegin(), mConstructionRecords.cend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		return source != mConstructionRecords.cend() && (source->p || source->q);
	}

	shared_ptr<const DoorSectorObject> World::removeSectorDoorButton(uint32_t sectorIndex,
		uint32_t objectIndex, optional<vector<AccessPermissionId>> resultingRequirement)
	{
		invalidateSimulationSnapshot();
		if (!mSimulationPaused)
			throw WorldException(this, "Removing a Door Button requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
		{
			throw WorldException(this, "The selected Door no longer exists");
		}
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (doorObject && (isLiftOwnedDoor(doorObject) || isShuttleOwnedDoor(doorObject)))
			throw WorldException(this, "Transport-owned Doors are read-only; their call button is managed by the transport");
		if (!doorObject)
		{
			throw WorldException(this, "The selected object is not a Door");
		}
		auto door = doorObject->getDoor();
		auto sector = mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector)
		{
			throw WorldException(this, "The selected Door does not belong to this Sector");
		}
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto const doorX = doorObject->getCellX();
		auto const doorY = doorObject->getCellY();
		auto const doorWidth = door->getCellsWide();
		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorY
					&& record.b == doorX
					&& record.c == doorWidth;
			});
		if (source == mConstructionRecords.end())
		{
			throw WorldException(this, "This Door does not support an added Door Button");
		}
		if (!source->p && !source->q)
		{
			throw WorldException(this, "This Door has no Door Buttons to remove");
		}

		// Collapse two independent operations only when their requirements agree.
		// A caller resolving a conflict must supply the resulting all-of set;
		// omitting it refuses before the rebuild, leaving the Door untouched.
		array<vector<AccessPermissionId>, 2> sideRequirements;
		auto resource = mTraversalResources.find(door->getTraversalResourceId());
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (!point) continue;
			size_t side = point->mSector == SectorId{ static_cast<uint64_t>(door->getBackSector()->getIndex()) + 1 } ? 1 : 0;
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (point->mPermissionRequirement.test(bit))
					sideRequirements[side].push_back(AccessPermissionId{ bit + 1 });
		}
		vector<AccessPermissionId> collapsed;
		if (resultingRequirement) collapsed = *resultingRequirement;
		else if (sideRequirements[0] == sideRequirements[1]) collapsed = sideRequirements[0];
		else throw WorldException(this,
			"Door controls have different permission requirements; choose the resulting manual Door requirement");
		bitset<AccessPermission::Capacity> seenPermissions;
		for (auto permission : collapsed)
		{
			if (!lookupAccessPermission(permission))
				throw WorldException(this, "The resulting Door requirement references an unknown Access permission");
			if (seenPermissions.test(permission.value - 1))
				throw WorldException(this, "The resulting Door requirement contains a duplicate Access permission");
			seenPermissions.set(permission.value - 1);
		}

		// The record is the authored source of truth: clearing its control flags
		// and restoring the pre-Button activation mode, then replaying, removes
		// both Buttons, their InteractionPoints, and their traversal bindings.
		vector<ConstructionRecord> records = mConstructionRecords;
		for (auto& record : records)
		{
			if (record.type == ConstructionType::Door && record.layer == doorLayer
				&& record.a == doorY && record.b == doorX && record.c == doorWidth)
			{
				record.p = false;
				record.q = false;
				record.i = record.preButtonActivationMode >= 0
					? record.preButtonActivationMode
					: static_cast<int32_t>(DoorActivationMode::Manual);
				record.preButtonActivationMode = -1;
				record.values.clear();
				if (record.i == static_cast<int32_t>(DoorActivationMode::Manual))
					for (auto permission : collapsed)
						record.values.push_back(static_cast<uint32_t>(permission.value));
				record.controlPermissionRequirements = {};
			}
		}
		rebuildFromConstructionRecords(std::move(records));

		// The rebuild replaced every object, so re-resolve the Door on its front
		// Layer and hand the fresh object back for the caller's selection.
		auto const& cell = mLayers[doorLayer]->getCellDefinition(doorX, doorY);
		if (cell.sectorObjectType == SectorObjectType::Door && cell.sectorObjectIndex != ~0u)
		{
			auto rebuilt = dynamic_pointer_cast<DoorSectorObject>(
				_getSector(cell.sectorIndex)->_getObject(cell.sectorObjectIndex));
			if (rebuilt) return rebuilt;
		}
		return nullptr;
	}

	World::CreateDoorResult World::_addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, bool controlsAreExternallyBound)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::addSectorDoor({}, {}, {}, {})", layerIndex, y, x, options.width);

		auto const cellsWide = options.width;
		constexpr uint32_t levelsHigh = 1;

		// The placement preflight is the sole home of the rejecting checks. It has
		// proved every cell of the rectangle is occupied inside one Sector on each
		// Layer of the pair, so the Door's two Sectors come straight from the
		// authored cell.
		validateSectorDoorPlacement(caller, layerIndex, y, x, options, controlsAreExternallyBound);

		auto const backLayer = layerBehind(layerIndex);
		auto frontLayer = getLayer(layerIndex);
		auto behindLayer = getLayer(backLayer);
		shared_ptr<Sector> sectors[2] = {
			_getSector(frontLayer->getCellDefinition(x, y).sectorIndex),
			_getSector(behindLayer->getCellDefinition(x, y).sectorIndex)
		};

		// Create door, making sure we add it to the other Location as well
		// If there is a button, then a stateful button will control a stateless door - the state
		// can only be in one object.
		auto doorObject = createDoor(layerIndex, x, y, cellsWide, options.height);
		auto doorSectorObject = dynamic_pointer_cast<DoorSectorObject>(doorObject.sector->_getObject(doorObject.index));
		auto door = doorSectorObject->getDoor();
		door->setOpenStyle(options.openStyle);
		door->setSpeedOverride(options.speedOverride);
		door->setHeightScale(options.heightScale);
		// Generated transport Doors use the same construction helper but are not
		// independently breakable in the ordinary Door slice.
		door->mBreakable = isLocationLike(sectors[0]->getType())
			&& isLocationLike(sectors[1]->getType());
		door->mInitiallyBroken = door->mBroken = door->mBreakable && options.initiallyBroken;
		door->mBrokenOpenPercentage = options.brokenOpenPercentage;
		if (door->mBroken) door->mOpenPct = options.brokenOpenPercentage;
		auto traversalResource = createDoorTraversalResource(
			format("Door at {},{}", x, y), door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		if (options.crossingLanes != 0)
		{
			configureDoorCrossingLanes(traversalResource, options.crossingLanes);
		}

		// Door approaches are explicit resource geometry. Seed each side at the
		// threshold, which Door placement already proved has traversable floor.
		// Queue positions are expanded below only across contiguous usable floor;
		// an unrelated gap elsewhere in a multi-level Room must not invalidate the Door.
		auto const threshold = Vector2{ x + cellsWide * 0.5f, (float)y };
		for (auto const& sector : sectors)
			configureDoorQueueLane(traversalResource,
				SectorId{ (uint64_t)sector->getIndex() + 1 }, threshold,
				Vector2::UNIT_X, 0.0f);

		// Each side owns an independent centre-first queue. Expand left and right
		// until that direction reaches either the Sector boundary or a floor gap.
		auto queueResource = mTraversalResources.find(traversalResource);
		auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		for (auto& lane : queueResource->mQueueLanes)
		{
			auto sector = mSectors[(size_t)lane.sector.value - 1];
			auto positionFits = [&](float positionX)
			{
				if (positionX - halfWidth < sector->getCellX0() - 0.001f
					|| positionX + halfWidth > sector->getCellX1() + 1.0f + 0.001f
					|| threshold.y < sector->getCellY0() - 0.001f
					|| threshold.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
					return false;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(positionX));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(threshold.y));
				return mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot();
			};

			vector<Vector2> positions{ threshold };
			bool scanLeft = true, scanRight = true;
			for (uint32_t step = 1; scanLeft || scanRight; ++step)
			{
				auto const distance = step * (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
				auto const left = threshold.x - distance;
				auto const right = threshold.x + distance;
				if (scanLeft)
				{
					scanLeft = positionFits(left);
					if (scanLeft) positions.push_back({ left, threshold.y });
				}
				if (scanRight)
				{
					scanRight = positionFits(right);
					if (scanRight) positions.push_back({ right, threshold.y });
				}
			}
			lane.origin = threshold;
			lane.direction = Vector2::UNIT_X;
			lane.extent = positions.empty() ? 0.0f : max(abs(positions.back().x - threshold.x),
				abs(positions.front().x - threshold.x));
			lane.positions = std::move(positions);
			lane.positionOwners.assign(lane.positions.size(), {});
		}

		// Only the authored Layer carries the Door in its cell grid. The shared
		// Door remains registered in the destination Sector for rendering and
		// traversal, without replacing that Layer's own object occupancy.
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = frontLayer->getCellDefinition(ix, iy);
			cellDef0.sectorObjectIndex = doorObject.index;
			cellDef0.sectorObjectType = doorObject.type;
		}

		// Bind physical controls to typed device commands
		CreateObjectResult createdControls[2];

		for (int i = 0; i < 2; ++i)
		{
			if (options.controls[i])
			{
				auto buttonObject = _createDoorButton(sectors[i], x, y, cellsWide, CORE_BUTTON_F_AUTO_REENABLE, &createdControls[i].index, (uint32_t)i);
				createdControls[i].type = SectorObjectType::InteractionPoint;
				createdControls[i].sector = sectors[i];
				// A Door Button renders like its Door: solid on the authored Layer,
				// an outline from every other Layer.
				static_pointer_cast<Button>(
					buttonObject.sector->getObject(buttonObject.index)->_getObject())
					->_setThresholdLayer(layerIndex);

				if (!controlsAreExternallyBound)
				{
					DeviceCommand command;
					command.type = DeviceCommandType::OpenDoor;
					command.desiredState = true;
					command.traversalResource = traversalResource;
					auto point = createPhysicalControlInteractionPoint("Door button",
						createdControls[i], (float)y, CORE_RESOURCE_SLOT_STANDING_HEIGHT * 0.4f,
						getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
					auto interaction = mInteractionPoints.find(point);
					for (auto permission : options.controlPermissionRequirements[i])
					{
						interaction->mPermissionRequirement.set(permission.value - 1);
					}
					addTraversalControl(traversalResource, point);
				}
			}
		}


		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}

	bool World::canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, string* diagnostic) const
	{
		string caller = format("World::addSectorWindow({}, {}, {}, {})", layerIndex, y, x, cellsWide);
		try
		{
			validateLayer(caller, layerIndex);
			validateBounds(caller, x, y, cellsWide, levelsHigh);

			// A Window joins its own Layer to the Layer directly behind it.  The back-most
			// Layer has nothing behind it, so a Window there would cross no threshold and
			// could never take part in the Graph.  Maps written before the rule can still
			// replay one from that Layer - see createWindow() - but none may be authored.
			if (!mDeserializingConstruction && isBackMostLayer(layerIndex, getLayerCount()))
				throw WorldException(this, format(
					"{} - a Window needs a Layer behind it, and Layer {} is the back-most Layer",
					caller, layerIndex));

			vector<uint32_t> requiredLayers{ layerIndex };
			if (layerIndex + 1 < getLayerCount()) requiredLayers.push_back(layerBehind(layerIndex));

			// The front Layer of the pair keeps the strict one-Sector rule: a Window
			// whose own wall straddles two Sectors has no coherent frame.  The Layer
			// behind may span several Backgrounds, but may not mix one with a Room.
			for (auto requiredLayer : requiredLayers)
				validateSpaceOnlyInOneSector(caller, requiredLayer, x, y, cellsWide, levelsHigh,
					requiredLayer != layerIndex);

			for (auto requiredLayer : requiredLayers)
			{
				auto layer = getLayer(requiredLayer);
				// A back-side Window may span several Backgrounds/Locations.
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto owner = layer->getCellDefinition(ix, iy).sectorIndex;
						if (owner != ~0u) validatePanelWallRectangle(owner,
							{float(x) + CORE_WINDOW_X_INSET, float(y) + CORE_WINDOW_Y_OFFSET},
							{float(x + cellsWide) - CORE_WINDOW_X_INSET,
								float(y + levelsHigh - 1) + CORE_WINDOW_Y_OFFSET + CORE_WINDOW_HEIGHT});
					}
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& cellDef = layer->getCellDefinition(ix, iy);
						if (cellDef.sectorIndex == ~0u)
							throw WorldException(this, format("{} - Layer {} cell at {},{} is not occupied.",
								caller, requiredLayer, ix, iy));
						if (requiredLayer == layerIndex)
							validateObjectAllowedInSector(caller, SectorObjectType::Window, cellDef.sectorIndex);
						else
							validateObjectAllowedInSectorAsLookTarget(caller, SectorObjectType::Window, cellDef.sectorIndex);
						if (requiredLayer == layerIndex
							&& (cellDef.hasObject() || !cellDef.markers.empty()))
							throw WorldException(this, format("{} - another object occupies cell at {},{}",
								caller, ix, iy));
					}
			}
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint32_t World::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		invalidateSimulationSnapshot();
		return addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, {}).window.index;
	}

	World::CreateWindowResult World::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options)
	{
		return addWindowAperture(layerIndex, y, x, cellsWide, levelsHigh, options, false);
	}

	World::CreateWindowResult World::addWindowAperture(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options, bool boothWindow)
	{
		string diagnostic;
		if (boothWindow && (options.traversable || options.style != Window::Style::Clear
			|| (options.initialState != Window::State::Open && options.initialState != Window::State::Closed)))
			throw WorldException(this, "BoothWindow supports only non-traversable Open or Closed shutters");
		if (!(boothWindow ? canAddBoothWindow(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic)
			: canAddSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic)))
			throw WorldException(this, diagnostic);
		if (options.traversable)
		{
			// A Window may look into a Background, but a traversable Window would admit
			// Agents into the Sector behind it, and a Background is seen through, never
			// entered: it owns no walkable floor and takes no part in traversal.
			if (layerIndex + 1 < getLayerCount())
			{
				auto const backLayer = layerBehind(layerIndex);
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& backCell = mLayers[backLayer]->getCellDefinition(ix, iy);
						if (backCell.sectorIndex != ~0u
							&& mSectors[backCell.sectorIndex]->getType() == SectorType::Background)
							throw WorldException(this, format(
								"A traversable Window cannot cross into the Background at {},{}: a Background can be looked into, but never entered",
								ix, iy));
					}
			}
		}
		beginStructuralEdit("addSectorWindow");
		auto layer = getLayer(layerIndex);

		// Create window
		auto createdWindow = createWindow(layerIndex, x, y, cellsWide, levelsHigh, nullptr, boothWindow);
		auto windowIndex = createdWindow.index;
		auto windowObjType = createdWindow.type;
		auto windowSector = createdWindow.sector;
		auto windowObject = dynamic_pointer_cast<WindowSectorObject>(windowSector->_getObject(windowIndex));
		auto window = windowObject->getWindow();
		window->setState(options.initialState, options.style);
		if (boothWindow)
		{
			auto booth = static_pointer_cast<BoothWindow>(window);
			if (mNextBoothWindowId == 0) throw overflow_error("BoothWindow device identity space is exhausted");
			booth->mDeviceId = BoothWindowId{ mNextBoothWindowId++ };
			mBoothWindows.emplace(booth->mDeviceId, booth);
			DeviceCommand toggle;
			toggle.type = DeviceCommandType::ToggleBoothWindow;
			toggle.boothWindow = booth->mDeviceId;
			booth->mPanel = createInteractionPoint("BoothWindow back-side panel",
				SectorId{ static_cast<uint64_t>(booth->getBackSector()->getIndex()) + 1 },
				{ static_cast<float>(x) + 0.5f, static_cast<float>(y) }, 0.25f,
				getFixedTimestep(), {{ toggle, InteractionBindingRequirement::Required }});
			mInteractionPoints.find(booth->mPanel)->mBoothWindowOwner = booth->mDeviceId;
		}
		TraversalResourceId traversalResource;
		if (options.traversable && window->getFrontSector() && window->getBackSector())
		{
			traversalResource = createWindowTraversalResource(format("Window at {},{}", x, y), window);
			window->configureTraversal(true, traversalResource);
		}

		// Set layers
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorObjectIndex = windowIndex;
				cellDef.sectorObjectType = windowObjType;
			}

		ConstructionRecord record{ boothWindow ? ConstructionType::BoothWindow : ConstructionType::Window };
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = levelsHigh;
		record.p = options.traversable;
		record.i = static_cast<int32_t>(options.initialState);
		record.j = static_cast<int32_t>(options.style);
		recordConstruction(std::move(record));
		return { { windowIndex, windowObjType, windowSector }, window, traversalResource };
	}

	bool World::canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
		uint32_t width, uint32_t height, string* diagnostic) const
	{
		auto reject = [&](string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (width != 1 || height != 1) return reject("BoothWindow requires a fixed one-cell-wide, one-Level-high footprint");
		if (layer >= getLayerCount() || layer + 1 >= getLayerCount())
			return reject("BoothWindow needs an adjacent Layer behind it");
		if (x >= mCellsWide || y >= mLevelsHigh) return reject("BoothWindow position is outside the World");
		for (auto side : { layer, layer + 1 })
		{
			auto const& cell = mLayers[side]->getCellDefinition(x, y);
			if (cell.sectorIndex == ~0u || !isLocationLike(mSectors[cell.sectorIndex]->getType()))
				return reject(format("BoothWindow requires a Room, Corridor, or Facade on Layer {}", side));
			if (cell.floorType == CellFloorType::None || cell.floorType == CellFloorType::ForceBridge)
				return reject(format("BoothWindow requires a walkable approach on Layer {} at Level {}", side, y));
		}
		return canAddSectorWindow(layer, y, x, width, height, diagnostic);
	}

	shared_ptr<const BoothWindow> World::lookupBoothWindow(BoothWindowId id) const
	{
		auto found = mBoothWindows.find(id);
		return found == mBoothWindows.end() ? nullptr : found->second.lock();
	}

	DeviceOperationId World::submitDeviceCommand(DeviceCommand const& command)
	{
		return mSimulationCoordinator.submitDeviceCommand(command);
	}

	World::CreateWindowResult World::addBoothWindow(uint32_t layer, uint32_t y, uint32_t x, Window::State state)
	{
		return addWindowAperture(layer, y, x, 1, 1, { false, state, Window::Style::Clear }, true);
	}

	bool World::setBoothWindowInitialState(uint32_t layer, uint32_t y, uint32_t x, Window::State state)
	{
		if (state != Window::State::Open && state != Window::State::Closed)
			throw WorldException(this, "BoothWindow initial state must be Open or Closed");
		auto records = mConstructionRecords;
		for (auto& record : records)
			if (record.type == ConstructionType::BoothWindow && record.a == layer && record.b == y && record.c == x)
			{
				if (record.i == static_cast<int32_t>(state)) return false;
				record.i = static_cast<int32_t>(state);
				rebuildFromConstructionRecords(std::move(records));
				modify();
				return true;
			}
		return false;
	}

	bool World::getSectorWindowOptions(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions& options) const
	{
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return (record.type == ConstructionType::Window || record.type == ConstructionType::BoothWindow) && record.a == layerIndex
					&& record.b == y && record.c == x && record.d == cellsWide
					&& record.e == levelsHigh;
			});
		if (found == mConstructionRecords.rend()) return false;
		options.traversable = found->p;
		options.initialState = static_cast<Window::State>(found->i);
		options.style = static_cast<Window::Style>(found->j);
		return true;
	}

	bool World::canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (layerIndex >= mLayers.size() || y >= mLevelsHigh
			|| (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT))
			return reject("Bulkhead Door position is outside the world");
		uint32_t thresholdX;
		if (side == CORE_SIDE_LEFT)
		{
			if (x == 0 || x >= mCellsWide) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x;
		}
		else
		{
			if (x >= mCellsWide - 1) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x + 1;
		}
		if (!isFiniteTiming(options.holdOpenSeconds))
			return reject("Bulkhead Door hold-open time must be finite and non-negative");
		if (!Door::speedIsValid(options.speedOverride))
			return reject("Bulkhead Door speed must be finite and positive");
		if (!Door::brokenOpenPercentageIsValid(options.brokenOpenPercentage))
			return reject("Bulkhead Door broken open percentage must be a finite value from 0 to 1");
		if (!isfinite(options.automaticSensorDistance)
			|| options.automaticSensorDistance < 0.0f)
			return reject("Bulkhead Door automatic sensor distance must be finite and non-negative");
		if (options.crossingLanes != 1)
			return reject("Bulkhead Doors support exactly one crossing lane");
		if (options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
			return reject("Physical controls require a remote-controlled Bulkhead Door");
		if (!mDeserializingConstruction)
			for (auto const& requirement : options.controlPermissionRequirements)
				for (auto permission : requirement)
					if (!lookupAccessPermission(permission))
						return reject("A Bulkhead Door control requirement references an unknown Access permission");
		try
		{
			auto const& left = mLayers[layerIndex]->getCellDefinition(thresholdX - 1, y);
			auto const& right = mLayers[layerIndex]->getCellDefinition(thresholdX, y);
			if (!left.occupied() || !right.occupied())
				return reject("Bulkhead Doors require a Location on each side");
			if (left.sectorIndex == right.sectorIndex)
				return reject("Bulkhead Doors must connect two distinct Locations");
			if (!dynamic_pointer_cast<const Location>(mSectors[left.sectorIndex])
				|| !dynamic_pointer_cast<const Location>(mSectors[right.sectorIndex]))
				return reject("Bulkhead Doors can only connect Locations");
			// A Bulkhead Door is set into a pair of wall ends. A Facade has no
			// wall ends, so there is nowhere for one to go (ADR 0003).
			if (mSectors[left.sectorIndex]->getType() == SectorType::Facade
				|| mSectors[right.sectorIndex]->getType() == SectorType::Facade)
				return reject("A Bulkhead Door cannot connect a Facade: a Facade has no wall ends");
			if (!left.isTraversableOnFoot() || !right.isTraversableOnFoot())
				return reject("Bulkhead Door placement requires a traversable floor on both sides");
			if (left.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u
				|| right.bulkheadIndices[CORE_SIDE_LEFT] != ~0u)
				return reject("A Bulkhead Door already occupies this boundary");
			if (left.sectorObjectType == SectorObjectType::Door
				|| isWindowAperture(left.sectorObjectType)
				|| right.sectorObjectType == SectorObjectType::Door
				|| isWindowAperture(right.sectorObjectType))
				return reject("Another object blocks Bulkhead Door placement");
			validateObjectAllowedInSector("World::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, left.sectorIndex);
			validateObjectAllowedInSector("World::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, right.sectorIndex);
			for (auto owner : {left.sectorIndex, right.sectorIndex})
				validatePanelWallRectangle(owner,
					{float(thresholdX) - CORE_BULKHEAD_DOOR_WIDTH * 0.5f, float(y)},
					{float(thresholdX) + CORE_BULKHEAD_DOOR_WIDTH * 0.5f, float(y) + CORE_DOOR_HEIGHT});
			vector<physicalControl::Demand> demands;
			for (int controlSide = 0; controlSide < CORE_NUM_SIDES; ++controlSide)
				if (options.controls[controlSide])
					demands.push_back(insetControlDemand(mSectors[controlSide == CORE_SIDE_LEFT ? left.sectorIndex : right.sectorIndex],
						physicalControl::OwnerType::BulkheadDoor, { layerIndex, thresholdX, y, 2, 1 }, y, controlSide));
			if (demands.empty()) validatePhysicalControlBoundary(layerIndex, y, thresholdX);
			else validatePhysicalControlAdditions(demands, thresholdX);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateBulkheadDoorResult World::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y,
		uint32_t x, int side)
	{
		invalidateSimulationSnapshot();
		return addSectorBulkheadDoor(layerIndex, y, x, side, CreateBulkheadDoorOptions{});
	}

	World::CreateBulkheadDoorResult World::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options)
	{
		invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(side);

		string caller = format("World::addSectorBulkheadDoor({}, {}, {}, {})", layerIndex, y, x, side);
		string diagnostic;
		// The option and placement preflight runs before beginStructuralEdit() so a
		// refused Bulkhead Door add is a true no-op and no non-finite timing
		// reaches the tick conversion (#198).
		if (!canAddSectorBulkheadDoor(layerIndex, y, x, side, options, &diagnostic))
			throw WorldException(this, format("{} - {}", caller, diagnostic));

		beginStructuralEdit("addSectorBulkheadDoor");

		// Get locations on either side.
		auto layer = getLayer(layerIndex);

		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		// Check that there are no Doors or Windows in cells X and X-1, as there won't
		// be space for them.
		if (cellDef0.sectorObjectType == SectorObjectType::Door || isWindowAperture(cellDef0.sectorObjectType))
		{
			throw WorldException(this, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx0, y));
		}
		if (cellDef1.sectorObjectType == SectorObjectType::Door || isWindowAperture(cellDef1.sectorObjectType))
		{
			throw WorldException(this, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx1, y));
		}

		validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef0.sectorIndex);
		validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef1.sectorIndex);
		
		// Create Bulkhead door
		auto doorObject = createBulkheadDoor(layerIndex, x, y, side);

		// Update cells
		cellDef0.bulkheadIndices[CORE_SIDE_RIGHT] = doorObject.index;
		cellDef1.bulkheadIndices[CORE_SIDE_LEFT] = doorObject.index;
		reflowAllPhysicalControls();

		// Add buttons: place two, one of each side.
		auto sector0 = _getSector(cellDef0.sectorIndex);
		auto sector1 = _getSector(cellDef1.sectorIndex);

		auto dooSectorObject = doorObject.sector->_getObject(doorObject.index);
		auto door = dynamic_pointer_cast<BulkheadDoorSectorObject>(dooSectorObject)->getDoor();

		auto traversalResource = createDoorTraversalResource(format("Bulkhead door at {},{}", x, y),
			door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		door->setAutomaticSensorDistance(options.automaticSensorDistance);
		door->setSpeedOverride(options.speedOverride);
		door->mBreakable = true;
		door->mInitiallyBroken = door->mBroken = options.initiallyBroken;
		door->mBrokenOpenPercentage = options.brokenOpenPercentage;
		if (door->mBroken) door->mOpenPct = options.brokenOpenPercentage;
		configureDoorCrossingLanes(traversalResource, options.crossingLanes);

		// Same-layer geometry gets explicit approaches on opposite sides of the
		// threshold; no Y/layer heuristic participates in authorization.
		auto threshold = Vector2{ (float)cx1, (float)y };
		auto leftOrigin = threshold - Vector2::UNIT_X * CORE_RESOURCE_QUEUE_SLOT_PITCH;
		auto rightOrigin = threshold + Vector2::UNIT_X * CORE_RESOURCE_QUEUE_SLOT_PITCH;
		configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector0->getIndex() + 1 },
			leftOrigin, Vector2::NEGATIVE_UNIT_X,
			max(0.0f, leftOrigin.x - (sector0->getCellX0() + CORE_RESOURCE_SLOT_WIDTH * 0.5f)));
		configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector1->getIndex() + 1 },
			rightOrigin, Vector2::UNIT_X,
			max(0.0f, (sector1->getCellX1() + 1.0f - CORE_RESOURCE_SLOT_WIDTH * 0.5f) - rightOrigin.x));

		CreateObjectResult createdControls[2];

		for (int i = 0; i < CORE_NUM_SIDES; ++i)
		{
			if (!options.controls[i]) continue;
			auto sector = i == CORE_SIDE_LEFT ? sector0 : sector1;
			createdControls[i] = _createBulkheadDoorButton(sector, y, i);
			if (options.activationMode == DoorActivationMode::RemoteControlled)
			{
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = traversalResource;
				auto point = createPhysicalControlInteractionPoint("Bulkhead door button",
					createdControls[i], (float)y, 0.15f, getFixedTimestep(),
					{ { command, InteractionBindingRequirement::Required } });
				auto interaction = mInteractionPoints.find(point);
				for (auto permission : options.controlPermissionRequirements[i])
				{
					interaction->mPermissionRequirement.set(permission.value - 1);
				}
				addTraversalControl(traversalResource, point);
			}
		}
		ConstructionRecord record{ ConstructionType::BulkheadDoor };
		record.a = layerIndex; record.b = y; record.c = x; record.i = side;
		record.p = options.controls[0]; record.q = options.controls[1];
		record.j = static_cast<int32_t>(options.activationMode);
		record.x = options.holdOpenSeconds; record.d = options.crossingLanes;
		record.y = options.automaticSensorDistance;
		record.initiallyBroken = options.initiallyBroken;
		record.brokenOpenPercentage = options.brokenOpenPercentage;
		record.doorSpeed = options.speedOverride;
		for (size_t controlSide = 0; controlSide < 2; ++controlSide)
			for (auto permission : options.controlPermissionRequirements[controlSide])
				record.controlPermissionRequirements[controlSide].push_back(
					static_cast<uint32_t>(permission.value));
		recordConstruction(std::move(record));
		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}

	World::CreateObjectResult World::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
	{
		invalidateSimulationSnapshot();
		auto sector = _getSector(sectorIndex);
		if (!isLocationLike(sector->getType()) || xOffset >= sector->getCellsWide())
			throw WorldException(this, "Light switch requires an authored Location host cell");
		physicalControl::Demand demand;
		demand.owner = { physicalControl::OwnerType::LocationLightSwitch,
			{ sector->getLayerIndex(), sector->getCellX() + xOffset, sector->getCellY(), 1, 1 },
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { 0, sector->getCellX() + xOffset, sector->getCellY() } };
		(void)planPhysicalControls(sector->getLayerIndex(), sectorIndex, sector->getCellY(), &demand);
		beginStructuralEdit("addSectorLightSwitch");
		auto ctrl = _createSectorButton("Lightswitch", sector, xOffset, sector->getCellY(), CORE_BUTTON_F_AUTO_REENABLE);

		auto object = ctrl.sector->_getObject(ctrl.index);
		DeviceCommand command;
		command.type = DeviceCommandType::SetSectorLights;
		command.target = SectorId{ (uint64_t)sectorIndex + 1 };
		command.desiredState = !sector->areLightsOn();
		createPhysicalControlInteractionPoint("Light switch", ctrl,
			(float)object->getCellY(), 0.15f, getFixedTimestep(),
			{ { command, InteractionBindingRequirement::Required } });
		ConstructionRecord record{ ConstructionType::LightSwitch };
		record.a = sectorIndex; record.b = xOffset;
		recordConstruction(std::move(record));
		return ctrl;
	}

	bool World::canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (sectorIndex >= mSectors.size()) return reject("Walkway Room does not exist");
		auto location = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!location) return reject("Walkways require a Location");
		if (levelIndex == 0) return reject("Walkways must be placed above the Location's ground floor");
		if (levelIndex >= location->getLevelsHigh() || xOffset >= location->getCellsWide())
			return reject("Walkway position is outside the Location");

		auto const& cellDef = mLayers[location->getLayerIndex()]->getCellDefinition(
			location->getCellX() + xOffset, location->getCellY() + levelIndex);
		if (cellDef.floorType == CellFloorType::Ground)
			return reject("Walkways cannot replace a ground floor");
		if (cellDef.floorType == CellFloorType::Walkway)
			return reject("A Walkway already occupies this cell");
		if (cellDef.floorType == CellFloorType::ForceBridge)
			return reject("A Force Bridge already occupies this cell");
		if (cellDef.floorType != CellFloorType::None)
			return reject("The destination floor cell is occupied");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateObjectResult World::addSectorWalkway(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::addSectorWalkway({}, {}, {})", sectorIndex, levelIndex, xOffset);
		string diagnostic;
		if (!canAddSectorWalkway(sectorIndex, levelIndex, xOffset, &diagnostic))
			throw WorldException(this, format("{} - {}", caller, diagnostic));

		// Once editing an already-built document, replay the authored structure so
		// every Ladder in this column can shorten to the newly nearest Walkway.
		if (mBuildFinished && !mDeserializingConstruction)
		{
			// The detached replay below validates all affected control families,
			// including new Ladder endpoints and Platform Stops. Do not dirty the
			// live topology before that validation succeeds.
			if (!mSimulationPaused)
				throw WorldException(this, "addSectorWalkway is a structural edit and requires pauseSimulation() before it can run");
			auto room = _getSector(sectorIndex);
			for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			{
				auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(room->getObject(i));
				if (ladderObject && ladderObject->getCellX() == room->getCellX() + xOffset)
				{
					auto ladder = ladderObject->getLadder();
					auto base = ladderObject->getCellY() - room->getCellY();
					auto top = base + ladder->getLevelsHigh() - 1;
					if (levelIndex > base && levelIndex < top && roomLadderIsActive(ladder))
						throw WorldException(this, "A Room Ladder cannot be resized while it is in use");
				}
				auto liftObject = dynamic_pointer_cast<LiftSectorObject>(room->getObject(i));
				if (liftObject && liftObject->getCellX() == room->getCellX() + xOffset
					&& platformLiftIsActive(liftObject->getLift()))
					throw WorldException(this, "The PlatformLift shaft cannot be changed while it is in use");
			}
			auto records = mConstructionRecords;
			ConstructionRecord record{ ConstructionType::Walkway };
			record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
			// A PlatformLift validates authored landing Walkways while replaying, so
			// keep a newly added Walkway before PlatformLifts in the same Room.
			auto beforeLift = find_if(records.begin(), records.end(), [&](auto const& candidate)
				{ return candidate.type == ConstructionType::PlatformLift && candidate.a == sectorIndex; });
			records.insert(beforeLift, record);
			if (!normalizeRoomLadderRecords(records, diagnostic))
				throw WorldException(this, diagnostic);
			rebuildFromConstructionRecords(std::move(records));
			auto rebuilt = _getSector(sectorIndex);
			for (uint32_t i = 0; i < rebuilt->getNumObjects(); ++i)
			{
				auto object = rebuilt->getObject(i);
				if (object && object->getObjectType() == SectorObjectType::Walkway
					&& object->getCellX() == rebuilt->getCellX() + xOffset
					&& object->getCellY() == rebuilt->getCellY() + levelIndex)
					return { i, SectorObjectType::Walkway, rebuilt };
			}
			throw WorldException(this, "Could not locate the added Walkway");
		}
		beginStructuralEdit("addSectorWalkway");

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + xOffset,
			sector->getCellY() + levelIndex);
		auto createdWalkway = createWalkway(layerIndex,
			sector->getCellX() + xOffset, sector->getCellY() + levelIndex);

		cellDef.floorIndex = createdWalkway.index;
		cellDef.floorType = CellFloorType::Walkway;
		ConstructionRecord record{ ConstructionType::Walkway };
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
		recordConstruction(std::move(record));
		return createdWalkway;
	}

	bool World::canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
		string* diagnostic) const
	{
		return canAddSectorMarkerImpl(sectorIndex, levelIndex, xOffset, diagnostic, false);
	}

	bool World::canAddSectorMarkerImpl(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
		string* diagnostic, bool allowCoincident) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (sectorIndex >= mSectors.size()) return reject("Marker sector does not exist");
		auto const& sector = mSectors[sectorIndex];
		if (!sector || !sector->sectorSupportsObjectType(SectorObjectType::Marker))
			return reject("This sector does not support Markers");
		if (levelIndex >= sector->getLevelsHigh()) return reject("Marker level is outside the sector");
		if (!isfinite(xOffset) || xOffset < 0.0f || xOffset >= sector->getSize().x)
			return reject("Marker position is outside the sector");

		auto const cellX = sector->getCellX() + (uint32_t)floor(xOffset);
		auto const cellY = sector->getCellY() + levelIndex;
		auto const& cellDef = mLayers[sector->getLayerIndex()]->getCellDefinition(cellX, cellY);
		if (cellDef.floorType != CellFloorType::Ground
			&& cellDef.floorType != CellFloorType::Walkway)
			return reject("Markers require ground or a Walkway");

		auto const globalX = sector->getCellX() + xOffset;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
			if (!markerObject) continue;
			auto marker = markerObject->getMarker();
			if (!allowCoincident && marker->getCellY() == cellY
				&& fabs(marker->getCellX() + marker->getOffset() - globalX) <= 0.05f)
				return reject("A Marker already exists at this position");
		}

		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::markerNameTaken(string const& trimmed, MarkerId except) const
	{
		// Inspect objects directly: getMarkerIds() followed by lookupMarker()
		// rescans all objects for every identity, making repeated insertion
		// cubic (including both validated restoration replays).
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (!object) continue;
				auto marker = object->getMarker();
				if (marker->getId() != except && marker->getName() == trimmed) return true;
			}
		}
		return false;
	}

	string World::nextGeneratedMarkerName() const
	{
		for (uint64_t suffix = 1; suffix != 0; ++suffix)
		{
			auto candidate = format("Marker {}", suffix);
			if (!markerNameTaken(candidate)) return candidate;
		}
		throw WorldException(this, "No unique generated Marker name is available");
	}

	shared_ptr<Marker> World::mutableMarker(MarkerId id) const
	{
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (!object) continue;
				auto marker = const_pointer_cast<Marker>(object->getMarker());
				if (marker->getId() == id) return marker;
			}
		}
		return nullptr;
	}

	vector<MarkerId> World::getMarkerIds() const
	{
		vector<MarkerId> result;
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (object) result.push_back(object->getMarker()->getId());
			}
		}
		sort(result.begin(), result.end());
		return result;
	}

	shared_ptr<const Marker> World::lookupMarker(MarkerId id) const
	{
		return mutableMarker(id);
	}

	bool World::canRenameMarker(MarkerId id, string const& name, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (!id || !lookupMarker(id)) return reject("Marker does not exist");
		auto const trimmed = Marker::trimName(name);
		string reason;
		if (!Marker::nameIsValid(trimmed, &reason)) return reject(std::move(reason));
		if (markerNameTaken(trimmed, id)) return reject("A Marker with this name already exists");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::renameMarker(MarkerId id, string const& name, string* diagnostic)
	{
		if (!canRenameMarker(id, name, diagnostic)) return false;
		auto const trimmed = Marker::trimName(name);
		auto marker = mutableMarker(id);
		if (marker->getName() == trimmed) return true;
		auto record = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[id](ConstructionRecord const& candidate)
			{
				return (candidate.type == ConstructionType::Marker && candidate.markerId == id)
					|| (candidate.type == ConstructionType::Furniture && std::any_of(candidate.furnitureDestinations.begin(),
						candidate.furnitureDestinations.end(), [id](auto const& p) { return p.marker == id; }));
			});
		if (record == mConstructionRecords.end())
			throw WorldException(this, "renameMarker - Marker has no authored record");
		invalidateSimulationSnapshot();
		marker->setName(trimmed);
		if (record->type == ConstructionType::Furniture)
		{
			for (auto& point : record->furnitureDestinations) if (point.marker == id) point.name = trimmed;
			for (auto& instance : mFurniture)
				for (auto& point : instance.destinations) if (point.marker == id) point.name = trimmed;
		}
		else record->name = trimmed;
		modify();
		return true;
	}

	bool World::setMarkerProperties(MarkerId id, MarkerProperties properties,
		string* diagnostic)
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
			return reject("Marker properties contain unknown bits");
		auto marker = mutableMarker(id);
		if (!marker) return reject("Marker does not exist");
		if (mBuildFinished && !mSimulationPaused)
			return reject("Marker pathing properties can only be changed while the simulation is paused");
		if (marker->getProperties() == properties)
		{
			if (diagnostic) diagnostic->clear();
			return true;
		}
		auto record = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[id](ConstructionRecord const& candidate)
			{
				return (candidate.type == ConstructionType::Marker && candidate.markerId == id)
					|| (candidate.type == ConstructionType::Furniture && std::any_of(candidate.furnitureDestinations.begin(),
						candidate.furnitureDestinations.end(), [id](auto const& p) { return p.marker == id; }));
			});
		if (record == mConstructionRecords.end())
			throw WorldException(this, "setMarkerProperties - Marker has no authored record");
		invalidateSimulationSnapshot();
		marker->setProperties(properties);
		if (record->type == ConstructionType::Furniture)
		{
			for (auto& point : record->furnitureDestinations) if (point.marker == id) point.properties = properties;
			for (auto& instance : mFurniture)
				for (auto& point : instance.destinations) if (point.marker == id) point.properties = properties;
		}
		else record->c = properties;
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateObjectResult World::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		return addSectorMarker(sectorIndex, levelIndex, xOffset,
			nextGeneratedMarkerName(), vertexIdentifier);
	}

	World::CreateObjectResult World::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, string const& name, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddSectorMarker(sectorIndex, levelIndex, xOffset, &diagnostic))
			throw WorldException(this, "World::addSectorMarker - " + diagnostic);
		auto const trimmed = Marker::trimName(name);
		if (!Marker::nameIsValid(trimmed, &diagnostic))
			throw WorldException(this, "World::addSectorMarker - " + diagnostic);
		if (markerNameTaken(trimmed))
			throw WorldException(this, "World::addSectorMarker - A Marker with this name already exists");
		if (mNextMarkerId == 0)
			throw WorldException(this, "World::addSectorMarker - Marker ID space is exhausted");
		auto const id = MarkerId{ mNextMarkerId };
		mNextMarkerId = mNextMarkerId == numeric_limits<uint64_t>::max() ? 0 : mNextMarkerId + 1;
		return addSectorMarkerRestored(sectorIndex, levelIndex, xOffset, id, trimmed,
			0, vertexIdentifier);
	}

	World::CreateObjectResult World::addSectorMarkerRestored(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, MarkerId id, string name,
		MarkerProperties properties, uint32_t* vertexIdentifier, bool allowCoincident)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!id) throw WorldException(this, "Marker ID cannot be zero");
		if (properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
			throw WorldException(this, "Marker properties contain unknown bits");
		if (lookupMarker(id)) throw WorldException(this, "Marker ID is already in use");
		if (!canAddSectorMarkerImpl(sectorIndex, levelIndex, xOffset, &diagnostic, allowCoincident))
			throw WorldException(this, "World::addSectorMarker - " + diagnostic);
		name = Marker::trimName(name);
		if (!Marker::nameIsValid(name, &diagnostic))
			throw WorldException(this, "World::addSectorMarker - " + diagnostic);
		if (markerNameTaken(name))
			throw WorldException(this, "World::addSectorMarker - A Marker with this name already exists");
		beginStructuralEdit("addSectorMarker");

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + (uint32_t)xOffset,
			sector->getCellY() + levelIndex);
		auto createdMarker = createMarker(layerIndex, sector->getCellX(),
			sector->getCellY() + levelIndex, xOffset, id, name, properties,
			vertexIdentifier);
		cellDef.markers.push_back(createdMarker.index);
		ConstructionRecord record{ ConstructionType::Marker };
		record.a = sectorIndex; record.b = levelIndex; record.c = properties;
		record.x = xOffset;
		record.markerId = id; record.name = std::move(name);
		recordConstruction(std::move(record));
		return createdMarker;
	}

	bool World::canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (sectorIndex >= mSectors.size()) return reject("The Marker Sector does not exist");
		auto const sector = mSectors[sectorIndex];
		if (!sector || objectIndex >= sector->getNumObjects())
			return reject("The Marker object does not exist");
		auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(objectIndex));
		if (!markerObject) return reject("The selected object is not a Marker");
		auto const marker = markerObject->getMarker()->getId();
		if (isFurnitureMarker(marker)) return reject("A Furniture-owned Marker cannot be deleted independently");
		return markerHasNoBehaviourReferences(marker, diagnostic);
	}

	bool World::markerHasNoBehaviourReferences(MarkerId marker, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		vector<string> references;
		function<void(AgentBehaviourConfigurationValue const&, string const&,
			AgentId, Agent const&)> collectReferences;
		collectReferences = [&](AgentBehaviourConfigurationValue const& value,
			string const& path, AgentId agentId, Agent const& agent)
		{
			if (auto const* referenced =
				agentBehaviourConfigurationGetIf<MarkerId>(&value);
				referenced && *referenced == marker)
			{
				references.push_back(format(
					"Agent '{}' ({}) configuration field '{}'",
					agent.getName(), agentId.value, path));
			}
			else if (auto const* list =
				agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationList>(&value))
			{
				for (size_t index = 0; index < list->size(); ++index)
					collectReferences((*list)[index],
						path + "[" + to_string(index) + "]", agentId, agent);
			}
			else if (auto const* record =
				agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationRecord>(&value))
			{
				for (auto const& [field, nested] : *record)
					collectReferences(nested, path + "." + field, agentId, agent);
			}
		};
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			if (!agent || !agent->getBehaviourAssignment()) continue;
			for (auto const& [field, value] :
				agent->getBehaviourAssignment()->configuration)
				collectReferences(value, field, agentId, *agent);
		}
		if (!references.empty())
		{
			string message = format("Marker '{}' is referenced by:",
				lookupMarker(marker)->getName());
			for (auto const& reference : references) message += "\n- " + reference;
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		}
		return true;
	}

	bool World::removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
		string* diagnostic)
	{
		if (!canRemoveSectorMarker(sectorIndex, objectIndex, diagnostic)) return false;
		auto sector = _getSector(sectorIndex);
		auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(objectIndex));

		beginStructuralEdit("removeSectorMarker");
		auto const marker = markerObject->getMarker();
		auto& cellDef = mLayers[sector->getLayerIndex()]->getCellDefinition(
			marker->getCellX(), marker->getCellY());
		auto const found = find(cellDef.markers.begin(), cellDef.markers.end(), objectIndex);
		if (found == cellDef.markers.end())
			throw WorldException(this, "removeSectorMarker - Marker is not registered in its cell");
		cellDef.markers.erase(found);
		if (!sector->removeSectorObject(objectIndex)) return false;

		ConstructionRecord record{ ConstructionType::RemoveMarker };
		record.a = sectorIndex;
		record.b = objectIndex;
		record.markerId = marker->getId();
		recordConstruction(std::move(record));
		return true;
	}

	World::CreateForceBridgeResult World::addSectorForceBridge(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		invalidateSimulationSnapshot();
		return addSectorForceBridge(sectorIndex, levelIndex, xOffset, CreateForceBridgeOptions{});
	}

	bool World::calculateSectorForceBridgeWidthToRight(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset, uint32_t& width, string* diagnostic) const
	{
		width = 0;
		auto reject = [&](string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (sectorIndex >= mSectors.size()) return reject("Force Bridge Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Force Bridges can only be placed in Rooms");
		if (levelIndex >= room->getLevelsHigh() || xOffset == 0 || xOffset >= room->getCellsWide())
			return reject("The Force Bridge and both supports must remain inside its Room");
		auto layer = mLayers[room->getLayerIndex()];
		uint32_t x = room->getCellX() + xOffset, y = room->getCellY() + levelIndex;
		auto const& left = layer->getCellDefinition(x - 1, y);
		if (left.floorType != CellFloorType::Ground && left.floorType != CellFloorType::Walkway)
			return reject("The Force Bridge requires Ground or a Walkway on its left");
		uint32_t roomRight = room->getCellX() + room->getCellsWide();
		for (uint32_t ix = x; ix < roomRight; ++ix)
		{
			auto const floor = layer->getCellDefinition(ix, y).floorType;
			if (floor == CellFloorType::Walkway)
			{
				width = ix - x;
				if (width == 0) return reject("Drop the Force Bridge in the gap before the Walkway");
				if (width > CORE_FORCEBRIDGE_MAX_SIZE)
					return reject(format("The next Walkway is farther than the maximum Force Bridge width of {}", CORE_FORCEBRIDGE_MAX_SIZE));
				if (diagnostic) diagnostic->clear();
				return true;
			}
			if (floor != CellFloorType::None)
				return reject("Another floor object blocks the gap before the next Walkway");
		}
		return reject("No Walkway exists to the right of this gap");
	}

	bool World::canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, CreateForceBridgeOptions const& options, string* diagnostic) const
	{
		auto reject = [&](string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (options.fromSide != CORE_SIDE_LEFT && options.fromSide != CORE_SIDE_RIGHT)
			return reject("Force Bridge extension side is invalid");
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			return reject(format("Force Bridge width must be [1,{}]", CORE_FORCEBRIDGE_MAX_SIZE));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			return reject("An extensible Force Bridge requires one or two controls");
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			return reject("A non-extensible Force Bridge must be permanently extended and have no controls");
		if (sectorIndex >= mSectors.size()) return reject("Force Bridge Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Force Bridges can only be placed in Rooms");
		if (levelIndex >= room->getLevelsHigh() || xOffset == 0
			|| (uint64_t)xOffset + options.width >= room->getCellsWide())
			return reject("The Force Bridge and both supports must remain inside its Room");
		auto layer = mLayers[room->getLayerIndex()];
		uint32_t x = room->getCellX() + xOffset, y = room->getCellY() + levelIndex;
		for (uint32_t ix = x; ix < x + options.width; ++ix)
			if (layer->getCellDefinition(ix, y).floorType != CellFloorType::None)
				return reject("The Force Bridge span must be clear air");
		auto supported = [](CellDefinition const& cell)
			{ return cell.floorType == CellFloorType::Ground || cell.floorType == CellFloorType::Walkway; };
		if (!supported(layer->getCellDefinition(x - 1, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its left");
		if (!supported(layer->getCellDefinition(x + options.width, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its right");
		try
		{
			validateSectorForceBridgeOptions("World::canAddSectorForceBridge", options);
			vector<physicalControl::Demand> demands;
			for (uint32_t i = 0; i < options.controlCount; ++i)
				demands.push_back(insetControlDemand(room, physicalControl::OwnerType::ForceBridge,
					{ room->getLayerIndex(), x, y, options.width, 1 }, y,
					i == 0 ? options.fromSide : 1 - options.fromSide));
			validatePhysicalControlAdditions(demands);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateForceBridgeResult World::addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateForceBridgeOptions const& options)
	{
		invalidateSimulationSnapshot();
		string caller = format("World::addSectorForceBridge({}, {}, {}, {}, {}, {})", sectorIndex, levelIndex, xOffset, options.width, options.fromSide, options.startExtended);
		string diagnostic;
		if (!canAddSectorForceBridge(sectorIndex, levelIndex, xOffset, options, &diagnostic))
			throw WorldException(this, format("{} - {}", caller, diagnostic));
		beginStructuralEdit("addSectorForceBridge");
		ASSERT_SIDE_OK(options.fromSide);

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + levelIndex;
		auto layer = getLayer(layerIndex);

		// Create and mark every cell in the authored span as one floor object.
		auto fbObject = createForceBridge(layerIndex, x, y, options);
		for (uint32_t ix = x; ix < x + options.width; ++ix)
		{
			auto& cellDef = layer->getCellDefinition(ix, y);
			cellDef.floorIndex = fbObject.index;
			cellDef.floorType = CellFloorType::ForceBridge;
		}

		auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(
			fbObject.sector->_getObject(fbObject.index))->getForceBridge();
		auto traversalResource = createForceBridgeTraversalResource("Force bridge", forceBridge);
		forceBridge->configureTraversal(traversalResource);
		forceBridge->mInitiallyBroken = forceBridge->mBroken = options.extensible && options.initiallyBroken;
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		configureForceBridgeQueueLanes(traversalResource,
			SectorId{ (uint64_t)sector->getIndex() + 1 },
			{ Vector2{ (float)x - halfAgentWidth, (float)y },
				Vector2{ (float)(x + options.width) + halfAgentWidth, (float)y } });

		// See if a physical control is needed
		CreateObjectResult createdControls[2];

		if (options.extensible)
		{
			if (options.controlCount > 0) forceBridge->addPreparationSide(options.fromSide);
			if (options.controlCount > 1) forceBridge->addPreparationSide(1 - options.fromSide);
			if (x == sector->getCellX0() && (x + options.width - 1) == sector->getCellX1())
			{
				throw WorldException(this, format("{} - No space to place Force Bridge controls", caller));
			}

			if (options.controlCount > 0)
			{
				createdControls[0] = _createForceBridgeButton(fbObject.sector, x, y, options.width, options.fromSide, 0);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[0], (float)y, CORE_RESOURCE_SLOT_WIDTH * 0.5f + 0.001f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				if (auto interaction = mInteractionPoints.find(point))
					for (auto permission : options.controlPermissionRequirements[options.fromSide])
						interaction->mPermissionRequirement.set(permission.value - 1);
				addTraversalControl(traversalResource, point);
			}
			if (options.controlCount > 1)
			{
				createdControls[1] = _createForceBridgeButton(fbObject.sector, x, y, options.width, 1 - options.fromSide, 0);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[1], (float)y, CORE_RESOURCE_SLOT_WIDTH * 0.5f + 0.001f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				if (auto interaction = mInteractionPoints.find(point))
					for (auto permission : options.controlPermissionRequirements[1 - options.fromSide])
						interaction->mPermissionRequirement.set(permission.value - 1);
				addTraversalControl(traversalResource, point);
			}
		}
		// Keep the aggregate at three authored object slots (bridge plus two
		// controls) so changing control count cannot shift unrelated object indices.
		for (uint32_t i = options.controlCount; i < 2; ++i)
			sector->addSectorObject(nullptr);

		CreateForceBridgeResult result{
			fbObject,
			{ createdControls[0], createdControls[1] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::ForceBridge };
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset; record.d = options.width;
		record.i = options.fromSide; record.p = options.extensible; record.q = options.startExtended;
		record.e = options.controlCount;
		record.initiallyBroken = options.extensible && options.initiallyBroken;
		for (size_t side = 0; side < 2; ++side)
			for (auto permission : options.controlPermissionRequirements[side])
				record.controlPermissionRequirements[side].push_back(
					static_cast<uint32_t>(permission.value));
		recordConstruction(std::move(record));
		return result;
	}

	bool World::canAddRoomLadder(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, uint32_t* levelsHigh, string* diagnostic) const
	{
		auto reject = [&](string reason)
		{
			if (levelsHigh) *levelsHigh = 0;
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (sectorIndex >= mSectors.size()) return reject("Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Room Ladders can only be placed in Rooms");
		if (xOffset >= room->getCellsWide() || levelIndex >= room->getLevelsHigh())
			return reject("Ladder base is outside the Room");

		auto const x = room->getCellX() + xOffset;
		auto const y = room->getCellY() + levelIndex;
		auto const& base = mLayers[room->getLayerIndex()]->getCellDefinition(x, y);
		if (base.floorType != CellFloorType::Ground && base.floorType != CellFloorType::Walkway)
			return reject("Drop the Room Ladder on Ground or a Walkway");

		uint32_t topLevel = ~0u;
		for (uint32_t offset = levelIndex + 1; offset < room->getLevelsHigh(); ++offset)
		{
			auto const& cell = mLayers[room->getLayerIndex()]->getCellDefinition(
				x, room->getCellY() + offset);
			if (cell.floorType == CellFloorType::Walkway) { topLevel = offset; break; }
		}
		if (topLevel == ~0u) return reject("No Walkway exists above this position");

		uint32_t const topY = room->getCellY() + topLevel;
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto object = room->getObject(i);
			// A Door's back approach may share the column, matching Door
			// creation after Ladder authoring. Its front footprint still blocks.
			if (auto door = dynamic_pointer_cast<const DoorSectorObject>(object);
				door && door->getDoor()->getBackSector() == room) continue;
			if (!object || object->getObjectType() == SectorObjectType::Walkway
				|| object->getObjectType() == SectorObjectType::InteractionPoint) continue;
			uint32_t const objectX0 = object->getCellX();
			uint32_t const objectX1 = objectX0 + (uint32_t)ceil(object->getSize().x) - 1;
			if (x < objectX0 || x > objectX1) continue;
			uint32_t const objectY0 = object->getCellY();
			uint32_t const objectY1 = objectY0 + (uint32_t)ceil(object->getSize().y) - 1;
			if (object->getObjectType() == SectorObjectType::Ladder)
			{
				// Two independently authored ladders may meet, but never overlap.
				if (max(y, objectY0) < min(topY, objectY1))
					return reject("Another Ladder overlaps this Ladder's interior");
			}
			else if (max(y, objectY0) <= min(topY, objectY1))
				return reject("Another object blocks the Ladder");
		}
		if (levelsHigh) *levelsHigh = topLevel - levelIndex + 1;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateLadderResult World::addRoomLadder(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		invalidateSimulationSnapshot();
		return addRoomLadder(sectorIndex, levelIndex, xOffset, { 0, false, true });
	}

	World::CreateLadderResult World::addRoomLadder(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset, CreateLadderOptions options)
	{
		invalidateSimulationSnapshot();
		uint32_t height{}; string diagnostic;
		if (!canAddRoomLadder(sectorIndex, levelIndex, xOffset, &height, &diagnostic))
			throw WorldException(this, format("World::addRoomLadder - {}", diagnostic));
		options.levelsHigh = height;
		return addSectorLadder(sectorIndex, levelIndex, xOffset, options);
	}

	World::CreateLadderResult World::addSectorLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateLadderOptions const& options)
	{
		invalidateSimulationSnapshot();
		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + levelIndex;

		auto y0 = y;
		auto y1 = y + options.levelsHigh - 1;

		// Checks
		string caller = format("World::addSectorLadder({}, {}, {}, {})", sectorIndex, levelIndex, xOffset, options.startExtended);

		validateSectorLadderOptions(caller, options);
		validateObjectAllowedInSector(caller, SectorObjectType::Ladder, sectorIndex);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, 1, options.levelsHigh);

		if (options.levelsHigh < 2)
		{
			throw WorldException(this, format("{} - Ladder at {},{} must be at least 2 levels high", caller, x, y));
		}

		if (levelIndex > sector->getLevelsHigh())
		{
			throw WorldException(this, format("{} - levelIndex={} out of bounds", caller, levelIndex));
		}

		auto const& baseCell = mLayers[layerIndex]->getCellDefinition(x, y);
		// Stacked Room Ladders may share exactly one Walkway endpoint.
		if (baseCell.hasObject() && baseCell.sectorObjectType != SectorObjectType::Ladder)
			validateCellHasNoObject(caller, layerIndex, x, y);
		// Authored replays may encounter the Ladder before a later Walkway record;
		// the normalized construction set guarantees both endpoint floors exist by
		// finishBuild(). Interactive creation still validates immediately.
		if (!mDeserializingConstruction)
		{
			validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y0);
			validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y1);
		}

		physicalControl::Geometry controlGeometry{ layerIndex, x, y, 1, options.levelsHigh };
		if (options.extensible)
			validatePhysicalControlAdditions({
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y0, 1),
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y1, 1) });
		beginStructuralEdit("addSectorLadder");

		// Create
		auto layer = getLayer(layerIndex);

		auto ladderObject = createLadderSectorObject(layerIndex, x, y, options);
		auto ladder = dynamic_pointer_cast<LadderSectorObject>(
			ladderObject.sector->_getObject(ladderObject.index))->getLadder();
		ladder->mInitiallyBroken = ladder->mBroken = options.extensible && options.initiallyBroken;
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalBatchLimit);
		auto const location = SectorId{ (uint64_t)sectorIndex + 1 };
		configureLadderQueueLanes(traversalResource, { location, location },
			{ Vector2{ (float)x + 0.5f, (float)y0 },
				Vector2{ (float)x + 0.5f, (float)y1 } });
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult& control, size_t endpoint)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createPhysicalControlInteractionPoint("Ladder extension control",
				control, (float)object->getCellY(), 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
			if (auto interaction = mInteractionPoints.find(point))
				for (auto permission : options.controlPermissionRequirements[endpoint])
					interaction->mPermissionRequirement.set(permission.value - 1);
			addTraversalControl(traversalResource, point);
		};

		for (uint32_t iy = y0; iy <= y1; ++iy)
		{
			auto& cellDef = layer->getCellDefinition(x, iy);

			cellDef.sectorObjectType = SectorObjectType::Ladder;
			cellDef.sectorObjectIndex = ladderObject.index;
		}

		// See if a physical control is needed
		CreateObjectResult createdControls[2];

		if (options.extensible)
		{
			// Endpoints independently participate in the complete row allocation.
			createdControls[CORE_LADDER_ENDPOINT_LOW] = createPhysicalControl("Ladder button", layerIndex, y0,
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y0, 1), 0);
			registerExtensionControl(createdControls[CORE_LADDER_ENDPOINT_LOW], CORE_LADDER_ENDPOINT_LOW);


			// Upper
			createdControls[CORE_LADDER_ENDPOINT_HIGH] = createPhysicalControl("Ladder button", layerIndex, y1,
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y1, 1), 0);
			registerExtensionControl(createdControls[CORE_LADDER_ENDPOINT_HIGH], CORE_LADDER_ENDPOINT_HIGH);

		}
		else
		{
			// Reserve the two control slots so toggling extensibility does not shift
			// independently authored object indices in this Room.
			sector->addSectorObject(nullptr);
			sector->addSectorObject(nullptr);
		}

		CreateLadderResult result{
			ladderObject,
			{ createdControls[CORE_LADDER_ENDPOINT_LOW], createdControls[CORE_LADDER_ENDPOINT_HIGH] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::SectorLadder };
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
		record.d = options.levelsHigh; record.e = options.directionalBatchLimit;
		record.p = options.extensible; record.q = options.startExtended;
		record.initiallyBroken = options.extensible && options.initiallyBroken;
		for (size_t endpoint = 0; endpoint < 2; ++endpoint)
			for (auto permission : options.controlPermissionRequirements[endpoint])
				record.controlPermissionRequirements[endpoint].push_back(
					static_cast<uint32_t>(permission.value));
		recordConstruction(std::move(record));
		return result;
	}

	vector<World::PlatformLiftStopCandidate> World::getPlatformLiftStopCandidates(
		uint32_t sectorIndex, uint32_t xOffset) const
	{
		vector<PlatformLiftStopCandidate> result;
		if (sectorIndex >= mSectors.size()) return result;
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor() || xOffset >= room->getCellsWide()) return result;
		auto x = room->getCellX() + xOffset;
		auto sideAvailable = [&](uint32_t y, int side)
		{
			try
			{
				auto demand = transportControlDemand(room, physicalControl::OwnerType::PlatformLift,
					{ room->getLayerIndex(), x, room->getCellY(), 1, y - room->getCellY() + 1 }, x, y, 1);
				return any_of(demand.candidates.begin(), demand.candidates.end(), [&](auto const& candidate)
					{ return candidate.cellX == (side == CORE_SIDE_LEFT ? x : x + 1); });
			}
			catch (WorldException const&) { return false; }
		};
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto walkway = dynamic_pointer_cast<const WalkwaySectorObject>(room->getObject(i));
			if (!walkway || walkway->getCellX() != x || walkway->getCellY() <= room->getCellY()) continue;
			auto level = walkway->getCellY() - room->getCellY();
			if (level >= room->getLevelsHigh()) continue;
			result.push_back({ level, sideAvailable(walkway->getCellY(), CORE_SIDE_LEFT),
				sideAvailable(walkway->getCellY(), CORE_SIDE_RIGHT) });
		}
		sort(result.begin(), result.end(), [](auto const& a, auto const& b)
			{ return a.levelOffset < b.levelOffset; });
		return result;
	}

	bool World::canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset,
		CreateLiftOptions const& requested, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (sectorIndex >= mSectors.size()) return reject("PlatformLift Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("PlatformLifts can only be placed in Rooms");
		if (xOffset >= room->getCellsWide()) return reject("PlatformLift position is outside the Room");
		if (requested.cellsWide != 1) return reject("Editor PlatformLifts are one cell wide");
		if (!isFiniteTiming(requested.platformStopDurationSeconds))
			return reject("PlatformLift stop duration must be finite and non-negative");
		auto stops = requested.stopOffsets;
		sort(stops.begin(), stops.end());
		stops.erase(unique(stops.begin(), stops.end()), stops.end());
		if (stops.size() < 2 || stops.front() != 0)
			return reject("A PlatformLift requires ground and at least one Walkway stop");
		auto candidates = getPlatformLiftStopCandidates(sectorIndex, xOffset);
		auto x = room->getCellX() + xOffset;
		auto layer = mLayers[room->getLayerIndex()];
		for (size_t i = 1; i < stops.size(); ++i)
		{
			auto found = find_if(candidates.begin(), candidates.end(), [&](auto const& candidate)
				{ return candidate.levelOffset == stops[i]; });
			if (found == candidates.end())
				return reject(format("No Walkway exists at level {} in the PlatformLift column", stops[i]));
			if (!found->leftButton && !found->rightButton)
				return reject(format("No valid PlatformLift control support at level {}", stops[i]));
		}
		try
		{
			vector<physicalControl::Demand> demands;
			for (auto stop : stops)
				demands.push_back(transportControlDemand(room, physicalControl::OwnerType::PlatformLift,
					{ room->getLayerIndex(), x, room->getCellY(), requested.cellsWide, stops.back() + 1 },
					x, room->getCellY() + stop, requested.cellsWide));
			validatePhysicalControlAdditions(demands);
		}
		catch (WorldException const& error) { return reject(error.what()); }
		uint32_t top = stops.back();
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto object = room->getObject(i);
			// A Door's back approach may share the column, matching Door
			// creation after Platform lift authoring (#437). Keep front blockers.
			if (auto door = dynamic_pointer_cast<const DoorSectorObject>(object);
				door && door->getDoor()->getBackSector() == room) continue;
			if (!object || object->getObjectType() == SectorObjectType::Walkway
				|| object->getObjectType() == SectorObjectType::InteractionPoint) continue;
			uint32_t objectRight = object->getCellX() + (uint32_t)ceil(object->getSize().x);
			uint32_t objectTop = object->getCellY() + (uint32_t)ceil(object->getSize().y);
			if (x >= object->getCellX() && x < objectRight
				&& room->getCellY() < objectTop && room->getCellY() + top >= object->getCellY())
				return reject("Another object blocks the PlatformLift shaft");
		}
		for (uint32_t level = 0; level <= top; ++level)
			if (!layer->getCellDefinition(x, room->getCellY() + level).markers.empty())
				return reject("A Marker blocks the PlatformLift shaft");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreatePlatformLiftResult World::addSectorPlatformLift(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateLiftOptions const& options)
	{
		invalidateSimulationSnapshot();
		string placementDiagnostic;
		if (levelIndex != 0)
			throw WorldException(this, "PlatformLifts must be placed on a Room's ground floor");
		if (!canAddPlatformLift(sectorIndex, xOffset, options, &placementDiagnostic))
			throw WorldException(this, placementDiagnostic);
		beginStructuralEdit("addSectorPlatformLift");
		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + levelIndex;

		// Checks
		string caller = format("World::addSectorPlatformLift({}, {}, {})", sectorIndex, levelIndex, xOffset);

		validateLiftOptions(caller, options);
		validateObjectAllowedInSector(caller, SectorObjectType::Lift, sectorIndex);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.cellsWide, options.stopOffsets.back());

		if (options.stopOffsets.size() < 2)
		{
			throw WorldException(this, format("{} - PlatformLift at {},{} must have at least 2 stops", caller, x, y));
		}

		if (levelIndex > sector->getLevelsHigh())
		{
			throw WorldException(this, format("{} - levelIndex={} out of bounds", caller, levelIndex));
		}

		physicalControl::Geometry controlGeometry{ layerIndex, x, y, options.cellsWide, options.stopOffsets.back() + 1 };
		for (auto stopOffset : options.stopOffsets)
		{
			auto iy = y + stopOffset;

			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				// An upper platform stop normally replaces a walkway tile. The lift
				// remains the cell's traversable object after construction.
				validateCellTraversableOnFoot(caller, "PlatformLift", layerIndex, ix, iy);
			}

		}

		// Create
		auto liftObject = createPlatformLiftSectorObject(layerIndex, x, y, options);

		for (uint32_t iy = y; iy < y + (options.stopOffsets.back() + 1); ++iy)
		{
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
					
				cellDef.sectorObjectType = SectorObjectType::Lift;
				cellDef.sectorObjectIndex = liftObject.index;
			}
		}

		CreatePlatformLiftResult liftRes;
		liftRes.lift = liftObject;
		for (auto stopOffset : options.stopOffsets)
		{
			CreateObjectResult control = createPhysicalControl("Platform lift button", layerIndex, y + stopOffset,
				transportControlDemand(sector, physicalControl::OwnerType::PlatformLift, controlGeometry,
					x, y + stopOffset, options.cellsWide), CORE_BUTTON_F_AUTO_REENABLE);
			liftRes.buttons.push_back(control);
		}

		// Platform lifts use the same manifest, dwell, cutoff, destination and LOOK
		// policies as enclosed lifts. Their stops deliberately have no door resource:
		// the coordinator owns a virtual boarding boundary instead.
		auto lift = dynamic_pointer_cast<LiftSectorObject>(
			liftObject.sector->_getObject(liftObject.index))->getLift();
		vector<LiftStop> stops;
		for (auto stopOffset : options.stopOffsets)
			stops.push_back({ SectorId{ (uint64_t)sector->getIndex() + 1 },
				(float)(y + stopOffset), {}, {} });
		auto coordinator = createOpenPlatformLiftTraversalResource("Open platform lift journey", lift,
			SectorId{ (uint64_t)sector->getIndex() + 1 }, stops, options.capacity,
			options.platformStopDurationSeconds);
		lift->configureTraversal(coordinator, options.capacity,
			options.platformStopDurationSeconds);
		liftRes.traversalResource = coordinator;
		auto resource = mTraversalResources.find(coordinator);

		// Open platforms have no landing Door resource, so their coordinator owns
		// one physical waiting lane per stop. The positions use the same queue-ticket
		// allocator, spacing, progress tracking, and cancellation cleanup as Doors,
		// Lifts, and Ladders.
		resource->mQueueLanes.clear();
		resource->mQueueLanes.resize(stops.size());
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		for (uint32_t stop = 0; stop < stops.size(); ++stop)
		{
			// Queue support is authored and independent at each Stop, not inherited
			// from another Stop or changed by motion/control conflict reassignment.
			auto rightX = x + options.cellsWide;
			auto rightFloor = rightX <= sector->getCellX1()
				? layer->getCellDefinition(rightX, y + options.stopOffsets[stop]).floorType : CellFloorType::None;
			auto side = rightFloor == CellFloorType::Ground || rightFloor == CellFloorType::Walkway
				? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
			auto queueDirection = side == CORE_SIDE_RIGHT ? 1.0f : -1.0f;
			auto& lane = resource->mQueueLanes[stop];
			lane.sector = SectorId{ (uint64_t)sector->getIndex() + 1 };
			lane.origin = { side == CORE_SIDE_RIGHT ? (float)(x + options.cellsWide) : (float)x,
				stops[stop].globalPosition };
			lane.direction = Vector2::UNIT_X * queueDirection;
			for (uint32_t step = 0; ; ++step)
			{
				auto distance = halfAgentWidth + step * (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
				auto position = lane.origin + lane.direction * distance;
				if (position.x - halfAgentWidth < sector->getCellX0() - 0.001f
					|| position.x + halfAgentWidth > sector->getCellX1() + 1.0f + 0.001f)
					break;
				auto cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
				auto cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
				if (!layer->getCellDefinition(cellX, cellY).isTraversableOnFoot()) break;
				lane.positions.push_back(position);
			}
			lane.positionOwners.resize(lane.positions.size());
			lane.extent = lane.positions.empty() ? 0.0f
				: lane.origin.distanceTo(lane.positions.back());
		}

		auto standingWidth = CORE_RESOURCE_SLOT_WIDTH * options.capacity;
		auto standingStart = x + (options.cellsWide - standingWidth) * 0.5f
			+ CORE_RESOURCE_SLOT_WIDTH * 0.5f - sector->getPosition().x;
		for (uint32_t i = 0; i < options.capacity; ++i)
			resource->mCapacityPositions[i] = { standingStart + CORE_RESOURCE_SLOT_WIDTH * i, 0.0f };

		for (uint32_t i = 0; i < liftRes.buttons.size(); ++i)
		{
			auto& buttonResult = liftRes.buttons[i];
			DeviceCommand call;
			call.type = DeviceCommandType::CallLift;
			call.traversalResource = coordinator;
			call.stopIndex = i;
			auto callPoint = createPhysicalControlInteractionPoint("Platform lift landing call",
				buttonResult, stops[i].globalPosition, 0.15f, getFixedTimestep(),
				{ { call, InteractionBindingRequirement::Required } });
			if (i < options.landingControlPermissionRequirements.size())
				for (auto permission : options.landingControlPermissionRequirements[i])
					mInteractionPoints.find(callPoint)->mPermissionRequirement.set(permission.value - 1);
			mAuthoredControlRequirements[callPoint] = { mDeserializingConstruction ? mConstructionReplayIndex : mConstructionRecords.size(), i };
			resource->mLiftStops[i].callControl = callPoint;

			DeviceCommand select;
			select.type = DeviceCommandType::SelectLiftDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Platform lift destination selector",
				stops[i].locationSector, { x + options.cellsWide * 0.5f, resource->mLiftPosition },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			resource->mControls.push_back(selector);
			if (i == 0) liftRes.interiorSelector = selector;
		}
		resource->mLiftSelector = liftRes.interiorSelector;

		lift->mInitiallyBroken = options.initiallyBroken;
		setLiftBroken(coordinator, options.initiallyBroken);
		ConstructionRecord record{ ConstructionType::PlatformLift };
		record.initiallyBroken = options.initiallyBroken;
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
		record.d = options.cellsWide; record.e = options.capacity;
		record.z = options.platformStopDurationSeconds;
		record.values = options.stopOffsets;
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			record.landingControlPermissionRequirements.emplace_back();
			for (auto permission : requirement)
				record.landingControlPermissionRequirements.back().push_back(
					static_cast<uint32_t>(permission.value));
		}
		recordConstruction(std::move(record));
		return liftRes;
	}

	void World::beginStructuralEdit(string const& operation, bool preserveOtherDumbwaiterCycles)
	{
		invalidateSimulationSnapshot();
		if (mBuildFinished && !mSimulationPaused)
		{
			throw WorldException(this, format(
				"{} is a structural edit and requires pauseSimulation() before it can run", operation));
		}
		modify();
		if (!preserveOtherDumbwaiterCycles)
			mOnlyDumbwaiterTopologyEdits = false;
		mTopologyDirty = true;
		mTopologyValid = false;
		mTopologyDiagnostic = "Traversal topology has unvalidated structural edits";
	}

	// Topology event publication and the teardown of live traversal for a topology
	// rebuild live in SimulationCoordinator (ADR 0004 stage 5); World's
	// pause/resume protocol calls them.

	// Pause and resume are the edit/simulation boundary and stay here, on the
	// structural-edit side of that boundary (ADR 0004 stage 5): they belong to
	// the same contract as beginStructuralEdit, finishBuild and
	// rebuildTraversalTopology, which refuses a structural edit unless the
	// simulation is paused and refuses a resume over dirty topology. The
	// simulation-side work they perform - tearing down every live traversal,
	// remembering each Agent's route intent, restoring those routes onto the new
	// graph and publishing the boundary events - lives in SimulationCoordinator.
	void World::pauseSimulation()
	{
		invalidateSimulationSnapshot();
		if (mSimulationPaused) return;
		if (mCurrentPhase != SimulationPhase::None)
			throw WorldException(this, "Simulation cannot be paused from inside a simulation phase");

		mSimulationPaused = true;
		mAccumulatedTime = 0.0;
		mSimulationCoordinator.cancelAllTraversalForTopologyRebuild();
		mSimulationCoordinator.publishTopologyEvent(SimulationEventType::SimulationPaused);
	}

	bool World::clearAgentPath(AgentId id)
	{
		if (!isSimulationPaused() || agentBehaviourOwnsMovement(id)) return false;
		auto agent = mAgents.find(id);
		if (!agent) return false;
		// Clearing authored route intent is not permission to discard a committed
		// Airlock passenger. Runtime cancellation completes the opposite exit.
		if (agent->getSector() && agent->getSector()->getType() == SectorType::Airlock) return false;
		if (agent->getSector() && agent->getSector()->getType() == SectorType::Chamber)
		{
			mSimulationCoordinator.cancelAgentMovement(id, false);
			agent->clearPath();
			modify();
			return true;
		}
		mSimulationCoordinator.clearAgentMovementForBehaviourEdit(id);
		agent->clearPath();
		modify();
		return true;
	}

	bool World::getPausedPathIntent(Agent const& agent, TopologyPathIntent& intent) const
	{
		for (auto const& [id, candidate] : mPausedPathIntents)
		{
			if (mAgents.find(id) == &agent)
			{
				intent = candidate;
				return true;
			}
		}
		// A request made while paused starts Route planning and clears the old
		// runtime Path/paused intent. Expose its current destination for editor
		// inspection and previews without advancing the planning timer.
		if (isSimulationPaused())
		{
			auto const id = getAgentId(&agent);
			if (auto goal = mMovementGoals.find(id); goal != mMovementGoals.end()
				&& !goal->second.cancelling && !goal->second.actionInvalidated)
			{
				intent = {};
				intent.destinationMarker = goal->second.marker;
				intent.destinationSector = goal->second.sector;
				intent.destinationPosition = goal->second.position;
				if (intent.destinationSector && intent.destinationSector.value <= mSectors.size())
					intent.destinationLocalPosition = intent.destinationPosition
						- mSectors[intent.destinationSector.value - 1]->getPosition();
				intent.wasPathing = goal->second.startPathing;
				return true;
			}
		}
		return false;
	}

	void World::validateTraversalTopology(Graph const& graph) const
	{
		auto validSector = [&](SectorId id) { return id && id.value <= mSectors.size(); };
		auto require = [&](bool condition, string const& diagnostic)
		{
			if (!condition) throw WorldException(this, diagnostic);
		};
		require(mTraversalRequests.entries().empty() && mTraversalPermits.entries().empty(),
			"Traversal requests and permits must be drained before topology replacement");

		for (auto const& edge : graph.getEdges())
		{
			auto id = edge->getTraversalResourceId();
			bool requiresAuthority = edge->getType() == EdgeType::Door
				|| edge->getType() == EdgeType::BulkheadDoor || edge->getType() == EdgeType::Window
				|| edge->getType() == EdgeType::ForceBridge || edge->getType() == EdgeType::Ladder
				|| edge->getType() == EdgeType::LadderMount || edge->getType() == EdgeType::Lift
				|| edge->getType() == EdgeType::LiftMount || edge->getType() == EdgeType::Shuttle
				|| edge->getType() == EdgeType::ShuttleMount;
			require(!requiresAuthority || id,
				format("Edge {} ({}) has no traversal authority", edge->getId(), edge->getDescription()));
			if (!id) continue; // Explicit immediate-permit policy.
			auto resource = mTraversalResources.find(id);
			require(resource != nullptr, format("Edge {} references removed traversal resource {}",
				edge->getId(), id.value));
			bool compatible = edge->getType() == EdgeType::Door || edge->getType() == EdgeType::BulkheadDoor
				? resource->mDoor != nullptr || resource->mAirlock != nullptr || resource->mSecurityScanner != nullptr
				: edge->getType() == EdgeType::Window ? resource->mWindow != nullptr
				: edge->getType() == EdgeType::ForceBridge ? resource->mForceBridge != nullptr
				: edge->getType() == EdgeType::Ladder || edge->getType() == EdgeType::LadderMount
					? resource->mLadder != nullptr
				: edge->getType() == EdgeType::Stairwell || edge->getType() == EdgeType::StairwellMount
					? resource->mStairwell != nullptr
				: edge->getType() == EdgeType::Lift || edge->getType() == EdgeType::LiftMount
					? resource->mLift != nullptr
				: edge->getType() == EdgeType::Shuttle || edge->getType() == EdgeType::ShuttleMount
					? resource->mShuttle != nullptr : true;
			require(compatible, format("Edge {} references an incompatible traversal resource {}",
				edge->getId(), id.value));
		}

		for (auto const& [id, resourcePtr] : mTraversalResources.entries())
		{
			auto const& resource = *resourcePtr;
			if (resource.mDoor || resource.mForceBridge)
			{
				if (resource.mDoor) require(resource.mDoor->getTraversalResourceId() == id,
					format("Door resource {} is not the door's sole configured authority", id.value));
				auto const maximumCrossingLanes = resource.mDoor
					? resource.mDoor->getCellsWide() : 1u;
				require(!resource.mCrossingOwners.empty()
					&& resource.mCrossingOwners.size() <= maximumCrossingLanes,
					format("Queued crossing resource {} has invalid crossing-lane geometry", id.value));
				set<SectorId> approachSectors;
				for (auto const& lane : resource.mQueueLanes)
				{
					if (!lane.sector) continue;
					require(validSector(lane.sector)
						&& (!resource.mDoor || approachSectors.insert(lane.sector).second),
						format("Queued crossing resource {} has invalid approach sectors", id.value));
					require(lane.positions.size() == lane.positionOwners.size() && !lane.positions.empty(),
						format("Queued crossing resource {} has invalid queue-position storage", id.value));
					auto sector = mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& position.x - CORE_RESOURCE_SLOT_WIDTH * 0.5f >= sector->getCellX0() - 0.001f
							&& position.x + CORE_RESOURCE_SLOT_WIDTH * 0.5f <= sector->getCellX1() + 1.001f
							&& position.y >= sector->getCellY0() - 0.001f
							&& position.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT <= sector->getCellY1() + 1.001f,
							format("Queued crossing resource {} has a queue position outside its approach sector", id.value));
				}
			}
			if (resource.mOpenPlatformLift)
			{
				require(resource.mQueueLanes.size() == resource.mLiftStops.size(),
					format("Platform-lift resource {} does not have one queue lane per stop", id.value));
				for (uint32_t stop = 0; stop < resource.mQueueLanes.size(); ++stop)
				{
					auto const& lane = resource.mQueueLanes[stop];
					require(lane.sector == resource.mLiftSector && validSector(lane.sector)
						&& lane.positions.size() == lane.positionOwners.size()
						&& !lane.positions.empty(),
						format("Platform-lift resource {} has invalid queue geometry at stop {}",
							id.value, stop));
					auto sector = mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& abs(position.y - resource.mLiftStops[stop].globalPosition) <= 0.001f
							&& position.x - CORE_RESOURCE_SLOT_WIDTH * 0.5f
								>= sector->getCellX0() - 0.001f
							&& position.x + CORE_RESOURCE_SLOT_WIDTH * 0.5f
								<= sector->getCellX1() + 1.001f,
							format("Platform-lift resource {} has an invalid queue position at stop {}",
								id.value, stop));
				}
			}
			if (resource.mWindow)
				require(resource.mWindow->getTraversalResourceId() == id,
					format("Window resource {} is not the window's configured authority", id.value));
			if (resource.mLadder)
				require(resource.mLadder->getTraversalResourceId() == id,
					format("Ladder resource {} is not the ladder's configured authority", id.value));
			if (resource.mStairwell)
				require(resource.mStairwell->getTraversalResourceId() == id,
					format("Stairwell resource {} is not the stairwell's configured authority", id.value));
			if (resource.mForceBridge)
				require(resource.mForceBridge->getTraversalResourceId() == id,
					format("Force-bridge resource {} is not the bridge's configured authority", id.value));
			if (resource.mLift)
				require(resource.mLift->getTraversalResourceId() == id,
					format("Lift resource {} is not the lift's configured authority", id.value));
			if (resource.mShuttle)
				require(resource.mShuttle->getTraversalResourceId() == id,
					format("Shuttle resource {} is not the shuttle's configured authority", id.value));

			if (resource.mCapacity)
			{
				require(resource.mCapacityPositions.size() == resource.mCapacity
					&& resource.mOccupants.size() == resource.mCapacity
					&& resource.mAdmissionReservations.size() == resource.mCapacity,
					format("Traversal resource {} has inconsistent capacity positions", id.value));
				for (uint32_t i = 0; i < resource.mCapacityPositions.size(); ++i)
				{
					auto const& position = resource.mCapacityPositions[i];
					require(isfinite(position.x) && isfinite(position.y),
						format("Traversal resource {} has a non-finite capacity position", id.value));
					require(!resource.mOccupants[i] || mAgents.find(resource.mOccupants[i]),
						format("Traversal resource {} contains a removed manifest occupant", id.value));
					require(!resource.mAdmissionReservations[i]
						|| mTraversalRequests.find(resource.mAdmissionReservations[i]),
						format("Traversal resource {} contains a stale admission reservation", id.value));
					for (uint32_t j = 0; j < i; ++j)
						require(position.distanceTo(resource.mCapacityPositions[j]) > 0.001f,
							format("Traversal resource {} has overlapping capacity positions", id.value));
				}
			}
			for (auto requestId : resource.mAdmissionQueue)
				require(mTraversalRequests.find(requestId) != nullptr,
					format("Traversal resource {} contains a stale admission queue entry", id.value));
			for (auto owner : resource.mVirtualBoundaryOwners)
				require(!owner || mTraversalRequests.find(owner),
					format("Traversal resource {} contains a stale boundary owner", id.value));
			for (auto const& [leaseId, lease] : resource.mOpenLeases)
			{
				(void)leaseId;
				require(lease.kind == DoorOpenLeaseKind::ExternalHoldOpen || (lease.request
					&& mTraversalRequests.find(lease.request)),
					format("Traversal resource {} contains a stale open lease", id.value));
			}

			for (auto controlId : resource.mControls)
			{
				auto control = mInteractionPoints.find(controlId);
				require(control != nullptr, format("Traversal resource {} references removed control {}",
					id.value, controlId.value));
				auto expected = resource.mLiftCoordinator ? resource.mLiftCoordinator : id;
				require(any_of(control->mBindings.begin(), control->mBindings.end(), [&](auto const& binding)
					{ return binding.command.traversalResource == expected; }),
					format("Control {} does not target traversal resource {}", controlId.value, expected.value));
				if (resource.mDoor)
					require(any_of(resource.mQueueLanes.begin(), resource.mQueueLanes.end(),
						[&](auto const& lane) { return lane.sector == control->mSector; }),
						format("Control {} is unreachable from resource {} approaches", controlId.value, id.value));
				if (resource.mLift || resource.mShuttle)
					require(control->mSector == resource.mLiftSector,
						format("Transport selector {} is outside resource {}", controlId.value, id.value));
			}

			if (resource.mLift || resource.mShuttle)
			{
				require(resource.mLiftStops.size() >= 2 && validSector(resource.mLiftSector),
					format("Transport resource {} has invalid stops or transit sector", id.value));
				for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
				{
					auto const& value = resource.mLiftStops[stop];
					require(validSector(value.locationSector) && isfinite(value.globalPosition)
						&& (stop == 0 || value.globalPosition > resource.mLiftStops[stop - 1].globalPosition),
						format("Transport resource {} has invalid stop {} geometry", id.value, stop));
					if (!resource.mOpenPlatformLift)
					{
						auto landing = mTraversalResources.find(value.landingResource);
						require(landing && landing->mDoor && landing->mLiftCoordinator == id
							&& landing->mLiftStopIndex == stop,
							format("Transport resource {} has invalid landing-door mapping at stop {}", id.value, stop));
					}
					auto call = mInteractionPoints.find(value.callControl);
					require(call && call->mSector == value.locationSector
						&& any_of(call->mBindings.begin(), call->mBindings.end(), [&](auto const& binding)
							{ return binding.command.traversalResource == id
								&& binding.command.stopIndex == stop; }),
						format("Transport resource {} has an invalid landing control at stop {}", id.value, stop));
				}
				if (resource.mShuttle)
					for (auto const& door : resource.mShuttleDoors)
					{
						auto landing = mTraversalResources.find(door.landingResource);
						require(door.stopIndex < resource.mLiftStops.size()
							&& door.carriageIndex < resource.mShuttleCarriages.size()
							&& validSector(door.locationSector) && landing && landing->mDoor
							&& landing->mLiftCoordinator == id && landing->mLiftStopIndex == door.stopIndex,
							format("Shuttle resource {} has an invalid carriage-door mapping", id.value));
					}
			}
		}

		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			require(validSector(point->mSector),
				format("Interaction point {} has an invalid sector", pointId.value));
			for (auto const& binding : point->mBindings)
				if (binding.command.type == DeviceCommandType::RequestAirlock)
					require(validSector(binding.command.target)
						&& getSector(binding.command.target.value - 1)->getType() == SectorType::Airlock
						&& binding.command.stopIndex < 2,
						format("Interaction point {} targets a removed Airlock or invalid entry side", pointId.value));
				else if (binding.command.type == DeviceCommandType::ToggleBoothWindow
					|| binding.command.type == DeviceCommandType::SetBoothWindowState)
					require(bool(lookupBoothWindow(binding.command.boothWindow)),
						format("Interaction point {} targets a removed BoothWindow", pointId.value));
				else if (binding.command.type == DeviceCommandType::PressDumbwaiterLanding)
					require(lookupDumbwaiter(binding.command.dumbwaiter) && binding.command.stopIndex < 2,
						format("Interaction point {} targets a removed Dumbwaiter", pointId.value));
				else if (binding.command.type == DeviceCommandType::SetAccessPanelState)
					require(bool(lookupAccessPanel(binding.command.accessPanel)),
						format("Interaction point {} targets a removed Access panel", pointId.value));
				else if (binding.command.type != DeviceCommandType::SetSectorLights)
					require(mTraversalResources.find(binding.command.traversalResource) != nullptr,
						format("Interaction point {} targets removed traversal resource {}",
							pointId.value, binding.command.traversalResource.value));
		}
	}


	void World::buildGraph()
	{
		RestorationTiming timing("graph-resources");
		invalidateSimulationSnapshot();
		mGraph->build();
		mGraph->validate();
		validateTraversalTopology(*mGraph);
	}

	bool World::rebuildTraversalTopology()
	{
		invalidateSimulationSnapshot();
		if (!mBuildFinished)
		{
			mTopologyDiagnostic = "finishBuild() must establish the initial topology";
			return false;
		}
		if (!mSimulationPaused)
		{
			mTopologyDiagnostic = "Traversal topology can only be rebuilt while the simulation is paused";
			return false;
		}

		auto candidate = make_shared<Graph>(this);
		try
		{
			candidate->build();
			candidate->validate();
			validateTraversalTopology(*candidate);
			mSimulationCoordinator.cancelAllTraversalForTopologyRebuild();
			mGraph = std::move(candidate);
			if (mTopologyDirty && !mOnlyDumbwaiterTopologyEdits)
				for (auto const& sector : mSectors)
					if (auto unit = std::dynamic_pointer_cast<Dumbwaiter>(sector))
						mSimulationCoordinator.resetDumbwaiter(*unit);
			mOnlyDumbwaiterTopologyEdits = true;
			mTopologyDirty = false;
			mTopologyValid = true;
			mTopologyDiagnostic.clear();
			++mTopologyGeneration;
			mSimulationCoordinator.restorePausedPathIntents();
			auto const& graphLog = mGraph->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuilt);
			return true;
		}
		catch (Exception const& error)
		{
			mTopologyDirty = true;
			mTopologyValid = false;
			mTopologyDiagnostic = error.getMessage();
			auto const& graphLog = candidate->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mBuildLog.push_back({ "Topology rebuild", ~0u, LogLevel::Error, mTopologyDiagnostic });
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuildFailed, mTopologyDiagnostic);
			return false;
		}
		catch (exception const& error)
		{
			mTopologyDirty = true;
			mTopologyValid = false;
			mTopologyDiagnostic = error.what();
			mBuildLog.push_back({ "Topology rebuild", ~0u, LogLevel::Error, mTopologyDiagnostic });
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuildFailed, mTopologyDiagnostic);
			return false;
		}
	}

	bool World::resumeSimulation()
	{
		invalidateSimulationSnapshot();
		if (!mAgentBehaviourDependencyDiagnostic.empty())
		{
			mSimulationPaused = true;
			return false;
		}
		if (!mSimulationPaused) return true;
		if (mTopologyDirty || !mTopologyValid)
		{
			if (mTopologyDiagnostic.empty())
				mTopologyDiagnostic = "Traversal topology contains unvalidated structural edits";
			return false;
		}
		mSimulationCoordinator.restorePausedPathIntents();
		mSimulationPaused = false;
		mAccumulatedTime = 0.0;
		mSimulationCoordinator.publishTopologyEvent(SimulationEventType::SimulationResumed);
		return true;
	}

	void World::finishBuild()
	{
		invalidateSimulationSnapshot();
		reflowAllPhysicalControls(true);
		validateRetainedAccessPanels();
		if (mBuildFinished)
		{
			if (!mSimulationPaused)
				throw WorldException(this, "A finished world must be paused before rebuilding topology");
			if (!rebuildTraversalTopology()) throw WorldException(this, mTopologyDiagnostic);
			return;
		}
		try
		{
			buildGraph();
			mBuildFinished = true;
			mTopologyDirty = false;
			mOnlyDumbwaiterTopologyEdits = true;
			mTopologyValid = true;
			mTopologyDiagnostic.clear();
			++mTopologyGeneration;
		}
		catch(Exception const& e)
		{
			auto const& graphLog = mGraph->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mTopologyValid = false;
			mTopologyDiagnostic = e.getMessage();
			throw;
		}

		for (auto const& [id, requirement] : mPendingPermissionRequirements)
			if (auto point = mInteractionPoints.find(id)) point->mPermissionRequirement = requirement;
		mPendingPermissionRequirements.clear();

		auto const& graphLog = mGraph->getBuildLog();
		mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
	}

	shared_ptr<const Sector> World::getSectorAtPosition(uint32_t layerIndex, float x, float y) const
	{
		// Get cell
		int cellX = (int)x;
		int cellY = (int)y;

		if (cellX < 0 || cellY < 0 || cellX >= (int)getCellsWide() || cellY >= (int)getLevelsHigh())
		{
			return nullptr;
		}

		auto const& layer = mLayers[layerIndex];

		auto const& cellDef = layer->getCellDefinition(cellX, cellY);
		return cellDef.sectorIndex != ~0u ? getSector(cellDef.sectorIndex) : nullptr;
	}

	Agent* World::getAgentAtPosition(uint32_t layerIndex, float x, float y) const
	{
		// Get cell
		int cellX = (int)x;
		int cellY = (int)y;

		auto const& layer = mLayers[layerIndex];

		try
		{
			auto const& cellDef = layer->getCellDefinition(cellX, cellY);
			auto sector = getSector(cellDef.sectorIndex);
		
			auto const& agents = sector->getAgents();

			for (auto agent : agents)
			{
				auto bounds = agent->getBounds();

				if (bounds.pointInShape(x, y))
				{
					return agent;
				}
			}
		}
		catch (WorldException&)
		{
			// This should just catch an out-of-bounds validation check, which we don't mind failing.
			return nullptr;
		}

		return nullptr;
	}

	shared_ptr<const Object> World::getObjectAtPosition(uint32_t layerIndex, float x, float y,
		shared_ptr<const SectorObject>* sectorObject) const
	{
		// Explicit boundary-owned Buttons can extend into the preceding cell.
		// Resolve their visible geometry before cell ownership (and Door leaves),
		// but return the authored host SectorObject for interaction/selection.
		for (auto const& placement : mPhysicalControlPlacements)
		{
			if (placement.layerIndex != layerIndex || placement.objectIndex == ~0u) continue;
			auto object = mSectors[placement.sectorIndex]->getObject(placement.objectIndex);
			if (!object || !object->pointInside(x, y)) continue;
			if (sectorObject) *sectorObject = object;
			return object->_getObject();
		}
		// Degenerate panel indicators can extend just beyond their owning cell.
		// Their editor hit region does not change the authored/collision rectangle.
		for (auto const& sector : getSectors(layerIndex))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto panel = dynamic_pointer_cast<const AccessPanelSectorObject>(sector->getObject(i));
					panel && panel->pointInside(x, y))
				{
					if (sectorObject) *sectorObject = panel;
					return panel->_getObject();
				}
		// An open platform is deliberately rendered a little below its nominal
		// floor. Hit-test its current geometry before resolving the pointer through
		// a grid cell, because that rendered strip may lie in the cell below its Room.
		for (auto const& sector : getSectors(layerIndex))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(sector->getObject(i));
				if (!liftObject) continue;
				Vector2 first, second;
				liftObject->getLift()->getCurrentShape(first, second);
				auto minX = min(first.x, second.x), maxX = max(first.x, second.x);
				auto minY = min(first.y, second.y), maxY = max(first.y, second.y);
				if (x < minX || x > maxX || y < minY || y > maxY) continue;
				if (sectorObject) *sectorObject = liftObject;
				return liftObject->getLift();
			}
		try
		{
			auto const& cell = mLayers[layerIndex]->getCellDefinition((int)x, (int)y);
			if (!cell.occupied()) return nullptr;
			auto sector = getSector(cell.sectorIndex);
			// A Facade hosts objects exactly as a Location does (ADR 0003), so
			// hit-testing resolves through it too; without this, controls hosted
			// by a Facade - light switches, door buttons - can never be hovered
			// or clicked (ticket #51).
			if (isLocationLike(sector->getType()))
				return sector->getObjectAtPosition(x, y, sectorObject);
			if (sector->getType() == SectorType::Ladder)
				return dynamic_pointer_cast<const LadderTransit>(sector)->getLadder();
		}
		catch (WorldException const&) {}
		return nullptr;
	}

	// Agent creation, placement, and waking live in SimulationCoordinator
	// (ADR 0004). World owns the Agent registry (ADR 0001) and forwards, so
	// no caller outside World names the coordinator.

	AgentId World::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId, uint32_t levelOffset, float xOffset)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.addOwnedAgentToSector(std::move(agent), sectorId, levelOffset, xOffset);
	}

	AgentId World::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.addOwnedAgentToSector(std::move(agent), sectorId);
	}

	void World::registerBundledAgentTypes()
	{
		if (mAgentTypes.contains("Human")) return;
		auto definition = std::make_shared<AgentTypeDefinition>(
			bundledHumanAgentType());
		mAgentTypes.emplace(definition->typeId, std::move(definition));
	}

	std::shared_ptr<const AgentTypeDefinition> World::resolveAgentType(
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
				auto found = mAgentTypes.find(typeId);
				if (found != mAgentTypes.end()
					&& found->second->resourceName != resourceName)
					throw SerializationException("Agent type ID '" + typeId
						+ "' is declared by competing resources in this World");
				auto definition = std::make_shared<AgentTypeDefinition>(std::move(*resolved));
				mAgentTypes[typeId] = definition;
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
			for (auto const& [id, definition] : mAgentTypes)
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
			if (mAgentTypes.contains(resolved->typeId))
				throw SerializationException(
					"Agent type ID '" + resolved->typeId
					+ "' is declared by competing resources in this World");
			auto definition = std::make_shared<AgentTypeDefinition>(*resolved);
			auto const stored = definition->typeId;
			mAgentTypes.emplace(stored, std::move(definition));
			return mAgentTypes.at(stored);
		}
		// Legacy record: the stable type ID names a bundled or attached type.
		auto found = mAgentTypes.find(typeId);
		if (found == mAgentTypes.end())
			throw SerializationException("Unsupported Agent type '" + typeId + "'");
		return found->second;
	}

	std::unique_ptr<Agent> World::makeScriptAgent(std::string const& typeId,
		std::string const& name)
	{
		auto found = mAgentTypes.find(typeId);
		if (found == mAgentTypes.end())
			throw SerializationException("Unsupported Agent type '" + typeId + "'");
		auto const& definition = *found->second;
		auto result = mAgentTypeRuntime->construct(definition.typeId,
			definition.source, name);
		if (!result.succeeded)
			throw WorldException(this, "Agent type '" + definition.typeId
				+ "' resource '" + definition.resourceName + "': " + result.diagnostic);
		auto agent = std::unique_ptr<Agent>(new Agent(name));
		agent->setTypeIdentity(definition.typeId, definition.displayName,
			definition.resourceName, result.baseline);
		agent->mLuaInstance = std::move(result.instance);
		return agent;
	}

	std::unique_ptr<Agent> World::makeScriptAgentForPlacement(std::string const& typeId,
		std::string const& name, set<AccessPermissionId> const& grants,
		set<PermissionSetId> const& sets)
	{
		auto agent = makeScriptAgent(typeId, name);
		for (auto permission : grants)
		{
			auto found = lookupAccessPermission(permission);
			if (!found) throw invalid_argument(found.diagnostic);
			agent->mDirectAccessGrants.set(permission.value - 1);
		}
		for (auto permissionSet : sets)
		{
			auto found = lookupPermissionSet(permissionSet);
			if (!found) throw invalid_argument(found.diagnostic);
		}
		agent->mPermissionSets = sets;
		return agent;
	}

	bool World::attachAgentType(std::string resourceName, std::string source,
		std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
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
		if (mAgentTypes.contains(preflight.typeId))
			return reject("Agent type ID '" + preflight.typeId
				+ "' is already registered in this World");
		auto definition = std::make_shared<AgentTypeDefinition>();
		definition->typeId = std::move(preflight.typeId);
		definition->displayName = std::move(preflight.displayName);
		definition->resourceName = std::move(resourceName);
		definition->source = std::move(source);
		mAgentTypes.emplace(definition->typeId, std::move(definition));
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::hasAgentType(std::string_view typeId) const
	{
		return mAgentTypes.contains(std::string(typeId));
	}

	std::string World::agentTypeResourceName(std::string_view typeId) const
	{
		auto found = mAgentTypes.find(std::string(typeId));
		return found == mAgentTypes.end() ? std::string{} : found->second->resourceName;
	}

	bool World::detachUnusedAgentType(std::string_view typeId)
	{
		if (typeId == "Human") return false;
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getTypeId() == typeId) return false;
		}
		return mAgentTypes.erase(std::string(typeId)) != 0;
	}

	std::string World::agentTypeDisplayName(std::string_view typeId) const
	{
		auto found = mAgentTypes.find(std::string(typeId));
		return found == mAgentTypes.end() ? std::string{} : found->second->displayName;
	}

	unique_ptr<Agent> World::makePlacementQuery(string const& typeId, string const& name,
		set<AccessPermissionId> const& grants, set<PermissionSetId> const& sets) const
	{
		auto agent = Agent::create(*mAgentTypes.at(typeId), name);
		for (auto permission : grants)
		{
			auto found = lookupAccessPermission(permission);
			if (!found) throw invalid_argument(found.diagnostic);
			agent->mDirectAccessGrants.set(permission.value - 1);
		}
		for (auto permissionSet : sets)
		{
			auto found = lookupPermissionSet(permissionSet);
			if (!found) throw invalid_argument(found.diagnostic);
		}
		agent->mPermissionSets = sets;
		return agent;
	}

	void World::validateAgentLocationPlacement(Sector const& sector, Agent const& agent) const
	{
		if (sector.getType() == SectorType::Dumbwaiter)
			throw invalid_argument("Agents cannot enter a Dumbwaiter shaft or car");
		if (sector.getType() == SectorType::Airlock || sector.getType() == SectorType::Chamber)
			throw invalid_argument(sector.getType() == SectorType::Chamber
				? "Agents cannot be placed inside authored Security scanners"
				: "Agents must enter Airlock chambers through coordinated traversal");
		if (canAgentAccessLocation(sector, agent)) return;
		auto missing = static_cast<Location const&>(sector).getPermissionRequirement() & ~effectiveAccessGrants(agent);
		auto diagnostic = format("Agent '{}' cannot be placed in Location '{}': missing Access permissions", agent.getName(), sector.getName());
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (missing.test(bit))
				diagnostic += format(" {} ('{}')", bit + 1, mAccessPermissions[bit]->getName());
		throw invalid_argument(diagnostic);
	}

	bool World::canPlaceAgentInLocation(uint32_t sectorId, set<AccessPermissionId> const& grants,
		set<PermissionSetId> const& sets, string* diagnostic) const
	{
		try
		{
			auto agent = makePlacementQuery("Human", "New Agent", grants, sets);
			validateAgentLocationPlacement(*getSector(sectorId), *agent);
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AgentId World::createAgent(string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset,
		set<AccessPermissionId> const& grants, set<PermissionSetId> const& sets)
	{
		return createAgent("Human", name, sectorId, levelOffset, xOffset, grants, sets);
	}

	AgentId World::createAgent(string const& name, uint32_t sectorId,
		set<AccessPermissionId> const& grants, set<PermissionSetId> const& sets)
	{
		return createAgent("Human", name, sectorId, grants, sets);
	}

	AgentId World::createAgent(string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createAgent(name, sectorId, levelOffset, xOffset);
	}

	AgentId World::createAgent(string const& name, uint32_t sectorId)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createAgent(name, sectorId);
	}

	AgentId World::createAgent(string typeId, string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createAgent(std::move(typeId), name, sectorId, levelOffset, xOffset);
	}

	AgentId World::createAgent(string typeId, string const& name, uint32_t sectorId)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createAgent(std::move(typeId), name, sectorId);
	}

	AgentId World::createAgent(string typeId, string const& name, uint32_t sectorId,
		uint32_t levelOffset, float xOffset, set<AccessPermissionId> const& grants,
		set<PermissionSetId> const& sets)
	{
		return addOwnedAgentToSector(
			makeScriptAgentForPlacement(typeId, name, grants, sets),
			sectorId, levelOffset, xOffset);
	}

	AgentId World::createAgent(string typeId, string const& name, uint32_t sectorId,
		set<AccessPermissionId> const& grants, set<PermissionSetId> const& sets)
	{
		return addOwnedAgentToSector(
			makeScriptAgentForPlacement(typeId, name, grants, sets), sectorId);
	}

	void World::wakeAllAgents()
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.wakeAllAgents();
	}

	// Snapshot world - the per-entity projections and the whole-world
	// snapshot - lives in SimulationCoordinator (ADR 0004 stage 5). World
	// keeps the whole-world forward with the rest of the tick pipeline below,
	// and this one private forward: creating and removing a traversal resource is
	// entity ownership which stays with World (ADR 0001), and the lifecycle
	// events those paths publish carry the resource snapshot the coordinator
	// builds.
	TraversalResourceSnapshot World::makeTraversalResourceSnapshot(TraversalResourceId id,
		TraversalResource const& resource) const
	{
		return mSimulationCoordinator.makeTraversalResourceSnapshot(id, resource);
	}

	// The queue and admission core - traversal-request creation, queue tickets,
	// queue positions and their refresh, the door queue grant and release, the
	// ladder admission family with its entry-spacing rule, traversal progress and
	// timeouts, permit expiry, and the grant / allocate / deny / commit / cancel /
	// release transaction lifecycle - lives in SimulationCoordinator (ADR 0004).
	// World keeps the entity registries (ADR 0001) and forwards the entry
	// points which still have a caller outside World: Agent's request
	// creation, queue-join query and transaction calls, and the deactivation and
	// topology-rebuild paths which deny, cancel and release. The queue, door
	// queue and ladder admission helpers are reached only from inside the
	// coordinator now, so no forward is left for them.

	TraversalRequestId World::createTraversalRequest(Agent const& agent,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& source,
		shared_ptr<const Vertex> const& destination)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createTraversalRequest(agent, edge, source, destination);
	}

	bool World::stopForAvailableQueuePosition(Agent& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
		float movementDistance)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.stopForAvailableQueuePosition(agent, edge, endpoint, movementDistance);
	}

	bool World::isAtDoorCrossingArrival(Agent const& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& threshold)
	{
		return mSimulationCoordinator.isAtDoorCrossingArrival(agent, edge, threshold);
	}

	void World::refreshQueuePositions(TraversalResource& resource)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.refreshQueuePositions(resource);
	}

	void World::updateTraversalProgressAndTimeouts()
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.updateTraversalProgressAndTimeouts();
	}

	void World::allocateTraversalRequest(TraversalRequestId requestId,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& destination)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.allocateTraversalRequest(requestId, edge, destination);
	}

	// Lift scheduling - stop lookup, destination finding, disembark demand, stop
	// requests, next-stop choice, boarding-direction compatibility, and lift
	// admission release - lives in SimulationCoordinator (ADR 0004). World
	// keeps these entry points and forwards, so no caller outside World
	// names the coordinator.

	uint32_t World::findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const
	{
		return mSimulationCoordinator.findLiftStop(resource, endpoint);
	}

	uint32_t World::findAgentLiftDestination(Agent const& agent,
		TraversalResource const& resource) const
	{
		return mSimulationCoordinator.findAgentLiftDestination(agent, resource);
	}

	bool World::liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const
	{
		return mSimulationCoordinator.liftHasDisembarkDemand(resource, stop);
	}

	void World::addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.addLiftStopRequest(resource, stop, owner);
	}

	void World::removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.removeLiftStopRequest(resource, stop, owner);
	}

	uint32_t World::chooseNextLiftStop(TraversalResource& resource) const
	{
		return mSimulationCoordinator.chooseNextLiftStop(resource);
	}

	bool World::isLiftBoardingDirectionCompatible(TraversalResource& resource,
		uint32_t originStop, uint32_t destinationStop)
	{
		return mSimulationCoordinator.isLiftBoardingDirectionCompatible(resource, originStop, destinationStop);
	}

	void World::releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.releaseLiftAdmission(requestId, resource);
	}


	// Shuttle door assignment - the passenger-carriage lookup, the boarding and disembark
	// door selection, and the door-traversal retargeting they share - lives in
	// SimulationCoordinator (ADR 0004), together with the static door-offset helper
	// which gives the carriage/door indexing its meaning. Both selections are reached
	// only from inside the coordinator now, so no World forward is left for them.

	// Passenger safe exits - the safe-exit request, the safe-exit path assignment,
	// and the onboard destination replacement - live in SimulationCoordinator
	// (ADR 0004). World keeps these entry points and forwards, so no caller
	// outside World names the coordinator.

	void World::requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.requestLiftPassengerSafeExit(passenger, reason);
	}

	void World::assignLiftSafeExitPaths(TraversalResource& resource)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.assignLiftSafeExitPaths(resource);
	}

	bool World::replaceOnboardLiftDestination(Agent& agent, shared_ptr<Path> const& path,
		uint32_t& sourceNode)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.replaceOnboardLiftDestination(agent, path, sourceNode);
	}

	// Open platform lift traversal allocation - calling the platform, boarding at the
	// reserved queue position, onboard destination selection, and disembark through the
	// virtual boundary - lives in SimulationCoordinator (ADR 0004), alongside the
	// admission release which undoes it. It is reached only from the coordinator's own
	// lift allocation dispatcher now, so no World forward is left for it.

	void World::denyTraversalRequest(TraversalRequestId requestId, TraversalFailureReason reason)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.denyTraversalRequest(requestId, reason);
	}

	bool World::commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
		shared_ptr<const Vertex> const& destination)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.commitTraversal(agent, requestId, permitId, destination);
	}

	void World::cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
		bool requestSafeTransportExit)
	{
		mSimulationCoordinator.cancelTraversal(requestId, permitId, requestSafeTransportExit);
	}

	void World::releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.releaseTraversal(requestId, permitId);
	}


	// Agent lookup, id resolution, removal, and traversal-ownership release live
	// in SimulationCoordinator (ADR 0004). World owns the registries (ADR
	// 0001) and forwards, so no caller outside World names the coordinator.

	AgentId World::getAgentId(Agent const* agent) const
	{
		return mSimulationCoordinator.getAgentId(agent);
	}

	MovementCommandResult World::moveAgentToMarker(AgentId agent, MarkerId marker,
		std::string_view action)
	{
		if (!lookupMarker(marker)) return { MovementCommandStatus::UnknownMarker };
		if (!actionAvailable(marker, action)) return { MovementCommandStatus::UnavailableAction };
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.moveAgentToMarker(agent, marker, false, action);
	}

	MovementCommandResult World::moveAgentToNamedMarker(AgentId agent, std::string const& name,
		std::string_view action)
	{
		for (auto id : getMarkerIds())
			if (lookupMarker(id)->getName() == name) return moveAgentToMarker(agent, id, action);
		return { MovementCommandStatus::UnknownMarker };
	}

	std::vector<std::string> World::availableAgentActions(MarkerId marker) const
	{
		if (!lookupMarker(marker)) return {};
		auto result = std::vector<std::string>{ std::string(IdleAction) };
		if (actionAvailable(marker, UseFurnitureAction)) result.emplace_back(UseFurnitureAction);
		for (auto const& action : markerActions(marker))
			if (actionAvailable(marker, action) && std::find(result.begin(), result.end(), action) == result.end())
				result.push_back(action);
		return result;
	}

	MovementCommandResult World::inspectBehaviourMoveToMarker(
		AgentId agent, MarkerId marker, std::string_view action) const
	{
		return mSimulationCoordinator.inspectMoveAgentToMarker(agent, marker, true, action);
	}

	MovementCommandResult World::moveBehaviourAgentToMarker(AgentId agent, MarkerId marker,
		std::string_view action)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.moveAgentToMarker(agent, marker, true, action);
	}

	MovementCommandResult World::cancelAgentMovement(AgentId agent)
	{
		return mSimulationCoordinator.cancelAgentMovement(agent);
	}

	MovementCommandResult World::inspectBehaviourMovementCancellation(AgentId agent) const
	{
		return mSimulationCoordinator.inspectCancelAgentMovement(agent, true);
	}

	MovementCommandResult World::cancelBehaviourAgentMovement(AgentId agent)
	{
		return mSimulationCoordinator.cancelAgentMovement(agent, true);
	}

	EntityLookup<Agent> World::lookupAgent(AgentId id)
	{
		return mSimulationCoordinator.lookupAgent(id);
	}

	EntityLookup<Agent const> World::lookupAgent(AgentId id) const
	{
		return mSimulationCoordinator.lookupAgent(id);
	}

	bool World::holdsTraversalOwnership(AgentId id) const
	{
		return mSimulationCoordinator.holdsTraversalOwnership(id);
	}

	bool World::isAgentInQueue(AgentId id) const
	{
		return mSimulationCoordinator.isAgentInQueue(id);
	}

	void World::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.releaseAgentFromResource(resource, id);
	}

	void World::releaseTraversalOwnership(AgentId id)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.releaseTraversalOwnership(id);
	}

	EntityRemovalResult World::removeAgent(AgentId id)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.removeAgent(id);
	}

	// Agent activation is judged and written in SimulationCoordinator, where the
	// rest of the Agent lifecycle lives (ADR 0004); these forward.
	bool World::canSetAgentActive(AgentId id, bool active, string* diagnostic) const
	{
		return mSimulationCoordinator.canSetAgentActive(id, active, diagnostic);
	}

	bool World::setAgentActive(AgentId id, bool active, string* diagnostic)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.setAgentActive(id, active, diagnostic);
	}

	bool World::setAgentIndividualColour(AgentId id, optional<AgentColour> value,
		string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualColour() == value)
		{
			if (diagnostic) *diagnostic = "The individual Agent Colour is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualColour(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualEscalatorWalkingChance(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentEscalatorWalkingChanceIsValid(*value, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualEscalatorWalkingChance() == value)
		{
			if (diagnostic) *diagnostic = "The individual Escalator walking chance is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualEscalatorWalkingChance(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualWalkSpeedModifier(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentWalkSpeedModifierRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualWalkSpeedModifier() == value)
		{
			if (diagnostic) *diagnostic = "The individual Walk speed modifier is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualWalkSpeedModifier(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualHeightModifier(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentHeightModifierRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualHeightModifier() == value)
		{
			if (diagnostic) *diagnostic = "The individual Height modifier is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualHeightModifier(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualStairSpeedModifier(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentStairSpeedModifierRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualStairSpeedModifier() == value)
		{
			if (diagnostic) *diagnostic = "The individual Stair speed modifier is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualStairSpeedModifier(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualLadderSpeedModifier(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentLadderSpeedModifierRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualLadderSpeedModifier() == value)
		{
			if (diagnostic) *diagnostic = "The individual Ladder speed modifier is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualLadderSpeedModifier(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualInteractionAversion(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentInteractionAversionRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualInteractionAversion() == value)
		{
			if (diagnostic) *diagnostic = "The individual Interaction aversion is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualInteractionAversion(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualEffortAversion(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentEffortAversionRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualEffortAversion() == value)
		{
			if (diagnostic) *diagnostic = "The individual Effort aversion is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualEffortAversion(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualWaitingAversion(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentWaitingAversionRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualWaitingAversion() == value)
		{
			if (diagnostic) *diagnostic = "The individual Waiting aversion is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualWaitingAversion(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualCrowdAversion(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentCrowdAversionRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualCrowdAversion() == value)
		{
			if (diagnostic) *diagnostic = "The individual Crowd aversion is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualCrowdAversion(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualRiskAversion(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentRiskAversionRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualRiskAversion() == value)
		{
			if (diagnostic) *diagnostic = "The individual Risk aversion is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualRiskAversion(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualRouteFamiliarity(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentRouteFamiliarityRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualRouteFamiliarity() == value)
		{
			if (diagnostic) *diagnostic = "The individual Route familiarity is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualRouteFamiliarity(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualRoutePersistence(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentRoutePersistenceRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualRoutePersistence() == value)
		{
			if (diagnostic) *diagnostic = "The individual Route persistence is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualRoutePersistence(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualMinimumRoutePlanningTime(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentMinimumRoutePlanningTimeRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualMinimumRoutePlanningTime() == value)
		{
			if (diagnostic) *diagnostic = "The individual Minimum route planning time is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualMinimumRoutePlanningTime(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualMaximumRoutePlanningTime(AgentId id,
		optional<float> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !agentMaximumRoutePlanningTimeRangeIsValid({ *value, *value }, diagnostic)) return false;
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualMaximumRoutePlanningTime() == value)
		{
			if (diagnostic) *diagnostic = "The individual Maximum route planning time is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualMaximumRoutePlanningTime(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualPermissionAdherence(AgentId id,
		optional<bool> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualPermissionAdherence() == value)
		{
			if (diagnostic) *diagnostic = "The individual Permission adherence is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		auto const before = lookup.entity->getEffectivePermissionAdherence().value;
		lookup.entity->setIndividualPermissionAdherence(value);
		auto const after = lookup.entity->getEffectivePermissionAdherence().value;
		if (before != after)
		{
			if (after) replanAgentAfterAuthorizationRefusal(id);
			else beginVoluntaryRoutePlanning(id);
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentIndividualMobilityProfile(AgentId id,
		optional<MobilityProfile> value, string* diagnostic)
	{
		auto lookup = lookupAgent(id);
		if (!lookup) { if (diagnostic) *diagnostic = lookup.diagnostic; return false; }
		if (value && !mobilityProfileIsValid(*value))
		{
			if (diagnostic) *diagnostic = "The individual Mobility profile contains an invalid Mobility use";
			return false;
		}
		if (!mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause the simulation before editing individual Agent properties";
			return false;
		}
		if (lookup.entity->getIndividualMobilityProfile() == value)
		{
			if (diagnostic) *diagnostic = "The individual Mobility profile is unchanged";
			return false;
		}
		invalidateSimulationSnapshot();
		lookup.entity->setIndividualMobilityProfile(value);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// Agent groups are authored World data, not simulation state (ADR 0006).
	// They live in the World's own registry and never reach the coordinator,
	// so creating and renaming need no pause, dirty no topology, and leave every
	// runtime snapshot and simulation event exactly as it was.

	bool World::agentGroupNameTaken(std::string const& trimmed, AgentGroupId except) const
	{
		// Case-sensitive by design: names differing only by case are distinct
		// groups, so "Night shift" and "night shift" may coexist.
		for (auto const& [id, group] : mAgentGroups.entries())
		{
			if (id == except) continue;
			if (group && group->getName() == trimmed) return true;
		}
		return false;
	}

	uint32_t World::getAgentGroupCount() const
	{
		return static_cast<uint32_t>(mAgentGroups.entries().size());
	}

	std::vector<AgentGroupId> World::getAgentGroupIds() const
	{
		// The registry is keyed by the monotonically allocated ID, so this is
		// creation order and a rename cannot disturb it.
		std::vector<AgentGroupId> ids;
		ids.reserve(mAgentGroups.entries().size());
		for (auto const& [id, group] : mAgentGroups.entries())
		{
			(void)group;
			ids.push_back(id);
		}
		return ids;
	}

	EntityLookup<AgentGroup const> World::lookupAgentGroup(AgentGroupId id) const
	{
		EntityLookup<AgentGroup const> lookup;
		auto const* group = mAgentGroups.find(id);
		if (!group)
		{
			lookup.diagnostic = format("Agent group {} is not defined in this World", id.value);
			return lookup;
		}
		lookup.entity = group;
		return lookup;
	}

	std::string const& World::getAgentGroupName(AgentGroupId id) const
	{
		auto const lookup = lookupAgentGroup(id);
		if (!lookup) throw WorldException(this, lookup.diagnostic);
		return lookup.entity->getName();
	}

	bool World::canAddAgentGroup(std::string const& name, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const trimmed = AgentGroup::trimName(name);
		if (!AgentGroup::nameIsValid(trimmed, diagnostic)) return false;
		if (agentGroupNameTaken(trimmed))
		{
			if (diagnostic) *diagnostic = format("The Agent group \"{}\" already exists", trimmed);
			return false;
		}
		// The ID space can be spent. A loaded document may legitimately carry a
		// group at the very top of the range, and the ID past that is zero - the
		// null handle, which is no group at all: it cannot be assigned to an
		// Agent, and a save that wrote one could never be read back (#123).
		if (mAgentGroups.exhausted())
		{
			if (diagnostic) *diagnostic =
				"This World has issued every Agent group ID and cannot create another";
			return false;
		}
		return true;
	}

	AgentGroupId World::addAgentGroup(std::string const& name)
	{
		invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddAgentGroup(name, &diagnostic))
			throw WorldException(this, diagnostic);

		// Nothing above mutates, so the group is created only once its name has
		// passed: a refused add leaves the World exactly as it was found.
		// tryAdd rather than add, because a spent ID space is a refusal the
		// World can report rather than an identity it hands out blind: what
		// comes back from here is always a live, nonzero AgentGroupId.
		auto const id = mAgentGroups.tryAdd(AgentGroup::create(AgentGroup::trimName(name)));
		if (!id)
		{
			throw WorldException(this,
				"This World has issued every Agent group ID and cannot create another");
		}
		modify();
		return *id;
	}

	bool World::canRenameAgentGroup(AgentGroupId id, std::string const& name,
		std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		if (!lookupAgentGroup(id))
		{
			if (diagnostic) *diagnostic = format("Agent group {} is not defined in this World", id.value);
			return false;
		}

		auto const trimmed = AgentGroup::trimName(name);
		if (!AgentGroup::nameIsValid(trimmed, diagnostic)) return false;
		if (agentGroupNameTaken(trimmed, id))
		{
			if (diagnostic) *diagnostic = format("The Agent group \"{}\" already exists", trimmed);
			return false;
		}
		return true;
	}

	bool World::renameAgentGroup(AgentGroupId id, std::string const& name,
		std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (!canRenameAgentGroup(id, name, diagnostic)) return false;

		// Identity and position are untouched: only the name moves, and it moves
		// in place inside the same registry entry.
		auto* group = mAgentGroups.find(id);
		group->setName(AgentGroup::trimName(name));
		modify();
		return true;
	}

	bool World::canSetAgentGroup(AgentId agent, AgentGroupId group, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const agentLookup = lookupAgent(agent);
		if (!agentLookup)
		{
			if (diagnostic) *diagnostic = agentLookup.diagnostic;
			return false;
		}

		// An empty AgentGroupId is the "no Agent group" value rather than a
		// reference to something that may be missing, so clearing an assignment
		// never has to be judged against the group registry.
		if (!group) return true;

		auto const lookup = lookupAgentGroup(group);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		return true;
	}

	bool World::setAgentGroup(AgentId agent, AgentGroupId group, std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// Both halves are judged before a single field is written, so a refusal
		// leaves every Agent and every group exactly as it was found.
		if (!canSetAgentGroup(agent, group, diagnostic)) return false;

		// Only the Agent's own reference moves. The group is not told, and no
		// other Agent is touched: membership is read off the Agent, which is
		// why a rename never needs to rewrite its members.
		auto* target = mAgents.find(agent);
		target->setAgentGroupId(group);
		modify();
		return true;
	}

	AgentGroupId World::getAgentGroup(AgentId agent) const
	{
		auto const lookup = lookupAgent(agent);
		if (!lookup) throw WorldException(this, lookup.diagnostic);
		return lookup.entity->getAgentGroupId();
	}

	bool World::isAgentGroupActive(AgentGroupId group) const
	{
		auto const lookup = lookupAgentGroup(group);
		if (!lookup) throw WorldException(this, lookup.diagnostic);

		// This is an aggregate view of the members' own flags, not state kept by
		// the group. In a mixed group the open eye means that clicking it can
		// deactivate every remaining active member in one operation.
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent && agent->getAgentGroupId() == group && agent->isActive())
				return true;
		}
		return false;
	}

	bool World::canSetAgentGroupActive(AgentGroupId group, bool /* active */,
		std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const lookup = lookupAgentGroup(group);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		if (!mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Agent groups cannot be activated or deactivated while the simulation is running";
			return false;
		}
		return true;
	}

	bool World::setAgentGroupActive(AgentGroupId group, bool active,
		std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// Judge the group and the pause gate before changing the first member, so
		// a refusal can never leave a partly toggled group.
		if (!canSetAgentGroupActive(group, active, diagnostic)) return false;

		for (auto const& [id, agent] : mAgents.entries())
		{
			if (agent && agent->getAgentGroupId() == group)
			{
				// The IDs came from the live registry and the pause gate was checked
				// above, so the coordinator cannot refuse any member now. Keep the
				// actual activation write on its Agent-lifecycle seam (ADR 0004).
				(void)mSimulationCoordinator.setAgentActive(id, active);
			}
		}
		return true;
	}

	bool World::canAssignAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		auto const agentLookup = lookupAgent(agent);
		if (!agentLookup) return reject(agentLookup.diagnostic);
		if (!mSimulationPaused)
			return reject("Pause the simulation before assigning an Agent tag");
		if (!mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");
		if (!tag || !mAgentTagRegistry->lookupAgentTag(tag))
			return reject(format("Agent tag {} is not defined in the attached registry", tag.value));
		if (agentLookup.entity->hasAgentTag(tag))
			return reject(format("Agent '{}' is already assigned to Agent tag #{}",
				agentLookup.entity->getName(), mAgentTagRegistry->getAgentTagName(tag)));

		auto const* assignedDefinition = mAgentTagRegistry->lookupAgentTag(tag);
		for (auto const existing : agentLookup.entity->getAgentTagIds())
		{
			auto const* source = mAgentTagRegistry->lookupAgentTag(existing);
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
			if (assignedDefinition->getWalkSpeedModifier()
				&& source->getWalkSpeedModifier())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Walk speed modifier is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(),
					source->getName()));
			}
			if (assignedDefinition->getHeightModifier() && source->getHeightModifier())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Height modifier is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(),
					source->getName()));
			}
			if (assignedDefinition->getStairSpeedModifier() && source->getStairSpeedModifier())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Stair speed modifier is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getLadderSpeedModifier() && source->getLadderSpeedModifier())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Ladder speed modifier is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getInteractionAversion() && source->getInteractionAversion())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Interaction aversion is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getEffortAversion() && source->getEffortAversion())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Effort aversion is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getWaitingAversion() && source->getWaitingAversion())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Waiting aversion is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getCrowdAversion() && source->getCrowdAversion())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Crowd aversion is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getRiskAversion() && source->getRiskAversion())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Risk aversion is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getRouteFamiliarity() && source->getRouteFamiliarity())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Route familiarity is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getRoutePersistence() && source->getRoutePersistence())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Route persistence is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getMinimumRoutePlanningTime() && source->getMinimumRoutePlanningTime())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Minimum route planning time is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getMaximumRoutePlanningTime() && source->getMaximumRoutePlanningTime())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Maximum route planning time is already inherited from #{}",
					agentLookup.entity->getName(), assignedDefinition->getName(), source->getName()));
			}
			if (assignedDefinition->getPermissionAdherence() && source->getPermissionAdherence())
			{
				return reject(format(
					"Agent '{}' cannot be assigned to #{} because Permission adherence is already inherited from #{}",
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
		return true;
	}

	bool World::assignAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (!canAssignAgentTag(agent, tag, diagnostic)) return false;
		auto* target = mAgents.find(agent);
		auto const* definition = mAgentTagRegistry->lookupAgentTag(tag);
		optional<AgentPropertySample> walkSpeedSample;
		optional<AgentPropertySample> heightSample;
		optional<AgentPropertySample> stairSpeedSample;
		optional<AgentPropertySample> ladderSpeedSample;
		optional<AgentPropertySample> interactionAversionSample;
		optional<AgentPropertySample> effortAversionSample;
		optional<AgentPropertySample> waitingAversionSample;
		optional<AgentPropertySample> crowdAversionSample;
		optional<AgentPropertySample> riskAversionSample;
		optional<AgentPropertySample> routeFamiliaritySample;
		optional<AgentPropertySample> routePersistenceSample;
		optional<AgentPropertySample> minimumRoutePlanningTimeSample;
		optional<AgentPropertySample> maximumRoutePlanningTimeSample;
		if (auto const* property = definition->getWalkSpeedModifier())
		{
			walkSpeedSample = AgentPropertySample{
				SampledAgentPropertyType::WalkSpeedModifier, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getHeightModifier())
		{
			heightSample = AgentPropertySample{
				SampledAgentPropertyType::HeightModifier, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getStairSpeedModifier())
		{
			stairSpeedSample = AgentPropertySample{
				SampledAgentPropertyType::StairSpeedModifier, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getLadderSpeedModifier())
		{
			ladderSpeedSample = AgentPropertySample{
				SampledAgentPropertyType::LadderSpeedModifier, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getInteractionAversion())
		{
			interactionAversionSample = AgentPropertySample{
				SampledAgentPropertyType::InteractionAversion, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getEffortAversion())
		{
			effortAversionSample = AgentPropertySample{
				SampledAgentPropertyType::EffortAversion, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getWaitingAversion())
		{
			waitingAversionSample = AgentPropertySample{
				SampledAgentPropertyType::WaitingAversion, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getCrowdAversion())
		{
			crowdAversionSample = AgentPropertySample{
				SampledAgentPropertyType::CrowdAversion, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getRiskAversion())
		{
			riskAversionSample = AgentPropertySample{
				SampledAgentPropertyType::RiskAversion, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getRouteFamiliarity())
		{
			routeFamiliaritySample = AgentPropertySample{
				SampledAgentPropertyType::RouteFamiliarity, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getRoutePersistence())
		{
			routePersistenceSample = AgentPropertySample{
				SampledAgentPropertyType::RoutePersistence, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getMinimumRoutePlanningTime())
		{
			minimumRoutePlanningTimeSample = AgentPropertySample{
				SampledAgentPropertyType::MinimumRoutePlanningTime, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		if (auto const* property = definition->getMaximumRoutePlanningTime())
		{
			maximumRoutePlanningTimeSample = AgentPropertySample{
				SampledAgentPropertyType::MaximumRoutePlanningTime, tag, property->revision,
				sampleAgentModifier(property->range) };
		}
		auto const adherenceBefore = target->getEffectivePermissionAdherence().value;
		target->assignAgentTag(tag);
		if (walkSpeedSample) target->setWalkSpeedModifierSample(*walkSpeedSample);
		if (heightSample) target->setHeightModifierSample(*heightSample);
		if (stairSpeedSample) target->setStairSpeedModifierSample(*stairSpeedSample);
		if (ladderSpeedSample) target->setLadderSpeedModifierSample(*ladderSpeedSample);
		if (interactionAversionSample) target->setInteractionAversionSample(*interactionAversionSample);
		if (effortAversionSample) target->setEffortAversionSample(*effortAversionSample);
		if (waitingAversionSample) target->setWaitingAversionSample(*waitingAversionSample);
		if (crowdAversionSample) target->setCrowdAversionSample(*crowdAversionSample);
		if (riskAversionSample) target->setRiskAversionSample(*riskAversionSample);
		if (routeFamiliaritySample) target->setRouteFamiliaritySample(*routeFamiliaritySample);
		if (routePersistenceSample) target->setRoutePersistenceSample(*routePersistenceSample);
		if (minimumRoutePlanningTimeSample) target->setMinimumRoutePlanningTimeSample(*minimumRoutePlanningTimeSample);
		if (maximumRoutePlanningTimeSample) target->setMaximumRoutePlanningTimeSample(*maximumRoutePlanningTimeSample);
		auto const adherenceAfter = target->getEffectivePermissionAdherence().value;
		if (adherenceBefore != adherenceAfter)
		{
			if (adherenceAfter) replanAgentAfterAuthorizationRefusal(agent);
			else beginVoluntaryRoutePlanning(agent);
		}
		modify();
		return true;
	}

	bool World::canRemoveAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		auto const agentLookup = lookupAgent(agent);
		if (!agentLookup) return reject(agentLookup.diagnostic);
		if (!mSimulationPaused)
			return reject("Pause the simulation before removing an Agent tag");
		if (!mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");
		if (!tag || !mAgentTagRegistry->lookupAgentTag(tag))
			return reject(format("Agent tag {} is not defined in the attached registry", tag.value));
		if (!agentLookup.entity->hasAgentTag(tag))
			return reject(format("Agent '{}' is not assigned to Agent tag #{}",
				agentLookup.entity->getName(), mAgentTagRegistry->getAgentTagName(tag)));
		return true;
	}

	bool World::removeAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (!canRemoveAgentTag(agent, tag, diagnostic)) return false;
		auto* target = mAgents.find(agent);
		auto const adherenceBefore = target->getEffectivePermissionAdherence().value;
		target->removeAgentTag(tag);
		if (target->getWalkSpeedModifierSample()
			&& target->getWalkSpeedModifierSample()->sourceTag == tag)
			target->clearWalkSpeedModifierSample();
		if (target->getHeightModifierSample()
			&& target->getHeightModifierSample()->sourceTag == tag)
			target->clearHeightModifierSample();
		if (target->getStairSpeedModifierSample()
			&& target->getStairSpeedModifierSample()->sourceTag == tag)
			target->clearStairSpeedModifierSample();
		if (target->getLadderSpeedModifierSample()
			&& target->getLadderSpeedModifierSample()->sourceTag == tag)
			target->clearLadderSpeedModifierSample();
		if (target->getInteractionAversionSample()
			&& target->getInteractionAversionSample()->sourceTag == tag)
			target->clearInteractionAversionSample();
		if (target->getEffortAversionSample()
			&& target->getEffortAversionSample()->sourceTag == tag)
			target->clearEffortAversionSample();
		if (target->getWaitingAversionSample()
			&& target->getWaitingAversionSample()->sourceTag == tag)
			target->clearWaitingAversionSample();
		if (target->getCrowdAversionSample()
			&& target->getCrowdAversionSample()->sourceTag == tag)
			target->clearCrowdAversionSample();
		if (target->getRiskAversionSample()
			&& target->getRiskAversionSample()->sourceTag == tag)
			target->clearRiskAversionSample();
		if (target->getRouteFamiliaritySample()
			&& target->getRouteFamiliaritySample()->sourceTag == tag)
			target->clearRouteFamiliaritySample();
		if (target->getRoutePersistenceSample()
			&& target->getRoutePersistenceSample()->sourceTag == tag)
			target->clearRoutePersistenceSample();
		if (target->getMinimumRoutePlanningTimeSample()
			&& target->getMinimumRoutePlanningTimeSample()->sourceTag == tag)
			target->clearMinimumRoutePlanningTimeSample();
		if (target->getMaximumRoutePlanningTimeSample()
			&& target->getMaximumRoutePlanningTimeSample()->sourceTag == tag)
			target->clearMaximumRoutePlanningTimeSample();
		auto const adherenceAfter = target->getEffectivePermissionAdherence().value;
		if (adherenceBefore != adherenceAfter)
		{
			if (adherenceAfter) replanAgentAfterAuthorizationRefusal(agent);
			else beginVoluntaryRoutePlanning(agent);
		}
		modify();
		return true;
	}

	bool World::validateAgentTagAssignments(set<AgentTagId> const& tags,
		optional<AgentPropertySample> const& walkSpeedSample,
		optional<AgentPropertySample> const& heightSample,
		optional<AgentPropertySample> const& stairSpeedSample,
		optional<AgentPropertySample> const& ladderSpeedSample,
		optional<AgentPropertySample> const& interactionAversionSample,
		optional<AgentPropertySample> const& effortAversionSample,
		optional<AgentPropertySample> const& waitingAversionSample,
		optional<AgentPropertySample> const& crowdAversionSample,
		optional<AgentPropertySample> const& riskAversionSample,
		optional<AgentPropertySample> const& routeFamiliaritySample,
		optional<AgentPropertySample> const& routePersistenceSample,
		optional<AgentPropertySample> const& minimumRoutePlanningTimeSample,
		optional<AgentPropertySample> const& maximumRoutePlanningTimeSample,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (tags.empty())
		{
			if (walkSpeedSample || heightSample || stairSpeedSample || ladderSpeedSample
				|| interactionAversionSample || effortAversionSample || waitingAversionSample
				|| crowdAversionSample || riskAversionSample || routeFamiliaritySample
				|| routePersistenceSample || minimumRoutePlanningTimeSample || maximumRoutePlanningTimeSample)
				return reject("An untagged Agent cannot carry modifier samples");
			return true;
		}
		if (!mAgentTagRegistry)
			return reject("This World has no attached Agent tag registry");

		AgentTagId escalatorWalkingChanceSource{};
		AgentTagId colourSource{};
		AgentTagId walkSpeedSource{};
		AgentTagId heightSource{};
		AgentTagId stairSpeedSource{};
		AgentTagId ladderSpeedSource{};
		AgentTagId interactionAversionSource{};
		AgentTagId effortAversionSource{};
		AgentTagId waitingAversionSource{};
		AgentTagId crowdAversionSource{};
		AgentTagId riskAversionSource{};
		AgentTagId routeFamiliaritySource{};
		AgentTagId routePersistenceSource{};
		AgentTagId minimumRoutePlanningTimeSource{};
		AgentTagId maximumRoutePlanningTimeSource{};
		AgentTagId permissionAdherenceSource{};
		AgentTagId mobilityProfileSource{};
		AgentWalkSpeedModifierProperty const* walkSpeedProperty{ nullptr };
		AgentHeightModifierProperty const* heightProperty{ nullptr };
		AgentStairSpeedModifierProperty const* stairSpeedProperty{ nullptr };
		AgentLadderSpeedModifierProperty const* ladderSpeedProperty{ nullptr };
		AgentInteractionAversionProperty const* interactionAversionProperty{ nullptr };
		AgentEffortAversionProperty const* effortAversionProperty{ nullptr };
		AgentWaitingAversionProperty const* waitingAversionProperty{ nullptr };
		AgentCrowdAversionProperty const* crowdAversionProperty{ nullptr };
		AgentRiskAversionProperty const* riskAversionProperty{ nullptr };
		AgentRouteFamiliarityProperty const* routeFamiliarityProperty{ nullptr };
		AgentRoutePersistenceProperty const* routePersistenceProperty{ nullptr };
		AgentMinimumRoutePlanningTimeProperty const* minimumRoutePlanningTimeProperty{ nullptr };
		AgentMaximumRoutePlanningTimeProperty const* maximumRoutePlanningTimeProperty{ nullptr };
		for (auto const tag : tags)
		{
			auto const* definition = mAgentTagRegistry->lookupAgentTag(tag);
			if (!definition)
				return reject(format("Agent tag {} is not defined in the attached registry",
					tag.value));
			if (definition->getColour())
			{
				if (colourSource)
					return reject(format("Colour is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(colourSource), definition->getName()));
				colourSource = tag;
			}
			if (definition->getEscalatorWalkingChance())
			{
				if (escalatorWalkingChanceSource)
					return reject(format("Escalator walking chance is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(escalatorWalkingChanceSource), definition->getName()));
				escalatorWalkingChanceSource = tag;
			}
			if (auto const* property = definition->getWalkSpeedModifier())
			{
				if (walkSpeedSource)
					return reject(format(
						"Walk speed modifier is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(walkSpeedSource),
						definition->getName()));
				walkSpeedSource = tag;
				walkSpeedProperty = property;
			}
			if (auto const* property = definition->getHeightModifier())
			{
				if (heightSource)
					return reject(format("Height modifier is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(heightSource), definition->getName()));
				heightSource = tag;
				heightProperty = property;
			}
			if (auto const* property = definition->getStairSpeedModifier())
			{
				if (stairSpeedSource)
					return reject(format("Stair speed modifier is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(stairSpeedSource), definition->getName()));
				stairSpeedSource = tag;
				stairSpeedProperty = property;
			}
			if (auto const* property = definition->getLadderSpeedModifier())
			{
				if (ladderSpeedSource)
					return reject(format("Ladder speed modifier is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(ladderSpeedSource), definition->getName()));
				ladderSpeedSource = tag;
				ladderSpeedProperty = property;
			}
			if (auto const* property = definition->getInteractionAversion())
			{
				if (interactionAversionSource)
					return reject(format("Interaction aversion is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(interactionAversionSource), definition->getName()));
				interactionAversionSource = tag;
				interactionAversionProperty = property;
			}
			if (auto const* property = definition->getEffortAversion())
			{
				if (effortAversionSource)
					return reject(format("Effort aversion is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(effortAversionSource), definition->getName()));
				effortAversionSource = tag;
				effortAversionProperty = property;
			}
			if (auto const* property = definition->getWaitingAversion())
			{
				if (waitingAversionSource)
					return reject(format("Waiting aversion is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(waitingAversionSource), definition->getName()));
				waitingAversionSource = tag;
				waitingAversionProperty = property;
			}
			if (auto const* property = definition->getCrowdAversion())
			{
				if (crowdAversionSource)
					return reject(format("Crowd aversion is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(crowdAversionSource), definition->getName()));
				crowdAversionSource = tag;
				crowdAversionProperty = property;
			}
			if (auto const* property = definition->getRiskAversion())
			{
				if (riskAversionSource)
					return reject(format("Risk aversion is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(riskAversionSource), definition->getName()));
				riskAversionSource = tag;
				riskAversionProperty = property;
			}
			if (auto const* property = definition->getRouteFamiliarity())
			{
				if (routeFamiliaritySource)
					return reject(format("Route familiarity is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(routeFamiliaritySource), definition->getName()));
				routeFamiliaritySource = tag;
				routeFamiliarityProperty = property;
			}
			if (auto const* property = definition->getRoutePersistence())
			{
				if (routePersistenceSource)
					return reject(format("Route persistence is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(routePersistenceSource), definition->getName()));
				routePersistenceSource = tag;
				routePersistenceProperty = property;
			}
			if (auto const* property = definition->getMinimumRoutePlanningTime())
			{
				if (minimumRoutePlanningTimeSource)
					return reject(format("Minimum route planning time is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(minimumRoutePlanningTimeSource), definition->getName()));
				minimumRoutePlanningTimeSource = tag;
				minimumRoutePlanningTimeProperty = property;
			}
			if (auto const* property = definition->getMaximumRoutePlanningTime())
			{
				if (maximumRoutePlanningTimeSource)
					return reject(format("Maximum route planning time is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(maximumRoutePlanningTimeSource), definition->getName()));
				maximumRoutePlanningTimeSource = tag;
				maximumRoutePlanningTimeProperty = property;
			}
			if (definition->getPermissionAdherence())
			{
				if (permissionAdherenceSource)
					return reject(format("Permission adherence is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(permissionAdherenceSource), definition->getName()));
				permissionAdherenceSource = tag;
			}
			if (definition->getMobilityProfile())
			{
				if (mobilityProfileSource)
					return reject(format("Mobility profile is inherited from both #{} and #{}",
						mAgentTagRegistry->getAgentTagName(mobilityProfileSource), definition->getName()));
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
					mAgentTagRegistry->getAgentTagName(source)));
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

		return validateSample("Walk speed modifier",
			SampledAgentPropertyType::WalkSpeedModifier, walkSpeedSource,
			walkSpeedProperty ? &walkSpeedProperty->range : nullptr,
			walkSpeedProperty ? walkSpeedProperty->revision : 0, walkSpeedSample)
			&& validateSample("Height modifier", SampledAgentPropertyType::HeightModifier,
				heightSource, heightProperty ? &heightProperty->range : nullptr,
				heightProperty ? heightProperty->revision : 0, heightSample)
			&& validateSample("Stair speed modifier", SampledAgentPropertyType::StairSpeedModifier,
				stairSpeedSource, stairSpeedProperty ? &stairSpeedProperty->range : nullptr,
				stairSpeedProperty ? stairSpeedProperty->revision : 0, stairSpeedSample)
			&& validateSample("Ladder speed modifier", SampledAgentPropertyType::LadderSpeedModifier,
				ladderSpeedSource, ladderSpeedProperty ? &ladderSpeedProperty->range : nullptr,
				ladderSpeedProperty ? ladderSpeedProperty->revision : 0, ladderSpeedSample)
			&& validateSample("Interaction aversion", SampledAgentPropertyType::InteractionAversion,
				interactionAversionSource,
				interactionAversionProperty ? &interactionAversionProperty->range : nullptr,
				interactionAversionProperty ? interactionAversionProperty->revision : 0,
				interactionAversionSample)
			&& validateSample("Effort aversion", SampledAgentPropertyType::EffortAversion,
				effortAversionSource,
				effortAversionProperty ? &effortAversionProperty->range : nullptr,
				effortAversionProperty ? effortAversionProperty->revision : 0,
				effortAversionSample)
			&& validateSample("Waiting aversion", SampledAgentPropertyType::WaitingAversion,
				waitingAversionSource,
				waitingAversionProperty ? &waitingAversionProperty->range : nullptr,
				waitingAversionProperty ? waitingAversionProperty->revision : 0,
				waitingAversionSample)
			&& validateSample("Crowd aversion", SampledAgentPropertyType::CrowdAversion,
				crowdAversionSource,
				crowdAversionProperty ? &crowdAversionProperty->range : nullptr,
				crowdAversionProperty ? crowdAversionProperty->revision : 0,
				crowdAversionSample)
			&& validateSample("Risk aversion", SampledAgentPropertyType::RiskAversion,
				riskAversionSource,
				riskAversionProperty ? &riskAversionProperty->range : nullptr,
				riskAversionProperty ? riskAversionProperty->revision : 0,
				riskAversionSample)
			&& validateSample("Route familiarity", SampledAgentPropertyType::RouteFamiliarity,
				routeFamiliaritySource,
				routeFamiliarityProperty ? &routeFamiliarityProperty->range : nullptr,
				routeFamiliarityProperty ? routeFamiliarityProperty->revision : 0,
				routeFamiliaritySample)
			&& validateSample("Route persistence", SampledAgentPropertyType::RoutePersistence,
				routePersistenceSource,
				routePersistenceProperty ? &routePersistenceProperty->range : nullptr,
				routePersistenceProperty ? routePersistenceProperty->revision : 0,
				routePersistenceSample)
			&& validateSample("Minimum route planning time", SampledAgentPropertyType::MinimumRoutePlanningTime,
				minimumRoutePlanningTimeSource,
				minimumRoutePlanningTimeProperty ? &minimumRoutePlanningTimeProperty->range : nullptr,
				minimumRoutePlanningTimeProperty ? minimumRoutePlanningTimeProperty->revision : 0,
				minimumRoutePlanningTimeSample)
			&& validateSample("Maximum route planning time", SampledAgentPropertyType::MaximumRoutePlanningTime,
				maximumRoutePlanningTimeSource,
				maximumRoutePlanningTimeProperty ? &maximumRoutePlanningTimeProperty->range : nullptr,
				maximumRoutePlanningTimeProperty ? maximumRoutePlanningTimeProperty->revision : 0,
				maximumRoutePlanningTimeSample);
	}

	bool World::restoreAgentTagAssignments(AgentId agent,
		set<AgentTagId> const& tags,
		optional<AgentPropertySample> const& walkSpeedSample,
		optional<AgentPropertySample> const& heightSample,
		optional<AgentPropertySample> const& stairSpeedSample,
		optional<AgentPropertySample> const& ladderSpeedSample,
		optional<AgentPropertySample> const& interactionAversionSample,
		optional<AgentPropertySample> const& effortAversionSample,
		optional<AgentPropertySample> const& waitingAversionSample,
		optional<AgentPropertySample> const& crowdAversionSample,
		optional<AgentPropertySample> const& riskAversionSample,
		optional<AgentPropertySample> const& routeFamiliaritySample,
		optional<AgentPropertySample> const& routePersistenceSample,
		optional<AgentPropertySample> const& minimumRoutePlanningTimeSample,
		optional<AgentPropertySample> const& maximumRoutePlanningTimeSample,
		string* diagnostic)
	{
		invalidateSimulationSnapshot();
		if (diagnostic) diagnostic->clear();
		auto const lookup = lookupAgent(agent);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		if (!mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Pause the simulation before restoring Agent tag assignments";
			return false;
		}
		if (!validateAgentTagAssignments(tags, walkSpeedSample, heightSample,
			stairSpeedSample, ladderSpeedSample, interactionAversionSample, effortAversionSample,
			waitingAversionSample, crowdAversionSample, riskAversionSample,
			routeFamiliaritySample, routePersistenceSample, minimumRoutePlanningTimeSample, maximumRoutePlanningTimeSample, diagnostic))
			return false;

		auto* target = mAgents.find(agent);
		if (target->getAgentTagIds() == tags
			&& target->getWalkSpeedModifierSample() == walkSpeedSample
			&& target->getHeightModifierSample() == heightSample
			&& target->getStairSpeedModifierSample() == stairSpeedSample
			&& target->getLadderSpeedModifierSample() == ladderSpeedSample
			&& target->getInteractionAversionSample() == interactionAversionSample
			&& target->getEffortAversionSample() == effortAversionSample
			&& target->getWaitingAversionSample() == waitingAversionSample
			&& target->getCrowdAversionSample() == crowdAversionSample
			&& target->getRiskAversionSample() == riskAversionSample
			&& target->getRouteFamiliaritySample() == routeFamiliaritySample
			&& target->getRoutePersistenceSample() == routePersistenceSample
			&& target->getMinimumRoutePlanningTimeSample() == minimumRoutePlanningTimeSample
			&& target->getMaximumRoutePlanningTimeSample() == maximumRoutePlanningTimeSample) return true;

		target->setAgentTags(tags);
		if (walkSpeedSample) target->setWalkSpeedModifierSample(*walkSpeedSample);
		else target->clearWalkSpeedModifierSample();
		if (heightSample) target->setHeightModifierSample(*heightSample);
		else target->clearHeightModifierSample();
		if (stairSpeedSample) target->setStairSpeedModifierSample(*stairSpeedSample);
		else target->clearStairSpeedModifierSample();
		if (ladderSpeedSample) target->setLadderSpeedModifierSample(*ladderSpeedSample);
		else target->clearLadderSpeedModifierSample();
		if (interactionAversionSample) target->setInteractionAversionSample(*interactionAversionSample);
		else target->clearInteractionAversionSample();
		if (effortAversionSample) target->setEffortAversionSample(*effortAversionSample);
		else target->clearEffortAversionSample();
		if (waitingAversionSample) target->setWaitingAversionSample(*waitingAversionSample);
		else target->clearWaitingAversionSample();
		if (crowdAversionSample) target->setCrowdAversionSample(*crowdAversionSample);
		else target->clearCrowdAversionSample();
		if (riskAversionSample) target->setRiskAversionSample(*riskAversionSample);
		else target->clearRiskAversionSample();
		if (routeFamiliaritySample) target->setRouteFamiliaritySample(*routeFamiliaritySample);
		else target->clearRouteFamiliaritySample();
		if (routePersistenceSample) target->setRoutePersistenceSample(*routePersistenceSample);
		else target->clearRoutePersistenceSample();
		if (minimumRoutePlanningTimeSample) target->setMinimumRoutePlanningTimeSample(*minimumRoutePlanningTimeSample);
		else target->clearMinimumRoutePlanningTimeSample();
		if (maximumRoutePlanningTimeSample) target->setMaximumRoutePlanningTimeSample(*maximumRoutePlanningTimeSample);
		else target->clearMaximumRoutePlanningTimeSample();
		modify();
		return true;
	}

	set<AgentTagId> const& World::getAgentTags(AgentId agent) const
	{
		auto const lookup = lookupAgent(agent);
		if (!lookup) throw WorldException(this, lookup.diagnostic);
		return lookup.entity->getAgentTagIds();
	}

	uint32_t World::getAgentGroupMemberCount(AgentGroupId id) const
	{
		// Judged the way every other group query judges its ID: counting a
		// group this World never issued is an error, not a zero that could
		// be mistaken for a group that happens to be empty.
		auto const lookup = lookupAgentGroup(id);
		if (!lookup) throw WorldException(this, lookup.diagnostic);

		// The Agents are the membership record, so the count reads them off
		// the Agent registry rather than off the group. Nothing is cached on
		// the group and nothing is synchronised by hand: every assignment, and
		// every removal, is reflected the next time this is asked.
		//
		// The scan is over the World's whole Agent registry on purpose.
		// Where an Agent sits - which Layer, which Sector, which path, whether
		// it is idle, walking, waiting at a door or riding a lift - is not
		// part of the question, and a count that walked the spatial index
		// instead would answer a different one.
		uint32_t count{ 0 };
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (agent && agent->getAgentGroupId() == id) ++count;
		}
		return count;
	}

	bool World::canDeleteAgentGroup(AgentGroupId id, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		// An empty AgentGroupId names no group at all, so there is nothing for
		// the request to delete. Refusing it keeps "deleted" meaning something
		// this World actually did rather than a no-op that reports success.
		if (!id)
		{
			if (diagnostic) *diagnostic = "No Agent group was given to delete";
			return false;
		}

		// Judged the way every other group query judges its ID: deleting a
		// group this World never issued is an error, not a success that
		// quietly matched nothing.
		auto const lookup = lookupAgentGroup(id);
		if (!lookup)
		{
			if (diagnostic) *diagnostic = lookup.diagnostic;
			return false;
		}
		return true;
	}

	bool World::deleteAgentGroup(AgentGroupId id, std::string* diagnostic)
	{
		invalidateSimulationSnapshot();
		// The whole operation is judged before a single field is written, so a
		// refusal leaves the World exactly as it was found: no group gone,
		// no assignment cleared, no half-deletion for a save to write down.
		if (!canDeleteAgentGroup(id, diagnostic)) return false;

		// The assignments go first, through the same field setAgentGroup()
		// writes, and they all go before the group does. That ordering is what
		// makes the deletion atomic from an Agent's point of view: at no point
		// does this World hold an Agent carrying an AgentGroupId it cannot
		// resolve, which is the dangling state the file format refuses.
		//
		// The scan covers the whole Agent registry for the same reason the
		// count does. A member that is walking, waiting at a Door, riding a
		// lift, or idle on the back-most Layer is still a member, and every
		// one of them has to come back to no Agent group.
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || agent->getAgentGroupId() != id) continue;
			agent->setAgentGroupId(AgentGroupId{});
		}

		// Only now can the group itself go. The registry keeps its next-ID
		// counter, so the deleted AgentGroupId is never handed to a later
		// group: an old reference can never silently come back pointing at
		// something new, and an undo snapshot that still names the ID brings
		// back the same identity, in the same creation-order position.
		mAgentGroups.remove(id);
		modify();
		return true;
	}

	// Interaction and device-operation orchestration - the InteractionPoint and
	// InteractionRequest lifecycles, the DeviceOperation lifecycle, pressing
	// physical controls, and the per-tick interaction phases - lives in
	// SimulationCoordinator (ADR 0004). World keeps the registries
	// (ADR 0001) and forwards every entry point, so no caller outside World
	// names the coordinator.

	bool World::accessPermissionNameTaken(string const& name, AccessPermissionId except) const
	{
		for (size_t i = 0; i < mAccessPermissions.size(); ++i)
			if (mAccessPermissions[i] && i + 1 != except.value
				&& mAccessPermissions[i]->getName() == name) return true;
		return false;
	}

	uint32_t World::getAccessPermissionCount() const
	{
		return static_cast<uint32_t>(count_if(mAccessPermissions.begin(), mAccessPermissions.end(),
			[](auto const& value) { return value != nullptr; }));
	}

	vector<AccessPermissionId> World::getAccessPermissionIds() const
	{
		vector<AccessPermissionId> result;
		for (size_t i = 0; i < mAccessPermissions.size(); ++i)
			if (mAccessPermissions[i]) result.push_back(AccessPermissionId{ i + 1 });
		return result;
	}

	EntityLookup<AccessPermission const> World::lookupAccessPermission(AccessPermissionId id) const
	{
		if (id.value && id.value <= mAccessPermissions.size() && mAccessPermissions[id.value - 1])
			return { mAccessPermissions[id.value - 1].get(), {} };
		return { nullptr, format("Access permission {} is invalid or has been deleted", id.value) };
	}

	string const& World::getAccessPermissionName(AccessPermissionId id) const
	{
		auto found = lookupAccessPermission(id);
		if (!found) throw invalid_argument(found.diagnostic);
		return found.entity->getName();
	}

	bool World::canAddAccessPermission(string const& raw, string* diagnostic) const
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Access permissions can only be edited while the simulation is paused");
		auto name = AccessPermission::trimName(raw);
		if (!AccessPermission::nameIsValid(name, diagnostic)) return false;
		if (accessPermissionNameTaken(name)) return reject(format("Access permission '{}' already exists", name));
		if (getAccessPermissionCount() == AccessPermission::Capacity)
			return reject("A World cannot define more than 256 Access permissions");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AccessPermissionId World::addAccessPermission(string const& raw)
	{
		string diagnostic;
		if (!canAddAccessPermission(raw, &diagnostic)) throw invalid_argument(diagnostic);
		for (size_t i = 0; i < mAccessPermissions.size(); ++i) if (!mAccessPermissions[i])
		{
			mAccessPermissions[i] = AccessPermission::create(AccessPermission::trimName(raw));
			modify();
			return AccessPermissionId{ i + 1 };
		}
		throw invalid_argument("A World cannot define more than 256 Access permissions");
	}

	bool World::renameAccessPermission(AccessPermissionId id, string const& raw, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Access permissions can only be edited while the simulation is paused");
		auto found = lookupAccessPermission(id);
		if (!found) return reject(found.diagnostic);
		auto name = AccessPermission::trimName(raw);
		if (!AccessPermission::nameIsValid(name, diagnostic)) return false;
		if (accessPermissionNameTaken(name, id)) return reject(format("Access permission '{}' already exists", name));
		if (name == found.entity->getName()) { if (diagnostic) diagnostic->clear(); return true; }
		mAccessPermissions[id.value - 1]->setName(std::move(name));
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::isLocationPermissionEligible(uint32_t sectorIndex) const
	{
		return sectorIndex < mSectors.size()
			&& mSectors[sectorIndex]->getType() == SectorType::Location;
	}

	World::ConstructionRecord const* World::findLocationPermissionRecord(uint32_t sectorIndex) const
	{
		uint32_t index = 0;
		for (auto const& record : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(record.type)) continue;
			if (index++ == sectorIndex)
				return record.type == ConstructionType::Room || record.type == ConstructionType::Corridor
					? &record : nullptr;
		}
		return nullptr;
	}

	vector<AccessPermissionId> World::getLocationPermissionRequirement(uint32_t sectorIndex) const
	{
		if (sectorIndex >= mSectors.size()) throw invalid_argument("The selected Location does not exist");
		if (!isLocationPermissionEligible(sectorIndex))
			throw invalid_argument("Location permission requirements are supported only by Rooms and Corridors");
		vector<AccessPermissionId> result;
		auto const& requirement = static_cast<Location const&>(*mSectors[sectorIndex]).getPermissionRequirement();
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (requirement.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool World::setLocationPermissionRequirement(uint32_t sectorIndex,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Location permissions can only be edited while the simulation is paused");
		if (sectorIndex >= mSectors.size()) return reject("The selected Location does not exist");
		if (!isLocationPermissionEligible(sectorIndex))
			return reject("Location permission requirements are supported only by Rooms and Corridors");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			if (next.test(permission.value - 1)) return reject("Duplicate Access permission in Location requirement");
			next.set(permission.value - 1);
		}
		auto record = findLocationPermissionRecord(sectorIndex);
		if (!record) return reject("The selected Location has no authored definition");
		auto& location = static_cast<Location&>(*mSectors[sectorIndex]);
		if (location.mPermissionRequirement != next)
		{
			auto& authored = mConstructionRecords[record - mConstructionRecords.data()].locationPermissionRequirement;
			authored.clear();
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (next.test(bit)) authored.push_back(static_cast<uint32_t>(bit + 1));
			location.mPermissionRequirement = next;
			for (auto const& [id, agent] : mAgents.entries())
				if (!canAgentAccessLocation(location, *agent) && agentPathEntersLocation(*agent, location))
					replanAgentAfterAuthorizationRefusal(id);
			modify();
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::canAgentAccessLocation(Sector const& sector, Agent const& agent) const
	{
		if (sector.getType() != SectorType::Location) return true;
		auto const& requirement = static_cast<Location const&>(sector).getPermissionRequirement();
		return requirement.none() || (requirement & ~effectiveAccessGrants(agent)).none();
	}

	World::ConstructionRecord const* World::findLiftDestinationRecord(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		if (objectIndex != ~0u)
		{
			if (sectorIndex >= mSectors.size() || objectIndex >= mSectors[sectorIndex]->getNumObjects()) return nullptr;
			auto object = dynamic_pointer_cast<const LiftSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
			if (!object) return nullptr;
			for (auto const& record : mConstructionRecords)
				if (record.type == ConstructionType::PlatformLift && record.a == sectorIndex
					&& record.c + mSectors[sectorIndex]->getCellX() == object->getCellX()
					&& record.b + mSectors[sectorIndex]->getCellY() == object->getCellY()) return &record;
			return nullptr;
		}
		uint32_t index = 0;
		for (auto const& record : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(record.type)) continue;
			if (index++ == sectorIndex)
				return record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle ? &record : nullptr;
		}
		return nullptr;
	}

	World::ConstructionRecord const* World::findLiftDestinationRecord(TraversalResource const& resource) const
	{
		auto sector = static_cast<uint32_t>(resource.mLiftSector.value - 1);
		if (!resource.mOpenPlatformLift) return findLiftDestinationRecord(sector);
		if (sector >= mSectors.size()) return nullptr;
		for (uint32_t i = 0; i < mSectors[sector]->getNumObjects(); ++i)
		{
			auto object = dynamic_pointer_cast<const LiftSectorObject>(mSectors[sector]->getObject(i));
			if (object && object->getLift() == resource.mLift) return findLiftDestinationRecord(sector, i);
		}
		return nullptr;
	}

	vector<uint32_t> World::getLiftDestinationLevels(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		auto record = findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record) throw invalid_argument("Unknown Lift, Platform lift, or Shuttle");
		auto levels = record->values;
		for (auto& level : levels) level += record->type == ConstructionType::Lift
			? record->a : record->type == ConstructionType::Shuttle ? record->b
			: mSectors[sectorIndex]->getCellY() + record->b;
		return levels;
	}

	vector<AccessPermissionId> World::getLiftDestinationPermissionRequirement(
		uint32_t sectorIndex, uint32_t stopIndex, uint32_t objectIndex) const
	{
		auto record = findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) throw invalid_argument("Unknown transport destination Stop");
		vector<AccessPermissionId> result;
		if (stopIndex < record->destinationPermissionRequirements.size())
			for (auto id : record->destinationPermissionRequirements[stopIndex]) result.push_back(AccessPermissionId{ id });
		return result;
	}

	bool World::setLiftDestinationPermissionRequirement(uint32_t sectorIndex, uint32_t stopIndex,
		vector<AccessPermissionId> const& permissions, string* diagnostic, uint32_t objectIndex)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Destination permissions can only be edited while the simulation is paused");
		auto record = findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) return reject("Unknown transport destination Stop");
		vector<uint32_t> next;
		for (auto id : permissions)
		{
			auto found = lookupAccessPermission(id);
			if (!found) return reject(found.diagnostic);
			if (find(next.begin(), next.end(), id.value) != next.end()) return reject("Duplicate Access permission");
			next.push_back(static_cast<uint32_t>(id.value));
		}
		sort(next.begin(), next.end());
		auto& requirements = mConstructionRecords[record - mConstructionRecords.data()].destinationPermissionRequirements;
		if (requirements.empty() && next.empty()) { if (diagnostic) diagnostic->clear(); return true; }
		if (requirements.empty()) requirements.resize(record->values.size());
		if (requirements[stopIndex] != next)
		{
			auto previous = requirements[stopIndex];
			requirements[stopIndex] = std::move(next);
			for (auto const& [agentId, agent] : mAgents.entries())
			{
				for (auto id : requirements[stopIndex])
					if (find(previous.begin(), previous.end(), id) == previous.end()
						&& !effectiveAccessGrants(*agent).test(id - 1))
						reconsiderAgentAuthorizationPath(*agent, AccessPermissionId{ id }, false);
				for (auto id : previous)
					if (find(requirements[stopIndex].begin(), requirements[stopIndex].end(), id)
						== requirements[stopIndex].end())
						beginVoluntaryRoutePlanning(agentId);
			}
			modify();
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::AccessPermissionUsage World::getAccessPermissionUsage(AccessPermissionId id) const
	{
		if (!lookupAccessPermission(id)) throw invalid_argument("Unknown Access permission");
		AccessPermissionUsage usage;
		auto bit = id.value - 1;
		for (auto const& [agentId, agent] : mAgents.entries())
		{ (void)agentId; if (agent->mDirectAccessGrants.test(bit)) ++usage.directAgentGrants; }
		for (auto const& [setId, permissionSet] : mPermissionSets.entries())
		{ (void)setId; if (permissionSet->mPermissions.test(bit)) ++usage.permissionSetMemberships; }
		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{ (void)pointId; if (point->mPermissionRequirement.test(bit)) ++usage.interactionPointRequirements; }
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mDoor && resource->mDoor->mPermissionRequirement.test(bit))
				++usage.manualDoorRequirements;
		}
		// Count current owners, rather than replay metadata or a cached counter.
		for (auto const& sector : mSectors)
			if (sector->getType() == SectorType::Location
				&& static_cast<Location const&>(*sector).getPermissionRequirement().test(bit))
				++usage.locationRequirements;
		for (auto const& record : mConstructionRecords)
		{
			for (auto const& requirement : record.destinationPermissionRequirements)
				if (find(requirement.begin(), requirement.end(), id.value) != requirement.end())
					++usage.liftDestinationRequirements;
		}
		return usage;
	}

	bool World::deleteAccessPermission(AccessPermissionId id, string* diagnostic)
	{
		if (!mSimulationPaused)
		{ if (diagnostic) *diagnostic = "Access permissions can only be edited while the simulation is paused"; return false; }
		auto found = lookupAccessPermission(id);
		if (!found) { if (diagnostic) *diagnostic = found.diagnostic; return false; }
		auto bit = id.value - 1;
		bool destinationRoutesChanged = getAccessPermissionUsage(id).liftDestinationRequirements != 0;
		// Clear all references before releasing the slot, so a reused identity can
		// never inherit an old grant or requirement.
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			agent->mDirectAccessGrants.reset(bit);
			agent->mRuntimeDirectGrantAdditions.reset(bit);
			agent->mRuntimeDirectGrantRemovals.reset(bit);
		}
		for (auto const& [setId, permissionSet] : mPermissionSets.entries())
		{ (void)setId; permissionSet->mPermissions.reset(bit); }
		for (auto const& [pointId, point] : mInteractionPoints.entries()) { (void)pointId; point->mPermissionRequirement.reset(bit); }
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mDoor) resource->mDoor->mPermissionRequirement.reset(bit);
		}
		for (auto const& sector : mSectors)
			if (sector->getType() == SectorType::Location)
				static_cast<Location&>(*sector).mPermissionRequirement.reset(bit);
		for (auto& record : mConstructionRecords)
		{
			auto& locationRequirement = record.locationPermissionRequirement;
			locationRequirement.erase(remove(locationRequirement.begin(), locationRequirement.end(), id.value), locationRequirement.end());
			if (record.type == ConstructionType::Door)
				record.values.erase(remove(record.values.begin(), record.values.end(), id.value),
					record.values.end());
			for (auto& requirement : record.controlPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
			for (auto& requirement : record.landingControlPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
			for (auto& requirement : record.destinationPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
		}
		mAccessPermissions[bit].reset();
		if (destinationRoutesChanged)
			for (auto const& [agentId, agent] : mAgents.entries())
			{
				(void)agent;
				beginVoluntaryRoutePlanning(agentId);
			}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::setAgentAccessPermissionGrant(AgentId agentId, AccessPermissionId permission,
		bool granted, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Access permission grants can only be edited while the simulation is paused");
		auto agent = mAgents.find(agentId);
		if (!agent) return reject(format("Agent {} is invalid", agentId.value));
		auto permissionLookup = lookupAccessPermission(permission);
		if (!permissionLookup) return reject(permissionLookup.diagnostic);
		auto bit = permission.value - 1;
		if (agent->mDirectAccessGrants.test(bit) == granted)
			return reject(granted ? "The Agent already has this direct grant" : "The Agent does not have this direct grant");
		auto effectiveBefore = effectiveAccessGrants(*agent).test(bit);
		agent->mDirectAccessGrants.set(bit, granted);
		auto effectiveAfter = effectiveAccessGrants(*agent).test(bit);
		if (effectiveBefore != effectiveAfter)
			reconsiderAgentAuthorizationPath(*agent, permission, effectiveAfter);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> World::getAgentDirectAccessGrants(AgentId id) const
	{
		auto agent = mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (agent->mDirectAccessGrants.test(bit) && mAccessPermissions[bit])
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bitset<256> World::currentDirectAccessGrants(Agent const& agent) const
	{
		return (agent.mDirectAccessGrants | agent.mRuntimeDirectGrantAdditions)
			& ~agent.mRuntimeDirectGrantRemovals;
	}

	set<PermissionSetId> World::currentPermissionSets(Agent const& agent) const
	{
		auto result = agent.mPermissionSets;
		result.insert(agent.mRuntimePermissionSetAdditions.begin(),
			agent.mRuntimePermissionSetAdditions.end());
		for (auto id : agent.mRuntimePermissionSetRemovals) result.erase(id);
		return result;
	}

	bitset<256> World::effectiveAccessGrants(Agent const& agent) const
	{
		auto result = currentDirectAccessGrants(agent);
		for (auto id : currentPermissionSets(agent))
			if (auto permissionSet = mPermissionSets.find(id)) result |= permissionSet->mPermissions;
		return result;
	}

	AccessPermissionId World::accessPermissionNamed(string_view name) const
	{
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (mAccessPermissions[bit] && mAccessPermissions[bit]->getName() == name)
				return AccessPermissionId{ bit + 1 };
		return {};
	}

	PermissionSetId World::permissionSetNamed(string_view name) const
	{
		for (auto const& [id, permissionSet] : mPermissionSets.entries())
			if (permissionSet->getName() == name) return id;
		return {};
	}

	bool World::setAgentRuntimeAccessPermissionGrant(AgentId agentId,
		AccessPermissionId permission, bool granted)
	{
		invalidateSimulationSnapshot();
		auto* agent = mAgents.find(agentId);
		if (!agent || !lookupAccessPermission(permission)) return false;
		auto const bit = permission.value - 1;
		auto const current = currentDirectAccessGrants(*agent).test(bit);
		if (current == granted) return false;
		auto const effectiveBefore = effectiveAccessGrants(*agent).test(bit);
		if (granted)
		{
			agent->mRuntimeDirectGrantRemovals.reset(bit);
			agent->mRuntimeDirectGrantAdditions.set(bit,
				!agent->mDirectAccessGrants.test(bit));
		}
		else
		{
			agent->mRuntimeDirectGrantAdditions.reset(bit);
			agent->mRuntimeDirectGrantRemovals.set(bit,
				agent->mDirectAccessGrants.test(bit));
		}
		auto const effectiveAfter = effectiveAccessGrants(*agent).test(bit);
		if (effectiveBefore != effectiveAfter)
			reconsiderAgentAuthorizationPath(*agent, permission, effectiveAfter);
		return true;
	}

	vector<AccessPermissionId> World::getAgentCurrentDirectAccessGrants(AgentId id) const
	{
		auto agent = mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto grants = currentDirectAccessGrants(*agent);
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (grants.test(bit) && mAccessPermissions[bit])
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool World::setAgentRuntimePermissionSetAssignment(AgentId agentId,
		PermissionSetId id, bool assigned)
	{
		invalidateSimulationSnapshot();
		auto* agent = mAgents.find(agentId);
		auto* permissionSet = mPermissionSets.find(id);
		if (!agent || !permissionSet) return false;
		auto current = currentPermissionSets(*agent);
		if (current.contains(id) == assigned) return false;
		auto const before = effectiveAccessGrants(*agent);
		if (assigned)
		{
			agent->mRuntimePermissionSetRemovals.erase(id);
			if (!agent->mPermissionSets.contains(id))
				agent->mRuntimePermissionSetAdditions.insert(id);
		}
		else
		{
			agent->mRuntimePermissionSetAdditions.erase(id);
			if (agent->mPermissionSets.contains(id))
				agent->mRuntimePermissionSetRemovals.insert(id);
		}
		auto const after = effectiveAccessGrants(*agent);
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (before.test(bit) != after.test(bit))
				reconsiderAgentAuthorizationPath(*agent,
					AccessPermissionId{ bit + 1 }, after.test(bit));
		return true;
	}

	vector<PermissionSetId> World::getAgentCurrentPermissionSetAssignments(AgentId id) const
	{
		auto agent = mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto assignments = currentPermissionSets(*agent);
		return { assignments.begin(), assignments.end() };
	}

	vector<AccessPermissionId> World::getAgentEffectiveAccessGrants(AgentId id) const
	{
		auto agent = mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto grants = effectiveAccessGrants(*agent);
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (grants.test(bit) && mAccessPermissions[bit]) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	World::EffectiveAccessGrantSources World::getAgentAccessGrantSources(
		AgentId agentId, AccessPermissionId permission) const
	{
		auto agent = mAgents.find(agentId);
		if (!agent) throw invalid_argument("Unknown Agent");
		if (!lookupAccessPermission(permission)) throw invalid_argument("Unknown Access permission");
		EffectiveAccessGrantSources result;
		auto bit = permission.value - 1;
		result.direct = currentDirectAccessGrants(*agent).test(bit);
		for (auto id : currentPermissionSets(*agent))
			if (auto permissionSet = mPermissionSets.find(id); permissionSet && permissionSet->mPermissions.test(bit))
				result.permissionSets.push_back(id);
		return result;
	}

	bool World::permissionSetNameTaken(string const& name, PermissionSetId except) const
	{
		for (auto const& [id, permissionSet] : mPermissionSets.entries())
			if (id != except && permissionSet->getName() == name) return true;
		return false;
	}

	uint32_t World::getPermissionSetCount() const
	{ return static_cast<uint32_t>(mPermissionSets.entries().size()); }

	vector<PermissionSetId> World::getPermissionSetIds() const
	{
		vector<PermissionSetId> result;
		for (auto const& [id, permissionSet] : mPermissionSets.entries())
		{ (void)permissionSet; result.push_back(id); }
		return result;
	}

	EntityLookup<PermissionSet const> World::lookupPermissionSet(PermissionSetId id) const
	{
		auto found = mPermissionSets.find(id);
		return found ? EntityLookup<PermissionSet const>{ found, {} }
			: EntityLookup<PermissionSet const>{ nullptr, format("Permission set {} is invalid or has been deleted", id.value) };
	}

	string const& World::getPermissionSetName(PermissionSetId id) const
	{
		auto found = lookupPermissionSet(id);
		if (!found) throw invalid_argument(found.diagnostic);
		return found.entity->getName();
	}

	PermissionSetId World::addPermissionSet(string const& raw)
	{
		if (!mSimulationPaused) throw invalid_argument("Permission sets can only be edited while the simulation is paused");
		auto name = PermissionSet::trimName(raw);
		string diagnostic;
		if (!PermissionSet::nameIsValid(name, &diagnostic)) throw invalid_argument(diagnostic);
		if (permissionSetNameTaken(name)) throw invalid_argument(format("Permission set '{}' already exists", name));
		if (mPermissionSets.exhausted()) throw invalid_argument("This World has issued every Permission set ID and cannot create another");
		auto id = mPermissionSets.tryAdd(PermissionSet::create(std::move(name)));
		if (!id) throw invalid_argument("This World has issued every Permission set ID and cannot create another");
		modify(); return *id;
	}

	bool World::renamePermissionSet(PermissionSetId id, string const& raw, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto found = mPermissionSets.find(id); if (!found) return reject("Unknown Permission set");
		auto name = PermissionSet::trimName(raw);
		if (!PermissionSet::nameIsValid(name, diagnostic)) return false;
		if (permissionSetNameTaken(name, id)) return reject(format("Permission set '{}' already exists", name));
		if (name == found->getName()) { if (diagnostic) diagnostic->clear(); return true; }
		found->setName(std::move(name)); modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	uint32_t World::getPermissionSetUsageCount(PermissionSetId id) const
	{
		if (!lookupPermissionSet(id)) throw invalid_argument("Unknown Permission set");
		uint32_t count = 0;
		for (auto const& [agentId, agent] : mAgents.entries())
		{ (void)agentId; if (agent->mPermissionSets.contains(id)) ++count; }
		return count;
	}

	bool World::deletePermissionSet(PermissionSetId id, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto permissionSet = mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		auto permissions = permissionSet->mPermissions;
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			auto const before = effectiveAccessGrants(*agent);
			agent->mPermissionSets.erase(id);
			agent->mRuntimePermissionSetAdditions.erase(id);
			agent->mRuntimePermissionSetRemovals.erase(id);
			auto const after = effectiveAccessGrants(*agent);
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (permissions.test(bit) && before.test(bit) != after.test(bit))
					reconsiderAgentAuthorizationPath(*agent,
						AccessPermissionId{ bit + 1 }, after.test(bit));
		}
		mPermissionSets.remove(id); modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	vector<AccessPermissionId> World::getPermissionSetPermissions(PermissionSetId id) const
	{
		auto permissionSet = mPermissionSets.find(id); if (!permissionSet) throw invalid_argument("Unknown Permission set");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (permissionSet->mPermissions.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool World::setPermissionSetAccessPermission(PermissionSetId id,
		AccessPermissionId permission, bool included, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto permissionSet = mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		auto access = lookupAccessPermission(permission); if (!access) return reject(access.diagnostic);
		auto bit = permission.value - 1;
		if (permissionSet->mPermissions.test(bit) == included) return reject(included ? "The Permission set already contains this Access permission" : "The Permission set does not contain this Access permission");
		vector<pair<Agent*, bool>> affectedAgents;
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (currentPermissionSets(*agent).contains(id))
				affectedAgents.emplace_back(agent.get(), effectiveAccessGrants(*agent).test(bit));
		}
		permissionSet->mPermissions.set(bit, included);
		for (auto const& [agent, before] : affectedAgents)
		{
			auto after = effectiveAccessGrants(*agent).test(bit);
			if (before != after) reconsiderAgentAuthorizationPath(*agent, permission, after);
		}
		modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	vector<PermissionSetId> World::getAgentPermissionSetAssignments(AgentId id) const
	{
		auto agent = mAgents.find(id); if (!agent) throw invalid_argument("Unknown Agent");
		return { agent->mPermissionSets.begin(), agent->mPermissionSets.end() };
	}

	bool World::setAgentPermissionSetAssignment(AgentId agentId, PermissionSetId id,
		bool assigned, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mSimulationPaused) return reject("Permission set assignments can only be edited while the simulation is paused");
		auto agent = mAgents.find(agentId); if (!agent) return reject("Unknown Agent");
		auto permissionSet = mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		if (agent->mPermissionSets.contains(id) == assigned) return reject(assigned ? "The Agent already has this Permission set" : "The Agent does not have this Permission set");
		auto before = effectiveAccessGrants(*agent);
		if (assigned) agent->mPermissionSets.insert(id); else agent->mPermissionSets.erase(id);
		auto after = effectiveAccessGrants(*agent);
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (before.test(bit) != after.test(bit)) reconsiderAgentAuthorizationPath(*agent, AccessPermissionId{ bit + 1 }, after.test(bit));
		modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	bool World::isInteractionPointPermissionEligible(InteractionPointId id) const
	{
		auto point = mInteractionPoints.find(id);
		if (!point || !point->mSector || point->mAccessPanelOwner) return false;
		return none_of(point->mBindings.begin(), point->mBindings.end(), [](auto const& binding)
		{
			return binding.command.type == DeviceCommandType::SelectLiftDestination
				|| binding.command.type == DeviceCommandType::SelectShuttleDestination;
		});
	}

	bool World::setInteractionPointPermissionRequirement(InteractionPointId id,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Permission requirements can only be edited while the simulation is paused");
		auto point = mInteractionPoints.find(id);
		if (!point) return reject(format("Interaction point {} is invalid", id.value));
		if (!isInteractionPointPermissionEligible(id)) return reject("This Interaction point is not eligible for an Access permission requirement");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			auto bit = permission.value - 1;
			if (next.test(bit)) return reject(format("Access permission {} appears more than once", permission.value));
			next.set(bit);
		}
		if (next == point->mPermissionRequirement) { if (diagnostic) diagnostic->clear(); return true; }
		auto const previous = point->mPermissionRequirement;
		point->mPermissionRequirement = next;
		if (auto booth = lookupBoothWindow(point->mBoothWindowOwner))
		{
			for (auto& record : mConstructionRecords)
				if (record.type == ConstructionType::BoothWindow && record.a == booth->getFrontLayer()
					&& record.b == static_cast<uint32_t>(booth->getPosition().y)
					&& record.c == static_cast<uint32_t>(booth->getPosition().x))
				{
					auto& stored = record.controlPermissionRequirements[1];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		}
		if (auto placement = physicalControlPlacement(id);
			placement && placement->owner.type == physicalControl::OwnerType::LocationLightSwitch)
			for (auto& record : mConstructionRecords)
				if (record.type == ConstructionType::LightSwitch && record.a == placement->sectorIndex
					&& record.b + placement->owner.hostingLocation.x == placement->owner.geometry.x)
				{
					auto& stored = record.controlPermissionRequirements[0];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		if (auto unit = lookupDumbwaiter(point->mDumbwaiterOwner))
			for (auto& record : mConstructionRecords)
				if (record.type == ConstructionType::Dumbwaiter && record.dumbwaiterId == unit->getId())
				{
					auto& stored = record.controlPermissionRequirements[unit->getLandingButton(0) == id ? 0 : 1];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			bool usesPoint = find(resource->mControls.begin(), resource->mControls.end(), id)
				!= resource->mControls.end();
			if (!usesPoint)
				usesPoint = any_of(resource->mLiftStops.begin(), resource->mLiftStops.end(),
					[id](auto const& stop) { return stop.callControl == id; });
			if (usesPoint) replanAgentsAffectedByControlRequirement(resourceId, previous, next);
		}
		if (auto authored = mAuthoredControlRequirements.find(id);
			authored != mAuthoredControlRequirements.end()
			&& authored->second.constructionRecord < mConstructionRecords.size())
		{
			auto& requirements = mConstructionRecords[authored->second.constructionRecord]
				.landingControlPermissionRequirements;
			if (requirements.size() <= authored->second.slot)
				requirements.resize(authored->second.slot + 1);
			auto& stored = requirements[authored->second.slot];
			stored.clear();
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
		}
		// Generated controls also persist the requirement against their stable
		// authored side or endpoint. Interaction point IDs are replay-order handles
		// and cannot safely identify a control after a structural rebuild.
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId;
			auto control = find(resource->mControls.begin(), resource->mControls.end(), id);
			if (control == resource->mControls.end()) continue;
			size_t side = static_cast<size_t>(distance(resource->mControls.begin(), control));
			if (resource->mDoor)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(resource->mDoor))
					side = point->mSector == SectorId{ static_cast<uint64_t>(bulkhead->getSideSector(CORE_SIDE_RIGHT)->getIndex()) + 1 } ? 1 : 0;
				else side = point->mSector == SectorId{ static_cast<uint64_t>(resource->mDoor->getBackSector()->getIndex()) + 1 } ? 1 : 0;
			}
			else if (resource->mForceBridge)
				side = side == 0 ? resource->mForceBridge->getFromSide()
					: 1 - resource->mForceBridge->getFromSide();
			for (auto& record : mConstructionRecords)
			{
				bool matches = false;
				if (record.type == ConstructionType::Door && resource->mDoor)
					matches = record.layer == resource->mDoor->getFrontLayer()
						&& record.a == static_cast<uint32_t>(resource->mDoor->getPosition().y)
						&& record.b == static_cast<uint32_t>(resource->mDoor->getPosition().x)
						&& record.c == resource->mDoor->getCellsWide();
				else if (record.type == ConstructionType::BulkheadDoor && resource->mDoor)
					matches = record.a == resource->mDoor->getFrontLayer()
						&& record.b == static_cast<uint32_t>(resource->mDoor->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(round(resource->mDoor->getPosition().x
								+ resource->mDoor->getSize().x * 0.5f));
				else if (record.type == ConstructionType::ForceBridge && resource->mForceBridge
					&& record.a < mSectors.size())
					matches = mSectors[record.a]->getCellX() + record.c
						== static_cast<uint32_t>(resource->mForceBridge->getPosition().x)
						&& mSectors[record.a]->getCellY() + record.b
						== static_cast<uint32_t>(resource->mForceBridge->getPosition().y);
				else if (record.type == ConstructionType::Airlock && resource->mAirlock)
					matches = record.layer == resource->mAirlock->getLayerIndex()
						&& record.a == resource->mAirlock->getCellY()
						&& record.b == resource->mAirlock->getCellX();
				else if (record.type == ConstructionType::Ladder && resource->mLadder)
					matches = record.b == static_cast<uint32_t>(resource->mLadder->getPosition().x)
						&& record.a == static_cast<uint32_t>(resource->mLadder->getPosition().y);
				else if (record.type == ConstructionType::SectorLadder && resource->mLadder
					&& record.a < mSectors.size())
					matches = mSectors[record.a]->getCellX() + record.c
						== static_cast<uint32_t>(resource->mLadder->getPosition().x)
						&& mSectors[record.a]->getCellY() + record.b
						== static_cast<uint32_t>(resource->mLadder->getPosition().y);
				if (!matches || side >= 2) continue;
				record.controlPermissionRequirements[side].clear();
				for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
					if (next.test(bit)) record.controlPermissionRequirements[side].push_back(
						static_cast<uint32_t>(bit + 1));
				break;
			}
			break;
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> World::getInteractionPointPermissionRequirement(InteractionPointId id) const
	{
		auto point = mInteractionPoints.find(id);
		if (!point) throw invalid_argument("Unknown Interaction point");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (point->mPermissionRequirement.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	vector<AccessPermissionId> World::missingInteractionPermissions(
		InteractionPoint const& point, Agent const& agent) const
	{
		vector<AccessPermissionId> result;
		auto missing = point.mPermissionRequirement & ~effectiveAccessGrants(agent);
		for (auto const& binding : point.mBindings)
			for (auto permission : missingLiftDestinationPermissions(binding.command, getAgentId(&agent)))
				missing.set(permission.value - 1);
		for (size_t bit = 0; bit < mAccessPermissions.size(); ++bit)
			if (missing.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool World::agentSatisfiesDoorPermission(Door const& door, Agent const& agent) const
	{
		return (door.mPermissionRequirement & ~effectiveAccessGrants(agent)).none();
	}

	bool World::isManualDoorPermissionEligible(TraversalResourceId id) const
	{
		auto resource = mTraversalResources.find(id);
		return resource && resource->mDoor
			&& !dynamic_pointer_cast<BulkheadDoor>(resource->mDoor)
			&& !resource->mLiftCoordinator && !resource->mShuttle
			&& resource->mDoorActivationMode == DoorActivationMode::Manual
			&& resource->mControls.empty();
	}

	bool World::setManualDoorPermissionRequirement(TraversalResourceId id,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Permission requirements can only be edited while the simulation is paused");
		if (!isManualDoorPermissionEligible(id))
			return reject("This Door is not a buttonless manual ordinary Door");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			if (next.test(permission.value - 1))
				return reject(format("Access permission {} appears more than once", permission.value));
			next.set(permission.value - 1);
		}
		auto resource = mTraversalResources.find(id);
		if (resource->mDoor->mPermissionRequirement == next)
		{ if (diagnostic) diagnostic->clear(); return true; }
		auto const previous = resource->mDoor->mPermissionRequirement;
		auto door = resource->mDoor;
		auto record = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& value)
			{
				return value.type == ConstructionType::Door
					&& value.layer == door->getFrontLayer()
					&& value.a == static_cast<uint32_t>(door->getPosition().y)
					&& value.b == static_cast<uint32_t>(door->getPosition().x)
					&& value.c == door->getCellsWide();
			});
		if (record == mConstructionRecords.rend())
			return reject("The selected Door no longer has an authored definition");
		record->values.clear();
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (next.test(bit)) record->values.push_back(static_cast<uint32_t>(bit + 1));
		resource->mDoor->mPermissionRequirement = next;
		replanAgentsAffectedByControlRequirement(id, previous, next);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> World::getManualDoorPermissionRequirement(
		TraversalResourceId id) const
	{
		auto resource = mTraversalResources.find(id);
		if (!resource || !resource->mDoor) throw invalid_argument("Unknown Door");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (resource->mDoor->mPermissionRequirement.test(bit))
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool World::canAgentOpenManualDoor(TraversalResourceId doorId, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		return resource && resource->mDoor && agent
			&& agentSatisfiesDoorPermission(*resource->mDoor, *agent);
	}

	bool World::canAgentOperateDoorControl(TraversalResourceId doorId, SectorId approach,
		AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mDoor || !agent) return false;
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (point && point->mSector == approach
				&& missingInteractionPermissions(*point, *agent).empty()) return true;
		}
		return false;
	}

	bool World::agentAdheresToDoorPermission(TraversalResourceId doorId,
		SectorId approach, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mDoor || !agent) return false;
		// This slice applies only to ordinary Doors. Transport and Bulkhead Door
		// adherence remain separate resource slices.
		if (dynamic_pointer_cast<BulkheadDoor>(resource->mDoor)
			|| resource->mLiftCoordinator || resource->mShuttle) return true;
		if (!agent->getEffectivePermissionAdherence().value) return true;
		if (resource->mDoorActivationMode == DoorActivationMode::Manual)
			return agentSatisfiesDoorPermission(*resource->mDoor, *agent);
		if (resource->mDoorActivationMode != DoorActivationMode::RemoteControlled)
			return true;

		bool applicableControl{ false };
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (!point || point->mSector != approach) continue;
			applicableControl = true;
			if (missingInteractionPermissions(*point, *agent).empty()) return true;
		}
		// No approach-side control means there is no applicable requirement to
		// adhere to; requirements on the opposite side are deliberately ignored.
		return !applicableControl;
	}

	bool World::canAgentOperateExtensibleControl(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& approachPosition, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mExtensible || !agent) return false;
		auto applicable = [&](InteractionPoint const& point)
		{
			if (point.mSector != approach) return false;
			// A Room may contain both controls. Keep the opposite endpoint's
			// requirement out of this operation rather than treating every control
			// in the shared Sector as one combined requirement.
			if (resource->mForceBridge)
			{
				auto const middle = resource->mForceBridge->getPosition().x
					+ resource->mForceBridge->getSize().x * 0.5f;
				return (point.mPosition.x > middle) == (approachPosition.x > middle);
			}
			if (resource->mLadder)
			{
				// Ladder geometry extends above the upper landing for handholds; use
				// the logical landing midpoint rather than the rendered object's centre.
				auto const lower = resource->mLadder->getPosition().y
					- CORE_LADDER_HEIGHT_OFF_GROUND;
				auto const middle = lower
					+ static_cast<float>(resource->mLadder->getLevelsHigh() - 1) * 0.5f;
				return (point.mPosition.y > middle) == (approachPosition.y > middle);
			}
			return true;
		};
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (point && applicable(*point)
				&& missingInteractionPermissions(*point, *agent).empty()) return true;
		}
		return false;
	}

	bool World::agentAdheresToExtensiblePermission(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& approachPosition, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto agent = mAgents.find(agentId);
		if (!resource || !agent) return false;
		// Callers decide passage usability from local observation or memory. Do
		// not inspect remote extension here: requirements are authored facts.
		if ((!resource->mForceBridge && !resource->mLadder)
			|| !resource->mExtensible) return true;
		if (!agent->getEffectivePermissionAdherence().value) return true;

		// If no control applies on this approach there is no approach-side
		// permission requirement to adhere to. Existing preparation-side and
		// availability rules still decide whether traversal is physically possible.
		bool applicableControl = false;
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (!point || point->mSector != approach) continue;
			bool applicable = true;
			if (resource->mForceBridge)
			{
				auto const middle = resource->mForceBridge->getPosition().x
					+ resource->mForceBridge->getSize().x * 0.5f;
				applicable = (point->mPosition.x > middle) == (approachPosition.x > middle);
			}
			else if (resource->mLadder)
			{
				auto const lower = resource->mLadder->getPosition().y
					- CORE_LADDER_HEIGHT_OFF_GROUND;
				auto const middle = lower
					+ static_cast<float>(resource->mLadder->getLevelsHigh() - 1) * 0.5f;
				applicable = (point->mPosition.y > middle) == (approachPosition.y > middle);
			}
			if (!applicable) continue;
			applicableControl = true;
			if (missingInteractionPermissions(*point, *agent).empty()) return true;
		}
		return !applicableControl;
	}

	vector<AccessPermissionId> World::missingLiftDestinationPermissions(
		DeviceCommand const& command, AgentId agentId) const
	{
		vector<AccessPermissionId> result;
		if (!agentId || (command.type != DeviceCommandType::SelectLiftDestination
			&& command.type != DeviceCommandType::SelectShuttleDestination)) return result;
		auto resource = mTraversalResources.find(command.traversalResource);
		if (!resource || (!resource->mLift && !resource->mShuttle)
			|| command.stopIndex >= resource->mLiftStops.size()) return result;
		auto agent = mAgents.find(agentId);
		auto grants = agent ? effectiveAccessGrants(*agent) : bitset<256>{};
		auto record = findLiftDestinationRecord(*resource);
		if (record && command.stopIndex < record->destinationPermissionRequirements.size())
			for (auto permission : record->destinationPermissionRequirements[command.stopIndex])
				if (!grants.test(permission - 1)) result.push_back(AccessPermissionId{ permission });
		return result;
	}

	bool World::canAgentUseLiftJourney(TraversalResourceId resourceId,
		Vector2 const& origin, Vector2 const& destination, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto destinationStop = resource && resource->mLiftCoordinator ? resource->mLiftStopIndex : ~0u;
		if (resource && resource->mLiftCoordinator)
		{
			resourceId = resource->mLiftCoordinator;
			resource = mTraversalResources.find(resourceId);
		}
		if (!resource || (!resource->mLift && !resource->mShuttle)) return true;
		auto stop = destinationStop != ~0u ? destinationStop : mSimulationCoordinator.findLiftStop(*resource, destination);
		DeviceCommand command;
		command.type = DeviceCommandType::SelectLiftDestination;
		command.traversalResource = resourceId;
		command.stopIndex = stop;
		if (missingLiftDestinationPermissions(command, agentId).empty()) return true;
		auto agent = mAgents.find(agentId);
		if (!agent || !agent->getSector()) return false;
		bool local = find(resource->mOccupants.begin(), resource->mOccupants.end(), agentId)
			!= resource->mOccupants.end();
		if (!agentAdheresToLiftDestinationPermission(resourceId, stop, agentId)) return false;
		if (local) return true;
		// Only an open, boardable car at this Agent's landing reveals a usable
		// shared journey. Never consult a remote car's live destination requests.
		if (!local && resource->mShuttle)
			local = any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(), [&](auto const& door)
			{
				return door.locationSector.value == agent->getSector()->getIndex() + 1
					&& isTransportLocallyBoardable(door.landingResource, origin);
			});
		if (!local && !resource->mShuttle)
		{
			auto originStop = mSimulationCoordinator.findLiftStop(*resource, origin);
			if (originStop < resource->mLiftStops.size())
			{
				auto const& landing = resource->mLiftStops[originStop];
				local = landing.locationSector.value == agent->getSector()->getIndex() + 1
					&& std::abs(agent->getGlobalPosition().y - landing.globalPosition) <= 0.5f
					&& isTransportLocallyBoardable(resource->mOpenPlatformLift ? resourceId : landing.landingResource, origin);
			}
		}
		return local && stop < resource->mLiftStopRequestOwners.size()
			&& (!resource->mLiftStopRequestOwners[stop].empty()
				|| (!resource->mLiftMoving && resource->mLiftCurrentStop == stop));
	}

	bool World::agentAdheresToLiftDestinationPermission(TraversalResourceId resourceId,
		uint32_t destinationStop, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (resource && resource->mLiftCoordinator)
		{
			resourceId = resource->mLiftCoordinator;
			resource = mTraversalResources.find(resourceId);
		}
		if (!resource || (!resource->mLift && !resource->mShuttle)
			|| destinationStop >= resource->mLiftStops.size()) return false;
		// Destination requirements are shared by every vehicle, coupled Carriage,
		// and boarding origin of each transport.
		if (find(resource->mOccupants.begin(), resource->mOccupants.end(), agentId)
			!= resource->mOccupants.end()) return true;
		auto agent = mAgents.find(agentId);
		if (!agent) return false;
		DeviceCommand command;
		command.type = resource->mShuttle
			? DeviceCommandType::SelectShuttleDestination
			: DeviceCommandType::SelectLiftDestination;
		command.traversalResource = resourceId;
		command.stopIndex = destinationStop;
		return missingLiftDestinationPermissions(command, agentId).empty()
			|| !agent->getEffectivePermissionAdherence().value;
	}

	bool World::canAgentOperateTransportLandingControl(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& endpoint, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto agent = mAgents.find(agentId);
		if (!resource || !agent) return false;
		if (resource->mLiftCoordinator)
		{
			for (auto pointId : resource->mControls)
			{
				auto point = mInteractionPoints.find(pointId);
				if (point && point->mSector == approach
					&& missingInteractionPermissions(*point, *agent).empty()) return true;
			}
			return false;
		}
		if (!resource->mLift && !resource->mShuttle) return false;
		auto stop = mSimulationCoordinator.findLiftStop(*resource, endpoint);
		if (stop >= resource->mLiftStops.size()) return false;
		auto point = mInteractionPoints.find(resource->mLiftStops[stop].callControl);
		return point && point->mSector == approach
			&& missingInteractionPermissions(*point, *agent).empty();
	}

	bool World::agentAdheresToTransportLandingPermission(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& endpoint, AgentId agentId) const
	{
		auto agent = mAgents.find(agentId);
		return agent && (!agent->getEffectivePermissionAdherence().value
			|| canAgentOperateTransportLandingControl(resourceId, approach, endpoint, agentId));
	}

	bool World::isAgentTransportOccupant(TraversalResourceId resourceId, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (resource && resource->mLiftCoordinator)
			resource = mTraversalResources.find(resource->mLiftCoordinator);
		return agentId && resource && (resource->mLift || resource->mShuttle)
			&& find(resource->mOccupants.begin(), resource->mOccupants.end(), agentId) != resource->mOccupants.end();
	}

	bool World::isTransportLocallyBoardable(TraversalResourceId resourceId,
		Vector2 const& endpoint) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return false;
		if (resource->mLiftCoordinator)
		{
			auto coordinator = mTraversalResources.find(resource->mLiftCoordinator);
			return coordinator && resource->mDoor && resource->mDoor->isOpen()
				&& !(coordinator->mLift && coordinator->mLift->isBroken())
				&& !(coordinator->mShuttle && coordinator->mShuttle->isBroken())
				&& !coordinator->mLiftMoving
				&& coordinator->mLiftCurrentStop == resource->mLiftStopIndex
				&& coordinator->mLiftStopPhase == LiftStopPhase::Boarding;
		}
		if (!resource->mLift && !resource->mShuttle) return false;
		auto stop = mSimulationCoordinator.findLiftStop(*resource, endpoint);
		return stop < resource->mLiftStops.size() && !(resource->mLift && resource->mLift->isBroken())
			&& !(resource->mShuttle && resource->mShuttle->isBroken()) && !resource->mLiftMoving
			&& resource->mLiftCurrentStop == stop
			&& resource->mLiftStopPhase == LiftStopPhase::Boarding;
	}

	bool World::canAgentTraverseManualDoorNow(TraversalResourceId doorId, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		if (!resource || !resource->mDoor
			|| resource->mDoorActivationMode != DoorActivationMode::Manual
			|| resource->mDoor->admitsNewCrossings()) return true;
		if (resource->mDoor->isIndependentlyBroken())
		{
			auto agent = mAgents.find(agentId);
			return agent && resource->mDoor->admitsBrokenPassage(*agent, agent->getGlobalPosition().y);
		}
		return canAgentOpenManualDoor(doorId, agentId);
	}

	void World::replanAgentAfterAuthorizationRefusal(AgentId id)
	{
		mSimulationCoordinator.replanAgentAfterAuthorizationRefusal(id);
	}

	void World::beginVoluntaryRoutePlanning(AgentId id)
	{
		mSimulationCoordinator.replanAgentAfterAuthorizationRefusal(id, true);
	}

	void World::replanAgentsAffectedByControlRequirement(TraversalResourceId resource,
		bitset<256> const& previous, bitset<256> const& next)
	{
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			auto const grants = effectiveAccessGrants(*agent);
			auto const satisfiedBefore = (previous & ~grants).none();
			auto const satisfiedAfter = (next & ~grants).none();
			if (satisfiedBefore == satisfiedAfter) continue;
			if (satisfiedAfter)
			{
				// Loosening a requirement may reveal a preferable alternate route,
				// but does not invalidate the Agent's current Path.
				beginVoluntaryRoutePlanning(agentId);
				continue;
			}

			auto goal = mMovementGoals.find(agentId);
			auto path = agent->mPath.path;
			auto fromNode = agent->mPath.targetNode;
			if (!path && goal != mMovementGoals.end())
			{
				path = goal->second.retainedPath;
				fromNode = goal->second.retainedFromNode;
			}
			if (!path || path->nodes.empty()) continue;
			for (uint32_t i = fromNode + 1; i < path->nodes.size(); ++i)
			{
				auto const& edge = path->nodes[i].edge;
				if (edge && edge->getTraversalResourceId() == resource)
				{
					replanAgentAfterAuthorizationRefusal(agentId);
					break;
				}
			}
		}
	}

	bool World::agentPathEntersLocation(Agent const& agent, Sector const& location) const
	{
		auto path = agent.mPath.path;
		auto fromNode = agent.mPath.targetNode;
		if (!path)
			if (auto goal = mMovementGoals.find(getAgentId(&agent)); goal != mMovementGoals.end())
			{
				path = goal->second.retainedPath;
				fromNode = goal->second.retainedFromNode;
			}
		if (!path) return false;
		auto source = agent.getSector();
		// A committed entry finishes safely, then must plan from its new
		// occupancy before following any further affected movement.
		if (agent.mTraversalTask && agent.mTraversalTask->permit)
		{
			auto committedDestination = agent.mTraversalTask->destinationVertex->getSector().get();
			if (committedDestination == &location && source != committedDestination) return true;
			source = committedDestination;
			fromNode += agent.mTraversalTask->pathNodesConsumed;
		}
		// Source-owned waypoints are allowed only for escape, not an internal
		// destination (including a destination at the current position).
		if (source == &location && !path->nodes.empty()
			&& path->nodes.back().targetVertex->getSector().get() == &location) return true;
		for (uint32_t i = fromNode + 1; i < path->nodes.size(); ++i)
		{
			auto target = path->nodes[i].targetVertex->getSector().get();
			if (target == &location && source != target) return true;
			source = target;
		}
		return false;
	}

	void World::reconsiderAgentAuthorizationPath(Agent& agent, AccessPermissionId changed,
		bool gained)
	{
		auto const bit = changed.value - 1;
		for (auto const& sector : mSectors)
		{
			if (sector->getType() != SectorType::Location
				|| !static_cast<Location const&>(*sector).getPermissionRequirement().test(bit)) continue;
			if (gained)
			{
				if (canAgentAccessLocation(*sector, agent))
					beginVoluntaryRoutePlanning(getAgentId(&agent));
			}
			else if (agentPathEntersLocation(agent, *sector))
				replanAgentAfterAuthorizationRefusal(getAgentId(&agent));
		}
		auto path = agent.mPath.path;
		auto fromNode = agent.mPath.targetNode;
		if (!path && !gained)
			if (auto goal = mMovementGoals.find(getAgentId(&agent)); goal != mMovementGoals.end())
			{
				path = goal->second.retainedPath;
				fromNode = goal->second.retainedFromNode;
			}
		if (!path || path->nodes.empty()
			|| (gained && agent.mTraversalTask && agent.mTraversalTask->permit)) return;
		auto resourceUsesPermission = [&](TraversalResource const& resource)
		{
			if (resource.mDoor && resource.mDoor->mPermissionRequirement.test(bit)) return true;
			for (auto pointId : resource.mControls)
			{
				auto point = mInteractionPoints.find(pointId);
				if (point && point->mPermissionRequirement.test(bit)) return true;
			}
			if (gained && (resource.mLift || resource.mShuttle))
				if (auto record = findLiftDestinationRecord(resource))
					for (auto const& requirement : record->destinationPermissionRequirements)
						if (find(requirement.begin(), requirement.end(), changed.value) != requirement.end()) return true;
			for (auto const& stop : resource.mLiftStops)
			{
				auto point = mInteractionPoints.find(stop.callControl);
				if (point && point->mPermissionRequirement.test(bit)) return true;
			}
			return false;
		};
		auto edgeUsesPermission = [&](shared_ptr<const Edge> const& edge)
		{
			if (!edge) return false;
			auto resource = mTraversalResources.find(edge->getTraversalResourceId());
			return resource && resourceUsesPermission(*resource);
		};
		bool currentRelevant = false;
		for (uint32_t i = fromNode + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			currentRelevant = currentRelevant || edgeUsesPermission(node.edge);
			if (gained || !node.edge) continue;
			auto resource = mTraversalResources.find(node.edge->getTraversalResourceId());
			// Only the alighting edge identifies the selected destination. Passing
			// an intermediate Stop or boarding must not require its permissions.
			if (!resource) continue;
			auto source = path->nodes[i - 1].targetVertex;
			if (!source || !source->getSector()) continue;
			auto liftId = resource->mLiftCoordinator;
			auto stop = resource->mLiftStopIndex;
			if (resource->mOpenPlatformLift)
			{
				if (node.edge->getType() != EdgeType::LiftMount || !node.targetVertex->getObject()) continue;
				liftId = node.edge->getTraversalResourceId();
				stop = mSimulationCoordinator.findLiftStop(*resource, source->getPosition());
			}
			else if (isLocationLike(source->getSector()->getType())) continue;
			auto lift = mTraversalResources.find(liftId);
			if (!lift || (!lift->mLift && !lift->mShuttle)) continue;
			auto record = findLiftDestinationRecord(*lift);
			if (!record || stop >= record->destinationPermissionRequirements.size()) continue;
			auto const& requirement = record->destinationPermissionRequirements[stop];
			if (find(requirement.begin(), requirement.end(), changed.value) != requirement.end()
				&& !canAgentUseLiftJourney(node.edge->getTraversalResourceId(), agent.getGlobalPosition(),
					source->getPosition(), getAgentId(&agent))) currentRelevant = true;
		}
		if (!gained)
		{
			if (currentRelevant)
				replanAgentAfterAuthorizationRefusal(getAgentId(&agent));
			return;
		}
		bool availableRouteChanged = false;
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId;
			availableRouteChanged = availableRouteChanged || resourceUsesPermission(*resource);
		}
		if (!availableRouteChanged) return;
		beginVoluntaryRoutePlanning(getAgentId(&agent));
	}

	InteractionPointId World::createInteractionPoint(string const& name)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createInteractionPoint(name);
	}

	InteractionPointId World::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createInteractionPoint(name, sector, position, reach, durationSeconds, std::move(bindings));
	}

	EntityLookup<InteractionPoint> World::lookupInteractionPoint(InteractionPointId id)
	{
		return mSimulationCoordinator.lookupInteractionPoint(id);
	}

	EntityLookup<InteractionPoint const> World::lookupInteractionPoint(InteractionPointId id) const
	{
		return mSimulationCoordinator.lookupInteractionPoint(id);
	}

	EntityRemovalResult World::removeInteractionPoint(InteractionPointId id)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.removeInteractionPoint(id);
	}

	DeviceOperationId World::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		return mSimulationCoordinator.findOrCreateDeviceOperation(command, requester);
	}

	InteractionRequestId World::requestInteraction(InteractionPointId pointId, AgentId actorId)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.requestInteraction(pointId, actorId);
	}

	InteractionRequestId World::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.requestInteractionForTraversal(point, actor);
	}

	InteractionRequestId World::requestInteractionWhilePassing(
		InteractionPointId pointId, AgentId actorId)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.requestInteractionWhilePassing(pointId, actorId);
	}

	EntityLookup<InteractionRequest const> World::lookupInteractionRequest(InteractionRequestId id) const
	{
		return mSimulationCoordinator.lookupInteractionRequest(id);
	}

	bool World::cancelInteraction(InteractionRequestId id)
	{
		return mSimulationCoordinator.cancelInteraction(id);
	}

	EntityRemovalResult World::removeInteractionRequest(InteractionRequestId id)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.removeInteractionRequest(id);
	}

	DeviceOperationId World::createDeviceOperation(string const& name, AgentId requester)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.createDeviceOperation(name, requester);
	}

	EntityLookup<DeviceOperation> World::lookupDeviceOperation(DeviceOperationId id)
	{
		return mSimulationCoordinator.lookupDeviceOperation(id);
	}

	EntityLookup<DeviceOperation const> World::lookupDeviceOperation(DeviceOperationId id) const
	{
		return mSimulationCoordinator.lookupDeviceOperation(id);
	}

	bool World::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		return mSimulationCoordinator.cancelDeviceOperation(id, requester);
	}

	EntityRemovalResult World::removeDeviceOperation(DeviceOperationId id)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.removeDeviceOperation(id);
	}

	TraversalResourceId World::createTraversalResource(string const& name)
	{
		invalidateSimulationSnapshot();
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createDoorTraversalResource(string const& name,
		shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds)
	{
		invalidateSimulationSnapshot();
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// resource is a true no-op: no traversal resource, event, or dirty
		// document (#198).
		if (!door || !isFiniteTiming(holdOpenSeconds))
		{
			throw invalid_argument("A door traversal resource requires a Door and a finite, non-negative hold time");
		}
		beginStructuralEdit("createDoorTraversalResource");
		auto holdTicks = secondsToTicks(holdOpenSeconds, getFixedTimestep());
		auto laneCount = max(1u, door->getCellsWide());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(door), mode, holdTicks)));
		mTraversalResources.find(id)->mCrossingOwners.resize(laneCount);
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createWindowTraversalResource(string const& name,
		shared_ptr<Window> window)
	{
		if (!window || window->isBoothWindow())
			throw invalid_argument("A Window crossing resource requires an ordinary Window, not a BoothWindow");
		invalidateSimulationSnapshot();
		beginStructuralEdit("createWindowTraversalResource");
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(window))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createLadderTraversalResource(string const& name,
		shared_ptr<Ladder> ladder, SectorId ladderSector, uint32_t directionalBatchLimit)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("createLadderTraversalResource");
		if (!ladder || !ladderSector || ladderSector.value > mSectors.size()
			|| directionalBatchLimit == 0)
		{
			throw invalid_argument("A ladder traversal resource requires a Ladder, sector, and positive batch limit");
		}
		// Capacity is a physical property of the usable vertical span: each slot keeps
		// neighbouring Agents at least CORE_LADDER_SLOT_SPACING apart in render space.
		// Even a short valid Ladder admits one Agent.
		auto crossedLevels = (float)(ladder->getLevelsHigh() - 1);
		auto agentSpacing = CORE_LADDER_SLOT_SPACING / CORE_CELL_YX_RENDER_RATIO;
		auto capacity = max(1u, (uint32_t)floor(crossedLevels / agentSpacing));
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = ladder->getPosition();
		auto x = origin.x + ladder->getSize().x * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ x, origin.y + (agentSpacing >= crossedLevels
				? crossedLevels * 0.5f : agentSpacing * ((float)i + 0.5f)) });
		}

		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, ladder, ladder->isExtensible() ? static_pointer_cast<ExtensibleObject>(ladder) : nullptr,
			ladderSector, agentSpacing, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId liftSector, vector<LiftStop> stops, uint32_t capacity,
		float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		invalidateSimulationSnapshot();
		if (!lift || !liftSector || liftSector.value > mSectors.size() || stops.size() < 2
			|| capacity == 0)
		{
			throw invalid_argument("A lift traversal resource requires a lift sector, at least two stops, and positive capacity");
		}
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(minimumDwellSeconds) || !isFiniteTiming(maximumBoardingSeconds)
			|| maximumBoardingSeconds < minimumDwellSeconds)
		{
			throw invalid_argument("A lift traversal resource requires finite dwell timing with 0 <= minimum dwell <= maximum boarding time");
		}
		beginStructuralEdit("createLiftTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (!stops[i].locationSector || stops[i].locationSector.value > mSectors.size())
				throw invalid_argument(format("Lift stop {} has an invalid location sector mapping", i));
			auto landing = mTraversalResources.find(stops[i].landingResource);
			if (!landing || !landing->mDoor)
				throw invalid_argument(format("Lift stop {} has no valid landing-door traversal mapping", i));
			if (!isfinite(stops[i].globalPosition))
				throw invalid_argument(format("Lift stop {} has a non-finite position", i));
			if (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition)
				throw invalid_argument(format("Lift stop {} is not strictly above the previous stop", i));
		}
		auto usableWidth = lift->getSize().x;
		if (capacity > (uint32_t)floor(usableWidth / CORE_RESOURCE_SLOT_WIDTH))
			throw invalid_argument("Declared lift capacity cannot be represented by separated interior positions");
		vector<Vector2> positions(capacity);
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto const occupantTargets = packOccupants(capacity, 0,
			{ halfAgentWidth, usableWidth - halfAgentWidth }, CORE_RESOURCE_SLOT_WIDTH,
			mTraversalGeometryPolicy.occupantClearance, OccupantPackingOrder::Forward,
			OccupantPackingLayout::Compact);
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { occupantTargets[i], 0.0f };
		auto minimumDwellTicks = secondsToTicks(minimumDwellSeconds, getFixedTimestep());
		auto maximumBoardingTicks = secondsToTicks(maximumBoardingSeconds, getFixedTimestep());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(lift), liftSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createOpenPlatformLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId locationSector, vector<LiftStop> stops, uint32_t capacity,
		float stopDurationSeconds)
	{
		invalidateSimulationSnapshot();
		if (!lift || !locationSector || locationSector.value > mSectors.size() || stops.size() < 2
			|| capacity == 0)
			throw invalid_argument("An open platform lift requires a location, at least two stops, and positive capacity");
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(stopDurationSeconds))
			throw invalid_argument("An open platform lift requires a finite, non-negative stop duration");
		beginStructuralEdit("createOpenPlatformLiftTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (stops[i].locationSector != locationSector || stops[i].landingResource
				|| !isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Platform lift stop {} has invalid virtual-boundary geometry", i));
		}
		if (capacity > (uint32_t)floor(lift->getSize().x / CORE_RESOURCE_SLOT_WIDTH))
			throw invalid_argument("Declared platform lift capacity cannot be represented by separated standing positions");
		vector<Vector2> positions(capacity);
		auto start = (lift->getSize().x - capacity * CORE_RESOURCE_SLOT_WIDTH) * 0.5f
			+ CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { start + i * CORE_RESOURCE_SLOT_WIDTH, 0.0f };
		auto stopDurationTicks = secondsToTicks(stopDurationSeconds, getFixedTimestep());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name,
			std::move(lift), locationSector, std::move(stops), capacity,
			stopDurationTicks, stopDurationTicks, std::move(positions))));
		auto resource = mTraversalResources.find(id);
		resource->mOpenPlatformLift = true;
		resource->mVirtualBoundaryOwners.resize(1);
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *resource);
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createShuttleTraversalResource(string const& name,
		shared_ptr<Shuttle> shuttle, SectorId shuttleSector, vector<LiftStop> stops,
		uint32_t capacity, float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		invalidateSimulationSnapshot();
		if (!shuttle || shuttle->getNumCars() == 0 || !shuttleSector
			|| shuttleSector.value > mSectors.size() || stops.size() < 2 || capacity == 0)
			throw invalid_argument("A coupled shuttle requires valid stops, carriages, and positive per-carriage capacity");
		// Timing is judged before beginStructuralEdit() and before the stop loop,
		// so a refused resource changes nothing and no non-finite value reaches
		// the tick conversion (#198).
		if (!isFiniteTiming(minimumDwellSeconds) || !isFiniteTiming(maximumBoardingSeconds)
			|| maximumBoardingSeconds < minimumDwellSeconds)
			throw invalid_argument("A coupled shuttle requires finite dwell timing with 0 <= minimum dwell <= maximum boarding time");
		beginStructuralEdit("createShuttleTraversalResource");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			auto landing = mTraversalResources.find(stops[i].landingResource);
			if (!stops[i].locationSector || stops[i].locationSector.value > mSectors.size()
				|| !landing || !landing->mDoor)
				throw invalid_argument(format("Shuttle stop {} has an invalid location or landing-door mapping", i));
			if (!isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Shuttle stop {} has invalid linear geometry", i));
		}
		auto usableWidth = (float)shuttle->getCarWidth();
		auto const representablePositions = maximumShuttleCarriageCapacity(
			shuttle->getCarWidth());
		if (capacity > representablePositions)
			throw invalid_argument("Declared shuttle capacity cannot be represented by buffered carriage standing positions");
		vector<Vector2> positions;
		positions.reserve(capacity * shuttle->getNumCars());
		auto const first = CORE_RESOURCE_SLOT_WIDTH * 0.5f + CORE_SHUTTLE_OCCUPANT_CLEARANCE;
		auto const last = usableWidth - CORE_RESOURCE_SLOT_WIDTH * 0.5f
			- CORE_SHUTTLE_OCCUPANT_CLEARANCE;
		for (uint32_t carriage = 0; carriage < shuttle->getNumCars(); ++carriage)
			for (uint32_t i = 0; i < capacity; ++i)
			{
				auto const progress = capacity == 1 ? 0.5f : (float)i / (float)(capacity - 1);
				positions.push_back({ carriage * (shuttle->getCarWidth() + 1.0f)
					+ first + (last - first) * progress, 0.0f });
			}
		auto minimumDwellTicks = secondsToTicks(minimumDwellSeconds, getFixedTimestep());
		auto maximumBoardingTicks = secondsToTicks(maximumBoardingSeconds, getFixedTimestep());
		auto shuttlePtr = shuttle;
		auto stopCount = (uint32_t)stops.size();
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(shuttle), shuttleSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		auto resource = mTraversalResources.find(id);
		resource->mShuttleCarriages.reserve(shuttlePtr->getNumCars());
		for (uint32_t carriage = 0; carriage < shuttlePtr->getNumCars(); ++carriage)
			resource->mShuttleCarriages.push_back({ carriage, carriage * capacity, capacity,
				std::vector<std::vector<TraversalResourceId>>(stopCount), {}, {}, {},
				TraversalDirection::None });
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createStairwellTraversalResource(string const& name,
		shared_ptr<Stairwell> stairwell, SectorId stairwellSector, uint32_t capacity,
		uint32_t directionalBatchLimit)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("createStairwellTraversalResource");
		if (!stairwell || !stairwellSector || stairwellSector.value > mSectors.size()
			|| capacity == 0 || directionalBatchLimit == 0)
		{
			throw invalid_argument("A narrow stairwell resource requires a Stairwell, sector, capacity, and batch limit");
		}
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = stairwell->getPosition();
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ origin.x + ((float)i + 0.5f) * stairwell->getSize().x / capacity,
				origin.y + stairwell->getSize().y * 0.5f });
		}
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, stairwell, stairwellSector, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId World::createForceBridgeTraversalResource(string const& name,
		shared_ptr<ForceBridge> forceBridge)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("createForceBridgeTraversalResource");
		if (!forceBridge) throw invalid_argument("A force bridge traversal resource requires a ForceBridge");
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, forceBridge, forceBridge->isExtensible()
				? static_pointer_cast<ExtensibleObject>(forceBridge) : nullptr)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	void World::configureForceBridgeQueueLanes(TraversalResourceId resourceId,
		SectorId sectorId, array<Vector2, 2> const& endpoints)
	{
		invalidateSimulationSnapshot();
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mForceBridge || !sectorId || sectorId.value > mSectors.size())
			throw invalid_argument("Force Bridge queue lanes require a Force Bridge and source sector");
		auto sector = mSectors[(size_t)sectorId.value - 1];
		auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto const spacing = (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
		for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
		{
			auto const direction = approach == 0 ? Vector2::NEGATIVE_UNIT_X : Vector2::UNIT_X;
			auto const endpoint = endpoints[approach];
			auto const boundary = approach == 0
				? sector->getCellX0() + halfWidth : sector->getCellX1() + 1.0f - halfWidth;
			auto const extent = max(0.0f, approach == 0
				? endpoint.x - boundary : boundary - endpoint.x);
			auto& lane = resource->mQueueLanes[approach];
			lane.sector = sectorId;
			lane.origin = endpoint;
			lane.direction = direction;
			lane.extent = 0.0f;
			for (float distance = 0.0f; distance <= extent + 0.001f; distance += spacing)
			{
				auto position = endpoint + direction * distance;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
				if (!mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot()) break;
				lane.positions.push_back(position);
				lane.extent = distance;
			}
			lane.positionOwners.assign(lane.positions.size(), {});
			if (lane.positions.empty())
				throw invalid_argument("A Force Bridge approach has no usable queue positions");
		}
		resource->mCrossingOwners.assign(1, {});
	}

	void World::configureLadderQueueLanes(TraversalResourceId resourceId,
		array<SectorId, 2> const& sectors, array<Vector2, 2> const& endpoints)
	{
		invalidateSimulationSnapshot();
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mLadder)
			throw invalid_argument("Ladder queue lanes require a Ladder traversal resource");

		auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto const spacing = (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
		for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
		{
			if (!sectors[approach] || sectors[approach].value > mSectors.size())
				throw invalid_argument("Ladder queue lane has an invalid approach sector");
			auto sector = mSectors[(size_t)sectors[approach].value - 1];
			auto const endpoint = endpoints[approach];
			auto positionFits = [&](float positionX)
			{
				if (positionX - halfWidth < sector->getCellX0() - 0.001f
					|| positionX + halfWidth > sector->getCellX1() + 1.0f + 0.001f
					|| endpoint.y < sector->getCellY0() - 0.001f
					|| endpoint.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
					return false;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(positionX));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(endpoint.y));
				return mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot();
			};

			// Unlike a Door, the Ladder endpoint itself must remain clear for
			// mounting and dismounting. Queue positions therefore begin at step 1;
			// admission later requires the selected Agent to have reached one of them.
			vector<Vector2> positions;
			bool scanLeft = true, scanRight = true;
			for (uint32_t step = 1; scanLeft || scanRight; ++step)
			{
				auto const distance = step * spacing;
				auto const left = endpoint.x - distance;
				auto const right = endpoint.x + distance;
				if (scanLeft)
				{
					scanLeft = positionFits(left);
					if (scanLeft) positions.push_back({ left, endpoint.y });
				}
				if (scanRight)
				{
					scanRight = positionFits(right);
					if (scanRight) positions.push_back({ right, endpoint.y });
				}
			}

			auto& lane = resource->mQueueLanes[approach];
			lane.sector = sectors[approach];
			lane.origin = endpoint;
			lane.direction = Vector2::UNIT_X;
			lane.extent = positions.empty() ? 0.0f : spacing;
			for (auto const& position : positions)
				lane.extent = max(lane.extent, abs(position.x - endpoint.x));
			lane.positions = std::move(positions);
			lane.positionOwners.assign(lane.positions.size(), {});
		}
	}

	bool World::configureDoorQueueLane(TraversalResourceId resourceId, SectorId sectorId,
		Vector2 origin, Vector2 direction, float extent)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("configureDoorQueueLane");
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !sectorId || sectorId.value > mSectors.size()
			|| extent < 0.0f || direction.length() < 0.001f)
		{
			throw invalid_argument("A door queue lane requires a door, source sector, direction, and non-negative extent");
		}
		direction.normalise();
		auto sector = mSectors[(size_t)sectorId.value - 1];
		auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		auto positionFits = [&](Vector2 const& position)
		{
			if (position.x - halfWidth < sector->getCellX0() - 0.001f
				|| position.x + halfWidth > sector->getCellX1() + 1.0f + 0.001f
				|| position.y < sector->getCellY0() - 0.001f
				|| position.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
			{
				return false;
			}
			auto const cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
			auto const cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
			return mLayers[sector->getLayerIndex()]->getCellDefinition(cellX, cellY).isTraversableOnFoot();
		};
		if (!positionFits(origin) || !positionFits(origin + direction * extent))
		{
			throw invalid_argument("Door queue lane does not fit inside its source sector");
		}

		QueueLane* lane = nullptr;
		for (auto& candidate : resource->mQueueLanes)
		{
			if (candidate.sector == sectorId)
			{
				lane = &candidate;
				break;
			}
			if (!candidate.sector && !lane)
			{
				lane = &candidate;
			}
		}
		if (!lane)
		{
			throw invalid_argument("A door traversal resource supports exactly two approach lanes");
		}
		if (!lane->queue.empty())
		{
			throw invalid_argument("An active door queue lane cannot be reconfigured");
		}
		lane->sector = sectorId;
		lane->origin = origin;
		lane->direction = direction;
		lane->extent = extent;
		lane->positions.clear();
		lane->positionOwners.clear();
		auto const spacing = (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
		for (float distance = 0.0f; distance <= extent + 0.001f; distance += spacing)
		{
			auto position = origin + direction * distance;
			if (!positionFits(position))
			{
				throw invalid_argument("A generated door queue position is outside its source sector");
			}
			lane->positions.push_back(position);
			lane->positionOwners.push_back({});
		}
		return true;
	}

	bool World::configureDoorCrossingLanes(TraversalResourceId resourceId, uint32_t laneCount)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("configureDoorCrossingLanes");
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || laneCount == 0)
		{
			throw invalid_argument("A door crossing requires at least one lane");
		}
		if (laneCount > resource->mDoor->getCellsWide())
		{
			throw invalid_argument("Door crossing lane count exceeds usable threshold width");
		}
		if (any_of(resource->mCrossingOwners.begin(), resource->mCrossingOwners.end(),
			[](TraversalRequestId owner) { return (bool)owner; }))
		{
			return false;
		}
		resource->mCrossingOwners.assign(laneCount, {});
		return true;
	}

	// Door open lease acquisition and release live in SimulationCoordinator
	// (ADR 0004). World forwards both the resource-reference form the
	// traversal machinery uses and the handle form external holders use.

	DoorOpenLeaseId World::acquireDoorOpenLease(TraversalResource& resource,
		DoorOpenLeaseKind kind, TraversalRequestId request)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.acquireDoorOpenLease(resource, kind, request);
	}

	bool World::releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.releaseDoorOpenLease(resource, lease);
	}

	DoorOpenLeaseId World::acquireDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseKind kind)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.acquireDoorOpenLease(resource, kind);
	}

	bool World::releaseDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseId lease)
	{
		invalidateSimulationSnapshot();
		return mSimulationCoordinator.releaseDoorOpenLease(resource, lease);
	}

	bool World::setDoorSensorObservation(TraversalResourceId resourceId, DoorSensorId sensor,
		DoorSensorObservation observation)
	{
		invalidateSimulationSnapshot();
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !sensor) return false;
		if (observation == DoorSensorObservation::Clear) resource->mSensorObservations.erase(sensor);
		else resource->mSensorObservations[sensor] = observation;
		return true;
	}

	bool World::setTraversalResourceEnabled(TraversalResourceId resourceId, bool enabled)
	{
		invalidateSimulationSnapshot();
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return false;
		if (resource->mEnabled == enabled
			&& ((!resource->mLift && !resource->mShuttle) || !resource->mLiftDraining)) return true;
		resource->mEnabled = enabled;
		if (!resource->mLift && !resource->mShuttle)
			return true;

		if (enabled)
		{
			resource->mLiftDraining = false;
			return true;
		}

		resource->mLiftDraining = true;
		vector<AgentId> occupants;
		for (auto occupant : resource->mOccupants) if (occupant) occupants.push_back(occupant);
		for (auto occupant : occupants)
			requestLiftPassengerSafeExit(occupant, TraversalFailureReason::ResourceDisabled);

		vector<TraversalRequestId> rejected;
		for (auto const& [requestId, request] : mTraversalRequests.entries())
		{
			auto authority = mTraversalResources.find(request->mResource);
			if (request->mState == TraversalRequestState::Pending
				&& (request->mResource == resourceId
					|| (authority && authority->mLiftCoordinator == resourceId))
				&& find(occupants.begin(), occupants.end(), request->mOwner) == occupants.end())
				rejected.push_back(requestId);
		}
		for (auto requestId : rejected)
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
		return true;
	}

	bool World::addTraversalControl(TraversalResourceId resourceId, InteractionPointId controlId)
	{
		invalidateSimulationSnapshot();
		beginStructuralEdit("addTraversalControl");
		auto resource = mTraversalResources.find(resourceId);
		auto control = mInteractionPoints.find(controlId);
		if (!resource || (!resource->mDoor && !resource->mExtensible) || !control)
		{
			return false;
		}
		if (find(resource->mControls.begin(), resource->mControls.end(), controlId) == resource->mControls.end())
		{
			resource->mControls.push_back(controlId);
			if (resource->mExtensible) resource->mExtensible->addExtensionControlSector(control->mSector);
			sort(resource->mControls.begin(), resource->mControls.end());
		}
		return true;
	}

	EntityLookup<TraversalResource> World::lookupTraversalResource(TraversalResourceId id)
	{
		auto entity = mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource>{ entity, {} }
			: EntityLookup<TraversalResource>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<TraversalResource const> World::lookupTraversalResource(TraversalResourceId id) const
	{
		auto entity = mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource const>{ entity, {} }
			: EntityLookup<TraversalResource const>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	TraversalResourceId World::getTraversalResourceId(Object const* object) const
	{
		if (!object) return {};
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			bool matches = resource->mDoor.get() == object || resource->mWindow.get() == object
				|| resource->mLadder.get() == object || resource->mForceBridge.get() == object
				|| resource->mLift.get() == object || resource->mShuttle.get() == object
				|| resource->mStairwell.get() == object;
			if (!matches) continue;
			return resource->mLiftCoordinator ? resource->mLiftCoordinator : id;
		}
		return {};
	}

	EntityRemovalResult World::removeTraversalResource(TraversalResourceId id)
	{
		invalidateSimulationSnapshot();
		auto found = lookupTraversalResource(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		bool structural = found.entity->mDoor || found.entity->mWindow || found.entity->mLadder
			|| found.entity->mForceBridge || found.entity->mLift || found.entity->mShuttle
			|| found.entity->mStairwell;
		if (!structural && mGraph)
			structural = any_of(mGraph->getEdges().begin(), mGraph->getEdges().end(),
				[id](auto const& edge) { return edge->getTraversalResourceId() == id; });
		if (structural) beginStructuralEdit("removeTraversalResource");
		auto hasOwner = [](auto const& values)
			{ return any_of(values.begin(), values.end(), [](auto value) { return (bool)value; }); };
		bool owned = hasOwner(found.entity->mOccupants)
			|| hasOwner(found.entity->mAdmissionReservations)
			|| hasOwner(found.entity->mCrossingOwners)
			|| hasOwner(found.entity->mVirtualBoundaryOwners)
			|| !found.entity->mAdmissionQueue.empty() || !found.entity->mOpenLeases.empty()
			|| !found.entity->mExtensionRequestLeases.empty()
			|| !found.entity->mExtensionOccupantLeases.empty()
			|| any_of(found.entity->mQueueLanes.begin(), found.entity->mQueueLanes.end(),
				[](auto const& lane) { return !lane.queue.empty(); })
			|| any_of(found.entity->mLiftStopRequestOwners.begin(), found.entity->mLiftStopRequestOwners.end(),
				[](auto const& owners) { return !owners.empty(); });
		if (owned)
			return { false, format("TraversalResource handle {} still has active ownership and cannot be replaced safely", id.value) };
		auto snapshot = makeTraversalResourceSnapshot(id, *found.entity);
		mTraversalResources.remove(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceRemoved;
		event.traversalResource = std::move(snapshot);
		mEvents.push_back(std::move(event));
		return { true, {} };
	}

	EntityLookup<TraversalRequest const> World::lookupTraversalRequest(TraversalRequestId id) const
	{
		auto entity = mTraversalRequests.find(id);
		return entity ? EntityLookup<TraversalRequest const>{ entity, {} }
			: EntityLookup<TraversalRequest const>{ nullptr, format("TraversalRequest handle {} is invalid or has been released", id.value) };
	}

	TraversalWaitingPolicy const& World::getTraversalWaitingPolicy() const
	{
		return mTraversalWaitingPolicy;
	}

	void World::setTraversalWaitingPolicy(TraversalWaitingPolicy policy)
	{
		invalidateSimulationSnapshot();
		if (policy.localGoalTimeoutTicks == 0 || policy.permitProgressTimeoutTicks == 0
			|| policy.replanIntervalTicks == 0 || policy.queueDelayPerAgentSeconds < 0.0f
			|| policy.replanEtaMarginSeconds < 0.0f)
		{
			throw invalid_argument("Traversal waiting policy durations must be positive and costs non-negative");
		}
		mTraversalWaitingPolicy = policy;
	}

	TraversalGeometryPolicy const& World::getTraversalGeometryPolicy() const
	{
		return mTraversalGeometryPolicy;
	}

	void World::setTraversalGeometryPolicy(TraversalGeometryPolicy policy)
	{
		if (!isfinite(policy.minimumQueueSeparation)
			|| !isfinite(policy.advanceStepThreshold)
			|| !isfinite(policy.overflowTailSeparation)
			|| !isfinite(policy.occupantClearance)
			|| policy.minimumQueueSeparation < CORE_RESOURCE_QUEUE_SLOT_PITCH
			|| policy.advanceStepThreshold < 0.0f
			|| policy.overflowTailSeparation < 0.0f
			|| policy.occupantClearance < 0.0f)
		{
			throw invalid_argument("Traversal geometry policy values must be finite and non-negative, and queue separation cannot be below the authored lane pitch");
		}
		invalidateSimulationSnapshot();
		mTraversalGeometryPolicy = policy;

		// Lift slots are authored when the traversal resource is built, while the
		// geometry policy remains configurable afterwards. Keep existing enclosed
		// cars and their car-side boarding lanes in sync with the World policy.
		for (auto const& entry : mTraversalResources.entries())
		{
			auto& resource = *entry.second;
			if (!resource.mLift || resource.mOpenPlatformLift || resource.mShuttle
				|| resource.mCapacityPositions.empty()) continue;
			auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
			auto const usableWidth = resource.mLift->getSize().x;
			auto const targets = packOccupants(resource.mCapacityPositions.size(), 0,
				{ halfAgentWidth, usableWidth - halfAgentWidth }, CORE_RESOURCE_SLOT_WIDTH,
				policy.occupantClearance, OccupantPackingOrder::Forward,
				OccupantPackingLayout::Compact);
			for (size_t i = 0; i < targets.size(); ++i)
				resource.mCapacityPositions[i].x = targets[i];

			for (auto const& stop : resource.mLiftStops)
			{
				auto landing = mTraversalResources.find(stop.landingResource);
				if (!landing) continue;
				for (auto& lane : landing->mQueueLanes)
				{
					if (lane.sector != resource.mLiftSector) continue;
					lane.positions.clear();
					for (auto const& position : resource.mCapacityPositions)
						lane.positions.push_back({ resource.mLift->getPosition().x + position.x,
							stop.globalPosition + position.y });
					lane.positionOwners.resize(lane.positions.size());
					if (!lane.positions.empty())
					{
						lane.origin = lane.positions.front();
						lane.direction = Vector2::UNIT_X;
						lane.extent = lane.positions.back().x - lane.positions.front().x;
					}
				}
			}
		}
	}

	uint32_t World::countStandingAgentsOnEscalator(Agent const* observer, Edge const* edge) const
	{
		if (!observer || !edge) return 0;
		auto const count = edge->getStandingRouteAgents();
		return count - (observer->isActive() && observer->isStandingOnEscalator(edge) ? 1u : 0u);
	}

	void World::captureTransportRouteQueues(TraversalResource const& resource) const
	{
		if (resource.mCapturedRouteQueueEpoch == resource.mRouteQueueEpoch
			&& resource.mRouteQueuedByStop.size() == resource.mLiftStops.size()
			&& resource.mRouteQueuedByShuttleDoor.size() == resource.mShuttleDoors.size()) return;
		resource.mRouteQueuedByStop.assign(resource.mLiftStops.size(), 0);
		resource.mRouteQueuedByShuttleDoor.assign(resource.mShuttleDoors.size(), 0);
		for (auto id : resource.mAdmissionQueue)
		{
			auto request = mTraversalRequests.find(id);
			if (!request) continue;
			if (!resource.mShuttle)
			{
				auto stop = findLiftStop(resource, request->mSourceEndpoint);
				if (stop < resource.mRouteQueuedByStop.size()) ++resource.mRouteQueuedByStop[stop];
				continue;
			}
			for (size_t index = 0; index < resource.mShuttleDoors.size(); ++index)
			{
				auto const& door = resource.mShuttleDoors[index];
				if (request->mSourceSector != door.locationSector) continue;
				if (any_of(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
					[&](auto const& candidate) { return candidate.landingResource == request->mResource
						&& candidate.stopIndex == door.stopIndex
						&& candidate.accessZoneIndex == door.accessZoneIndex; }))
					++resource.mRouteQueuedByShuttleDoor[index];
			}
		}
		resource.mCapturedRouteQueueEpoch = resource.mRouteQueueEpoch;
		++resource.mRouteQueueSnapshotBuildCount;
	}

	optional<ShuttleRouteAccessObservation> World::observeShuttleAccess(
		TraversalResourceId resourceId, Vector2 const& endpoint, bool includeLocalQueue) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return nullopt;
		auto coordinator = resource->mLiftCoordinator
			? mTraversalResources.find(resource->mLiftCoordinator) : resource;
		if (!coordinator || !coordinator->mShuttle) return nullopt;
		auto door = find_if(coordinator->mShuttleDoors.begin(), coordinator->mShuttleDoors.end(),
			[&](auto const& candidate)
			{
				return candidate.landingResource == resourceId
					|| abs(coordinator->mLiftStops[candidate.stopIndex].globalPosition
						+ candidate.carriagePosition - endpoint.x) < 0.001f;
			});
		if (door == coordinator->mShuttleDoors.end()) return nullopt;
		// Stops, Carriages and access-zone associations are fixed for this resource
		// identity. Structural edits replace the resource; queue changes do not
		// recompute these authored facts. During initial construction the number
		// of mapped Doors can still grow before the Graph is published.
		if (coordinator->mRouteShuttleDoorCapacities.size() != coordinator->mShuttleDoors.size())
		{
			coordinator->mRouteShuttleDoorCapacities.assign(coordinator->mShuttleDoors.size(), 0);
			for (size_t index = 0; index < coordinator->mShuttleDoors.size(); ++index)
			{
				auto const& access = coordinator->mShuttleDoors[index];
				for (auto const& carriage : coordinator->mShuttleCarriages)
					if (any_of(coordinator->mShuttleDoors.begin(), coordinator->mShuttleDoors.end(),
						[&](auto const& candidate) { return candidate.stopIndex == access.stopIndex
							&& candidate.accessZoneIndex == access.accessZoneIndex
							&& candidate.carriageIndex == carriage.index; }))
						coordinator->mRouteShuttleDoorCapacities[index] += carriage.capacity;
			}
		}
		auto const capacity = coordinator->mRouteShuttleDoorCapacities[door - coordinator->mShuttleDoors.begin()];
		uint32_t queued = 0;
		if (includeLocalQueue)
		{
			captureTransportRouteQueues(*coordinator);
			queued = coordinator->mRouteQueuedByShuttleDoor[door - coordinator->mShuttleDoors.begin()];
		}
		return ShuttleRouteAccessObservation{ queued, max(1u, capacity),
			(float)coordinator->mLiftMinimumDwellTicks * getFixedTimestep(),
			coordinator->mLiftStops[door->stopIndex].globalPosition };
	}

	optional<LiftRouteAccessObservation> World::observeLiftAccess(
		TraversalResourceId resourceId, Vector2 const& sourceEndpoint, bool includeLocalQueue) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return nullopt;
		auto coordinator = resource->mLiftCoordinator
			? mTraversalResources.find(resource->mLiftCoordinator) : resource;
		if (!coordinator || !coordinator->mLift || coordinator->mLiftStops.empty()) return nullopt;

		auto stop = findLiftStop(*coordinator, sourceEndpoint);
		if (stop >= coordinator->mLiftStops.size()) return nullopt;
		uint32_t queued = 0;
		if (includeLocalQueue)
		{
			captureTransportRouteQueues(*coordinator);
			queued = coordinator->mRouteQueuedByStop[stop];
		}
		return LiftRouteAccessObservation{ queued, max(1u, coordinator->mCapacity),
			(float)coordinator->mLiftMinimumDwellTicks * getFixedTimestep() };
	}

	void World::captureDoorRouteObservation(TraversalResource const& resource) const
	{
		// Ordinary and Bulkhead Doors have exactly two authored approaches.
		// Compare O(1) scalar facts, never requests or Agents. This also catches
		// direct authored reconfiguration without fragile queue-mutation hooks.
		TraversalResource::DoorRouteObservationKey key;
		for (size_t index = 0; index < 2; ++index)
		{
			auto const& lane = resource.mQueueLanes[index];
			key.sectors[index] = lane.sector;
			key.queued[index] = lane.queue.size();
			key.positions[index] = lane.positions.size();
		}
		key.crossingLanes = max<size_t>(1, resource.mCrossingOwners.size());
		key.open = resource.mDoor->isOpen();
		key.activationMode = resource.mDoor->getActivationMode();
		if (resource.mDoorRouteObservationEpoch && key == resource.mDoorRouteObservationKey) return;
		resource.mDoorRouteObservationKey = key;
		++resource.mDoorRouteObservationEpoch;
		resource.mDoorRouteServiceBatches = static_cast<float>(
			(key.queued[0] + key.queued[1] + key.crossingLanes - 1) / key.crossingLanes);
		for (size_t index = 0; index < 2; ++index)
		{
			size_t queued = 0, positions = 0;
			for (size_t other = 0; other < 2; ++other)
				if (key.sectors[index] == key.sectors[other])
				{
					queued += key.queued[other];
					positions += key.positions[other];
				}
			resource.mDoorRouteDensity[index] = static_cast<float>(queued) / max<size_t>(1, positions);
		}
	}

	float World::observeAccessZoneDensity(TraversalResourceId resourceId,
		SectorId sourceSector) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return 0.0f;
		if (resource->mDoor && resource->mQueueLanes.size() == 2)
		{
			captureDoorRouteObservation(*resource);
			for (size_t index = 0; index < 2; ++index)
				if (resource->mDoorRouteObservationKey.sectors[index] == sourceSector)
					return resource->mDoorRouteDensity[index];
			return 0.0f;
		}
		size_t queued = 0;
		size_t positions = 0;
		for (auto const& lane : resource->mQueueLanes)
		{
			if (lane.sector != sourceSector) continue;
			queued += lane.queue.size();
			positions += lane.positions.size();
		}
		return (float)queued / (float)max<size_t>(1, positions);
	}

	float World::estimateTraversalDelay(TraversalResourceId resourceId, SectorId sourceSector) const
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return 0.0f;

		if (resource->mSecurityScanner)
		{
			// Only the entry approach queue is observable: neither occupancy,
			// reservations nor a remote live phase are route-planning knowledge.
			size_t ahead = 0;
			for (auto const& lane : resource->mQueueLanes)
				if (lane.sector == sourceSector) ahead += lane.queue.size();
			auto const& chamber = *resource->mSecurityScanner;
			auto capacity = std::max<size_t>(1, resource->mCapacity);
			return static_cast<float>((ahead + capacity - 1) / capacity) * (4 * CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME
				+ chamber.getPreDelaySeconds() + chamber.getScanSeconds() + chamber.getPostPauseSeconds());
		}
		if (resource->mAirlock)
		{
			// Only this approach's queue is locally observable. Opposing demand
			// and chamber reservations are not a remote queue oracle.
			size_t ahead = 0;
			for (auto const& lane : resource->mQueueLanes)
				if (lane.sector == sourceSector) ahead += lane.queue.size();
			auto const capacity = max<size_t>(1, resource->mCapacity);
			auto const batches = (ahead + capacity - 1) / capacity;
			return static_cast<float>(batches) * (4 * CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME
				+ 2 * resource->mAirlock->getCycleSeconds());
		}
		if (resource->mLiftCoordinator)
		{
			auto lift = mTraversalResources.find(resource->mLiftCoordinator);
			if (!lift) return 0.0f;
			auto stop = resource->mLiftStopIndex;
			float delay = mTraversalWaitingPolicy.queueDelayPerAgentSeconds
				* (float)count_if(lift->mAdmissionQueue.begin(), lift->mAdmissionQueue.end(),
					[&](TraversalRequestId id)
					{
						auto request = mTraversalRequests.find(id);
						return request && request->mSourceSector == sourceSector;
					}) / max(1u, lift->mCapacity);
			if (stop < lift->mLiftStops.size())
			{
				delay += abs(lift->mLiftPosition - lift->mLiftStops[stop].globalPosition)
					/ (lift->mShuttle ? lift->mShuttle->getSpeed() : lift->mLift->getSpeed());
				// Existing scheduled stops add stable preparation/service cost without
				// creating a reservation as a side effect of path search.
				for (uint32_t i = 0; i < lift->mLiftStops.size(); ++i)
					if (i != stop && !lift->mLiftStopRequestOwners[i].empty())
						delay += (float)lift->mLiftMinimumDwellTicks * getFixedTimestep();
			}
			return delay;
		}
		if (resource->mLift || resource->mShuttle)
		{
			float delay = (float)resource->mLiftMinimumDwellTicks * getFixedTimestep();
			for (auto const& owners : resource->mLiftStopRequestOwners)
				if (!owners.empty()) delay += mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
			return delay;
		}

		if (resource->mDoor && resource->mQueueLanes.size() == 2)
		{
			captureDoorRouteObservation(*resource);
			return resource->mDoorRouteServiceBatches * mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
		}
		size_t ahead = 0;
		for (auto const& lane : resource->mQueueLanes) ahead += lane.queue.size();
		auto lanes = max<size_t>(1, resource->mCrossingOwners.size());
		return (float)((ahead + lanes - 1) / lanes)
			* mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
	}

	// The tick pipeline - lift and door resource advancement, the simulation
	// phases, tick event publication, the simulation clock and event consumption -
	// lives in SimulationCoordinator (ADR 0004 stage 5), as does every snapshot
	// builder removed above. World keeps the forwards which still have callers
	// outside itself.

	// Per-tick interaction phases - device-operation advancement, allocation,
	// movement and result resolution - run in SimulationCoordinator (ADR 0004).
	void World::advanceDeviceOperations()
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.advanceDeviceOperations();
	}

	void World::pressPhysicalControl(InteractionPointId pointId)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.pressPhysicalControl(pointId);
	}

	void World::tryPressUpcomingDoorButton(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.tryPressUpcomingDoorButton(agent, movementStart, movementEnd);
	}

	void World::allocateInteractions()
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.allocateInteractions();
	}

	void World::moveInteractions(float frameTime)
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.moveInteractions(frameTime);
	}

	void World::updateInteractionResults()
	{
		invalidateSimulationSnapshot();
		mSimulationCoordinator.updateInteractionResults();
	}

	void World::update(float elapsedSeconds)
	{
		mSimulationCoordinator.update(elapsedSeconds);
	}

	bool World::advanceTick()
	{
		return mSimulationCoordinator.advanceTick();
	}

	bool World::advanceTicks(uint64_t count)
	{
		return mSimulationCoordinator.advanceTicks(count);
	}

	uint64_t World::getSimulationTick() const
	{
		return mSimulationCoordinator.getSimulationTick();
	}

	SimulationPhase World::getCurrentSimulationPhase() const
	{
		return mSimulationCoordinator.getCurrentSimulationPhase();
	}

	void World::setSimulationTimeScale(double scale)
	{
		if (std::isnan(scale)) return;
		mTimeScale = std::clamp(scale, 0.05, 100.0);
	}

	SimulationSnapshot const& World::getSimulationSnapshotView() const
	{
		if (!mSnapshotValid || mSnapshotMutationRevision != observationRevision)
		{
			mSnapshotCache = mSimulationCoordinator.getSimulationSnapshot();
			mSnapshotMutationRevision = observationRevision;
			mSnapshotValid = true;
			++mSnapshotBuildCount;
		}
		return mSnapshotCache;
	}

	SimulationSnapshot World::getSimulationSnapshot() const
	{
		return getSimulationSnapshotView();
	}

	vector<SimulationEvent> World::consumeSimulationEvents()
	{
		return mSimulationCoordinator.consumeSimulationEvents();
	}

} // core