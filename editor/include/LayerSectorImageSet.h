#pragma once

// The two words the Layers table and the Artwork panel say about a Layer's
// Sector Image set (#1091).
//
// Both are pure functions of the World, and both live here rather than beside
// the widgets that call them for one reason: what they say is the user-visible
// half of the binding, and a check that cannot reach them can only prove the
// World's state and not what the author is told about it. The editor draws
// these strings; the headless editor check asserts them. Keeping them in a
// header the editor owns and the check reads is what stops the two drifting -
// a check that re-typed the format would pass against a panel that had
// changed its wording, which is the same way the pre-#1030 install checks
// proved a helper while the caller went untested.
//
// Neither function decides anything. A Layer's binding is a
// `std::optional<std::string>` on the World, set through the authoring seam;
// these only put that fact into sentences.

#include <string>
#include <cstdint>

#include "core/World.h"

namespace editor
{
	// What a Layer with no Sector binding of its own draws, said in the words
	// the row shows.
	//
	// Naming the World's set rather than saying "inherited" is what makes the
	// entry answer the question the author is actually asking - which sheet
	// is this Layer on today - and makes that same entry the way back: picking
	// it is how a binding comes off, so the state the row shows and the state
	// the row can put back are the same words.
	inline std::string layerSectorInheritLabel(std::string const& worldSectorImageSet)
	{
		return "Inherit (" + worldSectorImageSet + ")";
	}

	// The Layers that bind a Sector Image set of their own, named in Layer
	// order.
	//
	// Empty when every Layer inherits, which is the case the Artwork panel
	// stays quiet about: with nothing overriding, the Sector combo answers
	// "what does this World draw" by itself, and a line saying so would be
	// noise. Non-empty is the case where the combo no longer answers that on
	// its own, which is exactly when the panel has to say why.
	inline std::string layerSectorOverrideSummary(core::World const& world)
	{
		std::string names;
		for (std::uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
		{
			auto const& binding = world.layerSectorImageSet(layer);
			if (!binding || binding->empty()) continue;
			if (!names.empty()) names += ", ";
			names += world.getLayerName(layer);
		}
		if (names.empty()) return {};
		return "Layers with their own Sector sheet: " + names
			+ ". The set above is what every other Layer draws.";
	}
}
