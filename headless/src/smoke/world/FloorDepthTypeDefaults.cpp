// The author's floor depth scheme: one default per Sector type (#1103).
//
// #1101 put a depth on a Sector and made the Layers behind it move. This is
// the knob above that: a table of the eleven Sector types, one signed number
// each, so "all Rooms sit 24px deep, all Corridors sit 8px deep" is two
// numbers rather than one per Room.
//
// The whole feature is a resolution rule and a sparse map, so that is what
// gets pinned here rather than any picture:
//
//   * a type with no entry has no opinion and reads as zero. That is the
//     ordinary case, not an error, and it is what lets a World written before
//     the table existed draw exactly the flat picture it drew before;
//   * a type default moves the effective depth of every Sector following that
//     type, in one write, without the write touching any of those Sectors.
//     The Sectors are read through their type, which is why one number here
//     moves all of them at once;
//   * a Sector that has overridden its type is not moved. That is the whole
//     meaning of an override, and the only thing that can tell "following
//     Rooms at 24" from "Rooms that happen to be at 24 by themselves" is the
//     presence of the Sector's own field - a distinction a dense field
//     destroys on the first write;
//   * the staircase follows. A type default is a depth like any other once
//     resolved, so the accumulated Layer offsets move with it through the
//     same single sum #1101 established, and a Layer lifted by an overridden
//     Sector in front is lifted by the override rather than by the type that
//     Sector would otherwise have followed;
//   * setting a type back to zero removes its entry rather than storing a
//     zero, so a type cleared saves the way it saved before it was ever set;
//   * the band is the same one Level either way as the per-Sector write, and
//     refused rather than clamped - a type default moves every Sector
//     following it, so a mistyped 5000 here flings more of the picture away
//     than one mistyped Sector can;
//   * all eleven types are in the table and all eleven take a write. A scheme
//     with a type missing from the table is one the author has to discover is
//     incomplete by finding the gap.
//
// Everything goes through the World's own queries rather than reaching for
// the Sector objects or the map, because the queries are the seam: the panel,
// the renderer and the shipped game all ask the World, and none of them is
// allowed to reconstruct the arithmetic on its own.

