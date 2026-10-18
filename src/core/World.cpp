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
#include "core/PlacementValidator.h"
#include "core/AgentGroupRegistry.h"
#include "core/PermissionRequirementStore.h"
#include "core/AuthorizationLedger.h"
#include "core/AgentTypeCatalog.h"
#include "core/MarkerStore.h"
#include "core/TraversalResourceRegistry.h"
#include "core/DeviceConditionStore.h"
#include "core/FurnitureStore.h"
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
#include "core/AgentProfileEditor.h"
#include "core/AgentTagService.h"
#include "core/WorldBuilder.h"
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
#include "core/RegistryBinding.h"
#include "core/SerializationException.h"


namespace core
{
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
		, mPlacementValidator(std::make_unique<PlacementValidator>(*this))
	, mAgentGroupRegistry(std::make_unique<AgentGroupRegistry>(*this))
	, mPermissionRequirementStore(std::make_unique<PermissionRequirementStore>(*this))
	, mAuthorizationLedger(std::make_unique<AuthorizationLedger>(*this))
	, mAgentTypeCatalog(std::make_unique<AgentTypeCatalog>(*this))
	, mMarkerStore(std::make_unique<MarkerStore>(*this))
	, mTraversalRegistry(std::make_unique<TraversalResourceRegistry>(*this))
	, mDeviceConditionStore(std::make_unique<DeviceConditionStore>(*this))
	, mFurnitureStore(std::make_unique<FurnitureStore>(*this))
	, mAgentTagService(std::make_unique<AgentTagService>(*this))
	, mAgentProfileEditor(std::make_unique<AgentProfileEditor>(*this))
	, mWorldBuilder(std::make_unique<WorldBuilder>(*this))
		, mAgentTypeRuntime(std::make_unique<AgentTypeRuntimeAdapter>(
			agentTypeRuntimeLimits))
		, mAgentBehaviourRuntime(std::make_unique<AgentBehaviourRuntimeAdapter>(
			behaviourRuntimeLimits))
	{
		// The registry bindings are configured before anything can reach
		// them; they hold references to the World-owned state, not the
		// state itself (#676, ADR 0022).
		configureAgentTagRegistryBinding();
		configureAgentBehaviourRegistryBinding();
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
		return PlacementValidator::dimensionsAreSupported(cellsWide, levelsHigh,
			layerCount, diagnostic);
	}

	World::~World()
	{
		// Close behaviour coroutines before registries and domain storage begin
		// destruction. Teardown is best-effort and cannot prevent World close.
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
			if (agent && agent->getBehaviourAssignment() && !agent->isPlayerControlled())
				mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
		modify();
		return true;
	}

	// The tag family policy: every tag-specific guard and clearing step the
	// shared binding runs at the point where the previous duplicated method
	// performed it. The binding keeps the ordering contract; the policy keeps
	// the tag semantics exactly as they were (#676).
	void World::configureAgentTagRegistryBinding()
	{
		RegistryBinding<AgentTagRegistry>::Policy policy;
		policy.label = "Agent tag registry";
		policy.preflightAttach = [this](AgentTagRegistry& registry, bool)
			-> CommitHook
		{
			if (getAgentTagAssignmentCount() != 0)
			{
				throw invalid_argument(
					"Cannot switch Agent tag registries while Agent tag assignments exist; use the confirmed destructive action to clear assignments and samples first");
			}
			string diagnostic;
			if (!agentTagAssignmentsAreValid(registry, &diagnostic))
				throw invalid_argument(diagnostic);
			return {};
		};
		policy.preflightAttachAndClear = [this](AgentTagRegistry&)
			-> CommitHook
		{
			string diagnostic;
			if (!allAgentTagAssignmentsCanBeCleared(&diagnostic))
				throw invalid_argument(diagnostic);
			// Construct and register the replacement before changing any
			// authored state. Everything after registration is non-refusing,
			// so clearing assignments, clearing samples, and changing the
			// namespace commit as one operation.
			return [this] { clearAllAgentTagAssignmentsAndSamples(); };
		};
		policy.guardDetach = [this]
		{
			if (getAgentTagAssignmentCount() != 0 || getAgentTagSampleCount() != 0)
			{
				throw invalid_argument(
					"Cannot detach the Agent tag registry while assignments or samples exist; use the confirmed destructive action to clear them first");
			}
		};
		policy.clearAssignmentsBeforeDetach = [this]
		{
			clearAllAgentTagAssignmentsAndSamples();
			return CommitHook{};
		};
		policy.resolveBody = [this](shared_ptr<AgentTagRegistry> registry,
			shared_ptr<AgentTagRegistry>& registrySlot)
		{
			// Reconciliation is completed before registration, so a refusal
			// neither exposes this World through the shared registry nor
			// changes any Agent.
			reconcileAgentTagAssignments(*registry);
			if (registrySlot) registrySlot->unregisterWorld(*this);
			registrySlot = std::move(registry);
			registrySlot->registerWorld(*this);

			// Saved route intent stays unevaluated until reconciliation has
			// made every tag-supplied routing property and persisted sample
			// effective (#194, #221).
			rebuildRestoredAgentPaths();
		};
		policy.preflightReplaceCopy = [this](string const&,
			shared_ptr<AgentTagRegistry> const& replacement)
			-> CommitHook
		{
			if (!mAgentTagRegistry || !mAgentTagRegistryReference)
				throw invalid_argument(
					"The World has no attached Agent tag registry to copy");
			if (!replacement
				|| !AgentTagRegistry::uuidIsValid(replacement->getUuid()))
				throw invalid_argument(
					"Cannot attach an invalid Agent tag registry copy");
			if (replacement == mAgentTagRegistry
				|| replacement->getUuid() == mAgentTagRegistry->getUuid())
				throw invalid_argument(
					"An Agent tag registry copy must have a new UUID");
			if (!mAgentTagRegistry->hasEquivalentDefinitions(*replacement))
				throw invalid_argument(
					"An Agent tag registry copy must preserve every definition and allocator");

			string diagnostic;
			if (!agentTagAssignmentsAreValid(*replacement, &diagnostic))
				throw invalid_argument(diagnostic);
			return {};
		};
		mAgentTagRegistryBinding = std::make_unique<RegistryBinding<AgentTagRegistry>>(
			*this, std::move(policy), mAgentTagRegistryReference,
			mAgentTagRegistry);
	}

