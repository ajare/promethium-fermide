#pragma once

#include <string>
#include <vector>
#include <map>

#include <willpower/common/Logger.h>

#include <willpower/application/AudioOptions.h>

#include "AudioOutputOptions.h"
#include "AudioSimulationOptions.h"
#include "VideoOptions.h"
#include "WorldDataGenerationOptions.h"

struct ProgramOptions {
  struct ResourceLocation {
    std::string type;
    std::string path;
    std::string definitionFile;
  };

  struct Debugging {
    bool inGame;
  };

  // Input settings handed to the game DLL. They govern control of the player
  // in the 3d world; menus and other 2d interfaces are unaffected.
  struct Input {
    // Matches the game's own default: the flat scale SDL used to apply to
    // relative motion, now applied once, where it can be configured.
    float mouseSensitivity{0.3f};
  };

public:
  int screenWidth, screenHeight;

  bool fullScreen, vSync;

  std::string dll;

  std::vector<ResourceLocation> resourceLocations;

  std::map<std::string, std::string> arguments;

  bool audioEnabled;

  wp::application::AudioOptions audio;

  bw::app::AudioOutput audioOutput{bw::app::AudioOutput::Speakers};

  bw::app::AudioSimulationOptions audioSimulation;

  Input input;

  bw::app::VideoOptions video;

  bw::app::WorldDataGenerationOptions worldDataGeneration;

  Debugging debugging;
};

ProgramOptions parseProgramOptions(std::string const& filename);

void logProgramOptions(ProgramOptions const& options, wp::Logger* logger);