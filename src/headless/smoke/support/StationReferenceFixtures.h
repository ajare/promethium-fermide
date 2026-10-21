#pragma once
#include "core/StationReference.h"

// Test-only access to authored reference YAML, independent of the working
// directory. Historical accessor names keep physics assertions unchanged;
// production configuration loading belongs to StationConfigurationResource.
namespace core
{
	StationReferenceDocument const& bundledStationReference();
	std::string const& bundledStationReferenceText();

	struct BundledStationScenario
	{
		std::string id;
		std::string text;
		StationReferenceDocument document;
	};
	// The four original emergency/recovery acceptance fixtures, in test order.
	std::vector<BundledStationScenario> const& bundledStationScenarios();
	StationReferenceDocument const& bundledStationLongRunWorkload();
	std::string const& bundledStationLongRunWorkloadText();
}
