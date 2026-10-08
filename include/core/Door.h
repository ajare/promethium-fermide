#pragma once

#include <bitset>
#include <cstdint>
#include <memory>

#include "core/Defines.h"
#include "core/OpenableObject.h"
#include "core/Coordination.h"
#include "core/DeviceCondition.h"
#include <optional>

namespace core
{
	class Sector;
	class Agent;

	class Door : public OpenableObject
	{
		friend class World;
		friend class SimulationCoordinator;
	public:
		// The authored visual manner in which the Door's leaf or leaves reveal its
		// threshold. OpenApart's two leaves remain one logical Door. A tall OpenUp
		// Door takes longer so its leaf keeps the regular Door's vertical speed.
		enum struct OpenStyle { OpenUp, OpenLeft, OpenRight, OpenApart };
		enum struct Height { Regular, Tall };
		// The crossing mode an Agent uses at this threshold. Standing crosses in
		// its current/standing envelope; Crawling is the automatic low-Door
		// fallback (30% height, 50% crossing speed); None means infeasible.
		enum struct DoorCrossingMode { None, Standing, Crawling };

	private:
		uint32_t mCellsWide;
		Height mHeight{ Height::Regular };
		OpenStyle mOpenStyle{ OpenStyle::OpenUp };
		std::weak_ptr<const Sector> mSectors[2];
		DoorActivationMode mActivationMode{ DoorActivationMode::Manual };
		TraversalResourceId mTraversalResource;
		float mHoldOpenTime{ CORE_DOOR_STAY_OPEN_TIME };
		std::optional<float> mSpeedOverride;
		std::optional<float> mHeightScale;
		uint32_t mOpenLeaseCount{ 0 };
		bool mObstructed{ false };
		bool mAirlockOwned{ false };
		bool mSecurityScannerOwned{ false };
		bool mLiftOwned{ false };
		bool mShuttleOwned{ false };
		void advanceCoordinatedMotion(float frameTime);
		bool mBreakable{ false };
		bool mInitiallyBroken{ false };
		bool mBroken{ false };
		// Authored frozen-open fraction (0..1) applied when the Door is Broken.
		float mBrokenOpenPercentage{ 0.0f };
		// A buttonless manual ordinary Door owns the authorization for its implicit
		// opening interaction. It is shared by both directed graph edges.
		std::bitset<256> mPermissionRequirement;

	protected:
		Door(float x, float y, float width, float height, uint32_t cellsWide,
			std::shared_ptr<const Sector> sectors[2]);

	public:
		Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
			std::shared_ptr<const Sector> sectors[2], Height height = Height::Regular);

