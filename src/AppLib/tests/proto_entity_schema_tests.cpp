#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "applib/AnimationDatabase.h"
#include "applib/EntityHandler.h"
#include "applib/ProtoEntity.h"
#include "applib/ProtoEntityDefaultDefinitionFactory.h"
#include "willpower/common/DataNode.h"
#include "willpower/common/StructuredData.h"

namespace {
class TestEntityHandler final : public applib::EntityHandler {
  std::string getPrototypeName(int) override { return "Test"; }
  void setupImpl(applib::Entity*) override {}
  void destroyImpl(applib::Entity*) override {}
  bool updateImpl(applib::Entity*, bool, float) override { return true; }
};

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void requireRejected(std::string const& field) {
  StructuredData properties("Properties");
  properties.addEntry(field, StructuredData(field, ""));
  StructuredData definition("Definition");
  definition.addEntry("Properties", properties);
  wp::DataNode node(definition);

  auto handler = std::make_shared<TestEntityHandler>();
  auto animations = std::make_shared<applib::AnimationDatabase>(nullptr);
  applib::ProtoEntity entity("Test", "", "", {}, nullptr, handler, animations);
  applib::ProtoEntityDefaultDefinitionFactory factory;

  try {
    factory.create(&entity, nullptr, &node);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The unknown-field error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error("Prototype properties accepted unsupported field '" + field + "'.");
}
}  // namespace

int main() {
  try {
    requireRejected("BeamEmitter");
    requireRejected("MisspelledProperty");
    std::cout << "Prototype-property schema validation passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