#include "Checks.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	using smoke::require;
	using WorldPtr = std::shared_ptr<core::World>;

	// A World with two Layers and nothing recorded about floor depth at all.
	// The checks place what they need on Layer 0, the front-most, so the
	// Layer behind has something to be lifted by.
	WorldPtr blankScheme(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, 40, 24);
		while (world->getLayerCount() < 2) world->addLayer();
		return world;
	}

	// The one Sector of this type on this Layer, or a failure naming the type
	// and the count. Going through the World's Sector list rather than the
	// index each builder returns keeps the check honest about what is actually
	// in the World.
	uint32_t onlySectorOfType(core::World const& world, core::SectorType type,
		uint32_t layer = 0)
	{
		std::vector<uint32_t> found;
		for (auto const& sector : world.getSectors(layer))
			if (sector->getType() == type) found.push_back(sector->getIndex());
		require(found.size() == 1, "The fixture holds " + std::to_string(found.size())
			+ " Sectors of type " + core::getSectorTypeString(type) + " on Layer "
			+ std::to_string(layer) + "; the check needs exactly one");
		return found.front();
	}

	// ------------------------------------------------------------------
	// Absence.
	// ------------------------------------------------------------------

	// A World whose author never opened the type table: every type answers
	// zero, no type reports an entry, and the staircase is flat.
	void aTypeNobodySetHasNoOpinion(smoke::Context const&)
	{
		auto const world = blankScheme("Untouched scheme");
		for (auto const type : core::allSectorTypes())
		{
			require(!world->hasSectorTypeFloorDepth(type),
				"Type " + core::getSectorTypeString(type) + " reports a recorded "
				"default in a World whose type table was never written to");
			require(world->sectorTypeFloorDepth(type) == 0,
				"Type " + core::getSectorTypeString(type) + " answers "
				+ std::to_string(world->sectorTypeFloorDepth(type))
				+ " with no entry recorded; absence must read as zero");
		}
		for (uint32_t layer = 0; layer < world->getLayerCount(); ++layer)
			require(world->layerFloorOffsetPx(layer) == 0,
				"Layer " + std::to_string(layer) + " draws "
				+ std::to_string(world->layerFloorOffsetPx(layer))
				+ "px off with every type unset");
	}

	// The table covers the whole enum, and every row's name is one the wire
	// format can read back. A scheme that cannot name one of its own types is
	// a scheme that loses a row on save.
	void theTableCoversEverySectorType(smoke::Context const&)
	{
		auto const& types = core::allSectorTypes();
		require(types.size() == 11, "The type table lists " + std::to_string(types.size())
			+ " Sector types; the author's table has eleven rows");
		for (auto const type : types)
		{
			auto const name = core::getSectorTypeString(type);
			auto const back = core::sectorTypeFromString(name);
			require(back.has_value() && *back == type,
				"Sector type name '" + name + "' does not read back as the type "
				"that wrote it, so a saved scheme could lose a row");
		}
		require(!core::sectorTypeFromString("NotAType").has_value(),
			"A name that is not a Sector type read back as one");
	}

	// ------------------------------------------------------------------
	// A type default moves every Sector following it.
	// ------------------------------------------------------------------

	// Setting a type's depth moves the effective depth of the Sectors of that
	// type and of no other. Two Rooms are placed to show one write reaching
	// both of them; a Background is placed to show it reaching nothing else.
	void aTypeDefaultMovesEverySectorFollowingIt(smoke::Context const&)
	{
		auto const world = blankScheme("Rooms at 24");
		world->addRoom("Room A", 0, 2, 1, 3, 2);
		world->addRoom("Room B", 0, 2, 8, 3, 2);
		world->addBackground(0, 2, 15, 3, 2);
		world->finishBuild();

		std::vector<uint32_t> rooms;
		for (auto const& sector : world->getSectors(0))
			if (sector->getType() == core::SectorType::Location)
				rooms.push_back(sector->getIndex());
		require(rooms.size() == 2, "The fixture placed " + std::to_string(rooms.size())
			+ " Rooms; the check needs two to show one write reaching both");

		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Setting the Room type default to 24 was not accepted as a change");
		for (auto const room : rooms)
			require(world->effectiveFloorDepth(room) == 24,
				"Room " + std::to_string(room) + " answers "
				+ std::to_string(world->effectiveFloorDepth(room))
				+ " after the Room type default was set to 24; a Sector with no "
				"override of its own follows its type");

		require(world->effectiveFloorDepth(onlySectorOfType(*world,
			core::SectorType::Background)) == 0,
			"A Background moved under a Room type edit; a type default governs "
			"its own type and nothing else");
	}

	// Two types, two depths, both visible at once: the scheme is a table and
	// not one number with a favourite type.
	void twoTypeDefaultsReadSideBySide(smoke::Context const&)
	{
		auto const world = blankScheme("Two defaults");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->addBackground(0, 2, 8, 3, 2);
		world->addFacade("Facade", 0, 2, 15, 3, 2);
		world->finishBuild();

		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Room default refused");
		require(world->setSectorTypeFloorDepth(core::SectorType::Background, -8).changed,
			"Background default refused");

		require(world->effectiveFloorDepth(onlySectorOfType(*world,
			core::SectorType::Location)) == 24, "Room not following its type");
		require(world->effectiveFloorDepth(onlySectorOfType(*world,
			core::SectorType::Background)) == -8, "Background not following its type");
		require(world->effectiveFloorDepth(onlySectorOfType(*world,
			core::SectorType::Facade)) == 0, "Facade picked up a default it was never given");
	}

	// The staircase follows the type map through the same single sum #1101
	// established: a Layer's contribution is the deepest effective floor in
	// the Layer in front, and a type default is an effective floor like any
	// other.
	void theStaircaseFollowsTheTypeMap(smoke::Context const&)
	{
		auto const world = blankScheme("Staircase from types");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->finishBuild();
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 30).changed,
			"Room default refused");

		require(world->layerFloorOffsetPx(0) == 0,
			"Layer 0 moved under its own type default; a depth is a lift handed "
			"backwards and never to itself");
		require(world->layerFloorOffsetPx(1) == 30,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
			+ "px off; the deepest effective floor in Layer 0 is the Room at its "
			"type's 30, so the Layer behind draws 30px higher");

		// Moving the type moves the staircase with it, with no invalidation
		// step between the write and the read.
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 12).changed,
			"Room default change refused");
		require(world->layerFloorOffsetPx(1) == 12,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
			+ "px off after the Room type moved to 12; the staircase is derived "
			"from the World every time it is asked");
	}

	// A type default is the deepest floor in its Layer only when it actually
	// is one: a shallower type default does not lower a Layer already lifted
	// by a deeper type in front of it.
	void theDeepestEffectiveFloorStillWins(smoke::Context const&)
	{
		auto const world = blankScheme("Deepest wins");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->addBackground(0, 2, 8, 3, 2);
		world->finishBuild();
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 40).changed,
			"Room default refused");
		require(world->setSectorTypeFloorDepth(core::SectorType::Background, 10).changed,
			"Background default refused");
		require(world->layerFloorOffsetPx(1) == 40,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
			+ "px off with types at 40 and 10 in front; the contribution of a "
			"Layer is its deepest floor, not the sum of its types");
	}

	// ------------------------------------------------------------------
	// Overrides are not moved.
	// ------------------------------------------------------------------

	// A Sector that carries its own depth stays exactly where its own field
	// put it when its type moves. This is the criterion the whole override
	// feature exists to satisfy, and it is checked against a Sector of the
	// very type being moved - a Background left alone while Rooms move proves
	// nothing about overrides.
	void anOverriddenSectorStaysPutWhenItsTypeMoves(smoke::Context const&)
	{
		auto const world = blankScheme("Override holds");
		world->addRoom("Overridden", 0, 2, 1, 3, 2);
		world->addRoom("Following", 0, 2, 8, 3, 2);
		world->finishBuild();

		std::optional<uint32_t> overridden;
		std::optional<uint32_t> following;
		for (auto const& sector : world->getSectors(0))
		{
			if (sector->getType() != core::SectorType::Location) continue;
			if (sector->getName() == "Overridden") overridden = sector->getIndex();
			else following = sector->getIndex();
		}
		require(overridden.has_value() && following.has_value(),
			"The fixture did not place both named Rooms on Layer 0");
		auto const overriddenRoom = *overridden;
		auto const followingRoom = *following;

		require(world->setSectorFloorDepth(overriddenRoom, -16).changed,
			"Setting the Sector's own depth was refused");
		require(world->hasFloorDepthOverride(overriddenRoom),
			"A Sector with its own recorded depth does not report an override, so "
			"a type edit could not tell it from a following Sector");
		require(!world->hasFloorDepthOverride(followingRoom),
			"A Sector with no recorded depth reports an override");

		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Room default refused");
		require(world->effectiveFloorDepth(overriddenRoom) == -16,
			"An overridden Room answers "
			+ std::to_string(world->effectiveFloorDepth(overriddenRoom))
			+ " after its type moved to 24; the override means what it says and "
			"the Sector stays where its own field put it");
		require(world->effectiveFloorDepth(followingRoom) == 24,
			"The following Room did not move with its type");
	}

	// The override is per-Sector, not per-type: clearing one Sector's override
	// puts that Sector on the type and leaves the type itself alone.
	void clearingOneOverridePutsThatSectorOnItsType(smoke::Context const&)
	{
		auto const world = blankScheme("Override cleared");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->finishBuild();
		auto const room = onlySectorOfType(*world, core::SectorType::Location);
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Room default refused");
		require(world->setSectorFloorDepth(room, -16).changed, "override refused");
		require(world->effectiveFloorDepth(room) == -16, "override not in force");

		require(world->clearSectorFloorDepth(room).changed, "clear refused");
		require(!world->hasFloorDepthOverride(room),
			"A cleared Sector still reports an override");
		require(world->effectiveFloorDepth(room) == 24,
			"A Room whose override was cleared answers "
			+ std::to_string(world->effectiveFloorDepth(room))
			+ "; with no override of its own it follows its type, which is 24");
		require(world->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"Clearing one Sector's override changed the type's default");
	}

	// An overridden Sector in front contributes its own depth to the
	// staircase, not its type's, so the Layer behind is lifted by what the
	// Sector actually draws at rather than by what its type says.
	void theStaircaseUsesTheOverrideNotTheType(smoke::Context const&)
	{
		auto const world = blankScheme("Override in the sum");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->finishBuild();
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 40).changed,
			"Room default refused");
		auto const room = onlySectorOfType(*world, core::SectorType::Location);
		require(world->setSectorFloorDepth(room, 5).changed, "override refused");

		// The Room is the only Sector in Layer 0, so the deepest effective
		// floor in front is the override at 5 rather than the type at 40.
		require(world->layerFloorOffsetPx(1) == 5,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
			+ "px off with the only Room overridden to 5 and its type at 40; the "
			"sum takes the effective depth, so an override the staircase ignored "
			"would be an override that moves nothing");
	}

	// ------------------------------------------------------------------
	// Sparseness and the write's two halves.
	// ------------------------------------------------------------------

	// Setting a type back to zero removes its entry rather than storing a
	// zero, and the type then reads exactly as the unset case reads.
	void settingATypeToZeroRemovesItsEntry(smoke::Context const&)
	{
		auto const world = blankScheme("Cleared type");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->finishBuild();
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"default refused");
		require(world->hasSectorTypeFloorDepth(core::SectorType::Location),
			"a set type reports no entry");

		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 0).changed,
			"Setting a type at 24 back to zero was not seen as a change");
		require(!world->hasSectorTypeFloorDepth(core::SectorType::Location),
			"Setting a type back to zero left an entry behind; zero lifts nothing, "
			"which is what no entry already says, and a stored zero would be a "
			"third state meaning the absence without being it");
		require(world->sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"A cleared type does not read as zero");
		require(world->effectiveFloorDepth(onlySectorOfType(*world,
			core::SectorType::Location)) == 0,
			"A Room still answers a depth after its type was cleared");
		require(world->layerFloorOffsetPx(1) == 0,
			"Layer 1 is still lifted after the type that lifted it was cleared");
	}

	// Clearing is the same write as setting zero, and goes through it: same
	// answer, same stored shape, same change flag.
	void clearingATypeIsSettingATypeZero(smoke::Context const&)
	{
		auto const world = blankScheme("Clear equals zero");
		require(world->setSectorTypeFloorDepth(core::SectorType::Chamber, 18).changed,
			"default refused");
		require(world->clearSectorTypeFloorDepth(core::SectorType::Chamber).changed,
			"Clearing a set type was not seen as a change");
		require(!world->hasSectorTypeFloorDepth(core::SectorType::Chamber),
			"Clearing left a Chamber entry behind");

		// And clearing a type that was never set is accepted and changes
		// nothing - a legal request, and not worth an undo step.
		auto const quiet = world->clearSectorTypeFloorDepth(core::SectorType::Chamber);
		require(quiet.accepted, "Clearing an unset type was refused");
		require(!quiet.changed, "Clearing an unset type was reported as a change");
		require(!world->setSectorTypeFloorDepth(core::SectorType::Chamber, 0).changed,
			"Setting an unset type to zero was reported as a change");
	}

	// Re-asking for the depth a type already carries is legal and changes
	// nothing, which is what lets the panel tell a write worth an undo step
	// from a keystroke that landed on the number that was already there.
	void reaskingForTheSameTypeDepthChangesNothing(smoke::Context const&)
	{
		auto const world = blankScheme("Same value");
		require(world->setSectorTypeFloorDepth(core::SectorType::Lift, 33).changed,
			"default refused");
		auto const again = world->setSectorTypeFloorDepth(core::SectorType::Lift, 33);
		require(again.accepted, "Re-asking for the same type depth was refused");
		require(!again.changed, "Re-asking for the same type depth was reported "
			"as a change, which would make every repeat keystroke an undo step");
	}

	// ------------------------------------------------------------------
	// The band.
	// ------------------------------------------------------------------

	// One Level either way, refused rather than clamped, with the type named in
	// the refusal because the author is looking at a table row and "out of
	// range" without a subject sends them looking in the wrong place.
	void aTypeDefaultOutsideTheBandIsRefused(smoke::Context const&)
	{
		auto const world = blankScheme("Out of band");
		std::string diagnostic;
		require(!world->canSetSectorTypeFloorDepth(core::SectorType::Location,
			CORE_LEVEL_HEIGHT_PIXELS + 1, &diagnostic),
			"A type default one pixel outside the band was accepted");
		require(diagnostic.find(core::getSectorTypeString(core::SectorType::Location))
			!= std::string::npos,
			"The refusal does not name the type: '" + diagnostic + "'");

		auto const write = world->setSectorTypeFloorDepth(core::SectorType::Location,
			9999, &diagnostic);
		require(!write.accepted, "A type default far outside the band was accepted");
		require(!write.changed, "A refused type write reported a change");
		require(!world->hasSectorTypeFloorDepth(core::SectorType::Location),
			"A refused type write left an entry on the World");

		require(!world->canSetSectorTypeFloorDepth(core::SectorType::Location,
			-CORE_LEVEL_HEIGHT_PIXELS - 1, &diagnostic),
			"A type default one pixel outside the negative band was accepted");

		// The edges themselves are inside the band.
		require(world->canSetSectorTypeFloorDepth(core::SectorType::Location,
			CORE_LEVEL_HEIGHT_PIXELS, &diagnostic),
			"The top of the band was refused: " + diagnostic);
		require(world->canSetSectorTypeFloorDepth(core::SectorType::Location,
			-CORE_LEVEL_HEIGHT_PIXELS, &diagnostic),
			"The bottom of the band was refused: " + diagnostic);
		require(world->setSectorTypeFloorDepth(core::SectorType::Location,
			-CORE_LEVEL_HEIGHT_PIXELS).changed,
			"An edge-of-band type default was not written");
	}

	// A refused write leaves the entries that were already there exactly as
	// they were, rather than half-applying the refusal.
	void aRefusedTypeWriteLeavesTheSchemeAsItWas(smoke::Context const&)
	{
		auto const world = blankScheme("Refusal leaves no trace");
		world->addRoom("Room", 0, 2, 1, 3, 2);
		world->finishBuild();
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"default refused");
		std::string diagnostic;
		require(!world->setSectorTypeFloorDepth(core::SectorType::Location, 5000,
			&diagnostic).accepted, "out-of-band write accepted");
		require(world->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"A refused write changed the type's stored depth to "
			+ std::to_string(world->sectorTypeFloorDepth(core::SectorType::Location)));
		require(world->layerFloorOffsetPx(1) == 24,
			"A refused write moved the staircase");
	}

	// ------------------------------------------------------------------
	// Every type is settable.
	// ------------------------------------------------------------------

	// Each of the eleven types takes a distinct depth and reads it back. A
	// table with a row that silently does not write is the failure this rules
	// out; it needs no Sectors at all, because the map is the thing under
	// test and a type with no Sector of it in the World is a perfectly normal
	// World to have an opinion about.
	void everyTypeTakesItsOwnDepth(smoke::Context const&)
	{
		auto const world = blankScheme("Eleven defaults");
		auto const& types = core::allSectorTypes();
		for (size_t i = 0; i < types.size(); ++i)
		{
			auto const depth = static_cast<int32_t>(i + 1) * 7 - 38;
			require(world->setSectorTypeFloorDepth(types[i], depth).changed,
				"Setting a default on type " + core::getSectorTypeString(types[i])
				+ " was not accepted as a change");
		}
		for (size_t i = 0; i < types.size(); ++i)
		{
			auto const expected = static_cast<int32_t>(i + 1) * 7 - 38;
			require(world->sectorTypeFloorDepth(types[i]) == expected,
				"Type " + core::getSectorTypeString(types[i]) + " answers "
				+ std::to_string(world->sectorTypeFloorDepth(types[i]))
				+ " rather than the " + std::to_string(expected) + " it was given");
			require(world->hasSectorTypeFloorDepth(types[i]),
				"Type " + core::getSectorTypeString(types[i]) + " took a non-zero "
				"default but reports no entry");
		}
	}
}

