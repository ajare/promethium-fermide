#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <yaml-cpp/yaml.h>
#include <core/Furniture.h>
#include <iostream>
#include <stdexcept>
#include <core/World.h>
#include <game/WorldRuntime.h>
#include <core/Agent.h>
#include "../../headless/src/smoke/support/AgentPoseTestAccess.h"
#include <core/Window.h>
#include "PlayerTransportSelectorDrawing.h"
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/common/Logger.h>
#include <mpp/Logger.h>
#include <mpp/ResourceManager.h>
#include "imgui/imgui.h"
#include "ApplicationResources.h"
#include "DirectoryResourceLocation.h"
#include "Map.h"
#include "MapResources.h"
// The game's own seam to the shared renderer, and the game's own font
// metrics. Everything the checks below assert on is produced by pf-render
// through those two - the game has no renderer of its own to test (#1002).
#include "MppFontMetrics.h"
#include "WorldCanvas.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "SectorTileset.h"
#include "ObjectTileset.h"

namespace {
namespace app = pf::application::render;
using pf::render::FontRef;
using pf::render::WorldFont;
using pf::render::packColour;

void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}
void require(bool condition, std::string const& message) {
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
// The tilesets the shared producer draws with, installed from the same
// source manifest the game loads at run time. The texture handle is a
// placeholder: these checks never touch a GPU, they read the command stream
// the tileset helpers record.
// The texture handle the installed tilesets carry. These checks never touch
// a GPU - they read the command stream the tileset helpers record - so the
// handle is a placeholder that only has to be non-zero.
pf::render::TextureId const kInstalled{1};
// The objects tileset goes in by value, not by move: the caller keeps it as
// the reference copy the Furniture checks read back.
void installTilesets(SectorTileset sectors, ObjectTileset const& objects,
    ObjectTileset agents, ObjectTileset furniture) {
  setSectorTileset(std::move(sectors), kInstalled);
  setObjectTileset(objects, kInstalled);
  setAgentTileset(std::move(agents), kInstalled);
  setFurnitureTileset(std::move(furniture), kInstalled);
}
ObjectTileset installAtlases(std::filesystem::path const& directory) {
  auto manifest = YAML::LoadFile((directory / "Resources.yaml").string());
  auto resource = [&](std::string const& name) {
    for (auto entry : manifest["Resources"]["Resource"])
      if (entry["name"].as<std::string>() == name) return entry;
    throw std::runtime_error("Missing manifest resource: " + name);
  };
  auto sectorsSize = pngSize(directory / resource("SectorAtlasImage")["location"].as<std::string>());
  SectorTileset sectors;
  sectors.width = sectorsSize.first; sectors.height = sectorsSize.second;
  for (auto image : resource("SectorAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    SectorTileRegion region{image["x"].as<int>(), image["y"].as<int>(),
                           image["width"].as<int>(), image["height"].as<int>()};
    if (name.starts_with("boundary-")) sectors.boundaries.emplace(name.substr(9), region);
    else sectors.surfaces.emplace(name, region);
  }
  auto objectsSize = pngSize(directory / resource("ObjectAtlasImage")["location"].as<std::string>());
  // Tintability is an authored fact, never an inferred one. The tileset behind
  // the Objects atlas is the only source: the rule that used to derive it here
  // by matching a tile's name against `agent`, `human-*`, `robot-standing`
  // and `cleaning-bot-standing` is deleted (#1018). A new Agent type can no
  // longer become tintable by accident of what it is called, and a tile named
  // without a hint can no longer fail to be.
  auto const authored = ObjectTileset::load(directory / "textures/objects.tileset.yaml");
  auto tintable = [&](std::string const& name) {
    auto const found = authored.sprites.find(name);
    require(found != authored.sprites.end(),
        "The authored tileset defines no tile the atlas declares");
    return found->second.tintable;
  };
  ObjectTileset objects;
  objects.width = objectsSize.first; objects.height = objectsSize.second;
  for (auto image : resource("ObjectAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    objects.sprites.emplace(name, ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(),
        image["width"].as<int>(), image["height"].as<int>()}, tintable(name)});
  }
  ObjectTileset agents;
  agents.width = objectsSize.first; agents.height = objectsSize.second;
  for (auto image : resource("AgentAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    require(!objects.sprites.contains(name), "Agent tiles must be removed from ObjectAtlas");
    agents.sprites.emplace(name, ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(),
        image["width"].as<int>(), image["height"].as<int>()}, tintable(name)});
  }
  require(agents.sprites.size() == 8, "AgentAtlas must contain all eight extracted tiles");
  ObjectTileset furniture;
  furniture.width = objectsSize.first; furniture.height = objectsSize.second;
  for (auto image : resource("FurnitureAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    auto const& alias = objects.sprites.at(name).region;
    require(alias.x == image["x"].as<int>() && alias.y == image["y"].as<int>()
        && alias.width == image["width"].as<int>() && alias.height == image["height"].as<int>(),
        "Furniture compatibility alias must preserve its rectangle");
    furniture.sprites.emplace(name, ObjectSprite{alias, false});
  }
  require(furniture.sprites.size() == 7, "FurnitureAtlas must include chairs, sofas, desks and beds");
  installTilesets(std::move(sectors), objects, std::move(agents), std::move(furniture));
  return objects;
}
std::size_t textured(WorldDrawList const& draws, WorldDrawList::Texture texture) {
  std::size_t count{};
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == texture) ++count;
  return count;
}
void checkCanvas(WorldDrawList const& draws, float width, float height) {
  for (auto const& command : draws.commands()) {
    std::visit([&](auto const& primitive) {
      if constexpr (requires { primitive.clip; }) {
        require(primitive.clip.minimum.x >= 0 && primitive.clip.minimum.y >= 0 &&
                    primitive.clip.maximum.x <= width && primitive.clip.maximum.y <= height,
                "Game geometry must retain its window clip");
      }
      if constexpr (requires { primitive.positions; }) {
        for (auto const& position : primitive.positions)
          require(std::isfinite(position.x) && std::isfinite(position.y), "Non-finite geometry");
      }
      if constexpr (requires { primitive.texcoords; }) {
        for (auto const& uv : primitive.texcoords)
          require(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1, "Invalid atlas UV");
      }
    }, command);
  }
}

