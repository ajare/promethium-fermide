#pragma once
#include "core/ActionRegistry.h"

#include <string>
#include <string_view>
#include <array>
#include <bitset>
#include <vector>
#include <set>
#include <memory>
#include <map>
#include <optional>


#include "core/Defines.h"
#include "core/PhysicalControlPlacement.h"
#include "core/AgentGroup.h"
#include "core/AccessPermission.h"
#include "core/AccessPanel.h"
#include "core/PermissionSet.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Background.h"
#include "core/Facade.h"
#include "core/Layer.h"
#include "core/Location.h"
#include "core/Marker.h"
#include "core/Furniture.h"
#include "core/SectorType.h"
#include "core/ChamberSubtype.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Window.h"
#include "core/Dumbwaiter.h"
#include "core/WindowSectorObject.h"
#include "core/Graph.h"
#include "core/Log.h"
#include "core/Simulation.h"
#include "core/SimulationStepTiming.h"
#include "core/Coordination.h"
#include "core/SimulationCoordinator.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"


namespace core
{

	class AgentTagRegistry;
	class AgentBehaviourRegistry;

	class World : public Serializable
	{
		friend class Agent;
		friend class AgentTagRegistry;
		friend class AgentBehaviourRegistry;
		friend class AgentBehaviourRuntimeAdapter;
		friend class Graph;
		// The coordinator owns no entities; it drives the registries below and
		// the private machinery beside them on the World's behalf (ADR 0004).
		friend class SimulationCoordinator;
		friend struct WorldAgentRestorationTestAccess;

	public:

		struct CreateObjectResult
		{
			uint32_t index{ ~0u };
			SectorObjectType type{ SectorObjectType::None };
			std::shared_ptr<Sector> sector;
			InteractionPointId interactionPoint{};
		};

		struct CreateDoorOptions
		{
			uint32_t width{ 1 };
			Door::Height height{ Door::Height::Regular };
			bool controls[2] = { false, false };
			DoorActivationMode activationMode{ DoorActivationMode::Manual };
			float holdOpenSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			// Zero derives one lane per cell of usable threshold width.
			uint32_t crossingLanes{ 0 };
			Door::OpenStyle openStyle{ Door::OpenStyle::OpenUp };
			std::array<std::vector<AccessPermissionId>, 2> controlPermissionRequirements{};
			bool initiallyBroken{ false };
		};

		struct CreateDoorResult
		{
			CreateObjectResult door;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateBulkheadDoorOptions
		{
			bool controls[2] = { true, true };
			DoorActivationMode activationMode{ DoorActivationMode::RemoteControlled };
			float holdOpenSeconds{ CORE_BULKHEAD_DOOR_STAY_OPEN_TIME };
			uint32_t crossingLanes{ 1 };
			float automaticSensorDistance{ CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE };
			std::array<std::vector<AccessPermissionId>, 2> controlPermissionRequirements{};
			bool initiallyBroken{ false };
		};

		struct CreateBulkheadDoorResult
		{
			CreateObjectResult door;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateDumbwaiterOptions
		{
			uint32_t initialStop{ 0 };
			float travelSeconds{ 2.0f };
			std::array<std::vector<AccessPermissionId>, 2> landingPermissionRequirements{};
		};
		bool canAddDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x,
			CreateDumbwaiterOptions const& options, std::string* diagnostic = nullptr) const;
		DumbwaiterId addDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x,
			CreateDumbwaiterOptions const& options);
		DumbwaiterId addDumbwaiter(uint32_t shaftLayer, uint32_t y, uint32_t x)
		{ return addDumbwaiter(shaftLayer, y, x, CreateDumbwaiterOptions{}); }
		std::shared_ptr<const Dumbwaiter> lookupDumbwaiter(DumbwaiterId id) const;
		struct DumbwaiterMovePlan
		{
			bool valid{false};
			std::string diagnostic;
			DumbwaiterId id{};
			uint32_t layer{0}, y{0}, x{0};
		};
		DumbwaiterMovePlan planMoveDumbwaiter(DumbwaiterId id, uint32_t shaftLayer, uint32_t y, uint32_t x) const;
		bool applyDumbwaiterMove(DumbwaiterMovePlan const& plan);
		bool configureDumbwaiter(DumbwaiterId id, CreateDumbwaiterOptions const& options);
		bool removeDumbwaiter(DumbwaiterId id);
		bool hasDumbwaiters() const;
		bool isDumbwaiterOwnedControl(std::shared_ptr<const SectorObject> const& object) const;

		struct CreateWindowOptions
		{
			bool traversable{ false };
			Window::State initialState{ Window::State::Closed };
			Window::Style style{ Window::Style::Clear };
		};

		struct CreateWindowResult
		{
			CreateObjectResult window;
			std::shared_ptr<Window> object;
			TraversalResourceId traversalResource;
		};

		struct CreateForceBridgeOptions
		{
			uint32_t width{ 1 };
			int fromSide{ CORE_SIDE_LEFT };
			bool extensible{ true };  // implies controlled
			bool startExtended{ true };
			uint32_t controlCount{ 1 };
			// Indexed by physical side: left, then right.
			std::array<std::vector<AccessPermissionId>, 2> controlPermissionRequirements{};
			bool initiallyBroken{ false };
		};

		struct CreateForceBridgeResult
		{
			CreateObjectResult forceBridge;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateLadderOptions
		{
			uint32_t levelsHigh;
			bool extensible;  // implies controlled
			bool startExtended;
			uint32_t directionalBatchLimit{ 4 };
			// Indexed by endpoint: low, then high.
			std::array<std::vector<AccessPermissionId>, 2> controlPermissionRequirements{};
			bool initiallyBroken{ false };
		};

		struct CreateLadderResult
		{
			CreateObjectResult ladder;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateStairwellOptions
		{
			uint32_t levelsHigh;
			int mountSide;
			// Zero preserves ordinary, unconstrained bidirectional stairs.
			uint32_t directionalCapacity{ 0 };
			uint32_t directionalBatchLimit{ 4 };
		};

		struct CreateStairwellResult
		{
			uint32_t sectorIndex{ ~0u };
			TraversalResourceId traversalResource;
		};

		struct CreateStaircaseOptions
		{
			uint32_t cellsWide{ 2 };
			// CORE_SIDE_RIGHT rises left-to-right; CORE_SIDE_LEFT is mirrored.
			int riseSide{ CORE_SIDE_RIGHT };
			// Zero is stationary; positive moves up and negative moves down.
			float speed{ 0.0f };
			bool initiallyBroken{ false };
		};

		struct CreateLiftOptions
		{
			uint32_t cellsWide{ 1 };
			std::vector<uint32_t> stopOffsets;
			uint32_t capacity{ 2 };
			float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
			float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			uint32_t initialStop{ 0 };
			// Zero preserves the legacy API behaviour of ending at the highest stop.
			uint32_t levelsHigh{ 0 };
			// Used only by open PlatformLifts. Enclosed Lifts retain their separate
			// minimum-dwell and maximum-boarding timings.
			float platformStopDurationSeconds{ CORE_PLATFORM_LIFT_STOP_DURATION };
			// Per-stop landing-Door opening styles, parallel to stopOffsets.  ~0u
			// means "no override": the generated Door keeps its owner default
			// (OpenApart for a Lift).  The Lift's topology stays fixed; only the
			// authored style varies per stop.
			std::vector<uint32_t> stopDoorOpenStyles{};
			// Per-stop landing call requirements, parallel to stopOffsets.
			std::vector<std::vector<AccessPermissionId>> landingControlPermissionRequirements{};
			// Whole-transport authored condition, shared by Lifts and Platform lifts.
			bool initiallyBroken{ false };
		};

		struct CreateLiftResult
		{
			CreateObjectResult lift;
			std::vector<CreateDoorResult> doors;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct CreatePlatformLiftResult
		{
			CreateObjectResult lift;
			std::vector<CreateObjectResult> buttons;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct CreateShuttleOptions
		{
			uint32_t numCars;
			uint32_t carWidth;
			std::vector<uint32_t> stopOffsets;
			uint32_t initialStop;
			// Passenger capacity of each carriage, not the coupled vehicle total.
			uint32_t capacity{ 1 };
			float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
			float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			bool allowPartialLandings{ false };
			// Bit N selects carriage cell N as a one-cell-wide door.
			uint32_t doorMask{ 1u << 1 };
			// Per-Door opening style overrides across the fixed stop/carriage/door
			// grid, indexed (stop * numCars + carriage) * doorCount + door where
			// doorCount is the number of selected doorMask cells.  ~0u means "no
			// override": the generated Door keeps its owner default (OpenUp for a
			// Shuttle).  The Shuttle's topology stays fixed; only the authored
			// style varies per Door.
			std::vector<uint32_t> doorOpenStyles{};
			// Per physical landing control, using the same fixed grid as doorOpenStyles.
			std::vector<std::vector<AccessPermissionId>> landingControlPermissionRequirements{};
			// One authored condition for the complete coupled vehicle.
			bool initiallyBroken{ false };
		};

		struct CreateShuttleResult
		{
			CreateObjectResult shuttle;
			// Fixed stop/carriage/door grid; unsupported partial landings are empty.
			std::vector<CreateDoorResult> doors;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct ObjectMovePlan
		{
			bool valid{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 };
			// Preview dimensions may differ from the source object (Room Ladders
			// and PlatformLifts recalculate their height at the destination).
			uint32_t previewWidth{ 0 }, previewHeight{ 0 };
			// True when the plan resizes the object instead of only moving it.
			bool resizeRequested{ false };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct PlatformLiftStopCandidate
		{
			uint32_t levelOffset{ 0 };
			bool leftButton{ false };
			bool rightButton{ false };
		};

		struct PlatformLiftEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			CreateLiftOptions options;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct WalkwayEditPlan
		{
			bool valid{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		// A rectangular edit to a Sector's footprint: remove it, move it, or resize
		// it in place.  Rooms and Corridors are the primary subject, and a Background
		// shares the shape because it is edited the same way and needs the same
		// consequence list: taking a Background away from the Windows looking into it
		// takes those Windows with it.
		struct LocationEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 }, levelsHigh{ 0 };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const
			{
				return !consequences.empty();
			}
		};

		struct LiftEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 }, levelsHigh{ 0 };
			std::vector<uint32_t> stopOffsets;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct ShuttleEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 };
			// The coupled vehicle layout the plan authors: carriage count, carriage
			// width in cells, and the door mask selecting one-cell doors within a
			// carriage.  A plan always carries the resolved layout, so a plan that
			// leaves the vehicle alone carries the current values unchanged.
			uint32_t numCars{ 0 };
			uint32_t carWidth{ 0 };
			uint32_t doorMask{ 0 };
			std::vector<uint32_t> stopOffsets;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		// Both stationary chamber types share validated construction replay.
		struct AirlockEditPlan
		{
			bool valid = false, remove = false;
			bool chamber = false, leftToRight = true;
			std::optional<ChamberSubtype> subtype;
			uint32_t sectorIndex = ~0u, x = 0, y = 0, width = 0;
			std::string diagnostic;
		};

		struct LadderEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, levelsHigh{ 0 };
			CreateLadderOptions options{ 0, false, true };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct StairwellEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, levelsHigh{ 0 };
			CreateStairwellOptions options{ 0, CORE_SIDE_LEFT };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct StaircaseEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 };
			CreateStaircaseOptions options{};
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct ShuttleStopCandidate
		{
			uint32_t sectorIndex{ ~0u };
			uint32_t stopOffset{ 0 };
		};

		// Deleting a Layer is destructive, so its consequences are listed before the
		// user confirms them.  Every Sector on the deleted Layer is removed, Transits
		// on that Layer and on the Layer directly behind it lose their landings and
		// are removed, thresholds which cross the deleted Layer are removed, and
		// Agents in removed Sectors are removed.  A Window which the compaction would
		// leave on the back-most Layer is removed too, since a Window needs a Layer
		// behind it.  The Windows on the Layer in front which looked into the deleted
		// Layer go with it, and each one is named in the consequence list.  Layers
		// behind the deleted Layer compact forward by one, keeping their names.
		struct LayerDeletePlan
		{
			bool valid{ false };
			uint32_t layerIndex{ 0 };
			std::string layerName;
			uint32_t layerCountBefore{ 0 };
			uint32_t layerCountAfter{ 0 };
			uint32_t locationsRemoved{ 0 };
			uint32_t transitsRemoved{ 0 };
			uint32_t backgroundsRemoved{ 0 };
			uint32_t doorsRemoved{ 0 };
			uint32_t windowsRemoved{ 0 };
			// Windows which never crossed the deleted Layer, but are deleted because the
			// compaction leaves them on the back-most Layer with nothing behind them.
			uint32_t windowsStranded{ 0 };
			uint32_t agentsRemoved{ 0 };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		// A route destination retained when pauseSimulation tears down live
		// traversal. Each Agent that had a path keeps one until resumeSimulation
		// replays it onto the rebuilt graph.
		struct TopologyPathNode
		{
			uint32_t sectorIndex;
			Vector2 position;
			VertexType type;
			VertexSubType subType;
			std::string key;
			EdgeType edgeType;
			int depth;
			bool furnitureRoute;
			float cumulativePerceivedCost;
			std::optional<float> objectiveDurationSeconds;
			std::optional<EvaluatedRouteCost> diagnosticCost;
		};

		struct TopologyPathIntent
		{
			MarkerId destinationMarker{};
			SectorId destinationSector;
			Vector2 destinationPosition;
			Vector2 destinationLocalPosition;
			bool wasPathing{ false };
			// Values only: neither a paused intent nor a planning goal may keep the
			// retired graph alive. Rebinding requires unchanged geometry and depth.
			std::vector<TopologyPathNode> retainedNodes;
			std::optional<RouteDiagnosticContext> retainedContext;
			bool resumeLocalTraversal{ false };
			// A continuous stair crossing can resume from its physical pause position
			// instead of walking back to the edge's source Vertex.
			bool resumeContinuousTraversal{ false };
			EdgeType traversalEdgeType{ EdgeType::Location };
			Vector2 traversalSourcePosition;
			Vector2 traversalDestinationPosition;
			// Preserves the exact historical explanation after pause tears down the
			// live Path. It is transient and never serialized.
			std::optional<PathRouteDiagnostics> routeDiagnostics;
		};

	public:

		static CreateDoorOptions ManualDoor1Options, RemoteControlledDoor1Options, UnavailableDoor1Options;

		static CreateDoorOptions ManualDoor2Options, RemoteControlledDoor2Options, UnavailableDoor2Options;

	private:

		std::string mName;

		// Process-local, opaque identity used to keep World-owned clipboard
		// references from acquiring meaning in another World. It is deliberately
		// neither authored nor serialized.
		std::string mClipboardIdentity;

		// Authored entropy root for deterministic per-Agent behaviour streams.
		// Live stream positions remain runtime-only and are recreated from this
		// value, Agent ID, and behaviour ID on reset or reload.
		uint64_t mRandomSeed{ 0 };

		uint32_t mCellsWide, mLevelsHigh;

		std::vector<std::shared_ptr<Layer>> mLayers;
		std::vector<std::string> mLayerNames;
		std::vector<std::string> mLevelNames;

		std::vector<std::shared_ptr<Sector>> mSectors;


		std::shared_ptr<Graph> mGraph;

		// Simulation behaviour belongs to the coordinator (ADR 0004). World
		// owns it and stays the facade (design pattern) through which every
		// caller, Agent included, reaches it; the coordinator owns no entities
		// and reaches the registries below through this World.
		SimulationCoordinator mSimulationCoordinator;

		EntityRegistry<AgentId, Agent> mAgents;

		// Legacy pointer-facing APIs use this reverse index only to recover an ID;
		// the registry above remains the sole owner.
		std::map<Agent const*, AgentId> mAgentIds;

