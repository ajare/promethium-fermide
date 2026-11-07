#pragma once

#include <memory>

#include <applib/Map.h>

#include "Application.h"

class WorldResource;

namespace game {
class WorldRuntime;
}

namespace pf::application {

PF_APPLICATION_API void registerMapResourceTypes(
    wp::application::resourcesystem::ResourceManager& manager);

// Resource identity and geometry belong to AppLib; the running World belongs
// to the game-facing façade. Constructing this resource does not create or
// load a World.
//
// The Map reaches its World through the `game::WorldRuntime` that owns and
// drives it (#970, spec #960). `getWorld()` is gone: the runtime's const
// view is the only World a Map can name, so nothing holding a Map can mutate
// what it renders.
class PF_APPLICATION_API Map final : public applib::Map {
  std::shared_ptr<WorldResource> mWorldResource;

public:
  Map(std::string const& name, std::string const& namesp,
      std::string const& source, std::map<std::string, std::string> const& tags,
      wp::application::resourcesystem::ResourceLocation* location);
  ~Map() override;

  Map(Map const&) = delete;
  Map& operator=(Map const&) = delete;

  // The runtime that owns this Map's World, read from the resource on every
  // call rather than cached. That is deliberate: the Map must never be the
  // reason a World outlives the resource that owns it, so it holds the
  // resource rather than the runtime. When the resource system destroys the
  // World data - or re-creates it - this follows immediately, and a Map whose
  // World is gone reports no runtime at all.
  //
  // Null means there is nothing loaded. Otherwise the World comes back as the
  // runtime's `core::World const&`, which is as mutable as this Map can get.
  std::shared_ptr<game::WorldRuntime> getRuntime() const noexcept;

  // The WorldResource owns the World; these bind the Map to it. A resource
  // that has not run its create() - and so has no runtime to drive - is
  // refused rather than stored, so a Map that claims to be loaded always has
  // something behind it. Passing null clears the binding.
  void setWorldResource(std::shared_ptr<WorldResource> resource);
  void clearWorldResource() noexcept;
};

class PF_APPLICATION_API MapResourceFactory final
    : public wp::application::resourcesystem::ResourceFactory {
public:
  MapResourceFactory();
  wp::application::resourcesystem::Resource* createResource(
      std::string const& name, std::string const& namesp,
      std::string const& source, std::map<std::string, std::string> const& tags,
      wp::application::resourcesystem::ResourceLocation* location) override;
};

}  // namespace pf::application
