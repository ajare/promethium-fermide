// The author's per-Sector-type floor depth scheme, authored through the seam
// and taken back in one step (#1103).
//
// The World tier pins what a type default means. This tier pins what it means
// to *edit* one, which is the half the ticket's undo criterion is written
// against and the half that only exists at the seam:
//
//   * one type default change is one undo step. The author typed one number
//     into one row of the table; one `undo()` takes it back. Not two undos,
//     not a partial one that leaves the staircase moved;
//   * redo puts back the same depth for the same type, not a default and not
//     the previous value;
//   * setting a type back to zero is a write like any other and one undo puts
//     the cleared depth back - the case a set-only seam loses, since there is
//     no "unset" to write and a naive implementation would have nothing to
//     restore;
//   * a refused write leaves nothing on the undo stack. A refusal that pushed
//     a snapshot would leave the author with an undo that undoes nothing and
//     shift every later step by one;
//   * an accepted write that changed nothing is not a step - re-typing the
//     number the row already carries must not become undoable;
//   * the Layer offsets the renderer reads follow the type default through
//     undo and redo, so the picture and the number cannot come apart across
//     the history;
//   * a Sector that overrides its type is unmoved by undoing and redoing the
//     type. The override is a separate fact about a different object, and the
//     history of the type must not reach into it.
//
// Everything goes through `WorldAuthoring` rather than the World directly.
// The editor only ever holds the handle, and a type default that undoes when
// set on the World but not when set through the handle is broken where it
// matters.

#include "Checks.h"
#include "Fixture.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

#include <cstdint>
#include <memory>
#include <string>

namespace
{
	using smoke::require;