		// Authored Agent group definitions (ADR 0006). Registry keys are
		// allocated monotonically and never reused, so iterating the registry
		// enumerates the groups in creation order - before and after a rename,
		// and across save/load, which restores each group under its own ID.
		EntityRegistry<AgentGroupId, AgentGroup> mAgentGroups;

		// Access permission identity is its fixed slot plus one. A slot is not
		// returned to the allocator until deletion has cleared every reference.
		std::array<std::unique_ptr<AccessPermission>, AccessPermission::Capacity> mAccessPermissions{};
		EntityRegistry<PermissionSetId, PermissionSet> mPermissionSets;
		std::map<InteractionPointId, std::bitset<256>> mPendingPermissionRequirements;
		struct AuthoredControlRequirement
		{
			size_t constructionRecord{ 0 };
			size_t slot{ 0 };
		};
		std::map<InteractionPointId, AuthoredControlRequirement> mAuthoredControlRequirements;

		// Marker identity is carried by the authored Marker itself. The high-water
		// mark remains after deletion, so an identity is never issued twice.
		uint64_t mNextMarkerId{ 1 };
		std::shared_ptr<const FurnitureCatalogue> mFurnitureCatalogue;
		std::string mFurnitureCatalogueFilename;
		std::vector<FurnitureInstance> mFurniture;
		uint64_t mNextFurnitureId{ 1 };
		struct MovementGoal
		{
			MarkerId marker{};
			Vector2 position;
			bool cancelling{ false };
			SectorId sector{};
			RouteLossReason routeLossReason{ RouteLossReason::None };
			bool behaviourOwned{ false };
			bool planningDeferred{ false };
			bool startPathing{ true };
			RouteLossReason planningFailureReason{ RouteLossReason::Unreachable };
			std::optional<TopologyPathIntent> fallbackIntent;
			// Voluntary planning owns historical evidence, never an active traversal Path.
			std::shared_ptr<Path> retainedPath;
			uint32_t retainedFromNode{ 0 };
			uint64_t voluntaryPlanningStartedTick{ 0 };
			std::string selectedAction{ IdleAction };
			bool actionInvalidated{ false };
		};
		std::map<AgentId, MovementGoal> mMovementGoals;
		std::shared_ptr<const ActionRegistry> mActionRegistry;
		std::string mActionRegistryFilename;
		std::map<MarkerId, std::vector<std::string>> mMarkerActions;
		bool mActionExecutionFailed{ false };
		bool actionAvailable(MarkerId marker, std::string_view action) const;
		void executeMarkerAction(AgentId agent, MarkerId marker, std::string_view action, SimulationEvent& event);
		FurnitureInstance const* furnitureForMarker(MarkerId marker) const;
		ActionViews actionViews(AgentId agent, MarkerId marker) const;
		void applyActionResult(AgentId agent, MarkerId marker, ActionExecutionResult result, SimulationEvent& event,
			bool finishing = false);
		void finishFurnitureUse(AgentId agent);
		std::vector<SimulationEvent> mPendingMovementOutcomes;

		struct AgentTagRegistryReference
		{
			std::string filename;
			std::string expectedUuid;
		};

		// Agent tag definitions are an independent external document (ADR 0007).
		// The World persists only this basename/UUID reference. The loaded
		// registry is deliberately not a child for dirty-state purposes.
		std::optional<AgentTagRegistryReference> mAgentTagRegistryReference;
		std::shared_ptr<AgentTagRegistry> mAgentTagRegistry;

		// Agent behaviour definitions are an independent external package
		// document. The World persists only this package-directory basename
		// and expected UUID reference. The loaded registry is deliberately not a
		// child for dirty-state purposes.
		struct AgentBehaviourRegistryReference
		{
			std::string packageName;
			std::string expectedUuid;
		};
		std::optional<AgentBehaviourRegistryReference> mAgentBehaviourRegistryReference;
		std::shared_ptr<AgentBehaviourRegistry> mAgentBehaviourRegistry;
		// An incompatible newer schema is a recoverable dependency state: authored
		// values stay attached to their recorded revision, while simulation and
		// registry save remain blocked until a coordinated migration repairs them.
		std::string mAgentBehaviourDependencyDiagnostic;
		// Every World owns its own live Lua state. The adapter's pimpl keeps all
		// Lua/sol2 types out of this domain header and its per-Agent environments
		// prevent mutable module or instance state crossing assignments.
		std::unique_ptr<AgentBehaviourRuntimeAdapter> mAgentBehaviourRuntime;

		bool validateAgentBehaviourAssignmentAgainst(
			AgentBehaviourRegistry const& registry, AgentBehaviourId behaviour,
			uint64_t revision, AgentBehaviourConfiguration const& configuration,
			AgentBehaviourConfiguration* normalized = nullptr,
			std::string* diagnostic = nullptr) const;
		bool inspectAgentBehaviourAssignments(AgentBehaviourRegistry const& registry,
			std::string* diagnostic = nullptr) const;
		uint32_t countAgentBehaviourAssignments() const;
		MovementCommandResult inspectBehaviourMoveToMarker(AgentId agent,
			MarkerId marker, std::string_view action = IdleAction) const;
		MovementCommandResult moveBehaviourAgentToMarker(AgentId agent, MarkerId marker,
			std::string_view action = IdleAction);
		MovementCommandResult inspectBehaviourMovementCancellation(AgentId agent) const;
		MovementCommandResult cancelBehaviourAgentMovement(AgentId agent);

		// Coordinated external-document history keeps only a weak copy. It can
		// therefore recognize that this exact World closed without retaining it
		// or mistaking a later World allocated at the same address for it.
		std::shared_ptr<void const> mLifetimeToken{ std::make_shared<uint8_t>(0) };

		enum class AgentTagSampleRepairAction
		{
			None,
			Clear,
			Resample
		};

		struct AgentTagReconciliation
		{
			AgentId agent{};
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

		// Checks every assigned stable ID and inherited property against a
		// prospective registry. Opening may additionally plan repairs for samples
		// whose definitions legitimately changed while the World was closed.
		bool inspectAgentTagAssignments(AgentTagRegistry const& registry,
			bool allowSampleReconciliation,
			std::vector<AgentTagReconciliation>* repairs,
			std::string* diagnostic = nullptr) const;
		bool agentTagAssignmentsAreValid(AgentTagRegistry const& registry,
			std::string* diagnostic = nullptr) const;
		void applyAgentTagReconciliations(
			std::vector<AgentTagReconciliation> const& repairs);
		void reconcileAgentTagAssignments(AgentTagRegistry const& registry);
		uint32_t countAgentTagAssignments(AgentTagId id) const;
		void clearAgentTagAssignments(AgentTagId id);
		void clearAllAgentTagAssignmentsAndSamples();
		void addAgentTagWalkSpeedModifierSamples(AgentTagId id,
			AgentWalkSpeedModifierProperty const& property);
		void clearAgentTagWalkSpeedModifierSamples(AgentTagId id);
		void addAgentTagHeightModifierSamples(AgentTagId id,
			AgentHeightModifierProperty const& property);
		void clearAgentTagHeightModifierSamples(AgentTagId id);
		void addAgentTagStairSpeedModifierSamples(AgentTagId id,
			AgentStairSpeedModifierProperty const& property);
		void clearAgentTagStairSpeedModifierSamples(AgentTagId id);
		void addAgentTagLadderSpeedModifierSamples(AgentTagId id,
			AgentLadderSpeedModifierProperty const& property);
		void clearAgentTagLadderSpeedModifierSamples(AgentTagId id);
		void addAgentTagInteractionAversionSamples(AgentTagId id,
			AgentInteractionAversionProperty const& property);
		void clearAgentTagInteractionAversionSamples(AgentTagId id);
		void addAgentTagEffortAversionSamples(AgentTagId id,
			AgentEffortAversionProperty const& property);
		void clearAgentTagEffortAversionSamples(AgentTagId id);
		void addAgentTagWaitingAversionSamples(AgentTagId id,
			AgentWaitingAversionProperty const& property);
		void clearAgentTagWaitingAversionSamples(AgentTagId id);
		void addAgentTagCrowdAversionSamples(AgentTagId id,
			AgentCrowdAversionProperty const& property);
		void clearAgentTagCrowdAversionSamples(AgentTagId id);
		void addAgentTagRiskAversionSamples(AgentTagId id,
			AgentRiskAversionProperty const& property);
		void clearAgentTagRiskAversionSamples(AgentTagId id);
		void addAgentTagRouteFamiliaritySamples(AgentTagId id,
			AgentRouteFamiliarityProperty const& property);
		void clearAgentTagRouteFamiliaritySamples(AgentTagId id);
		void addAgentTagRoutePersistenceSamples(AgentTagId id,
			AgentRoutePersistenceProperty const& property);
		void clearAgentTagRoutePersistenceSamples(AgentTagId id);
		void addAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id,
			AgentMinimumRoutePlanningTimeProperty const& property);
		void clearAgentTagMinimumRoutePlanningTimeSamples(AgentTagId id);
		void addAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id,
			AgentMaximumRoutePlanningTimeProperty const& property);
		void clearAgentTagMaximumRoutePlanningTimeSamples(AgentTagId id);

		// Case-sensitive name lookup across the groups this World owns, with
		// one group optionally excluded so a group renaming itself to the name
		// it already carries is not its own collision.
		bool agentGroupNameTaken(std::string const& trimmed,
			AgentGroupId except = AgentGroupId{}) const;
		bool accessPermissionNameTaken(std::string const& trimmed,
			AccessPermissionId except = AccessPermissionId{}) const;
		bool permissionSetNameTaken(std::string const& trimmed,
			PermissionSetId except = PermissionSetId{}) const;
		std::bitset<256> currentDirectAccessGrants(Agent const& agent) const;
		std::set<PermissionSetId> currentPermissionSets(Agent const& agent) const;
		std::bitset<256> effectiveAccessGrants(Agent const& agent) const;
		AccessPermissionId accessPermissionNamed(std::string_view name) const;
		PermissionSetId permissionSetNamed(std::string_view name) const;
		std::vector<AccessPermissionId> missingInteractionPermissions(
			InteractionPoint const& point, Agent const& agent) const;
		bool agentSatisfiesDoorPermission(Door const& door, Agent const& agent) const;
		void replanAgentsAffectedByControlRequirement(TraversalResourceId resource,
			std::bitset<256> const& previous, std::bitset<256> const& next);
		bool agentPathEntersLocation(Agent const& agent, Sector const& location) const;
		void reconsiderAgentAuthorizationPath(Agent& agent, AccessPermissionId changed,
			bool gained);

		bool markerNameTaken(std::string const& trimmed,
			MarkerId except = MarkerId{}) const;
		std::string nextGeneratedMarkerName() const;
		std::shared_ptr<Marker> mutableMarker(MarkerId id) const;

		EntityRegistry<InteractionPointId, InteractionPoint> mInteractionPoints;

		EntityRegistry<InteractionRequestId, InteractionRequest> mInteractionRequests;

		EntityRegistry<DeviceOperationId, DeviceOperation> mDeviceOperations;
		// Runtime-only device identities, independent of movement admission.
		uint64_t mNextAccessPanelId{ 1 };
		std::map<AccessPanelId, std::weak_ptr<AccessPanel>> mAccessPanels;
		uint64_t mNextBoothWindowId{ 1 };
		uint64_t mNextDumbwaiterId{ 1 };
		std::map<BoothWindowId, std::weak_ptr<BoothWindow>> mBoothWindows;

		EntityRegistry<TraversalResourceId, TraversalResource> mTraversalResources;

		EntityRegistry<TraversalRequestId, TraversalRequest> mTraversalRequests;

		EntityRegistry<TraversalPermitId, TraversalPermit> mTraversalPermits;

		uint64_t mSimulationTick{ 0 };
		SimulationStepTiming mSimulationStepTiming;

		uint64_t mNextEventSequence{ 1 };

		uint64_t mNextQueueTicketValue{ 1 };

		uint64_t mNextDoorOpenLeaseValue{ 1 };

		double mAccumulatedTime{ 0.0 };
		double mTimeScale{ 1.0 };

		mutable SimulationSnapshot mSnapshotCache;
		mutable bool mSnapshotValid{ false };
		mutable uint64_t mSnapshotBuildCount{ 0 };
		mutable uint64_t mSnapshotMutationRevision{ 0 };
		mutable std::map<AgentId, InteractionRequestId> mPendingInteractionIndex;
		mutable bool mPendingInteractionIndexValid{ false };
		mutable uint64_t mPendingInteractionIndexRevision{ 0 };
		std::map<AgentId, AgentSnapshot> mTickAgents;
		std::map<DeviceOperationId, DeviceOperationState> mTickOperations;
		uint64_t mTickOperationLimit{ 0 };
		bool mRecordingTickChanges{ false };

		SimulationPhase mCurrentPhase{ SimulationPhase::None };
		SimulationObserver* mSimulationObserver{ nullptr };

		std::vector<SimulationEvent> mEvents;

		RouteChoicePolicy mRouteChoicePolicy;
		TraversalWaitingPolicy mTraversalWaitingPolicy;

		TraversalGeometryPolicy mTraversalGeometryPolicy;

		// Structural edits are transactional at the graph boundary. The world may
		// only be changed after an explicit pause; the previous graph remains live
		// until a replacement has built and validated successfully.
		bool mBuildFinished{ false };
		bool mSimulationPaused{ false };
		bool mTopologyDirty{ true };
		// Whole-unit edits already reset their own device, not unrelated cycles.
		bool mOnlyDumbwaiterTopologyEdits{ true };
		bool mTopologyValid{ false };
		uint64_t mTopologyGeneration{ 0 };
		std::string mTopologyDiagnostic;

		std::map<AgentId, TopologyPathIntent> mPausedPathIntents;

		struct RestoredPathIntent
		{
			std::shared_ptr<const Vertex> destination;
			bool active{ false };
		};
		// Deserialization restores route intent before an external Agent-tag
		// registry is available. Keep that intent unevaluated until registry
		// reconciliation has supplied every effective routing property (#221).
		std::map<AgentId, RestoredPathIntent> mPendingRestoredPathIntents;

		// Non-fatal document-load diagnostics. These are transient editor feedback,
		// never authored World data. An unreachable saved destination leaves its
		// Agent idle instead of making the entire document malformed.
		std::vector<std::string> mLoadWarnings;

		Log mBuildLog;

		// Authored facade operations are the persistence boundary. Replaying them
		// reconstructs sectors, objects, controls, and traversal resources while
		// finishBuild() regenerates graph and pathing data.
		enum class ConstructionType : uint8_t
		{
			Corridor,
			Room,
			Ladder,
			Stairwell,
			Staircase,
			Lift,
			Shuttle,
			Door,
			Window,
			BulkheadDoor,
			LightSwitch,
			ForceBridge,
			SectorLadder,
			PlatformLift,
			Walkway,
			Marker,
			RemoveWall,
			RemoveMarker,
			ObjectTombstone,
			// Appended last: version 1 stored the record kind numerically, so every
			// earlier value has to keep its number.
			Background,
			// Appended after Background for the same reason: a Facade is its own
			// producing record, replayed with all wall ends open intrinsically
			// (ADR 0003).
			Facade,
			Airlock,
			Furniture,
			Chamber,
			BoothWindow,
			Dumbwaiter,
			MoveDumbwaiter,
			AccessPanel,
			ConfigureAccessPanel,
			RemoveAccessPanel
		};

		// Compact tagged command storage. Field meanings are determined by type and
		// kept private so the public model is not coupled to its YAML representation.
		struct ConstructionRecord
		{
			ConstructionType type{};
			std::string name{};

