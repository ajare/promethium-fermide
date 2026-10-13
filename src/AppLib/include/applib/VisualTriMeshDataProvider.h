#pragma once

#include <mpp/helper/TriangleBatchDataProvider.h>

#include "Platform.h"

namespace applib {

class EntityFacade;

class VisualTriMeshDataProvider : public mpp::helper::TriangleBatch2DDataProvider<mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat, mpp::mesh::DataTypeFloat> {
protected:
  EntityFacade* mwFacade;

public:
  explicit VisualTriMeshDataProvider(EntityFacade* facade)
      : mwFacade(facade) {
    setNumPrimitives(0);
  }

  void getBounds(glm::vec3& bMin, glm::vec3& bMax) override {
    bMin.x = bMin.y = bMin.z = -1e10f;
    bMax.x = bMax.y = bMax.z = 1e10f;
  }

  virtual bool update(float frameTime) {
    VAR_UNUSED(frameTime);

    setNumPrimitives(0);
    return true;
  }
};

};  // namespace applib
