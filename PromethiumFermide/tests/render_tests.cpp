#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <yaml-cpp/yaml.h>
#include <core/Furniture.h>
#include <iostream>
#include <stdexcept>
#include <core/World.h>
#include <game/WorldRuntime.h>
#include <game/WorldAuthoring.h>
#include <core/Agent.h>
#include "../../headless/src/smoke/support/AgentPoseTestAccess.h"
#include "../../headless/src/support/SectorDrawSheet.h"
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
#include "WorldViewportZoom.h"
#include "AtlasInstall.h"
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
// The installed Sector sheet and the `background` tile cut from it, kept
// so a check can say which tile a quad sampled. Nothing identifies a
// Background by colour any more - it is artwork like every other surface -
// so the tile is what a check reads.
int gSectorAtlasWidth{0};
int gSectorAtlasHeight{0};
SectorTileRegion gBackgroundTile{};
// The objects tileset goes in by value, not by move: the caller keeps it as
// the reference copy the Furniture checks read back. Since #1024 the
// Furniture checks read the Furniture sheet, so that is what `installAtlases`
// hands back.
void installTilesets(SectorTileset sectors, ObjectTileset const& objects,
    ObjectTileset agents, ObjectTileset furniture) {
  setSectorTileset(std::move(sectors), kInstalled);
  setObjectTileset(objects, kInstalled);
  setAgentTileset(std::move(agents), kInstalled);
  setFurnitureTileset(std::move(furniture), kInstalled);
}
// Installs the four viewport atlases from the same source manifest and
// atlas-set registry the game loads at run time. Each atlas is read from
// its own sheet and its own authored tileset: since the #1013 terminus
// deleted the merged `objects.png` sheet, the Objects, Agent and Furniture
// atlases are three separate images with three separate tilesets, and a
// check that paired all three with one file would be describing a bundle
// that no longer exists.
ObjectTileset installAtlases(std::filesystem::path const& directory) {
  auto manifest = YAML::LoadFile((directory / "Resources.yaml").string());
  auto registry = YAML::LoadFile((directory / "atlas-sets.yaml").string());
  auto resource = [&](std::string const& name) {
    for (auto entry : manifest["Resources"]["Resource"])
      if (entry["name"].as<std::string>() == name) return entry;
    throw std::runtime_error("Missing manifest resource: " + name);
  };
  // The authored tileset the atlas-set registry names for one atlas. The
  // registry is the single mapping from a manifest `ImageSet` to the file
  // that says what its tiles mean, so reading it here rather than naming a
  // path keeps this honest about what the game actually installs.
  auto authoredFor = [&](std::string const& atlas) {
    auto const entry = registry["sets"][atlas];
    require(bool(entry), "The atlas-set registry names no tileset for " + atlas);
    return ObjectTileset::load(
        (directory / entry["tileset"].as<std::string>()).string());
  };
  // The pairing this test drives: every tile the manifest declares is looked
  // up in the authored tileset behind the atlas, and nothing about a tile is
  // inferred from what it is called. The rule that used to derive a tile's
  // tintability by matching its name against `agent`, `human-*`,
  // `robot-standing` and `cleaning-bot-standing` is gone with the concept
  // it served (#1018).
  auto spritesFor = [&](std::string const& atlas) {
    auto const authored = authoredFor(atlas);
    auto const size = pngSize(
        directory / resource(atlas + "Image")["location"].as<std::string>());
    ObjectTileset tiles;
    tiles.width = size.first;
    tiles.height = size.second;
    for (auto image : resource(atlas)["Definitions"]["Definition"]["Images"]["Image"]) {
      auto name = image["name"].as<std::string>();
      auto const found = authored.sprites.find(name);
      require(found != authored.sprites.end(),
          "The authored " + atlas + " tileset defines no tile the atlas declares");
      tiles.sprites.emplace(name, ObjectSprite{{image["x"].as<int>(), image["y"].as<int>(),
          image["width"].as<int>(), image["height"].as<int>()}});
    }
    // The generated manifest and the authored tileset are the same list.
    // An authored tile the manifest never got is artwork nothing draws; a
    // declared tile the tileset never defines is a lookup that fails at run
    // time. Both are the drift this pairing exists to catch.
    require(tiles.sprites.size() == authored.sprites.size(),
        "The " + atlas + " manifest declares " + std::to_string(tiles.sprites.size())
        + " tiles and its authored tileset authors "
        + std::to_string(authored.sprites.size()));
    return tiles;
  };

  auto sectorsSize = pngSize(directory / resource("SectorAtlasImage")["location"].as<std::string>());
  SectorTileset sectors;
  sectors.width = sectorsSize.first; sectors.height = sectorsSize.second;
  gSectorAtlasWidth = sectorsSize.first; gSectorAtlasHeight = sectorsSize.second;
  for (auto image : resource("SectorAtlas")["Definitions"]["Definition"]["Images"]["Image"]) {
    auto name = image["name"].as<std::string>();
    SectorTileRegion region{image["x"].as<int>(), image["y"].as<int>(),
                           image["width"].as<int>(), image["height"].as<int>()};
    if (name.starts_with("boundary-")) sectors.boundaries.emplace(name.substr(9), region);
    else {
      if (name == "background") gBackgroundTile = region;
      sectors.surfaces.emplace(name, region);
    }
  }

  auto objects = spritesFor("ObjectAtlas");
  auto agents = spritesFor("AgentAtlas");
  // The Agent rows live on their own sheet, not in the Objects one.
  for (auto const& [name, sprite] : agents.sprites)
    require(!objects.sprites.contains(name), "Agent tiles must be removed from ObjectAtlas");
  // The Furniture sheet carries the role names the shipped catalogues use
  // plus the bare `chair` cell, and nothing else (#1029, #1024).
  auto furniture = spritesFor("FurnitureAtlas");
  // By value, not by move: the caller keeps this copy to read the Furniture
  // rectangles back against what it drew.
  installTilesets(std::move(sectors), objects, std::move(agents), furniture);
  return furniture;
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
// renderer kept its constructed Objects default and a World asking for
// the new Objects sheet drew the old one everywhere but in the editor.
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
  require(pushed[1].name == "ObjectAtlas",
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
      {"Sector", "SectorAtlas", ""},
      {"Objects", "BrokenAtlas",
          "Objects atlas 'BrokenAtlas' cannot be used by this World: 1 tile(s) do "
          "not match what the object renderer draws\n  - tile 'lift-car' is not in "
          "the atlas"},
      {"Agent", "AgentAtlas", ""},
      {"Furniture", "FurnitureAtlas", ""}};
  auto const report = app::worldAtlasReport(refused);
  require(report.find("BrokenAtlas") != std::string::npos
      && report.find("lift-car") != std::string::npos,
      "A refused Objects atlas does not reach the game's report with its reason: "
      + report);
  refused[1].diagnostic.clear();
  require(app::worldAtlasReport(refused).empty(),
      "An atlas selection every slot accepted still produced a report");
}

