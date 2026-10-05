#pragma once

#include "Smoke.h"
#include <vector>

namespace editor_smoke
{
	void registerAccessPanels(std::vector<smoke::Check>& checks);
	void registerBoothWindows(std::vector<smoke::Check>& checks);
	void registerAirlocks(std::vector<smoke::Check>& checks);
	void registerSecurityScanners(std::vector<smoke::Check>& checks);
	void registerBackground(std::vector<smoke::Check>& checks);
	void registerFacade(std::vector<smoke::Check>& checks);
	void registerDoorPanel(std::vector<smoke::Check>& checks);
	void registerBrokenExtensibles(std::vector<smoke::Check>& checks);
	void registerBrokenLifts(std::vector<smoke::Check>& checks);
	void registerBrokenPlatformLifts(std::vector<smoke::Check>& checks);
	void registerBrokenShuttles(std::vector<smoke::Check>& checks);
	void registerBrokenEscalators(std::vector<smoke::Check>& checks);
	void registerPalette(std::vector<smoke::Check>& checks);
	void registerHistory(std::vector<smoke::Check>& checks);
	void registerEscalators(std::vector<smoke::Check>& checks);
	void registerIsolation(std::vector<smoke::Check>& checks);
}

namespace editor_smoke { void registerFurniture(std::vector<smoke::Check>& checks); }
