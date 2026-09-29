#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core
{
	enum class AgentPropertyType
	{
		Colour,
		EscalatorWalkingChance,
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
		MobilityProfile
	};

	struct AgentPropertyMetadata
	{
		std::string_view name;
		// No namespace means the property belongs to the default, unlabelled group.
		std::optional<std::string_view> propertyNamespace;
	};

	inline constexpr AgentPropertyMetadata agentPropertyMetadata(AgentPropertyType type)
	{
		switch (type)
		{
		case AgentPropertyType::Colour: return { "Colour", std::nullopt };
		case AgentPropertyType::EscalatorWalkingChance:
			return { "Escalator walking chance", "Pathing" };
		case AgentPropertyType::WalkSpeedModifier:
			return { "Walk speed modifier", std::nullopt };
		case AgentPropertyType::HeightModifier: return { "Height modifier", std::nullopt };
		case AgentPropertyType::StairSpeedModifier: return { "Stair speed modifier", "Pathing" };
		case AgentPropertyType::LadderSpeedModifier: return { "Ladder speed modifier", "Pathing" };
		case AgentPropertyType::InteractionAversion: return { "Interaction aversion", "Pathing" };
		case AgentPropertyType::EffortAversion: return { "Effort aversion", "Pathing" };
		case AgentPropertyType::WaitingAversion: return { "Waiting aversion", "Pathing" };
		case AgentPropertyType::CrowdAversion: return { "Crowd aversion", "Pathing" };
		case AgentPropertyType::RiskAversion: return { "Risk aversion", "Pathing" };
		case AgentPropertyType::RouteFamiliarity: return { "Route familiarity", "Pathing" };
		case AgentPropertyType::RoutePersistence: return { "Route persistence", "Pathing" };
		case AgentPropertyType::MobilityProfile: return { "Mobility profile", "Pathing" };
		}
		return { {}, std::nullopt };
	}

	// The first hardcoded Agent property. It deliberately has no alpha channel:
	// ordinary Agents are opaque, and selection replaces this colour entirely.
	struct AgentColour
	{
		uint8_t r{ 179 };
		uint8_t g{ 77 };
		uint8_t b{ 77 };

		bool operator==(AgentColour const& other) const = default;
	};

	inline constexpr AgentColour EditorDefaultAgentColour{};
	inline constexpr AgentColour SelectedAgentColour{ 251, 188, 4 };

	// The sixteen pastel Colours that intrinsic tag display Colours draw from.
	// Draws need not be distinct; each draw is uniform and independent.
	inline constexpr std::array<AgentColour, 16> AgentTagColourPalette
	{
		AgentColour{ 255, 179, 186 }, // pastel pink
		AgentColour{ 255, 205, 178 }, // pastel peach
		AgentColour{ 255, 223, 186 }, // pastel apricot
		AgentColour{ 255, 239, 186 }, // pastel yellow
		AgentColour{ 240, 255, 190 }, // pastel lime-yellow
		AgentColour{ 202, 255, 191 }, // pastel lime
		AgentColour{ 186, 255, 201 }, // pastel mint
		AgentColour{ 186, 255, 235 }, // pastel aqua
		AgentColour{ 186, 225, 255 }, // pastel sky
		AgentColour{ 195, 198, 255 }, // pastel periwinkle
		AgentColour{ 222, 194, 255 }, // pastel lavender
		AgentColour{ 240, 190, 255 }, // pastel orchid
		AgentColour{ 255, 196, 246 }, // pastel fuchsia
		AgentColour{ 255, 214, 214 }, // pastel rose
		AgentColour{ 226, 213, 198 }, // pastel sand
		AgentColour{ 205, 218, 205 }, // pastel sage
	};

	struct AgentEscalatorWalkingChanceProperty
	{
		float value{ 0.0f };
		uint64_t revision{ 0 };
		bool operator==(AgentEscalatorWalkingChanceProperty const&) const = default;
	};

	bool agentEscalatorWalkingChanceIsValid(float value,
		std::string* diagnostic = nullptr);

	struct AgentColourProperty
	{
		AgentColour value{};
		uint64_t revision{ 0 };

		bool operator==(AgentColourProperty const& other) const = default;
	};

	struct AgentModifierRange
	{
		float minimum{ 1.0f };
		float maximum{ 1.0f };

		bool operator==(AgentModifierRange const& other) const = default;
	};

	inline constexpr float AgentWalkSpeedModifierMinimum{ 0.8f };
	inline constexpr float AgentWalkSpeedModifierMaximum{ 1.2f };
	inline constexpr AgentModifierRange DefaultAgentWalkSpeedModifierRange{};
	inline constexpr float AgentHeightModifierMinimum{ 0.7f };
	inline constexpr float AgentHeightModifierMaximum{ 1.0f };
	inline constexpr AgentModifierRange DefaultAgentHeightModifierRange{};
	inline constexpr float AgentStairSpeedModifierMinimum{ 0.5f };
	inline constexpr float AgentStairSpeedModifierMaximum{ 1.5f };
	inline constexpr AgentModifierRange DefaultAgentStairSpeedModifierRange{};
	inline constexpr float AgentLadderSpeedModifierMinimum{ 0.5f };
	inline constexpr float AgentLadderSpeedModifierMaximum{ 1.5f };
	inline constexpr AgentModifierRange DefaultAgentLadderSpeedModifierRange{};
	inline constexpr float AgentInteractionAversionMinimum{ 0.0f };
	inline constexpr float AgentInteractionAversionMaximum{ 3.0f };
	inline constexpr AgentModifierRange DefaultAgentInteractionAversionRange{};
	inline constexpr float AgentEffortAversionMinimum{ 0.0f };
	inline constexpr float AgentEffortAversionMaximum{ 3.0f };
	inline constexpr AgentModifierRange DefaultAgentEffortAversionRange{};
	inline constexpr float AgentWaitingAversionMinimum{ 0.5f };
	inline constexpr float AgentWaitingAversionMaximum{ 3.0f };
	inline constexpr AgentModifierRange DefaultAgentWaitingAversionRange{};
	inline constexpr float AgentCrowdAversionMinimum{ 0.0f };
	inline constexpr float AgentCrowdAversionMaximum{ 3.0f };
	inline constexpr AgentModifierRange DefaultAgentCrowdAversionRange{};
	inline constexpr float AgentRiskAversionMinimum{ 0.0f };
	inline constexpr float AgentRiskAversionMaximum{ 3.0f };
	inline constexpr AgentModifierRange DefaultAgentRiskAversionRange{};
	inline constexpr float AgentRouteFamiliarityMinimum{ 0.0f };
	inline constexpr float AgentRouteFamiliarityMaximum{ 1.0f };
	inline constexpr AgentModifierRange DefaultAgentRouteFamiliarityRange{ 0.5f, 0.5f };
	inline constexpr float AgentRoutePersistenceMinimum{ 0.0f };
	inline constexpr float AgentRoutePersistenceMaximum{ 1.0f };
	inline constexpr AgentModifierRange DefaultAgentRoutePersistenceRange{ 0.15f, 0.15f };

	struct AgentWalkSpeedModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentWalkSpeedModifierProperty const& other) const = default;
	};

	struct AgentHeightModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentHeightModifierProperty const& other) const = default;
	};

	struct AgentStairSpeedModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentStairSpeedModifierProperty const& other) const = default;
	};

	struct AgentLadderSpeedModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentLadderSpeedModifierProperty const& other) const = default;
	};

	struct AgentInteractionAversionProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentInteractionAversionProperty const& other) const = default;
	};

	struct AgentEffortAversionProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentEffortAversionProperty const& other) const = default;
	};

	struct AgentWaitingAversionProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentWaitingAversionProperty const& other) const = default;
	};

	struct AgentCrowdAversionProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentCrowdAversionProperty const& other) const = default;
	};

	struct AgentRiskAversionProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentRiskAversionProperty const& other) const = default;
	};

	struct AgentRouteFamiliarityProperty
	{
		AgentModifierRange range{ DefaultAgentRouteFamiliarityRange };
		uint64_t revision{ 0 };

		bool operator==(AgentRouteFamiliarityProperty const& other) const = default;
	};

	struct AgentRoutePersistenceProperty
	{
		AgentModifierRange range{ DefaultAgentRoutePersistenceRange };
		uint64_t revision{ 0 };

		bool operator==(AgentRoutePersistenceProperty const& other) const = default;
	};

	enum class TraversalKind : uint8_t
	{
		Staircase,
		Escalator,
		Stairwell,
		Ladder,
		Lift,
		PlatformLift,
		Shuttle,
		Door,
		Buttons,
		Count
	};

	enum class MobilityUse : uint8_t
	{
		CanUse,
		CannotUse,
		OnlyIfNoOtherOption
	};

	struct MobilityProfile
	{
		std::array<MobilityUse, static_cast<size_t>(TraversalKind::Count)> uses{};

		[[nodiscard]] MobilityUse get(TraversalKind kind) const
		{
			return uses.at(static_cast<size_t>(kind));
		}
		void set(TraversalKind kind, MobilityUse use)
		{
			uses.at(static_cast<size_t>(kind)) = use;
		}
		bool operator==(MobilityProfile const&) const = default;
	};

	inline constexpr bool mobilityProfileIsValid(MobilityProfile const& profile)
	{
		for (auto const use : profile.uses)
			if (use != MobilityUse::CanUse && use != MobilityUse::CannotUse
				&& use != MobilityUse::OnlyIfNoOtherOption) return false;
		return true;
	}

	struct AgentMobilityProfileProperty
	{
		MobilityProfile value{};
		uint64_t revision{ 0 };
		bool operator==(AgentMobilityProfileProperty const&) const = default;
	};

	void agentColourToFloats(AgentColour const& colour, float out[3]);
	AgentColour agentColourFromFloats(float const in[3]);

	// Draws one pastel from AgentTagColourPalette uniformly at random. Tag
	// creation and legacy-load backfill share this so automatic Colours behave
	// identically. The generator is never consulted by load or simulation reset.
	AgentColour sampleAgentTagColour();
	bool agentWalkSpeedModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentHeightModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentStairSpeedModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentLadderSpeedModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentInteractionAversionRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentEffortAversionRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentWaitingAversionRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentCrowdAversionRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentRiskAversionRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentRouteFamiliarityRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentRoutePersistenceRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	float sampleAgentModifier(AgentModifierRange const& range);

	// A named reusable set of Agent properties. Property types are hardcoded;
	// the optional values and their revisions are authored registry data.
	class AgentTag
	{
		friend class AgentTagRegistry;

		std::string mName;
		// The intrinsic display Colour used when rendering the tag itself (for
		// example its Selection-panel chip). It is deliberately separate from
		// the optional Agent Colour property below, which Agents inherit.
		AgentColour mDisplayColour{};
		std::optional<AgentColourProperty> mColour;
		std::optional<AgentEscalatorWalkingChanceProperty> mEscalatorWalkingChance;
		void setEscalatorWalkingChance(AgentEscalatorWalkingChanceProperty property)
		{ mEscalatorWalkingChance = property; }
		void removeEscalatorWalkingChance() { mEscalatorWalkingChance.reset(); }
		std::optional<AgentWalkSpeedModifierProperty> mWalkSpeedModifier;
		std::optional<AgentHeightModifierProperty> mHeightModifier;
		std::optional<AgentStairSpeedModifierProperty> mStairSpeedModifier;
		std::optional<AgentLadderSpeedModifierProperty> mLadderSpeedModifier;
		std::optional<AgentInteractionAversionProperty> mInteractionAversion;
		std::optional<AgentEffortAversionProperty> mEffortAversion;
		std::optional<AgentWaitingAversionProperty> mWaitingAversion;
		std::optional<AgentCrowdAversionProperty> mCrowdAversion;
		std::optional<AgentRiskAversionProperty> mRiskAversion;
		std::optional<AgentRouteFamiliarityProperty> mRouteFamiliarity;
		std::optional<AgentRoutePersistenceProperty> mRoutePersistence;
		std::optional<AgentMobilityProfileProperty> mMobilityProfile;

		explicit AgentTag(std::string name)
			: mName(std::move(name))
		{
		}

		void setName(std::string name) { mName = std::move(name); }
		void setDisplayColour(AgentColour colour) { mDisplayColour = colour; }
		void setColour(AgentColourProperty colour) { mColour = colour; }
		void removeColour() { mColour.reset(); }
		void setWalkSpeedModifier(AgentWalkSpeedModifierProperty property)
		{
			mWalkSpeedModifier = property;
		}
		void removeWalkSpeedModifier() { mWalkSpeedModifier.reset(); }
		void setHeightModifier(AgentHeightModifierProperty property)
		{
			mHeightModifier = property;
		}
		void removeHeightModifier() { mHeightModifier.reset(); }
		void setStairSpeedModifier(AgentStairSpeedModifierProperty property)
		{
			mStairSpeedModifier = property;
		}
		void removeStairSpeedModifier() { mStairSpeedModifier.reset(); }
		void setLadderSpeedModifier(AgentLadderSpeedModifierProperty property)
		{
			mLadderSpeedModifier = property;
		}
		void removeLadderSpeedModifier() { mLadderSpeedModifier.reset(); }
		void setInteractionAversion(AgentInteractionAversionProperty property)
		{
			mInteractionAversion = property;
		}
		void removeInteractionAversion() { mInteractionAversion.reset(); }
		void setEffortAversion(AgentEffortAversionProperty property)
		{
			mEffortAversion = property;
		}
		void removeEffortAversion() { mEffortAversion.reset(); }
		void setWaitingAversion(AgentWaitingAversionProperty property)
		{
			mWaitingAversion = property;
		}
		void removeWaitingAversion() { mWaitingAversion.reset(); }
		void setCrowdAversion(AgentCrowdAversionProperty property)
		{
			mCrowdAversion = property;
		}
		void removeCrowdAversion() { mCrowdAversion.reset(); }
		void setRiskAversion(AgentRiskAversionProperty property)
		{
			mRiskAversion = property;
		}
		void removeRiskAversion() { mRiskAversion.reset(); }
		void setRouteFamiliarity(AgentRouteFamiliarityProperty property)
		{
			mRouteFamiliarity = property;
		}
		void removeRouteFamiliarity() { mRouteFamiliarity.reset(); }
		void setRoutePersistence(AgentRoutePersistenceProperty property)
		{
			mRoutePersistence = property;
		}
		void removeRoutePersistence() { mRoutePersistence.reset(); }
		void setMobilityProfile(AgentMobilityProfileProperty property)
		{
			mMobilityProfile = property;
		}
		void removeMobilityProfile() { mMobilityProfile.reset(); }

	public:
		static constexpr size_t MaxNameCharacters{ 12 };

		static std::unique_ptr<AgentTag> create(std::string name);
		static bool nameIsValid(std::string const& name, std::string* diagnostic = nullptr);

		std::string const& getName() const { return mName; }
		AgentColour getDisplayColour() const { return mDisplayColour; }
		AgentEscalatorWalkingChanceProperty const* getEscalatorWalkingChance() const
		{ return mEscalatorWalkingChance ? &*mEscalatorWalkingChance : nullptr; }
		AgentColourProperty const* getColour() const
		{
			return mColour ? &*mColour : nullptr;
		}
		AgentWalkSpeedModifierProperty const* getWalkSpeedModifier() const
		{
			return mWalkSpeedModifier ? &*mWalkSpeedModifier : nullptr;
		}
		AgentHeightModifierProperty const* getHeightModifier() const
		{
			return mHeightModifier ? &*mHeightModifier : nullptr;
		}
		AgentStairSpeedModifierProperty const* getStairSpeedModifier() const
		{
			return mStairSpeedModifier ? &*mStairSpeedModifier : nullptr;
		}
		AgentLadderSpeedModifierProperty const* getLadderSpeedModifier() const
		{
			return mLadderSpeedModifier ? &*mLadderSpeedModifier : nullptr;
		}
		AgentInteractionAversionProperty const* getInteractionAversion() const
		{
			return mInteractionAversion ? &*mInteractionAversion : nullptr;
		}
		AgentEffortAversionProperty const* getEffortAversion() const
		{
			return mEffortAversion ? &*mEffortAversion : nullptr;
		}
		AgentWaitingAversionProperty const* getWaitingAversion() const
		{
			return mWaitingAversion ? &*mWaitingAversion : nullptr;
		}
		AgentCrowdAversionProperty const* getCrowdAversion() const
		{
			return mCrowdAversion ? &*mCrowdAversion : nullptr;
		}
		AgentRiskAversionProperty const* getRiskAversion() const
		{
			return mRiskAversion ? &*mRiskAversion : nullptr;
		}
		AgentRouteFamiliarityProperty const* getRouteFamiliarity() const
		{
			return mRouteFamiliarity ? &*mRouteFamiliarity : nullptr;
		}
		AgentRoutePersistenceProperty const* getRoutePersistence() const
		{
			return mRoutePersistence ? &*mRoutePersistence : nullptr;
		}
		AgentMobilityProfileProperty const* getMobilityProfile() const
		{
			return mMobilityProfile ? &*mMobilityProfile : nullptr;
		}
	};
}
