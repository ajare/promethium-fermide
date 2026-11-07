#include <iostream>
#include <stdexcept>
#include <string>

#include "core/Exceptions.h"
#include "core/MarkerStore.h"
#include "core/Sector.h"
#include "core/Window.h"
#include "core/World.h"
#include "core/WorldBuilder.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("WorldBuilder check failed: " + what);
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
		throw std::runtime_error("WorldBuilder check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The builder is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the SectorObject
		// authoring family is unit-testable without driving the World
		// facade alone. World keeps the state it edits (ADR 0001) - the
		// Sectors, Layers, cells, and construction records; the builder
		// holds only a reference.
		World world("wb-checks", 10, 3);
		WorldBuilder builder(world);
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 2);
		world.addRoom("Back", 1, 0, 0, 8, 2);

		std::string diagnostic;

		// ---------------------------------------------------------------
		// Walkway: placement is judged before anything is written.
		// ---------------------------------------------------------------
		require(!builder.canAddSectorWalkway(9999, 1, 1, &diagnostic)
			&& diagnostic.find("Room does not exist") != std::string::npos,
			"walkway in an unknown Room was accepted");
		require(!builder.canAddSectorWalkway(room, 0, 1, &diagnostic)
			&& diagnostic.find("above the Location's ground floor") != std::string::npos,
			"walkway at ground level was accepted");
		require(!builder.canAddSectorWalkway(room, 2, 1, &diagnostic)
			&& diagnostic.find("outside the Location") != std::string::npos,
			"walkway above the Room was accepted");
		require(!builder.canAddSectorWalkway(room, 1, 8, &diagnostic)
			&& diagnostic.find("outside the Location") != std::string::npos,
			"walkway past the Room's right edge was accepted");

		auto const walkway = builder.addSectorWalkway(room, 1, 0);
		require(walkway.type == SectorObjectType::Walkway,
			"walkway placement did not report a Walkway");
		require(!builder.canAddSectorWalkway(room, 1, 0, &diagnostic)
			&& diagnostic.find("already occupies") != std::string::npos,
			"a second walkway on an occupied cell was accepted");
		require(world.canAddSectorWalkway(room, 1, 1, &diagnostic),
			"facade disagrees with the builder on a free walkway cell: " + diagnostic);

		// ---------------------------------------------------------------
		// Force bridge: option refusals are judged before any placement.
		// ---------------------------------------------------------------
		World::CreateForceBridgeOptions fbOptions;
		fbOptions.width = 2;
		fbOptions.extensible = true;
		fbOptions.startExtended = true;
		fbOptions.controlCount = 1;
		// With only the left-support Walkway in place, the right support
		// is what the bridge still lacks.
		require(!builder.canAddSectorForceBridge(room, 1, 1, fbOptions, &diagnostic)
			&& diagnostic.find("Ground or a Walkway on its right") != std::string::npos,
			"Force Bridge without a right support was accepted: " + diagnostic);
		require(builder.addSectorWalkway(room, 1, 3).type == SectorObjectType::Walkway,
			"right-support walkway was not created");
		require(builder.canAddSectorForceBridge(room, 1, 1, fbOptions, &diagnostic),
			"valid Force Bridge was refused: " + diagnostic);

		auto badOptions = fbOptions;
		badOptions.width = 0;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic)
			&& diagnostic.find("width must be") != std::string::npos,
			"zero-width Force Bridge was accepted");
		badOptions = fbOptions;
		badOptions.width = CORE_FORCEBRIDGE_MAX_SIZE + 1;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic),
			"over-wide Force Bridge was accepted");
		badOptions = fbOptions;
		badOptions.fromSide = 5;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic)
			&& diagnostic.find("side is invalid") != std::string::npos,
			"invalid extension side was accepted");
		badOptions = fbOptions;
		badOptions.controlCount = 0;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic)
			&& diagnostic.find("requires one or two controls") != std::string::npos,
			"extensible bridge with no controls was accepted");
		badOptions = fbOptions;
		badOptions.extensible = false;
		badOptions.controlCount = 1;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic)
			&& diagnostic.find("permanently extended") != std::string::npos,
			"non-extensible bridge with a control was accepted");
		badOptions = fbOptions;
		badOptions.extensible = false;
		badOptions.controlCount = 0;
		badOptions.startExtended = false;
		require(!builder.canAddSectorForceBridge(room, 1, 1, badOptions, &diagnostic),
			"non-extensible bridge without start-extended was accepted");

		// Placement refusals: a span that is not clear air.
		badOptions = fbOptions;
		badOptions.width = 1;
		require(!builder.canAddSectorForceBridge(room, 1, 3, badOptions, &diagnostic)
			&& diagnostic.find("clear air") != std::string::npos,
			"Force Bridge over an occupied span was accepted");

		// A refused add leaves nothing behind.
		auto const objectsBefore = world.getSector(room)->getNumObjects();
		requireThrows([&]{ builder.addSectorForceBridge(room, 1, 3, badOptions); },
			"adding a Force Bridge over an occupied span did not throw");
		require(world.getSector(room)->getNumObjects() == objectsBefore,
			"a refused Force Bridge add created an object");

		auto const bridge = builder.addSectorForceBridge(room, 1, 1, fbOptions);
		require(bridge.forceBridge.type == SectorObjectType::ForceBridge,
			"Force Bridge add did not report a ForceBridge");
		require(bridge.controls[0].index != ~0u && bridge.controls[1].index == ~0u,
			"one-control bridge did not create exactly one control");
		require(bool(bridge.traversalResource),
			"Force Bridge add created no traversal resource");
		require(!builder.canAddSectorWalkway(room, 1, 1, &diagnostic)
			&& diagnostic.find("Force Bridge already occupies") != std::string::npos,
			"walkway onto a Force Bridge cell was accepted");

		// ---------------------------------------------------------------
		// Window: the back-most Layer can never take a new one.
		// ---------------------------------------------------------------
		require(!builder.canAddSectorWindow(1, 0, 5, 1, 1, &diagnostic)
			&& diagnostic.find("back-most Layer") != std::string::npos,
			"Window on the back-most Layer was accepted");
		require(builder.canAddSectorWindow(0, 0, 5, 1, 1, &diagnostic),
			"valid Window was refused: " + diagnostic);

		World::CreateWindowOptions windowOptions;
		windowOptions.traversable = true;
		auto const window = builder.addSectorWindow(0, 0, 5, 1, 1, windowOptions);
		require(window.window.type == SectorObjectType::Window && window.object != nullptr,
			"traversable Window add did not produce a Window");
		require(bool(window.traversalResource),
			"traversable Window created no traversal resource");
		require(!builder.canAddSectorWindow(0, 0, 5, 1, 1, &diagnostic)
			&& diagnostic.find("another object occupies") != std::string::npos,
			"second Window onto an occupied cell was accepted");

		// A non-traversable Window may still be authored where it looks
		// into the back Layer.
		auto const plain = builder.addSectorWindow(0, 1, 5, 1, 1, {});
		require(plain.window.type == SectorObjectType::Window
			&& !bool(plain.traversalResource),
			"non-traversable Window unexpectedly owns a traversal resource");
		auto const facadeWindowIndex = world.addSectorWindow(0, 1, 6, 1, 1);
		require(facadeWindowIndex != ~0u,
			"facade uint32 Window add did not return an object index");

		// ---------------------------------------------------------------
		// BoothWindow: fixed footprint, adjacent Layer, walkable approach.
		// ---------------------------------------------------------------
		require(!builder.canAddBoothWindow(0, 0, 7, 2, 1, &diagnostic)
			&& diagnostic.find("one-cell-wide, one-Level-high") != std::string::npos,
			"oversized BoothWindow footprint was accepted");
		require(!builder.canAddBoothWindow(1, 0, 4, 1, 1, &diagnostic)
			&& diagnostic.find("adjacent Layer behind") != std::string::npos,
			"BoothWindow on the back-most Layer was accepted");
		require(builder.canAddBoothWindow(0, 0, 4, 1, 1, &diagnostic),
			"valid BoothWindow was refused: " + diagnostic);
		auto const booth = builder.addBoothWindow(0, 0, 4, Window::State::Open);
		require(booth.window.type == SectorObjectType::BoothWindow,
			"BoothWindow add did not report a BoothWindow");
		require(booth.object != nullptr && booth.object->isBoothWindow(),
			"BoothWindow add returned no BoothWindow object");
		require(!bool(booth.traversalResource),
			"a BoothWindow was given a traversal resource");
		requireThrows([&]{ builder.addBoothWindow(0, 0, 3, Window::State::Closed);
			builder.addBoothWindow(0, 0, 3, Window::State::Open); },
			"second BoothWindow onto the same cell was accepted");

		// ---------------------------------------------------------------
		// Light switch: Location host required, control and interaction
		// point wired on acceptance.
		// ---------------------------------------------------------------
		auto const bg = world.addBackground(0, 0, 9, 1, 1);
		requireThrows([&]{ builder.addSectorLightSwitch(bg, 0); },
			"light switch on a Background was accepted");
		auto const lightSwitch = builder.addSectorLightSwitch(room, 2);
		require(lightSwitch.sector != nullptr && lightSwitch.index != ~0u,
			"light switch add returned no object");

		// ---------------------------------------------------------------
		// Marker placement validation now lives in the marker store; the
		// World facade and the store judge identically.
		// ---------------------------------------------------------------
		MarkerStore store(world);
		require(store.canAddSectorMarker(room, 0, 1.5f, &diagnostic),
			"Marker on the ground floor was refused: " + diagnostic);
		require(world.canAddSectorMarker(room, 0, 1.5f, &diagnostic),
			"facade disagrees with the store on a valid Marker cell");
		require(!store.canAddSectorMarker(room, 1, 4.5f, &diagnostic)
			&& diagnostic.find("ground or a Walkway") != std::string::npos,
			"Marker over open air was accepted");
		require(!store.canAddSectorMarker(room, 0, -0.5f, &diagnostic)
			&& diagnostic.find("outside the sector") != std::string::npos,
			"negative-offset Marker was accepted");
		require(!store.canAddSectorMarker(9999, 0, 1.0f, &diagnostic)
			&& diagnostic.find("sector does not exist") != std::string::npos,
			"Marker in an unknown sector was accepted");
		auto const marker = store.addSectorMarker(room, 0, 1.5f);
		require(marker.type == SectorObjectType::Marker,
			"Marker placement did not create a Marker");
		require(!store.canAddSectorMarker(room, 0, 1.5f, &diagnostic)
			&& diagnostic.find("already exists at this position") != std::string::npos,
			"coincident Marker was accepted without allowCoincident");
		require(store.canAddSectorMarkerImpl(room, 0, 1.5f, &diagnostic, true),
			"coincident Marker was refused with allowCoincident");

		// ---------------------------------------------------------------
		// Facade/builder agreement: both reach the same World state.
		// ---------------------------------------------------------------
		require(world.canAddSectorWindow(0, 0, 7, 1, 1, &diagnostic)
			== builder.canAddSectorWindow(0, 0, 7, 1, 1, &diagnostic),
			"facade and builder disagree on a Window verdict");
		require(builder.addSectorWalkway(room, 1, 5).type == SectorObjectType::Walkway,
			"support walkway for the facade bridge was not created");
		auto const facadeBridge = world.addSectorForceBridge(room, 1, 4);
		require(facadeBridge.forceBridge.type == SectorObjectType::ForceBridge,
			"facade Force Bridge add did not reach the builder");
		require(!builder.canAddSectorWalkway(room, 1, 4, &diagnostic),
			"builder does not see the facade-added Force Bridge cell");

		// ---------------------------------------------------------------
		// Built-document walkway add: refused while running, replayed
		// while paused so the column is rebuilt around the new Walkway.
		// ---------------------------------------------------------------
		world.finishBuild();
		requireThrows([&]{ world.addSectorWalkway(room, 1, 7); },
			"built-World walkway add was accepted while running");
		world.pauseSimulation();
		auto const replayed = world.addSectorWalkway(room, 1, 7);
		require(replayed.type == SectorObjectType::Walkway,
			"paused built-World walkway replay did not produce a Walkway");

		std::cout << "WorldBuilder SectorObjects checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "WorldBuilder SectorObjects checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
