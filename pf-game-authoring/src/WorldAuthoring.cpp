#include "game/WorldAuthoring.h"

#include <utility>

namespace game
{
	WorldAuthoring::WorldAuthoring(WorldRuntime& runtime) noexcept
		: runtime_(&runtime)
	{
	}

	WorldAuthoring::~WorldAuthoring() = default;

	WorldAuthoring::WorldAuthoring(WorldAuthoring&& other) noexcept
		: runtime_(other.runtime_)
	{
		other.runtime_ = nullptr;
	}

	WorldAuthoring& WorldAuthoring::operator=(WorldAuthoring&& other) noexcept
	{
		if (this != &other)
		{
			runtime_ = other.runtime_;
			other.runtime_ = nullptr;
		}
		return *this;
	}

	core::World& WorldAuthoring::world() noexcept
	{
		// The one place the mutable World is taken. `WorldRuntime::authoringWorld`
		// is private and granted to this class alone, so the privilege does not
		// come from a cast that any caller could repeat (#967).
		return runtime_->authoringWorld();
	}

	WorldRuntime& WorldAuthoring::runtime() noexcept
	{
		return *runtime_;
	}

	WorldRuntime const& WorldAuthoring::runtime() const noexcept
	{
		return *runtime_;
	}
}
