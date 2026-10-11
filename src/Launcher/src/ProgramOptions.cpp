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

namespace {
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

[[noreturn]] void shadowValueError(
    string const& filename, string const& field, string const& requirement) {
  auto message = "Could not load '" + filename +
                 "'.  Value of /Configuration/Video/Shadows/" + field +
                 " " + requirement + ".";
  throw runtime_error(message.c_str());
}

float parseShadowFloat(
    string const& filename, DataNode* shadows, string const& field,
    float defaultValue) {
  auto node = shadows->getOptionalChild(field);
  if (!node) return defaultValue;
  auto const& text = node->getValue();
  float value{};
  auto [end, error] = from_chars(text.data(), text.data() + text.size(), value);
  if (error != errc{} || end != text.data() + text.size() || !isfinite(value)) {
    shadowValueError(filename, field, "must be a finite number");
  }
  return value;
}

[[noreturn]] void playerTorchValueError(
    string const& filename, string const& field, string const& requirement) {
  auto message = "Could not load '" + filename +
                 "'.  Value of /Configuration/Video/PlayerTorch/" + field +
                 " " + requirement + ".";
  throw runtime_error(message.c_str());
}

void parsePlayerTorchOptions(
    string const& filename, DataNode* playerTorch,
    bw::app::PlayerTorchOptions& options) {
  playerTorch->requireOnlyChildren({"Radius", "Falloff"});

  auto parseFloat = [&](string const& field, float defaultValue) {
    auto node = playerTorch->getOptionalChild(field);
    if (!node) return defaultValue;
    auto const& text = node->getValue();
    float value{};
    auto [end, error] =
        from_chars(text.data(), text.data() + text.size(), value);
    if (error != errc{} || end != text.data() + text.size() ||
        !isfinite(value)) {
      playerTorchValueError(filename, field, "must be a finite number");
    }
    return value;
  };

  options.attenuationRadius =
      parseFloat("Radius", options.attenuationRadius);
  options.attenuationFalloff =
      parseFloat("Falloff", options.attenuationFalloff);
  if (options.attenuationRadius <= 0.0f) {
    playerTorchValueError(filename, "Radius", "must be greater than zero");
  }
  if (options.attenuationFalloff < 0.0f ||
      options.attenuationFalloff > options.attenuationRadius) {
    playerTorchValueError(
        filename, "Falloff", "must be between zero and Radius");
  }
}

void parseWaterReflectionOptions(
    string const& filename, DataNode* waterReflections,
    bw::app::WaterReflectionOptions& options) {
  waterReflections->requireOnlyChildren({"Technique", "PlanarResolution"});

  if (auto node = waterReflections->getOptionalChild("Technique")) {
    auto name = utils::StringUtils::toLower(node->getValue());
    auto technique = bw::app::waterReflectionTechniqueFromName(name);
    if (!technique) {
      auto message = "Could not load '" + filename +
                     "'.  Value of /Configuration/Video/WaterReflections/Technique must be 'screen-space' or 'planar'.";
      throw runtime_error(message.c_str());
    }
    options.technique = *technique;
  }

  if (auto node = waterReflections->getOptionalChild("PlanarResolution")) {
    auto name = utils::StringUtils::toLower(node->getValue());
    auto resolution = bw::app::planarReflectionResolutionFromName(name);
    if (!resolution) {
      auto message = "Could not load '" + filename +
                     "'.  Value of /Configuration/Video/WaterReflections/PlanarResolution must be 'full', 'half' or 'quarter'.";
      throw runtime_error(message.c_str());
    }
    options.planarResolution = *resolution;
  }
}

void parseWorldDataGenerationOptions(
    string const& filename, DataNode* generation,
    bw::app::WorldDataGenerationOptions& options) {
  if (generation->getData().isValue()) {
    // The YAML reader represents an explicitly empty section as an empty
    // scalar. It is the partial configuration case and keeps the defaults.
    if (generation->getValue().empty()) return;
    auto message = "Could not load '" + filename +
                   "'.  /Configuration/Game/WorldDataGeneration must be a section.";
    throw runtime_error(message.c_str());
  }
  generation->requireOnlyChildren(
      {"Mode", "StartInterval", "AlwaysUpdateVertices", "AllowCommitIfVisible"});

  if (auto node = generation->getOptionalChild("Mode")) {
    auto name = utils::StringUtils::toLower(node->getValue());
    auto mode = bw::app::worldDataGenerationModeFromName(name);
    if (!mode) {
      auto message = "Could not load '" + filename +
                     "'.  Value of /Configuration/Game/WorldDataGeneration/Mode must be 'asynchronous' or 'synchronous'.";
      throw runtime_error(message.c_str());
    }
    options.mode = *mode;
  }

  if (auto node = generation->getOptionalChild("StartInterval")) {
    auto const& text = node->getValue();
    float value{};
    auto [end, error] =
        from_chars(text.data(), text.data() + text.size(), value);
    if (error != errc{} || end != text.data() + text.size() ||
        !isfinite(value) || value < 0.0f) {
      auto message = "Could not load '" + filename +
                     "'.  Value of /Configuration/Game/WorldDataGeneration/StartInterval must be a finite non-negative number.";
      throw runtime_error(message.c_str());
    }
    options.startInterval = value;
  }

  auto parseBool = [&](char const* field, bool& option) {
    auto boolNode = generation->getOptionalChild(field);
    if (!boolNode) return;
    auto value = utils::StringUtils::toLower(boolNode->getValue());
    if (value != "true" && value != "false") {
      auto message = "Could not load '" + filename + "'.  Value of /Configuration/Game/WorldDataGeneration/" +
                     field + " must be 'true' or 'false'.";
      throw runtime_error(message.c_str());
    }
    option = value == "true";
  };
  parseBool("AlwaysUpdateVertices", options.alwaysUpdateVertices);
  parseBool("AllowCommitIfVisible", options.allowCommitIfVisible);
}

[[noreturn]] void audioValueError(
    string const& filename, string const& field, string const& requirement) {
  throw runtime_error(
      "Could not load '" + filename +
      "'.  Value of /Configuration/Audio/" + field + " " + requirement + ".");
}

bool parseAudioBool(
    string const& filename, DataNode* parent, char const* field,
    bool defaultValue) {
  auto node = parent->getOptionalChild(field);
  if (!node) return defaultValue;
  auto value = utils::StringUtils::toLower(node->getValue());
  if (value != "true" && value != "false") {
    audioValueError(filename, field, "must be 'true' or 'false'");
  }
  return value == "true";
}

template <class Value>
Value parsePositiveAudioNumber(
    string const& filename, DataNode* parent, char const* field,
    string const& presetName) {
  auto const& text = parent->getChild(field)->getValue();
  Value value{};
  auto [end, error] = from_chars(text.data(), text.data() + text.size(), value);
  if (error != errc{} || end != text.data() + text.size() ||
      (is_floating_point_v<Value> && !isfinite(value)) || value <= Value{}) {
    audioValueError(
        filename, "QualityPresets/" + presetName + "/" + field,
        "must be a finite number greater than zero");
  }
  return value;
}

void parseAudioSimulationOptions(
    string const& filename, DataNode* audio,
    bw::app::AudioSimulationOptions& options) {
  if (auto quality = audio->getOptionalChild("QualityPreset")) {
    options.qualityPreset = quality->getValue();
    if (options.qualityPreset.empty()) {
      audioValueError(filename, "QualityPreset", "must name a preset");
    }
  }
  options.qualityMayBeModifiedLive = parseAudioBool(
      filename, audio, "QualityMayBeModifiedLive",
      options.qualityMayBeModifiedLive);

  options.features.occlusion = parseAudioBool(
      filename, audio, "Occlusion", options.features.occlusion);
  options.features.transmission = parseAudioBool(
      filename, audio, "Transmission", options.features.transmission);
  options.features.reflections = parseAudioBool(
      filename, audio, "Reflections", options.features.reflections);
  options.features.airAbsorption = parseAudioBool(
      filename, audio, "AirAbsorption", options.features.airAbsorption);
  if (!options.features.occlusion && options.features.transmission) {
    audioValueError(
        filename, "Transmission",
        "cannot be enabled while Occlusion is disabled");
  }

  if (auto presets = audio->getOptionalChild("QualityPresets")) {
    presets->requireOnlyChildren({"Preset"});
    options.presets.clear();
    auto preset = presets->getOptionalChild("Preset");
    while (preset) {
      preset->requireOnlyChildren(
          {"Name", "RayCount", "BounceCount", "ImpulseResponseDuration",
           "AmbisonicOrder", "ReflectionSourceCap", "SimulationUpdateRate"});
      bw::app::AudioQualityPreset value;
      value.name = preset->getChild("Name")->getValue();
      if (value.name.empty()) {
        audioValueError(filename, "QualityPresets/Name", "must not be empty");
      }
      if (ranges::any_of(options.presets, [&](auto const& existing) {
            return existing.name == value.name;
          })) {
        audioValueError(
            filename, "QualityPresets/" + value.name,
            "must have a unique name");
      }
      value.rayCount = parsePositiveAudioNumber<uint32_t>(
          filename, preset, "RayCount", value.name);
      value.bounceCount = parsePositiveAudioNumber<uint32_t>(
          filename, preset, "BounceCount", value.name);
      value.impulseResponseDuration = parsePositiveAudioNumber<float>(
          filename, preset, "ImpulseResponseDuration", value.name);
      value.ambisonicOrder = parsePositiveAudioNumber<uint32_t>(
          filename, preset, "AmbisonicOrder", value.name);
      value.reflectionSourceCap = parsePositiveAudioNumber<uint32_t>(
          filename, preset, "ReflectionSourceCap", value.name);
      value.simulationUpdateRate = parsePositiveAudioNumber<float>(
          filename, preset, "SimulationUpdateRate", value.name);
      options.presets.push_back(move(value));
      if (!preset->next()) break;
    }
    if (options.presets.empty()) {
      audioValueError(filename, "QualityPresets", "must contain a Preset");
    }
  }
  if (!options.findPreset(options.qualityPreset)) {
    audioValueError(
        filename, "QualityPreset",
        "must name one of the configured QualityPresets");
  }
}

void parseShadowOptions(
    string const& filename, DataNode* shadows,
    bw::app::ShadowOptions& options) {
  shadows->requireOnlyChildren(
      {"Enabled", "FaceResolution", "Range", "NearPlane", "ConstantBias",
       "NormalBias", "Filter", "FilterRadius", "FadeStart"});

  if (auto node = shadows->getOptionalChild("Enabled")) {
    auto value = utils::StringUtils::toLower(node->getValue());
    if (value != "true" && value != "false") {
      shadowValueError(filename, "Enabled", "must be 'true' or 'false'");
    }
    options.enabled = value == "true";
  }

  if (auto node = shadows->getOptionalChild("FaceResolution")) {
    auto const& text = node->getValue();
    size_t value{};
    auto [end, error] = from_chars(text.data(), text.data() + text.size(), value);
    if (error != errc{} || end != text.data() + text.size() || value == 0) {
      shadowValueError(filename, "FaceResolution", "must be a positive integer");
    }
    options.faceResolution = value;
  }

  options.range = parseShadowFloat(filename, shadows, "Range", options.range);
  options.nearPlane =
      parseShadowFloat(filename, shadows, "NearPlane", options.nearPlane);
  options.constantBias = parseShadowFloat(
      filename, shadows, "ConstantBias", options.constantBias);
  options.normalBias =
      parseShadowFloat(filename, shadows, "NormalBias", options.normalBias);
  options.filterRadius = parseShadowFloat(
      filename, shadows, "FilterRadius", options.filterRadius);
  options.fadeStart =
      parseShadowFloat(filename, shadows, "FadeStart", options.fadeStart);

  if (options.nearPlane <= 0.0f) {
    shadowValueError(filename, "NearPlane", "must be greater than zero");
  }
  if (options.range <= options.nearPlane) {
    shadowValueError(filename, "Range", "must be greater than NearPlane");
  }
  if (options.constantBias < 0.0f) {
    shadowValueError(filename, "ConstantBias", "must be non-negative");
  }
  if (options.normalBias < 0.0f) {
    shadowValueError(filename, "NormalBias", "must be non-negative");
  }
  if (options.filterRadius < 0.0f) {
    shadowValueError(filename, "FilterRadius", "must be non-negative");
  }
  if (options.fadeStart < 0.0f || options.fadeStart > 1.0f) {
    shadowValueError(filename, "FadeStart", "must be between zero and one");
  }

  if (auto node = shadows->getOptionalChild("Filter")) {
    auto name = utils::StringUtils::toLower(node->getValue());
    auto filter = bw::app::shadowFilterFromName(name);
    if (!filter) {
      shadowValueError(filename, "Filter", "must be 'hard' or 'pcf'");
    }
    options.filter = *filter;
  }
}
}  // namespace

