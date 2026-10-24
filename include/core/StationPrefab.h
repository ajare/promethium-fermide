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

	// #811: the composite boundary. A placed instance presents its exposed
	// ports as the pins of one composite node: the composite's node type is
	// the prefab id, its node id is the instance id, and only the exposed
	// port names resolve through it. A link naming a composite pin resolves
	// to the aliased internal pin and then runs through the existing link
	// evaluator and writer unchanged, so the verdict, the refusal reason
	// and the reference field written are exactly the direct ones.
	struct PlacedPrefab
	{
		std::string prefabId;
		std::string instanceId;
		std::map<std::string, StationPinRef> exposedPins;
	};

	// Resolve one composite exposed-pin reference to the aliased internal
	// pin. The pin must name this composite (its node type is the prefab id
	// and its node id the instance id); a port the template did not expose
	// is refused, so the boundary never reaches an internal connector the
	// template kept private.
	bool resolveExposedPin(PlacedPrefab const& placed, StationPinRef const& compositePin,
		StationPinRef& resolved, std::string& reason);

	// Resolve both ends of a candidate link against the placed composites.
	// An end that names no placed composite passes through unchanged, so
	// composite and ordinary draft pins mix freely in one link.
	bool resolvePrefabLink(std::vector<PlacedPrefab> const& placed,
		CandidateLink const& link, CandidateLink& resolved, std::string& reason);

	// Evaluate a link through the composite boundary: the ends are
	// resolved first, then the existing evaluator runs behind the
	// resolver untouched - the same verdict and the same refusal reason as
	// the equivalent direct pair.
	LinkVerdict evaluatePrefabLink(StationSystemsSettings const& draft,
		std::vector<PlacedPrefab> const& placed, CandidateLink const& link);

	// Write the reference a link through the composite boundary stands
	// for: the ends are resolved first, then the existing writer runs
	// untouched, so the same reference field lands as for the direct pair.
	bool applyPrefabLink(StationSystemsSettings& draft,
		std::vector<PlacedPrefab> const& placed, CandidateLink const& link,
		std::string& reason);

	// Catalogue-load template validation: expand the template in
	// isolation - the template id standing in for the instance id - with
	// stub peers on the exposed ports, and run the existing configuration
	// validator. Every exposed port must have a legal counterpart
	// connector in the port grammar for its stub peer to present, and
	// must leave room on its aliased connector for the outside link; the
	// isolated expansion must pass the existing validator. A failing
	// template is refused as a whole with a diagnostic.
	bool validatePrefabTemplate(PrefabTemplate const& tmpl, std::string& reason);
}
