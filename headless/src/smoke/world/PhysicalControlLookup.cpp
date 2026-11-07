// Derived physical-control lookup by InteractionPointId, for ticket #625.
//
// physicalControlPlacement() used to scan every placement and cast each
// hosted object, and pressPhysicalControl() scanned every Sector object
// separately. Both now resolve through a derived point-to-placement index
// maintained at the canonical bind seam and rebuilt when placement indices
// shift. These checks pin the contract:
//
//   * every bound control resolves to its own placement (Sector/object
//     identity and Button geometry), including a same-row pair
//   * hot-path lookups perform no placement scan at all (operation-count
//     evidence: the full-scan visit counter never moves during lookups)
//   * press feedback disables exactly the resolved control; unknown or
//     null points are inert no-ops
//   * an in-place placement erase (Dumbwaiter detach) shifts later
//     placement indices without leaving stale resolutions: the detached
//     landing points stop resolving, and every other control keeps its
//     correct Sector/object identity, geometry and press behaviour
//   * reset-and-replay re-derives the index from the canonical bindings
//     with unchanged resolution and geometry
//
// Registration: world smoke module.

#include "Checks.h"
#include "../support/DumbwaiterFixture.h"

#include "core/World.h"
#include "core/Button.h"
#include "core/Sector.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace core
{
	// Declared a friend of World: exercises the private derived lookup,
	// press forward and lookup-operation counters.
	struct PhysicalControlLookupTestAccess
	{
		static World::PhysicalControlPlacement const* placement(World const& world,
			InteractionPointId point)
		{
			return world.physicalControlPlacement(point);
		}
		static std::optional<Vector2> centre(World const& world, InteractionPointId point)
		{
			return world.physicalButtonCentre(point);
		}
		static void press(World& world, InteractionPointId point)
		{
			world.pressPhysicalControl(point);
		}
		static std::size_t indexedControls(World const& world)
		{
			return world.mPhysicalControlByPoint.size();
		}
		static std::map<InteractionPointId, uint32_t> const& index(World const& world)
		{
			return world.mPhysicalControlByPoint;
		}
		static uint64_t scanVisits(World const& world)
		{
			return world.mPhysicalControlPlacementScanVisits;
		}
		static void resetScanVisits(World& world)
		{
			world.mPhysicalControlPlacementScanVisits = 0;
		}
	};
}

namespace
{
	using smoke::require;
	using Access = core::PhysicalControlLookupTestAccess;

	std::shared_ptr<const core::Button> buttonAt(core::World const& world,
		uint32_t sectorIndex, uint32_t objectIndex)
	{
		auto object = world.getSector(sectorIndex)->getObject(objectIndex);
		return object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
	}

	// A one-Room World with two Light switches on the same row (Level).
	struct RowSwitches
	{
		core::World world{ "Physical control lookup", 12, 2 };
		uint32_t room{};
		core::World::CreateObjectResult lower;
		core::World::CreateObjectResult upper;

		RowSwitches()
		{
			room = world.addRoom("Room", 0, 0, 0, 12, 1);
			world.finishBuild();
			world.pauseSimulation();
			lower = world.addSectorLightSwitch(room, 4);
			upper = world.addSectorLightSwitch(room, 5);
			require(bool(lower.interactionPoint) && bool(upper.interactionPoint),
				"Same-row Light switch controls were not bound");
			require(lower.index != upper.index, "Distinct switches share an object slot");
		}
	};

	// The derived lookup resolves the control to its own placement, and the
	// resolved geometry matches the live Button.
	void requireResolves(core::World const& world, core::World::CreateObjectResult const& control,
		std::string const& label)
	{
		auto const* placement = Access::placement(world, control.interactionPoint);
		require(placement != nullptr, label + " control does not resolve");
		require(placement->objectIndex == control.index
			&& placement->sectorIndex == control.sector->getIndex(),
			label + " control resolved to the wrong placement");
		auto button = buttonAt(world, placement->sectorIndex, placement->objectIndex);
		require(button && button->getInteractionPointId() == control.interactionPoint,
			label + " resolved placement does not host this point's Button");
		auto const centre = Access::centre(world, control.interactionPoint);
		require(centre.has_value(), label + " Button centre is missing");
		require(centre->distanceTo(button->getPosition() + button->getSize() * 0.5f) < 0.0001f,
			label + " Button centre does not match the resolved placement geometry");
	}

	// Resolution, same-row placement, geometry and O(1) lookup evidence (#625).
	void resolvesWithoutScanning()
	{
		RowSwitches f;
		requireResolves(f.world, f.lower, "Lower");
		requireResolves(f.world, f.upper, "Upper");
		require(Access::indexedControls(f.world) == 2,
			"Derived index does not hold exactly the bound controls");

		// Both switches share one row: they resolve to distinct placements
		// on the same Level through the derived lookup.
		require(Access::placement(f.world, f.lower.interactionPoint)->cellY
			== Access::placement(f.world, f.upper.interactionPoint)->cellY,
			"Same-row switches resolved to different Levels");
		require(Access::placement(f.world, f.lower.interactionPoint)
			->candidates[Access::placement(f.world, f.lower.interactionPoint)->currentCandidate].cellX
			!= Access::placement(f.world, f.upper.interactionPoint)
				->candidates[Access::placement(f.world, f.upper.interactionPoint)->currentCandidate].cellX,
			"Distinct switches resolved to the same host cell");

		// Operation evidence: hot-path lookups visit zero placements.
		Access::resetScanVisits(f.world);
		for (unsigned i = 0; i < 500; ++i)
		{
			require(Access::placement(f.world, f.lower.interactionPoint)
				&& Access::placement(f.world, f.upper.interactionPoint),
				"Repeated lookup failed to resolve");
			(void)Access::centre(f.world, f.lower.interactionPoint);
		}
		require(Access::scanVisits(f.world) == 0,
			"Derived lookup visited placements instead of resolving directly");
	}

