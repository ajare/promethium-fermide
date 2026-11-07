#include <stdexcept>
#include <stack>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <type_traits>

#include "utils/StringUtils.h"
#include "utils/YamlReader.h"
#include "willpower/common/DataNode.h"

#include "Platform.h"

#if APP_PLATFORM == APP_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include "ProgramOptions.h"

using namespace std;
using namespace wp;

StructuredData importStructuredData(utils::StructuredData const& source) {
  if (source.isValue()) {
    return StructuredData(source.getName(), source.getValue());
  }
  StructuredData result(source.getName());
  for (auto const& entry : source) {
    result.addEntry(entry.first, importStructuredData(entry.second));
  }
  return result;
}

ProgramOptions parseProgramOptions(string const& filename) {
  unique_ptr<utils::YamlReader> reader(utils::YamlReader::fromFile(filename));
  auto configurationData = importStructuredData(reader->readTree());
  DataNode configuration(configurationData);

  ProgramOptions pOpts;

  auto videoNode = configuration.getChild("Video");
  auto gameNode = configuration.getChild("Game");
  auto audioNode = configuration.getChild("Audio");
  auto inputNode = configuration.getOptionalChild("Input");

  videoNode->requireOnlyChildren({"Width", "Height", "Fullscreen", "VSync"});
  gameNode->requireOnlyChildren(
      { "DLL", "ResourceLocations", "Debug", "Arguments" });

  pOpts.screenWidth = utils::StringUtils::parseInt(videoNode->getChild("Width")->getValue());
  pOpts.screenHeight = utils::StringUtils::parseInt(videoNode->getChild("Height")->getValue());

  string fullScreenStr = videoNode->getChild("Fullscreen")->getValue();
  string vsyncStr = videoNode->getChild("VSync")->getValue();

  pOpts.fullScreen = fullScreenStr == "true" || fullScreenStr == "yes";
  pOpts.vSync = vsyncStr == "true" || fullScreenStr == "yes";

  // Get game DLL
  pOpts.dll = gameNode->getChild("DLL")->getProperty("path");

  // Get game resource locations
  auto resourceLocationNode = gameNode->getChild("ResourceLocations")->getChild("ResourceLocation");
  do {
    ProgramOptions::ResourceLocation rl;

    rl.type = resourceLocationNode->getProperty("type");
    rl.path = resourceLocationNode->getProperty("path");
    rl.definitionFile = resourceLocationNode->getProperty("definition");

    pOpts.resourceLocations.push_back(rl);
  } while (resourceLocationNode->next());

  // Get input options. The whole section is optional; the defaults in
  // ProgramOptions::Input stand in for anything left out.
  if (inputNode) {
    inputNode->requireOnlyChildren({"MouseSensitivity"});

    auto sensitivityNode = inputNode->getOptionalChild("MouseSensitivity");
    if (sensitivityNode) {
      // parseFloat reports unparseable text as zero, which this rejects along
      // with the zero and negative values a user could write deliberately.
      float sensitivity = utils::StringUtils::parseFloat(sensitivityNode->getValue());

      if (!isfinite(sensitivity) || sensitivity <= 0.0f) {
        string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Input/MouseSensitivity must be a number greater than zero.";
        throw runtime_error(errMsg.c_str());
      }

      pOpts.input.mouseSensitivity = sensitivity;
    }
  }

  // Get debug options
  pOpts.debugging.inGame = false;

  auto debugNode = gameNode->getOptionalChild("Debug");
  if (debugNode) {
    auto inGameNode = debugNode->getOptionalChild("InGame");
    if (inGameNode) {
      string enabled = inGameNode->getValue();
      transform(enabled.begin(), enabled.end(), enabled.begin(), ::tolower);

      if (enabled != "enabled" && enabled != "disabled") {
        string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Game/Debug/InGame must be either 'enabled' or 'disabled'.";
        throw runtime_error(errMsg.c_str());
      }

      pOpts.debugging.inGame = enabled == "enabled";
    }
  }

  // Get arguments
  auto argumentsNode = gameNode->getOptionalChild("Arguments");
  if (argumentsNode) {
    auto argumentNode = argumentsNode->getOptionalChild("Argument");
    while (argumentNode) {
      string argumentName = argumentNode->getProperty("name");
      string argumentValue = argumentNode->getProperty("value");

      pOpts.arguments[argumentName] = argumentValue;

      if (!argumentNode->next()) {
        break;
      }
    }
  }

  return pOpts;
}

void logProgramOptions(ProgramOptions const& options, Logger* logger) {
  logger->info("");
  logger->info("Program Options");
  logger->info("---------------");

  logger->info(std::format("Video size: {}x{}", options.screenWidth, options.screenHeight));
  logger->info(std::format("Fullscreen: {}", options.fullScreen));
  logger->info(std::format("VSync enabled: {}", options.vSync));

  logger->info(std::format("DLL: {}", options.dll));
  logger->info("");
  logger->info("Resource locations:");

  for (auto const& location : options.resourceLocations) {
    logger->info("- " + location.type + ": " + location.path);
  }

  logger->info("");
  logger->info("Debugging:");
  logger->info(std::format("- In-game: {}", options.debugging.inGame));
  logger->info("");
}