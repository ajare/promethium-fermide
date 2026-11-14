// A floor depth is authored through the seam and taken back in one step (#1101).
//
// The seam is where the ticket's promise about undo lives. A Sector's depth
// is one number, and the whole feature would be undone just as well by a
// World snapshot that happened to carry it - which is exactly the problem:
// "it happens to work" is not a contract, and a depth that rides along in a
// snapshot without being checked is a depth that can stop riding along.
//
// So the checks here are the shape of the edit rather than the value:
//
//   * one write is exactly one undo step. One `commitEdit` after one write,
//     and one `undo()` takes the whole thing back - not two undos, not a
//     partial one;
//   * redo puts back the same depth, not a default and not the previous
//     value;
//   * clearing is a write like any other and takes one step back to the
//     depth it cleared, which is the case a set-to-zero-only seam loses;
//   * a refused write leaves nothing on the undo stack. A refusal that
//     pushed a snapshot would leave the author with an undo that undoes
//     nothing and shifts every later step by one;
//   * an accepted write that changed nothing is not a step. Repainting the
//     depth a Sector already carries, or clearing one that was never set,
//     must not mark the document dirty or become undoable;
//   * the Layer offset the renderer reads follows the depth through undo and
//     redo, so the picture and the number cannot come apart across the
//     history.
//
// Everything goes through `WorldAuthoring` rather than the World directly.
// The editor only ever holds the handle, and a depth that undoes when set on
// the World but not when set through the handle is broken where it matters.

#include "Checks.h"
#include "Fixture.h"

