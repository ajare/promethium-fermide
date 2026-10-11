#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ProgramOptions.h"

namespace {
void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

std::filesystem::path writeConfiguration(std::string const& extraGameField,
                                         std::string const& inputSection = "",
                                         std::string const& renderScaleLine = "",
                                         std::string const& antiAliasingLine = "",
                                         std::string const& renderTextureFilterLine = "",
                                         std::string const& ambientOcclusionLine = "",
                                         std::string const& horizontalMaterialsLine = "",
                                         std::string const& shadowsSection = "",
                                         std::string const& playerTorchSection = "",
                                         std::string const& waterReflectionsSection = "",
                                         std::string const& worldDataGenerationSection = "",
                                         std::string const& audioOutputLine = "") {
  auto path = std::filesystem::temp_directory_path() / "boolean-world-program-options-test.yaml";
  std::ofstream stream(path);
  stream << "Configuration:\n"
            "  Video:\n"
            "    Width: 1024\n"
            "    Height: 768\n"
            "    Fullscreen: false\n"
            "    VSync: true\n"
         << renderScaleLine
         << antiAliasingLine
         << renderTextureFilterLine
         << ambientOcclusionLine
         << horizontalMaterialsLine
         << waterReflectionsSection
         << playerTorchSection
         << shadowsSection
         << "  Audio:\n"
            "    Enabled: false\n"
            "    Channels: 32\n"
            "    Sync: false\n"
         << audioOutputLine
         << inputSection
         << "  Game:\n"
         << worldDataGenerationSection
         << "    DLL:\n"
            "      path: BooleanWorld.dll\n"
            "    ResourceLocations:\n"
            "      ResourceLocation:\n"
            "        type: Directory\n"
            "        path: resources\n"
            "        definition: Resources.yaml\n";
  if (!extraGameField.empty()) {
    stream << "    " << extraGameField << ": retired\n";
  }
  return path;
}

void requireRejected(std::string const& field) {
  auto path = writeConfiguration(field);
  try {
    (void)parseProgramOptions(path.string());
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The unknown-field error did not identify '" + field + "'.");
    std::filesystem::remove(path);
    return;
  }
  std::filesystem::remove(path);
  throw std::runtime_error("Game configuration accepted unsupported field '" + field + "'.");
}

ProgramOptions parseWithInput(std::string const& inputSection) {
  auto path = writeConfiguration("", inputSection);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithRenderScale(std::string const& renderScaleLine) {
  auto path = writeConfiguration("", "", renderScaleLine);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithAntiAliasing(std::string const& antiAliasingLine) {
  auto path = writeConfiguration("", "", "", antiAliasingLine);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithRenderTextureFilter(std::string const& filterLine) {
  auto path = writeConfiguration("", "", "", "", filterLine);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithAmbientOcclusion(
    std::string const& ambientOcclusionLine) {
  auto path = writeConfiguration(
      "", "", "", "", "", ambientOcclusionLine);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithHorizontalMaterials(std::string const& line) {
  auto path = writeConfiguration("", "", "", "", "", "", line);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithWaterReflections(std::string const& section) {
  auto path = writeConfiguration(
      "", "", "", "", "", "", "", "", "", section);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithPlayerTorch(std::string const& section) {
  auto path = writeConfiguration("", "", "", "", "", "", "", "", section);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithShadows(std::string const& section) {
  auto path = writeConfiguration("", "", "", "", "", "", "", section);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithWorldDataGeneration(std::string const& section) {
  auto path = writeConfiguration(
      "", "", "", "", "", "", "", "", "", "", section);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

ProgramOptions parseWithAudioOutput(std::string const& audioOutputLine) {
  auto path = writeConfiguration(
      "", "", "", "", "", "", "", "", "", "", "", audioOutputLine);
  try {
    auto options = parseProgramOptions(path.string());
    std::filesystem::remove(path);
    return options;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

void requireAudioOutputRejected(
    std::string const& audioOutputLine, std::string const& description) {
  try {
    (void)parseWithAudioOutput(audioOutputLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("Output") != std::string::npos,
            "The audio-output error did not name the field.");
    return;
  }
  throw std::runtime_error("Audio configuration accepted " + description + ".");
}

void requireAudioFieldRejected(
    std::string const& audioLine, std::string const& field) {
  try {
    (void)parseWithAudioOutput(audioLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The unknown-field error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error("Audio configuration accepted unsupported field '" + field + "'.");
}

void requireWorldDataGenerationRejected(
    std::string const& section, std::string const& field) {
  try {
    (void)parseWithWorldDataGeneration(section);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The World data Generation error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error(
      "Game configuration accepted invalid WorldDataGeneration/" + field + ".");
}

void requireWaterReflectionsRejected(
    std::string const& section, std::string const& field) {
  try {
    (void)parseWithWaterReflections(section);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The Water reflections error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error(
      "Video configuration accepted invalid WaterReflections/" + field + ".");
}

void requirePlayerTorchRejected(
    std::string const& section, std::string const& field) {
  try {
    (void)parseWithPlayerTorch(section);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The Player Torch error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error(
      "Video configuration accepted invalid PlayerTorch/" + field + ".");
}

void requireShadowsRejected(
    std::string const& section, std::string const& field) {
  try {
    (void)parseWithShadows(section);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "The shadow error did not identify '" + field + "'.");
    return;
  }
  throw std::runtime_error("Video configuration accepted invalid Shadows/" + field + ".");
}

void requireInputRejected(std::string const& inputSection, std::string const& description) {
  try {
    (void)parseWithInput(inputSection);
  } catch (std::exception const&) {
    return;
  }
  throw std::runtime_error("Input configuration accepted " + description + ".");
}

void requireRenderScaleRejected(std::string const& renderScaleLine, std::string const& description) {
  try {
    (void)parseWithRenderScale(renderScaleLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("RenderScale") != std::string::npos,
            "The render-scale error did not name the field.");
    return;
  }
  throw std::runtime_error("Video configuration accepted " + description + ".");
}

void requireAntiAliasingRejected(
    std::string const& antiAliasingLine,
    std::string const& description) {
  try {
    (void)parseWithAntiAliasing(antiAliasingLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("AA") != std::string::npos,
            "The anti-aliasing error did not name the field.");
    return;
  }
  throw std::runtime_error("Video configuration accepted " + description + ".");
}

void requireRenderTextureFilterRejected(
    std::string const& filterLine,
    std::string const& description) {
  try {
    (void)parseWithRenderTextureFilter(filterLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("RenderTextureFilter") != std::string::npos,
            "The render-texture filter error did not name the field.");
    return;
  }
  throw std::runtime_error("Video configuration accepted " + description + ".");
}

void requireHorizontalMaterialsRejected(
    std::string const& line, std::string const& description) {
  try {
    (void)parseWithHorizontalMaterials(line);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("HorizontalMaterials") !=
                std::string::npos,
            "The horizontal-material error did not name the field.");
    return;
  }
  throw std::runtime_error(
      "Video configuration accepted " + description + ".");
}

void checkedInConfigurationsDeclareAcceptedFiveSecondInterval() {
  auto const root = std::filesystem::path(BW_LAUNCHER_SOURCE_DIR);
  for (auto const& relative : std::vector<std::filesystem::path>{
           "resources/Shipping/Game.yaml",
           "support/ASTRALEMPRESS/Debug/Game.yaml",
           "support/ASTRALEMPRESS/MemCheck/Game.yaml",
           "support/ASTRALEMPRESS/Release/Game.yaml",
           "support/ygernasaturna/Release/Game.yaml"}) {
    auto options = parseProgramOptions((root / relative).string());
    require(options.worldDataGeneration.mode ==
                    bw::app::WorldDataGenerationMode::Asynchronous &&
                options.worldDataGeneration.startInterval == 5.0f,
            "Checked-in configuration does not declare asynchronous Generation with a five-second interval: " +
                relative.string());
    auto const& audio = options.audioSimulation;
    require(audio.presets.size() == 3 && audio.qualityPreset == "Medium" &&
                audio.presets[0].rayCount == 2048 &&
                audio.presets[1].bounceCount == 4 &&
                audio.presets[2].impulseResponseDuration == 2.0f &&
                audio.presets[2].ambisonicOrder == 2 &&
                audio.presets[2].reflectionSourceCap == 8 &&
                audio.presets[2].simulationUpdateRate == 15.0f,
            "Checked-in audio quality presets did not parse intact: " +
                relative.string());
  }
}

void requireAmbientOcclusionRejected(
    std::string const& ambientOcclusionLine,
    std::string const& description) {
  try {
    (void)parseWithAmbientOcclusion(ambientOcclusionLine);
  } catch (std::exception const& error) {
    require(std::string(error.what()).find("AmbientOcclusion") != std::string::npos,
            "The ambient-occlusion error did not name the field.");
    return;
  }
  throw std::runtime_error("Video configuration accepted " + description + ".");
}
}  // namespace

int main() {
  try {
    auto validPath = writeConfiguration("");
    auto options = parseProgramOptions(validPath.string());
    std::filesystem::remove(validPath);
    require(options.dll == "BooleanWorld.dll", "Valid configuration did not parse.");

    requireRejected("GameResource");
    requireRejected("MisspelledOption");

    auto defaultGeneration = parseWithWorldDataGeneration("").worldDataGeneration;
    require(defaultGeneration.mode ==
                    bw::app::WorldDataGenerationMode::Asynchronous &&
                defaultGeneration.startInterval == 5.0f,
            "A missing WorldDataGeneration section did not keep its defaults.");
    auto emptyGeneration =
        parseWithWorldDataGeneration("    WorldDataGeneration:\n")
            .worldDataGeneration;
    require(emptyGeneration == defaultGeneration,
            "An empty WorldDataGeneration section did not keep its defaults.");
    require(parseWithWorldDataGeneration(
                "    WorldDataGeneration:\n      Mode: SyNcHrOnOuS\n")
                    .worldDataGeneration.mode ==
                bw::app::WorldDataGenerationMode::Synchronous,
            "Synchronous Generation mode did not use standard case handling.");
    require(parseWithWorldDataGeneration(
                "    WorldDataGeneration:\n      Mode: ASYNCHRONOUS\n")
                    .worldDataGeneration.mode ==
                bw::app::WorldDataGenerationMode::Asynchronous,
            "Asynchronous Generation mode did not use standard case handling.");
    require(parseWithWorldDataGeneration(
                "    WorldDataGeneration:\n      StartInterval: 0\n")
                    .worldDataGeneration.startInterval == 0.0f,
            "A zero Generation start interval did not parse.");
    require(parseWithWorldDataGeneration(
                "    WorldDataGeneration:\n      StartInterval: 2.75\n")
                    .worldDataGeneration.startInterval == 2.75f,
            "A fractional Generation start interval did not parse intact.");
    require(parseWithWorldDataGeneration(
                "    WorldDataGeneration:\n      StartInterval: 45\n")
                    .worldDataGeneration.startInterval == 45.0f,
            "A Generation start interval above the F4 range was rejected.");
    auto visibleGeneration = parseWithWorldDataGeneration(
        "    WorldDataGeneration:\n"
        "      AlwaysUpdateVertices: true\n"
        "      AllowCommitIfVisible: true\n").worldDataGeneration;
    require(visibleGeneration.alwaysUpdateVertices &&
                visibleGeneration.allowCommitIfVisible,
            "Visible animated-vertex Generation options did not parse.");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      Interval: 5\n", "Interval");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      Mode: worker\n", "Mode");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      StartInterval: -1\n", "StartInterval");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      StartInterval: nan\n", "StartInterval");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      StartInterval: inf\n", "StartInterval");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      StartInterval: soon\n", "StartInterval");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      AlwaysUpdateVertices: yes\n",
        "AlwaysUpdateVertices");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration:\n      AllowCommitIfVisible: no\n",
        "AllowCommitIfVisible");
    requireWorldDataGenerationRejected(
        "    WorldDataGeneration: five\n", "WorldDataGeneration");
    checkedInConfigurationsDeclareAcceptedFiveSecondInterval();

    require(parseWithInput("").input.mouseSensitivity == 0.3f,
            "A configuration without an Input section did not default the mouse sensitivity.");
    require(parseWithInput("  Input:\n").input.mouseSensitivity == 0.3f,
            "An empty Input section did not default the mouse sensitivity.");
    require(parseWithInput("  Input:\n    MouseSensitivity: 2.5\n").input.mouseSensitivity == 2.5f,
            "The configured mouse sensitivity did not parse.");

    requireInputRejected("  Input:\n    MouseSensitivity: 0\n", "a zero mouse sensitivity");
    requireInputRejected("  Input:\n    MouseSensitivity: -1.5\n", "a negative mouse sensitivity");
    requireInputRejected("  Input:\n    MouseSensitivity: fast\n", "an unparseable mouse sensitivity");
    requireInputRejected("  Input:\n    MouseSensitivty: 2.0\n", "a misspelled input field");

    require(parseWithRenderScale("").video.renderScale == bw::app::RenderScale::Full,
            "A configuration without RenderScale did not default to full.");
    require(parseWithRenderScale("    RenderScale: HaLf\n").video.renderScale == bw::app::RenderScale::Half,
            "The configured half render scale did not parse case-insensitively.");
    require(parseWithRenderScale("    RenderScale: QUARTER\n").video.renderScale == bw::app::RenderScale::Quarter,
            "The configured quarter render scale did not parse case-insensitively.");
    require(parseWithRenderScale("    RenderScale: EiGhTh\n").video.renderScale == bw::app::RenderScale::Eighth,
            "The configured eighth render scale did not parse case-insensitively.");
    requireRenderScaleRejected("    RenderScale: sixteenth\n", "an unknown render scale");
    requireRenderScaleRejected("    RenderScale: 2\n", "a numeric render scale");
    requireRenderScaleRejected("    RenderScale:\n", "an empty render scale");

    require(parseWithAntiAliasing("").video.antiAliasing == bw::app::AntiAliasing::Off,
            "A configuration without AA did not default to off.");
    require(parseWithAntiAliasing("    AA: MSAA-2X\n").video.antiAliasing == bw::app::AntiAliasing::Msaa2x,
            "The configured 2x MSAA did not parse case-insensitively.");
    require(parseWithAntiAliasing("    AA: msaa-4x\n").video.antiAliasing == bw::app::AntiAliasing::Msaa4x,
            "The configured 4x MSAA did not parse.");
    require(parseWithAntiAliasing("    AA: msaa-8x\n").video.antiAliasing == bw::app::AntiAliasing::Msaa8x,
            "The configured 8x MSAA did not parse.");
    require(parseWithAntiAliasing("    AA: FXAA\n").video.antiAliasing == bw::app::AntiAliasing::Fxaa,
            "The configured FXAA did not parse case-insensitively.");
    requireAntiAliasingRejected("    AA: msaa-16x\n", "an unsupported MSAA sample count");
    requireAntiAliasingRejected("    AA: 4\n", "a numeric AA setting");
    requireAntiAliasingRejected("    AA:\n", "an empty AA setting");

    require(parseWithAmbientOcclusion("").video.ambientOcclusion ==
                bw::app::AmbientOcclusion::GtaoDepth,
            "A configuration without AmbientOcclusion did not default to depth-normal GTAO.");
    require(parseWithAmbientOcclusion("    AmbientOcclusion: SsAo\n")
                    .video.ambientOcclusion == bw::app::AmbientOcclusion::Ssao,
            "The configured SSAO method did not parse case-insensitively.");
    require(parseWithAmbientOcclusion("    AmbientOcclusion: GTAO-DEPTH\n")
                    .video.ambientOcclusion ==
                bw::app::AmbientOcclusion::GtaoDepth,
            "The configured depth-normal GTAO method did not parse case-insensitively.");
    require(parseWithAmbientOcclusion("    AmbientOcclusion: gtao-normals\n")
                    .video.ambientOcclusion ==
                bw::app::AmbientOcclusion::GtaoNormals,
            "The configured MRT-normal GTAO method did not parse.");
    require(parseWithAmbientOcclusion("    AmbientOcclusion: none\n")
                    .video.ambientOcclusion == bw::app::AmbientOcclusion::None,
            "The configured disabled ambient occlusion did not parse.");
    requireAmbientOcclusionRejected(
        "    AmbientOcclusion: gtao\n", "the retired GTAO spelling");
    requireAmbientOcclusionRejected(
        "    AmbientOcclusion: hbao\n", "an unknown ambient-occlusion method");
    requireAmbientOcclusionRejected(
        "    AmbientOcclusion:\n", "an empty ambient-occlusion method");

    require(parseWithRenderTextureFilter("").video.renderTextureFilter ==
                bw::app::RenderTextureFilter::Linear,
            "A configuration without RenderTextureFilter did not default to linear.");
    require(parseWithRenderTextureFilter("    RenderTextureFilter: NeArEsT\n").video.renderTextureFilter ==
                bw::app::RenderTextureFilter::Nearest,
            "The configured nearest filter did not parse case-insensitively.");
    requireRenderTextureFilterRejected(
        "    RenderTextureFilter: bilinear\n", "an unknown render-texture filter");
    requireRenderTextureFilterRejected(
        "    RenderTextureFilter:\n", "an empty render-texture filter");

    require(parseWithHorizontalMaterials("").video.horizontalMaterials ==
                bw::app::HorizontalMaterials::TwoDimensional,
            "A configuration without HorizontalMaterials did not default to 2d.");
    require(parseWithHorizontalMaterials("    HorizontalMaterials: 3D\n")
                    .video.horizontalMaterials ==
                bw::app::HorizontalMaterials::ThreeDimensional,
            "The configured 3d horizontal materials did not parse case-insensitively.");
    requireHorizontalMaterialsRejected(
        "    HorizontalMaterials: planar\n", "an unknown horizontal-material mode");
    requireHorizontalMaterialsRejected(
        "    HorizontalMaterials:\n", "an empty horizontal-material mode");

    auto defaultWaterReflections =
        parseWithWaterReflections("").video.waterReflections;
    require(defaultWaterReflections.technique ==
                bw::app::WaterReflectionTechnique::ScreenSpace &&
                defaultWaterReflections.planarResolution ==
                    bw::app::PlanarReflectionResolution::Half,
            "Missing WaterReflections did not default to Screen-space and Half.");
    auto waterReflections = parseWithWaterReflections(
                                "    WaterReflections:\n"
                                "      Technique: PlAnAr\n"
                                "      PlanarResolution: QuArTeR\n")
                                .video.waterReflections;
    require(waterReflections.technique ==
                bw::app::WaterReflectionTechnique::Planar &&
                waterReflections.planarResolution ==
                    bw::app::PlanarReflectionResolution::Quarter,
            "Named Water reflection values did not parse case-insensitively.");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      Technique: hybrid\n", "Technique");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      Technique: 1\n", "Technique");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      Technique:\n", "Technique");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      PlanarResolution: eighth\n",
        "PlanarResolution");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      PlanarResolution: 2\n",
        "PlanarResolution");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      PlanarResolution:\n",
        "PlanarResolution");
    requireWaterReflectionsRejected(
        "    WaterReflections:\n      Resolution: half\n", "Resolution");

    auto defaultPlayerTorch = parseWithPlayerTorch("").video.playerTorch;
    require(defaultPlayerTorch.attenuationRadius == 192.0f &&
                defaultPlayerTorch.attenuationFalloff == 64.0f,
            "A missing PlayerTorch block did not use attenuation defaults.");
    auto playerTorch = parseWithPlayerTorch(
                           "    PlayerTorch:\n"
                           "      Radius: 88.5\n"
                           "      Falloff: 22.25\n")
                           .video.playerTorch;
    require(playerTorch.attenuationRadius == 88.5f &&
                playerTorch.attenuationFalloff == 22.25f,
            "Configured Player Torch attenuation was not parsed intact.");
    requirePlayerTorchRejected(
        "    PlayerTorch:\n      Unknown: 1\n", "Unknown");
    requirePlayerTorchRejected(
        "    PlayerTorch:\n      Radius: 0\n", "Radius");
    requirePlayerTorchRejected(
        "    PlayerTorch:\n      Radius: nan\n", "Radius");
    requirePlayerTorchRejected(
        "    PlayerTorch:\n      Falloff: -1\n", "Falloff");
    requirePlayerTorchRejected(
        "    PlayerTorch:\n      Radius: 10\n      Falloff: 11\n", "Falloff");

    auto defaultShadows = parseWithShadows("").video.shadows;
    require(defaultShadows.enabled && defaultShadows.faceResolution == 1024 &&
                defaultShadows.nearPlane == 0.25f &&
                defaultShadows.range == 192.0f &&
                defaultShadows.filter == bw::app::ShadowFilter::Pcf &&
                defaultShadows.filterRadius == 1.0f &&
                defaultShadows.fadeStart == 0.9f,
            "A missing Shadows block did not use Player Torch defaults.");
    auto shadows = parseWithShadows(
                       "    Shadows:\n"
                       "      Enabled: false\n"
                       "      FaceResolution: 1537\n"
                       "      Range: 88.5\n"
                       "      NearPlane: 0.75\n"
                       "      ConstantBias: 0.00125\n"
                       "      NormalBias: 0.0045\n"
                       "      Filter: HaRd\n"
                       "      FilterRadius: 2.25\n"
                       "      FadeStart: 0.625\n")
                       .video.shadows;
    require(!shadows.enabled && shadows.faceResolution == 1537 &&
                shadows.range == 88.5f && shadows.nearPlane == 0.75f &&
                shadows.constantBias == 0.00125f &&
                shadows.normalBias == 0.0045f &&
                shadows.filter == bw::app::ShadowFilter::Hard &&
                shadows.filterRadius == 2.25f && shadows.fadeStart == 0.625f,
            "Configured Player Torch shadows were not parsed intact.");

    requireShadowsRejected("    Shadows:\n      Unknown: 1\n", "Unknown");
    requireShadowsRejected("    Shadows:\n      Enabled: 1\n", "Enabled");
    requireShadowsRejected("    Shadows:\n      FaceResolution: 1.5\n", "FaceResolution");
    requireShadowsRejected("    Shadows:\n      FaceResolution: 0\n", "FaceResolution");
    requireShadowsRejected("    Shadows:\n      Range: nan\n", "Range");
    requireShadowsRejected("    Shadows:\n      NearPlane: 0\n", "NearPlane");
    requireShadowsRejected("    Shadows:\n      Range: 1\n      NearPlane: 1\n", "Range");
    requireShadowsRejected("    Shadows:\n      ConstantBias: -0.1\n", "ConstantBias");
    requireShadowsRejected("    Shadows:\n      NormalBias: inf\n", "NormalBias");
    requireShadowsRejected("    Shadows:\n      Filter: soft\n", "Filter");
    requireShadowsRejected("    Shadows:\n      FilterRadius: -1\n", "FilterRadius");
    requireShadowsRejected("    Shadows:\n      FadeStart: 1.01\n", "FadeStart");

    require(parseWithAudioOutput("").audioOutput == bw::app::AudioOutput::Speakers,
            "A configuration without Audio/Output did not default to Speakers.");
    require(parseWithAudioOutput("    Output: HeAdPhOnEs\n").audioOutput ==
                bw::app::AudioOutput::Headphones,
            "The configured Headphones output did not parse case-insensitively.");
    require(parseWithAudioOutput("    Output: SPEAKERS\n").audioOutput ==
                bw::app::AudioOutput::Speakers,
            "The configured Speakers output did not parse case-insensitively.");
    require(parseWithAudioOutput("    Output: surround\n").audioOutput ==
                bw::app::AudioOutput::Surround,
            "The configured Surround output did not parse.");
    require(parseWithAudioOutput("    Output: Headphones\n").audio.speakerMode ==
                wp::application::SpeakerMode::Stereo,
            "Headphones did not force a stereo FMOD software format.");
    require(parseWithAudioOutput("    Output: Speakers\n").audio.speakerMode ==
                wp::application::SpeakerMode::Default,
            "Speakers did not follow the OS device's FMOD software format.");
    require(parseWithAudioOutput("    Output: Surround\n").audio.speakerMode ==
                wp::application::SpeakerMode::Surround5Point1,
            "Surround did not select a 5.1 FMOD software format.");
    requireAudioOutputRejected("    Output: quadraphonic\n", "an unknown audio output");
    requireAudioOutputRejected("    Output:\n", "an empty audio output");
    requireAudioFieldRejected("    Volume: 5\n", "Volume");

    auto simulation = parseWithAudioOutput(
        "    QualityPreset: Low\n"
        "    QualityMayBeModifiedLive: false\n"
        "    Occlusion: true\n"
        "    Transmission: false\n"
        "    Reflections: false\n"
        "    AirAbsorption: true\n"
        "    QualityPresets:\n"
        "      Preset:\n"
        "        - Name: Low\n"
        "          RayCount: 1024\n"
        "          BounceCount: 2\n"
        "          ImpulseResponseDuration: 0.5\n"
        "          AmbisonicOrder: 1\n"
        "          ReflectionSourceCap: 3\n"
        "          SimulationUpdateRate: 8\n").audioSimulation;
    require(simulation.presets.size() == 1 && simulation.qualityPreset == "Low" &&
                !simulation.qualityMayBeModifiedLive &&
                simulation.presets[0].rayCount == 1024 &&
                simulation.presets[0].reflectionSourceCap == 3 &&
                !simulation.features.transmission && !simulation.features.reflections,
            "Audio simulation options did not parse intact.");
    requireAudioFieldRejected(
        "    Occlusion: false\n    Transmission: true\n", "Transmission");
    requireAudioFieldRejected("    QualityPreset: Missing\n", "QualityPreset");
    requireAudioFieldRejected("    Occlussion: true\n", "Occlussion");

    std::cout << "Program-options schema validation passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
