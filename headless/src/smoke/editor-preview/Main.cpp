#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerEditorPreviewDocument(checks);
	registerEditorPreviewParity(checks);
	registerEditorPreviewBoundary(checks);
	registerEditorPreviewAuthoringRoute(checks);
	registerEditorPreviewHistorySeam(checks);
	registerEditorPreviewSimulationControls(checks);
	return smoke::main("editor-preview", checks, argc, argv);
}
