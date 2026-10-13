#pragma once

#include <applib/MapResourceDefinitionFactory.h>

#include "Application.h"

namespace pf::application {

class PF_APPLICATION_API MapWorldDefinitionFactory final
    : public applib::MapResourceDefinitionFactory {
public:
  MapWorldDefinitionFactory();
  void create(wp::application::resourcesystem::Resource* resource,
              wp::application::resourcesystem::ResourceManager* resourceMgr,
              wp::DataNode* node) override;
};

}  // namespace pf::application