// The missing-render-stack case, which is where the four atlas-selection
// entry points used to disagree: Sector and Agent answered with an empty
// diagnostic - indistinguishable from an atlas that had been accepted -
// while Objects and Furniture reported the missing stack. An empty diagnostic
// has to mean exactly one thing, so all four refuse the same way, and the
// editor's Sector, Agent and Furniture artwork panels get the report the
// Objects panel already showed (#1034).
//
// This whole test process is CPU-only and never creates a render stack, so
// every call below lands in the missing-stack branch.
void checkAtlasSelectionWithoutRenderStack() {
  require(!worldRenderStackActive(),
      "A render stack is live, so the missing-stack case cannot be checked here");
  std::array<char const*, 4> const slots{"Sector", "Objects", "Agent",
      "Furniture"};
  std::array<std::string, 4> const refused{
      selectWorldSectorImageSet("SectorAtlas"),
      selectWorldObjectImageSet("ObjectAtlas"),
      selectWorldAgentImageSet("AgentAtlas"),
      selectWorldFurnitureImageSet("FurnitureAtlas")};
  for (std::size_t index = 0; index < refused.size(); ++index)
    require(!refused[index].empty(), std::string("The ") + slots[index]
        + " selection answered with nothing while no render stack exists, which a "
          "panel reads as an atlas that was accepted");
  for (std::size_t index = 1; index < refused.size(); ++index)
    require(refused[index] == refused[0], std::string("The ") + slots[index]
        + " slot reports a missing render stack differently from the Sector one: '"
        + refused[index] + "' against '" + refused[0] + "'");

  // The same refusals arrive through the game's own slot table, which is the
  // shape every artwork panel's report line prints, so no slot can come back
  // quieter than another on the way up.
  auto world = std::make_shared<core::World>("No render stack", 8, 2);
  world->addRoom("Front", 0, 0, 0, 8, 1);
  world->addRoom("Back", 1, 0, 0, 8, 1);
  world->finishBuild();
  world->pauseSimulation();
  auto const pushed = app::applyWorldAtlasSelections(*world);
  require(pushed.size() == refused.size(),
      "The game pushed " + std::to_string(pushed.size())
      + " atlas selections against a four-slot table");
  for (std::size_t index = 0; index < pushed.size(); ++index)
    require(pushed[index].diagnostic == refused[index],
        "The game's '" + pushed[index].slot + "' slot reports a missing render "
        "stack differently from the renderer's own entry for that slot: '"
        + pushed[index].diagnostic + "' against '" + refused[index] + "'");
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
      {"Sector", "SectorAtlas", ""},
      {"Objects", "ObjectAtlasBroken",
          "Objects atlas 'ObjectAtlasBroken' cannot be used by this World: 1 "
          "tile(s) do not match what the object renderer draws"
          "\n  - tile 'lift-car' is not in the atlas"},
      {"Agent", "AgentAtlas", ""},
      {"Furniture", "FurnitureAtlas", ""}};
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

