#include "WorldChecks.h"
#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void furniture(smoke::Context const& context)
	{
		using smoke::require;
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/chair.world.yaml"));
		require(demo->furniture().size() == 3 && demo->getMarkerIds().size() == 5, "Required chair demonstration World is incomplete");
		demo->advanceTicks(600);
		require(std::abs(demo->getSimulationSnapshot().agents.front().globalPosition.x - 2.75f) < 0.01f, "Demo Agent cannot reach chair");
		auto root = context.temporaryRoot();
		auto cataloguePath = root / "chair.furniture.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/chair.furniture.yaml"), cataloguePath);
		auto world = std::make_shared<core::World>("Chair document", 8, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->attachFurnitureCatalogue("chair.furniture.yaml", core::FurnitureCatalogue::load(cataloguePath));
		auto id = world->placeFurniture(room, "chair", 1.25f, 0, "Desk chair");
		auto marker = world->furniture().front().marker;
		world->renameMarker(marker, "Workstation"); world->finishBuild(); world->pauseSimulation();
		// Unrelated structural reconstruction carries the owned layout/identity.
		world->applyLocationEdit(world->planResizeLocation(room, 0, 0, 8, 2));
		require(world->furniture().front().id == id && world->lookupMarker(marker)->getName() == "Workstation", "Replay lost chair identities");
		for (auto filename : { "chair.world.yaml", "chair.world" })
		{
			world->saveTo((root / filename).string());
			auto loaded = core::loadWorldDocument(root / filename);
			require(loaded->furniture().size() == 1 && loaded->furniture().front().id == id
				&& loaded->furniture().front().x == 1.25f && loaded->furniture().front().marker == marker
				&& loaded->lookupMarker(marker)->getName() == "Workstation"
				&& loaded->furnitureCatalogueFilename() == "chair.furniture.yaml", "World format lost Furniture reference or identities");
			loaded->resetSimulation();
			require(loaded->furniture().front().marker == marker, "Reset lost chair Marker identity");
			loaded->pauseSimulation();
			auto nextId = loaded->placeFurniture(room, "chair", 4, 0, "New chair");
			require(nextId > id && loaded->furniture().back().marker.value > marker.value, "Reopening reused Furniture or Marker identity");
		}
		auto expectFailure = [&](std::string const& fragment) {
			try { (void)core::loadWorldDocument(root / "chair.world.yaml"); }
			catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); return; }
			require(false, "Incompatible Furniture dependency was silently accepted");
		};
		auto catalogue = YAML::LoadFile(cataloguePath.string());
		auto saveCatalogue = [&] { std::ofstream file(cataloguePath); file << catalogue; };
		catalogue["furnitureCatalogue"]["definitions"][0]["label"] = "Renamed chair";
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["label"] = "Renamed seat";
		saveCatalogue();
		auto revised = core::loadWorldDocument(root / "chair.world.yaml");
		require(revised->furnitureCatalogue()->definition("chair")->label == "Renamed chair"
			&& revised->lookupMarker(marker)->getName() == "Workstation", "Reload used an embedded snapshot or renamed an owned Marker");
		catalogue["furnitureCatalogue"]["uuid"] = "e78a6c36-7902-4abc-9c90-1876058b32f4"; saveCatalogue(); expectFailure("UUID mismatch");
		catalogue["furnitureCatalogue"]["uuid"] = world->furnitureCatalogue()->uuid();
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["key"] = "removed-seat"; saveCatalogue(); expectFailure("Missing Furniture usable-point key");
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["key"] = "seat";
		catalogue["furnitureCatalogue"]["definitions"][0]["key"] = "removed-chair"; saveCatalogue(); expectFailure("Missing Furniture definition");
		std::filesystem::remove(cataloguePath); expectFailure("Missing Furniture catalogue");
		core::World legacy("No Furniture", 2, 1); legacy.addCorridor(0, 0, 2); legacy.finishBuild();
		legacy.saveTo((root / "legacy.world").string());
		require(!core::loadWorldDocument(root / "legacy.world")->furnitureCatalogue(), "Unfurnished World acquired a catalogue dependency");
		// Portable references survive moving the complete project directory.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/chair.furniture.yaml"), cataloguePath);
		auto moved = root / "moved"; std::filesystem::create_directory(moved);
		std::filesystem::rename(cataloguePath, moved / cataloguePath.filename());
		std::filesystem::rename(root / "chair.world", moved / "chair.world");
		require(core::loadWorldDocument(moved / "chair.world")->furniture().front().marker == marker, "Project relocation broke relative Furniture reference");
	}
}
