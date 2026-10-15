#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/Serializer.h"
#include "core/StationSystems.h"

namespace core
{
	// Read the complete "stationSystems" map from the current serializer
	// scope with the authoritative World document reader (#422). Shared by
	// the World document reader and the packaged reference station parser
	// so the authored station configuration format has exactly one
	// reader; the map is validated on close and a malformed or physically
	// inconsistent map throws SerializationException.
	void readStationSystemsSettingsMap(Serializer& serializer,
		std::uint32_t version, StationSystemsSettings& settings);

	// One documented coefficient of the packaged reference station: what
	// the coefficient is, the value as authored, the units it is read in,
	// and where the number comes from (#422).
	struct StationReferenceProvenance
	{
		std::string coefficient;
		std::string value;
		std::string units;
		std::string source;
		bool operator==(StationReferenceProvenance const&) const = default;
	};

	// The packaged reference station document (#422): one reconciled,
	// editable configuration that demonstrates the implemented Station
	// systems with internally consistent source/load/thermal budgets and
	// starting inventories, carried together with the coefficient
	// provenance, the provisional biological/process/thermal assumptions
	// and the qualification limits that scope what the configuration
	// claims. The settings are ordinary StationSystemsSettings: the
	// reference is the same authored configuration the editor edits and
	// the World persists, never a parallel configuration or clock.
	struct StationReferenceDocument
	{
		std::string name;
		std::string summary;
		// The World schema version the settings section is read under.
		std::uint32_t schemaVersion{0};
		std::vector<StationReferenceProvenance> provenance;
		// Named assumption sections: topic -> documented text. A list of
		// pairs rather than a map so the documented order is stable.
		std::vector<std::pair<std::string, std::string>> assumptions;
		std::string qualification;
		// The documented reproducible normal-operation scenario length,
		// in simulated seconds, and its notes.
		double scenarioSeconds{3600.0};
		std::string scenarioNotes;
		StationSystemsSettings settings;
	};

	// Parse a reference station document. The "stationSystems" section is
	// read through the authoritative World station systems reader, so the
	// reference configuration format and the persisted World format never
	// diverge. Returns false with a diagnostic on a malformed or
	// physically inconsistent document; a deliberately short inventory
	// (an intentional operational shortage) is a valid document.
	bool parseStationReference(std::string const& text,
		StationReferenceDocument& document, std::string* diagnostic);

	// The reference station packaged with the application resource,
	// parsed once from the embedded resource text on first use. Throws
	// std::runtime_error if the packaged resource is broken: the bundle
	// itself failing to parse is a build defect, never a user error.
	StationReferenceDocument const& bundledStationReference();
	// The raw packaged resource text, for display and re-editing.
	std::string const& bundledStationReferenceText();
}
