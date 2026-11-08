#include "Checks.h"

#include "game/WorldRuntime.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerGameRuntimeOwnership(checks);
	registerGameRuntimeTicking(checks);
	registerGameRuntimeLifecycle(checks);
	registerGameRuntimeObservation(checks);
	registerGameRuntimePlayerInput(checks);
	registerGameRuntimeRenderView(checks);
	registerGameRuntimeConstReadSurface(checks);
	registerGameRuntimeReadSurface(checks);
	registerGameRuntimeSharedLibrary(checks);
	return smoke::main("game-runtime", checks, argc, argv);
}
