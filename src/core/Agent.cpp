#include "core/Agent.h"
#include "core/AgentTypeRuntime.h"
#include "core/AgentType.h"
#include "core/DoorEdge.h"
#include "core/BulkheadDoorEdge.h"

#include "core/World.h"
#include "core/Edge.h"
#include "core/Location.h"
#include "core/Marker.h"
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/AgentTagRegistry.h"
#include "core/Log.h"
#include "core/MobilityProfile.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"

#include <algorithm>
#include <algorithm>
#include <algorithm>
#include <type_traits>
#include <variant>

namespace core
{

	using namespace std;

	namespace
	{
		void serializeBehaviourValue(Serializer& serializer,
			AgentBehaviourConfigurationValue const& value)
		{
			serializer.writeString("type", agentBehaviourConfigurationValueTypeName(value));
			visit([&](auto const& typed)
			{
				using T = decay_t<decltype(typed)>;
				if constexpr (is_same_v<T, bool>) serializer.writeBool("value", typed);
				else if constexpr (is_same_v<T, int64_t>) serializer.writeInt64("value", typed);
				else if constexpr (is_same_v<T, double>) serializer.writeDouble("value", typed);
				else if constexpr (is_same_v<T, string>) serializer.writeString("value", typed);
				else if constexpr (is_same_v<T, AgentBehaviourAction>)
					serializer.writeString("value", typed.reference);
				else if constexpr (is_same_v<T, AgentBehaviourDuration>)
					serializer.writeUint64("value", typed.ticks);
				else if constexpr (is_same_v<T, MarkerId>)
					serializer.writeUint64("value", typed.value);
				else if constexpr (is_same_v<T, AgentBehaviourConfigurationList>)
				{
					serializer.beginArray("value");
					for (auto const& item : typed)
					{
						serializer.beginMap("");
						serializeBehaviourValue(serializer, item);
						serializer.endMap();
					}
					serializer.endArray();
				}
				else
				{
					serializer.beginArray("value");
					for (auto const& [field, item] : typed)
					{
						serializer.beginMap("");
						serializer.writeString("field", field);
						serializeBehaviourValue(serializer, item);
						serializer.endMap();
					}
					serializer.endArray();
				}
			}, value.value);
		}