// #1094 - a Layer bound to its own Sector sheet draws that sheet in the
// shipped game, not the World's.
//
// The whole chain runs here: the game's own atlas load (`MapResources`
// scan / load / loadAtlases) takes the bound sheet up off the manifest,
// the game's per-frame artwork sync installs it beside the World's own,
// and the game's render pass records the World. The sheet identity is
// then read off the recorded triangles, which is the only place a
// per-Layer binding can be seen to have taken effect. A sheet that is
// loaded and never installed draws the World's art - which is exactly
// what the shipped game did with every check in the tree still green,
// because every check that saw the Layer's sheet had installed it itself.
//
// The install handed to the sync is the renderer's own
// `AtlasInstaller::installSectorSheet` - the very call
// `WorldRenderStack::installLayerSectorImageSet` makes. The stack is the
// one link in the chain that cannot exist without a live OpenGL context;
// the install it runs is GPU-free by design (#1087), so the check
// supplies that one piece of plumbing and the game's own sync decides
// which sheets to take up and drives the install. Nothing in this check
// installs a Layer's sheet of its own - it says which install the game
// should be calling, and reads back which sheets the game actually asked
// that install for.
void checkLayerSectorSheetDrawing(std::filesystem::path const& directory) {
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

  // Every name that has gone through the install the game was handed,
  // since this was last cleared. "The game asked for this sheet" and "the
  // game did not ask for that one" are both facts this check can read.
  std::vector<std::string> installed;
  pf::render::AtlasInstaller installer;
  // The two-step the render stack's own `installLayerSectorImageSet` runs:
  // resolve the name, then put a sheet in under it whatever the resolve
  // left behind. A name that resolved to nothing - absent from the
  // manifest, or a resource that is not an Image set - gets the empty
  // sheet rather than no sheet at all, because the draw resolve only falls
  // through to the World's picture when the name is missing from the
  // registry, and a binding the author can go and fix has to show for
  // itself (#1096).
  auto const install = [&](std::string const& name) {
    installed.push_back(name);
    ResourcePtr resource;
    std::string diagnostic;
    try {
      resource = manager.getResource(name);
    } catch (std::exception const& error) {
      diagnostic = error.what();
    }
    if (resource) installer.installSectorSheet(name, resource, diagnostic);
    else installer.installEmptySectorSheet(name);
    return diagnostic;
  };

  // The Sector triangles of one recorded frame, counted by the sheet each
  // one says it was drawn from. That identity is what the renderer put on
  // the command (#1089); read no further than the command itself, it says
  // which sheet the pixels came from, which is the whole question.
  auto const sectorSheets = [](WorldDrawList const& draws) {
    std::map<std::string, int> bySheet;
    for (auto const& command : draws.commands())
      if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle
          && triangle->texture == WorldDrawList::Texture::SectorAtlas)
        ++bySheet[triangle->sectorSheet];
    return bySheet;
  };
  auto const namesText = [](std::vector<std::string> const& names) {
    std::string text;
    for (auto const& name : names) {
      if (!text.empty()) text += ", ";
      text += name;
    }
    return text.empty() ? std::string("<none>") : text;
  };
  auto const sheetsText = [](std::map<std::string, int> const& bySheet) {
    std::string text;
    for (auto const& [sheet, count] : bySheet) {
      if (!text.empty()) text += ", ";
      text += sheet + " x" + std::to_string(count);
    }
    return text.empty() ? std::string("<none>") : text;
  };
  auto const askedFor = [](std::vector<std::string> const& names, std::string const& sheet) {
    return static_cast<std::size_t>(std::count(names.begin(), names.end(), sheet));
  };
  auto const of = [](std::map<std::string, int> const& bySheet, std::string const& sheet) {
    auto const found = bySheet.find(sheet);
    return found == bySheet.end() ? 0 : found->second;
  };
  auto const total = [](std::map<std::string, int> const& bySheet) {
    int cells{};
    for (auto const& [sheet, count] : bySheet) cells += count;
    return cells;
  };
  // The missing-art magenta one recorded frame carries. A cell whose sheet
  // holds no such tile is filled with it and carries no sheet at all, so
  // it never shows up among the Sector-family triangles and has to be
  // counted on its own (#1096).
  auto const magentaCells = [](WorldDrawList const& draws) {
    auto const magenta = packColour(255, 0, 255, 255);
    int cells{};
    for (auto const& command : draws.commands())
      if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle
          && triangle->texture == WorldDrawList::Texture::None
          && triangle->colour == magenta)
        ++cells;
    return cells;
  };

  // The game's own load path, with the shipped manifest behind it.
  pf::application::MapResources resources(manager);
  resources.scan();
  resources.load();
  auto* map = resources.getMap();
  require(map != nullptr && map->getRuntime() != nullptr,
      "The game's own load path produced no World to draw (#1094)");
  auto const& world = map->getRuntime()->world();
  require(world.getLayerCount() >= 2,
      "The shipped World has " + std::to_string(world.getLayerCount())
          + " Layer(s); telling a bound Layer from an unbound one needs two (#1094)");
  auto const worldSheet = world.sectorImageSetResourceName();
  auto const layerSheet = std::string("SectorAtlasLayer1");
  std::uint32_t const front{0};
  std::uint32_t const back{1};
  // A binding is an authoring act, so it goes in through the authoring
  // seam the Layers table uses rather than onto the World by hand (#970).
  auto const bind = [&](std::uint32_t layer, std::optional<std::string> sheet) {
    game::WorldAuthoring authoring(*map->getRuntime());
    authoring.setLayerSectorImageSet(layer, std::move(sheet));
  };

  // One frame the way the game's render pass runs it: sync the artwork,
  // then record the World and count its Sector triangles by sheet.
  struct Frame {
    std::map<std::string, int> bySheet;
    app::WorldArtworkSync sync;
    std::vector<std::string> asked;
    int magenta{};
  };
  auto const frame = [&](char const* tag) {
    installed.clear();
    auto const sync = app::syncWorldArtwork(world, install);
    auto const draws = app::drawWorld(world, 1280, 720);
    Frame shot{sectorSheets(draws), sync, installed, magentaCells(draws)};
    require(total(shot.bySheet) > 0,
        std::string("The shipped World drew no Sector artwork in the '") + tag
            + "' pass, so nothing here can tell one sheet from another (#1094)");
    return shot;
  };

  // The game takes its own sheets up off the manifest first.
  resources.loadAtlases();

  // The World's own sheet, installed the way the render stack installs it
  // at start-up (#1002). Both sheets this check compares then come from
  // the same install path, so anything they draw differently is something
  // the binding did and nothing else.
  std::string worldInstall;
  installer.installSectorSlot(worldSheet, manager.getResource(worldSheet), worldInstall);
  require(worldInstall.empty(),
      "The World's own Sector sheet was refused by the install the game drives: " + worldInstall
          + " (#1094)");

  // ---- Nothing bound: every Sector wears the World's own sheet. ----------
  auto const inherited = frame("inherit");
  require(inherited.sync.layerSheets.empty() && inherited.asked.empty(),
      "A World with no Layer bound took up a Layer Sector sheet: " + namesText(inherited.asked)
          + " (#1094)");
  require(inherited.bySheet.size() == 1 && of(inherited.bySheet, worldSheet) > 0,
      "With no Layer bound, the game did not draw every Sector from the World's own '" + worldSheet
          + "' sheet: " + sheetsText(inherited.bySheet) + " (#1094)");
  auto const cells = total(inherited.bySheet);

  // ---- The front Layer bound to its own sheet. ---------------------------
  bind(front, layerSheet);
  // The game's own atlas load takes the newly-bound sheet up off the
  // manifest; the game's own artwork sync is what installs it beside the
  // World's. Only the second step can fail the way this check is about: a
  // sheet loaded and never installed draws the World's art.
  resources.loadAtlases();
  auto const frontBound = frame("front Layer bound");
  require(askedFor(frontBound.asked, layerSheet) == 1,
      "The game asked for the bound Layer sheet "
          + std::to_string(askedFor(frontBound.asked, layerSheet))
          + " time(s) through its own artwork sync, expected exactly once: "
          + namesText(frontBound.asked) + " (#1094)");
  require(askedFor(frontBound.asked, worldSheet) == 0,
      "The game put the World's own sheet '" + worldSheet
          + "' through the Layer alongside path, which replaces the Sector slot's own entry (#1094)");
  require(frontBound.sync.layerSheets.size() == 1 && frontBound.sync.layerSheets[0].layer == front
          && frontBound.sync.layerSheets[0].name == layerSheet
          && frontBound.sync.layerSheets[0].accepted(),
      "The Layer sheet the game took up is Layer "
          + (frontBound.sync.layerSheets.empty()
                 ? std::string("<none>")
                 : std::to_string(frontBound.sync.layerSheets[0].layer) + " '"
                       + frontBound.sync.layerSheets[0].name + "': "
                       + frontBound.sync.layerSheets[0].diagnostic)
          + " (#1094)");
  auto const frontCells = of(frontBound.bySheet, layerSheet);
  require(frontCells > 0,
      "A Layer bound to its own Sector sheet drew no triangle from it; the game drew "
          + sheetsText(frontBound.bySheet) + " (#1094)");
  require(of(frontBound.bySheet, worldSheet) == cells - frontCells,
      "Binding the front Layer left " + std::to_string(of(frontBound.bySheet, worldSheet))
          + " of " + std::to_string(cells) + " Sector triangles on the World's sheet and moved "
          + std::to_string(frontCells) + " onto the Layer's; the game drew "
          + sheetsText(frontBound.bySheet) + " (#1094)");
  require(total(frontBound.bySheet) == cells,
      "Binding the Layer's own sheet changed how many Sector triangles are drawn ("
          + std::to_string(total(frontBound.bySheet)) + " against " + std::to_string(cells)
          + "); a binding is artwork only and moves no geometry (#1094)");

  // ---- The back Layer bound instead: only its own triangles move. --------
  bind(front, std::nullopt);
  bind(back, layerSheet);
  resources.loadAtlases();
  auto const backBound = frame("back Layer bound");
  auto const backCells = of(backBound.bySheet, layerSheet);
  require(backCells > 0,
      "A back Layer bound to its own Sector sheet drew no triangle from it, seen through the "
          "apertures in front of it; the game drew " + sheetsText(backBound.bySheet) + " (#1094)");
  require(total(backBound.bySheet) == cells,
      "Binding the back Layer changed how many Sector triangles are drawn; a binding is artwork "
          "only and moves no geometry (#1094)");
  // Every Sector triangle belongs to exactly one Layer, so the front's
  // share and the back's share are the whole canvas between them. Each
  // binding moved its own Layer's triangles and no one else's.
  require(frontCells + backCells == cells,
      "Binding the front Layer moved " + std::to_string(frontCells) + " Sector triangles and "
          "binding the back Layer moved " + std::to_string(backCells) + ", which is not the "
          + std::to_string(cells) + " the canvas draws; one of the two bindings reached past its "
          "own Layer (#1094)");

  // ---- A Layer bound to the World's own set is not installed again. -----
  bind(back, std::nullopt);
  bind(front, worldSheet);
  auto const asWorld = frame("bound to the World's own set");
  require(asWorld.sync.layerSheets.empty() && asWorld.asked.empty(),
      "A Layer bound to the World's own set was taken up through the Layer alongside path: "
          + namesText(asWorld.asked) + ", which replaces the sheet the Sector slot owns (#1094)");
  require(asWorld.bySheet == inherited.bySheet,
      "A Layer bound to the World's own set did not draw what an unbound Layer draws: "
          + sheetsText(asWorld.bySheet) + " against " + sheetsText(inherited.bySheet) + " (#1094)");

  // ---- Two Layers bound to one sheet take it up once. ------------------
  bind(front, layerSheet);
  bind(back, layerSheet);
  auto const shared = frame("two Layers, one sheet");
  require(shared.asked.size() == 1 && shared.asked[0] == layerSheet,
      "Two Layers bound to one Sector sheet took it up " + std::to_string(shared.asked.size())
          + " time(s): " + namesText(shared.asked) + " (#1094)");
  require(shared.sync.layerSheets.size() == 1 && shared.sync.layerSheets[0].layer == front
          && shared.sync.layerSheets[0].accepted(),
      "The shared Layer sheet was not reported as taken up once by the front Layer (#1094)");
  require(of(shared.bySheet, worldSheet) == 0
          && of(shared.bySheet, layerSheet) == cells,
      "Two Layers bound to one sheet did not draw the whole canvas from it: "
          + sheetsText(shared.bySheet) + " against " + std::to_string(cells)
          + " Sector triangles (#1094)");

  // ---- An empty binding name asks the game for nothing. ----------------
  bind(front, std::string());
  bind(back, std::nullopt);
  auto const empty = frame("empty binding name");
  require(empty.asked.empty() && empty.sync.layerSheets.empty(),
      "An empty Layer binding name was taken up as a Sector sheet: " + namesText(empty.asked)
          + " (#1094)");
  require(empty.bySheet == inherited.bySheet,
      "An empty Layer binding name changed what the Layer draws: " + sheetsText(empty.bySheet)
          + " against " + sheetsText(inherited.bySheet) + " (#1094)");

  // ---- Unbinding hands the Layer back to the World's sheet. --------------
  bind(front, std::nullopt);
  auto const cleared = frame("unbound again");
  require(cleared.sync.layerSheets.empty() && cleared.asked.empty(),
      "An unbound Layer still asked for a Sector sheet: " + namesText(cleared.asked) + " (#1094)");
  require(cleared.bySheet == inherited.bySheet,
      "Unbinding the Layer did not restore the World's own sheet: " + sheetsText(cleared.bySheet)
          + " against " + sheetsText(inherited.bySheet) + " (#1094)");

  // ---- A binding that resolves to nothing paints magenta (#1096). -------
  //
  // The back Layer is the one broken here so the front Layer keeps drawing
  // the World's sheet in the same frame: the empty sheet goes in beside the
  // drawn one and must not reach past its own Layer. The back Layer's own
  // Sector triangles are pure surface - the frames are authored on the
  // front wall and wear the front Layer's sheet - so the magenta that
  // replaces them is counted against the same `backCells` this check
  // already measured for that Layer.
  auto const magentaOver = [](Frame const& shot, Frame const& baseline) {
    return shot.magenta - baseline.magenta;
  };
  require(inherited.magenta == 0,
      "The shipped World drew " + std::to_string(inherited.magenta)
          + " missing-art magenta triangle(s) with every Layer unbound, so a magenta count "
            "here would not say anything about a broken binding (#1096)");

  // Shape one: the name is in no manifest at all.
  bind(back, std::string("NoSuchSectorSheet"));

  bool refusedLoad{false};
  try {
    resources.loadAtlases();
  } catch (std::exception const&) {
    refusedLoad = true;
  }
  require(!refusedLoad,
      "A Layer bound to a name that does not exist refused the World's atlas load (#1096)");
  auto const missing = frame("back Layer bound to a name that does not exist");
  require(askedFor(missing.asked, "NoSuchSectorSheet") == 1,
      "A Layer bound to a name that does not exist was taken up "
          + std::to_string(askedFor(missing.asked, "NoSuchSectorSheet"))
          + " time(s) through the game's own artwork sync, expected exactly once: "
          + namesText(missing.asked) + " (#1096)");
  require(missing.sync.layerSheets.size() == 1 && missing.sync.layerSheets[0].layer == back
          && missing.sync.layerSheets[0].name == "NoSuchSectorSheet"
          && !missing.sync.layerSheets[0].accepted(),
      "A Layer bound to a name that does not exist was not reported as refused: "
          + (missing.sync.layerSheets.empty()
                 ? std::string("<none>")
                 : std::to_string(missing.sync.layerSheets[0].layer) + " '"
                       + missing.sync.layerSheets[0].name + "' -> '"
                       + missing.sync.layerSheets[0].diagnostic + "' (#1096)"));
  // Nothing draws the broken name: the sheet installed under it has no
  // tiles to draw, which is the point of it.
  require(of(missing.bySheet, "NoSuchSectorSheet") == 0,
      "A Sector sheet installed empty still drew " + std::to_string(of(missing.bySheet, "NoSuchSectorSheet"))
          + " Sector-family triangle(s) (#1096)");
  // The Layer does not fall through to the World's sheet: its whole share
  // of the canvas left it, and the front Layer's share stayed on it.
  require(of(missing.bySheet, worldSheet) == cells - backCells,
      "A Layer bound to a name that does not exist left " + std::to_string(of(missing.bySheet, worldSheet))
          + " Sector triangles on the World's '" + worldSheet + "' sheet of " + std::to_string(cells)
          + ", where the front Layer's " + std::to_string(cells - backCells)
          + " alone belong; the broken Layer drew " + sheetsText(missing.bySheet) + " (#1096)");
  // And it paints magenta for exactly the cells it took off the World's
  // sheet, seen through the render resolve rather than read off the
  // install's own bookkeeping.
  require(magentaOver(missing, inherited) == backCells,
      "A Layer bound to a name that does not exist painted " + std::to_string(magentaOver(missing, inherited))
          + " missing-art magenta triangle(s) against the " + std::to_string(backCells)
          + " that Layer draws; the author is not getting the visual signal the binding is "
            "broken (#1096)");

  // Shape two: the name resolves, but to something that is not an Image
  // set. Same class of bad binding, and the same picture on screen.
  //
  // `Citadel` is a shipped World document - a resource the manifest really
  // registers, so the resolve really does come back with something, and
  // really does come back with the wrong kind of something. Guarded here so
  // a manifest edit that renames it fails with this sentence rather than
  // with a wrong-kind refusal that never arrived.
  ResourcePtr wrongKindResource;
  try {
    wrongKindResource = manager.getResource("Citadel");
  } catch (std::exception const&) {
  }
  require(wrongKindResource
          && !std::dynamic_pointer_cast<ImageSetResource>(wrongKindResource),
      "'Citadel' is no longer a registered resource that is not an Image set, so this "
          "check no longer exercises the wrong-kind Layer binding shape (#1096)");
  bind(back, std::string("Citadel"));
  refusedLoad = false;
  try {
    resources.loadAtlases();
  } catch (std::exception const&) {
    refusedLoad = true;
  }
  require(!refusedLoad,
      "A Layer bound to a resource that is not an Image set refused the World's atlas load (#1096)");
  auto const wrongKind = frame("back Layer bound to a resource that is not an Image set");
  require(askedFor(wrongKind.asked, "Citadel") == 1,
      "A Layer bound to a resource that is not an Image set was taken up "
          + std::to_string(askedFor(wrongKind.asked, "Citadel"))
          + " time(s) through the game's own artwork sync, expected exactly once: "
          + namesText(wrongKind.asked) + " (#1096)");
  require(wrongKind.sync.layerSheets.size() == 1 && wrongKind.sync.layerSheets[0].layer == back
          && wrongKind.sync.layerSheets[0].name == "Citadel"
          && !wrongKind.sync.layerSheets[0].accepted(),
      "A Layer bound to a resource that is not an Image set was not reported as refused: "
          + (wrongKind.sync.layerSheets.empty()
                 ? std::string("<none>")
                 : std::to_string(wrongKind.sync.layerSheets[0].layer) + " '"
                       + wrongKind.sync.layerSheets[0].name + "' -> '"
                       + wrongKind.sync.layerSheets[0].diagnostic + "' (#1096)"));
  require(of(wrongKind.bySheet, "Citadel") == 0
          && of(wrongKind.bySheet, worldSheet) == cells - backCells,
      "A Layer bound to a resource that is not an Image set did not come off the World's sheet: "
          + sheetsText(wrongKind.bySheet) + " against " + std::to_string(cells - backCells)
          + " front-Layer triangles (#1096)");
  require(magentaOver(wrongKind, inherited) == backCells,
      "A Layer bound to a resource that is not an Image set painted "
          + std::to_string(magentaOver(wrongKind, inherited))
          + " missing-art magenta triangle(s) against the " + std::to_string(backCells)
          + " that Layer draws (#1096)");

  // The two shapes agree. Same class of bad binding, one picture.
  require(wrongKind.bySheet == missing.bySheet
          && wrongKind.magenta == missing.magenta,
      "The two failure shapes disagree on screen - a name that does not exist drew "
          + sheetsText(missing.bySheet) + " with " + std::to_string(missing.magenta)
          + " magenta, a name naming the wrong kind drew " + sheetsText(wrongKind.bySheet)
          + " with " + std::to_string(wrongKind.magenta) + " (#1096)");

  // ---- Unbinding hands the Layer back to the World's sheet. --------------
  bind(back, std::nullopt);
  auto const restored = frame("broken binding cleared");
  require(restored.magenta == 0 && restored.bySheet == inherited.bySheet,
      "Clearing a broken Layer binding did not restore what the Layer drew before: "
          + sheetsText(restored.bySheet) + " with " + std::to_string(restored.magenta)
          + " magenta against " + sheetsText(inherited.bySheet) + " (#1096)");

  resources.unload();
  // This check put the real, installed World Sector sheet on the draw path;
  // the checks that follow read the fixture the module installed for them,
  // so put that back (#1094).
  installAtlases(directory);
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