// The command-stream seam, compared as text. Both recordings come from the
// one producer, so a difference means the game's pass changed the viewport,
// added a pass of its own, or dropped one the editor has (#1002).
std::string signature(WorldDrawList const& draws) {
  std::ostringstream out;
  out.setf(std::ios::fixed); out.precision(4);
  for (auto const& command : draws.commands())
    std::visit([&out](auto const& primitive) {
      using Kind = std::decay_t<decltype(primitive)>;
      auto clip = [&out](auto const& value) {
        out << " clip[" << value.clip.minimum.x << "," << value.clip.minimum.y
            << " " << value.clip.maximum.x << "," << value.clip.maximum.y << "]";
      };
      if constexpr (std::is_same_v<Kind, WorldDrawList::Barrier>) out << "barrier\n";
      else if constexpr (std::is_same_v<Kind, WorldDrawList::Line>) {
        out << "line " << primitive.from.x << "," << primitive.from.y << " "
            << primitive.to.x << "," << primitive.to.y << " " << primitive.colour
            << " w" << primitive.thickness;
        clip(primitive); out << "\n";
      }
      else if constexpr (std::is_same_v<Kind, WorldDrawList::Triangle>) {
        out << "tri " << int(primitive.texture) << " " << primitive.colour;
        for (auto const& p : primitive.positions) out << " " << p.x << "," << p.y;
        for (auto const& uv : primitive.texcoords) out << " uv" << uv.x << "," << uv.y;
        clip(primitive); out << "\n";
      }
      else if constexpr (std::is_same_v<Kind, WorldDrawList::Text>) {
        out << "text " << int(primitive.font.font) << "@" << primitive.font.size << " "
            << primitive.position.x << "," << primitive.position.y << " "
            << primitive.colour << " \"" << primitive.value << "\"";
        clip(primitive); out << "\n";
      }
    }, command);
  return out.str();
}
// Records the same World, Layer and viewport through the game's pass and
// through the editor's, and requires the two command streams to be the same
// stream. The game's pass sets the viewport it recorded with, so the editor
// recording that follows lands on exactly the same canvas.
void requireSameCanvasAsEditor(std::shared_ptr<core::World> const& world,
    float width, float height, char const* what) {
  auto const game = app::drawWorld(*world, width, height);
  WorldDrawList editor(WorldDrawList::ClipRectangle{{0, 0}, {width, height}});
  ::renderWorld(world, &editor);
  auto const gameStream = signature(game);
  auto const editorStream = signature(editor);
  if (gameStream == editorStream) return;
  std::ostringstream out;
  out << "The game canvas is not the editor's World canvas for " << what
     << ": " << game.commands().size() << " commands against "
     << editor.commands().size() << ".";
  throw std::runtime_error(out.str());
}

struct Body { float width{}, height{}; };
Body agentBodyExtent(WorldDrawList const& draws) {
  float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
  bool found = false;
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == WorldDrawList::Texture::AgentAtlas) {
      found = true;
      for (auto const& p : triangle->positions) {
        left = std::min(left, p.x); right = std::max(right, p.x);
        top = std::min(top, p.y); bottom = std::max(bottom, p.y);
      }
    }
  require(found, "Agent pose artwork must reach the game canvas");
  return Body{right - left, bottom - top};
}