		uint32_t getCellsWide() const;
		Height getHeight() const { return mHeight; }
		void setHeight(Height height);
		static bool heightScaleIsValid(std::optional<float> scale);
		static float effectiveHeight(Height height, std::optional<float> scale = {});
		std::optional<float> getHeightScale() const { return mHeightScale; }
		bool setHeightScale(std::optional<float> scale);
		// Top clearance above the approach Floor; unrelated to arrival/lane tolerances.
		static constexpr float ClearanceTolerance = 0.00001f;
		static constexpr float StandingClearanceTolerance = ClearanceTolerance;
		bool admitsVerticalExtent(float topAboveFloor, float approachFloorY) const;
		// Shared clearance decision boundary for routing and every admission gate:
		// `classifyAgentCrossing(...) != None`. Combines the Agent's effective
		// traversal envelope with this Door's top clearance above the given
		// approach floor, preserving the Door's scope and pose/support semantics.
		bool admitsAgentTraversal(Agent const& agent, float approachFloorY,
			bool beginningMovement = false) const;
		// Shared clearance decision boundary for routing and every admission gate.
		// Standing when the Agent's current/standing envelope fits; otherwise
		// Crawling when the automatic fallback applies and its 30% envelope fits;
		// otherwise None. Retained lowered Action poses cross in their own
		// envelope and never trigger the automatic Crawling fallback.
		DoorCrossingMode classifyAgentCrossing(Agent const& agent, float approachFloorY,
			bool beginningMovement = false) const;
		// The crossing mode for a Broken Door's frozen aperture, mirroring
		// classifyAgentCrossing but against the scaled opening instead of the full
		// height. Vertical openings scale height (Standing, then automatic
		// Crawling); horizontal openings scale width and keep their full height.
		DoorCrossingMode classifyBrokenAgentCrossing(Agent const& agent, float openFraction,
			float approachFloorY, bool beginningMovement = false) const;
		DoorCrossingMode classifyBrokenAgentCrossing(Agent const& agent, float approachFloorY,
			bool beginningMovement = false) const
		{ return classifyBrokenAgentCrossing(agent, getOpenPercentage(), approachFloorY, beginningMovement); }
		// A Broken Door is frozen at some open fraction. A vertical opening
		// (OpenUp and Bulkhead Doors) admits passage when the Agent's standing
		// clearance extent fits under the scaled aperture height; a horizontal
		// opening (OpenApart, OpenLeft, OpenRight) admits it when the Agent's
		// width fits within the scaled aperture width. `openFraction` may come
		// from a remembered condition, not only the live percentage.
		bool admitsBrokenPassage(Agent const& agent, float openFraction, float approachFloorY,
			bool beginningMovement = false) const;
		bool admitsBrokenPassage(Agent const& agent, float approachFloorY,
			bool beginningMovement = false) const
		{ return admitsBrokenPassage(agent, getOpenPercentage(), approachFloorY, beginningMovement); }
		bool admitsStandingHeight(float standingHeight, float approachFeetY) const
		{ return admitsVerticalExtent(standingHeight, approachFeetY); }
		OpenStyle getOpenStyle() const;
		void setOpenStyle(OpenStyle style);
		static bool speedIsValid(std::optional<float> speed);
		static bool brokenOpenPercentageIsValid(float openPercentage);
		std::optional<float> getSpeedOverride() const { return mSpeedOverride; }
		virtual float getDefaultSpeed() const;
		float getSpeed() const { return mSpeedOverride.value_or(getDefaultSpeed()); }
		bool setSpeedOverride(std::optional<float> speed);

		// A Door joins exactly one adjacent Layer pair.  The index is the side of that
		// pair, not an absolute Layer index: 0 is the front Layer the Door is authored
		// on, 1 is the Layer directly behind it.
		std::shared_ptr<const Sector> getSector(uint32_t pairSide) const;
		std::shared_ptr<const Sector> getFrontSector() const { return mSectors[0].lock(); }
		std::shared_ptr<const Sector> getBackSector() const { return mSectors[1].lock(); }
		// The absolute Layers the Door crosses.  ~0u when a side has no Sector.
		uint32_t getFrontLayer() const;
		uint32_t getBackLayer() const;
		DoorActivationMode getActivationMode() const { return mActivationMode; }
		TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		void configureTraversal(DoorActivationMode mode, TraversalResourceId resource, float holdOpenTime);
		void acquireOpenLease();
		void releaseOpenLease();
		uint32_t getOpenLeaseCount() const { return mOpenLeaseCount; }
		void setObstructed(bool obstructed) { mObstructed = obstructed; }
		bool isObstructed() const { return mObstructed; }
		std::bitset<256> const& getPermissionRequirement() const { return mPermissionRequirement; }

		// Typed device operations call these commands; no callback/action queue exists.
		bool isAirlockOwned() const { return mAirlockOwned; }
		bool isSecurityScannerOwned() const { return mSecurityScannerOwned; }
		bool isLiftOwned() const { return mLiftOwned; }
		bool isShuttleOwned() const { return mShuttleOwned; }
		bool isChamberOwned() const { return mAirlockOwned || mSecurityScannerOwned; }
		bool isBreakable() const { return mBreakable; }
		bool isInitiallyBroken() const { return mInitiallyBroken; }
		bool isBroken() const { return mBroken; }
		// A Breakable Door's own Broken state. Transport landing Doors mirror their
		// coupled Lift/Shuttle's Broken flag in mBroken, so the authored broken-open
		// passage rule must never treat them as independently Broken.
		bool isIndependentlyBroken() const { return mBreakable && mBroken; }
		float getBrokenOpenPercentage() const { return mBrokenOpenPercentage; }
		bool admitsNewCrossings() const { return mBroken ? mOpenPct >= 1.0f : isOpen(); }
		std::optional<DeviceCondition> knownCondition(Agent const* agent,
			Sector const* observationSector) const;
		bool open() override;
		bool close() override;
		bool requestOpen();
		bool requestClose();
		void update(float frameTime) override;

		std::string getDescription() const override;
		float getOpenCloseTime() const override;
		float getTimeBeforeClosing() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