// What one pass of the game's camera recorded: the viewport it settled on
// and the Player artwork that came out of it. The viewport is read back
// through the mapping the editor draws that Layer by (#1105), so "the
// same screen positions the editor shows" is a statement about one
// mapping rather than about two pictures somebody has to eyeball (#1106).
struct GameCameraReading
{
  UISettings view{};
  float drawnTopY{ 1e9f };
  float drawnBottomY{ -1e9f };
  unsigned drawnPieces{ 0 };
};

GameCameraReading readGameCamera(core::World const& world, float width, float height) {
  auto const draws = app::drawWorld(world, width, height);
  GameCameraReading reading{};
  for (auto const& command : draws.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle
        && triangle->texture == WorldDrawList::Texture::AgentAtlas) {
      ++reading.drawnPieces;
      for (auto const& position : triangle->positions) {
        reading.drawnTopY = std::min(reading.drawnTopY, position.y);
        reading.drawnBottomY = std::max(reading.drawnBottomY, position.y);
      }
    }
  reading.view = app::worldCanvasViewport();
  return reading;
}

// The mapping one Layer's picture is drawn by, assembled from the shared
// pieces the renderer states: the viewport's own mapping at this zoom
// (#1100) lifted by the World's own staircase for that Layer (#1101),
// scaled by the one statement of the document-pixels-to-screen-pixels
// crossing (#1105). Composed here rather than called through
// `worldToScreenForLayer` because that helper lives in a translation unit
// the shipped game's shared object only carries while its own camera needs
// it, and a check that fails to link says nothing about the picture.
pf::render::WorldToScreen layerMapping(UISettings const& view,
    core::World const& world, uint32_t layer) {
  return pf::render::worldToScreenAt(view, view.worldZoom).withFloorLift(
      pf::render::floorLiftScreenPx(world.layerFloorOffsetPx(layer),
          view.worldZoom));
}