	struct PairedHandles
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		std::unique_ptr<game::WorldAuthoring> authoring;
	};

	// Two Layers: a Room and a Background on the front Layer, a Room behind.
	// The front Layer's Sectors are what a type default moves; the back Room
	// is what draws higher as a result.
	PairedHandles layeredHandles(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 16, 8);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 2, 1, 3, 2);
		world->addBackground(0, 2, 8, 3, 2);
		world->addRoom("Behind", 1, 2, 1, 3, 2);
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		return PairedHandles{ std::move(runtime), std::move(authoring) };
	}

	uint32_t sectorOfType(game::WorldAuthoring& authoring, core::SectorType type,
		uint32_t layer = 0)
	{
		std::vector<uint32_t> found;
		for (auto const& sector : authoring.world().getSectors(layer))
			if (sector->getType() == type) found.push_back(sector->getIndex());
		require(found.size() == 1, "Layer " + std::to_string(layer) + " holds "
			+ std::to_string(found.size()) + " Sectors of type "
			+ core::getSectorTypeString(type) + "; the check needs exactly one");
		return found.front();
	}

	// ------------------------------------------------------------------
	// One write, one step.
	// ------------------------------------------------------------------

	// Setting a type default is exactly one undo step, and redo restores it.
	void settingATypeDefaultIsOneUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Set type default");
		auto& authoring = *fixture.authoring;
		auto const room = sectorOfType(authoring, core::SectorType::Location);

		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured");
		auto const write = authoring.setSectorTypeFloorDepth(
			core::SectorType::Location, 24);
		require(write.accepted, "a type default of 24 inside the band was refused");
		require(write.changed, "setting a type default of 24 on a type at zero "
			"was not seen as a change");
		authoring.commitEdit(before);

		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"the type default was not authored");
		require(authoring.effectiveFloorDepth(room) == 24,
			"the Room does not follow the authored type default");
		require(authoring.layerFloorOffsetPx(1) == 24,
			"the Layer behind does not draw 24px off after the write");
		require(authoring.canUndo(), "a committed type default write is not undoable");

		require(authoring.undo(), "undo refused the type default write");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"undo left the type answering "
			+ std::to_string(authoring.sectorTypeFloorDepth(core::SectorType::Location))
			+ "; one undo must take a type default all the way back to none");
		require(!authoring.hasSectorTypeFloorDepth(core::SectorType::Location),
			"undo restored the type entry rather than the absence the author "
			"started from");
		require(authoring.effectiveFloorDepth(room) == 0,
			"the Room still answers a depth after the undo");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the Layer behind still draws "
			+ std::to_string(authoring.layerFloorOffsetPx(1))
			+ "px off after the undo; the picture and the number came apart "
			"across the history");

		require(authoring.redo(), "redo refused the undone type default write");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"redo restored "
			+ std::to_string(authoring.sectorTypeFloorDepth(core::SectorType::Location))
			+ " rather than the 24 that was authored");
		require(authoring.effectiveFloorDepth(room) == 24,
			"the Room does not follow the type default after the redo");
		require(authoring.layerFloorOffsetPx(1) == 24,
			"the Layer behind does not draw 24px off after the redo");
	}

	// One write is one step, not two: the undo after a type default is back to
	// none goes to whatever the previous edit was, not to another stage of the
	// same one.
	void oneTypeDefaultWriteDoesNotTakeTwoUndos(smoke::Context const&)
	{
		auto fixture = layeredHandles("One type step only");
		auto& authoring = *fixture.authoring;
		auto const room = sectorOfType(authoring, core::SectorType::Location);

		// A first edit to stand apart from, snapshotted before the add so it
		// is what undo restores rather than what undo re-applies.
		auto const beforeAdd = authoring.captureSnapshot();
		auto const sectorsBefore = authoring.world().getNumSectors();
		authoring.world().addRoom("Extra", 1, 2, 10, 2, 2);
		authoring.commitEdit(beforeAdd);

		auto const before = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 32).changed,
			"type default write refused");
		authoring.commitEdit(before);
		require(authoring.world().getNumSectors() == sectorsBefore + 1,
			"the type default write changed the Sector count");

		require(authoring.undo(), "undo refused the type default write");
		require(authoring.effectiveFloorDepth(room) == 0, "type default not undone");
		require(authoring.undo(), "undo refused the earlier edit");
		require(authoring.world().getNumSectors() == sectorsBefore,
			"the second undo did not take the Room add back, so the type default "
			"write was recorded as more than one step");
	}

	// Setting a type back to zero is a write like any other, and one undo puts
	// the cleared depth back rather than leaving the type at zero forever.
	void clearingATypeDefaultIsOneUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Clear type default");
		auto& authoring = *fixture.authoring;
		auto const room = sectorOfType(authoring, core::SectorType::Location);

		auto const beforeSet = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 64).changed,
			"type default refused");
		authoring.commitEdit(beforeSet);

		auto const before = authoring.captureSnapshot();
		auto const cleared = authoring.clearSectorTypeFloorDepth(core::SectorType::Location);
		require(cleared.accepted && cleared.changed,
			"clearing a type at 64 was not seen as a change");
		authoring.commitEdit(before);
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the clear did not take the depth off the type");
		require(authoring.effectiveFloorDepth(room) == 0,
			"the Room still answers a depth after its type was cleared");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the Layer behind still draws off after the clear");

		require(authoring.undo(), "undo refused the clear");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 64,
			"undo restored "
			+ std::to_string(authoring.sectorTypeFloorDepth(core::SectorType::Location))
			+ "; undoing a clear must put back the 64 that was cleared, not zero");
		require(authoring.effectiveFloorDepth(room) == 64,
			"the Room does not follow the restored type default");
		require(authoring.layerFloorOffsetPx(1) == 64,
			"the Layer behind does not draw 64px off after the clear was undone");

		require(authoring.redo(), "redo refused the undone clear");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"redo did not re-clear the type");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the Layer behind does not draw flat after the re-clear");
	}

	// A negative type default undoes and redoes the same way, and the push
	// down follows it back up.
	void aNegativeTypeDefaultUndoesAndRedoes(smoke::Context const&)
	{
		auto fixture = layeredHandles("Negative type default");
		auto& authoring = *fixture.authoring;
		auto const background = sectorOfType(authoring, core::SectorType::Background);

		auto const before = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Background, -20)
			.changed, "negative type default refused");
		authoring.commitEdit(before);
		require(authoring.effectiveFloorDepth(background) == -20,
			"the Background does not follow its type to -20");

		require(authoring.undo(), "undo refused the negative type default");
		require(authoring.effectiveFloorDepth(background) == 0,
			"undo left the Background at "
			+ std::to_string(authoring.effectiveFloorDepth(background)));

		require(authoring.redo(), "redo refused the negative type default");
		require(authoring.effectiveFloorDepth(background) == -20,
			"redo restored " + std::to_string(authoring.effectiveFloorDepth(background))
			+ " rather than -20");
	}

	// ------------------------------------------------------------------
	// Refusals and no-ops are not steps.
	// ------------------------------------------------------------------

	// A refused type default write leaves nothing on the undo stack.
	void aRefusedTypeDefaultWriteIsNotAnUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Refused type write");
		auto& authoring = *fixture.authoring;

		// Set the history to a known state with one committed edit, so a
		// spurious extra snapshot is visible as an extra undo.
		auto const beforeSet = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 16).changed,
			"type default refused");
		authoring.commitEdit(beforeSet);
		require(authoring.canUndo(), "the committed edit is not undoable");

		std::string diagnostic;
		auto const refused = authoring.setSectorTypeFloorDepth(
			core::SectorType::Location, 9999, &diagnostic);
		require(!refused.accepted, "an out-of-band type default was accepted");
		require(!refused.changed, "a refused type write reported a change");
		require(!diagnostic.empty(), "the refusal carried no diagnostic");

		// The one undo available is the committed edit, not a phantom of the
		// refusal: it takes the 16 back, which is what a step that never
		// happened must not do to the history above it.
		require(authoring.undo(), "undo refused the committed edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the undo did not land on the committed edit");
		require(!authoring.canUndo(), "a refused type write left an extra undo "
			"step on the stack");
	}

	// Re-asking for the depth a type already carries is accepted, changes
	// nothing, and is not a step.
	void reaskingForTheSameTypeDefaultIsNotAnUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Same type value");
		auto& authoring = *fixture.authoring;

		auto const beforeSet = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Facade, 28).changed,
			"type default refused");
		authoring.commitEdit(beforeSet);
		require(!authoring.canRedo(), "fixture drift");

		auto const again = authoring.setSectorTypeFloorDepth(core::SectorType::Facade, 28);
		require(again.accepted, "re-asking for the same type default was refused");
		require(!again.changed, "re-asking for the same type default reported a change");

		// Nothing was committed for the no-op, so the history is exactly where
		// the committed edit left it: one undo, and it lands on the absence.
		require(authoring.undo(), "undo refused the committed edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Facade) == 0,
			"the undo did not land on the absence before the committed edit");
		require(authoring.redo(), "redo refused the committed edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Facade) == 28,
			"redo did not restore the committed type default");
	}

	// ------------------------------------------------------------------
	// Overrides are outside the type's history.
	// ------------------------------------------------------------------

	// Undoing and redoing a type default leaves an overridden Sector of that
	// type exactly where its own field put it, both times. The type's history
	// must not reach into a Sector that stopped following it.
	void aTypeDefaultHistoryDoesNotTouchAnOverride(smoke::Context const&)
	{
		auto fixture = layeredHandles("Type history vs override");
		auto& authoring = *fixture.authoring;
		auto const room = sectorOfType(authoring, core::SectorType::Location);

		auto const beforeOverride = authoring.captureSnapshot();
		require(authoring.setSectorFloorDepth(room, -12).changed, "override refused");
		authoring.commitEdit(beforeOverride);
		require(authoring.hasFloorDepthOverride(room), "override not recorded");

		auto const beforeType = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 40).changed,
			"type default refused");
		authoring.commitEdit(beforeType);
		require(authoring.effectiveFloorDepth(room) == -12,
			"the override moved under the type default");

		require(authoring.undo(), "undo refused the type default");
		require(authoring.effectiveFloorDepth(room) == -12,
			"undoing the type default moved the overridden Sector to "
			+ std::to_string(authoring.effectiveFloorDepth(room))
			+ "; the override is a separate fact and the type's history does not "
			"own it");

		require(authoring.redo(), "redo refused the type default");
		require(authoring.effectiveFloorDepth(room) == -12,
			"redoing the type default moved the overridden Sector to "
			+ std::to_string(authoring.effectiveFloorDepth(room)));
		require(authoring.hasFloorDepthOverride(room),
			"the type default's history removed the Sector's override");
	}

	// Undoing the override itself puts the Sector back on its type, which is
	// the other half of the two facts staying separate.
	void undoingAnOverridePutsTheSectorBackOnItsType(smoke::Context const&)
	{
		auto fixture = layeredHandles("Override undone onto type");
		auto& authoring = *fixture.authoring;
		auto const room = sectorOfType(authoring, core::SectorType::Location);

		auto const beforeType = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 36).changed,
			"type default refused");
		authoring.commitEdit(beforeType);
		require(authoring.effectiveFloorDepth(room) == 36, "not following type");

		auto const beforeOverride = authoring.captureSnapshot();
		require(authoring.setSectorFloorDepth(room, 4).changed, "override refused");
		authoring.commitEdit(beforeOverride);
		require(authoring.effectiveFloorDepth(room) == 4, "override not in force");

		require(authoring.undo(), "undo refused the override");
		require(authoring.effectiveFloorDepth(room) == 36,
			"undoing the override left the Room at "
			+ std::to_string(authoring.effectiveFloorDepth(room))
			+ "; with no override it follows its type, which is still 36");
		require(authoring.layerFloorOffsetPx(1) == 36,
			"the Layer behind does not draw 36px off once the override is undone");
	}

	// ------------------------------------------------------------------
	// Two types, one history.
	// ------------------------------------------------------------------

	// Two type defaults set in two edits undo independently: the second undo
	// takes back the second type, not the first.
	void twoTypeDefaultsUndoIndependently(smoke::Context const&)
	{
		auto fixture = layeredHandles("Two type edits");
		auto& authoring = *fixture.authoring;

		auto const beforeRooms = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Room default refused");
		authoring.commitEdit(beforeRooms);
		auto const beforeBackground = authoring.captureSnapshot();
		require(authoring.setSectorTypeFloorDepth(core::SectorType::Background, 8).changed,
			"Background default refused");
		authoring.commitEdit(beforeBackground);

		require(authoring.undo(), "undo refused the Background edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Background) == 0,
			"the first undo did not take the Background default back");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"the first undo took the Room default with it");

		require(authoring.undo(), "undo refused the Room edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the second undo did not take the Room default back");

		require(authoring.redo(), "redo refused the Room edit");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"redo restored the wrong type");
		require(authoring.sectorTypeFloorDepth(core::SectorType::Background) == 0,
			"redo restored both types at once");
	}
}

