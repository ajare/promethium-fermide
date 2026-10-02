#pragma once

#include <string>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <map>
#include "core/DeviceCondition.h"
#include <utility>

#include "core/SectorPosition.h"
#include "core/Shape.h"
#include "core/Path.h"
#include "core/AgentTag.h"
#include "core/AgentBehaviour.h"
#include "core/EntityId.h"
#include "core/Serializable.h"


namespace core
{
	class World;
	class Sector;
	struct LiftRouteAccessObservation;

	struct PathIterator
	{
		std::shared_ptr<Path> path;
		uint32_t targetNode{ 0 };

		bool atEnd() const
		{
			return !path || targetNode >= (uint32_t)path->nodes.size();
		}
	};

	struct EffectiveAgentEscalatorWalkingChance
	{
		float value{ 0.0f };
		AgentTagId sourceTag{};
		bool individual{ false };
	};

	struct EffectiveAgentColour
	{
		AgentColour value{ EditorDefaultAgentColour };
		AgentTagId sourceTag{};
		bool individual{ false };
	};

	enum class SampledAgentPropertyType
	{
		WalkSpeedModifier,
		HeightModifier,
		StairSpeedModifier,
		LadderSpeedModifier,
		InteractionAversion,
		EffortAversion,
		WaitingAversion,
		CrowdAversion,
		RiskAversion,
		RouteFamiliarity,
		RoutePersistence,
		MinimumRoutePlanningTime,
		MaximumRoutePlanningTime
	};

	struct AgentPropertySample
	{
		SampledAgentPropertyType type{ SampledAgentPropertyType::WalkSpeedModifier };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		float value{ 1.0f };

		bool operator==(AgentPropertySample const& other) const = default;
	};

