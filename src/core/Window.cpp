#include <cassert>

#include "core/Defines.h"
#include "core/Window.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Window
	------

	This class represents a Window in a Location.  It may be on either Layer, and may have either another
	Location behind it, or nothing (ie looking into the void/space).

	This class is similar to Door in that it can in theory be opened, but because it has some extra states,
	due to extra styles, it needs to be implemented as a separate Shape subclass, and not an OpenableObject.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	- locations[2] is the fore and back Location (front first, adjacent layer behind second)
	*/
	Window::Window(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh, shared_ptr<const Sector> sectors[2])
		: Object((float)cellX + CORE_WINDOW_X_INSET, (float)cellY + CORE_WINDOW_Y_OFFSET, cellsWide - CORE_WINDOW_X_INSET * 2.0f, (levelsHigh - 1) + CORE_WINDOW_HEIGHT)
		, mCellsWide(cellsWide)
		, mLevelsHigh(levelsHigh)
		, mState(State::Closed)
		, mStyle(Style::Clear)
		, mSectors{ sectors[0], sectors[1] }
	{
	}

	/***

	getCellsWide()
	--------------

	Get the width of the Window, in cells.
	*/
	uint32_t Window::getCellsWide() const
	{
		return mCellsWide;
	}

	/***

	getLevelsHigh()
	--------------

	Get the height of the Window, in cells.
	*/
	uint32_t Window::getLevelsHigh() const
	{
		return mLevelsHigh;
	}

	/***

	getState()
	----------

	Get the state of the Window.  Similar to Door, but there are some extra states
	to deal with the different styles.  For instance, moving from untinted to tinted.
	*/
	Window::State const& Window::getState() const
	{
		return mState;
	}

	/***

	getStyle()
	----------

	While Windows are generally clear, some may be tinted of frosted.
	*/
	Window::Style Window::getStyle() const
	{
		return mStyle;
	}

	/***

	getSector()
	-----------

	Get the Location, for the given Layer.
	*/
	shared_ptr<const Sector> Window::getSector(uint32_t pairSide) const
	{
		ASSERT_PAIR_SIDE_OK(pairSide);

		return mSectors[pairSide].lock();
	}

	uint32_t Window::getFrontLayer() const
	{
		auto const front = mSectors[0].lock();
		return front ? front->getLayerIndex() : ~0u;
	}

	uint32_t Window::getBackLayer() const
	{
		auto const back = mSectors[1].lock();
		return back ? back->getLayerIndex() : ~0u;
	}

	void Window::setState(State state, Style style)
	{
		if (isBoothWindow() && static_cast<BoothWindow const*>(this)->getDumbwaiterOwner())
			throw invalid_argument("Dumbwaiter-owned shutters cannot be independently edited");
		if (isBoothWindow() && ((state != State::Open && state != State::Closed) || style != Style::Clear))
			throw invalid_argument("BoothWindow supports only Open or Closed shutters without glass styles");
		mState = state;
		mStyle = style;
	}

	void BoothWindow::setState(State state, Style style)
	{
		Window::setState(state, style);
		mTargetOpen = state == State::Open;
		mProgress = mTargetOpen ? 1.0f : 0.0f;
	}

	void BoothWindow::refreshState()
	{
		// Only the runtime device path may assign moving states. Base APIs still
		// reject glass styles and unsupported authored states.
		mState = mTargetOpen ? (mProgress == 1.0f ? State::Open : State::Opening)
			: (mProgress == 0.0f ? State::Closed : State::Closing);
	}

	void Window::configureTraversal(bool enabled, TraversalResourceId resource)
	{
		if (isBoothWindow() && (enabled || resource))
			throw invalid_argument("BoothWindow is never traversable");
		mTraversalConfigured = enabled;
		mTraversalResource = enabled ? resource : TraversalResourceId{};
	}

	bool Window::isNormallyTraversable() const
	{
		return mTraversalConfigured && mState == State::Open && mStyle == Style::Clear;
	}

	std::string Window::getDescription() const
	{
		return "Window";
	}


} // core