#pragma once

#include <willpower/viz/DynamicTriangleRenderer.h>

#include "EntityFacade.h"
#include "EntityFacadeFactory.h"
#include "EntityFacadeRenderOptions.h"
#include "VisualTriMeshDataProvider.h"
#include "AnimationDatabase.h"

namespace applib {

struct VisualTrianglesEntityFacadeRenderOptions : EntityFacadeRenderOptions {
};

class APPLIB_API VisualTriMeshEntityFacade : public EntityFacade {
public:
  typedef std::function<std::shared_ptr<VisualTriMeshDataProvider>(EntityFacade*)> DataProviderFactory;

private:
  DataProviderFactory mProviderFactory;

  wp::viz::DynamicTriangleRenderer<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat>* mEntityRenderer;

  std::shared_ptr<VisualTriMeshDataProvider> mRenderDataProvider;

  wp::application::resourcesystem::ResourcePtr mTexture;

protected:
  void createEntityRenderer(
      mpp::ScenePtr scene,
      mpp::RenderSystem* renderSystem,
      mpp::ResourceManager* renderResourceMgr,
      EntityFacadeRenderOptions const* options,
      int renderOrder) override;

public:
  VisualTriMeshEntityFacade(DataProviderFactory providerFactory, wp::application::resourcesystem::ResourcePtr texture, size_t initialSize);

  ~VisualTriMeshEntityFacade();

  void updateRenderer(wp::BoundingBox const& viewBounds, float frameTime) override;

  void setRenderersVisible(bool visible) override;
};

class VisualTriMeshEntityFacadeFactory : public EntityFacadeFactory {
  VisualTriMeshEntityFacade::DataProviderFactory mProviderFactory;

  wp::application::resourcesystem::ResourcePtr mTexture;

public:
  VisualTriMeshEntityFacadeFactory(VisualTriMeshEntityFacade::DataProviderFactory providerFactory, wp::application::resourcesystem::ResourcePtr texture)
      : mProviderFactory(providerFactory), mTexture(texture) {
  }

  EntityFacade* create(size_t initialSize) override {
    return new VisualTriMeshEntityFacade(mProviderFactory, mTexture, initialSize);
  }
};

}  // namespace applib
