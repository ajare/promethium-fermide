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
		// Prefix for auto-allocated instance ids: the first free instance of a
		// template with prefix "ac" is "ac1", giving "ac1.machine". Optional in
		// the template, where it defaults to the prefab id itself.
		std::string instancePrefix;
		StationSystemsSettings entries;
		std::vector<PrefabExposedPort> exposedPorts;
		std::vector<PrefabParameter> parameters;
	};

	// One instance override of an exposed parameter. The value's alternative
	// must match the dial's declared kind.
	using PrefabOverrideValue = std::variant<double, bool>;
	using PrefabOverrides = std::map<std::string, PrefabOverrideValue>;

	// #814: the provenance block placement writes into the station
	// configuration document: which template, which instance and which
	// overrides produced the flattened entries. The expansion is the truth
	// and stands on its own; this block is editor metadata used to re-group
	// the composite on load, and is inert wherever the template is absent.
	struct PrefabProvenance
	{
		std::string prefabId;
		std::string instanceId;
		PrefabOverrides overrides;
		bool operator==(PrefabProvenance const&) const = default;
	};

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

	// A loaded set of templates. The catalogue is transactional as a whole: one
	// malformed template refuses the entire document, so a half-loaded catalogue
	// — where a station silently loses some of its units — cannot exist.
	struct PrefabCatalogue
	{
		std::vector<PrefabTemplate> templates;

		PrefabTemplate const* find(std::string const& id) const;
	};

	// Parse a catalogue document: a 'prefabCatalogue' root holding a 'prefabs'
	// array of template maps. Refuses the whole catalogue on the first malformed
	// template or a duplicate template id.
	bool parsePrefabCatalogue(std::string const& yaml, PrefabCatalogue& out,
		std::string* diagnostic = nullptr);

	// The (nodeType, entryId) pairs of a settings object in the fixed order the
	// prefab machinery walks them. Callers that must lay expansion entries out
	// deterministically (the editor's placement offsets) use this rather than
	// inventing their own order.
	std::vector<std::pair<std::string, std::string>> prefabEntryOrder(
		StationSystemsSettings const& settings);

	// The template's declared dial defaults as an override map, so a freshly
	// placed instance lands at the values its dials advertise.
	PrefabOverrides defaultPrefabOverrides(PrefabTemplate const& tmpl);

	// The next instance id for `tmpl` that claims no entry id already present
	// in `draft`: `prefix` + 1, 2, 3, ...
	std::string allocatePrefabInstanceId(PrefabTemplate const& tmpl,
		StationSystemsSettings const& draft);

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
		// The overrides the instance was expanded with, carried so the
		// editor can persist the provenance block (#814) without
		// re-deriving it from the template.
		PrefabOverrides overrides;
	};

	// #814: rebuild the composite boundary record from persisted
	// provenance: the exposed-pin aliases are re-derived for the saved
	// instance id, and the saved overrides are validated against the
	// current template by re-expanding. A refusal (overrides the template
	// no longer accepts, an instance id the template would reject) leaves
	// `out` untouched: the flattened entries still load as ordinary
	// nodes, the unit simply does not re-group.
	bool reconstructPlacedPrefab(PrefabTemplate const& tmpl,
		PrefabProvenance const& provenance, PlacedPrefab& out, std::string& reason);

	// #817: live dial editing on a placed instance. The new value is
	// validated against the exposed parameter's declared kind and range,
	// then written atomically: the namespaced internal field in `draft` and
	// `placed.overrides` move together, or neither does. The write happens
	// on a candidate copy that runs through the existing configuration
	// validator in the same before/after shape the link evaluator uses -
	// the dial must not be the change that breaks an otherwise valid
	// draft, and a draft already invalid in exactly the same way is not
	// this edit's fault. Unknown dials, kind mismatches, non-finite or
	// out-of-range values, and an instance that no longer owns the mapped
	// internal entry are all refused with a diagnostic and no mutation.
	bool applyPrefabParameterOverride(StationSystemsSettings& draft,
		PrefabTemplate const& tmpl, PlacedPrefab& placed,
		std::string const& parameterName, PrefabOverrideValue const& value,
		std::string& reason);

	// #817: the instance's current value for one exposed dial, read from
	// the expanded internal entry itself - the truth the unit is running -
	// so the inspector never shows a dial reading the unit does not hold.
	// False when the parameter or its mapped entry/field is not in the
	// draft; `out` is untouched then.
	bool readPrefabParameterValue(StationSystemsSettings const& draft,
		PrefabTemplate const& tmpl, std::string const& instanceId,
		std::string const& parameterName, PrefabOverrideValue& out);

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

	// #815: whole-unit rename. A placed instance is renamed as one unit:
	// every namespaced internal id and every reference to it - from inside
	// the box and from outside nodes - is rewritten together, or nothing is.
	// The plan carries the instance's entries and the identities of every
	// reference the rename touches, so a confirmation and the mutation use
	// one core-owned view of the draft and a stale plan is refused.
	struct PrefabInstanceRename
	{
		std::string oldInstanceId;
		std::string newInstanceId;
		// (nodeType, old namespaced id) of every entry under the namespace,
		// in the fixed prefab walk order.
		std::vector<std::pair<std::string, std::string>> entries;
		// Every authored-link identity naming one of those entries at either
		// end: the internal plumbing and the outside references alike.
		std::vector<std::string> linkIdentities;
		bool operator==(PrefabInstanceRename const&) const = default;
	};

	// Validate a whole-unit rename against the draft: the new instance id
	// must be non-empty, must not contain the namespacing separator, must
	// differ from the old one, must not already own any entry, and must
	// keep every rewritten id inside the entry-id limit. The old instance
	// must own at least one entry. Refusals carry a diagnostic.
	bool planPrefabInstanceRename(StationSystemsSettings const& draft,
		std::string const& oldInstanceId, std::string const& newInstanceId,
		PrefabInstanceRename& plan, std::string* diagnostic = nullptr);

	// Apply the rename atomically: the plan is re-checked against the live
	// draft, then every internal entry is renamed through the same core
	// rename machinery the editor uses, on a candidate copy. Any failure
	// leaves the draft untouched.
	bool renamePrefabInstance(StationSystemsSettings& draft,
		PrefabInstanceRename const& plan, std::string& reason);

	// #815: whole-unit cascade delete. The plan names every entry under
	// the instance namespace and every authored link that touches the
	// expansion, so one confirmation covers the whole removal and the
	// apply cannot leave an orphaned internal or a dangling reference.
	struct PrefabInstanceRemoval
	{
		std::string instanceId;
		std::vector<std::pair<std::string, std::string>> entries;
		std::vector<std::string> linkIdentities;
		bool operator==(PrefabInstanceRemoval const&) const = default;
	};

	bool planPrefabInstanceRemoval(StationSystemsSettings const& draft,
		std::string const& instanceId, PrefabInstanceRemoval& plan,
		std::string* diagnostic = nullptr);

	// Remove the whole expansion atomically on a candidate copy: every
	// entry goes through the core node-removal machinery, which clears its
	// references and its connection properties, so no orphan and no
	// dangling reference survives. A refusal leaves the draft untouched.
	bool removePrefabInstance(StationSystemsSettings& draft,
		PrefabInstanceRemoval const& plan, std::string& reason);

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
