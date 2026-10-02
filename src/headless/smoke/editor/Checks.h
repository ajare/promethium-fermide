#pragma once

#include "Smoke.h"
#include <vector>

namespace editor_smoke
{
	void registerBackground(std::vector<smoke::Check>& checks);
	void registerFacade(std::vector<smoke::Check>& checks);
	void registerDoorPanel(std::vector<smoke::Check>& checks);
	void registerBrokenExtensibles(std::vector<smoke::Check>& checks);
	void registerPalette(std::vector<smoke::Check>& checks);
	void registerHistory(std::vector<smoke::Check>& checks);
	void registerEscalators(std::vector<smoke::Check>& checks);
	void registerIsolation(std::vector<smoke::Check>& checks);
}
