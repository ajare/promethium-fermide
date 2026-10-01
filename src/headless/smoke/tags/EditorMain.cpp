#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	tag_smoke::registerRegistryEditor(checks);
	tag_smoke::registerAssignmentEditor(checks);
	tag_smoke::registerRegistryChangeEditor(checks);
	tag_smoke::registerDocumentSaveEditor(checks);
	tag_smoke::registerReloadEditor(checks);
	tag_smoke::registerDeleteEditor(checks);
	tag_smoke::registerCoordinationEditor(checks);
	tag_smoke::registerClipboardEditor(checks);
	return smoke::main("agent-tags-editor", checks, argc, argv);
}
