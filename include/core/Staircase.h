#pragma once

#include <array>
#include <cstdint>

#include "core/Object.h"

namespace core
{
	// A single straight flight connecting two adjacent levels.
	class Staircase : public Object
	{
		uint32_t mCellsWide;
		int mRiseSide;
		float mSpeed;
		float mAnimationPhase{ 0.0f };
		bool mInitiallyBroken{ false };
		bool mBroken{ false };
		uint32_t mSectorIndex{ ~0u };
		friend class World;

	public:
		Staircase(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int riseSide,
			float speed = 0.0f);
		~Staircase() = default;

		[[nodiscard]] uint32_t getCellsWide() const { return mCellsWide; }
		[[nodiscard]] int getRiseSide() const { return mRiseSide; }
		// Zero is an ordinary staircase; positive moves up and negative moves down.
		[[nodiscard]] float getSpeed() const { return mSpeed; }
		[[nodiscard]] bool isEscalator() const { return mSpeed != 0.0f; }
		bool isBroken() const { return mBroken; }
		bool isInitiallyBroken() const { return mInitiallyBroken; }
		bool isMoving() const { return isEscalator() && !mBroken; }
		uint32_t getSectorIndex() const { return mSectorIndex; }
		// No live remote condition is consulted by route decisions.
		bool routeIsMoving(class Agent const* agent, bool locallyObserved) const;
		[[nodiscard]] uint32_t getStepCount() const { return mCellsWide * 8; }

		// Local coordinates, from the lower corridor endpoint to the upper one.
		[[nodiscard]] std::array<Vector2, 2> getPath() const;

		[[nodiscard]] float getAnimationPhase() const { return mAnimationPhase; }
		void update(float frameTime);

		[[nodiscard]] std::string getDescription() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
