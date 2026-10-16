#pragma once

#include <string>
#include <map>

#include "willpower/application/resourcesystem/Resource.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"

#include <entt/entt.hpp>

#include "Platform.h"
#include "Entity.h"
#include "EntityHandler.h"
#include "AnimationDatabase.h"
#include "willpower/common/DataNode.h"

namespace applib {

class APPLIB_API ProtoEntity : public wp::application::resourcesystem::Resource {
  friend class ProtoEntityResourceDefinitionFactory;

private:
  entt::entity mCompSysId;

protected:
  std::shared_ptr<EntityHandler> mEntityHandler;

  std::shared_ptr<AnimationDatabase> mAnimationDatabase;

private:
  virtual void loadExtraDefinitions(wp::DataNode* node, entt::entity protoId);

public:
  ProtoEntity(std::string const& name,
              std::string const& namesp,
              std::string const& source,
              std::map<std::string, std::string> const& tags,
              wp::application::resourcesystem::ResourceLocation* location,
              std::shared_ptr<EntityHandler> entityHandler,
              std::shared_ptr<AnimationDatabase> animDatabase);

  void setComponentSystemId(entt::entity id);

  entt::entity getComponentSystemId() const;
};

}  // namespace applib