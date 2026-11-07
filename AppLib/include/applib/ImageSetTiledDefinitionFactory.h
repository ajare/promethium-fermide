#pragma once

#include "willpower/application/Platform.h"
#include "willpower/application/resourcesystem/ImageSetResourceDefinitionFactory.h"

#include "Platform.h"
#include "willpower/common/DataNode.h"

namespace applib {
class APPLIB_API ImageSetTiledDefinitionFactory : public wp::application::resourcesystem::ImageSetResourceDefinitionFactory {
public:
  ImageSetTiledDefinitionFactory();

  void create(wp::application::resourcesystem::Resource* resource, wp::application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) override;
};

}  // namespace applib