	struct EffectiveAgentWalkSpeedModifier
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentHeightModifier
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentStairSpeedModifier
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentLadderSpeedModifier
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentInteractionAversion
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentEffortAversion
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentWaitingAversion
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentCrowdAversion
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentRiskAversion
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentRouteFamiliarity
	{
		float value{ 0.5f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentRoutePersistence
	{
		float value{ 0.15f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentMinimumRoutePlanningTime
	{
		float value{ 1.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentMaximumRoutePlanningTime
	{
		float value{ 3.0f };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentPermissionAdherence
	{
		bool value{ true };
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	struct EffectiveAgentMobilityProfile
	{
		MobilityProfile value{};
		AgentTagId sourceTag{};
		uint64_t propertyRevision{ 0 };
		bool individual{ false };
	};

	class Agent : public Serializable
	{
		friend class World;
		friend class AgentBehaviourRegistry;
		friend class SimulationCoordinator;
		friend class Sector;

	public:

		struct EdgeTraversalData
		{
			std::shared_ptr<Sector> curSector, nextSector;
			std::shared_ptr<const Vertex> nextVertex;
		};

		enum struct State
		{
			Idle,
			MovingToVertex,
			WaitingForTraversal,
			TraversingEdge,
			AwaitingTraversalCommit,
			RoutePlanning
		};

		struct TraversalTask
		{
			TraversalRequestId request;
			TraversalPermitId permit;
			std::shared_ptr<const Edge> edge;
			std::shared_ptr<const Vertex> sourceVertex;
			std::shared_ptr<const Vertex> destinationVertex;
			uint32_t pathNodesConsumed{ 1 };
			uint64_t traversalTicksRemaining{ 0 };
			std::optional<bool> escalatorWalking;

		};

	private:

		std::string mName;
		// Counter-based simulation stream, separate from authored samples and Lua.
		// Only entry into a moving Escalator consumes a draw; never serialized.
		uint64_t mEscalatorTraversalSequence{ 0 };
		// Independent episode stream and timer; never authored or serialized.
		uint64_t mRoutePlanningSequence{ 0 };
		uint64_t mRoutePlanningTotalTicks{ 0 };
		uint64_t mRoutePlanningRemainingTicks{ 0 };
		// Transient, Agent-local journey identity used only for stable route
		// perception. Reset/reload starts the deterministic sequence again.
		uint64_t mRouteJourneySequence{ 0 };
		uint32_t mRouteJourneyDestinationVertexId{ 0 };

		// The Agent group this Agent is assigned to (ADR 0006). An empty
		// AgentGroupId means no Agent group, which is the default for every
		// newly created Agent and for every Agent loaded from a document that
		// predates the assignment field. The reference is to the group's stable
		// identity, never to its name, so renaming a group rewrites nothing.
		//
		// This is authored editor metadata. It never reaches the runtime
		// snapshot, the simulation events, or any movement, pathfinding,
		// capacity or traversal decision.
		AgentGroupId mAgentGroup{};

		// Authored direct Access permission grants. Slots are World-owned and
		// bounded, making membership deterministic and duplicate-free.
		std::bitset<256> mDirectAccessGrants;
		// Authored Permission set assignments. This is intentionally separate
		// from the external Agent tag registry.
		std::set<PermissionSetId> mPermissionSets;

		// Runtime authorization overlays are current simulation state only. They
		// express deltas from the authored initial grants and assignments, are not
		// serialized, survive pause/resume, and disappear on Reset simulation.
		std::bitset<256> mRuntimeDirectGrantAdditions;
		std::bitset<256> mRuntimeDirectGrantRemovals;
		std::set<PermissionSetId> mRuntimePermissionSetAdditions;
		std::set<PermissionSetId> mRuntimePermissionSetRemovals;

		// Agent tag assignments are World-authored references into the one
		// attached Agent tag registry. A set makes duplicate assignment
		// structurally impossible in memory and gives persistence a stable numeric
		// order. New Agents start with the empty set.
		std::set<AgentTagId> mAgentTags;

		// Individual Agent properties are authored directly on this Agent and
		// override properties inherited from Agent tags.
		std::optional<AgentColour> mIndividualColour;
		std::optional<float> mIndividualEscalatorWalkingChance;
		std::optional<float> mIndividualWalkSpeedModifier;
		std::optional<float> mIndividualHeightModifier;
		std::optional<float> mIndividualStairSpeedModifier;
		std::optional<float> mIndividualLadderSpeedModifier;
		std::optional<float> mIndividualInteractionAversion;
		std::optional<float> mIndividualEffortAversion;
		std::optional<float> mIndividualWaitingAversion;
		std::optional<float> mIndividualCrowdAversion;
		std::optional<float> mIndividualRiskAversion;
		std::optional<float> mIndividualRouteFamiliarity;
		std::optional<float> mIndividualRoutePersistence;
		std::optional<float> mIndividualMinimumRoutePlanningTime;
		std::optional<float> mIndividualMaximumRoutePlanningTime;
		std::optional<bool> mIndividualPermissionAdherence;
		std::optional<MobilityProfile> mIndividualMobilityProfile;

		// Modifier samples are authored per-Agent values rather than transient
		// simulation state. Their source identity and property revision make the
		// draw inspectable and let loading distinguish stable data from stale data.
		std::optional<AgentPropertySample> mWalkSpeedModifierSample;
		std::optional<AgentPropertySample> mHeightModifierSample;
		std::optional<AgentPropertySample> mStairSpeedModifierSample;
		std::optional<AgentPropertySample> mLadderSpeedModifierSample;
		std::optional<AgentPropertySample> mInteractionAversionSample;
		std::optional<AgentPropertySample> mEffortAversionSample;
		std::optional<AgentPropertySample> mWaitingAversionSample;
		std::optional<AgentPropertySample> mCrowdAversionSample;
		std::optional<AgentPropertySample> mRiskAversionSample;
		std::optional<AgentPropertySample> mRouteFamiliaritySample;
		std::optional<AgentPropertySample> mRoutePersistenceSample;
		std::optional<AgentPropertySample> mMinimumRoutePlanningTimeSample;
		std::optional<AgentPropertySample> mMaximumRoutePlanningTimeSample;

		// Activation is authored state (#118): an activated Agent is simulated,
		// a deactivated one keeps its authored position and route but no tick
		// acts on it. Every Agent starts activated, and so does every Agent
		// loaded from a document that predates the field. The running-simulation
		// gate lives on World's setAgentActive seam, not here: direct Agent
		// mutation is the unchecked editor seam setFlags already uses.
		bool mActive{ true };

		// Authored configuration only. Runtime Lua instance state never enters the
		// Agent or a World document.
		std::optional<AgentBehaviourAssignment> mBehaviourAssignment;

		World* mWorld{ nullptr };
		std::map<TraversalResourceId, DeviceCondition> mRememberedDeviceConditions;
		// Escalators have no constrained traversal resource; their stable Transit
		// index identifies condition knowledge within this simulation topology.
		std::map<uint32_t, DeviceCondition> mRememberedEscalatorConditions;

		SectorPosition mPosition;

		// Authored position and route are kept separate from transient locomotion.
		// Simulation advances mPosition/mPath, while serialization and Reset use
		// this immutable baseline.
		SectorPosition mResetPosition;
		std::shared_ptr<Path> mResetPath;
		bool mResetPathActive{ false };

		uint32_t mFlags;

		State mState;

		PathIterator mPath;
		// Position before the current route began. Traversal requests use this (or
		// the preceding path node) to retain the side from which the Agent approached.
		Vector2 mPathStartPosition;

		std::optional<TraversalTask> mTraversalTask;
		std::shared_ptr<const Edge> mStandingRouteEdge;
		void syncStandingRouteObservation();
		// A queue request may be made while the preceding same-sector Location
		// edge is still active, allowing the Agent to stop before the queue tail.
		std::optional<TraversalTask> mQueuedTraversalTask;

		// Traversal resources assign local goals; the Agent remains the sole owner
		// of walking and advances itself during the movement phase.
		std::optional<Vector2> mTraversalLocalGoal;

		// Prevent repeated opportunistic presses while following the same immediate
		// Door edge. The saved interaction lets traversal preparation reuse a press
		// made while passing instead of sending the Agent back from the threshold.
		// This is transient locomotion state, not authored simulation data.
		TraversalResourceId mEarlyDoorPressResource;
		InteractionRequestId mEarlyDoorPressInteraction;
		bool mEarlyDoorPressAttempted{ false };

		// Set when locomotion stops at the outer edge of an available queue lane.
		// +1 approaches from the left, -1 from the right, and 0 requests at the endpoint.
		int mEarlyQueueApproachDirectionX{ 0 };

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		// Assignment belongs to World::setAgentGroup, which has already
		// judged both the Agent and the Agent group against this World.
		void setAgentGroupId(AgentGroupId id) { mAgentGroup = id; }
		void setDirectAccessGrants(std::bitset<256> grants) { mDirectAccessGrants = grants; }
		void setPermissionSets(std::set<PermissionSetId> sets) { mPermissionSets = std::move(sets); }

		// Assignment mutation belongs to World, which validates the Agent,
		// registry, tag, duplicate state, and paused simulation before calling.
		void assignAgentTag(AgentTagId id) { mAgentTags.insert(id); }
		void removeAgentTag(AgentTagId id) { mAgentTags.erase(id); }
		void setAgentTags(std::set<AgentTagId> tags) { mAgentTags = std::move(tags); }
		void setWalkSpeedModifierSample(AgentPropertySample sample)
		{
			mWalkSpeedModifierSample = sample;
		}
		void clearWalkSpeedModifierSample() { mWalkSpeedModifierSample.reset(); }
		void setHeightModifierSample(AgentPropertySample sample)
		{
			mHeightModifierSample = sample;
		}
		void clearHeightModifierSample() { mHeightModifierSample.reset(); }
		void setStairSpeedModifierSample(AgentPropertySample sample)
		{
			mStairSpeedModifierSample = sample;
		}
		void clearStairSpeedModifierSample() { mStairSpeedModifierSample.reset(); }
		void setLadderSpeedModifierSample(AgentPropertySample sample)
		{
			mLadderSpeedModifierSample = sample;
		}
		void clearLadderSpeedModifierSample() { mLadderSpeedModifierSample.reset(); }
		void setInteractionAversionSample(AgentPropertySample sample)
		{
			mInteractionAversionSample = sample;
		}
		void clearInteractionAversionSample() { mInteractionAversionSample.reset(); }
		void setEffortAversionSample(AgentPropertySample sample)
		{
			mEffortAversionSample = sample;
		}
		void clearEffortAversionSample() { mEffortAversionSample.reset(); }
		void setWaitingAversionSample(AgentPropertySample sample)
		{
			mWaitingAversionSample = sample;
		}
		void clearWaitingAversionSample() { mWaitingAversionSample.reset(); }
		void setCrowdAversionSample(AgentPropertySample sample)
		{
			mCrowdAversionSample = sample;
		}
		void clearCrowdAversionSample() { mCrowdAversionSample.reset(); }
		void setRiskAversionSample(AgentPropertySample sample)
		{
			mRiskAversionSample = sample;
		}
		void clearRiskAversionSample() { mRiskAversionSample.reset(); }
		void setRouteFamiliaritySample(AgentPropertySample sample)
		{
			mRouteFamiliaritySample = sample;
		}
		void clearRouteFamiliaritySample() { mRouteFamiliaritySample.reset(); }
		void setRoutePersistenceSample(AgentPropertySample sample)
		{
			mRoutePersistenceSample = sample;
		}
		void clearRoutePersistenceSample() { mRoutePersistenceSample.reset(); }
		void setMinimumRoutePlanningTimeSample(AgentPropertySample sample)
		{
			mMinimumRoutePlanningTimeSample = sample;
		}
		void clearMinimumRoutePlanningTimeSample() { mMinimumRoutePlanningTimeSample.reset(); }
		void setMaximumRoutePlanningTimeSample(AgentPropertySample sample)
		{
			mMaximumRoutePlanningTimeSample = sample;
		}
		void clearMaximumRoutePlanningTimeSample() { mMaximumRoutePlanningTimeSample.reset(); }
		void setIndividualColour(std::optional<AgentColour> value)
		{ mIndividualColour = value; modify(); }
		void setIndividualEscalatorWalkingChance(std::optional<float> value)
		{ mIndividualEscalatorWalkingChance = value; modify(); }
		void setIndividualWalkSpeedModifier(std::optional<float> value)
		{ mIndividualWalkSpeedModifier = value; modify(); }
		void setIndividualHeightModifier(std::optional<float> value)
		{ mIndividualHeightModifier = value; modify(); }
		void setIndividualStairSpeedModifier(std::optional<float> value)
		{ mIndividualStairSpeedModifier = value; modify(); }
		void setIndividualLadderSpeedModifier(std::optional<float> value)
		{ mIndividualLadderSpeedModifier = value; modify(); }
		void setIndividualInteractionAversion(std::optional<float> value)
		{ mIndividualInteractionAversion = value; modify(); }
		void setIndividualEffortAversion(std::optional<float> value)
		{ mIndividualEffortAversion = value; modify(); }
		void setIndividualWaitingAversion(std::optional<float> value)
		{ mIndividualWaitingAversion = value; modify(); }
		void setIndividualCrowdAversion(std::optional<float> value)
		{ mIndividualCrowdAversion = value; modify(); }
		void setIndividualRiskAversion(std::optional<float> value)
		{ mIndividualRiskAversion = value; modify(); }
		void setIndividualRouteFamiliarity(std::optional<float> value)
		{ mIndividualRouteFamiliarity = value; modify(); }
		void setIndividualRoutePersistence(std::optional<float> value)
		{ mIndividualRoutePersistence = value; modify(); }
		void setIndividualMinimumRoutePlanningTime(std::optional<float> value)
		{ mIndividualMinimumRoutePlanningTime = value; modify(); }
		void setIndividualMaximumRoutePlanningTime(std::optional<float> value)
		{ mIndividualMaximumRoutePlanningTime = value; modify(); }
		void setIndividualPermissionAdherence(std::optional<bool> value)
		{ mIndividualPermissionAdherence = value; modify(); }
		void setIndividualMobilityProfile(std::optional<MobilityProfile> value)
		{ mIndividualMobilityProfile = value; modify(); }
		void setBehaviourAssignment(AgentBehaviourAssignment assignment)
		{
			mBehaviourAssignment = std::move(assignment);
			modify();
		}
		void clearBehaviourAssignment()
		{
			mBehaviourAssignment.reset();
			modify();
		}

		// Authored placement checks Location authorization. Runtime-only callers
		// may finish an entry committed before authorization was lost.
		void setPosition(SectorPosition pos, bool authored = true);

		void attachToWorld(World* world);

		void assignPath(std::shared_ptr<Path> path, bool startPathing, bool markModified);

		void clearRuntimePath();

		bool moveToPosition(Vector2 const& pos, float frameTime, float speed);

		uint32_t getSkippablePathTarget(uint32_t vertexA) const;

		bool moveToPosition(Vector2 const& pos, float frameTime)
		{
			return moveToPosition(pos, frameTime, getWalkSpeed());
		}

		void startIdling();

		void startPathingInternal();

		bool nextPathNode();

		void moveToVertex(float frameTime);

		void collectTraversalIntent();

		void allocateTraversal();

		void commitTraversal();

		void cleanupTraversal();

		void considerTraversalReplan();


		void cancelTraversal();

		bool moveToVertexOffset(int dim, float offset, float frameTime);

		// Pathing helpers
		PathNode& getTargetPathNode() const;

		bool atEndOfPath() const;


	public:

		Agent(std::string const& name);

		virtual ~Agent();
		Agent(Agent const&) = delete;
		Agent& operator=(Agent const&) = delete;

		std::string const& getName() const;

		// The Agent group this Agent is assigned to. An empty AgentGroupId
		// means no Agent group.
		AgentGroupId getAgentGroupId() const { return mAgentGroup; }

		// Stable IDs of the Agent tags assigned to this Agent, in ascending
		// numeric order. The referenced definitions live in the World's
		// attached Agent tag registry.
		std::set<AgentTagId> const& getAgentTagIds() const { return mAgentTags; }
		bool hasAgentTag(AgentTagId id) const { return mAgentTags.contains(id); }

		std::optional<AgentColour> const& getIndividualColour() const
		{ return mIndividualColour; }
		std::optional<float> const& getIndividualEscalatorWalkingChance() const
		{ return mIndividualEscalatorWalkingChance; }
		std::optional<float> const& getIndividualWalkSpeedModifier() const
		{ return mIndividualWalkSpeedModifier; }
		std::optional<float> const& getIndividualHeightModifier() const
		{ return mIndividualHeightModifier; }
		std::optional<float> const& getIndividualStairSpeedModifier() const
		{ return mIndividualStairSpeedModifier; }
		std::optional<float> const& getIndividualLadderSpeedModifier() const
		{ return mIndividualLadderSpeedModifier; }
		std::optional<float> const& getIndividualInteractionAversion() const
		{ return mIndividualInteractionAversion; }
		std::optional<float> const& getIndividualEffortAversion() const
		{ return mIndividualEffortAversion; }
		std::optional<float> const& getIndividualWaitingAversion() const
		{ return mIndividualWaitingAversion; }
		std::optional<float> const& getIndividualCrowdAversion() const
		{ return mIndividualCrowdAversion; }
		std::optional<float> const& getIndividualRiskAversion() const
		{ return mIndividualRiskAversion; }
		std::optional<float> const& getIndividualRouteFamiliarity() const
		{ return mIndividualRouteFamiliarity; }
		std::optional<float> const& getIndividualRoutePersistence() const
		{ return mIndividualRoutePersistence; }
		std::optional<float> const& getIndividualMinimumRoutePlanningTime() const
		{ return mIndividualMinimumRoutePlanningTime; }
		std::optional<float> const& getIndividualMaximumRoutePlanningTime() const
		{ return mIndividualMaximumRoutePlanningTime; }
		std::optional<bool> const& getIndividualPermissionAdherence() const
		{ return mIndividualPermissionAdherence; }
		std::optional<MobilityProfile> const& getIndividualMobilityProfile() const
		{ return mIndividualMobilityProfile; }

		// Resolves Colour from the individual property first, then assigned tags;
		// an uncoloured Agent receives the editor default.
		EffectiveAgentColour getEffectiveColour() const;

		// Resolves an individual Walk speed value before the persisted tag sample.
		// An Agent without either source exposes the neutral modifier.
		EffectiveAgentWalkSpeedModifier getEffectiveWalkSpeedModifier() const;
		std::optional<AgentPropertySample> const& getWalkSpeedModifierSample() const
		{
			return mWalkSpeedModifierSample;
		}

		// Resolves an individual Height value before the persisted tag sample. It
		// scales only visual height and bounds; physical dimensions stay fixed.
		EffectiveAgentHeightModifier getEffectiveHeightModifier() const;
		EffectiveAgentStairSpeedModifier getEffectiveStairSpeedModifier() const;
		EffectiveAgentLadderSpeedModifier getEffectiveLadderSpeedModifier() const;
		EffectiveAgentInteractionAversion getEffectiveInteractionAversion() const;
		EffectiveAgentEffortAversion getEffectiveEffortAversion() const;
		EffectiveAgentWaitingAversion getEffectiveWaitingAversion() const;
		EffectiveAgentCrowdAversion getEffectiveCrowdAversion() const;
		EffectiveAgentRiskAversion getEffectiveRiskAversion() const;
		EffectiveAgentRouteFamiliarity getEffectiveRouteFamiliarity() const;
		EffectiveAgentRoutePersistence getEffectiveRoutePersistence() const;
		uint64_t getRoutePlanningTotalTicks() const { return mRoutePlanningTotalTicks; }
		uint64_t getRoutePlanningRemainingTicks() const { return mRoutePlanningRemainingTicks; }
		EffectiveAgentMinimumRoutePlanningTime getEffectiveMinimumRoutePlanningTime() const;
		EffectiveAgentMaximumRoutePlanningTime getEffectiveMaximumRoutePlanningTime() const;
		EffectiveAgentPermissionAdherence getEffectivePermissionAdherence() const;
		EffectiveAgentMobilityProfile getEffectiveMobilityProfile() const;
		uint64_t getRouteJourneyIdentity(Vertex const* destination) const;
		std::optional<AgentPropertySample> const& getHeightModifierSample() const
		{
			return mHeightModifierSample;
		}
		std::optional<AgentPropertySample> const& getStairSpeedModifierSample() const
		{
			return mStairSpeedModifierSample;
		}
		std::optional<AgentPropertySample> const& getLadderSpeedModifierSample() const
		{
			return mLadderSpeedModifierSample;
		}
		std::optional<AgentPropertySample> const& getInteractionAversionSample() const
		{
			return mInteractionAversionSample;
		}
		std::optional<AgentPropertySample> const& getEffortAversionSample() const
		{
			return mEffortAversionSample;
		}
		std::optional<AgentPropertySample> const& getWaitingAversionSample() const
		{
			return mWaitingAversionSample;
		}
		std::optional<AgentPropertySample> const& getCrowdAversionSample() const
		{
			return mCrowdAversionSample;
		}
		std::optional<AgentPropertySample> const& getRiskAversionSample() const
		{
			return mRiskAversionSample;
		}
		std::optional<AgentPropertySample> const& getRouteFamiliaritySample() const
		{
			return mRouteFamiliaritySample;
		}
		std::optional<AgentPropertySample> const& getRoutePersistenceSample() const
		{
			return mRoutePersistenceSample;
		}
		std::optional<AgentPropertySample> const& getMinimumRoutePlanningTimeSample() const
		{
			return mMinimumRoutePlanningTimeSample;
		}
		std::optional<AgentPropertySample> const& getMaximumRoutePlanningTimeSample() const
		{
			return mMaximumRoutePlanningTimeSample;
		}

		// Whether this Agent is simulated. Deactivation changes no authored
		// state: position and route stay as they are until an activated tick
		// or a reset works on them.
		bool isActive() const { return mActive; }

		std::optional<AgentBehaviourAssignment> const& getBehaviourAssignment() const
		{
			return mBehaviourAssignment;
		}

		void setActive(bool active);

		State getState() const;

		bool isInQueue() const;

		std::string getDescription() const;

		Sector const* getSector() const;

		Vector2 const& getLocalPosition() const;

		Vector2 getGlobalPosition() const;

		float getWidth() const;

		float getHeight() const;

		Shape getBounds() const;

		float getWalkSpeed() const;
		std::optional<DeviceCondition> rememberedEscalatorCondition(uint32_t sectorIndex) const;
		EffectiveAgentEscalatorWalkingChance getEffectiveEscalatorWalkingChance() const;
		std::optional<bool> getActiveEscalatorWalking() const
		{ return mTraversalTask ? mTraversalTask->escalatorWalking : std::nullopt; }
		bool isWalkingOnEscalator(Edge const* edge) const
		{ return mTraversalTask && mTraversalTask->edge.get() == edge
			&& mTraversalTask->escalatorWalking.value_or(false); }
		bool isStandingOnEscalator(Edge const* edge) const
		{ return mTraversalTask && mTraversalTask->edge.get() == edge
			&& mTraversalTask->escalatorWalking.has_value()
			&& !*mTraversalTask->escalatorWalking; }

		float getClimbSpeed() const;

		// Uses the World's configured stationary-stair baseline. Stair speed
		// modifiers are introduced separately; this is the neutral physical speed.
		float getStationaryStairSpeed(bool ascending) const;

		// Used by edge route-cost implementations; these are observations only.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;
		std::optional<DeviceCondition> rememberedDeviceCondition(TraversalResourceId resource) const;
		float observeAccessZoneDensity(TraversalResourceId resource,
			SectorId sourceSector) const;
		std::optional<ShuttleRouteAccessObservation> observeShuttleAccess(
			TraversalResourceId resource, Vector2 const& endpoint, bool includeLocalQueue) const;
		std::optional<LiftRouteAccessObservation> observeLiftAccess(
			TraversalResourceId resource, Vector2 const& sourceEndpoint,
			bool includeLocalQueue = true) const;
		uint32_t countObservedStandingEscalatorAgents(Edge const* edge) const;

		uint32_t getFlags() const;

		bool flagsSet(uint32_t flags) const;

		void setFlags(uint32_t flags);

		void unsetFlags(uint32_t flags);

		std::shared_ptr<Path> const& getPath() const;

		uint32_t getPathTargetNodeIndex() const;

		bool hasActiveLocomotionTask() const;

		TraversalRequestId getTraversalRequestId() const;

		TraversalPermitId getTraversalPermitId() const;

		EdgeTraversalData getEdgeTraversalData() const;

		void setPath(std::shared_ptr<Path> path, bool startPathing);

		void clearPath();

		void startPathing();

		void pausePathing();

		int chooseVertexOffset(int dim, std::pair<float, uint32_t> const* offsets, uint32_t numOffsets);


		void wake();

		void update(float frameTime);
	};

} // core
