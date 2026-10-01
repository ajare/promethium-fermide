#pragma once

#include "Smoke.h"
#include <vector>

namespace tag_smoke
{
	void registerRegistry(std::vector<smoke::Check>& checks);
	void registerAssignment(std::vector<smoke::Check>& checks);
	void registerReconciliation(std::vector<smoke::Check>& checks);
	void registerMobilityProfile(std::vector<smoke::Check>& checks);
	void registerRegistryEditor(std::vector<smoke::Check>& checks);
	void registerAssignmentEditor(std::vector<smoke::Check>& checks);
	void registerRegistryChangeEditor(std::vector<smoke::Check>& checks);
	void registerDocumentSaveEditor(std::vector<smoke::Check>& checks);
	void registerReloadEditor(std::vector<smoke::Check>& checks);
	void registerDeleteEditor(std::vector<smoke::Check>& checks);
	void registerCoordinationEditor(std::vector<smoke::Check>& checks);
	void registerClipboardEditor(std::vector<smoke::Check>& checks);
}
