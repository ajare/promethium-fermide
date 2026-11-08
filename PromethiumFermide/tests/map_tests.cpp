#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <core/World.h>
#include <game/WorldRuntime.h>
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

// A `requires` clause only reports "no such member" instead of failing to
// compile when the expression depends on a template parameter, so the
// absence-of-accessor half of the contract needs these two concepts. Without
// them the check could not be written at all: the compiler would simply
// reject the line rather than tell us the member is missing.
template <typename MapLike>
concept ExposesRawWorld = requires(MapLike& map) { map.getWorld(); };

template <typename MapLike>
concept ExposesWorldResource = requires(MapLike& map) { map.getWorldResource(); };

// The boundary contract (#970, spec #960), asserted at compile time.
//
// These used to pin `Map::getWorld()`'s return type. They now pin the shape
// of the façade (design pattern) the Map hands over instead: a Map gives out
// a WorldRuntime, the runtime gives out a `core::World const&`, and there is
// no accessor on Map that yields a World at all - not a mutable one, not
// even a raw const pointer a caller could const_cast into a habit. Whether
// the game can reach mutable World state is a question the compiler answers
// before anything runs.
static_assert(std::is_same_v<decltype(std::declval<Map&>().getRuntime()),
                            std::shared_ptr<game::WorldRuntime>>);
static_assert(std::is_same_v<decltype(std::declval<Map const&>().getRuntime()),
                             std::shared_ptr<game::WorldRuntime>>);
static_assert(!ExposesRawWorld<Map>,
              "Map must not expose a raw World: the runtime's const view is the only World a Map can name");
static_assert(!ExposesRawWorld<Map const>,
              "Map must not expose a raw World, even a const one");
static_assert(!ExposesWorldResource<Map>,
              "Map must not hand out the World resource either: the runtime is the boundary");

// The runtime hands out a const World whichever cv-qualification the runtime
// itself holds, and is never copied or shared out of its owner.
static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime&>().world()), core::World const&>);
static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime const&>().world()), core::World const&>);
static_assert(!std::is_copy_constructible_v<game::WorldRuntime>);
static_assert(!std::is_copy_assignable_v<game::WorldRuntime>);
static_assert(!std::is_constructible_v<game::WorldRuntime, core::World&>);
static_assert(!std::is_constructible_v<game::WorldRuntime, core::World*>);
// The runtime is not movable either (#977). A `WorldAuthoring` handle caches
// the address of the runtime it wraps, so a runtime that could move could
// leave one naming the shell it moved out of. The Map hands the runtime over
// as a `shared_ptr` and the game holds it the same way, so exclusive
// ownership still transfers freely - through the pointer, to a runtime that
// stays pinned where it was made.
static_assert(!std::is_move_constructible_v<game::WorldRuntime>);
static_assert(!std::is_move_assignable_v<game::WorldRuntime>);

void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}

void checkBorrowing(wp::application::resourcesystem::ResourceManager& manager,
                    std::shared_ptr<WorldResource> const& source) {
  auto const& runtime = source->runtime();
  auto const lifetime = runtime->world().getLifetimeToken();
  {
    Map map("Map", "Application", "", {}, nullptr);
    Map const& view = map;
    require(!map.getRuntime() && !view.getRuntime(), "Map must start without a World");
    map.setWorldResource(source);
    require(map.getRuntime() == runtime &&
                &map.getRuntime()->world() == &runtime->world() &&
                &view.getRuntime()->world() == &runtime->world(),
            "Map must borrow the WorldResource's exact World instance");

    // Reject an uncreated World resource without discarding the valid binding:
    // a resource that has not run its create() has no runtime to drive.
    auto uncreated = std::static_pointer_cast<WorldResource>(manager.getResource("Citadel"));
    require(!uncreated->runtime(), "An uncreated World resource must hold no runtime");
    bool rejected = false;
    try { map.setWorldResource(uncreated); } catch (std::invalid_argument const&) { rejected = true; }
    require(rejected && map.getRuntime() == runtime, "Invalid binding must preserve the current resource");

    map.clearWorldResource();
    require(!map.getRuntime() && !lifetime.expired(),
            "Clearing Map must not destroy a World owned by its resource");
    map.clearWorldResource();
    map.setWorldResource(source);
    map.setWorldResource(nullptr);
    require(!map.getRuntime() && !lifetime.expired(), "A null binding must only clear the adapter");
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
  require(map && !map->getRuntime(), "Factory must create an empty Application Map");
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
    require(rejected && !map->getRuntime() && !manager.isResourceLoaded(map) &&
                !manager.isResourceCreated(map) && !manager.isResourceCreated(source),
            "Failed MapLoad must roll back its resource acquisition and source data");
    fail = false;
    resources.load();
    require(map->getRuntime() && manager.isResourceLoaded(map),
            "A failed load must permit a subsequent successful retry");
  }
  auto map = std::dynamic_pointer_cast<Map>(manager.getResource("World", "World"));
  require(!map->getRuntime() && !manager.isResourceLoaded(map) && !manager.isResourceCreated(map),
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
    require(map && !map->getRuntime() && map->getDependentResource("World") == source,
            "Manifest must declare an unloaded Application Map with a World dependency");
    manager.createResource(map);
    require(map->getRuntime() && map->getRuntime()->world().getCellsWide() == 16 &&
                map->getRuntime()->world().getLevelsHigh() == 6 &&
                map->getRuntime()->world().getLayerCount() == 2,
            "WorldResource must deserialize the repository WorldFile");
    auto worldResource = std::static_pointer_cast<WorldResource>(source);
    require(map->getRuntime() == worldResource->runtime() &&
                &map->getRuntime()->world() == &worldResource->runtime()->world(),
            "Definition factory must reuse the dependency's World without deserializing a duplicate");
    checkBorrowing(manager, worldResource);
    auto const lifetime = map->getRuntime()->world().getLifetimeToken();
    manager.destroyResources({source});
    require(lifetime.expired() && !map->getRuntime(),
            "Map must not keep a stale World alive after dependency data is destroyed");
    manager.createResource(source);
    require(map->getRuntime() == worldResource->runtime() && map->getRuntime(),
            "A retained WorldResource must expose its newly created World");
    dllOnExit();
  }
}
}

int main(int argc, char** argv) {
  try {
    checkFactory();
    require(argc == 2, "Expected the repository resource directory");
    checkManifest(argv[1]);
    checkFailureCleanup(argv[1]);
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
