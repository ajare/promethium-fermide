#pragma once

#include <string>

#include "willpower/application/resourcesystem/Resource.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"

#include "willpower/geometry/Mesh.h"

#include "willpower/viz/StaticRenderer.h"

#include "Platform.h"

namespace applib {

class APPLIB_API Map : public wp::application::resourcesystem::Resource {
  friend class MapResourceDefinitionFactory;

public:
  typedef std::function<void(wp::geometry::Mesh*, wp::application::resourcesystem::ResourceManager*)> MeshCreationFunction;

private:
  // Geometry
  std::shared_ptr<wp::geometry::Mesh> mMesh;

  std::map<std::string, MeshCreationFunction> mMeshCreationFunctions;

  // Render options
  wp::viz::StaticRenderer::GridOptions mGridOptions;

private:
  void create(wp::application::resourcesystem::DataStreamPtr dataPtr, wp::application::resourcesystem::ResourceManager* resourceMgr) override;

  void destroy() override;

protected:
  void registerMeshCreationFunction(std::string const& name, MeshCreationFunction function);

public:
  Map(std::string const& name,
      std::string const& namesp,
      std::string const& source,
      std::map<std::string, std::string> const& tags,
      wp::application::resourcesystem::ResourceLocation* location,
      float accelGridSize);

  ~Map();

  std::shared_ptr<wp::geometry::Mesh> getMesh();

  wp::viz::StaticRenderer::GridOptions getGridOptions() const;

  MeshCreationFunction getMeshCreationFunction(std::string const& name) const;

  void setGridCellSize(float size);

  void setGridCellStrategy(wp::viz::StaticRenderer::GridOptions::CellStrategy strategy);

  void setGridPaddingPct(float pct);
};

}  // namespace applib