	// The behaviour family policy: pause gates, namespace-use guards, schema
	// inspection, candidate-runtime preflight, and the live runtime swap the
	// shared binding runs at exactly the points where the previous
	// duplicated methods performed them (#676).
	void World::configureAgentBehaviourRegistryBinding()
	{
		RegistryBinding<AgentBehaviourRegistry>::Policy policy;
		policy.label = "Agent behaviour registry";
		policy.requirePausedForAttach = [this]
		{
			if (!isSimulationPaused())
				throw invalid_argument(
					"Pause the World before changing its Agent behaviour registry");
		};
		policy.requirePausedForAttachAndClear = [this]
		{
			if (!isSimulationPaused())
				throw invalid_argument(
					"Pause the World before replacing its Agent behaviour registry");
		};
		policy.requirePausedForDetach = [this]
		{
			if (!isSimulationPaused())
				throw invalid_argument(
					"Pause the World before detaching its Agent behaviour registry");
		};
		policy.requirePausedForReplaceCopy = [this]
		{
			if (!isSimulationPaused())
				throw invalid_argument(
					"Pause the World before replacing its Agent behaviour registry with a Save As copy");
		};
		// A behaviour switch that keeps the same persisted reference is not a
		// World change; only a changed reference dirties the document.
		policy.modifyOnlyIfReferenceChanged = true;
		// The destructive switch must never skip its clearing because the
		// caller passed the already-attached registry.
		policy.attachAndClearIdenticalIsNoOp = false;
		policy.preflightAttach = [this](AgentBehaviourRegistry& registry,
			bool sameNamespace) -> CommitHook
		{
			if (countAgentBehaviourAssignments() != 0 && !sameNamespace)
			{
				throw invalid_argument(
					"Cannot replace a used Agent behaviour registry; use the confirmed destructive action to clear every assignment and configuration first");
			}
			string assignmentDiagnostic;
			if (!inspectAgentBehaviourAssignments(registry,
				&assignmentDiagnostic))
				throw invalid_argument(assignmentDiagnostic);

			// Build every assigned factory in a private candidate runtime
			// before the shared dependency list, persisted reference,
			// assignments, or live runtime changes. A repaired expected
			// package therefore becomes usable as one operation and a bad
			// factory leaves the unresolved World untouched.
			unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
			vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
			if (!AgentBehaviourRuntimeAdapter::prepareReload(*this, registry,
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
			return [this, candidate = std::move(candidateRuntime)]() mutable
			{
				mAgentBehaviourRuntime->teardownAll(*this,
					AgentBehaviourTeardownReason::Reload);
				candidate->appendDiagnostics(
					mAgentBehaviourRuntime->consumeDiagnostics());
				mAgentBehaviourRuntime = std::move(candidate);
			};
		};
		policy.afterAttach = [this]
		{
			mAgentBehaviourDependencyDiagnostic.clear();
			for (auto const& [agentId, agent] : mAgents.entries())
				if (agent && agent->getBehaviourAssignment()
					&& !agent->isPlayerControlled())
					mSimulationCoordinator.clearAgentMovementForBehaviourEdit(
						agentId);
		};
		policy.preflightAttachAndClear = [this](AgentBehaviourRegistry&)
			-> CommitHook
		{
			// Allocation and shared registration are the only potentially
			// refusing in-memory steps. Complete them before any authored
			// configuration is lost.
			auto candidateRuntime = make_unique<AgentBehaviourRuntimeAdapter>(
				mAgentBehaviourRuntime->getLimits());
			return [this, candidate = std::move(candidateRuntime)]() mutable
			{
				mAgentBehaviourRuntime->teardownAll(*this,
					AgentBehaviourTeardownReason::Unassignment);
				candidate->appendDiagnostics(
					mAgentBehaviourRuntime->consumeDiagnostics());
				for (auto const& [agentId, agent] : mAgents.entries())
				{
					if (!agent || !agent->getBehaviourAssignment()
						|| agent->isPlayerControlled()) continue;
					mSimulationCoordinator.clearAgentMovementForBehaviourEdit(
						agentId);
					agent->clearBehaviourAssignment();
				}
				mAgentBehaviourRuntime = std::move(candidate);
			};
		};
		policy.afterAttachAndClear = [this]
		{
			mAgentBehaviourDependencyDiagnostic.clear();
		};
		policy.afterReplaceCopy = [this]
		{
			// The Save As swap resolves the dependency: the copied package
			// is attached, so any stale unavailable diagnostic is cleared.
			mAgentBehaviourDependencyDiagnostic.clear();
		};
		policy.guardDetach = [this]
		{
			if (countAgentBehaviourAssignments() != 0)
				throw invalid_argument(
					"Cannot detach a used Agent behaviour registry; use the confirmed destructive action to clear every assignment and configuration first");
		};
		policy.clearAssignmentsBeforeDetach = [this]
		{
			auto candidateRuntime = make_unique<AgentBehaviourRuntimeAdapter>(
				mAgentBehaviourRuntime->getLimits());
			mAgentBehaviourRuntime->teardownAll(*this,
				AgentBehaviourTeardownReason::Unassignment);
			candidateRuntime->appendDiagnostics(
				mAgentBehaviourRuntime->consumeDiagnostics());
			for (auto const& [agentId, agent] : mAgents.entries())
			{
				if (!agent || !agent->getBehaviourAssignment()
					|| agent->isPlayerControlled()) continue;
				mSimulationCoordinator.clearAgentMovementForBehaviourEdit(
					agentId);
				agent->clearBehaviourAssignment();
			}
			return [this, candidate = std::move(candidateRuntime)]() mutable
			{
				mAgentBehaviourRuntime = std::move(candidate);
			};
		};
		policy.afterDetach = [this]
		{
			mAgentBehaviourDependencyDiagnostic.clear();
		};
		policy.resolveBody = [this](shared_ptr<AgentBehaviourRegistry> registry,
			shared_ptr<AgentBehaviourRegistry>& registrySlot)
		{
			auto previousRegistry = registrySlot;
			auto const registryChanges = previousRegistry != registry;
			if (registryChanges) registry->registerWorld(*this);
			auto rollbackRegistry = [&]
			{
				if (registryChanges) registry->unregisterWorld(*this);
			};

			AgentBehaviourSchemaMigrationPreview preview;
			string previewDiagnostic;
			if (!registry->previewDefinitionsFrom(*registry, preview,
				&previewDiagnostic))
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
				if (item.compatibility
					== AgentBehaviourSchemaCompatibility::Incompatible)
				{
					string fields;
					for (auto const& field : item.fields)
						fields += (fields.empty() ? "" : ", ") + field.path
							+ " (" + field.diagnostic + ")";
					mismatches.push_back(format(
						"World '{}' / Agent '{}' ({}) / {}: {}",
						getName(), item.agentName, item.agent.value,
						item.behaviourName, fields));
					continue;
				}
				auto const* agent = mAgents.find(item.agent);
				auto const* definition =
					registry->lookupAgentBehaviour(item.behaviour);
				if (!agent || !agent->getBehaviourAssignment() || !definition)
				{
					rollbackRegistry();
					throw invalid_argument(
						"An Agent behaviour reconciliation target disappeared");
				}
				AgentBehaviourConfiguration normalized;
				string validation;
				if (!validateAgentBehaviourAssignmentAgainst(*registry,
					item.behaviour, definition->getRevision(),
					agent->getBehaviourAssignment()->configuration,
					&normalized, &validation))
				{
					rollbackRegistry();
					throw invalid_argument(validation);
				}
				reconciled[item.agent] = AgentBehaviourAssignment{
					item.behaviour, definition->getRevision(),
					std::move(normalized) };
			}
			if (!mismatches.empty())
			{
				registrySlot = registry;
				mAgentBehaviourDependencyDiagnostic.clear();
				for (auto const& mismatch : mismatches)
					mAgentBehaviourDependencyDiagnostic +=
						(mAgentBehaviourDependencyDiagnostic.empty() ? ""
							: "\n") + mismatch;
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
			registrySlot = registry;
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
				if (agent && agent->getBehaviourAssignment()
					&& !agent->isPlayerControlled())
					mSimulationCoordinator.clearAgentMovementForBehaviourEdit(
						agentId);
		};
		policy.preflightReplaceCopy = [this](string const&,
			shared_ptr<AgentBehaviourRegistry> const& replacement)
			-> CommitHook
		{
			if (!mAgentBehaviourRegistry || !replacement)
				throw invalid_argument(
					"Save As requires attached source and copied Agent behaviour registries");
			if (replacement == mAgentBehaviourRegistry
				|| replacement->getUuid()
					== mAgentBehaviourRegistry->getUuid())
				throw invalid_argument(
					"An Agent behaviour registry copy must have a new UUID");
			if (!mAgentBehaviourRegistry->hasEquivalentDefinitions(
					*replacement))
				throw invalid_argument(
					"An Agent behaviour registry copy must preserve every definition, revision, and allocator");
			string assignmentDiagnostic;
			if (!inspectAgentBehaviourAssignments(*replacement,
				&assignmentDiagnostic))
				throw invalid_argument(assignmentDiagnostic);

			unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
			vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
			if (!AgentBehaviourRuntimeAdapter::prepareReload(*this,
				*replacement, candidateRuntime, runtimeDiagnostics))
				throw invalid_argument(
					"Copied Agent behaviour package runtime preflight failed");
			return [this, candidate = std::move(candidateRuntime)]() mutable
			{
				mAgentBehaviourRuntime->teardownAll(*this,
					AgentBehaviourTeardownReason::Reload);
				candidate->appendDiagnostics(
					mAgentBehaviourRuntime->consumeDiagnostics());
				mAgentBehaviourRuntime = std::move(candidate);
			};
		};
		policy.markUnavailable = [this](
			AgentBehaviourRegistryReference const& reference,
			string diagnostic)
		{
			// An admitted in-memory registry/runtime is not discarded merely
			// because a later disk resolution attempt failed. Freshly
			// deserialized Worlds have no attachment here; explicit
			// replacement failures therefore preserve the old dependency and
			// runtime as well.
			mAgentBehaviourDependencyDiagnostic = format(
				"Agent behaviour registry dependency '{}' is unavailable. {} authored assignment{} remain{} unresolved.\n{}",
				reference.resourceName,
				countAgentBehaviourAssignments(),
				countAgentBehaviourAssignments() == 1 ? "" : "s",
				countAgentBehaviourAssignments() == 1 ? "s" : "",
				std::move(diagnostic));
			mSimulationPaused = true;
		};
		mAgentBehaviourRegistryBinding =
			std::make_unique<RegistryBinding<AgentBehaviourRegistry>>(
				*this, std::move(policy),
				mAgentBehaviourRegistryReference, mAgentBehaviourRegistry);
	}

	// The tag/behaviour registry reference and attachment queries forward to
	// the shared RegistryBinding state machine (#676, ADR 0022).
	bool World::hasAgentTagRegistryReference() const
	{
		return mAgentTagRegistryBinding->hasReference();
	}

	bool World::hasAttachedAgentTagRegistry() const
	{
		return mAgentTagRegistryBinding->hasAttachedRegistry();
	}

	string const& World::getAgentTagRegistryResourceName() const
	{
		return mAgentTagRegistryBinding->resourceName();
	}

	string const& World::getExpectedAgentTagRegistryUuid() const
	{
		return mAgentTagRegistryBinding->expectedUuid();
	}

	shared_ptr<AgentTagRegistry> const& World::getAgentTagRegistry() const
	{
		return mAgentTagRegistryBinding->registry();
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
		mAgentTagRegistryBinding->attach(std::move(resourceName),
			std::move(registry));
	}

	void World::attachAgentTagRegistryAndClearAssignments(string resourceName,
		shared_ptr<AgentTagRegistry> registry)
	{
		mAgentTagRegistryBinding->attachAndClearAssignments(
			std::move(resourceName), std::move(registry));
	}

	void World::detachAgentTagRegistry()
	{
		mAgentTagRegistryBinding->detach();
	}

	void World::detachAgentTagRegistryAndClearAssignments()
	{
		mAgentTagRegistryBinding->detachAndClearAssignments();
	}

	void World::resolveAgentTagRegistry(shared_ptr<AgentTagRegistry> registry)
	{
		mAgentTagRegistryBinding->resolve(std::move(registry));
	}

	void World::replaceAgentTagRegistryWithIndependentCopy(string resourceName,
		shared_ptr<AgentTagRegistry> registry)
	{
		mAgentTagRegistryBinding->replaceWithIndependentCopy(
			std::move(resourceName), std::move(registry));
	}

	bool World::hasAgentBehaviourRegistryReference() const
	{
		return mAgentBehaviourRegistryBinding->hasReference();
	}

	bool World::hasAttachedAgentBehaviourRegistry() const
	{
		return mAgentBehaviourRegistryBinding->hasAttachedRegistry();
	}

	string const& World::getAgentBehaviourRegistryResourceName() const
	{
		return mAgentBehaviourRegistryBinding->resourceName();
	}

	string const& World::getExpectedAgentBehaviourRegistryUuid() const
	{
		return mAgentBehaviourRegistryBinding->expectedUuid();
	}

	shared_ptr<AgentBehaviourRegistry> const& World::getAgentBehaviourRegistry() const
	{
		return mAgentBehaviourRegistryBinding->registry();
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
			if (behaviourId == PlayerControlledBehaviour)
			{
				if (revision != PlayerControlledBehaviourRevision || !configuration.empty())
					return reject("Player controlled requires revision 1 and no configuration");
				if (normalized) normalized->clear();
				return true;
			}
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
		mAgentBehaviourRegistryBinding->attach(std::move(resourceName),
			std::move(registry));
	}

	void World::attachAgentBehaviourRegistryAndClearAssignments(
		string resourceName, shared_ptr<AgentBehaviourRegistry> registry)
	{
		mAgentBehaviourRegistryBinding->attachAndClearAssignments(
			std::move(resourceName), std::move(registry));
	}

	void World::detachAgentBehaviourRegistry()
	{
		mAgentBehaviourRegistryBinding->detach();
	}

	void World::detachAgentBehaviourRegistryAndClearAssignments()
	{
		mAgentBehaviourRegistryBinding->detachAndClearAssignments();
	}

	void World::resolveAgentBehaviourRegistry(
		shared_ptr<AgentBehaviourRegistry> registry)
	{
		mAgentBehaviourRegistryBinding->resolve(std::move(registry));
	}

	void World::markAgentBehaviourRegistryUnavailable(string diagnostic)
	{
		mAgentBehaviourRegistryBinding->markUnavailable(std::move(diagnostic));
	}

	void World::replaceAgentBehaviourRegistryWithIndependentCopy(
		string resourceName, shared_ptr<AgentBehaviourRegistry> registry)
	{
		mAgentBehaviourRegistryBinding->replaceWithIndependentCopy(
			std::move(resourceName), std::move(registry));
	}

	bool World::validateAgentBehaviourAssignment(AgentBehaviourId behaviour,
		uint64_t revision, AgentBehaviourConfiguration const& configuration,
		AgentBehaviourConfiguration* normalized, string* diagnostic) const
	{
		if (behaviour == PlayerControlledBehaviour)
		{
			bool valid = revision == PlayerControlledBehaviourRevision && configuration.empty();
			if (diagnostic) *diagnostic = valid ? "" : "Player controlled requires revision 1 and no configuration";
			if (valid && normalized) normalized->clear();
			return valid;
		}
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
			if (agent && agent->getBehaviourAssignment() && !agent->isPlayerControlled()) ++count;
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
		if (!mAgentBehaviourRuntime->isInstanceDisabled(id)
			&& !mAgentBehaviourRuntime->isInstanceCompleted(id)) return true;
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
		return mAgentTagService->inspectAssignments(registry, allowSampleReconciliation,
			repairs, diagnostic);
	}

	bool World::agentTagAssignmentsAreValid(AgentTagRegistry const& registry,
		string* diagnostic) const
	{
		return mAgentTagService->assignmentsAreValid(registry, diagnostic);
	}

	void World::applyAgentTagReconciliations(
		vector<AgentTagReconciliation> const& repairs)
	{
		mAgentTagService->applyReconciliations(repairs);
	}

	void World::reconcileAgentTagAssignments(AgentTagRegistry const& registry)
	{
		mAgentTagService->reconcileAssignments(registry);
	}

	uint32_t World::countAgentTagAssignments(AgentTagId id) const
	{
		return mAgentTagService->countAssignments(id);
	}

	void World::clearAgentTagAssignments(AgentTagId id)
	{
		mAgentTagService->clearAssignments(id);
	}

	bool World::allAgentTagAssignmentsCanBeCleared(string* diagnostic) const
	{
		return mAgentTagService->allAssignmentsCanBeCleared(diagnostic);
	}

	void World::clearAllAgentTagAssignmentsAndSamples()
	{
		mAgentTagService->clearAllAssignmentsAndSamples();
	}

	namespace
	{
		// Builds one plain tag-sample row: the shared add/clear paths drive
		// the Agent's per-property sample accessors through these pointers,
		// exactly as the replaced facade methods did by hand. There is no
		// per-property extra today - the Height modifier's pose sync already
		// lives inside the Agent's own sample accessors.
		TagSampleDescriptor plainTagSample(
			SampledAgentPropertyType type,
			std::optional<AgentPropertySample> const& (Agent::*read)() const,
			void (Agent::*write)(AgentPropertySample),
			void (Agent::*clear)())
		{
			TagSampleDescriptor descriptor;
			descriptor.type = type;
			descriptor.readSample = [read](Agent const& agent)
				-> std::optional<AgentPropertySample> const&
				{ return (agent.*read)(); };
			descriptor.writeSample = [write](Agent& agent,
				AgentPropertySample const& sample) { (agent.*write)(sample); };
			descriptor.clearSample = [clear](Agent& agent) { (agent.*clear)(); };
			return descriptor;
		}
	}

	// The one tag-sample table (ADR 0022, #677): every addAgentTag*Samples /
	// clearAgentTag*Samples facade method names its row and the shared paths
	// below apply it. Built once on first use.
	TagSampleTable const& World::tagSampleTable()
	{
		static TagSampleTable const table = []()
		{
			TagSampleTable t;
			t.walkSpeedModifier = plainTagSample(
				SampledAgentPropertyType::WalkSpeedModifier,
				&Agent::getWalkSpeedModifierSample,
				&Agent::setWalkSpeedModifierSample,
				&Agent::clearWalkSpeedModifierSample);
			t.heightModifier = plainTagSample(
				SampledAgentPropertyType::HeightModifier,
				&Agent::getHeightModifierSample,
				&Agent::setHeightModifierSample,
				&Agent::clearHeightModifierSample);
			t.stairSpeedModifier = plainTagSample(
				SampledAgentPropertyType::StairSpeedModifier,
				&Agent::getStairSpeedModifierSample,
				&Agent::setStairSpeedModifierSample,
				&Agent::clearStairSpeedModifierSample);
			t.ladderSpeedModifier = plainTagSample(
				SampledAgentPropertyType::LadderSpeedModifier,
				&Agent::getLadderSpeedModifierSample,
				&Agent::setLadderSpeedModifierSample,
				&Agent::clearLadderSpeedModifierSample);
			t.interactionAversion = plainTagSample(
				SampledAgentPropertyType::InteractionAversion,
				&Agent::getInteractionAversionSample,
				&Agent::setInteractionAversionSample,
				&Agent::clearInteractionAversionSample);
			t.effortAversion = plainTagSample(
				SampledAgentPropertyType::EffortAversion,
				&Agent::getEffortAversionSample,
				&Agent::setEffortAversionSample,
				&Agent::clearEffortAversionSample);
			t.waitingAversion = plainTagSample(
				SampledAgentPropertyType::WaitingAversion,
				&Agent::getWaitingAversionSample,
				&Agent::setWaitingAversionSample,
				&Agent::clearWaitingAversionSample);
			t.crowdAversion = plainTagSample(
				SampledAgentPropertyType::CrowdAversion,
				&Agent::getCrowdAversionSample,
				&Agent::setCrowdAversionSample,
				&Agent::clearCrowdAversionSample);
			t.riskAversion = plainTagSample(
				SampledAgentPropertyType::RiskAversion,
				&Agent::getRiskAversionSample,
				&Agent::setRiskAversionSample,
				&Agent::clearRiskAversionSample);
			t.routeFamiliarity = plainTagSample(
				SampledAgentPropertyType::RouteFamiliarity,
				&Agent::getRouteFamiliaritySample,
				&Agent::setRouteFamiliaritySample,
				&Agent::clearRouteFamiliaritySample);
			t.routePersistence = plainTagSample(
				SampledAgentPropertyType::RoutePersistence,
				&Agent::getRoutePersistenceSample,
				&Agent::setRoutePersistenceSample,
				&Agent::clearRoutePersistenceSample);
			t.minimumRoutePlanningTime = plainTagSample(
				SampledAgentPropertyType::MinimumRoutePlanningTime,
				&Agent::getMinimumRoutePlanningTimeSample,
				&Agent::setMinimumRoutePlanningTimeSample,
				&Agent::clearMinimumRoutePlanningTimeSample);
			t.maximumRoutePlanningTime = plainTagSample(
				SampledAgentPropertyType::MaximumRoutePlanningTime,
				&Agent::getMaximumRoutePlanningTimeSample,
				&Agent::setMaximumRoutePlanningTimeSample,
				&Agent::clearMaximumRoutePlanningTimeSample);
			return t;
		}();
		return table;
	}

	// The one add path: every Agent assigned the tag gets a fresh sample of
	// the definition's range, stamped with the descriptor's property type,
	// the tag, and the definition revision (ADR 0012).
	void World::applyAgentTagSamples(TagSampleDescriptor const& descriptor,
		AgentTagId id, AgentModifierRange const& range, uint64_t revision)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !agent->hasAgentTag(id)) continue;
			descriptor.writeSample(*agent, AgentPropertySample{
				descriptor.type, id, revision, sampleAgentModifier(range) });
			changed = true;
		}
		if (changed) modify();
	}

	// The one clear path: only samples sourced from this tag are dropped;
	// individual overrides and other tags' samples are untouched.
	void World::clearAgentTagSamples(TagSampleDescriptor const& descriptor,
		AgentTagId id)
	{
		invalidateSimulationSnapshot();
		bool changed{ false };
		for (auto& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			if (!agent || !descriptor.readSample(*agent)
				|| descriptor.readSample(*agent)->sourceTag != id) continue;
			descriptor.clearSample(*agent);
			changed = true;
		}
		if (changed) modify();
	}

	void World::addAgentTagWalkSpeedModifierSamples(AgentTagId id,
		AgentWalkSpeedModifierProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().walkSpeedModifier, id,
			property.range, property.revision);
	}

	void World::clearAgentTagWalkSpeedModifierSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().walkSpeedModifier, id);
	}

	bool World::canApplyAgentTagHeightModifier(AgentTagId id, std::optional<AgentModifierRange> range,
		std::string* diagnostic) const
	{
		return mAgentTagService->canApplyHeightModifier(id, range, diagnostic);
	}

	void World::addAgentTagHeightModifierSamples(AgentTagId id,
		AgentHeightModifierProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().heightModifier, id,
			property.range, property.revision);
	}

	void World::clearAgentTagHeightModifierSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().heightModifier, id);
	}

	void World::addAgentTagStairSpeedModifierSamples(AgentTagId id,
		AgentStairSpeedModifierProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().stairSpeedModifier, id,
			property.range, property.revision);
	}

	void World::clearAgentTagStairSpeedModifierSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().stairSpeedModifier, id);
	}

	void World::addAgentTagLadderSpeedModifierSamples(AgentTagId id,
		AgentLadderSpeedModifierProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().ladderSpeedModifier, id,
			property.range, property.revision);
	}

	void World::clearAgentTagLadderSpeedModifierSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().ladderSpeedModifier, id);
	}

	void World::addAgentTagInteractionAversionSamples(AgentTagId id,
		AgentInteractionAversionProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().interactionAversion, id,
			property.range, property.revision);
	}

	void World::clearAgentTagInteractionAversionSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().interactionAversion, id);
	}

	void World::addAgentTagEffortAversionSamples(AgentTagId id,
		AgentEffortAversionProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().effortAversion, id,
			property.range, property.revision);
	}

	void World::clearAgentTagEffortAversionSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().effortAversion, id);
	}

	void World::addAgentTagWaitingAversionSamples(AgentTagId id,
		AgentWaitingAversionProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().waitingAversion, id,
			property.range, property.revision);
	}

	void World::clearAgentTagWaitingAversionSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().waitingAversion, id);
	}

	void World::addAgentTagCrowdAversionSamples(AgentTagId id,
		AgentCrowdAversionProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().crowdAversion, id,
			property.range, property.revision);
	}

	void World::clearAgentTagCrowdAversionSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().crowdAversion, id);
	}

	void World::addAgentTagRiskAversionSamples(AgentTagId id,
		AgentRiskAversionProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().riskAversion, id,
			property.range, property.revision);
	}

	void World::clearAgentTagRiskAversionSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().riskAversion, id);
	}

	void World::addAgentTagRouteFamiliaritySamples(AgentTagId id,
		AgentRouteFamiliarityProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().routeFamiliarity, id,
			property.range, property.revision);
	}

	void World::clearAgentTagRouteFamiliaritySamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().routeFamiliarity, id);
	}

	void World::addAgentTagRoutePersistenceSamples(AgentTagId id,
		AgentRoutePersistenceProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().routePersistence, id,
			property.range, property.revision);
	}

	void World::clearAgentTagRoutePersistenceSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().routePersistence, id);
	}

	void World::addAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id,
		AgentMinimumRoutePlanningTimeProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().minimumRoutePlanningTime, id,
			property.range, property.revision);
	}

	void World::clearAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().minimumRoutePlanningTime, id);
	}

	void World::addAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id,
		AgentMaximumRoutePlanningTimeProperty const& property)
	{
		applyAgentTagSamples(tagSampleTable().maximumRoutePlanningTime, id,
			property.range, property.revision);
	}

	void World::clearAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id)
	{
		clearAgentTagSamples(tagSampleTable().maximumRoutePlanningTime, id);
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
		mPlacementValidator->validateCellOccupied(caller, layerIndex, x, y);
	}

	void World::validateCellUnoccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		mPlacementValidator->validateCellUnoccupied(caller, layerIndex, x, y);
	}

	void World::validateCellIsInSector(string const& caller, uint32_t x, uint32_t y, shared_ptr<const Sector> sector) const
	{
		mPlacementValidator->validateCellIsInSector(caller, x, y, sector);
	}

	void World::validateCellHasNoObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		mPlacementValidator->validateCellHasNoObject(caller, layerIndex, x, y);
	}

	void World::validateCellHasNoDoor(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		mPlacementValidator->validateCellHasNoDoor(caller, layerIndex, x, y);
	}


	void World::validateCellTraversableOnFoot(string const& caller, string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		mPlacementValidator->validateCellTraversableOnFoot(caller, desiredObject, layerIndex, x, y);
	}

	void World::validateLayer(string const& caller, uint32_t layerIndex) const
	{
		mPlacementValidator->validateLayer(caller, layerIndex);
	}

	void World::validateBounds(string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		mPlacementValidator->validateBounds(caller, x, y, cellsWide, levelsHigh);
	}

	void World::validateLayerSpace(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		mPlacementValidator->validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);
	}

	void World::validateObjectAllowedInSector(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		mPlacementValidator->validateObjectAllowedInSector(caller, type, sectorIndex);
	}

	void World::validateObjectAllowedInSectorAsLookTarget(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		mPlacementValidator->validateObjectAllowedInSectorAsLookTarget(caller, type, sectorIndex);
	}

	void World::validateSpaceOnlyInOneSector(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, bool allowAllBackgroundSpan) const
	{
		mPlacementValidator->validateSpaceOnlyInOneSector(caller, layerIndex, x, y, cellsWide, levelsHigh, allowAllBackgroundSpan);
	}

	void World::validateSectorDoorOptions(string const& caller, CreateDoorOptions const& options) const
	{
		mPlacementValidator->validateSectorDoorOptions(caller, options);
	}

	void World::validateSectorDoorPlacement(string const& caller, uint32_t layerIndex, uint32_t y,
		uint32_t x, CreateDoorOptions const& options, bool controlsAreExternallyBound) const
	{
		mPlacementValidator->validateSectorDoorPlacement(caller, layerIndex, y, x, options, controlsAreExternallyBound);
	}

	void World::validateSectorForceBridgeOptions(string const& caller, CreateForceBridgeOptions const& options) const
	{
		mPlacementValidator->validateSectorForceBridgeOptions(caller, options);
	}

	void World::validateSectorLadderOptions(string const& caller, CreateLadderOptions const& options) const
	{
		mPlacementValidator->validateSectorLadderOptions(caller, options);
	}

	void World::validateLiftOptions(string const& caller, CreateLiftOptions const& options) const
	{
		mPlacementValidator->validateLiftOptions(caller, options);
	}

	void World::validateShuttleOptions(string const& caller, CreateShuttleOptions const& options) const
	{
		mPlacementValidator->validateShuttleOptions(caller, options);
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







	World::CreateObjectResult World::createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier, bool boothWindow)
	{
		return mWorldBuilder->createWindow(layerIndex, x, y, cellsWide, levelsHigh, vertexIdentifier, boothWindow);
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
		return mWorldBuilder->addCorridor(y, x, cellsWide, levelsHigh);
	}

	uint32_t World::addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		return mWorldBuilder->addCorridor(layerIndex, y, x, cellsWide, levelsHigh);
	}

	uint32_t World::addRoom(string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight)
	{
		return mWorldBuilder->addRoom(name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight);
	}

	bool World::canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, string* diagnostic) const
	{
		return mWorldBuilder->canAddBackground(layerIndex, y, x, cellsWide, levelsHigh, diagnostic);
	}

	uint32_t World::addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, BackgroundColour const& colour)
	{
		return mWorldBuilder->addBackground(layerIndex, y, x, cellsWide, levelsHigh, colour);
	}

	bool World::canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, string* diagnostic) const
	{
		return mWorldBuilder->canAddFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, diagnostic);
	}

	uint32_t World::addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		return mWorldBuilder->addFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, colour);
	}

	uint32_t World::addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		return mWorldBuilder->addFacade(name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, colour);
	}

	bool World::canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
		string* diagnostic) const
	{
		return mWorldBuilder->canAddLadder(layerIndex, y, x, levelsHigh, diagnostic);
	}

	World::CreateLadderResult World::addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLadderOptions const& options)
	{
		return mWorldBuilder->addLadder(layerIndex, y, x, options);
	}

	bool World::canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
		string* diagnostic) const
	{
		return mWorldBuilder->canAddStairwell(layerIndex, y, x, levelsHigh, diagnostic);
	}

	uint32_t World::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, int mountSide)
	{
		return mWorldBuilder->addStairwell(layerIndex, y, x, levelsHigh, mountSide);
	}

	World::CreateStairwellResult World::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateStairwellOptions const& options)
	{
		return mWorldBuilder->addStairwell(layerIndex, y, x, options);
	}

	bool World::validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
		int riseSide, string& diagnostic) const
	{
		return mPlacementValidator->validateStaircaseEndpoint(layerIndex, x, y, upperEndpoint, riseSide, diagnostic);
	}

	bool World::canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, string* diagnostic) const
	{
		return mWorldBuilder->canAddStaircase(layerIndex, y, x, cellsWide, riseSide, diagnostic);
	}

	uint32_t World::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, float speed)
	{
		return mWorldBuilder->addStaircase(layerIndex, y, x, cellsWide, riseSide, speed);
	}

	bool World::setEscalatorInitiallyBroken(uint32_t sectorIndex, bool broken)
	{
		return mDeviceConditionStore->setEscalatorInitiallyBroken(sectorIndex, broken);
	}

	bool World::setEscalatorBroken(uint32_t sectorIndex, bool broken)
	{
		return mDeviceConditionStore->setEscalatorBroken(sectorIndex, broken);
	}

	uint32_t World::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, CreateStaircaseOptions const& options)
	{
		return mWorldBuilder->addStaircase(layerIndex, y, x, options);
	}

	std::vector<World::LiftLandingRow> World::getLiftLandingRows(uint32_t layerIndex,
		uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		return mWorldBuilder->getLiftLandingRows(layerIndex, y, x, cellsWide, levelsHigh);
	}

	World::CreateLiftResult World::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		return mWorldBuilder->addLift(layerIndex, y, x, cellsWide, levelsHigh);
	}

	World::CreateLiftResult World::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLiftOptions const& options)
	{
		return mWorldBuilder->addLift(layerIndex, y, x, options);
	}

	World::CreateShuttleResult World::addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options)
	{
		return mWorldBuilder->addShuttle(layerIndex, y, x, cellsWide, options);
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
		return mDeviceConditionStore->isControlTargetBroken(pointId);
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
						record.overrides, {}, record.initiallyBroken, record.transportDoorSpeeds, record.stopShortNames };
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
		return mWorldBuilder->canAddCorridorDoor(layerIndex, y, x, diagnostic);
	}

	bool World::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, string* diagnostic) const
	{
		return mWorldBuilder->canAddCorridorDoor(layerIndex, y, x, options, diagnostic);
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
		auto const ceiling = location->getPosition().y + scale.value_or(1.0f);
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->physicalMovementSector() == location.get()
				&& !agentPoseStateFits(*agent, ceiling - agent->getGlobalPosition().y))
				return reject("Room height would invalidate an occupied pose or Furniture finishing posture");
		}
		invalidateSimulationSnapshot();
		if (!location->setHeightScale(scale)) return reject("Height scale could not be applied");
		for (auto it = mConstructionRecords.rbegin(); it != mConstructionRecords.rend(); ++it)
			if (it->type == ConstructionType::Room && it->a == location->getLayerIndex()
				&& it->b == location->getCellY() && it->c == location->getCellX()
				&& it->d == location->getCellsWide() && it->e == location->getLevelsHigh())
			{ it->roomHeightScale = scale; break; }
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->physicalMovementSector() == location.get()) agent->syncPoseToSector();
		}
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
		return mDeviceConditionStore->setLiftInitiallyBroken(id, broken);
	}

	bool World::setLiftBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setLiftBroken(id, broken);
	}

	bool World::setShuttleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setShuttleInitiallyBroken(id, broken);
	}

	bool World::setShuttleBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setShuttleBroken(id, broken);
	}

	optional<DeviceCondition> World::knownTransportCondition(TraversalResourceId id,
		Agent const* agent, Sector const* observationSector) const
	{
		return mDeviceConditionStore->knownTransportCondition(id, agent, observationSector);
	}

	bool World::setDoorInitiallyBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setDoorInitiallyBroken(id, broken);
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
		return mDeviceConditionStore->setDoorSpeedOverride(id, speed);
	}

	bool World::setDoorBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setDoorBroken(id, broken);
	}

	bool World::setExtensibleBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setExtensibleBroken(id, broken);
	}

	bool World::setExtensibleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		return mDeviceConditionStore->setExtensibleInitiallyBroken(id, broken);
	}

	// ----------------------------------------------------------------------
	// Furniture facade (design pattern) -> FurnitureStore (ADR 0022).
	// World keeps ownership of the instances, the catalogue reference, and
	// the ID allocation (ADR 0001); the behaviour lives in the store.
	// ----------------------------------------------------------------------

	void World::attachFurnitureCatalogue(std::string resourceName,
		std::shared_ptr<const FurnitureCatalogue> catalogue)
	{
		mFurnitureStore->attachFurnitureCatalogue(std::move(resourceName), std::move(catalogue));
	}

	bool World::reloadFurnitureCatalogue(std::filesystem::path const& path, std::string* diagnostic)
	{
		return mFurnitureStore->reloadFurnitureCatalogue(path, diagnostic);
	}

	bool World::isFurnitureMarker(MarkerId id) const
	{
		return mFurnitureStore->isFurnitureMarker(id);
	}

	bool World::canPlaceFurniture(uint32_t sector, std::string const& definition,
		float x, float y, std::string const& name, std::string* diagnostic, int localDepth) const
	{
		return mFurnitureStore->canPlaceFurniture(sector, definition, x, y, name, diagnostic, localDepth);
	}

	uint64_t World::placeFurniture(uint32_t sector, std::string const& definition,
		float x, float y, std::string const& name, int localDepth)
	{
		return mFurnitureStore->placeFurniture(sector, definition, x, y, name, localDepth);
	}

	bool World::canEditFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth) const
	{
		return mFurnitureStore->canEditFurniture(id, x, y, name, diagnostic, localDepth);
	}

	bool World::editFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth)
	{
		return mFurnitureStore->editFurniture(id, x, y, name, diagnostic, localDepth);
	}

	bool World::canRemoveFurniture(uint64_t id, std::string* diagnostic) const
	{
		return mFurnitureStore->canRemoveFurniture(id, diagnostic);
	}

	bool World::removeFurniture(uint64_t id, std::string* diagnostic)
	{
		return mFurnitureStore->removeFurniture(id, diagnostic);
	}

	std::string World::furnitureSupportDiagnostic(uint32_t sector, uint32_t x, uint32_t y) const
	{
		return mFurnitureStore->furnitureSupportDiagnostic(sector, x, y);
	}

	void World::restoreFurniture(ConstructionRecord const& record)
	{
		mFurnitureStore->restoreFurniture(record);
	}

	World::ResolvedUsablePoint World::resolveUsablePoint(MarkerId marker,
		std::shared_ptr<const FurnitureCatalogue> const& catalogue) const
	{
		return mFurnitureStore->resolveUsablePoint(marker, catalogue);
	}

	FurnitureInstance const* World::furnitureForMarker(MarkerId marker) const
	{
		return mFurnitureStore->furnitureForMarker(marker);
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
		return mWorldBuilder->addSectorDoor(layerIndex, y, x);
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
		return mWorldBuilder->addSectorDoor(layerIndex, y, x, options);
	}

	void World::addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex)
	{
		mWorldBuilder->addSectorDoorButton(sectorIndex, objectIndex);
	}

	bool World::canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		return mWorldBuilder->canAddSectorDoorButton(sectorIndex, objectIndex);
	}

	bool World::canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		return mWorldBuilder->canRemoveSectorDoorButton(sectorIndex, objectIndex);
	}

	shared_ptr<const DoorSectorObject> World::removeSectorDoorButton(uint32_t sectorIndex,
		uint32_t objectIndex, optional<vector<AccessPermissionId>> resultingRequirement)
	{
		return mWorldBuilder->removeSectorDoorButton(sectorIndex, objectIndex, resultingRequirement);
	}


	bool World::canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, string* diagnostic) const
	{
		return mWorldBuilder->canAddSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, diagnostic);
	}

	uint32_t World::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		return mWorldBuilder->addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh);
	}

	World::CreateWindowResult World::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options)
	{
		return mWorldBuilder->addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, options);
	}

	World::CreateWindowResult World::addWindowAperture(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options, bool boothWindow)
	{
		return mWorldBuilder->addWindowAperture(layerIndex, y, x, cellsWide, levelsHigh, options, boothWindow);
	}

	bool World::canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
		uint32_t width, uint32_t height, string* diagnostic) const
	{
		return mWorldBuilder->canAddBoothWindow(layer, y, x, width, height, diagnostic);
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
		return mWorldBuilder->addBoothWindow(layer, y, x, state);
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
		return mWorldBuilder->canAddSectorBulkheadDoor(layerIndex, y, x, side, options, diagnostic);
	}

	World::CreateBulkheadDoorResult World::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y,
		uint32_t x, int side)
	{
		return mWorldBuilder->addSectorBulkheadDoor(layerIndex, y, x, side);
	}

	World::CreateBulkheadDoorResult World::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options)
	{
		return mWorldBuilder->addSectorBulkheadDoor(layerIndex, y, x, side, options);
	}

	World::CreateObjectResult World::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
	{
		return mWorldBuilder->addSectorLightSwitch(sectorIndex, xOffset);
	}

	bool World::canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, string* diagnostic) const
	{
		return mWorldBuilder->canAddSectorWalkway(sectorIndex, levelIndex, xOffset, diagnostic);
	}

	World::CreateObjectResult World::addSectorWalkway(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		return mWorldBuilder->addSectorWalkway(sectorIndex, levelIndex, xOffset);
	}

	bool World::canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
		string* diagnostic) const
	{
		return mMarkerStore->canAddSectorMarker(sectorIndex, levelIndex, xOffset, diagnostic);
	}

	bool World::canAddSectorMarkerImpl(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
		string* diagnostic, bool allowCoincident) const
	{
		return mMarkerStore->canAddSectorMarkerImpl(sectorIndex, levelIndex, xOffset, diagnostic,
			allowCoincident);
	}

	bool World::markerNameTaken(string const& trimmed, MarkerId except) const
	{
		return mMarkerStore->markerNameTaken(trimmed, except);
	}

	shared_ptr<const Marker> World::lookupMarker(MarkerId id) const
	{
		return mMarkerStore->lookupMarker(id);
	}

	vector<MarkerId> World::getMarkerIds() const
	{
		return mMarkerStore->getMarkerIds();
	}

	bool World::canRenameMarker(MarkerId id, string const& name, string* diagnostic) const
	{
		return mMarkerStore->canRenameMarker(id, name, diagnostic);
	}

	bool World::renameMarker(MarkerId id, string const& name, string* diagnostic)
	{
		return mMarkerStore->renameMarker(id, name, diagnostic);
	}

	bool World::setMarkerProperties(MarkerId id, MarkerProperties properties,
		string* diagnostic)
	{
		return mMarkerStore->setMarkerProperties(id, properties, diagnostic);
	}

	World::CreateObjectResult World::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier)
	{
		return mMarkerStore->addSectorMarker(sectorIndex, levelIndex, xOffset, vertexIdentifier);
	}

	World::CreateObjectResult World::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, string const& name, uint32_t* vertexIdentifier)
	{
		return mMarkerStore->addSectorMarker(sectorIndex, levelIndex, xOffset, name, vertexIdentifier);
	}

	World::CreateObjectResult World::addSectorMarkerRestored(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, MarkerId id, string name,
		MarkerProperties properties, uint32_t* vertexIdentifier, bool allowCoincident)
	{
		return mMarkerStore->addSectorMarkerRestored(sectorIndex, levelIndex, xOffset,
			id, std::move(name), properties, vertexIdentifier, allowCoincident);
	}

	bool World::canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
		string* diagnostic) const
	{
		return mMarkerStore->canRemoveSectorMarker(sectorIndex, objectIndex, diagnostic);
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
		return mMarkerStore->removeSectorMarker(sectorIndex, objectIndex, diagnostic);
	}

	World::CreateForceBridgeResult World::addSectorForceBridge(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		return mWorldBuilder->addSectorForceBridge(sectorIndex, levelIndex, xOffset);
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
		return mWorldBuilder->canAddSectorForceBridge(sectorIndex, levelIndex, xOffset, options, diagnostic);
	}

	World::CreateForceBridgeResult World::addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateForceBridgeOptions const& options)
	{
		return mWorldBuilder->addSectorForceBridge(sectorIndex, levelIndex, xOffset, options);
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
			command.type = DeviceCommandType::ToggleExtendedState;
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
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y0, 1), CORE_BUTTON_F_AUTO_REENABLE);
			registerExtensionControl(createdControls[CORE_LADDER_ENDPOINT_LOW], CORE_LADDER_ENDPOINT_LOW);


			// Upper
			createdControls[CORE_LADDER_ENDPOINT_HIGH] = createPhysicalControl("Ladder button", layerIndex, y1,
				transportControlDemand(sector, physicalControl::OwnerType::Ladder, controlGeometry, x, y1, 1), CORE_BUTTON_F_AUTO_REENABLE);
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
		record.stopShortNames = options.stopShortNames;
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
		clearPlayerInput();
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
		mPlacementValidator->validateTraversalTopology(graph);
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
		mAgentTypeCatalog->registerBundledAgentTypes();
	}

	std::shared_ptr<const AgentTypeDefinition> World::resolveAgentType(
		std::string const& typeId, std::string const& resourceName, bool reconstruct)
	{
		return mAgentTypeCatalog->resolveAgentType(typeId, resourceName, reconstruct);
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
			definition.resourceName, result.baseline, result.defaultMobilityProfile);
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
		return mAgentTypeCatalog->attachAgentType(std::move(resourceName), std::move(source),
			diagnostic);
	}

	bool World::hasAgentType(std::string_view typeId) const
	{
		return mAgentTypeCatalog->hasAgentType(typeId);
	}

	std::string World::agentTypeResourceName(std::string_view typeId) const
	{
		return mAgentTypeCatalog->agentTypeResourceName(typeId);
	}

	optional<AgentTypeDefinition> World::registeredAgentTypeDefinition(
		string_view typeId, string_view resourceName) const
	{
		return mAgentTypeCatalog->registeredAgentTypeDefinition(typeId, resourceName);
	}

	optional<AgentPhysicalBaseline> World::registeredAgentTypeBaseline(
		string_view typeId, string_view resourceName) const
	{
		return mAgentTypeCatalog->registeredAgentTypeBaseline(typeId, resourceName);
	}

	bool World::detachUnusedAgentType(std::string_view typeId)
	{
		return mAgentTypeCatalog->detachUnusedAgentType(typeId);
	}

	std::string World::agentTypeDisplayName(std::string_view typeId) const
	{
		return mAgentTypeCatalog->agentTypeDisplayName(typeId);
	}

	void World::validateAgentLocationPlacement(Sector const& sector, Agent const& agent,
		std::optional<float> floorY) const
	{
		mPlacementValidator->validateAgentLocationPlacement(sector, agent, floorY);
	}

	void World::validateAgentLocationPermissions(Sector const& sector,
		bitset<256> const& grants, string const& name) const
	{
		if (sector.getType() == SectorType::Dumbwaiter)
			throw invalid_argument("Agents cannot enter a Dumbwaiter shaft or car");
		if (sector.getType() == SectorType::Airlock || sector.getType() == SectorType::Chamber)
			throw invalid_argument(sector.getType() == SectorType::Chamber
				? "Agents cannot be placed inside authored Security scanners"
				: "Agents must enter Airlock chambers through coordinated traversal");
		if (sector.getType() != SectorType::Location) return;
		auto missing = static_cast<Location const&>(sector).getPermissionRequirement() & ~grants;
		if (missing.none()) return;
		auto diagnostic = format("Agent '{}' cannot be placed in Location '{}': missing Access permissions", name, sector.getName());
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
			// This is an authorization query, not a physical Agent construction.
			// Keep it independent of Lua and resource resolution on drag frames.
			bitset<256> effective;
			for (auto permission : grants)
			{
				auto found = lookupAccessPermission(permission);
				if (!found) throw invalid_argument(found.diagnostic);
				effective.set(permission.value - 1);
			}
			for (auto permissionSet : sets)
			{
				auto found = lookupPermissionSet(permissionSet);
				if (!found) throw invalid_argument(found.diagnostic);
				effective |= found.entity->mPermissions;
			}
			validateAgentLocationPermissions(*getSector(sectorId), effective, "New Agent");
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
		set<PermissionSetId> const& sets, optional<ObjectUsage> objectUsage, optional<float> objectUsageDistance,
		set<AgentTagId> const& tags)
	{
		if ((objectUsage || objectUsageDistance || !tags.empty()) && !mSimulationPaused)
			throw SerializationException("Pause the simulation before authoring Object usage overrides or Agent tags");
		auto agent = makeScriptAgentForPlacement(typeId, name, grants, sets);
		for (auto tag : tags) if (!mAgentTagRegistry || !mAgentTagRegistry->lookupAgentTag(tag))
			throw SerializationException("Agent tag is not defined in the attached registry");
		if (!agent->objectUsageConfigurationIsValid(mAgentTagRegistry.get(), tags, objectUsage, objectUsageDistance))
			throw SerializationException("Invalid Object usage overrides: effective Arms distance must be finite and positive");
		agent->setIndividualObjectUsage(objectUsage);
		agent->setIndividualObjectUsageDistance(objectUsageDistance);
		agent->setAgentTags(tags);
		return addOwnedAgentToSector(std::move(agent), sectorId, levelOffset, xOffset);
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
	// and this one private forward through the traversal resource registry
	// (ADR 0022): creating and removing a traversal resource is entity
	// ownership which stays with World (ADR 0001), and the lifecycle
	// events those paths publish carry the resource snapshot the coordinator
	// builds.
	TraversalResourceSnapshot World::makeTraversalResourceSnapshot(TraversalResourceId id,
		TraversalResource const& resource) const
	{
		return mTraversalRegistry->makeTraversalResourceSnapshot(id, resource);
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
		return mTraversalRegistry->holdsTraversalOwnership(id);
	}

	bool World::isAgentInQueue(AgentId id) const
	{
		return mSimulationCoordinator.isAgentInQueue(id);
	}

	void World::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		mTraversalRegistry->releaseAgentFromResource(resource, id);
	}

	void World::releaseTraversalOwnership(AgentId id)
	{
		mTraversalRegistry->releaseTraversalOwnership(id);
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

	// The per-Agent individual-profile edit family (ADR 0022, #683): the
	// behaviour lives in the profile editor, which drives every single-
	// property override through the #677 descriptor table and keeps the
	// coupled object-usage pair on its own composite path. World keeps
	// ownership of the Agent registry being edited (ADR 0001) and stays
	// the facade (design pattern), forwarding every entry point below.

	bool World::setAgentIndividualColour(AgentId agent, optional<AgentColour> value,
		string* diagnostic)
	{
		return mAgentProfileEditor->setColour(agent, value, diagnostic);
	}

	bool World::setAgentIndividualEscalatorWalkingChance(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setEscalatorWalkingChance(agent, value, diagnostic);
	}

	bool World::setAgentIndividualWalkSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setWalkSpeedModifier(agent, value, diagnostic);
	}

	bool World::setAgentIndividualHeightModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setHeightModifier(agent, value, diagnostic);
	}

	bool World::setAgentIndividualStairSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setStairSpeedModifier(agent, value, diagnostic);
	}

	bool World::setAgentIndividualLadderSpeedModifier(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setLadderSpeedModifier(agent, value, diagnostic);
	}

	bool World::setAgentIndividualInteractionAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setInteractionAversion(agent, value, diagnostic);
	}

	bool World::setAgentIndividualEffortAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setEffortAversion(agent, value, diagnostic);
	}

	bool World::setAgentIndividualWaitingAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setWaitingAversion(agent, value, diagnostic);
	}

	bool World::setAgentIndividualCrowdAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setCrowdAversion(agent, value, diagnostic);
	}

	bool World::setAgentIndividualRiskAversion(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setRiskAversion(agent, value, diagnostic);
	}

	bool World::setAgentIndividualRouteFamiliarity(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setRouteFamiliarity(agent, value, diagnostic);
	}

	bool World::setAgentIndividualRoutePersistence(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setRoutePersistence(agent, value, diagnostic);
	}

	bool World::setAgentIndividualMinimumRoutePlanningTime(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setMinimumRoutePlanningTime(agent, value, diagnostic);
	}

	bool World::setAgentIndividualMaximumRoutePlanningTime(AgentId agent,
		optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setMaximumRoutePlanningTime(agent, value, diagnostic);
	}

	bool World::setAgentIndividualObjectUsage(AgentId agent, optional<ObjectUsage> value, string* diagnostic)
	{
		return mAgentProfileEditor->setObjectUsage(agent, value, diagnostic);
	}

	bool World::setAgentIndividualObjectUsageDistance(AgentId agent, optional<float> value, string* diagnostic)
	{
		return mAgentProfileEditor->setObjectUsageDistance(agent, value, diagnostic);
	}

	bool World::setAgentObjectUsageOverrides(AgentId agent, optional<ObjectUsage> mode,
		optional<float> distance, string* diagnostic)
	{
		return mAgentProfileEditor->setObjectUsageOverrides(agent, mode, distance, diagnostic);
	}

	bool World::setAgentIndividualPermissionAdherence(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return mAgentProfileEditor->setPermissionAdherence(agent, value, diagnostic);
	}

	bool World::setAgentIndividualRemoteAccessPanels(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return mAgentProfileEditor->setRemoteAccessPanels(agent, value, diagnostic);
	}

	bool World::setAgentIndividualRemoteBoothWindowShutters(AgentId agent,
		optional<bool> value, string* diagnostic)
	{
		return mAgentProfileEditor->setRemoteBoothWindowShutters(agent, value, diagnostic);
	}

	bool World::setAgentIndividualMobilityProfile(AgentId agent,
		optional<MobilityProfile> value, string* diagnostic)
	{
		return mAgentProfileEditor->setMobilityProfile(agent, value, diagnostic);
	}


	void World::agentObjectUsageChanged(AgentId id, ObjectUsage beforeMode, float beforeDistance)
	{
		auto* agent = mAgents.find(id);
		if (!agent) return;
		invalidateSimulationSnapshot();
		mSimulationCoordinator.agentObjectUsageChanged(id);
		if (beforeMode == agent->getObjectUsage()
			&& (agent->getObjectUsage() == ObjectUsage::None || beforeDistance == agent->getObjectUsageDistance())) return;
		if (agent->getObjectUsage() == ObjectUsage::None || agent->getObjectUsageDistance() < beforeDistance)
			replanAgentAfterAuthorizationRefusal(id);
		else beginVoluntaryRoutePlanning(id);
		modify();
	}


	// Agent groups are authored World data, not simulation state (ADR 0006).
	// The behaviour lives in the Agent group registry (ADR 0022); World keeps
	// ownership of the registry itself (ADR 0001) and stays the facade
	// (design pattern), forwarding every entry point below.

	bool World::agentGroupNameTaken(std::string const& trimmed, AgentGroupId except) const
	{
		return mAgentGroupRegistry->nameTaken(trimmed, except);
	}

	uint32_t World::getAgentGroupCount() const
	{
		return mAgentGroupRegistry->count();
	}

	std::vector<AgentGroupId> World::getAgentGroupIds() const
	{
		return mAgentGroupRegistry->ids();
	}

	EntityLookup<AgentGroup const> World::lookupAgentGroup(AgentGroupId id) const
	{
		return mAgentGroupRegistry->lookup(id);
	}

	std::string const& World::getAgentGroupName(AgentGroupId id) const
	{
		return mAgentGroupRegistry->name(id);
	}

	bool World::canAddAgentGroup(std::string const& name, std::string* diagnostic) const
	{
		return mAgentGroupRegistry->canAdd(name, diagnostic);
	}

	AgentGroupId World::addAgentGroup(std::string const& name)
	{
		return mAgentGroupRegistry->add(name);
	}

	bool World::canRenameAgentGroup(AgentGroupId id, std::string const& name,
		std::string* diagnostic) const
	{
		return mAgentGroupRegistry->canRename(id, name, diagnostic);
	}

	bool World::renameAgentGroup(AgentGroupId id, std::string const& name,
		std::string* diagnostic)
	{
		return mAgentGroupRegistry->rename(id, name, diagnostic);
	}

	bool World::canSetAgentGroup(AgentId agent, AgentGroupId group, std::string* diagnostic) const
	{
		return mAgentGroupRegistry->canSetAgentGroup(agent, group, diagnostic);
	}

	bool World::setAgentGroup(AgentId agent, AgentGroupId group, std::string* diagnostic)
	{
		return mAgentGroupRegistry->setAgentGroup(agent, group, diagnostic);
	}

	AgentGroupId World::getAgentGroup(AgentId agent) const
	{
		return mAgentGroupRegistry->agentGroup(agent);
	}

	bool World::isAgentGroupActive(AgentGroupId group) const
	{
		return mAgentGroupRegistry->isActive(group);
	}

	bool World::canSetAgentGroupActive(AgentGroupId group, bool active,
		std::string* diagnostic) const
	{
		return mAgentGroupRegistry->canSetActive(group, active, diagnostic);
	}

	bool World::setAgentGroupActive(AgentGroupId group, bool active,
		std::string* diagnostic)
	{
		return mAgentGroupRegistry->setActive(group, active, diagnostic);
	}

	bool World::canAssignAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		return mAgentTagService->canAssign(agent, tag, diagnostic);
	}

	bool World::assignAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		return mAgentTagService->assign(agent, tag, diagnostic);
	}

	bool World::canRemoveAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic) const
	{
		return mAgentTagService->canRemove(agent, tag, diagnostic);
	}

	bool World::removeAgentTag(AgentId agent, AgentTagId tag,
		string* diagnostic)
	{
		return mAgentTagService->remove(agent, tag, diagnostic);
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
		// The facade keeps the named-parameter contract; the service drives
		// the thirteen sampled properties through its descriptor rows.
		AgentTagSampleSet samples{ walkSpeedSample, heightSample, stairSpeedSample,
			ladderSpeedSample, interactionAversionSample, effortAversionSample,
			waitingAversionSample, crowdAversionSample, riskAversionSample,
			routeFamiliaritySample, routePersistenceSample,
			minimumRoutePlanningTimeSample, maximumRoutePlanningTimeSample };
		return mAgentTagService->validateAssignments(tags, samples, diagnostic);
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
		AgentTagSampleSet samples{ walkSpeedSample, heightSample, stairSpeedSample,
			ladderSpeedSample, interactionAversionSample, effortAversionSample,
			waitingAversionSample, crowdAversionSample, riskAversionSample,
			routeFamiliaritySample, routePersistenceSample,
			minimumRoutePlanningTimeSample, maximumRoutePlanningTimeSample };
		return mAgentTagService->restoreAssignments(agent, tags, samples, diagnostic);
	}

	set<AgentTagId> const& World::getAgentTags(AgentId agent) const
	{
		return mAgentTagService->tags(agent);
	}

	uint32_t World::getAgentGroupMemberCount(AgentGroupId id) const
	{
		return mAgentGroupRegistry->memberCount(id);
	}

	bool World::canDeleteAgentGroup(AgentGroupId id, std::string* diagnostic) const
	{
		return mAgentGroupRegistry->canDelete(id, diagnostic);
	}

	bool World::deleteAgentGroup(AgentGroupId id, std::string* diagnostic)
	{
		return mAgentGroupRegistry->remove(id, diagnostic);
	}

	// Interaction and device-operation orchestration - the InteractionPoint and
	// InteractionRequest lifecycles, the DeviceOperation lifecycle, pressing
	// physical controls, and the per-tick interaction phases - lives in
	// SimulationCoordinator (ADR 0004). World keeps the registries
	// (ADR 0001) and forwards every entry point, so no caller outside World
	// names the coordinator.

	// The AccessPermission/PermissionSet authoring and the Agent grant/
	// assignment behaviour live in the authorization ledger (ADR 0022,
	// #684). World keeps the state the ledger edits (ADR 0001) and stays
	// the facade (design pattern); the private helpers forward so the
	// live `canAgent…Now` eligibility checks keep reading the facts in
	// place - the ledger holds authorization facts, not live traversal
	// evaluation.

	bool World::accessPermissionNameTaken(string const& name, AccessPermissionId except) const
	{
		return mAuthorizationLedger->accessPermissionNameTaken(name, except);
	}

	uint32_t World::getAccessPermissionCount() const
	{
		return mAuthorizationLedger->accessPermissionCount();
	}

	vector<AccessPermissionId> World::getAccessPermissionIds() const
	{
		return mAuthorizationLedger->accessPermissionIds();
	}

	EntityLookup<AccessPermission const> World::lookupAccessPermission(AccessPermissionId id) const
	{
		return mAuthorizationLedger->lookupAccessPermission(id);
	}

	string const& World::getAccessPermissionName(AccessPermissionId id) const
	{
		return mAuthorizationLedger->accessPermissionName(id);
	}

	bool World::canAddAccessPermission(string const& raw, string* diagnostic) const
	{
		return mAuthorizationLedger->canAddAccessPermission(raw, diagnostic);
	}

	AccessPermissionId World::addAccessPermission(string const& raw)
	{
		return mAuthorizationLedger->addAccessPermission(raw);
	}

	bool World::renameAccessPermission(AccessPermissionId id, string const& raw, string* diagnostic)
	{
		return mAuthorizationLedger->renameAccessPermission(id, raw, diagnostic);
	}

	bool World::isLocationPermissionEligible(uint32_t sectorIndex) const
	{
		return mPermissionRequirementStore->isLocationEligible(sectorIndex);
	}

	vector<AccessPermissionId> World::getLocationPermissionRequirement(uint32_t sectorIndex) const
	{
		return mPermissionRequirementStore->locationRequirement(sectorIndex);
	}

	bool World::setLocationPermissionRequirement(uint32_t sectorIndex,
	vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		return mPermissionRequirementStore->setLocationRequirement(sectorIndex, permissions, diagnostic);
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

	bool World::isValidTransportStopShortName(string_view name)
	{
		return !name.empty() && name.size() <= 3 && all_of(name.begin(), name.end(), [](char c)
			{ return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
	}

	string World::getTransportStopShortName(uint32_t sectorIndex, uint32_t stopIndex, uint32_t objectIndex) const
	{
		auto record = findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) throw invalid_argument("Unknown transport destination Stop");
		return record->stopShortNames.empty() ? to_string(stopIndex) : record->stopShortNames.at(stopIndex);
	}

	string World::getTransportStopShortName(TraversalResourceId resourceId, uint32_t stopIndex) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto record = resource ? findLiftDestinationRecord(*resource) : nullptr;
		if (!record || stopIndex >= record->values.size()) throw invalid_argument("Unknown transport destination Stop");
		return record->stopShortNames.empty() ? to_string(stopIndex) : record->stopShortNames.at(stopIndex);
	}

	bool World::setTransportStopShortName(uint32_t sectorIndex, uint32_t stopIndex,
		string const& name, string* diagnostic, uint32_t objectIndex)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mSimulationPaused) return reject("Stop short names can only be edited while the simulation is paused");
		if (!isValidTransportStopShortName(name)) return reject("Use 1–3 uppercase alphanumeric characters (A–Z, 0–9)");
		auto record = findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) return reject("Unknown transport destination Stop");
		if (getTransportStopShortName(sectorIndex, stopIndex, objectIndex) != name)
		{
			auto& names = mConstructionRecords[record - mConstructionRecords.data()].stopShortNames;
			if (names.empty())
				for (size_t i = 0; i < record->values.size(); ++i) names.push_back(to_string(i));
			names[stopIndex] = name;
			modify();
		}
		if (diagnostic) diagnostic->clear();
		return true;
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
		return mPermissionRequirementStore->destinationRequirement(sectorIndex, stopIndex, objectIndex);
	}

	bool World::setLiftDestinationPermissionRequirement(uint32_t sectorIndex, uint32_t stopIndex,
	vector<AccessPermissionId> const& permissions, string* diagnostic, uint32_t objectIndex)
	{
		return mPermissionRequirementStore->setDestinationRequirement(sectorIndex, stopIndex,
			permissions, diagnostic, objectIndex);
	}

	World::AccessPermissionUsage World::getAccessPermissionUsage(AccessPermissionId id) const
	{
		return mAuthorizationLedger->accessPermissionUsage(id);
	}

	bool World::deleteAccessPermission(AccessPermissionId id, string* diagnostic)
	{
		return mAuthorizationLedger->deleteAccessPermission(id, diagnostic);
	}

	bool World::setAgentAccessPermissionGrant(AgentId agentId, AccessPermissionId permission,
		bool granted, string* diagnostic)
	{
		return mAuthorizationLedger->setAgentAccessPermissionGrant(agentId, permission, granted,
			diagnostic);
	}

	vector<AccessPermissionId> World::getAgentDirectAccessGrants(AgentId id) const
	{
		return mAuthorizationLedger->getAgentDirectAccessGrants(id);
	}
	bitset<256> World::currentDirectAccessGrants(Agent const& agent) const
	{
		return mAuthorizationLedger->currentDirectAccessGrants(agent);
	}
	set<PermissionSetId> World::currentPermissionSets(Agent const& agent) const
	{
		return mAuthorizationLedger->currentPermissionSets(agent);
	}
	bitset<256> World::effectiveAccessGrants(Agent const& agent) const
	{
		return mAuthorizationLedger->effectiveAccessGrants(agent);
	}
	AccessPermissionId World::accessPermissionNamed(string_view name) const
	{
		return mAuthorizationLedger->accessPermissionNamed(name);
	}
	PermissionSetId World::permissionSetNamed(string_view name) const
	{
		return mAuthorizationLedger->permissionSetNamed(name);
	}
	bool World::setAgentRuntimeAccessPermissionGrant(AgentId agentId,
		AccessPermissionId permission, bool granted)
	{
		return mAuthorizationLedger->setAgentRuntimeAccessPermissionGrant(agentId, permission, granted);
	}
	vector<AccessPermissionId> World::getAgentCurrentDirectAccessGrants(AgentId id) const
	{
		return mAuthorizationLedger->getAgentCurrentDirectAccessGrants(id);
	}
	bool World::setAgentRuntimePermissionSetAssignment(AgentId agentId,
		PermissionSetId id, bool assigned)
	{
		return mAuthorizationLedger->setAgentRuntimePermissionSetAssignment(agentId, id, assigned);
	}
	vector<PermissionSetId> World::getAgentCurrentPermissionSetAssignments(AgentId id) const
	{
		return mAuthorizationLedger->getAgentCurrentPermissionSetAssignments(id);
	}
	vector<AccessPermissionId> World::getAgentEffectiveAccessGrants(AgentId id) const
	{
		return mAuthorizationLedger->getAgentEffectiveAccessGrants(id);
	}
	World::EffectiveAccessGrantSources World::getAgentAccessGrantSources(
		AgentId agentId, AccessPermissionId permission) const
	{
		return mAuthorizationLedger->getAgentAccessGrantSources(agentId, permission);
	}
	bool World::permissionSetNameTaken(string const& name, PermissionSetId except) const
	{
		return mAuthorizationLedger->permissionSetNameTaken(name, except);
	}
	uint32_t World::getPermissionSetCount() const
	{
		return mAuthorizationLedger->permissionSetCount();
	}
	vector<PermissionSetId> World::getPermissionSetIds() const
	{
		return mAuthorizationLedger->permissionSetIds();
	}
	EntityLookup<PermissionSet const> World::lookupPermissionSet(PermissionSetId id) const
	{
		return mAuthorizationLedger->lookupPermissionSet(id);
	}
	string const& World::getPermissionSetName(PermissionSetId id) const
	{
		return mAuthorizationLedger->permissionSetName(id);
	}
	PermissionSetId World::addPermissionSet(string const& raw)
	{
		return mAuthorizationLedger->addPermissionSet(raw);
	}
	bool World::renamePermissionSet(PermissionSetId id, string const& raw, string* diagnostic)
	{
		return mAuthorizationLedger->renamePermissionSet(id, raw, diagnostic);
	}
	uint32_t World::getPermissionSetUsageCount(PermissionSetId id) const
	{
		return mAuthorizationLedger->permissionSetUsageCount(id);
	}
	bool World::deletePermissionSet(PermissionSetId id, string* diagnostic)
	{
		return mAuthorizationLedger->deletePermissionSet(id, diagnostic);
	}
	vector<AccessPermissionId> World::getPermissionSetPermissions(PermissionSetId id) const
	{
		return mAuthorizationLedger->permissionSetPermissions(id);
	}
	bool World::setPermissionSetAccessPermission(PermissionSetId id,
		AccessPermissionId permission, bool included, string* diagnostic)
	{
		return mAuthorizationLedger->setPermissionSetAccessPermission(id, permission, included, diagnostic);
	}
	vector<PermissionSetId> World::getAgentPermissionSetAssignments(AgentId id) const
	{
		return mAuthorizationLedger->getAgentPermissionSetAssignments(id);
	}
	bool World::setAgentPermissionSetAssignment(AgentId agentId, PermissionSetId id,
		bool assigned, string* diagnostic)
	{
		return mAuthorizationLedger->setAgentPermissionSetAssignment(agentId, id, assigned, diagnostic);
	}

	bool World::isInteractionPointPermissionEligible(InteractionPointId id) const
	{
		return mPermissionRequirementStore->isInteractionPointEligible(id);
	}

	bool World::setInteractionPointPermissionRequirement(InteractionPointId id,
	vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		return mPermissionRequirementStore->setInteractionPointRequirement(id, permissions, diagnostic);
	}

	vector<AccessPermissionId> World::getInteractionPointPermissionRequirement(InteractionPointId id) const
	{
		return mPermissionRequirementStore->interactionPointRequirement(id);
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
		return mPermissionRequirementStore->isManualDoorEligible(id);
	}

	bool World::setManualDoorPermissionRequirement(TraversalResourceId id,
	vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		return mPermissionRequirementStore->setManualDoorRequirement(id, permissions, diagnostic);
	}

	vector<AccessPermissionId> World::getManualDoorPermissionRequirement(
	TraversalResourceId id) const
	{
		return mPermissionRequirementStore->manualDoorRequirement(id);
	}

	optional<Vector2> World::remoteOrdinaryDoorCentre(TraversalResourceId doorId) const
	{
		auto resource = mTraversalResources.find(doorId);
		if (!resource || !resource->mDoor || typeid(*resource->mDoor) != typeid(Door)
			|| resource->mLiftCoordinator || resource->mDoor->isLiftOwned() || resource->mDoor->isShuttleOwned()
			|| resource->mDoorActivationMode != DoorActivationMode::Manual) return nullopt;
		auto const& door = *resource->mDoor;
		if (!door.getFrontSector() || !door.getBackSector()
			|| !isLocationLike(door.getFrontSector()->getType())
			|| !isLocationLike(door.getBackSector()->getType())) return nullopt;
		return door.getPosition() + door.getSize() * .5f;
	}

	bool World::agentCanOperateManualDoorHere(TraversalResourceId doorId, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		if (!resource || !agent || !agent->isActive() || !resource->mEnabled
			|| !agentHasFeasibleManualDoorOperation(doorId, agentId) || resource->mDoor->isBroken()) return false;
		auto const& door = *resource->mDoor;
		if (door.getFrontSector().get() != agent->getSector() && door.getBackSector().get() != agent->getSector()) return false;
		if (agent->getObjectUsage() == ObjectUsage::RemoteControl)
		{
			auto centre = remoteOrdinaryDoorCentre(doorId);
			return centre && agent->getGlobalPosition().distanceTo(*centre) <= agent->getObjectUsageDistance();
		}
		auto endpoint = door.getPosition() + Vector2{door.getSize().x * .5f, 0.f};
		return agentCanPhysicallyOperate(*agent, agent->getGlobalPosition().distanceTo(endpoint), numeric_limits<float>::max());
	}

	bool World::canAgentOpenManualDoor(TraversalResourceId doorId, AgentId agentId) const
	{
		return agentHasFeasibleManualDoorOperation(doorId, agentId);
	}

	bool World::agentHasFeasibleManualDoorOperation(TraversalResourceId doorId, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mDoor || !agent
			|| !agentSatisfiesDoorPermission(*resource->mDoor, *agent)) return false;
		if (agent->getObjectUsage() == ObjectUsage::Arms) return true;
		auto centre = remoteOrdinaryDoorCentre(doorId);
		// Routing asks whether the ordinary floor approach can enter range, not
		// whether the Agent's current remote position is already in range.
		return centre && agent->getObjectUsage() == ObjectUsage::RemoteControl
			&& centre->y - resource->mDoor->getPosition().y <= agent->getObjectUsageDistance();
	}

	bool World::canAgentOperateDoorControl(TraversalResourceId doorId, SectorId approach,
		AgentId agentId) const
	{
		auto resource = mTraversalResources.find(doorId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mDoor || !agent || !agentCanOperateObjects(*agent)) return false;
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (point && point->mSector == approach
				&& (agent->getObjectUsage() == ObjectUsage::Arms
					|| remoteButtonApproachDistance(pointId, approach, point->mPosition, agentId))
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

	bool World::extensibleControlAppliesToApproach(TraversalResource const& resource,
		InteractionPoint const& point, SectorId approach, Vector2 const& approachPosition)
	{
		if (point.mSector != approach) return false;
		// Both controls can occupy one Room, but opposite-end requirements remain
		// independent. Exact midpoint positions belong to the left/lower side.
		if (resource.mForceBridge)
		{
			auto const middle = resource.mForceBridge->getPosition().x
				+ resource.mForceBridge->getSize().x * 0.5f;
			return (point.mPosition.x > middle) == (approachPosition.x > middle);
		}
		if (resource.mLadder)
		{
			// Handholds extend above the upper landing: classify using the logical
			// landing midpoint, not the rendered object's centre.
			auto const lower = resource.mLadder->getPosition().y
				- CORE_LADDER_HEIGHT_OFF_GROUND;
			auto const middle = lower
				+ static_cast<float>(resource.mLadder->getLevelsHigh() - 1) * 0.5f;
			return (point.mPosition.y > middle) == (approachPosition.y > middle);
		}
		return true;
	}

	bool World::canAgentOperateExtensibleControl(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& approachPosition, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto agent = mAgents.find(agentId);
		if (!resource || !resource->mExtensible || !agent || !agentCanOperateObjects(*agent)) return false;
		for (auto pointId : resource->mControls)
		{
			auto point = mInteractionPoints.find(pointId);
			if (point && extensibleControlAppliesToApproach(*resource, *point, approach, approachPosition)
				&& (agent->getObjectUsage() == ObjectUsage::Arms
					|| remoteButtonApproachDistance(pointId, approach, approachPosition, agentId))
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
			if (!point || !extensibleControlAppliesToApproach(*resource, *point, approach, approachPosition)) continue;
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
		return agentHasFeasibleLiftJourney(resourceId, origin, destination, agentId);
	}

	bool World::agentHasFeasibleLiftJourney(TraversalResourceId resourceId,
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
		auto agent = mAgents.find(agentId);
		return (agent && agentHasFeasibleLiftSelection(resourceId, stop, *agent))
			|| agentCanReuseAcceptedLiftJourney(resourceId, stop, origin, agentId);
	}

	bool World::agentHasFeasibleLiftSelection(TraversalResourceId resourceId,
		uint32_t stop, Agent const& agent) const
	{
		DeviceCommand command;
		command.type = DeviceCommandType::SelectLiftDestination;
		command.traversalResource = resourceId;
		command.stopIndex = stop;
		// Projected capability plus authorization. This must never issue a command;
		// runtime selectors recheck actual occupancy, Sector and current range.
		return (agent.getObjectUsage() == ObjectUsage::Arms
			|| agentHasFeasibleRemoteLiftSelection(resourceId, agent))
			&& missingLiftDestinationPermissions(command, getAgentId(&agent)).empty();
	}

	bool World::agentCanReuseAcceptedLiftJourney(TraversalResourceId resourceId,
		uint32_t stop, Vector2 const& origin, AgentId agentId) const
	{
		auto resource = mTraversalResources.find(resourceId);
		auto agent = mAgents.find(agentId);
		if (!resource || !agent || !agent->getSector()) return false;
		bool local = find(resource->mOccupants.begin(), resource->mOccupants.end(), agentId)
			!= resource->mOccupants.end();
		if (!agentAdheresToLiftDestinationPermission(resourceId, stop, agentId)) return false;
		if (local && agent->getObjectUsage() == ObjectUsage::Arms) return true;
		if (local)
		{
			// None may finish an accepted journey, but occupancy alone cannot
			// authorize a new destination selection.
			auto committed = resource->mLiftPassengerDestinations.find(agentId);
			if ((committed != resource->mLiftPassengerDestinations.end() && committed->second == stop)
				|| (!resource->mLiftMoving && resource->mLiftCurrentStop == stop)) return true;
		}
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
			&& (!resource->mLiftStopRequestOwners[stop].empty() || resource->mPlayerStopRequests.contains(stop)
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
		return agentHasFeasibleTransportLandingOperation(resourceId, approach, endpoint, agentId);
	}

	bool World::agentHasFeasibleTransportLandingOperation(TraversalResourceId resourceId,
		SectorId approach, Vector2 const& endpoint, AgentId agentId) const
	{
		auto agent = mAgents.find(agentId);
		if (!agent || !agentCanOperateObjects(*agent)
			|| !agentSatisfiesTransportLandingPermission(resourceId, approach, endpoint, agentId)) return false;
		if (agent->getObjectUsage() == ObjectUsage::Arms) return true;
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return false;
		if (resource->mLiftCoordinator)
		{
			for (auto control : resource->mControls)
				if (remoteButtonApproachDistance(control, approach, endpoint, agentId)) return true;
			return false;
		}
		auto stop = mSimulationCoordinator.findLiftStop(*resource, endpoint);
		return stop < resource->mLiftStops.size()
			&& remoteButtonApproachDistance(resource->mLiftStops[stop].callControl, approach, endpoint, agentId).has_value();
	}

	bool World::agentSatisfiesTransportLandingPermission(TraversalResourceId resourceId,
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
			|| agentSatisfiesTransportLandingPermission(resourceId, approach, endpoint, agentId));
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
		// Admission's capability check deliberately remains projected: actual
		// manual operation revalidates present-position reach separately.
		return agentHasFeasibleManualDoorOperation(doorId, agentId);
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
				&& !agentHasFeasibleLiftJourney(node.edge->getTraversalResourceId(), agent.getGlobalPosition(),
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
		return mTraversalRegistry->createTraversalResource(name);
	}

	TraversalResourceId World::createDoorTraversalResource(string const& name,
		shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds)
	{
		return mTraversalRegistry->createDoorTraversalResource(name, std::move(door), mode, holdOpenSeconds);
	}

	TraversalResourceId World::createWindowTraversalResource(string const& name,
		shared_ptr<Window> window)
	{
		return mTraversalRegistry->createWindowTraversalResource(name, std::move(window));
	}

	TraversalResourceId World::createLadderTraversalResource(string const& name,
		shared_ptr<Ladder> ladder, SectorId ladderSector, uint32_t directionalBatchLimit)
	{
		return mTraversalRegistry->createLadderTraversalResource(name, std::move(ladder), ladderSector,
			directionalBatchLimit);
	}

	TraversalResourceId World::createLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId liftSector, vector<LiftStop> stops, uint32_t capacity,
		float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		return mTraversalRegistry->createLiftTraversalResource(name, std::move(lift), liftSector,
			std::move(stops), capacity, minimumDwellSeconds, maximumBoardingSeconds);
	}

	TraversalResourceId World::createOpenPlatformLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId locationSector, vector<LiftStop> stops, uint32_t capacity,
		float stopDurationSeconds)
	{
		return mTraversalRegistry->createOpenPlatformLiftTraversalResource(name, std::move(lift),
			locationSector, std::move(stops), capacity, stopDurationSeconds);
	}

	TraversalResourceId World::createShuttleTraversalResource(string const& name,
		shared_ptr<Shuttle> shuttle, SectorId shuttleSector, vector<LiftStop> stops,
		uint32_t capacity, float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		return mTraversalRegistry->createShuttleTraversalResource(name, std::move(shuttle), shuttleSector,
			std::move(stops), capacity, minimumDwellSeconds, maximumBoardingSeconds);
	}

	TraversalResourceId World::createStairwellTraversalResource(string const& name,
		shared_ptr<Stairwell> stairwell, SectorId stairwellSector, uint32_t capacity,
		uint32_t directionalBatchLimit)
	{
		return mTraversalRegistry->createStairwellTraversalResource(name, std::move(stairwell),
			stairwellSector, capacity, directionalBatchLimit);
	}

	TraversalResourceId World::createForceBridgeTraversalResource(string const& name,
		shared_ptr<ForceBridge> forceBridge)
	{
		return mTraversalRegistry->createForceBridgeTraversalResource(name, std::move(forceBridge));
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
		return mDeviceConditionStore->setDoorSensorObservation(resourceId, sensor, observation);
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
		return mTraversalRegistry->lookupTraversalResource(id);
	}

	EntityLookup<TraversalResource const> World::lookupTraversalResource(TraversalResourceId id) const
	{
		const TraversalResourceRegistry& registry = *mTraversalRegistry;
		return registry.lookupTraversalResource(id);
	}

	TraversalResourceId World::getTraversalResourceId(Object const* object) const
	{
		return mTraversalRegistry->getTraversalResourceId(object);
	}

	EntityRemovalResult World::removeTraversalResource(TraversalResourceId id)
	{
		return mTraversalRegistry->removeTraversalResource(id);
	}

	EntityLookup<TraversalRequest const> World::lookupTraversalRequest(TraversalRequestId id) const
	{
		auto entity = mTraversalRequests.find(id);
		return entity ? EntityLookup<TraversalRequest const>{ entity, {} }
			: EntityLookup<TraversalRequest const>{ nullptr, format("TraversalRequest handle {} is invalid or has been released", id.value) };
	}

	TraversalWaitingPolicy const& World::getTraversalWaitingPolicy() const
	{
		return mTraversalRegistry->getTraversalWaitingPolicy();
	}

	void World::setTraversalWaitingPolicy(TraversalWaitingPolicy policy)
	{
		mTraversalRegistry->setTraversalWaitingPolicy(policy);
	}

	TraversalGeometryPolicy const& World::getTraversalGeometryPolicy() const
	{
		return mTraversalRegistry->getTraversalGeometryPolicy();
	}

	void World::setTraversalGeometryPolicy(TraversalGeometryPolicy policy)
	{
		mTraversalRegistry->setTraversalGeometryPolicy(policy);
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
		mSimulationCoordinator.tryAnticipatoryInteraction(agent, movementStart, movementEnd);
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