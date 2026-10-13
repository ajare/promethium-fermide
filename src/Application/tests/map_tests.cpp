#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <core/World.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>

#include "DirectoryResourceLocation.h"

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

void checkOwnership() {
  std::weak_ptr<void const> finalLifetime;
  {
    Map map("Map", "Application", "", {}, nullptr);
    Map const& view = map;
    require(!map.getWorld() && !view.getWorld(), "Map must start without a World");

    auto first = std::make_unique<core::World>("First", 2, 1);
    auto const firstLifetime = first->getLifetimeToken();
    auto* firstPointer = first.get();
    map.setWorld(std::move(first));
    require(!first && map.getWorld() == firstPointer && view.getWorld() == firstPointer,
            "Map must take exclusive ownership and expose the same World");
    require(view.getWorld()->getName() == "First" &&
                view.getWorld()->getCellsWide() == 2,
            "World must remain accessible through Map");

    auto second = std::make_unique<core::World>("Second", 3, 1);
    auto const secondLifetime = second->getLifetimeToken();
    map.setWorld(std::move(second));
    require(firstLifetime.expired() && !secondLifetime.expired(),
            "Replacing the World must destroy the old instance");
    require(view.getWorld()->getName() == "Second", "Replacement World missing");

    map.clearWorld();
    require(secondLifetime.expired() && !view.getWorld(),
            "Clearing must destroy the World");
    map.clearWorld(); // Repeated clearing is harmless.

    auto third = std::make_unique<core::World>("Third", 1, 1);
    auto const thirdLifetime = third->getLifetimeToken();
    map.setWorld(std::move(third));
    map.setWorld(nullptr);
    require(thirdLifetime.expired() && !map.getWorld(),
            "Setting a null World must release ownership");

    auto final = std::make_unique<core::World>("Final", 1, 1);
    finalLifetime = final->getLifetimeToken();
    map.setWorld(std::move(final));
  }
  require(finalLifetime.expired(), "Destroying Map must destroy its World");
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

  auto world = std::make_unique<core::World>("Owned", 1, 1);
  auto const lifetime = world->getLifetimeToken();
  map->setWorld(std::move(world));
  resource.reset();
  require(lifetime.expired(), "Resource destruction must release the World");
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
      return new uint8_t[1]{'['}; // Invalid YAML, after the TextFile has been created.
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
    auto source = map->getDependentResource("Yaml");
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
    require(source->getType() == "TextFile" &&
                source->getSource() == "test-worlds/room-height-test-1.world.yaml",
            "Manifest must reference the room-height WorldFile");
    require(map && !map->getWorld() && map->getDependentResource("Yaml") == source,
            "Manifest must declare an unloaded Application Map with a Yaml dependency");
    manager.createResource(map);
    require(map->getWorld() && map->getWorld()->getCellsWide() == 16 &&
                map->getWorld()->getLevelsHigh() == 3 && map->getWorld()->getLayerCount() == 2,
            "Definition factory must deserialize the staged WorldFile");
    dllOnExit();
  }
}
}

int main(int argc, char** argv) {
  try {
    checkOwnership();
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