#include "core/Defines.h"
#include "core/Sector.h"
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

	// Two Layers, one Room on each, behind a handle. The back Room is the one
	// that moves; the front Room is what carries the depth.
	PairedHandles layeredHandles(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 16, 8);
		while (world->getLayerCount() < 2) world->addLayer();
		auto const front = world->addRoom("Front", 0, 2, 2, 3, 2);
		world->addRoom("Behind", 1, 2, 2, 3, 2);
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		// The front Room's index is taken before the handle is in the way,
		// and re-read through the handle below rather than trusted: an index
		// that drifted across the ownership transfer would be the bug, not
		// the fixture.
		require(authoring->world().getSectors(0).front()->getIndex() == front,
			"The front Room's index changed crossing into the runtime");
		return PairedHandles{ std::move(runtime), std::move(authoring) };
	}

	uint32_t frontOn(game::WorldAuthoring& authoring, uint32_t layer)
	{
		auto const sectors = authoring.world().getSectors(layer);
		require(!sectors.empty(), "Layer " + std::to_string(layer)
			+ " holds no Sectors, so the case cannot say anything");
		return sectors.front()->getIndex();
	}

	// ------------------------------------------------------------------
	// One write, one step.
	// ------------------------------------------------------------------

	// Setting a depth is exactly one undo step, and redo restores it.
	void settingADepthIsOneUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Set depth");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured");
		auto const write = authoring.setSectorFloorDepth(front, 48);
		require(write.accepted, "a depth of 48 inside the band was refused");
		require(write.changed, "setting a depth of 48 on a Sector at zero "
			"was not seen as a change");
		authoring.commitEdit(before);

		require(authoring.effectiveFloorDepth(front) == 48,
			"the depth was not authored");
		require(authoring.layerFloorOffsetPx(1) == 48,
			"the Layer behind does not draw 48px off after the write");
		require(authoring.canUndo(), "a committed depth write is not undoable");

		require(authoring.undo(), "undo refused the depth write");
		require(authoring.effectiveFloorDepth(front) == 0,
			"undo left the Sector answering "
				+ std::to_string(authoring.effectiveFloorDepth(front))
				+ "; one undo must take a depth write all the way back to none");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the Layer behind still draws "
				+ std::to_string(authoring.layerFloorOffsetPx(1))
				+ "px off after the undo");

		require(authoring.redo(), "redo refused the undone depth write");
		require(authoring.effectiveFloorDepth(front) == 48,
			"redo restored " + std::to_string(authoring.effectiveFloorDepth(front))
				+ " rather than the 48 that was authored");
		require(authoring.layerFloorOffsetPx(1) == 48,
			"the Layer behind does not draw 48px off after the redo");
	}

	// One write is one step, not two: a second `undo()` after the depth is
	// back to none goes to whatever the previous edit was, not to another
	// stage of the same one.
	void oneDepthWriteDoesNotTakeTwoUndos(smoke::Context const&)
	{
		auto fixture = layeredHandles("One step only");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		// A first edit to stand apart from: a Room added on Layer 1's other
		// side, so the second undo has somewhere visibly different to land.
		// The snapshot is taken before the add, because it is what undo
		// restores - captured after, it would restore the add instead of
		// taking it back.
		auto const beforeAdd = authoring.captureSnapshot();
		auto const roomsBefore = authoring.world().getNumSectors();
		authoring.world().addRoom("Extra", 1, 2, 10, 2, 2);
		authoring.commitEdit(beforeAdd);

		auto const before = authoring.captureSnapshot();
		require(authoring.setSectorFloorDepth(front, 32).changed,
			"depth write refused");
		authoring.commitEdit(before);
		require(authoring.world().getNumSectors() == roomsBefore + 1,
			"the depth write changed the Sector count");

		require(authoring.undo(), "undo refused the depth write");
		require(authoring.effectiveFloorDepth(front) == 0, "depth not undone");
		// The next undo back is the Room add, not a second stage of the
		// depth write.
		require(authoring.undo(), "undo refused the earlier edit");
		require(authoring.world().getNumSectors() == roomsBefore,
			"the second undo did not take the Room add back, so the depth write "
			"was recorded as more than one step");
	}

	// Clearing is a write like any other, and one undo puts the cleared
	// depth back rather than leaving the Sector at zero forever.
	void clearingADepthIsOneUndoStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Clear depth");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		authoring.commitEdit(authoring.captureSnapshot());
		require(authoring.setSectorFloorDepth(front, 64).changed, "depth refused");
		authoring.commitEdit(authoring.captureSnapshot());

		auto const before = authoring.captureSnapshot();
		auto const cleared = authoring.clearSectorFloorDepth(front);
		require(cleared.accepted && cleared.changed,
			"clearing a Sector at 64 was not seen as a change");
		authoring.commitEdit(before);
		require(authoring.effectiveFloorDepth(front) == 0,
			"the clear did not take the depth off");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the Layer behind still draws off after the clear");

		require(authoring.undo(), "undo refused the clear");
		require(authoring.effectiveFloorDepth(front) == 64,
			"undo restored " + std::to_string(authoring.effectiveFloorDepth(front))
				+ "; undoing a clear must put back the 64 that was cleared, not zero");
		require(authoring.layerFloorOffsetPx(1) == 64,
			"the Layer behind does not draw 64px off after the clear was undone");

		require(authoring.redo(), "redo refused the undone clear");
		require(authoring.effectiveFloorDepth(front) == 0,
			"redo did not re-clear the depth");
	}

	// A negative depth undoes and redoes the same way, and the push down
	// follows it back up.
	void aNegativeDepthUndoesAndRedoes(smoke::Context const&)
	{
		auto fixture = layeredHandles("Negative depth");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		auto const before = authoring.captureSnapshot();
		require(authoring.setSectorFloorDepth(front, -40).changed,
			"a depth of -40 inside the band was refused");
		authoring.commitEdit(before);
		require(authoring.layerFloorOffsetPx(1) == -40,
			"the Layer behind is pushed " + std::to_string(authoring.layerFloorOffsetPx(1))
				+ "px; -40 in front pushes it down 40");

		require(authoring.undo(), "undo refused the negative depth write");
		require(authoring.effectiveFloorDepth(front) == 0,
			"undo left a negative depth behind");
		require(authoring.layerFloorOffsetPx(1) == 0,
			"the push down outlived the depth it came from");

		require(authoring.redo(), "redo refused the undone negative write");
		require(authoring.effectiveFloorDepth(front) == -40,
			"redo restored " + std::to_string(authoring.effectiveFloorDepth(front))
				+ " rather than -40");
	}

	// ------------------------------------------------------------------
	// Refusals and no-ops.
	// ------------------------------------------------------------------

	// A value outside the band is refused, and a refusal leaves nothing on
	// the undo stack and nothing on the Sector.
	void anOutOfBandDepthIsRefusedAndLeavesNoTrace(smoke::Context const&)
	{
		auto fixture = layeredHandles("Out of band");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);
		int32_t const band = CORE_LEVEL_HEIGHT_PIXELS;

		require(authoring.canSetSectorFloorDepth(front, band),
			"a depth of exactly one Level either way should be inside the band");
		require(authoring.canSetSectorFloorDepth(front, -band),
			"a depth of exactly one Level the other way should be inside the band");
		require(!authoring.canSetSectorFloorDepth(front, band + 1),
			"a depth past one Level was accepted");
		require(!authoring.canSetSectorFloorDepth(front, -(band + 1)),
			"a depth past one Level the other way was accepted");

		std::string diagnostic;
		auto const write = authoring.setSectorFloorDepth(front, band + 1, &diagnostic);
		require(!write.accepted, "setSectorFloorDepth accepted an out-of-band value");
		require(!write.changed, "a refused write reported a change");
		require(!diagnostic.empty(),
			"an out-of-band refusal said nothing about why it refused");
		require(authoring.effectiveFloorDepth(front) == 0,
			"a refused write still reached the Sector");
		require(!authoring.canUndo(),
			"a refused write put something on the undo stack, which would shift "
			  "every later step by one and leave the author undoing nothing");
	}

	// The band is stated with the Sector that was asked about, so the
	// diagnostic a mistyped value produces points at something.
	void theRefusalSaysWhichSectorAndWhatBand(smoke::Context const&)
	{
		auto fixture = layeredHandles("Refusal wording");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		std::string diagnostic;
		(void)authoring.setSectorFloorDepth(front, 99999, &diagnostic);
		require(diagnostic.find("99999") != std::string::npos,
			"the refusal does not name the value it refused: " + diagnostic);
		require(diagnostic.find(std::to_string(CORE_LEVEL_HEIGHT_PIXELS))
				!= std::string::npos,
			"the refusal does not name the band it refused against: " + diagnostic);
		require(diagnostic.find("Front") != std::string::npos,
			"the refusal does not name the Sector it refused for: " + diagnostic);
	}

	// Repainting the depth a Sector already carries is accepted and changes
	// nothing: no undo step, no dirty document.
	void rewritingTheSameDepthIsNotAStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Same depth twice");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		auto const before = authoring.captureSnapshot();
		require(authoring.setSectorFloorDepth(front, 48).changed, "first write refused");
		authoring.commitEdit(before);
		authoring.markSaved();
		require(!authoring.isModified(), "markSaved left the handle modified");

		auto const write = authoring.setSectorFloorDepth(front, 48);
		require(write.accepted, "rewriting the same depth was refused");
		require(!write.changed, "rewriting the depth a Sector already carries was "
			"reported as a change");
		require(!authoring.isModified(),
			"a write that authored nothing marked the document modified");
	}

	// Clearing a Sector that was never set is the same no-op.
	void clearingAnUnsetDepthIsNotAStep(smoke::Context const&)
	{
		auto fixture = layeredHandles("Clear nothing");
		auto& authoring = *fixture.authoring;
		auto const front = frontOn(authoring, 0);

		authoring.commitEdit(authoring.captureSnapshot());
		authoring.markSaved();
		auto const cleared = authoring.clearSectorFloorDepth(front);
		require(cleared.accepted, "clearing an unset depth was refused");
		require(!cleared.changed, "clearing a depth that was never set was "
			"reported as a change");
		require(!authoring.isModified(),
			"clearing nothing marked the document modified");
	}

	// A Sector index outside the World is refused with something to say,
	// and reaches nothing.
	void anIndexOutsideTheWorldIsRefused(smoke::Context const&)
	{
		auto fixture = layeredHandles("No such Sector");
		auto& authoring = *fixture.authoring;
		std::string diagnostic;
		auto const write = authoring.setSectorFloorDepth(99999u, 48, &diagnostic);
		require(!write.accepted, "a Sector index outside the World was accepted");
		require(!diagnostic.empty(),
			"an out-of-range index was refused without a word about why");
		require(!authoring.canUndo(),
			"a refused write reached the undo stack");
	}
}