void checkFurnitureCatalogueReload(std::filesystem::path const& directory) {
  using namespace wp::application::resourcesystem;
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      core::FurnitureCatalogue::setResourceLoader({});
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{std::filesystem::temp_directory_path() / ("pf-furniture-reload-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directories(cleanup.path);
  auto source = cleanup.path / "chair.furniture.lua";
  std::filesystem::copy_file(directory / "test-worlds/chair.furniture.lua", source);
  std::ifstream input(source);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  input.close();
  wp::Logger logger;
  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  core::FurnitureCatalogue::setResourceLoader([&manager](std::filesystem::path const& path) {
    return FurnitureCatalogueResource::createSnapshot(manager, path)->catalogue();
  });
  // Use the editor's resource-backed loader and World's real reload transaction.
  auto world = std::make_shared<core::World>("Catalogue reload", 4, 2);
  world->pauseSimulation();
  world->attachFurnitureCatalogue("ChairCatalogue", core::FurnitureCatalogue::load(source));
  auto original = world->furnitureCatalogue();
  auto count = manager.getResourcesByType("FurnitureCatalogue").size();
  auto label = text.find("label = \"Chair\"");
  require(label != std::string::npos, "Missing fixture label");
  text.replace(label, std::string("label = \"Chair\"").size(), "label = \"Reloaded chair\"");
  std::ofstream(source) << text;
  std::string diagnostic;
  if (!world->reloadFurnitureCatalogue(source, &diagnostic)) throw std::runtime_error(diagnostic);
  require(manager.getResourcesByType("FurnitureCatalogue").size() == count,
      "Reload added another UserFurniture entry to the catalogue selector");
  require(world->furnitureCatalogueResourceName() == "ChairCatalogue"
      && world->furnitureCatalogue() != original
      && world->furnitureCatalogue()->definition("chair")->label == "Reloaded chair"
      && original->definition("chair")->label == "Chair",
      "Reload must preserve selection and old snapshots while reading fresh data");
  auto accepted = world->furnitureCatalogue();
  std::ofstream(source) << "invalid Lua!";
  require(!world->reloadFurnitureCatalogue(source, &diagnostic) && !diagnostic.empty(),
      "Malformed catalogue reload must fail");
  require(world->furnitureCatalogue() == accepted
      && manager.getResourcesByType("FurnitureCatalogue").size() == count,
      "Failed reload must preserve the accepted snapshot without registering a catalogue");
  std::ofstream(source) << text;
  if (!world->reloadFurnitureCatalogue(source, &diagnostic)) throw std::runtime_error(diagnostic);
  require(manager.getResourcesByType("FurnitureCatalogue").size() == count,
      "Repeated reload must not register synthetic catalogues");
}
void checkAtlasReload(std::filesystem::path const& directory) {
  using namespace wp::application::resourcesystem;
  wp::Logger logger;
  mpp::Logger renderLogger;
  mpp::ResourceManager renderResources(nullptr, &renderLogger);
  ResourceManager manager(nullptr, &renderResources, nullptr, &logger);
  registerApplicationResourceTypes(manager);
  manager.addResourceLocationFactory("Directory",
      [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new DirectoryResourceLocation(&logger, path, definition);
      });
  manager.addResourceLocation("Directory", directory.string(), "Resources.yaml");
  manager.scanLocations();
  // Match editor startup and switching SectorAtlas -> StationAtlas -> SectorAtlas.
  // ImageResource declares its real MPP texture here; GPU upload is not needed.
  std::map<std::string, mpp::ResourcePtr> declarations;
  for (auto const* name : {"SectorAtlas", "StationAtlas", "SectorAtlas", "SectorAtlas",
           "AgentAtlas", "FurnitureAtlas", "ObjectAtlas", "AgentAtlas",
           "StationFurnitureAtlas", "DoctorFemale1Atlas", "FurnitureAtlas"}) {
    auto resource = manager.acquireResource(name);
    try {
      manager.createResource(resource);
      manager.loadResource(resource);
    } catch (std::exception const& error) {
      manager.releaseResource(resource);
      throw std::runtime_error(std::string("Sector ImageSet unavailable: ") + error.what());
    }
    auto set = std::dynamic_pointer_cast<ImageSetResource>(resource);
    auto image = std::dynamic_pointer_cast<ImageResource>(set->getImage());
    auto texture = image->getMppResource();
    auto [entry, inserted] = declarations.emplace(image->getQualifiedName(), texture);
    require(inserted || entry->second == texture,
        "Atlas reload must reuse its registered texture, including shared ImageSets");
    auto stream = std::dynamic_pointer_cast<mpp::TextureStream>(texture->getResourceStream());
    stream->load(); // Exercise pixel refresh without GPU upload.
    require(stream->getWidth() == static_cast<unsigned>(image->getWidth())
        && stream->getHeight() == static_cast<unsigned>(image->getHeight())
        && std::equal(image->getData(), image->getData() + image->getSize(), stream->getData()),
        "Reloaded texture must read the Image's current decoded pixels");
    manager.releaseResource(resource);
    require(renderResources.getResource(image->getQualifiedName()) == texture,
        "Image release must retain the reusable backend declaration");
    if (!manager.isResourceLoaded(image)) {
      require(!texture->isReferenced(), "Unloaded image leaked its MPP acquisition");
      require(!stream->getData(), "Unreferenced image retained stale copied pixels");
      manager.destroyResources({resource, image});
    }
  }
}
// The game's canvas selection path (#1026). The World owns which atlas
// fills each of the four viewport slots; the game owns handing all four of
// them over every frame. Until this ticket it handed over three, so the
// renderer kept its constructed `ObjectAtlas` default and a World asking
// for `ObjectAtlasNew` drew the old sheet everywhere but in the editor.
//
// The renderer's install path needs a live OpenGL context, which no
// headless check has, so what is asserted here is the game's half of the
// seam: the one table the render pass walks, row by row - that all four
// slots are in it, that each row reads its own World field, and that each
// row is bound to the renderer's own entry for that slot rather than to
// another slot's. The Objects row being bound to `selectWorldObjectImageSet`
// is what carries the Objects contract (#1022) into the shipped game, and
// the render smoke module already pins what that entry refuses.
void checkCanvasAtlasSelection() {
  auto const& slots = app::worldAtlasSlots();
  std::array<char const*, 4> const expectedSlots{"Sector", "Objects", "Agent",
      "Furniture"};
  require(slots.size() == expectedSlots.size(),
      "The game selects " + std::to_string(slots.size())
      + " viewport atlases; a World makes four selections");
  for (std::size_t index = 0; index < slots.size(); ++index)
    require(expectedSlots[index] == std::string(slots[index].slot),
        "The game's atlas slot " + std::to_string(index) + " is '"
        + std::string(slots[index].slot) + "', expected '" + expectedSlots[index] + "'");

  // Each row installs through its own renderer entry. A row wired to
  // another slot's entry would push the right name into the wrong slot -
  // and for Objects would skip the contract that keeps a Door's artwork
  // inside the collision that stops the Agent.
  require(slots[0].install == &selectWorldSectorImageSet,
      "The Sector slot is not installed through selectWorldSectorImageSet");
  require(slots[1].install == &selectWorldObjectImageSet,
      "The Objects slot is not installed through selectWorldObjectImageSet, so a "
      "World's Objects atlas selection never reaches the shipped game");
  require(slots[2].install == &selectWorldAgentImageSet,
      "The Agent slot is not installed through selectWorldAgentImageSet");
  require(slots[3].install == &selectWorldFurnitureImageSet,
      "The Furniture slot is not installed through selectWorldFurnitureImageSet");

  auto world = std::make_shared<core::World>("Atlas selection", 8, 2);
  world->addRoom("Front", 0, 0, 0, 8, 1);
  world->addRoom("Back", 1, 0, 0, 8, 1);
  world->finishBuild();
  world->pauseSimulation();
  std::array<std::string, 4> const selected{world->sectorImageSetResourceName(),
      world->objectImageSetResourceName(), world->agentImageSetResourceName(),
      world->furnitureImageSetResourceName()};
  for (std::size_t index = 0; index < slots.size(); ++index)
    require(slots[index].selected(*world) == selected[index],
        "The game's '" + std::string(expectedSlots[index])
        + "' slot does not read the World's own selection for that slot");

  // A fresh World selects the new family, and the game pushes the Objects
  // name with the other three rather than leaving the slot at the
  // renderer's default - which is what makes #1023 land in the game and
  // not only in the editor.
  auto const pushed = app::applyWorldAtlasSelections(*world);
  require(pushed.size() == slots.size(),
      "The game pushed " + std::to_string(pushed.size())
      + " atlas selections against a four-slot table");
  require(std::array<std::string, 4>{pushed[0].name, pushed[1].name,
      pushed[2].name, pushed[3].name} == selected,
      "The game did not push the World's four atlas selections");
  require(pushed[1].name == "ObjectAtlasNew",
      "A fresh World's Objects selection is not what the game pushes: "
      + pushed[1].name);
  require(pushed[1].slot == std::string("Objects"),
      "The Objects name was pushed through a slot named '" + pushed[1].slot + "'");

  // Repointing the World's Objects atlas changes what the game pushes on
  // the very next pass. The game holds no copy of the choice of its own,
  // so there is nothing that can go stale against a live artwork change.
  world->setObjectImageSetResourceName("ObjectAtlas");
  auto const repointed = app::applyWorldAtlasSelections(*world);
  require(repointed[1].name == "ObjectAtlas",
      "Repointing the World's Objects atlas did not change what the game pushes: "
      + repointed[1].name);
  require(repointed[0].name == selected[0] && repointed[2].name == selected[2]
      && repointed[3].name == selected[3],
      "Repointing the Objects atlas moved another slot's selection with it");

  // A refused atlas reaches the game's report with the renderer's reason
  // in it, which is the whole of the difference between a World that says
  // its Objects set cannot carry its geometry and one that quietly draws a
  // Door outside its collision. An accepted set reports nothing, so the
  // report is a signal rather than noise.
  std::vector<app::WorldAtlasSelection> refused{
      {"Sector", "SectorAtlasNew", ""},
      {"Objects", "BrokenAtlas",
          "Objects atlas 'BrokenAtlas' cannot be used by this World: 1 tile(s) do "
          "not match what the object renderer draws\n  - tile 'lift-car' is not in "
          "the atlas"},
      {"Agent", "AgentAtlasNew", ""},
      {"Furniture", "FurnitureAtlasNew", ""}};
  auto const report = app::worldAtlasReport(refused);
  require(report.find("BrokenAtlas") != std::string::npos
      && report.find("lift-car") != std::string::npos,
      "A refused Objects atlas does not reach the game's report with its reason: "
      + report);
  refused[1].diagnostic.clear();
  require(app::worldAtlasReport(refused).empty(),
      "An atlas selection every slot accepted still produced a report");
}

// The game's report for a refused atlas, driven through the same
// `wp::Logger` the game's own state hands the canvas (#1026). The game
// has no artwork panel to show a refusal in, so the log is the whole of
// what a player or a support pass gets: it has to carry the renderer's
// reason, and it must not carry it sixty times a second.
void checkCanvasAtlasReport() {
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{std::filesystem::temp_directory_path() / ("pf-canvas-atlas-report-"
      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directories(cleanup.path);

  auto refused = std::vector<app::WorldAtlasSelection>{
      {"Sector", "SectorAtlasNew", ""},
      {"Objects", "ObjectAtlasBroken",
          "Objects atlas 'ObjectAtlasBroken' cannot be used by this World: 1 "
          "tile(s) do not match what the object renderer draws"
          "\n  - tile 'lift-car' is not in the atlas"},
      {"Agent", "AgentAtlasNew", ""},
      {"Furniture", "FurnitureAtlasNew", ""}};
  auto accepted = refused;
  accepted[1].diagnostic.clear();
  auto changed = refused;
  changed[1].name = "ObjectAtlasAlsoBroken";
  changed[1].diagnostic = "Objects atlas 'ObjectAtlasAlsoBroken' is not registered";

  // One pass through a logger of its own, read back once the logger has
  // closed and flushed: the answer is whether the pass wrote, and what the
  // game's log actually says.
  auto pass = [&](std::string const& tag,
      std::vector<app::WorldAtlasSelection> const& selections,
      std::string& lastReported) {
    auto const path = cleanup.path / ("report-" + tag + ".log");
    bool wrote{false};
    {
      wp::Logger logger;
      require(logger.open(path.string()),
          "Could not open the game's log at " + path.string());
      wrote = app::reportWorldAtlasSelections(logger, selections, lastReported);
    }
    std::ifstream file(path);
    require(bool(file), "Could not read back the game's log at " + path.string());
    std::stringstream body;
    body << file.rdbuf();
    return std::pair<bool, std::string>{wrote, body.str()};
  };

  std::string lastReported;
  auto const first = pass("refused", refused, lastReported);
  require(first.first, "A refused Objects atlas wrote nothing to the game's log");
  require(first.second.find("ObjectAtlasBroken") != std::string::npos
      && first.second.find("lift-car") != std::string::npos,
      "The game's log does not carry the refusal's reason: " + first.second);

  // The selection runs every frame. A refusal that repeats writes once,
  // not once per frame.
  auto const again = pass("repeat", refused, lastReported);
  require(!again.first && again.second.find("ObjectAtlasBroken") == std::string::npos,
      "The same refusal was reported again; the selection runs every frame and "
      "the log would fill at frame rate");

  auto const next = pass("changed", changed, lastReported);
  require(next.first && next.second.find("ObjectAtlasAlsoBroken") != std::string::npos,
      "A different refusal did not reach the game's log");

  auto const recovered = pass("recovered", accepted, lastReported);
  require(!recovered.first && recovered.second.find("Objects atlas") == std::string::npos,
      "A selection every slot accepted still wrote to the game's log");

  // Recovery clears what was remembered, so a refusal that comes back is
  // heard about again. `changed` is what was last reported, so repeating
  // it after a clean pass is the case a remembered refusal would
  // silence - and the reason the report forgets rather than keeps.
  auto const regression = pass("regression", changed, lastReported);
  require(regression.first
      && regression.second.find("ObjectAtlasAlsoBroken") != std::string::npos,
      "After a clean pass the refusal that came back stayed silent: the report "
      "remembered a refusal the game had already recovered from");
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
  // The game's pass is handed the runtime's const view exactly as StatePlay
  // hands it (#970). Nothing in this check holds the World, so nothing here
  // can keep it alive past the resource that owns it.
  auto const& stagedWorld = map->getRuntime()->world();
  auto const tick = stagedWorld.getSimulationTick();
  for (auto const& size : {std::pair{1280.0f, 720.0f}, std::pair{320.0f, 200.0f}}) {
    auto draws = app::drawWorld(stagedWorld, size.first, size.second);
    require(textured(draws, WorldDrawList::Texture::SectorAtlas) > 0,
            "Loaded game Map must draw its sector atlas artwork");
    checkCanvas(draws, size.first, size.second);
  }
  require(stagedWorld.getSimulationTick() == tick, "Rendering must leave World simulation paused");
  resources.unload();
  require(!map->getRuntime(), "Drawing must not retain the World after MapUnload");
}

// The text the game could never draw before: a Sector's capacity readout and
// a Marker's identification. Both are World text the shared producer emits
// and the game's injected MPP-backed metrics make measurable (#1002).
void checkWorldText(std::filesystem::path const& directory) {
  auto world = std::make_shared<core::World>("Game text", 12, 2);
  auto room = world->addRoom("Front", 0, 0, 0, 3, 1);
  world->addCorridor(0, 0, 4, 3, 1);
  world->addRoom("Back", 1, 0, 0, 12, 1);
  world->addChamber(0, 0, 3, 1, true);
  world->addSectorMarker(room, 0, 1.5f, "Meeting point");
  world->finishBuild(); world->pauseSimulation();

  auto const withAtlas = app::drawWorld(*world, 1280, 720);
  bool capacity = false;
  bool markerArtwork = false;
  for (auto const& command : withAtlas.commands()) {
    if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
      capacity = capacity || text->value.find("Capacity:") != std::string::npos;
    if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
      markerArtwork = markerArtwork
          || triangle->texture == WorldDrawList::Texture::ObjectAtlas;
  }
  require(capacity, "The game canvas must carry the Sector capacity readout");
  require(markerArtwork, "The game canvas must carry the Marker's artwork");
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "a Sector readout and a Marker");

  // With no object artwork to draw, the Marker falls back to the icon glyph
  // as a text run - the path that needs the game's own font metrics, and
  // the one that used to be unreachable in the shipped game.
  clearObjectTileset();
  auto const withoutArtwork = app::drawWorld(*world, 1280, 720);
  bool markerGlyph = false;
  for (auto const& command : withoutArtwork.commands())
    if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
      markerGlyph = markerGlyph || text->font.font == WorldFont::Icons;
  require(markerGlyph,
      "A Marker with no artwork must fall back to the icon face through the "
      "game's injected font metrics");
  installAtlases(directory);
}

// The pose the game could never draw: an Agent Lying on a Bed is drawn
// rotated, an Agent Sitting is drawn squashed, and both are drawn exactly
// as the editor draws them (#1002).
void checkAgentPoses() {
  auto world = std::make_shared<core::World>("Game poses", 12, 2);
  world->addRoom("Front", 0, 0, 0, 12, 1);
  world->addRoom("Back", 1, 0, 0, 12, 1);
  world->finishBuild(); world->pauseSimulation();
  auto id = world->createAgent("Actor", 0, 0, 3.5f);
  auto* agent = world->lookupAgent(id).entity;
  require(agent != nullptr, "Agent fixture creation failed");
  auto const near = [](float a, float b) { return std::abs(a - b) < 0.01f; };

  core::AgentPoseTestAccess::set(*agent, core::Pose::Standing);
  auto const standing = agentBodyExtent(app::drawWorld(*world, 1280, 720));
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "a Standing Agent");

  core::AgentPoseTestAccess::set(*agent, core::Pose::Sitting);
  auto const sitting = agentBodyExtent(app::drawWorld(*world, 1280, 720));
  require(near(sitting.width, standing.width) && sitting.height < standing.height,
      "A Sitting Agent must be drawn squashed against its Standing body, not standing");
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "a Sitting Agent");

  core::AgentPoseTestAccess::set(*agent, core::Pose::Lying);
  auto const lying = agentBodyExtent(app::drawWorld(*world, 1280, 720));
  require(lying.width > lying.height && near(lying.width, standing.height)
      && near(lying.height, standing.width),
      "A Lying Agent must be drawn rotated onto its side, not standing upright");
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "a Lying Agent");
}

