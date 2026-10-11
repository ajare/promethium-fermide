#pragma once

#include <willpower/viz/DynamicQuadRenderer.h>

#include "EntityFacade.h"
#include "EntityFacadeFactory.h"
#include "EntityFacadeRenderOptions.h"
#include "VisualSpriteDataProvider.h"
#include "AnimationDatabase.h"

namespace applib {

struct VisualSpriteEntityFacadeRenderOptions : EntityFacadeRenderOptions {
  wp::viz::RotationOptions rotationType;
};

class APPLIB_API VisualSpriteEntityFacade : public EntityFacade {
public:
  typedef std::function<std::shared_ptr<VisualSpriteDataProvider>(EntityFacade*)> DataProviderFactory;

private:
  DataProviderFactory mProviderFactory;

  wp::viz::DynamicQuadRenderer<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat>* mEntityRenderer;

  std::shared_ptr<VisualSpriteDataProvider> mRenderDataProvider;

  wp::application::resourcesystem::ResourcePtr mTexture;

  std::shared_ptr<AnimationDatabase> mAnimDatabase;

protected:
  void createEntityRenderer(
      mpp::ScenePtr scene,
      mpp::RenderSystem* renderSystem,
      mpp::ResourceManager* renderResourceMgr,
      EntityFacadeRenderOptions const* options,
      int renderOrder) override;

public:
  VisualSpriteEntityFacade(DataProviderFactory providerFactory, wp::application::resourcesystem::ResourcePtr texture, std::shared_ptr<AnimationDatabase> animDatabase, size_t initialSize);

  ~VisualSpriteEntityFacade();

  void updateRenderer(wp::BoundingBox const& viewBounds, float frameTime) override;

  void setRenderersVisible(bool visible) override;
};

class VisualSpriteEntityFacadeFactory : public EntityFacadeFactory {
  wp::application::resourcesystem::ResourcePtr mTexture;

  std::shared_ptr<AnimationDatabase> mAnimDatabase;

  VisualSpriteEntityFacade::DataProviderFactory mProviderFactory;

public:
  VisualSpriteEntityFacadeFactory(wp::application::resourcesystem::ResourcePtr texture, std::shared_ptr<AnimationDatabase> animDatabase)
      : EntityFacadeFactory(), mTexture(texture), mAnimDatabase(animDatabase) {
    mProviderFactory = [animDatabase](auto facade) {
      return std::make_shared<VisualSpriteDataProvider>(facade, animDatabase);
    };
  }

  EntityFacade* create(size_t initialSize) override {
    return new VisualSpriteEntityFacade(mProviderFactory, mTexture, mAnimDatabase, initialSize);
  }
};

}  // namespace applib
