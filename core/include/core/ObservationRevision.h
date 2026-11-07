#pragma once

#include <cstdint>

namespace core
{
	// Conservative invalidation for authored child edits and registry structure.
	// Simulation and editing are single-threaded. This is not document state.
	// An edit in another World may invalidate a projection, but never changes it.
	inline uint64_t observationRevision = 0;
}
