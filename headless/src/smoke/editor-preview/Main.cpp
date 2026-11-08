#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerEditorPreviewDocument(checks);
	registerEditorPreviewParity(checks);
	registerEditorPreviewBoundary(checks);
	registerEditorPreviewAuthoringRoute(checks);
	return smoke::main("editor-preview", checks, argc, argv);
}
