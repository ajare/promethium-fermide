#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <core/Furniture.h>
#include <iostream>
#include <stdexcept>
#include <core/World.h>
#include <core/Agent.h>
#include <core/Window.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>
#include "ApplicationResources.h"
#include "DirectoryResourceLocation.h"
#include "Map.h"
#include "MapResources.h"
#include "WorldRenderer.h"
#include "SectorTileset.h"
#include "ObjectTileset.h"

namespace {
namespace render = pf::application::render;
void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}
std::pair<int, int> pngSize(std::filesystem::path const& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 8> data{};
  file.seekg(16); file.read(reinterpret_cast<char*>(data.data()), data.size());
  require(bool(file), "Unable to read atlas PNG dimensions");
  auto dimension = [&](int offset) {
    return int((std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16) |
               (std::uint32_t(data[offset + 2]) << 8) | std::uint32_t(data[offset + 3]));
  };
  return {dimension(0), dimension(4)};
}
render::ObjectTileset installAtlases(std::filesystem::path const& directory) {
  auto manifest = YAML::LoadFile((directory / "Resources.yaml").string());
  auto resource = [&](std::string const& name) {
    for (auto entry : manifest["Resources"]["Resource"])
      if (entry["name"].as<std::string>() == name) return entry;
    throw std::runtime_error("Missing manifest resource: " + name);
  };
  auto sectorsSize = pngSize(directory / resource("SectorAtlasImage")["location"].as<std::string>());
  render::SectorTileset sectors;
  sectors.width = sectorsSize.first; sectors.height = sectorsSize.second;
  for (auto image : resource("SectorAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    render::SectorTileRegion region{image["x"].as<int>(), image["y"].as<int>(),
                                   image["width"].as<int>(), image["height"].as<int>()};
    if (name.starts_with("boundary-")) sectors.boundaries.emplace(name.substr(9), region);
    else sectors.surfaces.emplace(name, region);
  }
  auto objectsSize = pngSize(directory / resource("ObjectAtlasImage")["location"].as<std::string>());
  render::ObjectTileset objects;
  objects.width = objectsSize.first; objects.height = objectsSize.second;
  for (auto image : resource("ObjectAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    bool tintable = name == "agent" || name == "marker" || name.starts_with("human-") ||
                    name == "robot-standing" || name == "cleaning-bot-standing";
    objects.sprites.emplace(name, render::ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(),
        image["width"].as<int>(), image["height"].as<int>()}, tintable});
  }
  render::setSectorTileset(std::move(sectors));
  render::setObjectTileset(objects);
  return objects;
}
std::size_t textured(render::WorldDrawList const& draws, render::WorldDrawList::Texture texture) {
  std::size_t count{};
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == texture) ++count;
  return count;
}
void checkCanvas(render::WorldDrawList const& draws, float width, float height) {
  for (auto const& command : draws.commands()) {
    std::visit([&](auto const& primitive) {
      if constexpr (requires { primitive.clip; }) {
        require(primitive.clip.minimum.x >= 0 && primitive.clip.minimum.y >= 0 &&
                    primitive.clip.maximum.x <= width && primitive.clip.maximum.y <= height,
                "Application geometry must retain its window clip");
      }
      if constexpr (requires { primitive.positions; }) {
        for (auto const& position : primitive.positions)
          require(std::isfinite(position.x) && std::isfinite(position.y), "Non-finite geometry");
        for (auto const& uv : primitive.texcoords)
          require(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1, "Invalid atlas UV");
      }
    }, command);
  }
}
void checkStagedMap(std::filesystem::path const& directory) {
  using namespace wp::application::resourcesystem;
  wp::Logger logger;
  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  registerApplicationResourceTypes(manager);
  manager.addResourceLocationFactory("Directory",
      [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new DirectoryResourceLocation(&logger, path, definition);
      });
  manager.addResourceLocation("Directory", directory.string(), "Resources.yaml");
  pf::application::MapResources resources(manager);
  resources.scan(); resources.load();
  auto map = std::dynamic_pointer_cast<pf::application::Map>(manager.getResource("World", "World"));
  auto world = map->getWorldResource()->world();
  auto const tick = world->getSimulationTick();
  for (auto const& size : {std::pair{1280.0f, 720.0f}, std::pair{320.0f, 200.0f}}) {
    auto draws = render::drawWorld(world, size.first, size.second);
    require(textured(draws, render::WorldDrawList::Texture::SectorAtlas) > 0,
            "Loaded Application Map must draw its sector atlas artwork");
    checkCanvas(draws, size.first, size.second);
  }
  require(world->getSimulationTick() == tick, "Rendering must leave World simulation paused");
  world.reset(); resources.unload();
  require(!map->getWorld(), "Drawing must not retain the World after MapUnload");
}
void checkNoOverlaysAndAgents() {
  auto world = std::make_shared<core::World>("Application render", 12, 2);
  auto room = world->addRoom("Front", 0, 0, 0, 12, 1);
  auto back = world->addRoom("Back", 1, 0, 0, 12, 1);
  world->finishBuild(); world->pauseSimulation();
  auto baseline = render::drawWorld(world, 1280, 720);
  world->addSectorMarker(room, 0, 8.5f, "Invisible annotation");
  world->finishBuild(); world->pauseSimulation();
  auto marked = render::drawWorld(world, 1280, 720);
  require(marked.commands().size() == baseline.commands().size(),
          "Markers and graph vertices must not add Application drawing commands");
  auto id = world->createAgent("Visible", room, 0, 1.5f);
  auto agent = world->lookupAgent(id).entity;
  require(agent != nullptr, "Agent fixture creation failed");
  auto visible = render::drawWorld(world, 1280, 720);
  auto const sprites = textured(visible, render::WorldDrawList::Texture::ObjectAtlas);
  require(sprites >= 2, "Front-Layer Agent must draw pose artwork without an ImGui font/context");
  world->createAgent("Occluded", back, 0, 1.5f);
  auto occluded = render::drawWorld(world, 1280, 720);
  require(textured(occluded, render::WorldDrawList::Texture::ObjectAtlas) == sprites,
          "Agent on a Layer without an aperture must remain hidden");
  require(occluded.commands().size() == visible.commands().size(),
          "Back Layer must not contribute a wireframe or debug overlay");
  checkCanvas(occluded, 1280, 720);
  std::weak_ptr<core::World> lifetime = world;
  world.reset();
  require(lifetime.expired(), "Command building must not keep a stale render World alive");
}
void checkPlayerCamera() {
  auto world = std::make_shared<core::World>("Follow Player", 80, 8);
  world->addRoom("Front", 0, 0, 0, 80, 1);
  auto back = world->addRoom("Back", 1, 5, 0, 80, 1);
  world->finishBuild(); world->pauseSimulation();
  auto id = world->createAgent("Back-Layer player", back, 0, 50.f);
  require(world->setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}),
          "Camera fixture must accept built-in assignment");
  auto centred = [](render::WorldDrawList const& draws, float width) {
    float low = width, high = 0;
    bool found = false;
    for (auto const& command : draws.commands())
      if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == render::WorldDrawList::Texture::ObjectAtlas) {
        found = true;
        for (auto position : triangle->positions) {
          low = std::min(low, position.x); high = std::max(high, position.x);
        }
      }
    require(found && std::abs((low + high) * 0.5f - width * 0.5f) < 0.001f,
            "Player artwork must be visible on its Layer and centred horizontally");
  };
  auto draws = render::drawWorld(world, 640, 360);
  centred(draws, 640); checkCanvas(draws, 640, 360);
  require(world->resumeSimulation(), "Camera fixture cannot resume");
  world->setPlayerMovementDirection(1); world->update(1.f);
  auto const tick = world->getSimulationTick();
  draws = render::drawWorld(world, 320, 200);
  centred(draws, 320); checkCanvas(draws, 320, 200);
  require(world->getSimulationTick() == tick, "Following must not update simulation");
  world->pauseSimulation(); world->clearAgentBehaviourAssignment(id);
  require(textured(render::drawWorld(world, 640, 360), render::WorldDrawList::Texture::ObjectAtlas) == 0,
          "No Player must restore Layer-0 camera rather than reveal a back-Layer Agent");
}