// The camera-follow contract, stated against the mapping the editor draws
// by: the Player's body centre is the canvas centre, the feet the game
// recorded sit on the feet the editor's mapping of that Layer answers,
// and the whole body is inside the canvas rather than hung off its top.
void requirePlayerCentred(core::World const& world, GameCameraReading const& reading,
    core::Agent const& player, float width, float height, std::string const& where) {
  require(reading.drawnPieces > 0, "the Player's pose artwork must reach the game canvas" + where);
  auto const position = player.getGlobalPosition();
  auto const mapping = layerMapping(reading.view, world,
      static_cast<uint32_t>(reading.view.visibleLayer));
  require(std::abs(mapping.toScreenX(position.x) - width * 0.5f) < 0.001f
      && std::abs(mapping.toScreenY(position.y + player.getHeight() * 0.5f)
          - height * 0.5f) < 0.001f,
      "the camera must keep the Player's own Layer centred on the canvas" + where
          + ": centre lands at " + std::to_string(mapping.toScreenX(position.x)) + ","
          + std::to_string(mapping.toScreenY(position.y + player.getHeight() * 0.5f))
          + " against " + std::to_string(width * 0.5f) + "," + std::to_string(height * 0.5f));
  require(std::abs(reading.drawnBottomY - mapping.toScreenY(position.y)) < 0.001f,
      "the Player the game recorded must sit where the editor's Layer mapping puts its feet" + where
          + ": " + std::to_string(reading.drawnBottomY) + " against "
          + std::to_string(mapping.toScreenY(position.y)));
  require(reading.drawnTopY >= 0.0f && reading.drawnBottomY <= height,
      "the shifted Player must be drawn whole inside the canvas" + where + ": top "
          + std::to_string(reading.drawnTopY) + " against a canvas "
          + std::to_string(height) + " tall");
}

