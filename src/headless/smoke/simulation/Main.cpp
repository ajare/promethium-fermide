#include "Observation.h"
#include "Smoke.h"

namespace
{
	void observation(smoke::Context const&)
	{
		runSimulationObservationSmokeChecks();
	}

	constexpr smoke::Check checks[] = {
		{ "observation", observation },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("simulation", checks, argc, argv);
}
