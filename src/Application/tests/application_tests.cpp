#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <applib/StateTransitionData.h>
#include <willpower/application/StateExceptions.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <core/World.h>
#include <core/Agent.h>
#include <core/DoorSectorObject.h>
#include <core/AirlockTransit.h>
#include <core/ChamberTransit.h>
#include <core/WindowSectorObject.h>
#include <core/AccessPanel.h>
#include <core/YamlSerializer.h>
#include <cmath>

#include "Application.h"
#include "Map.h"
#include "MapResources.h"
#include "StateLifecycle.h"
#include "StatePlay.h"
#include "DirectoryResourceLocation.h"

namespace {
void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}

// Preserve the production ImageSet/dependency path, replacing only GPU upload.
class HeadlessAtlasImage final : public wp::application::resourcesystem::ImageResource {
  wp::application::resourcesystem::ResourceManager& mManager;
  bool& mFail;
  using Resource::parseData;
  void create(wp::application::resourcesystem::DataStreamPtr data,
              wp::application::resourcesystem::ResourceManager*) override {
    parseData(data); // Decode the real PNG; no definition is needed for this test resource.
  }
  bool load(mpp::RenderSystem*, mpp::ResourceManager*) override {
    auto map = std::dynamic_pointer_cast<pf::application::Map>(mManager.getResource("World", "World"));
    require(map && map->getWorld() && mManager.isResourceLoaded(map),
            "Atlas post work must run after Map loading completes");
    if (mFail && getName() == "ObjectAtlasImage")
      throw std::runtime_error("Injected atlas upload failure");
    return true;
  }
  bool unload(mpp::RenderSystem*, mpp::ResourceManager*) override { return true; }
public:
  HeadlessAtlasImage(std::string const& name,
                     wp::application::resourcesystem::ResourceManager& manager, bool& fail,
                     wp::application::resourcesystem::ResourceLocation* location)
      : ImageResource(name, "", name == "SectorAtlasImage"
            ? "textures/sector-atlas-draft.png" : "textures/objects.png", {}, location),
        mManager(manager), mFail(fail) {}
};

void checkAtlases(wp::application::resourcesystem::ResourceManager& manager, bool loaded) {
  for (auto name : {"SectorAtlas", "ObjectAtlas"}) {
    auto set = std::dynamic_pointer_cast<wp::application::resourcesystem::ImageSetResource>(
        manager.getResource(name));
    // Other manifest resources retain ObjectAtlas dependencies even when not
    // used by this World; MapUnload must respect those acquisitions.
    if (!loaded && std::string_view(name) == "ObjectAtlas") {
      require(set && manager.isResourceLoaded(set) == manager.isResourceLoaded(set->getImage()),
              "Shared Object atlas and image must retain consistent loaded state");
      continue;
    }
    require(set && manager.isResourceLoaded(set) == loaded &&
                manager.isResourceCreated(set) == loaded &&
                manager.isResourceLoaded(set->getImage()) == loaded &&
                manager.isResourceCreated(set->getImage()) == loaded,
            "Atlas resources and images must follow MapLoad/MapUnload lifetime");
  }
}

void checkAtlasFailure(wp::application::resourcesystem::ResourceManager& manager, bool& fail) {
  pf::application::MapResources resources(manager);
  resources.scan();
  applib::StateTransitionData arguments;
  arguments.userData = &resources;
  pf::application::StateMapLoad state;
  state._resume(&arguments);
  fail = true;
  bool rejected = false;
  try { state._update(0.f); }
  catch (wp::application::ReturnFromStateException const&) {
    throw std::runtime_error("Failed post work must not transition to Play");
  }
  catch (std::runtime_error const&) { rejected = true; }
  require(rejected && !resources.getMap(), "Failed atlas post work must roll back MapLoad");
  auto map = manager.getResource("World", "World");
  require(!manager.isResourceCreated(map) && !manager.isResourceLoaded(map) &&
              !manager.isResourceCreated(map->getDependentResource("World")),
          "Failed atlas post work must destroy Map and World data");
  checkAtlases(manager, false);
  auto objects = manager.getResource("ObjectAtlas");
  require(!manager.isResourceCreated(objects) && !manager.isResourceLoaded(objects) &&
              !manager.isResourceCreated(objects->getDependentResource("Image")),
          "Failed Object atlas upload must destroy partially created atlas data");
  fail = false;
  bool returned = false;
  try { state._update(0.f); }
  catch (wp::application::ReturnFromStateException const&) { returned = true; }
  require(returned && resources.getMap(), "MapLoad must permit retry after post-work failure");
  checkAtlases(manager, true);
  resources.unload();
  resources.unload();
  checkAtlases(manager, false);
}

void requireNoMap(applib::StateTransitionData const& data) {
  auto const& map = data.mapData;
  require(map.nextMapName.empty() && !map.prevMap.map && !map.nextMap.map &&
              !map.prevMap.mapRenderer && !map.nextMap.mapRenderer &&
              !map.prevMap.mapCollisionSim && !map.nextMap.mapCollisionSim,
          "Lifecycle must not populate map data");
}

void checkPlayerKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  auto key = [&](KeyEvent event, Key value) { play._injectKeyInput(event, value, {}); };
  auto x = [&](core::AgentId id) { return world.lookupAgent(id).entity->getGlobalPosition().x; };
  world.pauseSimulation();
  auto first = world.getPlayerControlledAgent();
  if (!first) first = world.createAgent("Keyboard player", 0, 0, 1.f);
  auto second = world.createAgent("Other player", 0, 0, 1.5f);
  require(world.setAgentBehaviourAssignment(second, core::PlayerControlledBehaviour, 1, {}) &&
              (world.lookupAgent(first).entity->isPlayerControlled() ||
               world.setAgentBehaviourAssignment(first, core::PlayerControlledBehaviour, 1, {})),
          "Built-in must be assignable without a behaviour registry");
  require(world.resumeSimulation(), "Cannot resume keyboard fixture");
  float start = x(first), other = x(second);
  auto speed = world.lookupAgent(first).entity->getWalkSpeed();
  key(KeyEvent::Pressed, Key::RightArrow); play._update(1.f);
  require(std::abs(x(first) - start - speed) < 0.0001f && x(second) == other,
          "App arrows must move only the first eligible Agent");
  key(KeyEvent::Pressed, Key::LeftArrow); play._update(1.f);
  require(std::abs(x(first) - start - speed) < 0.0001f, "Both arrows must cancel");
  key(KeyEvent::Released, Key::RightArrow); play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Held left must reverse direction");
  key(KeyEvent::Released, Key::LeftArrow); play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Released arrows must stop immediately");
  key(KeyEvent::Pressed, Key::RightArrow);
  play._suspend(true, true, true);
  play._suspend(false, false, false);
  play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Suspending must clear held Player input");
  world.pauseSimulation();
  require(world.clearAgentBehaviourAssignment(first) && world.clearAgentBehaviourAssignment(second),
          "Cannot clear Player assignments");
  require(world.resumeSimulation(), "Cannot resume without Player");
  key(KeyEvent::Pressed, Key::RightArrow); play._update(1.f);
  require(!world.getPlayerControlledAgent() && std::abs(x(first) - start) < 0.0001f,
          "No eligible Agent must disable Player input");
  key(KeyEvent::Released, Key::RightArrow);
}

void checkPlayerDoorKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  core::World fixture("Keyboard Door", 8, 2);
  auto front = fixture.addRoom("Front", 0, 0, 0, 8, 1);
  fixture.addRoom("Back", 1, 0, 0, 8, 1);
  core::World::CreateDoorOptions options;
  options.holdOpenSeconds = 30.f;
  auto result = fixture.addSectorDoor(0, 0, 2, options);
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Keyboard player", front, 0, 2.5f);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}),
          "Cannot assign keyboard Door player");
  auto writer = core::YamlSerializer::toString();
  core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation();
  require(world.deserialize(*reader, data), "Cannot install keyboard Door fixture");
  auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(
      world.getSector(front)->getObject(result.door.index))->getDoor();
  require(world.resumeSimulation(), "Cannot resume keyboard Door fixture");
  auto key = [&](KeyEvent event) { play._injectKeyInput(event, Key::UpArrow, {}); };
  key(KeyEvent::Pressed); play._update(1.f / 60.f);
  require(door->isOpening(), "App Up must open a manual Door");
  key(KeyEvent::Pressed); play._update(3.f);
  key(KeyEvent::Pressed); play._update(.2f);
  require(door->isOpen() && world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0,
          "Holding/repeating Up must not queue crossing");
  key(KeyEvent::Released); key(KeyEvent::Pressed); play._update(.2f);
  require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 1,
          "A fresh Up must cross the open doorway");
  key(KeyEvent::Released); key(KeyEvent::Pressed);
  play._suspend(true, true, true); play._suspend(false, false, false); play._update(.2f);
  require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 1,
          "Suspension must discard a pending Up action");
  key(KeyEvent::Pressed); play._update(.2f);
  require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0,
          "Suspension must clear the held Up latch");
  key(KeyEvent::Released);
}

void checkPlayerPartialDoorKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  core::World fixture("Keyboard partially raised Door", 8, 2);
  auto front = fixture.addRoom("Front", 0, 0, 0, 8, 1);
  auto back = fixture.addRoom("Back", 1, 0, 0, 8, 1);
  core::World::CreateDoorOptions options; options.speedOverride = .05f; options.holdOpenSeconds = 30.f;
  auto created = fixture.addSectorDoor(0, 0, 2, options);
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Player", front, 0, 2.5f);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign partial Door Player");
  auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install partial Door keyboard fixture"); world.resumeSimulation();
  auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(world.getSector(front)->getObject(created.door.index))->getDoor();
  auto key = [&](Key k, bool held) { play._injectKeyInput(held ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
  key(Key::C, true); key(Key::UpArrow, true); play._update(core::World::getFixedTimestep());
  auto actor = world.lookupAgent(id).entity;
  while (door->getOpenPercentage() * door->getSize().y < actor->getStandingHeight() * .3f + .01f)
    play._update(core::World::getFixedTimestep());
  key(Key::RightArrow, true); key(Key::UpArrow, true); play._update(core::World::getFixedTimestep());
  require(!actor->hasActiveLocomotionTask(), "Repeated Up must not cross even after a Crawling gap becomes clear");
  key(Key::UpArrow, false); key(Key::UpArrow, true); play._update(core::World::getFixedTimestep());
  require(actor->hasActiveLocomotionTask() && actor->getPose() == core::Pose::Crawling,
          "Actual fresh Up with C+Right must cross beneath the partially raised leaf");
  key(Key::UpArrow, false); key(Key::RightArrow, false); key(Key::C, false); play._update(.4f);
  require(actor->getSector()->getIndex() == back && door->isOpening(), "Keyboard crossing must complete before full opening");
}

void checkPlayerStairKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  core::World fixture("Keyboard platforms", 8, 2);
  auto bottom = fixture.addRoom("Bottom", 0, 0, 0, 8, 1);
  auto top = fixture.addRoom("Top", 0, 1, 0, 8, 1);
  auto stairs = fixture.addStairwell(1, 0, 2, 2, CORE_SIDE_LEFT);
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Keyboard player", bottom, 0, 3.f);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign stair player");
  auto writer = core::YamlSerializer::toString();
  core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard stair fixture");
  require(world.resumeSimulation(), "Cannot resume stair fixture");
  auto key = [&](Key k, bool held) { play._injectKeyInput(held ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
  auto y = [&] { return world.lookupAgent(id).entity->getGlobalPosition().y; };
  auto at = [&](float expected, char const* message) { require(std::abs(y() - expected) < .0001f, message); };
  key(Key::UpArrow, true); play._update(.2f);
  require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs, "App Up must enter Stairwell");
  key(Key::UpArrow, false); key(Key::RightArrow, true); play._update(5.f); at(0.f, "No selection must stay on platform");
  key(Key::RightArrow, false); key(Key::LeftArrow, true); play._update(5.f);
  key(Key::UpArrow, true); play._update(.2f); at(0.f, "Wrong horizontal departure must not turn automatically");
  require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs, "Departure Up must not exit");
  key(Key::LeftArrow, false); key(Key::RightArrow, true); play._update(5.f); at(.25f, "Up must select first ascent");
  key(Key::RightArrow, false); key(Key::LeftArrow, true); play._update(5.f); at(.75f, "Held Up must select ascent at next departure");
  key(Key::LeftArrow, false); key(Key::RightArrow, true); play._update(5.f); at(1.f, "Must arrive at Level platform");
  key(Key::UpArrow, false); key(Key::RightArrow, false);
  key(Key::LeftArrow, true); play._update(5.f); // Approach junction without selection.
  key(Key::LeftArrow, false); key(Key::RightArrow, true); play._update(.1f); // Walk back out on the platform.
  key(Key::UpArrow, true); play._update(.2f);
  require(world.lookupAgent(id).entity->getSector()->getIndex() == top, "Fresh Up while walking away must still exit the doorway");
  key(Key::UpArrow, false); key(Key::RightArrow, false); play._update(0.f);
  key(Key::UpArrow, true); play._update(.2f); // Re-enter, preserving off-centre X.
  key(Key::UpArrow, false); play._update(0.f);
  key(Key::UpArrow, true); key(Key::LeftArrow, true); play._update(5.f);
  require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs, "Holding Up on approach must not exit prematurely");
  at(1.f, "Selecting unavailable ascent at the top must stop entry");
  key(Key::UpArrow, false);
  key(Key::DownArrow, true); play._update(5.f); at(.75f, "App Down must select descent");
  key(Key::UpArrow, true); key(Key::LeftArrow, false); key(Key::RightArrow, true); play._update(5.f);
  at(.75f, "Both selection keys must stop at flight junction");
  play._suspend(true, true, true); play._suspend(false, false, false);
  key(Key::RightArrow, true); play._update(.2f); at(.75f, "Suspension must clear held Down and Up selection");
  key(Key::RightArrow, false);
}

void checkPlayerStaircaseKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  for (float belt : {0.f, -.1f, .1f})
  for (bool descending : {false, true}) {
    core::World fixture("Keyboard Staircase", 12, 2);
    auto bottom = fixture.addRoom("Bottom", 0, 0, 0, 12, 1);
    auto top = fixture.addCorridor(1, 0, 12);
    auto stairs = fixture.addStaircase(1, 0, 4, {3, CORE_SIDE_RIGHT, belt});
    fixture.finishBuild(); fixture.pauseSimulation();
    auto id = fixture.createAgent("Keyboard player", descending ? top : bottom, 0, descending ? 7.15f : 3.85f);
    require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Staircase player");
    auto writer = core::YamlSerializer::toString();
    core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install Staircase key fixture");
    require(world.resumeSimulation(), "Cannot resume Staircase fixture");
    auto key = [&](Key k, bool held) { play._injectKeyInput(held ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
    auto horizontal = descending ? Key::LeftArrow : Key::RightArrow;
    auto vertical = descending ? Key::DownArrow : Key::UpArrow;
    key(horizontal, true); key(vertical, true); play._update(.6f);
    require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs,
            "Actual Up/Down key input must mount the appropriate Staircase endpoint");
    key(Key::C, true); play._update(.1f);
    auto actor = world.lookupAgent(id).entity;
    require(actor->getPose() == core::Pose::Crawling && actor->getPoseImageTile() == "human-crawling",
            "Actual C+horizontal must crawl visually on Staircases and Escalators in both directions");
    auto stopped = actor->getGlobalPosition();
    key(horizontal, false); key(vertical, false); play._update(.2f);
    require(actor->getPose() == core::Pose::Crouching, "Held C without horizontal input must crouch on steps");
    key(Key::C, false); play._update(0.f);
    require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().distanceTo(stopped) - std::abs(belt) * .2f) < .0001f,
            "Releasing horizontal input must stop on stationary stairs or ride moving Escalator steps");
    key(horizontal, true);
    for (int second = 0; second < 12; ++second) play._update(1.f);
    require(world.lookupAgent(id).entity->getSector()->getIndex() == (descending ? bottom : top),
            "App Staircase movement must automatically dismount without another vertical key press");
    key(horizontal, false); play._update(0.f);
  }
}

void checkPlayerLadderKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  for (bool embedded : {false, true})
  for (bool descending : {false, true}) {
    core::World fixture("Keyboard Ladder", 12, 3);
    auto bottom = fixture.addRoom("Bottom", 0, 0, 0, 12, embedded ? 3 : 1);
    auto top = embedded ? bottom : fixture.addCorridor(2, 0, 12);
    if (embedded) fixture.addSectorWalkway(bottom, 2, 4);
    if (embedded) fixture.addSectorLadder(bottom, 0, 4, {3, false, true});
    else fixture.addLadder(1, 0, 4, {3, false, true});
    fixture.finishBuild(); fixture.pauseSimulation();
    auto id = fixture.createAgent("Keyboard player", descending ? top : bottom,
                                  embedded && descending ? 2 : 0, 4.35f);
    require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Ladder player");
    auto writer = core::YamlSerializer::toString();
    core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install Ladder key fixture");
    require(world.resumeSimulation(), "Cannot resume Ladder fixture");
    auto key = [&](Key k, bool held) { play._injectKeyInput(held ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
    auto vertical = descending ? Key::DownArrow : Key::UpArrow;
    auto opposite = descending ? Key::UpArrow : Key::DownArrow;
    auto position = [&] { return world.lookupAgent(id).entity->getGlobalPosition(); };
    key(Key::RightArrow, true); key(vertical, true); play._update(.6f);
    require(position().y > 0.f && position().y < 2.f, "Actual Up/Down input must mount fixed Ladders");
    key(Key::C, true); play._update(.1f);
    require(world.lookupAgent(id).entity->getPose() == core::Pose::Crawling,
            "Actual C must preserve Crawling while climbing either Ladder type");
    key(Key::RightArrow, false);
    auto before = position(); play._update(.2f);
    require(world.lookupAgent(id).entity->getPose() == core::Pose::Crouching,
            "C without horizontal input must crouch while climbing vertically");
    key(Key::C, false);
    require(std::abs(position().distanceTo(before) - world.lookupAgent(id).entity->getClimbSpeed() * .2f) < .0001f,
            "Climbing must use vertical input without requiring Left/Right");
    key(vertical, false); before = position(); play._update(.2f);
    require(position().distanceTo(before) < .0001f, "Released Up/Down must stop exactly on the Ladder");
    key(vertical, true); key(opposite, true); play._update(.2f);
    require(position().distanceTo(before) < .0001f, "Both actual vertical keys must cancel climbing");
    key(vertical, false); play._update(.1f);
    require((position().y > before.y) == descending, "Opposite actual vertical key must reverse the climb");
    key(opposite, false); key(vertical, true);
    for (int second = 0; second < 12; ++second) play._update(1.f);
    require(world.lookupAgent(id).entity->getSector()->getIndex() == (descending ? bottom : top)
              && std::abs(position().y - (descending ? 0.f : 2.f)) < .0001f,
            "App Ladder arrival must automatically dismount into its supported landing");
    key(vertical, false); play._update(0.f);
  }
}

void checkPlayerPoseKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  core::World fixture("Keyboard Player poses", 12, 2);
  auto room = fixture.addRoom("Room", 0, 0, 0, 12, 1); fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Player", room, 0, 3.f);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign keyboard pose Player");
  auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard pose Player"); world.resumeSimulation();
  auto actor = world.lookupAgent(id).entity;
  auto key = [&](Key k, bool pressed) { play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, k, {}); play._update(core::World::getFixedTimestep()); };
  key(Key::DownArrow, true); require(actor->getPose() == core::Pose::Standing, "Down alone must no longer crouch on a Floor");
  key(Key::DownArrow, false);
  key(Key::C, true); require(actor->getPose() == core::Pose::Crouching, "Actual C must crouch");
  key(Key::RightArrow, true); require(actor->getPose() == core::Pose::Crawling, "Actual C+Right must crawl");
  auto x = actor->getGlobalPosition().x;
  for (int i = 0; i < 10; ++i) key(Key::C, true);
  require(actor->getPose() == core::Pose::Crawling && actor->getGlobalPosition().x > x, "Repeated C must retain crawling motion");
  key(Key::C, false); require(actor->getPose() == core::Pose::Standing, "Releasing C during motion must stand");
  key(Key::C, true); key(Key::RightArrow, false);
  require(actor->getPose() == core::Pose::Crouching, "Stopping horizontal movement must crouch while C remains held");
  x = actor->getGlobalPosition().x; key(Key::LeftArrow, true);
  require(actor->getPose() == core::Pose::Crawling && actor->getGlobalPosition().x < x, "Actual C+Left must crawl");
  key(Key::RightArrow, true); require(actor->getPose() == core::Pose::Crouching, "Opposite horizontal keys must cancel movement and crouch");
  key(Key::LeftArrow, false); key(Key::RightArrow, false); key(Key::C, false);
  require(actor->getPose() == core::Pose::Standing, "Releasing all posture input must restore Standing");
  play._injectKeyInput(KeyEvent::Pressed, Key::LeftShift, wp::application::KeyModifiers::Shift);
  play._injectKeyInput(KeyEvent::Pressed, Key::C, wp::application::KeyModifiers::Shift);
  play._update(core::World::getFixedTimestep());
  require(actor->getPose() == core::Pose::Standing, "Shift must suppress C posture input even without a selector");
  play._injectKeyInput(KeyEvent::Released, Key::C, wp::application::KeyModifiers::Shift);
  play._injectKeyInput(KeyEvent::Released, Key::LeftShift, {});
}

void checkPlayerAccessPanelKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  auto key = [&](bool pressed) { play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, Key::UpArrow, {}); };
  for (bool inFront : {false, true}) {
    core::World fixture("Keyboard Access panel", 12, 2);
    auto room = fixture.addRoom("Host", 0, 0, 0, 8, 1);
    auto other = fixture.addRoom("Other Layer", 1, 0, 0, 8, 1);
    fixture.addAccessPanel(room, 0, 3); fixture.finishBuild(); fixture.pauseSimulation();
    auto id = fixture.createAgent("Player", inFront ? room : other, 0, 3.5f);
    require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign keyboard panel Player");
    auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard Access panel"); world.resumeSimulation();
    std::shared_ptr<const core::AccessPanel> panel;
    for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
      if (auto object = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(world.getSector(room)->getObject(i))) panel = object->getPanel();
    require(bool(panel), "Missing keyboard Access panel");
    auto step = [&] { play._update(core::World::getFixedTimestep()); };
    key(true); step(); for (int i = 0; i < 10; ++i) { key(true); step(); }
    for (int i = 0; i < 70; ++i) step(); key(false);
    require(panel->getState() == (inFront ? core::AccessPanel::State::Open : core::AccessPanel::State::Closed),
      "Actual Up must open panel only in its owning Location; held/repeated input must not close it");
    size_t requests = 0;
    for (auto const& event : world.getSimulationEvents())
      if (event.type == core::SimulationEventType::InteractionRequestAdded && event.interactionRequest.actor == id) ++requests;
    require(requests == (inFront ? 1u : 0u), "Actual repeated Up must issue exactly one eligible panel action");
    key(true); step(); key(false); for (int i = 0; i < 70; ++i) step();
    auto actor = world.lookupAgent(id).entity;
    require(panel->getState() == core::AccessPanel::State::Closed && actor->getSector()->getIndex() == (inFront ? room : other)
      && std::abs(actor->getGlobalPosition().x - 3.5f) < .001f, "Fresh Up must close panel without auto-approach or alignment");
  }
}

void checkPlayerBoothWindowKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  auto key = [&](bool pressed) { play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, Key::UpArrow, {}); };
  for (bool backSide : {false, true}) {
    core::World fixture("Keyboard BoothWindow", 12, 2);
    auto front = fixture.addRoom("Front", 0, 0, 0, 8, 1);
    auto back = fixture.addRoom("Back", 1, 0, 0, 8, 1);
    fixture.addBoothWindow(0, 0, 3, core::Window::State::Closed);
    fixture.finishBuild(); fixture.pauseSimulation();
    auto id = fixture.createAgent("Player", backSide ? back : front, 0, 3.5f);
    require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign keyboard BoothWindow Player");
    auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard BoothWindow"); world.resumeSimulation();
    std::shared_ptr<const core::BoothWindow> booth;
    for (uint32_t i = 0; i < world.getSector(front)->getNumObjects(); ++i)
      if (auto object = std::dynamic_pointer_cast<const core::WindowSectorObject>(world.getSector(front)->getObject(i)))
        booth = std::dynamic_pointer_cast<const core::BoothWindow>(object->getWindow());
    require(bool(booth), "Missing keyboard BoothWindow");
    auto step = [&] { play._update(core::World::getFixedTimestep()); };
    key(true); step();
    for (int i = 0; i < 10; ++i) { key(true); step(); }
    for (int i = 0; i < 120; ++i) step();
    key(false);
    require(booth->getTargetOpen() == backSide && booth->getProgress() == (backSide ? 1.f : 0.f),
      "Actual Up must open only from back-side panel; repeat must not retoggle");
    size_t requests = 0;
    for (auto const& event : world.getSimulationEvents())
      if (event.type == core::SimulationEventType::InteractionRequestAdded && event.interactionRequest.point == booth->getPanel()) ++requests;
    require(requests == (backSide ? 1u : 0u), "Held/repeated Up must create exactly one eligible back-side operation");
    key(true); step(); key(false); for (int i = 0; i < 120; ++i) step();
    auto actor = world.lookupAgent(id).entity;
    require(!booth->getTargetOpen() && booth->getProgress() == 0.f && actor->getSector()->getIndex() == (backSide ? back : front)
      && std::abs(actor->getGlobalPosition().x - 3.5f) < .001f, "Fresh Up must close without auto-approach or BoothWindow traversal");
  }
}

void checkPlayerChamberKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  auto key = [&](Key k, bool pressed) { play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
  for (auto subtype : {core::ChamberSubtype::SecurityScanner, core::ChamberSubtype::Decontamination})
  for (bool crawl : {false, true}) {
    core::World fixture("Keyboard Chamber", 12, 2);
    auto left = fixture.addRoom("Left", 0, 0, 0, 4, 1);
    auto right = fixture.addRoom("Right", 0, 0, 6, 4, 1);
    auto index = fixture.addChamber(0, 0, 4, 2, true, subtype);
    fixture.finishBuild(); fixture.pauseSimulation();
    require(fixture.setChamberConfiguration(index, .5f, .2f, .3f, .2f), "Cannot configure keyboard Chamber");
    auto id = fixture.createAgent("Player", left, 0, 3.5f);
    require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign keyboard Chamber Player");
    auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard Chamber"); world.resumeSimulation();
    auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world.getSector(index));
    auto actor = world.lookupAgent(id).entity;
    auto step = [&] {
      play._update(core::World::getFixedTimestep());
      require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Keyboard Chamber movement must preserve interlock");
      auto envelope = actor->getPoseEnvelope(actor->getPose()); auto pos = actor->getGlobalPosition();
      for (int side : {0, 1}) {
        core::Vector2 min, max; chamber->getDoor(side)->getCurrentShape(min, max);
        if (pos.x + envelope->x * .5f > min.x + .0001f && pos.x - envelope->x * .5f < max.x - .0001f)
          require(pos.y + envelope->y <= min.y + .0001f, "Keyboard Player's actual pose must not overlap Bulkhead leaf");
      }
    };
    if (crawl) key(Key::C, true);
    key(Key::RightArrow, true);
    for (int i = 0; i < 1800 && actor->getSector()->getIndex() != index; ++i) step();
    key(Key::RightArrow, false);
    require(actor->getSector()->getIndex() == index, "Actual Right must enter sensor-operated Chamber without Up");
    if (crawl) require(actor->getPose() == core::Pose::Crawling, "Actual C+Right must preserve Crawling through Chamber entry");
    auto stopped = actor->getGlobalPosition(); for (int i = 0; i < 60; ++i) step();
    require(actor->getGlobalPosition().distanceTo(stopped) < .001f, "Release must stop Chamber Player without slot alignment");
    if (subtype == core::ChamberSubtype::SecurityScanner) {
      require(chamber->getPhase() == core::SecurityScannerPhase::Positioning, "Scanner must wait for explicit scan-position approach");
      key(Key::RightArrow, true);
      for (int i = 0; i < 600 && chamber->getPhase() == core::SecurityScannerPhase::Positioning; ++i) step();
      key(Key::RightArrow, false);
    }
    stopped = actor->getGlobalPosition();
    for (int i = 0; i < 4000 && !chamber->getDoor(1)->isOpen(); ++i) step();
    require(chamber->getDoor(1)->isOpen() && actor->getGlobalPosition().distanceTo(stopped) < .001f,
      "Processing must open exit without moving Player");
    key(Key::RightArrow, true);
    for (int i = 0; i < 1800 && actor->getSector()->getIndex() != right; ++i) step();
    key(Key::RightArrow, false);
    require(actor->getSector()->getIndex() == right && world.getSimulationSnapshot().securityScanners.front().occupants.empty(),
      "Actual Right must manually leave both Chamber subtypes");
    if (crawl) {
      require(actor->getPose() == core::Pose::Crawling, "Actual C+Right must preserve Crawling through Chamber exit");
      key(Key::C, false); step(); require(actor->getPose() == core::Pose::Standing, "Actual C release must safely restore Standing after exit");
    }
  }
}

void checkPlayerAirlockKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  auto key = [&](Key k, bool pressed) { play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
  core::World fixture("Keyboard Airlock", 12, 2);
  auto left = fixture.addRoom("Left", 0, 0, 0, 4, 1);
  auto right = fixture.addRoom("Right", 0, 0, 6, 4, 1);
  auto index = fixture.addAirlock(0, 0, 4, 2, 1.f);
  auto original = std::dynamic_pointer_cast<const core::AirlockTransit>(fixture.getSector(index));
  float x = fixture.lookupInteractionPoint(original->getControl(0)).entity->getPosition().x;
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Player", left, 0, x);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Airlock keyboard Player");
  auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard Airlock"); world.resumeSimulation();
  auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
  auto actor = world.lookupAgent(id).entity;
  auto step = [&] {
    play._update(core::World::getFixedTimestep());
    require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Keyboard input must preserve Airlock interlock");
  };
  key(Key::UpArrow, true); step(); key(Key::UpArrow, false);
  for (int i = 0; i < 1200 && !chamber->getDoor(0)->isOpen(); ++i) step();
  require(actor->getSector()->getIndex() == left && std::abs(actor->getGlobalPosition().x - x) < .001f, "Calling Airlock must not automatically approach");
  key(Key::RightArrow, true);
  for (int i = 0; i < 600 && actor->getSector()->getIndex() != index; ++i) step();
  key(Key::RightArrow, false);
  require(actor->getSector()->getIndex() == index, "Actual Right must enter called Airlock");
  auto stopped = actor->getGlobalPosition();
  for (int i = 0; i < 4000 && !chamber->getDoor(1)->isOpen(); ++i) step();
  require(chamber->getDoor(1)->isOpen() && actor->getGlobalPosition().distanceTo(stopped) < .001f, "Cycling must retain stopped Player and open opposite exit");
  key(Key::RightArrow, true);
  for (int i = 0; i < 1200 && actor->getSector()->getIndex() != right; ++i) step();
  key(Key::RightArrow, false);
  require(actor->getSector()->getIndex() == right && world.getSimulationSnapshot().airlocks.front().occupants.empty(), "Actual Right must manually exit and release Airlock capacity");
}

void checkPlayerTransportKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key; using wp::application::KeyEvent;
  auto key = [&](Key k, bool pressed, bool shift = false) {
    play._injectKeyInput(pressed ? KeyEvent::Pressed : KeyEvent::Released, k,
      shift ? wp::application::KeyModifiers::Shift : wp::application::KeyModifiers::None);
  };
  core::World fixture("Keyboard Lift", 8, 6);
  auto room = fixture.addRoom("Origin", 0, 0, 0, 8, 1);
  fixture.addRoom("Middle", 0, 2, 0, 8, 1); fixture.addRoom("Top", 0, 4, 0, 8, 1);
  core::World::CreateLiftOptions options; options.cellsWide = 2; options.stopOffsets = {0, 2, 4};
  options.minimumDwellSeconds = 10.f; options.maximumBoardingSeconds = 20.f;
  auto made = fixture.addLift(1, 0, 2, options);
  auto control = made.doors[0].controls[0].interactionPoint;
  float x = fixture.lookupInteractionPoint(control).entity->getPosition().x;
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Player", room, 0, x);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign keyboard Lift Player");
  auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data; data.markSerializedUnmodified = false;
  fixture.serialize(*writer, data); writer->serialize();
  auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
  world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install keyboard Lift"); world.resumeSimulation();
  auto actor = world.lookupAgent(id).entity;
  auto& playState = static_cast<pf::application::StatePlay&>(play);
  require(!playState.isDebugModeEnabled(), "Play debug must default off");
  key(Key::F11, true); key(Key::F11, true);
  require(playState.isDebugModeEnabled() && !playState.getDebugLift(), "F11 repeats must not undo the debug toggle or select an unpressed Lift");
  key(Key::F11, false);
  key(Key::RightArrow, true, true); play._update(.2f);
  require(std::abs(actor->getGlobalPosition().x - x) < .001f, "Shift+cursor must suppress walking outside selector");
  key(Key::RightArrow, false); key(Key::Space, true); key(Key::Space, false);
  key(Key::UpArrow, true); play._update(1.f); key(Key::UpArrow, false);
  require(playState.getDebugLift() == made.traversalResource, "An actual Lift Button press must select that Lift for debug readout");
  require(!world.getSimulationEvents().empty(), "Debug observation must not consume simulation events");
  auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(world.getSector(made.doors[0].door.sector->getIndex())->getObject(made.doors[0].door.index))->getDoor();
  float centre = door->getPosition().x + door->getSize().x * .5f;
  auto direction = x < centre ? Key::RightArrow : Key::LeftArrow;
  key(direction, true);
  for (int i = 0; i < 120 && std::abs(actor->getGlobalPosition().x - centre) > .05f; ++i) play._update(core::World::getFixedTimestep());
  key(direction, false);
  for (int i = 0; i < 120 && !door->isOpen(); ++i) play._update(core::World::getFixedTimestep());
  key(Key::UpArrow, true); play._update(1.f); key(Key::UpArrow, false);
  auto selector = world.getPlayerTransportSelector(); require(selector && selector->focus == 1, "Actual Up must board and expose Lift selector");
  auto position = actor->getGlobalPosition();
  key(Key::UpArrow, true, true); key(Key::UpArrow, true, true); play._update(.1f);
  selector = world.getPlayerTransportSelector(); require(selector && selector->focus == 2, "Fresh Shift+Up must browse only once");
  require(actor->getGlobalPosition().distanceTo(position) < .001f, "Browsing must not move or exit Player");
  key(Key::UpArrow, false, true); key(Key::Space, true); key(Key::Space, true); key(Key::Space, false);
  selector = world.getPlayerTransportSelector();
  require(selector && selector->stops.back().pending, "Actual Space must request focused Stop without closing selector");
  key(Key::DownArrow, true, true); key(Key::DownArrow, false, true);
  require(world.getPlayerTransportSelector()->focus == 1, "Shift+Down must browse in physical order");
  auto readout = pf::application::liftDebugReadout(world, playState.getDebugLift());
  require(readout && readout->lines[1].starts_with("Level: ") && readout->lines[5].starts_with("Dwell min/left: ")
      && readout->lines[7].starts_with("Cutoff left: "), "Debug readout must expose position and both dwell/boarding timers");
  bool sawFractionalLevel = false;
  for (int second = 0; second < 30 && !sawFractionalLevel; ++second) {
    play._update(1.f);
    auto snapshot = world.getSimulationSnapshot();
    for (auto const& resource : snapshot.traversalResources)
      if (resource.id == made.traversalResource && resource.liftMoving) {
        auto before = pf::application::liftDebugReadout(world, playState.getDebugLift());
        play._update(core::World::getFixedTimestep());
        auto after = pf::application::liftDebugReadout(world, playState.getDebugLift());
        require(before && after && before->lines[1] != after->lines[1], "Debug must show live fractional position, not the pressed Stop or an integer Level");
        sawFractionalLevel = true;
        break;
      }
  }
  require(sawFractionalLevel, "Debug fixture must exercise a moving Lift");
  key(Key::F11, true); key(Key::F11, false);
  require(!playState.isDebugModeEnabled() && !playState.getDebugLift(), "A fresh F11 must disable and clear debug overlays");
}

void checkPlayerButtonKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  auto install = [&](core::World& fixture) {
    auto writer = core::YamlSerializer::toString();
    core::SerializationWorkData data; data.markSerializedUnmodified = false;
    fixture.serialize(*writer, data); writer->serialize();
    auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
    world.pauseSimulation(); require(world.deserialize(*reader, data), "Cannot install Button keyboard fixture");
    require(world.resumeSimulation(), "Cannot resume Button fixture");
  };
  auto key = [&](Key k, bool held) { play._injectKeyInput(held ? KeyEvent::Pressed : KeyEvent::Released, k, {}); };
  auto pressCount = [&] {
    size_t count = 0;
    for (auto const& event : world.consumeSimulationEvents())
      if (event.type == core::SimulationEventType::InteractionRequestAdded) ++count;
    return count;
  };
  core::World fixture("Keyboard Button", 12, 2);
  auto room = fixture.addRoom("Room", 0, 0, 0, 12, 1);
  auto control = fixture.addSectorLightSwitch(room, 4);
  auto startX = fixture.lookupInteractionPoint(control.interactionPoint).entity->getPosition().x - .125f;
  fixture.finishBuild(); fixture.pauseSimulation();
  auto id = fixture.createAgent("Keyboard player", room, 0, startX);
  require(fixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Button player");
  install(fixture); world.consumeSimulationEvents();
  key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 1, "Actual Up must press a reachable Button");
  require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().x - startX) < .0001f, "Button key must not move the Player");
  key(Key::UpArrow, true); play._update(1.f);
  require(pressCount() == 0, "Held Up and keyboard repeat must not repeat a Button action");
  key(Key::UpArrow, false); key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 1, "A fresh Up press must allow another Button action");
  key(Key::UpArrow, false); play._update(1.f);
  world.pauseSimulation(); world.setAgentIndividualObjectUsageDistance(id, .0625f); world.resumeSimulation();
  key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 0, "Actual Up must honor effective arm length");
  key(Key::UpArrow, false);

  core::World ladderFixture("Keyboard extensible Ladder", 12, 2);
  auto bottom = ladderFixture.addRoom("Bottom", 0, 0, 0, 12, 1);
  auto top = ladderFixture.addCorridor(1, 0, 12);
  auto result = ladderFixture.addLadder(1, 0, 4, {2, true, false});
  auto buttonX = ladderFixture.lookupInteractionPoint(result.controls[0].interactionPoint).entity->getPosition().x;
  ladderFixture.finishBuild(); ladderFixture.pauseSimulation();
  id = ladderFixture.createAgent("Keyboard player", bottom, 0, buttonX);
  require(ladderFixture.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign extensible Ladder player");
  install(ladderFixture); world.consumeSimulationEvents();
  key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 1, "Up must activate the extensible Ladder Button");
  for (int second = 0; second < 8; ++second) play._update(1.f);
  require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().x - buttonX) < .0001f, "Ladder extension must not auto-approach its mount");
  key(Key::UpArrow, false); key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 1, "A second Up must toggle the Ladder toward retraction");
  for (int second = 0; second < 8; ++second) play._update(1.f);
  auto const retracted = world.getSimulationSnapshot().traversalResources.front();
  require(!retracted.extended && !retracted.retractionPending,
    "Actual Up must complete retraction of an unused extensible Ladder");
  key(Key::UpArrow, false); key(Key::UpArrow, true); play._update(.1f);
  require(pressCount() == 1, "A third Up must toggle the Ladder toward extension");
  for (int second = 0; second < 8; ++second) play._update(1.f);
  // Keep Up held while walking from the Button to the contextual mount.
  // A fresh press while still at the Button would correctly retract again.
  auto horizontal = buttonX < 4.5f ? Key::RightArrow : Key::LeftArrow;
  key(horizontal, true);
  for (int second = 0; second < 10; ++second) play._update(1.f);
  require(world.lookupAgent(id).entity->getSector()->getIndex() == top, "Actual Up must climb and dismount a fully extended Ladder");
  key(horizontal, false); key(Key::UpArrow, false); play._update(0.f);
}

