#include "core/MobilityProfile.h"

#include "core/Agent.h"
#include "core/Edge.h"
#include "core/SerializationException.h"
#include "core/Serializer.h"

#include <array>
#include <format>
#include <string_view>

namespace core
{
	namespace
	{
		struct TraversalEntry
		{
			TraversalKind kind;
			std::string_view name;
		};

		constexpr std::array TraversalEntries{
			TraversalEntry{ TraversalKind::Staircase, "staircase" },
			TraversalEntry{ TraversalKind::Escalator, "escalator" },
			TraversalEntry{ TraversalKind::Stairwell, "stairwell" },
			TraversalEntry{ TraversalKind::Ladder, "ladder" },
			TraversalEntry{ TraversalKind::Lift, "lift" },
			TraversalEntry{ TraversalKind::PlatformLift, "platformLift" },
			TraversalEntry{ TraversalKind::Shuttle, "shuttle" },
			TraversalEntry{ TraversalKind::Door, "door" },
			TraversalEntry{ TraversalKind::Buttons, "buttons" }
		};

		std::string_view mobilityUseName(MobilityUse use)
		{
			switch (use)
			{
			case MobilityUse::CanUse: return "canUse";
			case MobilityUse::CannotUse: return "cannotUse";
			case MobilityUse::OnlyIfNoOtherOption: return "onlyIfNoOtherOption";
			}
			return {};
		}

		MobilityUse parseMobilityUse(std::string_view name)
		{
			if (name == "canUse") return MobilityUse::CanUse;
			if (name == "cannotUse") return MobilityUse::CannotUse;
			if (name == "onlyIfNoOtherOption") return MobilityUse::OnlyIfNoOtherOption;
			throw SerializationException(std::format(
				"Unsupported Mobility profile use '{}'", name));
		}

		MobilityUse combineMobilityUse(MobilityUse left, MobilityUse right)
		{
			if (left == MobilityUse::CannotUse || right == MobilityUse::CannotUse)
				return MobilityUse::CannotUse;
			if (left == MobilityUse::OnlyIfNoOtherOption
				|| right == MobilityUse::OnlyIfNoOtherOption)
				return MobilityUse::OnlyIfNoOtherOption;
			return MobilityUse::CanUse;
		}

		bool rejects(MobilityUse use, bool allowFallback)
		{
			return use == MobilityUse::CannotUse
				|| (!allowFallback && use == MobilityUse::OnlyIfNoOtherOption);
		}
	}

	bool routeRejectsButtons(RouteDecisionContext const& context)
	{
		return context.mobilityProfile
			? rejects(context.mobilityProfile->get(TraversalKind::Buttons), context.allowFallbackMobility)
			: agentRejectsButtons(context.legacyAgent, context.allowFallbackMobility);
	}

	bool routeRejectsEdge(RouteDecisionContext const& context, Edge const& edge, TraversalKind kind)
	{
		if (!context.mobilityProfile)
			return agentRejectsEdge(context.legacyAgent, edge, kind, context.allowFallbackMobility);
		auto use = context.mobilityProfile->get(kind);
		if (edge.requiresButton())
			use = combineMobilityUse(use, context.mobilityProfile->get(TraversalKind::Buttons));
		return rejects(use, context.allowFallbackMobility);
	}

	void serializeMobilityProfile(Serializer& serializer, MobilityProfile const& profile)
	{
		if (!mobilityProfileIsValid(profile))
			throw SerializationException("Cannot serialize a Mobility profile with an invalid Mobility use");
		serializer.beginArray("uses");
		for (auto const& entry : TraversalEntries)
		{
			serializer.beginMap("");
			serializer.writeString("traversal", std::string(entry.name));
			serializer.writeString("use", std::string(mobilityUseName(profile.get(entry.kind))));
			serializer.endMap();
		}
		serializer.endArray();
	}

	MobilityProfile deserializeMobilityProfile(Serializer& serializer)
	{
		MobilityProfile profile;
		// Migrate the former forbidden-traversal bitfield without changing its
		// meaning. Newly serialized profiles always use explicit ternary entries.
		if (serializer.hasField("value"))
		{
			auto const mask = serializer.readUint32("value");
			if ((mask & ~uint32_t{ 0x1ff }) != 0)
				throw SerializationException(
					"Serialized Mobility profile contains reserved traversal bits");
			for (size_t index = 0; index < TraversalEntries.size(); ++index)
				if ((mask & (uint32_t{ 1 } << index)) != 0)
					profile.set(TraversalEntries[index].kind, MobilityUse::CannotUse);
			return profile;
		}

		std::array<bool, TraversalEntries.size()> seen{};
		serializer.beginArray("uses");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const traversal = serializer.readString("traversal");
			auto const use = parseMobilityUse(serializer.readString("use"));
			auto found = false;
			for (size_t index = 0; index < TraversalEntries.size(); ++index)
			{
				if (TraversalEntries[index].name != traversal) continue;
				if (seen[index]) throw SerializationException(std::format(
					"Serialized Mobility profile contains traversal '{}' more than once",
					traversal));
				seen[index] = true;
				profile.set(TraversalEntries[index].kind, use);
				found = true;
				break;
			}
			if (!found) throw SerializationException(std::format(
				"Unsupported Mobility profile traversal '{}'", traversal));
			serializer.endMap();
		}
		serializer.endArray();
		for (auto const present : seen)
			if (!present) throw SerializationException(
				"Serialized Mobility profile does not contain every traversal entry");
		return profile;
	}

	MobilityUse agentTraversalUse(Agent const* agent, TraversalKind kind)
	{
		return agent ? agent->getEffectiveMobilityProfile().value.get(kind)
			: MobilityUse::CanUse;
	}

	MobilityUse agentEdgeUse(Agent const* agent, Edge const& edge, TraversalKind kind)
	{
		auto use = agentTraversalUse(agent, kind);
		if (edge.requiresButton())
			use = combineMobilityUse(use, agentTraversalUse(agent, TraversalKind::Buttons));
		return use;
	}

	bool agentForbidsTraversal(Agent const* agent, TraversalKind kind)
	{
		return agentTraversalUse(agent, kind) == MobilityUse::CannotUse;
	}

	bool agentForbidsButtons(Agent const* agent)
	{
		return agentForbidsTraversal(agent, TraversalKind::Buttons);
	}

	bool agentForbidsEdge(Agent const* agent, Edge const& edge, TraversalKind kind)
	{
		return agentEdgeUse(agent, edge, kind) == MobilityUse::CannotUse;
	}

	bool agentRejectsTraversal(Agent const* agent, TraversalKind kind, bool allowFallback)
	{
		return rejects(agentTraversalUse(agent, kind), allowFallback);
	}

	bool agentRejectsButtons(Agent const* agent, bool allowFallback)
	{
		return agentRejectsTraversal(agent, TraversalKind::Buttons, allowFallback);
	}

	bool agentRejectsEdge(Agent const* agent, Edge const& edge, TraversalKind kind,
		bool allowFallback)
	{
		return rejects(agentEdgeUse(agent, edge, kind), allowFallback);
	}
}
