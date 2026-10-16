#include "willpower/application/resourcesystem/ResourceExceptions.h"

#include "willpower/geometry/MeshHelpers.h"
#include "willpower/geometry/MeshOperations.h"
#include "willpower/geometry/EdgeFilter.h"

#include "Map.h"
#include "MapGeometryObjectAttributes.h"

namespace applib {

using namespace std;
using namespace wp;
using namespace wp::geometry;

Map::Map(string const& name,
         string const& namesp,
         string const& source,
         map<string, string> const& tags,
         application::resourcesystem::ResourceLocation* location,
         float accelGridSize)
    : application::resourcesystem::Resource(name, namesp, "Map", source, tags, location) {
  // Default grid options
  mGridOptions.useGrid = true;
  mGridOptions.cellSize = accelGridSize;
  mGridOptions.paddingPct = 1.0f;
  mGridOptions.strategy = viz::StaticRenderer::GridOptions::CellStrategy::Intersects;
}

Map::~Map() {
}

void Map::create(application::resourcesystem::DataStreamPtr dataPtr, application::resourcesystem::ResourceManager* resourceMgr) {
  parseData(dataPtr);
  parseDefinition(resourceMgr);
}

void Map::destroy() {
  mMesh.reset();
}

void Map::registerMeshCreationFunction(string const& name, MeshCreationFunction function) {
  auto it = mMeshCreationFunctions.insert(make_pair(name, function));
  if (!it.second) {
    throw Exception("Mesh creation function '" + name + "' already registered.");
  }
}

Map::MeshCreationFunction Map::getMeshCreationFunction(string const& name) const {
  auto it = mMeshCreationFunctions.find(name);
  if (it == mMeshCreationFunctions.end()) {
    throw Exception("Mesh creation function '" + name + "' not registered.");
  }

  return it->second;
}

shared_ptr<wp::geometry::Mesh> Map::getMesh() {
  return mMesh;
}

viz::StaticRenderer::GridOptions Map::getGridOptions() const {
  return mGridOptions;
}

void Map::setGridCellSize(float size) {
  mGridOptions.cellSize = size;
}

void Map::setGridCellStrategy(viz::StaticRenderer::GridOptions::CellStrategy strategy) {
  mGridOptions.strategy = strategy;
}

void Map::setGridPaddingPct(float pct) {
  mGridOptions.paddingPct = pct;
}

}  // namespace applib