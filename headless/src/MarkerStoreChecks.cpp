#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"
#include "core/Marker.h"
#include "core/MarkerStore.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("MarkerStore check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("MarkerStore check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The store is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the Marker record
		// family is unit-testable without driving the World facade. World
		// keeps the state it edits (ADR 0001) - the `mNextMarkerId`
		// allocation and the Sector-held Markers; the collaborator holds
		// only a reference.
		World world("ms-checks", 8, 3);
		MarkerStore store(world);
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 2);
		world.addRoom("Back", 1, 0, 0, 8, 2);

		std::string diagnostic;

		// ---------------------------------------------------------------
		// Generated-name placement: deterministic "Marker N" names.
		// ---------------------------------------------------------------
		auto const generated = store.addSectorMarker(room, 0, 1.5f);
		require(generated.type == SectorObjectType::Marker,
			"generated placement did not create a Marker");
		require(store.getMarkerIds().size() == 1
			&& store.lookupMarker(store.getMarkerIds().front())->getName() == "Marker 1",
			"first generated Marker is not 'Marker 1'");
		store.addSectorMarker(room, 0, 2.5f);
		require(store.lookupMarker(store.getMarkerIds().back())->getName() == "Marker 2",
			"second generated Marker is not 'Marker 2'");
		require(store.nextGeneratedMarkerName() == "Marker 3",
			"generated name did not skip taken suffixes");

		// ---------------------------------------------------------------
		// Explicit naming: trimming, uniqueness, and refusal-before-write.
		// ---------------------------------------------------------------
		store.addSectorMarker(room, 0, 3.5f, "  Entrance  ");
		auto const entrance = store.getMarkerIds()[2];
		require(store.lookupMarker(entrance)->getName() == "Entrance",
			"explicit name was not trimmed on placement");
		requireThrows([&]{ store.addSectorMarker(room, 0, 4.5f, "Entrance"); },
			"duplicate name did not throw");
		requireThrows([&]{ store.addSectorMarker(room, 0, 4.5f, ""); },
			"empty name did not throw");
		requireThrows([&]{ store.addSectorMarker(room, 0, 4.5f, "Entrance"); },
			"duplicate name did not throw");
		require(store.getMarkerIds().size() == 3,
			"a refused placement still created a Marker");
		requireThrows([&]{ store.addSectorMarker(9999, 0, 1.5f, "Nowhere"); },
			"placement in an unknown sector did not throw");
		requireThrows([&]{ store.addSectorMarker(room, 9, 1.5f, "Above"); },
			"placement above the sector did not throw");

		require(store.markerNameTaken("Entrance")
			&& !store.markerNameTaken("Entrance", entrance)
			&& !store.markerNameTaken("entrance"),
			"markerNameTaken disagrees with the held names");

		// ---------------------------------------------------------------
		// Lookup: ascending IDs, mutable/const agreement, miss is nullptr.
		// ---------------------------------------------------------------
		auto ids = store.getMarkerIds();
		require(ids.size() == 3 && ids[0] < ids[1] && ids[1] < ids[2],
			"getMarkerIds is not ascending");
		require(store.mutableMarker(entrance)
			&& store.mutableMarker(entrance)->getId() == entrance
			&& store.lookupMarker(entrance) == store.mutableMarker(entrance),
			"mutable/const lookup disagree");
		require(!store.lookupMarker(MarkerId{ 9999 }) && !store.mutableMarker(MarkerId{ 9999 }),
			"unknown ID resolved to a Marker");

		// ---------------------------------------------------------------
		// Rename: trim, same-name no-op, duplicate refusal, unknown throw.
		// ---------------------------------------------------------------
		require(store.renameMarker(entrance, "  Lobby  ", &diagnostic),
			"rename refused: " + diagnostic);
		require(store.lookupMarker(entrance)->getName() == "Lobby",
			"rename did not store the trimmed name");
		require(!store.canRenameMarker(entrance, "Marker 1", &diagnostic)
			&& diagnostic.find("already exists") != std::string::npos,
			"rename onto a taken name was accepted");
		require(store.renameMarker(entrance, "Lobby", &diagnostic)
			&& store.lookupMarker(entrance)->getName() == "Lobby",
			"same-name rename was not a silent success");
		require(!store.renameMarker(MarkerId{ 9999 }, "Ghost", &diagnostic),
			"rename of an unknown Marker was accepted");

		// ---------------------------------------------------------------
		// Properties: unknown bits, unknown Marker, and the pause gate.
		// ---------------------------------------------------------------
		auto const blocksPathing = markerPropertyBit(MarkerProperty::BlocksPathing);
		require(!store.setMarkerProperties(entrance, 1u << 31, &diagnostic)
			&& diagnostic.find("unknown bits") != std::string::npos,
			"unknown property bits were accepted");
		require(!store.setMarkerProperties(MarkerId{ 9999 }, 0, &diagnostic),
			"properties were set on an unknown Marker");
		require(store.setMarkerProperties(entrance, blocksPathing, &diagnostic)
			&& store.lookupMarker(entrance)->hasProperty(MarkerProperty::BlocksPathing),
			"property edit refused before build: " + diagnostic);

		world.finishBuild();
		require(!store.setMarkerProperties(entrance, 0, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"property edit accepted on a running built World");
		world.pauseSimulation();
		require(store.setMarkerProperties(entrance, 0, &diagnostic)
			&& !store.lookupMarker(entrance)->hasProperty(MarkerProperty::BlocksPathing),
			"property edit refused while paused: " + diagnostic);
		world.pauseSimulation();

		// ---------------------------------------------------------------
		// Restoration: identity, duplicate identity, and property checks
		// happen before anything is written. Structural edits below run
		// paused: beginStructuralEdit refuses them on a running built World.
		// ---------------------------------------------------------------
		requireThrows([&]{ store.addSectorMarkerRestored(room, 0, 5.5f, MarkerId{}, "Zero"); },
			"zero-ID restoration did not throw");
		requireThrows([&]{ store.addSectorMarkerRestored(room, 0, 5.5f, entrance, "Copy"); },
			"duplicate-ID restoration did not throw");
		requireThrows([&]{ store.addSectorMarkerRestored(room, 0, 5.5f,
			MarkerId{ 4242 }, "Bad bits", 1u << 31); },
			"restoration with unknown property bits did not throw");
		requireThrows([&]{ store.addSectorMarkerRestored(room, 0, 5.5f,
			MarkerId{ 4242 }, "Lobby"); },
			"restoration onto a taken name did not throw");
		require(store.getMarkerIds().size() == 3,
			"a refused restoration still created a Marker");
		auto const restored = store.addSectorMarkerRestored(room, 0, 5.5f,
			MarkerId{ 4242 }, " Restored ", blocksPathing);
		require(restored.type == SectorObjectType::Marker
			&& store.lookupMarker(MarkerId{ 4242 })->getName() == "Restored"
			&& store.lookupMarker(MarkerId{ 4242 })->hasProperty(MarkerProperty::BlocksPathing),
			"restoration did not land with its authored identity");

		// ---------------------------------------------------------------
		// Removal: judged before touching; IDs are never reused.
		// ---------------------------------------------------------------
		require(!store.canRemoveSectorMarker(9999, 0, &diagnostic)
			&& diagnostic.find("Sector does not exist") != std::string::npos,
			"removal from an unknown sector was accepted");
		require(!store.canRemoveSectorMarker(room, 999, &diagnostic)
			&& diagnostic.find("object does not exist") != std::string::npos,
			"removal of an unknown object index was accepted");
		require(store.removeSectorMarker(room, restored.index, &diagnostic),
			"Marker removal refused: " + diagnostic);
		require(!store.lookupMarker(MarkerId{ 4242 }),
			"a removed Marker still resolves");
		require(store.getMarkerIds().size() == 3,
			"removal did not reduce the Marker set");

		// A removed ID is never handed out again: the high-water mark only
		// moves forward.
		auto const before = store.getMarkerIds().back();
		store.addSectorMarker(room, 0, 6.5f);
		require(store.getMarkerIds().back().value > before.value,
			"a fresh Marker reused or regressed the ID space");

		// ---------------------------------------------------------------
		// The World facade forwards to the same state the store edits.
		// ---------------------------------------------------------------
		require(world.renameMarker(entrance, "Renamed via facade", &diagnostic)
			&& store.lookupMarker(entrance)->getName() == "Renamed via facade",
			"facade rename did not reach the store");
		require(store.renameMarker(entrance, "Renamed via store", &diagnostic)
			&& world.lookupMarker(entrance)->getName() == "Renamed via store",
			"store rename did not reach the facade");
		require(world.getMarkerIds() == store.getMarkerIds(),
			"facade and store disagree on the Marker set");
		auto const facadeAdded = world.addSectorMarker(room, 0, 7.5f, "Facade added");
		auto const facadeId = store.getMarkerIds().back();
		require(store.lookupMarker(facadeId)->getName() == "Facade added"
			&& facadeAdded.type == SectorObjectType::Marker,
			"facade placement did not reach the store");
		require(world.removeSectorMarker(room, facadeAdded.index, &diagnostic)
			&& !store.lookupMarker(facadeId),
			"facade removal did not reach the store");

		std::cout << "MarkerStore checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "MarkerStore checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
