#pragma once

#include "core/StationPorts.h"
#include "core/StationSystems.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace core
{
	// Prefabs (#810): authoring-time composite machine templates. A template
	// carries internal entries written in the existing stationSystems entry
	// vocabulary, an exposed-port alias table, and a parameter (dial) mapping
	// table. Expansion flattens a template into ordinary draft entries with
	// namespaced ids; the simulation, the authoritative reader and the
	// validator never learn that prefabs exist.
	inline constexpr char const* PrefabIdSeparator{"."};

	// One exposed port: a composite-facing name aliasing one connector of an
	// internal template entry.
	struct PrefabExposedPort
	{
		std::string name;
		std::string nodeType;
		std::string entryId;
		std::string portName;
		bool operator==(PrefabExposedPort const&) const = default;
	};

	enum class PrefabParameterKind { Number, Boolean };

	// One exposed dial: a parameter mapped to one field of an internal
	// template entry, with its default and permitted numeric range.
	struct PrefabParameter
	{
		std::string name;
		std::string nodeType;
		std::string entryId;
		std::string field;
		PrefabParameterKind kind{PrefabParameterKind::Number};
		double defaultValue{0.0};
		double minValue{0.0};
		double maxValue{0.0};
		bool boolDefault{false};
		bool operator==(PrefabParameter const&) const = default;
	};

	// A parsed prefab template. The internal entries are ordinary
	// StationSystemsSettings read through the authoritative stationSystems
	// reader, so the existing entry format and validation rules apply verbatim.
	struct PrefabTemplate
	{
		std::string id;
		std::string displayName;
		StationSystemsSettings entries;
		std::vector<PrefabExposedPort> exposedPorts;
		std::vector<PrefabParameter> parameters;
	};

	// One instance override of an exposed parameter. The value's alternative
	// must match the dial's declared kind.
	using PrefabOverrideValue = std::variant<double, bool>;
	using PrefabOverrides = std::map<std::string, PrefabOverrideValue>;

	// The result of expanding one template for one instance: namespaced draft
	// entries with all internal links written, the internal links seen as
	// authored-link records, and the exposed-port aliases resolved to
	// namespaced pins.
	struct PrefabExpansion
	{
		StationSystemsSettings entries;
		std::vector<AuthoredLink> links;
		std::map<std::string, StationPinRef> exposedPins;
	};

	// Parse a prefab template document. A malformed or invalid template is
	// refused as a whole with a diagnostic: internal entries are read through
	// the authoritative stationSystems reader and validated, template entry
	// ids containing the namespacing separator are refused, and every exposed
	// port and parameter must resolve to a real internal entry and connector.
	bool parsePrefabTemplate(std::string const& yaml, PrefabTemplate& out,
		std::string* diagnostic = nullptr);

	// Pure expansion: namespaced entries, internal links and exposed pins,
	// with overrides applied to their mapped internal fields. Unknown
	// override names, kind mismatches and out-of-range values are refused.
	// Deterministic: identical inputs produce identical outputs.
	bool expandPrefab(PrefabTemplate const& tmpl, PrefabOverrides const& overrides,
		std::string const& instanceId, PrefabExpansion& out, std::string& reason);

	// Per-type cap pre-check: count every slot the template consumes against
	// the draft's current counts and the per-type ceilings. Any shortfall
	// refuses the whole expansion with a reason naming the type.
	bool checkPrefabCaps(PrefabTemplate const& tmpl,
		StationSystemsSettings const& draft, std::string& reason);

	// Merge an expansion into the draft atomically: the per-type cap
	// pre-check and an id-collision check run against a candidate copy, so a
	// refusal leaves the draft untouched.
	bool applyPrefabExpansion(StationSystemsSettings& draft,
		PrefabExpansion const& expansion, std::string& reason);
}
