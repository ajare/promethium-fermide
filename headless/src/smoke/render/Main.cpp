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
		{ "agentPathTargets", isolated<agentPathTargets> },
		{ "carriageDoors", isolated<carriageDoors> },
		{ "carriageImages", isolated<carriageImages> },
	};
	render_smoke::registerAgentPoses(checks);
	render_smoke::registerFontMetrics(checks);
	render_smoke::registerAirlocks(checks);
	render_smoke::registerFurniture(checks);
	render_smoke::registerSecurityScanners(checks);
	render_smoke::registerBoothWindows(checks);
	render_smoke::registerObjectAtlas(checks);
	render_smoke::registerFurnitureAtlas(checks);
	render_smoke::registerSectorAtlas(checks);
	render_smoke::registerSectorSurfaceDrawContract(checks);
	render_smoke::registerSectorSurfacePerCellResolution(checks);
	render_smoke::registerSectorSurfaceBottomClip(checks);
	render_smoke::registerSectorSurfaceVariants(checks);
	render_smoke::registerDoorframeCells(checks);
	render_smoke::registerWindowframeCells(checks);
	render_smoke::registerSectorTilePreview(checks);
	render_smoke::registerAgentAtlas(checks);
	render_smoke::registerWalkwayArtwork(checks);
	render_smoke::registerBulkheadAndPanelArtwork(checks);
	render_smoke::registerObjectArtworkNew(checks);
	render_smoke::registerObjectAtlasContract(checks);
	render_smoke::registerAtlasInstallPath(checks);
	render_smoke::registerVehicleArtworkNew(checks);
	render_smoke::registerTransportDoorLeaves(checks);
	render_smoke::registerTallDoorLeaves(checks);
	render_smoke::registerAccessPanels(checks);
	render_smoke::registerDrawOrder(checks);
	render_smoke::registerDoorOpenApart(checks);
	render_smoke::registerDoorOpenLeft(checks);
	render_smoke::registerBrokenExtensibles(checks);
	render_smoke::registerBrokenEscalators(checks);
	render_smoke::registerBrokenLifts(checks);
	render_smoke::registerBrokenPlatformLifts(checks);
	render_smoke::registerBrokenShuttles(checks);
	render_smoke::registerDoorOpenRight(checks);
	render_smoke::registerUnplannedDoorStyle(checks);
	render_smoke::registerFacades(checks);
	render_smoke::registerFacadeDrawOrder(checks);
	render_smoke::registerViewportCulling(checks);
	render_smoke::registerViewportDragScroll(checks);
	render_smoke::registerViewportZoom(checks);
	render_smoke::registerLifetime(checks);
	render_smoke::registerDoorButtons(checks);
	render_smoke::registerSerializationRendering(checks);
	render_smoke::registerScaledDoors(checks);
	render_smoke::registerFloorDepthLift(checks);
	render_smoke::registerFloorDepthAttached(checks);
	render_smoke::registerFloorDepthPicking(checks);
	render_smoke::registerFloorDepthWorldClip(checks);
	render_smoke::registerFloorDepthGraphOverlay(checks);
	return smoke::main("render", checks, argc, argv);
}
