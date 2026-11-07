#include "VisualTriMeshEntityFacade.h"
#include "ModelInstance.h"

namespace applib {
using namespace std;
using namespace wp;

VisualTriMeshEntityFacade::VisualTriMeshEntityFacade(DataProviderFactory providerFactory, wp::application::resourcesystem::ResourcePtr texture, size_t initialSize)
    : EntityFacade(initialSize), mProviderFactory(providerFactory), mEntityRenderer(nullptr), mTexture(texture) {
}

VisualTriMeshEntityFacade::~VisualTriMeshEntityFacade() {
  delete mEntityRenderer;
}

void VisualTriMeshEntityFacade::createEntityRenderer(
    mpp::ScenePtr scene,
    mpp::RenderSystem* renderSystem,
    mpp::ResourceManager* renderResourceMgr,
    EntityFacadeRenderOptions const* options,
    int renderOrder) {
  mRenderDataProvider = mProviderFactory(this);

  viz::TriangleRendererOptions entityRenderOptions{
      false,
      false,
      false,
      false,
      false,
      mTexture->getMppResource()};
  mEntityRenderer = new viz::DynamicTriangleRenderer<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat>(
      "Entities",
      entityRenderOptions,
      mRenderDataProvider,
      renderResourceMgr);

  mEntityRenderer->build(renderSystem, renderResourceMgr);
  mEntityRenderer->addToScene(scene, renderOrder);
}

void VisualTriMeshEntityFacade::updateRenderer(BoundingBox const& viewBounds, float frameTime) {
  mRenderDataProvider->update(frameTime);
  mEntityRenderer->update(viewBounds, frameTime);
}

void VisualTriMeshEntityFacade::setRenderersVisible(bool visible) {
  mEntityRenderer->setVisible(visible);
}

}  // namespace applib