// The shipped game draws the same depth the author saw, and its camera
// carries the offset of the Layer the Player is on as the Player changes
// Layers (#1106).
void checkPlayerCameraFloorDepth() {
  float const width = 640.0f;
  float const height = 360.0f;
  auto world = std::make_shared<core::World>("Follow Player at depth", 40, 3);
  while (world->getLayerCount() < 3) world->addLayer();
  auto const front = world->addRoom("Front", 0, 0, 0, 20, 1);
  auto const middle = world->addRoom("Middle", 1, 0, 0, 20, 1);
  auto const back = world->addRoom("Back", 2, 0, 0, 20, 1);
  world->finishBuild();
  world->pauseSimulation();
  std::string diagnostic{};
  require(world->setSectorFloorDepth(front, 48, &diagnostic).accepted,
      "the depth fixture must accept its front depth: " + diagnostic);
  require(world->setSectorFloorDepth(middle, 24, &diagnostic).accepted,
      "the depth fixture must accept its middle depth: " + diagnostic);
  require(world->layerFloorOffsetPx(0) == 0 && world->layerFloorOffsetPx(1) == 48
      && world->layerFloorOffsetPx(2) == 72,
      "the staircase the camera has to carry is 0, 48, 72");

  // One Agent per Layer, all at the same World position. The controlled one
  // moves between them, which drives the same per-frame read of the
  // Player's own Sector that a Door or Stairwell crossing drives: the
  // camera asks the World where the Player is every frame and takes that
  // Layer's offset with it.
  std::array<core::AgentId, 3> player{};
  std::array<uint32_t, 3> const layers{ front, middle, back };
  for (std::size_t i = 0; i < layers.size(); ++i)
    player[i] = world->createAgent("Layer Agent", layers[i], 0, 10.f);
  core::AgentId controlled{};
  bool following{ false };
  auto const follow = [&](std::size_t i) {
    if (following) world->clearAgentBehaviourAssignment(controlled);
    require(world->setAgentBehaviourAssignment(player[i],
        core::PlayerControlledBehaviour, 1, {}), "the camera fixture must take the Player");
    controlled = player[i];
    following = true;
    auto const reading = readGameCamera(*world, width, height);
    require(reading.view.visibleLayer == static_cast<int>(i),
        "the camera must show the Layer the Player is on, not the one it left");
    requirePlayerCentred(*world, reading, *world->lookupAgent(player[i]).entity,
        width, height, " with the Player on Layer " + std::to_string(i));
    return reading;
  };
  auto const frontView = follow(0);
  auto const middleView = follow(1);
  auto const backView = follow(2);
  // The camera moved by exactly the Layer offsets it is following. A camera
  // that ignored the lift would leave all three identical, and a camera
  // that applied the Sector's own depth rather than the Layer's
  // accumulated offset would be 24 off by the third Layer.
  require(middleView.view.yOffset == frontView.view.yOffset - 48.0f
      && backView.view.yOffset == frontView.view.yOffset - 72.0f,
      "the camera must follow each Layer's accumulated offset: "
          + std::to_string(frontView.view.yOffset) + ", "
          + std::to_string(middleView.view.yOffset) + ", "
          + std::to_string(backView.view.yOffset));
  require(middleView.view.xOffset == frontView.view.xOffset
      && backView.view.xOffset == frontView.view.xOffset,
      "floor depth shifts the picture vertically and leaves the horizontal centring alone");
  // The game's pass is the editor's pass: same World, same viewport, one
  // command stream.
  requireSameCanvasAsEditor(world, 1280.0f, 720.0f, "a World with floor depth");
}

