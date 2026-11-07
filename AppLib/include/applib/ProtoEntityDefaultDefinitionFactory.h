#pragma once

#include <string>
#include <vector>

#include "Platform.h"
#include "ProtoEntityResourceDefinitionFactory.h"
#include "willpower/common/DataNode.h"

namespace applib {
class APPLIB_API ProtoEntityDefaultDefinitionFactory : public ProtoEntityResourceDefinitionFactory {
  void createProtoEntity(ProtoEntity* entity, wp::application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) override;

protected:
  virtual std::vector<std::string> getExtraPropertyNames() const;

public:
  ProtoEntityDefaultDefinitionFactory();
};

}  // namespace applib