void registerFloorDepthTypeDefaults(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthType/aTypeNobodySetHasNoOpinion",
		aTypeNobodySetHasNoOpinion });
	checks.push_back({ "floorDepthType/theTableCoversEverySectorType",
		theTableCoversEverySectorType });
	checks.push_back({ "floorDepthType/aTypeDefaultMovesEverySectorFollowingIt",
		aTypeDefaultMovesEverySectorFollowingIt });
	checks.push_back({ "floorDepthType/twoTypeDefaultsReadSideBySide",
		twoTypeDefaultsReadSideBySide });
	checks.push_back({ "floorDepthType/theStaircaseFollowsTheTypeMap",
		theStaircaseFollowsTheTypeMap });
	checks.push_back({ "floorDepthType/theDeepestEffectiveFloorStillWins",
		theDeepestEffectiveFloorStillWins });
	checks.push_back({ "floorDepthType/anOverriddenSectorStaysPutWhenItsTypeMoves",
		anOverriddenSectorStaysPutWhenItsTypeMoves });
	checks.push_back({ "floorDepthType/clearingOneOverridePutsThatSectorOnItsType",
		clearingOneOverridePutsThatSectorOnItsType });
	checks.push_back({ "floorDepthType/theStaircaseUsesTheOverrideNotTheType",
		theStaircaseUsesTheOverrideNotTheType });
	checks.push_back({ "floorDepthType/settingATypeToZeroRemovesItsEntry",
		settingATypeToZeroRemovesItsEntry });
	checks.push_back({ "floorDepthType/clearingATypeIsSettingATypeZero",
		clearingATypeIsSettingATypeZero });
	checks.push_back({ "floorDepthType/reaskingForTheSameTypeDepthChangesNothing",
		reaskingForTheSameTypeDepthChangesNothing });
	checks.push_back({ "floorDepthType/aTypeDefaultOutsideTheBandIsRefused",
		aTypeDefaultOutsideTheBandIsRefused });
	checks.push_back({ "floorDepthType/aRefusedTypeWriteLeavesTheSchemeAsItWas",
		aRefusedTypeWriteLeavesTheSchemeAsItWas });
	checks.push_back({ "floorDepthType/everyTypeTakesItsOwnDepth",
		everyTypeTakesItsOwnDepth });
}
