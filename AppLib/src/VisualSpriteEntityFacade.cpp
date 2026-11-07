#include <willpower/application/resourcesystem/AnimationSetResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>

#include "VisualSpriteEntityFacade.h"
#include "ModelInstance.h"

namespace applib {
using namespace std;
using namespace wp;

VisualSpriteEntityFacade::VisualSpriteEntityFacade(DataProviderFactory providerFactory, wp::application::resourcesystem::ResourcePtr texture, std::shared_ptr<AnimationDatabase> animDatabase, size_t initialSize)
    : EntityFacade(initialSize), mProviderFactory(providerFactory), mEntityRenderer(nullptr), mTexture(texture), mAnimDatabase(animDatabase) {
}

VisualSpriteEntityFacade::~VisualSpriteEntityFacade() {
  delete mEntityRenderer;
}

void VisualSpriteEntityFacade::createEntityRenderer(
    mpp::ScenePtr scene,
    mpp::RenderSystem* renderSystem,
    mpp::ResourceManager* renderResourceMgr,
    EntityFacadeRenderOptions const* options,
    int renderOrder) {
  mRenderDataProvider = mProviderFactory(this);

  // Calculate render options.
  // RotationOptions: texcoords is faster, but should only be used for small
  //                  projectile-like objects
  // FixedTexCoords:  always keep this to false, for flexibility
  // MaxSize:         request from resource
  // Texture:         request from resource
  // IndexWidth:      16-bit should be enough

  mpp::ResourcePtr texture{nullptr};
  uint32_t maxWidth, maxHeight;
  auto const& resType = mTexture->getType();

  if (resType == "AnimationSet") {
    auto res = static_cast<application::resourcesystem::AnimationSetResource*>(mTexture.get());
    texture = res->getImage()->getMppResource();
    res->getMaximumDimensions(maxWidth, maxHeight);
  } else if (resType == "ImageSet") {
    auto res = static_cast<application::resourcesystem::ImageSetResource*>(mTexture.get());
    texture = res->getImage()->getMppResource();
    res->getMaximumDimensions(maxWidth, maxHeight);
  } else {
    throw Exception("Unsupported image resource type for Entity rendering: " + resType + ".  Needs to be one of AnimationSet|ImageSet.");
  }

  auto quadOptions = static_cast<VisualSpriteEntityFacadeRenderOptions const*>(options);

  viz::QuadRendererOptions entityRenderOptions{
      quadOptions->rotationType,
      false,
      maxWidth,
      maxHeight,
      texture,
      16};

  mEntityRenderer = new viz::DynamicQuadRenderer<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat>(
      "Entities",
      entityRenderOptions,
      mRenderDataProvider,
      renderResourceMgr);

  mEntityRenderer->build(renderSystem, renderResourceMgr);
  mEntityRenderer->addToScene(scene, renderOrder);
}

void VisualSpriteEntityFacade::updateRenderer(BoundingBox const& viewBounds, float frameTime) {
  mRenderDataProvider->update(frameTime);
  mEntityRenderer->update(viewBounds, frameTime);
}

void VisualSpriteEntityFacade::setRenderersVisible(bool visible) {
  mEntityRenderer->setVisible(visible);
}

}  // namespace applib