void checkAgentsAndMarkers() {
  auto world = std::make_shared<core::World>("Game agents", 12, 2);
  auto room = world->addRoom("Front", 0, 0, 0, 12, 1);
  auto back = world->addRoom("Back", 1, 0, 0, 12, 1);
  world->finishBuild(); world->pauseSimulation();
  auto baseline = app::drawWorld(*world, 1280, 720);
  world->addSectorMarker(room, 0, 8.5f, "Authored destination");
  world->finishBuild(); world->pauseSimulation();
  auto marked = app::drawWorld(*world, 1280, 720);
  require(marked.commands().size() > baseline.commands().size(),
          "An authored Marker must contribute its artwork to the game canvas");
  auto id = world->createAgent("Visible", room, 0, 1.5f);
  auto agent = world->lookupAgent(id).entity;
  require(agent != nullptr, "Agent fixture creation failed");
  auto visible = app::drawWorld(*world, 1280, 720);
  auto const sprites = textured(visible, WorldDrawList::Texture::AgentAtlas);
  require(sprites >= 2, "Front-Layer Agent must draw pose artwork through the shared renderer");
  world->createAgent("Occluded", back, 0, 1.5f);
  auto occluded = app::drawWorld(*world, 1280, 720);
  require(textured(occluded, WorldDrawList::Texture::AgentAtlas) == sprites,
          "Agent on a Layer without an aperture must remain hidden");
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "hidden back-Layer content");
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
  auto centred = [](WorldDrawList const& draws, float width) {
    float low = width, high = 0;
    bool found = false;
    for (auto const& command : draws.commands())
      if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == WorldDrawList::Texture::AgentAtlas) {
        found = true;
        for (auto position : triangle->positions) {
          low = std::min(low, position.x); high = std::max(high, position.x);
        }
      }
    require(found && std::abs((low + high) * 0.5f - width * 0.5f) < 0.001f,
            "Player artwork must be visible on its Layer and centred horizontally");
  };
  auto draws = app::drawWorld(*world, 640, 360);
  centred(draws, 640); checkCanvas(draws, 640, 360);
  require(world->resumeSimulation(), "Camera fixture cannot resume");
  world->setPlayerMovementDirection(1); world->update(1.f);
  auto const tick = world->getSimulationTick();
  draws = app::drawWorld(*world, 320, 200);
  centred(draws, 320); checkCanvas(draws, 320, 200);
  require(world->getSimulationTick() == tick, "Following must not update simulation");
  world->pauseSimulation(); world->clearAgentBehaviourAssignment(id);
  require(textured(app::drawWorld(*world, 640, 360), WorldDrawList::Texture::AgentAtlas) == 0,
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
  auto outlines = [](WorldDrawList const& draws) {
    unsigned count = 0;
    auto const& commands = draws.commands();
    for (size_t i = 0; i + 3 < commands.size(); ++i) {
      auto horizontal = std::get_if<WorldDrawList::Line>(&commands[i]);
      auto vertical = std::get_if<WorldDrawList::Line>(&commands[i + 1]);
      if (!horizontal || !vertical || horizontal->from.y != horizontal->to.y || vertical->from.x != vertical->to.x) continue;
      float width = std::abs(horizontal->to.x - horizontal->from.x);
      float height = std::abs(vertical->to.y - vertical->from.y);
      if (height == 0 || std::abs(width / height - CORE_STAIRWELL_PASSAGE_WIDTH * CORE_CELL_WIDTH_PIXELS
          / (CORE_STAIRWELL_DOORWAY_HEIGHT * CORE_LEVEL_HEIGHT_PIXELS)) > .001f) continue;
      bool rectangle = true;
      for (size_t side = 0; side < 4; ++side) {
        auto line = std::get_if<WorldDrawList::Line>(&commands[i + side]);
        auto next = std::get_if<WorldDrawList::Line>(&commands[i + (side + 1) % 4]);
        rectangle = rectangle && line && next && line->colour == packColour(0, 0, 0) && line->thickness == 1.f
          && line->to.x == next->from.x && line->to.y == next->from.y;
      }
      if (rectangle) {
        // Emitted geometry outside its aperture clip is not a visible doorway.
        for (size_t side = 0; side < 4; ++side) {
          auto const& line = std::get<WorldDrawList::Line>(commands[i + side]);
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
  // Two Levels of Stairwell doorway, seen from a Level-0 landing that the
  // player camera centres on. At the current cell size that needs more than
  // a 720-pixel canvas to keep both doorway boxes on screen.
  float const viewWidth = 1280.0f;
  float const viewHeight = 3.0f * CORE_LEVEL_HEIGHT_PIXELS;
  auto landingCount = outlines(app::drawWorld(*world, viewWidth, viewHeight));
  if (landingCount != 2) throw std::runtime_error(
          "Open Stairwell doorways must have visible boxes when viewed through landing apertures: expected 2, got "
          + std::to_string(landingCount));
  world->resumeSimulation(); world->requestPlayerDoorAction(); world->update(.2f);
  require(world->lookupAgent(id).entity->getSector()->getIndex() == stairs, "Player must enter Stairwell to view its Layer");
  auto draws = app::drawWorld(*world, viewWidth, viewHeight);
  auto count = outlines(draws);
  if (count != 2) throw std::runtime_error("Viewed Stairwell must outline every Level doorway: expected 2, got " + std::to_string(count));
  checkCanvas(draws, viewWidth, viewHeight);
}

void checkFurniture(std::filesystem::path const& directory, ObjectTileset const& atlas) {
  auto world = std::make_shared<core::World>("Game Furniture", 8, 2);
  auto room = world->addRoom("Front", 0, 0, 0, 8, 1);
  auto back = world->addRoom("Back", 1, 0, 0, 8, 1);
  world->attachFurnitureCatalogue("chair.furniture.lua",
      core::FurnitureCatalogue::load(directory / "test-worlds/chair.furniture.lua"));
  world->placeFurniture(room, "chair", 2.25f, 0, "Visible chair");
  world->placeFurniture(back, "chair", 2.25f, 0, "Hidden chair");
  world->finishBuild(); world->pauseSimulation();
  auto draws = app::drawWorld(*world, 1280, 720);
  require(textured(draws, WorldDrawList::Texture::FurnitureAtlas) == 4,
          "Only the front chair should draw, including foreground restoration");
  auto const& chair = atlas.sprites.at("chair").region;
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == WorldDrawList::Texture::FurnitureAtlas)
      for (auto uv : triangle->texcoords)
        require(uv.x >= float(chair.x) / atlas.width && uv.x <= float(chair.x + chair.width) / atlas.width &&
                    uv.y >= float(chair.y) / atlas.height && uv.y <= float(chair.y + chair.height) / atlas.height,
                "Furniture usable-point Markers must not draw over its artwork");
  checkCanvas(draws, 1280, 720);
  auto graph = world->getGraph();
  auto markers = world->getMarkerIds();
  auto assertFallback = [&] {
    auto missing = app::drawWorld(*world, 1280, 720);
    require(textured(missing, WorldDrawList::Texture::FurnitureAtlas) == 0,
        "Missing Furniture artwork must not fall back to ObjectAtlas");
    std::size_t magenta{};
    for (auto const& command : missing.commands())
      if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == WorldDrawList::Texture::None
          && triangle->colour == packColour(255, 0, 255, 255)) ++magenta;
    require(magenta == 4, "Missing Furniture tiles must render cell-sized magenta rectangles");
    require(world->getGraph() == graph && world->getMarkerIds() == markers,
        "Artwork selection changed physical Furniture layout");
  };
  clearFurnitureTileset();
  assertFallback();
  // Artwork pixels of an alternate atlas this check invents: 600 x 256, with the
  // chair tile at 8,63 sized 71 x 193. None of it is World geometry.
  ObjectTileset custom;
  custom.width = 600; custom.height = 256;
  custom.sprites.emplace("other", ObjectSprite{{0, 0, 11, 13}, false});
  setFurnitureTileset(custom, kInstalled);
  assertFallback();
  custom.sprites.emplace("chair", ObjectSprite{{8, 63, 71, 193}, false});
  setFurnitureTileset(custom, kInstalled);
  auto changed = app::drawWorld(*world, 1280, 720);
  require(textured(changed, WorldDrawList::Texture::FurnitureAtlas) == 4,
      "Alternate Furniture tile proportions must remain usable");
  for (auto const& command : changed.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == WorldDrawList::Texture::FurnitureAtlas)
      for (auto uv : triangle->texcoords)
        require(uv.x >= 8.f / 600 && uv.x <= 79.f / 600 && uv.y >= 63.f / 256 && uv.y <= 256.f / 256,
            "Selected Furniture atlas UVs were not applied");
  installAtlases(directory);
}
void checkNestedApertures() {
  auto world = std::make_shared<core::World>("Game apertures", 6, 2);
  while (world->getLayerCount() < 3) world->addLayer();
  world->addRoom("Front", 0, 0, 0, 6, 1);
  world->addRoom("Middle", 1, 0, 0, 6, 1);
  world->addBackground(2, 0, 0, 6, 1, {23, 67, 109});
  world->finishBuild(); world->pauseSimulation();
  auto hidden = app::drawWorld(*world, 1280, 720);
  auto hasBackColour = [](WorldDrawList const& draws) {
    for (auto const& command : draws.commands())
      if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
          triangle && triangle->colour == packColour(23, 67, 109, 255)) return true;
    return false;
  };
  require(!hasBackColour(hidden), "Deeper Background must not leak through an opaque Room");
  world->addSectorWindow(0, 0, 1, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->addSectorWindow(1, 0, 2, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->finishBuild(); world->pauseSimulation();
  auto revealed = app::drawWorld(*world, 1280, 720);
  require(hasBackColour(revealed), "Aligned clear Windows must reveal the deeper Background");
  for (auto const& command : revealed.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->colour == packColour(23, 67, 109, 255))
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
  // Artwork pixels, not World units: this invented atlas is 512 x 320 with a
  // 128 x 320 corridor tile. The producer tiles by World cell regardless of
  // the source grid, so the atlas numbers say nothing about cell size - but
  // the probed area below is deliberately derived from the World cell.
  SectorTileset tiles;
  tiles.width = 512; tiles.height = 320;
  tiles.surfaces.emplace("corridor", SectorTileRegion{0, 0, 128, 320});
  setSectorTileset(tiles, kInstalled);
  WorldDrawList commands({{0, 0}, {1000, 1000}});
  float const zoom = 2.f;
  // Two World cells wide, three quarters of a Level tall: two tiles side by
  // side in one row, that row cropped at three quarters of its tile height
  // rather than stretched to fit. A tile is a whole Level tall on screen, so
  // this probe never reaches a second Level; the crop is the point - the UV
  // stops at the same 0.75 fraction as the geometry instead of running the
  // whole tile.
  float const probeWide = 2.0f * CORE_CELL_WIDTH_PIXELS * zoom;
  float const probeHigh = 0.75f * CORE_LEVEL_HEIGHT_PIXELS * zoom;
  require(drawSectorTileSurface("corridor", &commands, {0, 0}, {probeWide, probeHigh}, pf::render::White, zoom),
      "Station corridor must render");
  require(textured(commands, WorldDrawList::Texture::SectorAtlas) == 4,
      "Two World cells must each render a tile at zoom 2 independent of source size");
  auto const& triangle = std::get<WorldDrawList::Triangle>(commands.commands().front());
  float maxX{}, maxY{}, maxUvY{};
  for (int i = 0; i < 3; ++i) {
    maxX = std::max(maxX, triangle.positions[i].x);
    maxY = std::max(maxY, triangle.positions[i].y);
    maxUvY = std::max(maxUvY, triangle.texcoords[i].y);
  }
  require(maxX == CORE_CELL_WIDTH_PIXELS * zoom && maxY == probeHigh
      && maxUvY < .76f && maxUvY > .74f,
      "Tile must fill 1x1 World units and crop a partial-height cell");
  WorldDrawList missing({{0, 0}, {1000, 1000}});
  require(drawSectorTileSurface("room", &missing, {0, 0},
      {CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS}, pf::render::White),
      "Missing tile must emit fallback geometry");
  auto const& fallback = std::get<WorldDrawList::Triangle>(missing.commands().front());
  require(fallback.texture == WorldDrawList::Texture::None
      && fallback.colour == packColour(255, 0, 255), "Missing tile must be opaque magenta");
  // One World cell wide; the 2 is a plain line's pixel height, not a World unit.
  require(!drawSectorTileBoundary("floor", &missing, {0, 0}, {CORE_CELL_WIDTH_PIXELS, 2}),
      "Missing boundary must request plain-line fallback");
  setSectorTileset({}, kInstalled);
  WorldDrawList unavailable({{0, 0}, {1000, 1000}});
  require(drawSectorTileSurface("corridor", &unavailable, {0, 0},
      {CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS}, pf::render::White),
      "Unavailable ImageSet must emit fallback geometry");
  clearSectorTileset();
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
    // Artwork pixels of a stand-in Agent atlas; its tile grid is unrelated to
    // the World cell, which is why the fallback below is sized in World units.
    ObjectTileset tiles; tiles.width = 128; tiles.height = 128;
    if (present & 1) tiles.sprites.emplace("human-standing", ObjectSprite{{0, 0, 26, 72}, true});
    if (present & 2) tiles.sprites.emplace("human-sitting", ObjectSprite{{32, 0, 26, 43}, true});
    setAgentTileset(std::move(tiles), kInstalled);
    // The game camera fits the World into the canvas and only stays at zoom 1
    // when the canvas has room; the fallback below compares against unscaled
    // World-unit bounds, so give it a canvas that does not shrink the World.
    auto list = app::drawWorld(*world, 1280, 800);
    unsigned missing = 0;
    float left = 1e6f, right = -1e6f, top = 1e6f, bottom = -1e6f;
    for (auto const& command : list.commands())
      if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == WorldDrawList::Texture::None
          && triangle->colour == packColour(255, 0, 255, 255)) {
        ++missing;
        for (auto p : triangle->positions) {
          left = std::min(left, p.x); right = std::max(right, p.x);
          top = std::min(top, p.y); bottom = std::max(bottom, p.y);
        }
      }
    if (present == 3) require(missing == 0 && textured(list, WorldDrawList::Texture::AgentAtlas) == 4,
        "Complete Agent artwork must use its independent texture channel");
    else require(missing == 4 && near(right - left, agent->getWidth() * CORE_CELL_WIDTH_PIXELS)
        && near(bottom - top, agent->getHeight() * CORE_LEVEL_HEIGHT_PIXELS),
        "Absent Agent atlas, pose or Standing tile must draw magenta physical bounds");
    auto const after = agent->getBounds().getSize();
    require(near(after.x, physical.x) && near(after.y, physical.y), "Agent artwork changed simulation geometry");
  }
}

// The game's own text system, measured through the interface the shared
// renderer asks questions with. MPP lays a run out one fixed cell per byte
// on a 16 pixel line; the game's metrics have to answer with what MPP will
// actually draw, or every centred label in the game drifts.
void checkMppFontMetrics() {
  app::MppFontMetrics metrics;
  auto const native = metrics.measure({WorldFont::Default, 0.0f}, "Ab");
  require(native.y == app::MppFontMetrics::NativeLineHeight,
      "The MPP face must measure at its own 16 pixel line height");
  require(native.x == 2.0f * app::MppFontMetrics::advanceFor('A'),
      "A run must measure as the sum of its glyph advances");
  auto const scaled = metrics.measure({WorldFont::Default, 32.0f}, "Ab");
  require(std::abs(scaled.x - native.x * 2.0f) < 0.001f
      && std::abs(scaled.y - native.y * 2.0f) < 0.001f,
      "A sized ref must scale the whole run");
  require(metrics.measure({WorldFont::Default, 0.0f}, nullptr).x == 0.0f,
      "A null run must measure as nothing rather than throw");
  require(metrics.tessellate({WorldFont::Default, 0.0f}, "Ab").empty(),
      "An unsized run stays on MPP's native text path and tessellates nothing");
  auto const glyphs = metrics.tessellate({WorldFont::Icons, 16.0f}, "AB");
  require(glyphs.size() == 12, "Two glyphs must tessellate to two quads");
  for (auto const& vertex : glyphs) {
    require(vertex.position.x >= 0.0f && vertex.position.y >= 0.0f
        && vertex.position.y <= app::MppFontMetrics::NativeLineHeight,
        "Tessellated glyph geometry must sit on the text baseline box");
    require(vertex.texcoord.x >= 0.0f && vertex.texcoord.x <= 1.0f
        && vertex.texcoord.y >= 0.0f && vertex.texcoord.y <= 1.0f,
        "Tessellated glyph UVs must stay inside the atlas");
  }
  auto const atlas = metrics.atlas();
  require(!atlas.empty() && atlas.width == atlas.height,
      "The game must hand the renderer the MPP font atlas to draw transformed text with");
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the repository resource directory");
    auto directory = std::filesystem::path(argv[1]);
    // The game's own font metrics, installed the way the game installs them.
    // Every text measurement below runs through MPP, not through a font the
    // renderer reached for itself (#999, #1002).
    app::MppFontMetrics fontMetrics;
    pf::render::installFontMetrics(&fontMetrics);
    checkMppFontMetrics();
    // The same source manifest supplies the CPU-only artwork rectangles; no
    // graphics context, texture upload, auxiliary manifest or ImGui is needed.
    checkSectorTileScaling();
    auto atlas = installAtlases(directory);
    checkFurnitureCatalogueReload(directory);
    checkAtlasReload(directory);
    checkStagedMap(directory);
    checkCanvasAtlasSelection();
    checkCanvasAtlasReport();
    checkWorldText(directory);
    checkAgentPoses();
    checkAgentsAndMarkers();
    checkPlayerCamera();
    checkPlayerSelectorDrawing();
    checkStairwellDoorwayOutlines();
    checkFurniture(directory, atlas);
    checkNestedApertures();
    checkAgentArtworkFallback();
    // There is no "no World" case to check any more: the game's pass takes
    // the `core::World const&` its WorldRuntime published, so a caller
    // cannot even express rendering nothing but a null World (#970).
    auto empty = std::make_shared<core::World>("Empty", 4, 2);
    require(app::drawWorld(*empty, 1280, 720).commands().empty(), "Empty World must draw nothing");
    require(app::drawWorld(*empty, 0, 0).commands().empty(), "Minimized window must draw nothing");
    clearObjectTileset(); clearSectorTileset(); clearAgentTileset(); clearFurnitureTileset();
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