			// The Layer the record's object is authored on.  For a Transit this is the
			// Layer it sits on; for a threshold it is the front Layer of its pair.
			// Records written before Transits and Doors carried a Layer leave this unset
			// and replay against the front pair, which is where every legacy object lived.
			uint32_t layer{ ~0u };

			uint32_t a{ 0 }, b{ 0 }, c{ 0 }, d{ 0 }, e{ 0 }, f{ 0 }, g{ 0 }, h{ 0 };
			int32_t i{ 0 }, j{ 0 };
			float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
			ChamberSubtype chamberSubtype{ ChamberSubtype::SecurityScanner };
			float scannerSensorDistance{ 0.5f };
			std::optional<float> accessPanelSpeed{};
			bool p{ false }, q{ false };
			bool initiallyBroken{ false };
			// Door: the activation mode the Door had before the editor's Buttons
			// option first gave it Buttons, or -1 when the Buttons were loaded as
			// part of the authored definition. Removal restores a recorded mode and
			// otherwise falls back to manual activation.
			int32_t preButtonActivationMode{ -1 };
			std::vector<uint32_t> values{};
			// Lift: per-stop landing-Door opening style overrides, parallel to
			// values (stopOffsets).  Shuttle: per-Door overrides across the fixed
			// stop/carriage/door grid.  ~0u means "no override"; a record whose
			// overrides are all defaults persists none of them.
			std::vector<uint32_t> overrides{};
			// Ordinary and Bulkhead Door: the requirements of the controls on the
			// two authored approach sides. Unlike Interaction point IDs, these stay
			// associated with their side when a structural edit rebuilds the World.
			std::array<std::vector<uint32_t>, 2> controlPermissionRequirements{};
			// Lift/Platform lift: stop order. Shuttle: fixed stop/carriage/door grid.
			std::vector<std::vector<uint32_t>> landingControlPermissionRequirements{};
			// Lift, Platform lift, and Shuttle requirements, parallel to values (Stops).
			std::vector<std::vector<uint32_t>> destinationPermissionRequirements{};
			// Room/Corridor: all-of passage requirement, independent of controls.
			std::vector<uint32_t> locationPermissionRequirement{};
			void retainDestinationRequirements(std::vector<uint32_t> const& stops);
			// Marker / RemoveMarker: stable World-local identity. Marker also
			// uses name above and c for its MarkerProperties bitfield. Zero identity
			// occurs only while migrating versions 1-10.
			MarkerId markerId{};
			uint64_t furnitureId{ 0 };
			int furnitureDepth{ 0 };
			std::string definitionKey{};
			std::vector<FurnitureDestination> furnitureDestinations{};
			DumbwaiterId dumbwaiterId{};
		};

		bool validateAccessPanel(uint32_t sectorIndex, uint32_t levelOffset, uint32_t cellX,
			AccessPanelGeometry geometry, uint32_t ignoredObject, std::string* diagnostic) const;
		static void retireRemovedAccessPanelRecords(std::vector<ConstructionRecord>& records);
		void validateRetainedAccessPanels() const;
		void validatePanelWallRectangle(uint32_t sectorIndex, Vector2 min, Vector2 max) const;
		void restoreFurniture(ConstructionRecord const& record);
		bool markerHasNoBehaviourReferences(MarkerId id, std::string* diagnostic) const;
		std::string furnitureSupportDiagnostic(uint32_t sector, uint32_t x, uint32_t y) const;
		ConstructionRecord const* findLocationPermissionRecord(uint32_t sectorIndex) const;
		ConstructionRecord const* findLiftDestinationRecord(uint32_t sectorIndex, uint32_t objectIndex = ~0u) const;
		ConstructionRecord const* findLiftDestinationRecord(TraversalResource const& resource) const;
		std::vector<ConstructionRecord> mConstructionRecords;
		bool mDeserializingConstruction{ false };

		using PhysicalControlCandidate = physicalControl::Candidate;

		struct PhysicalControlPlacement
		{
			uint32_t layerIndex{ 0 };
			uint32_t sectorIndex{ 0 };
			uint32_t objectIndex{ 0 };
			uint32_t cellY{ 0 };
			std::vector<PhysicalControlCandidate> candidates;
			uint32_t defaultCandidate{ 0 };
			uint32_t currentCandidate{ 0 };
			Vector2 interactionOffset{};
			bool hasInteractionOffset{ false };
			physicalControl::Owner owner{};
		};

		PhysicalControlPlacement const* physicalControlPlacement(InteractionPointId point) const;
		std::vector<PhysicalControlPlacement> mPhysicalControlPlacements;
		bool mResolvingPhysicalControls{ false };
		// Current record ordinal during detached/live replay, independent of the
		// not-yet-adopted construction log. Used for stable landing permissions.
		size_t mConstructionReplayIndex{ 0 };

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		void recordConstruction(ConstructionRecord record);
		bool preflightDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
			CreateDumbwaiterOptions const& options, DumbwaiterId ignored, std::string* diagnostic) const;
		void attachDumbwaiter(std::shared_ptr<Dumbwaiter> const& unit);
		void detachDumbwaiter(std::shared_ptr<const Dumbwaiter> const& unit);
		DumbwaiterId createDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
			CreateDumbwaiterOptions const& options, DumbwaiterId id);

		static std::string constructionTypeName(ConstructionType type);

		static ConstructionType constructionTypeFromName(std::string const& name);

		// A record which creates a Sector. A producing record's position among the
		// producers is its live Sector index, so every record-to-Sector mapping has to
		// agree on exactly this set.
		static bool constructionTypeCreatesSector(ConstructionType type);

		void serializeConstructionRecord(Serializer& serializer, ConstructionRecord const& record) const;

		ConstructionRecord deserializeConstructionRecord(Serializer& serializer, uint32_t version) const;

		void applyConstructionRecord(ConstructionRecord const& record);

		bool prepareLocationEdit(LocationEditPlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			std::string& diagnostic) const;

		bool prepareObjectMove(ObjectMovePlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			uint32_t& newObjectIndex, std::string& diagnostic) const;

		bool normalizeRoomLadderRecords(std::vector<ConstructionRecord>& records,
			std::string& diagnostic) const;

		bool roomLadderIsActive(std::shared_ptr<const Ladder> const& ladder) const;

		bool forceBridgeIsActive(std::shared_ptr<const ForceBridge> const& forceBridge) const;

		bool platformLiftIsActive(std::shared_ptr<const Lift> const& lift) const;

