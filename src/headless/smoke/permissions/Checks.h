#pragma once

#include "Smoke.h"
#include <vector>

namespace permission_smoke
{
	void registerLocations(std::vector<smoke::Check>& checks);
	void registerLocationChanges(std::vector<smoke::Check>& checks);
	void registerLocationEditor(std::vector<smoke::Check>& checks);
	void registerAccess(std::vector<smoke::Check>& checks);
	void registerDestinations(std::vector<smoke::Check>& checks);
	void registerDestinationEditor(std::vector<smoke::Check>& checks);
	void registerAccessEditor(std::vector<smoke::Check>& checks);
	void registerAdherence(std::vector<smoke::Check>& checks);
	void registerAdherenceEditor(std::vector<smoke::Check>& checks);
	void registerLandingAdherence(std::vector<smoke::Check>& checks);
	void registerLandingAdherenceEditor(std::vector<smoke::Check>& checks);
	void registerInteractionMobility(std::vector<smoke::Check>& checks);
	void registerInteractionGeometry(std::vector<smoke::Check>& checks);
	void registerPreflight(std::vector<smoke::Check>& checks);
	void registerRefusal(std::vector<smoke::Check>& checks);
}
