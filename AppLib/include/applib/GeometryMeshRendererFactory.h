#pragma once

#include <willpower/viz/GeometryMeshRenderer.h>

namespace applib {

class GeometryMeshRendererFactory {
public:
  GeometryMeshRendererFactory() {
  }

  virtual wp::viz::GeometryMeshRenderer* create(std::string const& name, std::shared_ptr<wp::geometry::Mesh> mesh, wp::viz::StaticRenderer::GridOptions const& gridOptions, size_t indexWidth, mpp::ResourceManager* renderResourceMgr) = 0;
};

}  // namespace applib