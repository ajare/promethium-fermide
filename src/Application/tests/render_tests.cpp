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
#include "../../headless/smoke/support/AgentPoseTestAccess.h"
#include <core/Window.h>
#include "PlayerTransportSelectorDrawing.h"
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
  render::ObjectTileset agents;
  agents.width = objectsSize.first; agents.height = objectsSize.second;
  for (auto image : resource("AgentAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    require(!objects.sprites.contains(name), "Agent tiles must be removed from ObjectAtlas");
    agents.sprites.emplace(name, render::ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(),
        image["width"].as<int>(), image["height"].as<int>()}, true});
  }
  require(agents.sprites.size() == 8, "AgentAtlas must contain all eight extracted tiles");
  render::setAgentTileset(std::move(agents));
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
  auto const sprites = textured(visible, render::WorldDrawList::Texture::AgentAtlas);
  require(sprites >= 2, "Front-Layer Agent must draw pose artwork without an ImGui font/context");
  world->createAgent("Occluded", back, 0, 1.5f);
  auto occluded = render::drawWorld(world, 1280, 720);
  require(textured(occluded, render::WorldDrawList::Texture::AgentAtlas) == sprites,
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
          triangle && triangle->texture == render::WorldDrawList::Texture::AgentAtlas) {
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
  require(textured(render::drawWorld(world, 640, 360), render::WorldDrawList::Texture::AgentAtlas) == 0,
          "No Player must restore Layer-0 camera rather than reveal a back-Layer Agent");
}

