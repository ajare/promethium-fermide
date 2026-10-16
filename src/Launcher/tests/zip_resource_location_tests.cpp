#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.c"

#include "willpower/application/resourcesystem/DataStream.h"
#include "willpower/common/Logger.h"

#include "ZipResourceLocation.h"

namespace {
void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

std::filesystem::path createArchive() {
  auto const suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() /
              ("boolean-world-zip-resource-location-" + std::to_string(suffix) + ".zip");

  std::string const manifest =
      "Resources:\n"
      "  Resource:\n"
      "    - type: TextFile\n"
      "      name: Greeting\n"
      "      location: text\\greeting.txt\n"
      "    - type: Image\n"
      "      name: Pixel\n"
      "      location: images\\pixel.png\n";
  std::string const greeting = "Hello from a Zip resource.\n";
  constexpr std::array<std::uint8_t, 67> pixelPng{
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x01, 0xe5, 0x27, 0xd4, 0xa2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
      0x60, 0x82};

  mz_zip_archive archive{};
  require(mz_zip_writer_init_file(&archive, path.string().c_str(), 0) == MZ_TRUE,
          "Could not create the Zip test fixture.");
  try {
    require(mz_zip_writer_add_mem(&archive, "Resources.yaml", manifest.data(), manifest.size(), MZ_BEST_COMPRESSION) == MZ_TRUE,
            "Could not add the resource manifest to the Zip test fixture.");
    require(mz_zip_writer_add_mem(&archive, "text/greeting.txt", greeting.data(), greeting.size(), MZ_BEST_COMPRESSION) == MZ_TRUE,
            "Could not add text data to the Zip test fixture.");
    require(mz_zip_writer_add_mem(&archive, "images/pixel.png", pixelPng.data(), pixelPng.size(), MZ_BEST_COMPRESSION) == MZ_TRUE,
            "Could not add image data to the Zip test fixture.");
    require(mz_zip_writer_finalize_archive(&archive) == MZ_TRUE,
            "Could not finalize the Zip test fixture.");
  } catch (...) {
    mz_zip_writer_end(&archive);
    std::filesystem::remove(path);
    throw;
  }
  require(mz_zip_writer_end(&archive) == MZ_TRUE, "Could not close the Zip test fixture.");
  return path;
}
}  // namespace

int main() {
  auto archivePath = createArchive();
  try {
    wp::Logger logger;
    {
      ZipResourceLocation location(&logger, archivePath.string(), "Resources.yaml");
      location.scan();
      location.validateResourceDefinitions();

      auto const& records = location.getNamespaceRecords().at("").resourceRecords;
      auto const& greetingRecord = records.at("Greeting");
      wp::application::resourcesystem::DataStream greeting(
          &location, greetingRecord.baseData.location, greetingRecord.namesp);
      greeting.read();
      std::string const greetingText(
          reinterpret_cast<char const*>(greeting.getData()), greeting.getSize());
      require(greetingText == "Hello from a Zip resource.\n", "Text data loaded from the Zip did not match.");

      auto const& pixelRecord = records.at("Pixel");
      wp::application::resourcesystem::DataStream pixel(
          &location, pixelRecord.baseData.location, pixelRecord.namesp);
      pixel.read();
      require(pixel.getSize() == 67, "Image data loaded from the Zip had the wrong size.");
      require(pixel.getData()[0] == 0x89 && pixel.getData()[1] == 'P' && pixel.getData()[2] == 'N' && pixel.getData()[3] == 'G',
              "Image data loaded from the Zip did not have a PNG signature.");

      try {
        std::uint32_t missingSize = 0;
        delete[] location.readData("missing/file.bin", &missingSize);
        throw std::runtime_error("A missing Zip member was read successfully.");
      } catch (std::exception const& error) {
        std::string const message = error.what();
        require(message.find("missing/file.bin") != std::string::npos &&
                    message.find(archivePath.string()) != std::string::npos,
                "The missing-member error did not identify the member and archive.");
      }
    }

    require(std::filesystem::remove(archivePath),
            "The Zip archive remained open after its resource location was destroyed.");
    std::cout << "Zip resource location integration checks passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::filesystem::remove(archivePath);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