void registerGameAuthoringFloorDepth(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepth/settingADepthIsOneUndoStep",
		settingADepthIsOneUndoStep });
	checks.push_back({ "floorDepth/oneDepthWriteDoesNotTakeTwoUndos",
		oneDepthWriteDoesNotTakeTwoUndos });
	checks.push_back({ "floorDepth/clearingADepthIsOneUndoStep",
		clearingADepthIsOneUndoStep });
	checks.push_back({ "floorDepth/aNegativeDepthUndoesAndRedoes",
		aNegativeDepthUndoesAndRedoes });
	checks.push_back({ "floorDepth/anOutOfBandDepthIsRefusedAndLeavesNoTrace",
		anOutOfBandDepthIsRefusedAndLeavesNoTrace });
	checks.push_back({ "floorDepth/theRefusalSaysWhichSectorAndWhatBand",
		theRefusalSaysWhichSectorAndWhatBand });
	checks.push_back({ "floorDepth/rewritingTheSameDepthIsNotAStep",
		rewritingTheSameDepthIsNotAStep });
	checks.push_back({ "floorDepth/clearingAnUnsetDepthIsNotAStep",
		clearingAnUnsetDepthIsNotAStep });
	checks.push_back({ "floorDepth/anIndexOutsideTheWorldIsRefused",
		anIndexOutsideTheWorldIsRefused });
}