void checkStairwellDoorwayOutlines() {
  auto world = std::make_shared<core::World>("Stairwell exits", 8, 2);
  auto bottom = world->addRoom("Bottom", 0, 0, 0, 8, 1);
  world->addRoom("Top", 0, 1, 0, 8, 1);
  auto stairs = world->addStairwell(1, 0, 2, 2, CORE_SIDE_LEFT);
  world->finishBuild(); world->pauseSimulation();
  auto id = world->createAgent("Player", bottom, 0, 3.f);
  require(world->setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Stairwell camera player");
  auto outlines = [](render::WorldDrawList const& draws) {
    unsigned count = 0;
    auto const& commands = draws.commands();
    for (size_t i = 0; i + 3 < commands.size(); ++i) {
      auto horizontal = std::get_if<render::WorldDrawList::Line>(&commands[i]);
      auto vertical = std::get_if<render::WorldDrawList::Line>(&commands[i + 1]);
      if (!horizontal || !vertical || horizontal->from.y != horizontal->to.y || vertical->from.x != vertical->to.x) continue;
      float width = std::abs(horizontal->to.x - horizontal->from.x);
      float height = std::abs(vertical->to.y - vertical->from.y);
      if (height == 0 || std::abs(width / height - CORE_STAIRWELL_PASSAGE_WIDTH * CORE_CELL_WIDTH_PIXELS
          / (CORE_STAIRWELL_DOORWAY_HEIGHT * CORE_LEVEL_HEIGHT_PIXELS)) > .001f) continue;
      bool rectangle = true;
      for (size_t side = 0; side < 4; ++side) {
        auto line = std::get_if<render::WorldDrawList::Line>(&commands[i + side]);
        auto next = std::get_if<render::WorldDrawList::Line>(&commands[i + (side + 1) % 4]);
        rectangle = rectangle && line && next && line->colour == render::packColour(0, 0, 0) && line->thickness == 1.f
          && line->to.x == next->from.x && line->to.y == next->from.y;
      }
      if (rectangle) {
        // Emitted geometry outside its aperture clip is not a visible doorway.
        for (size_t side = 0; side < 4; ++side) {
          auto const& line = std::get<render::WorldDrawList::Line>(commands[i + side]);
          rectangle = rectangle && line.from.x >= line.clip.minimum.x - .001f
            && line.from.x <= line.clip.maximum.x + .001f
            && line.from.y >= line.clip.minimum.y - .001f
            && line.from.y <= line.clip.maximum.y + .001f;
        }
        if (rectangle) ++count;
      }
    }
    return count;
  };
  auto landingCount = outlines(render::drawWorld(world, 1280, 720));
  if (landingCount != 2) throw std::runtime_error(
          "Open Stairwell doorways must have visible boxes when viewed through landing apertures: expected 2, got "
          + std::to_string(landingCount));
  world->resumeSimulation(); world->requestPlayerDoorAction(); world->update(.2f);
  require(world->lookupAgent(id).entity->getSector()->getIndex() == stairs, "Player must enter Stairwell to view its Layer");
  auto draws = render::drawWorld(world, 1280, 720);
  auto count = outlines(draws);
  if (count != 2) throw std::runtime_error("Viewed Stairwell must outline every Level doorway: expected 2, got " + std::to_string(count));
  checkCanvas(draws, 1280, 720);
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

void checkPlayerSelectorDrawing() {
  core::World::PlayerTransportSelector selector;
  selector.focus = 18;
  for (uint32_t stop = 0; stop < 30; ++stop) selector.stops.push_back({stop, std::to_string(stop), stop == 0, stop == 0 || stop == 18});
  for (bool horizontal : {false, true}) {
    selector.horizontal = horizontal;
    int labels = 0, focus = 0, green = 0, amber = 0, white = 0;
    pf::drawPlayerTransportSelector(selector, 100.f, 300.f,
      [&](float x0, float y0, float x1, float y1, uint32_t colour) {
        require(x1 > x0 && y1 > y0, "Selector must emit nondegenerate fixed-size geometry");
        if (colour == 0xe0282020u) { ++labels; require(x1 - x0 == 34.f && y1 - y0 == 18.f, "Labels must stay readable at fixed pixel size"); }
        if (colour == 0xff60e060u) ++green;
        if (colour == 0xff40bfffu) ++amber;
        if (colour == 0xffffffffu) ++white;
      },
      [&](float, float, float, float, uint32_t colour) { require(colour == 0xffffff00u, "Focus must be cyan"); ++focus; });
    require(labels == 8 && focus == 1 && green > 0 && amber > 0 && white > 0,
      "Windowed selector must show current outside window, pending, focus and normal labels");
  }
}

void checkSectorTileScaling() {
  render::SectorTileset tiles;
  tiles.width = 512; tiles.height = 320;
  tiles.surfaces.emplace("corridor", render::SectorTileRegion{0, 0, 128, 320});
  render::setSectorTileset(tiles);
  render::WorldDrawList commands({{0, 0}, {1000, 1000}});
  require(render::drawSectorTileSurface("corridor", &commands, {0, 0}, {256, 240}, render::White, 2.f),
      "Station corridor must render");
  require(textured(commands, render::WorldDrawList::Texture::SectorAtlas) == 4,
      "Two World cells must each render a tile at zoom 2 independent of source size");
  auto const& triangle = std::get<render::WorldDrawList::Triangle>(commands.commands().front());
  float maxX{}, maxY{}, maxUvY{};
  for (int i = 0; i < 3; ++i) {
    maxX = std::max(maxX, triangle.positions[i].x);
    maxY = std::max(maxY, triangle.positions[i].y);
    maxUvY = std::max(maxUvY, triangle.texcoords[i].y);
  }
  require(maxX == 128.f && maxY == 240.f && maxUvY < .76f && maxUvY > .74f,
      "Tile must fill 1x1 World units and crop a partial-height cell");
  render::WorldDrawList missing({{0, 0}, {1000, 1000}});
  require(render::drawSectorTileSurface("room", &missing, {0, 0}, {64, 160}, render::White),
      "Missing tile must emit fallback geometry");
  auto const& fallback = std::get<render::WorldDrawList::Triangle>(missing.commands().front());
  require(fallback.texture == render::WorldDrawList::Texture::None
      && fallback.colour == render::packColour(255, 0, 255), "Missing tile must be opaque magenta");
  require(!render::drawSectorTileBoundary("floor", &missing, {0, 0}, {64, 2}),
      "Missing boundary must request plain-line fallback");
  render::setSectorTileset({});
  render::WorldDrawList unavailable({{0, 0}, {1000, 1000}});
  require(render::drawSectorTileSurface("corridor", &unavailable, {0, 0}, {64, 160}, render::White),
      "Unavailable ImageSet must emit fallback geometry");
  render::clearSectorTileset();
}

void checkAgentArtworkFallback() {
  auto near = [](float a, float b) { return std::abs(a - b) < .001f; };
  auto world = std::make_shared<core::World>("Agent artwork", 8, 2);
  world->addRoom("Room", 0, 0, 0, 8, 2); world->finishBuild(); world->pauseSimulation();
  auto id = world->createAgent("Actor", 0, 0, 3.5f);
  auto* agent = world->lookupAgent(id).entity;
  core::AgentPoseTestAccess::set(*agent, core::Pose::Sitting);
  auto const physical = agent->getBounds().getSize();
  for (int present = 0; present < 4; ++present) {
    render::ObjectTileset tiles; tiles.width = 128; tiles.height = 128;
    if (present & 1) tiles.sprites.emplace("human-standing", render::ObjectSprite{{0, 0, 26, 72}, true});
    if (present & 2) tiles.sprites.emplace("human-sitting", render::ObjectSprite{{32, 0, 26, 43}, true});
    render::setAgentTileset(std::move(tiles));
    auto list = render::drawWorld(world, 1024, 720);
    unsigned missing = 0;
    float left = 1e6f, right = -1e6f, top = 1e6f, bottom = -1e6f;
    for (auto const& command : list.commands())
      if (auto triangle = std::get_if<render::WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == render::WorldDrawList::Texture::None
          && triangle->colour == render::packColour(255, 0, 255, 255)) {
        ++missing;
        for (auto p : triangle->positions) {
          left = std::min(left, p.x); right = std::max(right, p.x);
          top = std::min(top, p.y); bottom = std::max(bottom, p.y);
        }
      }
    if (present == 3) require(missing == 0 && textured(list, render::WorldDrawList::Texture::AgentAtlas) == 4,
        "Complete Agent artwork must use its independent texture channel");
    else require(missing == 4 && near(right - left, agent->getWidth() * 64.f)
        && near(bottom - top, agent->getHeight() * 160.f),
        "Absent Agent atlas, pose or Standing tile must draw magenta physical bounds");
    auto const after = agent->getBounds().getSize();
    require(near(after.x, physical.x) && near(after.y, physical.y), "Agent artwork changed simulation geometry");
  }
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the repository resource directory");
    auto directory = std::filesystem::path(argv[1]);
    // The same source manifest supplies the CPU-only artwork rectangles; no
    // graphics context, texture upload, auxiliary manifest or ImGui is needed.
    checkSectorTileScaling();
    auto atlas = installAtlases(directory);
    checkStagedMap(directory);
    checkNoOverlaysAndAgents();
    checkPlayerCamera();
    checkPlayerSelectorDrawing();
    checkStairwellDoorwayOutlines();
    checkFurniture(directory, atlas);
    checkNestedApertures();
    checkAgentArtworkFallback();
    require(render::drawWorld(nullptr, 1280, 720).commands().empty(), "Null World must draw nothing");
    auto empty = std::make_shared<core::World>("Empty", 4, 2);
    require(render::drawWorld(empty, 1280, 720).commands().empty(), "Empty World must draw nothing");
    require(render::drawWorld(empty, 0, 0).commands().empty(), "Minimized window must draw nothing");
    render::clearObjectTileset(); render::clearSectorTileset(); render::clearAgentTileset();
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
