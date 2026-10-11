#pragma once

#include <vector>

#include <mpp/helper/QuadBatchDataProvider.h>

#include <willpower/application/resourcesystem/Resource.h>

#include "Platform.h"
#include "AnimationDatabase.h"

namespace applib {

class EntityFacade;

class APPLIB_API VisualSpriteDataProvider : public mpp::helper::QuadBatchDataProvider<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat> {
  EntityFacade* mwFacade;

  std::shared_ptr<AnimationDatabase> mAnimationDatabase;

public:
  VisualSpriteDataProvider(EntityFacade* facade, std::shared_ptr<AnimationDatabase> animationDatabase);

  void getBounds(glm::vec3& bMin, glm::vec3& bMax) override;

  void position(uint32_t index, float& x, float& y) override;

  void angle(uint32_t index, float& angle) override;

  void direction(uint32_t index, float& x, float& y) override;

  void textureAtlasTexcoords(uint32_t index, float& u0, float& v0, float& u1, float& v1) override;

  void dimensions(uint32_t index, float& halfWidth, float& halfHeight) override;

  mpp::Colour diffuse() override;

  bool update(float frameTime) override;
};

}  // namespace applib