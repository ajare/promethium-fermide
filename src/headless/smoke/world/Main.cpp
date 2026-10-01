#include "Checks.h"
#include "Smoke.h"
#include <iterator>

namespace
{
	void layers(smoke::Context const&) { runEditorLayerSmokeChecks(); }
	void backgroundSector(smoke::Context const&) { runBackgroundSectorSmokeChecks(); }
	void backgroundPaint(smoke::Context const&) { runBackgroundPaintSmokeChecks(); }
	void backgroundPlacement(smoke::Context const&) { runBackgroundPlacementSmokeChecks(); }
	void backgroundCascadeDelete(smoke::Context const&) { runBackgroundCascadeDeleteSmokeChecks(); }
	void windowLayers(smoke::Context const&) { runWindowLayerSmokeChecks(); }
	void windowIntoBackground(smoke::Context const&) { runWindowIntoBackgroundSmokeChecks(); }
	void windowMultiBackground(smoke::Context const&) { runWindowMultiBackgroundSmokeChecks(); }
	void facades(smoke::Context const&) { runFacadeSmokeChecks(); }
	void zeroSizeLocations(smoke::Context const&) { runZeroSizeLocationSmokeChecks(); }
	void thresholdLayerOverlap(smoke::Context const&) { runThresholdLayerOverlapSmokeChecks(); }

	constexpr smoke::Check existingChecks[] = {
		{ "marker-identity", [](smoke::Context const&) { runMarkerIdentitySmokeChecks(); } },
		{ "two-sided-buttons", [](smoke::Context const&) { runDoorTwoSidedButtonSmokeChecks(); } },
		{ "layers", layers },
		{ "background-sector", backgroundSector },
		{ "background-paint", backgroundPaint },
		{ "background-placement", backgroundPlacement },
		{ "background-cascade-delete", backgroundCascadeDelete },
		{ "window-layers", windowLayers },
		{ "window-into-background", windowIntoBackground },
		{ "window-multi-background", windowMultiBackground },
		{ "facades", facades },
		{ "zero-size-locations", zeroSizeLocations },
		{ "threshold-layer-overlap", thresholdLayerOverlap },
	};
}

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks(std::begin(existingChecks), std::end(existingChecks));
	registerObjectEditing(checks);
	registerFloorsAndWalls(checks);
	registerTopology(checks);
	registerLayerDeletion(checks);
	return smoke::main("world", checks, argc, argv);
}
