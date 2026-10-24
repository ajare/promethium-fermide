#pragma once

#include "core/StationSystems.h"
#include "core/StationPrefab.h"
#include <map>
#include <string>
#include <utility>

namespace core
{
	// Presentation metadata is deliberately not part of StationSystemsSettings.
	struct StationConfigurationDocument
	{
		StationSystemsSettings settings;
		// #814: the provenance blocks written by prefab placement. Editor
		// metadata only - the flattened expansion in `settings` is the
		// truth, and this block lets the editor re-group each placed unit
		// as a composite on load. It is inert when the template is absent.
		std::vector<PrefabProvenance> prefabs;
		std::map<std::string, std::pair<float, float>> positions;
		float panX{24}, panY{24}, zoom{0.8f};
	};

	std::string serializeStationConfiguration(StationConfigurationDocument const& document);
	bool parseStationConfiguration(std::string const& yaml, StationConfigurationDocument& document,
		std::string* diagnostic = nullptr);
	void saveStationConfiguration(std::string const& path, StationConfigurationDocument const& document);
	bool loadStationConfiguration(std::string const& path, StationConfigurationDocument& document,
		std::string* diagnostic = nullptr);
}
