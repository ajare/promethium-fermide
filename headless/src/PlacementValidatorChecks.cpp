#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/PlacementValidator.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("PlacementValidator check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (core::WorldException const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("PlacementValidator check did not throw: " + what);
	}

	template<typename Fn>
	void requireNoThrow(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
			++checksRun;
		}
		catch (std::exception const& error)
		{
			throw std::runtime_error("PlacementValidator check threw unexpectedly ("
				+ what + "): " + error.what());
		}
	}
}

int main()
{
	using namespace core;

	try
	{
		// The validator is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the read-only validation
		// family is unit-testable without driving the World facade.
		World world("validator-checks", 8, 4);
		PlacementValidator validator(world);

		// ---------------------------------------------------------------
		// dimensionsAreSupported (static rule shared by the World
		// constructor, the editor, and the loader).
		// ---------------------------------------------------------------
		std::string diagnostic;
		require(PlacementValidator::dimensionsAreSupported(4, 2, 2, &diagnostic),
			"4x2 over 2 layers refused");
		require(!PlacementValidator::dimensionsAreSupported(0, 4, 2, &diagnostic)
			&& !diagnostic.empty(), "zero width accepted");
		require(!PlacementValidator::dimensionsAreSupported(4, 0, 2, &diagnostic),
			"zero height accepted");
		require(!PlacementValidator::dimensionsAreSupported(4, 2, 1, &diagnostic),
			"single-layer World accepted");
		require(!PlacementValidator::dimensionsAreSupported(4, 2, CORE_MAX_LAYERS + 1, &diagnostic),
			"layer count above the cap accepted");
		require(!PlacementValidator::dimensionsAreSupported(
				CORE_MAX_WORLD_CELLS / 2 + 1, CORE_MAX_LEVELS, 2, &diagnostic),
			"cell product over the cap accepted");
		// The 64-bit product must refuse a wrapping pair rather than let it
		// slip under the cap (#184).
		require(!PlacementValidator::dimensionsAreSupported(
				static_cast<uint32_t>(std::numeric_limits<uint32_t>::max()), 2, 2, &diagnostic),
			"overflowing dimensions accepted");

		// ---------------------------------------------------------------
		// Layer and bounds checks.
		// ---------------------------------------------------------------
		requireNoThrow([&] { validator.validateLayer("check", 0); }, "layer 0");
		requireNoThrow([&] { validator.validateLayer("check", 1); }, "layer 1");
		requireThrows([&] { validator.validateLayer("check", 2); }, "layer out of bounds");

		requireNoThrow([&] { validator.validateBounds("check", 7, 3, 1, 1); },
			"bounds at the far corner");
		requireThrows([&] { validator.validateBounds("check", 8, 0, 1, 1); },
			"x out of bounds");
		requireThrows([&] { validator.validateBounds("check", 7, 0, 2, 1); },
			"cellsWide overflow past the edge");
		requireThrows([&] { validator.validateBounds("check", 0, 4, 1, 1); },
			"y out of bounds");
		requireThrows([&] { validator.validateBounds("check", 0, 3, 1, 2); },
			"levelsHigh overflow past the edge");

		// ---------------------------------------------------------------
		// Cell checks over an authored Location.
		// ---------------------------------------------------------------
		auto const roomIndex = world.addRoom("Room", 0, 1, 1, 2, 2);
		auto room = world.getSector(static_cast<uint32_t>(roomIndex));

		requireNoThrow([&] { validator.validateCellOccupied("check", 0, 1, 1); },
			"cell inside the Room is occupied");
		requireThrows([&] { validator.validateCellUnoccupied("check", 0, 1, 1); },
			"Room cell reported unoccupied");
		requireNoThrow([&] { validator.validateCellUnoccupied("check", 0, 0, 0); },
			"empty cell is unoccupied");
		requireThrows([&] { validator.validateCellOccupied("check", 0, 0, 0); },
			"empty cell reported occupied");
		requireNoThrow([&] { validator.validateCellHasNoObject("check", 0, 1, 1); },
			"bare Room cell has no object");
		requireNoThrow([&] { validator.validateCellHasNoDoor("check", 0, 1, 1); },
			"bare Room cell has no door");
		requireNoThrow([&] { validator.validateCellIsInSector("check", 1, 1, room); },
			"Room cell is in its Room");
		requireThrows([&] { validator.validateCellIsInSector("check", 0, 0, room); },
			"empty cell reported inside the Room");
		requireNoThrow([&] { validator.validateCellTraversableOnFoot("check", "thing", 0, 1, 1); },
			"Room ground floor is traversable on foot");
		requireThrows([&] { validator.validateCellTraversableOnFoot("check", "thing", 0, 1, 2); },
			"floorless upper Room row reported traversable on foot");

		// ---------------------------------------------------------------
		// Layer space and single-Sector span checks.
		// ---------------------------------------------------------------
		requireNoThrow([&] { validator.validateLayerSpace("check", 0, 4, 0, 3, 2); },
			"free span on an empty area");
		requireThrows([&] { validator.validateLayerSpace("check", 0, 1, 1, 2, 2); },
			"occupied span reported free");
		requireNoThrow([&] { validator.validateSpaceOnlyInOneSector("check", 0, 1, 1, 2, 2); },
			"span inside one Sector refused");
		requireThrows([&] { validator.validateSpaceOnlyInOneSector("check", 0, 0, 1, 3, 1); },
			"span crossing the Room boundary accepted");

		// ---------------------------------------------------------------
		// Sector object admission.
		// ---------------------------------------------------------------
		requireNoThrow([&] { validator.validateObjectAllowedInSector("check",
			SectorObjectType::Door, static_cast<uint32_t>(roomIndex)); },
			"Door refused in a Location");
		requireThrows([&] { validator.validateObjectAllowedInSector("check",
			SectorObjectType::Door, 999); },
			"Door admitted against a missing Sector");

		// ---------------------------------------------------------------
		// Door option checks.
		// ---------------------------------------------------------------
		{
			World::CreateDoorOptions options;
			requireNoThrow([&] { validator.validateSectorDoorOptions("check", options); },
				"default Door options");
			options.width = 0;
			requireThrows([&] { validator.validateSectorDoorOptions("check", options); },
				"zero-width Door accepted");
			options.width = 1;
			options.crossingLanes = 5;
			requireThrows([&] { validator.validateSectorDoorOptions("check", options); },
				"crossing lanes above the threshold width accepted");
			options.crossingLanes = 0;
			options.holdOpenSeconds = std::numeric_limits<float>::quiet_NaN();
			requireThrows([&] { validator.validateSectorDoorOptions("check", options); },
				"NaN hold-open time accepted");
			options.holdOpenSeconds = 5.0f;
			options.speedOverride = -1.0f;
			requireThrows([&] { validator.validateSectorDoorOptions("check", options); },
				"negative Door speed accepted");
			options.speedOverride = std::nullopt;
			requireNoThrow([&] { validator.validateSectorDoorOptions("check", options); },
				"repaired Door options");
		}

		// ---------------------------------------------------------------
		// Lift option checks.
		// ---------------------------------------------------------------
		{
			World::CreateLiftOptions options;
			options.stopOffsets = { 0, 3 };
			requireNoThrow([&] { validator.validateLiftOptions("check", options); },
				"two-stop Lift options");
			options.stopOffsets = { 3, 0 };
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"non-increasing stop offsets accepted");
			options.stopOffsets = { 0 };
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"single-stop Lift accepted");
			options.stopOffsets = { 0, 3 };
			options.capacity = 0;
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"zero-capacity Lift accepted");
			options.capacity = 2;
			options.initialStop = 9;
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"out-of-range initial stop accepted");
			options.initialStop = 0;
			options.cellsWide = 0;
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"zero-width Lift accepted");
			options.cellsWide = 1;
			options.minimumDwellSeconds = std::numeric_limits<float>::infinity();
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"infinite dwell time accepted");
			options.minimumDwellSeconds = 1.0f;
			options.maximumBoardingSeconds = 0.5f;
			requireThrows([&] { validator.validateLiftOptions("check", options); },
				"maximum boarding below minimum dwell accepted");
			options.maximumBoardingSeconds = 5.0f;
			requireNoThrow([&] { validator.validateLiftOptions("check", options); },
				"repaired Lift options");
		}

		// ---------------------------------------------------------------
		// Ladder and ForceBridge option checks.
		// ---------------------------------------------------------------
		{
			World::CreateLadderOptions options{ 2, true, true };
			requireNoThrow([&] { validator.validateSectorLadderOptions("check", options); },
				"default Ladder options");
			options.directionalBatchLimit = 0;
			requireThrows([&] { validator.validateSectorLadderOptions("check", options); },
				"zero directional batch limit accepted");
		}
		{
			World::CreateForceBridgeOptions options;
			requireNoThrow([&] { validator.validateSectorForceBridgeOptions("check", options); },
				"default ForceBridge options");
			options.width = 0;
			requireThrows([&] { validator.validateSectorForceBridgeOptions("check", options); },
				"zero-width ForceBridge accepted");
			options.width = CORE_FORCEBRIDGE_MAX_SIZE + 1;
			requireThrows([&] { validator.validateSectorForceBridgeOptions("check", options); },
				"over-long ForceBridge accepted");
		}

		// ---------------------------------------------------------------
		// Staircase endpoint checks.
		// ---------------------------------------------------------------
		{
			std::string endpointDiagnostic;
			require(validator.validateStaircaseEndpoint(0, 1, 1, false, CORE_SIDE_RIGHT,
					endpointDiagnostic),
				"Room ground floor refused as a lower Staircase landing");
			require(!validator.validateStaircaseEndpoint(0, 0, 0, false, CORE_SIDE_RIGHT,
					endpointDiagnostic),
				"empty cell accepted as a Staircase landing");
			require(!endpointDiagnostic.empty(), "refused landing left no diagnostic");
		}

		std::cout << "PlacementValidator checks passed (" << checksRun << " checks)\n";
	}
	catch (std::exception const& error)
	{
		std::cerr << "PlacementValidator checks FAILED: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