void checkFurniture(std::filesystem::path const& directory, render::ObjectTileset const& atlas) {
  auto world = std::make_shared<core::World>("Application Furniture", 8, 2);
  auto room = world->addRoom("Front", 0, 0, 0, 8, 1);
  auto back = world->addRoom("Back", 1, 0, 0, 8, 1);
  world->attachFurnitureCatalogue("chair.furniture.lua",
      core::FurnitureCatalogue::load(directory / "test-worlds/chair.furniture.lua"));
  world->placeFurniture(room, "chair", 2.25f, 0, "Visible chair");
  world->placeFurniture(back, "chair", 2.25f, 0, "Hidden chair");
  world->finishBuild(); world->pauseSimulation();
  auto draws = render::drawWorld(world, 1280, 720);
  require(textured(draws, render::WorldDrawList::Texture::ObjectAtlas) == 4,
          "Only the front chair should draw, including foreground restoration");
  auto const& chair = atlas.sprites.at("chair").region;
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == render::WorldDrawList::Texture::ObjectAtlas)
      for (auto uv : triangle->texcoords)
        require(uv.x >= float(chair.x) / atlas.width && uv.x <= float(chair.x + chair.width) / atlas.width &&
                    uv.y >= float(chair.y) / atlas.height && uv.y <= float(chair.y + chair.height) / atlas.height,
                "Furniture usable-point Markers must not draw over its artwork");
  checkCanvas(draws, 1280, 720);
}
void checkNestedApertures() {
  auto world = std::make_shared<core::World>("Application apertures", 6, 2);
  while (world->getLayerCount() < 3) world->addLayer();
  world->addRoom("Front", 0, 0, 0, 6, 1);
  world->addRoom("Middle", 1, 0, 0, 6, 1);
  world->addBackground(2, 0, 0, 6, 1, {23, 67, 109});
  world->finishBuild(); world->pauseSimulation();
  auto hidden = render::drawWorld(world, 1280, 720);
  auto hasBackColour = [](render::WorldDrawList const& draws) {
    for (auto const& command : draws.commands())
      if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
          triangle && triangle->colour == render::packColour(23, 67, 109, 255)) return true;
    return false;
  };
  require(!hasBackColour(hidden), "Deeper Background must not leak through an opaque Room");
  world->addSectorWindow(0, 0, 1, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->addSectorWindow(1, 0, 2, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->finishBuild(); world->pauseSimulation();
  auto revealed = render::drawWorld(world, 1280, 720);
  require(hasBackColour(revealed), "Aligned clear Windows must reveal the deeper Background");
  for (auto const& command : revealed.commands())
    if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
        triangle && triangle->colour == render::packColour(23, 67, 109, 255))
      require(triangle->clip.maximum.x - triangle->clip.minimum.x < 3 * CORE_CELL_WIDTH_PIXELS,
              "Deeper Background must retain the intersection of both Window apertures");
  checkCanvas(revealed, 1280, 720);
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the staged Application resource directory");
    auto directory = std::filesystem::path(argv[1]);
    // The same source manifest supplies the CPU-only artwork rectangles; no
    // graphics context, texture upload, auxiliary manifest or ImGui is needed.
    auto atlas = installAtlases(directory);
    checkStagedMap(directory);
    checkNoOverlaysAndAgents();
    checkPlayerCamera();
    checkFurniture(directory, atlas);
    checkNestedApertures();
    require(render::drawWorld(nullptr, 1280, 720).commands().empty(), "Null World must draw nothing");
    auto empty = std::make_shared<core::World>("Empty", 4, 2);
    require(render::drawWorld(empty, 1280, 720).commands().empty(), "Empty World must draw nothing");
    require(render::drawWorld(empty, 0, 0).commands().empty(), "Minimized window must draw nothing");
    render::clearObjectTileset(); render::clearSectorTileset();
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