ProgramOptions parseProgramOptions(string const& filename) {
  unique_ptr<utils::YamlReader> reader(utils::YamlReader::fromFile(filename));
  auto configurationData = importStructuredData(reader->readTree());
  DataNode configuration(configurationData);

  ProgramOptions pOpts;

  auto videoNode = configuration.getChild("Video");
  auto gameNode = configuration.getChild("Game");
  auto audioNode = configuration.getChild("Audio");
  auto inputNode = configuration.getOptionalChild("Input");

  videoNode->requireOnlyChildren({"Width", "Height", "Fullscreen", "VSync", "RenderScale", "AA", "AmbientOcclusion", "RenderTextureFilter", "HorizontalMaterials", "WaterReflections", "PlayerTorch", "Shadows"});
  gameNode->requireOnlyChildren(
      {"DLL", "ResourceLocations", "Debug", "Arguments",
       "WorldDataGeneration"});
  audioNode->requireOnlyChildren(
      {"Enabled", "Channels", "Sync", "Output", "QualityPreset",
       "QualityMayBeModifiedLive", "Occlusion", "Transmission",
       "Reflections", "AirAbsorption", "QualityPresets"});

  pOpts.screenWidth = utils::StringUtils::parseInt(videoNode->getChild("Width")->getValue());
  pOpts.screenHeight = utils::StringUtils::parseInt(videoNode->getChild("Height")->getValue());

  string fullScreenStr = videoNode->getChild("Fullscreen")->getValue();
  string vsyncStr = videoNode->getChild("VSync")->getValue();

  pOpts.fullScreen = fullScreenStr == "true" || fullScreenStr == "yes";
  pOpts.vSync = vsyncStr == "true" || fullScreenStr == "yes";

  auto renderScaleNode = videoNode->getOptionalChild("RenderScale");
  if (renderScaleNode) {
    auto renderScaleName = utils::StringUtils::toLower(renderScaleNode->getValue());
    auto renderScale = bw::app::renderScaleFromName(renderScaleName);
    if (!renderScale) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Video/RenderScale must be 'full', 'half', 'quarter' or 'eighth'.";
      throw runtime_error(errMsg.c_str());
    }

    pOpts.video.renderScale = *renderScale;
  }

  auto antiAliasingNode = videoNode->getOptionalChild("AA");
  if (antiAliasingNode) {
    auto antiAliasingName =
        utils::StringUtils::toLower(antiAliasingNode->getValue());
    auto antiAliasing = bw::app::antiAliasingFromName(antiAliasingName);
    if (!antiAliasing) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Video/AA must be 'off', 'msaa-2x', 'msaa-4x', 'msaa-8x' or 'fxaa'.";
      throw runtime_error(errMsg.c_str());
    }

    pOpts.video.antiAliasing = *antiAliasing;
  }

  auto ambientOcclusionNode = videoNode->getOptionalChild("AmbientOcclusion");
  if (ambientOcclusionNode) {
    auto ambientOcclusionName =
        utils::StringUtils::toLower(ambientOcclusionNode->getValue());
    auto ambientOcclusion =
        bw::app::ambientOcclusionFromName(ambientOcclusionName);
    if (!ambientOcclusion) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Video/AmbientOcclusion must be 'ssao', 'gtao-depth', 'gtao-normals' or 'none'.";
      throw runtime_error(errMsg.c_str());
    }

    pOpts.video.ambientOcclusion = *ambientOcclusion;
  }

  auto horizontalMaterialsNode =
      videoNode->getOptionalChild("HorizontalMaterials");
  if (horizontalMaterialsNode) {
    auto materialsName =
        utils::StringUtils::toLower(horizontalMaterialsNode->getValue());
    auto materials = bw::app::horizontalMaterialsFromName(materialsName);
    if (!materials) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Video/HorizontalMaterials must be '2d' or '3d'.";
      throw runtime_error(errMsg.c_str());
    }
    pOpts.video.horizontalMaterials = *materials;
  }

  if (auto waterReflectionsNode =
          videoNode->getOptionalChild("WaterReflections")) {
    parseWaterReflectionOptions(
        filename, waterReflectionsNode, pOpts.video.waterReflections);
  }

  if (auto playerTorchNode = videoNode->getOptionalChild("PlayerTorch")) {
    parsePlayerTorchOptions(
        filename, playerTorchNode, pOpts.video.playerTorch);
  }

  if (auto shadowsNode = videoNode->getOptionalChild("Shadows")) {
    parseShadowOptions(filename, shadowsNode, pOpts.video.shadows);
  }

  auto renderTextureFilterNode =
      videoNode->getOptionalChild("RenderTextureFilter");
  if (renderTextureFilterNode) {
    auto filterName =
        utils::StringUtils::toLower(renderTextureFilterNode->getValue());
    auto filter = bw::app::renderTextureFilterFromName(filterName);
    if (!filter) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Video/RenderTextureFilter must be 'linear' or 'nearest'.";
      throw runtime_error(errMsg.c_str());
    }

    pOpts.video.renderTextureFilter = *filter;
  }

  if (auto generationNode =
          gameNode->getOptionalChild("WorldDataGeneration")) {
    parseWorldDataGenerationOptions(
        filename, generationNode, pOpts.worldDataGeneration);
  }

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

  // Get audio options
  pOpts.audioEnabled = utils::StringUtils::parseBool(audioNode->getChild("Enabled")->getValue());
  pOpts.audio.numChannels = utils::StringUtils::parseInt(audioNode->getChild("Channels")->getValue());
  pOpts.audio.synchronous = utils::StringUtils::parseBool(audioNode->getChild("Sync")->getValue());

  if (auto outputNode = audioNode->getOptionalChild("Output")) {
    auto outputName = utils::StringUtils::toLower(outputNode->getValue());
    auto output = bw::app::audioOutputFromName(outputName);
    if (!output) {
      string errMsg = "Could not load '" + filename + "'.  Value of /Configuration/Audio/Output must be 'Headphones', 'Speakers' or 'Surround'.";
      throw runtime_error(errMsg.c_str());
    }
    pOpts.audioOutput = *output;
  }
  pOpts.audio.speakerMode = bw::app::audioOutputSpeakerMode(pOpts.audioOutput);
  parseAudioSimulationOptions(filename, audioNode, pOpts.audioSimulation);

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
  logger->info(std::format("World render scale: {}", bw::app::renderScaleName(options.video.renderScale)));
  logger->info(std::format(
      "World anti-aliasing: {}",
      bw::app::antiAliasingName(options.video.antiAliasing)));
  logger->info(std::format(
      "World ambient occlusion: {}",
      bw::app::ambientOcclusionName(options.video.ambientOcclusion)));
  logger->info(std::format(
      "World render texture filter: {}",
      bw::app::renderTextureFilterName(options.video.renderTextureFilter)));
  logger->info(std::format(
      "Horizontal materials: {}",
      bw::app::horizontalMaterialsName(options.video.horizontalMaterials)));
  logger->info(std::format(
      "Water reflections: {}, Planar resolution {}",
      bw::app::waterReflectionTechniqueName(
          options.video.waterReflections.technique),
      bw::app::planarReflectionResolutionName(
          options.video.waterReflections.planarResolution)));
  auto const& playerTorch = options.video.playerTorch;
  logger->info(std::format(
      "Player Torch attenuation: radius {}, falloff {}",
      playerTorch.attenuationRadius, playerTorch.attenuationFalloff));
  auto const& shadows = options.video.shadows;
  logger->info(std::format(
      "Player Torch shadows: {}, {}px faces, range {}, near {}, biases {}/{}, {} radius {}, fade {}",
      shadows.enabled ? "enabled" : "disabled", shadows.faceResolution,
      shadows.range, shadows.nearPlane, shadows.constantBias,
      shadows.normalBias, bw::app::shadowFilterName(shadows.filter),
      shadows.filterRadius, shadows.fadeStart));

  logger->info(std::format("Audio enabled: {}", options.audioEnabled));

  if (options.audioEnabled) {
    logger->info(std::format("Audio synchronous: {}", options.audio.synchronous));
    logger->info(std::format("Audio channels: {}", options.audio.numChannels));
    logger->info(std::format("Audio output: {}", bw::app::audioOutputName(options.audioOutput)));
  }

  logger->info(std::format("Mouse sensitivity: {}", options.input.mouseSensitivity));
  logger->info(std::format(
      "WorldData Generation: {}, start interval {} seconds",
      bw::app::worldDataGenerationModeName(options.worldDataGeneration.mode),
      options.worldDataGeneration.startInterval));

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