// One Level of depth on a canvas barely two Levels tall: the whole point
// of carrying the lift in the centring is that the Player the game is
// watching does not get hung off the top of the window.
void checkPlayerCameraDeepLayerStaysOnScreen() {
  float const width = 640.0f;
  float const height = 2.0f * CORE_LEVEL_HEIGHT_PIXELS;
  auto world = std::make_shared<core::World>("Deep Layer camera", 20, 2);
  auto const front = world->addRoom("Front", 0, 0, 0, 20, 1);
  auto const deep = world->addRoom("Deep", 1, 0, 0, 20, 1);
  world->finishBuild();
  world->pauseSimulation();
  std::string diagnostic{};
  require(world->setSectorFloorDepth(front, CORE_LEVEL_HEIGHT_PIXELS, &diagnostic).accepted,
      "the depth fixture must accept one Level of depth: " + diagnostic);
  require(world->layerFloorOffsetPx(1) == CORE_LEVEL_HEIGHT_PIXELS,
      "the Layer behind a one-Level floor sits a whole Level up");
  auto const id = world->createAgent("Deep player", deep, 0, 10.f);
  require(world->setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}),
      "the depth camera fixture must take the Player");
  auto const reading = readGameCamera(*world, width, height);
  requirePlayerCentred(*world, reading, *world->lookupAgent(id).entity, width, height,
      " with the Player a Level behind a deep floor");
  // Non-vacuity: centring the unlifted position on this canvas leaves the
  // body centre exactly on the top edge with a whole Level of lift, so the
  // check above is testing the camera's pull-back rather than a fixture
  // too loose to notice the difference.
  require(height * 0.5f - CORE_LEVEL_HEIGHT_PIXELS <= 0.0f,
      "the deep fixture must be tight enough that the unlifted centring runs off the top");
}

