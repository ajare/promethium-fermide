#include <filesystem>
#include <iostream>

#include <willpower/application/resourcesystem/ResourceManifestDocument.h>
#include <willpower/application/resourcesystem/ResourceSchemaCatalog.h>

namespace resources = wp::application::resourcesystem;

int main(int argc, char const* const* argv) {
  if (argc != 3) {
    std::cerr << "Usage: resource-schema-validator BUNDLE MANIFEST\n";
    return 2;
  }

  try {
    resources::ResourceSchemaCatalog catalog{std::filesystem::path(argv[1])};
    auto document = resources::ResourceManifestDocument::load(argv[2]);
    auto validation = document.validate(catalog.snapshot());
    if (validation.valid()) {
      std::cout << "Valid Resource Manifest: " << argv[2] << '\n';
      return 0;
    }

    for (auto const& diagnostic : validation.diagnostics) {
      std::cerr << "Resource Manifest at "
                << (diagnostic.instancePath.empty() ? "/"
                                                    : diagnostic.instancePath)
                << ": " << diagnostic.message << '\n';
    }
    return 4;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 5;
  }
}
