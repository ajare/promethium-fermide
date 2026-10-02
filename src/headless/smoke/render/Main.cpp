#include "Checks.h"
#include "Walls.h"
#include "AgentPaths.h"
#include "Transports.h"

int main(int argc, char** argv)
{
	using render_smoke::isolated;
	std::vector<smoke::Check> checks{
		{ "walls", isolated<[](smoke::Context const&) { runWallRenderSmokeChecks(); }> },
		{ "agentPaths", isolated<agentPaths> },
		{ "carriageDoors", isolated<carriageDoors> },
		{ "carriageImages", isolated<carriageImages> },
	};
	render_smoke::registerDrawOrder(checks);
	render_smoke::registerDoorOpenApart(checks);
	render_smoke::registerDoorOpenLeft(checks);
	render_smoke::registerBrokenExtensibles(checks);
	render_smoke::registerBrokenEscalators(checks);
	render_smoke::registerDoorOpenRight(checks);
	render_smoke::registerFacades(checks);
	render_smoke::registerFacadeDrawOrder(checks);
	render_smoke::registerViewportCulling(checks);
	render_smoke::registerViewportDragScroll(checks);
	render_smoke::registerViewportZoom(checks);
	render_smoke::registerLifetime(checks);
	render_smoke::registerDoorButtons(checks);
	render_smoke::registerSerializationRendering(checks);
	return smoke::main("render", checks, argc, argv);
}
