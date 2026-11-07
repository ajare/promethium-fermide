#pragma once

#include <string>
#include <memory>

#include "core/Vector2.h"


namespace core
{

	// Intended as a base class for Interactables, Windows, etc.
	class Area
	{
		uint32_t mCellX, mCellY;

		// Offset within the Cell, not global/Location position
		Vector2 mCellOffset;

		Vector2 mSize;

	protected:
		void setCellPosition(uint32_t cellX, uint32_t cellY);

		// Recompute the bounding size in place. A Sector uses this when a derived
		// effective height (for example a one-cell Room's height override) changes
		// its vertical extent without reconstructing the Area.
		void setSize(Vector2 const& size) { mSize = size; }

	public:

		Area(uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height);

		virtual ~Area() = default;

		[[nodiscard]] uint32_t getCellX() const;

		[[nodiscard]] uint32_t getCellY() const;

		[[nodiscard]] Vector2 const& getCellOffset() const;

		[[nodiscard]] Vector2 getPosition() const;

		[[nodiscard]] Vector2 const& getSize() const;

		void getBounds(Vector2& minExtent, Vector2& maxExtent) const;

		virtual void getPhysicalBounds(Vector2& minExtent, Vector2& maxExtent) const;

		[[nodiscard]] bool pointInBounds(float x, float y) const;
	};

} // core
