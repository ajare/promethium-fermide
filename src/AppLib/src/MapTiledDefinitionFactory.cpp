#include <format>
#include <utils/StringUtils.h>

#include "willpower/application/resourcesystem/XmlFileResource.h"
#include "willpower/application/resourcesystem/ImageSetResource.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/common/Exceptions.h"

#include "Map.h"
#include "MapTiledDefinitionFactory.h"
#include "MapGeometryObjectAttributes.h"

namespace applib {
using namespace std;
using namespace utils;
using namespace wp;

MapTiledDefinitionFactory::MapTiledDefinitionFactory()
    : MapResourceDefinitionFactory("Tiled",
                                   new VertexAttributeFactory,
                                   new EdgeAttributeFactory,
                                   new PolygonAttributeFactory,
                                   new PolygonVertexAttributeFactory) {
}

vector<string> MapTiledDefinitionFactory::parseCsv(application::resourcesystem::Resource* resource, string const& data, int width, int height) const {
  auto cells = utils::StringUtils::split(data, "\t\n ,");
  if (cells.size() != width * height) {
    throw application::resourcesystem::ResourceException(resource, "tileset data in layer does not match the specified width/height of the map.");
  }

  return cells;
}

vector<string> MapTiledDefinitionFactory::getLayerCellData(application::resourcesystem::Resource* resource, XmlNode const* layerNode, int idOffset, int* width, int* height) const {
  int mapWidth = utils::StringUtils::parseInt(layerNode->getAttribute("width"));
  int mapHeight = utils::StringUtils::parseInt(layerNode->getAttribute("height"));

  if (mapWidth <= 0 || mapWidth > MaxDimension || mapHeight <= 0 || mapHeight > MaxDimension) {
    throw application::resourcesystem::ResourceException(resource, "invalid map dimensions.");
  }

  if (width) {
    *width = mapWidth;
  }
  if (height) {
    *height = mapHeight;
  }

  auto dataNode = layerNode->getChild("data");
  auto encoding = dataNode->getAttribute("encoding");

  vector<string> cells;
  if (encoding == "csv") {
    cells = parseCsv(resource, dataNode->getValue(), mapWidth, mapHeight);
  } else {
    throw application::resourcesystem::ResourceException(resource, "unsupported layer-data encoding: " + encoding);
  }

  // Awkward stuff to subtract offset
  for (auto& cell : cells) {
    auto id = utils::StringUtils::parseInt(cell);
    id -= idOffset;

    cell = std::format("{}", id);
  }

  return cells;
}

vector<MapTiledDefinitionFactory::Strip> MapTiledDefinitionFactory::generateStrips(vector<string> const& cells, int width, int height, size_t maxDim) const {
  vector<Strip> strips;

  for (int y = 0; y < height; ++y) {
    auto v = cells[y * height];

    Strip strip{
        0, (uint32_t)y, 1, 1, v};

    int x = 1;
    while (x < width) {
      auto nv = cells[y * height + x];

      if (strip.w > maxDim) {
        // Add strip
        strips.push_back(strip);

        strip.x = (uint32_t)x;
        strip.w = 1;
        strip.id = nv;
      } else {
        if (v == nv) {
          strip.w++;
        } else {
          v = nv;

          // Add strip
          strips.push_back(strip);

          strip.x = x;
          strip.w = 1;
          strip.id = v;
        }
      }

      x++;
    }

    // Add strip
    strips.push_back(strip);
  }

  return strips;
}

void MapTiledDefinitionFactory::create(application::resourcesystem::Resource* resource, application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  auto mapRes = static_cast<Map*>(resource);

  // Create mesh
  createMesh(mapRes);

  // Load data from tiled file
  auto fileRes = static_cast<application::resourcesystem::XmlFileResource*>(mapRes->getDependentResource("File").get());
  auto imageSetRes = static_cast<application::resourcesystem::ImageSetResource*>(mapRes->getDependentResource("TilesImageSet").get());
  auto imageRes = static_cast<application::resourcesystem::ImageResource*>(imageSetRes->getDependentResource("Image").get());

  auto mapNode = fileRes->getReader()->getNode("map");

  // Verify the specified tile is our dependent resource
  auto tileSetNode = mapNode->getChild("tileset");
  auto tileSetSource = tileSetNode->getAttribute("source");
  auto tileSpecRes = imageSetRes->getDependentResource("TileSpecs");

  if (tileSetSource != tileSpecRes->getSource()) {
    throw application::resourcesystem::ResourceException(resource, "tileset specified in Tiled file is not listed as a dependent resource.");
  }

  int idOffset = utils::StringUtils::parseInt(tileSetNode->getAttribute("firstgid"));

  // Get size info
  int mapWidth = utils::StringUtils::parseInt(mapNode->getAttribute("width"));
  int mapHeight = utils::StringUtils::parseInt(mapNode->getAttribute("height"));
  float tileWidth = utils::StringUtils::parseFloat(mapNode->getAttribute("tilewidth"));
  float tileHeight = utils::StringUtils::parseFloat(mapNode->getAttribute("tileheight"));

  // Read layers.  For now just read the first one; spec to come later.
  auto baseLayerNode = mapNode->getChild("layer");

  int layerWidth, layerHeight;
  auto cells = getLayerCellData(resource, baseLayerNode, idOffset, &layerWidth, &layerHeight);
  auto strips = generateStrips(cells, layerWidth, layerHeight, 16);

  //
  // Create mesh
  //
  auto mesh = mapRes->getMesh();

  // Vertices
  auto baseVertexIndex = mesh->getNumVertices();
  for (int y = 0; y <= layerHeight; ++y) {
    for (int x = 0; x <= layerWidth; ++x) {
      mesh->addVertex(geometry::Vertex(x * tileWidth, y * tileHeight));
    }
  }

  // Horizontal edges
  auto baseEdgeIndex = mesh->getNumEdges();
  for (int y = 0; y <= layerHeight; ++y) {
    for (int x = 0; x < layerWidth; ++x) {
      auto v0 = (layerWidth + 1) * y + x;
      auto v1 = (layerWidth + 1) * y + x + 1;
      mesh->addEdge(geometry::Edge(baseVertexIndex + v0, baseVertexIndex + v1));
    }
  }

  // Vertical edges
  for (int x = 0; x <= layerWidth; ++x) {
    for (int y = 0; y < layerHeight; ++y) {
      auto v0 = (layerWidth + 1) * y + x;
      auto v1 = (layerWidth + 1) * (y + 1) + x;
      mesh->addEdge(geometry::Edge(baseVertexIndex + v0, baseVertexIndex + v1));
    }
  }

  // Polygons
  auto basePolygonIndex = mesh->getNumPolygons();
  auto numHorzEdges = layerWidth * (layerHeight + 1);
  for (int y = 0; y < layerHeight; ++y) {
    for (int x = 0; x < layerWidth; ++x) {
      uint32_t e0 = baseEdgeIndex + layerWidth * y + x;
      uint32_t e1 = baseEdgeIndex + numHorzEdges + layerHeight * (x + 1) + y;
      uint32_t e2 = baseEdgeIndex + layerWidth * (y + 1) + x;
      uint32_t e3 = baseEdgeIndex + numHorzEdges + layerHeight * x + y;

      auto pv0 = (uint32_t)mesh->getEdge(e0).getFirstVertex();
      auto pv1 = (uint32_t)mesh->getEdge(e0).getSecondVertex();
      auto pv2 = (uint32_t)mesh->getEdge(e2).getSecondVertex();
      auto pv3 = (uint32_t)mesh->getEdge(e2).getFirstVertex();

      vector<uint32_t> edgeData{
          pv0, pv1, e0,
          pv1, pv2, e1,
          pv2, pv3, e2,
          pv3, pv0, e3};

      auto polygonIndex = mesh->addPolygon(geometry::Polygon(edgeData));
    }
  }

  // Strips
  for (auto const& strip : strips) {
    uint32_t y = layerHeight - strip.y - 1;
    for (uint32_t x = strip.x; x < strip.x + strip.w; ++x) {
      auto polygonIndex = basePolygonIndex + y * layerWidth + x;
      auto const& tileDef = imageSetRes->getImageDefinition(strip.id);

      // Add attributes now
      PolygonAttributes polyAttributes{
          1, 1, 1, geometry::UserAttributePolygonColourType::Polygon, {imageRes->getMppResource()->getName()}};

      mesh->setPolygonUserData(polygonIndex, &polyAttributes);

      uint32_t e0 = baseEdgeIndex + layerWidth * y + x;
      uint32_t e2 = baseEdgeIndex + layerWidth * (y + 1) + x;

      auto pv0 = (uint32_t)mesh->getEdge(e0).getFirstVertex();
      auto pv1 = (uint32_t)mesh->getEdge(e0).getSecondVertex();
      auto pv2 = (uint32_t)mesh->getEdge(e2).getSecondVertex();
      auto pv3 = (uint32_t)mesh->getEdge(e2).getFirstVertex();

      PolygonVertexAttributes pva0{tileDef.u[0], tileDef.v[0], 0, 0, 1, 0, 1, 1, 1};
      PolygonVertexAttributes pva1{tileDef.u[1], tileDef.v[0], 0, 0, 1, 0, 1, 1, 1};
      PolygonVertexAttributes pva2{tileDef.u[1], tileDef.v[1], 0, 0, 1, 0, 1, 1, 1};
      PolygonVertexAttributes pva3{tileDef.u[0], tileDef.v[1], 0, 0, 1, 0, 1, 1, 1};

      mesh->setPolygonVertexUserData(polygonIndex, pv0, &pva0);
      mesh->setPolygonVertexUserData(polygonIndex, pv1, &pva1);
      mesh->setPolygonVertexUserData(polygonIndex, pv2, &pva2);
      mesh->setPolygonVertexUserData(polygonIndex, pv3, &pva3);
    }
  }

  // Set grid / render options
  mapRes->setGridCellStrategy(viz::StaticRenderer::GridOptions::CellStrategy::CentreInside);
  mapRes->setGridCellSize(192.0f);
  mapRes->setGridPaddingPct(0.0f);
}

}  // namespace applib