		bool preparePlatformLiftEdit(PlatformLiftEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareLiftEdit(LiftEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareShuttleEdit(ShuttleEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		// Resize the Shuttle track and, when the vehicle arguments are supplied
		// (non-zero), re-author its coupled vehicle in the same edit.  A zero
		// argument keeps that part of the authored vehicle as it is.
		ShuttleEditPlan planResizeShuttleWithVehicle(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth,
			uint32_t doorMask) const;

		bool prepareAirlockEdit(AirlockEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;
		bool prepareLadderEdit(LadderEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareStairwellEdit(StairwellEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		std::vector<ConstructionRecord> canonicalConstructionRecords(
			std::vector<ConstructionRecord> records) const;

		std::set<DumbwaiterId> locationEditDumbwaiters(LocationEditPlan const& plan) const;
		std::array<uint32_t, 4> dumbwaiterRecordLandings(ConstructionRecord const& record) const;
		void removeDumbwaiterRecords(std::vector<ConstructionRecord>& records,
			std::set<DumbwaiterId> const& removed, std::vector<uint32_t>* sectorMap = nullptr) const;
		void reconcileDumbwaiterReplay(std::vector<ConstructionRecord>& records,
			std::vector<uint32_t>& originalToReplay) const;
		void removeBoothWindowsAtSupport(std::vector<ConstructionRecord>& records,
			uint32_t layer, uint32_t x, uint32_t y) const;

		void rebuildFromConstructionRecords(std::vector<ConstructionRecord> records,
			uint32_t movedSectorIndex = ~0u, int deltaX = 0, int deltaY = 0);

		// What a reset/replay path keeps of a live Agent so the Agent can be put
		// back once the replay is done. The Agent group assignment belongs to what
		// is kept: an edit that keeps an Agent keeps how it is classified (#122).
		// Activation and Agent behaviour assignment belong to what is kept for the
		// same reason: an edit that keeps an Agent keeps its simulation and movement
		// authority. The Sector the
		// Agent was standing in travels with it because the edits decide which
		// Agents they shift or drop by where those Agents were, not by where they
		// land afterwards.
		struct CarriedAgent
		{
			AgentId id;
			std::string name;
			uint32_t flags{ 0 };
			uint32_t sectorIndex{ 0 };
			uint32_t layer{ 0 };
			Vector2 position{};
			AgentGroupId agentGroup{};
			std::bitset<256> directAccessGrants;
			std::set<PermissionSetId> permissionSets;
			std::set<AgentTagId> agentTags;
			std::optional<AgentPropertySample> walkSpeedModifierSample;
			std::optional<AgentPropertySample> heightModifierSample;
			std::optional<AgentPropertySample> stairSpeedModifierSample;
			std::optional<AgentPropertySample> ladderSpeedModifierSample;
			std::optional<float> individualLadderSpeedModifier;
			std::optional<AgentBehaviourAssignment> behaviourAssignment;
			bool active{ true };
			uint64_t routePlanningSequence{ 0 };
			uint64_t routePlanningTotalTicks{ 0 };
			uint64_t routePlanningRemainingTicks{ 0 };
			std::optional<float> individualMinimumRoutePlanningTime;
			std::optional<float> individualMaximumRoutePlanningTime;
			std::optional<AgentPropertySample> minimumRoutePlanningTimeSample;
			std::optional<AgentPropertySample> maximumRoutePlanningTimeSample;
			// Every remaining authored individual property and persisted sample
			// travels too: a structural replay is neither Reset nor re-authoring, so
			// losing one would silently revert the Agent to a neutral default (#328).
			std::optional<AgentColour> individualColour;
			std::optional<float> individualEscalatorWalkingChance;
			std::optional<float> individualWalkSpeedModifier;
			std::optional<float> individualHeightModifier;
			std::optional<float> individualStairSpeedModifier;
			std::optional<float> individualInteractionAversion;
			std::optional<float> individualEffortAversion;
			std::optional<float> individualWaitingAversion;
			std::optional<float> individualCrowdAversion;
			std::optional<float> individualRiskAversion;
			std::optional<float> individualRouteFamiliarity;
			std::optional<float> individualRoutePersistence;
			std::optional<bool> individualPermissionAdherence;
			std::optional<MobilityProfile> individualMobilityProfile;
			std::optional<AgentPropertySample> interactionAversionSample;
			std::optional<AgentPropertySample> effortAversionSample;
			std::optional<AgentPropertySample> waitingAversionSample;
			std::optional<AgentPropertySample> crowdAversionSample;
			std::optional<AgentPropertySample> riskAversionSample;
			std::optional<AgentPropertySample> routeFamiliaritySample;
			std::optional<AgentPropertySample> routePersistenceSample;
			// Runtime authorization overlays survive pause/resume and disappear only
			// on Reset; a structural edit is a pause, not a Reset, so they travel
			// with the Agent as well (#328).
			std::bitset<256> runtimeDirectGrantAdditions;
			std::bitset<256> runtimeDirectGrantRemovals;
			std::set<PermissionSetId> runtimePermissionSetAdditions;
			std::set<PermissionSetId> runtimePermissionSetRemovals;
			// Escalator and route-journey deterministic draws stay on the
			// uninterrupted run's stream, exactly like the route-planning stream
			// above: structural replay is not Reset (#328).
			uint64_t escalatorTraversalSequence{ 0 };
			uint64_t routeJourneySequence{ 0 };
			int localDepth{ 0 };
			Vector2 resetPosition;
			uint32_t resetLayer;
			int resetDepth;
			MarkerId resetDestinationMarker;
			bool resetPathActive;
			std::string resetAction;
			Pose pose{ Pose::Standing };
			MarkerId occupiedUsablePoint{};
		};

		// Captures every Agent that stands in a Sector. A path adjusts the carried
		// Layer, position or membership for the edit it is about to replay, then
		// hands the result to restoreCarriedAgents().
		std::vector<CarriedAgent> captureAgentsForReplay() const;

		// Puts carried Agents back into the rebuilt World. One that no longer
		// has somewhere legal to stand is left behind - that is the edit's own
		// casualty, and it stops counting towards its group - and one that comes
		// back comes back assigned to the same Agent group, provided this World
		// still owns that group, so no replay can leave a dangling assignment.
		//
		// `landingChecked` re-judges the floor under each Agent before it is put
		// back. Edits that move or remove floor beneath the World (Layer
		// deletion, Room and Background editing) ask for it; edits inside an
		// unchanged footprint (Door/Window removal, object movement, plain
		// record replay) do not, which is what each of those paths did before
		// they shared this seam.
		void restoreCarriedAgents(std::vector<CarriedAgent> const& carried,
			bool landingChecked);

		static std::string defaultLayerName(uint32_t layer);

		struct LayerDeleteImpact
		{
			std::vector<bool> sectorRemoved;
			uint32_t locationsRemoved{ 0 };
			uint32_t transitsRemoved{ 0 };
			uint32_t backgroundsRemoved{ 0 };
			uint32_t doorsRemoved{ 0 };
			uint32_t windowsRemoved{ 0 };
			// Windows which do not cross the deleted Layer but would compact onto the new
			// back-most Layer, and so lose the Layer behind them.
			uint32_t windowsStranded{ 0 };
		};

		// Layers of the live Sectors which hold the threshold object authored at a
		// cell.  A threshold is shared by the Sectors on both sides of it.
		std::set<uint32_t> thresholdLayers(SectorObjectType type, uint32_t x, uint32_t y) const;

		// Every distinct Window the World holds, as it is registered in a Sector.
		// A Window carries no cell position of its own - the cell it sits on belongs
		// to its SectorObject - so the two travel together.  A Window's SectorObject
		// is registered in both of the Sectors it joins, so the same Window is seen
		// twice while scanning and is reported once.
		std::vector<std::shared_ptr<const WindowSectorObject>> allWindowObjects() const;

		// The Windows which look into `background` and stop looking at it once the
		// Background occupies the given footprint.  A Background which is removed
		// covers nothing, so every Window looking into it is uncovered; a moved or
		// shrunk one uncovers only the cells it lets go of.  A Window looks straight
		// behind itself, so its back cells are its own rectangle on the Layer behind.
		std::vector<std::shared_ptr<const WindowSectorObject>> windowsUncoveredByBackground(
			std::shared_ptr<const Sector> const& background, bool covered,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		// The consequence lines naming every Window which loses the Background the
		// plan edits, so the confirmation popup spells out the cascade rather than
		// only counting it.
		void addUncoveredWindowConsequences(LocationEditPlan& plan) const;

		// Authored construction records rewritten for a Background removal, move, or
		// resize: the Background record follows the plan, the records of the Windows
		// which lose it are dropped, and every remaining Sector index is re-pointed
		// against the compacted World.
		bool prepareBackgroundEdit(LocationEditPlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			std::string& diagnostic) const;

		// Authored construction records rewritten for a Layer deletion: casualties
		// are dropped and every remaining Layer index compacts forward by one.
		std::vector<ConstructionRecord> recordsWithoutLevel(uint32_t level,
			std::vector<bool>& removed, std::vector<std::string>& consequences) const;
		std::vector<ConstructionRecord> recordsWithoutLayer(uint32_t layerIndex,
			LayerDeleteImpact& impact) const;

		void resetForDeserialization(std::string name, uint32_t cellsWide, uint32_t levelsHigh,
			bool preserveBehaviourRuntime = false);

		// Evaluate saved route intent only after every effective Agent property is
		// available. Worlds without a tag registry call this at the end of
		// deserialization; referenced registries call it after reconciliation.
		void rebuildRestoredAgentPaths();

		// Constructs a validation candidate with the same dimensions and layer count as this World.
		std::unique_ptr<World> makeCandidateWorld() const;

		// Shared bodies of the Location and Facade footprint edits. They have the
		// same occupiable cascade, gated on the Sector type each public entry point
		// permits and refusing anything else with that entry point's diagnostic.
		LocationEditPlan planResizeOccupiable(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t cellsWide, uint32_t levelsHigh,
			SectorType requiredType, std::string const& refusal) const;

		LocationEditPlan planRemoveOccupiable(uint32_t sectorIndex,
			SectorType requiredType, std::string const& refusal) const;

		void validateCellOccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellUnoccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellIsInSector(std::string const& caller, uint32_t x, uint32_t y, std::shared_ptr<const Sector> sector) const;

		void validateCellHasNoObject(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasNoDoor(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		bool validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
			int riseSide, std::string& diagnostic) const;


		void validateCellTraversableOnFoot(std::string const& caller, std::string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateLayer(std::string const& caller, uint32_t layerIndex) const;

		void validateBounds(std::string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		void validateLayerSpace(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		void validateObjectAllowedInSector(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		void validateObjectAllowedInSectorAsLookTarget(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		// The span must fall inside a single Sector, because crossing a Sector boundary
		// breaks the traversal geometry the caller is about to build. The one
		// relaxation is allowAllBackgroundSpan, used for the Layer behind a Window:
		// there a span of nothing but Backgrounds may cover several Background
		// Sectors, since a Background takes no part in traversal and what lies behind
		// an aperture is read from the cell grid. A span that mixes a Background with
		// any other Sector is still refused.
		void validateSpaceOnlyInOneSector(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, bool allowAllBackgroundSpan = false) const;

		void validateSectorDoorOptions(std::string const& caller, CreateDoorOptions const& options) const;

		void validateSectorForceBridgeOptions(std::string const& caller, CreateForceBridgeOptions const& options) const;

		void validateSectorLadderOptions(std::string const& caller, CreateLadderOptions const& options) const;

		void validateLiftOptions(std::string const& caller, CreateLiftOptions const& options) const;

		void validateShuttleOptions(std::string const& caller, CreateShuttleOptions const& options) const;

		void beginStructuralEdit(std::string const& operation, bool preserveOtherDumbwaiterCycles = false);

		// The simulation-side work of a topology rebuild - taking every live
		// traversal apart, remembering the route each Agent was working to,
		// restoring those routes onto the rebuilt graph, and publishing the
		// boundary events - lives in SimulationCoordinator (ADR 0004 stage 5).
		// pauseSimulation and resumeSimulation stay here, on the structural-edit
		// side of the edit/simulation boundary, and call it.

		void validateTraversalTopology(Graph const& graph) const;

		std::shared_ptr<Sector> _getSector(uint32_t index);

		std::shared_ptr<Layer> getLayer(uint32_t layerIndex);

		uint32_t createLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor);

		// A Transit is created on layerIndex and lands on the Layer directly in front of
		// it, so every landing cell is read from layerInFront(layerIndex).
		uint32_t createLadder(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options);

		uint32_t createStairwell(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t levelsHigh, int mountSide);

		uint32_t createStaircase(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			int riseSide, float speed);

		CreateObjectResult createLift(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			uint32_t levelsHigh, std::vector<uint32_t> const& stopOffsets);

		CreateObjectResult createShuttle(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, std::vector<uint32_t> const& stopOffsets);

		// A Door is authored on the front Layer of its pair and opens into the Layer
		// directly behind it.
		CreateObjectResult createDoor(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			Door::Height height = Door::Height::Regular, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier = nullptr, bool boothWindow = false);
		CreateWindowResult addWindowAperture(uint32_t layer, uint32_t y, uint32_t x,
			uint32_t width, uint32_t height, CreateWindowOptions const& options, bool boothWindow);

		CreateObjectResult createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side);

		// All stationary physical owners use authored canonical demands.
		CreateObjectResult createPhysicalControl(std::string const& name, uint32_t layerIndex,
			uint32_t y, physicalControl::Demand const& demand, uint32_t flags,
			uint32_t* vertexIdentifier = nullptr);
		physicalControl::Demand transportControlDemand(std::shared_ptr<const Sector> sector,
			physicalControl::OwnerType type, physicalControl::Geometry geometry,
			uint32_t x, uint32_t y, uint32_t width) const;
		physicalControl::Demand insetControlDemand(std::shared_ptr<const Sector> sector,
			physicalControl::OwnerType type, physicalControl::Geometry geometry,
			uint32_t y, int side) const;
		void validatePhysicalControlAdditions(std::vector<physicalControl::Demand> const& demands,
			uint32_t blockedX = ~0u) const;
		physicalControl::Demand doorControlDemand(std::shared_ptr<const Sector> sector,
			uint32_t x, uint32_t y, uint32_t width, uint32_t role = 0) const;
		physicalControl::Demand validPhysicalControlDemand(physicalControl::Demand demand,
			uint32_t y, uint32_t blockedX = ~0u, uint32_t openedX = ~0u,
			uint32_t unsupportedX = ~0u) const;
		struct PhysicalControlPlan
		{
			std::vector<uint32_t> row, assignment;
			std::vector<physicalControl::Demand> demands;
		};
		void validatePanelControlPlan(PhysicalControlPlan const& plan) const;
		PhysicalControlPlan planPhysicalControls(uint32_t layer, uint32_t sector, uint32_t y,
			physicalControl::Demand const* extra = nullptr, uint32_t blockedX = ~0u,
			uint32_t openedX = ~0u, uint32_t unsupportedX = ~0u, bool validatePanels = true) const;
		void validatePhysicalControlBoundary(uint32_t layer, uint32_t y, uint32_t blockedX) const;
		void validatePhysicalControlSectorCreation(uint32_t layer, uint32_t x, uint32_t y,
			uint32_t width, uint32_t height, bool walls) const;
		void reflowAllPhysicalControls(bool finalPolicy = false);
		void reflowPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y);
		void applyPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y,
			std::vector<uint32_t> const& row, std::vector<uint32_t> const& assignment);
		void bindPhysicalControl(CreateObjectResult& control, InteractionPointId point);
		InteractionPointId createPhysicalControlInteractionPoint(std::string const& name,
			CreateObjectResult& control, float standingY, float reach,
			float durationSeconds, std::vector<InteractionBinding> bindings);

		CreateObjectResult createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createMarker(uint32_t layerIndex, uint32_t x, uint32_t y,
			float xOffset, MarkerId id, std::string name, MarkerProperties properties,
			uint32_t* vertexIdentifier = nullptr);
		bool canAddSectorMarkerImpl(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
			std::string* diagnostic, bool allowCoincident) const;
		CreateObjectResult addSectorMarkerRestored(uint32_t sectorIndex,
			uint32_t levelIndex, float xOffset, MarkerId id, std::string name,
			MarkerProperties properties = 0, uint32_t* vertexIdentifier = nullptr, bool allowCoincident = false);

		CreateObjectResult createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y, CreateForceBridgeOptions const& options);

		CreateObjectResult createLadderSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createPlatformLiftSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLiftOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateDoorResult _addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options,
			bool controlsAreExternallyBound = false);

		// The complete Door option and placement preflight. Every rejecting check a
		// Door add can make lives here, so the public addSectorDoor() can run it
		// before beginStructuralEdit() and a refused call stays a true no-op
		// (ticket #196). _addSectorDoor() runs it again so the lift and shuttle
		// builders stay self-validating.
		void validateSectorDoorPlacement(std::string const& caller, uint32_t layerIndex, uint32_t y,
			uint32_t x, CreateDoorOptions const& options, bool controlsAreExternallyBound) const;

		CreateObjectResult _createSectorButton(std::string const& name, std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createDoorButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index = nullptr, uint32_t approachSide = 0);

		CreateObjectResult _createBulkheadDoorButton(std::shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index = nullptr);

		CreateObjectResult _createForceBridgeButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index = nullptr);

		uint32_t addLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor);

		void buildGraph();

		std::unique_ptr<Agent> makeAgentForPlacement(std::string const& name,
			std::set<AccessPermissionId> const& grants, std::set<PermissionSetId> const& sets) const;

		// Forwards to SimulationCoordinator, which owns Agent placement (ADR 0004).
		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId, uint32_t levelOffset, float xOffset);

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId);

		// Snapshot world - every per-entity projection and the whole-world
		// SimulationSnapshot - lives in SimulationCoordinator (ADR 0004 stage 5).
		// World keeps the whole-world forward in the public section below, plus
		// one private forward: creating and removing a traversal resource is entity
		// ownership which stays with World (ADR 0001), and the lifecycle events
		// those paths publish carry the resource snapshot the coordinator builds.
		TraversalResourceSnapshot makeTraversalResourceSnapshot(TraversalResourceId id,
			TraversalResource const& resource) const;

		// Interaction and device-operation orchestration lives in
		// SimulationCoordinator (ADR 0004); each entry point below - public or
		// private - forwards to it.
		DeviceOperationId findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester);

		void advanceDeviceOperations();

		void allocateInteractions();

		void moveInteractions(float frameTime);

		void tryPressUpcomingDoorButton(Agent& agent, Vector2 const& movementStart,
			Vector2 const& movementEnd);

		void pressPhysicalControl(InteractionPointId point);

		void updateInteractionResults();

		bool interactionRequestEligible(InteractionPointId point, AgentId actor, bool requireReach = false) const;
		InteractionRequestId requestInteractionForTraversal(InteractionPointId point, AgentId actor);

		InteractionRequestId requestInteractionWhilePassing(InteractionPointId point, AgentId actor);

		// Remote-door and extensible traversal preparation, and the queue and
		// admission core - traversal-request creation, queue tickets, queue
		// positions and their refresh, the door queue grant and release, the
		// ladder admission family with its entry-spacing rule, traversal progress
		// and timeouts, permit expiry, and the grant / allocate / deny / commit /
		// cancel / release transaction lifecycle - all live in
		// SimulationCoordinator (ADR 0004). World keeps the entry points which
		// still have a caller outside World and forwards them; the helpers
		// reached only from inside the coordinator keep no forward. Configuring a
		// traversal resource's queue lanes stays with World: that is entity
		// ownership (ADR 0001), not coordination.
		void refreshQueuePositions(TraversalResource& resource);

		bool stopForAvailableQueuePosition(Agent& agent,
			std::shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
			float movementDistance);

		// Ticket #98: door-aware arrival check for Agent's vertex movement. The
		// forward keeps the crossing-band request-creation gate in
		// SimulationCoordinator.
		bool isAtDoorCrossingArrival(Agent const& agent,
			std::shared_ptr<const Edge> const& edge, Vector2 const& threshold);

		void configureLadderQueueLanes(TraversalResourceId resource,
			std::array<SectorId, 2> const& sectors,
			std::array<Vector2, 2> const& endpoints);

		void configureForceBridgeQueueLanes(TraversalResourceId resource,
			SectorId sector, std::array<Vector2, 2> const& endpoints);

		void updateTraversalProgressAndTimeouts();

		// Door open lease acquisition and release live in SimulationCoordinator
		// (ADR 0004); these forward.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResource& resource,
			DoorOpenLeaseKind kind, TraversalRequestId request = {});

		bool releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease);

		// The lift allocation dispatcher and its branches - the platform lift
		// dispatch, the journey resource and stop resolution, the enabled check
		// and the boarding / riding / disembarking classification - live in
		// SimulationCoordinator (ADR 0004), reached only from the coordinator's
		// own traversal-request allocation, so no forward is left for them.

		uint32_t findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const;
		void captureTransportRouteQueues(TraversalResource const& resource) const;
		void captureDoorRouteObservation(TraversalResource const& resource) const;

		uint32_t findAgentLiftDestination(Agent const& agent, TraversalResource const& resource) const;

		bool liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const;

		void addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		void removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		uint32_t chooseNextLiftStop(TraversalResource& resource) const;

		bool isLiftBoardingDirectionCompatible(TraversalResource& resource,
			uint32_t originStop, uint32_t destinationStop);

		void releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource);

		// Shuttle door assignment - the passenger-carriage lookup, the boarding and
		// disembark door selection, and the retargeting they share - lives in
		// SimulationCoordinator (ADR 0004). Both selections are reached only from
		// inside the coordinator now, so no forward is left for them.

		void requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason);

		void assignLiftSafeExitPaths(TraversalResource& resource);

		bool replaceOnboardLiftDestination(Agent& agent, std::shared_ptr<Path> const& path,
			uint32_t& sourceNode);

		// The traversal transaction lifecycle below - request creation, allocation,
		// denial, commit, cancel and release - also lives in SimulationCoordinator
		// (ADR 0004); these forward. The grant is reached only from inside the
		// coordinator, so no forward is left for it.
		TraversalRequestId createTraversalRequest(Agent const& agent, std::shared_ptr<const Edge> const& edge,
			std::shared_ptr<const Vertex> const& source, std::shared_ptr<const Vertex> const& destination);

		void allocateTraversalRequest(TraversalRequestId requestId,
			std::shared_ptr<const Edge> const& edge, std::shared_ptr<const Vertex> const& destination);

		void denyTraversalRequest(TraversalRequestId requestId,
			TraversalFailureReason reason = TraversalFailureReason::None);

