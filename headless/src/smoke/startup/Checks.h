#pragma once

#include "Smoke.h"

#include <vector>

namespace startup_smoke
{
	void registerChecks(std::vector<smoke::Check>& checks);
	// Exercise the same child lifecycle with a one-second synthetic budget.
	void registerSyntheticTimeoutCheck(std::vector<smoke::Check>& checks);
}