void registerFloorDepthTypeDefaultsAuthoring(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthTypeAuthoring/settingATypeDefaultIsOneUndoStep",
		settingATypeDefaultIsOneUndoStep });
	checks.push_back({ "floorDepthTypeAuthoring/oneTypeDefaultWriteDoesNotTakeTwoUndos",
		oneTypeDefaultWriteDoesNotTakeTwoUndos });
	checks.push_back({ "floorDepthTypeAuthoring/clearingATypeDefaultIsOneUndoStep",
		clearingATypeDefaultIsOneUndoStep });
	checks.push_back({ "floorDepthTypeAuthoring/aNegativeTypeDefaultUndoesAndRedoes",
		aNegativeTypeDefaultUndoesAndRedoes });
	checks.push_back({ "floorDepthTypeAuthoring/aRefusedTypeDefaultWriteIsNotAnUndoStep",
		aRefusedTypeDefaultWriteIsNotAnUndoStep });
	checks.push_back({ "floorDepthTypeAuthoring/reaskingForTheSameTypeDefaultIsNotAnUndoStep",
		reaskingForTheSameTypeDefaultIsNotAnUndoStep });
	checks.push_back({ "floorDepthTypeAuthoring/aTypeDefaultHistoryDoesNotTouchAnOverride",
		aTypeDefaultHistoryDoesNotTouchAnOverride });
	checks.push_back({ "floorDepthTypeAuthoring/undoingAnOverridePutsTheSectorBackOnItsType",
		undoingAnOverridePutsTheSectorBackOnItsType });
	checks.push_back({ "floorDepthTypeAuthoring/twoTypeDefaultsUndoIndependently",
		twoTypeDefaultsUndoIndependently });
}
