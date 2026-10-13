#pragma once

#include <memory>

#include <applib/Map.h>

#include "Application.h"

namespace core {
class World;
}

namespace pf::application {

// Resource identity and geometry belong to AppLib; the simulation aggregate
// remains a World. Constructing this resource does not create or load a World.
class PF_APPLICATION_API Map final : public applib::Map {
  std::unique_ptr<core::World> mWorld;

public:
  Map(std::string const& name, std::string const& namesp,
      std::string const& source, std::map<std::string, std::string> const& tags,
      wp::application::resourcesystem::ResourceLocation* location);
  ~Map() override;

  Map(Map const&) = delete;
  Map& operator=(Map const&) = delete;

  // Borrowed pointers remain valid until replacement, clearing, or destruction.
  core::World* getWorld() noexcept;
  core::World const* getWorld() const noexcept;
  void setWorld(std::unique_ptr<core::World> world) noexcept;
  void clearWorld() noexcept;
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
