#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <core/World.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>

#include "DirectoryResourceLocation.h"

#include "ApplicationResources.h"
#include "Map.h"
#include "MapResources.h"

namespace {
using pf::application::Map;

static_assert(std::is_base_of_v<applib::Map, Map>);
static_assert(!std::is_copy_constructible_v<Map>);
static_assert(!std::is_copy_assignable_v<Map>);
static_assert(std::is_same_v<decltype(std::declval<Map&>().getWorld()), core::World*>);
static_assert(std::is_same_v<decltype(std::declval<Map const&>().getWorld()), core::World const*>);

void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}

void checkBorrowing(wp::application::resourcesystem::ResourceManager& manager,
                    std::shared_ptr<WorldResource> const& source) {
  auto const lifetime = source->world()->getLifetimeToken();
  {
    Map map("Map", "Application", "", {}, nullptr);
    Map const& view = map;
    require(!map.getWorld() && !view.getWorld(), "Map must start without a World");
    map.setWorldResource(source);
    require(map.getWorldResource() == source && map.getWorld() == source->world().get() &&
                view.getWorld() == source->world().get(),
            "Map must borrow the WorldResource's exact World instance");

    // Reject an uncreated World resource without discarding the valid binding.
    auto uncreated = std::static_pointer_cast<WorldResource>(manager.getResource("Citadel"));
    bool rejected = false;
    try { map.setWorldResource(uncreated); } catch (std::invalid_argument const&) { rejected = true; }
    require(rejected && map.getWorldResource() == source, "Invalid binding must preserve the current resource");

    map.clearWorldResource();
    require(!map.getWorld() && !map.getWorldResource() && !lifetime.expired(),
            "Clearing Map must not destroy a World owned by its resource");
    map.clearWorldResource();
    map.setWorldResource(source);
    map.setWorldResource(nullptr);
    require(!map.getWorld() && !lifetime.expired(), "A null binding must only clear the adapter");
    map.setWorldResource(source);
  }
  require(!lifetime.expired(), "Map destruction must not destroy another resource's World");
}

void checkFactory() {
  pf::application::MapResourceFactory factory;
  std::unique_ptr<wp::application::resourcesystem::Resource> resource(
      factory.createResource("Example", "Application", "example.world.yaml",
                             {{"Purpose", "Test"}}, nullptr));
  auto* map = dynamic_cast<Map*>(resource.get());
  require(map && !map->getWorld(), "Factory must create an empty Application Map");
  require(map->getName() == "Example" && map->getNamespace() == "Application" &&
              map->getType() == "Map" && map->getSource() == "example.world.yaml" &&
              map->getTag("Purpose") == "Test",
          "Factory must preserve resource metadata");
  require(map->getGridOptions().cellSize == 512.0f,
          "Map must retain the reference acceleration grid size");

}

class FailingWorldLocation final : public DirectoryResourceLocation {
  bool& mFail;
public:
  FailingWorldLocation(wp::Logger* logger, std::string const& path,
                       std::string const& definition, bool& fail)
      : DirectoryResourceLocation(logger, path, definition), mFail(fail) {}
  uint8_t* readData(std::string const& source, uint32_t* size) override {
    if (mFail && source == "test-worlds/room-height-test-1.world.yaml") {
      *size = 1;
      return new uint8_t[1]{'['}; // Invalid YAML for the WorldResource loader.
    }
    return DirectoryResourceLocation::readData(source, size);
  }
};

void checkFailureCleanup(std::string const& directory) {
  using namespace wp::application::resourcesystem;
  wp::Logger logger;
  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  dllOnEntry(&logger, &manager, nullptr);
  bool fail = true;
  manager.addResourceLocationFactory("Directory",
      [&logger, &fail](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new FailingWorldLocation(&logger, path, definition, fail);
      });
  manager.addResourceLocation("Directory", directory, "Resources.yaml");
  {
    pf::application::MapResources resources(manager, &logger);
    resources.scan();
    auto map = std::dynamic_pointer_cast<Map>(manager.getResource("World", "World"));
    auto source = map->getDependentResource("World");
    bool rejected = false;
    try { resources.load(); } catch (std::exception const&) { rejected = true; }
    require(rejected && !map->getWorld() && !manager.isResourceLoaded(map) &&
                !manager.isResourceCreated(map) && !manager.isResourceCreated(source),
            "Failed MapLoad must roll back its resource acquisition and source data");
    fail = false;
    resources.load();
    require(map->getWorld() && manager.isResourceLoaded(map),
            "A failed load must permit a subsequent successful retry");
  }
  auto map = std::dynamic_pointer_cast<Map>(manager.getResource("World", "World"));
  require(!map->getWorld() && !manager.isResourceLoaded(map) && !manager.isResourceCreated(map),
          "Guard destruction must release a loaded resource");
  dllOnExit();
}

void checkManifest(std::string const& directory) {
  using namespace wp::application::resourcesystem;
  // Repeat with fresh managers to exercise registry cleanup between app runs.
  for (int run = 0; run < 2; ++run) {
    wp::Logger logger;
    ResourceManager manager(nullptr, nullptr, nullptr, &logger);
    dllOnEntry(&logger, &manager, nullptr);
    manager.addResourceLocationFactory("Directory",
        [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
          return new DirectoryResourceLocation(&logger, path, definition);
        });
    manager.addResourceLocation("Directory", directory, "Resources.yaml");
    manager.scanLocations();
    auto source = manager.getResource("WorldFile", "World");
    auto resource = manager.getResource("World", "World");
    auto map = std::dynamic_pointer_cast<Map>(resource);
    require(source->getType() == "World" &&
                source->getSource() == "test-worlds/room-height-test-1.world.yaml",
            "Manifest must reference the room-height WorldFile");
    require(map && !map->getWorld() && map->getDependentResource("World") == source,
            "Manifest must declare an unloaded Application Map with a World dependency");
    manager.createResource(map);
    require(map->getWorld() && map->getWorld()->getCellsWide() == 16 &&
                map->getWorld()->getLevelsHigh() == 3 && map->getWorld()->getLayerCount() == 2,
            "WorldResource must deserialize the staged WorldFile");
    auto worldResource = std::static_pointer_cast<WorldResource>(source);
    require(map->getWorldResource() == worldResource && map->getWorld() == worldResource->world().get(),
            "Definition factory must reuse the dependency's World without deserializing a duplicate");
    checkBorrowing(manager, worldResource);
    auto const lifetime = map->getWorld()->getLifetimeToken();
    manager.destroyResources({source});
    require(lifetime.expired() && !map->getWorld(),
            "Map must not keep a stale World alive after dependency data is destroyed");
    manager.createResource(source);
    require(map->getWorld() == worldResource->world().get() && map->getWorld(),
            "A retained WorldResource must expose its newly created World");
    dllOnExit();
  }
}
}

int main(int argc, char** argv) {
  try {
    checkFactory();
    require(argc == 2, "Expected the staged application resource directory");
    checkManifest(argv[1]);
    checkFailureCleanup(argv[1]);
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
