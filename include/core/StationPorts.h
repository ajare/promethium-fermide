#pragma once

#include "core/StationSystems.h"

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
			PortDescriptor{"Transfer", "Destination", StationPortDirection::Output, "material", "inventory", 1, {}}};
		return descriptors;
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
