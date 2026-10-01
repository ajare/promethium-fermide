#include "Walls.h"
#include "Smoke.h"

namespace
{
	void walls(smoke::Context const&)
	{
		runWallRenderSmokeChecks();
	}

	constexpr smoke::Check checks[] = {
		{ "walls", walls },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("render", checks, argc, argv);
}
