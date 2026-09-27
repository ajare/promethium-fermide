#include "core/Layer.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	Layer::Layer(World const* world, uint32_t cellsWide, uint32_t levelsHigh, uint32_t z)
		: mwWorld(world)
		, mCellsWide(cellsWide)
		, mLevelsHigh(levelsHigh)
		, mZ(z)
	{
		// The World refuses dimensions whose product cannot back a Layer within
		// the cell budget, so this stays 64-bit clear of any wrap; the cast keeps
		// the arithmetic wide on every platform rather than trusting the check.
		mCells.resize(static_cast<size_t>(cellsWide) * levelsHigh);
	}

	uint32_t Layer::getCellsWide() const
	{
		return mCellsWide;
	}

	uint32_t Layer::getLevelsHigh() const
	{
		return mLevelsHigh;
	}

	uint32_t Layer::getZ() const
	{
		return mZ;
	}

	void Layer::validateCellBounds(uint32_t x, uint32_t y) const
	{
		// Bounds checks
		if (x >= mCellsWide)
		{
			throw WorldException(mwWorld, format("Layer::validateCellBounds({}, {}) - x={} is out of bounds", x, y, x));
		}

		if (y >= mLevelsHigh)
		{
			throw WorldException(mwWorld, format("Layer::validateCellBounds({}, {}) - y={} is out of bounds", x, y, y));
		}
	}

	CellDefinition const& Layer::getCellDefinition(uint32_t x, uint32_t y) const
	{
		validateCellBounds(x, y);
		return mCells[static_cast<size_t>(y) * mCellsWide + x];
	}

	CellDefinition& Layer::getCellDefinition(uint32_t x, uint32_t y)
	{
		validateCellBounds(x, y);
		return mCells[static_cast<size_t>(y) * mCellsWide + x];

	}

} // core