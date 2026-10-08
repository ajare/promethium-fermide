#include "Checks.h"
#include "../agent/Checks.h"
#include "../tags/Checks.h"
#include "../behaviours/Checks.h"
#include "../permissions/Checks.h"
#include "../routing/Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	agent_smoke::registerGroupEditor(checks);
	agent_smoke::registerGroupAssignmentEditor(checks);
	agent_smoke::registerGroupCountEditor(checks);
	agent_smoke::registerGroupDeleteEditor(checks);
	agent_smoke::registerGroupIdAllocationEditor(checks);
	agent_smoke::registerGroupTopologyEditor(checks);
	agent_smoke::registerActivationEditor(checks);
	agent_smoke::registerGroupClipboardEditor(checks);
	agent_smoke::registerAgentTypeEditor(checks);
	agent_smoke::registerColourEditor(checks);
	agent_smoke::registerWalkSpeedEditor(checks);
	agent_smoke::registerHeightEditor(checks);
	agent_smoke::registerPoseEditor(checks);
	tag_smoke::registerRegistryEditor(checks);
	tag_smoke::registerAssignmentEditor(checks);
	tag_smoke::registerRegistryChangeEditor(checks);
	tag_smoke::registerDocumentSaveEditor(checks);
	tag_smoke::registerReloadEditor(checks);
	tag_smoke::registerDeleteEditor(checks);
	tag_smoke::registerCoordinationEditor(checks);
	tag_smoke::registerClipboardEditor(checks);
	behaviour_smoke::registerRegistryEditor(checks);
	behaviour_smoke::registerAssignmentEditor(checks);
	behaviour_smoke::registerPortabilityEditor(checks);
	behaviour_smoke::registerDeleteEditor(checks);
	behaviour_smoke::registerSchemaReconciliationEditor(checks);
	behaviour_smoke::registerWorkflowEditor(checks);
	permission_smoke::registerLocationEditor(checks);
	permission_smoke::registerLocationLifecycleEditor(checks);
	permission_smoke::registerLocationPlacementEditor(checks);
	permission_smoke::registerDestinationEditor(checks);
	permission_smoke::registerAccessEditor(checks);
	permission_smoke::registerAdherenceEditor(checks);
	permission_smoke::registerLandingAdherenceEditor(checks);
	editor_smoke::registerEscalators(checks);
	routing_smoke::registerPlanningEditor(checks);
	routing_smoke::registerPlanningTimeEditor(checks);
	editor_smoke::registerAirlocks(checks);
	editor_smoke::registerFurniture(checks);
	editor_smoke::registerMarkerActions(checks);
	editor_smoke::registerSecurityScanners(checks);
	editor_smoke::registerBoothWindows(checks);
	editor_smoke::registerAccessPanels(checks);
	editor_smoke::registerBackground(checks);
	editor_smoke::registerFacade(checks);
	editor_smoke::registerBrokenExtensibles(checks);
	editor_smoke::registerBrokenEscalators(checks);
	editor_smoke::registerBrokenLifts(checks);
	editor_smoke::registerBrokenPlatformLifts(checks);
	editor_smoke::registerBrokenShuttles(checks);
	editor_smoke::registerDoorPanel(checks);
	editor_smoke::registerPalette(checks);
	editor_smoke::registerHistory(checks);
	editor_smoke::registerIsolation(checks);
	editor_smoke::registerScaledDoors(checks);
	editor_smoke::registerRoomHeightScale(checks);
	return smoke::main("editor", checks, argc, argv);
}
