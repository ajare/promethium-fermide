#pragma once

#include "core/StationSystems.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core
{
	enum class StationPortDirection { Input, Output };
	// Structural declarations only. Material, unit and hazard compatibility
	// remain the configuration validator's responsibility. Types without a
	// descriptor are projected by Flow but cannot yet be authored there.
	struct PortDescriptor
	{
		std::string_view nodeType;
		std::string_view name;
		StationPortDirection direction;
		std::string_view kind;
		std::string_view counterpartKind;
		size_t maximumLinks; // zero means unbounded
		// What the user has to insert when two ports of this family cannot
		// join directly ("a Transfer or Conversion"). Empty: no hint given.
		std::string_view intermediary{};
		// An optional port is one the node may or may not wire: it is always
		// drawn and reads as unused until connected, so an unwired optional
		// port reads as a choice rather than an omission.
		bool optional{false};
	};
	inline std::span<PortDescriptor const> stationPortDescriptors()
	{
		// A Store's inventory is one material pool with one in and one out
		// face, referenced by as many transfers as the design needs. A
		// Transfer names exactly one source store and exactly one destination
		// store, so each of its faces carries at most one link.
		static constexpr std::array descriptors{
			PortDescriptor{"Store", "Inventory in", StationPortDirection::Input, "inventory", "material", 0, {}},
			PortDescriptor{"Store", "Inventory out", StationPortDirection::Output, "inventory", "material", 0,
				"a Transfer or Conversion"},
			PortDescriptor{"Transfer", "Source", StationPortDirection::Input, "material", "inventory", 1, {}},
			PortDescriptor{"Transfer", "Destination", StationPortDirection::Output, "material", "inventory", 1, {}},
			// A Conversion's growable port sets: up to MaxStationConversionFlows
			// connectors per side, each one a real connector writing one flow
			// of the authored entry. The first slot on each side is required
			// (the validator refuses a conversion without one); the slots the
			// user grows in are optional until wired, so an unwired grown slot
			// reads as a choice rather than an omission. Names carry the 1-based
			// flow position, so a pin never depends on container order of the
			// draft's other entries.
			PortDescriptor{"Conversion", "Conversion input 1", StationPortDirection::Input, "material", "inventory", 1, {}, false},
			PortDescriptor{"Conversion", "Conversion input 2", StationPortDirection::Input, "material", "inventory", 1, {}, true},
			PortDescriptor{"Conversion", "Conversion input 3", StationPortDirection::Input, "material", "inventory", 1, {}, true},
			PortDescriptor{"Conversion", "Conversion input 4", StationPortDirection::Input, "material", "inventory", 1, {}, true},
			PortDescriptor{"Conversion", "Conversion output 1", StationPortDirection::Output, "material", "inventory", 1, {}, false},
			PortDescriptor{"Conversion", "Conversion output 2", StationPortDirection::Output, "material", "inventory", 1, {}, true},
			PortDescriptor{"Conversion", "Conversion output 3", StationPortDirection::Output, "material", "inventory", 1, {}, true},
			PortDescriptor{"Conversion", "Conversion output 4", StationPortDirection::Output, "material", "inventory", 1, {}, true}};
		return descriptors;
	}

	// Growable port sets: a Conversion's input and output flow sets, one to
	// MaxStationConversionFlows connectors each. The floor of one is the
	// configuration validator's own rule, so the set can never be shrunk
	// into a shape the validator would reject.
	enum class StationPortSet { ConversionInputs, ConversionOutputs };

	struct StationPortSetDescriptor
	{
		std::string_view nodeType;
		StationPortSet set;
		std::string_view label; // "inputs" / "outputs", for affordance wording
	};
	inline std::span<StationPortSetDescriptor const> stationPortSetDescriptors()
	{
		static constexpr std::array sets{
			StationPortSetDescriptor{"Conversion", StationPortSet::ConversionInputs, "inputs"},
			StationPortSetDescriptor{"Conversion", StationPortSet::ConversionOutputs, "outputs"}};
		return sets;
	}

	struct StationPortSetStatus
	{
		size_t count{0};
		size_t minimum{1};
		size_t maximum{MaxStationConversionFlows};
		bool canGrow{false};
		bool canShrink{false};
		std::string growReason;   // why growing is refused; empty when allowed
		std::string shrinkReason; // why shrinking is refused; empty when allowed
	};

	// False when the node type carries no such set or names no entry.
	bool stationPortSetStatus(StationSystemsSettings const& settings,
		std::string const& nodeType, std::string const& nodeId,
		StationPortSet set, StationPortSetStatus& status);
	// Add one real connector to the set: an empty flow the user then wires.
	// Refused, with a reason, at the ceiling or for an unknown node.
	bool growStationPortSet(StationSystemsSettings& settings,
		std::string const& nodeType, std::string const& nodeId,
		StationPortSet set, std::string& reason);
	// Remove the newest connector from the set, wired or not. Refused at the
	// floor of one, so a conversion cannot be stripped below what the
	// validator accepts.
	bool shrinkStationPortSet(StationSystemsSettings& settings,
		std::string const& nodeType, std::string const& nodeId,
		StationPortSet set, std::string& reason);

	// Node types the Flow combobox offers, with the prefix their generated
	// ids start from. Derived service nodes are absent: they are never
	// authored, so they never appear as a placement choice.
	struct StationPlaceableType
	{
		std::string_view nodeType;
		std::string_view idPrefix;
	};
	inline std::span<StationPlaceableType const> stationPlaceableTypes()
	{
		static constexpr std::array placeable{
			StationPlaceableType{"Store", "store"},
			StationPlaceableType{"Conversion", "conversion"},
			StationPlaceableType{"Solar", "solar"}};
		return placeable;
	}

	// Derived service nodes: aggregate services with no configuration entry
	// of their own - the shared electrical pool and the per-class cooling
	// services. They draw inert, own no authored connector, and a link
	// naming one is refused as derived rather than as unknown.
	inline bool stationIsDerivedNodeType(std::string_view nodeType)
	{
		static constexpr std::array<std::string_view, 2> derived{"Power", "Cooling service"};
		return std::find(derived.begin(), derived.end(), nodeType) != derived.end();
	}

	// One connector of one authored node, named by configuration identity
	// alone. A pin never carries a container index, so inserting or removing
	// another draft entry can never renumber it.
	struct StationPinRef
	{
		std::string nodeType;
		std::string nodeId;
		std::string portName;
		bool operator==(StationPinRef const&) const = default;
	};

	// A proposed connection between two connectors, in the order the caller
	// saw them. evaluateStationLink and applyStationLink both orient the pair
	// by the descriptors' declared direction, so a caller never has to know
	// which way the drag went.
	struct CandidateLink
	{
		StationPinRef from;
		StationPinRef to;
		bool operator==(CandidateLink const&) const = default;
	};

	struct LinkVerdict
	{
		bool allowed{false};
		std::string reason; // why the link was refused; empty when it is allowed
	};

	// One reference the draft already carries, seen as a graph link. The
	// identity is the owning entry and the reference field behind it, so it
	// survives any reordering of the draft and names exactly what to clear.
	struct AuthoredLink
	{
		std::string identity;
		StationPinRef from;
		StationPinRef to;
		bool operator==(AuthoredLink const&) const = default;
	};

	// Every authored reference the descriptor table can present as a link.
	// This is the enumeration side of the same mapping applyStationLink
	// writes, so no caller re-derives which field a connector stands for.
	std::vector<AuthoredLink> stationAuthoredLinks(StationSystemsSettings const& settings);

	// May these two connectors join? Answered for any caller with no editor
	// context: structural first, from the descriptor table, then semantic,
	// from the configuration validator run against a copy of the draft with
	// the link applied and diffed against the pre-link error set. Only a
	// failure this link causes is reported as the reason, so a half-built
	// draft stays buildable. The table may be more permissive than the
	// validator, never less: nothing passes here that the validator rejects
	// because of this link.
	LinkVerdict evaluateStationLink(StationSystemsSettings const& settings, CandidateLink const& link);

	// Write the reference a link stands for into the draft. Returns false when
	// the pair maps to no reference field, leaving the draft untouched.
	bool applyStationLink(StationSystemsSettings& settings, CandidateLink const& link);
	// Remove the reference behind one authored-link identity. Returns false
	// when the identity names no entry, leaving the draft untouched.
	bool clearStationLink(StationSystemsSettings& settings, std::string const& identity);

	// Resolve and remove an authored Store or Transfer and all its connector
	// references through the same mapping used for links. Unsupported or
	// missing nodes are refused without changing the draft.
	struct StationNodeRemoval
	{
		std::string nodeType, nodeId;
		std::vector<std::string> linkIdentities;
	};
	bool planStationNodeRemoval(StationSystemsSettings const& settings,
		std::string const& nodeType, std::string const& nodeId, StationNodeRemoval& plan);
	bool removeStationNode(StationSystemsSettings& settings, StationNodeRemoval const& plan);
}
