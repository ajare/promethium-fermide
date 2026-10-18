#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Exceptions.h"
#include "core/FurnitureStore.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("FurnitureStore check failed: " + what);
		++checksRun;
	}

	std::filesystem::path fixture(std::string const& name)
	{
		return std::filesystem::path(PF_SOURCE_DIR)
			/ "src/headless/smoke/fixtures/furniture" / name;
	}

	std::shared_ptr<const core::FurnitureCatalogue> chairCatalogue()
	{
		return core::FurnitureCatalogue::readFile(fixture("chair.furniture.lua"));
	}

	bool throws(std::function<void()> fn)
	{
		try { fn(); return false; }
		catch (core::WorldException const&) { return true; }
		catch (...) { return false; }
	}
}

int main()
{
	using namespace core;

	try
	{
		// The store is constructed directly against a World it does not own:
		// the ADR 0022 proof-of-pattern is that the Furniture instance,
		// catalogue-reference, and usable-point resolution family is
		// unit-testable without driving the World facade. World keeps the
		// state it edits (ADR 0001); the collaborator holds only a
		// reference.

		// ---------------------------------------------------------------
		// Catalogue attach: Resource-name rules and the instance pin.
		// ---------------------------------------------------------------
		{
			World world("fs-attach", 12, 3);
			FurnitureStore store(world);
			auto catalogue = chairCatalogue();
			require(throws([&] { store.attachFurnitureCatalogue("", nullptr); }),
				"null catalogue with empty name accepted");
			require(throws([&] { store.attachFurnitureCatalogue("/abs/chair.furniture.lua", catalogue); }),
				"absolute Resource name accepted");
			require(throws([&] { store.attachFurnitureCatalogue("dir/chair.furniture.lua", catalogue); }),
				"Resource name with a parent path accepted");
			require(!world.furnitureCatalogue(), "failed attach installed a catalogue");

			store.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
			require(world.furnitureCatalogue() == catalogue
				&& world.furnitureCatalogueResourceName() == "chair.furniture.lua",
				"attach did not install the catalogue reference");

			// An empty instance list does not pin the catalogue.
			store.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
			require(world.furnitureCatalogue() == catalogue,
				"repeat identical attach changed the catalogue");

			// Once an instance uses the catalogue, any replacement is refused.
			world.addRoom("Room", 0, 0, 0, 11, 1);
			auto const id = store.placeFurniture(0, "chair", 1, 0, "Chair");
			require(id != 0 && store.isFurnitureMarker(world.furniture().front().destinations.front().marker),
				"placed instance not visible through the store");
			require(throws([&] { store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue()); }),
				"different catalogue package accepted while instances use it");
			require(throws([&] { store.attachFurnitureCatalogue("renamed.furniture.lua", catalogue); }),
				"same catalogue under a new Resource name accepted while instances use it");
			store.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
			require(world.furnitureCatalogue() == catalogue,
				"identical re-attach with live instances was refused");
		}

		// ---------------------------------------------------------------
		// Placement validation: every refusal is judged before any write.
		// ---------------------------------------------------------------
		{
			World bare("fs-bare", 12, 3);
			FurnitureStore bareStore(bare);
			std::string diagnostic;
			require(!bareStore.canPlaceFurniture(0, "chair", 1, 0, "Chair", &diagnostic, 0)
				&& diagnostic.find("No Furniture catalogue") != std::string::npos,
				"placement without a catalogue was accepted");

			World world("fs-place", 12, 3);
			FurnitureStore store(world);
			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.addRoom("Room", 0, 0, 0, 11, 1);

			require(!store.canPlaceFurniture(0, "stool", 1, 0, "Stool", &diagnostic, 0)
				&& diagnostic.find("Missing Furniture definition") != std::string::npos,
				"unknown definition key accepted");
			require(!store.canPlaceFurniture(0, "chair", 1, 0, "Chair", &diagnostic, -1)
				&& diagnostic.find("Local depth") != std::string::npos,
				"negative Local depth accepted");
			require(!store.canPlaceFurniture(999999u, "chair", 1, 0, "Chair", &diagnostic, 0)
				&& diagnostic.find("Room, Corridor or Facade") != std::string::npos,
				"placement outside the sector list accepted");
			require(!store.canPlaceFurniture(0, "chair", 1, 0.5f, "Chair", &diagnostic, 0)
				&& diagnostic.find("floor-aligned") != std::string::npos,
				"non-floor-aligned y accepted");
			require(!store.canPlaceFurniture(0, "chair", 10.5f, 0, "Chair", &diagnostic, 0)
				&& diagnostic.find("inside one Location") != std::string::npos,
				"footprint beyond the sector edge accepted");

			// The first chair places cleanly.
			auto const first = store.placeFurniture(0, "chair", 1, 0, "First");
			require(world.furniture().size() == 1 && world.furniture().front().id == first,
				"clean placement did not append the instance");
			require(!store.canPlaceFurniture(0, "chair", 1, 0, "Overlap", &diagnostic, 0)
				&& diagnostic.find("footprint overlaps") != std::string::npos,
				"same-depth overlapping footprint accepted");
			require(store.canPlaceFurniture(0, "chair", 1, 0, "Different depth", &diagnostic, 1),
				diagnostic);
			// A repeated name collides with the first chair's owned Marker.
			require(!store.canPlaceFurniture(0, "chair", 4, 0, "First", &diagnostic, 0)
				&& diagnostic.find("Marker with this name already exists") != std::string::npos,
				"duplicate owned Marker name accepted");
			require(store.canPlaceFurniture(0, "chair", 4, 0, "Second", &diagnostic, 0), diagnostic);

			// Refusals leave the World exactly as it was found.
			require(world.furniture().size() == 1, "a refused placement appended an instance");
		}

		// ---------------------------------------------------------------
		// The pause gate: a built, running World refuses every edit.
		// ---------------------------------------------------------------
		{
			World world("fs-gate", 12, 3);
			FurnitureStore store(world);
			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.addRoom("Room", 0, 0, 0, 11, 1);
			auto const id = store.placeFurniture(0, "chair", 1, 0, "Chair");
			world.finishBuild();
			world.markSaved();
			std::string diagnostic;

			world.resumeSimulation();
			require(!store.canPlaceFurniture(0, "chair", 4, 0, "Second", &diagnostic, 0)
				&& diagnostic.find("Pause the simulation before placing") != std::string::npos,
				"placement accepted while running");
			require(!store.canEditFurniture(id, 3, 0, "Chair", &diagnostic, std::nullopt)
				&& diagnostic.find("Pause the simulation before editing") != std::string::npos,
				"edit accepted while running");
			require(!store.canRemoveFurniture(id, &diagnostic)
				&& diagnostic.find("Pause the simulation before deleting") != std::string::npos,
				"removal accepted while running");
			require(!world.isModified(), "a refused running edit dirtied the document");
		}

		// ---------------------------------------------------------------
		// Edit and removal transactions on a paused World.
		// ---------------------------------------------------------------
		{
			World world("fs-edit", 12, 3);
			FurnitureStore store(world);
			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.addRoom("Room", 0, 0, 0, 11, 1);
			auto const id = store.placeFurniture(0, "chair", 1, 0, "Chair");
			world.finishBuild();
			world.pauseSimulation();
			std::string diagnostic;

			require(!store.canEditFurniture(id + 100, 3, 0, "Ghost", &diagnostic, std::nullopt)
				&& diagnostic.find("no longer exists") != std::string::npos,
				"edit of an unknown instance accepted");
			require(!store.editFurniture(id, 1, 0, "Chair", &diagnostic, std::nullopt),
				"an unchanged edit was reported as applied");
			require(store.editFurniture(id, 3, 0, "Moved chair", &diagnostic), diagnostic);
			require(world.furniture().size() == 1
				&& world.furniture().front().x == 3.f
				&& world.furniture().front().name == "Moved chair",
				"the edit did not reach the live instance");

			// The support diagnostic names what rests on the floor cell.
			require(store.furnitureSupportDiagnostic(0, 3, 0).find("Moved chair") != std::string::npos,
				"support diagnostic missed the resting instance");
			require(store.furnitureSupportDiagnostic(0, 8, 0).empty(),
				"support diagnostic reported an empty cell");

			require(!store.canRemoveFurniture(id + 100, &diagnostic)
				&& diagnostic.find("no longer exists") != std::string::npos,
				"removal of an unknown instance accepted");
			require(store.removeFurniture(id, &diagnostic), diagnostic);
			require(world.furniture().empty(), "removal left the instance behind");
			require(!store.isFurnitureMarker(MarkerId{ 1 }),
				"a Marker stayed Furniture-owned after removal");
		}

		// ---------------------------------------------------------------
		// Usable-point resolution: the single-pass chain and its misses.
		// ---------------------------------------------------------------
		{
			World world("fs-resolve", 12, 3);
			FurnitureStore store(world);
			require(!store.resolveUsablePoint(MarkerId{ 1 }),
				"resolution without a catalogue produced a hit");

			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.addRoom("Room", 0, 0, 0, 11, 1);
			auto const id = store.placeFurniture(0, "chair", 1, 0, "Chair");
			auto const seat = world.furniture().front().destinations.front().marker;

			auto const resolved = store.resolveUsablePoint(seat);
			require(resolved && resolved.marker == seat
				&& resolved.instance && resolved.instance->id == id
				&& resolved.definition && resolved.destination->key == "seat"
				&& resolved.point && resolved.sector,
				"usable-point chain did not resolve completely");
			require(!store.resolveUsablePoint(MarkerId{ 999 }),
				"an unknown Marker resolved to a usable point");
			require(!store.resolveUsablePoint(MarkerId{}),
				"the null Marker resolved to a usable point");
			require(store.furnitureForMarker(seat) == resolved.instance
				&& store.furnitureForMarker(MarkerId{ 999 }) == nullptr,
				"furnitureForMarker disagrees with the resolver");
		}

		// ---------------------------------------------------------------
		// Reload refusals: extension, pause gate, and unselected package.
		// ---------------------------------------------------------------
		{
			World world("fs-reload", 12, 3);
			FurnitureStore store(world);
			world.addRoom("Room", 0, 0, 0, 11, 1);
			world.finishBuild();
			world.pauseSimulation();
			std::string diagnostic;

			require(!store.reloadFurnitureCatalogue(fixture("chair.txt"), &diagnostic)
				&& diagnostic.find("must end with .furniture.lua") != std::string::npos,
				"a non-catalogue extension was accepted");
			require(!store.reloadFurnitureCatalogue(fixture("chair.furniture.lua"), &diagnostic)
				&& diagnostic.find("Select this catalogue before reloading") != std::string::npos,
				"reload without a selected catalogue was accepted");

			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.resumeSimulation();
			require(!store.reloadFurnitureCatalogue(fixture("chair.furniture.lua"), &diagnostic)
				&& diagnostic.find("Pause the simulation before reloading") != std::string::npos,
				"reload accepted while running");
		}

		// ---------------------------------------------------------------
		// Facade (design pattern) and store agree on every shared judgement.
		// ---------------------------------------------------------------
		{
			World world("fs-facade", 12, 3);
			FurnitureStore store(world);
			store.attachFurnitureCatalogue("chair.furniture.lua", chairCatalogue());
			world.addRoom("Room", 0, 0, 0, 11, 1);
			auto const id = store.placeFurniture(0, "chair", 1, 0, "Chair");
			auto const seat = world.furniture().front().destinations.front().marker;
			std::string a, b;
			require(world.canPlaceFurniture(0, "stool", 1, 0, "S", &a, 0)
					== store.canPlaceFurniture(0, "stool", 1, 0, "S", &b, 0) && a == b,
				"facade and store disagree on the missing-definition refusal");
			require(world.isFurnitureMarker(seat) == store.isFurnitureMarker(seat)
					&& world.isFurnitureMarker(MarkerId{ 999 }) == store.isFurnitureMarker(MarkerId{ 999 }),
				"facade and store disagree on Marker ownership");
			world.resumeSimulation();
			require(world.canEditFurniture(id, 3, 0, "Chair", &a, std::nullopt)
					== store.canEditFurniture(id, 3, 0, "Chair", &b, std::nullopt) && a == b,
				"facade and store disagree on the running-edit refusal");
			require(world.canRemoveFurniture(id, &a) == store.canRemoveFurniture(id, &b) && a == b,
				"facade and store disagree on the running-removal refusal");
			require(world.furniture().front().id == id, "facade instance list disagrees");
		}

		std::cout << "FurnitureStore checks passed: " << checksRun << "\n";
		return 0;
	}
	catch (std::exception const& e)
	{
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