// Drive the actual factory-created states through the same exception/resume
// contract as Launcher, without entering their graphics-dependent scenes.
void checkFlow(std::array<wp::application::StateFactory*, 6> const& factories,
               wp::application::resourcesystem::ResourceManager& manager,
               bool exitInPlay = false) {
  std::shared_ptr<pf::application::Map> map;
  std::weak_ptr<void const> worldLifetime;
  std::unique_ptr<wp::application::State> controller(factories[0]->createState());
  require(controller->getName() == "Controller", "Controller must be initial state");
  // Enable update gates without _enter's graphics setup. Leaving every channel
  // unsuspended invokes only AppLib's no-op suspend hook.
  controller->_suspend(false, false, false);

  for (std::size_t index = 1; index < factories.size(); ++index) {
    bool moved = false;
    void* arguments = nullptr;
    try {
      controller->_update(0.0f);
    } catch (wp::application::SuspendAndMoveToStateException const& change) {
      require(change.getNextState() == factories[index]->getType(),
              "Controller selected the wrong next state");
      require(change.getEventsSuspended() && change.getUpdateSuspended() &&
                  change.getRenderSuspended(),
              "Controller must suspend while its child is active");
      require(change.getArguments() != nullptr, "Missing Controller transition data");
      arguments = change.getArguments();
      auto const& data = *static_cast<applib::StateTransitionData*>(arguments);
      requireNoMap(data);
      require(data.userData != nullptr, "Controller must own the resource lifecycle");
      controller->_suspend(true, true, true, change.getArguments());
      moved = true;
    }
    require(moved, "Controller must dispatch each lifecycle state");

    std::unique_ptr<wp::application::State> child(factories[index]->createState());
    require(child->getName() == factories[index]->getType(), "State name mismatch");
    child->_resume(arguments);
    require(!child->_imGuiActive(), "Lifecycle must not display a GUI");
    require(child->getDebuggingText().empty(), "Lifecycle must have no debug text");

    bool returned = false;
    try {
      if (child->getName() == "Play") {
        require(map && manager.isResourceLoaded(map) && map->getWorld(),
                "Map must remain loaded throughout Play");
        checkAtlases(manager, true);
        auto const tick = map->getWorld()->getSimulationTick();
        require(!map->getWorld()->isSimulationPaused() &&
                    map->getWorld()->getSimulationTimeScale() == 1.0,
                "Play must start the simulation at normal speed");
        child->_update(1.0f);
        require(map->getWorld()->getSimulationTick() == tick + 60,
                "Play must advance one second at normal speed");
        checkPlayerKeys(*child, *map->getWorld());
        checkPlayerDoorKeys(*child, *map->getWorld());
        checkPlayerPartialDoorKeys(*child, *map->getWorld());
        checkPlayerStairKeys(*child, *map->getWorld());
        checkPlayerStaircaseKeys(*child, *map->getWorld());
        checkPlayerLadderKeys(*child, *map->getWorld());
        checkPlayerButtonKeys(*child, *map->getWorld());
        checkPlayerTransportKeys(*child, *map->getWorld());
        checkPlayerAirlockKeys(*child, *map->getWorld());
        checkPlayerChamberKeys(*child, *map->getWorld());
        checkPlayerBoothWindowKeys(*child, *map->getWorld());
        checkPlayerAccessPanelKeys(*child, *map->getWorld());
        checkPlayerPoseKeys(*child, *map->getWorld());
        auto const suspendTick = map->getWorld()->getSimulationTick();
        child->_suspend(true, true, true);
        child->_update(1.0f);
        require(map->getWorld()->getSimulationTick() == suspendTick,
                "Suspended Play must not update the World");
        child->_resume(arguments);
        if (exitInPlay) {
          child.reset();
          controller.reset(); // Same ordering as Launcher on an early exit.
          require(worldLifetime.expired() && !map->getWorld() &&
                      !manager.isResourceLoaded(map) && !manager.isResourceCreated(map),
                  "Early Controller destruction must release and destroy the Map");
          checkAtlases(manager, false);
          return;
        }
        child->_injectKeyInput(wp::application::KeyEvent::Released,
                               wp::application::Key::Escape, {});
        child->_injectKeyInput(wp::application::KeyEvent::Pressed,
                               wp::application::Key::Space, {});
        child->_injectKeyInput(wp::application::KeyEvent::Pressed,
                               wp::application::Key::Escape, {});
      } else {
        child->_update(0.0f);
      }
    } catch (wp::application::ReturnFromStateException const& change) {
      require(change.getArguments() != nullptr, "Child must return transition data");
      auto const& data = *static_cast<applib::StateTransitionData*>(change.getArguments());
      require(data.prevStateName == child->getName(), "Wrong returning state identity");
      requireNoMap(data);
      // Resume before destroying the child: its return data belongs to it.
      controller->_resume(change.getArguments());
      returned = true;
    }
    require(returned, "Child must return to Controller");
    if (child->getName() == "Load") {
      map = std::dynamic_pointer_cast<pf::application::Map>(manager.getResource("World", "World"));
      require(map && !map->getWorld(), "Load must scan but not create the Map");
      checkAtlases(manager, false);
    } else if (child->getName() == "MapLoad") {
      require(map->getWorld() && manager.isResourceLoaded(map),
              "MapLoad must create and load the World resource");
      checkAtlases(manager, true);
      require(map->getWorld()->getSimulationTick() == 0,
              "Loading must not advance the simulation");
      // Play must override a paused/non-normal runtime configuration on entry.
      map->getWorld()->pauseSimulation();
      map->getWorld()->setSimulationTimeScale(2.0);
      worldLifetime = map->getWorld()->getLifetimeToken();
    } else if (child->getName() == "MapUnload" || child->getName() == "Unload") {
      auto source = map->getDependentResource("World");
      require(worldLifetime.expired() && !map->getWorld() &&
                  !manager.isResourceLoaded(map) && !manager.isResourceCreated(map) &&
                  !manager.isResourceLoaded(source) && !manager.isResourceCreated(source),
              "Unloading must release both Map and its source data");
      checkAtlases(manager, false);
    }
  }

  bool finished = false;
  try {
    controller->_update(0.0f);
  } catch (wp::application::ReturnFromStateException const&) {
    finished = true;
  }
  require(finished, "Controller must exit after Unload");

  applib::StateTransitionData invalid;
  invalid.prevStateName = "MapTransition";
  bool rejected = false;
  try {
    controller->_resume(&invalid);
  } catch (std::runtime_error const&) {
    rejected = true;
  }
  require(rejected, "There must be no MapTransition flow");
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the repository resource directory");
    require(std::string_view(dllGetName()) == "Promethium Fermide",
            "Application name mismatch");
    require(dllSetArgument("Unknown", "true") != 0,
            "Unknown arguments must be rejected");
    require(!dllGetNextStateFactory(), "No factory before entry");

    constexpr std::array<std::string_view, 6> names{
        "Controller", "Load", "MapLoad", "Play", "MapUnload", "Unload"};
    for (int run = 0; run < 2; ++run) {
      wp::Logger logger;
      wp::application::resourcesystem::ResourceManager manager(nullptr, nullptr, nullptr, &logger);
      dllOnEntry(&logger, &manager, nullptr);
      DirectoryResourceLocation* location = nullptr;
      manager.addResourceLocationFactory("Directory",
          [&logger, &location](std::string const& path, std::string const& definition)
              -> wp::application::resourcesystem::ResourceLocation* {
            location = new DirectoryResourceLocation(&logger, path, definition);
            return location;
          });
      manager.addResourceLocation("Directory", argv[1], "Resources.yaml");
      bool failAtlas = false;
      for (auto name : {"SectorAtlasImage", "ObjectAtlasImage"})
        manager.addResource(std::make_shared<HeadlessAtlasImage>(name, manager, failAtlas, location));
      std::array<wp::application::StateFactory*, 6> factories{};
      for (std::size_t index = 0; index < names.size(); ++index) {
        factories[index] = dllGetNextStateFactory();
        require(factories[index] && factories[index]->getType() == names[index],
                "Unexpected state factory or registration order");
      }
      require(!dllGetNextStateFactory(), "Only six state factories expected");
      require(!dllGetNextStateFactory(), "Enumeration must stay exhausted");
      checkAtlasFailure(manager, failAtlas);
      checkFlow(factories, manager);
      checkFlow(factories, manager); // Recreate destroyed data with the same manager.
      checkFlow(factories, manager, true);
      dllOnExit();
      require(!dllGetNextStateFactory(), "No factory after exit");
      dllOnExit();
    }
    return 0;
  } catch (std::exception const& error) {
    dllOnExit();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