// A World with every floor depth at zero renders in the game exactly as
// it rendered before the lift term existed - the centring is the same
// arithmetic, not the same picture to within a tolerance.
void checkPlayerCameraFlatWorldUnchanged() {
  float const width = 640.0f;
  float const height = 360.0f;
  auto world = std::make_shared<core::World>("Flat camera", 20, 2);
  world->addRoom("Front", 0, 0, 0, 20, 1);
  auto const back = world->addRoom("Back", 1, 0, 0, 20, 1);
  world->finishBuild();
  world->pauseSimulation();
  auto const id = world->createAgent("Flat player", back, 0, 10.f);
  require(world->setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}),
      "the flat camera fixture must take the Player");
  auto const reading = readGameCamera(*world, width, height);
  auto& player = *world->lookupAgent(id).entity;
  requirePlayerCentred(*world, reading, player, width, height, " in a flat World");
  auto const centreY = player.getGlobalPosition().y + player.getHeight() * 0.5f;
  require(reading.view.yOffset == height * 0.5f - centreY * CORE_LEVEL_HEIGHT_PIXELS,
      "a flat World must centre by exactly the expression the camera used before floor depth: "
          + std::to_string(reading.view.yOffset));
  require(reading.view.xOffset == width * 0.5f
      - player.getGlobalPosition().x * CORE_CELL_WIDTH_PIXELS,
      "a flat World must keep its horizontal centring exactly: "
          + std::to_string(reading.view.xOffset));
  // The no-Player camera fits the World's Layer-0 content, and Layer 0 is
  // the one Layer nothing ever lifts: two Worlds with the same Layer-0
  // geometry, one of them lifted a whole Level behind its front Sector,
  // draw byte for byte the same fitted picture. Neither carries a Player,
  // so both take the fitted branch and nothing but the depth differs.
  auto fitted = [](int32_t depth) {
    auto fittedWorld = std::make_shared<core::World>("Fitted camera", 20, 2);
    auto const front = fittedWorld->addRoom("Front", 0, 0, 0, 20, 1);
    fittedWorld->addRoom("Back", 1, 0, 0, 20, 1);
    fittedWorld->finishBuild();
    fittedWorld->pauseSimulation();
    std::string diagnostic{};
    if (depth != 0 && !fittedWorld->setSectorFloorDepth(front, depth, &diagnostic).accepted)
      throw std::runtime_error("the fitted fixture refused its depth: " + diagnostic);
    return fittedWorld;
  };
  auto const flat = fitted(0);
  auto const deep = fitted(CORE_LEVEL_HEIGHT_PIXELS);
  require(deep->layerFloorOffsetPx(1) == CORE_LEVEL_HEIGHT_PIXELS,
      "the fitted deep fixture really is lifted: "
          + std::to_string(deep->layerFloorOffsetPx(1)));
  require(signature(app::drawWorld(*flat, width, height))
      == signature(app::drawWorld(*deep, width, height)),
      "the fitted no-Player camera must draw a deep World exactly as it draws the flat one");
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
  auto const& chair = atlas.sprites.at("chair-L0").region;
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
  // The shipped catalogue names the chair by its role tile (#1029), so the
  // alternate atlas has to carry that name for the piece to draw from it.
  custom.sprites.emplace("chair-L0", ObjectSprite{{8, 63, 71, 193}, false});
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
  // The Background's own tile, found by the UVs a quad sampled rather
  // than by a colour the renderer used to pack from the Sector.
  auto hasBackTile = [](WorldDrawList const& draws) {
    auto const left = float(gBackgroundTile.x) / float(gSectorAtlasWidth);
    auto const right = float(gBackgroundTile.x + gBackgroundTile.width) / float(gSectorAtlasWidth);
    auto const top = float(gBackgroundTile.y) / float(gSectorAtlasHeight);
    auto const bottom = float(gBackgroundTile.y + gBackgroundTile.height) / float(gSectorAtlasHeight);
    for (auto const& command : draws.commands())
      if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
          triangle && triangle->texture == WorldDrawList::Texture::SectorAtlas)
        for (auto uv : triangle->texcoords)
          if (uv.x >= left && uv.x <= right && uv.y >= top && uv.y <= bottom) return true;
    return false;
  };
  require(!hasBackTile(hidden), "Deeper Background must not leak through an opaque Room");
  world->addSectorWindow(0, 0, 1, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->addSectorWindow(1, 0, 2, 3, 1,
      {false, core::Window::State::Closed, core::Window::Style::Clear});
  world->finishBuild(); world->pauseSimulation();
  auto revealed = app::drawWorld(*world, 1280, 720);
  require(hasBackTile(revealed), "Aligned clear Windows must reveal the deeper Background");
  for (auto const& command : revealed.commands())
    if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
        triangle && triangle->texture == WorldDrawList::Texture::SectorAtlas) {
      auto const left = float(gBackgroundTile.x) / float(gSectorAtlasWidth);
      auto const right = float(gBackgroundTile.x + gBackgroundTile.width) / float(gSectorAtlasWidth);
      auto const top = float(gBackgroundTile.y) / float(gSectorAtlasHeight);
      auto const bottom = float(gBackgroundTile.y + gBackgroundTile.height) / float(gSectorAtlasHeight);
      auto const onBackground = std::any_of(std::begin(triangle->texcoords),
          std::end(triangle->texcoords), [&](auto uv) {
            return uv.x >= left && uv.x <= right && uv.y >= top && uv.y <= bottom;
          });
      if (!onBackground) continue;
      require(triangle->clip.maximum.x - triangle->clip.minimum.x < 3 * CORE_CELL_WIDTH_PIXELS,
              "Deeper Background must retain the intersection of both Window apertures");
    }
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
  // this probe never reaches a second Level; the crop is the point - the tile
  // keeps its bottom edge on the probed floor line at one artwork pixel to one
  // screen pixel, and the top quarter of the tile never reaches the screen.
  float const probeWide = 2.0f * CORE_CELL_WIDTH_PIXELS * zoom;
  float const probeHigh = 0.75f * CORE_LEVEL_HEIGHT_PIXELS * zoom;
  require(drawSectorTileSurface(drawingSectorSheet(), "corridor", &commands, {0, 0}, {probeWide, probeHigh}, zoom),
      "Station corridor must render");
  require(textured(commands, WorldDrawList::Texture::SectorAtlas) == 4,
      "Two World cells must each render a tile at zoom 2 independent of source size");
  auto const& triangle = std::get<WorldDrawList::Triangle>(commands.commands().front());
  float maxX{}, maxY{}, topUvY{ 1.f }, floorUvY{};
  for (int i = 0; i < 3; ++i) {
    maxX = std::max(maxX, triangle.positions[i].x);
    maxY = std::max(maxY, triangle.positions[i].y);
    // Atlas rows run top down, so the least uv.y is the topmost texel row the
    // crop still samples and the greatest is the tile's own floor end.
    topUvY = std::min(topUvY, triangle.texcoords[i].y);
    floorUvY = std::max(floorUvY, triangle.texcoords[i].y);
  }
  require(maxX == CORE_CELL_WIDTH_PIXELS * zoom && maxY == probeHigh
      && topUvY > .24f && topUvY < .26f && floorUvY > .99f,
      "Tile must fill 1x1 World units and crop the top of a partial-height cell");
  WorldDrawList missing({{0, 0}, {1000, 1000}});
  require(drawSectorTileSurface(drawingSectorSheet(), "room", &missing, {0, 0},
      {CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS}),
      "Missing tile must emit fallback geometry");
  auto const& fallback = std::get<WorldDrawList::Triangle>(missing.commands().front());
  require(fallback.texture == WorldDrawList::Texture::None
      && fallback.colour == packColour(255, 0, 255), "Missing tile must be opaque magenta");
  // One World cell wide; the 2 is a plain line's pixel height, not a World unit.
  require(!drawSectorTileBoundary(drawingSectorSheet(), "floor", &missing, {0, 0}, {CORE_CELL_WIDTH_PIXELS, 2}),
      "Missing boundary must request plain-line fallback");
  setSectorTileset({}, kInstalled);
  WorldDrawList unavailable({{0, 0}, {1000, 1000}});
  require(drawSectorTileSurface(drawingSectorSheet(), "corridor", &unavailable, {0, 0},
      {CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS}),
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
    checkLayerSectorSheetDrawing(directory);
    checkCanvasAtlasSelection();
    checkAtlasSelectionWithoutRenderStack();
    checkCanvasAtlasReport();
    checkWorldText(directory);
    checkAgentPoses();
    checkAgentsAndMarkers();
    checkPlayerCamera();
    checkPlayerCameraFloorDepth();
    checkPlayerCameraDeepLayerStaysOnScreen();
    checkPlayerCameraFlatWorldUnchanged();
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
