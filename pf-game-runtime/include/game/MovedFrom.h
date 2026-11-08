#pragma once

#include <stdexcept>
#include <string>

namespace game
{
	// The failure both halves of the game-facing boundary raise when they are
	// driven after the thing they were built over has been moved away (#978).
	//
	// It lives in the header rather than in each .cpp because the two halves
	// have to fail the same way: the same exception type, the same shape of
	// message, the type and the condition both named. Someone grepping for one
	// failure finds the other, and a smoke check can pin the contract instead
	// of taking the prose in the headers at its word.
	//
	// `std::logic_error` rather than an `assert`, because the boundary already
	// throws for the analogous misuse - a `WorldRuntime` refuses a null World
	// at the door with `std::invalid_argument` - and a guard that survives
	// into Release is worth more against a silent bug than a Debug-only one.
	//
	// `inline` and stateless, so it needs no export annotation to cross the
	// shared-library boundary: each consumer gets its own copy of the throw
	// (#976).
	[[noreturn]] inline void refuseMovedFrom(char const* typeAndAccessor)
	{
		throw std::logic_error(std::string("game::") + typeAndAccessor +
			" was called on a value that has been moved from");
	}

	// The dereference behind the guard: hand it the pointer and the accessor
	// that is asking, and it returns the pointee or refuses. The check exists
	// once per dereference helper rather than being written out at every call
	// site, so an accessor cannot forget it. A caller may also call it purely
	// for the check and discard the reference, which is why the return is not
	// `[[nodiscard]]`.
	template <typename T>
	T& requireLive(T* pointer, char const* typeAndAccessor)
	{
		if (!pointer) refuseMovedFrom(typeAndAccessor);
		return *pointer;
	}
}