		bool commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
			std::shared_ptr<const Vertex> const& destination);

		void cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
			bool requestSafeTransportExit = true);

		void releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId);

		// A handle an Agent owns inside a capacity resource outlives nothing: once the
		// Agent is gone the manifest slot can never be disembarked and the Lift, Shuttle
		// or Ladder is permanently one place short (ticket #57). Deleting an Agent
		// therefore surrenders every such claim before the entity is destroyed. The
		// release lives in SimulationCoordinator (ADR 0004); these forward.
		bool holdsTraversalOwnership(AgentId id) const;
		void releaseAgentFromResource(TraversalResource& resource, AgentId id);
		void releaseTraversalOwnership(AgentId id);

	public:

		World(std::string const& name, uint32_t cellsWide, uint32_t levelsHigh,
			AgentBehaviourRuntimeLimits behaviourRuntimeLimits = {});

		// A World allocates one CellDefinition per (x, level) on each Layer, so
		// dimensions are accepted only when the total across `layerCount` Layers
		// stays within CORE_MAX_WORLD_CELLS. The product is evaluated in 64 bits
		// so an overflowing pair is refused rather than wrapping to a short
		// allocation (#184). Returns false and fills `diagnostic` when the size
		// is unsupported; the editor and loader use the same rule as the
		// constructor.
		static bool dimensionsAreSupported(uint32_t cellsWide, uint32_t levelsHigh,
			uint32_t layerCount, std::string* diagnostic = nullptr);

		virtual ~World();

		std::string const& getName() const;
		std::string const& getClipboardIdentity() const { return mClipboardIdentity; }
		std::weak_ptr<void const> getLifetimeToken() const { return mLifetimeToken; }

		uint64_t getRandomSeed() const { return mRandomSeed; }
		// Seed edits replace all live streams and are therefore paused-only.
		bool setRandomSeed(uint64_t seed, std::string* diagnostic = nullptr);

		// A World references zero or one adjacent Agent tag registry by
		// basename and expected UUID. Attaching is an authored World change.
		// Resolving during open reconciles repairable modifier evolution and dirties
		// the World only when persisted samples need repair.
		bool hasAgentTagRegistryReference() const;
		bool hasAttachedAgentTagRegistry() const;
		std::string const& getAgentTagRegistryFilename() const;
		std::string const& getExpectedAgentTagRegistryUuid() const;
		std::shared_ptr<AgentTagRegistry> const& getAgentTagRegistry() const;
		uint64_t getAgentTagAssignmentCount() const;
		uint32_t getAgentTagAssignedAgentCount() const;
		uint64_t getAgentTagSampleCount() const;

		// Direct namespace changes are safe only while no Agent carries tag state.
		// The explicit clearing variants are the destructive transaction used after
		// editor confirmation; registry documents themselves are never changed.
		void attachAgentTagRegistry(std::string filename,
			std::shared_ptr<AgentTagRegistry> registry);
		void attachAgentTagRegistryAndClearAssignments(std::string filename,
			std::shared_ptr<AgentTagRegistry> registry);
		void detachAgentTagRegistry();
		void detachAgentTagRegistryAndClearAssignments();
		void resolveAgentTagRegistry(std::shared_ptr<AgentTagRegistry> registry);

		// Save As may move an intact World into an equivalent, independent tag
		// namespace. Unlike an ordinary switch, this preserves assignments and
		// samples, and refuses any definition or allocator difference.
		void replaceAgentTagRegistryWithIndependentCopy(std::string filename,
			std::shared_ptr<AgentTagRegistry> registry);

		// A World references zero or one adjacent Agent behaviour registry
		// package by directory basename and expected UUID. Attaching and detaching
		// are paused-only; a detach or incompatible switch is refused while an
		// Agent assignment depends on the namespace.
		bool hasAgentBehaviourRegistryReference() const;
		bool hasAttachedAgentBehaviourRegistry() const;
		std::string const& getAgentBehaviourRegistryPackageName() const;
		std::string const& getExpectedAgentBehaviourRegistryUuid() const;
		std::shared_ptr<AgentBehaviourRegistry> const& getAgentBehaviourRegistry() const;
		bool agentBehaviourConfigurationsAreValid() const
		{
			return mAgentBehaviourDependencyDiagnostic.empty();
		}
		std::string const& getAgentBehaviourDependencyDiagnostic() const
		{
			return mAgentBehaviourDependencyDiagnostic;
		}
		void attachAgentBehaviourRegistry(std::string packageName,
			std::shared_ptr<AgentBehaviourRegistry> registry);
		// Explicit destructive variants clear every authored assignment and its
		// configuration in the same paused-only registry-reference transaction.
		void attachAgentBehaviourRegistryAndClearAssignments(std::string packageName,
			std::shared_ptr<AgentBehaviourRegistry> registry);
		void detachAgentBehaviourRegistry();
		void detachAgentBehaviourRegistryAndClearAssignments();
		void resolveAgentBehaviourRegistry(
			std::shared_ptr<AgentBehaviourRegistry> registry);
		// A package load failure is dependency state, not malformed World data.
		// Recording it never dirties authored data; the persisted reference and
		// unresolved assignments remain available for repair.
		void markAgentBehaviourRegistryUnavailable(std::string diagnostic);

		// Save As may move an intact World into an equivalent, independent
		// behaviour package. Assignments remain unchanged while the package UUID
		// and basename are replaced atomically.
		void replaceAgentBehaviourRegistryWithIndependentCopy(
			std::string packageName,
			std::shared_ptr<AgentBehaviourRegistry> registry);

		// One paused-only, schema-validated authored Agent behaviour assignment.
		// Optional defaults are materialized before mutation. A failed validation
		// changes neither Agent nor World.
		bool validateAgentBehaviourAssignment(AgentBehaviourId behaviour,
			uint64_t revision, AgentBehaviourConfiguration const& configuration,
			AgentBehaviourConfiguration* normalized = nullptr,
			std::string* diagnostic = nullptr) const;
		bool setAgentBehaviourAssignment(AgentId agent, AgentBehaviourId behaviour,
			uint64_t revision, AgentBehaviourConfiguration const& configuration,
			std::string* diagnostic = nullptr);
		bool clearAgentBehaviourAssignment(AgentId agent,
			std::string* diagnostic = nullptr);
		std::optional<AgentBehaviourAssignment> const& getAgentBehaviourAssignment(
			AgentId agent) const;
		uint32_t getAgentBehaviourAssignmentCount() const;
		// True while an enabled instance, or its safe movement teardown, owns the
		// Agent's movement intent. Editor/manual path controls use this one gate.
		bool agentBehaviourOwnsMovement(AgentId agent) const;
		// Runtime failures are value diagnostics and are not authored or persisted.
		// Inspecting or consuming them does not consume the public simulation event
		// queue. The non-consuming snapshot supports persistent editor presentation;
		// consume is the explicit acknowledgement/clear operation.
		std::vector<AgentBehaviourRuntimeDiagnostic>
			getAgentBehaviourRuntimeDiagnostics() const;
		std::vector<AgentBehaviourRuntimeDiagnostic>
			consumeAgentBehaviourRuntimeDiagnostics();
		AgentBehaviourRuntimeLimits getAgentBehaviourRuntimeLimits() const;

		uint32_t getCellsWide() const;

		uint32_t getLevelsHigh() const;

		uint32_t getLayerCount() const;

		std::string const& getLayerName(uint32_t layerIndex) const;
		void setLayerName(uint32_t layerIndex, std::string name);

		// Appends a new back-most Layer with the default name and returns its index.
		// Throws if the World already has CORE_MAX_LAYERS layers.
		uint32_t addLayer();

		// Plans the destructive deletion of a Layer.  The plan is side-effect free
		// and validates that the compacted World can be rebuilt before it is
		// offered for confirmation.  A World must keep at least two Layers.
		struct LevelDeletePlan
		{
			uint32_t levelIndex{};
			bool valid{ false };
			std::string diagnostic;
			std::vector<std::string> consequences;
		};
		std::string const& getLevelName(uint32_t level) const;
		void setLevelName(uint32_t level, std::string name);
		void addLevel();
		LevelDeletePlan planDeleteLevel(uint32_t level) const;
		bool applyDeleteLevel(LevelDeletePlan const& plan);
		LayerDeletePlan planDeleteLayer(uint32_t layerIndex) const;

		// Applies a confirmed Layer deletion by rewriting and replaying the authored
		// construction records.  Leaves the simulation paused.  Throws if the Layer
		// can no longer be deleted.
		bool applyDeleteLayer(LayerDeletePlan const& plan);

		uint32_t getNumSectors() const;

		std::shared_ptr<const Layer> getLayer(uint32_t layerIndex) const;

		std::shared_ptr<const Sector> getSector(uint32_t index) const;

		std::vector<std::shared_ptr<const Sector>> getSectorsInBounds(uint32_t layerIndex, float x, float y, float width, float height) const;

		std::vector<std::shared_ptr<const Sector>> getSectors(uint32_t layerIndex) const;

		std::shared_ptr<const Graph> getGraph() const;

		Log const& getBuildLog() const;

		std::vector<std::string> const& getLoadWarnings() const
		{ return mLoadWarnings; }

		// Sector types
		// A Corridor is a Location, so it may sit on any Layer.  The Layer-less form
		// keeps the front-most Layer as its default.
		uint32_t addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh = 1);
		uint32_t addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh = 1);

		uint32_t addRoom(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT);

		// A Background is a non-occupiable Sector: it takes space on its own Layer and
		// nothing else. It may sit on any Layer, front-most and back-most included; a
		// "back layers only" rule would re-introduce the Fore/Back special-casing that
		// ADR 0002 removed. Adjacent Backgrounds are allowed and never merge: each
		// keeps its own colour.
		uint32_t addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh, BackgroundColour const& colour = {});

		bool canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh, std::string* diagnostic = nullptr) const;

		// A Facade is an occupiable Location with its perimeter walls all open by
		// construction: it hosts objects and agents exactly as a Room does and is
		// rendered as a solid opaque colour (ADR 0003). Placement follows the
		// Room rule - the same Layer, bounds, free-space, and height validation.
		// The named form is the persistence form: the record carries the Facade's
		// name alongside its footprint and packed colour. The unnamed form keeps
		// the generic "Facade" name.
		uint32_t addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT,
			BackgroundColour const& colour = Facade::defaultColour());

		uint32_t addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh, float topLevelHeight = CORE_ROOM_MAX_HEIGHT,
			BackgroundColour const& colour = Facade::defaultColour());

		bool canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh, float topLevelHeight, std::string* diagnostic = nullptr) const;
	
		// A Transit is authored on layerIndex, the Layer it occupies, and lands on the
		// Layer directly in front of it.  The front-most Layer can carry no Transit.
		CreateLadderResult addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLadderOptions const& options);

		bool canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
			std::string* diagnostic = nullptr) const;

		bool getLadderOptions(uint32_t sectorIndex, CreateLadderOptions& options) const;

		uint32_t addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh, int mountSide);

		CreateStairwellResult addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x,
			CreateStairwellOptions const& options);

		bool canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t levelsHigh,
			std::string* diagnostic = nullptr) const;

		bool getStairwellOptions(uint32_t sectorIndex, CreateStairwellOptions& options) const;

		uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide,
			float speed = 0.0f);
		uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, CreateStaircaseOptions const& options);
		bool canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide,
			std::string* diagnostic = nullptr) const;
		bool getStaircaseOptions(uint32_t sectorIndex, CreateStaircaseOptions& options) const;
		bool setEscalatorBroken(uint32_t sectorIndex, bool broken);
		bool setEscalatorInitiallyBroken(uint32_t sectorIndex, bool broken);

		CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLiftOptions const& options);

		// One landing row of an enclosed Lift shaft: the row of cells on the Layer
		// directly in front of the shaft's own Layer that the shaft overlaps at one
		// level offset.
		struct LiftLandingRow
		{
			uint32_t offset{ 0 };
			// The Location the shaft overlaps on the landing Layer; null over a gap.
			std::shared_ptr<const Location> location;
			// A single Location fills the shaft width and every cell is walkable.
			bool fullyOverlapping{ false };
			// An Object or Marker blocks the row.
			bool obstructed{ false };
			// The shaft leaves Location width beside it for the stop's call control.
			bool callButtonSpace{ true };

			// A row the shaft can serve as a stop.
			bool usableForStop() const
			{
				return fullyOverlapping && !obstructed && callButtonSpace;
			}
		};

		// The landing rows of a cellsWide-by-levelsHigh shaft at (y, x) on layerIndex.
		// Rows are always read from the Layer directly in front of layerIndex; the
		// front-most Layer has nothing in front of it and yields no rows.
		std::vector<LiftLandingRow> getLiftLandingRows(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh) const;

		// Derives stops from every fully overlapping landing-layer corridor row.
		CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh);

		// Airlocks use their own Layer, unlike other Transit landings. Their
		// thresholds are operated only through Airlock-owned coordination.
		bool canAddAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width,
			float cycleSeconds = 3.0f, std::string* diagnostic = nullptr) const;
		uint32_t addAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width,
			float cycleSeconds = 3.0f);
		AirlockEditPlan planResizeAirlock(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t width) const;
		AirlockEditPlan planRemoveAirlock(uint32_t sectorIndex) const;
		uint32_t applyAirlockEdit(AirlockEditPlan const& plan);
		bool setAirlockCycleSeconds(uint32_t sectorIndex, float seconds);
		bool canAgentEnterAirlock(TraversalResourceId resource, SectorId approach,
			AgentId agent, bool locallyObserved) const;
		bool isAirlockOwnedObject(std::shared_ptr<const SectorObject> const& object) const;
		bool isChamberOwnedObject(std::shared_ptr<const SectorObject> const& object) const;
		using ChamberEditPlan = AirlockEditPlan;
		ChamberEditPlan planResizeChamber(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t width, bool leftToRight) const;
		ChamberEditPlan planRemoveChamber(uint32_t sectorIndex) const;
		ChamberEditPlan planSetChamberSubtype(uint32_t sectorIndex, ChamberSubtype subtype) const;
		uint32_t applyChamberEdit(ChamberEditPlan const& plan);
		bool setChamberConfiguration(uint32_t sectorIndex, float sensorDistance,
			float preDelaySeconds, float scanSeconds, float postPauseSeconds);
		bool canAddChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width,
			std::string* diagnostic = nullptr, ChamberSubtype subtype = ChamberSubtype::SecurityScanner) const;
		uint32_t addChamber(uint32_t layer, uint32_t y, uint32_t x, uint32_t width,
			bool leftToRight = true, ChamberSubtype subtype = ChamberSubtype::SecurityScanner);

		CreateShuttleResult addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options);

		// Sector object types
		// Doors may connect any two Room, Corridor, or Facade Locations. The legacy
		// method name remains part of the public API. A Door is authored on the front
		// Layer of the pair it crosses.
		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			std::string* diagnostic = nullptr) const;

		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options,
			std::string* diagnostic = nullptr) const;

		// Resolves a cell over a lift to its complete landing-door footprint.  The
		// Layer searched is the Transit's own Layer, not the landing Layer.
		bool getLiftLandingGeometry(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t& landingX, uint32_t& landingWidth) const;

		bool isLiftOwnedDoor(std::shared_ptr<const SectorObject> const& object,
			uint32_t* liftSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		bool isLiftOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* liftSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		// Every output is independently optional: a caller may ask for any subset
		// and the query answers those fields without needing another supplied.
		// The optional doorIndex is the position within the carriage's selected
		// doorMask cells, matching the per-Door override grid of
		// setShuttleDoorOpenStyle.
		bool isShuttleOwnedDoor(std::shared_ptr<const SectorObject> const& object,
			uint32_t* shuttleSectorIndex = nullptr, uint32_t* stopIndex = nullptr,
			uint32_t* carriageIndex = nullptr, uint32_t* doorIndex = nullptr) const;

		bool isShuttleOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* shuttleSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		std::vector<uint32_t> getValidShuttleStopOffsets(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t numCars, uint32_t carWidth,
			bool allowPartialLandings, uint32_t doorMask) const;

		bool getShuttleOptions(Shuttle const* shuttle, CreateShuttleOptions& options) const;

		// Stop alignments a Door dropped at doorX could open onto.  The Layer searched
		// is the Shuttle Transit's own Layer, not the Layer the Door is authored on;
		// a caller placing a Door on Layer L passes layerBehind(L), matching
		// getLiftLandingGeometry.  A Shuttle on any other Layer is never returned.
		std::vector<ShuttleStopCandidate> getShuttleStopCandidatesForDoor(
			uint32_t shuttleLayer, uint32_t y, uint32_t doorX) const;

		bool getSectorDoorOptions(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
			CreateDoorOptions& options) const;

		// Re-authors an ordinary Door's physical height. Tall is available only to
		// Doors authored in Rooms; transport, Corridor and Facade Doors stay regular.
		bool setSectorDoorHeight(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
			Door::Height height, std::string* diagnostic = nullptr);

		// Re-authors an ordinary Door's opening style.  The authored construction
		// record is the persistence boundary, so the record and the live Door move
		// together: save/load, clipboard readback, moves, and undo/redo all carry
		// the new style. Opening style affects rendering and, for a tall OpenUp Door,
		// scales timing to preserve vertical speed; state and traversal are untouched.
		bool setSectorDoorOpenStyle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
			Door::OpenStyle style, std::string* diagnostic = nullptr);

		// Re-authors one Lift stop's landing Door opening style while the Lift's
		// topology stays fixed.  The override lives in the Lift's own record, not
		// in a Door record, so editing one stop affects no sibling Door; save/load
		// and snapshot-based undo/redo carry the choice.  The override is keyed by
		// the stop's absolute landing level, so moving or resizing the Lift while
		// retaining its stops keeps every style attached to its own stop.
		// Stops without an override keep the generated OpenApart default.
		// Transport-managed geometry, controls, timing, and traversal are untouched.
		bool setLiftStopDoorOpenStyle(uint32_t liftSectorIndex, uint32_t stopIndex,
			Door::OpenStyle style, std::string* diagnostic = nullptr);

		// Re-authors one Shuttle landing Door's opening style while the Shuttle's
		// topology stays fixed.  The override lives in the Shuttle's own record,
		// not in a Door record, so editing one Door affects no sibling Door at the
		// same or another stop; save/load and snapshot-based undo/redo carry the
		// choice.  stopIndex, carriageIndex, and doorIndex address the Door within
		// the fixed stop/carriage/door grid (doorIndex counts the carriage's
		// selected doorMask cells).  Doors without an override keep the generated
		// OpenUp default.  Transport-managed geometry, controls, timing, and
		// traversal are untouched.
		bool setShuttleDoorOpenStyle(uint32_t shuttleSectorIndex, uint32_t stopIndex,
			uint32_t carriageIndex, uint32_t doorIndex,
			Door::OpenStyle style, std::string* diagnostic = nullptr);

		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x);

		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options);

		// Gives the selected Door physical open controls on both sides of the
		// threshold: any side already carrying a Button keeps it, the missing
		// side(s) gain one, and the edit refuses - before changing anything -
		// when a side that lacks a Button has no space for one. Both Buttons
		// bind idempotent open-only commands to the Door's single traversal
		// resource, so pressing either one opens the Door and neither can close
		// it. The Door's pre-Button activation mode is recorded the first time
		// Buttons are added so removeSectorDoorButton can restore it.
		void addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex);

		// True when the selected Door is an ordinary sector Door whose record
		// still has a side without a Button. The Door panel uses this to disable
		// its Add Door Button action.
		bool canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;

		// Removes every Door Button from the selected Door and returns the rebuilt
		// Door object (the caller's selection handles go stale across the rebuild).
		// The activation mode recorded when Buttons were added in the editor is
		// restored; existing authored/YAML Buttons fall back to manual activation.
		// Null on failure.
		std::shared_ptr<const DoorSectorObject> removeSectorDoorButton(uint32_t sectorIndex,
			uint32_t objectIndex,
			std::optional<std::vector<AccessPermissionId>> resultingRequirement = std::nullopt);

		// True when removeSectorDoorButton will accept the selected ordinary Door:
		// its construction record currently carries at least one Button.
		bool canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;

		bool removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex);

		// A Window needs the Layer directly behind the Layer it is authored on, so the
		// back-most Layer can never take a new one.
		bool canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide = 1, uint32_t levelsHigh = 1,
			std::string* diagnostic = nullptr) const;

		uint32_t addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh);

		CreateWindowResult addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options);

		bool getSectorWindowOptions(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions& options) const;

		bool canAddAccessPanel(uint32_t sectorIndex, uint32_t levelOffset, uint32_t cellX,
			AccessPanelGeometry geometry = {}, std::string* diagnostic = nullptr) const;
		CreateObjectResult addAccessPanel(uint32_t sectorIndex, uint32_t levelOffset,
			uint32_t cellX, AccessPanelGeometry geometry = {}, std::optional<float> speed = {});
		bool configureAccessPanel(uint32_t sectorIndex, uint32_t objectIndex,
			AccessPanelGeometry geometry, std::string* diagnostic = nullptr, std::optional<float> speed = {});
		bool removeAccessPanel(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
			uint32_t width = 1, uint32_t height = 1, std::string* diagnostic = nullptr) const;
		CreateWindowResult addBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
			Window::State initialState = Window::State::Closed);
		bool setBoothWindowInitialState(uint32_t layer, uint32_t y, uint32_t x, Window::State state);

		bool removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side,
			CreateBulkheadDoorOptions const& options,
			std::string* diagnostic = nullptr) const;

		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side);

		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side, CreateBulkheadDoorOptions const& options);

		bool getSectorBulkheadDoorOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateBulkheadDoorOptions& options) const;

		std::shared_ptr<const SectorObject> applySectorBulkheadDoorOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateBulkheadDoorOptions const& options);

		bool removeSectorBulkheadDoor(uint32_t sectorIndex, uint32_t objectIndex);

		bool isBulkheadDoorOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* doorSectorIndex = nullptr, uint32_t* doorObjectIndex = nullptr) const;

		CreateObjectResult addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset, CreateForceBridgeOptions const& options);

		bool canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset, CreateForceBridgeOptions const& options,
			std::string* diagnostic = nullptr) const;

		bool calculateSectorForceBridgeWidthToRight(uint32_t sectorIndex,
			uint32_t levelIndex, uint32_t xOffset, uint32_t& width,
			std::string* diagnostic = nullptr) const;

		bool getSectorForceBridgeOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateForceBridgeOptions& options) const;

		std::shared_ptr<const SectorObject> applySectorForceBridgeOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateForceBridgeOptions const& options);

		bool removeSectorForceBridge(uint32_t sectorIndex, uint32_t objectIndex);

		bool isForceBridgeOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* forceBridgeSectorIndex = nullptr,
			uint32_t* forceBridgeObjectIndex = nullptr) const;

		CreateLadderResult addSectorLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateLadderOptions const& options);

		// Room Ladders are point-placed objects. Their height is always derived
		// from the nearest Walkway above their Ground/Walkway base.
		bool canAddRoomLadder(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset,
			uint32_t* levelsHigh = nullptr, std::string* diagnostic = nullptr) const;

		CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset);

		CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset, CreateLadderOptions options);

		bool getRoomLadderOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLadderOptions& options) const;

		std::shared_ptr<const SectorObject> applyRoomLadderOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateLadderOptions const& options);

		bool removeRoomLadder(uint32_t sectorIndex, uint32_t objectIndex);

		CreatePlatformLiftResult addSectorPlatformLift(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset, CreateLiftOptions const& options);

		std::vector<PlatformLiftStopCandidate> getPlatformLiftStopCandidates(
			uint32_t sectorIndex, uint32_t xOffset) const;

		bool canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset,
			CreateLiftOptions const& options, std::string* diagnostic = nullptr) const;

		bool getPlatformLiftOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLiftOptions& options) const;

		PlatformLiftEditPlan planPlatformLiftEdit(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLiftOptions const& options) const;

		PlatformLiftEditPlan planRemovePlatformLift(uint32_t sectorIndex,
			uint32_t objectIndex) const;

		std::shared_ptr<const SectorObject> applyPlatformLiftEdit(
			PlatformLiftEditPlan const& plan);

		WalkwayEditPlan planRemoveSectorWalkway(uint32_t sectorIndex,
			uint32_t objectIndex) const;

		bool applyWalkwayEdit(WalkwayEditPlan const& plan);

		bool canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex, uint32_t xOffset,
			std::string* diagnostic = nullptr) const;

		CreateObjectResult addSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset);

		bool removeSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
			std::string* diagnostic = nullptr) const;

		CreateObjectResult addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
			uint32_t* vertexIdentifier = nullptr);

		// Explicit naming follows the same validation as rename. The legacy
		// overload above generates a deterministic unique name for editor placement.
		CreateObjectResult addSectorMarker(uint32_t sectorIndex, uint32_t levelIndex,
			float xOffset, std::string const& name, uint32_t* vertexIdentifier = nullptr);

		// Runtime-only movement seam: no Path/Vertex access is needed by callers.
		// Accepted intent reports DestinationReached, RouteLost or MovementCancelled
		// through simulation events. Names resolve once, never again after edits.
		// Immutable built-ins plus assigned registry Actions, resolved at a Marker.
		// Target deletion cancels; route failure does not select a fallback Action.
		// Same pending goal and Action is a NoOp; another goal supersedes it,
		// publishing a distinct cancellation without interrupting committed traversal.
		MovementCommandResult moveAgentToMarker(AgentId agent, MarkerId marker,
			std::string_view action = IdleAction);
		MovementCommandResult moveAgentToNamedMarker(AgentId agent, std::string const& markerName,
			std::string_view action = IdleAction);
		std::vector<std::string> availableAgentActions(MarkerId marker) const;
		std::shared_ptr<const ActionRegistry> const& actionRegistry() const { return mActionRegistry; }
		std::string const& actionRegistryFilename() const { return mActionRegistryFilename; }
		bool selectActionRegistry(std::filesystem::path const& path, std::string* diagnostic = nullptr);
		bool clearActionRegistry(std::string* diagnostic = nullptr);
		bool setMarkerActions(MarkerId marker, std::vector<std::string> actions, std::string* diagnostic = nullptr);
		std::vector<std::string> markerActions(MarkerId marker) const;
		std::string agentActionDisplayName(std::string_view identity) const;
		bool authorAgentMarkerRequest(AgentId agent, MarkerId marker, std::string_view action,
			std::string* diagnostic = nullptr);
		// Never interrupts an in-flight threshold crossing or ejects an occupant.
		// Transport passengers finish their scheduled journey before cancellation;
		// wait for MovementCancelled before commanding a replacement destination.
		// Idle/repeated cancellation is an accepted NoOp, without another event.
		MovementCommandResult cancelAgentMovement(AgentId agent);
		std::vector<MarkerId> getMarkerIds() const;
		std::shared_ptr<const Marker> lookupMarker(MarkerId id) const;
		bool canRenameMarker(MarkerId id, std::string const& name,
			std::string* diagnostic = nullptr) const;
		bool renameMarker(MarkerId id, std::string const& name,
			std::string* diagnostic = nullptr);
		// Properties which affect routing can only be edited while a built World is paused.
		bool setMarkerProperties(MarkerId id, MarkerProperties properties,
			std::string* diagnostic = nullptr);

		bool canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
			std::string* diagnostic = nullptr) const;
		bool removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
			std::string* diagnostic = nullptr);

		// Plans are side-effect free. Applying a plan reconstructs the authored
		// structure atomically and leaves the simulation paused.
		LocationEditPlan planResizeLocation(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t levelsHigh) const;

		LocationEditPlan planRemoveLocation(uint32_t sectorIndex) const;

		// A Facade is occupiable, so resizing and deleting it use the same cascade
		// as a Room: plans name Agents and hosted objects which no longer fit, and
		// applying a plan rebuilds the rest of the World around the edit. Separate
		// entry points keep a Room edit from accidentally targeting a Facade.
		LocationEditPlan planResizeFacade(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t levelsHigh) const;

		LocationEditPlan planRemoveFacade(uint32_t sectorIndex) const;

		uint32_t applyLocationEdit(LocationEditPlan const& plan);

		// A Background exists only to be looked into, so taking it away takes the
		// Windows looking into it with it.  Deleting a Background, moving it, or
		// resizing it all uncover whatever back cells the new footprint no longer
		// covers, and every Window which loses what it looks into is named in the
		// plan's consequences before anything is applied.  A plan which uncovers
		// nothing needs no confirmation and applies silently.
		LocationEditPlan planRemoveBackground(uint32_t sectorIndex) const;

		LocationEditPlan planResizeBackground(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t levelsHigh) const;

		uint32_t applyBackgroundEdit(LocationEditPlan const& plan);

		// Recolour a Background in place. Colour is the only thing a Background owns,
		// so the live Sector and its authored ConstructionType::Background record are
		// patched together: no rebuild, no cascade, and nothing else in the World
		// reads a Background's colour. Returns false, with a diagnostic when one is
		// asked for, if the Sector is not a Background or has no authored record.
		bool setBackgroundColour(uint32_t sectorIndex, BackgroundColour const& colour,
			std::string* diagnostic = nullptr);

		// Recolour a Facade in place, the same patch shape setBackgroundColour uses:
		// the live Sector and its authored ConstructionType::Facade record are updated
		// together, so a save writes the new colour and a reload replays it. A
		// Facade's colour feeds only its own rendering, so this needs no plan and no
		// cascade. Returns false, with a diagnostic when one is asked for, if the
		// Sector is not a Facade or has no authored record.
		bool setFacadeColour(uint32_t sectorIndex, BackgroundColour const& colour,
			std::string* diagnostic = nullptr);

		LiftEditPlan planResizeLift(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t levelsHigh) const;

		LiftEditPlan planRemoveLift(uint32_t sectorIndex) const;

		LiftEditPlan planRemoveLiftStop(uint32_t sectorIndex, uint32_t stopIndex) const;

		uint32_t applyLiftEdit(LiftEditPlan const& plan);

		ShuttleEditPlan planResizeShuttle(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t cellsWide) const;

		// Re-author a Shuttle's coupled vehicle - carriage count, carriage width,
		// and the configured door positions within a carriage - over its current
		// track.  All three layout arguments are required and must be non-zero;
		// zero is the internal keep-current sentinel of the shared track-resize
		// path and is rejected here rather than silently meaning "unchanged".  Per-Door opening styles are reconciled by their structural
		// identity: a style survives only where the same stop, the same carriage
		// index, and the same configured carriage cell still exist and that cell
		// still lands on a supported landing.  Dropped carriages and deselected
		// door positions take their overrides with them instead of letting a
		// shifted grid index leak the style onto an unrelated physical Door, and
		// every newly generated Door uses the Shuttle's OpenUp default.
		ShuttleEditPlan planEditShuttleVehicle(uint32_t sectorIndex, uint32_t numCars,
			uint32_t carWidth, uint32_t doorMask) const;

		ShuttleEditPlan planRemoveShuttle(uint32_t sectorIndex) const;

		ShuttleEditPlan planRemoveShuttleStop(uint32_t sectorIndex, uint32_t stopIndex) const;

		ShuttleEditPlan planAddShuttleStop(uint32_t sectorIndex, uint32_t stopOffset) const;

		uint32_t applyShuttleEdit(ShuttleEditPlan const& plan);

		LadderEditPlan planResizeLadder(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateLadderOptions const& options) const;

		LadderEditPlan planRemoveLadder(uint32_t sectorIndex) const;

		uint32_t applyLadderEdit(LadderEditPlan const& plan);

		StairwellEditPlan planResizeStairwell(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateStairwellOptions const& options) const;

		StairwellEditPlan planRemoveStairwell(uint32_t sectorIndex) const;

		uint32_t applyStairwellEdit(StairwellEditPlan const& plan);

		StaircaseEditPlan planResizeStaircase(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateStaircaseOptions const& options) const;
		StaircaseEditPlan planRemoveStaircase(uint32_t sectorIndex) const;
		uint32_t applyStaircaseEdit(StaircaseEditPlan const& plan);

		ObjectMovePlan planMoveSectorObject(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y) const;

		// Windows resize from any edge. The resulting plan uses the same atomic replay
		// path as movement, preserving authored options while validating the complete
		// new footprint against normal Window placement rules.
		ObjectMovePlan planResizeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		// Regular Doors resize horizontally between one and two cells. Their one-level
		// footprint has a separately authored regular/tall physical height. Lift and
		// Shuttle landing doors are managed by their transport and refuse to resize.
		ObjectMovePlan planResizeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		std::shared_ptr<const SectorObject> applyObjectMove(ObjectMovePlan const& plan);

		bool canRemoveLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side,
			std::string* diagnostic = nullptr) const;

		bool canAddLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side,
			std::string* diagnostic = nullptr) const;

		void removeLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side);

		void addLocationWall(uint32_t sectorIndex, uint32_t levelIndex, int side);

		void finishBuild();

		// Runtime structural editing protocol. Pausing deterministically cancels
		// active edge transactions while retaining route destinations for the new
		// graph. A failed rebuild is atomic at the graph boundary and cannot resume.
		// Pause and resume are the edit/simulation boundary and live here, on the
		// editing side of it; the simulation-side work they drive - the teardown,
		// the paused route intents and the boundary events - lives in
		// SimulationCoordinator (ADR 0004 stage 5).
		void pauseSimulation();

		bool rebuildTraversalTopology();

		bool resumeSimulation();

		bool isSimulationPaused() const { return mSimulationPaused; }

		// Returns false when the Agent has no paused path intent.
		bool getPausedPathIntent(Agent const& agent, TopologyPathIntent& intent) const;

		// Editor operation: clears live/reset Paths and retained destination intent.
		// Requires a paused World and refuses behaviour-owned movement.
		bool clearAgentPath(AgentId agent);

		bool isTraversalTopologyDirty() const { return mTopologyDirty; }

		bool isTraversalTopologyValid() const { return mTopologyValid; }

		uint64_t getTopologyGeneration() const { return mTopologyGeneration; }

		std::string const& getTopologyDiagnostic() const { return mTopologyDiagnostic; }

		std::shared_ptr<const Sector> getSectorAtPosition(uint32_t layerIndex, float x, float y) const;

		Agent* getAgentAtPosition(uint32_t layerIndex, float x, float y) const;

		std::shared_ptr<const Object> getObjectAtPosition(uint32_t layerIndex, float x, float y,
			std::shared_ptr<const SectorObject>* sectorObject = nullptr) const;

		// World-owned replacement APIs. Callers retain typed IDs, not ownership.
		// Agent lifecycle - creation, placement, removal, lookup, id resolution,
		// waking, and traversal-ownership release - lives in SimulationCoordinator
		// (ADR 0004); every Agent entry point below forwards to it, as does every
		// InteractionPoint, InteractionRequest and DeviceOperation entry point.
		AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset);

		AgentId createAgent(std::string const& name, uint32_t sectorId);

		// Initial authorization is validated before ownership, placement or ID allocation.
		AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t levelOffset, float xOffset,
			std::set<AccessPermissionId> const& grants, std::set<PermissionSetId> const& sets);
		AgentId createAgent(std::string const& name, uint32_t sectorId,
			std::set<AccessPermissionId> const& grants, std::set<PermissionSetId> const& sets);
		bool canPlaceAgentInLocation(uint32_t sectorId, std::set<AccessPermissionId> const& grants,
			std::set<PermissionSetId> const& sets, std::string* diagnostic = nullptr) const;
		void validateAgentLocationPlacement(Sector const& sector, Agent const& agent) const;

		EntityLookup<Agent> lookupAgent(AgentId id);

		EntityLookup<Agent const> lookupAgent(AgentId id) const;

		EntityRemovalResult removeAgent(AgentId id);

		// Agent activation (#118). An activated Agent is simulated; a deactivated
		// one keeps its authored position and route but no tick acts on it.
		// Activation is judged before it is written: the only refusals are an
		// Agent the World does not own and a running simulation, since an
		// activation change mid-run would strand whatever traversal the Agent was
		// in the middle of. Like the Agent group assignment this is authored
		// state: it persists through save/load, reset, undo, and clipboard
		// placement, and it never dirties the traversal topology.
		bool canSetAgentActive(AgentId agent, bool active,
			std::string* diagnostic = nullptr) const;

		// Returns false and changes nothing when canSetAgentActive refuses,
		// reporting the reason through `diagnostic`.
		bool setAgentActive(AgentId agent, bool active,
			std::string* diagnostic = nullptr);

		bool setAgentIndividualColour(AgentId agent, std::optional<AgentColour> value,
			std::string* diagnostic = nullptr);
		bool setAgentIndividualEscalatorWalkingChance(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualWalkSpeedModifier(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualHeightModifier(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualStairSpeedModifier(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualLadderSpeedModifier(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualInteractionAversion(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualEffortAversion(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualWaitingAversion(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualCrowdAversion(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualRiskAversion(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualRouteFamiliarity(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualRoutePersistence(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualMinimumRoutePlanningTime(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualMaximumRoutePlanningTime(AgentId agent,
			std::optional<float> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualPermissionAdherence(AgentId agent,
			std::optional<bool> value, std::string* diagnostic = nullptr);
		bool setAgentIndividualMobilityProfile(AgentId agent,
			std::optional<MobilityProfile> value, std::string* diagnostic = nullptr);

		// Agent groups - authored, World-scoped classifications (ADR 0006).
		// These are the only way in: the registry itself is never handed out, so
		// no caller can rename or drop a group around the validation below.
		// Groups enumerate in creation order; renaming never disturbs it.
		//
		// Grouping is editor-only metadata, not topology, so creating and
		// renaming a group do not require a paused simulation and never dirty
		// the traversal graph.
		uint32_t getAgentGroupCount() const;

		// Group IDs in creation order, which is ascending ID order.
		std::vector<AgentGroupId> getAgentGroupIds() const;

		EntityLookup<AgentGroup const> lookupAgentGroup(AgentGroupId id) const;

		// Throws if the ID is not one this World issued.
		std::string const& getAgentGroupName(AgentGroupId id) const;

		bool canAddAgentGroup(std::string const& name, std::string* diagnostic = nullptr) const;

		// Creates the group under the trimmed name and returns its new stable
		// ID. Throws with the canAddAgentGroup() diagnostic if the name is
		// blank, overlong, or already taken in this World.
		AgentGroupId addAgentGroup(std::string const& name);

		bool canRenameAgentGroup(AgentGroupId id, std::string const& name,
			std::string* diagnostic = nullptr) const;

		// Renames in place, keeping the group's ID and its position in the
		// creation order. Returns false and changes nothing when the group is
		// unknown or the name is not acceptable, reporting why through
		// `diagnostic`.
		bool renameAgentGroup(AgentGroupId id, std::string const& name,
			std::string* diagnostic = nullptr);

		// Assigning an Agent to an Agent group, or clearing the assignment.
		// An empty `group` AgentGroupId means no Agent group, so clearing is
		// the same operation as assigning rather than a second path through
		// the API. Both the Agent and the group have to be ones this World
		// issued; a refusal changes nothing and reports why.
		//
		// Like the group definitions themselves this is editor-only metadata:
		// it needs no paused simulation, never dirties the traversal topology,
		// and leaves every runtime snapshot and simulation event as it was.
		bool canSetAgentGroup(AgentId agent, AgentGroupId group,
			std::string* diagnostic = nullptr) const;

		// Returns false and changes nothing when the Agent is unknown or the
		// Agent group is one this World never defined, reporting the reason
		// through `diagnostic`.
		bool setAgentGroup(AgentId agent, AgentGroupId group,
			std::string* diagnostic = nullptr);

		// The Agent group assigned to an Agent, or an empty AgentGroupId when
		// it has none. Throws if the Agent is not one this World owns.
		AgentGroupId getAgentGroup(AgentId agent) const;

		// Group activation is a bulk edit of the current members' own activation
		// flags, not an inherited group property. A group is reported active while
		// at least one member is active; an empty group is therefore inactive.
		// Throws if the ID is not one this World issued.
		bool isAgentGroupActive(AgentGroupId group) const;

		// Activates or deactivates every Agent currently assigned to `group`.
		// Membership and activation remain independent afterwards, so any Agent
		// can still be changed individually. As with per-Agent activation, the
		// whole operation is refused while the simulation is running. Validation
		// completes before any member is changed.
		bool canSetAgentGroupActive(AgentGroupId group, bool active,
			std::string* diagnostic = nullptr) const;
		bool setAgentGroupActive(AgentGroupId group, bool active,
			std::string* diagnostic = nullptr);

		// Agent tag assignments reference stable IDs from this World's one
		// attached Agent tag registry. Assignment and removal are paused-only
		// authored edits. Every refusal validates before mutation, so an unknown
		// Agent, absent registry, unknown tag, duplicate assignment, or removal of
		// an unassigned tag changes nothing.
		bool canAssignAgentTag(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr) const;
		bool assignAgentTag(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr);
		bool canRemoveAgentTag(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr) const;
		bool removeAgentTag(AgentId agent, AgentTagId tag,
			std::string* diagnostic = nullptr);

		// Validates a complete authored assignment/sample state against this
		// World's attached registry without changing either document. This is
		// the preflight used by same-registry Agent paste: every referenced tag,
		// inherited-property constraint, sample source, revision and value must
		// already be valid, so paste never silently resamples or drops state.
		bool validateAgentTagAssignments(std::set<AgentTagId> const& tags,
			std::optional<AgentPropertySample> const& walkSpeedSample,
			std::optional<AgentPropertySample> const& heightSample,
			std::optional<AgentPropertySample> const& stairSpeedSample,
			std::optional<AgentPropertySample> const& ladderSpeedSample,
			std::optional<AgentPropertySample> const& interactionAversionSample,
			std::optional<AgentPropertySample> const& effortAversionSample,
			std::optional<AgentPropertySample> const& waitingAversionSample,
			std::optional<AgentPropertySample> const& crowdAversionSample,
			std::optional<AgentPropertySample> const& riskAversionSample,
			std::optional<AgentPropertySample> const& routeFamiliaritySample,
			std::optional<AgentPropertySample> const& routePersistenceSample,
			std::optional<AgentPropertySample> const& minimumRoutePlanningTimeSample,
			std::optional<AgentPropertySample> const& maximumRoutePlanningTimeSample,
			std::string* diagnostic = nullptr) const;

		// Restores one Agent's complete tag state after the preflight above.
		// Like ordinary assignment this is paused-only. Validation completes
		// before mutation, and the registry itself is never changed.
		bool restoreAgentTagAssignments(AgentId agent,
			std::set<AgentTagId> const& tags,
			std::optional<AgentPropertySample> const& walkSpeedSample,
			std::optional<AgentPropertySample> const& heightSample,
			std::optional<AgentPropertySample> const& stairSpeedSample,
			std::optional<AgentPropertySample> const& ladderSpeedSample,
			std::optional<AgentPropertySample> const& interactionAversionSample,
			std::optional<AgentPropertySample> const& effortAversionSample,
			std::optional<AgentPropertySample> const& waitingAversionSample,
			std::optional<AgentPropertySample> const& crowdAversionSample,
			std::optional<AgentPropertySample> const& riskAversionSample,
			std::optional<AgentPropertySample> const& routeFamiliaritySample,
			std::optional<AgentPropertySample> const& routePersistenceSample,
			std::optional<AgentPropertySample> const& minimumRoutePlanningTimeSample,
			std::optional<AgentPropertySample> const& maximumRoutePlanningTimeSample,
			std::string* diagnostic = nullptr);

		// The assigned tag set in stable numeric order. Throws when `agent` is
		// not owned by this World.
		std::set<AgentTagId> const& getAgentTags(AgentId agent) const;

		// How many of this World's Agents are assigned to the Agent group.
		// The count is derived from the Agents themselves on every call rather
		// than kept alongside the group: the group holds no counter of its own,
		// so nothing can drift out of step with the assignments it reports.
		// It covers the whole World - every Layer, every Sector, and every
		// movement state - and is current the moment an assignment is made.
		// Throws if the ID is not one this World issued.
		uint32_t getAgentGroupMemberCount(AgentGroupId id) const;

		// Deleting an Agent group. The World is the sole mutation boundary:
		// the group and every Agent reference to it can only go together, here.
		bool canDeleteAgentGroup(AgentGroupId id,
			std::string* diagnostic = nullptr) const;

		// Removes the Agent group and, in the same operation, returns every
		// Agent assigned to it to no Agent group. The assignments are cleared
		// before the group is removed, so the World is never left holding an
		// Agent that names a group it does not own - the state a later save
		// would refuse to load back.
		//
		// Nothing is written until the ID has been judged: an unknown or empty
		// AgentGroupId is refused atomically, leaving every Agent, every group,
		// and the reason for the refusal exactly as the caller can read it back
		// through `diagnostic`.
		bool deleteAgentGroup(AgentGroupId id, std::string* diagnostic = nullptr);

		struct AccessPermissionUsage
		{
			uint32_t directAgentGrants{ 0 };
			uint32_t permissionSetMemberships{ 0 };
			uint32_t interactionPointRequirements{ 0 };
			uint32_t manualDoorRequirements{ 0 };
			uint32_t liftDestinationRequirements{ 0 }; // Includes Platform lifts and Shuttles.
			uint32_t locationRequirements{ 0 };
		};

		uint32_t getAccessPermissionCount() const;
		std::vector<AccessPermissionId> getAccessPermissionIds() const;
		EntityLookup<AccessPermission const> lookupAccessPermission(AccessPermissionId id) const;
		std::string const& getAccessPermissionName(AccessPermissionId id) const;
		bool canAddAccessPermission(std::string const& name, std::string* diagnostic = nullptr) const;
		AccessPermissionId addAccessPermission(std::string const& name);
		bool renameAccessPermission(AccessPermissionId id, std::string const& name,
			std::string* diagnostic = nullptr);
		AccessPermissionUsage getAccessPermissionUsage(AccessPermissionId id) const;
		bool deleteAccessPermission(AccessPermissionId id, std::string* diagnostic = nullptr);

		bool setAgentAccessPermissionGrant(AgentId agent, AccessPermissionId permission,
			bool granted, std::string* diagnostic = nullptr);
		bool grantAgentAccessPermission(AgentId agent, AccessPermissionId permission,
			std::string* diagnostic = nullptr)
		{ return setAgentAccessPermissionGrant(agent, permission, true, diagnostic); }
		bool revokeAgentAccessPermission(AgentId agent, AccessPermissionId permission,
			std::string* diagnostic = nullptr)
		{ return setAgentAccessPermissionGrant(agent, permission, false, diagnostic); }
		std::vector<AccessPermissionId> getAgentDirectAccessGrants(AgentId agent) const;

		// Current authorization is simulation state. These mutations are available
		// while paused or running, never dirty authored data, and Reset simulation
		// restores the authored direct grants and Permission set assignments.
		bool setAgentRuntimeAccessPermissionGrant(AgentId agent,
			AccessPermissionId permission, bool granted);
		std::vector<AccessPermissionId> getAgentCurrentDirectAccessGrants(AgentId agent) const;
		bool setAgentRuntimePermissionSetAssignment(AgentId agent,
			PermissionSetId set, bool assigned);
		std::vector<PermissionSetId> getAgentCurrentPermissionSetAssignments(AgentId agent) const;
		std::vector<AccessPermissionId> getAgentEffectiveAccessGrants(AgentId agent) const;

		struct EffectiveAccessGrantSources
		{
			bool direct{ false };
			std::vector<PermissionSetId> permissionSets;
		};
		EffectiveAccessGrantSources getAgentAccessGrantSources(AgentId agent,
			AccessPermissionId permission) const;

		uint32_t getPermissionSetCount() const;
		std::vector<PermissionSetId> getPermissionSetIds() const;
		EntityLookup<PermissionSet const> lookupPermissionSet(PermissionSetId id) const;
		std::string const& getPermissionSetName(PermissionSetId id) const;
		PermissionSetId addPermissionSet(std::string const& name);
		bool renamePermissionSet(PermissionSetId id, std::string const& name,
			std::string* diagnostic = nullptr);
		uint32_t getPermissionSetUsageCount(PermissionSetId id) const;
		bool deletePermissionSet(PermissionSetId id, std::string* diagnostic = nullptr);
		std::vector<AccessPermissionId> getPermissionSetPermissions(PermissionSetId id) const;
		bool setPermissionSetAccessPermission(PermissionSetId set,
			AccessPermissionId permission, bool included, std::string* diagnostic = nullptr);
		std::vector<PermissionSetId> getAgentPermissionSetAssignments(AgentId agent) const;
		bool setAgentPermissionSetAssignment(AgentId agent, PermissionSetId set,
			bool assigned, std::string* diagnostic = nullptr);

		// Static Location authorization (#273): authoring is paused-only; queries
		// remain available for inspection while running. Empty means unrestricted.
		bool isLocationPermissionEligible(uint32_t sectorIndex) const;
		std::vector<AccessPermissionId> getLocationPermissionRequirement(uint32_t sectorIndex) const;
		bool setLocationPermissionRequirement(uint32_t sectorIndex,
			std::vector<AccessPermissionId> const& permissions, std::string* diagnostic = nullptr);
		bool canAgentAccessLocation(Sector const& sector, Agent const& agent) const;

		bool isInteractionPointPermissionEligible(InteractionPointId point) const;
		// Shared transport destination API (historical Lift names). Omit objectIndex
		// for a Lift or Shuttle; specify the Room object for a Platform lift.
		// Positions are absolute Levels for Lifts and absolute x positions for Shuttles.
		std::vector<uint32_t> getLiftDestinationLevels(uint32_t sectorIndex, uint32_t objectIndex = ~0u) const;
		std::vector<AccessPermissionId> getLiftDestinationPermissionRequirement(
			uint32_t sectorIndex, uint32_t stopIndex, uint32_t objectIndex = ~0u) const;
		bool setLiftDestinationPermissionRequirement(uint32_t sectorIndex, uint32_t stopIndex,
			std::vector<AccessPermissionId> const& permissions, std::string* diagnostic = nullptr,
			uint32_t objectIndex = ~0u);

		bool setInteractionPointPermissionRequirement(InteractionPointId point,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr);
		std::vector<AccessPermissionId> getInteractionPointPermissionRequirement(
			InteractionPointId point) const;

		// Ordinary Doors only. Authored edits are paused and undoable; live edits
		// never dirty the document or change its initial condition.
		bool setLiftInitiallyBroken(TraversalResourceId lift, bool broken);
		bool setLiftBroken(TraversalResourceId lift, bool broken);
		bool setShuttleInitiallyBroken(TraversalResourceId shuttle, bool broken);
		bool setShuttleBroken(TraversalResourceId shuttle, bool broken);
		std::optional<DeviceCondition> knownTransportCondition(TraversalResourceId resource,
			Agent const* agent, Sector const* observationSector) const;
		std::optional<DeviceCondition> knownLiftCondition(TraversalResourceId resource,
			Agent const* agent, Sector const* observationSector) const
		{ return knownTransportCondition(resource, agent, observationSector); }
		bool setDoorInitiallyBroken(TraversalResourceId door, bool broken);
		bool setDoorBroken(TraversalResourceId door, bool broken);
		bool setExtensibleInitiallyBroken(TraversalResourceId resource, bool broken);
		bool setExtensibleBroken(TraversalResourceId resource, bool broken);
		bool isManualDoorPermissionEligible(TraversalResourceId door) const;
		bool setManualDoorPermissionRequirement(TraversalResourceId door,
			std::vector<AccessPermissionId> const& permissions,
			std::string* diagnostic = nullptr);
		std::vector<AccessPermissionId> getManualDoorPermissionRequirement(
			TraversalResourceId door) const;
		bool canAgentOpenManualDoor(TraversalResourceId door, AgentId agent) const;
		bool canAgentOperateDoorControl(TraversalResourceId door, SectorId approach,
			AgentId agent) const;
		// Permission adherence is willingness, not authorization: this checks only
		// whether an ordinary Door's applicable approach-side control requirements
		// are compatible with the Agent's effective property and grants.
		bool agentAdheresToDoorPermission(TraversalResourceId door, SectorId approach,
			AgentId agent) const;
		// Extensible controls are approach-specific even when both endpoints are in
		// one Location. The position distinguishes the physical side or endpoint.
		bool canAgentOperateExtensibleControl(TraversalResourceId resource,
			SectorId approach, Vector2 const& approachPosition, AgentId agent) const;
		// Permission adherence governs opportunistic use of a locally observed,
		// already-extended Force Bridge or extensible Ladder. It never authorizes
		// operating the controls themselves.
		bool agentAdheresToExtensiblePermission(TraversalResourceId resource,
			SectorId approach, Vector2 const& approachPosition, AgentId agent) const;
		std::vector<AccessPermissionId> missingLiftDestinationPermissions(
			DeviceCommand const& command, AgentId agent) const;
		bool canAgentUseLiftJourney(TraversalResourceId resource, Vector2 const& origin,
			Vector2 const& destination, AgentId agent) const;
		// Destination willingness only. Non-adherence never authorizes selection;
		// occupants are always allowed to complete an accepted journey.
		bool agentAdheresToLiftDestinationPermission(TraversalResourceId resource,
			uint32_t destinationStop, AgentId agent) const;
		bool canAgentOperateTransportLandingControl(TraversalResourceId resource,
			SectorId approach, Vector2 const& endpoint, AgentId agent) const;
		// Landing willingness only: destination selection and physical boarding
		// availability remain independent, and this is never an alighting gate.
		bool agentAdheresToTransportLandingPermission(TraversalResourceId resource,
			SectorId approach, Vector2 const& endpoint, AgentId agent) const;
		bool isAgentTransportOccupant(TraversalResourceId resource, AgentId agent) const;
		bool isTransportLocallyBoardable(TraversalResourceId resource,
			Vector2 const& endpoint) const;
		bool canAgentTraverseManualDoorNow(TraversalResourceId door, AgentId agent) const;
		void replanAgentAfterAuthorizationRefusal(AgentId agent);
		void beginVoluntaryRoutePlanning(AgentId agent);

		InteractionPointId createInteractionPoint(std::string const& name);

		InteractionPointId createInteractionPoint(std::string const& name, SectorId sector,
			Vector2 position, float reach, float durationSeconds, std::vector<InteractionBinding> bindings);

		EntityLookup<InteractionPoint> lookupInteractionPoint(InteractionPointId id);

		EntityLookup<InteractionPoint const> lookupInteractionPoint(InteractionPointId id) const;

		EntityRemovalResult removeInteractionPoint(InteractionPointId id);

		InteractionRequestId requestInteraction(InteractionPointId point, AgentId actor);
		// Editor history reconstructs into a new World; reserve runtime handle ranges before loading.
		void reserveAccessPanelIdentitiesFrom(World const& previous);
		std::shared_ptr<const AccessPanel> lookupAccessPanel(AccessPanelId id) const;
		bool canRequestAccessPanel(AccessPanelId id, AccessPanel::Action action, AgentId actor) const;
		InteractionRequestId requestAccessPanel(AccessPanelId id, AccessPanel::Action action, AgentId actor);

		EntityLookup<InteractionRequest const> lookupInteractionRequest(InteractionRequestId id) const;

		bool cancelInteraction(InteractionRequestId id);

		EntityRemovalResult removeInteractionRequest(InteractionRequestId id);

		// Activate a BoothWindow or Dumbwaiter user command without an authored edit.
		// Other device commands continue to activate through Interaction points.
		DeviceOperationId submitDeviceCommand(DeviceCommand const& command);
		DeviceOperationId pressDumbwaiterLanding(DumbwaiterId id, uint32_t stop);
		InteractionRequestId requestDumbwaiterLanding(DumbwaiterId id, uint32_t stop, AgentId actor);
		std::shared_ptr<const BoothWindow> lookupBoothWindow(BoothWindowId id) const;

		DeviceOperationId createDeviceOperation(std::string const& name, AgentId requester);

		EntityLookup<DeviceOperation> lookupDeviceOperation(DeviceOperationId id);

		EntityLookup<DeviceOperation const> lookupDeviceOperation(DeviceOperationId id) const;

		bool cancelDeviceOperation(DeviceOperationId id, AgentId requester);

		EntityRemovalResult removeDeviceOperation(DeviceOperationId id);

		TraversalResourceId createTraversalResource(std::string const& name);

		TraversalResourceId createDoorTraversalResource(std::string const& name,
			std::shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds);

		TraversalResourceId createWindowTraversalResource(std::string const& name,
			std::shared_ptr<Window> window);

		TraversalResourceId createLadderTraversalResource(std::string const& name,
			std::shared_ptr<Ladder> ladder, SectorId ladderSector,
			uint32_t directionalBatchLimit);

		TraversalResourceId createLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId liftSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createOpenPlatformLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId locationSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1,
			float stopDurationSeconds = CORE_PLATFORM_LIFT_STOP_DURATION);

		TraversalResourceId createShuttleTraversalResource(std::string const& name,
			std::shared_ptr<Shuttle> shuttle, SectorId shuttleSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createForceBridgeTraversalResource(std::string const& name,
			std::shared_ptr<ForceBridge> forceBridge);

		TraversalResourceId createStairwellTraversalResource(std::string const& name,
			std::shared_ptr<Stairwell> stairwell, SectorId stairwellSector,
			uint32_t capacity, uint32_t directionalBatchLimit);

		// Defines one physical waiting lane. The direction is normalized and
		// positions are generated at agent-safe spacing from origin through extent.
		// A door accepts at most two lanes, one per source sector.
		bool configureDoorQueueLane(TraversalResourceId resource, SectorId sector,
			Vector2 origin, Vector2 direction, float extent);

		// Configures independent threshold slots. Reconfiguration is rejected
		// while a crossing owns a lane.
		bool configureDoorCrossingLanes(TraversalResourceId resource, uint32_t laneCount);

		// External systems hold doors open through the same scoped safety
		// protocol. The lease protocol lives in SimulationCoordinator (ADR 0004);
		// these forward.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResourceId resource,
			DoorOpenLeaseKind kind = DoorOpenLeaseKind::ExternalHoldOpen);

		bool releaseDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseId lease);

		// Sensors report facts; only the traversal coordinator issues door actions.
		bool setDoorSensorObservation(TraversalResourceId resource, DoorSensorId sensor,
			DoorSensorObservation observation);

		// Disabling rejects future admission but never revokes active crossings.
		bool setTraversalResourceEnabled(TraversalResourceId resource, bool enabled);

		// Registers a physical control as applicable from its interaction point's sector.
		// Controlled traversal never falls back to operating the resource directly.
		bool addTraversalControl(TraversalResourceId resource, InteractionPointId control);

		EntityLookup<TraversalResource> lookupTraversalResource(TraversalResourceId id);

		EntityLookup<TraversalResource const> lookupTraversalResource(TraversalResourceId id) const;

		// Resolves physical geometry to the resource that owns its agent queue.
		// Transport landing doors resolve to their vehicle coordinator.
		TraversalResourceId getTraversalResourceId(Object const* object) const;

		EntityRemovalResult removeTraversalResource(TraversalResourceId id);

		EntityLookup<TraversalRequest const> lookupTraversalRequest(TraversalRequestId id) const;

		// True while the Agent owns logical membership in any simulation queue.
		// Pending work without a Queue ticket or queue entry does not count.
		bool isAgentInQueue(AgentId id) const;

		RouteChoicePolicy const& getRouteChoicePolicy() const { return mRouteChoicePolicy; }
		void setRouteChoicePolicy(RouteChoicePolicy policy) { mRouteChoicePolicy = policy; }

		TraversalWaitingPolicy const& getTraversalWaitingPolicy() const;

		void setTraversalWaitingPolicy(TraversalWaitingPolicy policy);

		TraversalGeometryPolicy const& getTraversalGeometryPolicy() const;

		void setTraversalGeometryPolicy(TraversalGeometryPolicy policy);

		// Pure route-cost queries: they create no ticket, operation, reservation, or permit.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;
		float observeAccessZoneDensity(TraversalResourceId resource,
			SectorId sourceSector) const;
		std::optional<ShuttleRouteAccessObservation> observeShuttleAccess(
			TraversalResourceId resource, Vector2 const& endpoint, bool includeLocalQueue) const;
		std::optional<LiftRouteAccessObservation> observeLiftAccess(
			TraversalResourceId resource, Vector2 const& sourceEndpoint,
			bool includeLocalQueue = true) const;
		uint32_t countStandingAgentsOnEscalator(Agent const* observer, Edge const* edge) const;

		// Wakes every Agent the World owns. Forwards to SimulationCoordinator,
		// where the Agent lifecycle lives (ADR 0004).
		void wakeAllAgents();

		// Restore authored Agent routes/positions and reconstruct all simulated
		// objects in their configured initial state.
		void resetSimulation();

		// Clear the modified state of the World and every Agent it owns, so
		// isModified() reports clean.  Only call this once a save has fully
		// succeeded; a save that fails must leave the dirty state intact.
		void markSaved();

		// Persist the World to filepath.  The clean-state transition happens
		// only after the file write has completely succeeded; any open, write,
		// flush, close, or replacement error throws and leaves the World and
		// its Agents exactly as dirty as they were before the attempt.
		void attachFurnitureCatalogue(std::string filename,
			std::shared_ptr<const FurnitureCatalogue> catalogue);
		auto const& furnitureCatalogue() const { return mFurnitureCatalogue; }
		std::string const& furnitureCatalogueFilename() const { return mFurnitureCatalogueFilename; }
		auto const& furniture() const { return mFurniture; }
		bool isFurnitureMarker(MarkerId id) const;
		std::optional<UsablePointAction> furnitureMarkerAction(MarkerId id) const;
		AgentId usablePointOccupant(MarkerId id) const;
		bool claimUsablePoint(AgentId agent, MarkerId marker);
		bool canEditFurniture(uint64_t id, float x, float y, std::string const& name,
			std::string* diagnostic = nullptr, std::optional<int> localDepth = std::nullopt) const;
		bool editFurniture(uint64_t id, float x, float y, std::string const& name,
			std::string* diagnostic = nullptr, std::optional<int> localDepth = std::nullopt);
		bool canRemoveFurniture(uint64_t id, std::string* diagnostic = nullptr) const;
		bool removeFurniture(uint64_t id, std::string* diagnostic = nullptr);
		bool canPlaceFurniture(uint32_t sector, std::string const& definition,
			float x, float y, std::string const& name, std::string* diagnostic = nullptr, int localDepth = 0) const;
		uint64_t placeFurniture(uint32_t sector, std::string const& definition,
			float x, float y, std::string const& name, int localDepth = 0);
		void saveTo(std::string const& filepath);

		// Rendering supplies elapsed wall time here.  It is accumulated and only
		// whole fixed simulation ticks are executed.
		//
		// The tick pipeline itself - the accumulator, the six simulation phases,
		// the per-phase lift, shuttle and door advancement, tick event
		// publication, the simulation clock and event consumption, and every
		// snapshot builder - lives in SimulationCoordinator (ADR 0004 stage 5);
		// every entry point below forwards to it.
		void update(float elapsedSeconds);

		// Non-owning; register/unregister on the simulation thread. The observer
		// must outlive its registration and may only read World during callbacks.
		void setSimulationObserver(SimulationObserver* observer) noexcept { mSimulationObserver = observer; }

		// Headless deterministic seam. These methods never use render timing and
		// report false when a behaviour failure stops the run before the next tick.
		bool advanceTick();

		bool advanceTicks(uint64_t count);

		static constexpr float getFixedTimestep()
		{
			return 1.0f / 60.0f;
		}

		uint64_t getSimulationTick() const;

		SimulationPhase getCurrentSimulationPhase() const;

		AgentId getAgentId(Agent const* agent) const;

		SimulationSnapshot getSimulationSnapshot() const;
		// Valid until the next tick or mutation; callers retaining data must copy it.
		SimulationSnapshot const& getSimulationSnapshotView() const;
		void invalidateSimulationSnapshot() const
		{
			mSnapshotValid = false;
			mPendingInteractionIndexValid = false;
		}
		uint64_t getSimulationSnapshotBuildCount() const { return mSnapshotBuildCount; }

		// Host/session state, deliberately excluded from authored serialization.
		void setSimulationTimeScale(double scale);
		double getSimulationTimeScale() const { return mTimeScale; }
		double getDeferredSimulationTime() const { return mAccumulatedTime; }
		// Mean completed tick cost in the trailing wall-clock second, or zero
		// when no ticks completed. Diagnostic only; includes event observation.
		double getAverageSimulationStepMicroseconds() const
		{
			return mSimulationStepTiming.averageMicroseconds();
		}
		static constexpr uint32_t getMaxTicksPerUpdate() { return 600; }

		std::vector<SimulationEvent> consumeSimulationEvents();
	};

} // core
