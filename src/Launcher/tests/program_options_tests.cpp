#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ProgramOptions.h"

namespace {
void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

class TemporaryConfiguration {
public:
  TemporaryConfiguration(
      std::string const& videoExtra = {}, std::string const& inputSection = {},
      std::string const& gameExtra = {})
      : mPath(std::filesystem::temp_directory_path() /
              "promethium-fermide-launcher-program-options-test.yaml") {
    std::ofstream stream(mPath);
    stream << "Configuration:\n"
              "  Video:\n"
              "    Width: 1024\n"
              "    Height: 768\n"
              "    Fullscreen: false\n"
              "    VSync: true\n"
           << videoExtra
           << "  Audio:\n"
              "    Enabled: false\n"
           << inputSection
           << "  Game:\n"
           << gameExtra
           << "    DLL:\n"
              "      path: BooleanWorld.dll\n"
              "    ResourceLocations:\n"
              "      ResourceLocation:\n"
              "        type: Directory\n"
              "        path: resources\n"
              "        definition: Resources.yaml\n"
              "    Debug:\n"
              "      InGame: enabled\n"
              "    Arguments:\n"
              "      Argument:\n"
              "        name: ThreadedLoading\n"
              "        value: false\n";
    if (!stream) {
      throw std::runtime_error("Could not write temporary Launcher configuration.");
    }
  }

  TemporaryConfiguration(TemporaryConfiguration const&) = delete;
  TemporaryConfiguration& operator=(TemporaryConfiguration const&) = delete;

  ~TemporaryConfiguration() { std::filesystem::remove(mPath); }

  ProgramOptions parse() const { return parseProgramOptions(mPath.string()); }

private:
  std::filesystem::path mPath;
};

void requireRejected(
    TemporaryConfiguration const& configuration, std::string const& field) {
  try {
    (void)configuration.parse();
  } catch (std::exception const& error) {
    require(
        std::string(error.what()).find(field) != std::string::npos,
        "The configuration error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error(
      "Launcher configuration accepted unsupported field '" + field + "'.");
}

void validConfigurationParses() {
  auto options = TemporaryConfiguration{}.parse();

  require(options.screenWidth == 1024 && options.screenHeight == 768,
          "Video dimensions did not parse.");
  require(!options.fullScreen && options.vSync,
          "Video flags did not parse.");
  require(options.dll == "BooleanWorld.dll", "DLL path did not parse.");
  require(options.resourceLocations.size() == 1,
          "Resource location did not parse.");
  auto const& resource = options.resourceLocations.front();
  require(resource.type == "Directory" && resource.path == "resources" &&
              resource.definitionFile == "Resources.yaml",
          "Resource location fields did not parse.");
  require(options.debugging.inGame, "Debug option did not parse.");
  require(options.arguments.size() == 1 &&
              options.arguments.at("ThreadedLoading") == "false",
          "Application argument did not parse.");
}

void inputOptionsParse() {
  require(TemporaryConfiguration{}.parse().input.mouseSensitivity == 0.3f,
          "A missing Input section did not use the default mouse sensitivity.");
  require(TemporaryConfiguration({}, "  Input:\n").parse()
                  .input.mouseSensitivity == 0.3f,
          "An empty Input section did not use the default mouse sensitivity.");
  require(TemporaryConfiguration(
              {}, "  Input:\n    MouseSensitivity: 2.5\n")
                  .parse()
                  .input.mouseSensitivity == 2.5f,
          "MouseSensitivity did not parse.");
}

void invalidInputOptionsAreRejected() {
  for (auto const& value : {"0", "-1.5", "fast", "nan", "inf"}) {
    requireRejected(
        TemporaryConfiguration(
            {}, "  Input:\n    MouseSensitivity: " + std::string(value) + "\n"),
        "MouseSensitivity");
  }
  requireRejected(
      TemporaryConfiguration({}, "  Input:\n    MouseSensitivty: 2.0\n"),
      "MouseSensitivty");
}

void removedOptionsAreRejected() {
  requireRejected(
      TemporaryConfiguration("    RenderScale: half\n"), "RenderScale");
  requireRejected(
      TemporaryConfiguration({}, {},
                             "    WorldDataGeneration:\n"
                             "      Mode: Asynchronous\n"),
      "WorldDataGeneration");
  requireRejected(
      TemporaryConfiguration({}, {}, "    GameResource: retired\n"),
      "GameResource");
}
}  // namespace

int main() {
  try {
    validConfigurationParses();
    inputOptionsParse();
    invalidInputOptionsAreRejected();
    removedOptionsAreRejected();

    std::cout << "Program-options schema validation passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
