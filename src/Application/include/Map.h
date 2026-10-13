#pragma once

#include <memory>

#include <applib/Map.h>

#include "Application.h"

namespace core {
class World;
}
class WorldResource;

namespace pf::application {

PF_APPLICATION_API void registerMapResourceTypes(
    wp::application::resourcesystem::ResourceManager& manager);

// Resource identity and geometry belong to AppLib; the simulation aggregate
// remains a World. Constructing this resource does not create or load a World.
class PF_APPLICATION_API Map final : public applib::Map {
  std::shared_ptr<WorldResource> mWorldResource;

public:
  Map(std::string const& name, std::string const& namesp,
      std::string const& source, std::map<std::string, std::string> const& tags,
      wp::application::resourcesystem::ResourceLocation* location);
  ~Map() override;

  Map(Map const&) = delete;
  Map& operator=(Map const&) = delete;

  // The WorldResource owns the World. These borrowed pointers remain valid only
  // while its created data is retained by the resource lifecycle.
  core::World* getWorld() noexcept;
  core::World const* getWorld() const noexcept;
  std::shared_ptr<WorldResource> const& getWorldResource() const noexcept;
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