		AgentBehaviourConfigurationValue deserializeBehaviourValue(
			Serializer& serializer, string const& field, size_t depth = 1)
		{
			if (depth > MaxAgentBehaviourConfigurationDepth)
				throw SerializationException(format(
					"Agent behaviour configuration field '{}' exceeds maximum nesting depth 16",
					field));
			auto const type = serializer.readString("type");
			if (type == "boolean") return serializer.readBool("value");
			if (type == "integer") return serializer.readInt64("value");
			if (type == "number") return serializer.readDouble("value");
			if (type == "string") return serializer.readString("value");
			if (type == "action") return AgentBehaviourAction{ serializer.readString("value") };
			if (type == "duration")
				return AgentBehaviourDuration{ serializer.readUint64("value") };
			if (type == "marker") return MarkerId{ serializer.readUint64("value") };
			if (type == "list")
			{
				AgentBehaviourConfigurationList list;
				serializer.beginArray("value");
				while (serializer.nextArrayItem())
				{
					if (list.size() >= MaxAgentBehaviourListElements)
						throw SerializationException(format(
							"Agent behaviour configuration field '{}' exceeds 4096 List elements",
							field));
					serializer.beginMap("");
					list.push_back(deserializeBehaviourValue(serializer,
						field + "[" + to_string(list.size()) + "]", depth + 1));
					serializer.endMap();
				}
				serializer.endArray();
				return list;
			}
			if (type == "record")
			{
				AgentBehaviourConfigurationRecord record;
				serializer.beginArray("value");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					auto nestedField = serializer.readString("field");
					if (nestedField.empty())
						throw SerializationException(format(
							"Agent behaviour configuration Record '{}' contains a blank field",
							field));
					auto nested = deserializeBehaviourValue(serializer,
						field + "." + nestedField, depth + 1);
					serializer.endMap();
					if (!record.emplace(nestedField, std::move(nested)).second)
						throw SerializationException(format(
							"Agent behaviour configuration field '{}.{}' appears twice",
							field, nestedField));
				}
				serializer.endArray();
				return record;
			}
			throw SerializationException(format(
				"Agent behaviour configuration field '{}' has unknown type '{}'", field, type));
		}
	}

	std::unique_ptr<Agent> Agent::create(AgentTypeDefinition const& definition, string const& name)
	{
		AgentTypeRuntimeAdapter runtime;
		auto result = runtime.construct(definition.typeId, definition.source, name);
		if (!result.succeeded)
			throw SerializationException("Agent type '" + definition.typeId
				+ "' resource '" + definition.resourceName + "': " + result.diagnostic);
		auto agent = std::unique_ptr<Agent>(new Agent(name));
		agent->setTypeIdentity(definition.typeId, definition.displayName,
			definition.resourceName, result.baseline, result.defaultMobilityProfile);
		agent->mLuaInstance = std::move(result.instance);
		return agent;
	}

	Vector2 Agent::placementDimensions(AgentPhysicalBaseline const& physical, float heightModifier)
	{
		return { physical.width, physical.standingHeight * heightModifier };
	}

	Agent::Agent(string const& name)
		: mName(name)
		, mFlags(0)
		, mState(State::Idle)
	{
	}

	Agent::~Agent()
	{
		if (mStandingRouteEdge) mStandingRouteEdge->changeStandingRouteAgents(-1);
	}

	void Agent::syncStandingRouteObservation()
	{
		auto standing = mActive && mTraversalTask
			&& mTraversalTask->escalatorWalking.has_value()
			&& !*mTraversalTask->escalatorWalking ? mTraversalTask->edge : nullptr;
		if (standing == mStandingRouteEdge) return;
		if (mStandingRouteEdge) mStandingRouteEdge->changeStandingRouteAgents(-1);
		if (standing) standing->changeStandingRouteAgents(1);
		mStandingRouteEdge = std::move(standing);
	}

	bool Agent::childrenModified() const
	{
		return false;
	}

	void Agent::serializeImpl(Serializer& serializer, SerializationWorkData&) const
	{
		serializer.beginMap("agent");
		serializer.writeString("type", mTypeId);
		// Stable type ID and application Resource reference (ADR 0010). Keep the
		// historical type wire slot, but do not persist revision-dependent display
		// names as authored identity. typeId remains authoritative on load.
		serializer.writeString("typeId", mTypeId);
		if (!mTypeResourceName.empty())
			serializer.writeString("resource", mTypeResourceName);
		serializer.writeString("name", mName);
		serializer.writeUint32("flags", mFlags);
		// The Agent group is written by its stable ID and never by name, so a
		// rename of the group leaves every assigned Agent's stored form
		// untouched (ADR 0006). An Agent with no group writes no field at all -
		// the same convention as an Agent with no path, which writes no `path`
		// map - so a missing field reads back as "no Agent group".
		if (mAgentGroup) serializer.writeUint64("group", mAgentGroup.value);
		if (!mAgentTags.empty())
		{
			serializer.beginArray("tags");
			// std::set iteration is ascending AgentTagId order, keeping document
			// diffs stable regardless of the order in which tags were assigned.
			for (auto const id : mAgentTags) serializer.writeUint64("", id.value);
			serializer.endArray();
		}
		if (mIndividualColour || mIndividualEscalatorWalkingChance
			|| mIndividualWalkSpeedModifier || mIndividualHeightModifier
			|| mIndividualStairSpeedModifier || mIndividualLadderSpeedModifier
			|| mIndividualInteractionAversion || mIndividualEffortAversion || mIndividualWaitingAversion
			|| mIndividualCrowdAversion
			|| mIndividualRiskAversion
			|| mIndividualRouteFamiliarity
			|| mIndividualRoutePersistence
			|| mIndividualMinimumRoutePlanningTime
			|| mIndividualMaximumRoutePlanningTime
			|| mIndividualPermissionAdherence.has_value()
			|| mIndividualMobilityProfile)
		{
			serializer.beginArray("individualProperties");
			auto beginProperty = [&serializer](char const* type)
			{
				serializer.beginMap("");
				serializer.writeString("type", type);
			};
			if (mIndividualColour)
			{
				beginProperty("colour");
				serializer.writeUint8("r", mIndividualColour->r);
				serializer.writeUint8("g", mIndividualColour->g);
				serializer.writeUint8("b", mIndividualColour->b);
				serializer.endMap();
			}
			auto writeFloatProperty = [&serializer, &beginProperty](char const* type, float value)
			{
				beginProperty(type);
				serializer.writeFloat("value", value);
				serializer.endMap();
			};
			if (mIndividualEscalatorWalkingChance)
				writeFloatProperty("escalatorWalkingChance", *mIndividualEscalatorWalkingChance);
			if (mIndividualWalkSpeedModifier)
				writeFloatProperty("walkSpeedModifier", *mIndividualWalkSpeedModifier);
			if (mIndividualHeightModifier)
				writeFloatProperty("heightModifier", *mIndividualHeightModifier);
			if (mIndividualStairSpeedModifier)
				writeFloatProperty("stairSpeedModifier", *mIndividualStairSpeedModifier);
			if (mIndividualLadderSpeedModifier)
				writeFloatProperty("ladderSpeedModifier", *mIndividualLadderSpeedModifier);
			if (mIndividualInteractionAversion)
				writeFloatProperty("interactionAversion", *mIndividualInteractionAversion);
			if (mIndividualEffortAversion)
				writeFloatProperty("effortAversion", *mIndividualEffortAversion);
			if (mIndividualWaitingAversion)
				writeFloatProperty("waitingAversion", *mIndividualWaitingAversion);
			if (mIndividualCrowdAversion)
				writeFloatProperty("crowdAversion", *mIndividualCrowdAversion);
			if (mIndividualRiskAversion)
				writeFloatProperty("riskAversion", *mIndividualRiskAversion);
			if (mIndividualRouteFamiliarity)
				writeFloatProperty("routeFamiliarity", *mIndividualRouteFamiliarity);
			if (mIndividualRoutePersistence)
				writeFloatProperty("routePersistence", *mIndividualRoutePersistence);
			if (mIndividualMinimumRoutePlanningTime)
				writeFloatProperty("minimumRoutePlanningTime", *mIndividualMinimumRoutePlanningTime);
			if (mIndividualMaximumRoutePlanningTime)
				writeFloatProperty("maximumRoutePlanningTime", *mIndividualMaximumRoutePlanningTime);
			if (mIndividualPermissionAdherence)
			{
				beginProperty("permissionAdherence");
				serializer.writeBool("value", *mIndividualPermissionAdherence);
				serializer.endMap();
			}
			if (mIndividualMobilityProfile)
			{
				beginProperty("mobilityProfile");
				serializeMobilityProfile(serializer, *mIndividualMobilityProfile);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (mWalkSpeedModifierSample || mHeightModifierSample || mStairSpeedModifierSample
			|| mLadderSpeedModifierSample || mInteractionAversionSample || mEffortAversionSample
			|| mWaitingAversionSample || mCrowdAversionSample || mRiskAversionSample
			|| mRouteFamiliaritySample || mRoutePersistenceSample || mMinimumRoutePlanningTimeSample || mMaximumRoutePlanningTimeSample)
		{
			serializer.beginArray("propertySamples");
			auto writeSample = [&serializer](char const* type,
				AgentPropertySample const& sample)
			{
				serializer.beginMap("");
				serializer.writeString("type", type);
				serializer.writeUint64("sourceTag", sample.sourceTag.value);
				serializer.writeUint64("propertyRevision", sample.propertyRevision);
				serializer.writeFloat("value", sample.value);
				serializer.endMap();
			};
			if (mWalkSpeedModifierSample)
				writeSample("walkSpeedModifier", *mWalkSpeedModifierSample);
			if (mHeightModifierSample)
				writeSample("heightModifier", *mHeightModifierSample);
			if (mStairSpeedModifierSample)
				writeSample("stairSpeedModifier", *mStairSpeedModifierSample);
			if (mLadderSpeedModifierSample)
				writeSample("ladderSpeedModifier", *mLadderSpeedModifierSample);
			if (mInteractionAversionSample)
				writeSample("interactionAversion", *mInteractionAversionSample);
			if (mEffortAversionSample)
				writeSample("effortAversion", *mEffortAversionSample);
			if (mWaitingAversionSample)
				writeSample("waitingAversion", *mWaitingAversionSample);
			if (mCrowdAversionSample)
				writeSample("crowdAversion", *mCrowdAversionSample);
			if (mRiskAversionSample)
				writeSample("riskAversion", *mRiskAversionSample);
			if (mRouteFamiliaritySample)
				writeSample("routeFamiliarity", *mRouteFamiliaritySample);
			if (mRoutePersistenceSample)
				writeSample("routePersistence", *mRoutePersistenceSample);
			if (mMinimumRoutePlanningTimeSample)
				writeSample("minimumRoutePlanningTime", *mMinimumRoutePlanningTimeSample);
			if (mMaximumRoutePlanningTimeSample)
				writeSample("maximumRoutePlanningTime", *mMaximumRoutePlanningTimeSample);
			serializer.endArray();
		}
		// An activated Agent writes no `active` key at all - the same convention
		// as an Agent with no path writing no `path` map - so a document written
		// before activation existed reads back with every Agent activated (#118).
		if (!mActive) serializer.writeBool("active", false);
		if (mBehaviourAssignment)
		{
			serializer.beginMap("behaviour");
			serializer.writeUint64("id", mBehaviourAssignment->behaviour.value);
			serializer.writeUint64("revision", mBehaviourAssignment->revision);
			serializer.beginArray("configuration");
			for (auto const& [field, value] : mBehaviourAssignment->configuration)
			{
				serializer.beginMap("");
				serializer.writeString("field", field);
				serializeBehaviourValue(serializer, value);
				serializer.endMap();
			}
			serializer.endArray();
			serializer.endMap();
		}
		serializer.endMap();
	}

	bool Agent::deserializeImpl(Serializer& serializer, SerializationWorkData&)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		serializer.beginMap("agent");
		auto const type = serializer.readString("type", true, "Human");
		// Presentation may change in the resolved revision; only the stable ID
		// is identity. World already validated the authoritative resource reference.
		auto const typeId = serializer.readString("typeId", true, type);
		if (typeId != getTypeId())
			throw SerializationException("Serialized Agent type ID '" + typeId
				+ "' does not match the resolved type '" + getTypeId() + "'");
		mName = serializer.readString("name");
		mFlags = serializer.readUint32("flags");
		// Absent means no Agent group. Whether an ID that is present actually
		// names a group this World owns is the World's call, made before
		// the Agent is taken in.
		mAgentGroup = AgentGroupId{ serializer.readUint64("group", true, 0) };
		set<AgentTagId> agentTags;
		if (serializer.hasField("tags"))
		{
			serializer.beginArray("tags");
			while (serializer.nextArrayItem())
			{
				auto const id = AgentTagId{ serializer.readUint64("") };
				if (!id)
					throw SerializationException("Serialized Agent tag ID cannot be zero");
				if (!agentTags.insert(id).second)
					throw SerializationException(format(
						"Serialized Agent tag IDs must be unique ({} appears twice)", id.value));
			}
			serializer.endArray();
		}
		mAgentTags = std::move(agentTags);
		mIndividualColour.reset();
		mIndividualEscalatorWalkingChance.reset();
		mIndividualWalkSpeedModifier.reset();
		mIndividualHeightModifier.reset();
		mIndividualStairSpeedModifier.reset();
		mIndividualLadderSpeedModifier.reset();
		mIndividualInteractionAversion.reset();
		mIndividualEffortAversion.reset();
		mIndividualWaitingAversion.reset();
		mIndividualCrowdAversion.reset();
		mIndividualRiskAversion.reset();
		mIndividualRouteFamiliarity.reset();
		mIndividualRoutePersistence.reset();
		mIndividualMinimumRoutePlanningTime.reset();
		mIndividualMaximumRoutePlanningTime.reset();
		mIndividualPermissionAdherence.reset();
		mIndividualMobilityProfile.reset();
		if (serializer.hasField("individualProperties"))
		{
			serializer.beginArray("individualProperties");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto const type = serializer.readString("type");
				if (type == "colour")
				{
					if (mIndividualColour)
						throw SerializationException("Serialized Agent contains more than one individual Colour");
					mIndividualColour = AgentColour{ serializer.readUint8("r"),
						serializer.readUint8("g"), serializer.readUint8("b") };
				}
				else if (type == "escalatorWalkingChance")
				{
					if (mIndividualEscalatorWalkingChance)
						throw SerializationException("Serialized Agent contains more than one individual Escalator walking chance");
					auto const value = serializer.readFloat("value");
					if (!agentEscalatorWalkingChanceIsValid(value))
						throw SerializationException("Serialized individual Escalator walking chance is invalid");
					mIndividualEscalatorWalkingChance = value;
				}
				else if (type == "walkSpeedModifier")
				{
					if (mIndividualWalkSpeedModifier)
						throw SerializationException("Serialized Agent contains more than one individual Walk speed modifier");
					auto const value = serializer.readFloat("value");
					if (!agentWalkSpeedModifierRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Walk speed modifier is invalid");
					mIndividualWalkSpeedModifier = value;
				}
				else if (type == "heightModifier")
				{
					if (mIndividualHeightModifier)
						throw SerializationException("Serialized Agent contains more than one individual Height modifier");
					auto const value = serializer.readFloat("value");
					if (!agentHeightModifierRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Height modifier is invalid");
					mIndividualHeightModifier = value;
				}
				else if (type == "stairSpeedModifier")
				{
					if (mIndividualStairSpeedModifier)
						throw SerializationException("Serialized Agent contains more than one individual Stair speed modifier");
					auto const value = serializer.readFloat("value");
					if (!agentStairSpeedModifierRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Stair speed modifier is invalid");
					mIndividualStairSpeedModifier = value;
				}
				else if (type == "ladderSpeedModifier")
				{
					if (mIndividualLadderSpeedModifier)
						throw SerializationException("Serialized Agent contains more than one individual Ladder speed modifier");
					auto const value = serializer.readFloat("value");
					if (!agentLadderSpeedModifierRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Ladder speed modifier is invalid");
					mIndividualLadderSpeedModifier = value;
				}
				else if (type == "interactionAversion")
				{
					if (mIndividualInteractionAversion)
						throw SerializationException("Serialized Agent contains more than one individual Interaction aversion");
					auto const value = serializer.readFloat("value");
					if (!agentInteractionAversionRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Interaction aversion is invalid");
					mIndividualInteractionAversion = value;
				}
				else if (type == "effortAversion")
				{
					if (mIndividualEffortAversion)
						throw SerializationException("Serialized Agent contains more than one individual Effort aversion");
					auto const value = serializer.readFloat("value");
					if (!agentEffortAversionRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Effort aversion is invalid");
					mIndividualEffortAversion = value;
				}
				else if (type == "waitingAversion")
				{
					if (mIndividualWaitingAversion)
						throw SerializationException("Serialized Agent contains more than one individual Waiting aversion");
					auto const value = serializer.readFloat("value");
					if (!agentWaitingAversionRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Waiting aversion is invalid");
					mIndividualWaitingAversion = value;
				}
				else if (type == "crowdAversion")
				{
					if (mIndividualCrowdAversion)
						throw SerializationException("Serialized Agent contains more than one individual Crowd aversion");
					auto const value = serializer.readFloat("value");
					if (!agentCrowdAversionRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Crowd aversion is invalid");
					mIndividualCrowdAversion = value;
				}
				else if (type == "riskAversion")
				{
					if (mIndividualRiskAversion)
						throw SerializationException("Serialized Agent contains more than one individual Risk aversion");
					auto const value = serializer.readFloat("value");
					if (!agentRiskAversionRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Risk aversion is invalid");
					mIndividualRiskAversion = value;
				}
				else if (type == "routeFamiliarity")
				{
					if (mIndividualRouteFamiliarity)
						throw SerializationException("Serialized Agent contains more than one individual Route familiarity");
					auto const value = serializer.readFloat("value");
					if (!agentRouteFamiliarityRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Route familiarity is invalid");
					mIndividualRouteFamiliarity = value;
				}
				else if (type == "routePersistence")
				{
					if (mIndividualRoutePersistence)
						throw SerializationException("Serialized Agent contains more than one individual Route persistence");
					auto const value = serializer.readFloat("value");
					if (!agentRoutePersistenceRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Route persistence is invalid");
					mIndividualRoutePersistence = value;
				}
				else if (type == "minimumRoutePlanningTime")
				{
					if (mIndividualMinimumRoutePlanningTime)
						throw SerializationException("Serialized Agent contains more than one individual Minimum route planning time");
					auto const value = serializer.readFloat("value");
					if (!agentMinimumRoutePlanningTimeRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Minimum route planning time is invalid");
					mIndividualMinimumRoutePlanningTime = value;
				}
				else if (type == "maximumRoutePlanningTime")
				{
					if (mIndividualMaximumRoutePlanningTime)
						throw SerializationException("Serialized Agent contains more than one individual Maximum route planning time");
					auto const value = serializer.readFloat("value");
					if (!agentMaximumRoutePlanningTimeRangeIsValid({ value, value }))
						throw SerializationException("Serialized individual Maximum route planning time is invalid");
					mIndividualMaximumRoutePlanningTime = value;
				}
				else if (type == "permissionAdherence")
				{
					if (mIndividualPermissionAdherence)
						throw SerializationException("Serialized Agent contains more than one individual Permission adherence");
					mIndividualPermissionAdherence = serializer.readBool("value");
				}
				else if (type == "mobilityProfile")
				{
					if (mIndividualMobilityProfile)
						throw SerializationException("Serialized Agent contains more than one individual Mobility profile");
					mIndividualMobilityProfile = deserializeMobilityProfile(serializer);
				}
				else throw SerializationException(format(
					"Unsupported individual Agent property type '{}'", type));
				serializer.endMap();
			}
			serializer.endArray();
		}
		mWalkSpeedModifierSample.reset();
		mHeightModifierSample.reset();
		mStairSpeedModifierSample.reset();
		mLadderSpeedModifierSample.reset();
		mInteractionAversionSample.reset();
		mEffortAversionSample.reset();
		mWaitingAversionSample.reset();
		mCrowdAversionSample.reset();
		mRiskAversionSample.reset();
		mRouteFamiliaritySample.reset();
		mRoutePersistenceSample.reset();
		mMinimumRoutePlanningTimeSample.reset();
		mMaximumRoutePlanningTimeSample.reset();
		if (serializer.hasField("propertySamples"))
		{
			serializer.beginArray("propertySamples");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto const type = serializer.readString("type");
				AgentPropertySample sample;
				std::optional<AgentPropertySample>* destination{ nullptr };
				char const* displayName{ nullptr };
				if (type == "walkSpeedModifier")
				{
					sample.type = SampledAgentPropertyType::WalkSpeedModifier;
					destination = &mWalkSpeedModifierSample;
					displayName = "Walk speed modifier";
				}
				else if (type == "heightModifier")
				{
					sample.type = SampledAgentPropertyType::HeightModifier;
					destination = &mHeightModifierSample;
					displayName = "Height modifier";
				}
				else if (type == "stairSpeedModifier")
				{
					sample.type = SampledAgentPropertyType::StairSpeedModifier;
					destination = &mStairSpeedModifierSample;
					displayName = "Stair speed modifier";
				}
				else if (type == "ladderSpeedModifier")
				{
					sample.type = SampledAgentPropertyType::LadderSpeedModifier;
					destination = &mLadderSpeedModifierSample;
					displayName = "Ladder speed modifier";
				}
				else if (type == "interactionAversion")
				{
					sample.type = SampledAgentPropertyType::InteractionAversion;
					destination = &mInteractionAversionSample;
					displayName = "Interaction aversion";
				}
				else if (type == "effortAversion")
				{
					sample.type = SampledAgentPropertyType::EffortAversion;
					destination = &mEffortAversionSample;
					displayName = "Effort aversion";
				}
				else if (type == "waitingAversion")
				{
					sample.type = SampledAgentPropertyType::WaitingAversion;
					destination = &mWaitingAversionSample;
					displayName = "Waiting aversion";
				}
				else if (type == "crowdAversion")
				{
					sample.type = SampledAgentPropertyType::CrowdAversion;
					destination = &mCrowdAversionSample;
					displayName = "Crowd aversion";
				}
				else if (type == "riskAversion")
				{
					sample.type = SampledAgentPropertyType::RiskAversion;
					destination = &mRiskAversionSample;
					displayName = "Risk aversion";
				}
				else if (type == "routeFamiliarity")
				{
					sample.type = SampledAgentPropertyType::RouteFamiliarity;
					destination = &mRouteFamiliaritySample;
					displayName = "Route familiarity";
				}
				else if (type == "routePersistence")
				{
					sample.type = SampledAgentPropertyType::RoutePersistence;
					destination = &mRoutePersistenceSample;
					displayName = "Route persistence";
				}
				else if (type == "minimumRoutePlanningTime")
				{
					sample.type = SampledAgentPropertyType::MinimumRoutePlanningTime;
					destination = &mMinimumRoutePlanningTimeSample;
					displayName = "Minimum route planning time";
				}
				else if (type == "maximumRoutePlanningTime")
				{
					sample.type = SampledAgentPropertyType::MaximumRoutePlanningTime;
					destination = &mMaximumRoutePlanningTimeSample;
					displayName = "Maximum route planning time";
				}
				else
				{
					throw SerializationException(format(
						"Unsupported sampled Agent property type '{}'", type));
				}
				if (*destination)
					throw SerializationException(format(
						"Serialized Agent contains more than one {} sample", displayName));
				sample.sourceTag = AgentTagId{ serializer.readUint64("sourceTag") };
				sample.propertyRevision = serializer.readUint64("propertyRevision");
				sample.value = serializer.readFloat("value");
				serializer.endMap();
				if (!sample.sourceTag || !mAgentTags.contains(sample.sourceTag))
					throw SerializationException(format(
						"{} sample source must be an assigned Agent tag", displayName));
				if (sample.propertyRevision == 0)
					throw SerializationException(
						"Sampled Agent property revision cannot be zero");
				// Finiteness and range are judged against the referenced property's
				// current revision when the World resolves its registry. A stale
				// value is repairable even when its old draw is no longer meaningful.
				*destination = sample;
			}
			serializer.endArray();
		}
		// Absent means activated: the default for every newly created Agent and
		// for every Agent loaded from a document that predates activation (#118).
		mActive = serializer.readBool("active", true, true);
		mBehaviourAssignment.reset();
		if (serializer.hasField("behaviour"))
		{
			serializer.beginMap("behaviour");
			AgentBehaviourAssignment assignment;
			assignment.behaviour = AgentBehaviourId{ serializer.readUint64("id") };
			assignment.revision = serializer.readUint64("revision");
			if (!assignment.behaviour)
				throw SerializationException("Serialized Agent behaviour ID cannot be zero");
			if (assignment.revision == 0)
				throw SerializationException("Serialized Agent behaviour revision cannot be zero");
			serializer.beginArray("configuration");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto field = serializer.readString("field");
				auto value = deserializeBehaviourValue(serializer, field);
				serializer.endMap();
				if (field.empty())
					throw SerializationException(
						"Serialized Agent behaviour configuration field cannot be blank");
				if (!assignment.configuration.emplace(field, std::move(value)).second)
					throw SerializationException(format(
						"Agent behaviour configuration field '{}' appears twice", field));
			}
			serializer.endArray();
			serializer.endMap();
			mBehaviourAssignment = std::move(assignment);
		}
		serializer.endMap();

		mState = State::Idle;
		mPath = {};
		mPathStartPosition = {};
		mResetPosition = {};
		mFurnitureUse.reset();
		mLocalDepth = mResetLocalDepth = 0;
		mResetDestinationMarker = {};
		mResetAction = "idle";
		mResetPath.reset();
		mResetPathActive = false;
		mEscalatorTraversalSequence = 0;
		mRememberedEscalatorConditions.clear();
		mRememberedDeviceConditions.clear();
		mOccupiedUsablePoint = {};
		mPose = Pose::Standing;
		mRetainedActionPose = false;
		mRoutePlanningSequence = 0;
		mRoutePlanningTotalTicks = 0;
		mRoutePlanningRemainingTicks = 0;
		mTraversalTask.reset();
		syncStandingRouteObservation();
		mQueuedTraversalTask.reset();
		mTraversalLocalGoal.reset();
		return true;
	}

	string const& Agent::getName() const
	{
		return mName;
	}

	optional<DeviceCondition> Agent::rememberedEscalatorCondition(uint32_t sectorIndex) const
	{
		auto found = mRememberedEscalatorConditions.find(sectorIndex);
		return found == mRememberedEscalatorConditions.end() ? nullopt
			: optional<DeviceCondition>{ found->second };
	}

	EffectiveAgentEscalatorWalkingChance Agent::getEffectiveEscalatorWalkingChance() const
	{
		if (mIndividualEscalatorWalkingChance)
			return { *mIndividualEscalatorWalkingChance, {}, true };
		if (mWorld && mWorld->hasAttachedAgentTagRegistry())
			for (auto const tag : mAgentTags)
			{
				auto const* definition = mWorld->getAgentTagRegistry()->lookupAgentTag(tag);
				if (definition)
					if (auto const* property = definition->getEscalatorWalkingChance())
						return { property->value, tag };
			}
		return {};
	}

	EffectiveAgentPermissionAdherence Agent::getEffectivePermissionAdherence() const
	{
		if (mIndividualPermissionAdherence)
			return { *mIndividualPermissionAdherence, {}, 0, true };
		if (mWorld && mWorld->hasAttachedAgentTagRegistry())
			for (auto const tag : mAgentTags)
			{
				auto const* definition = mWorld->getAgentTagRegistry()->lookupAgentTag(tag);
				if (definition)
					if (auto const* property = definition->getPermissionAdherence())
						return { property->value, tag, property->revision, false };
			}
		return {};
	}

	EffectiveAgentColour Agent::getEffectiveColour() const
	{
		EffectiveAgentColour effective;
		if (mIndividualColour)
		{
			effective.value = *mIndividualColour;
			effective.individual = true;
			return effective;
		}
		if (!mWorld || !mWorld->hasAttachedAgentTagRegistry()) return effective;

		auto const& registry = mWorld->getAgentTagRegistry();
		for (auto const tag : mAgentTags)
		{
			auto const* definition = registry->lookupAgentTag(tag);
			if (!definition) continue;
			auto const* colour = definition->getColour();
			if (!colour) continue;
			effective.value = colour->value;
			effective.sourceTag = tag;
			break;
		}
		return effective;
	}

	EffectiveAgentWalkSpeedModifier Agent::getEffectiveWalkSpeedModifier() const
	{
		EffectiveAgentWalkSpeedModifier effective;
		if (mIndividualWalkSpeedModifier)
		{
			effective.value = *mIndividualWalkSpeedModifier;
			effective.individual = true;
			return effective;
		}
		if (!mWalkSpeedModifierSample) return effective;
		effective.value = mWalkSpeedModifierSample->value;
		effective.sourceTag = mWalkSpeedModifierSample->sourceTag;
		effective.propertyRevision = mWalkSpeedModifierSample->propertyRevision;
		return effective;
	}

	EffectiveAgentHeightModifier Agent::getEffectiveHeightModifier() const
	{
		EffectiveAgentHeightModifier effective;
		if (mIndividualHeightModifier)
		{
			effective.value = *mIndividualHeightModifier;
			effective.individual = true;
			return effective;
		}
		if (!mHeightModifierSample) return effective;
		effective.value = mHeightModifierSample->value;
		effective.sourceTag = mHeightModifierSample->sourceTag;
		effective.propertyRevision = mHeightModifierSample->propertyRevision;
		return effective;
	}

	EffectiveAgentStairSpeedModifier Agent::getEffectiveStairSpeedModifier() const
	{
		EffectiveAgentStairSpeedModifier effective;
		if (mIndividualStairSpeedModifier)
		{
			effective.value = *mIndividualStairSpeedModifier;
			effective.individual = true;
			return effective;
		}
		if (!mStairSpeedModifierSample) return effective;
		effective.value = mStairSpeedModifierSample->value;
		effective.sourceTag = mStairSpeedModifierSample->sourceTag;
		effective.propertyRevision = mStairSpeedModifierSample->propertyRevision;
		return effective;
	}

	EffectiveAgentLadderSpeedModifier Agent::getEffectiveLadderSpeedModifier() const
	{
		EffectiveAgentLadderSpeedModifier effective;
		if (mIndividualLadderSpeedModifier)
		{
			effective.value = *mIndividualLadderSpeedModifier;
			effective.individual = true;
			return effective;
		}
		if (!mLadderSpeedModifierSample) return effective;
		effective.value = mLadderSpeedModifierSample->value;
		effective.sourceTag = mLadderSpeedModifierSample->sourceTag;
		effective.propertyRevision = mLadderSpeedModifierSample->propertyRevision;
		return effective;
	}

	EffectiveAgentInteractionAversion Agent::getEffectiveInteractionAversion() const
	{
		EffectiveAgentInteractionAversion effective;
		if (mIndividualInteractionAversion)
		{
			effective.value = *mIndividualInteractionAversion;
			effective.individual = true;
			return effective;
		}
		if (!mInteractionAversionSample) return effective;
		effective.value = mInteractionAversionSample->value;
		effective.sourceTag = mInteractionAversionSample->sourceTag;
		effective.propertyRevision = mInteractionAversionSample->propertyRevision;
		return effective;
	}

	EffectiveAgentEffortAversion Agent::getEffectiveEffortAversion() const
	{
		EffectiveAgentEffortAversion effective;
		if (mIndividualEffortAversion)
		{
			effective.value = *mIndividualEffortAversion;
			effective.individual = true;
			return effective;
		}
		if (!mEffortAversionSample) return effective;
		effective.value = mEffortAversionSample->value;
		effective.sourceTag = mEffortAversionSample->sourceTag;
		effective.propertyRevision = mEffortAversionSample->propertyRevision;
		return effective;
	}

	EffectiveAgentWaitingAversion Agent::getEffectiveWaitingAversion() const
	{
		EffectiveAgentWaitingAversion effective;
		if (mIndividualWaitingAversion)
		{
			effective.value = *mIndividualWaitingAversion;
			effective.individual = true;
			return effective;
		}
		if (!mWaitingAversionSample) return effective;
		effective.value = mWaitingAversionSample->value;
		effective.sourceTag = mWaitingAversionSample->sourceTag;
		effective.propertyRevision = mWaitingAversionSample->propertyRevision;
		return effective;
	}

	EffectiveAgentCrowdAversion Agent::getEffectiveCrowdAversion() const
	{
		EffectiveAgentCrowdAversion effective;
		if (mIndividualCrowdAversion)
		{
			effective.value = *mIndividualCrowdAversion;
			effective.individual = true;
			return effective;
		}
		if (!mCrowdAversionSample) return effective;
		effective.value = mCrowdAversionSample->value;
		effective.sourceTag = mCrowdAversionSample->sourceTag;
		effective.propertyRevision = mCrowdAversionSample->propertyRevision;
		return effective;
	}

	EffectiveAgentRiskAversion Agent::getEffectiveRiskAversion() const
	{
		EffectiveAgentRiskAversion effective;
		if (mIndividualRiskAversion)
		{
			effective.value = *mIndividualRiskAversion;
			effective.individual = true;
			return effective;
		}
		if (!mRiskAversionSample) return effective;
		effective.value = mRiskAversionSample->value;
		effective.sourceTag = mRiskAversionSample->sourceTag;
		effective.propertyRevision = mRiskAversionSample->propertyRevision;
		return effective;
	}

	EffectiveAgentRouteFamiliarity Agent::getEffectiveRouteFamiliarity() const
	{
		EffectiveAgentRouteFamiliarity effective;
		if (mIndividualRouteFamiliarity)
		{
			effective.value = *mIndividualRouteFamiliarity;
			effective.individual = true;
			return effective;
		}
		if (!mRouteFamiliaritySample) return effective;
		effective.value = mRouteFamiliaritySample->value;
		effective.sourceTag = mRouteFamiliaritySample->sourceTag;
		effective.propertyRevision = mRouteFamiliaritySample->propertyRevision;
		return effective;
	}

	int routeAgentLocalDepth(Agent const* agent)
	{
		return agent ? agent->getLocalDepth() : 0;
	}

	uint64_t Agent::getRouteJourneyIdentity(Vertex const* destination) const
	{
		if (!destination) return mRouteJourneySequence;
		return mRouteJourneyDestinationVertexId == destination->getId()
			? mRouteJourneySequence : mRouteJourneySequence + 1;
	}

	EffectiveAgentRoutePersistence Agent::getEffectiveRoutePersistence() const
	{
		EffectiveAgentRoutePersistence effective;
		if (mIndividualRoutePersistence)
		{
			effective.value = *mIndividualRoutePersistence;
			effective.individual = true;
			return effective;
		}
		if (!mRoutePersistenceSample) return effective;
		effective.value = mRoutePersistenceSample->value;
		effective.sourceTag = mRoutePersistenceSample->sourceTag;
		effective.propertyRevision = mRoutePersistenceSample->propertyRevision;
		return effective;
	}

	EffectiveAgentMinimumRoutePlanningTime Agent::getEffectiveMinimumRoutePlanningTime() const
	{
		EffectiveAgentMinimumRoutePlanningTime effective;
		if (mIndividualMinimumRoutePlanningTime)
		{
			effective.value = *mIndividualMinimumRoutePlanningTime;
			effective.individual = true;
			return effective;
		}
		if (!mMinimumRoutePlanningTimeSample) return effective;
		effective.value = mMinimumRoutePlanningTimeSample->value;
		effective.sourceTag = mMinimumRoutePlanningTimeSample->sourceTag;
		effective.propertyRevision = mMinimumRoutePlanningTimeSample->propertyRevision;
		return effective;
	}

	EffectiveAgentMaximumRoutePlanningTime Agent::getEffectiveMaximumRoutePlanningTime() const
	{
		EffectiveAgentMaximumRoutePlanningTime effective;
		if (mIndividualMaximumRoutePlanningTime)
		{
			effective.value = *mIndividualMaximumRoutePlanningTime;
			effective.individual = true;
		}
		else if (mMaximumRoutePlanningTimeSample)
		{
			effective.value = mMaximumRoutePlanningTimeSample->value;
			effective.sourceTag = mMaximumRoutePlanningTimeSample->sourceTag;
			effective.propertyRevision = mMaximumRoutePlanningTimeSample->propertyRevision;
		}
		// Present a usable interval without changing either authored value or sample.
		effective.value = std::max(effective.value, getEffectiveMinimumRoutePlanningTime().value);
		return effective;
	}

	EffectiveAgentMobilityProfile Agent::getEffectiveMobilityProfile() const
	{
		EffectiveAgentMobilityProfile effective;
		effective.value = mScriptDefaultMobilityProfile;
		if (mIndividualMobilityProfile)
		{
			effective.value = *mIndividualMobilityProfile;
			effective.individual = true;
			return effective;
		}
		if (!mWorld || !mWorld->hasAttachedAgentTagRegistry()) return effective;
		auto const& registry = mWorld->getAgentTagRegistry();
		for (auto const tag : mAgentTags)
		{
			auto const* definition = registry->lookupAgentTag(tag);
			if (!definition) continue;
			auto const* profile = definition->getMobilityProfile();
			if (!profile) continue;
			effective.value = profile->value;
			effective.sourceTag = tag;
			effective.propertyRevision = profile->revision;
			break;
		}
		return effective;
	}

	Agent::State Agent::getState() const
	{
		return mState;
	}

	bool Agent::isInQueue() const
	{
		return mWorld && mWorld->isAgentInQueue(mWorld->getAgentId(this));
	}

	string Agent::getDescription() const
	{
		return format("Agent: {}", getName());
	}

	Sector const* Agent::getSector() const
	{
		return mPosition.sector();
	}

	Vector2 const& Agent::getLocalPosition() const
	{
		return mPosition.local();
	}

	Vector2 Agent::getGlobalPosition() const
	{
		return mPosition.global();
	}

	float Agent::getWidth() const
	{
		return getPhysicalBaseline().width;
	}

	float Agent::getStandingHeight() const
	{
		return getPhysicalBaseline().standingHeight * getEffectiveHeightModifier().value;
	}

	float Agent::getTraversalDoorClearanceExtent(bool beginningMovement) const
	{
		return beginningMovement || mFurnitureUse ? getStandingHeight() : getDoorClearanceExtent();
	}

	float Agent::getSupportElevation() const
	{
		float support = 0.f;
		if (mWorld && mOccupiedUsablePoint)
		{
			if (auto instance = mWorld->furnitureForMarker(mOccupiedUsablePoint))
			{
				auto catalogue = mWorld->furnitureCatalogue();
				auto definition = catalogue ? catalogue->definition(instance->definitionKey) : nullptr;
				if (definition)
					for (auto const& destination : instance->destinations)
						if (destination.marker == mOccupiedUsablePoint)
							for (auto const& point : definition->usablePoints)
								if (point.key == destination.key) support = point.supportElevation;
			}
		}
		return support;
	}

	float Agent::getDoorClearanceExtent() const
	{
		// Lying rotates the upright body by 90 degrees: width becomes height.
		return getSupportElevation() + (mPose == Pose::Lying ? getWidth() : getHeight());
	}

	float Agent::getTraversalCrawlingDoorClearanceExtent(bool beginningMovement) const
	{
		auto const envelope = getPoseEnvelope(Pose::Crawling);
		if (!envelope) return std::numeric_limits<float>::infinity();
		auto const crawlingHeight = envelope->y;
		return (beginningMovement || mFurnitureUse)
			? crawlingHeight
			: getSupportElevation() + crawlingHeight;
	}

	float Agent::getHeight() const
	{
		return getStandingHeight() * getPoseHeightScale();
	}

	Pose Agent::requiredPoseFor(Sector const& sector) const
	{
		if (!sector.isRoom()) return Pose::Standing;
		auto const standing = getStandingHeight();
		auto const ceiling = sector.getEffectiveTopLevelHeight();
		if (standing <= ceiling + Door::ClearanceTolerance) return Pose::Standing;
		auto const& physical = getPhysicalBaseline();
		if (supportsPose(Pose::Crouching)
			&& standing * physical.poses.at(Pose::Crouching) <= ceiling + Door::ClearanceTolerance)
			return Pose::Crouching;
		// Preserve existing Human selection in this declaration slice. Full
		// context selection and no-fit refusal are delivered by #522.
		return supportsPose(Pose::Crawling) ? Pose::Crawling : Pose::Standing;
	}

	void Agent::syncPoseToSector()
	{
		if (mFurnitureUse || mRetainedActionPose) return; // Active use or a retained Action pose owns the pose.
		auto const* sector = getSector();
		// A same-layer Location walk commits its sector transfer at the far vertex,
		// so the logical sector can lag the Agent's body while it crosses an open
		// wall. Derive the pose from the sector under the body instead, so a low
		// Room stops ducking/crawling as soon as the Agent physically stands in a
		// taller one.
		if (mWorld && sector)
		{
			auto const global = getGlobalPosition();
			auto const physical = mWorld->getSectorAtPosition(sector->getLayerIndex(), global.x, global.y);
			if (physical && isLocationLike(physical->getType())) sector = physical.get();
		}
		mPose = sector ? requiredPoseFor(*sector) : Pose::Standing;
	}

	Shape Agent::getBounds() const
	{
		auto pos = getGlobalPosition();

		auto agentWidth2 = getWidth() * 0.5f;
		auto agentHeight = getHeight();

		auto pos0 = pos;
		pos0.x -= agentWidth2;

		auto pos1 = pos;
		pos1.x += agentWidth2;
		pos1.y += agentHeight;

		return { pos0, pos1 - pos0 };
	}

	float Agent::getWalkSpeed() const
	{
		return getPhysicalBaseline().walkSpeed
			* getEffectiveWalkSpeedModifier().value;
	}

	float Agent::getClimbSpeed() const
	{
		return getPhysicalBaseline().climbSpeed
			* getEffectiveLadderSpeedModifier().value;
	}

	float Agent::getStationaryStairSpeed(bool ascending) const
	{
		auto const& physical = getPhysicalBaseline();
		return (ascending ? physical.stairAscentSpeed : physical.stairDescentSpeed)
			* getEffectiveStairSpeedModifier().value;
	}

	float Agent::estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const
	{
		return mWorld ? mWorld->estimateTraversalDelay(resource, sourceSector) : 0.0f;
	}

	optional<DeviceCondition> Agent::rememberedDeviceCondition(TraversalResourceId resource) const
	{
		auto found = mRememberedDeviceConditions.find(resource);
		return found == mRememberedDeviceConditions.end() ? nullopt
			: optional<DeviceCondition>{ found->second };
	}

	float Agent::observeAccessZoneDensity(TraversalResourceId resource,
		SectorId sourceSector) const
	{
		return mWorld ? mWorld->observeAccessZoneDensity(resource, sourceSector) : 0.0f;
	}

	optional<ShuttleRouteAccessObservation> Agent::observeShuttleAccess(
		TraversalResourceId resource, Vector2 const& endpoint, bool includeLocalQueue) const
	{
		return mWorld ? mWorld->observeShuttleAccess(resource, endpoint, includeLocalQueue) : nullopt;
	}

	optional<LiftRouteAccessObservation> Agent::observeLiftAccess(
		TraversalResourceId resource, Vector2 const& sourceEndpoint, bool includeLocalQueue) const
	{
		return mWorld ? mWorld->observeLiftAccess(resource, sourceEndpoint, includeLocalQueue) : nullopt;
	}

	uint32_t Agent::countObservedStandingEscalatorAgents(Edge const* edge) const
	{
		return mWorld ? mWorld->countStandingAgentsOnEscalator(this, edge) : 0;
	}

	uint32_t Agent::getFlags() const
	{
		return mFlags;
	}

	bool Agent::flagsSet(uint32_t flags) const
	{
		return (mFlags & flags) != 0;
	}

	void Agent::setFlags(uint32_t flags)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		auto const updated = mFlags | flags;
		if (updated != mFlags)
		{
			mFlags = updated;
			modify();
		}
	}

	void Agent::unsetFlags(uint32_t flags)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		auto const updated = mFlags & ~flags;
		if (updated != mFlags)
		{
			mFlags = updated;
			modify();
		}
	}

	void Agent::setActive(bool active)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (active != mActive)
		{
			mActive = active;
			syncStandingRouteObservation();
			modify();
		}
	}

	void Agent::setPosition(SectorPosition pos, bool authored)
	{
		if (authored && mWorld && pos.sector())
			mWorld->validateAgentLocationPlacement(*pos.sector(), *this);
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		bool const changedSector = pos.sector() != mPosition.sector();
		if (!authored && mWorld
			&& (changedSector || pos.global().distanceTo(mPosition.global()) > 0.f))
		{
			mWorld->finishFurnitureUse(mWorld->getAgentId(this));
			mOccupiedUsablePoint = {};
		}
		if (changedSector) mLocalDepth = 0;
		mPosition = pos;
		// An admitted threshold crossing keeps its crawling Pose until the commit
		// transfers sector membership; every other movement derives the Pose from
		// the Sector's effective ceiling. This keeps a one-cell-high low Room's
		// occupant ducked/crawled while it walks within the Room, and stands it
		// back up the moment it enters a normal Sector.
		if (mTraversalTask && mTraversalTask->crawling)
			mPose = Pose::Crawling;
		else
			syncPoseToSector();
		if (authored)
		{
			mResetPosition = pos;
			mLocalDepth = mResetLocalDepth = 0;
			modify();
		}
	}

	void Agent::attachToWorld(World* world)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mWorld && mWorld != world)
		{
			throw Exception("Agent is already attached to another World");
		}
		mWorld = world;
	}

	shared_ptr<Path> const& Agent::getPath() const
	{
		return mPath.path;
	}

	uint32_t Agent::getPathTargetNodeIndex() const
	{
		return mPath.targetNode;
	}

	bool Agent::hasActiveLocomotionTask() const
	{
		return mTraversalTask.has_value();
	}

	TraversalRequestId Agent::getTraversalRequestId() const
	{
		return mTraversalTask ? mTraversalTask->request : TraversalRequestId{};
	}

	TraversalPermitId Agent::getTraversalPermitId() const
	{
		return mTraversalTask ? mTraversalTask->permit : TraversalPermitId{};
	}

	Agent::EdgeTraversalData Agent::getEdgeTraversalData() const
	{
		return {
			mPath.path->nodes[mPath.targetNode].targetVertex->getSector(),
			mPath.path->nodes[mPath.targetNode + 1].targetVertex->getSector(),
			mPath.path->nodes[mPath.targetNode + 1].targetVertex
		};
	}

	PathNode& Agent::getTargetPathNode() const
	{
		return mPath.path->nodes[mPath.targetNode];
	}

	bool Agent::atEndOfPath() const
	{
		return mPath.atEnd();
	}

	void Agent::setPath(shared_ptr<Path> path, bool startPathing)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mWorld
			&& mWorld->agentBehaviourOwnsMovement(mWorld->getAgentId(this))) return;
		if (!getSector() || getSector()->getType() != SectorType::Chamber)
		{
			mResetPosition = mPosition;
			mResetLocalDepth = mLocalDepth;
		}
		mResetPath = path;
		mResetDestinationMarker = {};
		mResetAction = "idle";
		if (path && !path->nodes.empty())
			if (auto marker = dynamic_pointer_cast<Marker>(path->nodes.back().targetVertex->getObject()))
				mResetDestinationMarker = marker->getId();
		mResetPathActive = startPathing;
		assignPath(std::move(path), startPathing, true);
	}

	void Agent::assignPath(shared_ptr<Path> path, bool startPathing, bool markModified)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		auto const destination = path && !path->nodes.empty()
			? path->nodes.back().targetVertex.get() : nullptr;
		auto acceptJourneyIdentity = [&]
		{
			if (destination && destination->getId() != mRouteJourneyDestinationVertexId)
			{
				++mRouteJourneySequence;
				mRouteJourneyDestinationVertexId = destination->getId();
			}
		};
		if (mWorld && getSector() && getSector()->getType() == SectorType::Chamber)
		{
			// Destination intent is independent of the already admitted journey.
			// Capture a replacement destination for planning after the forward exit,
			// without cancelling its live crossing, queue ticket, or occupancy.
			auto admitted = mPath.path;
			mPath.path = std::move(path);
			auto const id = mWorld->getAgentId(this);
			if (mPath.path)
			{
				// Explicit replacement supersedes any previously deferred intent.
				mWorld->mMovementGoals.erase(id);
				mWorld->replanAgentAfterAuthorizationRefusal(id);
			}
			else mWorld->cancelAgentMovement(id);
			mPath.path = std::move(admitted);
			if (markModified) modify();
			return;
		}
		// An onboard replacement remains the same transport journey. Retarget the
		// live ride request and stop-request ownership instead of cancelling into a
		// needless exit/reboard cycle.
		uint32_t replacementSource = 0;
		if (startPathing && path && mTraversalTask && !mTraversalTask->permit && mWorld
			&& mWorld->replaceOnboardLiftDestination(*this, path, replacementSource))
		{
			acceptJourneyIdentity();
			mPath.path = std::move(path);
			mPath.targetNode = replacementSource;
			mTraversalTask->edge = mPath.path->nodes[replacementSource + 1].edge;
			mTraversalTask->sourceVertex = mPath.path->nodes[replacementSource].targetVertex;
			mTraversalTask->destinationVertex = mPath.path->nodes[replacementSource + 1].targetVertex;
			mTraversalTask->permit = {};
			mState = State::WaitingForTraversal;
			if (markModified) modify();
			return;
		}

		// A granted permit freezes route intent until it is committed or expires.
		// Silently retaining the current path is safer than invalidating a crossing.
		if (mTraversalTask && mTraversalTask->permit)
		{
			return;
		}

		// Replacing only the suffix after the same immediate resource is a compatible
		// replan. Update the locomotion endpoints while retaining request/ticket age.
		if (startPathing && path && mTraversalTask && mWorld)
		{
			auto request = mWorld->lookupTraversalRequest(mTraversalTask->request);
			if (request && request.entity->getState() == TraversalRequestState::Pending)
			{
				for (uint32_t i = 0; i + 1 < path->nodes.size(); ++i)
				{
					auto const& source = path->nodes[i].targetVertex;
					auto const& destination = path->nodes[i + 1].targetVertex;
					auto const& edge = path->nodes[i + 1].edge;
					if (!source || !destination || !edge) continue;
					auto sourceSector = SectorId{ (uint64_t)source->getSector()->getIndex() + 1 };
					auto destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
					if (sourceSector == request.entity->getSourceSector()
						&& destinationSector == request.entity->getDestinationSector()
						&& edge->getTraversalResourceId() == request.entity->getResource()
						&& edge->getType() == request.entity->getEdgeType())
					{
						acceptJourneyIdentity();
						mPath.path = std::move(path);
						mPath.targetNode = i;
						mTraversalTask->edge = edge;
						mTraversalTask->sourceVertex = source;
						mTraversalTask->destinationVertex = destination;
						mState = State::WaitingForTraversal;
						if (markModified) modify();
						return;
					}
				}
			}
		}

		mPathStartPosition = mPosition.sector() ? getGlobalPosition() : Vector2::ZERO;
		clearRuntimePath();
		acceptJourneyIdentity();
		mPath.path = std::move(path);
		mPath.targetNode = 0;
		if (markModified) modify();

		if (startPathing)
		{
			startPathingInternal();
		}
	}

	void Agent::clearRuntimePath()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		cancelTraversal();
		mEarlyDoorPressResource = {};
		mEarlyDoorPressInteraction = {};
		mEarlyDoorPressAttempted = false;
		mEarlyQueueApproachDirectionX = 0;
		mPath.path = nullptr;
		mPath.targetNode = 0;
		mRoutePlanningTotalTicks = mRoutePlanningRemainingTicks = 0;
		mState = State::Idle;
	}

	void Agent::clearPath()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mWorld
			&& mWorld->agentBehaviourOwnsMovement(mWorld->getAgentId(this))) return;
		clearRuntimePath();
		if (!getSector() || getSector()->getType() != SectorType::Chamber)
		{
			mResetPosition = mPosition;
			mResetLocalDepth = mLocalDepth;
		}
		mResetDestinationMarker = {};
		mResetAction = "idle";
		mResetPath.reset();
		mResetPathActive = false;
		modify();
	}

	void Agent::startPathingInternal()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (!mPath.path || mPath.path->nodes.empty())
		{
			startIdling();
			return;
		}

		// Definition-owned use survives planning. Generic one-shot Action poses
		// retain the existing Path-start reset; only active use has a lifecycle.
		if (!mFurnitureUse)
		{
			mOccupiedUsablePoint = {};
			mPose = Pose::Standing;
			mRetainedActionPose = false;
		}
		cancelTraversal();
		mEarlyQueueApproachDirectionX = 0;
		mState = State::MovingToVertex;
		auto marker = std::dynamic_pointer_cast<Marker>(mPath.path->nodes.back().targetVertex->getObject());
		auto occupant = mWorld && marker ? mWorld->usablePointOccupant(marker->getId()) : AgentId{};
		if (mWorld && ((occupant && occupant != mWorld->getAgentId(this))
			|| !mWorld->canAgentAccessLocation(*mPath.path->nodes.back().targetVertex->getSector(), *this)))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started pathing"));
	}

	void Agent::startPathing()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mWorld
			&& mWorld->agentBehaviourOwnsMovement(mWorld->getAgentId(this))) return;
		startPathingInternal();
	}

	void Agent::pausePathing()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mWorld
			&& mWorld->agentBehaviourOwnsMovement(mWorld->getAgentId(this))) return;
		cancelTraversal();
		mState = State::Idle;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Paused pathing"));
	}

	void Agent::performDestinationAction()
	{
		// The coordinator invokes exactly the selected Marker Action after
		// arrival. Movement and intermediate passage never infer Furniture use.
	}

	bool Agent::nextPathNode()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		++mPath.targetNode;

		if (!mPath.path || mPath.targetNode >= mPath.path->nodes.size() - 1)
		{
			performDestinationAction();
			if (mState == State::RoutePlanning) return true;
			mState = State::Idle;
			mPath.path = nullptr;
			mPath.targetNode = 0;
			mRouteJourneyDestinationVertexId = 0;
			addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started idling"));
			return true;
		}

		mState = State::WaitingForTraversal;
		return false;
	}

	void Agent::startIdling()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		cancelTraversal();
		mEarlyDoorPressResource = {};
		mEarlyDoorPressInteraction = {};
		mEarlyDoorPressAttempted = false;
		mEarlyQueueApproachDirectionX = 0;
		mState = State::Idle;
		mPath.path = nullptr;
		mPath.targetNode = 0;
		mRouteJourneyDestinationVertexId = 0;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started idling"));
	}

	bool Agent::moveToPosition(Vector2 const& pos, float frameTime, float speed)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		auto agentPos = getGlobalPosition();
		auto posDist = agentPos.distanceTo(pos);
		auto moveDist = speed * frameTime;
		auto moveDelta = pos - agentPos;
		auto reachedPos = moveDist >= posDist;
		auto moveAmt = reachedPos ? moveDelta : moveDelta.normalisedCopy() * moveDist;

		setPosition({ mPosition.sector(), mPosition.local() + moveAmt }, false);

		return reachedPos;
	}

	int Agent::chooseVertexOffset(int /* dim */, pair<float, uint32_t> const* offsets, uint32_t /* numOffsets */)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		int chosen = 0;

		if (chosen < 0)
		{
			// Replan route?
			throw NotImplementedException("Agent::chooseVertexOffset() no offset chosen");
		}

		return (int)offsets[chosen].second;
	}

	uint32_t Agent::getSkippablePathTarget(uint32_t vertexA) const
	{
		if (!mPath.path || !getSector() || vertexA + 1 >= mPath.path->nodes.size()) return vertexA;
		if (getSector()->getType() == SectorType::Chamber
			|| (mPath.path->nodes[vertexA].targetVertex
				&& mPath.path->nodes[vertexA].targetVertex->getSector()->getType() == SectorType::Chamber)) return vertexA;
		auto const& nodeA = mPath.path->nodes[vertexA];
		if (!nodeA.targetVertex
			|| nodeA.targetVertex->getSubType() == VertexSubType::Interactable) return vertexA;
		auto const positionA = nodeA.targetVertex->getPosition();

		auto requiresActionAtSource = [&](shared_ptr<const Edge> const& edge)
		{
			if (!edge) return true;
			if (edge->getType() == EdgeType::Location) return false;
			if (edge->getType() == EdgeType::LiftMount && mWorld)
			{
				auto resource = mWorld->lookupTraversalResource(edge->getTraversalResourceId());
				// An open platform's mount edge is a topology-only exit handoff. Calls and
				// destination selection occur on its Lift edges instead.
				return !resource || !resource.entity->isOpenPlatformLift();
			}
			return true;
		};

		// Coincident nodes are topology-only. Look through them to obtain the next
		// physical vertex, but stop at an edge that requires an action at the current
		// waypoint (for example, operating a lift call control).
		auto vertexB = vertexA + 1;
		while (vertexB < mPath.path->nodes.size())
		{
			auto const& node = mPath.path->nodes[vertexB];
			if (!node.targetVertex || requiresActionAtSource(node.edge)
				|| node.edge->getLocalDepth() != (nodeA.edge ? nodeA.edge->getLocalDepth() : mLocalDepth)) return vertexA;
			if (node.targetVertex->getPosition().distanceTo(positionA) > 0.001f) break;
			++vertexB;
		}
		if (vertexB >= mPath.path->nodes.size()) return vertexA;

		auto const& nodeB = mPath.path->nodes[vertexB];
		auto const positionB = nodeB.targetVertex->getPosition();
		auto const agentPosition = getGlobalPosition();
		auto const layer = getSector()->getLayerIndex();
		// A skipped waypoint must not carry a stale Path past the entry gate of
		// another, currently unauthorized Location.
		if ((mWorld && nodeA.targetVertex->getSector() != nodeB.targetVertex->getSector()
				&& !mWorld->canAgentAccessLocation(*nodeB.targetVertex->getSector(), *this))
			|| nodeA.targetVertex->getSector()->getLayerIndex() != layer
			|| nodeB.targetVertex->getSector()->getLayerIndex() != layer
			|| abs(positionA.y - agentPosition.y) > 0.001f
			|| abs(positionA.y - positionB.y) > 0.001f
			|| abs(positionA.x - positionB.x) <= 0.001f) return vertexA;
		auto const sideA = positionA.x - agentPosition.x;
		auto const sideB = positionB.x - agentPosition.x;
		return sideA * sideB < -0.000001f ? vertexB : vertexA;
	}

	void Agent::moveToVertex(float frameTime)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (!mPath.path || mPath.targetNode >= mPath.path->nodes.size())
		{
			startIdling();
			return;
		}

		mPath.targetNode = getSkippablePathTarget(mPath.targetNode);
		auto const& targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();
		if (mWorld && mPath.targetNode + 1 < mPath.path->nodes.size()
			&& mWorld->stopForAvailableQueuePosition(*this,
				mPath.path->nodes[mPath.targetNode + 1].edge, targetPos,
				getWalkSpeed() * frameTime))
		{
			mState = State::WaitingForTraversal;
			return;
		}
		// Ticket #98: a plain Door's request-creation gate is the crossing-width
		// band. An Agent whose next edge crosses a Door enters the traversal
		// flow as soon as it stands within the crossing width at the threshold
		// row, instead of converging on the exact vertex. The early stop above
		// keeps precedence for contended doors - it claims the queue spot on
		// the way, so the two never double-start the flow; request creation
		// itself still happens once, in the next intent-collection phase.
		if (mWorld && mPath.targetNode + 1 < mPath.path->nodes.size()
			&& mWorld->isAtDoorCrossingArrival(*this,
				mPath.path->nodes[mPath.targetNode + 1].edge, targetPos))
		{
			mState = State::WaitingForTraversal;
			return;
		}
		if (!moveToPosition(targetPos, frameTime, getWalkSpeed()))
		{
			return;
		}

		if (mPath.targetNode + 1 >= mPath.path->nodes.size())
		{
			performDestinationAction();
			startIdling();
			return;
		}

		// The Agent has reached the source endpoint. Request creation is deferred
		// to the next intent-collection phase rather than changing membership here.
		mState = State::WaitingForTraversal;
	}

	void Agent::collectTraversalIntent()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mState != State::WaitingForTraversal || mTraversalTask || !mWorld
			|| !mPath.path || mPath.targetNode + 1 >= mPath.path->nodes.size())
		{
			return;
		}

		auto const& sourceNode = mPath.path->nodes[mPath.targetNode];
		auto const& destinationNode = mPath.path->nodes[mPath.targetNode + 1];
		if (!destinationNode.edge || !sourceNode.targetVertex || !destinationNode.targetVertex)
		{
			return;
		}

		TraversalTask task;
		task.edge = destinationNode.edge;
		task.sourceVertex = sourceNode.targetVertex;
		task.destinationVertex = destinationNode.targetVertex;
		task.request = mWorld->createTraversalRequest(*this, task.edge,
			task.sourceVertex, task.destinationVertex);
		mEarlyQueueApproachDirectionX = 0;
		mTraversalTask = std::move(task);
	}

	void Agent::allocateTraversal()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mState != State::WaitingForTraversal || !mTraversalTask || !mWorld)
		{
			return;
		}

		// Recheck before adopting even an already granted permit, never during crossing.
		// An onboard Lift or Shuttle passenger's committed exit is exempt: it must
		// finish its journey even if a live change made the opening or envelope
		// impossible.
		if (mTraversalTask->edge->getType() == EdgeType::Door
			&& getSector()->getType() != SectorType::Lift
			&& getSector()->getType() != SectorType::Shuttle
			&& !static_cast<DoorEdge const&>(*mTraversalTask->edge).getDoor()->admitsAgentTraversal(
				*this, getGlobalPosition().y))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}
		if (auto bulkhead = dynamic_cast<BulkheadDoorEdge const*>(mTraversalTask->edge.get());
			bulkhead && (bulkhead->isStandalone() || (bulkhead->getDoor()->isAirlockOwned()
				&& getSector()->getType() != SectorType::Airlock)
				|| (bulkhead->getDoor()->isSecurityScannerOwned()
					&& getSector()->getType() != SectorType::Chamber))
			&& !bulkhead->getDoor()->admitsAgentTraversal(*this, getGlobalPosition().y))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}
		auto requestLookup = mWorld->lookupTraversalRequest(mTraversalTask->request);
		if (!requestLookup) return;
		// A selected (or reinstalled stale) Path is intent, not authorization.
		// Recheck at entry start, even if another queue allocation already granted
		// this request. Underway entries never return to this allocation state.
		auto destinationSector = mTraversalTask->destinationVertex->getSector();
		if (destinationSector.get() != getSector()
			&& getSector()->getType() != SectorType::Airlock
			&& getSector()->getType() != SectorType::Chamber
			&& !mWorld->canAgentAccessLocation(*destinationSector, *this))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}
		if (destinationSector.get() != getSector() && destinationSector->getType() == SectorType::Airlock
			&& (agentForbidsEdge(this, *mTraversalTask->edge, TraversalKind::Door)
				|| !mWorld->canAgentEnterAirlock(mTraversalTask->edge->getTraversalResourceId(),
					SectorId{ static_cast<uint64_t>(getSector()->getIndex()) + 1 }, mWorld->getAgentId(this), true)))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}
		// A Path may predate an authorization change. Repeat the manual Door
		// opening check at the threshold, while still allowing passage through a
		// Door which is locally open and needs no operation.
		if (mTraversalTask->edge->getType() == EdgeType::Door
			&& !mWorld->canAgentTraverseManualDoorNow(
				mTraversalTask->edge->getTraversalResourceId(), mWorld->getAgentId(this)))
		{
			mWorld->replanAgentAfterAuthorizationRefusal(mWorld->getAgentId(this));
			return;
		}
		if (requestLookup.entity->getState() == TraversalRequestState::Pending)
		{
			mWorld->allocateTraversalRequest(mTraversalTask->request,
				mTraversalTask->edge, mTraversalTask->destinationVertex);
			// Authorization refusal can release this task and enter Route planning.
			if (!mTraversalTask) return;
			requestLookup = mWorld->lookupTraversalRequest(mTraversalTask->request);
		}
		// A resource allocation initiated while processing another agent may have
		// granted this request earlier in the phase. The owner adopts that permit
		// on its next allocation turn rather than remaining a ghost lane owner.
		if (requestLookup && requestLookup.entity->getState() == TraversalRequestState::Granted)
		{
			mTraversalTask->permit = requestLookup.entity->getPermit();
			auto vertexA = mPath.targetNode + 1;
			auto resource = mWorld->lookupTraversalResource(requestLookup.entity->getResource());
			if (resource && resource.entity->isOpenPlatformLift()
				&& requestLookup.entity->getEdgeType() == EdgeType::Lift)
			{
				// Adjacent Platform Lift edges form one transport journey. LOOK may pass
				// intermediate floors, so the traversal endpoint is the final contiguous
				// Lift vertex rather than the first graph segment.
				while (vertexA + 1 < mPath.path->nodes.size()
					&& mPath.path->nodes[vertexA + 1].edge
					&& mPath.path->nodes[vertexA + 1].edge->getType() == EdgeType::Lift)
					++vertexA;
				mTraversalTask->destinationVertex = mPath.path->nodes[vertexA].targetVertex;
				mTraversalTask->pathNodesConsumed = vertexA - mPath.targetNode;
			}
			auto const bulkhead = dynamic_cast<BulkheadDoorEdge const*>(mTraversalTask->edge.get());
			// The far-side boundary must be reached physically before standing up.
			auto const directTarget = bulkhead && (bulkhead->isStandalone() || bulkhead->getDoor()->isAirlockOwned()
				|| bulkhead->getDoor()->isSecurityScannerOwned())
				? vertexA : getSkippablePathTarget(vertexA);
			if (directTarget != vertexA)
			{
				mTraversalTask->destinationVertex = mPath.path->nodes[directTarget].targetVertex;
				mTraversalTask->pathNodesConsumed = directTarget - mPath.targetNode;
			}
			// Inter-layer thresholds have coincident 2D endpoints. Keep the
			// locomotion task visible for a short deterministic crossing instead
			// of committing in the permit-allocation tick. The crossing runs in
			// place at the Agent's current position; the far-side Door vertex is
			// never a movement target. An admitted automatic-Crawling crossing
			// doubles that duration and lowers the Pose for the crossing only.
			bool const doorCrossing = mTraversalTask->edge->getType() == EdgeType::Door;
			if (doorCrossing)
			{
				auto const& doorEdge = static_cast<DoorEdge const&>(*mTraversalTask->edge);
				// A Broken Door's crossing mode comes from its frozen aperture, not
				// its full height: a partially-open vertical Door can force Crawling.
				auto const mode = doorEdge.getDoor()->isIndependentlyBroken()
					? doorEdge.getDoor()->classifyBrokenAgentCrossing(*this, getGlobalPosition().y)
					: doorEdge.getDoor()->classifyAgentCrossing(*this, getGlobalPosition().y);
				// An onboard Lift or Shuttle passenger must finish its committed exit
				// even if its envelope or the opening changed during the ride.
				mTraversalTask->crawling = mode == Door::DoorCrossingMode::Crawling
					|| (mode == Door::DoorCrossingMode::None
						&& (getSector()->getType() == SectorType::Lift
							|| getSector()->getType() == SectorType::Shuttle));
				if (mTraversalTask->crawling) mPose = Pose::Crawling;
			}
			if (bulkhead && (bulkhead->isStandalone() || bulkhead->getDoor()->isAirlockOwned()
				|| bulkhead->getDoor()->isSecurityScannerOwned()))
			{
				// A Broken Bulkhead Door's crossing mode comes from its frozen
				// aperture, so a partially-open door can force Crawling.
				auto const mode = bulkhead->getDoor()->isIndependentlyBroken()
					? bulkhead->getDoor()->classifyBrokenAgentCrossing(*this, getGlobalPosition().y)
					: bulkhead->getDoor()->classifyAgentCrossing(*this, getGlobalPosition().y);
				// An admitted Chamber occupant must finish its exit even if its
				// envelope or the opening changed during the interlocked journey.
				mTraversalTask->crawling = mode == Door::DoorCrossingMode::Crawling
					|| (mode == Door::DoorCrossingMode::None && getSector()->getType() == SectorType::Chamber);
				if (mTraversalTask->crawling) mPose = Pose::Crawling;
			}
			mTraversalTask->traversalTicksRemaining =
				doorCrossing ? (mTraversalTask->crawling
					? static_cast<uint64_t>(6.0f / getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, Pose::Crawling).value())
					: 6) : 0;
			if (mTraversalTask->edge->getType() == EdgeType::Staircase
				&& mTraversalTask->edge->getTraversalSpeed(nullptr) > 0.0f)
			{
				// SplitMix64: stable integer arithmetic, with a stream keyed by Agent ID.
				// Admission is complete: this is the sole decision point for this task.
				uint64_t draw = (mWorld->getAgentId(this).value
					^ (mWorld->getRandomSeed() * 0xd1b54a32d192ed03ULL))
					+ 0x9e3779b97f4a7c15ULL * (++mEscalatorTraversalSequence);
				draw = (draw ^ (draw >> 30)) * 0xbf58476d1ce4e5b9ULL;
				draw = (draw ^ (draw >> 27)) * 0x94d049bb133111ebULL;
				draw ^= draw >> 31;
				auto const unit = static_cast<double>(draw >> 11) * 0x1.0p-53;
				mTraversalTask->escalatorWalking = unit < getEffectiveEscalatorWalkingChance().value;
			}
			syncStandingRouteObservation();
			if (mTraversalTask->destinationVertex->getSector().get() == getSector())
				mLocalDepth = mTraversalTask->edge->getLocalDepth();
			mState = State::TraversingEdge;
		}
	}

	void Agent::commitTraversal()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mState != State::AwaitingTraversalCommit || !mTraversalTask || !mWorld)
		{
			return;
		}

		if (!mWorld->commitTraversal(*this, mTraversalTask->request,
			mTraversalTask->permit, mTraversalTask->destinationVertex))
		{
			return;
		}

		auto const consumed = mTraversalTask->pathNodesConsumed;
		for (uint32_t i = 0; i < consumed && mPath.path; ++i) nextPathNode();
		// Automatic Crawling is crossing-scoped, but the destination Sector now
		// owns the Pose: a normal Sector stands the Agent back up, while a
		// one-cell-high low Room ducks or crawls it.
		syncPoseToSector();
	}

	void Agent::considerTraversalReplan()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (!mTraversalTask || mTraversalTask->permit || !mWorld || !mPath.path
			|| mPath.path->nodes.empty()) return;
		auto request = mWorld->lookupTraversalRequest(mTraversalTask->request);
		if (!request || !request.entity->getQueueTicket()) return;
		// A fixed Airlock batch is already being served. Do not discard its
		// reservation merely to reconsider a still-valid route while the door opens.
		if (request.entity->hasCapacityPosition()
			&& (mTraversalTask->destinationVertex->getSector()->getType() == SectorType::Airlock
				|| mTraversalTask->destinationVertex->getSector()->getType() == SectorType::Chamber)) return;
		auto const& policy = mWorld->getTraversalWaitingPolicy();
		auto waited = mWorld->getSimulationTick() - request.entity->getQueuedAtTick();
		if (waited < policy.minimumReplanWaitTicks
			|| (waited - policy.minimumReplanWaitTicks) % policy.replanIntervalTicks != 0) return;

		mWorld->beginVoluntaryRoutePlanning(mWorld->getAgentId(this));
	}

	void Agent::cleanupTraversal()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (!mTraversalTask || !mWorld)
		{
			return;
		}

		auto request = mWorld->lookupTraversalRequest(mTraversalTask->request);
		if (request && (request.entity->getState() == TraversalRequestState::Committed
			|| request.entity->getState() == TraversalRequestState::Cancelled))
		{
			syncPoseToSector();
			mWorld->releaseTraversal(mTraversalTask->request, mTraversalTask->permit);
			mTraversalTask.reset();
			syncStandingRouteObservation();
			mTraversalLocalGoal.reset();
			if (mQueuedTraversalTask)
			{
				mTraversalTask = std::move(mQueuedTraversalTask);
				mQueuedTraversalTask.reset();
				mState = State::WaitingForTraversal;
			}
			return;
		}
		considerTraversalReplan();
	}

	void Agent::cancelTraversal()
	{
		if (!mTraversalTask && !mQueuedTraversalTask) return;

		if (mWorld)
		{
			if (mTraversalTask)
			{
				mWorld->cancelTraversal(mTraversalTask->request, mTraversalTask->permit);
				mWorld->releaseTraversal(mTraversalTask->request, mTraversalTask->permit);
			}
			if (mQueuedTraversalTask)
			{
				mWorld->cancelTraversal(mQueuedTraversalTask->request, mQueuedTraversalTask->permit);
				mWorld->releaseTraversal(mQueuedTraversalTask->request, mQueuedTraversalTask->permit);
			}
		}
		syncPoseToSector();
		mTraversalTask.reset();
		syncStandingRouteObservation();
		mQueuedTraversalTask.reset();
		mTraversalLocalGoal.reset();
	}

	bool Agent::moveToVertexOffset(int dim, float offset, float frameTime)
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		ASSERT_DIM_OK(dim);

		auto targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();

		if (dim == CORE_DIM_X)
		{
			targetPos.x += offset;
		}
		else
		{
			targetPos.y += offset;
		}

		return moveToPosition(targetPos, frameTime, getWalkSpeed());
	}

	void Agent::wake()
	{
		if (mWorld) mWorld->invalidateSimulationSnapshot();
		if (mState != State::Idle)
		{
			return;
		}

		if (mPath.path)
		{
			startPathingInternal();
		}
	}

	void Agent::update(float frameTime)
	{
		switch (mState)
		{
		case State::RoutePlanning:
		case State::Idle:
			break;

		case State::MovingToVertex:
			moveToVertex(frameTime);
			break;

		case State::TraversingEdge:
			// A same-sector Location edge may lead directly to a queued threshold.
			// Stop and commit that unconstrained edge at the queue boundary so the
			// following Door/Ladder request is created before reaching its centre.
			if (mTraversalTask && mWorld && mPath.path
				&& mTraversalTask->edge->getType() == EdgeType::Location
				&& mTraversalTask->destinationVertex->getSector().get() == getSector()
				&& mPath.targetNode + 2 < mPath.path->nodes.size()
				&& mWorld->stopForAvailableQueuePosition(*this,
					mPath.path->nodes[mPath.targetNode + 2].edge,
					mTraversalTask->destinationVertex->getPosition(),
					getWalkSpeed() * frameTime))
			{
				auto const& sourceNode = mPath.path->nodes[mPath.targetNode + 1];
				auto const& destinationNode = mPath.path->nodes[mPath.targetNode + 2];
				TraversalTask queued;
				queued.edge = destinationNode.edge;
				queued.sourceVertex = sourceNode.targetVertex;
				queued.destinationVertex = destinationNode.targetVertex;
				queued.request = mWorld->createTraversalRequest(*this, queued.edge,
					queued.sourceVertex, queued.destinationVertex);
				mEarlyQueueApproachDirectionX = 0;
				mQueuedTraversalTask = std::move(queued);
				mState = State::AwaitingTraversalCommit;
				break;
			}
			if (mTraversalTask && mTraversalTask->traversalTicksRemaining > 0)
			{
				--mTraversalTask->traversalTicksRemaining;
				break;
			}
			if (mTraversalTask)
			{
				// Door crossings and enclosed transport rides commit in place. Lift
				// and Shuttle allocation first walks the passenger into the selected
				// exit's crossing band, preserving a buffered standing position when
				// it is already valid.
				auto resource = mWorld
					? mWorld->lookupTraversalResource(mTraversalTask->edge->getTraversalResourceId())
					: EntityLookup<TraversalResource>{};
				auto const commitsInPlace = mTraversalTask->edge->getType() == EdgeType::Door
					|| (mTraversalTask->edge->getType() == EdgeType::Lift
						&& resource && resource.entity->isLift()
						&& !resource.entity->isOpenPlatformLift())
					|| (mTraversalTask->edge->getType() == EdgeType::Shuttle
						&& resource && resource.entity->isShuttle());
				if (commitsInPlace)
				{
					mState = State::AwaitingTraversalCommit;
					break;
				}
				// Admission can be granted while standing in an endpoint queue.
				// Reach the mount horizontally before starting the vertical climb.
				if (mTraversalTask->edge->getType() == EdgeType::Ladder
					&& abs(getGlobalPosition().x - mTraversalTask->sourceVertex->getPosition().x) > 0.001f)
				{
					moveToPosition(mTraversalTask->sourceVertex->getPosition(), frameTime, getWalkSpeed());
					break;
				}
				float traversalSpeed = mTraversalTask->edge->getTraversalSpeed(
					this, mTraversalTask->destinationVertex);
				if (traversalSpeed <= 0.0f)
					traversalSpeed = mTraversalTask->edge->getType() == EdgeType::Ladder
						? getClimbSpeed() : getWalkSpeed();
				if (mTraversalTask->crawling) traversalSpeed *= 0.5f;
				if (moveToPosition(mTraversalTask->destinationVertex->getPosition(), frameTime,
					traversalSpeed))
					mState = State::AwaitingTraversalCommit;
			}
			break;

		case State::WaitingForTraversal:
			if (mTraversalLocalGoal)
			{
				moveToPosition(*mTraversalLocalGoal, frameTime, getWalkSpeed());
			}
			break;

		case State::AwaitingTraversalCommit:
			break;

		default:
			throw UnhandledException(mState, "Agent::State");
		}
	}

} // core