	// Press feedback follows the derived lookup (#625).
	void pressFeedbackResolvesExactly()
	{
		RowSwitches f;
		require(buttonAt(f.world, f.lower.sector->getIndex(), f.lower.index)->isEnabled()
			&& buttonAt(f.world, f.upper.sector->getIndex(), f.upper.index)->isEnabled(),
			"Fixture Buttons started disabled");
		Access::resetScanVisits(f.world);
		Access::press(f.world, f.lower.interactionPoint);
		require(!buttonAt(f.world, f.lower.sector->getIndex(), f.lower.index)->isEnabled(),
			"Press did not disable the resolved Button");
		require(buttonAt(f.world, f.upper.sector->getIndex(), f.upper.index)->isEnabled(),
			"Press bled into the sibling control on the same row");
		require(Access::scanVisits(f.world) == 0,
			"Press path searched Sector objects instead of using the derived lookup");

		// Unknown and null points are inert no-ops.
		Access::press(f.world, core::InteractionPointId{ 9999 });
		Access::press(f.world, core::InteractionPointId{});
		require(!buttonAt(f.world, f.lower.sector->getIndex(), f.lower.index)->isEnabled()
			&& buttonAt(f.world, f.upper.sector->getIndex(), f.upper.index)->isEnabled(),
			"Inert press changed control state");
	}

	// An in-place placement erase shifts later indices; nothing stale survives (#625).
	void detachShiftsIndicesWithoutStaleness()
	{
		auto world = dumbwaiter_fixture::make();
		auto id = world->addDumbwaiter(1, 0, 2, { 0, 2.f });
		world->finishBuild();
		auto unit = world->lookupDumbwaiter(id);
		require(unit != nullptr, "Dumbwaiter fixture was not created");

		// Create the switch after the Dumbwaiter so its placement index sits
		// behind the landing placements the detach will erase.
		auto landing = unit->getStop(0).sector;
		auto light = world->addSectorLightSwitch(landing->getIndex(), 0);
		requireResolves(*world, light, "Landing");
		require(Access::indexedControls(*world) == 3,
			"Fixture should index two landings plus the Light switch");
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto const* placement = Access::placement(*world, unit->getLandingButton(stop));
			require(placement != nullptr, "Landing control does not resolve before detach");
			auto hosted = buttonAt(*world, landing->getIndex(), placement->objectIndex);
			require(hosted && hosted->getInteractionPointId() == unit->getLandingButton(stop)
				&& placement->sectorIndex == landing->getIndex(),
				"Landing control resolved to the wrong placement");
		}

		require(world->removeDumbwaiter(id), "Dumbwaiter removal was refused");
		for (uint32_t stop = 0; stop < 2; ++stop)
			require(Access::placement(*world, unit->getLandingButton(stop)) == nullptr,
				"Detached landing still resolves after the placement erase");
		require(Access::indexedControls(*world) == 1,
			"Detached landings survived in the derived index");
		requireResolves(*world, light, "Post-detach");

		// Press feedback still follows the shifted index exactly.
		Access::resetScanVisits(*world);
		Access::press(*world, light.interactionPoint);
		require(!buttonAt(*world, light.sector->getIndex(), light.index)->isEnabled(),
			"Press after the index shift did not disable the resolved Button");
		Access::press(*world, unit->getLandingButton(0));
		require(Access::scanVisits(*world) == 0,
			"Post-detach lookups fell back to scanning placements");
	}

	// Reset-and-replay re-derives the index from canonical bindings (#625).
	void replayRebindsLookup()
	{
		RowSwitches f;
		requireResolves(f.world, f.lower, "Pre-reset lower");
		requireResolves(f.world, f.upper, "Pre-reset upper");
		f.world.resetSimulation();
		f.world.pauseSimulation();
		// Reconstruction rebuilt every control through the canonical bind
		// seam; the derived index must be self-consistent without repair.
		require(Access::indexedControls(f.world) == 2,
			"Replay did not re-derive exactly the bound controls");
		for (auto const& [point, placementIndex] : Access::index(f.world))
		{
			auto const* placement = Access::placement(f.world, point);
			require(placement != nullptr, "Replayed control does not resolve");
			auto hosted = buttonAt(f.world, placement->sectorIndex, placement->objectIndex);
			require(hosted && hosted->getInteractionPointId() == point,
				"Replayed index entry does not match its Button binding");
			auto const centre = Access::centre(f.world, point);
			require(centre.has_value()
				&& centre->distanceTo(hosted->getPosition() + hosted->getSize() * 0.5f) < 0.0001f,
				"Replayed Button geometry does not match the resolved placement");
			(void)placementIndex;
		}
		Access::resetScanVisits(f.world);
		require(Access::scanVisits(f.world) == 0, "Post-replay lookup scanned placements");
	}

	void runPhysicalControlLookupSmokeChecksImpl()
	{
		resolvesWithoutScanning();
		pressFeedbackResolvesExactly();
		detachShiftsIndicesWithoutStaleness();
		replayRebindsLookup();
	}
}

void runPhysicalControlLookupSmokeChecks()
{
	runPhysicalControlLookupSmokeChecksImpl();
}
