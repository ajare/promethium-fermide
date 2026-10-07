#include <algorithm>
#include <cmath>

#include "core/Door.h"
#include "core/BulkheadDoor.h"
#include "core/Sector.h"
#include "core/Agent.h"

namespace core
{
	Door::Door(float x, float y, float width, float height, uint32_t cellsWide,
		std::shared_ptr<const Sector> sectors[2])
		: OpenableObject(x, y, width, height), mCellsWide(cellsWide),
		  mSectors{ sectors[0], sectors[1] }
	{
	}

	Door::Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
		std::shared_ptr<const Sector> sectors[2], Height height)
		: Door((float)cellX + CORE_DOOR_X_INSET, (float)cellY,
			cellsWide - CORE_DOOR_X_INSET * 2.0f,
			height == Height::Tall ? CORE_DOOR_TALL_HEIGHT : CORE_DOOR_HEIGHT,
			cellsWide, sectors)
	{
		mHeight = height;
	}

	bool Door::speedIsValid(std::optional<float> speed)
	{
		return !speed || (std::isfinite(*speed) && *speed > 0.0f);
	}

	bool Door::setSpeedOverride(std::optional<float> speed)
	{
		if (isChamberOwned() || !speedIsValid(speed)) return false;
		mSpeedOverride = speed;
		return true;
	}

	bool Door::heightScaleIsValid(std::optional<float> scale)
	{
		return !scale || (std::isfinite(*scale) && *scale >= 0.1f && *scale <= 1.0f);
	}

	float Door::effectiveHeight(Height height, std::optional<float> scale)
	{
		return height == Height::Tall ? CORE_DOOR_TALL_HEIGHT : CORE_DOOR_HEIGHT * scale.value_or(1.0f);
	}

	bool Door::setHeightScale(std::optional<float> scale)
	{
		auto front = getFrontSector();
		auto back = getBackSector();
		if (typeid(*this) != typeid(Door) || isChamberOwned() || mHeight != Height::Regular
			|| !front || !back || !isLocationLike(front->getType()) || !isLocationLike(back->getType())
			|| !heightScaleIsValid(scale)) return false;
		mHeightScale = scale;
		setSize({ getSize().x, effectiveHeight(mHeight, scale) });
		return true;
	}

	bool Door::admitsVerticalExtent(float topAboveFloor, float approachFloorY) const
	{
		auto front = getFrontSector();
		auto back = getBackSector();
		// Standalone, Airlock and both Chamber subtypes (Security Scanner and
		// Decontamination) use their physical Bulkhead Door opening, never an
		// ordinary Door height override. Lift and Shuttle landing Doors likewise
		// use their physical opening. Other specialized thresholds retain their rules.
		bool const bulkhead = typeid(*this) == typeid(BulkheadDoor);
		auto supportedSector = [&](Sector const& sector) {
			return isLocationLike(sector.getType())
				|| (isAirlockOwned() && sector.getType() == SectorType::Airlock)
				|| (isSecurityScannerOwned() && sector.getType() == SectorType::Chamber)
				|| (mLiftOwned && sector.getType() == SectorType::Lift)
				|| (mShuttleOwned && sector.getType() == SectorType::Shuttle);
		};
		if ((!bulkhead && typeid(*this) != typeid(Door)) || !front || !back
			|| !supportedSector(*front) || !supportedSector(*back)) return true;
		auto const availableHeight = getPosition().y
			+ ((bulkhead || mLiftOwned || mShuttleOwned) ? getSize().y : effectiveHeight(mHeight, mHeightScale)) - approachFloorY;
		return topAboveFloor <= availableHeight + ClearanceTolerance;
	}

	Door::DoorCrossingMode Door::classifyAgentCrossing(Agent const& agent, float approachFloorY,
		bool beginningMovement) const
	{
		// The current envelope governs first. A retained lowered Action pose
		// crosses in that pose and never triggers the automatic Crawling fallback.
		if (admitsVerticalExtent(agent.getTraversalDoorClearanceExtent(beginningMovement),
			approachFloorY))
			return DoorCrossingMode::Standing;
		if (agent.admitsAutomaticDoorCrawling(beginningMovement)
			&& admitsVerticalExtent(agent.getTraversalCrawlingDoorClearanceExtent(beginningMovement),
				approachFloorY))
			return DoorCrossingMode::Crawling;
		return DoorCrossingMode::None;
	}

	bool Door::admitsAgentTraversal(Agent const& agent, float approachFloorY,
		bool beginningMovement) const
	{
		return classifyAgentCrossing(agent, approachFloorY, beginningMovement)
			!= DoorCrossingMode::None;
	}

	uint32_t Door::getCellsWide() const { return mCellsWide; }
	void Door::setHeight(Height height)
	{
		if (isChamberOwned() || (height == Height::Tall && mHeightScale)) return;
		mHeight = height;
		setSize({ getSize().x, effectiveHeight(height, mHeightScale) });
	}
	Door::OpenStyle Door::getOpenStyle() const { return mOpenStyle; }
	void Door::setOpenStyle(OpenStyle style) { if (!isChamberOwned()) mOpenStyle = style; }

	std::shared_ptr<const Sector> Door::getSector(uint32_t pairSide) const
	{
		ASSERT_PAIR_SIDE_OK(pairSide);
		return mSectors[pairSide].lock();
	}

	uint32_t Door::getFrontLayer() const
	{
		auto const front = mSectors[0].lock();
		return front ? front->getLayerIndex() : ~0u;
	}

	uint32_t Door::getBackLayer() const
	{
		auto const back = mSectors[1].lock();
		return back ? back->getLayerIndex() : ~0u;
	}

	void Door::configureTraversal(DoorActivationMode mode, TraversalResourceId resource,
		float holdOpenTime)
	{
		if (isChamberOwned()) return;
		mActivationMode = mode;
		mTraversalResource = resource;
		mHoldOpenTime = std::max(0.0f, holdOpenTime);
	}

	void Door::acquireOpenLease() { ++mOpenLeaseCount; }

	void Door::releaseOpenLease()
	{
		if (mOpenLeaseCount == 0) return;
		--mOpenLeaseCount;
		if (mOpenLeaseCount == 0 && isOpen()) mOpenWaitTime = mHoldOpenTime;
	}

	std::optional<DeviceCondition> Door::knownCondition(Agent const* agent,
		Sector const* observationSector) const
	{
		if (!mBreakable) return std::nullopt;
		if (observationSector && (observationSector == getFrontSector().get()
			|| observationSector == getBackSector().get()))
			return DeviceCondition{ mBroken, mOpenPct };
		return agent ? agent->rememberedDeviceCondition(mTraversalResource) : std::nullopt;
	}

	bool Door::open() { return !isChamberOwned() && !mBroken && OpenableObject::open(); }
	bool Door::close() { return !isChamberOwned() && !mBroken && OpenableObject::close(); }

	bool Door::requestOpen()
	{
		if (isChamberOwned() || mBroken) return false;
		if (isOpen() || isOpening()) return true;
		return open();
	}

	bool Door::requestClose()
	{
		if (isChamberOwned() || mBroken || mOpenLeaseCount != 0 || mObstructed) return false;
		if (isClosed() || isClosing()) return true;
		return close();
	}

	void Door::update(float frameTime)
	{
		// Airlock Doors are shared by two Sector object lists. Only their
		// coordinator advances them, exactly once per simulated tick.
		if (!isChamberOwned()) advanceCoordinatedMotion(frameTime);
	}

	void Door::advanceCoordinatedMotion(float frameTime)
	{
		if (mBroken) return;
		if (isOpening())
		{
			mOpenPct = std::min(mOpenPct + frameTime / getOpenCloseTime(), 1.0f);
			if (mOpenPct >= 1.0f)
			{
				mOpenPct = 1.0f;
				mState = State::Open;
				mOpenWaitTime = getTimeBeforeClosing();
			}
		}
		else if (isClosing())
		{
			if (mOpenLeaseCount != 0 || mObstructed)
			{
				requestOpen();
				return;
			}
			mOpenPct = std::max(mOpenPct - frameTime / getOpenCloseTime(), 0.0f);
			if (mOpenPct <= 0.0f)
			{
				mOpenPct = 0.0f;
				mState = State::Closed;
			}
		}
		else if (isOpen() && mOpenLeaseCount == 0 && !mObstructed)
		{
			mOpenWaitTime -= frameTime;
			if (mOpenWaitTime <= 0.0f) requestClose();
		}
	}

	std::string Door::getDescription() const { return "Door"; }
	float Door::getDefaultSpeed() const
	{
		// Retain legacy default durations for horizontal styles; vertical leaves
		// share the regular Door's physical speed regardless of height.
		float travel = mOpenStyle == OpenStyle::OpenUp ? CORE_DOOR_HEIGHT : getSize().x;
		if (mOpenStyle == OpenStyle::OpenApart) travel *= 0.5f;
		return travel / CORE_DOOR_OPEN_CLOSE_TIME;
	}

	float Door::getOpenCloseTime() const
	{
		float travel = mOpenStyle == OpenStyle::OpenUp ? getSize().y : getSize().x;
		if (mOpenStyle == OpenStyle::OpenApart) travel *= 0.5f;
		return travel / getSpeed();
	}
	float Door::getTimeBeforeClosing() const { return mHoldOpenTime; }
	void Door::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}
}
